// The process's module list: PE images we load and run as emulated code
// (the .AD module, ADXPL510.DLL), the stand-in host EXE at 0x400000, and the
// DLLs we emulate with shims (KERNEL32, USER32, …), which get pseudo HMODULEs
// with a real MZ/PE header page so code that inspects a module base finds one.
//
// Loading (ABI.md §2.1): an image is placed at its preferred base when that
// range is free — never for the AD images, which all prefer the host EXE's
// 0x400000 — else relocated to the first free 64K-aligned range of the image
// area. Its imports are bound: a DLL we run as code (found in the search
// directories and not a system DLL) is loaded recursively and bound by export
// name/ordinal; every other DLL is bound to shim thunks. DllMain runs in
// dependency order (engine before module) and DLL_PROCESS_ATTACH is delivered
// exactly once per mapped image: the Borland entry refuses a second attach
// (ABI.md §8), so reloading means mapping a fresh image.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "loader/pe.hh"
#include "win32/layout.hh"

namespace adw::win32 {

class Runtime;
struct ShimEntry;

struct Module {
  std::string name;        // file name as found ("ADXPL510.DLL", "TOASTERS.AD")
  std::string host_path;   // UTF-8
  std::string guest_path;  // what GetModuleFileNameA reports
  std::unique_ptr<loader::pe::Image> image;
  loader::pe::Loaded loaded;
  uint32_t base = 0, size = 0, entry = 0;
  bool dll = true;
  bool attached = false;   // DLL_PROCESS_ATTACH delivered
  bool attaching = false;
  int refs = 0;            // LoadLibrary/FreeLibrary count (static imports hold one)
  std::vector<Module*> deps;

  uint32_t rva_to_va(uint32_t rva) const { return base + rva; }
};

class ModuleTable {
 public:
  explicit ModuleTable(Runtime& rt);
  ~ModuleTable();

  // Directories (host, UTF-8) searched for a DLL named by an import or
  // LoadLibrary, in order, after the importing module's own directory.
  void add_search_dir(const std::string& host_dir);

  // Maps a PE image (host path, or a bare name to search for) and, recursively,
  // the emulated DLLs it imports; binds every import. Does not run DllMain.
  // Throws loader::LoaderError (damaged image) or std::runtime_error.
  Module* load(const std::string& path_or_name);
  // A whole program (RuntimeOptions::program): maps its EXE as the process
  // image, at its preferred base when that is free (0x400000: no stand-in
  // host EXE in this mode) and binds its imports; exe_handle(),
  // GetModuleHandleA(NULL) and PEB.ImageBaseAddress are its base from then
  // on. Not attached: the lane starts it at its entry point.
  Module* load_exe(const std::string& host_path);
  Module* exe() const { return exe_; }
  // DLL_PROCESS_ATTACH to m's dependencies and then m, each image at most
  // once. False when a DllMain returned FALSE (that module is not attached).
  bool attach(Module* m);
  // DLL_PROCESS_DETACH to every attached image, dependents first.
  void detach_all();

  // LoadLibrary(Ex)A: an image we can load (attached before returning), a shim
  // DLL, or 0 — a system DLL we do not emulate (dsound.dll, ABI.md §2.12) is
  // "not found". as_datafile: map without binding or attaching (never used by
  // our corpus except on the module itself, which is already loaded).
  uint32_t load_library(std::string_view name);
  bool free_library(uint32_t hmod);

  // GetModuleHandleA: NULL → the host EXE; a name → a loaded image or shim DLL
  // (case-insensitive, ".DLL" optional, path ignored); else 0.
  uint32_t handle_of(std::string_view name) const;
  uint32_t exe_handle() const;
  // GetProcAddress by name (case-sensitive, as Win32) or ordinal. Shim DLLs
  // answer with a thunk for any name (unimplemented ones report when called).
  uint32_t proc_address(uint32_t hmod, std::string_view name);
  uint32_t proc_address(uint32_t hmod, uint16_t ordinal);

  Module* find(std::string_view name) const;      // loaded image by file name
  Module* by_handle(uint32_t hmod) const;
  Module* containing(uint32_t addr) const;
  bool is_shim_module(uint32_t hmod) const;
  std::string shim_module_name(uint32_t hmod) const;
  // GetModuleFileNameA's answer ("" for an unknown handle).
  std::string file_name(uint32_t hmod) const;
  const std::vector<std::unique_ptr<Module>>& images() const { return modules_; }

  // Names that are always emulated, never loaded from disk, even if a file of
  // that name sits next to a module.
  static bool is_system_dll(std::string_view normalized_name);

 private:
  Module* load_image(const std::string& host_path, const std::string& name);
  std::optional<std::string> locate(std::string_view name, const std::string& first_dir) const;
  uint32_t place(uint32_t preferred, uint32_t size);
  uint32_t resolve_import(Module& importer, const loader::pe::Import& imp);
  uint32_t export_address(Module& m, const loader::pe::Export& e, int depth);
  // Logs a shim import bound without a signature (shims.hh: calling it stops the module).
  void warn_unknown_signature(const std::string& importer, const ShimEntry& e);
  uint32_t shim_module_handle(const std::string& dll) const;
  void build_exe_header();

  Runtime& rt_;
  std::vector<std::string> search_dirs_;
  std::vector<std::unique_ptr<Module>> modules_;   // load order
  std::map<uint32_t, uint32_t> occupied_;          // base -> size, image area
  mutable std::map<std::string, uint32_t> shim_handles_;  // normalized dll -> pseudo HMODULE
  std::set<std::string> missing_logged_;                  // "importer|DLL" already reported missing
  Module* exe_ = nullptr;                                 // the program's EXE (load_exe)
  uint32_t image_low_ = layout::kImageLow;                // lowest base an image may get
};

}  // namespace adw::win32
