// USER32's state, shared by its two files: user32.cc (the window model every
// module sees: the saver window, a real message queue, timers, input) and
// user32_program.cc (what a whole program adds: its windows' messages as
// Windows 95 sent them, parents and z-order, focus and activation, painting,
// menus, accelerators, hooks; ABI.md §2.15). Internal to adw_win32: lanes and
// other families use shim_families.hh.
#pragma once

#include <windows.h>

#include <algorithm>
#include <bitset>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "win32/runtime.hh"

namespace adw::win32::user32 {

constexpr const char* U = "USER32.DLL";

struct WndClass {
  std::string name;
  uint32_t atom = 0, style = 0, wndproc = 0, instance = 0, background = 0, icon = 0, cursor = 0, icon_sm = 0;
  int32_t cls_extra = 0, wnd_extra = 0;
  uint32_t menu_name = 0;  // lpszMenuName as given (GetClassInfoA hands it back)
};

struct Window {
  uint32_t hwnd = 0;
  std::string class_name, title;
  uint32_t wndproc = 0;  // guest address; 0 = host default procedure
  uint32_t parent = 0, style = 0, exstyle = 0, instance = 0, id = 0, user_data = 0;
  RECT rect{};           // screen coordinates
  std::vector<uint8_t> extra;
  std::vector<std::pair<std::string, uint32_t>> props;
  bool alive = true;
  // A whole program's windows (user32_program.cc).
  RECT client{};         // the client area, screen coordinates
  uint32_t menu = 0;     // a top-level window's menu bar
  uint64_t z = 0;        // z-order among siblings: higher is nearer the top
  bool invalid = false;  // part of the client area needs painting
  bool erase = false;    // ... and its background erasing first
  RECT update{};         // what needs painting, client coordinates
  bool shown_once = false;
};

constexpr uint32_t kFirstHwnd = 0x00010010, kHwndStep = 4;

// GetWindowLong indices as a 32-bit guest uses them (the x64 headers omit the
// non-PTR spellings of some).
constexpr int32_t kGwlWndProc = -4, kGwlHInstance = -6, kGwlHwndParent = -8, kGwlId = -12, kGwlStyle = -16,
                  kGwlExStyle = -20, kGwlUserData = -21;

// Cursor handles: system cursors (LoadCursorA(NULL, IDC_*)) are kSysCursorBase +
// their id; a module's own cursor resources get kCursorBase + 4n in load order.
constexpr uint32_t kSysCursorBase = 0x00030000, kCursorBase = 0x00040000;

struct Timer {
  uint32_t hwnd = 0, id = 0, elapse_ms = 0, proc = 0;
  uint64_t due_us = 0;  // virtual time the next WM_TIMER is due
};

// A whole program's menus, accelerator tables and hooks (user32_program.cc).
struct MenuItem {
  uint32_t flags = 0;  // MF_* (MF_POPUP, MF_SEPARATOR, MF_GRAYED, MF_CHECKED, …)
  uint32_t id = 0;     // the command, or the submenu's handle for MF_POPUP
  std::string text;
};
struct Menu {
  std::vector<MenuItem> items;
  bool alive = true;
};
struct Accel {
  uint8_t virt = 0;  // FVIRTKEY, FSHIFT, FCONTROL, FALT
  uint16_t key = 0, cmd = 0;
};
struct Hook {
  uint32_t handle = 0;
  int32_t type = 0;
  uint32_t proc = 0;
  bool alive = true;
};

struct User32State : RuntimeState {
  std::vector<WndClass> classes;
  std::vector<Window> windows;
  uint32_t saver = 0;
  // Cursors: nothing is ever shown, but the handles and counts are coherent.
  std::vector<std::pair<uint32_t, std::string>> cursors;  // (hInstance, resource name) → kCursorBase + 4n
  uint32_t cursor = 0;
  int32_t cursor_count = 0;  // ShowCursor's display count
  // The thread's message queue: what PostMessageA/PostQuitMessage/SetTimer
  // put there for PeekMessageA to find.
  std::deque<g32::MSG> posted;
  std::vector<Timer> timers;
  uint32_t next_timer_id = 1;
  bool quit = false;
  uint32_t quit_code = 0;
  // Input: the VKs down at the previous GetAsyncKeyState call (bit 0), and
  // whether the module asked the saver window to close.
  std::bitset<256> async_seen;
  bool wake = false;
  std::string last_box;  // the last MessageBoxA: "caption: text"

  // ---- a whole program (user32_program.cc) ----
  bool program = false;
  uint32_t desktop = 0;  // GetDesktopWindow's window (made on first use)
  uint32_t focus = 0, active = 0, capture = 0;
  bool app_active = false;  // WM_ACTIVATEAPP sent
  uint64_t next_z = 0;
  POINT cursor_pos{0, 0};   // the emulated pointer (SetCursorPos; never the host's)
  uint32_t msg_time = 0;    // GetMessageTime / GetMessagePos: the last message retrieved
  uint32_t msg_pos = 0;
  std::vector<Menu> menus;
  std::vector<std::vector<Accel>> accels;
  std::vector<Hook> hooks;
  std::vector<std::string> window_messages;  // RegisterWindowMessageA: 0xC000 + index
  std::vector<std::pair<uint32_t, std::string>> icons;  // (hInstance, resource name) → kIconBase + 4n
  uint64_t waits = 0;       // GetMessageA waits (frames ended there)
  // The input queue (user32_program_key/_mouse, while the lane lets the
  // program play): key and mouse messages, read after posted ones; the keys
  // and buttons as the messages so far had them; the last button press, for
  // double clicks.
  std::deque<g32::MSG> input;
  std::bitset<256> keys_down;
  uint32_t mouse_buttons = 0;
  bool mouse_seen = false;
  struct Click {
    uint32_t hwnd = 0, button = 0, time = 0;
    POINT pt{0, 0};
  } last_click;

  Window* get(uint32_t hwnd) {
    if (hwnd < kFirstHwnd || (hwnd - kFirstHwnd) % kHwndStep) return nullptr;
    size_t i = (hwnd - kFirstHwnd) / kHwndStep;
    return i < windows.size() && windows[i].alive ? &windows[i] : nullptr;
  }
  WndClass* find_class(std::string_view name) {
    for (WndClass& c : classes) {
      if (c.name.size() == name.size() &&
          std::equal(name.begin(), name.end(), c.name.begin(), [](char a, char b) { return tolower(a) == tolower(b); }))
        return &c;
    }
    return nullptr;
  }
  WndClass* find_atom(uint32_t atom) {
    for (WndClass& c : classes)
      if (c.atom == atom) return &c;
    return nullptr;
  }
  uint32_t add(Window w) {
    w.hwnd = kFirstHwnd + uint32_t(windows.size()) * kHwndStep;
    windows.push_back(std::move(w));
    return windows.back().hwnd;
  }
};

inline User32State& us(Runtime& rt) { return rt.state<User32State>(); }

// user32.cc: a class name argument (a string pointer, or an atom in the low
// word); a message sent to a window's procedure now; the next message
// PeekMessage would retrieve; DispatchMessage.
std::string class_arg(Runtime& rt, uint32_t p);
uint32_t send(Runtime& rt, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);
bool next_message(Runtime& rt, uint32_t hwnd_filter, uint32_t min, uint32_t max, bool remove, g32::MSG& out);
uint32_t dispatch(Runtime& rt, const g32::MSG& m);

// user32_program.cc: the WM_PAINT a program's window is due (next_message's
// turn for it, after posted messages and before timers). False when none.
bool program_paint_message(Runtime& rt, uint32_t hwnd_filter, g32::MSG& out);
// user32_program.cc: a menu from an RT_MENU template (LoadMenuA) and an
// accelerator table from an RT_ACCELERATOR resource (LoadAcceleratorsA);
// their handles, 0 for data too short to be one.
uint32_t menu_from_template(Runtime& rt, std::string_view data);
uint32_t accelerators_from_table(Runtime& rt, std::string_view data);

}  // namespace adw::win32::user32
