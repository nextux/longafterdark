// DSOUND.DLL — DirectSound 3 over the host audio engine (AUDIO.md §2.1, §7.2).
//
// Registered only while the engine is enabled (attach_audio): with sound off
// dsound.dll stays "not found", as it always was (ABI.md §2.12), and
// configure mode keeps RealUi's own DirectSoundCreate (0x8878000A, DSERR_ALLOCATED).
//
// Objects live in guest memory, where C++ code can call through them:
// {vtbl, refcount, host id, tag} on the guest heap, with two static vtables of
// stdcall thunks (11 IDirectSound slots, 21 IDirectSoundBuffer slots, `this`
// first). Every slot is implemented. The slots ADXPL510 calls (§2.1, VERIFIED
// by disassembly) are the contract; any other slot follows the DirectSound 3
// documentation and logs once under ADTRACE=sound as an "unverified
// DirectSound method" (TOAST2K and TOASTER2 carry their own copy of the
// engine, and POINTS, SLOWBURN and SWIRLING probe DirectSound themselves).
//
// A secondary buffer is a guest backing store (the truth: Lock hands out
// pointers into it, split at the end of the buffer), one engine buffer with
// the same bytes, and one engine voice. Unlock copies exactly the ranges it
// is given from the store into the engine buffer, audible from now; the
// engine's GiveTime reads its source buffers back through Lock, so the store
// stays current. DuplicateSoundBuffer shares the store and the engine buffer
// and gets its own voice. Volume and pan become one gain (gain_from_ds);
// SetFrequency sets the voice's rate. Positions and status come from the
// engine's model at the virtual time of the call, never from a device.
//
// Known gaps, deliberately left (no module of the 202 in the five releases
// asks for them): IDirectSound3DBuffer/Listener, capture and notification
// interfaces do not exist (QueryInterface answers E_NOINTERFACE).
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "adw/core/log.h"
#include "win32/audio.hh"
#include "win32/runtime.hh"
#include "win32/shims.hh"

namespace adw::win32 {

namespace {

// HRESULTs (dsound.h: MAKE_DSHRESULT(code) = 0x88780000 | code).
constexpr uint32_t DS_OK = 0;
constexpr uint32_t DSERR_CONTROLUNAVAIL = 0x8878001E;
constexpr uint32_t DSERR_INVALIDPARAM = 0x80070057;  // E_INVALIDARG
constexpr uint32_t DSERR_INVALIDCALL = 0x88780032;
constexpr uint32_t DSERR_PRIOLEVELNEEDED = 0x88780046;
constexpr uint32_t DSERR_OUTOFMEMORY = 0x8007000E;   // E_OUTOFMEMORY
constexpr uint32_t DSERR_BADFORMAT = 0x88780064;
constexpr uint32_t DSERR_ALREADYINITIALIZED = 0x88780082;
constexpr uint32_t DSERR_NOAGGREGATION = 0x80040110;  // CLASS_E_NOAGGREGATION
constexpr uint32_t E_NOINTERFACE_ = 0x80004002;

// DSBCAPS_*.
constexpr uint32_t kPrimary = 0x1, kLocSoftware = 0x8, kCtrlFrequency = 0x20, kCtrlPan = 0x40, kCtrlVolume = 0x80;
// DSBPLAY_LOOPING, DSBSTATUS_*, DSBLOCK_*.
constexpr uint32_t kPlayLooping = 1, kStatusPlaying = 1, kStatusLooping = 4;
constexpr uint32_t kLockFromWriteCursor = 1, kLockEntireBuffer = 2;
// DSCAPS_*.
constexpr uint32_t kCapsPrimaryMono = 0x1, kCapsPrimaryStereo = 0x2, kCapsPrimary8 = 0x4, kCapsPrimary16 = 0x8,
                   kCapsContinuousRate = 0x10, kCapsEmulDriver = 0x20, kCapsSecondaryMono = 0x100,
                   kCapsSecondaryStereo = 0x200, kCapsSecondary8 = 0x400, kCapsSecondary16 = 0x800;
constexpr uint32_t kSpeakerStereo = 4;  // DSSPEAKER_STEREO
constexpr int32_t kVolumeMin = -10000, kPanMax = 10000;
constexpr uint32_t kFreqMin = 100, kFreqMax = 100000;
constexpr uint32_t kBufferMin = 4, kBufferMax = 0x0FFFFFFF;

// Guest object: +0 vtbl, +4 refcount (a mirror), +8 host id, +12 tag.
constexpr uint32_t kObjSize = 16;
constexpr uint32_t kTagDevice = 0x444E5344;  // "DSND"
constexpr uint32_t kTagBuffer = 0x46425344;  // "DSBF"

struct Guid {
  uint8_t b[16];
  bool operator==(const Guid& o) const { return memcmp(b, o.b, 16) == 0; }
};
constexpr Guid make_guid(uint32_t d1, uint16_t d2, uint16_t d3, std::initializer_list<uint8_t> d4) {
  Guid g{};
  g.b[0] = uint8_t(d1), g.b[1] = uint8_t(d1 >> 8), g.b[2] = uint8_t(d1 >> 16), g.b[3] = uint8_t(d1 >> 24);
  g.b[4] = uint8_t(d2), g.b[5] = uint8_t(d2 >> 8), g.b[6] = uint8_t(d3), g.b[7] = uint8_t(d3 >> 8);
  int i = 8;
  for (uint8_t x : d4) g.b[i++] = x;
  return g;
}
constexpr Guid kIidUnknown = make_guid(0x00000000, 0x0000, 0x0000, {0xC0, 0, 0, 0, 0, 0, 0, 0x46});
constexpr Guid kIidDirectSound = make_guid(0x279AFA83, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60});
constexpr Guid kIidDirectSoundBuffer =
    make_guid(0x279AFA85, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60});
constexpr Guid kGuidNull{};
constexpr Guid kDefaultPlayback =
    make_guid(0xDEF00000, 0x9C6D, 0x47ED, {0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03});

// A secondary buffer's memory, shared by the buffer and its duplicates.
struct Store {
  uint32_t addr = 0, size = 0;  // the guest backing store
  audio::BufferId buffer = 0;
  audio::WaveFormat format;
  int users = 0;
};

struct Object {
  bool device = false;
  bool dead = false;       // its IDirectSound was released: the voice is gone
  uint32_t guest = 0;      // the guest object
  uint32_t refs = 1;
  uint32_t owner = 0;      // a buffer's IDirectSound (host id)
  // Buffers:
  bool primary = false, primary_playing = false;
  uint32_t flags = 0;      // DSBCAPS_* as created
  uint32_t store = 0;      // Store id (0: primary)
  audio::VoiceId voice = 0;
  int32_t volume = 0, pan = 0;
  uint32_t frequency = 0;  // 0 = the format's own
};

struct DsState : RuntimeState {
  bool registered = false;
  std::vector<ShimEntry*> ds_slots, buf_slots;
  uint32_t ds_vtbl = 0, buf_vtbl = 0;
  std::map<uint32_t, Object> objects;  // by host id
  std::map<uint32_t, Store> stores;
  uint32_t next_id = 1, next_store = 1;
  std::set<std::string> unverified;
};

DsState& ds(Runtime& rt) { return rt.state<DsState>(); }

// The two vtables, built on first use (DirectSoundCreate).
void ensure_vtables(Runtime& rt) {
  DsState& s = ds(rt);
  if (s.ds_vtbl) return;
  auto build = [&](const std::vector<ShimEntry*>& slots) {
    uint32_t v = rt.heap().alloc(uint32_t(slots.size() * 4), true);
    if (!v) throw GuestError(GuestError::Kind::fatal, "no guest memory for the DirectSound vtables");
    for (size_t i = 0; i < slots.size(); i++) rt.mem().write_u32l(v + uint32_t(4 * i), rt.shims().thunk_address(*slots[i]));
    return v;
  };
  s.ds_vtbl = build(s.ds_slots);
  s.buf_vtbl = build(s.buf_slots);
}

uint32_t new_object(Runtime& rt, bool device, Object o) {
  DsState& s = ds(rt);
  ensure_vtables(rt);
  uint32_t g = rt.heap().alloc(kObjSize, true);
  if (!g) return 0;
  uint32_t id = s.next_id++;
  o.device = device;
  o.guest = g;
  o.refs = 1;
  auto& mem = rt.mem();
  mem.write_u32l(g + 0, device ? s.ds_vtbl : s.buf_vtbl);
  mem.write_u32l(g + 4, 1);
  mem.write_u32l(g + 8, id);
  mem.write_u32l(g + 12, device ? kTagDevice : kTagBuffer);
  s.objects[id] = o;
  return g;
}

// The host object behind a guest `this`, or null (a stale or foreign pointer).
Object* find(Runtime& rt, uint32_t self, bool device, uint32_t* id_out = nullptr) {
  if (!self) return nullptr;
  DsState& s = ds(rt);
  uint32_t id = rt.mem().read_u32l(self + 8);
  auto it = s.objects.find(id);
  if (it == s.objects.end() || it->second.guest != self || it->second.device != device) return nullptr;
  if (id_out) *id_out = id;
  return &it->second;
}

void mirror_refs(Runtime& rt, const Object& o) { rt.mem().write_u32l(o.guest + 4, o.refs); }

void drop_store(Runtime& rt, uint32_t sid) {
  DsState& s = ds(rt);
  auto it = s.stores.find(sid);
  if (it == s.stores.end()) return;
  if (--it->second.users > 0) return;
  audio_engine(rt).release_buffer(it->second.buffer);
  rt.heap().free(it->second.addr);
  s.stores.erase(it);
}

// A buffer's voice and store go (its own Release, or its IDirectSound's).
void kill_buffer(Runtime& rt, Object& o) {
  if (o.dead) return;
  o.dead = true;
  if (o.voice) audio_engine(rt).destroy_voice(o.voice, audio_now(rt));
  o.voice = 0;
  if (o.store) drop_store(rt, o.store);
  o.store = 0;
}

Store* store_of(Runtime& rt, const Object& o) {
  DsState& s = ds(rt);
  auto it = s.stores.find(o.store);
  return it == s.stores.end() ? nullptr : &it->second;
}

// Logs a method outside ADXPL510's verified set, once per method.
void note_unverified(Runtime& rt, const std::string& name) {
  DsState& s = ds(rt);
  if (s.unverified.insert(name).second) trace("sound", "unverified DirectSound method %s", name.c_str());
}

Guid read_guid(Runtime& rt, uint32_t p) {
  Guid g{};
  rt.mem().memcpy(g.b, p, 16);
  return g;
}

// A secondary buffer's write cursor: the play cursor plus 10 ms of the
// voice's current rate, whole blocks, wrapped; the play cursor when stopped.
uint32_t write_cursor(Runtime& rt, const Object& o, const Store& st, uint32_t play, bool playing) {
  if (!playing || !st.size) return play;
  uint32_t align = std::max<uint32_t>(st.format.block_align, 1);
  uint32_t rate = audio_engine(rt).rate(o.voice);
  if (!rate) rate = st.format.rate;
  uint64_t lead = uint64_t((rate + 99) / 100) * align;
  return uint32_t((uint64_t(play) + lead) % st.size);
}

void apply_gain(Runtime& rt, const Object& o) {
  if (o.voice) audio_engine(rt).set_gain(o.voice, audio::gain_from_ds(o.volume, o.pan), audio_now(rt));
}

// ---- IDirectSound ---------------------------------------------------------------------------------

void release_device(Runtime& rt, uint32_t id) {
  DsState& s = ds(rt);
  // Every buffer it made (duplicates included) loses its voice and store.
  for (auto& [bid, b] : s.objects)
    if (!b.device && b.owner == id) kill_buffer(rt, b);
  auto it = s.objects.find(id);
  if (it == s.objects.end()) return;
  rt.heap().free(it->second.guest);
  s.objects.erase(it);
}

uint32_t create_buffer(Runtime& rt, uint32_t owner, uint32_t desc, uint32_t* out_guest) {
  auto& mem = rt.mem();
  uint32_t size = mem.read_u32l(desc + 0), flags = mem.read_u32l(desc + 4), bytes = mem.read_u32l(desc + 8);
  uint32_t pwfx = mem.read_u32l(desc + 16);
  if (size < 0x14) return DSERR_INVALIDPARAM;
  Object o;
  o.owner = owner;
  o.flags = flags | kLocSoftware;
  if (flags & kPrimary) {
    // A primary buffer: no engine buffer (AUDIO.md §7.2); ADXPL510 makes none.
    if (bytes || pwfx) return DSERR_INVALIDPARAM;
    o.primary = true;
    o.flags = flags;
    uint32_t g = new_object(rt, false, o);
    if (!g) return DSERR_OUTOFMEMORY;
    *out_guest = g;
    trace("sound", "CreateSoundBuffer(primary) -> 0x%08X", g);
    return DS_OK;
  }
  audio::WaveFormat fmt;
  if (!pwfx) return DSERR_INVALIDPARAM;
  if (!read_guest_waveformat(rt, pwfx, fmt) || !audio::playable(fmt)) {
    trace("sound", "CreateSoundBuffer: format tag 0x%X is not playable PCM", mem.read_u16l(pwfx));
    return DSERR_BADFORMAT;
  }
  if (bytes < kBufferMin || bytes > kBufferMax) return DSERR_INVALIDPARAM;
  audio::Engine& e = audio_engine(rt);
  uint32_t addr = rt.heap().alloc(bytes, true);
  if (!addr) return DSERR_OUTOFMEMORY;
  audio::BufferId b = e.create_buffer(fmt, bytes);
  audio::VoiceId v = b ? e.create_voice(b, audio::Bus::wave) : 0;
  if (!v) {
    if (b) e.release_buffer(b);
    rt.heap().free(addr);
    return DSERR_OUTOFMEMORY;
  }
  DsState& s = ds(rt);
  uint32_t sid = s.next_store++;
  s.stores[sid] = Store{addr, bytes, b, fmt, 1};
  o.store = sid;
  o.voice = v;
  uint32_t g = new_object(rt, false, o);
  if (!g) {
    e.destroy_voice(v, audio_now(rt));
    drop_store(rt, sid);
    return DSERR_OUTOFMEMORY;
  }
  *out_guest = g;
  trace("sound", "CreateSoundBuffer(%u bytes, %u Hz %u-bit %s, flags 0x%X) -> 0x%08X", bytes, fmt.rate, fmt.bits,
        fmt.channels == 2 ? "stereo" : "mono", flags, g);
  return DS_OK;
}

void register_device_methods(Runtime& rt) {
  DsState& s = ds(rt);
  ShimRegistry& r = rt.shims();
  auto slot = [&](const char* name, uint16_t bytes, bool verified, ShimFn fn) {
    std::string n = std::string("IDirectSound::") + name;
    ShimFn wrapped = verified ? std::move(fn) : ShimFn([n, fn = std::move(fn)](Call& c) {
      note_unverified(c.rt, n);
      fn(c);
    });
    s.ds_slots.push_back(&r.add("<dsound>", n, Conv::stdcall_, bytes, std::move(wrapped)));
  };
  // 0x00 QueryInterface(this, riid, ppv)
  slot("QueryInterface", 12, false, [](Call& c) {
    Object* o = find(c.rt, c.arg(0), true);
    if (!o) return c.ret(DSERR_INVALIDCALL);
    if (!c.arg(1) || !c.arg(2)) return c.ret(DSERR_INVALIDPARAM);
    Guid iid = read_guid(c.rt, c.arg(1));
    if (iid == kIidUnknown || iid == kIidDirectSound) {
      o->refs++;
      mirror_refs(c.rt, *o);
      c.mem().write_u32l(c.arg(2), o->guest);
      return c.ret(DS_OK);
    }
    c.mem().write_u32l(c.arg(2), 0);
    c.ret(E_NOINTERFACE_);
  });
  // 0x04 AddRef(this)
  slot("AddRef", 4, false, [](Call& c) {
    Object* o = find(c.rt, c.arg(0), true);
    if (!o) return c.ret(0);
    o->refs++;
    mirror_refs(c.rt, *o);
    c.ret(o->refs);
  });
  // 0x08 Release(this) — VERIFIED (XNoiseMaker::Open 0x42739f, Close 0x4274d8)
  slot("Release", 4, true, [](Call& c) {
    uint32_t id = 0;
    Object* o = find(c.rt, c.arg(0), true, &id);
    if (!o) return c.ret(0);
    uint32_t left = --o->refs;
    if (left) {
      mirror_refs(c.rt, *o);
      return c.ret(left);
    }
    trace("sound", "IDirectSound 0x%08X released", c.arg(0));
    release_device(c.rt, id);
    c.ret(0);
  });
  // 0x0C CreateSoundBuffer(this, lpcDSBufferDesc, lplpDSBuffer, pUnkOuter) — VERIFIED
  slot("CreateSoundBuffer", 16, true, [](Call& c) {
    uint32_t id = 0;
    if (!find(c.rt, c.arg(0), true, &id)) return c.ret(DSERR_INVALIDCALL);
    if (c.arg(3)) return c.ret(DSERR_NOAGGREGATION);
    if (!c.arg(1) || !c.arg(2)) return c.ret(DSERR_INVALIDPARAM);
    uint32_t g = 0;
    uint32_t hr = create_buffer(c.rt, id, c.arg(1), &g);
    c.mem().write_u32l(c.arg(2), hr == DS_OK ? g : 0);
    c.ret(hr);
  });
  // 0x10 GetCaps(this, lpDSCaps)
  slot("GetCaps", 8, false, [](Call& c) {
    if (!find(c.rt, c.arg(0), true)) return c.ret(DSERR_INVALIDCALL);
    uint32_t p = c.arg(1);
    if (!p || c.mem().read_u32l(p) != 96) return c.ret(DSERR_INVALIDPARAM);
    c.mem().memset(p + 4, 0, 92);
    c.mem().write_u32l(p + 4, kCapsPrimaryMono | kCapsPrimaryStereo | kCapsPrimary8 | kCapsPrimary16 |
                                  kCapsContinuousRate | kCapsEmulDriver | kCapsSecondaryMono | kCapsSecondaryStereo |
                                  kCapsSecondary8 | kCapsSecondary16);
    c.mem().write_u32l(p + 8, kFreqMin);    // dwMinSecondarySampleRate
    c.mem().write_u32l(p + 12, kFreqMax);   // dwMaxSecondarySampleRate
    c.mem().write_u32l(p + 16, 1);          // dwPrimaryBuffers
    c.ret(DS_OK);
  });
  // 0x14 DuplicateSoundBuffer(this, lpDsbOriginal, lplpDsbDuplicate) — VERIFIED (PlayNoise 0x427e95)
  slot("DuplicateSoundBuffer", 12, true, [](Call& c) {
    uint32_t id = 0;
    if (!find(c.rt, c.arg(0), true, &id)) return c.ret(DSERR_INVALIDCALL);
    if (!c.arg(1) || !c.arg(2)) return c.ret(DSERR_INVALIDPARAM);
    c.mem().write_u32l(c.arg(2), 0);
    Object* src = find(c.rt, c.arg(1), false);
    if (!src || src->dead) return c.ret(DSERR_INVALIDPARAM);
    if (src->primary) return c.ret(DSERR_INVALIDCALL);
    Store* st = store_of(c.rt, *src);
    if (!st) return c.ret(DSERR_INVALIDCALL);
    audio::Engine& e = audio_engine(c.rt);
    audio::VoiceId v = e.create_voice(st->buffer, audio::Bus::wave);
    if (!v) return c.ret(DSERR_OUTOFMEMORY);
    Object o;
    o.owner = id;
    o.flags = src->flags;
    o.store = src->store;
    o.voice = v;
    o.volume = src->volume;
    o.pan = src->pan;
    o.frequency = src->frequency;
    st->users++;
    uint32_t sid = src->store;
    uint32_t g = new_object(c.rt, false, o);
    if (!g) {
      e.destroy_voice(v, audio_now(c.rt));
      drop_store(c.rt, sid);
      return c.ret(DSERR_OUTOFMEMORY);
    }
    // Volume, pan and frequency travel with the copy; it starts stopped at 0.
    uint32_t nid = c.mem().read_u32l(g + 8);
    Object& n = ds(c.rt).objects[nid];
    if (n.volume || n.pan) apply_gain(c.rt, n);
    if (n.frequency) e.set_rate(v, n.frequency, audio_now(c.rt));
    c.mem().write_u32l(c.arg(2), g);
    c.ret(DS_OK);
  });
  // 0x18 SetCooperativeLevel(this, hwnd, dwLevel) — VERIFIED (Open 0x42734a, 0x427374)
  slot("SetCooperativeLevel", 12, true, [](Call& c) {
    if (!find(c.rt, c.arg(0), true)) return c.ret(DSERR_INVALIDCALL);
    trace("sound", "SetCooperativeLevel(0x%X, %u)", c.arg(1), c.arg(2));
    c.ret(DS_OK);
  });
  // 0x1C Compact(this)
  slot("Compact", 4, false, [](Call& c) { c.ret(find(c.rt, c.arg(0), true) ? DS_OK : DSERR_INVALIDCALL); });
  // 0x20 GetSpeakerConfig(this, lpdwSpeakerConfig)
  slot("GetSpeakerConfig", 8, false, [](Call& c) {
    if (!find(c.rt, c.arg(0), true)) return c.ret(DSERR_INVALIDCALL);
    if (!c.arg(1)) return c.ret(DSERR_INVALIDPARAM);
    c.mem().write_u32l(c.arg(1), kSpeakerStereo);
    c.ret(DS_OK);
  });
  // 0x24 SetSpeakerConfig(this, dwSpeakerConfig)
  slot("SetSpeakerConfig", 8, false, [](Call& c) { c.ret(find(c.rt, c.arg(0), true) ? DS_OK : DSERR_INVALIDCALL); });
  // 0x28 Initialize(this, lpGuid)
  slot("Initialize", 8, false, [](Call& c) {
    c.ret(find(c.rt, c.arg(0), true) ? DSERR_ALREADYINITIALIZED : DSERR_INVALIDCALL);
  });
}

// ---- IDirectSoundBuffer -----------------------------------------------------------------------------

void register_buffer_methods(Runtime& rt) {
  DsState& s = ds(rt);
  ShimRegistry& r = rt.shims();
  auto slot = [&](const char* name, uint16_t bytes, bool verified, ShimFn fn) {
    std::string n = std::string("IDirectSoundBuffer::") + name;
    ShimFn wrapped = verified ? std::move(fn) : ShimFn([n, fn = std::move(fn)](Call& c) {
      note_unverified(c.rt, n);
      fn(c);
    });
    s.buf_slots.push_back(&r.add("<dsound>", n, Conv::stdcall_, bytes, std::move(wrapped)));
  };
  // A live buffer (its IDirectSound not released), else the call's error.
  auto live = [](Call& c, uint32_t self) -> Object* {
    Object* o = find(c.rt, self, false);
    return o && !o->dead ? o : nullptr;
  };
  // 0x00 QueryInterface(this, riid, ppv)
  slot("QueryInterface", 12, false, [](Call& c) {
    Object* o = find(c.rt, c.arg(0), false);
    if (!o) return c.ret(DSERR_INVALIDCALL);
    if (!c.arg(1) || !c.arg(2)) return c.ret(DSERR_INVALIDPARAM);
    Guid iid = read_guid(c.rt, c.arg(1));
    if (iid == kIidUnknown || iid == kIidDirectSoundBuffer) {
      o->refs++;
      mirror_refs(c.rt, *o);
      c.mem().write_u32l(c.arg(2), o->guest);
      return c.ret(DS_OK);
    }
    c.mem().write_u32l(c.arg(2), 0);
    c.ret(E_NOINTERFACE_);
  });
  // 0x04 AddRef(this)
  slot("AddRef", 4, false, [](Call& c) {
    Object* o = find(c.rt, c.arg(0), false);
    if (!o) return c.ret(0);
    o->refs++;
    mirror_refs(c.rt, *o);
    c.ret(o->refs);
  });
  // 0x08 Release(this) — VERIFIED (GiveTime, LoadByID 0x428ee3)
  slot("Release", 4, true, [](Call& c) {
    uint32_t id = 0;
    Object* o = find(c.rt, c.arg(0), false, &id);
    if (!o) return c.ret(0);
    uint32_t left = --o->refs;
    if (left) {
      mirror_refs(c.rt, *o);
      return c.ret(left);
    }
    kill_buffer(c.rt, *o);
    c.rt.heap().free(o->guest);
    ds(c.rt).objects.erase(id);
    c.ret(0);
  });
  // 0x0C GetCaps(this, lpDSBufferCaps)
  slot("GetCaps", 8, false, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    uint32_t p = c.arg(1);
    if (!p || c.mem().read_u32l(p) != 20) return c.ret(DSERR_INVALIDPARAM);
    Store* st = store_of(c.rt, *o);
    c.mem().write_u32l(p + 4, o->flags);
    c.mem().write_u32l(p + 8, st ? st->size : 0);
    c.mem().write_u32l(p + 12, 0);  // dwUnlockTransferRate
    c.mem().write_u32l(p + 16, 0);  // dwPlayCpuOverhead
    c.ret(DS_OK);
  });
  // 0x10 GetCurrentPosition(this, lpdwPlay, lpdwWrite) — VERIFIED (GiveTime 0x42762c)
  slot("GetCurrentPosition", 12, true, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    uint32_t play = 0, write = 0;
    if (Store* st = store_of(c.rt, *o)) {
      audio::Engine& e = audio_engine(c.rt);
      audio::Time now = audio_now(c.rt);
      play = e.cursor(o->voice, now);
      write = write_cursor(c.rt, *o, *st, play, e.playing(o->voice, now));
    }
    if (c.arg(1)) c.mem().write_u32l(c.arg(1), play);
    if (c.arg(2)) c.mem().write_u32l(c.arg(2), write);
    c.ret(DS_OK);
  });
  // 0x14 GetFormat(this, lpwfxFormat, dwSizeAllocated, lpdwSizeWritten)
  slot("GetFormat", 16, false, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    Store* st = store_of(c.rt, *o);
    audio::WaveFormat f = st ? st->format : audio::pcm_format(22050, 2, 8);  // the primary's default
    uint32_t need = uint32_t(audio::waveformat_bytes(f).size());
    if (!c.arg(1)) {
      if (!c.arg(3)) return c.ret(DSERR_INVALIDPARAM);
      c.mem().write_u32l(c.arg(3), need);
      return c.ret(DS_OK);
    }
    uint32_t n = write_guest_waveformat(c.rt, c.arg(1), f, c.arg(2));
    if (c.arg(3)) c.mem().write_u32l(c.arg(3), n);
    c.ret(DS_OK);
  });
  // 0x18 GetVolume(this, lplVolume)
  slot("GetVolume", 8, false, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    if (!c.arg(1)) return c.ret(DSERR_INVALIDPARAM);
    if (!(o->flags & kCtrlVolume) && !o->primary) return c.ret(DSERR_CONTROLUNAVAIL);
    c.mem().write_u32l(c.arg(1), uint32_t(o->volume));
    c.ret(DS_OK);
  });
  // 0x1C GetPan(this, lplPan)
  slot("GetPan", 8, false, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    if (!c.arg(1)) return c.ret(DSERR_INVALIDPARAM);
    if (!(o->flags & kCtrlPan) && !o->primary) return c.ret(DSERR_CONTROLUNAVAIL);
    c.mem().write_u32l(c.arg(1), uint32_t(o->pan));
    c.ret(DS_OK);
  });
  // 0x20 GetFrequency(this, lpdwFrequency)
  slot("GetFrequency", 8, false, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    if (!c.arg(1)) return c.ret(DSERR_INVALIDPARAM);
    Store* st = store_of(c.rt, *o);
    if (!st || !(o->flags & kCtrlFrequency)) return c.ret(DSERR_CONTROLUNAVAIL);
    c.mem().write_u32l(c.arg(1), o->frequency ? o->frequency : st->format.rate);
    c.ret(DS_OK);
  });
  // 0x24 GetStatus(this, lpdwStatus) — VERIFIED (XNoiseMaker::GetStatus 0x428c39)
  slot("GetStatus", 8, true, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    if (!c.arg(1)) return c.ret(DSERR_INVALIDPARAM);
    uint32_t status = 0;
    if (o->primary) {
      status = o->primary_playing ? kStatusPlaying | kStatusLooping : 0;
    } else {
      audio::Engine& e = audio_engine(c.rt);
      audio::Time now = audio_now(c.rt);
      if (e.playing(o->voice, now)) status = kStatusPlaying | (e.looping(o->voice, now) ? kStatusLooping : 0);
    }
    c.mem().write_u32l(c.arg(1), status);
    c.ret(DS_OK);
  });
  // 0x28 Initialize(this, lpDirectSound, lpcDSBufferDesc)
  slot("Initialize", 12, false, [](Call& c) {
    c.ret(find(c.rt, c.arg(0), false) ? DSERR_ALREADYINITIALIZED : DSERR_INVALIDCALL);
  });
  // 0x2C Lock(this, dwWriteCursor, dwWriteBytes, lplpvAudioPtr1, lpdwAudioBytes1,
  //           lplpvAudioPtr2, lpdwAudioBytes2, dwFlags) — VERIFIED (LoadByID 0x428e81, GiveTime, PlayNoiseList)
  slot("Lock", 32, true, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    if (o->primary) return c.ret(DSERR_PRIOLEVELNEEDED);  // DSSCL_NORMAL cannot write the primary buffer
    Store* st = store_of(c.rt, *o);
    if (!st || !c.arg(3) || !c.arg(4)) return c.ret(DSERR_INVALIDPARAM);
    uint32_t offset = c.arg(1), bytes = c.arg(2), flags = c.arg(7);
    if (flags & kLockFromWriteCursor) {
      audio::Engine& e = audio_engine(c.rt);
      audio::Time now = audio_now(c.rt);
      offset = write_cursor(c.rt, *o, *st, e.cursor(o->voice, now), e.playing(o->voice, now));
    }
    if (flags & kLockEntireBuffer) bytes = st->size;
    if (!bytes || bytes > st->size || offset >= st->size) return c.ret(DSERR_INVALIDPARAM);
    uint32_t n1 = std::min(bytes, st->size - offset), n2 = bytes - n1;
    if (!c.arg(5)) n2 = 0;  // no second pointer: the lock ends at the end of the buffer
    c.mem().write_u32l(c.arg(3), st->addr + offset);
    c.mem().write_u32l(c.arg(4), n1);
    if (c.arg(5)) c.mem().write_u32l(c.arg(5), n2 ? st->addr : 0);
    if (c.arg(6)) c.mem().write_u32l(c.arg(6), n2);
    c.ret(DS_OK);
  });
  // 0x30 Play(this, dwReserved1, dwPriority, dwFlags) — VERIFIED (DoPlaySound 0x428bdd, 0x428bf0)
  slot("Play", 16, true, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    if (o->primary) {
      o->primary_playing = true;
      return c.ret(DS_OK);
    }
    audio_engine(c.rt).play(o->voice, c.arg(3) & kPlayLooping, audio_now(c.rt));
    c.ret(DS_OK);
  });
  // 0x34 SetCurrentPosition(this, dwNewPosition)
  slot("SetCurrentPosition", 8, false, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    if (o->primary) return c.ret(DSERR_INVALIDCALL);
    Store* st = store_of(c.rt, *o);
    if (!st || c.arg(1) >= st->size) return c.ret(DSERR_INVALIDPARAM);
    audio_engine(c.rt).set_cursor(o->voice, c.arg(1), audio_now(c.rt));
    c.ret(DS_OK);
  });
  // 0x38 SetFormat(this, lpcfxFormat): the primary's format needs DSSCL_PRIORITY; secondaries have theirs.
  slot("SetFormat", 8, false, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    c.ret(o->primary ? DSERR_PRIOLEVELNEEDED : DSERR_INVALIDCALL);
  });
  // 0x3C SetVolume(this, lVolume) — VERIFIED (DoPlaySound 0x428b9d, GiveTime)
  slot("SetVolume", 8, true, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    int32_t v = c.iarg(1);
    if (v > 0 || v < kVolumeMin) return c.ret(DSERR_INVALIDPARAM);
    if (!(o->flags & kCtrlVolume) && !o->primary) return c.ret(DSERR_CONTROLUNAVAIL);
    if (v == o->volume) return c.ret(DS_OK);
    o->volume = v;
    apply_gain(c.rt, *o);
    c.ret(DS_OK);
  });
  // 0x40 SetPan(this, lPan) — VERIFIED (DoPlaySound 0x428b86, PlayNoiseList)
  slot("SetPan", 8, true, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    int32_t p = c.iarg(1);
    if (p < -kPanMax || p > kPanMax) return c.ret(DSERR_INVALIDPARAM);
    if (!(o->flags & kCtrlPan) && !o->primary) return c.ret(DSERR_CONTROLUNAVAIL);
    if (p == o->pan) return c.ret(DS_OK);
    o->pan = p;
    apply_gain(c.rt, *o);
    c.ret(DS_OK);
  });
  // 0x44 SetFrequency(this, dwFrequency): 0 = the format's own.
  slot("SetFrequency", 8, false, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    uint32_t f = c.arg(1);
    if (o->primary || !(o->flags & kCtrlFrequency)) return c.ret(DSERR_CONTROLUNAVAIL);
    if (f && (f < kFreqMin || f > kFreqMax)) return c.ret(DSERR_INVALIDPARAM);
    o->frequency = f;
    audio_engine(c.rt).set_rate(o->voice, f, audio_now(c.rt));
    c.ret(DS_OK);
  });
  // 0x48 Stop(this) — VERIFIED (FindFreeChannel, StopNoise, StopAllNoise, GiveTime)
  slot("Stop", 4, true, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    if (o->primary) {
      o->primary_playing = false;
      return c.ret(DS_OK);
    }
    audio_engine(c.rt).stop(o->voice, audio_now(c.rt));
    c.ret(DS_OK);
  });
  // 0x4C Unlock(this, lpvAudioPtr1, dwAudioBytes1, lpvAudioPtr2, dwAudioBytes2) — VERIFIED
  // Copies exactly the given ranges of the backing store into the engine buffer.
  slot("Unlock", 20, true, [live](Call& c) {
    Object* o = live(c, c.arg(0));
    if (!o) return c.ret(DSERR_INVALIDCALL);
    if (o->primary) return c.ret(DSERR_INVALIDPARAM);
    Store* st = store_of(c.rt, *o);
    if (!st) return c.ret(DSERR_INVALIDPARAM);
    struct Range {
      uint32_t p, n;
    };
    Range ranges[2] = {{c.arg(1), c.arg(2)}, {c.arg(3), c.arg(4)}};
    for (const Range& g : ranges) {
      if (!g.p || !g.n) continue;
      if (g.p < st->addr || g.p - st->addr >= st->size || g.n > st->size - (g.p - st->addr))
        return c.ret(DSERR_INVALIDPARAM);
    }
    audio::Engine& e = audio_engine(c.rt);
    audio::Time now = audio_now(c.rt);
    for (const Range& g : ranges) {
      if (!g.p || !g.n) continue;
      const uint8_t* bytes = c.mem().at<uint8_t>(g.p, g.n);
      e.write_buffer(st->buffer, g.p - st->addr, std::span<const uint8_t>(bytes, g.n), now);
    }
    c.ret(DS_OK);
  });
  // 0x50 Restore(this): our buffers are never lost.
  slot("Restore", 4, false, [](Call& c) { c.ret(find(c.rt, c.arg(0), false) ? DS_OK : DSERR_INVALIDCALL); });
}

}  // namespace

void register_dsound(Runtime& rt) {
  DsState& s = ds(rt);
  if (s.registered) return;
  s.registered = true;
  register_device_methods(rt);
  register_buffer_methods(rt);
  // DirectSoundCreate(lpGuid, lplpDS, pUnkOuter): the default device only.
  rt.shims().add("DSOUND.DLL", "DirectSoundCreate", Conv::stdcall_, 12, [](Call& c) {
    if (!c.arg(1)) return c.ret(DSERR_INVALIDPARAM);
    c.mem().write_u32l(c.arg(1), 0);
    if (c.arg(2)) return c.ret(DSERR_NOAGGREGATION);
    if (c.arg(0)) {
      Guid g = read_guid(c.rt, c.arg(0));
      if (!(g == kGuidNull) && !(g == kDefaultPlayback)) return c.ret(DSERR_INVALIDPARAM);
    }
    uint32_t g = new_object(c.rt, true, Object{});
    if (!g) return c.ret(DSERR_OUTOFMEMORY);
    c.mem().write_u32l(c.arg(1), g);
    trace("sound", "DirectSoundCreate -> 0x%08X", g);
    c.ret(DS_OK);
  });
}

void dsound_end_process(Runtime& rt) {
  for (auto& [id, o] : ds(rt).objects)
    if (!o.device) kill_buffer(rt, o);
}

void register_dsound_absent(Runtime& rt) {
  if (ds(rt).registered) return;
  rt.shims().impl("DSOUND.DLL", "DirectSoundCreate", [](Call& c) {
    if (c.arg(1)) c.mem().write_u32l(c.arg(1), 0);
    trace("sound", "DirectSoundCreate -> DSERR_NODRIVER (sound is off)");
    c.ret(0x88780078);  // DSERR_NODRIVER
  });
}

}  // namespace adw::win32
