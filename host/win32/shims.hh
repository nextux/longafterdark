// Import thunks and the shim registry (DESIGN.md §5).
//
// Every function a guest imports from a DLL we emulate (KERNEL32, USER32, …)
// is bound to an 8-byte thunk in the thunk area:
//
//     CD FE        int 0xFE
//     id id        16-bit thunk id (read by the trap handler at EIP)
//     C3 CC CC CC  never reached: the handler always sets EIP itself
//
// The trap handler looks the id up here, gives the handler a Call (arguments
// read from the guest stack, results written to EAX/EDX), and then returns to
// the caller the way the convention says: stdcall pops `arg_bytes`, cdecl and
// varargs leave the arguments to the caller. An import with no handler logs
// "[unimpl] DLL!Name" once, is counted for the census, and returns 0 — but
// still pops the right number of bytes, because every Win32-lane import has a
// signature in signatures.cc.
//
// An import with no signature (a module the table has never seen) is logged
// when the module table binds it and listed in the census. Calling it throws
// GuestError naming the import: its argument size is unknown, so a "return 0"
// would leave the caller's stack unbalanced and fail somewhere else later.
//
// Adding a shim: see README.md ("Extension recipe").
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "win32/guest.hh"

namespace adw::cpu {
class X86Emulator;
}

namespace adw::win32 {

class Runtime;
class Call;

using ShimFn = std::function<void(Call&)>;

enum class Conv : uint8_t {
  stdcall_,  // WINAPI: the callee pops arg_bytes
  cdecl_,    // the caller pops; arg_bytes = the fixed arguments (tracing only)
  varargs,   // cdecl with a variable tail (wsprintfA); arg_bytes = the fixed part
  internal,  // a host routine (sentinels, SEH continuations): the handler owns EIP/ESP
};
const char* conv_name(Conv c);

// One row of the signature table (signatures.cc).
struct ShimSig {
  const char* dll;   // upper-case with extension: "KERNEL32.DLL"
  const char* name;
  Conv conv;
  uint16_t arg_bytes;
};
// Every import of the Win32 lane (API_SURFACE.md §1) plus what is resolved
// dynamically through GetProcAddress.
std::span<const ShimSig> win32_signatures();
// What a whole program imports besides (ABI.md §2.15), added to a program's
// registry only (ShimRegistry::add_signatures; shim_families.hh
// register_program_shims).
std::span<const ShimSig> program_signatures();

struct ShimEntry {
  std::string dll;   // normalized: "KERNEL32.DLL"
  std::string name;  // "GetTickCount", or "#12" when bound by ordinal
  Conv conv = Conv::stdcall_;
  uint16_t arg_bytes = 0;
  bool known_signature = false;
  ShimFn fn;          // empty = unimplemented
  uint16_t thunk = 0; // 0 = no thunk written yet
  uint64_t calls = 0;
  bool unimpl_reported = false;
  std::string key() const { return dll + "!" + name; }
};

// What a shim handler sees. Arguments are the dwords above the return address
// ([esp+4] is argument 0); results go to EAX (and EDX for 64-bit results).
class Call {
 public:
  Call(Runtime& rt, ShimEntry& fn, uint32_t esp) : rt(rt), fn(fn), esp(esp) {}

  Runtime& rt;
  ShimEntry& fn;
  const uint32_t esp;  // at entry: [esp] = return address

  MemoryContext& mem() const;
  cpu::X86Emulator& cpu() const;

  uint32_t ret_addr() const { return mem().read_u32l(esp); }
  uint32_t arg(int i) const { return mem().read_u32l(esp + 4 + 4 * uint32_t(i)); }
  int32_t iarg(int i) const { return int32_t(arg(i)); }
  bool null(int i) const { return arg(i) == 0; }
  // Guest strings / structs through argument i (a NULL pointer reads as "").
  std::string str(int i) const { return read_cstr(mem(), arg(i)); }
  std::u16string wstr(int i) const { return read_wstr(mem(), arg(i)); }
  template <typename T>
  T pod(int i) const { return read_pod<T>(mem(), arg(i)); }

  void ret(uint32_t v);
  void ret64(uint64_t v);
  void ret_bool(bool b) { ret(b ? 1 : 0); }
  uint32_t result() const { return result_; }
  // Also mirrored into TEB+0x34, where GetLastError reads it.
  void set_last_error(uint32_t e);

  // The handler set EIP/ESP itself (RaiseException, RtlUnwind, ExitProcess…):
  // the dispatcher must not "return" to the caller.
  void take_over() { took_over_ = true; }
  bool took_over() const { return took_over_; }

 private:
  uint32_t result_ = 0;
  bool took_over_ = false;
};

class ShimRegistry {
 public:
  explicit ShimRegistry(MemoryContext& mem);

  // "kernel32", "KERNEL32.dll" → "KERNEL32.DLL".
  static std::string normalize_dll(std::string_view dll);

  // ---- registration (family files) ----
  // Implements a function whose signature is in win32_signatures(). A name
  // that is not there throws std::logic_error: add the row to signatures.cc.
  void impl(std::string_view dll, std::string_view name, ShimFn fn);
  // Declares (and optionally implements) a function with an explicit signature.
  ShimEntry& add(std::string_view dll, std::string_view name, Conv conv, uint16_t arg_bytes, ShimFn fn = {});
  // More signature rows (program_signatures()): their DLLs become emulated DLLs.
  void add_signatures(std::span<const ShimSig> sigs);
  // A signature row or a declared entry exists for the name.
  bool known(std::string_view dll, std::string_view name) const;

  // ---- lookup ----
  bool has_dll(std::string_view dll) const;   // a DLL we emulate (a shim module)
  std::vector<std::string> dlls() const;
  ShimEntry* find(std::string_view dll, std::string_view name);
  // Finds or declares. A name with no row in signatures.cc is declared with
  // known_signature = false: the census lists it and a call to it throws.
  ShimEntry& get(std::string_view dll, std::string_view name);
  ShimEntry* by_thunk(uint16_t id);

  // ---- thunks ----
  // The guest address that calls `e` (written on first use).
  uint32_t thunk_address(ShimEntry& e);
  // A host routine at a fixed thunk (Conv::internal); returns its address.
  uint32_t internal_thunk(std::string_view name, ShimFn fn);
  static uint32_t address_of(uint16_t id);
  // The thunk id at `eip` if it is inside the thunk area, else -1.
  static int thunk_at(uint32_t eip);

  // ---- census ----
  std::vector<const ShimEntry*> unimplemented_called() const;
  uint64_t unimplemented_calls() const;
  // Imports bound with no signature (called or not), sorted by key.
  std::vector<const ShimEntry*> unknown_signatures() const;
  // "[census] <label>: N unimplemented API(s) called, M call(s)", one line
  // per such API, then (only when there are any) one line naming the imports
  // bound without a signature.
  void print_census(std::string_view label) const;

 private:
  MemoryContext& mem_;
  std::deque<ShimEntry> entries_;  // stable addresses
  std::unordered_map<std::string, ShimEntry*> by_key_;
  std::vector<ShimEntry*> by_thunk_;  // index = id; [0] = the reserved sentinel
  std::set<std::string> dlls_;
  std::unordered_map<std::string, const ShimSig*> sigs_;
};

}  // namespace adw::win32
