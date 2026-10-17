// The host audio engine as the Win32 shims see it (docs/AUDIO.md §7).
//
// One adw::audio::Engine per process: the lane hands it over with
// attach_audio() (LaneContext::audio, or audio::null_engine() when the host
// runs without one), and the DSOUND (dsound.cc), WINMM (winmm.cc) and
// MSACM32 (msacm32.cc) shims reach it through audio_engine(). Until a lane
// attaches one, and whenever the engine is disabled (no ADSOUND=1, no
// ADAUDIOOUT), every shim answers exactly as the silent host always did:
// dsound.dll is not found, MCI and aux have no devices, ACM has no driver and
// waveOut has no device (§7.1). Nothing but config()/enabled() is called on a
// disabled engine.
//
// Time: every audio call passes the engine audio_now(), the virtual clock
// peeked (rt.clock().now_us()); an audio call never moves the clock and never
// passes a time later than now (the engine takes the latest time it has seen
// as "now" for ever after).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "adw/core/audio.h"

namespace adw::win32 {

class Runtime;

struct AudioOptions {
  // The module belongs to the deluxe package (FILES\<MODDIR>): the Deluxe
  // music renames apply (AUDIO.md §7.7).
  bool deluxe = false;
};

// Makes `engine` the process's engine. With engine.enabled() it also
// registers DSOUND.DLL as a shim DLL, so LoadLibraryExA("dsound.dll") returns
// its handle and GetProcAddress("DirectSoundCreate") its thunk, and, for a
// deluxe module, adds the missing C:\AFTERDRK\Music\<long name>.mid files of
// the rename table to the VFS as read-only virtual files (their 8.3
// originals' bytes), so the engine's _access check and MCI_OPEN both find
// them. Call once, after register_all_shims() and the disk mounts, before
// the module loads. Configure mode never calls it (RealUi keeps its own
// DirectSoundCreate).
void attach_audio(Runtime& rt, audio::Engine& engine, const AudioOptions& opts = {});
// The attached engine, or audio::null_engine().
audio::Engine& audio_engine(Runtime& rt);
// What attach_audio() was given (defaults before it runs).
const AudioOptions& audio_options(Runtime& rt);
bool audio_enabled(Runtime& rt);
// Guest time of an audio call: the virtual clock, peeked.
audio::Time audio_now(Runtime& rt);

// Applies the engine's events up to now (winmm.cc): a finished waveOut chunk
// clears WHDR_INQUEUE and sets WHDR_DONE on its guest WAVEHDR (and queues its
// MM_WOM_DONE), a song end posts its pending MCI notification; then the
// waveOut callbacks that are due run in order (CALLBACK_WINDOW messages are
// posted, CALLBACK_FUNCTION procedures called, CALLBACK_EVENT events set),
// never re-entrantly. Every WINMM shim calls it at entry (timeGetTime
// included) and the lane before every Module() call, so a guest that polls
// a WAVEHDR sees WHDR_DONE exactly when virtual time passes the chunk's end.
// A no-op while the engine is disabled.
void audio_pump(Runtime& rt);

// DSOUND.DLL (dsound.cc): registers DirectSoundCreate and the IDirectSound /
// IDirectSoundBuffer method thunks. attach_audio() calls it when enabled.
void register_dsound(Runtime& rt);
// A program that imports DSOUND.DLL statically (pe32/program.cc) runs on a
// machine without a sound card while the engine is disabled: DirectSoundCreate
// exists and answers DSERR_NODRIVER (0x88780078), *lplpDS = NULL.
void register_dsound_absent(Runtime& rt);

// A whole program's process ends while the engine lives on (pe32/program.cc:
// Caps Lock starts the program again): every sound it made stops and goes,
// as Windows freed a dead process's sound — its DirectSound buffers
// (dsound.cc), sndPlaySoundA's voice (mmio.cc), its waveOut streams and MCI
// songs (winmm.cc), no callbacks. Call before the runtime goes. A no-op
// while the engine is disabled.
void audio_end_process(Runtime& rt);
void dsound_end_process(Runtime& rt);
void mmio_end_process(Runtime& rt);
void winmm_end_process(Runtime& rt);

// A WAVEFORMAT/PCMWAVEFORMAT/WAVEFORMATEX in guest memory: 16 bytes for PCM,
// 18 + cbSize (at most 18 + 256) for anything else, parsed with
// audio::parse_waveformat. `raw` (optional) receives the bytes read. A bad
// pointer faults like any guest access (guest.hh).
bool read_guest_waveformat(Runtime& rt, uint32_t addr, audio::WaveFormat& out, std::vector<uint8_t>* raw = nullptr);
// Writes f as WAVEFORMATEX (audio::waveformat_bytes), at most `cap` bytes;
// returns the bytes written.
uint32_t write_guest_waveformat(Runtime& rt, uint32_t addr, const audio::WaveFormat& f, uint32_t cap);

// The Deluxe music renames (AUDIO.md §7.7): the 8.3 file beside the module
// that a missing Music\<name> stands for (case-insensitive: "Flying
// Toasters.mid" -> "TOASTERS.MID"), or "" when `name` is not in the table.
std::string deluxe_music_file(std::string_view name);
// A guest path's Deluxe stand-in: for <dir>\Music\<name> with <name> in the
// table, <dir>\<8.3 name>; else "".
std::string deluxe_music_path(std::string_view guest_path);

}  // namespace adw::win32
