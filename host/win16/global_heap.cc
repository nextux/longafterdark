#include "win16/global_heap.hh"

#include <algorithm>

namespace adw::win16 {

GlobalHeap16::GlobalHeap16(cpu::MemoryContext& mem, win32::GuestHeap& arena, Ldt& ldt)
    : mem_(mem), arena_(arena), ldt_(ldt) {}

void GlobalHeap16::write_descriptors(GlobalBlock& b) {
  if (!b.base) {
    // Discarded: the selector stays allocated (it is the handle) but any load
    // of it faults #NP, as a discarded segment's did.
    for (uint16_t i = 0; i < b.tiles; i++) {
      SegDesc d{0, 0, false, b.code, true, false, 3};
      ldt_.set(uint16_t(b.sel + i * Ldt::kAhIncr), d);
    }
    return;
  }
  ldt_.set_block(b.sel, b.base, b.size, b.code, true);
}

GlobalBlock* GlobalHeap16::alloc_block(uint32_t size, bool code, uint16_t flags, uint16_t owner, uint32_t reserve,
                                       uint32_t align) {
  if (size > 0x00FF0000) return nullptr;  // Win16 blocks top out just under 16 MB
  uint32_t rsize = round_size(size);
  uint32_t resv = std::max(rsize, round_size(reserve));
  uint16_t tiles = Ldt::tiles_for(rsize);
  uint32_t base = arena_.alloc(resv, /*zero=*/true, std::max<uint32_t>(align, 16));
  if (!base) return nullptr;
  uint16_t sel = ldt_.alloc(tiles);
  if (!sel) {
    arena_.free(base);
    return nullptr;
  }
  GlobalBlock b;
  b.sel = sel;
  b.tiles = tiles;
  b.base = base;
  b.size = rsize;
  b.flags = flags;
  b.moveable = (flags & kMoveable) != 0;
  b.code = code;
  b.owner = owner;
  reserved_[base] = resv;
  auto [it, ok] = blocks_.emplace(sel, b);
  write_descriptors(it->second);
  return &it->second;
}

bool GlobalHeap16::set_limit(GlobalBlock& b, uint32_t size) {
  auto r = reserved_.find(b.base);
  if (r == reserved_.end() || round_size(size) > r->second) return false;
  uint16_t tiles = Ldt::tiles_for(round_size(size));
  if (tiles != b.tiles) return false;
  b.size = round_size(size);
  write_descriptors(b);
  if (on_moved_) on_moved_();
  return true;
}

uint16_t GlobalHeap16::alloc(uint16_t flags, uint32_t size) {
  if (size == 0 && (flags & kMoveable)) {
    // A moveable block of size 0 is allocated discarded: a handle, no memory.
    uint16_t sel = ldt_.alloc(1);
    if (!sel) return 0;
    GlobalBlock b;
    b.sel = sel;
    b.flags = flags;
    b.moveable = true;
    auto [it, ok] = blocks_.emplace(sel, b);
    write_descriptors(it->second);
    return it->second.handle();
  }
  GlobalBlock* b = alloc_block(size, false, flags, 0);
  if (!b) return 0;
  ldt_.set_tag(b->sel, "global");
  return b->handle();
}

GlobalBlock* GlobalHeap16::find(uint16_t h) {
  if (!Ldt::is_ldt(h)) return nullptr;
  uint16_t sel = uint16_t(h | 3);
  auto it = blocks_.upper_bound(sel);
  if (it == blocks_.begin()) return nullptr;
  --it;
  // The block's first selector, or one of its huge-block tiles.
  GlobalBlock& b = it->second;
  if (sel == b.sel) return &b;
  if (Ldt::index_of(sel) < Ldt::index_of(b.sel) + b.tiles && Ldt::index_of(sel) > Ldt::index_of(b.sel)) return &b;
  return nullptr;
}

const GlobalBlock* GlobalHeap16::find(uint16_t h) const { return const_cast<GlobalHeap16*>(this)->find(h); }

GlobalBlock* GlobalHeap16::containing(uint32_t linear) {
  for (auto& [sel, b] : blocks_) {
    if (b.base && linear >= b.base && linear - b.base < b.size) return &b;
  }
  return nullptr;
}

void GlobalHeap16::release(GlobalBlock& b) {
  if (b.base) {
    arena_.free(b.base);
    reserved_.erase(b.base);
  }
  ldt_.free(b.sel, 1);
  for (uint16_t i = 1; i < b.tiles; i++) ldt_.free(uint16_t(b.sel + i * Ldt::kAhIncr), 1);
}

uint16_t GlobalHeap16::free(uint16_t h) {
  GlobalBlock* b = find(h);
  if (!b) return h;
  uint16_t sel = b->sel;
  release(*b);
  blocks_.erase(sel);
  return 0;
}

void GlobalHeap16::free_owned(uint16_t owner) {
  for (auto it = blocks_.begin(); it != blocks_.end();) {
    if (it->second.owner == owner) {
      release(it->second);
      it = blocks_.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<uint16_t> GlobalHeap16::guest_handles() const {
  std::vector<uint16_t> out;
  for (const auto& [sel, b] : blocks_) {
    if (!b.owner) out.push_back(b.handle());
  }
  return out;
}

uint32_t GlobalHeap16::lock(uint16_t h) {
  GlobalBlock* b = find(h);
  if (!b || !b->base) return 0;
  if (b->moveable && b->locks < 0xFF) b->locks++;
  return uint32_t(b->sel) << 16;
}

bool GlobalHeap16::unlock(uint16_t h) {
  GlobalBlock* b = find(h);
  if (!b) return false;
  if (b->locks) b->locks--;
  return b->locks != 0;
}

uint32_t GlobalHeap16::size(uint16_t h) {
  GlobalBlock* b = find(h);
  return b ? b->size : 0;
}

uint32_t GlobalHeap16::handle(uint16_t sel) {
  GlobalBlock* b = find(sel);
  if (!b) return 0;
  return (uint32_t(b->sel) << 16) | b->handle();
}

uint16_t GlobalHeap16::flags(uint16_t h) {
  GlobalBlock* b = find(h);
  if (!b) return 0;
  uint16_t f = b->locks;
  if (b->flags & kDiscardable) f |= kFlagDiscardable;
  if (!b->base) f |= kFlagDiscarded;
  return f;
}

uint16_t GlobalHeap16::realloc(uint16_t h, uint32_t size, uint16_t flags) {
  GlobalBlock* b = find(h);
  if (!b) return 0;
  if (flags & kModify) {
    // Only the attributes change: GMEM_MODIFY with GMEM_MOVEABLE turns a
    // fixed block moveable (its handle changes with it).
    if (flags & kMoveable) b->moveable = true;
    b->flags = uint16_t((b->flags & ~kDiscardable) | (flags & kDiscardable));
    return b->handle();
  }
  if (size == 0) {
    // Discard a moveable, unlocked block (keep the handle).
    if (!b->moveable || b->locks) return 0;
    if (b->base) {
      arena_.free(b->base);
      reserved_.erase(b->base);
    }
    b->base = 0;
    b->size = 0;
    write_descriptors(*b);
    if (on_moved_) on_moved_();
    return b->handle();
  }
  uint32_t rsize = round_size(size);
  if (rsize > 0x00FF0000) return 0;
  uint16_t tiles = Ldt::tiles_for(rsize);
  bool may_move = b->moveable || (flags & kMoveable);
  uint32_t old_base = b->base, old_size = b->size;

  // Memory first.
  uint32_t new_base;
  if (!old_base) {
    new_base = arena_.alloc(rsize, true);
    if (!new_base) return 0;
    reserved_[new_base] = rsize;
  } else {
    uint32_t resv = reserved_.count(old_base) ? reserved_[old_base] : old_size;
    if (rsize <= resv) {
      new_base = old_base;
    } else {
      new_base = arena_.realloc(old_base, rsize, may_move, /*zero_growth=*/true);
      if (!new_base) return 0;
      reserved_.erase(old_base);
      reserved_[new_base] = rsize;
    }
    if (rsize > old_size) mem_.memset(new_base + old_size, 0, rsize - old_size);
  }

  // Then selectors.
  // Tiles are __AHINCR (8) apart, i.e. consecutive LDT indices, so a block's
  // selectors are one run of `tiles` entries.
  uint16_t sel = b->sel;
  if (tiles > b->tiles) {
    if (!ldt_.extend(sel, b->tiles, tiles)) {
      uint16_t nsel = ldt_.alloc(tiles);
      if (!nsel) return 0;
      GlobalBlock nb = *b;
      ldt_.free(b->sel, b->tiles);
      blocks_.erase(sel);
      nb.sel = nsel;
      nb.tiles = tiles;
      nb.base = new_base;
      nb.size = rsize;
      auto [it, ok] = blocks_.emplace(nsel, nb);
      write_descriptors(it->second);
      if (on_moved_) on_moved_();
      return it->second.handle();
    }
  } else if (tiles < b->tiles) {
    ldt_.free(uint16_t(sel + tiles * Ldt::kAhIncr), uint16_t(b->tiles - tiles));
  }
  b->tiles = tiles;
  b->base = new_base;
  b->size = rsize;
  write_descriptors(*b);
  if (on_moved_) on_moved_();
  return b->handle();
}

}  // namespace adw::win16
