// USER32.DLL — windows, input state, formatting, resources, and the few
// drawing helpers that live in USER (API_SURFACE.md §1 "USER32.DLL", 56
// functions).
//
// The window model is host-side and tiny: the saver window every module draws
// in (host_window(), AD_MODULE32 +0x10), and whatever the guest creates itself
// (ADXPL510's CD-audio notification window "blargCD", ABI.md §2.12). Guest
// window procedures run through call_guest; nothing is ever shown. The lane
// drives the module by messages to Module(), not by window messages (ABI.md
// §2.6), and nobody pumps the thread's queue for it. The queue itself is real,
// for a module that looks at it (HALLOFFA.AD's AD Online framework imports
// the whole message-loop set): PostMessageA/PostQuitMessage/SetTimer feed it,
// PeekMessageA/DispatchMessageA drain it, timers fall due by the virtual
// clock (peeked, never nudged). Cursors are handles and counts only.
//
// Input (GetAsyncKeyState/GetKeyState/GetCursorPos) reads the protocol's
// InputState (KEY/CAPS/MOUSE lines), so it is deterministic under lockstep.
//
// Configure mode (adhostwin --configure, INTERACTION.md §6): user32_real.cc
// wraps these handlers so that real-window guest handles (realui.hh) act on
// real windows, and DialogBox*/MessageBoxA become real; the emulated model
// here is what the saver always uses.
//
// Input: GetAsyncKeyState sets bit 0 when the key went down since the
// previous call for it; VK_LBUTTON/VK_RBUTTON/VK_MBUTTON come from the MOUSE
// bitmask. WM_CLOSE or SC_CLOSE posted or sent to the saver window raises the
// wake flag the lane publishes (§5.1).
//
// Known gaps, deliberately left (no module of the 202 in the five releases
// reaches them: their census runs with 0 unimplemented API calls;
// API_SURFACE.md §1 USER32):
//   * LoadBitmapA: system bitmaps (hInstance NULL, OBM_*) are refused, and
//     compressed (RLE) resources are not decoded.
//   * LoadIconA, CopyIcon, DestroyIcon, DrawIcon, DrawIconEx, ExtractIconA
//     (Bad Dog's desktop icons), EnumWindows, GetClassNameA, GetClassLongA,
//     GetWindowPlacement: imported by Bad Dog, never called (the Classic
//     lane's synthetic desktop, win16/README.md, is where a module sees one).
//   * Scroll bars (GetScrollInfo/SetScrollInfo/EnableScrollBar) on emulated
//     windows: none has any.
//   * Nothing delivers posted messages or WM_TIMER unless the module pumps its
//     own queue (the 1996 host's GetMessage loop did). No module in the corpus
//     posts or sets a timer (HALLOFFA.AD imports them); the census says so
//     with a "[census] ...: SetTimer xN" line (shims.cc print_census), which
//     the pe32 tests check stays absent. If one ever does, the lane should
//     drain the queue between frames.
//   * SystemParametersInfoA answers the screen-saver and work-area queries
//     only; everything else reports failure.
#include <windows.h>

#include <algorithm>
#include <bitset>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include "adw/core/log.h"
#include "win32/display.hh"
#include "win32/gdi_objects.hh"
#include "win32/modules.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"
#include "win32/user32_state.hh"

namespace adw::win32 {

using namespace user32;

namespace user32 {

// A class name argument: a string pointer, or an atom in the low word.
std::string class_arg(Runtime& rt, uint32_t p) {
  if (p < 0x10000) {
    if (WndClass* c = us(rt).find_atom(p)) return c->name;
    return "#" + std::to_string(p);
  }
  return read_cstr(rt.mem(), p);
}

uint32_t send(Runtime& rt, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
  Window* w = us(rt).get(hwnd);
  if (!w) return 0;
  if (!w->wndproc) return msg == WM_NCCREATE ? 1 : 0;
  trace("user", "message 0x%04X to window 0x%X (%s)", msg, hwnd, w->class_name.c_str());
  return rt.call_guest(w->wndproc, {hwnd, msg, wparam, lparam});
}

// The next message PeekMessage would retrieve, in Windows' order: posted
// messages, then WM_QUIT, then due timers. hwnd_filter: 0 = any, -1 = thread
// messages only, else that window. min/max: 0/0 = every message.
bool next_message(Runtime& rt, uint32_t hwnd_filter, uint32_t min, uint32_t max, bool remove, g32::MSG& out) {
  User32State& s = us(rt);
  auto in_range = [&](uint32_t msg) { return (min == 0 && max == 0) || (msg >= min && msg <= max); };
  auto for_hwnd = [&](uint32_t hwnd) {
    return hwnd_filter == 0 || (hwnd_filter == 0xFFFFFFFF ? hwnd == 0 : hwnd == hwnd_filter);
  };
  for (auto it = s.posted.begin(); it != s.posted.end(); ++it) {
    if (!for_hwnd(it->hwnd) || !in_range(it->message)) continue;
    out = *it;
    if (remove) s.posted.erase(it);
    return true;
  }
  // A whole program's input queue (user32_program_key/_mouse), after posted messages.
  for (auto it = s.input.begin(); it != s.input.end(); ++it) {
    if (!for_hwnd(it->hwnd) || !in_range(it->message)) continue;
    out = *it;
    if (remove) s.input.erase(it);
    return true;
  }
  if (s.quit && in_range(WM_QUIT)) {
    out = g32::MSG{0, WM_QUIT, s.quit_code, 0, rt.clock().tick_count(), 0, 0};
    if (remove) s.quit = false;
    return true;
  }
  // A whole program's windows that need painting (never removed: a window
  // gets WM_PAINT until it validates its update region).
  if (s.program && in_range(WM_PAINT) && program_paint_message(rt, hwnd_filter, out)) return true;
  if (in_range(WM_TIMER)) {
    uint64_t now = rt.clock().now_us();
    Timer* due = nullptr;
    for (Timer& t : s.timers)
      if (t.due_us <= now && for_hwnd(t.hwnd) && (!due || t.due_us < due->due_us)) due = &t;
    if (due) {
      out = g32::MSG{due->hwnd, WM_TIMER, due->id, due->proc, rt.clock().tick_count(), 0, 0};
      if (remove) due->due_us = now + uint64_t(due->elapse_ms) * 1000;
      return true;
    }
  }
  return false;
}

// DispatchMessage: a WM_TIMER with a TIMERPROC calls it; everything else goes
// to the window's procedure.
uint32_t dispatch(Runtime& rt, const g32::MSG& m) {
  if (m.message == WM_TIMER && m.lParam) {
    rt.call_guest(m.lParam, {m.hwnd, WM_TIMER, m.wParam, rt.clock().tick_count()});
    return 0;
  }
  return m.hwnd ? send(rt, m.hwnd, m.message, m.wParam, m.lParam) : 0;
}

}  // namespace user32

namespace {

// ---- wsprintfA ----------------------------------------------------------------------------------
//
// USER32's formatter: %[-][#][0][width][.precision][l|h]{c,C,d,i,u,x,X,s,S},
// no floating point, output capped at 1024 characters (including the NUL).
std::string format_wsprintf(Runtime& rt, const std::string& fmt, uint32_t args) {
  auto& mem = rt.mem();
  std::string out;
  auto next = [&]() {
    uint32_t v = mem.read_u32l(args);
    args += 4;
    return v;
  };
  for (size_t i = 0; i < fmt.size(); i++) {
    char ch = fmt[i];
    if (ch != '%') {
      out.push_back(ch);
      continue;
    }
    if (++i >= fmt.size()) break;
    if (fmt[i] == '%') {
      out.push_back('%');
      continue;
    }
    bool left = false, alt = false, zero = false;
    for (; i < fmt.size(); i++) {
      if (fmt[i] == '-') left = true;
      else if (fmt[i] == '#') alt = true;
      else if (fmt[i] == '0') zero = true;
      else break;
    }
    int width = 0, prec = -1;
    while (i < fmt.size() && isdigit(uint8_t(fmt[i]))) width = width * 10 + (fmt[i++] - '0');
    if (i < fmt.size() && fmt[i] == '.') {
      prec = 0;
      i++;
      while (i < fmt.size() && isdigit(uint8_t(fmt[i]))) prec = prec * 10 + (fmt[i++] - '0');
    }
    bool shrt = false, wide = false;
    for (; i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h' || fmt[i] == 'L' || fmt[i] == 'w'); i++) {
      if (fmt[i] == 'h') shrt = true;
      if (fmt[i] == 'l' || fmt[i] == 'w') wide = true;
    }
    if (i >= fmt.size()) break;
    char t = fmt[i];
    std::string field;
    switch (t) {
      case 'c':
      case 'C':
        field.push_back(char(next() & 0xFF));
        break;
      case 'd':
      case 'i': {
        int32_t v = int32_t(next());
        if (shrt) v = int16_t(v);
        std::string digits = std::to_string(v < 0 ? -int64_t(v) : int64_t(v));
        if (prec >= 0 && int(digits.size()) < prec) digits.insert(0, size_t(prec - int(digits.size())), '0');
        field = (v < 0 ? "-" : "") + digits;
        break;
      }
      case 'u': {
        uint32_t v = next();
        if (shrt) v &= 0xFFFF;
        field = std::to_string(v);
        if (prec >= 0 && int(field.size()) < prec) field.insert(0, size_t(prec - int(field.size())), '0');
        break;
      }
      case 'x':
      case 'X': {
        uint32_t v = next();
        if (shrt) v &= 0xFFFF;
        char b[16];
        snprintf(b, sizeof(b), t == 'x' ? "%x" : "%X", v);
        field = b;
        if (prec >= 0 && int(field.size()) < prec) field.insert(0, size_t(prec - int(field.size())), '0');
        if (alt && v) field.insert(0, t == 'x' ? "0x" : "0X");
        break;
      }
      case 's':
      case 'S': {
        uint32_t p = next();
        bool w = (t == 'S' && !shrt) || (t == 's' && wide);
        if (!p) {
          field = "(null)";
        } else if (w) {
          for (char16_t c16 : read_wstr(mem, p)) field.push_back(c16 < 0x100 ? char(c16) : '?');
        } else {
          field = read_cstr(mem, p);
        }
        if (prec >= 0 && int(field.size()) > prec) field.resize(size_t(prec));
        break;
      }
      default:
        // Unknown conversion: wsprintf copies the character.
        field.push_back(t);
        break;
    }
    if (int(field.size()) < width) {
      size_t pad = size_t(width - int(field.size()));
      if (left) {
        field.append(pad, ' ');
      } else if (zero && t != 's' && t != 'S' && t != 'c' && t != 'C') {
        size_t at = (!field.empty() && field[0] == '-') ? 1 : 0;
        field.insert(at, pad, '0');
      } else {
        field.insert(0, pad, ' ');
      }
    }
    out += field;
  }
  if (out.size() > 1023) out.resize(1023);
  return out;
}

// Virtual-key code and shift state for a character on a US keyboard
// (VkKeyScan is layout-dependent on a real host; a fixed table keeps runs
// reproducible).
uint32_t vk_scan_us(uint8_t ch) {
  if (ch >= 'a' && ch <= 'z') return uint32_t(ch - 'a' + 'A');
  if (ch >= 'A' && ch <= 'Z') return 0x100 | ch;
  if (ch >= '0' && ch <= '9') return ch;
  static const char* shifted_digits = ")!@#$%^&*(";
  for (int d = 0; d < 10; d++)
    if (ch == uint8_t(shifted_digits[d])) return 0x100 | uint32_t('0' + d);
  struct K {
    char c;
    uint16_t v;
  };
  static const K table[] = {
      {' ', 0x20},  {'\t', 0x09},  {'\r', 0x0D},  {'\b', 0x08},  {27, 0x1B},     {';', 0xBA},   {':', 0x1BA},
      {'=', 0xBB},  {'+', 0x1BB},  {',', 0xBC},   {'<', 0x1BC},  {'-', 0xBD},    {'_', 0x1BD},  {'.', 0xBE},
      {'>', 0x1BE}, {'/', 0xBF},   {'?', 0x1BF},  {'`', 0xC0},   {'~', 0x1C0},   {'[', 0xDB},   {'{', 0x1DB},
      {'\\', 0xDC}, {'|', 0x1DC},  {']', 0xDD},   {'}', 0x1DD},  {'\'', 0xDE},   {'"', 0x1DE},
  };
  for (const K& k : table)
    if (uint8_t(k.c) == ch) return k.v;
  return 0xFFFFFFFF;  // no key produces it (SHORT -1)
}

// Win95 default system colours (COLOR_SCROLLBAR … COLOR_INFOBK).
constexpr COLORREF kSysColors[] = {
    RGB(192, 192, 192), RGB(0, 128, 128),   RGB(0, 0, 128),     RGB(128, 128, 128), RGB(192, 192, 192),
    RGB(255, 255, 255), RGB(0, 0, 0),       RGB(0, 0, 0),       RGB(0, 0, 0),       RGB(255, 255, 255),
    RGB(192, 192, 192), RGB(192, 192, 192), RGB(128, 128, 128), RGB(0, 0, 128),     RGB(255, 255, 255),
    RGB(192, 192, 192), RGB(128, 128, 128), RGB(128, 128, 128), RGB(0, 0, 0),       RGB(192, 192, 192),
    RGB(255, 255, 255), RGB(0, 0, 0),       RGB(223, 223, 223), RGB(0, 0, 0),       RGB(255, 255, 225),
};

// GetKeyState / GetAsyncKeyState from the protocol's InputState. The mouse
// buttons come from the MOUSE bitmask (1 left, 2 right, 4 middle). The async
// form sets bit 0 when the key is down now and was not at the previous async
// call for it ("pressed since the last call", INTERACTION.md §5.1: the core
// shows every down to at least one step, so a poller that asks once a frame
// sees each press); GetKeyState's bit 0 is Caps Lock's toggle.
uint32_t key_state(Runtime& rt, uint32_t vk_arg, bool async) {
  uint32_t vk = vk_arg & 0xFF;
  const InputState& in = rt.input();
  bool down = in.keys.test(vk);
  if (vk == VK_LBUTTON) down = down || in.mouse_button || (in.mouse_buttons & kMouseLeft);
  if (vk == VK_RBUTTON) down = down || (in.mouse_buttons & kMouseRight);
  if (vk == VK_MBUTTON) down = down || (in.mouse_buttons & kMouseMiddle);
  uint32_t v = down ? 0xFFFF8000 : 0;
  if (async) {
    User32State& s = us(rt);
    if (down && !s.async_seen[vk]) v |= 1;
    s.async_seen[vk] = down;
  } else if (vk == VK_CAPITAL && in.caps) {
    v |= 1;  // toggled
  }
  return v;
}

// WM_CLOSE or WM_SYSCOMMAND(SC_CLOSE) to the saver window: the module asks
// the saver to end (INTERACTION.md §5.1, ADWS_WAKE).
void note_wake(Runtime& rt, uint32_t hwnd, uint32_t msg, uint32_t wparam) {
  User32State& s = us(rt);
  if (!hwnd || hwnd != s.saver) return;
  if (msg == WM_CLOSE || (msg == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_CLOSE)) {
    if (!s.wake) trace("user", "the module asked the saver window to close (message 0x%X)", msg);
    s.wake = true;
  }
}

}  // namespace

bool saver_wake_requested(Runtime& rt) { return us(rt).wake; }

bool post_guest_message(Runtime& rt, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam) {
  User32State& s = us(rt);
  if (hwnd == 0xFFFF) return true;  // HWND_BROADCAST: nobody else is listening
  if (hwnd && !s.get(hwnd)) return false;
  g32::MSG m{hwnd, msg, wparam, lparam, rt.clock().tick_count(), 0, 0};
  note_wake(rt, hwnd, m.message, m.wParam);
  s.posted.push_back(m);
  trace("user", "PostMessage(0x%X, 0x%04X, 0x%X, 0x%X)", hwnd, m.message, m.wParam, m.lParam);
  return true;
}

bool guest_class(Runtime& rt, std::string_view name, GuestClassInfo* out) {
  WndClass* k = us(rt).find_class(name);
  if (!k) return false;
  if (out) {
    out->name = k->name;
    out->wndproc = k->wndproc;
    out->style = k->style;
    out->background = k->background;
    out->cls_extra = k->cls_extra;
    out->wnd_extra = k->wnd_extra;
  }
  return true;
}

std::string guest_class_arg(Runtime& rt, uint32_t p) { return class_arg(rt, p); }

uint32_t host_window(Runtime& rt) {
  User32State& s = us(rt);
  if (s.saver) return s.saver;
  Window w;
  w.class_name = "WindowsScreenSaverClass";
  w.title = "Screen Saver";
  w.style = WS_POPUP | WS_VISIBLE;
  w.exstyle = WS_EX_TOPMOST;
  int wd = rt.display() ? rt.display()->width() : 640, ht = rt.display() ? rt.display()->height() : 480;
  w.rect = RECT{0, 0, wd, ht};
  s.saver = s.add(std::move(w));
  return s.saver;
}

std::string guest_wsprintf(Runtime& rt, const std::string& fmt, uint32_t args) {
  return format_wsprintf(rt, fmt, args);
}

void register_user32(ShimRegistry& r) {
  // ---- formatting ----
  r.impl(U, "wsprintfA", [](Call& c) {
    std::string out = format_wsprintf(c.rt, c.str(1), c.esp + 12);
    c.mem().memcpy(c.arg(0), out.c_str(), out.size() + 1);
    c.ret(uint32_t(out.size()));
  });
  r.impl(U, "CharToOemBuffA", [](Call& c) {
    uint32_t n = c.arg(2);
    std::string in = c.mem().read(c.arg(0), n);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(1252, 0, in.data(), int(n), w.data(), int(n));
    std::string out(n, '\0');
    ::WideCharToMultiByte(437, 0, w.data(), int(n), out.data(), int(n), nullptr, nullptr);
    c.mem().memcpy(c.arg(1), out.data(), n);
    c.ret(1);
  });

  // ---- message boxes and dialogs (never shown) ----
  r.impl(U, "MessageBoxA", [](Call& c) {
    log("MessageBox \"%s\": %s", c.str(2).c_str(), c.str(1).c_str());
    us(c.rt).last_box = c.str(2) + ": " + c.str(1);
    uint32_t type = c.arg(3) & 0xF;
    // The affirmative button of each style: OK, OK, Abort→Ignore, Yes, Yes, Retry.
    static const uint32_t kAnswer[] = {IDOK, IDOK, IDIGNORE, IDYES, IDYES, IDRETRY};
    c.ret(type < 6 ? kAnswer[type] : IDOK);
  });
  r.impl(U, "DialogBoxParamA", [](Call& c) {
    trace("user", "DialogBoxParam refused (configuration dialogs are not shown)");
    c.ret(IDCANCEL);
  });
  r.impl(U, "EndDialog", [](Call& c) { c.ret(1); });
  r.impl(U, "GetDlgItem", [](Call& c) { c.ret(0); });
  r.impl(U, "SendDlgItemMessageA", [](Call& c) { c.ret(0); });
  r.impl(U, "SetDlgItemTextA", [](Call& c) { c.ret(1); });
  r.impl(U, "CheckDlgButton", [](Call& c) { c.ret(1); });
  r.impl(U, "IsDlgButtonChecked", [](Call& c) { c.ret(0); });

  // ---- window classes and windows ----
  auto register_class = [](Call& c, const g32::WNDCLASSEXA& wc) {
    User32State& s = us(c.rt);
    std::string name = class_arg(c.rt, wc.lpszClassName);
    if (s.find_class(name)) {
      c.set_last_error(ERROR_CLASS_ALREADY_EXISTS);
      return c.ret(0);
    }
    WndClass k;
    k.name = name;
    k.atom = 0xC000 + uint32_t(s.classes.size());
    k.style = wc.style;
    k.wndproc = wc.lpfnWndProc;
    k.instance = wc.hInstance;
    k.background = wc.hbrBackground;
    k.icon = wc.hIcon;
    k.cursor = wc.hCursor;
    k.icon_sm = wc.hIconSm;
    k.cls_extra = wc.cbClsExtra;
    k.wnd_extra = wc.cbWndExtra;
    k.menu_name = wc.lpszMenuName;
    s.classes.push_back(k);
    trace("user", "RegisterClass \"%s\" -> 0x%X", name.c_str(), k.atom);
    c.ret(k.atom);
  };
  r.impl(U, "RegisterClassA", [register_class](Call& c) {
    g32::WNDCLASSA a = c.pod<g32::WNDCLASSA>(0);
    g32::WNDCLASSEXA wc{sizeof(g32::WNDCLASSEXA), a.style, a.lpfnWndProc, a.cbClsExtra, a.cbWndExtra, a.hInstance,
                        a.hIcon, a.hCursor, a.hbrBackground, a.lpszMenuName, a.lpszClassName, 0};
    register_class(c, wc);
  });
  r.impl(U, "RegisterClassExA", [register_class](Call& c) {
    g32::WNDCLASSEXA wc = c.pod<g32::WNDCLASSEXA>(0);
    if (wc.cbSize != sizeof(g32::WNDCLASSEXA)) {
      c.set_last_error(ERROR_INVALID_PARAMETER);
      return c.ret(0);
    }
    register_class(c, wc);
  });
  r.impl(U, "SetClassLongA", [](Call& c) {
    // (hwnd, index, value) → the previous value. The class fields a module
    // may replace; the extra class bytes are not kept (none of our classes
    // asks for any).
    Window* w = us(c.rt).get(c.arg(0));
    WndClass* k = w ? us(c.rt).find_class(w->class_name) : nullptr;
    if (!k) {
      c.set_last_error(w ? ERROR_INVALID_INDEX : ERROR_INVALID_WINDOW_HANDLE);
      return c.ret(0);
    }
    uint32_t* field = nullptr;
    switch (c.iarg(1)) {
      case -10: field = &k->background; break;  // GCL_HBRBACKGROUND
      case -12: field = &k->cursor; break;      // GCL_HCURSOR
      case -14: field = &k->icon; break;        // GCL_HICON
      case -24: field = &k->wndproc; break;     // GCL_WNDPROC
      case -26: field = &k->style; break;       // GCL_STYLE
      case -34: field = &k->icon_sm; break;     // GCL_HICONSM
      default:
        c.set_last_error(ERROR_INVALID_INDEX);
        return c.ret(0);
    }
    uint32_t old = *field;
    *field = c.arg(2);
    c.ret(old);
  });
  r.impl(U, "UnregisterClassA", [](Call& c) {
    User32State& s = us(c.rt);
    std::string name = class_arg(c.rt, c.arg(0));
    auto it = std::find_if(s.classes.begin(), s.classes.end(), [&](const WndClass& k) { return k.name == name; });
    if (it == s.classes.end()) return c.ret(0);
    s.classes.erase(it);
    c.ret(1);
  });
  r.impl(U, "CreateWindowExA", [](Call& c) {
    User32State& s = us(c.rt);
    std::string cls = class_arg(c.rt, c.arg(1));
    WndClass* k = s.find_class(cls);
    if (!k) {
      c.set_last_error(ERROR_CANNOT_FIND_WND_CLASS);
      return c.ret(0);
    }
    Window w;
    w.class_name = k->name;
    w.title = c.str(2);
    w.exstyle = c.arg(0);
    w.style = c.arg(3);
    int32_t x = c.iarg(4), y = c.iarg(5), cx = c.iarg(6), cy = c.iarg(7);
    if (x == int32_t(CW_USEDEFAULT)) x = y = 0;
    if (cx == int32_t(CW_USEDEFAULT)) cx = cy = 0;
    w.rect = RECT{x, y, x + cx, y + cy};
    w.parent = c.arg(8);
    w.id = c.arg(9);
    w.instance = c.arg(10);
    w.wndproc = k->wndproc;
    w.extra.assign(size_t(std::max(k->wnd_extra, 0)), 0);
    uint32_t hwnd = s.add(std::move(w));
    trace("user", "CreateWindowEx class \"%s\" \"%s\" -> 0x%X", cls.c_str(), c.str(2).c_str(), hwnd);
    // WM_NCCREATE and WM_CREATE with a CREATESTRUCTA on the heap.
    uint32_t cs = c.rt.heap().alloc(48, true);
    uint32_t fields[12] = {c.arg(11), c.arg(10), c.arg(9),  c.arg(8), uint32_t(cy),    uint32_t(cx),
                           uint32_t(y), uint32_t(x), c.arg(3), c.arg(2), c.arg(1), c.arg(0)};
    c.mem().memcpy(cs, fields, sizeof(fields));
    uint32_t ok = send(c.rt, hwnd, WM_NCCREATE, 0, cs);
    int32_t created = ok ? int32_t(send(c.rt, hwnd, WM_CREATE, 0, cs)) : -1;
    c.rt.heap().free(cs);
    if (!ok || created == -1) {
      if (Window* dead = s.get(hwnd)) dead->alive = false;
      return c.ret(0);
    }
    c.ret(hwnd);
  });
  r.impl(U, "DestroyWindow", [](Call& c) {
    uint32_t hwnd = c.arg(0);
    if (!us(c.rt).get(hwnd)) return c.ret(0);
    send(c.rt, hwnd, WM_DESTROY, 0, 0);
    send(c.rt, hwnd, WM_NCDESTROY, 0, 0);
    if (Window* w = us(c.rt).get(hwnd)) w->alive = false;
    c.ret(1);
  });
  r.impl(U, "DefWindowProcA", [](Call& c) { c.ret(c.arg(1) == WM_NCCREATE ? 1 : 0); });
  r.impl(U, "SendMessageA", [](Call& c) {
    note_wake(c.rt, c.arg(0), c.arg(1), c.arg(2));
    c.ret(send(c.rt, c.arg(0), c.arg(1), c.arg(2), c.arg(3)));
  });
  r.impl(U, "GetWindowLongA", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    int32_t i = c.iarg(1);
    switch (i) {
      case kGwlWndProc: return c.ret(w->wndproc);
      case kGwlHInstance: return c.ret(w->instance);
      case kGwlHwndParent: return c.ret(w->parent);
      case kGwlId: return c.ret(w->id);
      case kGwlStyle: return c.ret(w->style);
      case kGwlExStyle: return c.ret(w->exstyle);
      case kGwlUserData: return c.ret(w->user_data);
      default:
        if (i >= 0 && size_t(i) + 4 <= w->extra.size()) {
          uint32_t v;
          memcpy(&v, w->extra.data() + i, 4);
          return c.ret(v);
        }
        c.set_last_error(ERROR_INVALID_INDEX);
        return c.ret(0);
    }
  });
  r.impl(U, "SetWindowLongA", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    int32_t i = c.iarg(1);
    uint32_t v = c.arg(2), old = 0;
    auto swap = [&](uint32_t& field) {
      old = field;
      field = v;
    };
    switch (i) {
      case kGwlWndProc: swap(w->wndproc); break;
      case kGwlHInstance: swap(w->instance); break;
      case kGwlHwndParent: swap(w->parent); break;
      case kGwlId: swap(w->id); break;
      case kGwlStyle: swap(w->style); break;
      case kGwlExStyle: swap(w->exstyle); break;
      case kGwlUserData: swap(w->user_data); break;
      default:
        if (i >= 0 && size_t(i) + 4 <= w->extra.size()) {
          memcpy(&old, w->extra.data() + i, 4);
          memcpy(w->extra.data() + i, &v, 4);
          break;
        }
        c.set_last_error(ERROR_INVALID_INDEX);
        return c.ret(0);
    }
    c.ret(old);
  });
  r.impl(U, "GetParent", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    c.ret(w ? w->parent : 0);
  });
  r.impl(U, "GetActiveWindow", [](Call& c) { c.ret(host_window(c.rt)); });
  r.impl(U, "GetLastActivePopup", [](Call& c) { c.ret(c.arg(0)); });
  r.impl(U, "GetWindowRect", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    write_pod(c.mem(), c.arg(1), w->rect);
    c.ret(1);
  });
  r.impl(U, "MoveWindow", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    w->rect = RECT{c.iarg(1), c.iarg(2), c.iarg(1) + c.iarg(3), c.iarg(2) + c.iarg(4)};
    c.ret(1);
  });
  r.impl(U, "GetWindowTextA", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w || !c.arg(2)) return c.ret(0);
    c.ret(uint32_t(write_cstr(c.mem(), c.arg(1), w->title, c.arg(2))));
  });
  r.impl(U, "SetWindowTextA", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    w->title = c.str(1);
    c.ret(1);
  });
  r.impl(U, "ShowWindow", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    bool was = w->style & WS_VISIBLE;
    if (c.arg(1) == SW_HIDE) w->style &= ~WS_VISIBLE;
    else w->style |= WS_VISIBLE;
    c.ret_bool(was);
  });
  r.impl(U, "IsWindowVisible", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    c.ret_bool(w && (w->style & WS_VISIBLE));
  });
  r.impl(U, "UpdateWindow", [](Call& c) { c.ret(1); });
  r.impl(U, "InvalidateRect", [](Call& c) { c.ret(1); });
  r.impl(U, "EnableWindow", [](Call& c) { c.ret(0); });
  r.impl(U, "SetFocus", [](Call& c) { c.ret(host_window(c.rt)); });
  r.impl(U, "EnumThreadWindows", [](Call& c) {
    // The Borland RTL looks for an owner for its fatal-error MessageBox; it
    // copes with finding none.
    c.ret(1);
  });
  r.impl(U, "SetPropA", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    std::string key = class_arg(c.rt, c.arg(1));
    for (auto& p : w->props) {
      if (p.first == key) {
        p.second = c.arg(2);
        return c.ret(1);
      }
    }
    w->props.push_back({key, c.arg(2)});
    c.ret(1);
  });
  r.impl(U, "GetPropA", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    std::string key = class_arg(c.rt, c.arg(1));
    for (auto& p : w->props)
      if (p.first == key) return c.ret(p.second);
    c.ret(0);
  });
  r.impl(U, "RemovePropA", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    std::string key = class_arg(c.rt, c.arg(1));
    for (auto it = w->props.begin(); it != w->props.end(); ++it) {
      if (it->first == key) {
        uint32_t v = it->second;
        w->props.erase(it);
        return c.ret(v);
      }
    }
    c.ret(0);
  });

  r.impl(U, "GetClientRect", [](Call& c) {
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) {
      c.set_last_error(ERROR_INVALID_WINDOW_HANDLE);
      return c.ret(0);
    }
    RECT rc{0, 0, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top};
    write_pod(c.mem(), c.arg(1), rc);
    c.ret(1);
  });
  r.impl(U, "FindWindowA", [](Call& c) {
    // (class, title): only the windows of this process exist on our desktop.
    std::string cls = c.null(0) ? std::string() : class_arg(c.rt, c.arg(0));
    std::string title = c.str(1);
    auto ieq = [](const std::string& a, const std::string& b) {
      return a.size() == b.size() &&
             std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return tolower(x) == tolower(y); });
    };
    for (const Window& w : us(c.rt).windows) {
      if (!w.alive || w.parent) continue;
      if (!c.null(0) && !ieq(w.class_name, cls)) continue;
      if (!c.null(1) && w.title != title) continue;
      return c.ret(w.hwnd);
    }
    c.ret(0);
  });
  r.impl(U, "SetWindowPos", [](Call& c) {
    // (hwnd, insertAfter, x, y, cx, cy, flags)
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    uint32_t flags = c.arg(6);
    int32_t cx = w->rect.right - w->rect.left, cy = w->rect.bottom - w->rect.top;
    if (!(flags & SWP_NOMOVE)) w->rect = RECT{c.iarg(2), c.iarg(3), c.iarg(2) + cx, c.iarg(3) + cy};
    if (!(flags & SWP_NOSIZE)) {
      w->rect.right = w->rect.left + c.iarg(4);
      w->rect.bottom = w->rect.top + c.iarg(5);
    }
    if (flags & SWP_SHOWWINDOW) w->style |= WS_VISIBLE;
    if (flags & SWP_HIDEWINDOW) w->style &= ~WS_VISIBLE;
    c.ret(1);
  });
  r.impl(U, "BeginPaint", [](Call& c) {
    // Nothing is ever invalid on a screen nobody else draws on: the whole
    // client area, no erase pending, drawn on the emulated screen.
    Window* w = us(c.rt).get(c.arg(0));
    if (!w) return c.ret(0);
    g32::PAINTSTRUCT ps{};
    ps.hdc = c.rt.state<GdiTable>().screen_dc();
    ps.rcPaint = RECT{0, 0, w->rect.right - w->rect.left, w->rect.bottom - w->rect.top};
    write_pod(c.mem(), c.arg(1), ps);
    c.ret(ps.hdc);
  });
  r.impl(U, "EndPaint", [](Call& c) { c.ret(1); });

  // ---- scroll bars: no window here has any ----
  r.impl(U, "GetScrollInfo", [](Call& c) { c.ret(0); });
  r.impl(U, "SetScrollInfo", [](Call& c) { c.ret(0); });
  r.impl(U, "EnableScrollBar", [](Call& c) { c.ret(0); });

  // ---- cursors (never drawn: the saver hides the pointer) ----
  r.impl(U, "LoadCursorA", [](Call& c) {
    // (hInstance, name). NULL hInstance = a system cursor (IDC_ARROW 32512 …
    // IDC_HELP 32651): a fixed handle per id. Otherwise the module's own
    // RT_GROUP_CURSOR resource, if it has it.
    uint32_t hinst = c.arg(0), name_p = c.arg(1);
    User32State& s = us(c.rt);
    if (!hinst) {
      if (name_p >= 32512 && name_p <= 32651) return c.ret(kSysCursorBase + name_p);
      c.set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
      return c.ret(0);
    }
    loader::ResId name = name_p < 0x10000 ? loader::ResId::of(uint16_t(name_p)) : loader::ResId::of(c.str(1));
    if (name.is_string && !name.str.empty() && name.str[0] == '#')
      name = loader::ResId::of(uint16_t(strtoul(name.str.c_str() + 1, nullptr, 10)));
    if (!find_module_resource(c.rt, hinst, loader::ResId::of(uint16_t(12)), name)) {  // RT_GROUP_CURSOR
      c.set_last_error(ERROR_RESOURCE_NAME_NOT_FOUND);
      return c.ret(0);
    }
    std::pair<uint32_t, std::string> key{hinst, name.to_string()};
    auto it = std::find(s.cursors.begin(), s.cursors.end(), key);
    if (it == s.cursors.end()) it = s.cursors.insert(s.cursors.end(), key);
    c.ret(kCursorBase + 4 * uint32_t(it - s.cursors.begin()));
  });
  r.impl(U, "SetCursor", [](Call& c) {
    User32State& s = us(c.rt);
    uint32_t old = s.cursor;
    s.cursor = c.arg(0);
    c.ret(old);
  });
  r.impl(U, "ShowCursor", [](Call& c) {
    User32State& s = us(c.rt);
    s.cursor_count += c.arg(0) ? 1 : -1;
    c.ret(uint32_t(s.cursor_count));
  });

  // ---- the message queue and timers ----
  //
  // After Dark drives a module through Module() messages, not window messages
  // (ABI.md §2.6), but a module may still post to its own windows, set a
  // timer, or peek at its queue. The queue is real: PostMessageA and
  // PostQuitMessage put messages there, due timers produce WM_TIMER when it is
  // otherwise empty (as Windows does), and PeekMessageA/DispatchMessageA work
  // on it. Timers are due by the virtual clock, peeked, never nudged.
  r.impl(U, "PostMessageA", [](Call& c) {
    if (!post_guest_message(c.rt, c.arg(0), c.arg(1), c.arg(2), c.arg(3))) {
      c.set_last_error(ERROR_INVALID_WINDOW_HANDLE);
      return c.ret(0);
    }
    c.ret(1);
  });
  r.impl(U, "PostQuitMessage", [](Call& c) {
    User32State& s = us(c.rt);
    s.quit = true;
    s.quit_code = c.arg(0);
    trace("user", "PostQuitMessage(%d)", c.iarg(0));
  });
  r.impl(U, "PeekMessageA", [](Call& c) {
    // (msg, hwnd, min, max, remove)
    g32::MSG m{};
    bool remove = c.arg(4) & PM_REMOVE;
    if (next_message(c.rt, c.arg(1), c.arg(2), c.arg(3), remove, m)) {
      write_pod(c.mem(), c.arg(0), m);
      return c.ret(1);
    }
    c.ret(0);
  });
  r.impl(U, "TranslateMessage", [](Call& c) { c.ret(0); });  // no keyboard messages are ever queued
  r.impl(U, "DispatchMessageA", [](Call& c) {
    g32::MSG m = c.pod<g32::MSG>(0);
    c.ret(dispatch(c.rt, m));
  });
  r.impl(U, "SetTimer", [](Call& c) {
    // (hwnd, id, elapse, proc). A window timer is keyed by (hwnd, id); a
    // thread timer (hwnd NULL) gets a fresh id.
    User32State& s = us(c.rt);
    uint32_t hwnd = c.arg(0), id = c.arg(1), elapse = std::max<uint32_t>(c.arg(2), 10);
    if (hwnd && !s.get(hwnd)) {
      c.set_last_error(ERROR_INVALID_WINDOW_HANDLE);
      return c.ret(0);
    }
    if (!hwnd) id = 0x7FF0 + s.next_timer_id++;
    Timer t{hwnd, id, elapse, c.arg(3), c.rt.clock().now_us() + uint64_t(elapse) * 1000};
    auto it = std::find_if(s.timers.begin(), s.timers.end(),
                           [&](const Timer& x) { return x.hwnd == hwnd && x.id == id; });
    if (it != s.timers.end()) *it = t;
    else s.timers.push_back(t);
    trace("user", "SetTimer(0x%X, %u, %u ms, proc 0x%X)", hwnd, id, elapse, t.proc);
    c.ret(hwnd ? (id ? id : 1) : id);
  });
  r.impl(U, "KillTimer", [](Call& c) {
    User32State& s = us(c.rt);
    uint32_t hwnd = c.arg(0), id = c.arg(1);
    auto it = std::find_if(s.timers.begin(), s.timers.end(),
                           [&](const Timer& x) { return x.hwnd == hwnd && x.id == id; });
    if (it == s.timers.end()) return c.ret(0);
    s.timers.erase(it);
    c.ret(1);
  });

  r.impl(U, "SystemParametersInfoA", [](Call& c) {
    // (action, uiParam, pvParam, winIni): the few a screen saver asks; the rest
    // report failure, which callers treat as "use your default".
    uint32_t action = c.arg(0), pv = c.arg(2);
    int w = c.rt.display() ? c.rt.display()->width() : 640, h = c.rt.display() ? c.rt.display()->height() : 480;
    trace("user", "SystemParametersInfo(%u, %u, 0x%X)", action, c.arg(1), pv);
    switch (action) {
      case SPI_GETSCREENSAVEACTIVE:
        if (pv) c.mem().write_u32l(pv, 1);
        return c.ret(1);
      case SPI_GETSCREENSAVETIMEOUT:
        if (pv) c.mem().write_u32l(pv, 60);
        return c.ret(1);
      case 97:  // SPI_SETSCREENSAVERRUNNING (SPI_SCREENSAVERRUNNING): the previous state
        if (pv) c.mem().write_u32l(pv, 0);
        return c.ret(1);
      case SPI_GETWORKAREA:
        if (pv) write_pod(c.mem(), pv, RECT{0, 0, w, h});
        return c.ret(1);
      default:
        return c.ret(0);
    }
  });

  // ---- DCs: every window draws on the emulated screen ----
  r.impl(U, "GetDC", [](Call& c) { c.ret(c.rt.state<GdiTable>().screen_dc()); });
  r.impl(U, "ReleaseDC", [](Call& c) { c.ret(1); });

  // ---- input (the protocol's InputState) ----
  r.impl(U, "GetAsyncKeyState", [](Call& c) { c.ret(key_state(c.rt, c.arg(0), true)); });
  r.impl(U, "GetKeyState", [](Call& c) { c.ret(key_state(c.rt, c.arg(0), false)); });
  r.impl(U, "GetCursorPos", [](Call& c) {
    const InputState& in = c.rt.input();
    POINT p{in.mouse_x, in.mouse_y};
    write_pod(c.mem(), c.arg(0), p);
    c.ret(1);
  });
  r.impl(U, "VkKeyScanA", [](Call& c) { c.ret(vk_scan_us(uint8_t(c.arg(0)))); });

  // ---- rectangles ----
  r.impl(U, "SetRect", [](Call& c) {
    RECT rc{c.iarg(1), c.iarg(2), c.iarg(3), c.iarg(4)};
    write_pod(c.mem(), c.arg(0), rc);
    c.ret(1);
  });
  r.impl(U, "CopyRect", [](Call& c) {
    if (!c.arg(0) || !c.arg(1)) return c.ret(0);
    write_pod(c.mem(), c.arg(0), c.pod<RECT>(1));
    c.ret(1);
  });

  // ---- system metrics and colours ----
  r.impl(U, "GetSystemMetrics", [](Call& c) {
    int w = c.rt.display() ? c.rt.display()->width() : 640, h = c.rt.display() ? c.rt.display()->height() : 480;
    switch (c.iarg(0)) {
      case SM_CXSCREEN: case SM_CXFULLSCREEN: case SM_CXMAXIMIZED: return c.ret(uint32_t(w));
      case SM_CYSCREEN: case SM_CYFULLSCREEN: case SM_CYMAXIMIZED: return c.ret(uint32_t(h));
      case SM_CXICON: case SM_CYICON: case SM_CXCURSOR: case SM_CYCURSOR: return c.ret(32);
      case SM_CXSMICON: case SM_CYSMICON: return c.ret(16);
      case SM_CYCAPTION: return c.ret(18);
      case SM_CXBORDER: case SM_CYBORDER: return c.ret(1);
      case SM_CXFRAME: case SM_CYFRAME: return c.ret(4);
      case SM_MOUSEPRESENT: return c.ret(1);
      case SM_CMOUSEBUTTONS: return c.ret(2);
      default: return c.ret(0);
    }
  });
  r.impl(U, "GetSysColor", [](Call& c) {
    uint32_t i = c.arg(0);
    c.ret(i < std::size(kSysColors) ? kSysColors[i] : 0);
  });

  // ---- drawing helpers (on real GDI, through gdi_objects.hh) ----
  r.impl(U, "FillRect", [](Call& c) {
    GdiTable& g = c.rt.state<GdiTable>();
    HDC dc = g.host_dc(c.arg(0));
    if (!dc) return c.ret(0);
    RECT rc = c.pod<RECT>(1);
    c.rt.charge_pixels(int64_t(std::max<LONG>(rc.right - rc.left, 0)) * std::max<LONG>(rc.bottom - rc.top, 0));
    uint32_t hb = c.arg(2);
    HBRUSH brush;
    HBRUSH temp = nullptr;
    if (hb && hb <= COLOR_INFOBK + 1) {  // (HBRUSH)(COLOR_xxx + 1)
      temp = CreateSolidBrush(g.key_color(c.arg(0), kSysColors[hb - 1]));
      brush = temp;
    } else {
      brush = static_cast<HBRUSH>(g.realize_for(hb, c.arg(0)));
    }
    int ok = brush ? ::FillRect(dc, &rc, brush) : 0;
    if (temp) DeleteObject(temp);
    c.ret(uint32_t(ok));
  });
  r.impl(U, "DrawTextA", [](Call& c) {
    GdiTable& g = c.rt.state<GdiTable>();
    HDC dc = g.host_dc(c.arg(0));
    if (!dc) return c.ret(0);
    int32_t n = c.iarg(2);
    std::string text = n < 0 ? c.str(1) : c.mem().read(c.arg(1), uint32_t(n));
    RECT rc = c.pod<RECT>(3);
    uint32_t fmt = c.arg(4) & ~uint32_t(DT_MODIFYSTRING);
    int h = ::DrawTextA(dc, text.data(), int(text.size()), &rc, fmt);
    if (fmt & DT_CALCRECT) write_pod(c.mem(), c.arg(3), rc);
    c.ret(uint32_t(h));
  });

  // ---- resources ----
  r.impl(U, "LoadBitmapA", [](Call& c) {
    // A device bitmap made from an RT_BITMAP resource (a packed DIB). On a
    // Win95 palette display it is created through the screen DC with the
    // default palette selected, so every colour lands on the nearest of the 20
    // static colours.
    uint32_t hinst = c.arg(0), name_p = c.arg(1);
    if (!hinst) return c.ret(0);  // OBM_* system bitmaps: none here
    loader::ResId name = name_p < 0x10000 ? loader::ResId::of(uint16_t(name_p)) : loader::ResId::of(c.str(1));
    if (name.is_string && !name.str.empty() && name.str[0] == '#')
      name = loader::ResId::of(uint16_t(strtoul(name.str.c_str() + 1, nullptr, 10)));
    const loader::pe::Resource* res = find_module_resource(c.rt, hinst, loader::ResId::of(loader::rt::bitmap), name);
    Module* m = c.rt.modules().by_handle(hinst);
    if (!res || !m) return c.ret(0);
    std::string_view data = m->image->resource_data(*res);
    if (data.size() < sizeof(BITMAPINFOHEADER)) return c.ret(0);
    BITMAPINFOHEADER bh;
    memcpy(&bh, data.data(), sizeof(bh));
    int bpp = bh.biBitCount, w = bh.biWidth, h = std::abs(bh.biHeight);
    if (bh.biSize < sizeof(BITMAPINFOHEADER) || w <= 0 || h <= 0 || (bh.biCompression != BI_RGB &&
                                                                      bh.biCompression != BI_BITFIELDS)) {
      log("LoadBitmapA: unsupported bitmap resource (%d bpp, compression %u)", bpp, unsigned(bh.biCompression));
      return c.ret(0);
    }
    uint32_t ncolors = bpp <= 8 ? (bh.biClrUsed ? bh.biClrUsed : 1u << bpp) : 0;
    size_t table = bh.biSize + (bh.biCompression == BI_BITFIELDS && bh.biSize == 40 ? 12 : 0);
    size_t bits = table + 4 * size_t(ncolors);
    uint32_t stride = uint32_t(((w * bpp + 31) / 32) * 4);
    if (bits + size_t(stride) * h > data.size()) return c.ret(0);
    GdiTable& g = c.rt.state<GdiTable>();
    Display& d = g.display();
    uint32_t hb = g.create_device_bitmap(w, h, 8);
    GdiObject* o = g.get(hb, GdiType::bitmap);
    if (!o) return c.ret(0);
    auto match = [&](uint8_t r, uint8_t gg, uint8_t b) { return uint8_t(d.nearest_index(RGB(r, gg, b), true)); };
    std::vector<uint8_t> lut(ncolors);
    for (uint32_t i = 0; i < ncolors; i++) {
      const uint8_t* q = reinterpret_cast<const uint8_t*>(data.data() + table + 4 * i);
      lut[i] = match(q[2], q[1], q[0]);
    }
    std::map<uint32_t, uint8_t> cache;
    for (int y = 0; y < h; y++) {
      // Resource DIBs are bottom-up unless biHeight is negative.
      const uint8_t* row = reinterpret_cast<const uint8_t*>(data.data() + bits) +
                           size_t(bh.biHeight < 0 ? y : h - 1 - y) * stride;
      uint8_t* out = o->bmp.row(y);
      for (int x = 0; x < w; x++) {
        uint32_t idx = 0, rgb = 0;
        switch (bpp) {
          case 1: idx = (row[x >> 3] >> (7 - (x & 7))) & 1; break;
          case 4: idx = (row[x >> 1] >> ((x & 1) ? 0 : 4)) & 0xF; break;
          case 8: idx = row[x]; break;
          case 24: rgb = uint32_t(row[3 * x + 2]) << 16 | uint32_t(row[3 * x + 1]) << 8 | row[3 * x]; break;
          case 32: rgb = uint32_t(row[4 * x + 2]) << 16 | uint32_t(row[4 * x + 1]) << 8 | row[4 * x]; break;
          case 16: {
            uint32_t v = uint32_t(row[2 * x]) | uint32_t(row[2 * x + 1]) << 8;
            rgb = ((v >> 10) & 31) * 255 / 31 << 16 | ((v >> 5) & 31) * 255 / 31 << 8 | (v & 31) * 255 / 31;
            break;
          }
          default: break;
        }
        if (bpp <= 8) {
          out[x] = idx < lut.size() ? lut[idx] : 0;
        } else {
          auto it = cache.find(rgb);
          if (it == cache.end()) it = cache.emplace(rgb, match(uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb))).first;
          out[x] = it->second;
        }
      }
    }
    trace("user", "LoadBitmapA %s: %dx%d %d bpp -> 0x%X", name.to_string().c_str(), w, h, bpp, hb);
    c.ret(hb);
  });
  r.impl(U, "LoadStringA", [](Call& c) {
    uint32_t hinst = c.arg(0), id = c.arg(1), buf = c.arg(2), cap = c.arg(3);
    Module* m = c.rt.modules().by_handle(hinst);
    if (!m || !m->image || !buf) return c.ret(0);
    auto table = m->image->string_table(0x409);
    if (table.empty()) table = m->image->string_table();
    auto it = table.find(id);
    if (it == table.end()) {
      if (cap) c.mem().write_u8(buf, 0);
      return c.ret(0);
    }
    // UTF-8 (loader) → the ANSI code page.
    std::wstring w(size_t(::MultiByteToWideChar(CP_UTF8, 0, it->second.data(), int(it->second.size()), nullptr, 0)),
                   L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, it->second.data(), int(it->second.size()), w.data(), int(w.size()));
    std::string a(size_t(::WideCharToMultiByte(1252, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
    ::WideCharToMultiByte(1252, 0, w.data(), int(w.size()), a.data(), int(a.size()), nullptr, nullptr);
    c.ret(uint32_t(write_cstr(c.mem(), buf, a, cap)));
  });
}

}  // namespace adw::win32
