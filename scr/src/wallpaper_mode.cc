#include "wallpaper_mode.h"

#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>

#include "desktop_seed.h"
#include "paths.h"

namespace adw::scr {

namespace {

constexpr wchar_t kIcons[] = L"SHELLDLL_DefView";
constexpr wchar_t kWorker[] = L"WorkerW";
constexpr wchar_t kProgman[] = L"Progman";

HWND child_of_class(const DesktopWindows& w, HWND parent, const wchar_t* cls) {
  for (HWND c : w.children(parent)) {
    if (w.class_name(c) == cls) return c;
  }
  return nullptr;
}

std::string hex(HWND h) {
  char b[24];
  snprintf(b, sizeof b, "%llx", (unsigned long long)(uintptr_t)h);
  return b;
}

}  // namespace

DesktopWindows real_desktop_windows() {
  DesktopWindows w;
  w.children = [](HWND parent) {
    std::vector<HWND> v;
    // FindWindowEx walks one level, in z-order; with no parent, this
    // desktop's top-level windows.
    for (HWND c = FindWindowExW(parent, nullptr, nullptr, nullptr); c; c = FindWindowExW(parent, c, nullptr, nullptr)) {
      v.push_back(c);
      if (v.size() > 100000) break;   // a tree changing under the walk can't loop it for ever
    }
    return v;
  };
  w.class_name = [](HWND h) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, (int)std::size(cls));
    return std::wstring(cls);
  };
  return w;
}

std::string WallpaperHost::describe() const {
  switch (layout) {
    case WallpaperLayout::progman:
      return "progman (24H2): in Progman " + hex(parent) + " below SHELLDLL_DefView " + hex(icons);
    case WallpaperLayout::worker:
      return "workerw: in WorkerW " + hex(parent) + " behind SHELLDLL_DefView " + hex(icons);
    default:
      return progman ? "none: Progman " + hex(progman) + " holds no desktop icons" : "none: no Progman";
  }
}

WallpaperHost find_wallpaper_host(const DesktopWindows& w) {
  WallpaperHost h;
  const std::vector<HWND> top = w.children(nullptr);
  for (HWND t : top) {
    if (w.class_name(t) == kProgman) {
      h.progman = t;
      break;
    }
  }
  if (!h.progman) return h;
  // 24H2: the icons stay in Progman, and 0x052C gives it a WorkerW child
  // for the wallpaper; ours go between the two.
  if (HWND icons = child_of_class(w, h.progman, kIcons)) {
    if (child_of_class(w, h.progman, kWorker)) {
      h.layout = WallpaperLayout::progman;
      h.parent = h.progman;
      h.below = icons;
      h.icons = icons;
      return h;
    }
  }
  // Before it: the icons are in a top-level window (a WorkerW, or Progman
  // until 0x052C has run), and the wallpaper's WorkerW is the next top-level
  // WorkerW below it.
  for (size_t i = 0; i < top.size(); ++i) {
    HWND icons = child_of_class(w, top[i], kIcons);
    if (!icons) continue;
    for (size_t j = i + 1; j < top.size(); ++j) {
      if (w.class_name(top[j]) != kWorker) continue;
      h.layout = WallpaperLayout::worker;
      h.parent = top[j];
      h.icons = icons;
      return h;
    }
    break;   // one window holds the icons
  }
  return h;
}

DWORD wallpaper_ex_style(DWORD parent_ex) {
#ifndef WS_EX_NOREDIRECTIONBITMAP
  constexpr DWORD WS_EX_NOREDIRECTIONBITMAP = 0x00200000;
#endif
  return WS_EX_NOACTIVATE | ((parent_ex & WS_EX_NOREDIRECTIONBITMAP) ? WS_EX_LAYERED : 0);
}

HostRules host_rules(Mode mode, bool start_from_desktop) {
  HostRules r;
  switch (mode) {
    case Mode::run:
      r.seed = start_from_desktop ? SeedFrom::capture : SeedFrom::none;
      break;
    case Mode::preview:
      r.input = false;
      break;
    case Mode::window:
      r.input = r.toggles = false;
      r.power_request = true;
      break;
    case Mode::wallpaper:
      r.input = r.toggles = false;
      r.wallpaper_env = true;
      r.seed = SeedFrom::wallpaper;   // whatever StartFromDesktop says: it is the point of it
      break;
    default:
      break;
  }
  return r;
}

std::pair<std::wstring, std::wstring> wallpaper_env(const HostRules& rules) {
  return {kWallpaperEnv, rules.wallpaper_env ? L"1" : L""};
}

WallpaperPaint wallpaper_paint_way(const RECT& monitor) {
  return monitor.left == 0 && monitor.top == 0 ? WallpaperPaint::paint_desktop : WallpaperPaint::file;
}

namespace {

void fill(HDC dc, int w, int h, COLORREF c) {
  HBRUSH b = CreateSolidBrush(c);
  RECT r{0, 0, w, h};
  FillRect(dc, &r, b);
  DeleteObject(b);
}

// The picture at `path` as top-down 32-bpp BGRX (WIC); false when it can't be read.
bool decode(const std::wstring& path, std::vector<uint8_t>& px, int& w, int& h) {
  IWICImagingFactory* f = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
  bool ok = false;
  IWICBitmapDecoder* d = nullptr;
  IWICBitmapFrameDecode* fr = nullptr;
  IWICFormatConverter* cv = nullptr;
  UINT uw = 0, uh = 0;
  if (SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
      SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
      SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                               WICBitmapPaletteTypeCustom)) &&
      SUCCEEDED(cv->GetSize(&uw, &uh)) && uw > 0 && uh > 0 && (uint64_t)uw * uh <= (1ull << 28)) {
    px.resize((size_t)uw * uh * 4);
    ok = SUCCEEDED(cv->CopyPixels(nullptr, uw * 4, (UINT)px.size(), px.data()));
    w = (int)uw;
    h = (int)uh;
  }
  if (cv) cv->Release();
  if (fr) fr->Release();
  if (d) d->Release();
  f->Release();
  return ok;
}

// `px` (w x h) drawn at `dst` (monitor pixels) in `dc`, halftoned.
void blit(HDC dc, const std::vector<uint8_t>& px, int w, int h, int dx, int dy, int dw, int dh, int sx = 0,
          int sy = 0, int sw = -1, int sh = -1) {
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  SetStretchBltMode(dc, HALFTONE);
  SetBrushOrgEx(dc, 0, 0, nullptr);
  if (sw < 0) sw = w;
  if (sh < 0) sh = h;
  // StretchDIBits counts a top-down source's rows from its bottom.
  StretchDIBits(dc, dx, dy, dw, dh, sx, h - sy - sh, sw, sh, px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

}  // namespace

bool paint_wallpaper(HDC dc, const RECT& monitor, WallpaperPaint way, std::string* how) {
  const int mw = monitor.right - monitor.left, mh = monitor.bottom - monitor.top;
  if (!dc || mw <= 0 || mh <= 0) return false;
  fill(dc, mw, mh, GetSysColor(COLOR_DESKTOP));
  std::string first;
  if (way == WallpaperPaint::paint_desktop) {
    if (PaintDesktop(dc)) {
      GdiFlush();
      if (how) *how = "PaintDesktop";
      return true;
    }
    // A desktop user32 keeps no wallpaper for (a hidden one): the file, as for another monitor.
    first = "PaintDesktop failed, so ";
  }
  // The monitor's own wallpaper and how Explorer places it.
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IDesktopWallpaper* dw = nullptr;
  std::wstring file;
  DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
  COLORREF bg = GetSysColor(COLOR_DESKTOP);
  if (SUCCEEDED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)))) {
    UINT n = 0;
    dw->GetMonitorDevicePathCount(&n);
    for (UINT i = 0; i < n && file.empty(); ++i) {
      LPWSTR id = nullptr;
      RECT r{};
      if (SUCCEEDED(dw->GetMonitorDevicePathAt(i, &id)) && SUCCEEDED(dw->GetMonitorRECT(id, &r)) && EqualRect(&r, &monitor)) {
        LPWSTR p = nullptr;
        if (SUCCEEDED(dw->GetWallpaper(id, &p)) && p) {
          file = p;
          CoTaskMemFree(p);
        }
      }
      if (id) CoTaskMemFree(id);
    }
    // No monitor of that rectangle (its coordinates another DPI's): the one
    // every monitor shows, when they show one.
    LPWSTR all = nullptr;
    if (file.empty() && dw->GetWallpaper(nullptr, &all) == S_OK && all) file = all;
    if (all) CoTaskMemFree(all);
    dw->GetPosition(&pos);
    dw->GetBackgroundColor(&bg);
    dw->Release();
  }
  fill(dc, mw, mh, bg);
  std::vector<uint8_t> px;
  int w = 0, h = 0;
  bool ok = !file.empty() && decode(file, px, w, h);
  if (ok) {
    const double sx = (double)mw / w, sy = (double)mh / h;
    if (pos == DWPOS_STRETCH) {
      blit(dc, px, w, h, 0, 0, mw, mh);
    } else if (pos == DWPOS_FIT || pos == DWPOS_FILL || pos == DWPOS_SPAN) {
      // Fit: all of it inside; Fill (and Span, taken monitor by monitor): all
      // of the monitor covered, the rest cut off evenly.
      const double s = pos == DWPOS_FIT ? std::min(sx, sy) : std::max(sx, sy);
      const int dw2 = std::max(1, (int)(w * s + 0.5)), dh2 = std::max(1, (int)(h * s + 0.5));
      blit(dc, px, w, h, (mw - dw2) / 2, (mh - dh2) / 2, dw2, dh2);
    } else if (pos == DWPOS_TILE) {
      for (int y = 0; y < mh; y += h) {
        for (int x = 0; x < mw; x += w) blit(dc, px, w, h, x, y, w, h);
      }
    } else {   // DWPOS_CENTER
      blit(dc, px, w, h, (mw - w) / 2, (mh - h) / 2, w, h);
    }
    GdiFlush();
  }
  if (how) {
    static const char* const kPos[] = {"center", "tile", "stretch", "fit", "fill", "span"};
    *how = first + (ok ? std::string("the wallpaper file, ") + ((unsigned)pos < 6 ? kPos[pos] : "fill")
                       : file.empty() ? std::string("no wallpaper file: the background colour")
                                      : "the wallpaper file can't be read (" + narrow(file) + "): the background colour");
  }
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

std::vector<uint8_t> wallpaper_p6(const RECT& monitor, const RectI& src, SizeI emu, std::string* how) {
  const int mw = monitor.right - monitor.left, mh = monitor.bottom - monitor.top;
  std::vector<uint8_t> out;
  if (mw <= 0 || mh <= 0) return out;
  HDC screen = GetDC(nullptr);
  HDC dc = screen ? CreateCompatibleDC(screen) : nullptr;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = mw;
  bi.bmiHeader.biHeight = -mh;   // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = dc ? CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
  if (bmp) {
    HGDIOBJ old = SelectObject(dc, bmp);
    paint_wallpaper(dc, monitor, wallpaper_paint_way(monitor), how);   // its colour, at least
    shrink_parts(dc, mw, mh, {SeedShot{src, emu}}, [&](size_t, const std::vector<uint8_t>& p6) { out = p6; });
    SelectObject(dc, old);
    DeleteObject(bmp);
  } else if (how) {
    *how = "no memory for a picture of " + std::to_string(mw) + "x" + std::to_string(mh);
  }
  if (dc) DeleteDC(dc);
  if (screen) ReleaseDC(nullptr, screen);
  return out;
}

}  // namespace adw::scr
