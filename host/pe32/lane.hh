// adw_lane_pe32 — the AD4 lane: a Windows After Dark 4 module (a PE32 .AD)
// with the real ADXPL510.DLL engine, run on the Win32 guest runtime
// (host/win32) and driven the way AFTERDAR.SCR drove it (ABI.md §2).
//
//   init:  map the module (the engine comes in through its imports), deliver
//          DLL_PROCESS_ATTACH engine-first, find _Module@4 / Module, build the
//          348-byte AD_MODULE32 block, send SELECTED (0) → PREINITIALIZE (2) →
//          BLANK (3), each only if the previous returned 0.
//   step:  queued KEY/MOUSE messages, then DRAWFRAME (4); after the first
//          frame one PAINT (9) — the saver window's initial expose.
//   close: CLOSE (5), DESELECTED (1), DLL_PROCESS_DETACH, census.
//
// Whole programs (program.cc, ABI.md §2.15, the "desktoptoys" ABI): a PE32
// EXE — every After Dark module is a DLL — is run as the emulated machine's
// one Win32 process: Cyclops' Desktop Toys engine (Screen Wreckers'
// DTTY3203.EXE, MFC 4.2) with the command line TOYSAV32.SCR gave it,
// "C:\DTToys\dtty3203.exe /s /t<ADTOY-1>", in its ghost mode. The program
// owns its message loop; every frame ends inside it (an API call or a slice
// of code past the frame's share of the modelled machine, or its
// GetMessageA's wait) and the next one resumes it. Its registry, disk and
// window manager: program.cc. Caps Lock (not with ADWALLPAPER=1) starts the
// program again for play, "C:\DTToys\dtty3203.exe /t<ADTOY-1>" as the toy
// selector started it, with the saver's keys and mouse as its window
// messages (interactive, cursor); Caps Lock off, or play ending by itself,
// starts it again in ghost mode (program.cc "Caps Lock").
//
// Return values (ABI.md §2.5): 0 = ok; 3 = restart (the host re-sends 2 then
// 3 and uses that result); anything else is fatal, with the module's error
// text at block +0x50. The same code runs the MSVC-built STARRYNI.AD
// (_Module@4, no engine) and PSYCHO.AD (no engine), and After Dark 10th
// Anniversary's HALLOFFA.AD (the AD Online framework, Borland, no engine) and
// its two MSVC "Toasters 2k" builds.
//
// Where the engine comes from (package.hh, PACKAGES.md §7.2): a module under
// <win>\packages\<id>\<dir>\ searches its own folder, then <id>\ENGINE, and
// nothing else — ad10's ADXPL510.DLL 5.2 sits beside its modules. Any other
// module (Deluxe's FILES\AD40, a lone module) searches its folder, then
// <win>\FILES\AD40, as it always has. ADTRACE=lane logs the package, both
// folders and the search list in one line at init.
//
// The guest's disk (INTERACTION.md §7): C:\WINDOWS (an overlay with no lower
// files) and C:\AFTERDRK (the module's folder; the current directory, so the
// long-name music the importer's fix-ups add, Music\Toasters2k.mid, …,
// resolves there) both write to an upper layer: the package's
// <ADSTATE>\<pkg>\WINDOWS and <ADSTATE>\<pkg>\<MODDIR> (AD40, AD10TH, …), or
// memory when ADSTATE is unset (headless runs, censuses and FBHASH never see
// or write user state). C:\PICTURES (<module dir>\PICTURES, Art Critic's
// samples) and H:\<L>\… (the host's drives, for paths picked in configure
// mode) are read-only. The engine's WriteModulePrefs lands in MODULES.INI in
// the upper C:\WINDOWS.
//
// Interaction (INTERACTION.md §5.1): while the module wants events (block
// +0x08 bit 0, set by UserInput::SetMode when a game starts), each KEY line
// becomes message 7/8 and each MOUSE line 30 (moved) / 21 / 27 (left button
// down / up), with 24 once per step while the left button is held, all with
// MAKELONG(x, y); every such line counts as consumed (status().eaten = its
// seq). The right and middle buttons are polled (GetAsyncKeyState from the
// MOUSE bitmask). status(): interactive = bit 0, cursor = bit 1, rotate-ok =
// bit 2, source 1 (AD4) while interactive, wake when the module posted
// WM_CLOSE / SC_CLOSE to the saver window. ADCAPS gives the Caps Lock toggle
// the engine latches at PREINITIALIZE (GetCapsLockChange reacts to changes).
// NUMLOCK lines and ADNUMLOCK (Num Lock's toggle, for the Classic lane's
// Final Exam) change nothing here: the Win32 GetKeyState reports Caps Lock's
// toggle alone.
//
// Configure mode (adhostwin --configure, §6.1): ADPAGE's button sequence
// (0x9001fbf) on a fresh zeroed block with cbSize, +0x0C = the --owner as a
// guest handle, +0x14 = hModule, +0x3C = 50 and +0x40 = the control values
// (ADCVSET, else the module's defaults): Module(0), Module(6, slot),
// Module(1), each only if the previous returned 0; then DLL_PROCESS_DETACH.
// The module's dialogs, message boxes and file dialogs are real
// (win32/realui.hh), owned by the --owner window; the state overlay is
// persistent. Result: shown (a dialog, message box or file dialog appeared),
// nothing, or failed (a module error, a guest fault, a scripted dialog that
// timed out). ADCONFIGSCRIPT / ADCONFIGHIDDEN / ADCONFIGDUMP /
// ADCONFIGTIMEOUTMS drive it in tests (win32/config_script.hh).
//
// Small screens (§9.2): the modules were written for a 640x480 (or larger)
// Win95 display, and below it some refuse ("This Module needs a larger screen
// area to run!" — CYBER; "requires a minimum resolution of 510 x 342" —
// CRITIC). So an output smaller than 640x480 in either dimension (the saver's
// 320x240 /p preview) gets a guest display k times its size, the smallest
// whole k that reaches 640x480 (at most 8); the block's client rect,
// GetSystemMetrics and GetDeviceCaps all see the guest size, and every
// presented frame is that display averaged k×k → 1 and matched to the
// nearest hardware palette entry (6 bits per channel, lowest index on ties):
// a miniature of the full-screen saver. Mouse coordinates scale up to match.
// Deterministic; outputs of 640x480 and larger are untouched.
//
// Timing: the original loop never slept and most modules pace themselves from
// timeGetTime (ABI.md §2.7). This lane presents 60 frames per second of
// virtual time (frame_interval_us; ADPACEMS overrides), and each clock read
// advances time by ADREADSTEPUS microseconds (default 5) so deadline loops
// inside one call end. AFTERDAR.SCR called DRAWFRAME back to back as fast as
// the machine allowed, and modules that count calls (PSYCHO builds its next
// pattern 80 SetPixels per call) or redraw on every timer tick (SWIRLING,
// SLOWBURN) ran at that machine's speed. So every presented frame adds one
// frame period of a virtual ADMIPS machine (100 million instruction-
// equivalents a second, a P5-100..133) to a work account, and DRAWFRAMEs run
// back to back while it is positive, at most ADMAXDRAWS a frame. A call
// costs its instructions, ADAPICOST per API call (Win95's thunk layer) and
// ADPIXCOST per pixel a blit or fill writes (BitBlt, StretchBlt, PatBlt,
// SetDIBitsToDevice, StretchDIBits, SetDIBits, FillRect: 4 = 25 Mpixel/s, a
// PCI card of 1996; win32::Runtime::charge_pixels). Pixels count at 480 lines:
// on a taller guest display (the saver's Sharp/Custom scales) each costs
// (480/h)^2 as much, so a full-screen blit costs the same at every Scale and
// the setting changes sharpness, not speed. Deterministic headless.
//
// Long calls: a DRAWFRAME that does more than the account holds (SWIRLING's
// three full-screen blits, SLOWBURN's burn, PSYCHO's first call painting its
// whole pattern) runs on a fiber of its own, and at its first API call past
// the frame's share — or, streamed, past 90% of the period of wall time,
// when the host is slower than the modelled machine — the frame ends there:
// it is presented as the call left the screen, and the next frame resumes
// the call where it stopped, as a 1996 monitor showed a long call's drawing
// while it happened. Work a call does without an API call leaves a debt the
// next frames pay off before calling again (at most 30 frames' worth),
// presenting the screen as it stands. Queued KEY/MOUSE messages reach the
// module between two calls, never inside a suspended one. At close a
// suspended call is abandoned (the call hook throws win32::AbandonGuestCall
// and every call level restores the registers and the SEH chain) before
// CLOSE. Measured: SWIRLING ~16-25 DRAWFRAMEs per virtual second (was
// ~3,700), SLOWBURN ~14 once burning (was 60). ADMIPS=0 restores one
// DRAWFRAME per presented frame; ADPE32LONGCALLS=0 ends every frame at a
// call's end. ADTRACE=pace logs each frame's calls, work and account.
//
// Sound (docs/AUDIO.md §7): the host's audio engine (LaneContext::
// audio; audio::null_engine() without one) is attached to the runtime before
// the module loads (win32/audio.hh). Disabled, every sound API answers as the
// silent host always did: dsound.dll is not found, MCI, aux and waveOut have
// no devices, ACM no driver. Enabled, the block gets +0x04 |= kSoundOn and
// +0x3C = the engine's volume, dsound.dll loads (DirectSound over the
// engine), ACM decodes IMA-ADPCM, MCI plays MIDI files, aux 1 is the MIDI
// volume and waveOut streams (HALLOFFA); a Deluxe module also finds its
// music under the long names it asks for (Music\Flying Toasters.mid →
// TOASTERS.MID, §7.7). Finished waveOut chunks and song ends are applied
// before every Module() call. Configure mode never enables sound.
//
// Lane knobs (env, all optional):
//   ADSOUND=1          guest sound on (read by the host's engine, AUDIO.md §4;
//                      ADAUDIOOUT=<file.wav> also turns it on and captures it).
//                      Without an engine (a lane driven outside adhostwin) it
//                      only sets the "sound on" flag, and the engine then fails
//                      politely, as it always did
//   ADVOLUME=0..100    After Dark's volume slider, handed to the module at +0x3C
//                      while sound is on (default 50, the registry default; 0
//                      puts the engine in its silent timer-only mode). Sound
//                      off, +0x3C stays 50
//   ADPAINT=0          do not send the initial PAINT
//   ADREADSTEPUS=<us>  virtual µs per clock read (default 5)
//   ADCALLBUDGET=<n>   instructions one Module()/DllMain call may run before it
//                      counts as hung (default 1e9)
//   ADHEAPMB=<n>       guest heap arena size (default 128)
//   ADMIPS=<n>         virtual machine speed for the DRAWFRAME loop, million
//                      instruction-equivalents per second (default 100; 0 =
//                      one DRAWFRAME per presented frame)
//   ADAPICOST=<n>      instruction-equivalents charged per API call (default 500)
//   ADPIXCOST=<n>      instruction-equivalents charged per pixel a blit or fill
//                      writes, at 480 lines (default 4)
//   ADPE32LONGCALLS=0  no long calls: every frame ends at the end of a DRAWFRAME
//   ADMAXDRAWS=<n>     DRAWFRAME calls per presented frame, at most (default 64)
//   ADPE32SCALE=auto|<k>  the guest display is k times the output (default auto;
//                      see Small screens; 1 = off)
//   ADTOY=1..9         a Desktop Toys program's toy (program.cc; the catalog
//                      sets it per module); unset = the registry's ScrToy, 3
//   ADSEEDIMG=<spec>   what the screen holds before the first message
//                      (win32/display.hh "desktop seed"): ":win95"
//                      (teal) or a raw/P6/BMP file of any size (the .scr's
//                      delete-on-close capture included). Unset = black.
//                      Modules that keep the screen (SHADOW with Clear Screen
//                      First off, BADDOG, …) draw over it; SLOWBURN blanks it
//                      first (PACKAGES.md §12 lists who uses it). ADNOSEED=1
//                      ignores it.
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "adw/core/lane.h"

namespace adw::win32 {
class Runtime;
struct Module;
}  // namespace adw::win32

namespace adw::pe32 {

// AD_MODULE32 (ABI.md §2.3).
namespace block {
constexpr uint32_t kSize = 0x15C;
constexpr uint32_t kCbSize = 0x000, kFlags = 0x004, kModuleFlags = 0x008, kOwner = 0x00C, kHwnd = 0x010;
constexpr uint32_t kHModule = 0x014, kHdc = 0x018, kPalettes = 0x01C, kClient = 0x02C, kVolume = 0x03C;
constexpr uint32_t kControls = 0x040, kError = 0x050, kErrorSize = 260, kMessage = 0x154, kParam = 0x158;
// +0x04 host → module.
constexpr uint32_t kPalettized = 0x01, kSoundOn = 0x02, kRandomizer = 0x04, kMultiModule = 0x08, kNotDemo = 0x10;
// +0x08 module → host.
constexpr uint32_t kWantsEvents = 0x01, kWantsCursor = 0x02, kRotateOk = 0x04;
}  // namespace block

// Module() messages (ABI.md §2.5).
namespace msg {
constexpr uint32_t kSelected = 0, kDeselected = 1, kPreinitialize = 2, kBlank = 3, kDrawFrame = 4, kClose = 5;
constexpr uint32_t kButton = 6, kKeyDown = 7, kKeyUp = 8, kPaint = 9;
constexpr uint32_t kLButtonDown = 21, kLButtonHeld = 24, kLButtonUp = 27, kMouseMove = 30;
constexpr uint32_t kRestart = 3;  // a return value
}  // namespace msg

class Pe32Lane : public Lane {
 public:
  Pe32Lane();
  ~Pe32Lane() override;

  const char* name() const override { return "pe32"; }
  // --capabilities (core lane.h): After Dark 4's modules, and a toy of
  // Cyclops' Desktop Toys engine (a whole Win32 program: program.cc).
  std::vector<std::string> abis() const override { return {"afterdark", "desktoptoys"}; }
  bool init(const std::string& module_path, LaneContext& ctx) override;
  uint32_t frame_interval_us() const override { return 16667; }
  void on_command(const Command& c) override;
  StepResult step() override;
  void shutdown() override;
  LaneStatus status() const override;
  bool can_configure() const override { return true; }
  ConfigureResult configure(const std::string& module_path, LaneContext& ctx, const ConfigureRequest& req,
                            std::string* json_out) override;

  // For tests and diagnostics.
  win32::Runtime* runtime() { return rt_.get(); }
  uint32_t block_address() const { return block_; }
  uint64_t frames() const { return frames_; }
  int guest_scale() const { return guest_scale_; }
  bool program() const { return program_; }

  // Whether the file is a PE32 program (an EXE: IMAGE_FILE_DLL clear), which
  // this lane runs as a whole process (program.cc), not as a module DLL.
  static bool is_program_file(const std::string& path);
  // The Desktop Toys command line for toy n (1..9): the screen saver's, as
  // TOYSAV32.SCR built it ("<dir>\dtty3203.exe /s /t<n-1>"), or, for play,
  // the toy selector's (TOYSEL32.EXE: "<dir>\dtty3203.exe /t<n-1>");
  // program.cc "Command line", "Caps Lock".
  static std::string desktop_toys_command_line(const std::string& install_dir, int volume, int toy,
                                               bool screen_saver = true);
  // Whether the running program was started for play (Caps Lock; program.cc).
  bool playing() const { return playing_; }

  // The whole-number factor between the guest display and a w×h output
  // ("Small screens"): the smallest k with k*w >= 640 and k*h >= 480, at most 8.
  static int auto_guest_scale(int w, int h);
  // The small-screen presenter: `in` (k times `out`'s size) averaged k×k → 1
  // into `out`, each average matched to the nearest entry of in's palette
  // (6 bits per channel, lowest index on ties); a cell of one index keeps it.
  // `cache`/`gen`/`pal` carry the nearest-colour cache between calls.
  struct NearCache {
    std::array<RGBQUAD, 256> pal{};
    std::vector<uint32_t> slots;  // (generation << 8) | index, by 18-bit colour key
    uint32_t gen = 0;
  };
  static void downsample(const Screen& in, Screen& out, int k, NearCache& cache);

 private:
  bool init_impl(const std::string& module_path, LaneContext& ctx);
  // Sends one message; returns Module()'s result.
  uint32_t send(uint32_t message, uint32_t param = 0);
  // Sends `message` and applies the host's result rules; false = fatal (logged).
  bool deliver(uint32_t message, uint32_t param = 0);
  // PREINITIALIZE then BLANK (the start and every restart).
  bool start();
  // The host's result rules for a Module() result; false = fatal (logged).
  bool apply(uint32_t message, uint32_t result);
  // One DRAWFRAME (and the initial PAINT after the first); false = fatal (logged).
  bool draw_once();
  // What blits and fills writing `pixels` cost against the budget.
  uint64_t pixel_work(uint64_t pixels) const;
  // Queued input messages (and LBUTTONHELD once a frame) between two calls.
  bool pump_input();
  // The work done since the frame began, and whether the frame is over.
  uint64_t frame_work() const;
  bool frame_over() const;
  // The frame's run of DRAWFRAMEs; false = the module failed.
  bool draw_run();
  // Long calls.
  static void CALLBACK fiber_main(void* self);
  void fiber_body();
  void on_api_call();
  void make_fibers();
  void abandon_long_call();
  void free_fibers();
  std::string error_text() const;
  void census() const;
  // Builds the runtime, the disk, the display and the DLL search path
  // (shared by init and configure). False (logged) when the runtime cannot be made.
  void make_runtime(const std::string& path, LaneContext& ctx, Screen& display);
  // Control values for +0x40: ADCVSET/SET, else the module's defaults.
  void write_controls();
  // Small screens: the guest's view of the input (mouse scaled up).
  void sync_input();
  int32_t guest_x(int32_t x) const;
  int32_t guest_y(int32_t y) const;

  std::unique_ptr<win32::Runtime> rt_;
  LaneContext* ctx_ = nullptr;
  std::string module_name_;
  uint32_t entry_ = 0, block_ = 0;
  win32::Module* module_ = nullptr;
  bool selected_ = false, blanked_ = false, painted_ = false, send_paint_ = true, census_done_ = false;
  uint64_t frames_ = 0;
  // The 1996 host called DRAWFRAME back to back, never sleeping (ABI.md §2.7),
  // and modules that count calls instead of reading the clock (PSYCHO's
  // pattern builder, STARRYNI's windows) ran at the machine's speed. We model
  // that machine deterministically ("Timing"): each presented frame keeps
  // calling DRAWFRAME while the work account — a virtual CPU's frame period
  // less the instructions, API calls and pixels charged — is positive, or
  // until max_draws_ calls. ADMIPS=0 restores one call per frame.
  uint64_t mips_ = 0;          // virtual machine speed, million instruction-equivalents per second
  uint64_t api_cost_ = 0;      // instruction-equivalents charged per shim call
  uint64_t pix_cost_ = 0;      // instruction-equivalents charged per pixel a blit or fill writes
  uint32_t max_draws_ = 1;     // DRAWFRAME calls per presented frame, at most
  // The work account (lane.hh "Timing"): instruction-equivalents the frames
  // so far left over (at most one period's worth) or owe (a DRAWFRAME that did
  // more than its frame held), at most kMaxDebtFrames periods' worth.
  int64_t credit_ = 0;
  static constexpr int64_t kMaxDebtFrames = 30;
  // This frame: the counters when it began, its share of the account, its
  // wall-clock deadline (streamed; 0 headless) and the DRAWFRAMEs it ran.
  uint64_t i0_ = 0, a0_ = 0, p0_ = 0, frame_credit_ = 0, wall_deadline_us_ = 0;
  uint32_t frame_draws_ = 0;
  // Long calls: the frames' DRAWFRAMEs run on guest_fiber_; at the first API
  // call past the frame's end inside one it switches back (suspended_), and
  // the next step() resumes it.
  enum class Run { none, frame_done, suspended, failed, abandoned, error };
  bool long_calls_ = false, mid_call_ = false, suspended_ = false, abandon_ = false, converted_thread_ = false;
  void* host_fiber_ = nullptr;
  void* guest_fiber_ = nullptr;
  Run run_result_ = Run::none;
  std::exception_ptr fiber_error_;
  uint64_t long_frames_ = 0;  // frames that ended inside a DRAWFRAME (diagnostics)
  uint64_t draws_ = 0;         // DRAWFRAME calls so far (diagnostics)
  // Messages from KEY/MOUSE lines, delivered before the next DRAWFRAME, with
  // the input line each came from.
  struct Pending {
    uint32_t message, param;
    uint64_t seq;
  };
  std::deque<Pending> pending_;
  uint64_t eaten_ = 0;  // highest input seq delivered to the module as its own
  int32_t last_mouse_x_ = -1, last_mouse_y_ = -1;
  bool mouse_down_ = false;
  bool held_due_ = false;  // LBUTTONHELD not yet sent this frame
  // Small screens: the guest display (guest_scale_ times the output) and the
  // input the guest sees.
  int guest_scale_ = 1;
  std::unique_ptr<Screen> guest_screen_;
  InputState guest_input_;
  NearCache near_;

  // ---- a whole program (program.cc, ABI.md §2.15) ----
  bool init_program(const std::string& path, LaneContext& ctx);
  // The guest fiber's body: the program's entry point, run to its end.
  void program_body();
  StepResult step_program();
  void shutdown_program();
  // The runtime's slice and yield hooks (win32/runtime.hh): the frame ends
  // inside the program's own code, or in its message loop's wait.
  void on_slice();
  bool on_yield();
  // Ends the frame on the guest fiber (run_result_ = why) and returns when a
  // later frame resumes the program (throws AbandonGuestCall at shutdown).
  void suspend_frame(Run why);
  bool program_ = false;
  bool program_ended_ = false;
  std::string stop_text_;      // how the program ended
  uint32_t exe_entry_ = 0;     // the EXE's entry point (WinMainCRTStartup)
  uint64_t api_mark_ = 0;      // API calls at the last slice (hang detection)
  uint64_t quiet_insns_ = 0;   // instructions since an API call, by slices
  uint64_t waits_ = 0;         // frames that ended in the message loop's wait
  InputState ghost_input_;     // what the program sees in ghost mode: nothing
  // Caps Lock (program.cc "Caps Lock"): the process starts in ghost mode or
  // for play, and starts again when Caps Lock changes which.
  bool start_process(bool play);
  // Ends the running process as Windows ended a killed one: abandoned where
  // it waits, its sound stopped, the runtime gone.
  void end_process();
  bool restart_process();
  void program_command(const Command& c);
  LaneStatus program_status() const;
  // Caps Lock on, not the living wallpaper, and play has not ended by itself
  // since Caps Lock went on.
  bool wants_play() const;
  std::string program_path_;
  int toy_ = 0;
  bool toy_chosen_ = false;    // ADTOY named it (else the registry's ScrToy)
  bool wallpaper_ = false;     // ADWALLPAPER=1
  bool playing_ = false;       // the running process was started for play
  bool restart_ = false;       // the next step starts the program again (wants_play())
  bool game_over_ = false;     // play ended by itself under this Caps Lock
  uint32_t starts_ = 0;        // processes started
};

}  // namespace adw::pe32
