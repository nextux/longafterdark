// pe32 lane tests. Unit: control defaults from type-1000 records (ABI.md
// §2.10.2), and which package a module path belongs to with the DLL search it
// gets (PACKAGES.md §7.1/§7.2). --assets <adhostwin>: runs real AD4 modules
// headless through the whole lane (DllMains, SELECTED/PREINITIALIZE/BLANK,
// DRAWFRAME, CLOSE) and checks they finish cleanly, deterministically, and
// with an empty census of unimplemented APIs, and that a packaged copy of a
// Deluxe module without its engine does not borrow Deluxe's, and that the
// desktop seed (ADSEEDIMG) reaches SHADOW and nothing that blanks, and that
// the DRAWFRAME pacing (blit costs, long calls) keeps SWIRLING at a 1996
// machine's rate at any Scale and presents PSYCHO's first call as it draws; skips (77)
// when the imported assets are absent. --packages <adhostwin>: the same for every pe32 module of
// an imported After Dark 10th Anniversary package (win\packages\ad10), plus
// its package-only DLL search, its modules that are byte-identical to
// Deluxe's giving Deluxe's frames, and the long-name MIDI fix-ups resolving
// through the lane's C:\AFTERDRK; skips (77) without such a root.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "adw/core/audio.h"
#include "adw/core/clock.h"
#include "adw/core/env.h"
#include "adw/core/lane.h"
#include "adw/core/log.h"
#include "adw/core/protocol.h"
#include "adw/core/screen.h"
#include "adw/core/text.h"
#include "check.h"
#include "test_paths.h"
#include "pe32/controls.hh"
#include "pe32/lane.hh"
#include "pe32/package.hh"
#include "win32/guest.hh"
#include "win32/runtime.hh"

using adw::pe32::control_default;
using adw::pe32::dll_search_dirs;
using adw::pe32::locate_package;

namespace {

std::string record(uint16_t kind, int16_t def, size_t size) {
  std::string r(size, '\0');
  r[0] = char(kind & 0xFF);
  r[1] = char(kind >> 8);
  r[0x18] = char(uint16_t(def) & 0xFF);
  r[0x19] = char(uint16_t(def) >> 8);
  return r;
}
void put16(std::string& r, size_t off, uint16_t v) {
  r[off] = char(v & 0xFF);
  r[off + 1] = char(v >> 8);
}

}  // namespace

TEST(control_numeric_slider_clamps) {
  std::string r = record(2, 150, 0x34);
  put16(r, 0x30, 1);
  put16(r, 0x32, 100);
  CHECK_EQ(control_default(r), 100);
  put16(r, 0x18, uint16_t(-5));
  CHECK_EQ(control_default(r), 1);
}

TEST(control_popup_and_checkbox) {
  std::string p = record(3, 7, 0x20);
  put16(p, 0x16, 3);
  CHECK_EQ(control_default(p), 2);
  CHECK_EQ(control_default(record(5, 9, 0x20)), 1);
  CHECK_EQ(control_default(record(4, 9, 0x20)), 0);
  CHECK_EQ(control_default(std::string(4, '\0')), 0);  // too short
}

TEST(control_string_slider_prepends_zero) {
  // 2 labels (16 bytes each) at +0x20, then values 10, 20.
  std::string r = record(1, 15, 0x20 + 32 + 4);
  put16(r, 0x16, 2);
  put16(r, 0x40, 10);
  put16(r, 0x42, 20);
  CHECK_EQ(control_default(r), 10);
  put16(r, 0x18, 5);
  CHECK_EQ(control_default(r), 0);  // the prepended 0 stop
}

// ---- packages (PACKAGES.md §7.1/§7.2) ----------------------------------------

TEST(package_of_a_packaged_module) {
  auto p = locate_package("D:\\ad\\win\\packages\\ad10\\AD10TH\\HALLOFFA.AD");
  CHECK(p.packaged);
  CHECK_EQ(p.id, std::string("ad10"));
  CHECK_EQ(p.module_dir, std::string("D:\\ad\\win\\packages\\ad10\\AD10TH"));
  CHECK_EQ(p.root, std::string("D:\\ad\\win\\packages\\ad10"));
  CHECK_EQ(p.engine_dir, std::string("D:\\ad\\win\\packages\\ad10\\ENGINE"));
  // Only the package: the module dir, then its ENGINE — never FILES\AD40.
  auto dirs = dll_search_dirs(p, "D:\\ad\\win");
  CHECK_EQ(dirs.size(), size_t(2));
  if (dirs.size() == 2) {
    CHECK_EQ(dirs[0], p.module_dir);
    CHECK_EQ(dirs[1], p.engine_dir);
  }
  // "packages" in any case, '/' separators.
  auto q = locate_package("C:/Users/x/assets/win/Packages/tt/TWISTED/CHAM.AD");
  CHECK(q.packaged);
  CHECK_EQ(q.id, std::string("tt"));
}

TEST(package_of_a_module_in_engine) {
  // ad10's ENGINE\STARRYNI.AD: module dir and engine dir are one; searched once.
  auto p = locate_package("D:\\ad\\win\\packages\\ad10\\ENGINE\\STARRYNI.AD");
  CHECK(p.packaged);
  auto dirs = dll_search_dirs(p, "D:\\ad\\win");
  CHECK_EQ(dirs.size(), size_t(1));
  if (!dirs.empty()) CHECK_EQ(dirs[0], std::string("D:\\ad\\win\\packages\\ad10\\ENGINE"));
  auto q = locate_package("D:/ad/win/packages/ad10/ENGINE/STARRYNI.AD");
  CHECK_EQ(dll_search_dirs(q, "D:/ad/win").size(), size_t(1));
}

TEST(package_of_legacy_modules) {
  // Deluxe keeps the lane's original order: module dir, then <win>\FILES\AD40.
  auto p = locate_package("D:\\ad\\win\\FILES\\AD40\\TOASTERS.AD");
  CHECK(!p.packaged);
  CHECK_EQ(p.id, std::string("legacy"));
  auto dirs = dll_search_dirs(p, "D:\\ad\\win");
  CHECK_EQ(dirs.size(), size_t(2));
  if (dirs.size() == 2) {
    CHECK_EQ(dirs[0], std::string("D:\\ad\\win\\FILES\\AD40"));
    CHECK_EQ(dirs[1], std::string("D:\\ad\\win\\FILES\\AD40"));
  }
  auto e = locate_package("D:\\ad\\win\\FILES\\ENGINE\\STARRYNI.AD");
  CHECK(!e.packaged);
  auto edirs = dll_search_dirs(e, "D:\\ad\\win");
  CHECK(edirs.size() == 2 && edirs[1] == "D:\\ad\\win\\FILES\\AD40");
  // A folder named like a package id but not under "packages"; a lone module
  // near the drive root; a relative name.
  CHECK(!locate_package("D:\\stuff\\ad10\\AD10TH\\HALLOFFA.AD").packaged);
  CHECK(!locate_package("D:\\packagesX\\ad10\\AD10TH\\X.AD").packaged);
  CHECK(!locate_package("C:\\MODULE.AD").packaged);
  CHECK(!locate_package("C:\\dir\\MODULE.AD").packaged);
  CHECK(!locate_package("MODULE.AD").packaged);
  // A package root directly under a drive: its parent is the drive, not "packages".
  CHECK(!locate_package("C:\\ad10\\AD10TH\\X.AD").packaged);
  // A packages folder at the top of a drive still counts.
  CHECK(locate_package("C:\\packages\\ad10\\AD10TH\\X.AD").packaged);
}


// Small screens (lane.hh): the smallest whole k that reaches 640x480, at most 8.
TEST(small_screen_auto_scale) {
  using adw::pe32::Pe32Lane;
  CHECK_EQ(Pe32Lane::auto_guest_scale(320, 240), 2);
  CHECK_EQ(Pe32Lane::auto_guest_scale(640, 480), 1);
  CHECK_EQ(Pe32Lane::auto_guest_scale(1920, 1080), 1);
  CHECK_EQ(Pe32Lane::auto_guest_scale(200, 150), 4);
  CHECK_EQ(Pe32Lane::auto_guest_scale(640, 200), 3);
  CHECK_EQ(Pe32Lane::auto_guest_scale(1, 1), 8);
  CHECK_EQ(Pe32Lane::auto_guest_scale(0, 480), 1);
}

// The presenter: a cell of one index keeps it; a mixed cell is averaged and
// matched to the nearest palette entry (lowest index on ties); the palette
// follows the guest's.
TEST(small_screen_downsample) {
  using adw::pe32::Pe32Lane;
  adw::Screen in(4, 2), out(2, 1);
  RGBQUAD pal[256] = {};
  pal[1] = RGBQUAD{0, 0, 255, 0};  // red
  pal[2] = RGBQUAD{255, 0, 0, 0};  // blue
  pal[3] = RGBQUAD{0, 0, 128, 0};  // dark red (the average of red and black)
  pal[4] = RGBQUAD{0, 0, 128, 0};  // the same colour again: index 3 wins the tie
  in.set_entries(0, 256, pal);
  uint8_t* r0 = in.row(0);
  uint8_t* r1 = in.row(1);
  r0[0] = r0[1] = r1[0] = r1[1] = 2;  // left cell: all blue
  r0[2] = r1[2] = 1;                  // right cell: half red, half black
  r0[3] = r1[3] = 0;
  Pe32Lane::NearCache cache;
  Pe32Lane::downsample(in, out, 2, cache);
  CHECK_EQ(int(out.row(0)[0]), 2);
  CHECK_EQ(int(out.row(0)[1]), 3);
  CHECK(memcmp(out.palette().data(), in.palette().data(), sizeof(pal)) == 0);
  // Deterministic, cache or not.
  Pe32Lane::NearCache fresh;
  adw::Screen out2(2, 1);
  Pe32Lane::downsample(in, out2, 2, fresh);
  CHECK(memcmp(out2.row(0), out.row(0), 2) == 0);
}

// A whole program (program.cc): a PE32 EXE is one, a DLL (every After Dark
// module) is not; the Desktop Toys command line TOYSAV32.SCR built; the ABI
// in --capabilities. The images are made up (headers and a "ret").
TEST(program_recognition_and_command_line) {
  using adw::pe32::Pe32Lane;
  CHECK_EQ(Pe32Lane::desktop_toys_command_line("C:\\DTToys", 3, 1), std::string("C:\\DTToys\\dtty3203.exe /s /t0"));
  CHECK_EQ(Pe32Lane::desktop_toys_command_line("C:\\DTToys", 3, 9), std::string("C:\\DTToys\\dtty3203.exe /s /t8"));
  CHECK_EQ(Pe32Lane::desktop_toys_command_line("D:\\T", 12, 3), std::string("D:\\T\\dtty3212.exe /s /t2"));
  // Play (Caps Lock): the toy selector's command line, no ghost mode.
  CHECK_EQ(Pe32Lane::desktop_toys_command_line("C:\\DTToys", 3, 4, false), std::string("C:\\DTToys\\dtty3203.exe /t3"));
  Pe32Lane lane;
  auto abis = lane.abis();
  CHECK(std::find(abis.begin(), abis.end(), "desktoptoys") != abis.end());
  CHECK(std::find(abis.begin(), abis.end(), "afterdark") != abis.end());
  auto image = [](bool dll) {
    std::string f(0x400, '\0');
    auto u16 = [&](size_t o, uint16_t v) { memcpy(&f[o], &v, 2); };
    auto u32 = [&](size_t o, uint32_t v) { memcpy(&f[o], &v, 4); };
    f[0] = 'M', f[1] = 'Z';
    u32(0x3C, 0x40);
    memcpy(&f[0x40], "PE\0\0", 4);
    u16(0x44, 0x014C);
    u16(0x46, 1);
    u16(0x54, 0xE0);
    u16(0x56, dll ? 0x2103 : 0x0103);
    u16(0x58, 0x10B);
    u32(0x58 + 16, 0x1000);
    u32(0x58 + 28, 0x00400000);
    u32(0x58 + 32, 0x1000);
    u32(0x58 + 36, 0x200);
    u32(0x58 + 56, 0x2000);
    u32(0x58 + 60, 0x200);
    u32(0x58 + 92, 16);
    size_t s = 0x58 + 0xE0;
    memcpy(&f[s], ".text", 5);
    u32(s + 8, 0x10);
    u32(s + 12, 0x1000);
    u32(s + 16, 0x200);
    u32(s + 20, 0x200);
    u32(s + 36, 0x60000020);
    f[0x200] = char(0xC3);
    return f;
  };
  char tmp[MAX_PATH];
  GetTempPathA(MAX_PATH, tmp);
  std::string base = std::string(tmp) + "adw_pe32_prog_" + std::to_string(GetCurrentProcessId());
  for (int dll = 0; dll < 2; dll++) {
    std::string path = base + (dll ? ".dll" : ".exe"), bytes = image(dll);
    FILE* f = fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (!f) continue;
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    CHECK_EQ(Pe32Lane::is_program_file(path), !dll);
    remove(path.c_str());
  }
  CHECK(!Pe32Lane::is_program_file(base + ".missing"));
}

namespace {

// Runs adhostwin on `module` for `frames` frames; returns its exit code and output.
int run_host(const std::string& exe, const std::string& module, int frames, std::string& out) {
  _putenv_s("ADFRAMES", std::to_string(frames).c_str());
  _putenv_s("ADGOWAITMS", "0");
  _putenv_s("ADFBHASH", "1");
  std::string cmd = "\"\"" + exe + "\" " + module + " 2>&1\"";
  FILE* p = _popen(cmd.c_str(), "r");
  if (!p) return -1;
  char buf[512];
  out.clear();
  while (fgets(buf, sizeof(buf), p)) out += buf;
  return _pclose(p);
}

std::string hashes(const std::string& out);
std::string slurp(const std::string& p);
void remove_tree(const std::string& p);

int run_assets(const char* exe) {
  // The installed assets: AD_ASSETS_DIR, else the data folder's (read-only,
  // test_paths.h). Every host below is then given them explicitly, so none
  // resolves a default location under the real %LOCALAPPDATA%.
  const char* a = getenv("AD_ASSETS_DIR");
  const std::string assets = (a && *a) ? std::string(a) : adw_test::installed_assets_root();
  const std::string win = assets.empty() ? std::string() : adw::Env::parse({{"AD_ASSETS_DIR", assets}}).win_assets_dir();
  std::string root = win.empty() ? std::string() : win + "\\FILES\\";
  if (!exe || root.empty() || GetFileAttributesA((root + "AD40\\TOASTERS.AD").c_str()) == INVALID_FILE_ATTRIBUTES) {
    printf("assets or adhostwin not available: skipped\n");
    return 77;
  }
  adw_test::sandbox_spawned_hosts(assets, "pe32");
  // The engine modules of the task goal plus the two no-engine shapes
  // (PSYCHO: Borland, no ADXPL510; STARRYNI: MSVC _Module@4).
  const char* modules[] = {"FILES/AD40/TOASTERS.AD", "FILES/AD40/LIFE.AD", "FILES/AD40/MARBLES.AD",
                           "FILES/AD40/PSYCHO.AD", "FILES/ENGINE/STARRYNI.AD"};
  for (const char* m : modules) {
    std::string a, b;
    int rc = run_host(exe, m, 20, a);
    CHECK_EQ(rc, 0);
    CHECK(a.find("0 unimplemented API(s) called") != std::string::npos);
    // No timer or posted message waits for a message loop the host does not run (user32.cc).
    CHECK(a.find("through its own message calls") == std::string::npos);
    CHECK(a.find("FBHASH 19 ") != std::string::npos);
    run_host(exe, m, 20, b);
    CHECK(a == b);  // headless runs are deterministic
    printf("%s %s\n", a == b && rc == 0 ? "PASS" : "FAIL", m);
  }
  // A packaged module sees only its own package (PACKAGES.md §7.2): a copy of
  // TOASTERS.AD in a package without an engine must not borrow Deluxe's
  // FILES\AD40\ADXPL510.DLL, and says what is missing instead of crashing.
  {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = std::wstring(tmp) + L"adw_pe32_iso_" + std::to_wstring(GetCurrentProcessId());
    std::wstring mods = base + L"\\packages\\fake\\MODS";
    for (const std::wstring& d : {base, base + L"\\packages", base + L"\\packages\\fake", mods})
      CreateDirectoryW(d.c_str(), nullptr);
    std::wstring copy = mods + L"\\TOASTERS.AD";
    CopyFileW(adw::widen(root + "AD40\\TOASTERS.AD").c_str(), copy.c_str(), FALSE);
    std::string out;
    int rc = run_host(exe, "\"" + adw::narrow(copy) + "\"", 5, out);
    bool isolated = rc == 1 && out.find("ADXPL510.DLL was not found") != std::string::npos;
    CHECK(isolated);
    printf("%s packaged TOASTERS.AD without an engine stops cleanly (rc=%d)\n", isolated ? "PASS" : "FAIL", rc);
    DeleteFileW(copy.c_str());
    for (const std::wstring& d : {mods, base + L"\\packages\\fake", base + L"\\packages", base})
      RemoveDirectoryW(d.c_str());
  }
  // The desktop seed (ADSEEDIMG, win32/display.hh): SHADOW with Clear Screen
  // First off draws its agents and their shadows over it; with it on, the
  // module's own blank erases it (frames equal the unseeded run's); a module
  // that always blanks (TOASTERS) never shows it.
  {
    auto run = [&](const char* module, const char* seed, const char* cvset, int frames) {
      _putenv_s("ADSEEDIMG", seed);
      _putenv_s("ADCVSET", cvset);
      std::string out;
      int rc = run_host(exe, module, frames, out);
      _putenv_s("ADSEEDIMG", "");
      _putenv_s("ADCVSET", "");
      CHECK_EQ(rc, 0);
      return hashes(out);
    };
    std::string plain = run("FILES/AD40/SHADOW.AD", "", "", 40);
    std::string seeded = run("FILES/AD40/SHADOW.AD", ":win95", "", 40);
    bool shows = !plain.empty() && seeded != plain;
    bool cleared = run("FILES/AD40/SHADOW.AD", ":win95", "1=1", 40) == run("FILES/AD40/SHADOW.AD", "", "1=1", 40);
    bool blanked = run("FILES/AD40/TOASTERS.AD", ":win95", "", 20) == run("FILES/AD40/TOASTERS.AD", "", "", 20);
    CHECK(shows);
    CHECK(cleared);
    CHECK(blanked);
    printf("%s desktop seed: SHADOW keeps it, Clear Screen First and TOASTERS erase it\n",
           shows && cleared && blanked ? "PASS" : "FAIL");
  }
  // Pacing (lane.hh "Timing"): blits cost what a 1996 display card took
  // (ADPIXCOST), and a DRAWFRAME that outlasts its frame carries on in the
  // next ones. Swirling Magic's DRAWFRAME is three full-screen BitBlts plus
  // its particles: it used to run ~3,700 times a second of virtual time (62
  // calls a frame, the work budget blind to the blits); a P5 with a PCI card
  // managed ~15-25. The Scale setting changes sharpness, not speed: the rate
  // is the same at 480 and 720 lines (16:9). PSYCHO's first call (its whole
  // first pattern, ~520,000 SetPixels) is presented as it draws.
  {
    auto pace = [&](const char* module, int frames, const char* w, const char* h, uint64_t* inside) -> uint64_t {
      _putenv_s("ADTRACE", "lane");
      _putenv_s("ADSCREENW", w);
      _putenv_s("ADSCREENH", h);
      std::string out;
      int rc = run_host(exe, module, frames, out);
      _putenv_s("ADTRACE", "");
      _putenv_s("ADSCREENW", "");
      _putenv_s("ADSCREENH", "");
      unsigned long long draws = 0, over = 0, in = 0;
      size_t at = out.find(" DRAWFRAME calls over ");
      if (rc != 0 || at == std::string::npos) return 0;
      size_t line = out.rfind('\n', at);
      line = line == std::string::npos ? 0 : line + 1;
      size_t colon = out.find(": ", line);
      if (colon == std::string::npos ||
          sscanf(out.c_str() + colon + 2, "%llu DRAWFRAME calls over %llu frames (%llu", &draws, &over, &in) != 3) {
        return 0;
      }
      if (inside) *inside = in;
      return draws;
    };
    uint64_t psycho_inside = 0;
    uint64_t swirl = pace("FILES/AD40/SWIRLING.AD", 300, "640", "480", nullptr);
    uint64_t swirl480 = pace("FILES/AD40/SWIRLING.AD", 300, "856", "480", nullptr);
    uint64_t swirl720 = pace("FILES/AD40/SWIRLING.AD", 300, "1280", "720", nullptr);
    uint64_t psycho = pace("FILES/AD40/PSYCHO.AD", 120, "640", "480", &psycho_inside);
    double per_s = double(swirl) * 60.0 / 300.0;
    bool period = swirl && per_s >= 8 && per_s <= 45;
    bool scale_free = swirl480 && swirl720 && swirl720 * 10 >= swirl480 * 9 && swirl720 * 10 <= swirl480 * 11;
    bool presented = psycho && psycho_inside >= 60;
    CHECK(period);
    CHECK(scale_free);
    CHECK(presented);
    printf("%s pacing: SWIRLING %.1f DRAWFRAMEs per virtual second (%llu at 856x480, %llu at 1280x720 over 300 "
           "frames); PSYCHO's first pattern over %llu frames\n",
           period && scale_free && presented ? "PASS" : "FAIL", per_s, (unsigned long long)swirl480,
           (unsigned long long)swirl720, (unsigned long long)psycho_inside);
  }
  // Interaction (INTERACTION.md §5.1): RODGER goes interactive on a Caps Lock
  // change, takes KEY lines as messages 7/8 (consumed: status().eaten), and
  // drops out on the next change.
  {
    auto env = adw::Env::parse({{"AD_ASSETS_DIR", assets}});
    adw::Screen screen(640, 480);
    adw::VirtualClock clock(adw::VirtualClock::Mode::fixed_step, 16667);
    adw::InputState input;
    adw::LaneContext ctx{env, screen, clock, input};
    adw::pe32::Pe32Lane lane;
    bool ok = lane.init(root + "AD40\\RODGER.AD", ctx);
    CHECK(ok);
    uint64_t seq = 0;
    auto feed = [&](adw::Command c) {
      c.seq = ++seq;
      input.apply(c);
      lane.on_command(c);
    };
    auto steps = [&](int n) {
      for (int i = 0; i < n && ok; i++) {
        clock.begin_frame();
        ok = lane.step() == adw::StepResult::ok;
      }
    };
    steps(900);
    bool before = lane.status().interactive;
    adw::Command caps;
    caps.kind = adw::Command::Kind::caps;
    caps.a = 1;
    feed(caps);
    steps(8);
    bool playing = lane.status().interactive && lane.status().source == adw::kStatusSourceAd4;
    adw::Command key;
    key.kind = adw::Command::Kind::key;
    key.a = VK_LEFT;
    key.b = 1;
    feed(key);
    uint64_t key_seq = seq;
    steps(2);
    bool eaten = lane.status().eaten >= key_seq;
    key.b = 0;
    feed(key);
    steps(2);
    caps.a = 0;
    feed(caps);
    steps(8);
    bool stopped = !lane.status().interactive;
    bool good = ok && !before && playing && eaten && stopped;
    CHECK(good);
    printf("%s RODGER: interactive after Caps Lock (%d), keys consumed (%d), stops on the next change (%d)\n",
           good ? "PASS" : "FAIL", playing, eaten, stopped);
    lane.shutdown();
  }
  // Sound (AUDIO.md §7): with an enabled engine (no sink: nothing is played)
  // the block carries the sound bit and the engine's volume, dsound.dll
  // loads, and Flying Toasters starts its noises and its song, which it
  // finds through the Deluxe rename (Music\Flying Toasters.mid ->
  // TOASTERS.MID); without an engine the block is as it always was.
  {
    auto env = adw::Env::parse({{"AD_ASSETS_DIR", assets}});
    auto run = [&](adw::audio::Engine* engine, uint32_t* flags, uint32_t* volume) {
      adw::Screen screen(640, 480);
      adw::VirtualClock clock(adw::VirtualClock::Mode::fixed_step, 16667);
      adw::InputState input;
      adw::LaneContext ctx{env, screen, clock, input};
      ctx.audio = engine;
      adw::pe32::Pe32Lane lane;
      bool ok = lane.init(root + "AD40\\TOASTERS.AD", ctx);
      if (!ok) return false;
      *flags = lane.runtime()->mem().read_u32l(lane.block_address() + adw::pe32::block::kFlags);
      *volume = lane.runtime()->mem().read_u32l(lane.block_address() + adw::pe32::block::kVolume);
      for (int i = 0; i < 900 && ok; i++) {
        clock.begin_frame();
        ok = lane.step() == adw::StepResult::ok;
        if (engine) engine->advance(clock.now_us());
      }
      lane.shutdown();
      return ok;
    };
    adw::audio::Config cfg;
    cfg.guest_sound = true;
    cfg.volume = 70;
    auto engine = adw::audio::make_engine(cfg);
    uint32_t flags = 0, volume = 0, off_flags = 0, off_volume = 0;
    bool ok = run(engine.get(), &flags, &volume);
    adw::audio::Stats st = engine->stats();
    bool ok_off = run(nullptr, &off_flags, &off_volume);
    bool good = ok && (flags & adw::pe32::block::kSoundOn) && volume == 70 && st.voices_started > 0 &&
                st.songs_started > 0 && st.midi_events > 0 && ok_off &&
                !(off_flags & adw::pe32::block::kSoundOn) && off_volume == 50;
    CHECK(good);
    printf("%s TOASTERS sound on: flags 0x%X volume %u, %llu voice(s), %llu song(s), %llu MIDI event(s); "
           "off: flags 0x%X volume %u\n",
           good ? "PASS" : "FAIL", flags, volume, (unsigned long long)st.voices_started,
           (unsigned long long)st.songs_started, (unsigned long long)st.midi_events, off_flags, off_volume);
  }
  // Small screens (lane.hh): CYBER refuses 320x240 unless the guest display is
  // scaled; scaled, it runs and stays deterministic.
  {
    _putenv_s("ADSCREENW", "320");
    _putenv_s("ADSCREENH", "240");
    std::string a, b, c;
    int rc = run_host(exe, "FILES/AD40/CYBER.AD", 60, a);
    run_host(exe, "FILES/AD40/CYBER.AD", 60, b);
    _putenv_s("ADPE32SCALE", "1");
    int rc1 = run_host(exe, "FILES/AD40/CYBER.AD", 5, c);
    _putenv_s("ADPE32SCALE", "");
    _putenv_s("ADSCREENW", "");
    _putenv_s("ADSCREENH", "");
    bool good = rc == 0 && !hashes(a).empty() && hashes(a) == hashes(b) && rc1 != 0;
    CHECK(good);
    printf("%s CYBER at 320x240: scaled runs (exit %d, deterministic), unscaled refuses (exit %d)\n",
           good ? "PASS" : "FAIL", rc, rc1);
  }
  // Configure mode (INTERACTION.md §6): Messages 4.0's Custom button, hidden
  // and scripted, keeps the text in the package's state MODULES.INI, and a
  // later run with that state draws it.
  {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::string base = adw::narrow(tmp) + "adw_pe32_cfg_" + std::to_string(GetCurrentProcessId());
    CreateDirectoryW(adw::widen(base).c_str(), nullptr);
    std::string script = base + "\\script.txt", state = base + "\\state";
    FILE* f = _wfopen(adw::widen(script).c_str(), L"wb");
    fputs("TEXT 101 HELLO FROM CTEST\nCLICK 1\n", f);
    fclose(f);
    _putenv_s("ADSTATE", state.c_str());
    _putenv_s("ADCONFIGHIDDEN", "1");
    _putenv_s("ADCONFIGSCRIPT", script.c_str());
    std::string out;
    std::string cmd = "\"\"" + std::string(exe) + "\" --configure FILES/AD40/MESSAGES.AD --button 3 2>&1\"";
    FILE* p = _popen(cmd.c_str(), "r");
    char buf[512];
    while (p && fgets(buf, sizeof(buf), p)) out += buf;
    int rc = p ? _pclose(p) : -1;
    _putenv_s("ADCONFIGHIDDEN", "");
    _putenv_s("ADCONFIGSCRIPT", "");
    std::string ini = slurp(state + "\\deluxe\\WINDOWS\\MODULES.INI");
    bool kept = rc == 0 && out.find("\"result\":\"ok\"") != std::string::npos &&
                ini.find("HELLO FROM CTEST") != std::string::npos;
    _putenv_s("ADCVSET", "2=0");
    std::string with, without;
    run_host(exe, "FILES/AD40/MESSAGES.AD", 60, with);
    _putenv_s("ADSTATE", (base + "\\empty").c_str());
    run_host(exe, "FILES/AD40/MESSAGES.AD", 60, without);
    _putenv_s("ADSTATE", "");
    _putenv_s("ADCVSET", "");
    bool shown = !hashes(with).empty() && hashes(with) != hashes(without);
    CHECK(kept);
    CHECK(shown);
    printf("%s Messages 4.0 Custom: configure exit %d, text kept (%d), later run shows it (%d)\n",
           kept && shown ? "PASS" : "FAIL", rc, kept, shown);
    remove_tree(base);
  }
  return adw_test::failures() ? 1 : 0;
}

bool is_dir(const std::string& p) {
  DWORD a = GetFileAttributesW(adw::widen(p).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string slurp(const std::string& p) {
  std::string out;
  FILE* f = _wfopen(adw::widen(p).c_str(), L"rb");
  if (!f) return out;
  char buf[65536];
  for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) out.append(buf, n);
  fclose(f);
  return out;
}

void remove_tree(const std::string& p) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(adw::widen(p + "\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      std::wstring n = fd.cFileName;
      if (n == L"." || n == L"..") continue;
      std::string c = p + "\\" + adw::narrow(n);
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(c);
      else DeleteFileW(adw::widen(c).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryW(adw::widen(p).c_str());
}

std::vector<std::string> list_ad(const std::string& dir) {
  std::vector<std::string> out;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(adw::widen(dir + "\\*.AD").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(adw::narrow(fd.cFileName));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end());
  return out;
}

// The FBHASH lines of a run's output, in order.
std::string hashes(const std::string& out) {
  std::string h;
  size_t p = 0;
  while ((p = out.find("FBHASH ", p)) != std::string::npos) {
    size_t e = out.find('\n', p);
    if (e == std::string::npos) e = out.size() - 1;
    h += out.substr(p, e - p + 1);
    p = e + 1;
  }
  return h;
}

// Calls a KERNEL32 shim of a live lane's runtime with a guest string as its
// first argument and `more` dwords after it; returns EAX.
uint32_t call_path_shim(adw::win32::Runtime& rt, const char* name, const std::string& path,
                        std::initializer_list<uint32_t> more) {
  uint32_t s = rt.heap().alloc(uint32_t(path.size() + 1), true);
  adw::win32::write_cstr(rt.mem(), s, path, path.size() + 1);
  std::vector<uint32_t> args{s};
  args.insert(args.end(), more.begin(), more.end());
  auto& e = rt.shims().get("KERNEL32.DLL", name);
  uint32_t r = rt.call_guest(rt.shims().thunk_address(e), std::span<const uint32_t>(args), e.conv);
  rt.heap().free(s);
  return r;
}

// The Toasters 2k builds name their music by installed long names
// (Music\Toasters2k.mid, Music\Flying Toasters.mid, Music\Baby Toasters.mid),
// which only the importer's fix-ups provide (PACKAGES.md §4.3). They touch the
// file only once an MCI sequencer answers, and there is none here, so check
// what they would get: through a live lane's C:\AFTERDRK, each name must be
// found (absolute, as their _access check asks) and open for reading
// (relative to the current directory, as MCI_OPEN would).
void check_long_midi_names(const std::string& root, const std::string& module, const char* const* names) {
  auto env = adw::Env::parse({{"AD_ASSETS_DIR", root}});
  adw::Screen screen(640, 480);
  adw::VirtualClock clock(adw::VirtualClock::Mode::fixed_step, 16667);
  adw::InputState input;
  adw::LaneContext ctx{env, screen, clock, input};
  adw::pe32::Pe32Lane lane;
  bool ok = lane.init(module, ctx);
  CHECK(ok);
  if (!ok) return;
  adw::win32::Runtime& rt = *lane.runtime();
  for (const char* const* n = names; *n; n++) {
    std::string rel = std::string("Music\\") + *n;
    uint32_t attrs = call_path_shim(rt, "GetFileAttributesA", "C:\\AFTERDRK\\" + rel, {});
    uint32_t h = call_path_shim(rt, "CreateFileA", rel, {GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0});
    bool found = attrs != INVALID_FILE_ATTRIBUTES && h != 0xFFFFFFFF;
    printf("%s %s: %s\n", found ? "PASS" : "FAIL", module.substr(module.find_last_of('\\') + 1).c_str(),
           rel.c_str());
    CHECK(found);
    if (h != 0xFFFFFFFF) {
      auto& close = rt.shims().get("KERNEL32.DLL", "CloseHandle");
      rt.call_guest(rt.shims().thunk_address(close), {h}, close.conv);
    }
  }
  lane.shutdown();
}

int run_packages(const char* exe) {
  // AD_PE32_PKG_ROOT: an assets root holding win\packages\ad10 (a scratch
  // import, or PACKAGES.md §4.4's interim root); else the installed assets
  // (read-only, test_paths.h).
  std::string root;
  if (const char* r = getenv("AD_PE32_PKG_ROOT"); r && *r) {
    root = r;
  } else {
    root = adw_test::installed_assets_root();
  }
  std::string win = adw::Env::parse({{"AD_ASSETS_DIR", root}}).win_assets_dir();
  std::string pkg = win + "\\packages\\ad10";
  if (!exe || root.empty() || !is_dir(pkg + "\\AD10TH")) {
    printf("no win\\packages\\ad10 under '%s' (set AD_PE32_PKG_ROOT): skipped\n", root.c_str());
    return 77;
  }
  if (const char* t = getenv("ADTRACE")) adw::set_trace_categories({t});
  adw_test::sandbox_spawned_hosts(root, "pe32pkg");  // AD_ASSETS_DIR = root, scratch AD_LOCALAPPDATA
  std::vector<std::string> modules;
  for (const char* d : {"AD10TH", "ENGINE"})
    for (const std::string& f : list_ad(pkg + "\\" + d))
      if (adw::probe_module(pkg + "\\" + d + "\\" + f).kind == adw::LaneKind::pe32)
        modules.push_back(pkg + "\\" + d + "\\" + f);
  CHECK_EQ(modules.size(), size_t(17));  // AD10TH's 16 + ENGINE\STARRYNI.AD
  int identical = 0;
  for (const std::string& m : modules) {
    std::string q = "\"" + m + "\"";
    std::string a, b;
    int rc = run_host(exe, q, 60, a);
    bool clean = rc == 0 && a.find(": 0 unimplemented API(s) called") != std::string::npos &&
                 a.find("without a signature") == std::string::npos && a.find("FBHASH 59 ") != std::string::npos &&
                 a.find("through its own message calls") == std::string::npos;
    run_host(exe, q, 60, b);
    bool same = !hashes(a).empty() && hashes(a) == hashes(b);
    CHECK(clean);
    CHECK(same);
    // A module byte-identical to Deluxe's (when this root holds Deluxe too)
    // draws Deluxe's frames under ad10's own ADXPL510 5.2.
    std::string name = m.substr(m.find_last_of('\\') + 1);
    std::string deluxe = win + "\\FILES\\" + (name == "STARRYNI.AD" ? "ENGINE\\" : "AD40\\") + name;
    std::string verdict;
    if (GetFileAttributesW(adw::widen(deluxe).c_str()) != INVALID_FILE_ATTRIBUTES && slurp(deluxe) == slurp(m)) {
      std::string d;
      run_host(exe, "\"" + deluxe + "\"", 60, d);
      bool match = hashes(d) == hashes(a);
      CHECK(match);
      identical += match;
      verdict = match ? " (frames = Deluxe's)" : " (frames DIFFER from Deluxe's)";
    }
    printf("%s %s%s\n", clean && same ? "PASS" : "FAIL", name.c_str(), verdict.c_str());
  }
  if (is_dir(win + "\\FILES\\AD40")) printf("%d module(s) byte-identical to Deluxe's draw the same frames\n", identical);
  // The lane searches only the package; its one-line init summary says so.
  {
    _putenv_s("ADTRACE", "lane");
    std::string out;
    run_host(exe, "\"" + pkg + "\\AD10TH\\HALLOFFA.AD\"", 1, out);
    _putenv_s("ADTRACE", "");
    size_t at = out.find("package ad10");
    CHECK(at != std::string::npos);
    std::string line = at == std::string::npos ? "" : out.substr(at, out.find('\n', at) - at);
    bool isolated = line.find("DLL search:") != std::string::npos && line.find("FILES\\AD40") == std::string::npos &&
                    line.find("ad10\\ENGINE") != std::string::npos;
    CHECK(isolated);
    printf("%s lane summary: %s\n", isolated ? "PASS" : "FAIL", line.c_str());
  }
  static const char* const t2[] = {"Toasters2k.mid", "Baby Toasters.mid", nullptr};
  static const char* const t2k[] = {"Flying Toasters.mid", "Baby Toasters.mid", nullptr};
  check_long_midi_names(root, pkg + "\\AD10TH\\TOASTER2.AD", t2);
  check_long_midi_names(root, pkg + "\\AD10TH\\TOAST2K.AD", t2k);
  return adw_test::failures() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string a = argc > 1 ? argv[1] : "";
  if (a == "--assets") return run_assets(argc > 2 ? argv[2] : nullptr);
  if (a == "--packages") return run_packages(argc > 2 ? argv[2] : nullptr);
  return adw_test::run_all(argc > 1 ? argv[1] : nullptr);
}
