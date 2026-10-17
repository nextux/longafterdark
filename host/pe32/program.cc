// The pe32 lane's whole programs (lane.hh, docs/ABI.md §2.15, the "desktoptoys"
// ABI): a toy of Cyclops Software's Desktop Toys engine — DTTY3203.EXE of
// Beavis and Butt-head Screen Wreckers (Viacom New Media, 1996), an MFC 4.2
// program — run unchanged as the emulated machine's one Win32 process, the
// way Windows 95 ran it for the toys' screen saver, TOYSAV32.SCR.
//
// Recognition: the module file is a PE32 EXE (IMAGE_FILE_DLL clear; every
// After Dark 4 module is a DLL). The toy comes from the host variable ADTOY
// (1..9, DTTS03.DTX's order, which the catalog sets per module); without it
// the registry's ScrToy decides, as it did for TOYSAV32.SCR.
//
// Command line: TOYSAV32.SCR (0x401856..0x40197e) read ScrVol and ScrToy
// from HKLM\SOFTWARE\Cyclops Software\Desktop Toys Screensaver and ran
// ShellExecuteA(NULL, "open", <Path>\dtty32<vv>.exe, "/s /t<ScrToy>", <Path>,
// SW_SHOWNORMAL): "/s" is the program's ghost mode (the toy plays itself
// over the desktop) and "/t<k>" its toy k+1. So the guest's command line is
// "C:\DTToys\dtty3203.exe /s /t<ADTOY-1>" and its current directory
// C:\DTToys, the installer's default folder (BBDTW.INS: SdAskDestPath,
// "DTToys" on the target disk).
//
// Registry (win32 advapi32.cc): seeded as BBDTW.INS left it — Path,
// TSAppName, Sound, ScrnSaverSnd, FavVol, FavToy, ScrVol, ScrToy, all
// REG_SZ — with ScrToy matching the toy. The program reads its settings
// through its own CReg class (0x417d80); its writes stay in memory headless
// and persist under ADSTATE (<state>\<pkg>\WINDOWS\REGISTRY.INI).
//
// The disk (INTERACTION.md §7.2): C:\DTToys = the package folder (read-only
// lower: DTTY3203.EXE, DTTS03.*, DT03\) under the upper <state>\<pkg>\DTTOYS
// or memory, the current directory; C:\WINDOWS an overlay with no lower
// files. Nothing else: the host's drives are not mounted. (<pkg> is core's
// package_state_name: the EXE sits in its package's root, not in a module
// dir under it, so it is a legacy-<fnv32> name, as the Desktop Extras' is.)
//
// Frames: the program owns its message loop (MFC's CWinThread::Run), so the
// lane's one call is the program: it starts at the EXE's entry point on the
// lane's guest fiber and runs until it ends; every frame ends inside it — at
// the frame's share of the modelled machine in an API call (the call hook,
// lane.hh "Long calls"), between two slices of code that makes no API call
// (the slice hook), or where GetMessageA/WaitMessage find nothing to deliver
// (the yield hook, win32 user32 "programs") — and the next frame resumes it
// there. Deterministic headless: every suspension point is a count of
// instructions, API calls and pixels.
//
// Input: in ghost mode (the saver: /s, /p, previews, thumbnails, /window,
// the wallpaper) nothing reaches the program — it sees an empty input state,
// and the saver ends the run on input itself, as the program's own /s mode
// did (its mouse handlers and its key poll post WM_CLOSE to its frame).
//
// Caps Lock (INTERACTION.md §4.4, §5.1): /s mode cannot be played — every
// click or nudge ends it —, so Caps Lock on starts the program again for
// play, as the toy selector TOYSEL32.EXE started a toy: ShellExecuteA(...,
// <Path>\dtty3203.exe, "/t<toy-1>", <Path>, SW_SHOWNORMAL) (0x403b13..
// 0x403bdc), the toy at once, no ghost mode; Caps Lock off starts it again
// in ghost mode. The running process ends as a killed one did (abandoned
// where it waits, its sound stopped, the runtime gone: end_process) and the
// new one starts on a fresh machine over the desktop seed, in the same
// frame. While Caps Lock is on (from the CAPS line, the restart included)
// the status is interactive with the cursor (ADWS_INTERACTIVE, ADWS_CURSOR
// while the program's cursor is on; source AD4) and every input line is the
// program's (eaten); KEY and MOUSE lines become the window messages of
// Windows 95's input queue (win32 user32_program_key/_mouse: WM_KEYDOWN to
// the focus window, the mouse to the window under the pointer), and its
// GetAsyncKeyState/GetCursorPos read the saver's keys and mouse. The
// program's own end while playing — Esc twice (the first ends the toy, the
// second, with no toy, posts WM_CLOSE), its EXIT button, the frame's
// close — is a game's end (the 7th Level protocol's rule): ghost mode
// again, not interactive, until Caps Lock goes off and on. ADWALLPAPER=1
// never plays.
//
// The end: ExitProcess, the entry point returning, or a fault nobody handles
// stops the run (logged with the program's last message box, if any); the
// host exits 1. At shutdown a program still running is abandoned where it
// waits (win32::AbandonGuestCall), as a saver's process was killed.
#include <windows.h>

#include <algorithm>
#include <cinttypes>
#include <cstdlib>

#include "adw/core/audio.h"
#include "adw/core/log.h"
#include "adw/core/text.h"
#include "loader/image.hh"
#include "loader/pe.hh"
#include "pe32/lane.hh"
#include "win32/audio.hh"
#include "win32/display.hh"
#include "win32/modules.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"
#include "win32/vfs.hh"

namespace adw::pe32 {

using win32::GuestError;
using win32::Runtime;

namespace {

std::string dir_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? "." : p.substr(0, s);
}

std::string file_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}

std::string upper(std::string s) {
  for (char& c : s) c = char(toupper(uint8_t(c)));
  return s;
}

uint64_t env_number(const Env& env, const char* name, uint64_t def) {
  const std::string* v = env.get(name);
  if (!v || v->empty()) return def;
  char* end = nullptr;
  double d = strtod(v->c_str(), &end);
  if (end == v->c_str() || d < 0) {
    log("%s='%s' is not a number; using %" PRIu64, name, v->c_str(), def);
    return def;
  }
  return uint64_t(d);
}

// The installer's folder and the registry it wrote (BBDTW.INS).
constexpr const char* kInstallDir = "C:\\DTToys";
constexpr const char* kToysKey = "HKEY_LOCAL_MACHINE\\SOFTWARE\\Cyclops Software\\Desktop Toys Screensaver";
constexpr int kInstalledToy = 3;  // ScrToy "2": the third toy
constexpr int kVolume = 3;        // ScrVol "3": DTTY3203.EXE, Desktop Toys volume 3
constexpr int kToys = 9;

// Instructions a slice runs before the lane looks at the frame (on_slice).
constexpr uint64_t kSlice = 200'000;

}  // namespace

bool Pe32Lane::is_program_file(const std::string& path) {
  try {
    return !loader::pe::Image::from_file(path).header().is_dll();
  } catch (const std::exception&) {
    return false;  // not a PE image: the module path reports what it is
  }
}

std::string Pe32Lane::desktop_toys_command_line(const std::string& install_dir, int volume, int toy,
                                                bool screen_saver) {
  char file[32];
  snprintf(file, sizeof(file), "\\dtty32%s%d.exe", volume < 10 ? "0" : "", volume);
  return install_dir + file + (screen_saver ? " /s /t" : " /t") + std::to_string(toy - 1);
}

bool Pe32Lane::init_program(const std::string& path, LaneContext& ctx) {
  program_ = true;
  program_path_ = path;
  const Env& env = ctx.env;
  toy_ = kInstalledToy;
  toy_chosen_ = false;
  if (const std::string* s = env.get("ADTOY"); s && !s->empty()) {
    char* end = nullptr;
    long n = strtol(s->c_str(), &end, 10);
    if (end == s->c_str() || *end || n < 1 || n > kToys) {
      log("ADTOY='%s' is not a toy number 1..%d; using the registry's ScrToy (toy %d)", s->c_str(), kToys,
          kInstalledToy);
    } else {
      toy_ = int(n);
      toy_chosen_ = true;
    }
  }
  wallpaper_ = env.flag("ADWALLPAPER");
  ctx.clock.set_read_step_us(uint32_t(env_number(env, "ADREADSTEPUS", 5)));
  // The work account (lane.hh "Timing").
  if (!mips_) mips_ = 100;
  return start_process(wants_play());
}

bool Pe32Lane::wants_play() const { return ctx_ && ctx_->input.caps && !wallpaper_ && !game_over_; }

bool Pe32Lane::start_process(bool play) {
  const std::string& path = program_path_;
  const Env& env = ctx_->env;
  const int toy = toy_;
  const bool chosen = toy_chosen_;
  playing_ = play;
  starts_++;
  program_ended_ = suspended_ = mid_call_ = abandon_ = false;
  run_result_ = Run::none;
  stop_text_.clear();
  api_mark_ = quiet_insns_ = 0;
  census_done_ = false;
  try {
    win32::RuntimeOptions opts;
    opts.heap_size = uint32_t(std::clamp<uint64_t>(env_number(env, "ADHEAPMB", 128), 16, 1024) << 20);
    // The program's one call never returns while it runs: no budget on it;
    // a hang is ADCALLBUDGET instructions without an API call (on_slice).
    opts.call_budget = UINT64_MAX / 4;
    opts.program = true;
    opts.guest_module_dir = kInstallDir;
    opts.exe_path = std::string(kInstallDir) + "\\" + upper(module_name_);
    opts.command_line = desktop_toys_command_line(kInstallDir, kVolume, toy, !play);
    // Ghost mode sees no input; play sees the saver's keys and mouse.
    const InputState* input = !play ? &ghost_input_ : guest_screen_ ? &guest_input_ : &ctx_->input;
    rt_ = std::make_unique<Runtime>(opts, ctx_->clock, input);
    Runtime& rt = *rt_;
    win32::register_all_shims(rt.shims());
    win32::register_program_shims(rt.shims());
    win32::user32_set_program(rt, true);

    // The disk.
    win32::Vfs& vfs = rt.vfs();
    std::string pkg = package_state_dir(env, path);  // "" = in memory
    if (!pkg.empty()) vfs.set_state_root(env.state_root);
    vfs.mount_overlay("C:\\WINDOWS", "", pkg.empty() ? std::string() : pkg + "\\WINDOWS");
    vfs.mount_overlay(kInstallDir, dir_of(path), pkg.empty() ? std::string() : pkg + "\\DTTOYS");
    vfs.set_cwd(kInstallDir);

    // The registry as the installer left it (ScrToy names the toy the saver runs).
    const std::pair<const char*, std::string> values[] = {
        {"Path", kInstallDir},     {"TSAppName", "Toysel32.exe"},          {"Sound", "on"},
        {"ScrnSaverSnd", "on"},    {"FavVol", "3"},                        {"FavToy", "4"},
        {"ScrVol", std::to_string(kVolume)}, {"ScrToy", std::to_string(toy - 1)},
    };
    for (const auto& [name, value] : values) win32::registry_seed(rt, kToysKey, name, value);
    win32::registry_attach_file(rt, "C:\\WINDOWS\\REGISTRY.INI");
    // The toy the catalog names is the one TOYSAV32.SCR would have read, whatever a past run left.
    if (chosen) win32::registry_seed(rt, kToysKey, "ScrToy", std::to_string(toy - 1));
    // The saver's Sound decides, whatever a past run's Ctrl+S left: on, the
    // program's own switches are on (ghost mode reads ScrnSaverSnd, play
    // Sound; InitInstance 0x42d7ae); off, DirectSound has no driver.
    audio::Engine& sound = ctx_->audio ? *ctx_->audio : audio::null_engine();
    if (sound.enabled()) {
      win32::registry_seed(rt, kToysKey, "Sound", "on");
      win32::registry_seed(rt, kToysKey, "ScrnSaverSnd", "on");
    }

    win32::Display& display = rt.attach_display(guest_screen_ ? *guest_screen_ : ctx_->screen);
    win32::attach_audio(rt, sound, {});
    if (!sound.enabled()) win32::register_dsound_absent(rt);
    if (const std::string* s = env.get("ADSEEDIMG"); s && !s->empty() && !env.flag("ADNOSEED")) {
      std::string what;
      if (display.seed(*s, &what)) {
        trace("lane", "desktop seed: %s", what.c_str());
      } else {
        log("ADSEEDIMG: %s; the screen starts black", what.c_str());
      }
    }

    module_ = rt.modules().load_exe(path);
    exe_entry_ = module_->entry;
    trace("lane", "%s: a program (desktoptoys): mapped at 0x%08X, entry 0x%08X; command line \"%s\"%s, %s; disk "
          "%s over %s", module_name_.c_str(), module_->base, exe_entry_, opts.command_line.c_str(),
          chosen ? " (ADTOY)" : " (the registry's ScrToy)", play ? "play (Caps Lock)" : "ghost mode", kInstallDir,
          pkg.empty() ? "memory (no ADSTATE)" : pkg.c_str());
    if (!exe_entry_) {
      log("%s: the program has no entry point", module_name_.c_str());
      census();
      return false;
    }
    // Play: the pointer is where the saver's mouse is (no window yet to tell).
    if (play && ctx_->input.mouse_seen) {
      win32::user32_program_mouse(rt, guest_x(ctx_->input.mouse_x), guest_y(ctx_->input.mouse_y),
                                  ctx_->input.mouse_buttons);
    }
    // The program's frames always run on the guest fiber.
    make_fibers();
    if (!guest_fiber_) {
      census();
      return false;
    }
    long_calls_ = true;
    rt.set_yield_hook([this] { return on_yield(); });
    rt.set_slice_hook(kSlice, [this] { on_slice(); });
    return true;
  } catch (const GuestError& e) {
    log("%s: %s", module_name_.c_str(), e.what());
    if (rt_) rt_->log_state("guest state");
  } catch (const loader::LoaderError& e) {
    log("%s: cannot load: %s", module_name_.c_str(), e.what());
  } catch (const std::exception& e) {
    log("%s: %s", module_name_.c_str(), e.what());
  }
  census();
  return false;
}

void Pe32Lane::suspend_frame(Run why) {
  suspended_ = true;
  run_result_ = why;
  SwitchToFiber(host_fiber_);
  suspended_ = false;
  if (abandon_) throw win32::AbandonGuestCall{};
}

// Between two slices of guest code (win32 Runtime::set_slice_hook): code that
// runs long without an API call still ends its frame on time; ADCALLBUDGET
// instructions with no API call at all is a hang.
void Pe32Lane::on_slice() {
  if (rt_->api_calls() != api_mark_) {
    api_mark_ = rt_->api_calls();
    quiet_insns_ = 0;
  } else if ((quiet_insns_ += kSlice) >= env_number(ctx_->env, "ADCALLBUDGET", 1'000'000'000ull)) {
    throw GuestError(GuestError::Kind::hang, "the program ran " + std::to_string(quiet_insns_) +
                                                 " instructions without an API call (stopped at " +
                                                 rt_->describe(rt_->cpu().registers().eip) + ")");
  }
  if (mid_call_ && GetCurrentFiber() == guest_fiber_ && frame_over()) suspend_frame(Run::suspended);
}

// GetMessageA/WaitMessage with nothing to deliver: the frame ends in the wait.
bool Pe32Lane::on_yield() {
  if (!guest_fiber_ || GetCurrentFiber() != guest_fiber_) return false;
  waits_++;
  suspend_frame(Run::frame_done);
  return true;
}

void Pe32Lane::program_body() {
  try {
    mid_call_ = true;
    // WinMainCRTStartup takes no arguments and never returns: the CRT's exit
    // ends the process.
    uint32_t r = rt_->call_guest(exe_entry_, {}, win32::Conv::cdecl_);
    stop_text_ = "the program's entry point returned (" + std::to_string(r) + ")";
    run_result_ = Run::failed;
  } catch (const win32::AbandonGuestCall&) {
    stop_text_ = "abandoned at shutdown";
    run_result_ = Run::abandoned;
  } catch (const GuestError& e) {
    if (e.kind() == GuestError::Kind::exit) {
      stop_text_ = std::string("the program ended: ") + e.what();
      run_result_ = Run::failed;
    } else {
      fiber_error_ = std::current_exception();
      run_result_ = Run::error;
    }
  } catch (...) {
    fiber_error_ = std::current_exception();
    run_result_ = Run::error;
  }
  mid_call_ = false;
  suspended_ = false;
  program_ended_ = true;
  const std::string box = win32::user32_last_message_box(*rt_);
  if (!box.empty()) stop_text_ += "; its last message box: " + box;
  for (;;) SwitchToFiber(host_fiber_);
}

void Pe32Lane::end_process() {
  if (!rt_) return;
  try {
    if (suspended_ && guest_fiber_ && !program_ended_) {
      abandon_ = true;
      SwitchToFiber(guest_fiber_);
      abandon_ = false;
    }
  } catch (const std::exception& e) {
    log("%s: while closing: %s", module_name_.c_str(), e.what());
  }
  win32::audio_end_process(*rt_);
  census();
  rt_.reset();
  module_ = nullptr;
  free_fibers();
}

// Caps Lock changed what the run is, or play ended by itself: the program
// starts again as wants_play() says.
bool Pe32Lane::restart_process() {
  restart_ = false;
  const bool play = wants_play();
  trace("lane", "%s: frame %" PRIu64 ": %s: the program (%s) ends and starts again %s", module_name_.c_str(),
        frames_, game_over_ && playing_ ? "play ended by itself" : ctx_->input.caps ? "Caps Lock on" : "Caps Lock off",
        program_ended_ ? stop_text_.c_str() : "still running", play ? "for play" : "in ghost mode");
  end_process();
  return start_process(play);
}

void Pe32Lane::program_command(const Command& c) {
  using K = Command::Kind;
  if (!ctx_) return;
  if (c.kind == K::caps) {
    // Caps Lock off after play ended by itself: the next Caps Lock plays again.
    if (!ctx_->input.caps) game_over_ = false;
    restart_ = wants_play() != playing_ || program_ended_;
  }
  const bool input = c.kind == K::key || c.kind == K::caps || c.kind == K::numlock || c.kind == K::mouse;
  if (input && wants_play()) eaten_ = std::max(eaten_, c.seq);
  if (!playing_ || restart_ || program_ended_ || !rt_) return;
  sync_input();
  if (c.kind == K::key) win32::user32_program_key(*rt_, uint32_t(c.a), c.b != 0);
  if (c.kind == K::mouse) win32::user32_program_mouse(*rt_, guest_x(c.a), guest_y(c.b), uint32_t(c.c));
}

LaneStatus Pe32Lane::program_status() const {
  LaneStatus st;
  // Interactive from the CAPS line on (the restart included), as a game is.
  st.interactive = wants_play();
  st.cursor = st.interactive && (!rt_ || restart_ || win32::user32_cursor_shown(*rt_));
  st.source = st.interactive ? kStatusSourceAd4 : kStatusSourceNone;
  st.eaten = eaten_;
  return st;
}

StepResult Pe32Lane::step_program() {
  if (restart_ && !restart_process()) return StepResult::failed;
  if (!rt_ || program_ended_) return StepResult::failed;
  try {
    sync_input();
    i0_ = rt_->instructions();
    a0_ = rt_->api_calls();
    p0_ = rt_->pixels_charged();
    const uint64_t w0 = waits_;
    const bool resumed = suspended_;
    const uint32_t period = ctx_->clock.step_us();
    const int64_t budget = int64_t(mips_ * period);
    credit_ = std::min(credit_ + budget, budget);
    uint64_t work = 0;
    if (credit_ > 0) {
      frame_credit_ = uint64_t(credit_);
      wall_deadline_us_ =
          ctx_->clock.mode() == VirtualClock::Mode::realtime ? VirtualClock::system_wall_us() + period * 9 / 10 : 0;
      run_result_ = Run::none;
      SwitchToFiber(guest_fiber_);
      if (run_result_ == Run::error) {
        std::exception_ptr e = fiber_error_;
        fiber_error_ = nullptr;
        std::rethrow_exception(e);
      }
      if (program_ended_ && playing_ && !restart_) {
        // Its own end while playing: a game's end, ghost mode from the next frame.
        game_over_ = restart_ = true;
      }
      if (program_ended_ && !restart_) {
        log("%s: frame %" PRIu64 ": the program stopped: %s", module_name_.c_str(), frames_, stop_text_.c_str());
        census();
        return StepResult::failed;
      }
      work = frame_work();
    }
    credit_ = std::max(credit_ - int64_t(work), -kMaxDebtFrames * budget);
    if (suspended_ && run_result_ == Run::suspended) long_frames_++;
    trace("pace", "frame %" PRIu64 ":%s%s %" PRIu64 " instructions, %" PRIu64 " API calls, %" PRIu64
          " pixels, work %" PRIu64 ", account %" PRId64, frames_, resumed ? " resumed," : "",
          waits_ != w0 ? " ended in its wait," : "", rt_->instructions() - i0_, rt_->api_calls() - a0_,
          rt_->pixels_charged() - p0_, work, credit_);
    frames_++;
    GdiFlush();
    if (guest_screen_) downsample(*guest_screen_, ctx_->screen, guest_scale_, near_);
    ctx_->screen.mark_dirty();
    return StepResult::ok;
  } catch (const GuestError& e) {
    log("%s: frame %" PRIu64 ": %s", module_name_.c_str(), frames_, e.what());
    rt_->log_state("guest state");
  } catch (const std::exception& e) {
    log("%s: frame %" PRIu64 ": %s", module_name_.c_str(), frames_, e.what());
    rt_->log_state("guest state");
  }
  census();
  return StepResult::failed;
}

void Pe32Lane::shutdown_program() {
  if (!rt_) return;
  try {
    if (suspended_ && guest_fiber_ && !program_ended_) {
      abandon_ = true;
      SwitchToFiber(guest_fiber_);
      abandon_ = false;
    }
  } catch (const std::exception& e) {
    log("%s: while closing: %s", module_name_.c_str(), e.what());
  }
  trace("lane", "%s: %" PRIu64 " frames (%" PRIu64 " ended in its wait, %" PRIu64 " inside its code), %u start(s); %s",
        module_name_.c_str(), frames_, waits_, long_frames_, starts_,
        program_ended_ ? stop_text_.c_str() : "the program was still running");
  census();
  rt_.reset();
  free_fibers();
}

}  // namespace adw::pe32
