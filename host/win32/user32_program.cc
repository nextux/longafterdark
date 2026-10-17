// USER32.DLL for a whole program (docs/ABI.md §2.15; API_SURFACE.md §1
// "USER32.DLL", the Screen Wreckers rows): what Windows 95's USER did for an
// application that owns its windows and its message loop — Cyclops' Desktop
// Toys engine, DTTY3203.EXE, an MFC 4.2 program — on top of the model every
// After Dark module sees (user32.cc). The lane turns it on
// (user32_set_program); off, every handler here that replaces one of
// user32.cc's hands the call to it unchanged, and the ones only a program
// imports are never reached.
//
// Windows: parents (a child's coordinates are its parent's client
// coordinates; GetWindowRect and the stored rects are screen coordinates),
// owners, z-order among siblings, the non-client frame of Windows 95's
// metrics (DefWindowProc's WM_NCCALCSIZE: thick frame 4, dialog frame 3,
// border 1, caption 18, menu bar 19, client edge 2). CreateWindowExA sends
// what Windows 95 sent: the WH_CBT hook's HCBT_CREATEWND (MFC subclasses its
// windows there), WM_GETMINMAXINFO (top-level), WM_NCCREATE,
// WM_NCCALCSIZE, WM_CREATE, WM_SIZE, WM_MOVE, the parent's WM_PARENTNOTIFY,
// then ShowWindow for WS_VISIBLE. ShowWindow sends WM_SHOWWINDOW and, for a
// top-level window, activates it (WM_ACTIVATEAPP once, WM_NCACTIVATE,
// WM_ACTIVATE; DefWindowProc's WM_ACTIVATE sets the focus: WM_KILLFOCUS /
// WM_SETFOCUS). SetWindowPos/MoveWindow/DeferWindowPos send
// WM_WINDOWPOSCHANGING and WM_WINDOWPOSCHANGED (DefWindowProc turns it into
// WM_MOVE/WM_SIZE), moving the children along. DestroyWindow: HCBT_DESTROYWND,
// owned windows first, WM_DESTROY parent first, WM_NCDESTROY children first,
// the windows' timers and queued messages with them.
//
// Painting: an update rectangle per window (InvalidateRect/ValidateRect;
// a window that is shown or moved is invalidated whole, with erasing);
// GetMessageA/PeekMessageA produce WM_PAINT for the first visible window
// that needs it, after posted messages and before timers; BeginPaint sends
// WM_ERASEBKGND when erasing is due (DefWindowProc fills the class brush)
// and validates; UpdateWindow paints now. Every window draws on the one
// emulated screen, through the screen DC (GetDC, GetWindowDC, BeginPaint).
//
// The message loop: GetMessageA with nothing to deliver waits
// (Runtime::yield_frame: the lane ends the frame there and the next frame
// looks again); audio notifications are pumped first. GetMessageTime/
// GetMessagePos answer for the last message retrieved.
//
// Menus (LoadMenuA's RT_MENU templates; SetMenu/GetMenu, GetSubMenu,
// GetMenuItemCount/ID/State, EnableMenuItem, CheckMenuItem, ModifyMenuA,
// DestroyMenu), accelerators (LoadAcceleratorsA, TranslateAcceleratorA on
// key messages → WM_COMMAND), hooks (SetWindowsHookExA, CallNextHookEx,
// UnhookWindowsHookEx: emulated, never real; WH_CBT is called), focus,
// capture and activation, the desktop window, rectangles, coordinates and
// the small helpers MFC calls. Nothing is shown, nothing reaches the host's
// desktop, cursor or hooks: SetCursorPos moves the emulated pointer that
// GetCursorPos reads.
//
// Input (Caps Lock play, pe32/program.cc): the lane queues the saver's KEY
// and MOUSE lines as Windows 95's input queue held them
// (user32_program_key/_mouse; no guest code runs then): key messages to the
// focus window (else the active one) with a US keyboard's scan codes, the
// previous state and the transition in lParam, WM_SYSKEY* with Alt or F10;
// mouse moves (coalesced) and button messages to the capture window or the
// deepest visible window under the pointer, in its client coordinates, with
// MK_* in wParam, a second press within 500 ms and 4x4 pixels a double click
// on a CS_DBLCLKS class; a disabled window gets none. GetMessageA/
// PeekMessageA read them after posted messages; TranslateMessage posts
// WM_CHAR/WM_SYSCHAR (a US layout, Shift, Ctrl and Caps Lock). The mouse
// moves the emulated pointer; the pointer's own cursor is never drawn (the
// saver shows the arrow while the cursor is on: user32_cursor_shown). In
// ghost mode the queue stays empty.
//
// Known gaps, deliberately left (DTTY3203.EXE's ghost mode reaches none;
// each answers as nothing happened): WM_NCHITTEST, WM_SETCURSOR and
// WM_MOUSEACTIVATE are not sent for input (the toys' one window is the whole
// screen and always active), TrackPopupMenu (no selection),
// GrayStringA, TabbedTextOutA, carets, WinHelpA (FALSE), IsDialogMessageA
// (FALSE: no dialogs), GetNextDlgTabItem (0), DDE's lParam helpers (low and
// high word), ExcludeUpdateRgn (NULLREGION).
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "adw/core/log.h"
#include "win32/audio.hh"
#include "win32/display.hh"
#include "win32/gdi_objects.hh"
#include "win32/modules.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"
#include "win32/user32_state.hh"

namespace adw::win32 {

using namespace user32;

namespace {

constexpr uint32_t kMenuBase = 0x00050000, kAccelBase = 0x00060000, kHookBase = 0x00070000;
constexpr uint32_t kIconBase = 0x00080000, kSysIconBase = 0x00090000, kDwpBase = 0x000B0000;
constexpr uint32_t kHandleStep = 4;
// WINDOWPOS flags Windows adds for WM_WINDOWPOSCHANGED (not in the SDK headers).
constexpr uint32_t kSwpNoClientSize = 0x0800, kSwpNoClientMove = 0x1000;

// Windows 95's metrics (96 dpi, small fonts).
constexpr int kFrame = 4, kDlgFrame = 3, kBorder = 1, kEdge = 2, kCaption = 18, kSmCaption = 15, kMenuBar = 19;

int screen_w(Runtime& rt) { return rt.display() ? rt.display()->width() : 640; }
int screen_h(Runtime& rt) { return rt.display() ? rt.display()->height() : 480; }

bool program_on(Runtime& rt) { return us(rt).program; }

// A block of guest memory for a message's struct, freed at scope end.
struct GuestBlock {
  Runtime& rt;
  uint32_t addr;
  GuestBlock(Runtime& r, uint32_t size) : rt(r), addr(r.heap().alloc(size, true)) {
    if (!addr) throw GuestError(GuestError::Kind::fatal, "out of guest memory for a window message");
  }
  ~GuestBlock() { rt.heap().free(addr); }
};

uint32_t makelong(int32_t lo, int32_t hi) { return (uint32_t(uint16_t(hi)) << 16) | uint16_t(lo); }

bool is_child(const Window& w) { return (w.style & WS_CHILD) != 0; }

// The parent whose client area a child's coordinates are in (0 = the screen).
Window* parent_of(User32State& s, const Window& w) { return is_child(w) ? s.get(w.parent) : nullptr; }

POINT client_origin(User32State& s, uint32_t hwnd) {
  Window* w = s.get(hwnd);
  if (!w || hwnd == s.desktop) return POINT{0, 0};
  return POINT{w->client.left, w->client.top};
}

// The window and all its ancestors are visible.
bool really_visible(User32State& s, const Window& w) {
  const Window* p = &w;
  for (int depth = 0; p && depth < 64; depth++) {
    if (!(p->style & WS_VISIBLE)) return false;
    if (!is_child(*p)) return true;
    p = s.get(p->parent);
  }
  return p == nullptr;
}

uint32_t toplevel_of(User32State& s, uint32_t hwnd) {
  Window* w = s.get(hwnd);
  for (int depth = 0; w && is_child(*w) && depth < 64; depth++) {
    Window* p = s.get(w->parent);
    if (!p) break;
    w = p;
  }
  return w ? w->hwnd : 0;
}

bool is_descendant(User32State& s, uint32_t hwnd, uint32_t ancestor) {
  Window* w = s.get(hwnd);
  for (int depth = 0; w && depth < 64; depth++) {
    if (!is_child(*w)) return false;
    if (w->parent == ancestor) return true;
    w = s.get(w->parent);
  }
  return false;
}

// Siblings of the level `parent` names (0 = the top-level windows), topmost first.
std::vector<uint32_t> siblings(User32State& s, uint32_t parent) {
  std::vector<Window*> v;
  for (Window& w : s.windows) {
    if (!w.alive || w.hwnd == s.desktop || w.hwnd == s.saver) continue;
    if (parent ? (is_child(w) && w.parent == parent) : !is_child(w)) v.push_back(&w);
  }
  std::stable_sort(v.begin(), v.end(), [](const Window* a, const Window* b) {
    bool ta = a->exstyle & WS_EX_TOPMOST, tb = b->exstyle & WS_EX_TOPMOST;
    if (ta != tb) return ta;
    return a->z > b->z;
  });
  std::vector<uint32_t> out;
  for (Window* w : v) out.push_back(w->hwnd);
  return out;
}

std::vector<uint32_t> children_of(User32State& s, uint32_t hwnd) { return siblings(s, hwnd); }

// The deepest visible window at screen point `p`, topmost first (0 = none: the desktop).
uint32_t window_at(User32State& s, POINT p, uint32_t parent = 0) {
  for (uint32_t h : siblings(s, parent)) {
    Window* w = s.get(h);
    if (!w || !(w->style & WS_VISIBLE) || !PtInRect(&w->rect, p)) continue;
    uint32_t inner = window_at(s, p, h);
    return inner ? inner : h;
  }
  return 0;
}

// DefWindowProc's WM_NCCALCSIZE: the client rectangle inside window rect `r`.
RECT client_inside(const Window& w, RECT r) {
  int b = 0;
  if (w.style & WS_THICKFRAME) b = kFrame;
  else if ((w.exstyle & WS_EX_DLGMODALFRAME) || (w.style & (WS_DLGFRAME | WS_BORDER)) == WS_DLGFRAME) b = kDlgFrame;
  else if (w.style & WS_BORDER) b = kBorder;
  InflateRect(&r, -b, -b);
  if ((w.style & WS_CAPTION) == WS_CAPTION) r.top += (w.exstyle & WS_EX_TOOLWINDOW) ? kSmCaption : kCaption;
  if (!is_child(w) && w.menu) r.top += kMenuBar;
  if (w.exstyle & WS_EX_CLIENTEDGE) InflateRect(&r, -kEdge, -kEdge);
  if (w.exstyle & WS_EX_STATICEDGE) InflateRect(&r, -1, -1);
  if (r.right < r.left) r.right = r.left;
  if (r.bottom < r.top) r.bottom = r.top;
  return r;
}

// The window's client area after its frame changed: WM_NCCALCSIZE (wParam
// FALSE) with the window rectangle in its parent's client coordinates.
void calc_client(Runtime& rt, uint32_t hwnd) {
  User32State& s = us(rt);
  Window* w = s.get(hwnd);
  if (!w) return;
  POINT o = client_origin(s, parent_of(s, *w) ? w->parent : 0);
  RECT r = w->rect;
  OffsetRect(&r, -o.x, -o.y);
  GuestBlock b(rt, sizeof(RECT));
  write_pod(rt.mem(), b.addr, r);
  send(rt, hwnd, WM_NCCALCSIZE, FALSE, b.addr);
  if (!(w = s.get(hwnd))) return;
  r = read_pod<RECT>(rt.mem(), b.addr);
  OffsetRect(&r, o.x, o.y);
  w->client = r;
}

// Every window below `hwnd` moves by (dx, dy) on the screen.
void move_children(User32State& s, uint32_t hwnd, int dx, int dy) {
  for (uint32_t c : children_of(s, hwnd)) {
    if (Window* w = s.get(c)) {
      OffsetRect(&w->rect, dx, dy);
      OffsetRect(&w->client, dx, dy);
      move_children(s, c, dx, dy);
    }
  }
}

void invalidate_all(User32State& s, Window& w, bool erase, bool children) {
  w.invalid = true;
  w.erase = w.erase || erase;
  w.update = RECT{0, 0, w.client.right - w.client.left, w.client.bottom - w.client.top};
  if (!children) return;
  for (uint32_t c : children_of(s, w.hwnd))
    if (Window* cw = s.get(c)) invalidate_all(s, *cw, erase, true);
}

// The hooks of `type`, newest first: the newest runs and calls the next
// through CallNextHookEx.
uint32_t call_hook(Runtime& rt, int32_t type, int32_t code, uint32_t wp, uint32_t lp) {
  User32State& s = us(rt);
  for (size_t i = s.hooks.size(); i-- > 0;) {
    if (s.hooks[i].alive && s.hooks[i].type == type) {
      trace("user", "hook %d (code %d) -> 0x%X", type, code, s.hooks[i].proc);
      return rt.call_guest(s.hooks[i].proc, {uint32_t(code), wp, lp});
    }
  }
  return 0;
}

void activate(Runtime& rt, uint32_t hwnd);

uint32_t set_focus(Runtime& rt, uint32_t hwnd) {
  User32State& s = us(rt);
  uint32_t prev = s.focus;
  if (hwnd == prev) return prev;
  if (hwnd && !s.get(hwnd)) return 0;
  if (hwnd) {
    uint32_t top = toplevel_of(s, hwnd);
    if (top && top != s.active) activate(rt, top);
    if (s.focus == hwnd) return prev;  // the activation focused it already
    prev = s.focus;
  }
  if (prev && s.get(prev)) send(rt, prev, WM_KILLFOCUS, hwnd, 0);
  s.focus = hwnd;
  if (hwnd && s.get(hwnd)) send(rt, hwnd, WM_SETFOCUS, prev, 0);
  trace("user", "focus 0x%X -> 0x%X", prev, hwnd);
  return prev;
}

void activate(Runtime& rt, uint32_t hwnd) {
  User32State& s = us(rt);
  if (s.active == hwnd || !s.get(hwnd)) return;
  uint32_t prev = s.active;
  if (!s.app_active) {
    s.app_active = true;
    for (uint32_t t : siblings(s, 0)) send(rt, t, WM_ACTIVATEAPP, TRUE, rt.mem().read_u32l(rt.teb() + 0x24));
  }
  if (prev && s.get(prev)) {
    send(rt, prev, WM_NCACTIVATE, FALSE, 0);
    send(rt, prev, WM_ACTIVATE, WA_INACTIVE, hwnd);
  }
  s.active = hwnd;
  if (Window* w = s.get(hwnd)) w->z = ++s.next_z;
  trace("user", "activate 0x%X (was 0x%X)", hwnd, prev);
  send(rt, hwnd, WM_NCACTIVATE, TRUE, 0);
  Window* w = s.get(hwnd);
  send(rt, hwnd, WM_ACTIVATE, makelong(WA_ACTIVE, w && (w->style & WS_MINIMIZE) ? 1 : 0), prev);
}

// The WINDOWPOS messages and the move/size itself (SetWindowPos's work).
// x, y are in the parent's client coordinates.
bool set_window_pos(Runtime& rt, uint32_t hwnd, uint32_t after, int32_t x, int32_t y, int32_t cx, int32_t cy,
                    uint32_t flags) {
  User32State& s = us(rt);
  Window* w = s.get(hwnd);
  if (!w) return false;
  POINT o = client_origin(s, parent_of(s, *w) ? w->parent : 0);
  RECT old = w->rect, old_client = w->client;
  if (flags & SWP_NOMOVE) {
    x = old.left - o.x;
    y = old.top - o.y;
  }
  if (flags & SWP_NOSIZE) {
    cx = old.right - old.left;
    cy = old.bottom - old.top;
  }
  GuestBlock wp(rt, 28);
  auto put = [&](uint32_t f) {
    uint32_t v[7] = {hwnd, after, uint32_t(x), uint32_t(y), uint32_t(cx), uint32_t(cy), f};
    rt.mem().memcpy(wp.addr, v, sizeof(v));
  };
  put(flags);
  if (!(flags & SWP_NOSENDCHANGING)) {
    send(rt, hwnd, WM_WINDOWPOSCHANGING, 0, wp.addr);
    if (!(w = s.get(hwnd))) return false;
    x = int32_t(rt.mem().read_u32l(wp.addr + 8));
    y = int32_t(rt.mem().read_u32l(wp.addr + 12));
    cx = int32_t(rt.mem().read_u32l(wp.addr + 16));
    cy = int32_t(rt.mem().read_u32l(wp.addr + 20));
    flags = rt.mem().read_u32l(wp.addr + 24);
  }
  RECT nr{o.x + x, o.y + y, o.x + x + std::max(cx, 0), o.y + y + std::max(cy, 0)};
  w->rect = nr;
  if (!(flags & SWP_NOZORDER)) {
    if (after == uint32_t(-1)) w->exstyle |= WS_EX_TOPMOST;  // HWND_TOPMOST
    if (after == uint32_t(-2)) w->exstyle &= ~uint32_t(WS_EX_TOPMOST);  // HWND_NOTOPMOST
    if (after == 1) {  // HWND_BOTTOM
      uint64_t low = w->z;
      for (uint32_t h : siblings(s, is_child(*w) ? w->parent : 0))
        if (Window* sw = s.get(h)) low = std::min(low, sw->z);
      w->z = low ? low - 1 : 0;
    } else if (after == 0 || after == uint32_t(-1) || after == uint32_t(-2)) {
      w->z = ++s.next_z;
    } else if (Window* a = s.get(after)) {
      // Just below `after`: everything above it moves up one.
      for (uint32_t h : siblings(s, is_child(*w) ? w->parent : 0)) {
        Window* sw = s.get(h);
        if (sw && sw != w && sw->z > a->z) sw->z++;
      }
      w->z = a->z;
      a->z++;
      s.next_z = std::max(s.next_z, a->z);
    }
  }
  if (flags & SWP_SHOWWINDOW) w->style |= WS_VISIBLE;
  if (flags & SWP_HIDEWINDOW) w->style &= ~uint32_t(WS_VISIBLE);
  bool moved = nr.left != old.left || nr.top != old.top, sized = (nr.right - nr.left) != (old.right - old.left) ||
                                                                   (nr.bottom - nr.top) != (old.bottom - old.top);
  if (moved || sized || (flags & SWP_FRAMECHANGED)) {
    // The client area follows the frame; the children follow the client area.
    w->client = old_client;
    OffsetRect(&w->client, nr.left - old.left, nr.top - old.top);
    calc_client(rt, hwnd);
    if (!(w = s.get(hwnd))) return false;
    move_children(s, hwnd, w->client.left - old_client.left, w->client.top - old_client.top);
  }
  if ((moved || sized || (flags & (SWP_SHOWWINDOW | SWP_FRAMECHANGED))) && !(flags & SWP_NOREDRAW) &&
      (w->style & WS_VISIBLE))
    invalidate_all(s, *w, true, true);
  uint32_t done = flags;
  if (w->client.left == old_client.left && w->client.top == old_client.top) done |= kSwpNoClientMove;
  if (w->client.right - w->client.left == old_client.right - old_client.left &&
      w->client.bottom - w->client.top == old_client.bottom - old_client.top)
    done |= kSwpNoClientSize;
  put(done);
  rt.mem().write_u32l(wp.addr + 8, uint32_t(nr.left - o.x));
  rt.mem().write_u32l(wp.addr + 12, uint32_t(nr.top - o.y));
  send(rt, hwnd, WM_WINDOWPOSCHANGED, 0, wp.addr);
  if ((w = s.get(hwnd)) && !is_child(*w) && (w->style & WS_VISIBLE) && !(flags & SWP_NOACTIVATE)) activate(rt, hwnd);
  return true;
}

void size_messages(Runtime& rt, uint32_t hwnd) {
  User32State& s = us(rt);
  Window* w = s.get(hwnd);
  if (!w) return;
  uint32_t kind = (w->style & WS_MAXIMIZE) ? SIZE_MAXIMIZED : (w->style & WS_MINIMIZE) ? SIZE_MINIMIZED : SIZE_RESTORED;
  RECT c = w->client;
  POINT o = client_origin(s, parent_of(s, *w) ? w->parent : 0);
  send(rt, hwnd, WM_SIZE, kind, makelong(c.right - c.left, c.bottom - c.top));
  send(rt, hwnd, WM_MOVE, 0, makelong(c.left - o.x, c.top - o.y));
}

uint32_t show_window(Runtime& rt, uint32_t hwnd, int32_t cmd) {
  User32State& s = us(rt);
  Window* w = s.get(hwnd);
  if (!w) return 0;
  bool was = w->style & WS_VISIBLE;
  trace("user", "ShowWindow(0x%X, %d)%s", hwnd, cmd, was ? " (visible)" : "");
  if (cmd == SW_HIDE) {
    if (!was) return 0;
    send(rt, hwnd, WM_SHOWWINDOW, FALSE, 0);
    set_window_pos(rt, hwnd, 0, 0, 0, 0, 0,
                   SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (s.focus && (s.focus == hwnd || is_descendant(s, s.focus, hwnd))) s.focus = 0;
    if (s.active == hwnd) s.active = 0;
    return 1;
  }
  bool maximize = cmd == SW_SHOWMAXIMIZED, minimize = cmd == SW_SHOWMINIMIZED || cmd == SW_MINIMIZE ||
                                                  cmd == SW_SHOWMINNOACTIVE;
  bool no_activate = cmd == SW_SHOWNA || cmd == SW_SHOWNOACTIVATE || cmd == SW_SHOWMINNOACTIVE || cmd == SW_MINIMIZE;
  if (!was) send(rt, hwnd, WM_SHOWWINDOW, TRUE, 0);
  if (!(w = s.get(hwnd))) return 0;
  uint32_t flags = SWP_SHOWWINDOW | SWP_NOZORDER | SWP_NOACTIVATE;
  if (maximize && !is_child(*w)) {
    // The frame lies just outside the screen, as Windows maximizes.
    RECT full{0, 0, screen_w(rt), screen_h(rt)};
    RECT in = client_inside(*w, full);
    int fx = in.left - full.left, fy = (w->style & WS_THICKFRAME) ? kFrame : (in.left - full.left);
    w->style = (w->style | WS_MAXIMIZE) & ~uint32_t(WS_MINIMIZE);
    set_window_pos(rt, hwnd, 0, -fx, -fy, screen_w(rt) + 2 * fx, screen_h(rt) + 2 * fy, flags);
    size_messages(rt, hwnd);
  } else {
    if (minimize) w->style |= WS_MINIMIZE;
    else if (cmd == SW_RESTORE || cmd == SW_SHOWNORMAL) w->style &= ~uint32_t(WS_MINIMIZE | WS_MAXIMIZE);
    set_window_pos(rt, hwnd, 0, 0, 0, 0, 0, flags | SWP_NOMOVE | SWP_NOSIZE);
  }
  if ((w = s.get(hwnd)) && !w->shown_once) w->shown_once = true;
  if ((w = s.get(hwnd)) && !is_child(*w) && !no_activate && !minimize) activate(rt, hwnd);
  return was ? 1 : 0;
}

// DestroyWindow's messages, parent first (WM_DESTROY) and children first (WM_NCDESTROY).
void send_destroy(Runtime& rt, uint32_t hwnd) {
  send(rt, hwnd, WM_DESTROY, 0, 0);
  for (uint32_t c : children_of(us(rt), hwnd)) send_destroy(rt, c);
}

void send_ncdestroy(Runtime& rt, uint32_t hwnd) {
  User32State& s = us(rt);
  for (uint32_t c : children_of(s, hwnd)) send_ncdestroy(rt, c);
  send(rt, hwnd, WM_NCDESTROY, 0, 0);
  if (Window* w = s.get(hwnd)) {
    w->alive = false;
    if (!is_child(*w) && w->menu) {
      uint32_t i = (w->menu - kMenuBase) / kHandleStep;
      if (w->menu >= kMenuBase && i < s.menus.size()) s.menus[i].alive = false;
    }
  }
  s.timers.erase(std::remove_if(s.timers.begin(), s.timers.end(), [&](const Timer& t) { return t.hwnd == hwnd; }),
                 s.timers.end());
  s.posted.erase(std::remove_if(s.posted.begin(), s.posted.end(), [&](const g32::MSG& m) { return m.hwnd == hwnd; }),
                 s.posted.end());
  s.input.erase(std::remove_if(s.input.begin(), s.input.end(), [&](const g32::MSG& m) { return m.hwnd == hwnd; }),
                s.input.end());
  if (s.focus == hwnd) s.focus = 0;
  if (s.active == hwnd) s.active = 0;
  if (s.capture == hwnd) s.capture = 0;
}

bool destroy_window(Runtime& rt, uint32_t hwnd) {
  User32State& s = us(rt);
  Window* w = s.get(hwnd);
  if (!w || hwnd == s.desktop) return false;
  if (call_hook(rt, WH_CBT, HCBT_DESTROYWND, hwnd, 0)) return false;
  trace("user", "DestroyWindow 0x%X (%s)", hwnd, w->class_name.c_str());
  // Owned windows go first.
  for (Window& o : s.windows)
    if (o.alive && !is_child(o) && o.parent == hwnd && o.hwnd != hwnd) destroy_window(rt, o.hwnd);
  if (!(w = s.get(hwnd))) return true;
  if (is_child(*w) && !(w->exstyle & WS_EX_NOPARENTNOTIFY) && s.get(w->parent))
    send(rt, w->parent, WM_PARENTNOTIFY, makelong(WM_DESTROY, int32_t(w->id)), hwnd);
  if ((w = s.get(hwnd)) && (w->style & WS_VISIBLE)) {
    set_window_pos(rt, hwnd, 0, 0, 0, 0, 0, SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (s.focus && (s.focus == hwnd || is_descendant(s, s.focus, hwnd))) s.focus = 0;
  if (s.active == hwnd) s.active = 0;
  send_destroy(rt, hwnd);
  send_ncdestroy(rt, hwnd);
  return true;
}

uint32_t create_window(Call& c) {
  Runtime& rt = c.rt;
  User32State& s = us(rt);
  std::string cls = class_arg(rt, c.arg(1));
  WndClass* k = s.find_class(cls);
  if (!k) {
    trace("user", "CreateWindowEx: no class \"%s\"", cls.c_str());
    c.set_last_error(ERROR_CANNOT_FIND_WND_CLASS);
    return 0;
  }
  uint32_t ex = c.arg(0), style = c.arg(3), parent = c.arg(8), menu = c.arg(9), inst = c.arg(10);
  int32_t x = c.iarg(4), y = c.iarg(5), cx = c.iarg(6), cy = c.iarg(7);
  bool child = style & WS_CHILD;
  if (child && !s.get(parent)) {
    c.set_last_error(ERROR_TLW_WITH_WSCHILD);
    return 0;
  }
  // Windows 95: an overlapped window always has a caption and clips its siblings.
  if (!(style & (WS_POPUP | WS_CHILD))) style |= WS_CAPTION | WS_CLIPSIBLINGS;
  if (x == int32_t(CW_USEDEFAULT)) {
    x = y = 0;
  }
  if (cx == int32_t(CW_USEDEFAULT)) {
    if (child || (style & WS_POPUP)) {
      cx = cy = 0;
    } else {
      cx = screen_w(rt) * 3 / 4 - x;
      cy = screen_h(rt) * 3 / 4 - y;
    }
  }
  Window w;
  w.class_name = k->name;
  w.title = c.str(2);
  w.exstyle = ex;
  w.style = style & ~uint32_t(WS_VISIBLE);
  POINT o = child ? client_origin(s, parent) : POINT{0, 0};
  w.rect = RECT{o.x + x, o.y + y, o.x + x + std::max(cx, 0), o.y + y + std::max(cy, 0)};
  w.client = w.rect;
  w.parent = parent;
  w.id = child ? menu : 0;
  w.menu = child ? 0 : menu;
  w.instance = inst;
  w.wndproc = k->wndproc;
  w.extra.assign(size_t(std::max(k->wnd_extra, 0)), 0);
  w.z = ++s.next_z;
  uint32_t hwnd = s.add(std::move(w));
  trace("user", "CreateWindowEx class \"%s\" \"%s\" style 0x%X ex 0x%X (%d,%d %dx%d) parent 0x%X -> 0x%X",
        cls.c_str(), c.str(2).c_str(), style, ex, x, y, cx, cy, parent, hwnd);
  GuestBlock cs(rt, 48);
  uint32_t fields[12] = {c.arg(11), inst, menu, parent, uint32_t(cy), uint32_t(cx),
                         uint32_t(y), uint32_t(x), style, c.arg(2), c.arg(1), ex};
  rt.mem().memcpy(cs.addr, fields, sizeof(fields));
  auto fail = [&](bool destroy) {
    if (destroy && s.get(hwnd)) destroy_window(rt, hwnd);
    if (Window* dead = s.get(hwnd)) dead->alive = false;
    return 0u;
  };
  {
    GuestBlock cbt(rt, 8);
    rt.mem().write_u32l(cbt.addr, cs.addr);
    rt.mem().write_u32l(cbt.addr + 4, 0);  // HWND_TOP
    if (call_hook(rt, WH_CBT, HCBT_CREATEWND, hwnd, cbt.addr)) return fail(false);
  }
  if (!child) {
    GuestBlock mmi(rt, 40);
    int32_t v[10] = {0, 0, screen_w(rt), screen_h(rt), 0, 0, 112, 27, screen_w(rt) + 8, screen_h(rt) + 8};
    rt.mem().memcpy(mmi.addr, v, sizeof(v));
    send(rt, hwnd, WM_GETMINMAXINFO, 0, mmi.addr);
  }
  if (!s.get(hwnd) || !send(rt, hwnd, WM_NCCREATE, 0, cs.addr)) return fail(false);
  calc_client(rt, hwnd);
  if (!s.get(hwnd) || int32_t(send(rt, hwnd, WM_CREATE, 0, cs.addr)) == -1) return fail(true);
  if (!s.get(hwnd)) return 0;
  size_messages(rt, hwnd);
  if (child && !(ex & WS_EX_NOPARENTNOTIFY) && s.get(parent))
    send(rt, parent, WM_PARENTNOTIFY, makelong(WM_CREATE, int32_t(menu)), hwnd);
  if (s.get(hwnd) && (style & WS_VISIBLE)) show_window(rt, hwnd, (style & WS_MAXIMIZE) ? SW_SHOWMAXIMIZED : SW_SHOW);
  return s.get(hwnd) ? hwnd : 0;
}

// DefWindowProcA for a program's windows (Windows 95's defaults for what an
// MFC program leaves to it).
uint32_t def_window_proc(Runtime& rt, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  User32State& s = us(rt);
  Window* w = s.get(hwnd);
  if (!w) return 0;
  auto& mem = rt.mem();
  switch (msg) {
    case WM_NCCREATE:
      return 1;
    case WM_NCCALCSIZE: {
      RECT r = read_pod<RECT>(mem, lp);  // wParam TRUE: NCCALCSIZE_PARAMS, whose first rect is the same
      write_pod(mem, lp, client_inside(*w, r));
      return 0;
    }
    case WM_WINDOWPOSCHANGED: {
      uint32_t flags = mem.read_u32l(lp + 24);
      RECT c = w->client;
      POINT o = client_origin(s, parent_of(s, *w) ? w->parent : 0);
      if (!(flags & kSwpNoClientMove)) send(rt, hwnd, WM_MOVE, 0, makelong(c.left - o.x, c.top - o.y));
      if (!(flags & kSwpNoClientSize) && (w = s.get(hwnd))) {
        uint32_t kind = (w->style & WS_MAXIMIZE) ? SIZE_MAXIMIZED : (w->style & WS_MINIMIZE) ? SIZE_MINIMIZED
                                                                                           : SIZE_RESTORED;
        send(rt, hwnd, WM_SIZE, kind, makelong(c.right - c.left, c.bottom - c.top));
      }
      return 0;
    }
    case WM_CLOSE:
      destroy_window(rt, hwnd);
      return 0;
    case WM_PAINT: {
      w->invalid = w->erase = false;
      return 0;
    }
    case WM_ERASEBKGND: {
      WndClass* k = s.find_class(w->class_name);
      uint32_t brush = k ? k->background : 0;
      if (!brush) return 0;
      GdiTable& g = rt.state<GdiTable>();
      HDC dc = g.host_dc(wp);
      if (!dc) return 0;
      RECT rc = w->client;  // the screen DC: client area in screen coordinates
      COLORREF sys = 0;
      HBRUSH temp = nullptr, hb = nullptr;
      if (brush <= COLOR_INFOBK + 1) {
        sys = GetSysColor(int(brush - 1));
        temp = CreateSolidBrush(g.key_color(wp, sys));
        hb = temp;
      } else {
        hb = static_cast<HBRUSH>(g.realize_for(brush, wp));
      }
      if (hb) ::FillRect(dc, &rc, hb);
      if (temp) DeleteObject(temp);
      rt.charge_pixels(int64_t(rc.right - rc.left) * (rc.bottom - rc.top));
      return 1;
    }
    case WM_SETTEXT:
      w->title = read_cstr(mem, lp);
      return 1;
    case WM_GETTEXT:
      return wp ? uint32_t(write_cstr(mem, lp, w->title, wp)) : 0;
    case WM_GETTEXTLENGTH:
      return uint32_t(w->title.size());
    case WM_ACTIVATE:
      if (LOWORD(wp) != WA_INACTIVE && !HIWORD(wp)) set_focus(rt, hwnd);
      return 0;
    case WM_NCACTIVATE:
    case WM_QUERYENDSESSION:
    case WM_QUERYOPEN:
      return 1;
    case WM_MOUSEACTIVATE:
      return MA_ACTIVATE;
    case WM_NCHITTEST: {
      POINT p{int16_t(LOWORD(lp)), int16_t(HIWORD(lp))};
      if (PtInRect(&w->client, p)) return HTCLIENT;
      return PtInRect(&w->rect, p) ? HTBORDER : HTNOWHERE;
    }
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        if (WndClass* k = s.find_class(w->class_name); k && k->cursor) s.cursor = k->cursor;
        return 1;
      }
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_CLOSE) send(rt, hwnd, WM_CLOSE, 0, 0);
      return 0;
    case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN:
    case WM_CTLCOLORDLG: case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC:
      return rt.state<GdiTable>().stock(WHITE_BRUSH);
    default:
      return 0;
  }
}

// ---- menus ------------------------------------------------------------------------------------

Menu* menu_of(User32State& s, uint32_t h) {
  if (h < kMenuBase || (h - kMenuBase) % kHandleStep) return nullptr;
  size_t i = (h - kMenuBase) / kHandleStep;
  return i < s.menus.size() && s.menus[i].alive ? &s.menus[i] : nullptr;
}

uint32_t new_menu(User32State& s) {
  s.menus.push_back(Menu{});
  return kMenuBase + uint32_t(s.menus.size() - 1) * kHandleStep;
}

// An RT_MENU template's items from `p` (a MENUITEMTEMPLATE run ending with
// MF_END); text is UTF-16, made ANSI.
size_t parse_menu(User32State& s, uint32_t menu, std::string_view data, size_t p) {
  while (p + 2 <= data.size()) {
    uint16_t flags;
    memcpy(&flags, data.data() + p, 2);
    p += 2;
    MenuItem it;
    it.flags = flags & ~uint32_t(MF_END);
    if (!(flags & MF_POPUP)) {
      if (p + 2 > data.size()) break;
      uint16_t id;
      memcpy(&id, data.data() + p, 2);
      it.id = id;
      p += 2;
    }
    while (p + 2 <= data.size()) {
      uint16_t ch;
      memcpy(&ch, data.data() + p, 2);
      p += 2;
      if (!ch) break;
      it.text.push_back(ch < 0x100 ? char(ch) : '?');
    }
    if (flags & MF_POPUP) {
      uint32_t sub = new_menu(s);
      it.id = sub;
      p = parse_menu(s, sub, data, p);
    }
    if (it.text.empty() && !(flags & MF_POPUP) && it.id == 0) it.flags |= MF_SEPARATOR;
    if (Menu* m = menu_of(s, menu)) m->items.push_back(it);
    if (flags & MF_END) break;
  }
  return p;
}

// The item `item` names: a position (MF_BYPOSITION) or a command, searched
// through the submenus as Windows does. Null when absent.
MenuItem* find_item(User32State& s, uint32_t menu, uint32_t item, uint32_t flags, int depth = 0) {
  Menu* m = menu_of(s, menu);
  if (!m || depth > 16) return nullptr;
  if (flags & MF_BYPOSITION) return item < m->items.size() ? &m->items[item] : nullptr;
  for (MenuItem& it : m->items) {
    if (!(it.flags & MF_POPUP) && it.id == item) return &it;
    if (it.flags & MF_POPUP)
      if (MenuItem* in = find_item(s, it.id, item, flags, depth + 1)) return in;
  }
  return nullptr;
}

std::string resource_name(Call& c, int i) {
  uint32_t p = c.arg(i);
  return p < 0x10000 ? "#" + std::to_string(p) : c.str(i);
}

loader::ResId res_id(Call& c, int i) {
  uint32_t p = c.arg(i);
  loader::ResId name = p < 0x10000 ? loader::ResId::of(uint16_t(p)) : loader::ResId::of(c.str(i));
  if (name.is_string && !name.str.empty() && name.str[0] == '#')
    name = loader::ResId::of(uint16_t(strtoul(name.str.c_str() + 1, nullptr, 10)));
  return name;
}

std::string_view resource_bytes(Runtime& rt, uint32_t hinst, uint16_t type, const loader::ResId& name) {
  const loader::pe::Resource* res = find_module_resource(rt, hinst, loader::ResId::of(type), name);
  Module* m = rt.modules().by_handle(hinst);
  if (!res || !m || !m->image) return {};
  return m->image->resource_data(*res);
}

// ---- accelerators -----------------------------------------------------------------------------

// The shift state an accelerator entry asks for, as the keyboard has it now.
bool accel_shift_matches(Runtime& rt, const Accel& a) {
  const InputState& in = rt.input();
  bool shift = in.keys.test(VK_SHIFT), ctrl = in.keys.test(VK_CONTROL), alt = in.keys.test(VK_MENU);
  return shift == bool(a.virt & FSHIFT) && ctrl == bool(a.virt & FCONTROL) && alt == bool(a.virt & FALT);
}

// ---- input (user32_program_key / _mouse) ------------------------------------------------------

// A key's scan code on a US 101-key keyboard (set 1), for a key message's
// lParam bits 16..23 (0 for a key it lacks), and whether it is an extended key.
uint8_t scan_code(uint32_t vk) {
  static constexpr char kLetters[] = "\x1E\x30\x2E\x20\x12\x21\x22\x23\x17\x24\x25\x26\x32\x31\x18\x19\x10\x13\x1F"
                                     "\x14\x16\x2F\x11\x2D\x15\x2C";  // A..Z
  static constexpr uint8_t kPad[] = {0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49,  // NUMPAD0..9
                                     0x37, 0x4E, 0, 0x4A, 0x53, 0x35};  // * + sep - . /
  if (vk >= 'A' && vk <= 'Z') return uint8_t(kLetters[vk - 'A']);
  if (vk >= '1' && vk <= '9') return uint8_t(vk - '1' + 2);
  if (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) return kPad[vk - VK_NUMPAD0];
  if (vk >= VK_F1 && vk <= VK_F10) return uint8_t(vk - VK_F1 + 0x3B);
  switch (vk) {
    case '0': return 0x0B;
    case VK_ESCAPE: return 0x01;
    case VK_BACK: return 0x0E;
    case VK_TAB: return 0x0F;
    case VK_RETURN: return 0x1C;
    case VK_CONTROL: return 0x1D;
    case VK_SHIFT: return 0x2A;
    case VK_MENU: return 0x38;
    case VK_SPACE: return 0x39;
    case VK_CAPITAL: return 0x3A;
    case VK_NUMLOCK: return 0x45;
    case VK_F11: return 0x57;
    case VK_F12: return 0x58;
    case VK_HOME: return 0x47;
    case VK_UP: return 0x48;
    case VK_PRIOR: return 0x49;
    case VK_LEFT: return 0x4B;
    case VK_RIGHT: return 0x4D;
    case VK_END: return 0x4F;
    case VK_DOWN: return 0x50;
    case VK_NEXT: return 0x51;
    case VK_INSERT: return 0x52;
    case VK_DELETE: return 0x53;
    case VK_OEM_1: return 0x27;
    case VK_OEM_PLUS: return 0x0D;
    case VK_OEM_COMMA: return 0x33;
    case VK_OEM_MINUS: return 0x0C;
    case VK_OEM_PERIOD: return 0x34;
    case VK_OEM_2: return 0x35;
    case VK_OEM_3: return 0x29;
    case VK_OEM_4: return 0x1A;
    case VK_OEM_5: return 0x2B;
    case VK_OEM_6: return 0x1B;
    case VK_OEM_7: return 0x28;
    default: return 0;
  }
}

bool extended_key(uint32_t vk) {
  return (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE || vk == VK_DIVIDE ||
         vk == VK_NUMLOCK;
}

// The character TranslateMessage makes of a key going down on a US keyboard
// (0 = none), with Shift, Ctrl and Caps Lock as they stand.
uint32_t key_char(uint32_t vk, bool shift, bool ctrl, bool caps) {
  if (vk >= 'A' && vk <= 'Z') {
    if (ctrl) return vk - 'A' + 1;
    return (shift != caps) ? vk : vk + 32;
  }
  if (ctrl) return 0;
  if (vk >= '0' && vk <= '9') return shift ? uint8_t(")!@#$%^&*("[vk - '0']) : vk;
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return '0' + (vk - VK_NUMPAD0);
  struct Pair {
    uint32_t vk;
    char plain, shifted;
  };
  static constexpr Pair kKeys[] = {
      {VK_SPACE, ' ', ' '},     {VK_RETURN, '\r', '\r'},  {VK_ESCAPE, 27, 27},        {VK_BACK, 8, 8},
      {VK_TAB, 9, 9},           {VK_MULTIPLY, '*', '*'},  {VK_ADD, '+', '+'},         {VK_SUBTRACT, '-', '-'},
      {VK_DECIMAL, '.', '.'},   {VK_DIVIDE, '/', '/'},    {VK_OEM_1, ';', ':'},       {VK_OEM_PLUS, '=', '+'},
      {VK_OEM_COMMA, ',', '<'}, {VK_OEM_MINUS, '-', '_'}, {VK_OEM_PERIOD, '.', '>'}, {VK_OEM_2, '/', '?'},
      {VK_OEM_3, '`', '~'},     {VK_OEM_4, '[', '{'},     {VK_OEM_5, '\\', '|'},      {VK_OEM_6, ']', '}'},
      {VK_OEM_7, '\'', '"'},
  };
  for (const Pair& p : kKeys)
    if (p.vk == vk) return uint8_t(shift ? p.shifted : p.plain);
  return 0;
}

// Replaces user32.cc's handler for `name` with one that runs `fn` in program
// mode and the original otherwise.
void program_only(ShimRegistry& r, const char* name, std::function<void(Call&)> fn) {
  ShimEntry* e = r.find(U, name);
  ShimFn prev = e ? e->fn : ShimFn();
  r.impl(U, name, [prev, fn](Call& c) {
    if (program_on(c.rt) || !prev) return fn(c);
    prev(c);
  });
}

}  // namespace

namespace user32 {

bool program_paint_message(Runtime& rt, uint32_t hwnd_filter, g32::MSG& out) {
  User32State& s = us(rt);
  for (Window& w : s.windows) {
    if (!w.alive || !w.invalid || w.hwnd == s.desktop) continue;
    if (hwnd_filter && hwnd_filter != w.hwnd) continue;
    if (!really_visible(s, w)) continue;
    out = g32::MSG{w.hwnd, WM_PAINT, 0, 0, rt.clock().tick_count(), s.cursor_pos.x, s.cursor_pos.y};
    return true;
  }
  return false;
}

uint32_t menu_from_template(Runtime& rt, std::string_view data) {
  if (data.size() < 4) return 0;
  uint16_t offset;
  memcpy(&offset, data.data() + 2, 2);  // after the version word
  User32State& s = us(rt);
  uint32_t h = new_menu(s);
  parse_menu(s, h, data, 4 + size_t(offset));
  return h;
}

uint32_t accelerators_from_table(Runtime& rt, std::string_view data) {
  if (data.size() < 8) return 0;
  std::vector<Accel> table;
  for (size_t p = 0; p + 8 <= data.size(); p += 8) {
    uint16_t f, key, cmd;
    memcpy(&f, data.data() + p, 2);
    memcpy(&key, data.data() + p + 2, 2);
    memcpy(&cmd, data.data() + p + 4, 2);
    table.push_back(Accel{uint8_t(f & 0x7F), key, cmd});
    if (f & 0x80) break;  // the last entry
  }
  User32State& s = us(rt);
  s.accels.push_back(std::move(table));
  return kAccelBase + uint32_t(s.accels.size() - 1) * kHandleStep;
}

}  // namespace user32

void user32_set_program(Runtime& rt, bool on) { us(rt).program = on; }
bool user32_program(Runtime& rt) { return us(rt).program; }
std::string user32_last_message_box(Runtime& rt) { return us(rt).last_box; }
bool user32_cursor_shown(Runtime& rt) { return us(rt).cursor != 0 && us(rt).cursor_count >= 0; }

void user32_program_key(Runtime& rt, uint32_t vk, bool down) {
  User32State& s = us(rt);
  vk &= 0xFF;
  const bool was = s.keys_down[vk];
  s.keys_down[vk] = down;
  const uint32_t target = s.get(s.focus) ? s.focus : s.get(s.active) ? s.active : 0;
  if (!target) return;
  // Alt (without Ctrl) and F10 make system keys; lParam: a repeat count of 1,
  // the scan code, extended, Alt held (29), down before (30), going up (31).
  const bool alt = s.keys_down[VK_MENU] || vk == VK_MENU;
  const bool sys = (alt && !s.keys_down[VK_CONTROL]) || vk == VK_F10;
  const uint32_t msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
  const uint32_t lp = 1 | uint32_t(scan_code(vk)) << 16 | (extended_key(vk) ? 1u << 24 : 0) |
                      (sys && alt ? 1u << 29 : 0) | (was ? 1u << 30 : 0) | (down ? 0 : 1u << 31);
  s.input.push_back(g32::MSG{target, msg, vk, lp, rt.clock().tick_count(), s.cursor_pos.x, s.cursor_pos.y});
}

void user32_program_mouse(Runtime& rt, int32_t x, int32_t y, uint32_t buttons) {
  User32State& s = us(rt);
  x = std::clamp(x, 0, screen_w(rt) - 1);
  y = std::clamp(y, 0, screen_h(rt) - 1);
  const bool moved = !s.mouse_seen || x != s.cursor_pos.x || y != s.cursor_pos.y;
  const uint32_t before = s.mouse_buttons;
  s.mouse_seen = true;
  s.cursor_pos = POINT{x, y};
  s.mouse_buttons = buttons;
  // The capture window, else the window under the pointer (none: the desktop's, dropped).
  uint32_t target = s.get(s.capture) ? s.capture : window_at(s, s.cursor_pos);
  Window* w = s.get(target);
  if (!w || (w->style & WS_DISABLED)) return;
  const POINT o = client_origin(s, target);
  const uint32_t lp = makelong(x - o.x, y - o.y);
  const uint32_t now = rt.clock().tick_count();
  uint32_t mk = (buttons & kMouseLeft ? MK_LBUTTON : 0) | (buttons & kMouseRight ? MK_RBUTTON : 0) |
                (buttons & kMouseMiddle ? MK_MBUTTON : 0) | (s.keys_down[VK_SHIFT] ? MK_SHIFT : 0) |
                (s.keys_down[VK_CONTROL] ? MK_CONTROL : 0);
  auto queue = [&](uint32_t msg) { s.input.push_back(g32::MSG{target, msg, mk, lp, now, x, y}); };
  if (moved) {
    // Moves coalesce, as Windows' input queue merged them.
    if (!s.input.empty() && s.input.back().message == WM_MOUSEMOVE && s.input.back().hwnd == target) {
      s.input.back() = g32::MSG{target, WM_MOUSEMOVE, mk, lp, now, x, y};
    } else {
      queue(WM_MOUSEMOVE);
    }
  }
  struct Button {
    uint32_t bit, down, up, dbl;
  };
  static constexpr Button kButtons[] = {{kMouseLeft, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK},
                                        {kMouseRight, WM_RBUTTONDOWN, WM_RBUTTONUP, WM_RBUTTONDBLCLK},
                                        {kMouseMiddle, WM_MBUTTONDOWN, WM_MBUTTONUP, WM_MBUTTONDBLCLK}};
  for (const Button& b : kButtons) {
    if ((before ^ buttons) & b.bit) {
      if (!(buttons & b.bit)) {
        queue(b.up);
        continue;
      }
      // A second press of the button within the double-click time (500 ms)
      // and rectangle (4x4) on a CS_DBLCLKS window is a double click.
      WndClass* k = s.find_class(w->class_name);
      const auto& lc = s.last_click;
      bool dbl = k && (k->style & CS_DBLCLKS) && lc.hwnd == target && lc.button == b.bit && now - lc.time <= 500 &&
                 std::abs(x - lc.pt.x) <= 2 && std::abs(y - lc.pt.y) <= 2;
      queue(dbl ? b.dbl : b.down);
      s.last_click = dbl ? User32State::Click{} : User32State::Click{target, b.bit, now, POINT{x, y}};
    }
  }
}

void register_user32_program(ShimRegistry& r) {
  // ---- windows (replacing user32.cc's in program mode) ----
  program_only(r, "CreateWindowExA", [](Call& c) { c.ret(create_window(c)); });
  program_only(r, "DestroyWindow", [](Call& c) {
    if (!destroy_window(c.rt, c.arg(0))) {
      c.set_last_error(ERROR_INVALID_WINDOW_HANDLE);
      return c.ret(0);
    }
    c.ret(1);
  });
  program_only(r, "DefWindowProcA", [](Call& c) { c.ret(def_window_proc(c.rt, c.arg(0), c.arg(1), c.arg(2), c.arg(3))); });
  program_only(r, "ShowWindow", [](Call& c) { c.ret(show_window(c.rt, c.arg(0), c.iarg(1))); });
  program_only(r, "SetWindowPos", [](Call& c) {
    c.ret_bool(set_window_pos(c.rt, c.arg(0), c.arg(1), c.iarg(2), c.iarg(3), c.iarg(4), c.iarg(5), c.arg(6)));
  });
  program_only(r, "MoveWindow", [](Call& c) {
    c.ret_bool(set_window_pos(c.rt, c.arg(0), 0, c.iarg(1), c.iarg(2), c.iarg(3), c.iarg(4),
                              SWP_NOZORDER | SWP_NOACTIVATE | (c.arg(5) ? 0 : SWP_NOREDRAW)));
  });
  program_only(r, "GetClientRect", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w && c.arg(0) != us(c.rt).desktop) {
      c.set_last_error(ERROR_INVALID_WINDOW_HANDLE);
      return c.ret(0);
    }
    RECT rc = w ? RECT{0, 0, w->client.right - w->client.left, w->client.bottom - w->client.top}
                : RECT{0, 0, screen_w(c.rt), screen_h(c.rt)};
    write_pod(c.mem(), c.arg(1), rc);
    c.ret(1);
  });
  program_only(r, "GetParent", [](Call& c) {
    // A child's parent, a popup's owner; an overlapped window has none.
    Window* w = us(c.rt).get(c.arg(0));
    c.ret(w && (is_child(*w) || (w->style & WS_POPUP)) ? w->parent : 0);
  });
  program_only(r, "IsWindowVisible", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    c.ret_bool(w && really_visible(us(c.rt), *w));
  });
  program_only(r, "EnableWindow", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    bool was_disabled = w->style & WS_DISABLED;
    bool enable = c.arg(1);
    if (enable == was_disabled) {
      if (enable) w->style &= ~uint32_t(WS_DISABLED);
      else w->style |= WS_DISABLED;
      send(c.rt, c.arg(0), WM_ENABLE, enable, 0);
    }
    c.ret_bool(was_disabled);
  });
  // A child by its ID (MFC finds its view, AFX_IDW_PANE_FIRST, this way), and what goes through it.
  auto dlg_item = [](Runtime& rt, uint32_t parent, uint32_t id) -> uint32_t {
    for (uint32_t h : children_of(us(rt), parent))
      if (Window* w = us(rt).get(h); w && w->id == (id & 0xFFFF)) return h;
    return 0;
  };
  program_only(r, "GetDlgItem", [dlg_item](Call& c) { c.ret(dlg_item(c.rt, c.arg(0), c.arg(1))); });
  program_only(r, "SendDlgItemMessageA", [dlg_item](Call& c) {
    uint32_t h = dlg_item(c.rt, c.arg(0), c.arg(1));
    c.ret(h ? send(c.rt, h, c.arg(2), c.arg(3), c.arg(4)) : 0);
  });
  program_only(r, "SetDlgItemTextA", [dlg_item](Call& c) {
    uint32_t h = dlg_item(c.rt, c.arg(0), c.arg(1));
    c.ret_bool(h && send(c.rt, h, WM_SETTEXT, 0, c.arg(2)));
  });
  program_only(r, "CheckDlgButton", [dlg_item](Call& c) {
    uint32_t h = dlg_item(c.rt, c.arg(0), c.arg(1));
    if (h) send(c.rt, h, BM_SETCHECK, c.arg(2), 0);
    c.ret_bool(h != 0);
  });
  program_only(r, "IsDlgButtonChecked", [dlg_item](Call& c) {
    uint32_t h = dlg_item(c.rt, c.arg(0), c.arg(1));
    c.ret(h ? send(c.rt, h, BM_GETCHECK, 0, 0) : 0);
  });
  program_only(r, "GetActiveWindow", [](Call& c) { c.ret(us(c.rt).active); });
  program_only(r, "SetFocus", [](Call& c) { c.ret(set_focus(c.rt, c.arg(0))); });
  program_only(r, "GetSystemMetrics", [](Call& c) {
    int w = screen_w(c.rt), h = screen_h(c.rt);
    int v = 0;
    switch (c.iarg(0)) {
      case SM_CXSCREEN: case SM_CXFULLSCREEN: v = w; break;
      case SM_CYSCREEN: v = h; break;
      case SM_CYFULLSCREEN: v = h - kCaption; break;
      case SM_CXMAXIMIZED: v = w + 2 * kFrame; break;
      case SM_CYMAXIMIZED: v = h + 2 * kFrame; break;
      case SM_CXMAXTRACK: v = w + 2 * kFrame + 4; break;
      case SM_CYMAXTRACK: v = h + 2 * kFrame + 4; break;
      case SM_CXICON: case SM_CYICON: case SM_CXCURSOR: case SM_CYCURSOR: v = 32; break;
      case SM_CXICONSPACING: case SM_CYICONSPACING: v = 75; break;
      case SM_CXSMICON: case SM_CYSMICON: v = 16; break;
      case SM_CYCAPTION: v = kCaption; break;
      case SM_CYSMCAPTION: v = kSmCaption; break;
      case SM_CYMENU: v = kMenuBar; break;
      case SM_CXBORDER: case SM_CYBORDER: v = kBorder; break;
      case SM_CXEDGE: case SM_CYEDGE: v = kEdge; break;
      case SM_CXDLGFRAME: case SM_CYDLGFRAME: v = kDlgFrame; break;
      case SM_CXFRAME: case SM_CYFRAME: v = kFrame; break;
      case SM_CXVSCROLL: case SM_CYHSCROLL: case SM_CYVSCROLL: case SM_CXHSCROLL: case SM_CXHTHUMB:
      case SM_CYVTHUMB: v = 16; break;
      case SM_CXSIZE: case SM_CYSIZE: v = 18; break;
      case SM_CXMIN: v = 112; break;
      case SM_CYMIN: v = 27; break;
      case SM_CXMINTRACK: v = 112; break;
      case SM_CYMINTRACK: v = 27; break;
      case SM_CXDOUBLECLK: case SM_CYDOUBLECLK: v = 4; break;
      case SM_CXDRAG: case SM_CYDRAG: v = 2; break;
      case SM_MOUSEPRESENT: v = 1; break;
      case SM_CMOUSEBUTTONS: v = 2; break;
      default: v = 0; break;
    }
    c.ret(uint32_t(v));
  });
  program_only(r, "GetCursorPos", [](Call& c) {
    write_pod(c.mem(), c.arg(0), us(c.rt).cursor_pos);
    c.ret(1);
  });

  // ---- painting ----
  program_only(r, "BeginPaint", [](Call& c) {
    User32State& s = us(c.rt);
    Window* w = s.get(c.arg(0));
    if (!w) return c.ret(0);
    uint32_t hdc = c.rt.state<GdiTable>().screen_dc();
    g32::PAINTSTRUCT ps{};
    ps.hdc = hdc;
    ps.rcPaint = w->invalid ? w->update : RECT{0, 0, 0, 0};
    bool erase = w->erase;
    w->invalid = w->erase = false;
    ps.fErase = erase ? !send(c.rt, c.arg(0), WM_ERASEBKGND, hdc, 0) : FALSE;
    write_pod(c.mem(), c.arg(1), ps);
    c.ret(hdc);
  });
  program_only(r, "EndPaint", [](Call& c) { c.ret(1); });
  program_only(r, "InvalidateRect", [](Call& c) {
    User32State& s = us(c.rt);
    uint32_t hwnd = c.arg(0);
    bool erase = c.arg(2);
    auto one = [&](Window& w) {
      RECT whole{0, 0, w.client.right - w.client.left, w.client.bottom - w.client.top};
      RECT r = c.arg(1) ? c.pod<RECT>(1) : whole;
      IntersectRect(&r, &r, &whole);
      if (IsRectEmpty(&r)) return;
      if (w.invalid) UnionRect(&w.update, &w.update, &r);
      else w.update = r;
      w.invalid = true;
      w.erase = w.erase || erase;
    };
    if (!hwnd) {
      for (Window& w : s.windows)
        if (w.alive && w.hwnd != s.desktop) one(w);
      return c.ret(1);
    }
    Window* w = s.get(hwnd);
    if (!w) return c.ret(0);
    one(*w);
    c.ret(1);
  });
  r.impl(U, "ValidateRect", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    if (!c.arg(1)) {
      w->invalid = w->erase = false;
    } else {
      RECT v = c.pod<RECT>(1), in;
      IntersectRect(&in, &v, &w->update);
      if (EqualRect(&in, &w->update)) w->invalid = w->erase = false;
    }
    c.ret(1);
  });
  program_only(r, "UpdateWindow", [](Call& c) {
    User32State& s = us(c.rt);
    std::function<void(uint32_t)> paint = [&](uint32_t hwnd) {
      Window* w = s.get(hwnd);
      if (!w || !really_visible(s, *w)) return;
      if (w->invalid) send(c.rt, hwnd, WM_PAINT, 0, 0);
      for (uint32_t ch : children_of(s, hwnd)) paint(ch);
    };
    if (!s.get(c.arg(0))) return c.ret(0);
    paint(c.arg(0));
    c.ret(1);
  });
  program_only(r, "GetDC", [](Call& c) { c.ret(c.rt.state<GdiTable>().screen_dc()); });
  r.impl(U, "GetWindowDC", [](Call& c) { c.ret(c.rt.state<GdiTable>().screen_dc()); });
  r.impl(U, "ExcludeUpdateRgn", [](Call& c) { c.ret(NULLREGION); });

  // ---- the message loop ----
  r.impl(U, "GetMessageA", [](Call& c) {
    // (msg, hwnd, min, max)
    User32State& s = us(c.rt);
    for (;;) {
      audio_pump(c.rt);
      g32::MSG m{};
      if (next_message(c.rt, c.arg(1), c.arg(2), c.arg(3), true, m)) {
        s.msg_time = m.time;
        s.msg_pos = makelong(m.x, m.y);
        write_pod(c.mem(), c.arg(0), m);
        return c.ret(m.message == WM_QUIT ? 0 : 1);
      }
      s.waits++;
      if (!c.rt.yield_frame()) {
        throw GuestError(GuestError::Kind::fatal,
                         "GetMessageA waits for a message, and nothing can end the wait (no frames)");
      }
    }
  });
  program_only(r, "PeekMessageA", [](Call& c) {
    User32State& s = us(c.rt);
    audio_pump(c.rt);
    g32::MSG m{};
    bool remove = c.arg(4) & PM_REMOVE;
    if (!next_message(c.rt, c.arg(1), c.arg(2), c.arg(3), remove, m)) return c.ret(0);
    s.msg_time = m.time;
    s.msg_pos = makelong(m.x, m.y);
    write_pod(c.mem(), c.arg(0), m);
    c.ret(1);
  });
  program_only(r, "TranslateMessage", [](Call& c) {
    // A key going down posts its character (WM_CHAR, WM_SYSCHAR with Alt);
    // nonzero for every key message, as Windows answered.
    g32::MSG m = c.pod<g32::MSG>(0);
    if (m.message < WM_KEYFIRST || m.message > WM_KEYLAST) return c.ret(0);
    if (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN) {
      const User32State& s = us(c.rt);
      uint32_t ch = key_char(m.wParam & 0xFF, s.keys_down[VK_SHIFT], s.keys_down[VK_CONTROL], c.rt.input().caps);
      if (ch) post_guest_message(c.rt, m.hwnd, m.message == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, ch, m.lParam);
    }
    c.ret(1);
  });
  r.impl(U, "GetMessageTime", [](Call& c) { c.ret(us(c.rt).msg_time); });
  r.impl(U, "GetMessagePos", [](Call& c) { c.ret(us(c.rt).msg_pos); });
  r.impl(U, "CallWindowProcA", [](Call& c) {
    uint32_t proc = c.arg(0);
    if (!proc) return c.ret(def_window_proc(c.rt, c.arg(1), c.arg(2), c.arg(3), c.arg(4)));
    c.ret(c.rt.call_guest(proc, {c.arg(1), c.arg(2), c.arg(3), c.arg(4)}));
  });
  r.impl(U, "RegisterWindowMessageA", [](Call& c) {
    User32State& s = us(c.rt);
    std::string name = c.str(0);
    auto ieq = [&](const std::string& a) { return _stricmp(a.c_str(), name.c_str()) == 0; };
    auto it = std::find_if(s.window_messages.begin(), s.window_messages.end(), ieq);
    if (it == s.window_messages.end()) it = s.window_messages.insert(s.window_messages.end(), name);
    c.ret(0xC000 + uint32_t(it - s.window_messages.begin()));
  });

  // ---- hooks (emulated: never the host's) ----
  r.impl(U, "SetWindowsHookExA", [](Call& c) {
    // (idHook, lpfn, hmod, dwThreadId)
    User32State& s = us(c.rt);
    Hook h;
    h.type = c.iarg(0);
    h.proc = c.arg(1);
    h.handle = kHookBase + uint32_t(s.hooks.size()) * kHandleStep;
    s.hooks.push_back(h);
    trace("user", "SetWindowsHookEx(%d, 0x%X) -> 0x%X", h.type, h.proc, h.handle);
    c.ret(h.handle);
  });
  r.impl(U, "UnhookWindowsHookEx", [](Call& c) {
    User32State& s = us(c.rt);
    for (Hook& h : s.hooks) {
      if (h.handle == c.arg(0) && h.alive) {
        h.alive = false;
        return c.ret(1);
      }
    }
    c.ret(0);
  });
  r.impl(U, "CallNextHookEx", [](Call& c) {
    // (hhk, nCode, wParam, lParam): the next older hook of the same type.
    User32State& s = us(c.rt);
    size_t i = 0;
    while (i < s.hooks.size() && s.hooks[i].handle != c.arg(0)) i++;
    if (i >= s.hooks.size()) return c.ret(0);
    int32_t type = s.hooks[i].type;
    while (i-- > 0) {
      if (s.hooks[i].alive && s.hooks[i].type == type)
        return c.ret(c.rt.call_guest(s.hooks[i].proc, {c.arg(1), c.arg(2), c.arg(3)}));
    }
    c.ret(0);
  });

  // ---- the window tree ----
  r.impl(U, "GetDesktopWindow", [](Call& c) {
    User32State& s = us(c.rt);
    if (!s.desktop) {
      Window w;
      w.class_name = "#32769";
      w.style = WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN;
      w.rect = w.client = RECT{0, 0, screen_w(c.rt), screen_h(c.rt)};
      s.desktop = s.add(std::move(w));
    }
    c.ret(s.desktop);
  });
  r.impl(U, "IsWindow", [](Call& c) { c.ret_bool(us(c.rt).get(c.arg(0)) != nullptr); });
  r.impl(U, "IsWindowEnabled", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    c.ret_bool(w && !(w->style & WS_DISABLED));
  });
  r.impl(U, "IsIconic", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    c.ret_bool(w && (w->style & WS_MINIMIZE));
  });
  r.impl(U, "IsChild", [](Call& c) { c.ret_bool(is_descendant(us(c.rt), c.arg(1), c.arg(0))); });
  r.impl(U, "IsWindowUnicode", [](Call& c) { c.ret(0); });
  r.impl(U, "GetDlgCtrlID", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    c.ret(w && is_child(*w) ? w->id : 0);
  });
  r.impl(U, "GetWindowTextLengthA", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    c.ret(w ? uint32_t(w->title.size()) : 0);
  });
  r.impl(U, "GetClassNameA", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w || !c.arg(2)) return c.ret(0);
    c.ret(uint32_t(write_cstr(c.mem(), c.arg(1), w->class_name, c.arg(2))));
  });
  r.impl(U, "GetClassInfoA", [](Call& c) {
    // (hInstance, lpClassName, lpWndClass)
    WndClass* k = us(c.rt).find_class(class_arg(c.rt, c.arg(1)));
    if (!k) {
      c.set_last_error(ERROR_CLASS_DOES_NOT_EXIST);
      return c.ret(0);
    }
    g32::WNDCLASSA a{k->style, k->wndproc, k->cls_extra, k->wnd_extra, k->instance, k->icon, k->cursor,
                     k->background, k->menu_name, c.arg(1)};
    write_pod(c.mem(), c.arg(2), a);
    c.ret(1);
  });
  r.impl(U, "GetTopWindow", [](Call& c) {
    User32State& s = us(c.rt);
    uint32_t parent = c.arg(0) == s.desktop ? 0 : c.arg(0);
    auto v = siblings(s, parent);
    c.ret(v.empty() ? 0 : v.front());
  });
  r.impl(U, "GetWindow", [](Call& c) {
    User32State& s = us(c.rt);
    Window* w = s.get(c.arg(0));
    if (!w) return c.ret(0);
    uint32_t cmd = c.arg(1);
    if (cmd == GW_OWNER) return c.ret(!is_child(*w) ? w->parent : 0);
    if (cmd == GW_CHILD) {
      auto v = children_of(s, c.arg(0) == s.desktop ? 0 : c.arg(0));
      return c.ret(v.empty() ? 0 : v.front());
    }
    auto v = siblings(s, is_child(*w) ? w->parent : 0);
    auto it = std::find(v.begin(), v.end(), c.arg(0));
    if (v.empty()) return c.ret(0);
    switch (cmd) {
      case GW_HWNDFIRST: return c.ret(v.front());
      case GW_HWNDLAST: return c.ret(v.back());
      case GW_HWNDNEXT: return c.ret(it != v.end() && it + 1 != v.end() ? *(it + 1) : 0);
      case GW_HWNDPREV: return c.ret(it != v.end() && it != v.begin() ? *(it - 1) : 0);
      default: return c.ret(0);
    }
  });
  r.impl(U, "WindowFromPoint", [](Call& c) {
    // (POINT by value: x, y): the deepest visible window there, topmost first.
    User32State& s = us(c.rt);
    uint32_t h = window_at(s, POINT{c.iarg(0), c.iarg(1)});
    c.ret(h ? h : (s.desktop ? s.desktop : 0));
  });
  r.impl(U, "GetForegroundWindow", [](Call& c) { c.ret(us(c.rt).active); });
  r.impl(U, "SetForegroundWindow", [](Call& c) {
    uint32_t top = toplevel_of(us(c.rt), c.arg(0));
    if (!top) return c.ret(0);
    activate(c.rt, top);
    c.ret(1);
  });
  r.impl(U, "SetActiveWindow", [](Call& c) {
    uint32_t prev = us(c.rt).active;
    uint32_t top = toplevel_of(us(c.rt), c.arg(0));
    if (top) activate(c.rt, top);
    c.ret(prev);
  });
  r.impl(U, "BringWindowToTop", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    c.ret_bool(set_window_pos(c.rt, c.arg(0), 0, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE));
  });
  r.impl(U, "GetFocus", [](Call& c) { c.ret(us(c.rt).focus); });
  r.impl(U, "GetCapture", [](Call& c) { c.ret(us(c.rt).capture); });
  r.impl(U, "SetCapture", [](Call& c) {
    User32State& s = us(c.rt);
    uint32_t prev = s.capture;
    s.capture = s.get(c.arg(0)) ? c.arg(0) : 0;
    c.ret(prev);
  });
  r.impl(U, "ReleaseCapture", [](Call& c) {
    User32State& s = us(c.rt);
    uint32_t prev = s.capture;
    s.capture = 0;
    if (prev && s.get(prev)) send(c.rt, prev, WM_CAPTURECHANGED, 0, 0);
    c.ret(1);
  });
  r.impl(U, "ShowOwnedPopups", [](Call& c) { c.ret(1); });
  r.impl(U, "SetCursorPos", [](Call& c) {
    // The emulated pointer only (the host's cursor never moves).
    User32State& s = us(c.rt);
    s.cursor_pos = POINT{std::clamp<LONG>(c.iarg(0), 0, screen_w(c.rt) - 1),
                         std::clamp<LONG>(c.iarg(1), 0, screen_h(c.rt) - 1)};
    c.ret(1);
  });

  // ---- coordinates ----
  r.impl(U, "ClientToScreen", [](Call& c) {
    if (!us(c.rt).get(c.arg(0))) return c.ret(0);
    POINT o = client_origin(us(c.rt), c.arg(0)), p = c.pod<POINT>(1);
    write_pod(c.mem(), c.arg(1), POINT{p.x + o.x, p.y + o.y});
    c.ret(1);
  });
  r.impl(U, "ScreenToClient", [](Call& c) {
    if (!us(c.rt).get(c.arg(0))) return c.ret(0);
    POINT o = client_origin(us(c.rt), c.arg(0)), p = c.pod<POINT>(1);
    write_pod(c.mem(), c.arg(1), POINT{p.x - o.x, p.y - o.y});
    c.ret(1);
  });
  r.impl(U, "MapWindowPoints", [](Call& c) {
    // (from, to, points, count) → MAKELONG(dx, dy)
    POINT a = client_origin(us(c.rt), c.arg(0)), b = client_origin(us(c.rt), c.arg(1));
    int dx = a.x - b.x, dy = a.y - b.y;
    for (uint32_t i = 0; i < c.arg(3); i++) {
      uint32_t p = c.arg(2) + 8 * i;
      POINT q = read_pod<POINT>(c.mem(), p);
      write_pod(c.mem(), p, POINT{q.x + dx, q.y + dy});
    }
    c.ret(makelong(dx, dy));
  });
  r.impl(U, "AdjustWindowRectEx", [](Call& c) {
    // (rect, style, menu, exstyle): the window rect around a client rect.
    Window w;
    w.style = c.arg(1);
    w.exstyle = c.arg(3);
    w.menu = c.arg(2) ? 1 : 0;
    RECT r = c.pod<RECT>(0);
    RECT in = client_inside(w, RECT{0, 0, 1000, 1000});
    r.left -= in.left;
    r.top -= in.top;
    r.right += 1000 - in.right;
    r.bottom += 1000 - in.bottom;
    write_pod(c.mem(), c.arg(0), r);
    c.ret(1);
  });

  // ---- DeferWindowPos ----
  struct Dwp {
    std::vector<std::array<uint32_t, 7>> moves;
  };
  auto dwps = std::make_shared<std::map<uint32_t, Dwp>>();
  auto next_dwp = std::make_shared<uint32_t>(kDwpBase);
  r.impl(U, "BeginDeferWindowPos", [dwps, next_dwp](Call& c) {
    uint32_t h = (*next_dwp += kHandleStep);
    (*dwps)[h] = Dwp{};
    c.ret(h);
  });
  r.impl(U, "DeferWindowPos", [dwps](Call& c) {
    auto it = dwps->find(c.arg(0));
    if (it == dwps->end()) return c.ret(0);
    it->second.moves.push_back({c.arg(1), c.arg(2), c.arg(3), c.arg(4), c.arg(5), c.arg(6), c.arg(7)});
    c.ret(c.arg(0));
  });
  r.impl(U, "EndDeferWindowPos", [dwps](Call& c) {
    auto it = dwps->find(c.arg(0));
    if (it == dwps->end()) return c.ret(0);
    Dwp d = std::move(it->second);
    dwps->erase(it);
    for (const auto& m : d.moves)
      set_window_pos(c.rt, m[0], m[1], int32_t(m[2]), int32_t(m[3]), int32_t(m[4]), int32_t(m[5]), m[6]);
    c.ret(1);
  });

  // ---- menus ----
  r.impl(U, "LoadMenuA", [](Call& c) {
    std::string_view data = resource_bytes(c.rt, c.arg(0), 4, res_id(c, 1));  // RT_MENU
    uint32_t h = menu_from_template(c.rt, data);
    if (!h) c.set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
    trace("user", "LoadMenu %s -> 0x%X", resource_name(c, 1).c_str(), h);
    c.ret(h);
  });
  r.impl(U, "DestroyMenu", [](Call& c) {
    Menu* m = menu_of(us(c.rt), c.arg(0));
    if (!m) return c.ret(0);
    m->alive = false;
    c.ret(1);
  });
  r.impl(U, "GetMenu", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    c.ret(w && !is_child(*w) ? w->menu : 0);
  });
  r.impl(U, "SetMenu", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w || is_child(*w)) return c.ret(0);
    w->menu = c.arg(1);
    set_window_pos(c.rt, c.arg(0), 0, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    c.ret(1);
  });
  r.impl(U, "GetSubMenu", [](Call& c) {
    MenuItem* it = find_item(us(c.rt), c.arg(0), c.arg(1), MF_BYPOSITION);
    c.ret(it && (it->flags & MF_POPUP) ? it->id : 0);
  });
  r.impl(U, "GetMenuItemCount", [](Call& c) {
    Menu* m = menu_of(us(c.rt), c.arg(0));
    c.ret(m ? uint32_t(m->items.size()) : uint32_t(-1));
  });
  r.impl(U, "GetMenuItemID", [](Call& c) {
    MenuItem* it = find_item(us(c.rt), c.arg(0), c.arg(1), MF_BYPOSITION);
    c.ret(!it ? uint32_t(-1) : (it->flags & MF_POPUP) ? uint32_t(-1) : it->id);
  });
  r.impl(U, "GetMenuState", [](Call& c) {
    User32State& s = us(c.rt);
    MenuItem* it = find_item(s, c.arg(0), c.arg(1), c.arg(2));
    if (!it) return c.ret(uint32_t(-1));
    uint32_t st = it->flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_POPUP | MF_SEPARATOR | MF_MENUBREAK |
                               MF_MENUBARBREAK | MF_OWNERDRAW | MF_BITMAP | MF_HILITE);
    if (it->flags & MF_POPUP) {
      Menu* sub = menu_of(s, it->id);
      st = (st & 0xFF) | (uint32_t(sub ? sub->items.size() : 0) << 8);
    }
    c.ret(st);
  });
  r.impl(U, "EnableMenuItem", [](Call& c) {
    MenuItem* it = find_item(us(c.rt), c.arg(0), c.arg(1), c.arg(2));
    if (!it) return c.ret(uint32_t(-1));
    uint32_t prev = it->flags & (MF_GRAYED | MF_DISABLED);
    it->flags = (it->flags & ~uint32_t(MF_GRAYED | MF_DISABLED)) | (c.arg(2) & (MF_GRAYED | MF_DISABLED));
    c.ret(prev);
  });
  r.impl(U, "CheckMenuItem", [](Call& c) {
    MenuItem* it = find_item(us(c.rt), c.arg(0), c.arg(1), c.arg(2));
    if (!it) return c.ret(uint32_t(-1));
    uint32_t prev = it->flags & MF_CHECKED;
    it->flags = (it->flags & ~uint32_t(MF_CHECKED)) | (c.arg(2) & MF_CHECKED);
    c.ret(prev);
  });
  r.impl(U, "ModifyMenuA", [](Call& c) {
    // (menu, position, flags, newID, newItem)
    User32State& s = us(c.rt);
    MenuItem* it = find_item(s, c.arg(0), c.arg(1), c.arg(2));
    if (!it) return c.ret(0);
    uint32_t f = c.arg(2) & ~uint32_t(MF_BYPOSITION);
    it->flags = f;
    it->id = c.arg(3);
    if (!(f & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) it->text = c.str(4);
    c.ret(1);
  });
  r.impl(U, "SetMenuItemBitmaps", [](Call& c) {
    c.ret_bool(find_item(us(c.rt), c.arg(0), c.arg(1), c.arg(2)) != nullptr);
  });
  r.impl(U, "GetMenuCheckMarkDimensions", [](Call& c) { c.ret(makelong(13, 13)); });
  r.impl(U, "TrackPopupMenu", [](Call& c) { c.ret(0); });  // nothing is chosen: no menu is ever shown

  // ---- accelerators ----
  r.impl(U, "LoadAcceleratorsA", [](Call& c) {
    std::string_view data = resource_bytes(c.rt, c.arg(0), 9, res_id(c, 1));  // RT_ACCELERATOR
    uint32_t h = accelerators_from_table(c.rt, data);
    if (!h) c.set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
    c.ret(h);
  });
  r.impl(U, "TranslateAcceleratorA", [](Call& c) {
    // (hwnd, haccel, msg): a key message an entry matches becomes WM_COMMAND (1 in HIWORD(wParam)).
    User32State& s = us(c.rt);
    uint32_t h = c.arg(1);
    size_t i = (h - kAccelBase) / kHandleStep;
    if (h < kAccelBase || i >= s.accels.size()) return c.ret(0);
    g32::MSG m = c.pod<g32::MSG>(2);
    bool keydown = m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN;
    bool chr = m.message == WM_CHAR || m.message == WM_SYSCHAR;
    if (!keydown && !chr) return c.ret(0);
    for (const Accel& a : s.accels[i]) {
      bool virt = a.virt & FVIRTKEY;
      if (virt ? !(keydown && m.wParam == a.key && accel_shift_matches(c.rt, a))
               : !(chr && m.wParam == a.key))
        continue;
      send(c.rt, c.arg(0), (m.message == WM_SYSKEYDOWN || m.message == WM_SYSCHAR) ? WM_SYSCOMMAND : WM_COMMAND,
           makelong(a.cmd, 1), 0);
      return c.ret(1);
    }
    c.ret(0);
  });

  // ---- icons ----
  r.impl(U, "LoadIconA", [](Call& c) {
    uint32_t hinst = c.arg(0), name = c.arg(1);
    if (!hinst) {
      if (name >= 32512 && name <= 32517) return c.ret(kSysIconBase + name);  // IDI_*
      c.set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
      return c.ret(0);
    }
    if (!find_module_resource(c.rt, hinst, loader::ResId::of(uint16_t(14)), res_id(c, 1))) {  // RT_GROUP_ICON
      c.set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
      return c.ret(0);
    }
    User32State& s = us(c.rt);
    std::pair<uint32_t, std::string> key{hinst, res_id(c, 1).to_string()};
    auto it = std::find(s.icons.begin(), s.icons.end(), key);
    if (it == s.icons.end()) it = s.icons.insert(s.icons.end(), key);
    c.ret(kIconBase + kHandleStep * uint32_t(it - s.icons.begin()));
  });
  r.impl(U, "DestroyIcon", [](Call& c) { c.ret(1); });

  // ---- rectangles ----
  r.impl(U, "SetRectEmpty", [](Call& c) {
    if (!c.arg(0)) return c.ret(0);
    write_pod(c.mem(), c.arg(0), RECT{0, 0, 0, 0});
    c.ret(1);
  });
  r.impl(U, "IsRectEmpty", [](Call& c) {
    RECT a = c.pod<RECT>(0);
    c.ret_bool(a.left >= a.right || a.top >= a.bottom);
  });
  r.impl(U, "EqualRect", [](Call& c) {
    RECT a = c.pod<RECT>(0), b = c.pod<RECT>(1);
    c.ret_bool(EqualRect(&a, &b));
  });
  r.impl(U, "PtInRect", [](Call& c) {
    // (rect, POINT by value)
    RECT a = c.pod<RECT>(0);
    POINT p{c.iarg(1), c.iarg(2)};
    c.ret_bool(PtInRect(&a, p));
  });
  r.impl(U, "OffsetRect", [](Call& c) {
    RECT a = c.pod<RECT>(0);
    OffsetRect(&a, c.iarg(1), c.iarg(2));
    write_pod(c.mem(), c.arg(0), a);
    c.ret(1);
  });
  r.impl(U, "InflateRect", [](Call& c) {
    RECT a = c.pod<RECT>(0);
    InflateRect(&a, c.iarg(1), c.iarg(2));
    write_pod(c.mem(), c.arg(0), a);
    c.ret(1);
  });
  r.impl(U, "IntersectRect", [](Call& c) {
    RECT a = c.pod<RECT>(1), b = c.pod<RECT>(2), out;
    BOOL ok = IntersectRect(&out, &a, &b);
    write_pod(c.mem(), c.arg(0), out);
    c.ret_bool(ok);
  });
  r.impl(U, "UnionRect", [](Call& c) {
    RECT a = c.pod<RECT>(1), b = c.pod<RECT>(2), out;
    BOOL ok = UnionRect(&out, &a, &b);
    write_pod(c.mem(), c.arg(0), out);
    c.ret_bool(ok);
  });

  // ---- strings and colours ----
  auto char_case = [](Call& c, bool up) {
    // A string pointer, or a character in the low word.
    uint32_t p = c.arg(0);
    auto conv = [up](char ch) { return up ? char(toupper(uint8_t(ch))) : char(tolower(uint8_t(ch))); };
    if (p < 0x10000) return c.ret(uint32_t(uint8_t(conv(char(p & 0xFF)))));
    std::string s = read_cstr(c.mem(), p);
    for (char& ch : s) ch = conv(ch);
    c.mem().memcpy(p, s.data(), s.size());
    c.ret(p);
  };
  r.impl(U, "CharUpperA", [char_case](Call& c) { char_case(c, true); });
  r.impl(U, "CharLowerA", [char_case](Call& c) { char_case(c, false); });
  r.impl(U, "CharNextA", [](Call& c) {
    uint32_t p = c.arg(0);
    c.ret(p && c.mem().read<uint8_t>(p) ? p + 1 : p);
  });
  r.impl(U, "GetSysColorBrush", [](Call& c) {
    GdiTable& g = c.rt.state<GdiTable>();
    GdiObject o;
    o.type = GdiType::brush;
    o.brush_style = BS_SOLID;
    o.stock = true;
    o.color = GetSysColor(int(c.arg(0)));
    COLORREF k = Display::key_color(g.display().map_index(o.color, nullptr));
    o.host = ::CreateSolidBrush(k);
    o.made_for = k;
    c.ret(o.host ? g.add(std::move(o)) : 0);
  });
  r.impl(U, "DrawFocusRect", [](Call& c) {
    HDC dc = c.rt.state<GdiTable>().host_dc(c.arg(0));
    RECT rc = c.pod<RECT>(1);
    c.ret_bool(dc && ::DrawFocusRect(dc, &rc));
  });

  // ---- what an MFC program imports for dialogs, help and DDE, never reached in ghost mode ----
  r.impl(U, "WinHelpA", [](Call& c) { c.ret(0); });
  r.impl(U, "IsDialogMessageA", [](Call& c) { c.ret(0); });
  r.impl(U, "DefDlgProcA", [](Call& c) { c.ret(def_window_proc(c.rt, c.arg(0), c.arg(1), c.arg(2), c.arg(3))); });
  r.impl(U, "GetNextDlgTabItem", [](Call& c) { c.ret(0); });
  r.impl(U, "HideCaret", [](Call& c) { c.ret(1); });
  r.impl(U, "ShowCaret", [](Call& c) { c.ret(1); });
  r.impl(U, "UnpackDDElParam", [](Call& c) {
    // (msg, lParam, puiLo, puiHi)
    if (c.arg(2)) c.mem().write_u32l(c.arg(2), c.arg(1) & 0xFFFF);
    if (c.arg(3)) c.mem().write_u32l(c.arg(3), c.arg(1) >> 16);
    c.ret(1);
  });
  r.impl(U, "ReuseDDElParam", [](Call& c) { c.ret(makelong(int32_t(c.arg(3)), int32_t(c.arg(4)))); });
}

}  // namespace adw::win32
