// adw::win32::Runtime — one emulated Win32 process: the address space
// (layout.hh), the CPU in flat mode with a real TEB/PEB, the import thunks and
// shim registry, structured exception handling, the PE module table, guest
// memory services, and (once attached) the display.
//
// The thread model is the 1996 one reduced to what After Dark needs: one
// emulated thread (the runner thread), so there is one TEB and one stack.
//
// Control flow between host and guest:
//   * guest → host: `int 0xFE` thunks (shims.hh); CPU faults and `int n`
//     (n ≠ 0xFE) become SEH exceptions (seh.hh).
//   * host → guest: call_guest() pushes the arguments and a return address
//     pointing at the sentinel thunk and runs a nested emulation until the
//     callee returns there — for DllMain, Module(), window procedures, enum
//     procs. Nesting is unlimited; each level has its own run loop.
//   * fatal guest conditions (ExitProcess, an unhandled exception, a hung
//     call) throw GuestError, which unwinds every run loop at once.
//
// Shim families keep their own state in Runtime::state<T>() (a struct deriving
// from RuntimeState, created on first use), so a family file never needs to
// edit this header.
#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "X86Emulator.hh"
#include "adw/core/clock.h"
#include "adw/core/protocol.h"
#include "adw/core/screen.h"
#include "win32/guest.hh"
#include "win32/heap.hh"
#include "win32/layout.hh"
#include "win32/shims.hh"

namespace adw::win32 {

class Display;
class ModuleTable;
class Seh;
class Vfs;

// Thrown from the call hook (set_call_hook) to abandon the guest calls in
// progress — a lane closing while a DRAWFRAME is suspended (pe32/lane.hh
// "Long calls"). Every call_guest level puts back the registers and the SEH
// chain head it started with on the way out, so the host can call the guest
// again afterwards.
struct AbandonGuestCall {};

// Base of every per-family state object (see state<T>()).
struct RuntimeState {
  virtual ~RuntimeState() = default;
};

struct RuntimeOptions {
  uint32_t heap_size = layout::kDefaultHeapSize;
  // Instructions a single top-level call_guest may run before it counts as hung
  // (GuestError::Kind::hang). Nested calls share their outermost call's budget.
  uint64_t call_budget = 4'000'000'000ull;
  // What the Borland/MSVC runtimes see as the host process.
  std::string exe_path = "C:\\AFTERDRK\\AFTERDAR.SCR";
  std::string command_line = "C:\\AFTERDRK\\AFTERDAR.SCR /s";
  // Guest directory the modules appear to live in, and its host counterpart.
  std::string guest_module_dir = "C:\\AFTERDRK";
  // A whole program (pe32/program.cc, ABI.md §2.15): its EXE is the process
  // image (ModuleTable::load_exe), mapped at its preferred base with no
  // stand-in host EXE at 0x400000; exe_path and command_line are its own.
  bool program = false;
};

// The x86 CONTEXT fields the runtime moves between the CPU and guest memory.
struct Regs32 {
  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi, eip, eflags;
};

class Runtime {
 public:
  Runtime(const RuntimeOptions& opts, VirtualClock& clock, const InputState* input = nullptr);
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  const RuntimeOptions& options() const { return opts_; }
  cpu::MemoryContext& mem() { return *mem_; }
  const cpu::MemoryContext& mem() const { return *mem_; }
  cpu::X86Emulator& cpu() { return *cpu_; }
  VirtualClock& clock() { return clock_; }
  const InputState& input() const { return input_ ? *input_ : empty_input_; }
  void set_input(const InputState* input) { input_ = input; }

  ShimRegistry& shims() { return *shims_; }
  GuestHeap& heap() { return *heap_; }
  HandleHeap& handles() { return *handles_; }
  HeapSet& heaps() { return *heaps_; }
  ModuleTable& modules() { return *modules_; }
  Seh& seh() { return *seh_; }
  Vfs& vfs() { return *vfs_; }

  // The emulated screen (display.hh). attach_display() allocates the screen
  // DIB's bits in the heap arena, builds the Display over them and points
  // `screen` at them; until then display() is null.
  Display* display() { return display_.get(); }
  Display& attach_display(Screen& screen);

  template <typename T>
  T& state() {
    static_assert(std::is_base_of_v<RuntimeState, T>);
    auto key = std::type_index(typeid(T));
    auto it = states_.find(key);
    if (it == states_.end()) {
      std::unique_ptr<RuntimeState> p;
      if constexpr (std::is_constructible_v<T, Runtime&>) p = std::make_unique<T>(*this);
      else p = std::make_unique<T>();
      it = states_.emplace(key, std::move(p)).first;
    }
    return static_cast<T&>(*it->second);
  }

  // ---- host → guest calls ----
  // Calls `addr` with `args` (pushed right to left) and returns EAX. ESP and
  // every other register are restored afterwards, as they are around a real
  // callback, so the convention only matters as a check: a stdcall callee
  // must pop exactly its arguments, a cdecl one none (a mismatch means a wrong
  // prototype and is logged under ADTRACE=api). EDX of the result is in
  // last_edx().
  uint32_t call_guest(uint32_t addr, std::span<const uint32_t> args, Conv conv = Conv::stdcall_);
  uint32_t call_guest(uint32_t addr, std::initializer_list<uint32_t> args, Conv conv = Conv::stdcall_) {
    return call_guest(addr, std::span<const uint32_t>(args.begin(), args.size()), conv);
  }
  uint32_t last_edx() const { return last_edx_; }
  int call_depth() const { return int(boundaries_.size()); }
  // ESP at the innermost active call_guest (0 when none): guest code resumed
  // above it would be running in a frame the host has already left.
  uint32_t callback_boundary() const { return boundaries_.empty() ? 0 : boundaries_.back(); }
  uint32_t sentinel() const { return sentinel_; }

  // ---- thread ----
  uint32_t teb() const { return layout::kTeb; }
  uint32_t last_error() const;
  void set_last_error(uint32_t e);
  Regs32 regs() const;
  void set_regs(const Regs32& r);

  // ---- system area strings (GetCommandLineA and friends) ----
  // Copies a NUL-terminated block into the system area once; returns its address.
  uint32_t static_bytes(const std::string& key, std::string_view bytes);

  // ---- diagnostics ----
  std::string describe(uint32_t addr) const;  // "TOASTERS.AD+0x1e4cb", "thunk KERNEL32.DLL!GetVersion", …
  void log_state(const char* why) const;      // registers + top of stack → stderr

  // Instructions executed so far (all nesting levels).
  uint64_t instructions() const;
  // Shim calls dispatched so far (every thunk, internal ones included). With
  // instructions() it is the lane's deterministic measure of guest work.
  uint64_t api_calls() const { return api_calls_; }
  // Pixels the GDI shims' blits and fills wrote (each call's destination
  // area: BitBlt, StretchBlt, PatBlt, SetDIBitsToDevice, StretchDIBits,
  // SetDIBits, FillRect). A 1996 display card moved some 10-40 MB/s, so blits
  // bounded how often a module could draw; the lane's DRAWFRAME budget
  // charges them (pe32/lane.hh "Timing").
  void charge_pixels(int64_t n) {
    if (n > 0) pixels_ += uint64_t(n);
  }
  uint64_t pixels_charged() const { return pixels_; }
  // A hook run at every API call the guest makes, before the shim: the pe32
  // lane ends a presented frame there when a DRAWFRAME outlasts its frame
  // (pe32/lane.hh "Long calls"). It may switch to another fiber and come
  // back, or throw AbandonGuestCall. Null = none.
  void set_call_hook(std::function<void()> fn) { call_hook_ = std::move(fn); }
  // A whole program's waits (pe32/program.cc): GetMessageA/WaitMessage with
  // nothing to deliver call yield_frame(), which runs the hook: true = a
  // frame ended there and the guest was resumed by a later one (look again);
  // false = there is no frame to end (no hook), and the caller moves virtual
  // time on to what it waits for instead.
  void set_yield_hook(std::function<bool()> fn) { yield_hook_ = std::move(fn); }
  bool yield_frame() { return yield_hook_ ? yield_hook_() : false; }
  // Preemption for code that runs long without an API call (pe32/program.cc):
  // with a hook, call_guest runs the guest `instructions` at a time and calls
  // `fn` between two runs (it may switch fibers and come back, or throw).
  // Null = none: each call runs to its end or its budget, as before.
  void set_slice_hook(uint64_t instructions, std::function<void()> fn) {
    slice_ = instructions;
    slice_hook_ = std::move(fn);
  }

 private:
  void build_process();
  void on_interrupt(cpu::X86Emulator& cpu, uint8_t vector);
  void on_fault(cpu::X86Emulator& cpu, const cpu::X86Emulator::Fault& f);
  void dispatch_thunk(uint16_t id);

  RuntimeOptions opts_;
  VirtualClock& clock_;
  const InputState* input_;
  InputState empty_input_;

  std::shared_ptr<cpu::MemoryContext> mem_;
  std::unique_ptr<cpu::X86Emulator> cpu_;
  std::unique_ptr<ShimRegistry> shims_;
  std::unique_ptr<GuestHeap> heap_;
  std::unique_ptr<HandleHeap> handles_;
  std::unique_ptr<HeapSet> heaps_;
  std::unique_ptr<Vfs> vfs_;
  std::unique_ptr<ModuleTable> modules_;
  std::unique_ptr<Seh> seh_;
  std::unique_ptr<Display> display_;
  std::unordered_map<std::type_index, std::unique_ptr<RuntimeState>> states_;

  uint32_t sentinel_ = 0;
  std::vector<uint32_t> boundaries_;
  uint64_t budget_end_ = 0;
  uint64_t api_calls_ = 0;
  uint64_t pixels_ = 0;
  std::function<void()> call_hook_;
  std::function<bool()> yield_hook_;
  std::function<void()> slice_hook_;
  uint64_t slice_ = 0;
  uint32_t last_edx_ = 0;
  uint32_t strings_next_ = layout::kSystemStrings;
  std::unordered_map<std::string, uint32_t> strings_;
};

}  // namespace adw::win32
