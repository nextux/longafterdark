// Unit tests for the Win32 sound shims over the host audio engine
// (docs/AUDIO.md §7, §10.2): DirectSound (vtable layout, refcounts,
// CreateSoundBuffer validation, Lock/Unlock wrap and copy, duplicates sharing
// data, positions and status against virtual time, volume/pan/frequency),
// ACM (the guest enumeration callback in index order, stopping on FALSE;
// conversion equal to audio::decode), waveOut (WHDR_DONE exactly at the
// virtual end; pause/reset/close errors; window and function callbacks),
// MCI (every command and flag of §7.5; notifications), aux/mixer, the Deluxe
// music renames, the guest time of every engine call (the clock peeked,
// never moved), and the sound-off answers, which must stay those of the
// silent host. Linked into adw_win32_tests.
//
// Two engines: FakeEngine records every call (the call contract), and
// audio::make_engine with guest sound on and no sink (headless: nothing is
// ever played) checks the timing model end to end.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "X86Emulator.hh"
#include "adw/core/audio.h"
#include "check.h"
#include "win32/audio.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"
#include "win32/vfs.hh"

using namespace adw;
using namespace adw::win32;

namespace {

// ---- a recording engine ------------------------------------------------------------------------------

struct FakeEngine : audio::Engine {
  audio::Config cfg;
  std::vector<std::string> calls;
  std::vector<audio::Time> times;  // every t passed, in order
  struct Buf {
    audio::WaveFormat f;
    std::vector<uint8_t> bytes;
    bool released = false;
  };
  struct Voice {
    audio::BufferId buf = 0;
    bool playing = false, loop = false, destroyed = false;
    uint32_t cursor = 0, rate = 0;
    audio::Gain gain;
  };
  struct Stream {
    audio::WaveFormat f;
    std::vector<uint64_t> cookies;
    uint64_t position = 0;
    bool paused = false, closed = false;
  };
  struct Song {
    bool playing = false, closed = false;
    uint64_t position = 0, length = 2'000'000;
  };
  std::map<uint32_t, Buf> bufs;
  std::map<uint32_t, Voice> voices;
  std::map<uint32_t, Stream> streams;
  std::map<uint32_t, Song> songs;
  audio::Gain buses[2];
  std::vector<audio::Event> pending;  // the test queues events here
  uint32_t next = 1;

  explicit FakeEngine(bool on = true) { cfg.guest_sound = on; }
  void t(audio::Time x) { times.push_back(x); }
  void note(const std::string& s) { calls.push_back(s); }
  bool called(const std::string& prefix) const {
    for (const std::string& c : calls)
      if (c.rfind(prefix, 0) == 0) return true;
    return false;
  }

  const audio::Config& config() const override { return cfg; }
  audio::BufferId create_buffer(const audio::WaveFormat& pcm, uint32_t bytes) override {
    if (!audio::playable(pcm) || !bytes) return 0;
    uint32_t id = next++;
    bufs[id] = Buf{pcm, std::vector<uint8_t>(bytes, 0), false};
    note("create_buffer " + std::to_string(bytes));
    return id;
  }
  void write_buffer(audio::BufferId b, uint32_t offset, std::span<const uint8_t> bytes, audio::Time x) override {
    t(x);
    note("write_buffer " + std::to_string(b) + " " + std::to_string(offset) + " " + std::to_string(bytes.size()));
    auto& v = bufs[b].bytes;
    for (size_t i = 0; i < bytes.size() && offset + i < v.size(); i++) v[offset + i] = bytes[i];
  }
  void release_buffer(audio::BufferId b) override {
    note("release_buffer " + std::to_string(b));
    bufs[b].released = true;
  }
  const audio::WaveFormat* buffer_format(audio::BufferId b) const override {
    auto it = bufs.find(b);
    return it == bufs.end() ? nullptr : &it->second.f;
  }
  uint32_t buffer_size(audio::BufferId b) const override {
    auto it = bufs.find(b);
    return it == bufs.end() ? 0 : uint32_t(it->second.bytes.size());
  }
  audio::VoiceId create_voice(audio::BufferId b, audio::Bus) override {
    uint32_t id = next++;
    voices[id].buf = b;
    note("create_voice " + std::to_string(b));
    return id;
  }
  void destroy_voice(audio::VoiceId v, audio::Time x) override {
    t(x);
    voices[v].destroyed = true;
    note("destroy_voice " + std::to_string(v));
  }
  void play(audio::VoiceId v, bool loop, audio::Time x) override {
    t(x);
    voices[v].playing = true;
    voices[v].loop = loop;
    note(std::string("play ") + std::to_string(v) + (loop ? " loop" : ""));
  }
  void stop(audio::VoiceId v, audio::Time x) override {
    t(x);
    voices[v].playing = false;
    note("stop " + std::to_string(v));
  }
  void set_cursor(audio::VoiceId v, uint32_t off, audio::Time x) override {
    t(x);
    voices[v].cursor = off;
    note("set_cursor " + std::to_string(off));
  }
  uint32_t cursor(audio::VoiceId v, audio::Time x) override {
    t(x);
    return voices[v].cursor;
  }
  bool playing(audio::VoiceId v, audio::Time x) override {
    t(x);
    return voices[v].playing;
  }
  bool looping(audio::VoiceId v, audio::Time x) override {
    t(x);
    return voices[v].playing && voices[v].loop;
  }
  audio::Time end_time(audio::VoiceId, audio::Time x) override {
    t(x);
    return 0;
  }
  void set_gain(audio::VoiceId v, audio::Gain g, audio::Time x) override {
    t(x);
    voices[v].gain = g;
    note("set_gain " + std::to_string(v) + " " + std::to_string(g.left) + " " + std::to_string(g.right));
  }
  void set_rate(audio::VoiceId v, uint32_t hz, audio::Time x) override {
    t(x);
    voices[v].rate = hz;
    note("set_rate " + std::to_string(v) + " " + std::to_string(hz));
  }
  uint32_t rate(audio::VoiceId v) const override {
    auto it = voices.find(v);
    if (it == voices.end()) return 0;
    if (it->second.rate) return it->second.rate;
    auto b = bufs.find(it->second.buf);
    return b == bufs.end() ? 0 : b->second.f.rate;
  }
  audio::StreamId open_stream(const audio::WaveFormat& pcm, audio::Bus, audio::Time x) override {
    t(x);
    if (!audio::playable(pcm)) return 0;
    uint32_t id = next++;
    streams[id].f = pcm;
    note("open_stream");
    return id;
  }
  void stream_write(audio::StreamId s, std::span<const uint8_t> bytes, uint64_t cookie, audio::Time x) override {
    t(x);
    streams[s].cookies.push_back(cookie);
    note("stream_write " + std::to_string(bytes.size()));
  }
  void stream_pause(audio::StreamId s, audio::Time x) override {
    t(x);
    streams[s].paused = true;
    note("stream_pause");
  }
  void stream_restart(audio::StreamId s, audio::Time x) override {
    t(x);
    streams[s].paused = false;
    note("stream_restart");
  }
  void stream_reset(audio::StreamId s, audio::Time x) override {
    t(x);
    for (uint64_t c : streams[s].cookies) pending.push_back({audio::Event::Kind::chunk_done, s, c, x});
    streams[s].cookies.clear();
    note("stream_reset");
  }
  uint64_t stream_position(audio::StreamId s, audio::Time x) override {
    t(x);
    return streams[s].position;
  }
  bool stream_paused(audio::StreamId s) const override {
    auto it = streams.find(s);
    return it != streams.end() && it->second.paused;
  }
  void set_stream_gain(audio::StreamId, audio::Gain, audio::Time x) override { t(x); }
  void close_stream(audio::StreamId s, audio::Time x) override {
    t(x);
    streams[s].closed = true;
    note("close_stream");
  }
  audio::SongId load_song(std::span<const uint8_t> smf, std::string* error) override {
    if (smf.size() < 4 || memcmp(smf.data(), "MThd", 4) != 0) {
      if (error) *error = "not an SMF";
      return 0;
    }
    uint32_t id = next++;
    songs[id];
    note("load_song " + std::to_string(smf.size()));
    return id;
  }
  void song_play(audio::SongId s, audio::Time x) override {
    t(x);
    songs[s].playing = true;
    note("song_play");
  }
  void song_stop(audio::SongId s, audio::Time x) override {
    t(x);
    songs[s].playing = false;
    note("song_stop");
  }
  void song_seek(audio::SongId s, uint64_t us, audio::Time x) override {
    t(x);
    songs[s].playing = false;
    songs[s].position = std::min(us, songs[s].length);
    note("song_seek " + std::to_string(us));
  }
  uint64_t song_position(audio::SongId s, audio::Time x) override {
    t(x);
    return songs[s].position;
  }
  uint64_t song_length(audio::SongId s) const override {
    auto it = songs.find(s);
    return it == songs.end() ? 0 : it->second.length;
  }
  bool song_playing(audio::SongId s, audio::Time x) override {
    t(x);
    return songs[s].playing;
  }
  void close_song(audio::SongId s, audio::Time x) override {
    t(x);
    songs[s].closed = true;
    songs[s].playing = false;
    note("close_song");
  }
  void set_bus_gain(audio::Bus b, audio::Gain g, audio::Time x) override {
    t(x);
    buses[int(b)] = g;
    note(std::string("set_bus_gain ") + (b == audio::Bus::midi ? "midi" : "wave"));
  }
  audio::Gain bus_gain(audio::Bus b) const override { return buses[int(b)]; }
  void poll(audio::Time x, std::vector<audio::Event>& out) override {
    t(x);
    std::vector<audio::Event> keep;
    for (const audio::Event& e : pending) (e.at <= x ? out : keep).push_back(e);
    pending = keep;
  }
  audio::Time next_event_time() override {
    audio::Time m = 0;
    for (const audio::Event& e : pending) m = m ? std::min(m, e.at) : e.at;
    return m;
  }
  void advance(audio::Time x) override { t(x); }
  void shutdown(audio::Time x) override { t(x); }
  audio::Stats stats() const override { return {}; }
};

// ---- a runtime with the shims ----------------------------------------------------------------------------

struct Rt {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 1000};
  InputState input;
  std::unique_ptr<Runtime> rt;
  uint32_t buf = 0, data = 0, code = 0;
  Rt() {
    RuntimeOptions o;
    o.heap_size = 32u << 20;
    o.call_budget = 50'000'000;
    rt = std::make_unique<Runtime>(o, clock, &input);
    register_all_shims(rt->shims());
    rt->vfs().mount_overlay("C:\\WINDOWS", "", "");
    rt->vfs().mount_overlay("C:\\AFTERDRK", "", "");
    rt->vfs().set_cwd("C:\\AFTERDRK");
    buf = rt->heap().alloc(0x10000, true);
    data = rt->heap().alloc(0x1000, true);
    code = rt->heap().alloc(0x1000, true);
    clock.begin_frame();  // frame 0: virtual time 0
  }
  void attach(audio::Engine& e, bool deluxe = false) {
    AudioOptions o;
    o.deluxe = deluxe;
    attach_audio(*rt, e, o);
  }
  // Moves virtual time forward by `us`.
  void advance(uint32_t us) {
    clock.set_step_us(us);
    clock.begin_frame();
  }
  auto& mem() { return rt->mem(); }
  uint32_t call(const char* dll, const char* name, std::initializer_list<uint32_t> args) {
    ShimEntry& e = rt->shims().get(dll, name);
    return rt->call_guest(rt->shims().thunk_address(e), args, e.conv);
  }
  uint32_t mm(const char* name, std::initializer_list<uint32_t> args) { return call("WINMM.DLL", name, args); }
  uint32_t acm(const char* name, std::initializer_list<uint32_t> args) { return call("MSACM32.DLL", name, args); }
  // A COM method: slot = byte offset in the vtable; `this` goes first.
  uint32_t com(uint32_t obj, uint32_t slot, std::vector<uint32_t> args = {}) {
    uint32_t fn = mem().read_u32l(mem().read_u32l(obj) + slot);
    args.insert(args.begin(), obj);
    return rt->call_guest(fn, std::span<const uint32_t>(args), Conv::stdcall_);
  }
  uint32_t str(uint32_t off, const std::string& s) {
    write_cstr(mem(), buf + off, s, s.size() + 1);
    return buf + off;
  }
  uint32_t u32(uint32_t addr) { return mem().read_u32l(addr); }
  // Assembles `text` at `code` ("DATA" = the data table's address).
  cpu::X86Emulator::AssembleResult load(std::string text) {
    for (size_t p; (p = text.find("DATA")) != std::string::npos;) text.replace(p, 4, std::to_string(data));
    auto r = cpu::X86Emulator::assemble(text, nullptr, code);
    mem().memcpy(code, r.code.data(), r.code.size());
    return r;
  }
  uint32_t label(const cpu::X86Emulator::AssembleResult& r, const char* name) {
    return code + r.label_offsets.at(name);
  }
  // A WAVEFORMATEX at buf + off.
  uint32_t wfx(uint32_t off, const audio::WaveFormat& f) {
    auto b = audio::waveformat_bytes(f);
    mem().memcpy(buf + off, b.data(), b.size());
    return buf + off;
  }
};

// DirectSound slots (AUDIO.md §2.1).
constexpr uint32_t DS_Release = 0x08, DS_CreateSoundBuffer = 0x0C, DS_GetCaps = 0x10, DS_Duplicate = 0x14,
                   DS_SetCooperativeLevel = 0x18, DS_QueryInterface = 0x00, DS_AddRef = 0x04;
constexpr uint32_t B_Release = 0x08, B_GetCaps = 0x0C, B_GetCurrentPosition = 0x10, B_GetFormat = 0x14,
                   B_GetVolume = 0x18, B_GetFrequency = 0x20, B_GetStatus = 0x24, B_Lock = 0x2C, B_Play = 0x30,
                   B_SetCurrentPosition = 0x34, B_SetVolume = 0x3C, B_SetPan = 0x40, B_SetFrequency = 0x44,
                   B_Stop = 0x48, B_Unlock = 0x4C;
constexpr uint32_t DSERR_INVALIDPARAM = 0x80070057, DSERR_BADFORMAT = 0x88780064, DSERR_NOAGGREGATION = 0x80040110,
                   DSERR_CONTROLUNAVAIL = 0x8878001E, E_NOINTERFACE_ = 0x80004002;

// DirectSoundCreate through LoadLibraryExA/GetProcAddress, as XNoiseMaker::Open does.
uint32_t create_ds(Rt& t) {
  uint32_t h = t.call("KERNEL32.DLL", "LoadLibraryExA", {t.str(0x100, "dsound.dll"), 0, 0});
  if (!h) return 0;
  uint32_t fn = t.call("KERNEL32.DLL", "GetProcAddress", {h, t.str(0x120, "DirectSoundCreate")});
  if (!fn) return 0;
  uint32_t slot = t.buf + 0x140;
  if (t.rt->call_guest(fn, {0, slot, 0}, Conv::stdcall_) != 0) return 0;
  return t.u32(slot);
}

// A secondary buffer of `bytes` in format f (DSBUFFERDESC at buf+0x200, format at buf+0x240).
uint32_t make_buffer(Rt& t, uint32_t ds, const audio::WaveFormat& f, uint32_t bytes, uint32_t* hr = nullptr,
                     uint32_t flags = 0xE8, uint32_t size = 0x14) {
  uint32_t desc = t.buf + 0x200;
  t.mem().memset(desc, 0, 0x14);
  t.mem().write_u32l(desc + 0, size);
  t.mem().write_u32l(desc + 4, flags);
  t.mem().write_u32l(desc + 8, bytes);
  t.mem().write_u32l(desc + 16, t.wfx(0x240, f));
  uint32_t out = t.buf + 0x280;
  uint32_t r = t.com(ds, DS_CreateSoundBuffer, {desc, out, 0});
  if (hr) *hr = r;
  return r ? 0 : t.u32(out);
}

std::string thunk_name(Rt& t, uint32_t addr) {
  int id = ShimRegistry::thunk_at(addr);
  ShimEntry* e = id < 0 ? nullptr : t.rt->shims().by_thunk(uint16_t(id));
  return e ? e->name : std::string("?");
}

// A format-0 SMF: one note of 500 ms at the default tempo (480 PPQN).
std::vector<uint8_t> tiny_smf() {
  std::vector<uint8_t> v = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k', 0, 0, 0, 13,
                            0x00, 0x90, 0x3C, 0x40, 0x83, 0x60, 0x80, 0x3C, 0x00, 0x00, 0xFF, 0x2F, 0x00};
  return v;
}

bool has_message(Rt& t, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
  uint32_t m = t.buf + 0x3000;
  while (t.call("USER32.DLL", "PeekMessageA", {m, hwnd, msg, msg, PM_REMOVE})) {
    if (t.u32(m + 8) == wparam && t.u32(m + 12) == lparam) return true;
  }
  return false;
}

}  // namespace

// ---- sound off: the silent host's answers -----------------------------------------------------------------

TEST(audio_off_answers_as_the_silent_host) {
  for (int attached = 0; attached < 2; attached++) {
    Rt t;
    FakeEngine off(false);
    if (attached) t.attach(off);
    CHECK_EQ(t.call("KERNEL32.DLL", "LoadLibraryExA", {t.str(0, "dsound.dll"), 0, 0}), 0u);
    CHECK_EQ(t.mm("auxGetNumDevs", {}), 0u);
    CHECK_EQ(t.mm("auxGetVolume", {1, t.buf + 0x100}), 6u);
    CHECK_EQ(t.mm("auxSetVolume", {1, 0x7FFF7FFF}), 6u);
    CHECK_EQ(t.mm("waveOutGetNumDevs", {}), 0u);
    CHECK_EQ(t.mm("waveOutGetDevCapsA", {0, t.buf + 0x100, 52}), 2u);
    t.mem().write_u32l(t.buf + 0x180, 0x1234);
    CHECK_EQ(t.mm("waveOutOpen", {t.buf + 0x180, 0, t.wfx(0x200, audio::pcm_format(22050, 1, 16)), 0, 0, 0}), 6u);
    CHECK_EQ(t.u32(t.buf + 0x180), 0u);
    CHECK_EQ(t.mm("waveOutWrite", {0x7A00, t.buf, 32}), 5u);
    CHECK_EQ(t.mm("mciSendCommandA", {0, 0x803, 0x2000, t.buf + 0x300}), 306u);
    CHECK_EQ(t.mm("mciGetErrorStringA", {306, t.buf + 0x400, 256}), 1u);
    CHECK_EQ(read_cstr(t.mem(), t.buf + 0x400), std::string("The specified device is not installed."));
    CHECK_EQ(t.mm("mixerGetLineInfoA", {0, t.buf + 0x100, 0x50000000}), 6u);
    CHECK_EQ(t.acm("acmMetrics", {0, 50, t.buf + 0x100}), 6u);
    CHECK_EQ(t.acm("acmFormatEnumA", {0, t.buf + 0x100, t.code, 0, 0x10000}), 6u);
    CHECK_EQ(t.acm("acmFormatSuggest", {0, t.buf, t.buf + 0x100, 18, 0}), 6u);
    CHECK_EQ(t.acm("acmStreamOpen", {t.buf, 0, t.buf, t.buf + 0x100, 0, 0, 0, 0}), 6u);
    CHECK_EQ(t.acm("acmStreamSize", {1, 100, t.buf, 0}), 6u);
    CHECK_EQ(t.acm("acmStreamConvert", {1, t.buf, 0}), 6u);
    CHECK(off.calls.empty());  // a disabled engine is asked nothing but config()
  }
}

// ---- DirectSound -------------------------------------------------------------------------------------

// dsound.dll loads; every vtable slot resolves to its own thunk.
TEST(audio_dsound_vtables_and_refcounts) {
  Rt t;
  FakeEngine e;
  t.attach(e);
  uint32_t ds = create_ds(t);
  CHECK(ds != 0);
  if (!ds) return;
  static const char* kDs[] = {"QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps",
                              "DuplicateSoundBuffer", "SetCooperativeLevel", "Compact", "GetSpeakerConfig",
                              "SetSpeakerConfig", "Initialize"};
  uint32_t vt = t.u32(ds);
  for (int i = 0; i < 11; i++) CHECK_EQ(thunk_name(t, t.u32(vt + 4 * i)), std::string("IDirectSound::") + kDs[i]);
  CHECK_EQ(t.com(ds, DS_SetCooperativeLevel, {0x10010, 1}), 0u);
  // Refcounts (mirrored at +4), QueryInterface.
  CHECK_EQ(t.com(ds, DS_AddRef), 2u);
  CHECK_EQ(t.u32(ds + 4), 2u);
  uint32_t iid = t.buf + 0x500;
  const uint8_t unknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
  t.mem().memcpy(iid, unknown, 16);
  CHECK_EQ(t.com(ds, DS_QueryInterface, {iid, t.buf + 0x520}), 0u);
  CHECK_EQ(t.u32(t.buf + 0x520), ds);
  t.mem().write_u8(iid, 1);
  CHECK_EQ(t.com(ds, DS_QueryInterface, {iid, t.buf + 0x520}), E_NOINTERFACE_);
  CHECK_EQ(t.u32(t.buf + 0x520), 0u);
  // GetCaps needs dwSize 96.
  t.mem().write_u32l(t.buf + 0x600, 96);
  CHECK_EQ(t.com(ds, DS_GetCaps, {t.buf + 0x600}), 0u);
  CHECK(t.u32(t.buf + 0x604) & 0x20);  // DSCAPS_EMULDRIVER
  t.mem().write_u32l(t.buf + 0x600, 95);
  CHECK_EQ(t.com(ds, DS_GetCaps, {t.buf + 0x600}), DSERR_INVALIDPARAM);

  uint32_t b = make_buffer(t, ds, audio::pcm_format(22050, 1, 16), 1000);
  CHECK(b != 0);
  static const char* kBuf[] = {"QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition", "GetFormat",
                               "GetVolume", "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock", "Play",
                               "SetCurrentPosition", "SetFormat", "SetVolume", "SetPan", "SetFrequency", "Stop",
                               "Unlock", "Restore"};
  uint32_t bvt = t.u32(b);
  for (int i = 0; i < 21; i++) CHECK_EQ(thunk_name(t, t.u32(bvt + 4 * i)), std::string("IDirectSoundBuffer::") + kBuf[i]);
  // Releasing the device releases its buffers (voices destroyed, the store gone).
  CHECK_EQ(t.com(ds, DS_Release), 2u);
  CHECK_EQ(t.com(ds, DS_Release), 1u);
  CHECK_EQ(t.com(ds, DS_Release), 0u);
  CHECK(e.called("destroy_voice"));
  CHECK(e.called("release_buffer"));
  CHECK_EQ(t.com(b, B_Play, {0, 0, 0}), 0x88780032u);  // DSERR_INVALIDCALL: its device is gone
  CHECK_EQ(t.com(b, B_Release), 0u);
}

TEST(audio_dsound_create_buffer_validation) {
  Rt t;
  FakeEngine e;
  t.attach(e);
  uint32_t ds = create_ds(t);
  uint32_t hr = 0;
  make_buffer(t, ds, audio::pcm_format(22050, 1, 16), 1000, &hr, 0xE8, 0x10);
  CHECK_EQ(hr, DSERR_INVALIDPARAM);  // dwSize < 0x14
  make_buffer(t, ds, audio::ima_adpcm_format(22050, 1), 1000, &hr);
  CHECK_EQ(hr, DSERR_BADFORMAT);  // not PCM
  make_buffer(t, ds, audio::pcm_format(22050, 1, 16), 2, &hr);
  CHECK_EQ(hr, DSERR_INVALIDPARAM);  // < DSBSIZE_MIN
  uint32_t desc = t.buf + 0x200;
  CHECK_EQ(t.com(ds, DS_CreateSoundBuffer, {desc, t.buf + 0x280, 0x1234}), DSERR_NOAGGREGATION);
  // A primary buffer: no engine buffer; accepted Play/Stop, no Lock.
  uint32_t p = make_buffer(t, ds, audio::pcm_format(22050, 1, 16), 0, &hr, 0x1 | 0x80);
  CHECK_EQ(hr, DSERR_INVALIDPARAM);  // a primary buffer takes no size or format
  t.mem().write_u32l(desc + 8, 0);
  t.mem().write_u32l(desc + 16, 0);
  CHECK_EQ(t.com(ds, DS_CreateSoundBuffer, {desc, t.buf + 0x280, 0}), 0u);
  p = t.u32(t.buf + 0x280);
  CHECK(p != 0);
  CHECK_EQ(t.com(p, B_Play, {0, 0, 1}), 0u);
  CHECK_EQ(t.com(p, B_GetStatus, {t.buf + 0x300}), 0u);
  CHECK_EQ(t.u32(t.buf + 0x300), 5u);
  CHECK_EQ(t.com(p, B_Lock, {0, 4, t.buf + 0x310, t.buf + 0x314, 0, 0, 0}), 0x88780046u);  // DSERR_PRIOLEVELNEEDED
  CHECK(!e.called("create_buffer"));
  // A secondary: an engine buffer and a voice, flags as given (+LOCSOFTWARE).
  uint32_t b = make_buffer(t, ds, audio::pcm_format(11025, 1, 8), 500, &hr, 0xE0);
  CHECK_EQ(hr, 0u);
  t.mem().write_u32l(t.buf + 0x320, 20);
  CHECK_EQ(t.com(b, B_GetCaps, {t.buf + 0x320}), 0u);
  CHECK_EQ(t.u32(t.buf + 0x324), 0xE8u);
  CHECK_EQ(t.u32(t.buf + 0x328), 500u);
  CHECK_EQ(t.com(b, B_GetFormat, {t.buf + 0x340, 64, t.buf + 0x3A0}), 0u);
  CHECK_EQ(t.mem().read_u16l(t.buf + 0x340), 1u);
  CHECK_EQ(t.u32(t.buf + 0x344), 11025u);
  CHECK_EQ(t.u32(t.buf + 0x3A0), 18u);
  CHECK_EQ(t.com(b, B_GetFrequency, {t.buf + 0x3A4}), 0u);
  CHECK_EQ(t.u32(t.buf + 0x3A4), 11025u);
}

// Lock hands out pointers into the backing store, split at the end; Unlock
// copies exactly the given ranges into the engine at the current time.
TEST(audio_dsound_lock_unlock_wrap_and_copy) {
  Rt t;
  FakeEngine e;
  t.attach(e);
  uint32_t ds = create_ds(t);
  uint32_t b = make_buffer(t, ds, audio::pcm_format(22050, 1, 16), 1000);
  t.advance(12345);
  uint32_t p = t.buf + 0x400;
  CHECK_EQ(t.com(b, B_Lock, {900, 200, p, p + 4, p + 8, p + 12, 0}), 0u);
  uint32_t p1 = t.u32(p), n1 = t.u32(p + 4), p2 = t.u32(p + 8), n2 = t.u32(p + 12);
  CHECK_EQ(n1, 100u);
  CHECK_EQ(n2, 100u);
  CHECK_EQ(p1, p2 + 900);  // one contiguous store
  for (uint32_t i = 0; i < 100; i++) {
    t.mem().write_u8(p1 + i, uint8_t(i));
    t.mem().write_u8(p2 + i, uint8_t(200 - i));
  }
  e.calls.clear();
  e.times.clear();
  CHECK_EQ(t.com(b, B_Unlock, {p1, 100, p2, 100}), 0u);
  CHECK(e.called("write_buffer 1 900 100"));
  CHECK(e.called("write_buffer 1 0 100"));
  CHECK_EQ(e.bufs[1].bytes[905], uint8_t(5));
  CHECK_EQ(e.bufs[1].bytes[3], uint8_t(197));
  CHECK(!e.times.empty() && e.times.back() == t.clock.now_us());
  // A partial unlock (GiveTime unlocks what it copied): only that range.
  e.calls.clear();
  CHECK_EQ(t.com(b, B_Unlock, {p1, 10, 0, 0}), 0u);
  CHECK_EQ(e.calls.size(), size_t(1));
  CHECK(e.called("write_buffer 1 900 10"));
  // Out-of-range pointers and sizes.
  CHECK_EQ(t.com(b, B_Unlock, {p1, 200, 0, 0}), DSERR_INVALIDPARAM);
  CHECK_EQ(t.com(b, B_Lock, {1000, 4, p, p + 4, p + 8, p + 12, 0}), DSERR_INVALIDPARAM);
  CHECK_EQ(t.com(b, B_Lock, {0, 1001, p, p + 4, p + 8, p + 12, 0}), DSERR_INVALIDPARAM);
  // DSBLOCK_ENTIREBUFFER.
  CHECK_EQ(t.com(b, B_Lock, {0, 0, p, p + 4, p + 8, p + 12, 2}), 0u);
  CHECK_EQ(t.u32(p + 4), 1000u);
  CHECK_EQ(t.u32(p + 12), 0u);
  // The store stays current: a read-only Lock sees what was written.
  CHECK_EQ(t.com(b, B_Lock, {0, 4, p, p + 4, 0, 0, 0}), 0u);
  CHECK_EQ(t.mem().read_u8(t.u32(p) + 3), uint8_t(197));
}

// A duplicate shares the store and the engine buffer, gets its own voice,
// and copies volume, pan and frequency; volume and pan are one gain.
TEST(audio_dsound_duplicate_and_controls) {
  Rt t;
  FakeEngine e;
  t.attach(e);
  uint32_t ds = create_ds(t);
  uint32_t b = make_buffer(t, ds, audio::pcm_format(22050, 1, 16), 1000);
  CHECK_EQ(t.com(b, B_SetVolume, {uint32_t(-1000)}), 0u);
  CHECK_EQ(t.com(b, B_SetPan, {uint32_t(2500)}), 0u);
  CHECK_EQ(t.com(b, B_SetFrequency, {11025}), 0u);
  audio::Gain g = audio::gain_from_ds(-1000, 2500);
  CHECK(e.voices[2].gain == g);
  CHECK_EQ(e.voices[2].rate, 11025u);
  CHECK_EQ(t.com(b, B_SetVolume, {1}), DSERR_INVALIDPARAM);
  CHECK_EQ(t.com(b, B_SetVolume, {uint32_t(-10001)}), DSERR_INVALIDPARAM);
  CHECK_EQ(t.com(b, B_SetPan, {10001}), DSERR_INVALIDPARAM);
  CHECK_EQ(t.com(b, B_SetFrequency, {99}), DSERR_INVALIDPARAM);
  CHECK_EQ(t.com(b, B_GetVolume, {t.buf + 0x300}), 0u);
  CHECK_EQ(int32_t(t.u32(t.buf + 0x300)), -1000);
  uint32_t out = t.buf + 0x310;
  CHECK_EQ(t.com(ds, DS_Duplicate, {b, out}), 0u);
  uint32_t d = t.u32(out);
  CHECK(d != 0 && d != b);
  uint32_t p = t.buf + 0x400;
  t.com(b, B_Lock, {0, 8, p, p + 4, 0, 0, 0});
  uint32_t base_b = t.u32(p);
  t.com(d, B_Lock, {0, 8, p, p + 4, 0, 0, 0});
  CHECK_EQ(t.u32(p), base_b);  // the same store
  CHECK_EQ(e.voices.size(), size_t(2));
  auto dv = std::prev(e.voices.end());
  CHECK_EQ(dv->second.buf, e.voices.begin()->second.buf);  // the same engine buffer
  CHECK(dv->second.gain == g);
  CHECK_EQ(dv->second.rate, 11025u);
  CHECK(!dv->second.playing);
  // The shared store outlives the original; the last user releases it.
  CHECK_EQ(t.com(b, B_Release), 0u);
  CHECK(!e.called("release_buffer"));
  CHECK_EQ(t.com(d, B_Play, {0, 0, 0}), 0u);
  CHECK_EQ(t.com(d, B_Release), 0u);
  CHECK(e.called("release_buffer"));
  // Controls the buffer was not created with.
  uint32_t plain = make_buffer(t, ds, audio::pcm_format(22050, 1, 16), 100, nullptr, 0x0);
  CHECK_EQ(t.com(plain, B_SetVolume, {0}), DSERR_CONTROLUNAVAIL);
  CHECK_EQ(t.com(plain, B_SetFrequency, {0}), DSERR_CONTROLUNAVAIL);
}

// Positions and status follow virtual time (the real engine): 1 s of 22050 Hz
// 16-bit mono; the write cursor leads by 10 ms while playing.
TEST(audio_dsound_position_and_status_follow_virtual_time) {
  Rt t;
  audio::Config cfg;
  cfg.guest_sound = true;
  auto engine = audio::make_engine(cfg);
  t.attach(*engine);
  uint32_t ds = create_ds(t);
  uint32_t b = make_buffer(t, ds, audio::pcm_format(22050, 1, 16), 44100);
  CHECK(b != 0);
  if (!b) return;
  uint32_t pos = t.buf + 0x400, st = t.buf + 0x410;
  t.advance(1000);
  uint64_t t0 = t.clock.now_us();
  CHECK_EQ(t.com(b, B_Play, {0, 0, 0}), 0u);
  CHECK_EQ(t.com(b, B_GetStatus, {st}), 0u);
  CHECK_EQ(t.u32(st), 1u);  // DSBSTATUS_PLAYING
  t.advance(250000);
  uint64_t before = t.clock.now_us();
  CHECK_EQ(t.com(b, B_GetCurrentPosition, {pos, pos + 4}), 0u);
  CHECK_EQ(t.clock.now_us(), before);  // audio calls never move the clock
  CHECK_EQ(t.u32(pos), 5512u * 2);                  // floor(0.25 s * 22050) frames
  CHECK_EQ(t.u32(pos + 4), 5512u * 2 + 221u * 2);   // + 10 ms, whole frames
  t.advance(uint32_t(t0 + 1'000'000 - t.clock.now_us() - 1));
  CHECK_EQ(t.com(b, B_GetStatus, {st}), 0u);
  CHECK_EQ(t.u32(st), 1u);
  t.advance(1);
  CHECK_EQ(t.com(b, B_GetStatus, {st}), 0u);
  CHECK_EQ(t.u32(st), 0u);  // at its end
  CHECK_EQ(t.com(b, B_GetCurrentPosition, {pos, pos + 4}), 0u);
  CHECK_EQ(t.u32(pos), 0u);
  CHECK_EQ(t.u32(pos + 4), 0u);  // stopped: the write cursor is the play cursor
  // Looping wraps; SetCurrentPosition moves the cursor.
  CHECK_EQ(t.com(b, B_Play, {0, 0, 1}), 0u);
  t.advance(1'250'000);
  CHECK_EQ(t.com(b, B_GetStatus, {st}), 0u);
  CHECK_EQ(t.u32(st), 5u);  // PLAYING | LOOPING
  CHECK_EQ(t.com(b, B_GetCurrentPosition, {pos, 0}), 0u);
  CHECK_EQ(t.u32(pos), 5512u * 2);
  CHECK_EQ(t.com(b, B_Stop), 0u);
  CHECK_EQ(t.com(b, B_SetCurrentPosition, {1001}), 0u);
  CHECK_EQ(t.com(b, B_GetCurrentPosition, {pos, 0}), 0u);
  CHECK_EQ(t.u32(pos), 1000u);  // rounded down to a block
  CHECK_EQ(t.com(b, B_SetCurrentPosition, {44100}), DSERR_INVALIDPARAM);
}

// Every engine call made by the shims carries the clock's current value.
// A whole program's process ends (pe32 program.cc, Caps Lock): its
// DirectSound voices and buffers and its waveOut streams go, playing or
// not, released or not; nothing is called twice.
TEST(audio_end_process_stops_every_sound) {
  Rt t;
  FakeEngine e;
  t.attach(e);
  uint32_t ds = create_ds(t);
  uint32_t a = make_buffer(t, ds, audio::pcm_format(11025, 1, 8), 400);
  uint32_t b = make_buffer(t, ds, audio::pcm_format(22050, 1, 8), 800);
  CHECK(a && b);
  CHECK_EQ(t.com(a, B_Play, {0, 0, 1}), 0u);  // looping
  uint32_t ph = t.buf + 0x380, fmt = t.wfx(0x3C0, audio::pcm_format(11025, 1, 8));
  CHECK_EQ(t.mm("waveOutOpen", {ph, 0, fmt, 0, 0, 0}), 0u);
  CHECK_EQ(e.voices.size(), size_t(2));
  CHECK_EQ(e.streams.size(), size_t(1));
  audio_end_process(*t.rt);
  for (auto& [id, v] : e.voices) CHECK(v.destroyed);
  for (auto& [id, buf] : e.bufs) CHECK(buf.released);
  CHECK(e.streams.begin()->second.closed);
  size_t calls = e.calls.size();
  audio_end_process(*t.rt);
  CHECK_EQ(e.calls.size(), calls);
  // Sound off: nothing to do.
  Rt off;
  FakeEngine silent(false);
  off.attach(silent);
  audio_end_process(*off.rt);
  CHECK(silent.calls.empty());
}

TEST(audio_guest_time_is_the_clock_peeked) {
  Rt t;
  FakeEngine e;
  t.attach(e);
  t.clock.set_read_step_us(7);  // a time API read would move it
  uint32_t ds = create_ds(t);
  uint32_t b = make_buffer(t, ds, audio::pcm_format(22050, 1, 16), 1000);
  t.advance(4321);
  uint64_t now = t.clock.now_us();
  e.times.clear();
  uint32_t p = t.buf + 0x400;
  t.com(b, B_Play, {0, 0, 1});
  t.com(b, B_GetCurrentPosition, {p, p + 4});
  t.com(b, B_GetStatus, {p});
  t.com(b, B_SetVolume, {uint32_t(-500)});
  t.com(b, B_Stop);
  t.mm("auxSetVolume", {1, 0x12341234});
  CHECK(!e.times.empty());
  for (audio::Time x : e.times) CHECK_EQ(x, now);
  CHECK_EQ(t.clock.now_us(), now);
  // timeGetTime is a clock read (it nudges); the pump before it peeks.
  e.times.clear();
  uint32_t tick = t.mm("timeGetTime", {});
  CHECK_EQ(tick, uint32_t(VirtualClock::kBootOffsetMs + (now + 7) / 1000));
  CHECK(!e.times.empty() && e.times.front() == now);
}

// ---- ACM -------------------------------------------------------------------------------------------------

// acmFormatEnumA calls the guest callback for the standard IMA formats in
// index order, stops when it returns FALSE, and leaves that format in pwfx;
// then the engine's whole DecompressWave sequence.
TEST(audio_acm_enum_callback_and_convert) {
  Rt t;
  FakeEngine e;
  t.attach(e);
  uint32_t metric = t.buf + 0x100;
  CHECK_EQ(t.acm("acmMetrics", {0, 50, metric}), 0u);
  CHECK_EQ(t.u32(metric), 50u);
  CHECK_EQ(t.acm("acmMetrics", {0, 2, metric}), 0u);
  CHECK_EQ(t.u32(metric), 2u);
  CHECK_EQ(t.acm("acmMetrics", {0, 99, metric}), 8u);  // MMSYSERR_NOTSUPPORTED
  auto r = t.load(R"(
  cb:
    mov eax, [esp + 8]
    mov ecx, [DATA]
    mov edx, [eax + 4]
    mov [ecx], edx
    mov edx, [eax + 16]
    mov edx, [edx + 4]
    mov [ecx + 4], edx
    add ecx, 8
    mov [DATA], ecx
    mov eax, [esp + 12]
    cmp edx, eax
    je stop
    mov eax, 1
    ret 16
  stop:
    xor eax, eax
    ret 16
  )");
  uint32_t cb = t.label(r, "cb");
  t.mem().write_u32l(t.data, t.data + 16);
  uint32_t afd = t.buf + 0x200, pwfx = t.buf + 0x300;
  t.mem().memset(afd, 0, 0x98);
  t.mem().write_u32l(afd + 0, 0x98);
  t.mem().write_u32l(afd + 8, 0x11);
  t.mem().write_u32l(afd + 0x10, pwfx);
  t.mem().write_u32l(afd + 0x14, 50);
  t.mem().write_u16l(pwfx, 0x11);
  CHECK_EQ(t.acm("acmFormatEnumA", {0, afd, cb, 22050, 0x10000}), 0u);
  uint32_t calls = (t.u32(t.data) - (t.data + 16)) / 8;
  CHECK_EQ(calls, 5u);  // 8000 m/s, 11025 m/s, 22050 mono: stop
  static const uint32_t rates[] = {8000, 8000, 11025, 11025, 22050};
  for (uint32_t i = 0; i < calls && i < 5; i++) {
    CHECK_EQ(t.u32(t.data + 16 + 8 * i), i);
    CHECK_EQ(t.u32(t.data + 20 + 8 * i), rates[i]);
  }
  audio::WaveFormat found;
  CHECK(audio::parse_waveformat(std::span<const uint8_t>(t.mem().at<uint8_t>(pwfx, 20), 20), found));
  CHECK(found == audio::ima_adpcm_format(22050, 1));
  CHECK_EQ(read_cstr(t.mem(), afd + 0x18), std::string("22.050 kHz, 4 Bit, Mono"));
  // The callback returning TRUE throughout walks all 8.
  t.mem().write_u32l(t.data, t.data + 16);
  CHECK_EQ(t.acm("acmFormatEnumA", {0, afd, cb, 1, 0x10000}), 0u);
  CHECK_EQ((t.u32(t.data) - (t.data + 16)) / 8, 8u);
  CHECK_EQ(t.acm("acmFormatEnumA", {0, 0, cb, 1, 0x10000}), 11u);  // MMSYSERR_INVALPARAM

  // Suggest → PCM16; open (QUERY honoured); size; prepare; convert; close.
  uint32_t src = t.wfx(0x400, audio::ima_adpcm_format(22050, 1)), dst = t.buf + 0x440;
  t.mem().memset(dst, 0, 18);
  t.mem().write_u16l(dst, 1);
  CHECK_EQ(t.acm("acmFormatSuggest", {0, src, dst, 0x12, 0x10000}), 0u);
  audio::WaveFormat pcm;
  CHECK(audio::parse_waveformat(std::span<const uint8_t>(t.mem().at<uint8_t>(dst, 18), 18), pcm));
  CHECK(pcm == audio::pcm_format(22050, 1, 16));
  CHECK_EQ(t.acm("acmStreamOpen", {0, 0, src, dst, 0, 0, 0, 1}), 0u);  // QUERY
  CHECK_EQ(t.acm("acmStreamOpen", {t.buf + 0x480, 0, dst, src, 0, 0, 0, 4}), 512u);  // PCM → ADPCM: no
  CHECK_EQ(t.acm("acmStreamOpen", {t.buf + 0x480, 0, src, dst, 0, 0, 0, 4}), 0u);
  uint32_t has = t.u32(t.buf + 0x480);
  CHECK(has != 0);
  // Three whole blocks and a partial one (a valid header and some nibbles).
  std::vector<uint8_t> ima(3 * 512 + 100);
  uint32_t seed = 12345;
  for (uint8_t& x : ima) x = uint8_t((seed = seed * 1103515245u + 12345u) >> 24);
  for (size_t blk = 0; blk < ima.size(); blk += 512) ima[blk + 2] = uint8_t(ima[blk + 2] % 89), ima[blk + 3] = 0;
  uint32_t in = t.rt->heap().alloc(uint32_t(ima.size()));
  t.mem().memcpy(in, ima.data(), ima.size());
  uint32_t outsize = t.buf + 0x490;
  CHECK_EQ(t.acm("acmStreamSize", {has, uint32_t(ima.size()), outsize, 0}), 0u);
  auto want = audio::decode(audio::ima_adpcm_format(22050, 1), ima);
  // The size rounds up to whole blocks (imaadp32); the decode of a trailing
  // partial IMA block holds nothing, so the tail of the buffer stays zero.
  CHECK_EQ(size_t(t.u32(outsize)), audio::decoded_size(audio::ima_adpcm_format(22050, 1), ima.size()));
  CHECK(size_t(t.u32(outsize)) >= want.size());
  uint32_t n = t.u32(outsize);
  uint32_t hdr = t.rt->heap().alloc(n + 0x54, true);
  t.mem().write_u32l(hdr + 0, 0x54);
  t.mem().write_u32l(hdr + 0x0C, in);
  t.mem().write_u32l(hdr + 0x10, uint32_t(ima.size()));
  t.mem().write_u32l(hdr + 0x1C, hdr + 0x54);
  t.mem().write_u32l(hdr + 0x20, n);
  CHECK_EQ(t.acm("acmStreamConvert", {has, hdr, 0x10}), 514u);  // ACMERR_UNPREPARED
  CHECK_EQ(t.acm("acmStreamPrepareHeader", {has, hdr, 0}), 0u);
  CHECK_EQ(t.acm("acmStreamConvert", {has, hdr, 0x10}), 0u);
  CHECK_EQ(t.u32(hdr + 0x14), 3u * 512);  // cbSrcLengthUsed: the whole blocks the decoder consumed
  CHECK_EQ(t.u32(hdr + 0x24), uint32_t(want.size()));
  CHECK(t.u32(hdr + 4) & 0x10000);  // DONE
  CHECK(t.mem().memcmp(hdr + 0x54, want.data(), want.size()) == 0);
  // BLOCKALIGN: whole blocks only.
  CHECK_EQ(t.acm("acmStreamConvert", {has, hdr, 0x4}), 0u);
  CHECK_EQ(t.u32(hdr + 0x14), 3u * 512);
  CHECK_EQ(t.acm("acmStreamSize", {has, n, outsize, 1}), 0u);  // DESTINATION
  CHECK_EQ(size_t(t.u32(outsize)), audio::encoded_size_for(audio::ima_adpcm_format(22050, 1), n));
  CHECK_EQ(t.acm("acmStreamUnprepareHeader", {has, hdr, 0}), 0u);
  CHECK_EQ(t.acm("acmStreamUnprepareHeader", {has, hdr, 0}), 514u);
  CHECK_EQ(t.acm("acmStreamClose", {has, 0}), 0u);
  CHECK_EQ(t.acm("acmStreamClose", {has, 0}), 5u);

  // MS-ADPCM (the Totally Twisted format): a trailing partial block that
  // holds its header is decoded and consumed.
  audio::WaveFormat ms = audio::ms_adpcm_format(11025, 1);
  src = t.wfx(0x400, ms);
  CHECK_EQ(t.acm("acmStreamOpen", {t.buf + 0x480, 0, src, dst, 0, 0, 0, 4}), 512u);  // dst is 22050 Hz
  t.mem().write_u32l(dst + 4, 11025);
  t.mem().write_u32l(dst + 8, 22050);
  CHECK_EQ(t.acm("acmStreamOpen", {t.buf + 0x480, 0, src, dst, 0, 0, 0, 4}), 0u);
  has = t.u32(t.buf + 0x480);
  std::vector<uint8_t> msd(2 * ms.block_align + 40);
  for (uint8_t& x : msd) x = uint8_t((seed = seed * 1103515245u + 12345u) >> 24);
  for (size_t blk = 0; blk < msd.size(); blk += ms.block_align) msd[blk] = uint8_t(msd[blk] % 7);
  auto want_ms = audio::decode(ms, msd);
  CHECK(want_ms.size() > 2u * ms.samples_per_block * 2);  // the partial block holds samples
  in = t.rt->heap().alloc(uint32_t(msd.size()));
  t.mem().memcpy(in, msd.data(), msd.size());
  CHECK_EQ(t.acm("acmStreamSize", {has, uint32_t(msd.size()), outsize, 0}), 0u);
  n = t.u32(outsize);
  hdr = t.rt->heap().alloc(n + 0x54, true);
  t.mem().write_u32l(hdr + 0, 0x54);
  t.mem().write_u32l(hdr + 0x0C, in);
  t.mem().write_u32l(hdr + 0x10, uint32_t(msd.size()));
  t.mem().write_u32l(hdr + 0x1C, hdr + 0x54);
  t.mem().write_u32l(hdr + 0x20, n);
  CHECK_EQ(t.acm("acmStreamPrepareHeader", {has, hdr, 0}), 0u);
  CHECK_EQ(t.acm("acmStreamConvert", {has, hdr, 0x10 | 0x20}), 0u);
  CHECK_EQ(t.u32(hdr + 0x14), uint32_t(msd.size()));
  CHECK_EQ(t.u32(hdr + 0x24), uint32_t(want_ms.size()));
  CHECK(t.mem().memcmp(hdr + 0x54, want_ms.data(), want_ms.size()) == 0);
  // Less than one block: nothing to size.
  CHECK_EQ(t.acm("acmStreamSize", {has, 6, outsize, 0}), 512u);
  CHECK_EQ(t.acm("acmStreamClose", {has, 0}), 0u);
}

// ---- waveOut (the real engine's timing) -------------------------------------------------------------------

TEST(audio_waveout_done_exactly_at_the_virtual_end) {
  Rt t;
  audio::Config cfg;
  cfg.guest_sound = true;
  auto engine = audio::make_engine(cfg);
  t.attach(*engine);
  CHECK_EQ(t.mm("waveOutGetNumDevs", {}), 1u);
  uint32_t caps = t.buf + 0x100;
  CHECK_EQ(t.mm("waveOutGetDevCapsA", {0xFFFFFFFF, caps, 52}), 0u);
  CHECK_EQ(read_cstr(t.mem(), caps + 8), std::string("Long After Dark"));
  CHECK_EQ(t.u32(caps + 40), 0xFFFu);
  CHECK_EQ(t.u32(caps + 48) & 0x10, 0u);  // no WAVECAPS_SYNC
  CHECK_EQ(t.mm("waveOutGetDevCapsA", {1, caps, 52}), 2u);
  uint32_t fmt = t.wfx(0x200, audio::pcm_format(22050, 1, 16));
  CHECK_EQ(t.mm("waveOutOpen", {0, 0xFFFFFFFF, fmt, 0, 0, 1}), 0u);  // QUERY
  CHECK_EQ(t.mm("waveOutOpen", {0, 0, t.wfx(0x240, audio::ima_adpcm_format(22050, 1)), 0, 0, 1}), 32u);
  uint32_t ph = t.buf + 0x280;
  CHECK_EQ(t.mm("waveOutOpen", {ph, 0xFFFFFFFF, fmt, 0, 0, 0}), 0u);
  uint32_t h = t.u32(ph);
  CHECK(h != 0);
  // 100 ms of audio.
  uint32_t data = t.rt->heap().alloc(4410, true);
  uint32_t hdr = t.buf + 0x300;
  t.mem().memset(hdr, 0, 32);
  t.mem().write_u32l(hdr + 0, data);
  t.mem().write_u32l(hdr + 4, 4410);
  CHECK_EQ(t.mm("waveOutWrite", {h, hdr, 32}), 34u);  // WAVERR_UNPREPARED
  CHECK_EQ(t.mm("waveOutPrepareHeader", {h, hdr, 32}), 0u);
  t.advance(5000);
  uint64_t t0 = t.clock.now_us();
  CHECK_EQ(t.mm("waveOutWrite", {h, hdr, 32}), 0u);
  CHECK_EQ(t.u32(hdr + 16), 0x12u);  // PREPARED | INQUEUE
  CHECK_EQ(t.mm("waveOutUnprepareHeader", {h, hdr, 32}), 33u);  // WAVERR_STILLPLAYING
  CHECK_EQ(t.mm("waveOutClose", {h}), 33u);
  t.advance(uint32_t(t0 + 100000 - 1 - t.clock.now_us()));
  t.mm("timeGetTime", {});
  CHECK_EQ(t.u32(hdr + 16) & 1, 0u);
  t.advance(1);
  t.mm("timeGetTime", {});
  CHECK_EQ(t.u32(hdr + 16), 0x3u);  // DONE | PREPARED
  uint32_t mmt = t.buf + 0x340;
  t.mem().write_u32l(mmt, 4);  // TIME_BYTES
  CHECK_EQ(t.mm("waveOutGetPosition", {h, mmt, 12}), 0u);
  CHECK_EQ(t.u32(mmt + 4), 4410u);
  // Pause holds; reset returns everything queued, done, at once.
  CHECK_EQ(t.mm("waveOutWrite", {h, hdr, 32}), 0u);
  CHECK_EQ(t.mm("waveOutPause", {h}), 0u);
  t.advance(200000);
  t.mm("timeGetTime", {});
  CHECK_EQ(t.u32(hdr + 16) & 1, 0u);
  CHECK_EQ(t.mm("waveOutReset", {h}), 0u);
  CHECK_EQ(t.u32(hdr + 16), 0x3u);
  CHECK_EQ(t.mm("waveOutRestart", {h}), 0u);
  // Volume: the wave bus.
  CHECK_EQ(t.mm("waveOutSetVolume", {h, 0x40004000}), 0u);
  CHECK_EQ(t.mm("waveOutGetVolume", {0, t.buf + 0x380}), 0u);
  CHECK_EQ(t.u32(t.buf + 0x380), 0x40004000u);
  CHECK(engine->bus_gain(audio::Bus::wave) == audio::gain_from_mm(0x40004000));
  CHECK_EQ(t.mm("waveOutUnprepareHeader", {h, hdr, 32}), 0u);
  CHECK_EQ(t.mm("waveOutClose", {h}), 0u);
  CHECK_EQ(t.mm("waveOutClose", {h}), 5u);
}

// CALLBACK_WINDOW posts MM_WOM_OPEN/DONE/CLOSE; CALLBACK_FUNCTION runs at
// the next WINMM call (or Module() boundary), in order, not inside waveOutOpen.
TEST(audio_waveout_callbacks) {
  Rt t;
  FakeEngine e;
  t.attach(e);
  uint32_t hwnd = host_window(*t.rt);
  uint32_t fmt = t.wfx(0x200, audio::pcm_format(11025, 1, 8));
  uint32_t ph = t.buf + 0x280, hdr = t.buf + 0x300;
  CHECK_EQ(t.mm("waveOutOpen", {ph, 0, fmt, hwnd, 0, 0x10000}), 0u);
  uint32_t h = t.u32(ph);
  CHECK(has_message(t, hwnd, 0x3BB, h, 0));  // MM_WOM_OPEN
  t.mem().memset(hdr, 0, 32);
  t.mem().write_u32l(hdr + 0, t.buf + 0x1000);
  t.mem().write_u32l(hdr + 4, 64);
  t.mm("waveOutPrepareHeader", {h, hdr, 32});
  t.mm("waveOutWrite", {h, hdr, 32});
  e.pending.push_back({audio::Event::Kind::chunk_done, e.streams.begin()->first, hdr, t.clock.now_us()});
  t.mm("timeGetTime", {});
  CHECK_EQ(t.u32(hdr + 16), 0x3u);
  CHECK(has_message(t, hwnd, 0x3BD, h, hdr));  // MM_WOM_DONE
  CHECK_EQ(t.mm("waveOutClose", {h}), 0u);
  CHECK(has_message(t, hwnd, 0x3BC, h, 0));  // MM_WOM_CLOSE

  auto r = t.load(R"(
  proc:
    mov ecx, [DATA + 4]
    mov edx, [esp + 8]
    mov [ecx], edx
    mov edx, [esp + 16]
    mov [ecx + 4], edx
    add ecx, 8
    mov [DATA + 4], ecx
    ret 20
  )");
  t.mem().write_u32l(t.data + 4, t.data + 32);
  auto got = [&]() { return (t.u32(t.data + 4) - (t.data + 32)) / 8; };
  CHECK_EQ(t.mm("waveOutOpen", {ph, 0, fmt, t.label(r, "proc"), 77, 0x30000}), 0u);
  h = t.u32(ph);
  CHECK_EQ(got(), 0u);  // not inside waveOutOpen
  t.mm("waveOutPrepareHeader", {h, hdr, 32});
  CHECK_EQ(got(), 1u);  // at the next WINMM call
  CHECK_EQ(t.u32(t.data + 32), 0x3BBu);
  t.mm("waveOutWrite", {h, hdr, 32});
  e.pending.push_back({audio::Event::Kind::chunk_done, std::prev(e.streams.end())->first, hdr, t.clock.now_us()});
  win32::audio_pump(*t.rt);  // the lane's pump before Module()
  CHECK_EQ(got(), 2u);
  CHECK_EQ(t.u32(t.data + 40), 0x3BDu);
  CHECK_EQ(t.u32(t.data + 44), hdr);
  t.mm("waveOutClose", {h});
  t.mm("timeGetTime", {});
  CHECK_EQ(got(), 3u);
  CHECK_EQ(t.u32(t.data + 48), 0x3BCu);
}

// ---- MCI -------------------------------------------------------------------------------------------------

TEST(audio_mci_sequencer_commands) {
  Rt t;
  audio::Config cfg;
  cfg.guest_sound = true;
  auto engine = audio::make_engine(cfg);
  auto smf = tiny_smf();
  t.rt->vfs().add_virtual_file("C:\\AFTERDRK\\SONG.MID", smf);
  t.rt->vfs().add_virtual_file("C:\\AFTERDRK\\JUNK.MID", std::vector<uint8_t>(40, 'x'));
  t.attach(*engine);
  uint32_t hwnd = host_window(*t.rt);
  uint32_t op = t.buf + 0x100;  // MCI_OPEN_PARMSA
  auto open = [&](uint32_t flags, const char* type, const char* element, const char* alias = nullptr) {
    t.mem().memset(op, 0, 20);
    t.mem().write_u32l(op + 0, hwnd);
    if (type) t.mem().write_u32l(op + 8, t.str(0x200, type));
    if (element) t.mem().write_u32l(op + 12, t.str(0x240, element));
    if (alias) t.mem().write_u32l(op + 16, t.str(0x2C0, alias));
    return t.mm("mciSendCommandA", {0, 0x803, flags, op});
  };
  // The engine's test open: the device alone, then close.
  CHECK_EQ(open(0x2000, "Sequencer", nullptr), 0u);
  uint32_t probe = t.u32(op + 4);
  CHECK_EQ(probe, 1u);
  CHECK_EQ(t.mm("auxGetVolume", {1, t.buf + 0x180}), 0u);
  CHECK_EQ(t.mm("mciSendCommandA", {probe, 0x804, 0, 0}), 0u);
  // Errors.
  CHECK_EQ(open(0x2000, "cdaudio", nullptr), 306u);
  CHECK_EQ(open(0x2000 | 0x1000, nullptr, nullptr), 306u);  // type id 0: not the sequencer
  CHECK_EQ(open(0x2200, "sequencer", "MISSING.MID"), 275u);
  CHECK_EQ(open(0x2200, "sequencer", "JUNK.MID"), 296u);
  CHECK_EQ(open(0x200, nullptr, "SONG.XYZ"), 281u);
  CHECK_EQ(t.mm("mciSendCommandA", {99, 0x806, 0, t.buf + 0x300}), 257u);
  CHECK_EQ(t.mm("mciGetErrorStringA", {275, t.buf + 0x400, 256}), 1u);
  CHECK_EQ(read_cstr(t.mem(), t.buf + 0x400),
           std::string("Cannot find the specified file.  Make sure the path and filename are correct."));
  CHECK_EQ(t.mm("mciSendStringA", {t.str(0x500, "open cdaudio alias qwanza wait"), 0, 0, 0}), 306u);
  // Load: type + element (relative to the current directory) + alias; by type id too.
  CHECK_EQ(open(0x2200 | 0x400, "sequencer", "SONG.MID", "fred"), 0u);
  uint32_t id = t.u32(op + 4);
  CHECK(id > probe);
  CHECK_EQ(open(0x2200 | 0x400, "sequencer", "SONG.MID", "FRED"), 289u);  // MCIERR_DUPLICATE_ALIAS
  t.mem().memset(op, 0, 20);
  t.mem().write_u32l(op + 8, 0x20B);
  t.mem().write_u32l(op + 12, t.str(0x240, "C:\\AFTERDRK\\SONG.MID"));
  CHECK_EQ(t.mm("mciSendCommandA", {0, 0x803, 0x1000 | 0x2000 | 0x200, op}), 0u);
  uint32_t id2 = t.u32(op + 4);
  CHECK_EQ(t.mm("mciSendCommandA", {id2, 0x804, 0, 0}), 0u);
  // Set: port MIDI_MAPPER / 0 / other; time format ms / song pointer.
  uint32_t sp = t.buf + 0x300;
  t.mem().memset(sp, 0, 32);
  t.mem().write_u32l(sp + 16, 0xFFFFFFFF);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x80D, 0x20000, sp}), 0u);
  t.mem().write_u32l(sp + 16, 0);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x80D, 0x20000, sp}), 0u);
  t.mem().write_u32l(sp + 16, 5);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x80D, 0x20000, sp}), 338u);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x80D, 0x400, sp}), 0u);  // dwTimeFormat 0 = ms
  t.mem().write_u32l(sp + 4, 0x4003);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x80D, 0x400, sp}), 293u);
  // Status.
  uint32_t stp = t.buf + 0x340;
  auto status = [&](uint32_t item, uint32_t* v) {
    t.mem().memset(stp, 0, 16);
    t.mem().write_u32l(stp + 8, item);
    uint32_t err = t.mm("mciSendCommandA", {id, 0x814, 0x100, stp});
    *v = t.u32(stp + 4);
    return err;
  };
  uint32_t v = 0;
  CHECK_EQ(status(4, &v), 0u);
  CHECK_EQ(v, 0x20Du);  // stopped
  CHECK_EQ(status(1, &v), 0u);
  CHECK_EQ(v, 500u);  // length, ms
  CHECK_EQ(status(7, &v), 0u);
  CHECK_EQ(v, 1u);    // ready
  CHECK_EQ(status(3, &v), 0u);
  CHECK_EQ(v, 1u);    // tracks
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x814, 0, stp}), 273u);  // no MCI_STATUS_ITEM
  // Play with notify: the end is notified (SUCCESSFUL) at the first pump after it.
  uint32_t pp = t.buf + 0x380;
  t.mem().memset(pp, 0, 12);
  t.mem().write_u32l(pp, hwnd);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x806, 0x2, pp}), 274u);  // MCI_WAIT: would block
  t.advance(1000);
  uint64_t t0 = t.clock.now_us();
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x806, 0x1, pp}), 0u);
  CHECK_EQ(status(4, &v), 0u);
  CHECK_EQ(v, 0x20Eu);  // playing
  t.advance(200000);
  CHECK_EQ(status(2, &v), 0u);
  CHECK_EQ(v, 200u);    // position, ms
  t.advance(uint32_t(t0 + 500000 - t.clock.now_us()));
  t.mm("timeGetTime", {});
  CHECK(has_message(t, hwnd, 0x3B9, 1, id));  // MM_MCINOTIFY(SUCCESSFUL, id)
  CHECK_EQ(status(4, &v), 0u);
  CHECK_EQ(v, 0x20Du);
  // The engine's loop: stop, seek to start, play.
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x808, 0, 0}), 0u);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x807, 0x100, t.buf + 0x3A0}), 0u);
  CHECK_EQ(status(2, &v), 0u);
  CHECK_EQ(v, 0u);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x807, 0x300, t.buf + 0x3A0}), 284u);  // start and end
  // A pending play notify: STATUS|NOTIFY supersedes it, STOP aborts it.
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x806, 0x1, pp}), 0u);
  t.mem().write_u32l(stp, hwnd);
  t.mem().write_u32l(stp + 8, 4);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x814, 0x101, stp}), 0u);
  CHECK(has_message(t, hwnd, 0x3B9, 2, id));  // SUPERSEDED
  CHECK(has_message(t, hwnd, 0x3B9, 1, id));  // the status's own
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x806, 0x1, pp}), 0u);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x808, 0, 0}), 0u);
  CHECK(has_message(t, hwnd, 0x3B9, 4, id));  // ABORTED
  // From/to (ms): position 100..300, stopped at the first pump at or after
  // 300 (here 50 ms past it), and left at 300, as MCI reports it.
  t.mem().write_u32l(pp + 4, 100);
  t.mem().write_u32l(pp + 8, 300);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x806, 0x1 | 0x4 | 0x8, pp}), 0u);
  t.advance(250000);
  t.mm("timeGetTime", {});
  CHECK(has_message(t, hwnd, 0x3B9, 1, id));
  CHECK_EQ(status(4, &v), 0u);
  CHECK_EQ(v, 0x20Du);
  CHECK_EQ(status(2, &v), 0u);
  CHECK_EQ(v, 300u);
  t.mem().write_u32l(pp + 8, 600);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x806, 0x8, pp}), 282u);  // past the end
  // Close (all).
  CHECK_EQ(t.mm("mciSendCommandA", {0xFFFF, 0x806, 0, pp}), 279u);
  CHECK_EQ(t.mm("mciSendCommandA", {0xFFFF, 0x804, 0, 0}), 0u);
  CHECK_EQ(t.mm("mciSendCommandA", {id, 0x804, 0, 0}), 257u);
}

// ---- aux, mixer, waveIn ------------------------------------------------------------------------------------

TEST(audio_aux_mixer_wavein) {
  Rt t;
  FakeEngine e;
  t.attach(e);
  CHECK_EQ(t.mm("auxGetNumDevs", {}), 2u);
  uint32_t v = t.buf + 0x100;
  CHECK_EQ(t.mm("auxGetVolume", {1, v}), 0u);
  CHECK_EQ(t.u32(v), 0xFFFFFFFFu);
  CHECK_EQ(t.mm("auxSetVolume", {1, 0x7FFF7FFF}), 0u);
  CHECK(e.buses[1] == audio::gain_from_mm(0x7FFF7FFF));
  e.calls.clear();
  CHECK_EQ(t.mm("auxSetVolume", {1, 0x7FFF7FFF}), 0u);  // every GiveTime: no change, no engine call
  CHECK(!e.called("set_bus_gain"));
  CHECK_EQ(t.mm("auxGetVolume", {1, v}), 0u);
  CHECK_EQ(t.u32(v), 0x7FFF7FFFu);
  CHECK_EQ(t.mm("auxSetVolume", {0, 0x1234}), 0u);  // CD: stored only
  CHECK(!e.called("set_bus_gain"));
  CHECK_EQ(t.mm("auxGetVolume", {0, v}), 0u);
  CHECK_EQ(t.u32(v), 0x1234u);
  CHECK_EQ(t.mm("auxGetVolume", {2, v}), 2u);
  CHECK_EQ(t.mm("mixerGetLineInfoA", {0, t.buf + 0x200, 0x50000000}), 6u);  // keeps per-buffer volume
  CHECK_EQ(t.mm("waveInGetNumDevs", {}), 0u);
  CHECK_EQ(t.mm("waveInOpen", {t.buf, 0, 0, 0, 0, 0}), 6u);
}

// ---- the Deluxe music renames (§7.7) -----------------------------------------------------------------------

TEST(audio_deluxe_music_renames) {
  CHECK_EQ(deluxe_music_file("Flying Toasters.mid"), std::string("TOASTERS.MID"));
  CHECK_EQ(deluxe_music_file("flying toasters.MID"), std::string("TOASTERS.MID"));
  CHECK_EQ(deluxe_music_file("Baby Toasters.mid"), std::string("BABY.MID"));
  CHECK_EQ(deluxe_music_file("3dminor.mid"), std::string("3DMINOR.MID"));
  CHECK_EQ(deluxe_music_file("FIREBOMB.MID"), std::string("FIREBOMB.MID"));
  CHECK_EQ(deluxe_music_file("SEAPIXIE.mid"), std::string("SEAPIXIE.MID"));
  CHECK_EQ(deluxe_music_file("nutcrack.mid"), std::string());
  CHECK_EQ(deluxe_music_path("C:\\AFTERDRK\\Music\\Flying Toasters.mid"), std::string("C:\\AFTERDRK\\TOASTERS.MID"));
  CHECK_EQ(deluxe_music_path("C:\\AFTERDRK\\MUSIC\\baby toasters.mid"), std::string("C:\\AFTERDRK\\BABY.MID"));
  CHECK_EQ(deluxe_music_path("C:\\AFTERDRK\\Flying Toasters.mid"), std::string());
  auto smf = tiny_smf();
  for (int mode = 0; mode < 3; mode++) {  // deluxe on; not deluxe; sound off
    Rt t;
    t.rt->vfs().add_virtual_file("C:\\AFTERDRK\\TOASTERS.MID", smf);
    FakeEngine e(mode != 2);
    t.attach(e, mode != 1);
    const char* asked = "C:\\AFTERDRK\\Music\\Flying Toasters.mid";
    std::string bytes;
    bool found = t.rt->vfs().read_file(asked, &bytes);
    CHECK_EQ(found, mode == 0);
    if (mode == 0) {
      CHECK(bytes == std::string(smf.begin(), smf.end()));
      // The engine's _access check and its MCI_OPEN both see it.
      CHECK(t.call("KERNEL32.DLL", "GetFileAttributesA", {t.str(0, asked)}) != INVALID_FILE_ATTRIBUTES);
      uint32_t op = t.buf + 0x100;
      t.mem().memset(op, 0, 20);
      t.mem().write_u32l(op + 8, t.str(0x200, "sequencer"));
      t.mem().write_u32l(op + 12, t.str(0x240, "Music\\Flying Toasters.mid"));
      CHECK_EQ(t.mm("mciSendCommandA", {0, 0x803, 0x2200, op}), 0u);
      CHECK(e.called("load_song " + std::to_string(smf.size())));
      // A name the table does not know stays missing.
      CHECK(!t.rt->vfs().exists("C:\\AFTERDRK\\Music\\nutcrack.mid"));
    }
  }
}
