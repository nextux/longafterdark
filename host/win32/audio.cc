#include "win32/audio.hh"

#include <algorithm>
#include <cctype>
#include <string>

#include "adw/core/log.h"
#include "win32/runtime.hh"
#include "win32/vfs.hh"

namespace adw::win32 {

namespace {

struct AudioState : RuntimeState {
  audio::Engine* engine = nullptr;
  bool attached = false;
  AudioOptions opts;
};

AudioState& st(Runtime& rt) { return rt.state<AudioState>(); }

bool iequal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (tolower(uint8_t(a[i])) != tolower(uint8_t(b[i]))) return false;
  return true;
}

// AUDIO.md §7.7: what the Deluxe AD4 modules ask for (their string tables)
// and the file the Deluxe disc keeps in FILES\AD40. EMPIRICAL: from the 10th
// Anniversary's SETUP.INF renames and equal file sizes.
struct Rename {
  const char* asked;
  const char* file;
};
constexpr Rename kDeluxeMusic[] = {
    {"Flying Toasters.mid", "TOASTERS.MID"},
    {"Baby Toasters.mid", "BABY.MID"},
    {"3DMinor.MID", "3DMINOR.MID"},
    {"FIREBOMB.MID", "FIREBOMB.MID"},
    {"SEAPIXIE.mid", "SEAPIXIE.MID"},
};

}  // namespace

std::string deluxe_music_file(std::string_view name) {
  for (const Rename& r : kDeluxeMusic)
    if (iequal(name, r.asked)) return r.file;
  return {};
}

std::string deluxe_music_path(std::string_view guest_path) {
  size_t s = guest_path.find_last_of("\\/");
  if (s == std::string_view::npos) return {};
  std::string file = deluxe_music_file(guest_path.substr(s + 1));
  if (file.empty()) return {};
  std::string_view dir = guest_path.substr(0, s);
  size_t d = dir.find_last_of("\\/");
  std::string_view last = d == std::string_view::npos ? dir : dir.substr(d + 1);
  if (!iequal(last, "Music")) return {};
  std::string parent = d == std::string_view::npos ? std::string() : std::string(dir.substr(0, d));
  return parent.empty() ? file : parent + "\\" + file;
}

void attach_audio(Runtime& rt, audio::Engine& engine, const AudioOptions& opts) {
  AudioState& s = st(rt);
  s.engine = &engine;
  s.attached = true;
  s.opts = opts;
  if (!engine.enabled()) return;
  register_dsound(rt);
  if (!opts.deluxe) return;
  // The Deluxe renames as virtual files beside the module (C:\AFTERDRK is the
  // module's folder, the directory MakePathAbsoluteToAD resolves against):
  // only the ones missing there, each with its 8.3 original's bytes.
  Vfs& vfs = rt.vfs();
  const std::string dir = rt.options().guest_module_dir;
  int added = 0;
  for (const Rename& r : kDeluxeMusic) {
    std::string asked = dir + "\\Music\\" + r.asked;
    if (vfs.exists(asked)) continue;
    std::string bytes;
    if (!vfs.read_file(dir + "\\" + r.file, &bytes)) continue;
    vfs.add_virtual_file(asked, std::vector<uint8_t>(bytes.begin(), bytes.end()));
    trace("sound", "deluxe music: %s -> %s\\%s", asked.c_str(), dir.c_str(), r.file);
    added++;
  }
  if (added) trace("sound", "deluxe music: %d rename(s) added", added);
}

void audio_end_process(Runtime& rt) {
  if (!audio_enabled(rt)) return;
  dsound_end_process(rt);
  mmio_end_process(rt);
  winmm_end_process(rt);
}

audio::Engine& audio_engine(Runtime& rt) {
  AudioState& s = st(rt);
  return s.engine ? *s.engine : audio::null_engine();
}

const AudioOptions& audio_options(Runtime& rt) { return st(rt).opts; }

bool audio_enabled(Runtime& rt) {
  AudioState& s = st(rt);
  return s.engine && s.engine->enabled();
}

audio::Time audio_now(Runtime& rt) { return rt.clock().now_us(); }

bool read_guest_waveformat(Runtime& rt, uint32_t addr, audio::WaveFormat& out, std::vector<uint8_t>* raw) {
  if (!addr) return false;
  auto& mem = rt.mem();
  std::vector<uint8_t> b(16);
  mem.memcpy(b.data(), addr, 16);
  uint16_t tag = uint16_t(b[0] | (b[1] << 8));
  if (tag != audio::kTagPcm) {
    uint16_t cb = mem.read_u16l(addr + 16);
    size_t n = 18 + std::min<size_t>(cb, 256);
    b.resize(n);
    mem.memcpy(b.data() + 16, addr + 16, uint32_t(n - 16));
  }
  bool ok = audio::parse_waveformat(b, out);
  if (raw) *raw = std::move(b);
  return ok;
}

uint32_t write_guest_waveformat(Runtime& rt, uint32_t addr, const audio::WaveFormat& f, uint32_t cap) {
  std::vector<uint8_t> b = audio::waveformat_bytes(f);
  uint32_t n = std::min<uint32_t>(uint32_t(b.size()), cap);
  if (n) rt.mem().memcpy(addr, b.data(), n);
  return n;
}

}  // namespace adw::win32
