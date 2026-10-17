// KERNEL32.DLL — process, memory, modules, resources, time, files, profile
// strings, exceptions (API_SURFACE.md §1 "KERNEL32.DLL", 118 functions).
//
// Implemented: everything the Borland RTL's DLL startup (ABI.md §2.1), the
// engine's module setup and the AD4 message loop reach, plus the rest of the
// memory/string/time/module/resource families (they are small and shared).
//
// Files and profiles go through the guest file system (vfs.hh): overlays
// over the module's folders whose upper layer is the per-user state
// (ADSTATE) or memory (INTERACTION.md §7). CreateFileA/_lopen/_lcreat/
// OpenFile, ReadFile/WriteFile/_lread/_lwrite/_hread/_hwrite, SetEndOfFile,
// SetFilePointer, DeleteFileA, CopyFileA, MoveFileA, Create/RemoveDirectoryA,
// GetFileAttributesA and FindFirst/NextFileA (a merged listing) all use it;
// a file opened for writing is copied up and committed atomically on close.
// Get/WritePrivateProfileStringA, GetProfileStringA (and the Int/Write
// variants reached through GetProcAddress) use the shared IniStore, with
// WIN.INI's [Berkeley Systems] entries as seeds (ABI.md §2.11).
//
// Known gaps, deliberately left (no module of the 202 in the five releases
// needs more; API_SURFACE.md §1 KERNEL32):
//   * SetFileTime is accepted on writable files and ignored (the Vfs keeps
//     its own times); FindFirstFileA reports the write time as all three times.
//   * CompareStringA/W, LCMapStringA/W, GetStringTypeA/W, GetLocaleInfoA/W:
//     passthrough with LCID 0x0409 forced (STARRYNI's MSVC CRT) — unverified
//     against that CRT's exact expectations.
#include <windows.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include "adw/core/log.h"
#include "adw/core/text.h"
#include "win32/modules.hh"
#include "win32/runtime.hh"
#include "win32/seh.hh"
#include "win32/shim_families.hh"
#include "win32/ini_store.hh"
#include "win32/vfs.hh"

namespace adw::win32 {

namespace {

constexpr const char* K = "KERNEL32.DLL";

// ---- profiles (WIN.INI and private INI files): win32/ini_store.hh ---------------------------------
//
// The lane's seed entries under the files the Vfs shows: WIN.INI's
// [Berkeley Systems] entries the engine needs to find its directories
// (ABI.md §2.11: AD Ini Files / AD Data Files / After Dark). Writes go to the
// overlay's upper layer (memory, or the persistent state) — IniStore.

std::string upper(std::string_view s) {
  std::string o(s);
  for (char& c : o) c = char(toupper(uint8_t(c)));
  return o;
}

// ---- kernel objects -------------------------------------------------------------------------------

struct KObject {
  enum class Kind { free, std_in, std_out, std_err, file, find, semaphore } kind = Kind::free;
  std::unique_ptr<VfsFile> file;       // file
  std::vector<Vfs::DirEntry> found;    // find: the matches, and the next one to hand out
  size_t next = 0;
  std::string guest_path;  // file: the guest path; semaphore: its name ("" = unnamed)
};

// Handles are 0x100 + 4n (NT-style small multiples of 4); HFILE values
// (_lopen) are the same numbers.
constexpr uint32_t kHandleBase = 0x100, kHandleStep = 4;

struct Kernel32State : RuntimeState {
  explicit Kernel32State(Runtime& rt) : rt(rt), ini(rt.vfs()) {
    objects.resize(3);
    objects[0].kind = KObject::Kind::std_in;
    objects[1].kind = KObject::Kind::std_out;
    objects[2].kind = KObject::Kind::std_err;
    const std::string& ad = rt.options().guest_module_dir;
    ini.add_seed("C:\\WINDOWS\\WIN.INI", "Berkeley Systems", "AD Ini Files", "C:\\WINDOWS");
    ini.add_seed("C:\\WINDOWS\\WIN.INI", "Berkeley Systems", "AD Data Files", ad);
    ini.add_seed("C:\\WINDOWS\\WIN.INI", "Berkeley Systems", "After Dark", ad);
  }
  // Open files flush (commit to the upper layer) as they close.
  ~Kernel32State() override { objects.clear(); }

  Runtime& rt;
  IniStore ini;
  std::vector<KObject> objects;
  // Resources: per module base, data RVA → guest address of its
  // IMAGE_RESOURCE_DATA_ENTRY (what an HRSRC is on Win32).
  std::map<uint32_t, std::map<uint32_t, uint32_t>> resource_entries;
  // TLS slots handed out (TEB+0xE10, 64 of them).
  uint64_t tls_used = 0;
  // VirtualAlloc reservations: base → size.
  std::map<uint32_t, uint32_t> reservations;
  // The guest's FILETIME at virtual time 0.
  uint64_t time_base = 0;
  bool time_base_set = false;
  std::string std_line;  // guest stdout/stderr text awaiting its newline
  // A whole program's (ABI.md §2.15): the global atom table (GlobalAddAtomA:
  // 0xC000 + index, with its reference count; 0 = deleted), SetErrorMode's
  // and SetUnhandledExceptionFilter's previous values.
  std::vector<std::pair<std::string, uint32_t>> atoms;
  uint32_t error_mode = 0, unhandled_filter = 0;

  uint32_t add(KObject o) {
    for (size_t i = 3; i < objects.size(); i++) {
      if (objects[i].kind == KObject::Kind::free) {
        objects[i] = std::move(o);
        return kHandleBase + uint32_t(i) * kHandleStep;
      }
    }
    objects.push_back(std::move(o));
    return kHandleBase + uint32_t(objects.size() - 1) * kHandleStep;
  }
  KObject* get(uint32_t h) {
    if (h < kHandleBase || (h - kHandleBase) % kHandleStep) return nullptr;
    size_t i = (h - kHandleBase) / kHandleStep;
    if (i >= objects.size() || objects[i].kind == KObject::Kind::free) return nullptr;
    return &objects[i];
  }
  VfsFile* file(uint32_t h) {
    KObject* o = get(h);
    return o && o->kind == KObject::Kind::file ? o->file.get() : nullptr;
  }
  bool close(uint32_t h) {
    KObject* o = get(h);
    if (!o || o->kind == KObject::Kind::std_in || o->kind == KObject::Kind::std_out ||
        o->kind == KObject::Kind::std_err) {
      return o != nullptr;
    }
    *o = KObject{};  // a file flushes here
    return true;
  }
};

Kernel32State& ks(Runtime& rt) { return rt.state<Kernel32State>(); }

// The guest path of an INI file name: a bare name lives in the Windows directory.
std::string profile_path(Runtime& rt, const std::string& name) {
  if (name.find_first_of("\\/:") == std::string::npos) return "C:\\WINDOWS\\" + upper(name);
  return upper(rt.vfs().full_path(name));
}

// Writes a NUL-separated list (double-NUL terminated) the way
// GetPrivateProfileString does for NULL section/key: returns the characters
// copied, excluding the final NUL; size-2 when truncated.
uint32_t write_list(MemoryContext& mem, uint32_t buf, uint32_t size, const std::vector<std::string>& items) {
  if (!buf || size < 2) {
    if (buf && size) mem.write_u8(buf, 0);
    return 0;
  }
  std::string out;
  for (const std::string& s : items) {
    out += s;
    out.push_back('\0');
  }
  if (out.size() + 1 > size) {
    out.resize(size - 2);
    out.push_back('\0');
    mem.memcpy(buf, out.data(), out.size());
    mem.write_u8(buf + uint32_t(out.size()), 0);
    return size - 2;
  }
  if (out.empty()) out.push_back('\0');
  mem.memcpy(buf, out.data(), out.size());
  mem.write_u8(buf + uint32_t(out.size()), 0);
  return uint32_t(out.size() == 1 && items.empty() ? 0 : out.size());
}

uint32_t get_profile_string(Runtime& rt, const std::string& file, uint32_t section_p, uint32_t key_p,
                            uint32_t def_p, uint32_t buf, uint32_t size) {
  auto& mem = rt.mem();
  IniStore& ini = ks(rt).ini;
  std::string path = profile_path(rt, file);
  if (!section_p) return write_list(mem, buf, size, ini.sections(path));
  std::string section = read_cstr(mem, section_p);
  if (!key_p) return write_list(mem, buf, size, ini.keys(path, section));
  std::string key = read_cstr(mem, key_p);
  std::optional<std::string> found = ini.get(path, section, key);
  std::string value;
  if (found) {
    value = *found;
  } else {
    value = read_cstr(mem, def_p);
    while (!value.empty() && value.back() == ' ') value.pop_back();
  }
  // A value in matching quotes is returned without them.
  if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
    value = value.substr(1, value.size() - 2);
  }
  trace("file", "profile %s [%s] %s -> \"%s\"%s", file.c_str(), section.c_str(), key.c_str(), value.c_str(),
        found ? "" : " (default)");
  if (!buf || !size) return 0;
  return uint32_t(write_cstr(mem, buf, value, size));
}

// GetPrivateProfileInt: the value's leading decimal digits (with a sign), or
// the default when the key is absent.
uint32_t get_profile_int(Runtime& rt, const std::string& file, const std::string& section, const std::string& key,
                         int32_t def) {
  std::optional<std::string> v = ks(rt).ini.get(profile_path(rt, file), section, key);
  if (!v) return uint32_t(def);
  return uint32_t(strtol(v->c_str(), nullptr, 10));
}

bool write_profile_string(Runtime& rt, const std::string& file, uint32_t section_p, uint32_t key_p,
                          uint32_t value_p) {
  if (!section_p) return true;  // a flush request: every write is already on disk
  auto& mem = rt.mem();
  std::string section = read_cstr(mem, section_p), key = key_p ? read_cstr(mem, key_p) : std::string(),
              value = value_p ? read_cstr(mem, value_p) : std::string();
  bool ok = ks(rt).ini.set(profile_path(rt, file), section,
                           key_p ? std::optional<std::string_view>(key) : std::nullopt,
                           value_p ? std::optional<std::string_view>(value) : std::nullopt);
  trace("file", "WritePrivateProfileString %s [%s] %s = \"%s\" -> %s", file.c_str(), section.c_str(),
        key_p ? key.c_str() : "(delete section)", value_p ? value.c_str() : "(delete)", ok ? "ok" : "failed");
  return ok;
}

// ---- time -----------------------------------------------------------------------------------------

// 100 ns ticks between 1601-01-01 and a fixed headless "now": 1996-09-12
// 12:00:00, the date AD_SND.DLL was built (any fixed moment keeps runs
// reproducible; this one is period-correct).
constexpr uint64_t kHeadlessEpoch = 125132976000000000ull;

SYSTEMTIME filetime_to_systemtime(uint64_t ft) {
  FILETIME f{DWORD(ft), DWORD(ft >> 32)};
  SYSTEMTIME st{};
  FileTimeToSystemTime(&f, &st);
  return st;
}

// ---- resources ------------------------------------------------------------------------------------

loader::ResId res_id(const MemoryContext& mem, uint32_t v) {
  if (v < 0x10000) return loader::ResId::of(uint16_t(v));
  std::string s = read_cstr(mem, v);
  if (!s.empty() && s[0] == '#') return loader::ResId::of(uint16_t(strtoul(s.c_str() + 1, nullptr, 10)));
  return loader::ResId::of(s);
}

// Every IMAGE_RESOURCE_DATA_ENTRY of a mapped image, by the RVA of its data:
// walks the resource directory as the guest sees it (depth ≤ 3, as Win32).
std::map<uint32_t, uint32_t> map_resource_entries(const MemoryContext& mem, const Module& m) {
  std::map<uint32_t, uint32_t> out;
  const auto& dir = m.image->header().dirs[loader::pe::dir_resource];
  if (!dir.rva || !dir.size) return out;
  uint32_t root = m.base + dir.rva, end = root + dir.size;
  std::vector<std::pair<uint32_t, int>> todo = {{root, 0}};
  size_t visited = 0;
  while (!todo.empty() && visited++ < 100000) {
    auto [d, depth] = todo.back();
    todo.pop_back();
    if (d + 16 > end) continue;
    uint32_t n = uint32_t(mem.read_u16l(d + 12)) + mem.read_u16l(d + 14);
    for (uint32_t i = 0; i < n; i++) {
      uint32_t e = d + 16 + 8 * i;
      if (e + 8 > end) break;
      uint32_t off = mem.read_u32l(e + 4);
      if (off & 0x80000000) {
        if (depth < 3) todo.push_back({root + (off & 0x7FFFFFFF), depth + 1});
      } else if (root + off + 16 <= end) {
        out[mem.read_u32l(root + off)] = root + off;
      }
    }
  }
  return out;
}

}  // namespace

// The resource lookup the engine and USER32 (LoadString, LoadBitmap) share:
// US English first, then language-neutral, then whatever is there (ABI.md
// §2.10.1: "our readers prefer LANG 0x409, then neutral").
const loader::pe::Resource* find_module_resource(Runtime& rt, uint32_t hmod, const loader::ResId& type,
                                                 const loader::ResId& name) {
  Module* m = rt.modules().by_handle(hmod);
  if (!m || !m->image) return nullptr;
  for (auto lang : {std::optional<uint16_t>(0x409), std::optional<uint16_t>(0), std::optional<uint16_t>()}) {
    if (const auto* r = m->image->find_resource(type, name, lang)) return r;
  }
  return nullptr;
}

uint64_t guest_local_filetime(Runtime& rt) {
  Kernel32State& s = ks(rt);
  if (!s.time_base_set) {
    s.time_base_set = true;
    if (rt.clock().mode() == VirtualClock::Mode::fixed_step) {
      s.time_base = kHeadlessEpoch;
    } else {
      SYSTEMTIME st;
      ::GetLocalTime(&st);
      FILETIME f;
      SystemTimeToFileTime(&st, &f);
      s.time_base = (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
      s.time_base -= rt.clock().now_us() * 10;
    }
  }
  return s.time_base + rt.clock().read_us() * 10;
}

void register_kernel32(ShimRegistry& r) {
  // ---- process / thread ----
  r.impl(K, "GetVersion", [](Call& c) {
    // Windows 95 (4.00.950): build in bits 16..30, bit 31 set = not NT.
    c.ret(0xC3B60004);
  });
  r.impl(K, "GetCommandLineA", [](Call& c) {
    const std::string& cl = c.rt.options().command_line;
    c.ret(c.rt.static_bytes("cmdline", std::string_view(cl.c_str(), cl.size() + 1)));
  });
  static const char kEnv[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\WINDOWS;C:\\WINDOWS\\COMMAND\0"
                             "TEMP=C:\\WINDOWS\\TEMP\0windir=C:\\WINDOWS\0\0";
  r.impl(K, "GetEnvironmentStrings", [](Call& c) {
    c.ret(c.rt.static_bytes("environment", std::string_view(kEnv, sizeof(kEnv))));
  });
  r.impl(K, "GetEnvironmentStringsW", [](Call& c) {
    std::string w;
    for (char ch : std::string_view(kEnv, sizeof(kEnv))) {
      w.push_back(ch);
      w.push_back('\0');
    }
    c.ret(c.rt.static_bytes("environmentW", w));
  });
  r.impl(K, "FreeEnvironmentStringsA", [](Call& c) { c.ret(1); });
  r.impl(K, "FreeEnvironmentStringsW", [](Call& c) { c.ret(1); });
  r.impl(K, "SetEnvironmentVariableA", [](Call& c) { c.ret(1); });
  r.impl(K, "GetStartupInfoA", [](Call& c) {
    g32::STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (c.rt.options().program) {
      // A program started by ShellExecuteA(..., SW_SHOWNORMAL), as
      // TOYSAV32.SCR started the Desktop Toys (pe32/program.cc).
      si.dwFlags = STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_SHOWNORMAL;
    }
    write_pod(c.mem(), c.arg(0), si);
  });
  r.impl(K, "GetCurrentThreadId", [](Call& c) { c.ret(c.mem().read_u32l(c.rt.teb() + 0x24)); });
  r.impl(K, "GetCurrentProcess", [](Call& c) { c.ret(0xFFFFFFFF); });
  r.impl(K, "GetLastError", [](Call& c) { c.ret(c.rt.last_error()); });
  r.impl(K, "SetLastError", [](Call& c) { c.rt.set_last_error(c.arg(0)); });
  r.impl(K, "GetVersionExA", [](Call& c) {
    // OSVERSIONINFOA (148 bytes): the Windows 95 GetVersion describes.
    uint32_t p = c.arg(0);
    if (!p || c.mem().read_u32l(p) < 148) {
      c.set_last_error(ERROR_INSUFFICIENT_BUFFER);
      return c.ret(0);
    }
    c.mem().memset(p + 4, 0, 144);
    c.mem().write_u32l(p + 4, 4);            // dwMajorVersion
    c.mem().write_u32l(p + 8, 0);            // dwMinorVersion
    c.mem().write_u32l(p + 12, 0x040003B6);  // dwBuildNumber: 950, major.minor in the high word (9x)
    c.mem().write_u32l(p + 16, 1);           // VER_PLATFORM_WIN32_WINDOWS
    c.ret(1);
  });
  r.impl(K, "GetSystemInfo", [](Call& c) {
    // SYSTEM_INFO (36 bytes) of a one-CPU Pentium under Windows 95.
    uint32_t p = c.arg(0);
    c.mem().memset(p, 0, 36);
    c.mem().write_u16l(p + 0, 0);              // PROCESSOR_ARCHITECTURE_INTEL
    c.mem().write_u32l(p + 4, 0x1000);         // dwPageSize
    c.mem().write_u32l(p + 8, 0x00400000);     // lpMinimumApplicationAddress
    c.mem().write_u32l(p + 12, 0x7FFFFFFF);    // lpMaximumApplicationAddress
    c.mem().write_u32l(p + 16, 1);             // dwActiveProcessorMask
    c.mem().write_u32l(p + 20, 1);             // dwNumberOfProcessors
    c.mem().write_u32l(p + 24, 586);           // PROCESSOR_INTEL_PENTIUM
    c.mem().write_u32l(p + 28, 0x10000);       // dwAllocationGranularity
    // wProcessorLevel / wProcessorRevision: not used on Windows 95 (0).
  });
  r.impl(K, "CreateSemaphoreA", [](Call& c) {
    // (attrs, initial, maximum, name). One emulated thread and no wait
    // functions in the lane, so a semaphore is just a named handle; a second
    // create of the same name answers ERROR_ALREADY_EXISTS, the single-instance
    // test programs make with it.
    int32_t initial = c.iarg(1), maximum = c.iarg(2);
    if (maximum <= 0 || initial < 0 || initial > maximum) {
      c.set_last_error(ERROR_INVALID_PARAMETER);
      return c.ret(0);
    }
    auto& s = ks(c.rt);
    std::string name = c.str(3);
    bool exists = false;
    if (!name.empty())
      for (const KObject& o : s.objects)
        if (o.kind == KObject::Kind::semaphore && o.guest_path == name) exists = true;
    KObject o;
    o.kind = KObject::Kind::semaphore;
    o.guest_path = name;
    uint32_t h = s.add(std::move(o));
    c.set_last_error(exists ? ERROR_ALREADY_EXISTS : 0);
    c.ret(h);
  });
  r.impl(K, "SetConsoleCtrlHandler", [](Call& c) { c.ret(1); });
  r.impl(K, "SetHandleCount", [](Call& c) { c.ret(c.arg(0)); });
  r.impl(K, "ExitProcess", [](Call& c) {
    throw GuestError(GuestError::Kind::exit, "the guest called ExitProcess(" + std::to_string(c.arg(0)) + ")",
                     c.arg(0));
  });
  r.impl(K, "TerminateProcess", [](Call& c) {
    throw GuestError(GuestError::Kind::exit, "the guest called TerminateProcess(" + std::to_string(c.arg(1)) + ")",
                     c.arg(1));
  });
  // One emulated thread: critical sections never contend.
  for (const char* n : {"InitializeCriticalSection", "DeleteCriticalSection", "EnterCriticalSection",
                        "LeaveCriticalSection"}) {
    r.impl(K, n, [](Call&) {});
  }

  // ---- exceptions (seh.hh) ----
  r.impl(K, "RaiseException", [](Call& c) { c.rt.seh().raise_exception(c); });
  r.impl(K, "RtlUnwind", [](Call& c) { c.rt.seh().rtl_unwind(c); });
  r.impl(K, "UnhandledExceptionFilter", [](Call& c) { c.ret(c.rt.seh().unhandled_exception_filter(c.arg(0))); });

  // ---- TLS (TEB+0xE10: 64 slots, as NT's TlsSlots) ----
  r.impl(K, "TlsAlloc", [](Call& c) {
    auto& s = ks(c.rt);
    for (uint32_t i = 0; i < 64; i++) {
      if (!(s.tls_used & (1ull << i))) {
        s.tls_used |= 1ull << i;
        c.mem().write_u32l(c.rt.teb() + 0xE10 + 4 * i, 0);
        return c.ret(i);
      }
    }
    c.ret(0xFFFFFFFF);
  });
  r.impl(K, "TlsFree", [](Call& c) {
    uint32_t i = c.arg(0);
    auto& s = ks(c.rt);
    if (i >= 64 || !(s.tls_used & (1ull << i))) return c.ret(0);
    s.tls_used &= ~(1ull << i);
    c.ret(1);
  });
  r.impl(K, "TlsGetValue", [](Call& c) {
    uint32_t i = c.arg(0);
    if (i >= 64) {
      c.set_last_error(ERROR_INVALID_PARAMETER);
      return c.ret(0);
    }
    c.set_last_error(0);
    c.ret(c.mem().read_u32l(c.rt.teb() + 0xE10 + 4 * i));
  });
  r.impl(K, "TlsSetValue", [](Call& c) {
    uint32_t i = c.arg(0);
    if (i >= 64) return c.ret(0);
    c.mem().write_u32l(c.rt.teb() + 0xE10 + 4 * i, c.arg(1));
    c.ret(1);
  });

  // ---- memory: Global/Local (heap.hh HandleHeap; the two families are one on Win32) ----
  for (const char* p : {"Global", "Local"}) {
    std::string pre(p);
    r.impl(K, pre + "Alloc", [](Call& c) { c.ret(c.rt.handles().alloc(c.arg(0), c.arg(1))); });
    r.impl(K, pre + "Free", [](Call& c) { c.ret(c.rt.handles().free(c.arg(0))); });
    r.impl(K, pre + "Lock", [](Call& c) { c.ret(c.rt.handles().lock(c.arg(0))); });
    r.impl(K, pre + "Unlock", [](Call& c) {
      bool still = c.rt.handles().unlock(c.arg(0));
      if (!still) c.set_last_error(0);
      c.ret_bool(still);
    });
    r.impl(K, pre + "ReAlloc", [](Call& c) { c.ret(c.rt.handles().realloc(c.arg(0), c.arg(1), c.arg(2))); });
    r.impl(K, pre + "Size", [](Call& c) { c.ret(c.rt.handles().size(c.arg(0))); });
    r.impl(K, pre + "Flags", [](Call& c) { c.ret(c.rt.handles().flags(c.arg(0))); });
  }
  r.impl(K, "GlobalHandle", [](Call& c) { c.ret(c.rt.handles().handle_of(c.arg(0))); });
  r.impl(K, "GlobalMemoryStatus", [](Call& c) {
    // A comfortable 1996 machine: 32 MB of RAM, half of it free. The engine's
    // XHeap sizes its pools from these (API_SURFACE.md).
    g32::MEMORYSTATUS ms{};
    ms.dwLength = sizeof(ms);
    ms.dwMemoryLoad = 50;
    ms.dwTotalPhys = 32u << 20;
    ms.dwAvailPhys = 16u << 20;
    ms.dwTotalPageFile = 64u << 20;
    ms.dwAvailPageFile = 48u << 20;
    ms.dwTotalVirtual = 0x7FFE0000;
    ms.dwAvailVirtual = 0x7F000000;
    write_pod(c.mem(), c.arg(0), ms);
  });

  // ---- memory: heaps ----
  r.impl(K, "GetProcessHeap", [](Call& c) { c.ret(c.rt.heaps().process_heap()); });
  r.impl(K, "HeapCreate", [](Call& c) { c.ret(c.rt.heaps().create()); });
  r.impl(K, "HeapDestroy", [](Call& c) { c.ret_bool(c.rt.heaps().destroy(c.arg(0))); });
  r.impl(K, "HeapAlloc", [](Call& c) { c.ret(c.rt.heaps().alloc(c.arg(0), c.arg(1), c.arg(2))); });
  r.impl(K, "HeapFree", [](Call& c) { c.ret_bool(c.rt.heaps().free(c.arg(0), c.arg(2))); });
  r.impl(K, "HeapReAlloc", [](Call& c) { c.ret(c.rt.heaps().realloc(c.arg(0), c.arg(1), c.arg(2), c.arg(3))); });
  // (heap, flags, ptr): the requested size, or (SIZE_T)-1 for a block the heap does not own.
  r.impl(K, "HeapSize", [](Call& c) { c.ret(c.rt.heaps().size(c.arg(0), c.arg(2))); });

  // ---- memory: virtual (reserve = allocate, 64K-aligned and zeroed; commit is free) ----
  r.impl(K, "VirtualAlloc", [](Call& c) {
    uint32_t addr = c.arg(0), size = c.arg(1), type = c.arg(2);
    auto& s = ks(c.rt);
    if (addr) {
      // Committing (part of) an earlier reservation.
      auto it = s.reservations.upper_bound(addr);
      if (it != s.reservations.begin()) {
        --it;
        if (addr >= it->first && uint64_t(addr) + size <= uint64_t(it->first) + it->second) {
          return c.ret(align_down(addr, 0x1000));
        }
      }
      if (!(type & MEM_RESERVE)) {
        c.set_last_error(ERROR_INVALID_ADDRESS);
        return c.ret(0);
      }
      // A reservation at a caller-chosen address: not supported; hand out any.
    }
    uint32_t n = align_up(size ? size : 1, 0x1000);
    uint32_t p = c.rt.heap().alloc(n, true, 0x10000);
    if (!p) {
      c.set_last_error(ERROR_NOT_ENOUGH_MEMORY);
      return c.ret(0);
    }
    s.reservations[p] = n;
    c.ret(p);
  });
  r.impl(K, "VirtualFree", [](Call& c) {
    uint32_t addr = c.arg(0), type = c.arg(2);
    auto& s = ks(c.rt);
    if (type & MEM_RELEASE) {
      auto it = s.reservations.find(addr);
      if (it == s.reservations.end()) return c.ret(0);
      c.rt.heap().free(addr);
      s.reservations.erase(it);
    }
    c.ret(1);  // MEM_DECOMMIT: the pages stay (nothing reads freed pages in our corpus)
  });

  // ---- strings ----
  r.impl(K, "lstrlenA", [](Call& c) { c.ret(uint32_t(c.str(0).size())); });
  r.impl(K, "lstrcpyA", [](Call& c) {
    std::string s = c.str(1);
    c.mem().memcpy(c.arg(0), s.c_str(), s.size() + 1);
    c.ret(c.arg(0));
  });
  r.impl(K, "lstrcatA", [](Call& c) {
    std::string d = c.str(0), s = c.str(1);
    c.mem().memcpy(c.arg(0) + uint32_t(d.size()), s.c_str(), s.size() + 1);
    c.ret(c.arg(0));
  });

  // ---- code pages and locale (fixed: Windows-1252 / OEM 437 / en-US, for reproducibility) ----
  r.impl(K, "GetACP", [](Call& c) { c.ret(1252); });
  r.impl(K, "GetOEMCP", [](Call& c) { c.ret(437); });
  r.impl(K, "GetCPInfo", [](Call& c) {
    uint8_t info[20] = {};
    info[0] = 1;       // MaxCharSize
    info[4] = '?';     // DefaultChar
    c.mem().memcpy(c.arg(1), info, sizeof(info));
    c.ret(1);
  });
  auto host_cp = [](uint32_t cp) -> UINT { return cp == CP_ACP ? 1252 : cp == CP_OEMCP ? 437 : cp; };
  auto host_lcid = [](uint32_t lcid) -> LCID {
    return (lcid == 0 || lcid == LOCALE_SYSTEM_DEFAULT || lcid == LOCALE_USER_DEFAULT) ? 0x0409 : lcid;
  };
  r.impl(K, "MultiByteToWideChar", [host_cp](Call& c) {
    uint32_t cp = c.arg(0), flags = c.arg(1), src = c.arg(2), dst = c.arg(4);
    int32_t len = c.iarg(3), cap = c.iarg(5);
    std::string in = len < 0 ? read_cstr(c.mem(), src) + '\0' : c.mem().read(src, uint32_t(len));
    int n = ::MultiByteToWideChar(host_cp(cp), flags & ~MB_ERR_INVALID_CHARS, in.data(), int(in.size()), nullptr, 0);
    if (!cap) return c.ret(uint32_t(n));
    std::wstring out(size_t(n), L'\0');
    ::MultiByteToWideChar(host_cp(cp), flags & ~MB_ERR_INVALID_CHARS, in.data(), int(in.size()), out.data(), n);
    if (n > cap) {
      c.set_last_error(ERROR_INSUFFICIENT_BUFFER);
      return c.ret(0);
    }
    for (int i = 0; i < n; i++) c.mem().write_u16l(dst + 2 * uint32_t(i), uint16_t(out[size_t(i)]));
    c.ret(uint32_t(n));
  });
  r.impl(K, "WideCharToMultiByte", [host_cp](Call& c) {
    uint32_t cp = c.arg(0), flags = c.arg(1), src = c.arg(2), dst = c.arg(4), used_p = c.arg(7);
    int32_t len = c.iarg(3), cap = c.iarg(5);
    std::wstring in;
    if (len < 0) {
      for (char16_t ch : read_wstr(c.mem(), src)) in.push_back(wchar_t(ch));
      in.push_back(L'\0');
    } else {
      for (int32_t i = 0; i < len; i++) in.push_back(wchar_t(c.mem().read_u16l(src + 2 * uint32_t(i))));
    }
    std::string def = c.arg(6) ? std::string(1, char(c.mem().read_u8(c.arg(6)))) : std::string();
    BOOL used = FALSE;
    UINT hcp = host_cp(cp);
    bool utf = hcp == CP_UTF7 || hcp == CP_UTF8;
    int n = ::WideCharToMultiByte(hcp, flags, in.data(), int(in.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    ::WideCharToMultiByte(hcp, flags, in.data(), int(in.size()), out.data(), n,
                          utf || def.empty() ? nullptr : def.c_str(), utf ? nullptr : &used);
    if (used_p) c.mem().write_u32l(used_p, used ? 1 : 0);
    if (!cap) return c.ret(uint32_t(n));
    if (n > cap) {
      c.set_last_error(ERROR_INSUFFICIENT_BUFFER);
      return c.ret(0);
    }
    if (n) c.mem().memcpy(dst, out.data(), size_t(n));
    c.ret(uint32_t(n));
  });
  r.impl(K, "GetStringTypeA", [host_lcid](Call& c) {
    // (Locale, dwInfoType, lpSrcStr, cchSrc, lpCharType)
    int32_t len = c.iarg(3);
    std::string in = len < 0 ? read_cstr(c.mem(), c.arg(2)) + '\0' : c.mem().read(c.arg(2), uint32_t(len));
    std::vector<WORD> types(in.size());
    BOOL ok = GetStringTypeExA(host_lcid(c.arg(0)), c.arg(1), in.data(), int(in.size()), types.data());
    for (size_t i = 0; i < types.size(); i++) c.mem().write_u16l(c.arg(4) + 2 * uint32_t(i), types[i]);
    c.ret_bool(ok);
  });
  r.impl(K, "GetStringTypeW", [](Call& c) {
    // (dwInfoType, lpSrcStr, cchSrc, lpCharType)
    int32_t len = c.iarg(2);
    std::wstring in;
    if (len < 0) {
      for (char16_t ch : read_wstr(c.mem(), c.arg(1))) in.push_back(wchar_t(ch));
      in.push_back(L'\0');
    } else {
      for (int32_t i = 0; i < len; i++) in.push_back(wchar_t(c.mem().read_u16l(c.arg(1) + 2 * uint32_t(i))));
    }
    std::vector<WORD> types(in.size());
    BOOL ok = ::GetStringTypeW(c.arg(0), in.data(), int(in.size()), types.data());
    for (size_t i = 0; i < types.size(); i++) c.mem().write_u16l(c.arg(3) + 2 * uint32_t(i), types[i]);
    c.ret_bool(ok);
  });
  // Passthrough helpers for the A/W locale functions that take (lcid, flags, src, len, dst, cap).
  r.impl(K, "LCMapStringA", [host_lcid](Call& c) {
    int32_t len = c.iarg(3), cap = c.iarg(5);
    std::string in = len < 0 ? read_cstr(c.mem(), c.arg(2)) + '\0' : c.mem().read(c.arg(2), uint32_t(len));
    int n = ::LCMapStringA(host_lcid(c.arg(0)), c.arg(1), in.data(), int(in.size()), nullptr, 0);
    if (!cap) return c.ret(uint32_t(n));
    std::string out(size_t(std::max(n, 0)), '\0');
    n = ::LCMapStringA(host_lcid(c.arg(0)), c.arg(1), in.data(), int(in.size()), out.data(), n);
    if (n > cap) return c.ret(0);
    if (n > 0) c.mem().memcpy(c.arg(4), out.data(), size_t(n));
    c.ret(uint32_t(n));
  });
  r.impl(K, "LCMapStringW", [host_lcid](Call& c) {
    int32_t len = c.iarg(3), cap = c.iarg(5);
    std::wstring in;
    if (len < 0) {
      for (char16_t ch : read_wstr(c.mem(), c.arg(2))) in.push_back(wchar_t(ch));
      in.push_back(L'\0');
    } else {
      for (int32_t i = 0; i < len; i++) in.push_back(wchar_t(c.mem().read_u16l(c.arg(2) + 2 * uint32_t(i))));
    }
    int n = ::LCMapStringW(host_lcid(c.arg(0)), c.arg(1), in.data(), int(in.size()), nullptr, 0);
    if (!cap) return c.ret(uint32_t(n));
    std::wstring out(size_t(std::max(n, 0)), L'\0');
    n = ::LCMapStringW(host_lcid(c.arg(0)), c.arg(1), in.data(), int(in.size()), out.data(), n);
    if (n > cap) return c.ret(0);
    for (int i = 0; i < n; i++) c.mem().write_u16l(c.arg(4) + 2 * uint32_t(i), uint16_t(out[size_t(i)]));
    c.ret(uint32_t(n));
  });
  r.impl(K, "CompareStringA", [host_lcid](Call& c) {
    int32_t l1 = c.iarg(3), l2 = c.iarg(5);
    std::string a = l1 < 0 ? read_cstr(c.mem(), c.arg(2)) : c.mem().read(c.arg(2), uint32_t(l1));
    std::string b = l2 < 0 ? read_cstr(c.mem(), c.arg(4)) : c.mem().read(c.arg(4), uint32_t(l2));
    c.ret(uint32_t(::CompareStringA(host_lcid(c.arg(0)), c.arg(1), a.data(), int(a.size()), b.data(), int(b.size()))));
  });
  r.impl(K, "CompareStringW", [host_lcid](Call& c) {
    auto rd = [&](uint32_t p, int32_t l) {
      std::wstring s;
      if (l < 0) {
        for (char16_t ch : read_wstr(c.mem(), p)) s.push_back(wchar_t(ch));
      } else {
        for (int32_t i = 0; i < l; i++) s.push_back(wchar_t(c.mem().read_u16l(p + 2 * uint32_t(i))));
      }
      return s;
    };
    std::wstring a = rd(c.arg(2), c.iarg(3)), b = rd(c.arg(4), c.iarg(5));
    c.ret(uint32_t(::CompareStringW(host_lcid(c.arg(0)), c.arg(1), a.data(), int(a.size()), b.data(), int(b.size()))));
  });
  r.impl(K, "GetLocaleInfoA", [host_lcid](Call& c) {
    char buf[256] = {};
    int n = ::GetLocaleInfoA(host_lcid(c.arg(0)), c.arg(1), buf, sizeof(buf));
    uint32_t cap = c.arg(3);
    if (!cap) return c.ret(uint32_t(n));
    if (uint32_t(n) > cap) return c.ret(0);
    c.mem().memcpy(c.arg(2), buf, size_t(n));
    c.ret(uint32_t(n));
  });
  r.impl(K, "GetLocaleInfoW", [host_lcid](Call& c) {
    wchar_t buf[256] = {};
    int n = ::GetLocaleInfoW(host_lcid(c.arg(0)), c.arg(1), buf, 256);
    uint32_t cap = c.arg(3);
    if (!cap) return c.ret(uint32_t(n));
    if (uint32_t(n) > cap) return c.ret(0);
    for (int i = 0; i < n; i++) c.mem().write_u16l(c.arg(2) + 2 * uint32_t(i), uint16_t(buf[i]));
    c.ret(uint32_t(n));
  });

  // ---- time (VirtualClock only; ABI.md §4: every read advances) ----
  r.impl(K, "GetTickCount", [](Call& c) { c.ret(c.rt.clock().read_tick_count()); });
  // The guest's UTC is the host's local time with a zero bias, so localtime()
  // in any CRT shows the host's wall-clock time (and headless runs a fixed one).
  auto systime = [](Call& c) {
    SYSTEMTIME st = filetime_to_systemtime(guest_local_filetime(c.rt));
    write_pod(c.mem(), c.arg(0), st);
  };
  r.impl(K, "GetLocalTime", systime);
  r.impl(K, "GetSystemTime", systime);
  r.impl(K, "GetTimeZoneInformation", [](Call& c) {
    TIME_ZONE_INFORMATION tz{};  // 172 bytes, no pointers: bias 0, no DST
    write_pod(c.mem(), c.arg(0), tz);
    c.ret(TIME_ZONE_ID_UNKNOWN);
  });
  r.impl(K, "FileTimeToLocalFileTime", [](Call& c) {
    c.mem().write_u64l(c.arg(1), c.mem().read_u64l(c.arg(0)));  // bias 0 (see above)
    c.ret(1);
  });
  r.impl(K, "LocalFileTimeToFileTime", [](Call& c) {
    c.mem().write_u64l(c.arg(1), c.mem().read_u64l(c.arg(0)));  // bias 0
    c.ret(1);
  });
  // Pure calendar conversions: the host's, which depend on nothing but the input.
  r.impl(K, "FileTimeToSystemTime", [](Call& c) {
    FILETIME ft = read_pod<FILETIME>(c.mem(), c.arg(0));
    SYSTEMTIME st{};
    if (!::FileTimeToSystemTime(&ft, &st)) {
      c.set_last_error(ERROR_INVALID_PARAMETER);
      return c.ret(0);
    }
    write_pod(c.mem(), c.arg(1), st);
    c.ret(1);
  });
  r.impl(K, "SystemTimeToFileTime", [](Call& c) {
    SYSTEMTIME st = read_pod<SYSTEMTIME>(c.mem(), c.arg(0));
    FILETIME ft{};
    if (!::SystemTimeToFileTime(&st, &ft)) {
      c.set_last_error(ERROR_INVALID_PARAMETER);
      return c.ret(0);
    }
    write_pod(c.mem(), c.arg(1), ft);
    c.ret(1);
  });
  r.impl(K, "FileTimeToDosDateTime", [](Call& c) {
    FILETIME ft = read_pod<FILETIME>(c.mem(), c.arg(0));
    WORD d = 0, t = 0;
    BOOL ok = ::FileTimeToDosDateTime(&ft, &d, &t);
    if (c.arg(1)) c.mem().write_u16l(c.arg(1), d);
    if (c.arg(2)) c.mem().write_u16l(c.arg(2), t);
    c.ret_bool(ok);
  });

  // ---- modules (modules.hh) ----
  r.impl(K, "GetModuleHandleA", [](Call& c) {
    uint32_t h = c.null(0) ? c.rt.modules().exe_handle() : c.rt.modules().handle_of(c.str(0));
    if (!h) c.set_last_error(ERROR_MOD_NOT_FOUND);
    c.ret(h);
  });
  r.impl(K, "GetModuleFileNameA", [](Call& c) {
    std::string name = c.rt.modules().file_name(c.arg(0));
    if (name.empty()) {
      c.set_last_error(ERROR_MOD_NOT_FOUND);
      return c.ret(0);
    }
    c.ret(uint32_t(write_cstr(c.mem(), c.arg(1), name, c.arg(2))));
  });
  r.impl(K, "GetProcAddress", [](Call& c) {
    uint32_t h = c.arg(0), n = c.arg(1);
    uint32_t a = n < 0x10000 ? c.rt.modules().proc_address(h, uint16_t(n)) : c.rt.modules().proc_address(h, c.str(1));
    if (!a) c.set_last_error(ERROR_PROC_NOT_FOUND);
    c.ret(a);
  });
  auto load_library = [](Call& c, uint32_t flags) {
    std::string name = c.str(0);
    ModuleTable& mt = c.rt.modules();
    // LOAD_LIBRARY_AS_DATAFILE (2): modules open their own .AD for resources.
    if ((flags & 2) && mt.find(name)) return c.ret(mt.find(name)->base);
    uint32_t h = 0;
    try {
      h = mt.load_library(name);
    } catch (const loader::LoaderError& e) {
      log("LoadLibrary(%s): %s", name.c_str(), e.what());
    }
    trace("module", "LoadLibrary(\"%s\") -> 0x%08X", name.c_str(), h);
    if (!h) c.set_last_error(ERROR_MOD_NOT_FOUND);
    c.ret(h);
  };
  r.impl(K, "LoadLibraryA", [load_library](Call& c) { load_library(c, 0); });
  r.impl(K, "LoadLibraryExA", [load_library](Call& c) { load_library(c, c.arg(2)); });
  r.impl(K, "FreeLibrary", [](Call& c) { c.ret_bool(c.rt.modules().free_library(c.arg(0))); });

  // ---- resources (the loaded image's resource tree) ----
  r.impl(K, "FindResourceA", [](Call& c) {
    uint32_t hmod = c.arg(0) ? c.arg(0) : c.rt.modules().exe_handle();
    loader::ResId name = res_id(c.mem(), c.arg(1)), type = res_id(c.mem(), c.arg(2));
    const loader::pe::Resource* res = find_module_resource(c.rt, hmod, type, name);
    if (!res) {
      c.set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
      return c.ret(0);
    }
    // An HRSRC is the address of the resource's IMAGE_RESOURCE_DATA_ENTRY
    // {OffsetToData, Size, CodePage, Reserved} in the mapped image, as on
    // Win32 — code relies on that: ADXPL510's ResourceHandle::GetSize passes
    // the LoadResource pointer to SizeofResource, which just reads +4.
    Module* m = c.rt.modules().by_handle(hmod);
    auto& entries = ks(c.rt).resource_entries;
    auto it = entries.find(m->base);
    if (it == entries.end()) it = entries.emplace(m->base, map_resource_entries(c.mem(), *m)).first;
    auto e = it->second.find(res->data_rva);
    if (e == it->second.end()) {
      c.set_last_error(ERROR_RESOURCE_DATA_NOT_FOUND);
      return c.ret(0);
    }
    c.ret(e->second);
  });
  r.impl(K, "LoadResource", [](Call& c) {
    uint32_t hrsrc = c.arg(1);
    if (!hrsrc) return c.ret(0);
    // Relative to the module the entry lives in (hModule may be NULL for the EXE).
    Module* m = c.rt.modules().containing(hrsrc);
    uint32_t base = m ? m->base : c.arg(0);
    c.ret(base + c.mem().read_u32l(hrsrc));  // the HGLOBAL is the data's address, as on Win32
  });
  r.impl(K, "LockResource", [](Call& c) { c.ret(c.arg(0)); });
  r.impl(K, "FreeResource", [](Call& c) { c.ret(0); });
  r.impl(K, "SizeofResource", [](Call& c) {
    // Win32 reads the entry's Size without validating the handle; so do we
    // (an unmapped handle reads as 0, as Win32's guarded read returns).
    uint32_t hrsrc = c.arg(1);
    c.ret(hrsrc && c.mem().exists(hrsrc + 4, 4) ? c.mem().read_u32l(hrsrc + 4) : 0);
  });

  // ---- directories and system information ----
  r.impl(K, "GetWindowsDirectoryA", [](Call& c) {
    std::string d = "C:\\WINDOWS";
    if (c.arg(1) <= d.size()) return c.ret(uint32_t(d.size() + 1));
    c.ret(uint32_t(write_cstr(c.mem(), c.arg(0), d, c.arg(1))));
  });
  r.impl(K, "GetSystemDirectoryA", [](Call& c) {
    std::string d = "C:\\WINDOWS\\SYSTEM";
    if (c.arg(1) <= d.size()) return c.ret(uint32_t(d.size() + 1));
    c.ret(uint32_t(write_cstr(c.mem(), c.arg(0), d, c.arg(1))));
  });
  r.impl(K, "SetCurrentDirectoryA", [](Call& c) {
    bool ok = c.rt.vfs().set_cwd(c.str(0));
    if (!ok) c.set_last_error(ERROR_PATH_NOT_FOUND);
    c.ret_bool(ok);
  });
  r.impl(K, "GetDriveTypeA", [](Call& c) { c.ret(DRIVE_FIXED); });
  r.impl(K, "GetComputerNameA", [](Call& c) {
    std::string n = "AFTERDARK";
    uint32_t cap = c.mem().read_u32l(c.arg(1));
    if (cap <= n.size()) {
      c.mem().write_u32l(c.arg(1), uint32_t(n.size() + 1));
      c.set_last_error(ERROR_BUFFER_OVERFLOW);
      return c.ret(0);
    }
    write_cstr(c.mem(), c.arg(0), n, cap);
    c.mem().write_u32l(c.arg(1), uint32_t(n.size()));
    c.ret(1);
  });
  r.impl(K, "GetVolumeInformationA", [](Call& c) {
    // (root, volname, volname_size, serial*, maxcomp*, flags*, fsname, fsname_size)
    if (c.arg(1)) write_cstr(c.mem(), c.arg(1), "AFTERDARK", c.arg(2));
    if (c.arg(3)) c.mem().write_u32l(c.arg(3), 0x19960912);
    if (c.arg(4)) c.mem().write_u32l(c.arg(4), 255);
    if (c.arg(5)) c.mem().write_u32l(c.arg(5), FS_CASE_IS_PRESERVED);
    if (c.arg(6)) write_cstr(c.mem(), c.arg(6), "FAT", c.arg(7));
    c.ret(1);
  });

  // ---- profile strings (IniStore over the Vfs) ----
  r.impl(K, "GetProfileStringA", [](Call& c) {
    c.ret(get_profile_string(c.rt, "WIN.INI", c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4)));
  });
  r.impl(K, "GetPrivateProfileStringA", [](Call& c) {
    c.ret(get_profile_string(c.rt, c.str(5), c.arg(0), c.arg(1), c.arg(2), c.arg(3), c.arg(4)));
  });
  r.impl(K, "WritePrivateProfileStringA", [](Call& c) {
    bool ok = write_profile_string(c.rt, c.str(3), c.arg(0), c.arg(1), c.arg(2));
    if (!ok) c.set_last_error(ERROR_ACCESS_DENIED);
    c.ret_bool(ok);
  });
  // Reached only through GetProcAddress in our corpus; the same store.
  r.add(K, "WriteProfileStringA", Conv::stdcall_, 12, [](Call& c) {
    bool ok = write_profile_string(c.rt, "WIN.INI", c.arg(0), c.arg(1), c.arg(2));
    if (!ok) c.set_last_error(ERROR_ACCESS_DENIED);
    c.ret_bool(ok);
  });
  r.add(K, "GetPrivateProfileIntA", Conv::stdcall_, 16,
        [](Call& c) { c.ret(get_profile_int(c.rt, c.str(3), c.str(0), c.str(1), c.iarg(2))); });
  r.add(K, "GetProfileIntA", Conv::stdcall_, 12,
        [](Call& c) { c.ret(get_profile_int(c.rt, "WIN.INI", c.str(0), c.str(1), c.iarg(2))); });

  // ---- files (Vfs: overlays over the module's folders, INTERACTION.md §7) ----
  r.impl(K, "GetStdHandle", [](Call& c) {
    int32_t n = c.iarg(0);
    c.ret(n == -10 ? kHandleBase : n == -11 ? kHandleBase + kHandleStep : n == -12 ? kHandleBase + 2 * kHandleStep
                                                                                 : 0xFFFFFFFF);
  });
  r.impl(K, "SetStdHandle", [](Call& c) { c.ret(1); });
  r.impl(K, "GetFileType", [](Call& c) {
    KObject* o = ks(c.rt).get(c.arg(0));
    if (!o) return c.ret(FILE_TYPE_UNKNOWN);
    c.ret(o->kind == KObject::Kind::file ? FILE_TYPE_DISK : FILE_TYPE_CHAR);
  });
  // Opens through the Vfs; 0xFFFFFFFF (INVALID_HANDLE_VALUE / HFILE_ERROR) on failure.
  auto open_file = [](Call& c, const std::string& guest, Vfs::Access access, Vfs::Disposition disp,
                      bool report_exists) -> uint32_t {
    Vfs& vfs = c.rt.vfs();
    uint32_t err = 0;
    std::unique_ptr<VfsFile> f = vfs.open(guest, access, disp, &err);
    bool write = access != Vfs::Access::read || disp != Vfs::Disposition::open_existing;
    trace("file", "open \"%s\" -> \"%s\"%s: %s", guest.c_str(), vfs.to_host(guest).c_str(),
          write ? " for writing" : "", f ? "ok" : "failed");
    if (!f) {
      trace("file", "open \"%s\" failed (error %u)", guest.c_str(), err);
      c.set_last_error(err);
      return 0xFFFFFFFF;
    }
    // CreateFile reports whether CREATE_ALWAYS / OPEN_ALWAYS found the file.
    if (report_exists) c.set_last_error(err);
    KObject o;
    o.kind = KObject::Kind::file;
    o.guest_path = f->guest_path();
    o.file = std::move(f);
    uint32_t handle = ks(c.rt).add(std::move(o));
    trace("file", "open \"%s\" ok (handle 0x%X)", guest.c_str(), handle);
    return handle;
  };
  r.impl(K, "CreateFileA", [open_file](Call& c) {
    uint32_t access = c.arg(1), disposition = c.arg(4);
    bool write = access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA);
    bool read = access & (GENERIC_READ | GENERIC_ALL | GENERIC_EXECUTE | FILE_READ_DATA);
    Vfs::Access a = write ? (read ? Vfs::Access::read_write : Vfs::Access::write) : Vfs::Access::read;
    Vfs::Disposition d;
    switch (disposition) {
      case CREATE_NEW: d = Vfs::Disposition::create_new; break;
      case CREATE_ALWAYS: d = Vfs::Disposition::create_always; break;
      case OPEN_EXISTING: d = Vfs::Disposition::open_existing; break;
      case OPEN_ALWAYS: d = Vfs::Disposition::open_always; break;
      case TRUNCATE_EXISTING: d = Vfs::Disposition::truncate_existing; break;
      default:
        c.set_last_error(ERROR_INVALID_PARAMETER);
        return c.ret(0xFFFFFFFF);
    }
    c.ret(open_file(c, c.str(0), a, d, disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS));
  });
  auto lopen_access = [](uint32_t mode) {
    return (mode & 3) == 1 ? Vfs::Access::write : (mode & 3) == 2 ? Vfs::Access::read_write : Vfs::Access::read;
  };
  r.impl(K, "_lopen", [open_file, lopen_access](Call& c) {
    c.ret(open_file(c, c.str(0), lopen_access(c.arg(1)), Vfs::Disposition::open_existing, false));
  });
  r.impl(K, "_lcreat", [open_file](Call& c) {
    c.ret(open_file(c, c.str(0), Vfs::Access::read_write, Vfs::Disposition::create_always, false));
  });
  r.impl(K, "OpenFile", [open_file, lopen_access](Call& c) {
    // (name, OFSTRUCT*, style); OFSTRUCT = cBytes, fFixedDisk, nErrCode, 2 reserved WORDs, szPathName[128].
    std::string name = c.str(0);
    uint32_t of = c.arg(1), style = c.arg(2);
    Vfs& vfs = c.rt.vfs();
    std::string full = vfs.full_path(name);
    if (of) {
      c.mem().memset(of, 0, 136);
      c.mem().write_u8(of, 136);
      c.mem().write_u8(of + 1, 1);
      write_cstr(c.mem(), of + 8, full, 128);
    }
    auto fail = [&](uint32_t e) {
      if (of) c.mem().write_u16l(of + 2, uint16_t(e));
      c.set_last_error(e);
      return c.ret(0xFFFFFFFF);
    };
    if (style & OF_PARSE) return c.ret(0);
    if (style & OF_DELETE) {
      uint32_t e = 0;
      if (!vfs.remove(full, &e)) return fail(e);
      return c.ret(1);
    }
    if (style & OF_EXIST) {
      Vfs::Stat st;
      bool exists = vfs.stat(full, &st) && !st.dir;
      trace("file", "OpenFile(OF_EXIST) \"%s\": %s", full.c_str(), exists ? "found" : "not found");
      if (!exists) return fail(ERROR_FILE_NOT_FOUND);
      return c.ret(1);
    }
    Vfs::Access a = lopen_access(style);
    if (style & OF_CREATE) a = a == Vfs::Access::read ? Vfs::Access::read_write : a;
    uint32_t h = open_file(c, full, a, (style & OF_CREATE) ? Vfs::Disposition::create_always : Vfs::Disposition::open_existing,
                           false);
    if (h == 0xFFFFFFFF && of) c.mem().write_u16l(of + 2, uint16_t(c.rt.last_error()));
    c.ret(h);
  });
  auto read_file = [](Call& c, uint32_t h, uint32_t buf, uint32_t n) -> int64_t {
    VfsFile* f = ks(c.rt).file(h);
    if (!f) {
      c.set_last_error(ERROR_INVALID_HANDLE);
      return -1;
    }
    // One 64 KiB piece at a time: the count is the guest's (up to 4 GB), so
    // it never sizes a host allocation, and the read stops at the end of the file.
    std::vector<char> tmp(std::min<uint32_t>(n, 0x10000));
    uint32_t done = 0;
    while (done < n) {
      uint32_t chunk = std::min<uint32_t>(n - done, 0x10000);
      int64_t got = f->read(tmp.data(), chunk);
      if (got < 0) {
        if (done) break;
        c.set_last_error(ERROR_READ_FAULT);
        return -1;
      }
      if (got) c.mem().memcpy(buf + done, tmp.data(), size_t(got));
      done += uint32_t(got);
      if (uint32_t(got) < chunk) break;
    }
    return done;
  };
  r.impl(K, "ReadFile", [read_file](Call& c) {
    int64_t got = read_file(c, c.arg(0), c.arg(1), c.arg(2));
    if (c.arg(3)) c.mem().write_u32l(c.arg(3), got < 0 ? 0 : uint32_t(got));
    c.ret_bool(got >= 0);
  });
  r.impl(K, "_lread", [read_file](Call& c) { c.ret(uint32_t(read_file(c, c.arg(0), c.arg(1), c.arg(2)))); });
  r.impl(K, "_hread", [read_file](Call& c) { c.ret(uint32_t(read_file(c, c.arg(0), c.arg(1), c.arg(2)))); });
  // Bytes from guest memory into an open file; -1 (last error set) on failure.
  auto write_file = [](Call& c, uint32_t h, uint32_t buf, uint32_t n) -> int64_t {
    VfsFile* f = ks(c.rt).file(h);
    if (!f) {
      c.set_last_error(ERROR_INVALID_HANDLE);
      return -1;
    }
    if (!f->writable()) {
      c.set_last_error(ERROR_ACCESS_DENIED);
      return -1;
    }
    // One 64 KiB piece at a time (the count is the guest's). A file that
    // would outgrow Vfs::kMaxFileSize takes what fits: the disk is full.
    uint32_t done = 0;
    while (done < n) {
      uint32_t chunk = std::min<uint32_t>(n - done, 0x10000);
      std::string bytes = c.mem().read(buf + done, chunk);
      int64_t put = f->write(bytes.data(), chunk);
      if (put < 0) {
        if (done) break;
        c.set_last_error(ERROR_WRITE_FAULT);
        return -1;
      }
      done += uint32_t(put);
      if (uint32_t(put) < chunk) {
        c.set_last_error(ERROR_DISK_FULL);
        break;
      }
    }
    return done;
  };
  r.impl(K, "WriteFile", [write_file](Call& c) {
    auto& s = ks(c.rt);
    KObject* o = s.get(c.arg(0));
    uint32_t n = c.arg(2);
    if (o && (o->kind == KObject::Kind::std_out || o->kind == KObject::Kind::std_err)) {
      // The Borland RTL's fatal-error path writes here: show it in the log.
      std::string text = c.mem().read(c.arg(1), n);
      for (char ch : text) {
        if (ch == '\n') {
          log("[guest] %s", s.std_line.c_str());
          s.std_line.clear();
        } else if (ch != '\r') {
          s.std_line.push_back(ch);
        }
      }
      if (c.arg(3)) c.mem().write_u32l(c.arg(3), n);
      return c.ret(1);
    }
    int64_t put = write_file(c, c.arg(0), c.arg(1), n);
    if (c.arg(3)) c.mem().write_u32l(c.arg(3), put < 0 ? 0 : uint32_t(put));
    c.ret_bool(put == int64_t(n));  // short: ERROR_DISK_FULL
  });
  for (const char* n : {"_lwrite", "_hwrite"}) {
    r.impl(K, n, [write_file](Call& c) {
      // A zero-byte write truncates the file at its pointer (as in Win16).
      if (c.arg(2) == 0) {
        VfsFile* f = ks(c.rt).file(c.arg(0));
        if (!f || !f->truncate()) {
          c.set_last_error(f ? ERROR_ACCESS_DENIED : ERROR_INVALID_HANDLE);
          return c.ret(0xFFFFFFFF);
        }
        return c.ret(0);
      }
      int64_t put = write_file(c, c.arg(0), c.arg(1), c.arg(2));
      c.ret(put < 0 ? 0xFFFFFFFF : uint32_t(put));
    });
  }
  r.impl(K, "SetEndOfFile", [](Call& c) {
    VfsFile* f = ks(c.rt).file(c.arg(0));
    if (!f || !f->truncate()) {
      c.set_last_error(f ? ERROR_ACCESS_DENIED : ERROR_INVALID_HANDLE);
      return c.ret(0);
    }
    c.ret(1);
  });
  r.impl(K, "FlushFileBuffers", [](Call& c) {
    VfsFile* f = ks(c.rt).file(c.arg(0));
    c.ret_bool(!f || f->flush());
  });
  r.impl(K, "DeleteFileA", [](Call& c) {
    uint32_t err = 0;
    bool ok = c.rt.vfs().remove(c.str(0), &err);
    trace("file", "DeleteFile \"%s\": %s", c.str(0).c_str(), ok ? "ok" : "failed");
    if (!ok) c.set_last_error(err);
    c.ret_bool(ok);
  });
  auto seek = [](Call& c, uint32_t h, int64_t dist, uint32_t method) -> int64_t {
    VfsFile* f = ks(c.rt).file(h);
    if (!f) {
      c.set_last_error(ERROR_INVALID_HANDLE);
      return -1;
    }
    int whence = method == FILE_BEGIN ? SEEK_SET : method == FILE_CURRENT ? SEEK_CUR : SEEK_END;
    int64_t pos = f->seek(dist, whence);
    if (pos < 0) c.set_last_error(ERROR_NEGATIVE_SEEK);
    return pos;
  };
  r.impl(K, "SetFilePointer", [seek](Call& c) {
    int64_t dist = c.iarg(1);
    if (c.arg(2)) dist = int64_t((uint64_t(c.mem().read_u32l(c.arg(2))) << 32) | c.arg(1));
    int64_t pos = seek(c, c.arg(0), dist, c.arg(3));
    if (pos < 0) return c.ret(0xFFFFFFFF);
    if (c.arg(2)) c.mem().write_u32l(c.arg(2), uint32_t(uint64_t(pos) >> 32));
    c.ret(uint32_t(pos));
  });
  r.impl(K, "_llseek", [seek](Call& c) {
    int64_t pos = seek(c, c.arg(0), c.iarg(1), c.arg(2));
    c.ret(pos < 0 ? 0xFFFFFFFF : uint32_t(pos));
  });
  r.impl(K, "GetFileSize", [](Call& c) {
    VfsFile* f = ks(c.rt).file(c.arg(0));
    if (!f) {
      c.set_last_error(ERROR_INVALID_HANDLE);
      return c.ret(0xFFFFFFFF);
    }
    uint64_t sz = f->size();
    if (c.arg(1)) c.mem().write_u32l(c.arg(1), uint32_t(sz >> 32));
    c.ret(uint32_t(sz));
  });
  // (hFile, lpCreationTime, lpLastAccessTime, lpLastWriteTime): the host
  // file's times, unconverted, as FindFirstFileA reports them. adimport stamps
  // each copy with its source's date (the disc's), so they are the same on
  // every machine that imported the same disc. Memory-upper files carry the
  // Vfs's fixed 1996 clock.
  r.impl(K, "GetFileTime", [](Call& c) {
    VfsFile* f = ks(c.rt).file(c.arg(0));
    if (!f) {
      c.set_last_error(ERROR_INVALID_HANDLE);
      return c.ret(0);
    }
    uint64_t t[3] = {0, 0, 0};
    f->times(&t[0], &t[1], &t[2]);
    for (int i = 0; i < 3; i++)
      if (c.arg(1 + i)) c.mem().write_u64l(c.arg(1 + i), t[i]);
    trace("file", "GetFileTime \"%s\" -> write %016llX", f->guest_path().c_str(), (unsigned long long)t[2]);
    c.ret(1);
  });
  // Our files keep the times the Vfs gives them; a writable file accepts the
  // call (and ignores it), a read-only one refuses it.
  r.impl(K, "SetFileTime", [](Call& c) {
    VfsFile* f = ks(c.rt).file(c.arg(0));
    if (!f || !f->writable()) {
      c.set_last_error(f ? ERROR_ACCESS_DENIED : ERROR_INVALID_HANDLE);
      return c.ret(0);
    }
    c.ret(1);
  });
  r.impl(K, "CopyFileA", [](Call& c) {
    // (existing, new, bFailIfExists)
    Vfs& vfs = c.rt.vfs();
    std::string from = c.str(0), to = c.str(1);
    uint32_t err = 0;
    std::string bytes;
    Vfs::Stat st;
    if (!vfs.stat(from, &st) || st.dir || !vfs.read_file(from, &bytes)) {
      c.set_last_error(vfs.is_dir(from) ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND);
      return c.ret(0);
    }
    auto f = vfs.open(to, Vfs::Access::write,
                      c.arg(2) ? Vfs::Disposition::create_new : Vfs::Disposition::create_always, &err);
    bool ok = f && (bytes.empty() || f->write(bytes.data(), uint32_t(bytes.size())) == int64_t(bytes.size())) &&
              f->flush();
    trace("file", "CopyFile \"%s\" -> \"%s\": %s", from.c_str(), to.c_str(), ok ? "ok" : "failed");
    if (!ok) c.set_last_error(err ? err : ERROR_WRITE_FAULT);
    c.ret_bool(ok);
  });
  r.impl(K, "MoveFileA", [](Call& c) {
    uint32_t err = 0;
    bool ok = c.rt.vfs().rename(c.str(0), c.str(1), &err);
    trace("file", "MoveFile \"%s\" -> \"%s\": %s", c.str(0).c_str(), c.str(1).c_str(), ok ? "ok" : "failed");
    if (!ok) c.set_last_error(err);
    c.ret_bool(ok);
  });
  r.impl(K, "RemoveDirectoryA", [](Call& c) {
    uint32_t err = 0;
    bool ok = c.rt.vfs().remove_dir(c.str(0), &err);
    trace("file", "RemoveDirectory \"%s\": %s", c.str(0).c_str(), ok ? "ok" : "failed");
    if (!ok) c.set_last_error(err);
    c.ret_bool(ok);
  });
  r.impl(K, "CreateDirectoryA", [](Call& c) {
    uint32_t err = 0;
    bool ok = c.rt.vfs().make_dir(c.str(0), &err);
    trace("file", "CreateDirectory \"%s\": %s", c.str(0).c_str(), ok ? "ok" : err == ERROR_ALREADY_EXISTS ? "already exists" : "failed");
    if (!ok) c.set_last_error(err);
    c.ret_bool(ok);
  });
  r.impl(K, "CloseHandle", [](Call& c) {
    bool ok = ks(c.rt).close(c.arg(0));
    if (!ok) c.set_last_error(ERROR_INVALID_HANDLE);
    c.ret_bool(ok);
  });
  r.impl(K, "_lclose", [](Call& c) { c.ret(ks(c.rt).close(c.arg(0)) ? 0 : 0xFFFFFFFF); });
  r.impl(K, "GetFileAttributesA", [](Call& c) {
    Vfs& vfs = c.rt.vfs();
    std::string path = c.str(0);
    Vfs::Stat st;
    bool found = vfs.stat(path, &st);
    trace("file", "GetFileAttributes \"%s\" -> \"%s\": %s", path.c_str(), vfs.to_host(path).c_str(),
          found ? "found" : "not found");
    if (!found) {
      std::string parent = vfs.full_path(path);
      size_t s = parent.find_last_of('\\');
      bool dir_ok = s != std::string::npos && vfs.is_dir(s <= 2 ? parent.substr(0, 3) : parent.substr(0, s));
      c.set_last_error(dir_ok ? ERROR_FILE_NOT_FOUND : ERROR_PATH_NOT_FOUND);
      return c.ret(INVALID_FILE_ATTRIBUTES);
    }
    uint32_t a = st.attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                  FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ARCHIVE);
    c.ret(a ? a : FILE_ATTRIBUTE_NORMAL);
  });
  // WIN32_FIND_DATAA: attributes, 3 FILETIMEs, size hi/lo, 2 reserved,
  // cFileName[260], cAlternateFileName[14] (empty when the name is 8.3 already,
  // as Windows 95 leaves it).
  auto write_find = [](Call& c, uint32_t out, const Vfs::DirEntry& e) {
    auto& mem = c.mem();
    mem.memset(out, 0, 320);
    mem.write_u32l(out, e.attributes);
    mem.write_u64l(out + 4, e.write_time);
    mem.write_u64l(out + 12, e.write_time);
    mem.write_u64l(out + 20, e.write_time);
    mem.write_u32l(out + 28, uint32_t(e.size >> 32));
    mem.write_u32l(out + 32, uint32_t(e.size));
    write_cstr(mem, out + 44, e.name, 260);
    if (!Vfs::is_short_name(e.name)) write_cstr(mem, out + 304, e.short_name, 14);
  };
  r.impl(K, "FindFirstFileA", [write_find](Call& c) {
    Vfs& vfs = c.rt.vfs();
    std::string pattern = vfs.full_path(c.str(0));
    size_t s = pattern.find_last_of('\\');
    std::string dir = s <= 2 ? pattern.substr(0, 3) : pattern.substr(0, s);
    std::string name = pattern.substr(s + 1);
    trace("file", "FindFirstFile \"%s\" -> \"%s\"", c.str(0).c_str(), vfs.to_host(dir).c_str());
    if (!vfs.is_dir(dir)) {
      c.set_last_error(ERROR_PATH_NOT_FOUND);
      return c.ret(0xFFFFFFFF);
    }
    std::vector<Vfs::DirEntry> found = vfs.list(dir, name.empty() ? "*" : name);
    if (found.empty()) {
      c.set_last_error(ERROR_FILE_NOT_FOUND);
      return c.ret(0xFFFFFFFF);
    }
    write_find(c, c.arg(1), found[0]);
    KObject o;
    o.kind = KObject::Kind::find;
    o.found = std::move(found);
    o.next = 1;
    o.guest_path = dir;
    c.ret(ks(c.rt).add(std::move(o)));
  });
  r.impl(K, "FindNextFileA", [write_find](Call& c) {
    KObject* o = ks(c.rt).get(c.arg(0));
    if (!o || o->kind != KObject::Kind::find) {
      c.set_last_error(ERROR_INVALID_HANDLE);
      return c.ret(0);
    }
    if (o->next >= o->found.size()) {
      c.set_last_error(ERROR_NO_MORE_FILES);
      return c.ret(0);
    }
    write_find(c, c.arg(1), o->found[o->next++]);
    c.ret(1);
  });
  r.impl(K, "FindClose", [](Call& c) {
    KObject* o = ks(c.rt).get(c.arg(0));
    if (!o || o->kind != KObject::Kind::find) {
      c.set_last_error(ERROR_INVALID_HANDLE);
      return c.ret(0);
    }
    c.ret_bool(ks(c.rt).close(c.arg(0)));
  });
}

// What a whole program adds (Screen Wreckers' DTTY3203.EXE, MFC 4.2; ABI.md
// §2.15): names only program_signatures() has, so only a program's registry
// gets them (shim_families.hh register_program_shims).
void register_kernel32_program(ShimRegistry& r) {
  r.impl(K, "SetUnhandledExceptionFilter", [](Call& c) {
    // The filter is kept and handed back; an unhandled exception still stops
    // the program (seh.hh), as its default filter's dialog would have.
    uint32_t prev = ks(c.rt).unhandled_filter;
    ks(c.rt).unhandled_filter = c.arg(0);
    c.ret(prev);
  });
  r.impl(K, "SetErrorMode", [](Call& c) {
    uint32_t prev = ks(c.rt).error_mode;
    ks(c.rt).error_mode = c.arg(0);
    c.ret(prev);
  });
  r.impl(K, "GetCurrentThread", [](Call& c) { c.ret(0xFFFFFFFE); });  // the pseudo-handle
  r.impl(K, "GetProcessVersion", [](Call& c) {
    // The Windows version the process expects (0 = this one): Windows 95's 4.0.
    if (c.arg(0) && c.arg(0) != c.mem().read_u32l(c.rt.teb() + 0x20)) return c.ret(0);
    c.ret(0x00040000);
  });
  r.impl(K, "GetThreadLocale", [](Call& c) { c.ret(0x0409); });
  r.impl(K, "InterlockedIncrement", [](Call& c) {
    uint32_t v = c.mem().read_u32l(c.arg(0)) + 1;
    c.mem().write_u32l(c.arg(0), v);
    c.ret(v);
  });
  r.impl(K, "InterlockedDecrement", [](Call& c) {
    uint32_t v = c.mem().read_u32l(c.arg(0)) - 1;
    c.mem().write_u32l(c.arg(0), v);
    c.ret(v);
  });
  r.impl(K, "lstrcpynA", [](Call& c) {
    // At most n-1 characters and a NUL; n 0 copies nothing.
    uint32_t n = c.arg(2);
    if (n) write_cstr(c.mem(), c.arg(0), c.str(1), n);
    c.ret(c.arg(0));
  });
  auto cmp = [](Call& c, bool nocase) {
    // Windows' word sort (CompareStringA, en-US), as lstrcmp(i)A used it.
    std::string a = c.str(0), b = c.str(1);
    int r2 = ::CompareStringA(0x0409, nocase ? NORM_IGNORECASE : 0, a.c_str(), -1, b.c_str(), -1);
    c.ret(uint32_t(r2 - 2));
  };
  r.impl(K, "lstrcmpA", [cmp](Call& c) { cmp(c, false); });
  r.impl(K, "lstrcmpiA", [cmp](Call& c) { cmp(c, true); });
  r.impl(K, "MulDiv", [](Call& c) {
    // a * b / d rounded half away from zero; -1 for d 0 or an overflow.
    int64_t a = c.iarg(0), b = c.iarg(1), d = c.iarg(2);
    if (!d) return c.ret(0xFFFFFFFF);
    int64_t p = a * b, ad = d < 0 ? -d : d;
    int64_t q = ((p < 0) != (d < 0)) ? -((std::llabs(p) + ad / 2) / ad) : (std::llabs(p) + ad / 2) / ad;
    if (q > INT32_MAX || q < INT32_MIN) return c.ret(0xFFFFFFFF);
    c.ret(uint32_t(int32_t(q)));
  });
  for (const char* n : {"IsBadReadPtr", "IsBadWritePtr"}) {
    r.impl(K, n, [](Call& c) {
      // (lp, ucb): TRUE when any byte is unmapped.
      uint32_t p = c.arg(0), size = c.arg(1);
      if (!size) return c.ret(0);
      c.ret_bool(!p || !c.mem().exists(p, size));
    });
  }
  r.impl(K, "IsBadCodePtr", [](Call& c) { c.ret_bool(!c.arg(0) || !c.mem().exists(c.arg(0), 1)); });
  r.impl(K, "GetCurrentDirectoryA", [](Call& c) {
    // (nBufferLength, lpBuffer): the length, or the size needed with its NUL.
    std::string d = c.rt.vfs().cwd();
    if (c.arg(0) <= d.size()) return c.ret(uint32_t(d.size() + 1));
    c.ret(uint32_t(write_cstr(c.mem(), c.arg(1), d, c.arg(0))));
  });
  r.impl(K, "GetFullPathNameA", [](Call& c) {
    // (lpFileName, nBufferLength, lpBuffer, lpFilePart)
    std::string full = c.rt.vfs().full_path(c.str(0));
    if (c.arg(1) <= full.size()) return c.ret(uint32_t(full.size() + 1));
    write_cstr(c.mem(), c.arg(2), full, c.arg(1));
    if (c.arg(3)) {
      size_t s = full.find_last_of('\\');
      c.mem().write_u32l(c.arg(3), s == std::string::npos || s + 1 == full.size() ? 0 : c.arg(2) + uint32_t(s + 1));
    }
    c.ret(uint32_t(full.size()));
  });
  r.impl(K, "GetShortPathNameA", [](Call& c) {
    // (long, short, cch): the path as it exists (the guest's install folder
    // and its files have 8.3 names already).
    std::string p = c.str(0);
    if (!c.rt.vfs().exists(p)) {
      c.set_last_error(ERROR_FILE_NOT_FOUND);
      return c.ret(0);
    }
    if (c.arg(2) <= p.size()) return c.ret(uint32_t(p.size() + 1));
    c.ret(uint32_t(write_cstr(c.mem(), c.arg(1), p, c.arg(2))));
  });
  r.impl(K, "GetTempFileNameA", [](Call& c) {
    // (path, prefix, unique, out): <path>\<pre>XXXX.TMP, created when unique is 0.
    std::string dir = c.str(0), pre = c.str(1).substr(0, 3);
    uint32_t u = c.arg(2) & 0xFFFF;
    Vfs& vfs = c.rt.vfs();
    for (uint32_t tries = 0; tries < 0xFFFF; tries++) {
      uint32_t n = u ? u : tries + 1;
      char b[16];
      snprintf(b, sizeof(b), "%X.TMP", n);
      std::string path = dir + (dir.empty() || dir.back() == '\\' ? "" : "\\") + pre + b;
      if (!u) {
        if (vfs.exists(path)) continue;
        uint32_t err = 0;
        if (!vfs.write_file(path, "", &err)) {
          c.set_last_error(err);
          return c.ret(0);
        }
      }
      write_cstr(c.mem(), c.arg(3), path, MAX_PATH);
      return c.ret(n);
    }
    c.ret(0);
  });
  r.impl(K, "GetDiskFreeSpaceA", [](Call& c) {
    // (root, sectors/cluster, bytes/sector, free clusters, clusters): a 1996
    // disk of 1 GB with 500 MB free, in 32 KB clusters.
    if (c.arg(1)) c.mem().write_u32l(c.arg(1), 64);
    if (c.arg(2)) c.mem().write_u32l(c.arg(2), 512);
    if (c.arg(3)) c.mem().write_u32l(c.arg(3), 16000);
    if (c.arg(4)) c.mem().write_u32l(c.arg(4), 32000);
    c.ret(1);
  });
  r.impl(K, "GetStringTypeExA", [](Call& c) {
    // (Locale, dwInfoType, lpSrcStr, cchSrc, lpCharType): en-US, as GetStringTypeA.
    int32_t len = c.iarg(3);
    std::string in = len < 0 ? read_cstr(c.mem(), c.arg(2)) + '\0' : c.mem().read(c.arg(2), uint32_t(len));
    std::vector<WORD> types(in.size());
    BOOL ok = ::GetStringTypeExA(0x0409, c.arg(1), in.data(), int(in.size()), types.data());
    for (size_t i = 0; i < types.size(); i++) c.mem().write_u16l(c.arg(4) + 2 * uint32_t(i), types[i]);
    c.ret_bool(ok);
  });
  r.impl(K, "DuplicateHandle", [](Call& c) {
    // (srcProcess, src, dstProcess, lpDst, access, inherit, options): the
    // process and thread pseudo-handles become themselves; anything else fails.
    uint32_t src = c.arg(1);
    if (src != 0xFFFFFFFF && src != 0xFFFFFFFE) {
      c.set_last_error(ERROR_INVALID_HANDLE);
      return c.ret(0);
    }
    if (c.arg(3)) c.mem().write_u32l(c.arg(3), src);
    c.ret(1);
  });
  for (const char* n : {"LockFile", "UnlockFile"}) {
    // One process: every region is free.
    r.impl(K, n, [](Call& c) {
      if (!ks(c.rt).file(c.arg(0))) {
        c.set_last_error(ERROR_INVALID_HANDLE);
        return c.ret(0);
      }
      c.ret(1);
    });
  }
  // Atoms: the global table, case-insensitive; "#123" and MAKEINTATOM are integer atoms.
  r.impl(K, "GlobalAddAtomA", [](Call& c) {
    auto& atoms = ks(c.rt).atoms;
    uint32_t p = c.arg(0);
    if (p < 0x10000) return c.ret(p && p < 0xC000 ? p : 0);
    std::string name = c.str(0);
    if (!name.empty() && name[0] == '#') {
      uint32_t v = uint32_t(strtoul(name.c_str() + 1, nullptr, 10));
      return c.ret(v && v < 0xC000 ? v : 0);
    }
    for (size_t i = 0; i < atoms.size(); i++) {
      if (atoms[i].second && _stricmp(atoms[i].first.c_str(), name.c_str()) == 0) {
        atoms[i].second++;
        return c.ret(0xC000 + uint32_t(i));
      }
    }
    atoms.push_back({name, 1});
    c.ret(0xC000 + uint32_t(atoms.size() - 1));
  });
  r.impl(K, "GlobalDeleteAtom", [](Call& c) {
    auto& atoms = ks(c.rt).atoms;
    uint32_t a = c.arg(0) & 0xFFFF;
    if (a >= 0xC000 && a - 0xC000 < atoms.size() && atoms[a - 0xC000].second) atoms[a - 0xC000].second--;
    c.ret(0);
  });
  r.impl(K, "GlobalGetAtomNameA", [](Call& c) {
    // (atom, buffer, size): the name's length, 0 when there is no such atom.
    auto& atoms = ks(c.rt).atoms;
    uint32_t a = c.arg(0) & 0xFFFF;
    std::string name;
    if (a && a < 0xC000) name = "#" + std::to_string(a);
    else if (a >= 0xC000 && a - 0xC000 < atoms.size() && atoms[a - 0xC000].second) name = atoms[a - 0xC000].first;
    if (name.empty() || !c.arg(2)) return c.ret(0);
    c.ret(uint32_t(write_cstr(c.mem(), c.arg(1), name, c.arg(2))));
  });
}

IniStore& profile_store(Runtime& rt) { return ks(rt).ini; }

void close_guest_files(Runtime& rt) {
  Kernel32State& s = ks(rt);
  for (size_t i = 3; i < s.objects.size(); i++)
    if (s.objects[i].kind == KObject::Kind::file || s.objects[i].kind == KObject::Kind::find) s.objects[i] = KObject{};
}

}  // namespace adw::win32
