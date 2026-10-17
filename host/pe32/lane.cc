#include "pe32/lane.hh"

#include <windows.h>

#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/clock.h"
#include "adw/core/log.h"
#include "adw/core/text.h"
#include "loader/image.hh"
#include "pe32/controls.hh"
#include "pe32/package.hh"
#include "win32/audio.hh"
#include "win32/display.hh"
#include "win32/gdi_objects.hh"
#include "win32/config_script.hh"
#include "win32/modules.hh"
#include "win32/realui.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"
#include "win32/vfs.hh"

namespace adw::pe32 {

using win32::GuestError;
using win32::Runtime;

namespace {

std::string full_path(const std::string& p) {
  std::wstring w = widen(p);
  DWORD n = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
  if (!n) return p;
  std::wstring out(n, L'\0');
  n = GetFullPathNameW(w.c_str(), n, out.data(), nullptr);
  out.resize(n);
  return narrow(out);
}

std::string dir_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? "." : p.substr(0, s);
}

std::string file_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}

uint64_t env_u64(const Env& env, const char* name, uint64_t def) {
  const std::string* v = env.get(name);
  if (!v || v->empty()) return def;
  char* end = nullptr;
  double d = strtod(v->c_str(), &end);  // accepts 1e9 as well as plain integers
  if (end == v->c_str() || d < 0) {
    log("%s='%s' is not a number; using %" PRIu64, name, v->c_str(), def);
    return def;
  }
  return uint64_t(d);
}

bool env_flag_default(const Env& env, const char* name, bool def) { return env.get(name) ? env.flag(name) : def; }

// The guest's disk (INTERACTION.md §7.2):
//   C:\WINDOWS    an overlay with no lower files: its upper is the package's
//                 <state>\<pkg>\WINDOWS (WIN.INI, MODULES.INI, AFTERDRK.INI),
//                 or memory when there is no ADSTATE
//   C:\AFTERDRK   the module's folder (the install directory the original host
//                 SetCurrentDirectory'd to, ABI.md §2.1) under an upper
//                 <state>\<pkg>\<MODDIR> (AD40, AD10TH, …) or memory; the
//                 current directory
//   C:\PICTURES   <module dir>\PICTURES, read-only: Art Critic looks for its
//                 sample art at <module dir>\..\Pictures (the installed modules
//                 sat one level below the folder holding Pictures; the imported
//                 CD tree keeps PICTURES next to the modules)
//   H:\<L>\…      the host's drives, read-only (paths picked in file dialogs, §7.4)
void mount_disk(Runtime& rt, const Env& env, const std::string& module_path) {
  win32::Vfs& vfs = rt.vfs();
  const std::string& guest_dir = rt.options().guest_module_dir;
  std::string dir = dir_of(module_path);
  std::string pkg = package_state_dir(env, module_path);  // "" = in memory
  if (!pkg.empty()) vfs.set_state_root(env.state_root);
  vfs.mount_overlay("C:\\WINDOWS", "", pkg.empty() ? std::string() : pkg + "\\WINDOWS");
  vfs.mount_overlay(guest_dir, dir, pkg.empty() ? std::string() : pkg + "\\" + file_of(dir));
  std::string pics = dir + "\\PICTURES";
  DWORD a = GetFileAttributesW(widen(pics).c_str());
  if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
    size_t s = guest_dir.find_last_of('\\');
    std::string parent = s == std::string::npos ? guest_dir : guest_dir.substr(0, s);
    vfs.mount(parent + "\\PICTURES", pics, /*writable=*/false);
  }
  vfs.mount_host_drives(/*short_names=*/false);
  vfs.set_cwd(guest_dir);
  trace("lane", "state (%s): %s", package_state_name(env, module_path).c_str(),
        pkg.empty() ? "in memory" : pkg.c_str());
}

// The message names in logs.
const char* message_name(uint32_t m) {
  switch (m) {
    case msg::kSelected: return "SELECTED";
    case msg::kDeselected: return "DESELECTED";
    case msg::kPreinitialize: return "PREINITIALIZE";
    case msg::kBlank: return "BLANK";
    case msg::kDrawFrame: return "DRAWFRAME";
    case msg::kClose: return "CLOSE";
    case msg::kButton: return "BUTTON";
    case msg::kKeyDown: return "KEYDOWN";
    case msg::kKeyUp: return "KEYUP";
    case msg::kPaint: return "PAINT";
    case msg::kLButtonDown: return "LBUTTONDOWN";
    case msg::kLButtonHeld: return "LBUTTONHELD";
    case msg::kLButtonUp: return "LBUTTONUP";
    case msg::kMouseMove: return "MOUSEMOVE";
    default: return "?";
  }
}

}  // namespace

Pe32Lane::Pe32Lane() = default;

Pe32Lane::~Pe32Lane() {
  // A lane destroyed without shutdown() (init failed half-way) still frees
  // its runtime; the runtime owns every real GDI object.
  rt_.reset();
  free_fibers();
}

int Pe32Lane::auto_guest_scale(int w, int h) {
  if (w <= 0 || h <= 0) return 1;
  int k = 1;
  while (k < 8 && (int64_t(w) * k < 640 || int64_t(h) * k < 480)) k++;
  return k;
}

int32_t Pe32Lane::guest_x(int32_t x) const {
  if (!guest_screen_) return x;
  return std::clamp<int32_t>(x * guest_scale_ + guest_scale_ / 2, 0, guest_screen_->width() - 1);
}

int32_t Pe32Lane::guest_y(int32_t y) const {
  if (!guest_screen_) return y;
  return std::clamp<int32_t>(y * guest_scale_ + guest_scale_ / 2, 0, guest_screen_->height() - 1);
}

// The guest sees the output's mouse scaled up to its own display.
void Pe32Lane::sync_input() {
  if (!guest_screen_ || !ctx_) return;
  guest_input_ = ctx_->input;
  guest_input_.mouse_x = guest_x(ctx_->input.mouse_x);
  guest_input_.mouse_y = guest_y(ctx_->input.mouse_y);
}

void Pe32Lane::make_runtime(const std::string& path, LaneContext& ctx, Screen& display_screen) {
  const Env& env = ctx.env;
  win32::RuntimeOptions opts;
  opts.heap_size = uint32_t(std::clamp<uint64_t>(env_u64(env, "ADHEAPMB", 128), 16, 1024) << 20);
  opts.call_budget = env_u64(env, "ADCALLBUDGET", 1'000'000'000ull);
  // Deadline loops inside one call must see time move (ABI.md §4).
  ctx.clock.set_read_step_us(uint32_t(env_u64(env, "ADREADSTEPUS", 5)));
  rt_ = std::make_unique<Runtime>(opts, ctx.clock, guest_screen_ ? &guest_input_ : &ctx.input);
  Runtime& rt = *rt_;
  win32::register_all_shims(rt.shims());
  mount_disk(rt, env, path);
  rt.attach_display(display_screen);
  // Where the engine comes from (PACKAGES.md §7.2): a packaged module sees
  // only its own package (module dir, then <root>\ENGINE); anything else
  // keeps the lane's original order (module dir, then <win>\FILES\AD40).
  PackagePaths pkg = locate_package(path);
  std::vector<std::string> search = dll_search_dirs(pkg, env.win_assets_dir());
  for (const std::string& d : search) rt.modules().add_search_dir(d);
  if (tracing("lane")) {
    std::string list;
    for (const std::string& d : search) list += (list.empty() ? "" : "; ") + d;
    trace("lane", "pe32 %s: package %s, module dir %s, engine dir %s, DLL search: %s", module_name_.c_str(),
          pkg.id.c_str(), pkg.module_dir.c_str(), pkg.packaged ? pkg.engine_dir.c_str() : "-", list.c_str());
  }
}

void Pe32Lane::write_controls() {
  Runtime& rt = *rt_;
  auto& mem = rt.mem();
  for (int i = 0; i < 4; i++) {
    int32_t def = 0;
    if (const auto* res = win32::find_module_resource(rt, module_->base, loader::ResId::of(1000),
                                                      loader::ResId::of(uint16_t(i + 1)))) {
      def = control_default(module_->image->resource_data(*res));
    }
    int32_t v = ctx_->input.control(i, def);
    mem.write_u32l(block_ + block::kControls + 4 * uint32_t(i), uint32_t(v));
    trace("lane", "control %d = %d%s", i, v, v == def ? "" : " (ADCVSET)");
  }
}

// A failed init drops the runtime at once: it refers to ctx's screen, clock
// and input, which run_host destroys before the lane (lane.h).
bool Pe32Lane::init(const std::string& module_path, LaneContext& ctx) {
  if (init_impl(module_path, ctx)) return true;
  module_ = nullptr;
  entry_ = 0;
  block_ = 0;
  rt_.reset();
  free_fibers();
  ctx_ = nullptr;
  return false;
}

bool Pe32Lane::init_impl(const std::string& module_path, LaneContext& ctx) {
  ctx_ = &ctx;
  const Env& env = ctx.env;
  std::string path = full_path(module_path);
  module_name_ = file_of(path);
  send_paint_ = env_flag_default(env, "ADPAINT", true);
  // A mid-1990s Pentium (the AD4 box asked for a 486; most ran on a P5-90..133).
  // GDI calls on Win95 cost microseconds each: ~500 instructions' worth. Its
  // PCI display card moved ~25 MB/s: 4 instructions' worth per 8-bit pixel a
  // blit or fill writes (lane.hh "Timing").
  mips_ = env_u64(env, "ADMIPS", 100);
  api_cost_ = env_u64(env, "ADAPICOST", 500);
  pix_cost_ = env_u64(env, "ADPIXCOST", 4);
  max_draws_ = uint32_t(std::clamp<uint64_t>(env_u64(env, "ADMAXDRAWS", 64), 1, 100000));
  // Long calls (lane.hh "Timing"): on with the budget; ADPE32LONGCALLS=0 turns them off.
  long_calls_ = mips_ && !(env.get("ADPE32LONGCALLS") && !env.flag("ADPE32LONGCALLS"));
  // Small screens (lane.hh): the guest gets a display k times the output.
  guest_scale_ = auto_guest_scale(ctx.screen.width(), ctx.screen.height());
  if (const std::string* s = env.get("ADPE32SCALE"); s && !s->empty() && *s != "auto") {
    char* end = nullptr;
    long k = strtol(s->c_str(), &end, 10);
    if (end == s->c_str() || *end || k < 1 || k > 8) {
      log("ADPE32SCALE='%s' is not auto or 1..8; using auto (%d)", s->c_str(), guest_scale_);
    } else {
      guest_scale_ = int(k);
    }
  }
  if (int64_t(ctx.screen.width()) * guest_scale_ > Screen::kMaxDim ||
      int64_t(ctx.screen.height()) * guest_scale_ > Screen::kMaxDim) {
    guest_scale_ = 1;
  }
  if (guest_scale_ > 1) {
    guest_screen_ = std::make_unique<Screen>(ctx.screen.width() * guest_scale_, ctx.screen.height() * guest_scale_);
    sync_input();
    trace("lane", "%dx%d output: the guest display is %dx%d (x%d), averaged down each frame", ctx.screen.width(),
          ctx.screen.height(), guest_screen_->width(), guest_screen_->height(), guest_scale_);
  }

  // A PE32 EXE is a whole program, not a module (program.cc).
  if (is_program_file(path)) return init_program(path, ctx);

  try {
    make_runtime(path, ctx, guest_screen_ ? *guest_screen_ : ctx.screen);
    Runtime& rt = *rt_;
    win32::Display& display = *rt.display();
    // The host audio engine (AUDIO.md §7.1): disabled, every sound API
    // answers as the silent host always did; enabled, DirectSound, ACM,
    // waveOut, MCI and aux exist, and the block gets the sound bit and volume.
    audio::Engine& sound = ctx.audio ? *ctx.audio : audio::null_engine();
    win32::AudioOptions audio_opts;
    audio_opts.deluxe = package_state_name(env, path) == "deluxe";
    win32::attach_audio(rt, sound, audio_opts);
    if (sound.enabled())
      trace("lane", "sound on: volume %d%s", sound.config().volume, audio_opts.deluxe ? ", Deluxe music renames" : "");
    // The desktop the saver started over (display.hh "desktop seed"): the
    // module's own blank decides what survives. Off unless asked for, so
    // unseeded runs stay byte-identical.
    if (const std::string* s = env.get("ADSEEDIMG"); s && !s->empty() && !env.flag("ADNOSEED")) {
      std::string what;
      if (display.seed(*s, &what)) {
        trace("lane", "desktop seed: %s", what.c_str());
      } else {
        log("ADSEEDIMG: %s; the screen starts black", what.c_str());
      }
    }

    module_ = rt.modules().load(path);
    trace("lane", "%s mapped at 0x%08X", module_name_.c_str(), module_->base);
    if (!rt.modules().attach(module_)) {
      log("%s: a DLL failed to initialize (DllMain returned FALSE)", module_name_.c_str());
      census();
      return false;
    }
    // AFTERDAR.SCR 0x404850: _Module@4 (MSVC stdcall decoration) first, then Module.
    entry_ = rt.modules().proc_address(module_->base, "_Module@4");
    if (!entry_) entry_ = rt.modules().proc_address(module_->base, "Module");
    if (!entry_) {
      log("%s exports neither _Module@4 nor Module: not an After Dark 4 module", module_name_.c_str());
      census();
      return false;
    }

    // AD_MODULE32 (ABI.md §2.3), zero-filled with its size stamped.
    auto& mem = rt.mem();
    block_ = rt.heap().alloc(block::kSize, /*zero=*/true);
    if (!block_) throw std::runtime_error("no guest memory for the module block");
    uint32_t flags = block::kPalettized | block::kNotDemo;
    // Sound on with the engine; ADSOUND=1 without one (a lane driven without
    // run_host) sets the bit as it always did, and the engine then fails politely.
    if (sound.enabled() || env.flag("ADSOUND")) flags |= block::kSoundOn;
    int w = display.width(), h = display.height();  // the guest display (small screens: k× the output)
    mem.write_u32l(block_ + block::kCbSize, block::kSize);
    mem.write_u32l(block_ + block::kFlags, flags);
    mem.write_u32l(block_ + block::kHwnd, win32::host_window(rt));
    mem.write_u32l(block_ + block::kHModule, module_->base);  // before message 0 (PSYCHO reads it)
    mem.write_u32l(block_ + block::kHdc, rt.state<win32::GdiTable>().screen_dc());
    // +0x1C: AFTERDAR.SCR's four AD_PALETTE handles. No reader exists in the
    // engine or any AD4 module (ABI.md §2.3), so they stay 0.
    RECT client{0, 0, w, h};
    win32::write_pod(mem, block_ + block::kClient, client);
    // After Dark's volume slider (ADVOLUME) with sound on; else the registry default.
    mem.write_u32l(block_ + block::kVolume, uint32_t(sound.enabled() ? sound.config().volume : 50));
    write_controls();

    if (!deliver(msg::kSelected)) {
      census();
      return false;
    }
    selected_ = true;
    if (!start()) {
      census();
      return false;
    }
    if (long_calls_) make_fibers();
    return true;
  } catch (const GuestError& e) {
    log("%s: %s", module_name_.c_str(), e.what());
    if (rt_) rt_->log_state("guest state");
  } catch (const loader::LoaderError& e) {
    log("%s: cannot load: %s", module_name_.c_str(), e.what());
  } catch (const std::exception& e) {
    log("%s: %s", module_name_.c_str(), e.what());
    if (rt_) rt_->log_state("guest state");
  }
  census();
  return false;
}

uint32_t Pe32Lane::send(uint32_t message, uint32_t param) {
  // Finished waveOut chunks and song ends reach the guest before it runs (AUDIO.md §7.4).
  win32::audio_pump(*rt_);
  auto& mem = rt_->mem();
  mem.write_u32l(block_ + block::kMessage, message);
  mem.write_u32l(block_ + block::kParam, param);
  mem.write_u8(block_ + block::kError, 0);
  uint32_t r = rt_->call_guest(entry_, {block_}, win32::Conv::stdcall_);
  trace("lane", "Module(%u %s, 0x%X) -> %u", message, message_name(message), param, r);
  return r;
}

std::string Pe32Lane::error_text() const {
  std::string s = win32::read_cstr(rt_->mem(), block_ + block::kError, block::kErrorSize);
  return s.empty() ? "(no error text)" : s;
}

bool Pe32Lane::start() {
  // AFTERDAR.SCR 0x4020b5: PREINITIALIZE, and BLANK only if that returned 0.
  // A 3 from either is another restart; give up after a few in a row.
  for (int attempt = 0; attempt < 4; attempt++) {
    uint32_t r = send(msg::kPreinitialize);
    if (r == 0) {
      r = send(msg::kBlank);
      if (r == 0) {
        blanked_ = true;
        return true;
      }
    }
    if (r != msg::kRestart) {
      log("%s: %s", module_name_.c_str(), error_text().c_str());
      return false;
    }
  }
  log("%s: keeps asking to restart", module_name_.c_str());
  return false;
}

bool Pe32Lane::deliver(uint32_t message, uint32_t param) { return apply(message, send(message, param)); }

bool Pe32Lane::apply(uint32_t message, uint32_t r) {
  if (r == 0) return true;
  if (r == msg::kRestart) {
    trace("lane", "%s asked to restart after %s", module_name_.c_str(), message_name(message));
    blanked_ = false;
    return start();
  }
  log("%s: %s (Module(%s) returned %u)", module_name_.c_str(), error_text().c_str(), message_name(message), r);
  return false;
}

void Pe32Lane::on_command(const Command& c) {
  // A program: Caps Lock play (program.cc "Caps Lock").
  if (program_) return program_command(c);
  if (!rt_ || !block_) return;
  sync_input();
  auto& mem = rt_->mem();
  bool events = mem.read_u32l(block_ + block::kModuleFlags) & block::kWantsEvents;
  switch (c.kind) {
    case Command::Kind::set:
      // Control values are plain ints the module reads whenever it likes; no
      // message (ABI.md §2.10.3).
      if (c.a >= 0 && c.a < 4) mem.write_u32l(block_ + block::kControls + 4 * uint32_t(c.a), uint32_t(c.b));
      break;
    case Command::Kind::key:
      // Keys reach the module only when it asked for events (+0x08 bit 0), as
      // AFTERDAR.SCR's runner window did (INTERACTION.md §1.1); otherwise the
      // saver decides whether the key wakes it.
      if (events) pending_.push_back({c.b ? msg::kKeyDown : msg::kKeyUp, uint32_t(c.a), c.seq});
      break;
    case Command::Kind::mouse: {
      // The 1996 host sent 10–15, which the SDK dispatcher ignores; the SDK's
      // own mouse messages are 21/24/27/30 (ABI.md §2.5), with MAKELONG(x, y).
      // The engine also polls GetCursorPos/GetAsyncKeyState itself.
      int32_t x = guest_x(c.a), y = guest_y(c.b);
      bool left = uint32_t(c.c) & kMouseLeft;
      uint32_t pos = (uint32_t(uint16_t(y)) << 16) | uint16_t(x);
      size_t queued = pending_.size();
      if (events && (x != last_mouse_x_ || y != last_mouse_y_)) pending_.push_back({msg::kMouseMove, pos, c.seq});
      if (events && left && !mouse_down_) pending_.push_back({msg::kLButtonDown, pos, c.seq});
      if (events && !left && mouse_down_) pending_.push_back({msg::kLButtonUp, pos, c.seq});
      // A line the module takes as its own without a message (the right button, no move).
      if (events && pending_.size() == queued) eaten_ = std::max(eaten_, c.seq);
      last_mouse_x_ = x;
      last_mouse_y_ = y;
      mouse_down_ = left;
      break;
    }
    default:
      break;
  }
}

// One DRAWFRAME, and after the first one the saver window's initial PAINT:
// AFTERDAR.SCR sends PAINT after the DRAWFRAME that follows a WM_PAINT
// (0x4018b6). False = the module failed (logged).
bool Pe32Lane::draw_once() {
  uint32_t r;
  mid_call_ = true;
  try {
    r = send(msg::kDrawFrame);
  } catch (...) {
    mid_call_ = false;
    throw;
  }
  mid_call_ = false;
  draws_++;
  frame_draws_++;
  if (!apply(msg::kDrawFrame, r)) return false;
  if (send_paint_ && !painted_) {
    painted_ = true;
    if (!deliver(msg::kPaint)) return false;
  }
  return true;
}

// What `pixels` written by blits and fills cost, in instruction-equivalents
// (lane.hh "Timing"): ADPIXCOST each on a display of up to 480 lines; on a
// taller guest display (the saver's Sharp and Custom scales) a pixel is a
// smaller share of the screen, so a full-screen blit costs what it does at
// 480 lines and the Scale setting changes sharpness, not speed.
uint64_t Pe32Lane::pixel_work(uint64_t pixels) const {
  const uint64_t h = rt_ && rt_->display() ? uint64_t(std::max(rt_->display()->height(), 480)) : 480;
  return pix_cost_ * pixels * 480 * 480 / (h * h);
}

// Input messages reach the module between its calls, as the 1996 host's
// message loop pumped them between DRAWFRAMEs: queued KEY/MOUSE messages,
// then (once per frame while the left button is down and the module wants
// events) LBUTTONHELD. Not while a DRAWFRAME is suspended, nor in a frame
// that is still paying off a call's debt. False = the module failed.
bool Pe32Lane::pump_input() {
  while (!pending_.empty()) {
    Pending p = pending_.front();
    pending_.pop_front();
    if (!deliver(p.message, p.param)) return false;
    eaten_ = std::max(eaten_, p.seq);
  }
  if (held_due_) {
    held_due_ = false;
    bool events = rt_->mem().read_u32l(block_ + block::kModuleFlags) & block::kWantsEvents;
    if (events && mouse_down_) {
      uint32_t pos = (uint32_t(uint16_t(last_mouse_y_)) << 16) | uint16_t(last_mouse_x_);
      if (!deliver(msg::kLButtonHeld, pos)) return false;
    }
  }
  return true;
}

// The guest work done since this frame began (instruction-equivalents).
uint64_t Pe32Lane::frame_work() const {
  return (rt_->instructions() - i0_) + api_cost_ * (rt_->api_calls() - a0_) + pixel_work(rt_->pixels_charged() - p0_);
}

// The frame's run is over: its share of the work account is used up, or,
// streamed, 90% of its period of wall time is (the rest is the host's, to
// present).
bool Pe32Lane::frame_over() const {
  if (frame_work() >= frame_credit_) return true;
  return wall_deadline_us_ && VirtualClock::system_wall_us() >= wall_deadline_us_;
}

// The frame's run of DRAWFRAMEs (lane.hh "Timing"): until the frame is over
// or ADMAXDRAWS calls. False when the module failed.
bool Pe32Lane::draw_run() {
  for (;;) {
    if (!pump_input() || !draw_once()) return false;
    if (!blanked_) return true;  // a restart happened; present what it drew
    if (frame_draws_ >= max_draws_ || frame_over()) return true;
  }
}

void CALLBACK Pe32Lane::fiber_main(void* self) { static_cast<Pe32Lane*>(self)->fiber_body(); }

void Pe32Lane::fiber_body() {
  if (program_) {
    program_body();  // switches back for good when the program has ended
    return;
  }
  for (;;) {
    try {
      run_result_ = draw_run() ? Run::frame_done : Run::failed;
    } catch (const win32::AbandonGuestCall&) {
      run_result_ = Run::abandoned;
    } catch (...) {
      fiber_error_ = std::current_exception();
      run_result_ = Run::error;
    }
    mid_call_ = false;
    SwitchToFiber(host_fiber_);
  }
}

// The call hook (win32::Runtime::set_call_hook), on the guest fiber at an API
// call: inside a DRAWFRAME whose frame is over, the frame ends here and the
// call carries on when a later frame resumes it.
void Pe32Lane::on_api_call() {
  if (!mid_call_ || !guest_fiber_ || GetCurrentFiber() != guest_fiber_ || !frame_over()) return;
  suspended_ = true;
  run_result_ = Run::suspended;
  SwitchToFiber(host_fiber_);
  suspended_ = false;
  if (abandon_) throw win32::AbandonGuestCall{};
}

// Before the guest can be called on the host fiber again (CLOSE at shutdown):
// a DRAWFRAME the last frame ended inside is abandoned — the call hook
// throws, and every call level puts the machine back on the way out.
void Pe32Lane::abandon_long_call() {
  if (!suspended_ || !guest_fiber_) return;
  abandon_ = true;
  SwitchToFiber(guest_fiber_);
  abandon_ = false;
  trace("lane", "%s: abandoned the DRAWFRAME in progress", module_name_.c_str());
}

void Pe32Lane::free_fibers() {
  if (guest_fiber_) {
    DeleteFiber(guest_fiber_);
    guest_fiber_ = nullptr;
  }
  if (converted_thread_) {
    ConvertFiberToThread();
    converted_thread_ = false;
  }
  host_fiber_ = nullptr;
}

// Long calls (lane.hh "Timing"): the frames' DRAWFRAMEs run on a fiber of
// their own. Without one (logged) every DRAWFRAME ends its frame, as before.
void Pe32Lane::make_fibers() {
  if (IsThreadAFiber()) {
    host_fiber_ = GetCurrentFiber();
  } else {
    host_fiber_ = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    converted_thread_ = host_fiber_ != nullptr;
  }
  // The host's own 8 MB (the top-level CMakeLists.txt): shims and nested guest
  // calls recurse on it as they do on the main thread.
  if (host_fiber_) guest_fiber_ = CreateFiberEx(0, 8u << 20, FIBER_FLAG_FLOAT_SWITCH, &Pe32Lane::fiber_main, this);
  if (!guest_fiber_) {
    log("%s: no fiber for long calls (error %lu); every DRAWFRAME ends its frame", module_name_.c_str(),
        GetLastError());
    long_calls_ = false;
    free_fibers();
    return;
  }
  rt_->set_call_hook([this] { on_api_call(); });
}

StepResult Pe32Lane::step() {
  if (program_) return step_program();
  if (!rt_ || !entry_) return StepResult::failed;
  try {
    sync_input();
    held_due_ = true;
    // One presented frame = as many back-to-back DRAWFRAMEs as the virtual
    // machine fits into a frame period (lane.hh "Timing"): the frame adds a
    // period's worth to the work account, and DRAWFRAMEs run while it is
    // positive. A call that does more than the account holds is suspended at
    // its first API call past that (long calls) and resumed by the next frame;
    // work it does without an API call leaves a debt the next frames pay off
    // first, presenting the screen as it stands, as a 1996 machine still busy
    // with that call showed it. Work counts are deterministic headless, so so
    // is the number of calls.
    const uint64_t d0 = draws_;
    i0_ = rt_->instructions();
    a0_ = rt_->api_calls();
    p0_ = rt_->pixels_charged();
    frame_draws_ = 0;
    const bool resumed = suspended_;
    uint64_t work = 0;
    if (!mips_) {
      // ADMIPS=0: one DRAWFRAME per presented frame.
      if (!pump_input() || !draw_once()) return StepResult::failed;
    } else {
      const uint32_t period = ctx_->clock.step_us();
      const int64_t budget = int64_t(mips_ * period);
      credit_ = std::min(credit_ + budget, budget);
      if (credit_ > 0) {
        frame_credit_ = uint64_t(credit_);
        wall_deadline_us_ =
            ctx_->clock.mode() == VirtualClock::Mode::realtime ? VirtualClock::system_wall_us() + period * 9 / 10 : 0;
        if (long_calls_) {
          run_result_ = Run::none;
          SwitchToFiber(guest_fiber_);
          if (run_result_ == Run::error) {
            std::exception_ptr e = fiber_error_;
            fiber_error_ = nullptr;
            std::rethrow_exception(e);
          }
          if (run_result_ != Run::frame_done && run_result_ != Run::suspended) return StepResult::failed;
        } else if (!draw_run()) {
          return StepResult::failed;
        }
        work = frame_work();
      }
      credit_ = std::max(credit_ - int64_t(work), -kMaxDebtFrames * budget);
    }
    if (suspended_) long_frames_++;
    trace("pace", "frame %" PRIu64 ": %" PRIu64 " DRAWFRAME(s)%s%s, %" PRIu64 " instructions, %" PRIu64
          " API calls, %" PRIu64 " pixels, work %" PRIu64 ", account %" PRId64, frames_, draws_ - d0,
          resumed ? ", resumed" : "", suspended_ ? ", ended inside one" : "", rt_->instructions() - i0_,
          rt_->api_calls() - a0_, rt_->pixels_charged() - p0_, work, credit_);
    frames_++;
    // Real GDI batches drawing per thread: finish it before the host reads the pixels.
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

LaneStatus Pe32Lane::status() const {
  LaneStatus st;
  if (program_) return program_status();  // program.cc "Caps Lock"
  if (!rt_ || !block_) return st;
  uint32_t f = rt_->mem().read_u32l(block_ + block::kModuleFlags);
  st.interactive = f & block::kWantsEvents;
  st.cursor = f & block::kWantsCursor;
  st.rotate_ok = f & block::kRotateOk;
  st.source = st.interactive ? kStatusSourceAd4 : kStatusSourceNone;
  st.wake = win32::saver_wake_requested(*rt_);
  st.eaten = eaten_;
  return st;
}

void Pe32Lane::shutdown() {
  if (program_) return shutdown_program();
  if (!rt_) return;
  try {
    abandon_long_call();
    // AFTERDAR.SCR 0x402241: CLOSE if blanked, DESELECTED if selected, then FreeLibrary.
    if (!suspended_) {
      if (blanked_ && entry_) send(msg::kClose);
      if (selected_ && entry_) send(msg::kDeselected);
      rt_->modules().detach_all();
    }
  } catch (const std::exception& e) {
    log("%s: while closing: %s", module_name_.c_str(), e.what());
  }
  blanked_ = selected_ = false;
  trace("lane", "%s: %" PRIu64 " DRAWFRAME calls over %" PRIu64 " frames (%" PRIu64 " ended inside one)",
        module_name_.c_str(), draws_, frames_, long_frames_);
  census();
  rt_.reset();
  free_fibers();
}

void Pe32Lane::census() const {
  if (!rt_ || census_done_) return;
  const_cast<Pe32Lane*>(this)->census_done_ = true;
  rt_->shims().print_census(module_name_);
}

// ---- configure mode -------------------------------------------------------------------------------------

ConfigureResult Pe32Lane::configure(const std::string& module_path, LaneContext& ctx, const ConfigureRequest& req,
                                    std::string* json_out) {
  ctx_ = &ctx;
  const Env& env = ctx.env;
  std::string path = full_path(module_path);
  module_name_ = file_of(path);
  auto finish = [&](ConfigureResult r, int shown, const std::string& message,
                    const std::vector<std::string>& written) {
    if (json_out) *json_out = configure_json(r, shown, message, written);
    return r;
  };
  win32::ConfigScript script;
  std::string err;
  if (!script.load_env(env, &err)) {
    log("%s", err.c_str());
    return finish(ConfigureResult::failed, 0, err, {});
  }
  std::string message;
  bool ok = false;
  int shown = 0;
  std::vector<std::string> written;
  try {
    make_runtime(path, ctx, ctx.screen);
    Runtime& rt = *rt_;
    win32::RealUi& ui = win32::real_ui(rt);
    ui.enable(reinterpret_cast<HWND>(uintptr_t(req.owner)), &script);
    trace("lane", "configure %s: button %d, owner 0x%llX", module_name_.c_str(), req.slot,
          (unsigned long long)req.owner);
    module_ = rt.modules().load(path);
    if (!rt.modules().attach(module_)) {
      message = "a DLL failed to initialize (DllMain returned FALSE)";
    } else {
      entry_ = rt.modules().proc_address(module_->base, "_Module@4");
      if (!entry_) entry_ = rt.modules().proc_address(module_->base, "Module");
      if (!entry_) message = "not an After Dark 4 module (no Module export)";
    }
    if (entry_) {
      // ADPAGE 0x9001fbf: a fresh zeroed block; no PREINITIALIZE, BLANK or CLOSE.
      auto& mem = rt.mem();
      block_ = rt.heap().alloc(block::kSize, /*zero=*/true);
      if (!block_) throw std::runtime_error("no guest memory for the module block");
      mem.write_u32l(block_ + block::kCbSize, block::kSize);
      mem.write_u32l(block_ + block::kOwner, ui.owner_guest());
      mem.write_u32l(block_ + block::kHModule, module_->base);
      mem.write_u32l(block_ + block::kVolume, 50);
      write_controls();
      uint32_t r = send(msg::kSelected);
      if (r == 0) {
        r = send(msg::kButton, uint32_t(req.slot));
        if (r == 0) {
          ui.pump_modeless();
          r = send(msg::kDeselected);
        }
      }
      if (r != 0) {
        message = error_text();
        log("%s: %s (Module returned %u)", module_name_.c_str(), message.c_str(), r);
      }
      ok = r == 0;
    }
    rt.modules().detach_all();
  } catch (const GuestError& e) {
    message = e.what();
    log("%s: %s", module_name_.c_str(), e.what());
    if (rt_) rt_->log_state("guest state");
  } catch (const loader::LoaderError& e) {
    message = std::string("cannot load: ") + e.what();
    log("%s: %s", module_name_.c_str(), message.c_str());
  } catch (const std::exception& e) {
    message = e.what();
    log("%s: %s", module_name_.c_str(), e.what());
  }
  if (rt_) {
    shown = win32::real_ui(*rt_).shown();
    win32::close_guest_files(*rt_);  // files the module left open commit now
    written = rt_->vfs().written();
    census();
    rt_.reset();
  }
  if (script.timed_out()) {
    ok = false;
    if (message.empty()) message = "a dialog was still open after the script; it was cancelled";
  }
  ConfigureResult r = !ok ? ConfigureResult::failed : shown > 0 ? ConfigureResult::shown : ConfigureResult::nothing;
  trace("lane", "configure %s: %d dialog(s), %zu file(s) written", module_name_.c_str(), shown, written.size());
  return finish(r, shown, message, written);
}

// ---- small screens --------------------------------------------------------------------------------------

// The guest display averaged k×k → 1 into the output, each average matched to
// the nearest hardware palette entry (squared RGB distance, lowest index on
// ties) at 6 bits per channel, the VGA DAC's precision. A pure function of the
// guest's pixels and palette.
void Pe32Lane::downsample(const Screen& in, Screen& out, int k, NearCache& c) {
  const auto& pal = in.palette();
  if (c.slots.empty() || memcmp(pal.data(), c.pal.data(), sizeof(c.pal)) != 0) {
    c.pal = pal;
    if (c.slots.empty()) c.slots.assign(size_t(1) << 18, 0);
    if (++c.gen >= (1u << 24)) {
      std::fill(c.slots.begin(), c.slots.end(), 0u);
      c.gen = 1;
    }
  }
  if (memcmp(out.palette().data(), pal.data(), sizeof(c.pal)) != 0) out.set_entries(0, 256, pal.data());
  const int n = k * k, w = out.width(), h = out.height();
  for (int y = 0; y < h; y++) {
    uint8_t* dst = out.row(y);
    for (int x = 0; x < w; x++) {
      int r = 0, g = 0, b = 0;
      const uint8_t first = in.row(y * k)[size_t(x) * size_t(k)];
      bool uniform = true;
      for (int j = 0; j < k; j++) {
        const uint8_t* src = in.row(y * k + j) + size_t(x) * size_t(k);
        for (int i = 0; i < k; i++) {
          const RGBQUAD& q = pal[src[i]];
          r += q.rgbRed;
          g += q.rgbGreen;
          b += q.rgbBlue;
          uniform &= src[i] == first;
        }
      }
      if (uniform) {  // one index: itself, exactly
        dst[x] = first;
        continue;
      }
      r = (r + n / 2) / n;
      g = (g + n / 2) / n;
      b = (b + n / 2) / n;
      uint32_t key = (uint32_t(r >> 2) << 12) | (uint32_t(g >> 2) << 6) | uint32_t(b >> 2);
      uint32_t& slot = c.slots[key];
      if ((slot >> 8) != c.gen) {
        // The cell's centre colour, matched against every hardware entry.
        int cr = int(key >> 12) * 4 + 2, cg = int((key >> 6) & 63) * 4 + 2, cb = int(key & 63) * 4 + 2;
        int best = 0, best_d = INT32_MAX;
        for (int e = 0; e < 256; e++) {
          int dr = pal[size_t(e)].rgbRed - cr, dg = pal[size_t(e)].rgbGreen - cg, db = pal[size_t(e)].rgbBlue - cb;
          int dd = dr * dr + dg * dg + db * db;
          if (dd < best_d) best_d = dd, best = e;
        }
        slot = (c.gen << 8) | uint32_t(best);
      }
      dst[x] = uint8_t(slot & 0xFF);
    }
  }
}

}  // namespace adw::pe32

namespace adw {
std::unique_ptr<Lane> make_pe32_lane() { return std::make_unique<pe32::Pe32Lane>(); }
}  // namespace adw
