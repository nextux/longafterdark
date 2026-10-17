// The Win16 global heap: GlobalAlloc & co. over the host LDT (ldt.hh) and one
// arena allocator (win32::GuestHeap — host-level, no Win32 guest knowledge).
//
// Every block owns one selector, or __AHINCR-spaced tiles for a block over
// 64 KiB. Handles follow Windows 3.1/95 enhanced mode:
//   * GMEM_FIXED     handle == selector (…7)
//   * GMEM_MOVEABLE  handle == selector with bit 0 clear (…6)
// so find(h | 1) finds a block from either, and GlobalLock returns sel:0.
// Blocks never move by themselves (there is no compaction or discarding);
// GlobalReAlloc grows in place when it can and otherwise moves the block and
// rewrites its descriptors (the runtime then refreshes the CPU's segment
// caches), keeping the selector — only a block that needs more tiles than
// the selectors after it allow gets new selectors (and a new handle).
//
// Sizes are rounded up to 32 bytes — the Win16 global heap's granule — and
// the selector limit and GlobalSize cover the rounded size, as on Windows:
// modules lean on that slack (NONSENSE allocates lstrlen(s) bytes and
// lstrcpy's s into them, NUL included, which only faults when the length is
// a multiple of 32). Every
// fresh block is zero-filled whatever the flags: the arena hands out zeroed
// memory the first time, and reused memory is cleared too, so runs stay
// reproducible whatever order blocks come and go in.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "MemoryContext.hh"
#include "win16/ldt.hh"
#include "win32/heap.hh"

namespace adw::win16 {

struct GlobalBlock {
  uint16_t sel = 0;      // first selector
  uint16_t tiles = 1;
  uint32_t base = 0;     // linear address; 0 = discarded (a moveable block of size 0)
  uint32_t size = 0;     // bytes, rounded to 16 (GlobalSize)
  uint16_t flags = 0;    // GMEM_* it was allocated with
  uint8_t locks = 0;
  bool moveable = false;
  bool code = false;
  uint16_t owner = 0;    // hModule of the module whose segment this is (0 = guest allocation)

  uint16_t handle() const { return moveable ? uint16_t(sel & ~1) : sel; }
};

class GlobalHeap16 {
 public:
  static constexpr uint16_t kMoveable = 0x0002;
  static constexpr uint16_t kNoCompact = 0x0010;
  static constexpr uint16_t kNoDiscard = 0x0020;
  static constexpr uint16_t kZeroInit = 0x0040;
  static constexpr uint16_t kModify = 0x0080;
  static constexpr uint16_t kDiscardable = 0x0100;
  static constexpr uint16_t kShare = 0x2000;
  // GlobalFlags result bits.
  static constexpr uint16_t kFlagDiscardable = 0x0100;
  static constexpr uint16_t kFlagDiscarded = 0x4000;

  GlobalHeap16(cpu::MemoryContext& mem, win32::GuestHeap& arena, Ldt& ldt);

  // Called after descriptors of live selectors changed (a block moved).
  void set_on_moved(std::function<void()> fn) { on_moved_ = std::move(fn); }

  // ---- the KERNEL API ----
  uint16_t alloc(uint16_t flags, uint32_t size);                // handle or 0
  uint16_t realloc(uint16_t h, uint32_t size, uint16_t flags);  // handle or 0
  uint16_t free(uint16_t h);                                    // 0 on success, else h
  uint32_t lock(uint16_t h);                                    // sel:0000 or 0
  bool unlock(uint16_t h);                                      // TRUE while still locked
  uint32_t size(uint16_t h);                                    // 0 if invalid
  uint32_t handle(uint16_t sel);                                // DX:AX = selector:handle, 0 if unknown
  uint16_t flags(uint16_t h);

  // ---- host services ----
  // A block with a code or data descriptor for a module segment or a host
  // structure. `reserve` >= size bytes are allocated (so the segment can grow
  // in place up to it); the limit covers `size`. Returns null when out of memory.
  GlobalBlock* alloc_block(uint32_t size, bool code, uint16_t flags, uint16_t owner, uint32_t reserve = 0,
                           uint32_t align = 16);
  // Changes a block's descriptor limit within its reservation (DGROUP growth).
  bool set_limit(GlobalBlock& b, uint32_t size);
  GlobalBlock* find(uint16_t handle_or_selector);
  const GlobalBlock* find(uint16_t handle_or_selector) const;
  // The block whose memory contains a linear address.
  GlobalBlock* containing(uint32_t linear);
  // Frees every block owned by a module (FreeLibrary of its last reference).
  void free_owned(uint16_t owner);
  // The handles of the guest's own blocks (GlobalAlloc's, owner 0), in selector order: what a task
  // left (the ne16 lane's 7thlevel protocol frees those its player made when it starts it again).
  std::vector<uint16_t> guest_handles() const;
  size_t block_count() const { return blocks_.size(); }
  win32::GuestHeap& arena() { return arena_; }

 private:
  static constexpr uint32_t kGranule = 32;
  static uint32_t round_size(uint32_t n) { return n ? (n + kGranule - 1) & ~(kGranule - 1) : kGranule; }
  void write_descriptors(GlobalBlock& b);
  void release(GlobalBlock& b);

  cpu::MemoryContext& mem_;
  win32::GuestHeap& arena_;
  Ldt& ldt_;
  std::map<uint16_t, GlobalBlock> blocks_;       // by first selector
  std::map<uint32_t, uint32_t> reserved_;        // base → reserved bytes (arena block size)
  std::function<void()> on_moved_;
};

}  // namespace adw::win16
