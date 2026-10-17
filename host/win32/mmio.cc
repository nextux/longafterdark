// WINMM.DLL's multimedia file I/O and sndPlaySoundA, for a whole program
// (docs/ABI.md §2.15; API_SURFACE.md §1 "WINMM.DLL"; registered with the
// program shims, never for a module). Screen Wreckers' DTTY3203.EXE loads
// its .WAV files with them, the DirectSound sample's way: mmioOpenA, RIFF
// descent, mmioRead of the format, mmioGetInfo / mmioAdvance / mmioSetInfo
// for the data.
//
// mmioOpenA opens a file through the Vfs, read-only (MMIO_READ; a write or
// create open fails with MMIOERR_CANNOTOPEN, 257 — the toys never write),
// and keeps the file's bytes. Its I/O buffer (MMIO_ALLOCBUF, or always for
// mmioGetInfo) is one guest block holding the whole file: mmioGetInfo hands
// out pchBuffer = the block, pchNext = the current position, pchEndRead =
// the file's end, lBufOffset 0, lDiskOffset = the size; mmioAdvance (read)
// takes pchNext back and leaves the window where it is (the whole file is
// already there: at the end, pchNext == pchEndRead, as at a real EOF);
// mmioSetInfo takes pchNext back. mmioRead/mmioSeek/mmioDescend/mmioAscend
// work on the same position; mmioDescend finds chunks (kMmioFindChunk,
// kMmioFindRiff, kMmioFindList) within the parent and returns
// MMIOERR_CHUNKNOTFOUND (265) past it. Handles are 0x000C0000 + 4n.
//
// sndPlaySoundA (sound on): a file's or a kSndMemory image's PCM or
// IMA-/MS-ADPCM, one voice for the process (a new sound stops the last one;
// NULL stops it), kSndLoop loops, kSndNoStop refuses while one plays. A
// synchronous call (no SND_ASYNC) plays as an asynchronous one: the call
// returns at once, never a wait. Sound off: FALSE, as on a machine with no
// wave device. [LANE-2: checks the toys' sound end to end.]
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/log.h"
#include "win32/audio.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"
#include "win32/vfs.hh"

namespace adw::win32 {

namespace {

constexpr const char* W = "WINMM.DLL";
constexpr uint32_t kMmioBase = 0x000C0000, kMmioStep = 4;
// mmsystem.h's values (the SDK header needs more of Windows than this file includes).
constexpr uint32_t kMmioRead = 0x0, kMmioWrite = 0x1, kMmioReadWrite = 0x2, kMmioCreate = 0x1000, kMmioParse = 0x100,
                   kMmioDelete = 0x200, kMmioExist = 0x4000, kMmioAllocBuf = 0x10000;
constexpr uint32_t kMmioFindChunk = 0x10, kMmioFindRiff = 0x20, kMmioFindList = 0x40;
constexpr uint32_t kInvalHandle = 5;  // kInvalHandle
constexpr uint32_t kSndMemory = 0x4, kSndLoop = 0x8, kSndNoStop = 0x10;
constexpr uint32_t kErrCannotOpen = 257, kErrCannotSeek = 263, kErrChunkNotFound = 265, kErrInvalidParam = 11;
// MMIOINFO (72 bytes): the fields used here.
constexpr uint32_t kInfoFlags = 0, kInfoErr = 12, kInfoCchBuffer = 20, kInfoBuffer = 24, kInfoNext = 28,
                   kInfoEndRead = 32, kInfoEndWrite = 36, kInfoBufOffset = 40, kInfoDiskOffset = 44,
                   kInfoHmmio = 68, kInfoSize = 72;
// MMCKINFO (20 bytes): ckid, cksize, fccType, dwDataOffset, dwFlags.
constexpr uint32_t kFccRiff = 0x46464952, kFccList = 0x5453494C;  // "RIFF", "LIST"

struct MmioFile {
  bool open = false;
  std::string path;
  std::string bytes;
  uint32_t pos = 0;
  uint32_t buffer = 0;  // guest copy of the file (mmioGetInfo/MMIO_ALLOCBUF), 0 = none yet
};

struct MmioState : RuntimeState {
  std::vector<MmioFile> files;
  // sndPlaySoundA's one voice.
  audio::BufferId snd_buffer = 0;
  audio::VoiceId snd_voice = 0;
};

MmioState& ms(Runtime& rt) { return rt.state<MmioState>(); }

MmioFile* file(Runtime& rt, uint32_t h) {
  MmioState& s = ms(rt);
  if (h < kMmioBase || (h - kMmioBase) % kMmioStep) return nullptr;
  size_t i = (h - kMmioBase) / kMmioStep;
  return i < s.files.size() && s.files[i].open ? &s.files[i] : nullptr;
}

// The file's bytes in guest memory (made on first use).
uint32_t guest_buffer(Runtime& rt, MmioFile& f) {
  if (!f.buffer) {
    f.buffer = rt.heap().alloc(uint32_t(f.bytes.size()) + 1, true);
    if (!f.buffer) throw GuestError(GuestError::Kind::fatal, "out of guest memory for an mmio buffer");
    if (!f.bytes.empty()) rt.mem().memcpy(f.buffer, f.bytes.data(), f.bytes.size());
  }
  return f.buffer;
}

void write_info(Runtime& rt, uint32_t info, uint32_t h, MmioFile& f) {
  auto& mem = rt.mem();
  uint32_t b = guest_buffer(rt, f), n = uint32_t(f.bytes.size());
  mem.write_u32l(info + kInfoCchBuffer, n);
  mem.write_u32l(info + kInfoBuffer, b);
  mem.write_u32l(info + kInfoNext, b + std::min(f.pos, n));
  mem.write_u32l(info + kInfoEndRead, b + n);
  mem.write_u32l(info + kInfoEndWrite, b + n);
  mem.write_u32l(info + kInfoBufOffset, 0);
  mem.write_u32l(info + kInfoDiskOffset, n);
  mem.write_u32l(info + kInfoHmmio, h);
}

// The position pchNext names (mmioAdvance/mmioSetInfo).
void take_next(Runtime& rt, uint32_t info, MmioFile& f) {
  uint32_t next = rt.mem().read_u32l(info + kInfoNext);
  if (f.buffer && next >= f.buffer && next - f.buffer <= f.bytes.size()) f.pos = next - f.buffer;
}

uint32_t u32_at(const std::string& b, uint32_t p) {
  uint32_t v = 0;
  if (p + 4 <= b.size()) memcpy(&v, b.data() + p, 4);
  return v;
}

void stop_snd(Runtime& rt) {
  MmioState& s = ms(rt);
  if (!s.snd_voice) return;
  audio::Engine& e = audio_engine(rt);
  e.destroy_voice(s.snd_voice, audio_now(rt));
  e.release_buffer(s.snd_buffer);
  s.snd_voice = 0;
  s.snd_buffer = 0;
}

}  // namespace

void mmio_end_process(Runtime& rt) { stop_snd(rt); }

void register_mmio(ShimRegistry& r) {
  r.impl(W, "mmioOpenA", [](Call& c) {
    // (szFilename, lpmmioinfo, dwOpenFlags)
    uint32_t flags = c.arg(2), info = c.arg(1);
    std::string path = c.str(0);
    auto fail = [&](uint32_t err) {
      if (info) c.mem().write_u32l(info + kInfoErr, err);
      c.ret(0);
    };
    if (flags & (kMmioWrite | kMmioReadWrite | kMmioCreate | kMmioDelete | kMmioParse)) return fail(kErrCannotOpen);
    if (info && (c.mem().read_u32l(info + 8) || c.mem().read_u32l(info + 4))) return fail(kErrInvalidParam);  // custom/memory I/O
    MmioFile f;
    f.open = true;
    f.path = path;
    bool ok = c.rt.vfs().read_file(path, &f.bytes);
    trace("file", "mmioOpen \"%s\" -> \"%s\": %s", path.c_str(), c.rt.vfs().to_host(path).c_str(),
          ok ? "found" : "not found");
    if (!ok) return fail(kErrCannotOpen);
    if (flags & kMmioExist) return c.ret(1);
    MmioState& s = ms(c.rt);
    size_t i = 0;
    while (i < s.files.size() && s.files[i].open) i++;
    if (i == s.files.size()) s.files.emplace_back();
    s.files[i] = std::move(f);
    uint32_t h = kMmioBase + uint32_t(i) * kMmioStep;
    if (info) {
      c.mem().write_u32l(info + kInfoErr, 0);
      c.mem().write_u32l(info + kInfoHmmio, h);
    }
    c.ret(h);
  });
  r.impl(W, "mmioClose", [](Call& c) {
    MmioFile* f = file(c.rt, c.arg(0));
    if (!f) return c.ret(kInvalHandle);
    if (f->buffer) c.rt.heap().free(f->buffer);
    *f = MmioFile{};
    c.ret(0);
  });
  r.impl(W, "mmioRead", [](Call& c) {
    // (hmmio, pch, cch) → bytes read, -1 on error.
    MmioFile* f = file(c.rt, c.arg(0));
    if (!f) return c.ret(0xFFFFFFFF);
    uint32_t n = std::min<uint32_t>(c.arg(2), uint32_t(f->bytes.size()) - std::min<uint32_t>(f->pos, uint32_t(f->bytes.size())));
    if (n) c.mem().memcpy(c.arg(1), f->bytes.data() + f->pos, n);
    f->pos += n;
    c.ret(n);
  });
  r.impl(W, "mmioSeek", [](Call& c) {
    // (hmmio, lOffset, iOrigin) → the new position, -1 when out of the file.
    MmioFile* f = file(c.rt, c.arg(0));
    if (!f) return c.ret(0xFFFFFFFF);
    int64_t base = c.arg(2) == SEEK_CUR ? f->pos : c.arg(2) == SEEK_END ? int64_t(f->bytes.size()) : 0;
    int64_t p = base + c.iarg(1);
    if (p < 0 || p > int64_t(f->bytes.size())) return c.ret(0xFFFFFFFF);
    f->pos = uint32_t(p);
    c.ret(f->pos);
  });
  r.impl(W, "mmioDescend", [](Call& c) {
    // (hmmio, lpck, lpckParent, wFlags)
    MmioFile* f = file(c.rt, c.arg(0));
    if (!f) return c.ret(kInvalHandle);
    auto& mem = c.mem();
    uint32_t ck = c.arg(1), parent = c.arg(2), flags = c.arg(3);
    uint32_t end = uint32_t(f->bytes.size());
    if (parent) end = std::min(end, mem.read_u32l(parent + 12) + mem.read_u32l(parent + 4));
    uint32_t want_id = mem.read_u32l(ck), want_type = mem.read_u32l(ck + 8);
    if (flags & kMmioFindRiff) want_id = kFccRiff;
    if (flags & kMmioFindList) want_id = kFccList;
    bool find = flags & (kMmioFindChunk | kMmioFindRiff | kMmioFindList);
    for (uint32_t p = f->pos; p + 8 <= end;) {
      uint32_t id = u32_at(f->bytes, p), size = u32_at(f->bytes, p + 4);
      bool listy = id == kFccRiff || id == kFccList;
      uint32_t type = listy ? u32_at(f->bytes, p + 8) : 0;
      if (!find || (id == want_id && (!(flags & (kMmioFindRiff | kMmioFindList)) || type == want_type))) {
        mem.write_u32l(ck, id);
        mem.write_u32l(ck + 4, size);
        mem.write_u32l(ck + 8, type);
        mem.write_u32l(ck + 12, p + 8);
        mem.write_u32l(ck + 16, 0);
        f->pos = p + 8 + (listy ? 4 : 0);
        return c.ret(0);
      }
      uint64_t next = uint64_t(p) + 8 + size + (size & 1);
      if (next > end) break;
      p = uint32_t(next);
    }
    c.ret(kErrChunkNotFound);
  });
  r.impl(W, "mmioAscend", [](Call& c) {
    // (hmmio, lpck, wFlags): past the chunk's data and its pad byte.
    MmioFile* f = file(c.rt, c.arg(0));
    if (!f) return c.ret(kInvalHandle);
    uint32_t ck = c.arg(1);
    uint64_t p = uint64_t(c.mem().read_u32l(ck + 12)) + c.mem().read_u32l(ck + 4);
    p += p & 1;
    if (p > f->bytes.size()) return c.ret(kErrCannotSeek);
    f->pos = uint32_t(p);
    c.ret(0);
  });
  r.impl(W, "mmioGetInfo", [](Call& c) {
    // (hmmio, lpmmioinfo, wFlags)
    MmioFile* f = file(c.rt, c.arg(0));
    if (!f) return c.ret(kInvalHandle);
    c.mem().memset(c.arg(1), 0, kInfoSize);
    c.mem().write_u32l(c.arg(1) + kInfoFlags, kMmioRead | kMmioAllocBuf);
    write_info(c.rt, c.arg(1), c.arg(0), *f);
    c.ret(0);
  });
  r.impl(W, "mmioSetInfo", [](Call& c) {
    MmioFile* f = file(c.rt, c.arg(0));
    if (!f) return c.ret(kInvalHandle);
    take_next(c.rt, c.arg(1), *f);
    c.ret(0);
  });
  r.impl(W, "mmioAdvance", [](Call& c) {
    // (hmmio, lpmmioinfo, wFlags): the whole file is the buffer already.
    MmioFile* f = file(c.rt, c.arg(0));
    if (!f) return c.ret(kInvalHandle);
    if (c.arg(1)) {
      take_next(c.rt, c.arg(1), *f);
      write_info(c.rt, c.arg(1), c.arg(0), *f);
    }
    c.ret(0);
  });

  r.impl(W, "sndPlaySoundA", [](Call& c) {
    // (lpszSound, fuSound)
    Runtime& rt = c.rt;
    uint32_t flags = c.arg(1);
    if (!audio_enabled(rt)) return c.ret(0);
    MmioState& s = ms(rt);
    audio::Engine& e = audio_engine(rt);
    if (!c.arg(0)) {
      stop_snd(rt);
      return c.ret(1);
    }
    if ((flags & kSndNoStop) && s.snd_voice && e.playing(s.snd_voice, audio_now(rt))) return c.ret(0);
    std::string image;
    if (flags & kSndMemory) {
      std::string head = c.mem().read(c.arg(0), 12);
      size_t n = audio::riff_extent(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(head.data()), 12));
      if (!n) return c.ret(0);
      image = c.mem().read(c.arg(0), uint32_t(n));
    } else if (!rt.vfs().read_file(c.str(0), &image)) {
      trace("sound", "sndPlaySound \"%s\": no such file", c.str(0).c_str());
      return c.ret(0);
    }
    audio::Wave w;
    if (!audio::parse_wave(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(image.data()), image.size()), w) ||
        !audio::decodable(w.format))
      return c.ret(0);
    std::vector<uint8_t> pcm = audio::decode(w.format, w.data);
    audio::WaveFormat fmt = audio::decoded_format(w.format);
    stop_snd(rt);
    audio::Time now = audio_now(rt);
    s.snd_buffer = e.create_buffer(fmt, uint32_t(pcm.size()));
    if (!s.snd_buffer) return c.ret(0);
    e.write_buffer(s.snd_buffer, 0, pcm, now);
    s.snd_voice = e.create_voice(s.snd_buffer, audio::Bus::wave);
    e.play(s.snd_voice, (flags & kSndLoop) != 0, now);
    trace("sound", "sndPlaySound %s: %zu bytes%s", (flags & kSndMemory) ? "(memory)" : c.str(0).c_str(), pcm.size(),
          (flags & kSndLoop) ? ", looping" : "");
    c.ret(1);
  });
}

}  // namespace adw::win32
