// The 7th Level Runtime Player protocol (protocol.hh, lane.hh "7th Level
// scenes"): a scene of 7th Level's Runtime Player — a 7th Level resource
// file, Take Your Best Shot's BESTACT\*.BIN — run as Take Your Best Shot's
// desktop program BESTSHOT.EXE ran it: the real player, PLYMPTON.EXE, with
// its own libraries (PLYMU001, PLYMA001, PLYMS001, PLYMR001, which pull in
// DISPDIB), unchanged, as the runtime's task (win16/modules16.hh "Tasks").
// BESTSHOT.EXE started every scene the same way (6:00E6): a 0x6C-byte block
// that says the mode (GlobalAlloc(GMEM_SHARE | GMEM_MOVEABLE |
// GMEM_ZEROINIT)), WinExec("<player> <scene> /p<hBlock>"), then, the player
// running, SendMessage(its window, 0x783, BESTSHOT's main window, 0); to stop
// one it sent 0x783 with 0 and posted WM_CLOSE (6:005E). The host stands in
// for BESTSHOT.EXE: the block, the command line, the message and a window
// for the player to know (user16_host_window). The task is the protocol's
// one call, as scr's is: every frame ends inside it. The Desktop
// Pythonizer's scenes ("7L" 01 00, product "Python": INTERACT\*.BIN and
// three in PYTHON, and its Desktop Extras' FOOT.BIN and PENGUINS.BIN) run
// the same way with its own, older player, PYTHON.EXE (PYTHU001, PYTHA001,
// PYTHS001, PYTHR001), as its desktop program PYTHIZER.EXE started them
// (8:00E6: the same block, command line, message and stop); the scene's
// product picks the player and the stand-in's windows (package.hh
// SeventhProduct).
#include <windows.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/log.h"
#include "adw/core/text.h"
#include "ne16/protocol.hh"
#include "win16/dos16.hh"
#include "win16/global_heap.hh"
#include "win16/modules16.hh"
#include "win16/runtime16.hh"
#include "win16/shim_families16.hh"
#include "win32/vfs.hh"

namespace adw::ne16 {

using win16::GuestError16;
using win16::Runtime16;

namespace {

std::string file_of(const std::string& p) {
  size_t s = p.find_last_of("\\/");
  return s == std::string::npos ? p : p.substr(s + 1);
}

std::string lower(std::string s) {
  for (char& c : s) c = char(tolower(uint8_t(c)));
  return s;
}

bool host_file(const std::string& p) {
  DWORD a = GetFileAttributesW(widen(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// BESTSHOT.EXE's message to its player (6:03AD, 6:0073): wParam its main
// window when the scene starts, 0 when it stops it (PYTHIZER.EXE's 8:03D0,
// 8:0073 the same).
constexpr uint16_t kMsgParent = 0x783;
// BESTSHOT.EXE's main window (4:07A1): class and title "Take Your Best Shot",
// WS_OVERLAPPEDWINDOW. The block's +0x6A is its other top-level window
// (4:07EE), its UI: class "Take Your Best Shot " (the trailing space is
// BESTSHOT's), WS_POPUP | WS_CLIPCHILDREN. PYTHIZER.EXE's: "Pythonizer"
// (5:0CDD) and the UI "Pythonizer " (5:0D2A, class and title), the same
// styles (SeventhProduct).
// The scenes the installer's script listed under [Games] (7THLEVEL.INI).
constexpr const char* kGames[] = {"headshot.bin", "hotshot.bin", "lineshot.bin"};

// The 7th Level files of `dir` whose header names the product, but the
// desktop program's own, lower case, in name order.
std::vector<std::string> scenes_of(const std::string& dir, const SeventhProduct& product) {
  WIN32_FIND_DATAW fd;
  std::vector<std::string> names;
  HANDLE f = FindFirstFileW(widen(dir + "\\*.BIN").c_str(), &fd);
  if (f != INVALID_HANDLE_VALUE) {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      const std::string n = lower(narrow(fd.cFileName));
      std::string p;
      if (n != product.own_scene && seventh_header(dir + "\\" + narrow(fd.cFileName), &p, nullptr) &&
          _stricmp(p.c_str(), product.product) == 0) {
        names.push_back(n);
      }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace

std::string seventh_block_bytes(const SeventhBlock& b) {
  std::string d(kSeventhBlockSize, '\0');
  auto put = [&](size_t at, uint16_t v) {
    d[at] = char(v & 0xFF);
    d[at + 1] = char(v >> 8);
  };
  put(0x00, kSeventhBlockSize);
  put(0x02, uint16_t(b.mode));
  put(0x12, b.mute);
  put(0x14, b.not_interactive);
  if (b.mode == SeventhMode::saver) put(0x16, b.password);
  const std::string t = b.title.substr(0, 0x4F);
  memcpy(&d[0x1A], t.data(), t.size());
  put(0x6A, b.ui_window);
  return d;
}

SeventhBlock seventh_run_block(const Env& env, const std::string& title, bool caps, bool game) {
  SeventhBlock b;
  b.title = title;
  if (env.flag("ADWALLPAPER")) {
    // The living wallpaper BESTSHOT.EXE installed (4:05D0 → 7:03C0 with no
    // scene named: [BestShotControls] CurrentLivingWallpaper): its flag 0,
    // so mode 1 (7:04E9); +0x12 LivingWallpaperMute == YES, which the
    // installer left unset; 7:04F7 pushes 1 for the caller's flag, so +0x14 0.
    b.mode = SeventhMode::wallpaper;
    b.not_interactive = 0;
  } else if (caps && game) {
    // A game as BESTSHOT.EXE's Games page started it (2:06CA): mode 6, its
    // caller's mute (GamesMute, unset), 1 for the flag (+0x14 0).
    b.mode = SeventhMode::game;
    b.not_interactive = 0;
  } else {
    // The screen saver as BESTSHOT.EXE's /s started it (6:0490 → 6:068A)
    // with the controls its installer left: [BestShotControls] has no
    // ScreenSaverMute, ScreenSaverInteractive or ScreenSaverPasswordOn, so
    // mode 3, sound on, not interactive, no password. Caps Lock on: as with
    // ScreenSaverInteractive=YES, the interactive saver (+0x14 0).
    b.mode = SeventhMode::saver;
    b.not_interactive = caps ? 0 : 1;
  }
  return b;
}

bool seventh_is_game(const std::string& file) {
  const std::string n = lower(file_of(file));
  return std::any_of(std::begin(kGames), std::end(kGames), [&](const char* g) { return n == g; });
}

bool seventh_header(const std::string& host_path, std::string* product, std::string* title) {
  HANDLE h = CreateFileW(widen(host_path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  char head[0x60] = {};
  DWORD got = 0;
  if (!ReadFile(h, head, sizeof(head), &got, nullptr)) got = 0;
  CloseHandle(h);
  if (got < 6 || !seventh_level_header(head)) return false;
  // "7L" 02 00 (or 01 00), a length byte, the product (NUL), the title (to a NUL or 0xFB).
  size_t i = 5;
  std::string p, t;
  while (i < got && head[i]) p += head[i++];
  i++;
  while (i < got && head[i] && uint8_t(head[i]) != 0xFB) t += head[i++];
  if (product) *product = p;
  if (title) *title = t;
  return true;
}

std::string seventh_command_tail(const std::string& guest_scene, uint16_t hblock) {
  char hex[8];
  snprintf(hex, sizeof(hex), "%X", unsigned(hblock));
  return " " + guest_scene + " /p" + hex;
}

std::string seventh_guest_scene(const Ne16Layout& layout, const SeventhProduct& product, const SeventhPlayer& player) {
  const std::string name = file_of(layout.module_path);
  if (player.addon) return std::string(product.install_dir) + "\\" + win16::upper16(name);
  return std::string(kSeventhCdDrive) + "\\" + win16::upper16(file_of(layout.module_dir)) + "\\" + name;
}

// The guest's disk (INTERACTION.md §7.2): one Windows 3.1 machine with the
// product installed from its CD in the drive — Take Your Best Shot's:
//   C:\7THLEVEL\BESTSHOT  the player's folder (PLYMPTON.EXE and its DLLs:
//                         the installer's 0:, its default destination) and
//                         the current directory, under a copy-on-write upper
//                         layer <state>\<package>\BESTSHOT
//   D:\BESTACT            the scenes, on the CD (the installer's 1:bestact):
//                         read-only, as a CD is
//   C:\WINDOWS            over the package's windows dir when it has one and
//                         the runtime's virtual seed files, under
//                         <state>\<package>\WINDOWS: 7THLEVEL.INI, which the
//                         player and its libraries read and write
//   C:\WINDOWS\SYSTEM     the engine dir (none), read-only
//   H:\<L>\…              the host's drives, read-only, 8.3 names
// The Desktop Pythonizer's: C:\7THLEVEL\PYTHON over the package's PYTHON
// (PYTHON.EXE and its DLLs), D:\INTERACT and D:\PYTHON (its CD's scenes:
// 1:interact, 1:python); with its Desktop Extras' scene, the Pythonizer's
// package is the CD and the player's folder, and the Extras' scenes are in
// C:\7THLEVEL\PYTHON beside PYTHON.EXE (the floppy's 0:), read from the
// add-on's folder. Without ADSTATE every upper layer is memory. The profile
// seeds (win16/dos16.hh seed_seventh_level): 7THLEVEL.INI as the installers
// wrote it, WIN.INI's desktop.
void mount_seventh_disk(Runtime16& rt, const Env& env, const Ne16Layout& layout, const SeventhProduct& product,
                        const SeventhPlayer& player) {
  const win16::Runtime16Options& o = rt.options();
  win32::Vfs& vfs = rt.vfs();
  const std::string pkg = package_state_dir(env, layout.module_path);
  const size_t cut = player.dir.find_last_of("\\/");
  const std::string cd_root = cut == std::string::npos ? std::string(".") : player.dir.substr(0, cut);
  if (env.state_persistent()) vfs.set_state_root(env.state_root);
  vfs.mount_overlay(o.windows_dir, layout.windows_dir, pkg.empty() ? "" : pkg + "\\WINDOWS");
  vfs.mount_overlay(o.guest_dir, player.dir, pkg.empty() ? "" : pkg + "\\" + product.player_dir);
  // The CD: the scene's folder (not an add-on's), then the product's other
  // scene folders from the player's package.
  std::vector<std::pair<std::string, std::string>> cd;  // guest folder, host folder
  if (!player.addon) cd.emplace_back(win16::upper16(file_of(layout.module_dir)), layout.module_dir);
  for (const char* const* f = product.cd_folders; *f; ++f) {
    const std::string host = cd_root + "\\" + *f;
    const DWORD a = GetFileAttributesW(widen(host).c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (std::none_of(cd.begin(), cd.end(), [&](const auto& c) { return c.first == *f; })) cd.emplace_back(*f, host);
  }
  std::string cd_text;
  for (const auto& [guest, host] : cd) {
    vfs.mount(std::string(kSeventhCdDrive) + "\\" + guest, host, /*writable=*/false);
    cd_text += (cd_text.empty() ? "" : ", ") + std::string(kSeventhCdDrive) + "\\" + guest + " over " + host;
  }
  vfs.mount(o.system_dir, layout.engine_dir, /*writable=*/false);
  vfs.mount_host_drives(/*short_names=*/true);
  vfs.set_cwd(o.guest_dir);
  rt.modules().add_search_dir(player.dir);
  win16::SeventhLevelSeeds seeds;
  seeds.product = product.product;
  seeds.install_dir = o.guest_dir;
  seeds.cd = kSeventhCdDrive;
  // The scenes the installers' scripts listed: the CD's, here the 7th Level
  // files of the product in its folders but the desktop program's own
  // (BESTSHOT.BIN, HERALD.BIN), in name order, folder by folder; an add-on's
  // in the player's folder.
  for (const auto& [guest, host] : cd) {
    for (const std::string& n : scenes_of(host, product)) {
      const std::string key = std::string(kSeventhCdDrive) + "\\" + lower(guest) + "\\" + n;
      seeds.scenes.push_back(key);
      if (product.max_mode >= uint16_t(SeventhMode::game) &&
          std::find(std::begin(kGames), std::end(kGames), n) != std::end(kGames)) {
        seeds.games.push_back(key);
      }
    }
  }
  if (player.addon) {
    for (const std::string& n : scenes_of(layout.module_dir, product)) {
      std::vector<uint8_t> bytes;
      HANDLE h = CreateFileW(widen(layout.module_dir + "\\" + n).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, 0, nullptr);
      if (h == INVALID_HANDLE_VALUE) continue;
      LARGE_INTEGER size{};
      DWORD got = 0;
      if (GetFileSizeEx(h, &size) && size.QuadPart < LONGLONG(win32::Vfs::kMaxFileSize)) {
        bytes.resize(size_t(size.QuadPart));
        if (!ReadFile(h, bytes.data(), DWORD(bytes.size()), &got, nullptr) || got != bytes.size()) bytes.clear();
      }
      CloseHandle(h);
      if (bytes.empty()) continue;
      vfs.add_virtual_file(o.guest_dir + "\\" + win16::upper16(n), std::move(bytes));
      seeds.scenes.push_back(o.guest_dir + "\\" + n);
    }
  }
  win16::seed_seventh_level(rt, seeds);
  trace("lane", "disk: %s over %s, %s (read-only)%s, C:\\WINDOWS over %s%s; seeds: 7THLEVEL.INI (%s) with %zu scene(s)",
        o.guest_dir.c_str(), player.dir.c_str(), cd_text.c_str(),
        player.addon ? (", the add-on's scenes in " + o.guest_dir + " from " + layout.module_dir).c_str() : "",
        pkg.empty() ? "memory (no ADSTATE)" : pkg.c_str(),
        layout.windows_dir.empty() ? "" : (" and " + layout.windows_dir).c_str(), product.product, seeds.scenes.size());
}

namespace {

class SeventhProtocol : public Protocol16 {
 public:
  explicit SeventhProtocol(const Ne16Layout& layout) : layout_(layout), module_name_(file_of(layout.module_path)) {
    // The scene's header names the product, the product its player (package.hh).
    if (!seventh_header(layout_.module_path, &product_name_, &title_)) return;
    header_ = true;
    product_ = seventh_product(product_name_);
    if (product_) player_ = seventh_find_player(layout_, *product_);
  }

  const char* name() const override { return "7thlevel"; }
  void configure_runtime(win16::Runtime16Options& opts, LaneContext& ctx) override;
  void mount(Runtime16& rt, const Env& env) override {
    env_ = &env;
    if (product_ && !player_.dir.empty()) mount_seventh_disk(rt, env, layout_, *product_, player_);
  }
  bool load(Runtime16& rt, uint16_t hwnd, uint16_t hdc, LaneContext& ctx) override;
  Call call() override;
  // The screen saver and the living wallpaper: input ends the run (the
  // host's saver) or there is none, so the player gets none, as a Windows
  // 3.1 saver's program got none of the waking input. A game, or the
  // interactive screen saver, started for Caps Lock: keys and the mouse are
  // the player's (user16_set_app_input) — not while it is being stopped.
  bool takes_key_messages() const override { return plays_now(); }
  bool takes_mouse_messages() const override { return plays_now(); }
  // Caps Lock on (and the scene has a game or the interactive saver, and
  // its game has not ended since): the run plays from the CAPS line on, the
  // player starting again meanwhile.
  bool plays() const override { return wants_caps(); }
  // A game shows the cursor while its own one is on (ShowCursor's count);
  // the interactive saver shows it (1:77BA hides it only when not interactive).
  bool plays_cursor() const override { return wants_caps() && win16::user16_cursor_shown(*rt_); }
  bool runs_as_task() const override { return true; }
  bool close_suspended() override;
  void unload() override;
  std::string error_text() const override { return stop_text_; }
  bool check_button(int slot, std::string* why) override;
  void configure_button_runtime(win16::Runtime16Options& opts, const Env& env) override;
  Button button(Runtime16& rt, int slot, uint16_t owner16, LaneContext& ctx) override;
  void close() override;

 private:
  void run();
  // BESTSHOT.EXE's WinExec of the player for the run's mode (6:00E6): the
  // block, the task with its command tail. False when it could not (logged).
  bool start_player();
  // What a task's end left (user16_end_task, the player's and its
  // libraries' modules, the memory they had), before start_player again.
  void end_player();
  // BESTSHOT.EXE's EnumWindows and SendMessage after its WinExec returned
  // (user16_set_receive_hook): at the player's first message call. At each
  // later one: whether Caps Lock changed what the run is (restart_).
  void on_receive();
  // Whether the run is to be the game or the interactive saver: Caps Lock
  // on, not the living wallpaper, and not after its game ended by itself
  // under this Caps Lock.
  bool wants_caps() const;
  // Whether the player running now is that one, and takes the input.
  bool plays_now() const { return caps_ && !closing_ && !restart_ && !ended_; }

  // The guest's install folder: the product's, else Take Your Best Shot's.
  const char* install_dir() const {
    return product_ ? product_->install_dir : seventh_product("BestShot")->install_dir;
  }

  Ne16Layout layout_;
  std::string module_name_;
  // The scene's header was read; its product and the product's player (its folder "" when none).
  bool header_ = false;
  const SeventhProduct* product_ = nullptr;
  SeventhPlayer player_;
  const Env* env_ = nullptr;
  Runtime16* rt_ = nullptr;
  uint16_t hinstance_ = 0, hblock_ = 0, main_ = 0, ui_ = 0, player_wnd_ = 0;
  bool started_ = false, ended_ = false, closing_ = false, told_ = false;
  // The scene's: a game ([Games]), the living wallpaper (ADWALLPAPER=1).
  bool game_ = false, wallpaper_ = false;
  // The running player was started for Caps Lock; Caps Lock changed it (it
  // is being stopped, to start again); its game ended by itself (until Caps
  // Lock goes off).
  bool caps_ = false, restart_ = false, game_over_ = false;
  std::string title_, product_name_;
  SeventhBlock block_;
  // The guest's blocks before the player started (end_player frees the rest).
  std::vector<uint16_t> blocks_before_;
  uint32_t starts_ = 0;
  int exit_code_ = 0;
  std::string stop_text_;
};

void SeventhProtocol::configure_runtime(win16::Runtime16Options& opts, LaneContext& ctx) {
  opts.guest_dir = install_dir();
  // Sound off is a PC without a sound card (no wave device; ADSOUNDDEV still
  // decides when set): PLYMA001 then plays nothing and the scene runs on. The
  // silent device would not do — it never answers waveOutWrite (AUDIO.md §3,
  // invariant 4), and PLYMA001 waits for its first buffer's WHDR_DONE without
  // an API call (1:1887).
  opts.sound_device = ctx.audio && ctx.audio->enabled();
}

bool SeventhProtocol::load(Runtime16& rt, uint16_t hwnd, uint16_t hdc, LaneContext& ctx) {
  (void)hwnd;  // the player makes its own window
  (void)hdc;
  rt_ = &rt;
  env_ = &ctx.env;
  if (!header_) {
    log("%s: not a 7th Level resource file", module_name_.c_str());
    return false;
  }
  if (!product_) {
    log("%s: no 7th Level Runtime Player for the product \"%s\"", module_name_.c_str(), product_name_.c_str());
    return false;
  }
  const std::string player_host = player_.dir + "\\" + product_->player;
  if (player_.dir.empty() || !host_file(player_host)) {
    log("%s: no 7th Level Runtime Player (%s\\%s, for the product %s, beside the scene's folder or in a package "
        "beside its own)",
        module_name_.c_str(), product_->player_dir, product_->player, product_->product);
    return false;
  }
  // [Games]: Best Shot's; the Pythonizer's player takes no mode 6.
  game_ = product_->max_mode >= uint16_t(SeventhMode::game) && seventh_is_game(module_name_);
  wallpaper_ = ctx.env.flag("ADWALLPAPER");
  // The libraries ran under Windows 3.1's loader: WEP when they go.
  rt.modules().set_call_wep(true);
  // The desktop program's two top-level windows: the main one, which 0x783
  // names, and the UI, the block's +0x6A (neither shown: the saver covers them).
  main_ = win16::user16_host_window(rt, product_->main_class, product_->main_class, WS_OVERLAPPEDWINDOW);
  ui_ = win16::user16_host_window(rt, product_->ui_class, product_->ui_title, WS_POPUP | WS_CLIPCHILDREN);
  win16::user16_set_app_task(rt, true);
  return start_player();
}

bool SeventhProtocol::start_player() {
  Runtime16& rt = *rt_;
  const std::string player_host = player_.dir + "\\" + product_->player;
  caps_ = wants_caps();
  restart_ = ended_ = told_ = false;
  player_wnd_ = 0;
  blocks_before_ = rt.global().guest_handles();
  // The block (6:00E6), the player's to read at its start (1:6EB0).
  block_ = seventh_run_block(*env_, title_, caps_, game_);
  block_.ui_window = ui_;
  hblock_ = rt.global().alloc(kSeventhBlockFlags, kSeventhBlockSize);
  const uint32_t fp = hblock_ ? rt.global().lock(hblock_) : 0;
  if (!fp) {
    log("%s: no memory for the player's block", module_name_.c_str());
    return false;
  }
  const std::string bytes = seventh_block_bytes(block_);
  rt.write_bytes(fp, bytes.data(), bytes.size());
  rt.global().unlock(hblock_);
  // WinExec("<player> <scene> /p<hBlock>", SW_SHOWMAXIMIZED): the PSP's tail.
  const std::string scene = seventh_guest_scene(layout_, *product_, player_);
  const std::string tail = seventh_command_tail(scene, hblock_);
  const std::string guest = std::string(product_->install_dir) + "\\" + product_->player;
  uint16_t err = 0;
  win16::Module16* m = rt.modules().load_task(player_host, guest, tail, &err);
  if (!m || !rt.modules().task()) {
    log("%s: cannot load the 7th Level Runtime Player as a task (error %u)", module_name_.c_str(), err);
    return false;
  }
  hinstance_ = m->hinstance;
  rt.modules().set_task_cmd_show(SW_SHOWMAXIMIZED);  // BESTSHOT's WinExec(…, SW_SHOWMAXIMIZED)
  // A game's and the interactive saver's windows take the input (plays_now).
  win16::user16_set_app_input(rt, caps_);
  win16::user16_set_receive_hook(rt, [this] { on_receive(); });
  starts_++;
  const std::string nth = starts_ > 1 ? ", start " + std::to_string(starts_) : "";
  trace("lane", "%s: package %s, kind 7thlevel, \"%s\" (product %s), the player %s (%s), mode %u%s, block %04X, "
        "command tail \"%s\"%s",
        module_name_.c_str(), layout_.packaged ? layout_.package_id.c_str() : "legacy", title_.c_str(),
        product_name_.c_str(),
        guest.c_str(), m->image->description().c_str(), unsigned(block_.mode),
        block_.mode == SeventhMode::saver && !block_.not_interactive ? " (interactive)" : "", hblock_, tail.c_str(),
        nth.c_str());
  return true;
}

bool SeventhProtocol::wants_caps() const { return rt_ && rt_->input().caps && !wallpaper_ && !game_over_; }

// WinExec returned once the player first yielded, at its first message call;
// then BESTSHOT.EXE looked for its window (EnumWindows: a top-level window
// whose GWW_HINSTANCE is the player's) and sent it 0x783 with its main window,
// which the player received there. No window yet: BESTSHOT found none and
// sent nothing.
void SeventhProtocol::on_receive() {
  if (told_) {
    // Caps Lock off after a game that ended by itself: the next Caps Lock
    // starts it again.
    if (game_over_ && !rt_->input().caps) game_over_ = false;
    // Caps Lock changed what the run is: the player stops as BESTSHOT.EXE
    // stopped one (6:005E: 0x783 with 0, sent now, then WM_CLOSE), and
    // call() starts it again in the new mode once it has ended.
    if (closing_ || restart_ || wants_caps() == caps_) return;
    const uint16_t w = player_wnd_ ? player_wnd_ : win16::user16_main_window(*rt_, hinstance_);
    if (!w) return;
    restart_ = true;
    win16::user16_set_app_input(*rt_, false);
    win16::user16_send_message(*rt_, w, kMsgParent, 0, 0);
    win16::user16_post_message(*rt_, w, WM_CLOSE, 0, 0);
    trace("lane", "%s: Caps Lock %s: 0x783 (0) sent and WM_CLOSE posted to the player's window %04X, to start it as %s",
          module_name_.c_str(), rt_->input().caps ? "on" : "off", w,
          wants_caps() ? (game_ ? "the game (mode 6)" : "the interactive screen saver") : "the screen saver");
    return;
  }
  told_ = true;
  player_wnd_ = win16::user16_main_window(*rt_, hinstance_);
  if (!player_wnd_) {
    log("%s: the player had no window at its first message call: no 0x783", module_name_.c_str());
    return;
  }
  const uint32_t r = win16::user16_send_message(*rt_, player_wnd_, kMsgParent, main_, 0);
  trace("lane", "%s: SendMessage(%04X, 0x783, %04X, 0) -> %08X (the desktop program's, after WinExec)", module_name_.c_str(),
        player_wnd_, main_, r);
}

void SeventhProtocol::run() {
  ended_ = false;
  stop_text_.clear();
  try {
    uint32_t r = rt_->modules().run_task();
    exit_code_ = int(r & 0xFFFF);
    stop_text_ = "the player's start returned (" + std::to_string(exit_code_) + ")";
  } catch (const GuestError16& e) {
    if (e.kind() != GuestError16::Kind::exit) throw;
    stop_text_ = std::string("the player ended: ") + e.what();
  }
  ended_ = true;
  const std::string box = win16::user16_last_message_box(*rt_);
  if (!box.empty()) stop_text_ += "; its last message box: " + box;
}

// The task, started at the first call; it returns only when the player has
// ended (WM_QUIT, its MessageBox for an error first), which stops the run.
Protocol16::Call SeventhProtocol::call() {
  if (!started_) {
    started_ = true;
    for (;;) {
      run();
      if (closing_) break;
      // A game (or the interactive saver) that ended by itself — its own
      // quit, Shift+Esc — went back to BESTSHOT.EXE (1:267A): here to the
      // screen saver, not interactive, until Caps Lock goes off and on.
      if (!restart_ && caps_) {
        game_over_ = true;
        trace("lane", "%s: the %s ended by itself (%s): the screen saver again", module_name_.c_str(),
              game_ ? "game" : "interactive screen saver", stop_text_.c_str());
      } else if (!restart_) {
        break;
      }
      end_player();
      if (!start_player()) break;
    }
  }
  Call c;
  c.kind = Call::Kind::stop;
  c.code = exit_code_;
  return c;
}

// At shutdown: the player stops as BESTSHOT.EXE stopped it (6:005E) —
// SendMessage(player, 0x783, 0, 0), which the player received at its next
// message call (the receive hook), then PostMessage(player, WM_CLOSE) —; the
// lane resumes the task to that end (Ne16Lane::wind_down).
bool SeventhProtocol::close_suspended() {
  if (!rt_ || !started_ || ended_) return false;
  win16::user16_set_app_input(*rt_, false);
  const uint16_t w = player_wnd_ ? player_wnd_ : win16::user16_main_window(*rt_, hinstance_);
  if (!w || !win16::user16_window_exists(*rt_, w)) return false;
  closing_ = true;
  bool sent = false;
  win16::user16_set_receive_hook(*rt_, [this, w, sent]() mutable {
    if (sent) return;
    sent = true;
    win16::user16_send_message(*rt_, w, kMsgParent, 0, 0);
  });
  win16::user16_post_message(*rt_, w, WM_CLOSE, 0, 0);
  trace("lane", "%s: closing: 0x783 (0) sent and WM_CLOSE posted to the player's window %04X", module_name_.c_str(), w);
  return true;
}

void SeventhProtocol::end_player() {
  Runtime16& rt = *rt_;
  // Everything loaded here is the player's: it and its libraries.
  std::set<uint16_t> inst;
  for (const auto& mod : rt.modules().all()) {
    if (!mod->system) inst.insert(mod->hinstance);
  }
  win16::user16_set_receive_hook(rt, nullptr);
  win16::user16_end_task(rt, [&](uint16_t hinst, uint32_t proc) {
    if (hinst && inst.count(hinst)) return true;
    const win16::Module16* m = proc ? rt.modules().containing(uint16_t(proc >> 16)) : nullptr;
    return m && !m->system;
  });
  rt.modules().free_all();
  // The memory the task and its libraries had (Windows freed a task's when
  // it ended, a library's with it), the block among it.
  const std::vector<uint16_t> now = rt.global().guest_handles();
  size_t freed = 0;
  for (uint16_t h : now) {
    if (!std::binary_search(blocks_before_.begin(), blocks_before_.end(), h) && rt.global().free(h) == 0) freed++;
  }
  hblock_ = 0;
  trace("lane", "%s: the player ended (%s); its modules and %zu block(s) freed", module_name_.c_str(), stop_text_.c_str(),
        freed);
}

void SeventhProtocol::unload() {
  if (!rt_) return;
  trace("lane", "%s: %s; %" PRIu64 " message waits", module_name_.c_str(),
        ended_ ? stop_text_.c_str() : started_ ? "the player was still running" : "the player never started",
        win16::user16_app_waits(*rt_));
}

void SeventhProtocol::close() {
  if (!rt_) return;
  win16::user16_set_receive_hook(*rt_, nullptr);
  // BESTSHOT.EXE freed the block once WinExec returned (6:046F); the player
  // had copied it at its start. Here it goes with the run.
  if (hblock_) rt_->global().free(hblock_);
  hblock_ = 0;
}

// No buttons: Take Your Best Shot's settings were its desktop program's.
bool SeventhProtocol::check_button(int slot, std::string* why) {
  *why = "a 7th Level scene has no buttons (control " + std::to_string(slot) + ")";
  return false;
}

void SeventhProtocol::configure_button_runtime(win16::Runtime16Options& opts, const Env& env) {
  (void)env;
  opts.guest_dir = install_dir();
}

Protocol16::Button SeventhProtocol::button(Runtime16& rt, int slot, uint16_t owner16, LaneContext& ctx) {
  (void)rt;
  (void)slot;
  (void)owner16;
  (void)ctx;
  Button out;
  out.message = "a 7th Level scene has no buttons";
  return out;
}

}  // namespace

std::unique_ptr<Protocol16> make_seventh_protocol(const Ne16Layout& layout) {
  return std::make_unique<SeventhProtocol>(layout);
}

}  // namespace adw::ne16
