// WINMM.DLL — the AD4 frame clock, and the sound devices over the host audio
// engine (API_SURFACE.md §1 "WINMM.DLL"; AUDIO.md §7.4–§7.6).
//
// timeGetTime is THE clock of the AD4 lane: the engine's XTimer reads it and
// modules pace themselves from it (ABI.md §2.7). It reads the VirtualClock,
// whose every read advances time a little (the lane sets the read step), so
// deadline loops inside one Module() call terminate deterministically.
//
// Sound off (the engine disabled: no ADSOUND=1, no ADAUDIOOUT): exactly the
// silent host of old. MCI, aux, mixer, wave input and wave output answer "no
// driver / no device", which the engine and the modules handle by running
// silent (ABI.md §2.12).
//
// Sound on:
//   * waveOut (Hall of Fame's JSound, §2.7): one device, "Long After Dark",
//     every PCM format, no WAVECAPS_SYNC. An open device is an engine stream;
//     waveOutWrite queues the header's bytes (copied), and the stream's
//     chunk_done events come back as WHDR_DONE on the guest's WAVEHDRs. They
//     are applied at every WINMM call (timeGetTime included) and before every
//     Module() call (audio_pump), so a poller sees DONE exactly when virtual
//     time passes the chunk's end. CALLBACK_WINDOW posts MM_WOM_OPEN/DONE/
//     CLOSE; CALLBACK_FUNCTION procedures and CALLBACK_EVENT events run at the
//     next such point, in order, never re-entrantly (§8.6). The device volume
//     is the engine's wave bus.
//   * MCI (mciSendCommandA, §2.3, §7.5): the sequencer only, over the
//     engine's Standard MIDI File player. Open by type, type id or element
//     (a .mid through the VFS; the Deluxe renames, §7.7), with an alias, or
//     the device alone (the engine's test open). Set (port MIDI_MAPPER or 0,
//     time format ms), play (from/to/notify), stop, pause/resume, seek
//     (start/end/to), status (mode, length, position, ready, tracks, …),
//     close. MM_MCINOTIFY is posted to dwCallback: SUCCESSFUL at the song's
//     end (or its MCI_TO position, noticed at the first pump point after
//     it; the song is then left at that position),
//     ABORTED when stop/seek/pause/play/close interrupts a pending play, and
//     SUPERSEDED when another command asks for notification meanwhile.
//     mciSendStringA stays refused (only XCdAudio uses it: no CD audio).
//   * aux: two devices, 0 = CD audio (volume stored), 1 = the MIDI synth
//     (volume = the engine's MIDI bus). mixerGetLineInfoA keeps failing, so
//     the engine keeps per-buffer DirectSound volume (§2.4). waveIn stays at
//     0 devices: no microphone access, ever.
//
// Known gaps, deliberately left (no module of the 202 in the five releases
// asks for them): MCI song-pointer/SMPTE time formats, wave looping
// (WHDR_BEGINLOOP/ENDLOOP play once), and CALLBACK_THREAD.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "adw/core/log.h"
#include "win32/audio.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"
#include "win32/vfs.hh"

namespace adw::win32 {

namespace {
constexpr const char* W = "WINMM.DLL";

// MMSYSERR_* / WAVERR_*.
constexpr uint32_t kOk = 0;
constexpr uint32_t kBadDeviceId = 2;     // MMSYSERR_BADDEVICEID
constexpr uint32_t kInvalidHandle = 5;   // MMSYSERR_INVALHANDLE
constexpr uint32_t kNoDriver = 6;        // MMSYSERR_NODRIVER
constexpr uint32_t kNotSupported = 8;    // MMSYSERR_NOTSUPPORTED
constexpr uint32_t kInvalidFlag = 10;    // MMSYSERR_INVALFLAG
constexpr uint32_t kInvalidParam = 11;   // MMSYSERR_INVALPARAM
constexpr uint32_t kBadFormat = 32;      // WAVERR_BADFORMAT
constexpr uint32_t kStillPlaying = 33;   // WAVERR_STILLPLAYING
constexpr uint32_t kUnprepared = 34;     // WAVERR_UNPREPARED

// MCIERR_* (MCIERR_BASE 256).
constexpr uint32_t kMciInvalidDeviceId = 257, kMciUnrecognizedCommand = 261,
                   kMciMissingParameter = 273, kMciUnsupportedFunction = 274, kMciFileNotFound = 275,
                   kMciCannotUseAll = 279, kMciExtensionNotFound = 281, kMciOutOfRange = 282,
                   kMciFlagsNotCompatible = 284, kMciDuplicateAlias = 289, kMciBadTimeFormat = 293,
                   kMciInvalidFile = 296, kMciNullParameterBlock = 297, kMciMissingDeviceName = 292,
                   kMciDeviceNotInstalled = 306, kMciSeqPortNonexistent = 338;

// Messages and flags (mmsystem.h).
constexpr uint32_t MCI_OPEN = 0x803, MCI_CLOSE = 0x804, MCI_PLAY = 0x806, MCI_SEEK = 0x807, MCI_STOP = 0x808,
                   MCI_PAUSE = 0x809, MCI_INFO = 0x80A, MCI_GETDEVCAPS = 0x80B, MCI_SET = 0x80D,
                   MCI_STATUS = 0x814, MCI_RESUME = 0x855;
constexpr uint32_t MCI_NOTIFY_ = 0x1, MCI_WAIT_ = 0x2, MCI_FROM_ = 0x4, MCI_TO_ = 0x8, MCI_TRACK_ = 0x10;
constexpr uint32_t MCI_OPEN_ELEMENT_ = 0x200, MCI_OPEN_ALIAS_ = 0x400,
                   MCI_OPEN_ELEMENT_ID_ = 0x800, MCI_OPEN_TYPE_ID_ = 0x1000, MCI_OPEN_TYPE_ = 0x2000;
constexpr uint32_t MCI_SEEK_TO_START_ = 0x100, MCI_SEEK_TO_END_ = 0x200;
constexpr uint32_t MCI_STATUS_ITEM_ = 0x100, MCI_STATUS_START_ = 0x200;
constexpr uint32_t MCI_SET_TIME_FORMAT_ = 0x400, MCI_SEQ_SET_PORT_ = 0x20000;
constexpr uint32_t MCI_GETDEVCAPS_ITEM_ = 0x100;
constexpr uint32_t MCI_INFO_PRODUCT_ = 0x100, MCI_INFO_FILE_ = 0x200;
constexpr uint32_t MCI_ALL_DEVICE_ID_ = 0xFFFF;
constexpr uint32_t MCI_DEVTYPE_SEQUENCER_ = 0x20B;
constexpr uint32_t MCI_MODE_STOP_ = 0x20D, MCI_MODE_PLAY_ = 0x20E, MCI_MODE_PAUSE_ = 0x211;
constexpr uint32_t MCI_FORMAT_MILLISECONDS_ = 0;
constexpr uint32_t MIDI_MAPPER_ = 0xFFFFFFFF, MCI_SEQ_NONE_ = 65533;
constexpr uint32_t MM_MCINOTIFY_ = 0x3B9;
constexpr uint32_t MCI_NOTIFY_SUCCESSFUL_ = 1, MCI_NOTIFY_SUPERSEDED_ = 2, MCI_NOTIFY_ABORTED_ = 4;
constexpr uint32_t MM_WOM_OPEN_ = 0x3BB, MM_WOM_CLOSE_ = 0x3BC, MM_WOM_DONE_ = 0x3BD;
constexpr uint32_t WAVE_MAPPER_ = 0xFFFFFFFF, WAVE_FORMAT_QUERY_ = 0x1;
constexpr uint32_t CALLBACK_TYPEMASK_ = 0x70000, CALLBACK_NULL_ = 0, CALLBACK_WINDOW_ = 0x10000,
                   CALLBACK_THREAD_ = 0x20000, CALLBACK_FUNCTION_ = 0x30000, CALLBACK_EVENT_ = 0x50000;
constexpr uint32_t WHDR_DONE_ = 0x1, WHDR_PREPARED_ = 0x2, WHDR_BEGINLOOP_ = 0x4, WHDR_ENDLOOP_ = 0x8,
                   WHDR_INQUEUE_ = 0x10;
constexpr uint32_t kWaveHdrSize = 32;  // WAVEHDR (32-bit)
constexpr uint32_t TIME_MS_ = 1, TIME_SAMPLES_ = 2, TIME_BYTES_ = 4;

// Guest handles of open wave devices (a 16-bit-looking value, never a device id).
constexpr uint32_t kWaveHandleBase = 0x7A00;

struct WaveOut {
  uint32_t handle = 0;
  audio::StreamId stream = 0;
  audio::WaveFormat format;
  uint32_t callback_type = 0, callback = 0, instance = 0;
  std::deque<uint32_t> queued;  // WAVEHDR addresses, in queue order
};

// A waveOut callback that runs at the next pump point (function / event).
struct Deferred {
  uint32_t type = 0, callback = 0, handle = 0, msg = 0, instance = 0, p1 = 0;
};

struct MciDevice {
  std::string alias, element;
  audio::SongId song = 0;
  uint32_t notify_hwnd = 0;  // a PLAY's pending MCI_NOTIFY
  bool notify_pending = false;
  bool paused = false;
  bool has_to = false;
  uint64_t to_us = 0;
  uint32_t time_format = MCI_FORMAT_MILLISECONDS_;
};

struct WinmmState : RuntimeState {
  std::map<uint32_t, WaveOut> waves;  // by handle
  uint32_t next_wave = 0;
  std::deque<Deferred> deferred;
  bool pumping = false;
  std::map<uint32_t, MciDevice> mci;  // by device id
  uint32_t next_mci = 1;
  uint32_t wave_volume = 0xFFFFFFFF;  // waveOutSetVolume's last value
  uint32_t aux_volume[2] = {0xFFFFFFFF, 0xFFFFFFFF};
  std::vector<audio::Event> events;   // scratch for poll()
  bool event_logged = false;
};

WinmmState& ws(Runtime& rt) { return rt.state<WinmmState>(); }

WaveOut* wave_by_handle(WinmmState& s, uint32_t h) {
  auto it = s.waves.find(h);
  return it == s.waves.end() ? nullptr : &it->second;
}

// A device id (0 or WAVE_MAPPER) or an open handle.
bool is_wave_device(WinmmState& s, uint32_t id) { return id == 0 || id == WAVE_MAPPER_ || wave_by_handle(s, id); }

// ---- callbacks (§8.6) ---------------------------------------------------------------------------------

void wave_callback(Runtime& rt, WaveOut& w, uint32_t msg, uint32_t p1) {
  WinmmState& s = ws(rt);
  switch (w.callback_type) {
    case CALLBACK_WINDOW_:
      if (!post_guest_message(rt, w.callback, msg, w.handle, p1))
        trace("sound", "waveOut 0x%X: window 0x%X is gone (message 0x%X dropped)", w.handle, w.callback, msg);
      break;
    case CALLBACK_FUNCTION_:
    case CALLBACK_EVENT_:
      s.deferred.push_back(Deferred{w.callback_type, w.callback, w.handle, msg, w.instance, p1});
      break;
    default:
      break;
  }
}

void run_deferred(Runtime& rt) {
  WinmmState& s = ws(rt);
  while (!s.deferred.empty()) {
    Deferred d = s.deferred.front();
    s.deferred.pop_front();
    if (d.type == CALLBACK_FUNCTION_) {
      // void CALLBACK waveOutProc(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR)
      trace("sound", "waveOut 0x%X: callback 0x%08X(0x%X)", d.handle, d.callback, d.msg);
      rt.call_guest(d.callback, {d.handle, d.msg, d.instance, d.p1, 0}, Conv::stdcall_);
    } else if (d.type == CALLBACK_EVENT_) {
      // There is no event object model unless KERNEL32 grows SetEvent.
      ShimEntry* e = rt.shims().find("KERNEL32.DLL", "SetEvent");
      if (e && e->fn) {
        rt.call_guest(rt.shims().thunk_address(*e), {d.callback}, e->conv);
      } else if (!s.event_logged) {
        s.event_logged = true;
        trace("sound", "waveOut: CALLBACK_EVENT 0x%X cannot be set (no event objects)", d.callback);
      }
    }
  }
}

// MM_MCINOTIFY(code, device id) to `hwnd`.
void mci_notify(Runtime& rt, uint32_t hwnd, uint32_t code, uint32_t id) {
  if (!hwnd) return;
  trace("sound", "MCI device %u: notify 0x%X -> window 0x%X", id, code, hwnd);
  post_guest_message(rt, hwnd, MM_MCINOTIFY_, code, id);
}

// A pending PLAY notification ends: `code` is ABORTED or SUPERSEDED.
void end_pending(Runtime& rt, uint32_t id, MciDevice& d, uint32_t code) {
  if (!d.notify_pending) return;
  d.notify_pending = false;
  mci_notify(rt, d.notify_hwnd, code, id);
}

// Applies the engine's events up to `now` (no callbacks run here).
void apply_events(Runtime& rt, audio::Time now) {
  WinmmState& s = ws(rt);
  audio::Engine& e = audio_engine(rt);
  // MCI_TO: a playing song stops (and notifies) at the first pump point at
  // or after its end position, and is left AT that position, as MCI reports
  // it (the Classic lane's `play … to n` does the same, sound16.cc).
  for (auto& [id, d] : s.mci) {
    if (!d.has_to || !d.song || !e.song_playing(d.song, now)) continue;
    if (e.song_position(d.song, now) < d.to_us) continue;
    e.song_stop(d.song, now);
    e.song_seek(d.song, d.to_us, now);
    d.has_to = false;
    if (d.notify_pending) {
      d.notify_pending = false;
      mci_notify(rt, d.notify_hwnd, MCI_NOTIFY_SUCCESSFUL_, id);
    }
  }
  s.events.clear();
  e.poll(now, s.events);
  for (const audio::Event& ev : s.events) {
    switch (ev.kind) {
      case audio::Event::Kind::chunk_done: {
        for (auto& [h, w] : s.waves) {
          if (w.stream != ev.id) continue;
          uint32_t hdr = uint32_t(ev.cookie);
          auto q = std::find(w.queued.begin(), w.queued.end(), hdr);
          if (q == w.queued.end()) break;
          w.queued.erase(q);
          uint32_t flags = rt.mem().read_u32l(hdr + 16);
          rt.mem().write_u32l(hdr + 16, (flags & ~WHDR_INQUEUE_) | WHDR_DONE_);
          wave_callback(rt, w, MM_WOM_DONE_, hdr);
          break;
        }
        break;
      }
      case audio::Event::Kind::song_end:
        for (auto& [id, d] : s.mci) {
          if (d.song != ev.id) continue;
          d.has_to = false;
          if (d.notify_pending) {
            d.notify_pending = false;
            mci_notify(rt, d.notify_hwnd, MCI_NOTIFY_SUCCESSFUL_, id);
          }
        }
        break;
      case audio::Event::Kind::voice_end:
        break;  // DirectSound status is asked, not told
    }
  }
}

// Every WINMM shim runs this first (audio_pump).
void entry(Runtime& rt) { audio_pump(rt); }

// Registers a WINMM function whose handler runs after the pump.
void mm(ShimRegistry& r, const char* name, ShimFn fn) {
  r.impl(W, name, [fn = std::move(fn)](Call& c) {
    entry(c.rt);
    fn(c);
  });
}

// ---- MCI --------------------------------------------------------------------------------------------

bool iequal(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (tolower(uint8_t(a[i])) != tolower(uint8_t(b[i]))) return false;
  return true;
}

// The real texts (Windows' winmm string table, English).
const char* mci_error_text(uint32_t e) {
  switch (e) {
    case 257: return "Invalid MCI device ID.  Use the ID returned when opening the MCI device.";
    case 259: return "The driver cannot recognize the specified command parameter.";
    case 261: return "The driver cannot recognize the specified command.";
    case 262: return "There is a problem with your media device.  Make sure it is working correctly or contact the device manufacturer.";
    case 263: return "The specified device is not open or is not recognized by MCI.";
    case 264: return "Not enough memory available for this task.\r\n\r\nQuit one or more applications to increase available memory, and then try again.";
    case 265: return "The device name is already being used as an alias by this application.  Use a unique alias.";
    case 266: return "Unknown problem while loading the specified device driver.";
    case 267: return "No command was specified.";
    case 268: return "The output string was too large to fit in the return buffer.  Increase the size of the buffer.";
    case 269: return "The specified command requires a character-string parameter.  Please provide one.";
    case 270: return "The specified integer is invalid for this command.";
    case 271: return "The device driver returned an invalid return type.  Check with the device manufacturer about obtaining a new driver.";
    case 272: return "There is a problem with the device driver.  Check with the device manufacturer about obtaining a new driver.";
    case 273: return "The specified command requires a parameter.  Please supply one.";
    case 274: return "The MCI device you are using does not support the specified command.";
    case 275: return "Cannot find the specified file.  Make sure the path and filename are correct.";
    case 276: return "The device driver is not ready.";
    case 277: return "A problem occurred in initializing MCI.";
    case 278: return "There is a problem with the device driver.  The driver has closed.  Cannot access error.";
    case 279: return "Cannot use 'all' as the device name with the specified command.";
    case 280: return "Errors occurred in more than one device.  Specify each command and device separately to determine which devices caused the errors.";
    case 281: return "Cannot determine the device type from the given filename extension.";
    case 282: return "The specified parameter is out of range for the specified command.";
    case 284: return "The specified parameters cannot be used together.";
    case 286: return "Cannot save the specified file.  Make sure you have enough disk space or are still connected to the network.";
    case 287: return "Cannot find the specified device.  Make sure it is installed or that the device name is spelled correctly.";
    case 288: return "The specified device is now being closed.  Wait a few seconds, and then try again.";
    case 289: return "The specified alias is already being used in this application.  Use a unique alias.";
    case 290: return "The specified parameter is invalid for this command.";
    case 291: return "The device driver is already in use.  To share it, use the 'shareable' parameter with each 'open' command.";
    case 292: return "The specified command requires an alias, file, driver, or device name.  Please supply one.";
    case 293: return "The specified value for the time format is invalid.  Refer to the MCI documentation for valid formats.";
    case 294: return "A closing double-quotation mark is missing from the parameter value.  Please supply one.";
    case 295: return "A parameter or value was specified twice.  Only specify it once.";
    case 296: return "The specified file cannot be played on the specified MCI device.  The file may be corrupt, not in the correct format, or no file handler available for this format.";
    case 297: return "A null parameter block was passed to MCI.";
    case 298: return "Cannot save an unnamed file.  Supply a filename.";
    case 299: return "You must specify an alias when using the 'new' parameter.";
    case 300: return "Cannot use the 'notify' flag with auto-opened devices.";
    case 301: return "Cannot use a filename with the specified device.";
    case 302: return "Cannot carry out the commands in the order specified.  Correct the command sequence, and then try again.";
    case 303: return "Cannot carry out the specified command on an auto-opened device.  Wait until the device is closed, and then try again.";
    case 304: return "The filename is invalid.  Make sure the filename is not longer than 8 characters, followed by a period and an extension.";
    case 305: return "Cannot specify extra characters after a string enclosed in quotation marks.";
    case 306: return "The specified device is not installed on the system. To install the device, go to Control Panel, click Printers and Other Hardware, and then click Add Hardware.";
    case 307: return "Cannot access the specified file or MCI device.  Try changing directories or restarting your computer.";
    case 308: return "Cannot access the specified file or MCI device because the application cannot change directories.";
    case 309: return "Cannot access specified file or MCI device because the application cannot change drives.";
    case 310: return "Specify a device or driver name that is less than 79 characters.";
    case 311: return "Specify a device or driver name that is less than 69 characters.";
    case 312: return "The specified command requires an integer parameter.  Please provide one.";
    case 336: return "Cannot use the song-pointer time format and the SMPTE time-format together.";
    case 337: return "The specified MIDI device is already in use.  Wait until it is free, and then try again.";
    case 338: return "The specified MIDI device is not installed on the system. To install the driver, go to Control Panel, click Printers and Other Hardware, and then click Add Hardware.";
    case 339: return "The current MIDI Mapper setup refers to a MIDI device that is not installed on the system.  Use MIDI Mapper to edit the setup.";
    case 340: return "An error occurred using the specified port.";
    case 341: return "All multimedia timers are being used by other applications.  Quit one of these applications, and then try again.";
    case 342: return "There is no current MIDI port.";
    case 343: return "There are no MIDI devices installed on the system. To install the driver, go to Control Panel, click Printers and Other Hardware, and then click Add Hardware.";
    default: return nullptr;
  }
}

// The bytes of a MIDI file the guest names (relative to the current
// directory), with the Deluxe renames as a fallback. False = no such file.
bool read_song_file(Runtime& rt, const std::string& path, std::string* out) {
  Vfs& vfs = rt.vfs();
  if (vfs.read_file(path, out)) return true;
  // attach_audio() adds the renames as virtual files; this covers a path the
  // table knows but spelled another way (another directory level).
  if (!audio_options(rt).deluxe) return false;
  std::string alt = deluxe_music_path(vfs.full_path(path));
  return !alt.empty() && vfs.read_file(alt, out);
}

// What an element's extension opens: 0 = the sequencer, else an MCI error.
uint32_t device_for_element(const std::string& path) {
  size_t dot = path.find_last_of('.');
  size_t slash = path.find_last_of("\\/");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return kMciExtensionNotFound;
  std::string ext = path.substr(dot + 1);
  for (const char* e : {"mid", "midi", "rmi", "kar"})
    if (iequal(ext, e)) return 0;
  for (const char* e : {"wav", "avi", "cda"})
    if (iequal(ext, e)) return kMciDeviceNotInstalled;  // waveaudio, avivideo, cdaudio: none here
  return kMciExtensionNotFound;
}

uint32_t mci_open(Runtime& rt, uint32_t flags, uint32_t p) {
  WinmmState& s = ws(rt);
  auto& mem = rt.mem();
  if (!p) return kMciNullParameterBlock;
  std::string element, alias;
  if (flags & MCI_OPEN_ELEMENT_ID_) return kMciUnsupportedFunction;
  if (flags & MCI_OPEN_ELEMENT_) element = read_cstr(mem, mem.read_u32l(p + 12));
  if (flags & MCI_OPEN_TYPE_) {
    uint32_t t = mem.read_u32l(p + 8);
    if (flags & MCI_OPEN_TYPE_ID_) {
      if ((t & 0xFFFF) != MCI_DEVTYPE_SEQUENCER_) return kMciDeviceNotInstalled;
    } else {
      std::string type = read_cstr(mem, t);
      // "sequencer!<element>" names both.
      if (size_t bang = type.find('!'); bang != std::string::npos) {
        if (element.empty()) element = type.substr(bang + 1);
        type.resize(bang);
      }
      if (!iequal(type, "sequencer")) {
        trace("sound", "MCI_OPEN \"%s\": not installed (the sequencer is the only MCI device)", type.c_str());
        return kMciDeviceNotInstalled;
      }
    }
  } else if (!element.empty()) {
    if (uint32_t err = device_for_element(element)) return err;
  } else {
    return kMciMissingDeviceName;
  }
  if (flags & MCI_OPEN_ALIAS_) {
    alias = read_cstr(mem, mem.read_u32l(p + 16));
    for (auto& [id, d] : s.mci)
      if (!alias.empty() && iequal(d.alias, alias)) return kMciDuplicateAlias;
  }
  MciDevice d;
  d.alias = alias;
  d.element = element;
  if (!element.empty()) {
    std::string bytes;
    if (!read_song_file(rt, element, &bytes)) {
      trace("sound", "MCI_OPEN \"%s\": file not found", element.c_str());
      return kMciFileNotFound;
    }
    std::string err;
    d.song = audio_engine(rt).load_song(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), &err);
    if (!d.song) {
      trace("sound", "MCI_OPEN \"%s\": not a playable MIDI file (%s)", element.c_str(), err.c_str());
      return kMciInvalidFile;
    }
  }
  uint32_t id = s.next_mci++;
  s.mci[id] = std::move(d);
  mem.write_u32l(p + 4, id);
  trace("sound", "MCI_OPEN sequencer%s%s -> device %u", element.empty() ? "" : " ", element.c_str(), id);
  if (flags & MCI_NOTIFY_) mci_notify(rt, mem.read_u32l(p), MCI_NOTIFY_SUCCESSFUL_, id);
  return kOk;
}

void mci_close(Runtime& rt, uint32_t id, MciDevice& d) {
  end_pending(rt, id, d, MCI_NOTIFY_ABORTED_);
  if (d.song) audio_engine(rt).close_song(d.song, audio_now(rt));
  d.song = 0;
}

uint32_t ms_of(uint64_t us) { return uint32_t(std::min<uint64_t>(us / 1000, 0xFFFFFFFFu)); }

uint32_t mci_status(Runtime& rt, MciDevice& d, uint32_t flags, uint32_t p, uint32_t* value) {
  if (!(flags & MCI_STATUS_ITEM_)) return kMciMissingParameter;
  audio::Engine& e = audio_engine(rt);
  audio::Time now = audio_now(rt);
  uint32_t item = rt.mem().read_u32l(p + 8);
  switch (item) {
    case 1:  // MCI_STATUS_LENGTH
      if ((flags & MCI_TRACK_) && rt.mem().read_u32l(p + 12) != 1) return kMciOutOfRange;
      *value = d.song ? ms_of(e.song_length(d.song)) : 0;
      return kOk;
    case 2:  // MCI_STATUS_POSITION
      *value = !d.song || (flags & MCI_STATUS_START_) ? 0 : ms_of(e.song_position(d.song, now));
      return kOk;
    case 3:  // MCI_STATUS_NUMBER_OF_TRACKS
      *value = 1;
      return kOk;
    case 4:  // MCI_STATUS_MODE
      *value = d.song && e.song_playing(d.song, now) ? MCI_MODE_PLAY_ : d.paused ? MCI_MODE_PAUSE_ : MCI_MODE_STOP_;
      return kOk;
    case 5:  // MCI_STATUS_MEDIA_PRESENT
    case 7:  // MCI_STATUS_READY
      *value = 1;
      return kOk;
    case 6:  // MCI_STATUS_TIME_FORMAT
      *value = d.time_format;
      return kOk;
    case 8:  // MCI_STATUS_CURRENT_TRACK
      *value = 1;
      return kOk;
    case 0x4003:  // MCI_SEQ_STATUS_PORT
      *value = MIDI_MAPPER_;
      return kOk;
    case 0x4007:  // MCI_SEQ_STATUS_DIVTYPE: MCI_SEQ_DIV_PPQN
      *value = 0;
      return kOk;
    default:
      trace("sound", "MCI_STATUS item 0x%X: not supported", item);
      return kMciUnsupportedFunction;
  }
}

uint32_t mci_command(Runtime& rt, uint32_t id, uint32_t msg, uint32_t flags, uint32_t p) {
  WinmmState& s = ws(rt);
  auto& mem = rt.mem();
  audio::Engine& e = audio_engine(rt);
  if (msg == MCI_OPEN) return mci_open(rt, flags, p);
  if ((flags & MCI_NOTIFY_) && !p) return kMciNullParameterBlock;
  if (id == MCI_ALL_DEVICE_ID_) {
    if (msg != MCI_CLOSE) return kMciCannotUseAll;
    for (auto& [i, d] : s.mci) mci_close(rt, i, d);
    s.mci.clear();
    if (flags & MCI_NOTIFY_) mci_notify(rt, mem.read_u32l(p), MCI_NOTIFY_SUCCESSFUL_, id);
    return kOk;
  }
  auto it = s.mci.find(id);
  if (it == s.mci.end()) return kMciInvalidDeviceId;
  MciDevice& d = it->second;
  audio::Time now = audio_now(rt);
  const uint32_t callback = p ? mem.read_u32l(p) : 0;
  // The command's own notification (not PLAY's, which waits for the end).
  auto done = [&](uint32_t err) {
    if (err == kOk && (flags & MCI_NOTIFY_) && msg != MCI_PLAY) {
      end_pending(rt, id, d, MCI_NOTIFY_SUPERSEDED_);
      mci_notify(rt, callback, MCI_NOTIFY_SUCCESSFUL_, id);
    }
    return err;
  };
  switch (msg) {
    case MCI_CLOSE: {
      mci_close(rt, id, d);
      s.mci.erase(it);
      if (flags & MCI_NOTIFY_) mci_notify(rt, callback, MCI_NOTIFY_SUCCESSFUL_, id);
      trace("sound", "MCI_CLOSE device %u", id);
      return kOk;
    }
    case MCI_PLAY: {
      if (flags & MCI_WAIT_) {
        trace("sound", "MCI_PLAY with MCI_WAIT: not supported (it would block the emulation)");
        return kMciUnsupportedFunction;
      }
      if (!d.song) return kMciUnsupportedFunction;
      if ((flags & (MCI_FROM_ | MCI_TO_)) && !p) return kMciNullParameterBlock;
      uint64_t length = e.song_length(d.song);
      uint64_t from = 0, to = 0;
      if (flags & MCI_FROM_) {
        from = uint64_t(mem.read_u32l(p + 4)) * 1000;
        if (from > length) return kMciOutOfRange;
      }
      if (flags & MCI_TO_) {
        to = uint64_t(mem.read_u32l(p + 8)) * 1000;
        uint64_t start = (flags & MCI_FROM_) ? from : e.song_position(d.song, now);
        if (to > length || to < start) return kMciOutOfRange;
      }
      end_pending(rt, id, d, MCI_NOTIFY_ABORTED_);
      if (flags & MCI_FROM_) e.song_seek(d.song, from, now);
      d.paused = false;
      d.has_to = (flags & MCI_TO_) != 0;
      d.to_us = to;
      if (flags & MCI_NOTIFY_) {
        d.notify_pending = true;
        d.notify_hwnd = callback;
      }
      e.song_play(d.song, now);
      trace("sound", "MCI_PLAY device %u%s", id, (flags & MCI_NOTIFY_) ? " notify" : "");
      return kOk;
    }
    case MCI_STOP:
    case MCI_PAUSE: {
      end_pending(rt, id, d, MCI_NOTIFY_ABORTED_);
      if (d.song) e.song_stop(d.song, now);
      d.has_to = false;
      d.paused = msg == MCI_PAUSE && d.song;
      return done(kOk);
    }
    case MCI_RESUME: {
      if (d.paused && d.song) e.song_play(d.song, now);
      d.paused = false;
      return done(kOk);
    }
    case MCI_SEEK: {
      if (!d.song) return kMciUnsupportedFunction;
      uint32_t how = flags & (MCI_SEEK_TO_START_ | MCI_SEEK_TO_END_ | MCI_TO_);
      if (!how) return kMciMissingParameter;
      if (how & (how - 1)) return kMciFlagsNotCompatible;
      uint64_t length = e.song_length(d.song), target = 0;
      if (how == MCI_SEEK_TO_END_) target = length;
      if (how == MCI_TO_) {
        if (!p) return kMciNullParameterBlock;
        target = uint64_t(mem.read_u32l(p + 4)) * 1000;
        if (target > length) return kMciOutOfRange;
      }
      end_pending(rt, id, d, MCI_NOTIFY_ABORTED_);
      e.song_seek(d.song, target, now);
      d.has_to = false;
      d.paused = false;
      return done(kOk);
    }
    case MCI_STATUS: {
      if (!p) return kMciNullParameterBlock;
      uint32_t v = 0;
      uint32_t err = mci_status(rt, d, flags, p, &v);
      if (err == kOk) mem.write_u32l(p + 4, v);
      return done(err);
    }
    case MCI_SET: {
      if (!p) return kMciNullParameterBlock;
      bool any = false;
      if (flags & MCI_SET_TIME_FORMAT_) {
        uint32_t f = mem.read_u32l(p + 4);
        if (f != MCI_FORMAT_MILLISECONDS_) return kMciBadTimeFormat;
        d.time_format = f;
        any = true;
      }
      if (flags & MCI_SEQ_SET_PORT_) {
        uint32_t port = mem.read_u32l(p + 16);
        if (port != MIDI_MAPPER_ && port != 0 && port != MCI_SEQ_NONE_) return kMciSeqPortNonexistent;
        any = true;
      }
      if (flags & ~(MCI_NOTIFY_ | MCI_WAIT_ | MCI_SET_TIME_FORMAT_ | MCI_SEQ_SET_PORT_)) {
        trace("sound", "MCI_SET flags 0x%X: not supported", flags);
        return kMciUnsupportedFunction;
      }
      return done(any ? kOk : kMciMissingParameter);
    }
    case MCI_GETDEVCAPS: {
      if (!p) return kMciNullParameterBlock;
      if (!(flags & MCI_GETDEVCAPS_ITEM_)) return kMciMissingParameter;
      uint32_t item = mem.read_u32l(p + 8), v = 0;
      switch (item) {
        case 1: v = 0; break;                        // CAN_RECORD
        case 2: v = 1; break;                        // HAS_AUDIO
        case 3: v = 0; break;                        // HAS_VIDEO
        case 4: v = MCI_DEVTYPE_SEQUENCER_; break;   // DEVICE_TYPE
        case 5: v = 1; break;                        // USES_FILES
        case 6: v = 1; break;                        // COMPOUND_DEVICE
        case 7: v = 0; break;                        // CAN_EJECT
        case 8: v = 1; break;                        // CAN_PLAY
        case 9: v = 0; break;                        // CAN_SAVE
        default: return kMciUnsupportedFunction;
      }
      mem.write_u32l(p + 4, v);
      return done(kOk);
    }
    case MCI_INFO: {
      if (!p) return kMciNullParameterBlock;
      uint32_t buf = mem.read_u32l(p + 4), cap = mem.read_u32l(p + 8);
      std::string text;
      if (flags & MCI_INFO_PRODUCT_) text = "Long After Dark MIDI Sequencer";
      else if (flags & MCI_INFO_FILE_) text = d.element;
      else return kMciMissingParameter;
      if (buf && cap) write_cstr(mem, buf, text, cap);
      return done(kOk);
    }
    default:
      trace("sound", "mciSendCommand(device %u, msg 0x%X): not recognized", id, msg);
      return kMciUnrecognizedCommand;
  }
}

// ---- waveOut ------------------------------------------------------------------------------------------

void write_wave_caps(Runtime& rt, uint32_t p, uint32_t cap) {
  // WAVEOUTCAPSA: wMid, wPid, vDriverVersion, szPname[32], dwFormats, wChannels, wReserved1, dwSupport.
  uint8_t b[52] = {};
  b[0] = 1;       // MM_MICROSOFT
  b[2] = 2;       // MM_WAVE_MAPPER
  b[4] = 0x00;    // vDriverVersion 4.0
  b[5] = 0x04;
  const char name[] = "Long After Dark";
  memcpy(b + 8, name, sizeof(name));
  uint32_t formats = 0xFFF, support = 0x4 | 0x8 | 0x20;  // VOLUME | LRVOLUME | SAMPLEACCURATE (no SYNC)
  memcpy(b + 40, &formats, 4);
  b[44] = 2;  // wChannels
  memcpy(b + 48, &support, 4);
  rt.mem().memcpy(p, b, std::min<uint32_t>(cap, sizeof(b)));
}

void register_waveout(ShimRegistry& r) {
  r.impl(W, "waveOutGetNumDevs", [](Call& c) {
    entry(c.rt);
    c.ret(audio_enabled(c.rt) ? 1 : 0);
  });
  r.impl(W, "waveOutGetDevCapsA", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) return c.ret(kBadDeviceId);
    if (!is_wave_device(ws(c.rt), c.arg(0))) return c.ret(kBadDeviceId);
    if (!c.arg(1)) return c.ret(kInvalidParam);
    write_wave_caps(c.rt, c.arg(1), c.arg(2));
    c.ret(kOk);
  });
  r.impl(W, "waveOutOpen", [](Call& c) {
    entry(c.rt);
    // (phwo, uDeviceID, pwfx, dwCallback, dwInstance, fdwOpen)
    if (!audio_enabled(c.rt)) {
      if (c.arg(0)) c.mem().write_u32l(c.arg(0), 0);
      trace("sound", "waveOutOpen(device %d) refused: no wave output devices", c.iarg(1));
      return c.ret(kNoDriver);
    }
    WinmmState& s = ws(c.rt);
    uint32_t dev = c.arg(1), flags = c.arg(5);
    bool query = flags & WAVE_FORMAT_QUERY_;
    if (!query && c.arg(0)) c.mem().write_u32l(c.arg(0), 0);
    if (dev != 0 && dev != WAVE_MAPPER_) return c.ret(kBadDeviceId);
    if (!c.arg(2)) return c.ret(kInvalidParam);
    audio::WaveFormat f;
    if (!read_guest_waveformat(c.rt, c.arg(2), f) || !audio::playable(f)) {
      trace("sound", "waveOutOpen: format tag 0x%X is not playable PCM", c.mem().read_u16l(c.arg(2)));
      return c.ret(kBadFormat);
    }
    if (query) return c.ret(kOk);
    uint32_t type = flags & CALLBACK_TYPEMASK_;
    if (type != CALLBACK_NULL_ && type != CALLBACK_WINDOW_ && type != CALLBACK_FUNCTION_ && type != CALLBACK_EVENT_)
      return c.ret(type == CALLBACK_THREAD_ ? kNotSupported : kInvalidFlag);
    if (!c.arg(0)) return c.ret(kInvalidParam);
    audio::StreamId stream = audio_engine(c.rt).open_stream(f, audio::Bus::wave, audio_now(c.rt));
    if (!stream) return c.ret(kBadFormat);
    WaveOut w;
    w.handle = kWaveHandleBase + s.next_wave++;
    w.stream = stream;
    w.format = f;
    w.callback_type = type;
    w.callback = c.arg(3);
    w.instance = c.arg(4);
    uint32_t h = w.handle;
    s.waves[h] = w;
    c.mem().write_u32l(c.arg(0), h);
    trace("sound", "waveOutOpen(%u Hz %u-bit %s) -> 0x%X", f.rate, f.bits, f.channels == 2 ? "stereo" : "mono", h);
    wave_callback(c.rt, s.waves[h], MM_WOM_OPEN_, 0);
    c.ret(kOk);
  });
  r.impl(W, "waveOutClose", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) return c.ret(kInvalidHandle);
    WinmmState& s = ws(c.rt);
    WaveOut* w = wave_by_handle(s, c.arg(0));
    if (!w) return c.ret(kInvalidHandle);
    if (!w->queued.empty()) return c.ret(kStillPlaying);
    audio_engine(c.rt).close_stream(w->stream, audio_now(c.rt));
    WaveOut closing = *w;
    s.waves.erase(c.arg(0));
    wave_callback(c.rt, closing, MM_WOM_CLOSE_, 0);
    trace("sound", "waveOutClose(0x%X)", c.arg(0));
    c.ret(kOk);
  });
  r.impl(W, "waveOutPrepareHeader", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) return c.ret(kInvalidHandle);
    if (!wave_by_handle(ws(c.rt), c.arg(0))) return c.ret(kInvalidHandle);
    uint32_t hdr = c.arg(1);
    if (!hdr || c.arg(2) < kWaveHdrSize) return c.ret(kInvalidParam);
    uint32_t flags = c.mem().read_u32l(hdr + 16);
    if (flags & WHDR_INQUEUE_) return c.ret(kStillPlaying);
    c.mem().write_u32l(hdr + 16, flags | WHDR_PREPARED_);
    c.ret(kOk);
  });
  r.impl(W, "waveOutUnprepareHeader", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) return c.ret(kInvalidHandle);
    if (!wave_by_handle(ws(c.rt), c.arg(0))) return c.ret(kInvalidHandle);
    uint32_t hdr = c.arg(1);
    if (!hdr || c.arg(2) < kWaveHdrSize) return c.ret(kInvalidParam);
    uint32_t flags = c.mem().read_u32l(hdr + 16);
    if (flags & WHDR_INQUEUE_) return c.ret(kStillPlaying);
    c.mem().write_u32l(hdr + 16, flags & ~WHDR_PREPARED_);
    c.ret(kOk);
  });
  r.impl(W, "waveOutWrite", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) return c.ret(kInvalidHandle);
    WaveOut* w = wave_by_handle(ws(c.rt), c.arg(0));
    if (!w) return c.ret(kInvalidHandle);
    uint32_t hdr = c.arg(1);
    if (!hdr || c.arg(2) < kWaveHdrSize) return c.ret(kInvalidParam);
    auto& mem = c.mem();
    uint32_t flags = mem.read_u32l(hdr + 16);
    if (!(flags & WHDR_PREPARED_)) return c.ret(kUnprepared);
    if (flags & WHDR_INQUEUE_) return c.ret(kStillPlaying);
    if (flags & (WHDR_BEGINLOOP_ | WHDR_ENDLOOP_))
      trace("sound", "waveOutWrite(0x%X): WHDR_BEGINLOOP/ENDLOOP played once", c.arg(0));
    uint32_t data = mem.read_u32l(hdr + 0), len = mem.read_u32l(hdr + 4);
    std::span<const uint8_t> bytes;  // a bad lpData faults here, before anything is queued
    if (len) bytes = std::span<const uint8_t>(mem.at<uint8_t>(data, len), len);
    mem.write_u32l(hdr + 16, (flags | WHDR_INQUEUE_) & ~WHDR_DONE_);
    w->queued.push_back(hdr);
    audio_engine(c.rt).stream_write(w->stream, bytes, hdr, audio_now(c.rt));
    c.ret(kOk);
  });
  r.impl(W, "waveOutPause", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) return c.ret(kInvalidHandle);
    WaveOut* w = wave_by_handle(ws(c.rt), c.arg(0));
    if (!w) return c.ret(kInvalidHandle);
    audio_engine(c.rt).stream_pause(w->stream, audio_now(c.rt));
    c.ret(kOk);
  });
  r.impl(W, "waveOutRestart", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) return c.ret(kInvalidHandle);
    WaveOut* w = wave_by_handle(ws(c.rt), c.arg(0));
    if (!w) return c.ret(kInvalidHandle);
    audio_engine(c.rt).stream_restart(w->stream, audio_now(c.rt));
    c.ret(kOk);
  });
  r.impl(W, "waveOutReset", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) return c.ret(kInvalidHandle);
    WaveOut* w = wave_by_handle(ws(c.rt), c.arg(0));
    if (!w) return c.ret(kInvalidHandle);
    audio::Time now = audio_now(c.rt);
    audio_engine(c.rt).stream_reset(w->stream, now);
    // Every queued header comes back done before the call returns.
    apply_events(c.rt, now);
    c.ret(kOk);
  });
  r.impl(W, "waveOutGetPosition", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) return c.ret(kInvalidHandle);
    WaveOut* w = wave_by_handle(ws(c.rt), c.arg(0));
    if (!w) return c.ret(kInvalidHandle);
    uint32_t p = c.arg(1);
    if (!p || c.arg(2) < 8) return c.ret(kInvalidParam);
    uint64_t bytes = audio_engine(c.rt).stream_position(w->stream, audio_now(c.rt));
    uint32_t type = c.mem().read_u32l(p), v;
    if (type == TIME_SAMPLES_) {
      v = uint32_t(bytes / std::max<uint32_t>(w->format.block_align, 1));
    } else if (type == TIME_MS_) {
      v = uint32_t(bytes * 1000 / std::max<uint32_t>(w->format.avg_bytes, 1));
    } else {
      type = TIME_BYTES_;
      v = uint32_t(bytes);
    }
    c.mem().write_u32l(p, type);
    c.mem().write_u32l(p + 4, v);
    c.ret(kOk);
  });
  r.impl(W, "waveOutSetVolume", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) return c.ret(kInvalidHandle);
    WinmmState& s = ws(c.rt);
    if (!is_wave_device(s, c.arg(0))) return c.ret(c.arg(0) < kWaveHandleBase ? kBadDeviceId : kInvalidHandle);
    s.wave_volume = c.arg(1);
    audio_engine(c.rt).set_bus_gain(audio::Bus::wave, audio::gain_from_mm(c.arg(1)), audio_now(c.rt));
    c.ret(kOk);
  });
  r.impl(W, "waveOutGetVolume", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) return c.ret(kInvalidHandle);
    WinmmState& s = ws(c.rt);
    if (!is_wave_device(s, c.arg(0))) return c.ret(c.arg(0) < kWaveHandleBase ? kBadDeviceId : kInvalidHandle);
    if (!c.arg(1)) return c.ret(kInvalidParam);
    c.mem().write_u32l(c.arg(1), s.wave_volume);
    c.ret(kOk);
  });
}

}  // namespace

void audio_pump(Runtime& rt) {
  if (!audio_enabled(rt)) return;
  WinmmState& s = ws(rt);
  if (s.pumping) return;  // a callback's own WINMM call: nothing is delivered re-entrantly
  s.pumping = true;
  struct Reset {
    bool& flag;
    ~Reset() { flag = false; }
  } reset{s.pumping};
  apply_events(rt, audio_now(rt));
  run_deferred(rt);
}

void winmm_end_process(Runtime& rt) {
  WinmmState& s = ws(rt);
  audio::Engine& e = audio_engine(rt);
  for (auto& [h, w] : s.waves) e.close_stream(w.stream, audio_now(rt));
  s.waves.clear();
  for (auto& [id, d] : s.mci)
    if (d.song) e.close_song(d.song, audio_now(rt));
  s.mci.clear();
  s.deferred.clear();
}

void register_winmm(ShimRegistry& r) {
  r.impl(W, "timeGetTime", [](Call& c) {
    entry(c.rt);
    c.ret(c.rt.clock().read_tick_count());
  });

  r.impl(W, "mciSendCommandA", [](Call& c) {
    entry(c.rt);
    if (!audio_enabled(c.rt)) {
      trace("sound", "mciSendCommand(device 0x%X, msg 0x%X) refused: no MCI devices", c.arg(0), c.arg(1));
      return c.ret(kMciDeviceNotInstalled);
    }
    uint32_t err = mci_command(c.rt, c.arg(0), c.arg(1), c.arg(2), c.arg(3));
    if (err) trace("sound", "mciSendCommand(device %u, msg 0x%X, flags 0x%X) -> %u", c.arg(0), c.arg(1), c.arg(2), err);
    c.ret(err);
  });
  // CD audio (XCdAudio: "open cdaudio alias qwanza wait"): there is no disc.
  mm(r, "mciSendStringA", [](Call& c) {
    trace("sound", "mciSendString(\"%s\") refused: no MCI devices", c.str(0).c_str());
    if (c.arg(1) && c.arg(2)) c.mem().write_u8(c.arg(1), 0);
    c.ret(kMciDeviceNotInstalled);
  });
  mm(r, "mciGetErrorStringA", [](Call& c) {
    if (!audio_enabled(c.rt)) {
      if (c.arg(1) && c.arg(2)) write_cstr(c.mem(), c.arg(1), "The specified device is not installed.", c.arg(2));
      return c.ret(1);
    }
    const char* text = mci_error_text(c.arg(0));
    if (c.arg(1) && c.arg(2)) write_cstr(c.mem(), c.arg(1), text ? text : "", c.arg(2));
    c.ret(text ? 1 : 0);
  });

  // aux: 0 = CD audio (stored), 1 = the MIDI synth (the engine's MIDI bus).
  mm(r, "auxGetNumDevs", [](Call& c) { c.ret(audio_enabled(c.rt) ? 2 : 0); });
  mm(r, "auxGetVolume", [](Call& c) {
    if (!audio_enabled(c.rt)) return c.ret(kNoDriver);
    if (c.arg(0) > 1) return c.ret(kBadDeviceId);
    if (!c.arg(1)) return c.ret(kInvalidParam);
    c.mem().write_u32l(c.arg(1), ws(c.rt).aux_volume[c.arg(0)]);
    c.ret(kOk);
  });
  mm(r, "auxSetVolume", [](Call& c) {
    if (!audio_enabled(c.rt)) return c.ret(kNoDriver);
    if (c.arg(0) > 1) return c.ret(kBadDeviceId);
    WinmmState& s = ws(c.rt);
    uint32_t v = c.arg(1);
    // The engine sets it every GiveTime (sub_426fc1); only a change reaches the bus.
    if (c.arg(0) == 1 && v != s.aux_volume[1])
      audio_engine(c.rt).set_bus_gain(audio::Bus::midi, audio::gain_from_mm(v), audio_now(c.rt));
    s.aux_volume[c.arg(0)] = v;
    c.ret(kOk);
  });
  // No mixer, sound on or off: the engine then keeps per-buffer volume (AUDIO.md §2.4).
  mm(r, "mixerGetLineInfoA", [](Call& c) { c.ret(kNoDriver); });

  // Wave input: POINTS and SWIRLING's "music" mode listens to the line-in; there is none.
  mm(r, "waveInGetNumDevs", [](Call& c) { c.ret(0); });
  for (const char* n : {"waveInOpen", "waveInClose", "waveInAddBuffer", "waveInPrepareHeader", "waveInUnprepareHeader",
                        "waveInReset", "waveInStart"}) {
    mm(r, n, [](Call& c) { c.ret(kNoDriver); });
  }
  mm(r, "waveInGetErrorTextA", [](Call& c) {
    if (c.arg(1) && c.arg(2)) write_cstr(c.mem(), c.arg(1), "No audio input device is installed.", c.arg(2));
    c.ret(0);
  });

  // Wave output (HALLOFFA.AD's JSound). Sound off: no devices, so no handle is
  // ever opened; opening answers "no driver", capabilities of a device that
  // does not exist "bad device id", everything that takes a handle "invalid
  // handle", and the module plays nothing, as on a PC without a sound card.
  register_waveout(r);
}

}  // namespace adw::win32
