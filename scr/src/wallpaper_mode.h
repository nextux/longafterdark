// Wallpaper mode (args.h /wallpaper; scr/README.md "Wallpaper mode"): the
// saver's modules on the Windows desktop itself, behind its icons and under
// every window, as Take Your Best Shot's living wallpapers were. The parts
// that need no window of ours, so the tests can pin them: the classes, where
// in Explorer's windows ours go (over a made-up window tree as well as the
// real one), what each way of running gives its hosts, and the picture of the
// user's wallpaper every host starts on.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "args.h"
#include "geometry.h"

namespace adw::scr {

// Each monitor's window: a child of Explorer's desktop window (never
// top-level, never topmost), disabled so no key or click is ever its.
inline constexpr wchar_t kWallpaperClass[] = L"LongAfterDarkWallpaper";
inline constexpr DWORD kWallpaperStyle = WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS;
// A hidden top-level window of the same run, which hears what a child never
// does (the end of the session, Explorer starting again, a display change)
// and which /wallpaper /stop, or a second /wallpaper, finds and closes.
inline constexpr wchar_t kWallpaperControlClass[] = L"LongAfterDarkWallpaperControl";
// One /wallpaper per session ("Local\": this session's namespace).
inline constexpr wchar_t kWallpaperMutex[] = L"Local\\LongAfterDarkWallpaper";
// What every host /wallpaper starts is told (the others are told nothing:
// an empty value removes an inherited one). LANE-2's 7th Level scenes run
// their living-wallpaper form with it; every other module ignores it.
inline constexpr wchar_t kWallpaperEnv[] = L"ADWALLPAPER";
// Progman's message that makes Explorer put a WorkerW behind the icons
// (undocumented, as every living-wallpaper program uses it: wParam 0xD,
// lParam 1).
inline constexpr UINT kProgmanSpawnWorker = 0x052C;

// ---- where the windows go ----------------------------------------------------------

// A window tree as the search sees it: `children(parent)` lists a window's
// own children in z-order, the topmost first (a null parent: the top-level
// windows), and `class_name` names a window's class.
struct DesktopWindows {
  std::function<std::vector<HWND>(HWND parent)> children;
  std::function<std::wstring(HWND)> class_name;
};
// This desktop's (FindWindowEx's, so a hidden desktop shows no Explorer).
DesktopWindows real_desktop_windows();

enum class WallpaperLayout {
  none,      // no desktop of Explorer's: no Progman, or no icons (SHELLDLL_DefView) in it yet
  worker,    // before Windows 11 24H2: into the top-level WorkerW after the one holding the icons
  progman,   // from 24H2: into Progman, just below the icons (above its WorkerW child)
};
struct WallpaperHost {
  WallpaperLayout layout = WallpaperLayout::none;
  HWND parent = nullptr;     // the window ours are children of
  HWND below = nullptr;      // ours go just below this sibling in z-order (null: the top of `parent`'s)
  HWND progman = nullptr;    // Progman (0x052C's target), when there is one
  HWND icons = nullptr;      // SHELLDLL_DefView
  bool found() const { return layout != WallpaperLayout::none && parent; }
  std::string describe() const;   // for the log
};
// Where the wallpaper windows go, chosen by what the tree shows (never by the
// Windows build): Progman holding the icons and a WorkerW of its own is
// 24H2's (progman); else the top-level window holding the icons, followed in
// z-order by a top-level WorkerW, is the older one (worker); else none.
WallpaperHost find_wallpaper_host(const DesktopWindows& w);
// A window's extended style in a parent of extended style `parent_ex`:
// 24H2's Progman has no surface to draw children into
// (WS_EX_NOREDIRECTIONBITMAP; its icons are a layered child for that), so
// one there is layered too, at full opacity; in a WorkerW, an ordinary child.
// AD_SCR_WALL_LAYERED=0 or 1 overrides it (for a check on a real desktop).
DWORD wallpaper_ex_style(DWORD parent_ex);

// ---- what each way of running gives its hosts --------------------------------------

// Where a host's first picture (ADSEEDIMG, INTERACTION.md §8) comes from.
enum class SeedFrom {
  none,        // black: /p, /window (a stream must not show the desktop), /s with StartFromDesktop=0
  capture,     // the monitor as it is (/s): its window's first host only
  wallpaper,   // the user's wallpaper alone, no windows or icons (/wallpaper): every host
};
struct HostRules {
  bool input = true;            // keys, clicks and moves reach the owner's host and may end the run
  bool toggles = true;          // its hosts start with Caps Lock's and Num Lock's toggles (games)
  bool power_request = false;   // the display is kept on while it runs (/window)
  bool wallpaper_env = false;   // ADWALLPAPER=1
  SeedFrom seed = SeedFrom::none;
};
HostRules host_rules(Mode mode, bool start_from_desktop);
// The ADWALLPAPER variable for a host of `rules` ("1", or "" to remove it).
std::pair<std::wstring, std::wstring> wallpaper_env(const HostRules& rules);

// ---- the picture of the wallpaper ----------------------------------------------------

// How a monitor's wallpaper is painted into a picture of its size:
// PaintDesktop (user32's own painter: the real picture, checked on
// Windows 11 build 26300, but in a memory DC only where device and screen
// coordinates agree: the monitor at the virtual screen's 0,0), else (another
// monitor, or PaintDesktop failing, as on a hidden desktop) the monitor's
// file drawn as Explorer places it (IDesktopWallpaper, WIC), else its colour.
enum class WallpaperPaint { paint_desktop, file };
WallpaperPaint wallpaper_paint_way(const RECT& monitor);
// Paints `monitor`'s wallpaper into `dc` (a memory DC whose bitmap is the
// monitor's size) the `way` given; false when nothing could be painted
// (then it is the desktop colour). `how` says what was done, for the log.
bool paint_wallpaper(HDC dc, const RECT& monitor, WallpaperPaint way, std::string* how);
// A host's first picture on `monitor` (virtual-screen pixels): its wallpaper,
// the part `src` (monitor pixels, seed_source) shrunk or stretched to `emu`
// with HALFTONE, as a P6 (desktop_seed.h; empty on failure).
std::vector<uint8_t> wallpaper_p6(const RECT& monitor, const RectI& src, SizeI emu, std::string* how);

}  // namespace adw::scr
