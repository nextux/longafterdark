// The Classic lane's view of a module protocol (lane.hh "Module protocols"):
// what the lane's frame loop, configure path and shutdown ask of the host side
// of the protocol a module is driven by, so that the machinery around it — the
// Win16 runtime and its knobs, the display, the desktop seed and the
// small-screen guest display, the fiber that carries long calls, the draw
// budget, the scanout latch, input and status, the audio pump and the
// configure scaffolding — is the same whichever protocol runs.
//
//   ad3  After Dark 2.x/3.x modules (the export MODULE), driven the way
//        AFTERDAR.SCR drove them through OLDMOD32: a bridge (bridge.hh, the
//        real OLDMOD16.DLL or the native AD3 bridge) takes LOADADMODULE16,
//        MODULEMESSAGE16, SETMODULECTRLVALUES16, UNLOADADMODULE16 and
//        BUTTONPUSHED16 (ad3_protocol.cc, make_ad3_protocol).
//   imx  Intermission modules (SAVERINIT + SAVERDRAW: Star Wars Screen
//        Entertainment's .IMX; The Far Side's and Dilbert's .ASA animations
//        and .IMQ modules), driven the way Delrina's INTERMIS.EXE drove
//        them through a reader (imreader.hh: the real IMIMXPLY.IMQ or the
//        native reader, IMASAPLY.IMQ for an ASA animation, an IMQ module
//        itself; package.hh "Form"): SAVERMAIN(info, msg) with an IMINFO record —
//        LOAD/QUERY, one START then a DRAW per pass of its idle loop, each
//        inside its DC bracket, STOP and FREE, CONFIGURE for the button
//        (imx_protocol.cc, make_imx_protocol; lane.hh "Intermission (IMX)").
//        It takes no key messages, carries overruns, and has a pixel cost of
//        its own.
//   scr  Windows 3.1 screen savers (an application exporting SCREENSAVERPROC:
//        a .SCR built on SCRNSAVE.LIB, Johnny Castaway's SCRANTIC.SCR), run
//        as Windows 3.1 ran one: the program itself, unchanged, as the
//        runtime's task (win16/modules16.hh "Tasks") with the command line
//        "/s", its own message loop waiting in GetMessage (scr_protocol.cc,
//        make_scr_protocol; lane.hh "Windows 3.1 screen savers"). Its one
//        call is the task: it lasts the whole run, every frame ending inside
//        it (long calls are required), and at shutdown it is closed as on
//        the input that woke the saver. It takes no key or mouse messages;
//        its button runs the program with "/c".
//   7thlevel  a 7th Level Runtime Player scene (a 7th Level resource file: Take
//        Your Best Shot's BESTACT\*.BIN; the Desktop Pythonizer's), run as
//        the product's desktop program (BESTSHOT.EXE, PYTHIZER.EXE) ran it:
//        the real player (PLYMPTON.EXE, PYTHON.EXE: the header's product
//        picks it), with its own DLLs, as the runtime's task, its command line the scene
//        and "/p" with the handle of the 0x6C-byte block that says the mode
//        (seventh_protocol.cc, make_seventh_protocol; lane.hh "7th Level
//        scenes"). A task as scr's is; it takes no key or mouse messages in
//        the saver's mode and has no button.
// Ne16Lane's default factory picks one by the module's exports (package.hh
// detect_kind, ADNE16KIND), a data file by its header or extension.
//
// A run (Ne16Lane::init, step, on_command, shutdown):
//   configure_runtime   before the runtime exists: the protocol's choices and its runtime options
//   mount               the guest's disk and seeds, on the new runtime
//   load                the module, on the saver window and its screen DC
//   call, after_call    per presented frame, a run of calls (lane.hh "Pacing", "Long calls"), each
//                       after the lane delivered input and pumped audio; a frame whose whole budget
//                       pays back a carried overrun makes no call, and after_call alone runs (after
//                       the input and the audio pump)
//   set_control         each SET; send_controls when the guest can be called
//   unload, close       at shutdown, unless a call is still suspended; then every module is freed
// What the lane's machinery does differently for a protocol is asked, not
// decided by kind: whether KEY lines become key messages
// (takes_key_messages), whether overruns are carried (carries_overruns),
// what a blit's or fill's pixel costs the budget (pixel_cost) and which knob
// sets the modeled machine's speed (speed_knob).
// A button (Ne16Lane::configure): check_button, configure_button_runtime, mount,
// button, close; then every module is freed.
//
// Every guest call a protocol makes goes through the runtime's thunks
// (Runtime16::call_far), so the census, the api16 traces, virtual time, the
// scanout hook and the frame deadline see them as they see the module's own.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "adw/core/lane.h"
#include "ne16/package.hh"

namespace adw::win16 {
class Runtime16;
struct Runtime16Options;
}  // namespace adw::win16

namespace adw::ne16 {

class Protocol16 {
 public:
  virtual ~Protocol16() = default;
  // For logs and tests: "ad3/oldmod16", "ad3/native", "imx/imq", "imx/native".
  virtual const char* name() const = 0;

  // ---- a run ------------------------------------------------------------------------------------------------
  // The first thing Ne16Lane::init does after the layout, before the runtime
  // exists: the protocol's choices from the environment and the host's sound,
  // and the runtime options that are the protocol's to decide — the guest's
  // directories and the display's starting palette (ADDESKTOPPAL overrides
  // the palette afterwards). AD3: the bridge (ADNE16BRIDGE, logged when it is
  // not one of the choices), the volume and mute, the palette.
  virtual void configure_runtime(win16::Runtime16Options& opts, LaneContext& ctx) = 0;
  // The guest's disk and its seeds, on the new runtime before the display is
  // attached (a run and a button alike). AD3: mount_disk.
  virtual void mount(win16::Runtime16& rt, const Env& env) = 0;
  // Loads and starts the module on the saver window `hwnd` and its screen DC
  // `hdc`. False when it could not: the protocol has logged why, and the lane
  // prints the census and fails. AD3: the bridge, the AD palettes, the control
  // defaults with ADCVSET over them, LOADADMODULE16.
  virtual bool load(win16::Runtime16& rt, uint16_t hwnd, uint16_t hdc, LaneContext& ctx) = 0;
  // What one call did, for the lane's loop (lane.hh "Input and status").
  struct Call {
    enum class Kind {
      ok,             // go on
      toggle_events,  // the module now takes input as its own, or no longer (AD3: 0x0E; status source 2)
      cursor_on,      // it wants the cursor shown (AD3: 0x11) ...
      cursor_off,     // ... or hidden (AD3: 0x12)
      stop,           // it ended the run: the lane logs `code` with error_text() and fails the step
      wake,           // it asked the saver to end, as the user's input would (AD3: an After Dark 2.0
                      // module's 5): the lane reports the wake, logs it and calls it no more
    };
    Kind kind = Kind::ok;
    int code = 0;  // the protocol's own result
  };
  // One call of the frame's run. The lane has delivered the pending input
  // and pumped audio; the call may reach the frame's deadline inside an API
  // call, be suspended there and resume in the next step. AD3:
  // SetWindowOrgEx(hdc, 0, 0), MODULEMESSAGE16(DRAWFRAME).
  virtual Call call() = 0;
  // After each call that did not stop the run, outside it (never suspended),
  // and in a frame that makes no call (carries_overruns: the lane has
  // delivered the input and pumped audio). AD3: nothing (AFTERDAR.SCR's
  // message loop is the lane's audio pump).
  virtual void after_call() {}
  // Whether the saver's KEY lines reach the guest as key messages: the
  // WH_KEYBOARD chain, then the saver window's queue (lane.hh "Input and
  // status"). False: they are left in the host's key state alone, which
  // GetAsyncKeyState/GetKeyState read (the lane queues nothing). AD3: true.
  virtual bool takes_key_messages() const { return true; }
  // Whether MOUSE lines reach the guest as mouse messages in the saver
  // window's queue. False: the host's mouse state alone (GetCursorPos).
  // AD3, IMX: true; scr: false (a Windows 3.1 saver closed itself on a mouse
  // move; the host's saver ends the run on input itself).
  virtual bool takes_mouse_messages() const { return true; }
  // Whether the module now plays as a state of its own, outside the calls'
  // results (7thlevel: a game, or the interactive screen saver, while Caps
  // Lock is on): the lane reports it interactive (source 2, ADWS_INTERACTIVE)
  // and consumes every input line, as for AD3's 0x0E (Call::toggle_events);
  // plays_cursor, whether it wants the cursor shown meanwhile (ADWS_CURSOR).
  // AD3, IMX, scr: false.
  virtual bool plays() const { return false; }
  virtual bool plays_cursor() const { return false; }
  // Whether the protocol's calls can only run as long calls (lane.hh "Long
  // calls"): scr, whose one call is the program's task. The lane refuses the
  // module (exit 1) when they are off (ADMIPS=0, ADNE16LONGCALLS=0).
  virtual bool runs_as_task() const { return false; }
  // At shutdown with the call suspended: true when the protocol has asked
  // the module to end as its host ended it, and the lane is to resume the
  // call so that it can (Ne16Lane::wind_down: bounded; then abandoned if it
  // still runs). scr: WM_CLOSE to the program's window, what SCRNSAVE.LIB's
  // DefScreenSaverProc posted on the waking input (SCRANTIC saves its story
  // in its WM_DESTROY). AD3, IMX: false (the call is abandoned).
  virtual bool close_suspended() { return false; }
  // Whether the work a call completed within its frame did beyond the
  // frame's DRAWFRAME budget is carried into the next frames (lane.hh
  // "Pacing"): paid back from their budgets first, a frame whose whole budget
  // goes to it making no call. AD3: false (one call at least per frame).
  virtual bool carries_overruns() const { return false; }
  // What one pixel a GDI blit or fill writes costs the DRAWFRAME budget, in
  // instruction-equivalents (Runtime16Options::pixel_cost_insns, lane.hh
  // "Pacing"): `def`, or the environment variable `knob` when it is set. Each
  // protocol has its own knob, so one never changes the other's cost. AD3:
  // ADPIXCOST, 2.
  struct PixelCost {
    const char* knob;
    uint32_t def;
  };
  virtual PixelCost pixel_cost() const { return {"ADPIXCOST", 2}; }
  // The environment variable that sets the modeled machine's speed (lane.hh
  // "Pacing", Speed): a percent, 1..100 (unset: 100, the lane's machine as it
  // is), that scales the frame's DRAWFRAME budget and ADMAXDRAWS down and the
  // bound on what is owed up, so a module that steps once a call steps that
  // much less often. Null: the protocol has none, and its runs are at 100%
  // (the lane logs kImxSpeedKnob as ignored when it is set). IMX:
  // kImxSpeedKnob; AD3, scr: none.
  virtual const char* speed_knob() const { return nullptr; }
  // SET <index> <value>. True when the module must be sent the values: the
  // lane calls send_controls() at once, or — a call being suspended — before
  // the next call. AD3: ctrl4[index] for index 0..3.
  virtual bool set_control(int index, int32_t value) {
    (void)index;
    (void)value;
    return false;
  }
  // AD3: SETMODULECTRLVALUES16(volume, mute, ctrl4).
  virtual void send_controls() {}
  // At shutdown, with no call suspended: stops the module and releases it.
  // AD3: UNLOADADMODULE16.
  virtual void unload() = 0;
  // The module's error text, for the log line of a stop. AD3: the scratch
  // block's, "(no error text)" when empty.
  virtual std::string error_text() const { return {}; }

  // ---- a button ---------------------------------------------------------------------------------------------
  // Before anything is loaded: whether the module has button `slot`; false
  // with *why when it has not. AD3: the slot's control record is a button
  // (kind 4, ABI.md §2.10.2).
  virtual bool check_button(int slot, std::string* why) = 0;
  // configure_runtime's counterpart in configure mode: the choices, silently
  // and without sound, and the runtime options. AD3: the bridge, the palette.
  virtual void configure_button_runtime(win16::Runtime16Options& opts, const Env& env) = 0;
  // What a button left for the lane to report.
  struct Button {
    // False: nothing of the module ran; `message` says why, and the lane
    // reports a failure with no dialog and nothing written (no close).
    bool ran = false;
    // It ran, but failed for a reason of the protocol's own (AD3: AD_SND's
    // error id): the failure's message.
    std::string failure;
    // What the module said, ahead of the dialogs' notes (AD3: the error text
    // BUTTONPUSHED16 left).
    std::string message;
  };
  // Runs button `slot` on the mounted runtime, its dialogs real and owned by
  // `owner16`. The lane then closes the protocol, frees every module, takes
  // what was written, and reports `failure` when set, else a dialog that
  // failed, else shown or nothing by the dialogs shown. AD3: the bridge, the
  // scratch block, the controls, BUTTONPUSHED16.
  virtual Button button(win16::Runtime16& rt, int slot, uint16_t owner16, LaneContext& ctx) = 0;

  // ---- both -------------------------------------------------------------------------------------------------
  // After unload (a run) or button, before every module is freed. AD3: the
  // bridge's close (OLDMOD16's DLLENTRYPOINT(0) part).
  virtual void close() {}
};

// ---- the AD3 protocol (ad3_protocol.cc) --------------------------------------------------------------------

// The AD3 protocol for the module at layout.module_path.
std::unique_ptr<Protocol16> make_ad3_protocol(const Ne16Layout& layout);

// The scratch block the AD3 protocol passes the bridge pointers into (a fixed global block).
namespace scratch {
constexpr uint16_t kCtrl = 0x000;    // WORD ctrl4[4]
constexpr uint16_t kErrId = 0x010;   // WORD
constexpr uint16_t kPath = 0x020;    // char[260]
constexpr uint16_t kError = 0x130;   // char[260]
constexpr uint16_t kErrorSize = 260;
constexpr uint16_t kSize = 0x240;
}  // namespace scratch

// The guest's disk for a module (ad3_protocol.cc): C:\WINDOWS (over the
// package's windows dir when it has one), C:\AFTERDRK/C:\AFTERD~1
// (copy-on-write overlays whose upper layers are ADSTATE's package dir, or
// memory), C:\WINDOWS\SYSTEM (the engine dir) and H: (INTERACTION.md §7.2).
void mount_disk(win16::Runtime16& rt, const Env& env, const std::string& module_path, const Ne16Layout& layout);

// The value a control record (type 1000 resource) starts at (ABI.md §2.10.2):
// a kind-1 string slider's stop value, a kind-2 numeric slider's clamped
// default, a kind-3 popup's clamped index, a kind-5 checkbox's 0/1.
int16_t control_default16(std::string_view record);

// ---- the IMX protocol (imx_protocol.cc) --------------------------------------------------------------------

// The Intermission protocol for the module at layout.module_path, of the form
// `form` (package.hh "Form"): an IMX module read by IMIMXPLY.IMQ (or the
// native reader), an ASA animation read by IMASAPLY.IMQ, or an IMQ module
// that is its own reader.
std::unique_ptr<Protocol16> make_imx_protocol(const Ne16Layout& layout, ImxForm form = ImxForm::imx);

// What one pixel of a blit or fill costs an Intermission module's DRAWFRAME
// budget unless ADNE16IMXPIXCOST says otherwise (Protocol16::pixel_cost;
// why 4: lane.hh "Pacing").
constexpr uint32_t kImxPixelCost = 4;

// The knob that sets an Intermission module's machine speed, a percent
// (Protocol16::speed_knob; lane.hh "Pacing", Speed). The front end sets it
// from the module's Speed control (a catalog control with "host":
// "ADNE16IMXSPEED"); other protocols ignore it.
constexpr const char kImxSpeedKnob[] = "ADNE16IMXSPEED";

// The guest's disk for an Intermission module (imx_protocol.cc): C:\SAVER (the
// module dir, the current directory) and C:\WINDOWS (over the package's
// windows dir when it has one) as copy-on-write overlays whose upper layers
// are ADSTATE's package dir or memory, C:\WINDOWS\SYSTEM (the engine dir) and
// H:, with the Intermission profile seeds (win16/dos16.hh seed_intermission:
// `volume` is ANTSW.INI's Volume, 0 = Off).
void mount_imx_disk(win16::Runtime16& rt, const Env& env, const Ne16Layout& layout, int volume);

// ---- the scr protocol (scr_protocol.cc) --------------------------------------------------------------------

// The Windows 3.1 screen-saver protocol for the program at layout.module_path.
std::unique_ptr<Protocol16> make_scr_protocol(const Ne16Layout& layout);

// Where the program's installer put it on the guest's disk: C:\SIERRA\SCRANTIC
// for SCRANTIC.SCR (Johnny Castaway's installer's default destination), else
// C:\<module dir's name> — a rule by file name, never a package id.
std::string scr_install_dir(const Ne16Layout& layout);

// The guest's disk for a Windows 3.1 screen saver (scr_protocol.cc): the
// module dir at its install dir and C:\WINDOWS (over the package's windows
// dir when it has one) as copy-on-write overlays whose upper layers are
// ADSTATE's package dir or memory, C:\WINDOWS\SYSTEM (the engine dir), H:,
// with the profile seeds its installer left (win16/dos16.hh seed_scrnsave).
void mount_scr_disk(win16::Runtime16& rt, const Env& env, const Ne16Layout& layout);

// ---- the 7thlevel protocol (seventh_protocol.cc) -----------------------------------------------------------

// The 7th Level Runtime Player protocol for the scene at layout.module_path
// (its player: package.hh seventh_find_player).
std::unique_ptr<Protocol16> make_seventh_protocol(const Ne16Layout& layout);

// The player's modes, the block's +2 (BESTSHOT.EXE's callers of 6:00E6; the
// player's 1:6EB0 takes 0..6; its 1:70A2 starts each). The Desktop
// Pythonizer's PYTHIZER.EXE filled the same block at 8:00E6 for modes 1..4
// (its screen saver 8:042C, its living wallpaper 9:048B) and at 6:0C3A for
// mode 5, and its PYTHON.EXE takes 0..5 (1:6457).
enum class SeventhMode : uint16_t {
  program = 0,           // another 7th Level program (BESTSHOT's samplers)
  wallpaper = 1,         // the living wallpaper (7:04E9: its flag 0): the scene in a window at the
                         // bottom of the z-order (1:7266, 1:ADE2)
  wallpaper_preview = 2, // the living wallpaper's preview in BESTSHOT's dialog (10:2BBC: flag 1): topmost
  saver = 3,             // the screen saver (6:068A, BESTSHOT's /s)
  saver_preview = 4,     // the screen saver's preview in BESTSHOT's dialog (10:30B8)
  five = 5,              // (the player sets a flag of its own; no BESTSHOT caller; PYTHIZER's OLE animations)
  game = 6,              // a game (2:06CA; Best Shot only)
};
// What BESTSHOT.EXE's 6:00E6 put in the block (GlobalAlloc(GMEM_SHARE |
// GMEM_MOVEABLE | GMEM_ZEROINIT, 0x6C)) before it WinExec'd the player.
struct SeventhBlock {
  SeventhMode mode = SeventhMode::saver;
  uint16_t mute = 0;             // +0x12: [BestShotControls] ScreenSaverMute (GamesMute,
                                 // LivingWallpaperMute) == YES
  uint16_t not_interactive = 1;  // +0x14: 6:00E6's caller's flag == 0 — the screen saver's
                                 // ScreenSaverInteractive != YES; 0 for a game and the living wallpaper.
                                 // The player reads it in modes 3 and 4 (1:6F41): 0 is the interactive
                                 // saver, which no input closes (1:7656, 1:784E)
  uint16_t password = 0;         // +0x16 (mode 3 only): ScreenSaverPasswordOn == YES and a password
  std::string title;             // +0x1A: the scene's title, at most 0x4F characters
  uint16_t ui_window = 0;        // +0x6A: BESTSHOT's UI window (the host's stand-in)
};
constexpr uint16_t kSeventhBlockSize = 0x6C;
constexpr uint16_t kSeventhBlockFlags = 0x2042;  // GMEM_SHARE | GMEM_ZEROINIT | GMEM_MOVEABLE
// The block's 0x6C bytes as 6:00E6 filled them (the rest zero).
std::string seventh_block_bytes(const SeventhBlock& b);
// What a run of a scene is, the one place it is decided (lane.hh "7th Level
// scenes"): with ADWALLPAPER=1 (the saver's /wallpaper) the living wallpaper
// as BESTSHOT.EXE installed one (mode 1, 4:05D0 → 7:04E9: sound on, which
// LivingWallpaperMute's default leaves), whatever Caps Lock says; else, Caps
// Lock on (`caps`), a game scene's game (`game`: the installer's [Games];
// mode 6 as 2:06CA started it, sound on) or another scene's interactive
// screen saver (mode 3 with ScreenSaverInteractive=YES, +0x14 0); else the
// screen saver as BESTSHOT's /s started it with the installer's defaults
// (mode 3, sound on, not interactive, no password). Titled `title`.
SeventhBlock seventh_run_block(const Env& env, const std::string& title, bool caps = false, bool game = false);
// Whether the scene file `file` (a file name or path) is one of the games
// the installer listed under 7THLEVEL.INI's [Games]: HEADSHOT.BIN,
// HOTSHOT.BIN, LINESHOT.BIN (any case) — a rule by file name.
bool seventh_is_game(const std::string& file);
// A 7th Level resource file's product key and title, from its header ("7L"
// 02 00 — Take Your Best Shot's — or "7L" 01 00 — the Desktop Pythonizer's —,
// a length byte, the product NUL-terminated — "BestShot", "Python": the
// desktop program's [Apps] key for the player —, the title ending at a NUL or
// 0xFB). False when the file is not one.
bool seventh_header(const std::string& host_path, std::string* product, std::string* title);
// The player's command tail (the PSP's): BESTSHOT's (and PYTHIZER's)
// wsprintf(" %s /p%lX", scene, MAKELONG(hBlock, 0)) after its own name.
std::string seventh_command_tail(const std::string& guest_scene, uint16_t hblock);

// Where the installers put things on the guest's disk: the player's folder
// at the product's install_dir (SETUP.EXE's default: c:\7thLevel\BestShot,
// c:\7thLevel\Python), an add-on's scenes in it beside the player, the CD's
// scenes at D:\<folder>.
constexpr const char* kSeventhCdDrive = "D:";
// The guest's disk for a 7th Level scene (seventh_protocol.cc) whose player
// is `player` (package.hh seventh_find_player): the player's folder at the
// product's install_dir (the current directory) and C:\WINDOWS (over the
// package's windows dir when it has one) as copy-on-write overlays whose
// upper layers are ADSTATE's package dir or memory, the CD's folders —
// the scene's own (none for an add-on) and the product's cd_folders, from the
// player's package — at D:\<folder>, read-only; an add-on's scenes as files
// of the player's folder; C:\WINDOWS\SYSTEM (the engine dir), H:, with the
// profile seeds the installers left (win16/dos16.hh seed_seventh_level).
void mount_seventh_disk(win16::Runtime16& rt, const Env& env, const Ne16Layout& layout, const SeventhProduct& product,
                        const SeventhPlayer& player);
// The scene's path on the guest's disk, as its desktop program's
// 7THLEVEL.INI named it: D:\<FOLDER>\<FILE> on the CD, an add-on's
// <install_dir>\<FILE>.
std::string seventh_guest_scene(const Ne16Layout& layout, const SeventhProduct& product, const SeventhPlayer& player);

}  // namespace adw::ne16
