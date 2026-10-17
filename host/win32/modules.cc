#include "win32/modules.hh"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

#include "adw/core/log.h"
#include "adw/core/text.h"
#include "win32/layout.hh"
#include "win32/runtime.hh"
#include "win32/shims.hh"
#include "win32/vfs.hh"

namespace adw::win32 {

namespace pe = loader::pe;

namespace {

std::string upper(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = char(toupper(uint8_t(c)));
  return o;
}

std::string file_part(std::string_view path) {
  size_t p = path.find_last_of("\\/");
  return std::string(p == std::string_view::npos ? path : path.substr(p + 1));
}

std::string dir_part(std::string_view path) {
  size_t p = path.find_last_of("\\/");
  return p == std::string_view::npos ? std::string() : std::string(path.substr(0, p));
}

bool host_file_exists(const std::string& path) {
  DWORD a = GetFileAttributesW(widen(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Places images through the module table's allocator and copies them in.
class ArenaSink : public loader::ImageSink {
 public:
  ArenaSink(std::function<uint32_t(uint32_t, uint32_t)> place, cpu::MemoryContext& mem)
      : place_(std::move(place)), mem_(mem) {}
  uint32_t reserve(uint32_t preferred, uint32_t size) override {
    base_ = place_(preferred, size);
    return base_;
  }
  void write(uint32_t addr, const void* data, size_t size) override { mem_.memcpy(addr, data, size); }
  uint32_t base() const { return base_; }

 private:
  std::function<uint32_t(uint32_t, uint32_t)> place_;
  cpu::MemoryContext& mem_;
  uint32_t base_ = 0;
};

// A header page with just enough PE structure to look like a module base:
// MZ stub, "PE\0\0", a file header with no sections and an optional header
// with no directories (so no exports: GetProcAddress on it finds nothing).
void write_stub_header(cpu::MemoryContext& mem, uint32_t base, uint32_t size_of_image, bool dll) {
  mem.memset(base, 0, 0x1000);
  mem.write_u16l(base + 0x00, 0x5A4D);  // MZ
  mem.write_u32l(base + 0x3C, 0x80);    // e_lfanew
  uint32_t pe = base + 0x80;
  mem.write_u32l(pe, 0x00004550);                        // PE\0\0
  mem.write_u16l(pe + 4, 0x014C);                        // i386
  mem.write_u16l(pe + 6, 0);                             // no sections
  mem.write_u16l(pe + 20, 0xE0);                         // SizeOfOptionalHeader
  mem.write_u16l(pe + 22, dll ? 0x210E : 0x010F);        // Characteristics
  uint32_t oh = pe + 24;
  mem.write_u16l(oh + 0, 0x010B);                        // PE32
  mem.write_u32l(oh + 28, base);                         // ImageBase
  mem.write_u32l(oh + 32, 0x1000);                       // SectionAlignment
  mem.write_u32l(oh + 36, 0x200);                        // FileAlignment
  mem.write_u16l(oh + 40, 4);                            // OS 4.0
  mem.write_u16l(oh + 48, 4);                            // subsystem 4.0
  mem.write_u32l(oh + 56, size_of_image);                // SizeOfImage
  mem.write_u32l(oh + 60, 0x1000);                       // SizeOfHeaders
  mem.write_u16l(oh + 68, 2);                            // IMAGE_SUBSYSTEM_WINDOWS_GUI
  mem.write_u32l(oh + 92, 16);                           // NumberOfRvaAndSizes
}

}  // namespace

bool ModuleTable::is_system_dll(std::string_view n) {
  static const std::set<std::string, std::less<>> kSystem = {
      "ADVAPI32.DLL", "COMCTL32.DLL", "COMDLG32.DLL", "DDRAW.DLL",   "DSOUND.DLL",  "GDI32.DLL",
      "IMM32.DLL",    "KERNEL32.DLL", "MPR.DLL",      "MSACM32.DLL", "MSVFW32.DLL", "NTDLL.DLL",
      "OLE32.DLL",    "OLEAUT32.DLL", "SHELL32.DLL",  "USER32.DLL",  "VERSION.DLL", "WING32.DLL",
      "WINMM.DLL",    "WINSPOOL.DRV", "WSOCK32.DLL",
  };
  return kSystem.count(n) != 0;
}

ModuleTable::ModuleTable(Runtime& rt) : rt_(rt) {
  // A whole program's EXE takes 0x400000 itself (load_exe); a module's host is the stand-in.
  if (rt.options().program) image_low_ = layout::kExeBase;
  else build_exe_header();
}

ModuleTable::~ModuleTable() = default;

void ModuleTable::build_exe_header() {
  rt_.mem().allocate_at(layout::kExeBase, layout::kExeSize);
  write_stub_header(rt_.mem(), layout::kExeBase, layout::kExeSize, /*dll=*/false);
  occupied_[layout::kExeBase] = layout::kExeSize;
}

void ModuleTable::add_search_dir(const std::string& host_dir) {
  if (std::find(search_dirs_.begin(), search_dirs_.end(), host_dir) == search_dirs_.end())
    search_dirs_.push_back(host_dir);
}

uint32_t ModuleTable::exe_handle() const { return exe_ ? exe_->base : layout::kExeBase; }

std::optional<std::string> ModuleTable::locate(std::string_view name, const std::string& first_dir) const {
  std::string file = file_part(name);
  std::vector<std::string> dirs;
  if (!first_dir.empty()) dirs.push_back(first_dir);
  dirs.insert(dirs.end(), search_dirs_.begin(), search_dirs_.end());
  for (const std::string& d : dirs) {
    std::string p = d + "\\" + file;
    if (host_file_exists(p)) return p;
    if (file.find('.') == std::string::npos && host_file_exists(p + ".DLL")) return p + ".DLL";
  }
  return std::nullopt;
}

uint32_t ModuleTable::place(uint32_t preferred, uint32_t size) {
  uint32_t need = align_up(size, layout::kImageAlign);
  auto free_at = [&](uint32_t base) {
    if (base < image_low_ || uint64_t(base) + need > layout::kImageHigh) return false;
    auto it = occupied_.upper_bound(base);
    if (it != occupied_.end() && it->first < base + need) return false;
    if (it != occupied_.begin()) {
      auto prev = std::prev(it);
      if (prev->first + prev->second > base) return false;
    }
    return true;
  };
  uint32_t base = 0;
  if (preferred % layout::kImageAlign == 0 && free_at(preferred)) {
    base = preferred;
  } else {
    uint32_t cand = layout::kImageLow;
    for (auto& [b, s] : occupied_) {
      if (b >= layout::kImageLow && cand + uint64_t(need) <= b) break;
      cand = std::max(cand, align_up(b + s, layout::kImageAlign));
    }
    if (!free_at(cand)) throw std::runtime_error("image area exhausted");
    base = cand;
  }
  rt_.mem().allocate_at(base, need);
  occupied_[base] = need;
  return base;
}

Module* ModuleTable::find(std::string_view name) const {
  std::string n = upper(file_part(name));
  std::string with_dll = n.find('.') == std::string::npos ? n + ".DLL" : n;
  for (auto& m : modules_) {
    std::string mn = upper(m->name);
    if (mn == n || mn == with_dll) return m.get();
  }
  return nullptr;
}

Module* ModuleTable::by_handle(uint32_t hmod) const {
  for (auto& m : modules_)
    if (m->base == hmod) return m.get();
  return nullptr;
}

Module* ModuleTable::containing(uint32_t addr) const {
  for (auto& m : modules_)
    if (addr >= m->base && addr - m->base < m->size) return m.get();
  return nullptr;
}

Module* ModuleTable::load(const std::string& path_or_name) {
  if (Module* m = find(path_or_name)) return m;
  std::string host;
  if (path_or_name.find_first_of("\\/:") != std::string::npos && host_file_exists(path_or_name)) {
    host = path_or_name;
  } else if (auto p = locate(path_or_name, {})) {
    host = *p;
  } else {
    throw std::runtime_error("cannot find " + path_or_name);
  }
  return load_image(host, file_part(host));
}

Module* ModuleTable::load_exe(const std::string& host_path) {
  if (exe_) throw std::logic_error("the process image is already loaded");
  Module* m = load_image(host_path, file_part(host_path));
  if (m->dll) throw loader::LoaderError(loader::LoaderError::Kind::unsupported, m->name + ": a DLL, not a program");
  exe_ = m;
  rt_.mem().write_u32l(layout::kPeb + 0x08, m->base);  // PEB.ImageBaseAddress
  return m;
}

Module* ModuleTable::load_image(const std::string& host_path, const std::string& name) {
  auto img = std::make_unique<pe::Image>(pe::Image::from_file(host_path));
  if (img->header().machine != pe::machine_i386) {
    throw loader::LoaderError(loader::LoaderError::Kind::unsupported, name + ": not an i386 image");
  }
  auto owned = std::make_unique<Module>();
  Module* m = owned.get();
  m->name = name;
  m->host_path = host_path;
  m->guest_path = rt_.vfs().to_guest(host_path);
  if (m->guest_path.empty()) m->guest_path = rt_.options().guest_module_dir + "\\" + upper(name);
  m->dll = img->header().is_dll();
  // Registered before binding, so an import cycle finds it instead of looping.
  modules_.push_back(std::move(owned));
  ArenaSink sink([this](uint32_t pref, uint32_t size) { return place(pref, size); }, rt_.mem());
  try {
    m->loaded = pe::load(*img, sink, [&](const pe::Import& imp) { return resolve_import(*m, imp); });
  } catch (...) {
    if (uint32_t b = sink.base()) {
      occupied_.erase(b);
      rt_.mem().free(b);
    }
    modules_.erase(std::find_if(modules_.begin(), modules_.end(), [&](auto& p) { return p.get() == m; }));
    throw;
  }
  m->base = m->loaded.base;
  m->size = m->loaded.size;
  m->entry = m->loaded.entry;
  m->image = std::move(img);
  trace("module", "%s at 0x%08X (size 0x%X, entry 0x%08X, %zu relocations, %zu imports)", m->name.c_str(),
        m->base, m->size, m->entry, m->loaded.relocations_applied, m->loaded.imports.size());
  return m;
}

uint32_t ModuleTable::resolve_import(Module& importer, const pe::Import& imp) {
  std::string dll = ShimRegistry::normalize_dll(imp.dll);
  if (!is_system_dll(dll)) {
    Module* dep = find(dll);
    if (!dep) {
      if (auto p = locate(imp.dll, dir_part(importer.host_path))) dep = load_image(*p, file_part(*p));
    }
    if (dep) {
      if (std::find(importer.deps.begin(), importer.deps.end(), dep) == importer.deps.end()) {
        importer.deps.push_back(dep);
        dep->refs++;
      }
      if (!dep->image) {
        // A cycle back into an image still being bound: its exports are known
        // (parsed) but its base is being decided right now. Not in our corpus.
        throw std::runtime_error("circular import between " + importer.name + " and " + dep->name);
      }
      const pe::Export* e = imp.by_ordinal ? dep->image->find_export(imp.ordinal) : dep->image->find_export(imp.name);
      if (e) return export_address(*dep, *e, 0);
      std::string what = dll + "!" + (imp.by_ordinal ? "#" + std::to_string(imp.ordinal) : imp.name);
      log("warning: %s imports %s, which %s does not export", importer.name.c_str(), what.c_str(),
          dep->name.c_str());
      return rt_.shims().internal_thunk("unresolved " + what, [what](Call&) {
        throw GuestError(GuestError::Kind::fatal, "call to unresolved import " + what);
      });
    }
    // Once per importer and DLL (an engine brings a hundred imports or more).
    if (missing_logged_.insert(importer.name + "|" + dll).second) {
      log("warning: %s imports %s, which was not found; binding its imports to stubs (a call stops the module)",
          importer.name.c_str(), dll.c_str());
    }
  }
  ShimEntry& e = imp.by_ordinal ? rt_.shims().get(dll, "#" + std::to_string(imp.ordinal))
                                : rt_.shims().get(dll, imp.name);
  // A missing code DLL was just reported above; name the imports of the DLLs
  // we emulate that the signature table lacks.
  if (is_system_dll(dll)) warn_unknown_signature(importer.name, e);
  return rt_.shims().thunk_address(e);
}

void ModuleTable::warn_unknown_signature(const std::string& importer, const ShimEntry& e) {
  if (e.known_signature) return;
  log("warning: %s imports %s, which has no signature in win32/signatures.cc: the module stops if it calls it",
      importer.c_str(), e.key().c_str());
}

uint32_t ModuleTable::export_address(Module& m, const pe::Export& e, int depth) {
  if (!e.is_forwarder()) return m.base + e.rva;
  if (depth > 8) throw std::runtime_error("forwarder loop at " + m.name + "!" + e.name);
  // "DLL.Name" or "DLL.#12".
  size_t dot = e.forwarder.find('.');
  std::string dll = ShimRegistry::normalize_dll(e.forwarder.substr(0, dot));
  std::string fn = dot == std::string::npos ? "" : e.forwarder.substr(dot + 1);
  if (!is_system_dll(dll)) {
    Module* dep = find(dll);
    if (!dep) {
      if (auto p = locate(dll, dir_part(m.host_path))) dep = load_image(*p, file_part(*p));
    }
    if (dep && dep->image) {
      const pe::Export* t = (!fn.empty() && fn[0] == '#') ? dep->image->find_export(uint16_t(std::stoi(fn.substr(1))))
                                                          : dep->image->find_export(fn);
      if (t) return export_address(*dep, *t, depth + 1);
    }
  }
  ShimEntry& target = rt_.shims().get(dll, fn);
  warn_unknown_signature(m.name + " (forwarder " + e.forwarder + ")", target);
  return rt_.shims().thunk_address(target);
}

bool ModuleTable::attach(Module* m) {
  if (m->attached || m->attaching) return true;
  m->attaching = true;
  for (Module* d : m->deps) {
    if (!attach(d)) {
      m->attaching = false;
      log("%s: dependency %s failed to initialize", m->name.c_str(), d->name.c_str());
      return false;
    }
  }
  if (m->dll && m->entry) {
    trace("module", "%s: DLL_PROCESS_ATTACH", m->name.c_str());
    uint32_t ok = rt_.call_guest(m->entry, {m->base, DLL_PROCESS_ATTACH, 0});
    if (!ok) {
      m->attaching = false;
      log("%s: DllMain(DLL_PROCESS_ATTACH) returned FALSE", m->name.c_str());
      return false;
    }
  }
  m->attaching = false;
  m->attached = true;
  return true;
}

void ModuleTable::detach_all() {
  for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
    Module* m = it->get();
    if (!m->attached) continue;
    m->attached = false;
    if (!m->dll || !m->entry) continue;
    try {
      trace("module", "%s: DLL_PROCESS_DETACH", m->name.c_str());
      rt_.call_guest(m->entry, {m->base, DLL_PROCESS_DETACH, 0});
    } catch (const std::exception& e) {
      log("%s: DllMain(DLL_PROCESS_DETACH) failed: %s", m->name.c_str(), e.what());
    }
  }
}

uint32_t ModuleTable::shim_module_handle(const std::string& dll) const {
  auto it = shim_handles_.find(dll);
  if (it != shim_handles_.end()) return it->second;
  uint32_t n = uint32_t(shim_handles_.size());
  if (n >= layout::kMaxShimModules) return 0;
  uint32_t h = layout::kShimModuleBase + n * layout::kShimModuleStride;
  rt_.mem().allocate_at(h, 0x1000);
  write_stub_header(rt_.mem(), h, 0x1000, /*dll=*/true);
  shim_handles_[dll] = h;
  return h;
}

bool ModuleTable::is_shim_module(uint32_t hmod) const {
  for (auto& [d, h] : shim_handles_)
    if (h == hmod) return true;
  return false;
}

std::string ModuleTable::shim_module_name(uint32_t hmod) const {
  for (auto& [d, h] : shim_handles_)
    if (h == hmod) return d;
  return {};
}

uint32_t ModuleTable::handle_of(std::string_view name) const {
  if (name.empty()) return exe_handle();
  if (Module* m = find(name)) return m->base;
  std::string n = ShimRegistry::normalize_dll(name);
  if (n == upper(file_part(rt_.options().exe_path))) return exe_handle();
  if (rt_.shims().has_dll(n)) return shim_module_handle(n);
  return 0;
}

uint32_t ModuleTable::load_library(std::string_view name) {
  if (Module* m = find(name)) {
    m->refs++;
    return m->base;
  }
  std::string n = ShimRegistry::normalize_dll(name);
  if (rt_.shims().has_dll(n)) return shim_module_handle(n);
  if (is_system_dll(n)) return 0;  // e.g. dsound.dll: not available, the engine runs silent
  std::string host;
  if (name.find_first_of("\\/:") != std::string_view::npos) {
    host = rt_.vfs().to_host(name);
    if (!host.empty() && !host_file_exists(host)) host.clear();
  } else if (auto p = locate(name, {})) {
    host = *p;
  }
  if (host.empty()) return 0;
  Module* m = load_image(host, file_part(host));
  if (!attach(m)) return 0;
  m->refs++;
  return m->base;
}

bool ModuleTable::free_library(uint32_t hmod) {
  if (is_shim_module(hmod)) return true;
  Module* m = by_handle(hmod);
  if (!m) return false;
  // Images stay mapped: our corpus only frees what it loaded dynamically, and
  // keeping the pages means a dangling pointer into a freed DLL still reads
  // what it did (unmapping is what Windows would do; nothing here relies on it).
  if (m->refs > 0) m->refs--;
  return true;
}

uint32_t ModuleTable::proc_address(uint32_t hmod, std::string_view name) {
  if (Module* m = by_handle(hmod)) {
    const pe::Export* e = m->image ? m->image->find_export(name) : nullptr;
    return e ? export_address(*m, *e, 0) : 0;
  }
  if (is_shim_module(hmod)) {
    std::string dll = shim_module_name(hmod);
    // Only names we have a signature or handler for: code that probes for an
    // optional export must see it missing rather than get a stub.
    ShimEntry* e = rt_.shims().find(dll, name);
    if (!e) {
      if (!rt_.shims().known(dll, name)) return 0;
      e = &rt_.shims().get(dll, name);
    }
    return rt_.shims().thunk_address(*e);
  }
  return 0;
}

uint32_t ModuleTable::proc_address(uint32_t hmod, uint16_t ordinal) {
  if (Module* m = by_handle(hmod)) {
    const pe::Export* e = m->image ? m->image->find_export(ordinal) : nullptr;
    return e ? export_address(*m, *e, 0) : 0;
  }
  return 0;
}

std::string ModuleTable::file_name(uint32_t hmod) const {
  if (hmod == 0 || hmod == exe_handle()) return rt_.options().exe_path;
  if (Module* m = by_handle(hmod)) return m->guest_path;
  std::string d = shim_module_name(hmod);
  if (!d.empty()) return "C:\\WINDOWS\\SYSTEM\\" + d;
  return {};
}

}  // namespace adw::win32
