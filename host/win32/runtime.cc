#include "win32/runtime.hh"

#include <cinttypes>
#include <cstdio>
#include <stdexcept>

#include "adw/core/log.h"
#include "win32/display.hh"
#include "win32/modules.hh"
#include "win32/seh.hh"
#include "win32/vfs.hh"

namespace adw::win32 {

namespace {

using X86 = cpu::X86Emulator;

// TEB fields (NT layout; FS:[0]/[4]/[8]/[0x18] are the NT_TIB every Win32
// runtime reads, and Win95's TIB agrees on those).
constexpr uint32_t kTebExceptionList = 0x00, kTebStackBase = 0x04, kTebStackLimit = 0x08, kTebSelf = 0x18;
constexpr uint32_t kTebProcessId = 0x20, kTebThreadId = 0x24, kTebTlsPointer = 0x2C, kTebPeb = 0x30;
constexpr uint32_t kTebLastError = 0x34;
constexpr uint32_t kTebStaticTlsArray = 0xF80;  // what FS:[0x2C] points at (no static-TLS modules in our corpus)

constexpr uint32_t kProcessId = 0x00000AD4, kThreadId = 0x00000AD8;

// The vector every import thunk uses.
constexpr uint8_t kThunkVector = 0xFE;

// What EFLAGS a CONTEXT may set: the arithmetic flags, DF, and IF/bit 1 stay on.
constexpr uint32_t kUserFlags = 0x0CD5;

std::string hex32(uint32_t v) {
  char b[16];
  snprintf(b, sizeof(b), "0x%08X", v);
  return b;
}

}  // namespace

Runtime::Runtime(const RuntimeOptions& opts, VirtualClock& clock, const InputState* input)
    : opts_(opts), clock_(clock), input_(input) {
  if (opts_.heap_size < (4u << 20) || opts_.heap_size > layout::kMaxHeapSize) {
    throw std::invalid_argument("heap size out of range");
  }
  mem_ = std::make_shared<cpu::MemoryContext>();
  cpu_ = std::make_unique<cpu::X86Emulator>(mem_);
  shims_ = std::make_unique<ShimRegistry>(*mem_);
  build_process();
  heap_ = std::make_unique<GuestHeap>(*mem_, layout::kHeapBase, opts_.heap_size);
  handles_ = std::make_unique<HandleHeap>(*mem_, *heap_);
  heaps_ = std::make_unique<HeapSet>(*mem_, *heap_);
  mem_->write_u32l(layout::kPeb + 0x18, heaps_->process_heap());  // PEB.ProcessHeap
  vfs_ = std::make_unique<Vfs>();
  modules_ = std::make_unique<ModuleTable>(*this);
  seh_ = std::make_unique<Seh>(*this);
  seh_->install();

  cpu_->set_flat_mode(layout::kTeb, 0xFFF);
  cpu_->set_interrupt_handler([this](X86& c, uint8_t v) { on_interrupt(c, v); });
  cpu_->set_fault_handler([this](X86& c, const X86::Fault& f) { on_fault(c, f); });
  auto& r = cpu_->registers();
  // Leave the two Borland per-thread dwords at StackBase-8/-4 (ABI.md §4) and a
  // little slack above the first frame.
  r.w_esp(layout::kStackBase - 0x20);
  r.eip = sentinel_;
}

Runtime::~Runtime() {
  // Family state first (it may own real GDI objects selected into the
  // display's DC), then the display, then the address space.
  states_.clear();
  display_.reset();
}

void Runtime::build_process() {
  auto& mem = *mem_;
  mem.allocate_at(layout::kSystemBase, layout::kSystemSize);
  mem.memset(layout::kSystemBase, 0, layout::kSystemSize);
  mem.allocate_at(layout::kThunkBase, layout::kThunkSize);
  mem.memset(layout::kThunkBase, 0xCC, layout::kThunkSize);  // int3 between thunks
  mem.allocate_at(layout::kStackLimit, layout::kStackBase - layout::kStackLimit);

  // Thunk 0 is the sentinel call_guest returns to. run_until stops before it
  // executes; if a guest ever jumps there on its own, the trap reports it.
  sentinel_ = ShimRegistry::address_of(0);
  const uint8_t trap[4] = {0xCD, kThunkVector, 0x00, 0x00};
  mem.memcpy(sentinel_, trap, sizeof(trap));

  uint32_t teb = layout::kTeb;
  mem.write_u32l(teb + kTebExceptionList, 0xFFFFFFFF);  // end of the SEH chain
  mem.write_u32l(teb + kTebStackBase, layout::kStackBase);
  mem.write_u32l(teb + kTebStackLimit, layout::kStackLimit);
  mem.write_u32l(teb + kTebSelf, teb);
  mem.write_u32l(teb + kTebProcessId, kProcessId);
  mem.write_u32l(teb + kTebThreadId, kThreadId);
  mem.write_u32l(teb + kTebTlsPointer, teb + kTebStaticTlsArray);
  mem.write_u32l(teb + kTebPeb, layout::kPeb);

  uint32_t peb = layout::kPeb;
  mem.write_u32l(peb + 0x08, layout::kExeBase);         // ImageBaseAddress
  mem.write_u32l(peb + 0x10, layout::kProcessParams);   // ProcessParameters
}

Display& Runtime::attach_display(Screen& screen) {
  if (display_) throw std::logic_error("display already attached");
  uint32_t size = Display::bits_size(screen.width(), screen.height());
  uint32_t bits = heap_->alloc(size, /*zero=*/true, 0x10000);
  if (!bits) throw std::runtime_error("the heap arena cannot hold the screen surface");
  display_ = std::make_unique<Display>(screen, *mem_, bits);
  return *display_;
}

// ---- traps ---------------------------------------------------------------------------------------

void Runtime::on_interrupt(X86& cpu, uint8_t vector) {
  if (vector != kThunkVector) {
    seh_->on_software_interrupt(vector);
    return;
  }
  dispatch_thunk(mem_->read_u16l(cpu.registers().eip));
}

void Runtime::dispatch_thunk(uint16_t id) {
  ShimEntry* e = shims_->by_thunk(id);
  if (!e) {
    throw GuestError(GuestError::Kind::fatal, id == 0 ? "guest code jumped to the host-call sentinel"
                                                      : "call through an unknown thunk id " + std::to_string(id));
  }
  auto& r = cpu_->registers();
  uint32_t esp = r.r_esp();
  Call c(*this, *e, esp);
  e->calls++;
  api_calls_++;
  if (e->conv == Conv::internal) {
    e->fn(c);
    return;
  }
  if (!e->known_signature) {
    // Without a signature there is no way back: a stdcall callee pops its own
    // arguments, and how many is exactly what is unknown. Returning would
    // leave ESP off by that much and the guest would fail later, somewhere
    // unrelated (HALLOFFA.AD's LoadCursorA did). Stop here, by name.
    if (!e->unimpl_reported) {
      e->unimpl_reported = true;
      write_stderr("[unimpl] " + e->key() + " (unknown signature)\n");
    }
    std::string why = ModuleTable::is_system_dll(e->dll)
                          ? "an import with no known signature (its argument size is unknown, so it cannot "
                            "return); add it to win32/signatures.cc"
                          : e->dll + " was not found on the lane's DLL search path, so nothing implements it";
    throw GuestError(GuestError::Kind::fatal, "call to " + e->key() + " from " + describe(c.ret_addr()) + ": " + why);
  }

  bool traced = tracing("api");
  std::string args;
  if (traced) {
    int n = e->arg_bytes / 4;
    for (int i = 0; i < n; i++) {
      char b[16];
      snprintf(b, sizeof(b), "%s0x%X", i ? ", " : "", c.arg(i));
      args += b;
    }
    if (e->conv == Conv::varargs) args += ", ...";
  }
  uint32_t ret = c.ret_addr();
  if (call_hook_) call_hook_();

  if (e->fn) {
    e->fn(c);
  } else {
    if (!e->unimpl_reported) {
      e->unimpl_reported = true;
      write_stderr("[unimpl] " + e->key() + (e->known_signature ? "" : " (unknown signature)") + "\n");
    }
    c.ret(0);
  }

  if (traced) {
    trace("api", "%s(%s) -> 0x%X%s  [from %s]", e->key().c_str(), args.c_str(), c.result(),
          c.took_over() ? " (no return)" : "", describe(ret).c_str());
  }
  if (!c.took_over()) {
    uint32_t pop = e->conv == Conv::stdcall_ ? e->arg_bytes : 0;
    r.w_esp(esp + 4 + pop);
    r.eip = ret;
  }
}

void Runtime::on_fault(X86&, const X86::Fault& f) { seh_->on_fault(f); }

// ---- host → guest calls --------------------------------------------------------------------------

uint32_t Runtime::call_guest(uint32_t addr, std::span<const uint32_t> args, Conv conv) {
  auto& r = cpu_->registers();
  Regs32 saved = regs();
  uint32_t esp = r.r_esp();
  for (size_t i = args.size(); i-- > 0;) {
    esp -= 4;
    mem_->write_u32l(esp, args[i]);
  }
  esp -= 4;
  mem_->write_u32l(esp, sentinel_);
  r.w_esp(esp);
  r.eip = addr;

  if (boundaries_.empty()) budget_end_ = cpu_->cycles() + opts_.call_budget;
  const uint32_t seh_head = mem_->read_u32l(layout::kTeb);  // fs:[0]
  boundaries_.push_back(esp);
  X86::StopReason why;
  try {
    for (;;) {
      uint64_t now = cpu_->cycles();
      uint64_t budget = budget_end_ > now ? budget_end_ - now : 0;
      // A slice hook (set_slice_hook) gets control between two slices.
      uint64_t run = slice_hook_ && slice_ && slice_ < budget ? slice_ : budget;
      why = cpu_->run_until(sentinel_, run);
      if (why != X86::StopReason::LIMIT || run == budget) break;
      slice_hook_();
    }
  } catch (const AbandonGuestCall&) {
    // The machine as this level found it (the frames the call built on the
    // stack are dropped with their SEH registrations).
    boundaries_.pop_back();
    set_regs(saved);
    mem_->write_u32l(layout::kTeb, seh_head);
    throw;
  } catch (...) {
    boundaries_.pop_back();
    throw;
  }
  boundaries_.pop_back();
  if (why != X86::StopReason::ADDRESS) {
    std::string where = describe(r.eip);
    set_regs(saved);
    throw GuestError(GuestError::Kind::hang,
                     "guest call to " + describe(addr) + " did not return within " +
                         std::to_string(opts_.call_budget) + " instructions (stopped at " + where + ")");
  }
  uint32_t result = r.r_eax();
  last_edx_ = r.r_edx();
  if (tracing("api")) {
    uint32_t want = esp + 4 + (conv == Conv::stdcall_ ? 4 * uint32_t(args.size()) : 0);
    if (r.r_esp() != want) {
      trace("api", "call_guest %s: returned with ESP 0x%08X, expected 0x%08X for a %s callee", describe(addr).c_str(),
            r.r_esp(), want, conv_name(conv));
    }
  }
  set_regs(saved);
  return result;
}

// ---- registers / thread --------------------------------------------------------------------------

Regs32 Runtime::regs() const {
  const auto& r = cpu_->registers();
  return Regs32{r.r_eax(), r.r_ecx(), r.r_edx(), r.r_ebx(), r.r_esp(),
                r.r_ebp(), r.r_esi(), r.r_edi(), r.eip,     r.read_eflags()};
}

void Runtime::set_regs(const Regs32& s) {
  auto& r = cpu_->registers();
  r.w_eax(s.eax);
  r.w_ecx(s.ecx);
  r.w_edx(s.edx);
  r.w_ebx(s.ebx);
  r.w_esp(s.esp);
  r.w_ebp(s.ebp);
  r.w_esi(s.esi);
  r.w_edi(s.edi);
  r.eip = s.eip;
  r.write_eflags((r.read_eflags() & ~kUserFlags) | (s.eflags & kUserFlags));
}

uint32_t Runtime::last_error() const { return mem_->read_u32l(layout::kTeb + kTebLastError); }
void Runtime::set_last_error(uint32_t e) { mem_->write_u32l(layout::kTeb + kTebLastError, e); }

uint32_t Runtime::static_bytes(const std::string& key, std::string_view bytes) {
  auto it = strings_.find(key);
  if (it != strings_.end()) return it->second;
  uint32_t size = align_up(uint32_t(bytes.size()) + 2, 4);
  if (strings_next_ + size > layout::kSystemStringsEnd) {
    uint32_t a = heap_->alloc(size, true);
    if (!a) throw std::runtime_error("out of guest memory for " + key);
    mem_->memcpy(a, bytes.data(), bytes.size());
    strings_[key] = a;
    return a;
  }
  uint32_t a = strings_next_;
  strings_next_ += size;
  mem_->memcpy(a, bytes.data(), bytes.size());
  strings_[key] = a;
  return a;
}

uint64_t Runtime::instructions() const { return cpu_->cycles(); }

// ---- diagnostics ---------------------------------------------------------------------------------

std::string Runtime::describe(uint32_t addr) const {
  char b[96];
  int id = ShimRegistry::thunk_at(addr);
  if (id >= 0) {
    if (id == 0) return "the host-call sentinel";
    const ShimEntry* e = shims_->by_thunk(uint16_t(id));
    return e ? "thunk " + e->key() : "thunk #" + std::to_string(id);
  }
  if (Module* m = modules_ ? modules_->containing(addr) : nullptr) {
    snprintf(b, sizeof(b), "%s+0x%X", m->name.c_str(), addr - m->base);
    return b;
  }
  if (addr >= layout::kExeBase && addr < layout::kExeBase + layout::kExeSize) {
    snprintf(b, sizeof(b), "host EXE+0x%X", addr - layout::kExeBase);
    return b;
  }
  if (addr >= layout::kStackLimit && addr < layout::kStackBase) return "stack " + hex32(addr);
  if (heap_ && heap_->contains(addr)) return "heap " + hex32(addr);
  return hex32(addr);
}

void Runtime::log_state(const char* why) const {
  const auto& r = cpu_->registers();
  log("%s: eip=%08X (%s) eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ebp=%08X esp=%08X "
      "fs:[0]=%08X insns=%" PRIu64,
      why, r.eip, describe(r.eip).c_str(), r.r_eax(), r.r_ebx(), r.r_ecx(), r.r_edx(), r.r_esi(), r.r_edi(),
      r.r_ebp(), r.r_esp(), mem_->read_u32l(layout::kTeb), cpu_->cycles());
  std::string stack;
  for (uint32_t i = 0; i < 8; i++) {
    uint32_t a = r.r_esp() + 4 * i;
    if (!mem_->exists(a, 4)) break;
    char b[16];
    snprintf(b, sizeof(b), " %08X", mem_->read_u32l(a));
    stack += b;
  }
  log("%s: stack:%s", why, stack.c_str());
}

}  // namespace adw::win32
