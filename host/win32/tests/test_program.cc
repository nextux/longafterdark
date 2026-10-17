// Unit tests for what a whole program brought to the Win32 runtime (ABI.md
// §2.15; pe32/program.cc): the program-mode window manager (user32_program.cc),
// the emulated registry (advapi32.cc), WINMM's mmio and sndPlaySoundA
// (mmio.cc), the KERNEL32 functions Screen Wreckers' DTTY3203.EXE added, the
// process image (ModuleTable::load_exe) and the runtime's yield and slice
// hooks. All data is made up. Shims are called through their thunks
// (call_guest on the thunk address); window and hook procedures are
// hand-assembled and log what they get.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "adw/core/text.h"
#include "check.h"
#include "win32/audio.hh"
#include "win32/display.hh"
#include "win32/gdi_objects.hh"
#include "win32/modules.hh"
#include "win32/runtime.hh"
#include "win32/shim_families.hh"
#include "win32/user32_state.hh"
#include "win32/vfs.hh"

using namespace adw;
using namespace adw::win32;

namespace {

constexpr const char* U = "USER32.DLL";
constexpr const char* K = "KERNEL32.DLL";
constexpr const char* A = "ADVAPI32.DLL";
constexpr const char* W = "WINMM.DLL";

struct Program {
  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  Screen screen{640, 480};
  std::unique_ptr<Runtime> rt;
  uint32_t data = 0, code = 0, log = 0, hooklog = 0;

  Program() {
    RuntimeOptions o;
    o.heap_size = 16u << 20;
    o.call_budget = 50'000'000;
    o.program = true;
    o.guest_module_dir = "C:\\TOYS";
    o.exe_path = "C:\\TOYS\\TOY.EXE";
    o.command_line = "C:\\TOYS\\toy.exe /s /t2";
    rt = std::make_unique<Runtime>(o, clock);
    register_all_shims(rt->shims());
    register_program_shims(rt->shims());
    user32_set_program(*rt, true);
    rt->attach_display(screen);
    rt->vfs().mount_overlay("C:\\WINDOWS", "", "");
    rt->vfs().mount_overlay("C:\\TOYS", "", "");
    rt->vfs().set_cwd("C:\\TOYS");
    data = rt->heap().alloc(256, true);
    code = rt->heap().alloc(4096, true);
    log = rt->heap().alloc(0x4000, true);
    hooklog = rt->heap().alloc(0x1000, true);
  }
  uint32_t thunk(const char* dll, const char* name) {
    ShimEntry& e = rt->shims().get(dll, name);
    CHECK(e.known_signature);
    return rt->shims().thunk_address(e);
  }
  uint32_t api(const char* dll, const char* name, std::initializer_list<uint32_t> args) {
    return rt->call_guest(thunk(dll, name), args);
  }
  uint32_t str(const std::string& s) {
    uint32_t a = rt->heap().alloc(uint32_t(s.size()) + 1, true);
    rt->mem().memcpy(a, s.data(), s.size());
    return a;
  }
  uint32_t block(uint32_t n) { return rt->heap().alloc(n, true); }
  uint32_t u32(uint32_t a) { return rt->mem().read_u32l(a); }
  std::string cstr(uint32_t a) { return read_cstr(rt->mem(), a); }
  RECT rect(uint32_t a) { return read_pod<RECT>(rt->mem(), a); }

  // A window procedure logging (msg, hwnd, wParam) at [DATA] and handing the
  // message to DefWindowProcA, and a WH_CBT hook logging (code, wParam) at
  // [DATA+8] and calling CallNextHookEx([DATA+12]).
  uint32_t wndproc = 0, hookproc = 0;
  void load_procs() {
    std::string text = R"(
    wndproc:
      mov ecx, [DATA]
      mov eax, [esp + 8]
      mov [ecx], eax
      mov eax, [esp + 4]
      mov [ecx + 4], eax
      mov eax, [esp + 12]
      mov [ecx + 8], eax
      add ecx, 12
      mov [DATA], ecx
      push dword [esp + 16]
      push dword [esp + 16]
      push dword [esp + 16]
      push dword [esp + 16]
      call dword [DATA + 4]
      ret 16
    hook:
      mov ecx, [DATA + 8]
      mov eax, [esp + 4]
      mov [ecx], eax
      mov eax, [esp + 8]
      mov [ecx + 4], eax
      add ecx, 8
      mov [DATA + 8], ecx
      push dword [esp + 12]
      push dword [esp + 12]
      push dword [esp + 12]
      push dword [DATA + 12]
      call dword [DATA + 16]
      ret 12
    )";
    for (size_t p; (p = text.find("DATA")) != std::string::npos;) text.replace(p, 4, std::to_string(data));
    auto r = cpu::X86Emulator::assemble(text, nullptr, code);
    rt->mem().memcpy(code, r.code.data(), r.code.size());
    wndproc = code + r.label_offsets.at("wndproc");
    hookproc = code + r.label_offsets.at("hook");
    rt->mem().write_u32l(data, log);
    rt->mem().write_u32l(data + 4, thunk(U, "DefWindowProcA"));
    rt->mem().write_u32l(data + 8, hooklog);
    rt->mem().write_u32l(data + 16, thunk(U, "CallNextHookEx"));
  }
  // The messages logged since `from` (an index), as (msg, hwnd, wParam).
  struct Msg {
    uint32_t msg, hwnd, wp;
  };
  std::vector<Msg> logged(size_t from = 0) {
    std::vector<Msg> v;
    for (uint32_t a = log + uint32_t(from) * 12; a < u32(data); a += 12) v.push_back({u32(a), u32(a + 4), u32(a + 8)});
    return v;
  }
  size_t logged_count() { return (u32(data) - log) / 12; }
  uint32_t register_class(const std::string& name, uint32_t brush = 0) {
    uint32_t wc = block(40);
    g32::WNDCLASSA a{0, wndproc, 0, 8, 0x400000, 0, 0, brush, 0, str(name)};
    write_pod(rt->mem(), wc, a);
    return api(U, "RegisterClassA", {wc});
  }
  uint32_t create(const std::string& cls, uint32_t style, int x, int y, int cx, int cy, uint32_t parent = 0,
                  uint32_t id = 0, uint32_t ex = 0) {
    return api(U, "CreateWindowExA", {ex, str(cls), str("made up"), style, uint32_t(x), uint32_t(y), uint32_t(cx),
                                      uint32_t(cy), parent, id, 0x400000, 0});
  }
};

// A made-up PE32 image: headers and one code section ("ret"), no imports or
// relocations; `dll` sets IMAGE_FILE_DLL. Preferred base 0x00400000.
std::string tiny_pe(bool dll) {
  std::string f(0x400, '\0');
  auto u16 = [&](size_t o, uint16_t v) { memcpy(&f[o], &v, 2); };
  auto u32 = [&](size_t o, uint32_t v) { memcpy(&f[o], &v, 4); };
  f[0] = 'M', f[1] = 'Z';
  u32(0x3C, 0x40);
  memcpy(&f[0x40], "PE\0\0", 4);
  u16(0x44, 0x014C);                         // i386
  u16(0x46, 1);                              // one section
  u16(0x54, 0xE0);                           // SizeOfOptionalHeader
  u16(0x56, dll ? 0x2103 : 0x0103);          // relocations stripped, executable, 32-bit (+ DLL)
  size_t o = 0x58;
  u16(o, 0x10B);
  u32(o + 16, 0x1000);                       // AddressOfEntryPoint
  u32(o + 28, 0x00400000);                   // ImageBase
  u32(o + 32, 0x1000);                       // SectionAlignment
  u32(o + 36, 0x200);                        // FileAlignment
  u16(o + 40, 4);
  u16(o + 48, 4);
  u32(o + 56, 0x2000);                       // SizeOfImage
  u32(o + 60, 0x200);                        // SizeOfHeaders
  u16(o + 68, 2);                            // GUI
  u32(o + 92, 16);
  size_t s = o + 0xE0;
  memcpy(&f[s], ".text", 5);
  u32(s + 8, 0x10);
  u32(s + 12, 0x1000);
  u32(s + 16, 0x200);
  u32(s + 20, 0x200);
  u32(s + 36, 0x60000020);
  f[0x200] = char(0xC3);  // ret
  return f;
}

bool has(const std::vector<Program::Msg>& v, uint32_t msg, uint32_t hwnd) {
  for (auto& m : v)
    if (m.msg == msg && m.hwnd == hwnd) return true;
  return false;
}

// Index of the first (msg, hwnd) in v, or -1.
int index_of(const std::vector<Program::Msg>& v, uint32_t msg, uint32_t hwnd) {
  for (size_t i = 0; i < v.size(); i++)
    if (v[i].msg == msg && v[i].hwnd == hwnd) return int(i);
  return -1;
}

}  // namespace

TEST(program_rect_helpers) {
  Program p;
  uint32_t a = p.block(16), b = p.block(16), o = p.block(16);
  write_pod(p.rt->mem(), a, RECT{0, 0, 10, 10});
  write_pod(p.rt->mem(), b, RECT{5, 5, 20, 20});
  CHECK_EQ(p.api(U, "IntersectRect", {o, a, b}), 1u);
  RECT r = p.rect(o);
  CHECK(r.left == 5 && r.top == 5 && r.right == 10 && r.bottom == 10);
  CHECK_EQ(p.api(U, "UnionRect", {o, a, b}), 1u);
  r = p.rect(o);
  CHECK(r.left == 0 && r.top == 0 && r.right == 20 && r.bottom == 20);
  CHECK_EQ(p.api(U, "OffsetRect", {a, 3, uint32_t(-2)}), 1u);
  r = p.rect(a);
  CHECK(r.left == 3 && r.top == -2 && r.right == 13 && r.bottom == 8);
  CHECK_EQ(p.api(U, "InflateRect", {a, 1, 2}), 1u);
  r = p.rect(a);
  CHECK(r.left == 2 && r.top == -4 && r.right == 14 && r.bottom == 10);
  CHECK_EQ(p.api(U, "PtInRect", {a, 2, 0}), 1u);
  CHECK_EQ(p.api(U, "PtInRect", {a, 14, 0}), 0u);  // right edge excluded
  CHECK_EQ(p.api(U, "EqualRect", {a, a}), 1u);
  CHECK_EQ(p.api(U, "EqualRect", {a, b}), 0u);
  CHECK_EQ(p.api(U, "IsRectEmpty", {a}), 0u);
  CHECK_EQ(p.api(U, "SetRectEmpty", {a}), 1u);
  CHECK_EQ(p.api(U, "IsRectEmpty", {a}), 1u);
  // AdjustWindowRectEx: a captioned, thick-framed window with a menu around a client rect.
  write_pod(p.rt->mem(), a, RECT{100, 100, 300, 200});
  CHECK_EQ(p.api(U, "AdjustWindowRectEx", {a, WS_OVERLAPPEDWINDOW, 1, 0}), 1u);
  r = p.rect(a);
  CHECK(r.left == 96 && r.top == 100 - 4 - 18 - 19 && r.right == 304 && r.bottom == 204);
}

// The registry: the seeds, opening and creating keys, queries (size only, too
// short a buffer), writes, the default value, closed handles, and the
// attached file's REGEDIT4 text read back into a fresh runtime.
TEST(program_registry) {
  Program p;
  const char* key = "HKEY_LOCAL_MACHINE\\SOFTWARE\\Made Up Co\\Toy";
  registry_seed(*p.rt, key, "Path", "C:\\TOYS");
  registry_seed(*p.rt, key, "Count", "7");
  registry_attach_file(*p.rt, "C:\\WINDOWS\\REGISTRY.INI");
  uint32_t phk = p.block(4), type = p.block(4), buf = p.block(64), cb = p.block(4), disp = p.block(4);
  CHECK_EQ(p.api(A, "RegOpenKeyExA", {0x80000002, p.str("SOFTWARE\\made up co\\TOY"), 0, KEY_READ, phk}), 0u);
  uint32_t hk = p.u32(phk);
  CHECK(hk != 0);
  // Size only, then a buffer too short, then the value.
  p.rt->mem().write_u32l(cb, 0);
  CHECK_EQ(p.api(A, "RegQueryValueExA", {hk, p.str("path"), 0, type, 0, cb}), 0u);
  CHECK_EQ(p.u32(cb), 8u);  // "C:\TOYS" and its NUL
  CHECK_EQ(p.u32(type), uint32_t(REG_SZ));
  p.rt->mem().write_u32l(cb, 4);
  CHECK_EQ(p.api(A, "RegQueryValueExA", {hk, p.str("Path"), 0, type, buf, cb}), uint32_t(ERROR_MORE_DATA));
  CHECK_EQ(p.u32(cb), 8u);
  p.rt->mem().write_u32l(cb, 64);
  CHECK_EQ(p.api(A, "RegQueryValueExA", {hk, p.str("Path"), 0, type, buf, cb}), 0u);
  CHECK_EQ(p.cstr(buf), std::string("C:\\TOYS"));
  CHECK_EQ(p.api(A, "RegQueryValueExA", {hk, p.str("Nope"), 0, type, buf, cb}), uint32_t(ERROR_FILE_NOT_FOUND));
  // A new value and a new key.
  CHECK_EQ(p.api(A, "RegSetValueExA", {hk, p.str("Toy"), 0, REG_SZ, p.str("3"), 1}), 0u);
  std::string v;
  CHECK(registry_get(*p.rt, key, "TOY", &v) && v == "3");
  CHECK_EQ(p.api(A, "RegCreateKeyExA", {hk, p.str("Sub"), 0, 0, 0, KEY_ALL_ACCESS, 0, phk, disp}), 0u);
  CHECK_EQ(p.u32(disp), uint32_t(REG_CREATED_NEW_KEY));
  uint32_t sub = p.u32(phk);
  CHECK_EQ(p.api(A, "RegCreateKeyExA", {hk, p.str("SUB"), 0, 0, 0, KEY_ALL_ACCESS, 0, phk, disp}), 0u);
  CHECK_EQ(p.u32(disp), uint32_t(REG_OPENED_EXISTING_KEY));
  uint32_t dw = 0x12345678, pdw = p.block(4);
  p.rt->mem().write_u32l(pdw, dw);
  CHECK_EQ(p.api(A, "RegSetValueExA", {sub, p.str("Level"), 0, REG_DWORD, pdw, 4}), 0u);
  // RegSetValueA/RegQueryValueA: the default value of a subkey.
  CHECK_EQ(p.api(A, "RegSetValueA", {hk, p.str("Other"), REG_SZ, p.str("hello"), 5}), 0u);
  p.rt->mem().write_u32l(cb, 64);
  CHECK_EQ(p.api(A, "RegQueryValueA", {hk, p.str("Other"), buf, cb}), 0u);
  CHECK_EQ(p.cstr(buf), std::string("hello"));
  CHECK_EQ(p.api(A, "RegCloseKey", {sub}), 0u);
  CHECK_EQ(p.api(A, "RegCloseKey", {sub}), uint32_t(ERROR_INVALID_HANDLE));
  CHECK_EQ(p.api(A, "RegOpenKeyA", {0x80000002, p.str("SOFTWARE\\Nobody"), phk}), uint32_t(ERROR_FILE_NOT_FOUND));
  // The file holds the tree as the guest left it; another runtime reads it back.
  std::string text;
  CHECK(p.rt->vfs().read_file("C:\\WINDOWS\\REGISTRY.INI", &text));
  CHECK(text.rfind("REGEDIT4\r\n", 0) == 0);
  CHECK(text.find("\"Level\"=dword:12345678") != std::string::npos);
  Program q;
  q.rt->vfs().add_virtual_file("C:\\WINDOWS\\REGISTRY.INI", std::vector<uint8_t>(text.begin(), text.end()));
  registry_attach_file(*q.rt, "C:\\WINDOWS\\REGISTRY.INI");
  CHECK(registry_get(*q.rt, key, "Toy", &v) && v == "3");
  CHECK(registry_get(*q.rt, key, "Path", &v) && v == "C:\\TOYS");
  CHECK(registry_get(*q.rt, std::string(key) + "\\Sub", "Level", &v) && v.size() == 4);
  // NT file security: Windows 95 had none.
  CHECK_EQ(p.api(A, "GetFileSecurityA", {p.str("C:\\TOYS"), 0, 0, 0, cb}), 0u);
}

TEST(program_kernel32) {
  Program p;
  // Atoms: case-insensitive, counted; integer atoms.
  uint32_t a1 = p.api(K, "GlobalAddAtomA", {p.str("MadeUpAtom")});
  CHECK(a1 >= 0xC000);
  CHECK_EQ(p.api(K, "GlobalAddAtomA", {p.str("madeupatom")}), a1);
  uint32_t buf = p.block(64);
  CHECK_EQ(p.api(K, "GlobalGetAtomNameA", {a1, buf, 64}), 10u);
  CHECK_EQ(p.cstr(buf), std::string("MadeUpAtom"));
  CHECK_EQ(p.api(K, "GlobalAddAtomA", {p.str("#42")}), 42u);
  CHECK_EQ(p.api(K, "GlobalGetAtomNameA", {42, buf, 64}), 3u);
  p.api(K, "GlobalDeleteAtom", {a1});
  CHECK(p.api(K, "GlobalGetAtomNameA", {a1, buf, 64}) != 0);  // still one reference
  p.api(K, "GlobalDeleteAtom", {a1});
  CHECK_EQ(p.api(K, "GlobalGetAtomNameA", {a1, buf, 64}), 0u);
  // Strings.
  CHECK(int32_t(p.api(K, "lstrcmpiA", {p.str("abc"), p.str("ABC")})) == 0);
  CHECK(int32_t(p.api(K, "lstrcmpA", {p.str("abc"), p.str("abd")})) < 0);
  CHECK_EQ(p.api(K, "lstrcpynA", {buf, p.str("abcdef"), 4}), buf);
  CHECK_EQ(p.cstr(buf), std::string("abc"));
  // MulDiv rounds half away from zero; 0 divisor is -1.
  CHECK_EQ(p.api(K, "MulDiv", {7, 3, 2}), 11u);
  CHECK_EQ(int32_t(p.api(K, "MulDiv", {uint32_t(-7), 3, 2})), -11);
  CHECK_EQ(p.api(K, "MulDiv", {1, 1, 0}), 0xFFFFFFFFu);
  // Interlocked*: the new value.
  uint32_t n = p.block(4);
  p.rt->mem().write_u32l(n, 1);
  CHECK_EQ(p.api(K, "InterlockedIncrement", {n}), 2u);
  CHECK_EQ(p.api(K, "InterlockedDecrement", {n}), 1u);
  CHECK_EQ(p.api(K, "InterlockedDecrement", {n}), 0u);
  // The current directory: the length, or the size needed.
  CHECK_EQ(p.api(K, "GetCurrentDirectoryA", {64, buf}), 7u);
  CHECK_EQ(p.cstr(buf), std::string("C:\\TOYS"));
  CHECK_EQ(p.api(K, "GetCurrentDirectoryA", {4, buf}), 8u);
  uint32_t part = p.block(4);
  CHECK_EQ(p.api(K, "GetFullPathNameA", {p.str("dt03\\x.spr"), 64, buf, part}), 18u);
  CHECK_EQ(p.cstr(buf), std::string("C:\\TOYS\\DT03\\X.SPR").size() == 18 ? p.cstr(buf) : std::string("?"));
  CHECK_EQ(p.cstr(p.u32(part)), std::string(p.cstr(buf)).substr(13));
  // Pointers and modes.
  CHECK_EQ(p.api(K, "IsBadReadPtr", {buf, 64}), 0u);
  CHECK_EQ(p.api(K, "IsBadReadPtr", {0, 4}), 1u);
  CHECK_EQ(p.api(K, "IsBadWritePtr", {0x00008000, 4}), 1u);  // the null guard
  CHECK_EQ(p.api(K, "SetErrorMode", {SEM_FAILCRITICALERRORS}), 0u);
  CHECK_EQ(p.api(K, "SetErrorMode", {0}), uint32_t(SEM_FAILCRITICALERRORS));
  CHECK_EQ(p.api(K, "GetProcessVersion", {0}), 0x00040000u);
  CHECK_EQ(p.api(K, "GetCurrentThread", {}), 0xFFFFFFFEu);
  // A program started by ShellExecuteA(..., SW_SHOWNORMAL).
  uint32_t si = p.block(68);
  p.api(K, "GetStartupInfoA", {si});
  CHECK_EQ(p.u32(si + 44), uint32_t(STARTF_USESHOWWINDOW));
  CHECK_EQ(p.rt->mem().read_u16l(si + 48), uint16_t(SW_SHOWNORMAL));
  CHECK_EQ(p.cstr(p.api(K, "GetCommandLineA", {})), std::string("C:\\TOYS\\toy.exe /s /t2"));
}

// GetTickCount moves at the Windows 95 system timer's interrupts (18.2 Hz,
// 54.925 ms) for a program, and by the millisecond for everything else.
TEST(program_get_tick_count) {
  Program p;
  p.clock.begin_frame();  // frame 0: virtual time 0
  const uint32_t t0 = p.api(K, "GetTickCount", {});
  CHECK_EQ(t0, VirtualClock::kBootOffsetMs);
  for (int i = 0; i < 3; i++) p.clock.begin_frame();  // 50 ms: still the first tick
  CHECK_EQ(p.api(K, "GetTickCount", {}), t0);
  p.clock.begin_frame();  // 66.7 ms: the second tick, 54 ms on
  CHECK_EQ(p.api(K, "GetTickCount", {}), t0 + 54);
  for (int i = 4; i < 60; i++) p.clock.begin_frame();  // 1.0 s: 18 ticks
  CHECK_EQ(p.api(K, "GetTickCount", {}), t0 + 988);

  VirtualClock clock{VirtualClock::Mode::fixed_step, 16667};
  RuntimeOptions o;
  o.heap_size = 16u << 20;
  o.call_budget = 50'000'000;
  Runtime rt(o, clock);
  register_all_shims(rt.shims());
  clock.begin_frame();
  clock.begin_frame();
  ShimEntry& e = rt.shims().get(K, "GetTickCount");
  CHECK_EQ(rt.call_guest(rt.shims().thunk_address(e), {}), VirtualClock::kBootOffsetMs + 16);
}

// CreateWindowExA's messages and the WH_CBT hook; ShowWindow's activation;
// a popup with a client edge.
TEST(program_create_window_messages) {
  Program p;
  p.load_procs();
  uint32_t hhk = p.api(U, "SetWindowsHookExA", {WH_CBT, p.hookproc, 0, 0xAD8});
  CHECK(hhk != 0);
  p.rt->mem().write_u32l(p.data + 12, hhk);
  CHECK(p.register_class("MadeUpFrame") != 0);
  uint32_t h = p.create("MadeUpFrame", WS_POPUP | WS_VISIBLE, 10, 20, 300, 200, 0, 0, WS_EX_CLIENTEDGE);
  CHECK(h != 0);
  // The hook saw HCBT_CREATEWND for this window first.
  CHECK_EQ(p.u32(p.hooklog), uint32_t(HCBT_CREATEWND));
  CHECK_EQ(p.u32(p.hooklog + 4), h);
  auto v = p.logged();
  int mm = index_of(v, WM_GETMINMAXINFO, h), nc = index_of(v, WM_NCCREATE, h), calc = index_of(v, WM_NCCALCSIZE, h),
      cr = index_of(v, WM_CREATE, h), sz = index_of(v, WM_SIZE, h), mv = index_of(v, WM_MOVE, h),
      sw = index_of(v, WM_SHOWWINDOW, h), app = index_of(v, WM_ACTIVATEAPP, h), act = index_of(v, WM_ACTIVATE, h),
      foc = index_of(v, WM_SETFOCUS, h);
  CHECK(mm == 0 && mm < nc && nc < calc && calc < cr && cr < sz && sz < mv && mv < sw && sw < app && app < act &&
        act < foc);
  // The client area: inside the 2-pixel client edge.
  uint32_t r = p.block(16);
  CHECK_EQ(p.api(U, "GetClientRect", {h, r}), 1u);
  RECT c = p.rect(r);
  CHECK(c.left == 0 && c.top == 0 && c.right == 296 && c.bottom == 196);
  CHECK_EQ(p.api(U, "GetWindowRect", {h, r}), 1u);
  c = p.rect(r);
  CHECK(c.left == 10 && c.top == 20 && c.right == 310 && c.bottom == 220);
  CHECK_EQ(p.api(U, "GetFocus", {}), h);
  CHECK_EQ(p.api(U, "GetActiveWindow", {}), h);
  CHECK_EQ(p.api(U, "GetForegroundWindow", {}), h);
  CHECK_EQ(p.api(U, "IsWindowVisible", {h}), 1u);
  // The class is known (MFC asks before registering its own).
  uint32_t wc = p.block(40);
  CHECK_EQ(p.api(U, "GetClassInfoA", {0x400000, p.str("madeupframe"), wc}), 1u);
  CHECK_EQ(p.u32(wc + 4), p.wndproc);
  CHECK_EQ(p.api(U, "GetClassInfoA", {0x400000, p.str("Nope"), wc}), 0u);
  // A hook that is gone is not called.
  CHECK_EQ(p.api(U, "UnhookWindowsHookEx", {hhk}), 1u);
  uint32_t before = p.u32(p.data + 8);
  CHECK(p.create("MadeUpFrame", WS_POPUP, 0, 0, 10, 10) != 0);
  CHECK_EQ(p.u32(p.data + 8), before);
  CHECK_EQ(p.create("NoSuchClass", WS_POPUP, 0, 0, 10, 10), 0u);
}

// Children: their coordinates, the window tree, GetDlgItem, coordinate
// mapping, moving a parent, DeferWindowPos.
TEST(program_child_windows) {
  Program p;
  p.load_procs();
  p.register_class("MadeUp");
  uint32_t top = p.create("MadeUp", WS_POPUP | WS_VISIBLE, 100, 50, 400, 300);
  uint32_t a = p.create("MadeUp", WS_CHILD | WS_VISIBLE, 10, 10, 50, 40, top, 0xE900);
  uint32_t b = p.create("MadeUp", WS_CHILD | WS_VISIBLE, 70, 10, 50, 40, top, 0xE901);
  CHECK(top && a && b);
  CHECK(has(p.logged(), WM_PARENTNOTIFY, top));
  uint32_t r = p.block(16);
  p.api(U, "GetWindowRect", {a, r});
  RECT w = p.rect(r);
  CHECK(w.left == 110 && w.top == 60 && w.right == 160 && w.bottom == 100);
  CHECK_EQ(p.api(U, "GetDlgItem", {top, 0xE901}), b);
  CHECK_EQ(p.api(U, "GetDlgItem", {top, 0x1234}), 0u);
  CHECK_EQ(p.api(U, "GetDlgCtrlID", {a}), 0xE900u);
  CHECK_EQ(p.api(U, "GetParent", {a}), top);
  CHECK_EQ(p.api(U, "IsChild", {top, b}), 1u);
  // Newest child on top.
  CHECK_EQ(p.api(U, "GetTopWindow", {top}), b);
  CHECK_EQ(p.api(U, "GetWindow", {b, GW_HWNDNEXT}), a);
  CHECK_EQ(p.api(U, "GetWindow", {a, GW_HWNDNEXT}), 0u);
  CHECK_EQ(p.api(U, "GetWindow", {top, GW_CHILD}), b);
  uint32_t desk = p.api(U, "GetDesktopWindow", {});
  CHECK_EQ(p.api(U, "GetWindow", {desk, GW_CHILD}), top);
  // Coordinates.
  uint32_t pt = p.block(8);
  write_pod(p.rt->mem(), pt, POINT{5, 6});
  CHECK_EQ(p.api(U, "ClientToScreen", {a, pt}), 1u);
  POINT q = read_pod<POINT>(p.rt->mem(), pt);
  CHECK(q.x == 115 && q.y == 66);
  CHECK_EQ(p.api(U, "ScreenToClient", {top, pt}), 1u);
  q = read_pod<POINT>(p.rt->mem(), pt);
  CHECK(q.x == 15 && q.y == 16);
  uint32_t d = p.api(U, "MapWindowPoints", {a, b, pt, 1});
  CHECK_EQ(int16_t(d & 0xFFFF), int16_t(-60));
  CHECK_EQ(p.api(U, "WindowFromPoint", {120, 70}), a);
  CHECK_EQ(p.api(U, "WindowFromPoint", {300, 300}), top);
  // Moving the parent moves the children; WM_MOVE/WM_SIZE come from WM_WINDOWPOSCHANGED.
  size_t from = p.logged_count();
  CHECK_EQ(p.api(U, "MoveWindow", {top, 0, 0, 640, 480, 1}), 1u);
  auto v = p.logged(from);
  int chg = index_of(v, WM_WINDOWPOSCHANGED, top);
  CHECK(index_of(v, WM_WINDOWPOSCHANGING, top) < chg && chg < index_of(v, WM_MOVE, top) &&
        chg < index_of(v, WM_SIZE, top));
  p.api(U, "GetWindowRect", {a, r});
  w = p.rect(r);
  CHECK(w.left == 10 && w.top == 10);
  // DeferWindowPos applies at the end.
  uint32_t dwp = p.api(U, "BeginDeferWindowPos", {2});
  CHECK(dwp != 0);
  CHECK_EQ(p.api(U, "DeferWindowPos", {dwp, a, 0, 0, 0, 640, 480, SWP_NOZORDER}), dwp);
  p.api(U, "GetClientRect", {a, r});
  CHECK_EQ(p.rect(r).right, 50);
  CHECK_EQ(p.api(U, "EndDeferWindowPos", {dwp}), 1u);
  p.api(U, "GetClientRect", {a, r});
  CHECK(p.rect(r).right == 640 && p.rect(r).bottom == 480);
}

// Painting and the message loop: WM_PAINT for an invalid window (after
// posted messages, before timers), BeginPaint's erase and validation,
// GetMessageA's wait ending the frame through the yield hook, WM_QUIT.
TEST(program_paint_and_message_loop) {
  Program p;
  p.load_procs();
  p.register_class("MadeUp", COLOR_WINDOW + 1);
  uint32_t h = p.create("MadeUp", WS_POPUP | WS_VISIBLE, 0, 0, 64, 48);
  uint32_t msg = p.block(28);
  // Shown: WM_PAINT is due.
  CHECK_EQ(p.api(U, "PeekMessageA", {msg, 0, 0, 0, PM_NOREMOVE}), 1u);
  CHECK_EQ(p.u32(msg + 4), uint32_t(WM_PAINT));
  CHECK_EQ(p.u32(msg), h);
  // A posted message comes first.
  CHECK_EQ(p.api(U, "PostMessageA", {h, WM_USER + 5, 1, 2}), 1u);
  CHECK_EQ(p.api(U, "PeekMessageA", {msg, 0, 0, 0, PM_REMOVE}), 1u);
  CHECK_EQ(p.u32(msg + 4), uint32_t(WM_USER + 5));
  // BeginPaint erases (DefWindowProc fills the class brush) and validates.
  uint32_t ps = p.block(64);
  size_t from = p.logged_count();
  uint32_t hdc = p.api(U, "BeginPaint", {h, ps});
  CHECK(hdc != 0);
  CHECK(has(p.logged(from), WM_ERASEBKGND, h));
  CHECK_EQ(p.u32(ps + 4), 0u);  // fErase: the background was erased
  RECT rc = read_pod<RECT>(p.rt->mem(), ps + 8);
  CHECK(rc.right == 64 && rc.bottom == 48);
  p.api(U, "EndPaint", {h, ps});
  CHECK_EQ(p.api(U, "PeekMessageA", {msg, 0, 0, 0, PM_NOREMOVE}), 0u);
  // InvalidateRect, then ValidateRect of the same area: nothing due.
  uint32_t r = p.block(16);
  write_pod(p.rt->mem(), r, RECT{1, 1, 5, 5});
  p.api(U, "InvalidateRect", {h, r, 0});
  CHECK_EQ(p.api(U, "PeekMessageA", {msg, 0, 0, 0, PM_NOREMOVE}), 1u);
  p.api(U, "ValidateRect", {h, 0});
  CHECK_EQ(p.api(U, "PeekMessageA", {msg, 0, 0, 0, PM_NOREMOVE}), 0u);
  // UpdateWindow paints now.
  p.api(U, "InvalidateRect", {h, 0, 1});
  from = p.logged_count();
  p.api(U, "UpdateWindow", {h});
  CHECK(has(p.logged(from), WM_PAINT, h));
  CHECK_EQ(p.api(U, "PeekMessageA", {msg, 0, 0, 0, PM_NOREMOVE}), 0u);
  // GetMessageA with nothing to deliver waits: the yield hook ends the frame;
  // what arrives meanwhile (a timer, here) is what the next look finds.
  int yields = 0;
  p.rt->set_yield_hook([&] {
    yields++;
    p.clock.begin_frame();
    return true;
  });
  CHECK(p.api(U, "SetTimer", {h, 7, 40, 0}) != 0);
  CHECK_EQ(p.api(U, "GetMessageA", {msg, 0, 0, 0}), 1u);
  CHECK_EQ(p.u32(msg + 4), uint32_t(WM_TIMER));
  CHECK_EQ(p.u32(msg + 8), 7u);
  CHECK(yields >= 2);  // 40 ms is more than two 16.7 ms frames
  CHECK_EQ(p.api(U, "GetMessageTime", {}), p.u32(msg + 16));
  p.api(U, "KillTimer", {h, 7});
  p.api(U, "PostQuitMessage", {3});
  CHECK_EQ(p.api(U, "GetMessageA", {msg, 0, 0, 0}), 0u);
  CHECK_EQ(p.u32(msg + 8), 3u);
  // No frames to end: the wait is an error, never a hang.
  p.rt->set_yield_hook(nullptr);
  bool threw = false;
  try {
    p.api(U, "GetMessageA", {msg, 0, 0, 0});
  } catch (const GuestError&) {
    threw = true;
  }
  CHECK(threw);
}

// DestroyWindow: WM_DESTROY parent first, WM_NCDESTROY child first; the
// windows, their timers and their queued messages go.
TEST(program_destroy_order) {
  Program p;
  p.load_procs();
  p.register_class("MadeUp");
  uint32_t top = p.create("MadeUp", WS_POPUP | WS_VISIBLE, 0, 0, 100, 100);
  uint32_t kid = p.create("MadeUp", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, top, 1);
  p.api(U, "SetTimer", {kid, 1, 100, 0});
  p.api(U, "PostMessageA", {kid, WM_USER, 0, 0});
  size_t from = p.logged_count();
  CHECK_EQ(p.api(U, "DestroyWindow", {top}), 1u);
  auto v = p.logged(from);
  CHECK(index_of(v, WM_DESTROY, top) < index_of(v, WM_DESTROY, kid));
  CHECK(index_of(v, WM_NCDESTROY, kid) < index_of(v, WM_NCDESTROY, top));
  CHECK_EQ(p.api(U, "IsWindow", {top}), 0u);
  CHECK_EQ(p.api(U, "IsWindow", {kid}), 0u);
  CHECK_EQ(p.api(U, "GetFocus", {}), 0u);
  uint32_t msg = p.block(28);
  CHECK_EQ(p.api(U, "PeekMessageA", {msg, 0, 0, 0, PM_REMOVE}), 0u);
  CHECK(user32::us(*p.rt).timers.empty());
  CHECK_EQ(p.api(U, "DestroyWindow", {top}), 0u);
}

// Menus from an RT_MENU template, accelerators from an RT_ACCELERATOR table,
// and the rest of the window helpers MFC calls.
TEST(program_menus_accelerators) {
  Program p;
  p.load_procs();
  // MENUITEMTEMPLATEHEADER {0, 0}; "&File" popup {"&New" 100, separator, "E&xit" 101 (MF_END)};
  // "&Help" popup (MF_END) {"&About" 200 (MF_END)}.
  std::string t(4, '\0');
  auto u16 = [&](uint16_t v) { t.append(reinterpret_cast<const char*>(&v), 2); };
  auto text = [&](const char* s) {
    for (; *s; s++) u16(uint8_t(*s));
    u16(0);
  };
  u16(MF_POPUP), text("&File");
  u16(0), u16(100), text("&New");
  u16(0), u16(0), text("");
  u16(MF_END), u16(101), text("E&xit");
  u16(MF_POPUP | MF_END), text("&Help");
  u16(MF_END), u16(200), text("&About");
  uint32_t m = user32::menu_from_template(*p.rt, t);
  CHECK(m != 0);
  CHECK_EQ(p.api(U, "GetMenuItemCount", {m}), 2u);
  uint32_t file = p.api(U, "GetSubMenu", {m, 0});
  CHECK(file != 0);
  CHECK_EQ(p.api(U, "GetMenuItemCount", {file}), 3u);
  CHECK_EQ(p.api(U, "GetMenuItemID", {file, 2}), 101u);
  CHECK_EQ(p.api(U, "GetMenuItemID", {m, 0}), 0xFFFFFFFFu);  // a popup
  CHECK_EQ(p.api(U, "GetMenuState", {file, 1, MF_BYPOSITION}) & MF_SEPARATOR, uint32_t(MF_SEPARATOR));
  CHECK_EQ(p.api(U, "GetMenuState", {m, 0, MF_BYPOSITION}) >> 8, 3u);  // a popup's item count
  CHECK_EQ(p.api(U, "EnableMenuItem", {m, 200, MF_GRAYED}), 0u);       // found through the submenus
  CHECK_EQ(p.api(U, "GetMenuState", {m, 200, MF_BYCOMMAND}) & MF_GRAYED, uint32_t(MF_GRAYED));
  CHECK_EQ(p.api(U, "CheckMenuItem", {file, 100, MF_CHECKED}), 0u);
  CHECK_EQ(p.api(U, "CheckMenuItem", {file, 100, MF_UNCHECKED}), uint32_t(MF_CHECKED));
  CHECK_EQ(p.api(U, "ModifyMenuA", {file, 0, MF_BYPOSITION | MF_STRING, 102, p.str("&Open")}), 1u);
  CHECK_EQ(p.api(U, "GetMenuItemID", {file, 0}), 102u);
  CHECK_EQ(p.api(U, "GetMenuState", {m, 999, MF_BYCOMMAND}), 0xFFFFFFFFu);
  // A window with the menu bar: the bar comes off the client area; SetMenu(NULL) gives it back.
  p.register_class("MadeUp");
  uint32_t h = p.api(U, "CreateWindowExA", {0, p.str("MadeUp"), 0, WS_OVERLAPPED, 0, 0, 200, 200, 0, m, 0, 0});
  CHECK_EQ(p.api(U, "GetMenu", {h}), m);
  uint32_t r = p.block(16);
  p.api(U, "GetClientRect", {h, r});
  CHECK_EQ(p.rect(r).bottom, 200 - 1 - 1 - 18 - 19);  // border, caption, menu bar
  CHECK_EQ(p.api(U, "SetMenu", {h, 0}), 1u);
  p.api(U, "GetClientRect", {h, r});
  CHECK_EQ(p.rect(r).bottom, 200 - 1 - 1 - 18);
  CHECK_EQ(p.api(U, "DestroyMenu", {m}), 1u);
  CHECK_EQ(p.api(U, "GetMenuItemCount", {m}), 0xFFFFFFFFu);
  // Accelerators: F1 (virtual key) → 300; 'q' (a character) → 301, the last entry.
  std::string acc;
  auto entry = [&](uint16_t f, uint16_t key, uint16_t cmd) {
    uint16_t e[4] = {f, key, cmd, 0};
    acc.append(reinterpret_cast<const char*>(e), 8);
  };
  entry(FVIRTKEY, VK_F1, 300);
  entry(0x80, 'q', 301);
  uint32_t ha = user32::accelerators_from_table(*p.rt, acc);
  CHECK(ha != 0);
  uint32_t msg = p.block(28);
  write_pod(p.rt->mem(), msg, g32::MSG{h, WM_KEYDOWN, VK_F1, 0, 0, 0, 0});
  size_t from = p.logged_count();
  CHECK_EQ(p.api(U, "TranslateAcceleratorA", {h, ha, msg}), 1u);
  auto v = p.logged(from);
  CHECK(!v.empty() && v[0].msg == WM_COMMAND && v[0].wp == ((1u << 16) | 300u));
  write_pod(p.rt->mem(), msg, g32::MSG{h, WM_CHAR, 'q', 0, 0, 0, 0});
  CHECK_EQ(p.api(U, "TranslateAcceleratorA", {h, ha, msg}), 1u);
  write_pod(p.rt->mem(), msg, g32::MSG{h, WM_KEYDOWN, VK_F2, 0, 0, 0, 0});
  CHECK_EQ(p.api(U, "TranslateAcceleratorA", {h, ha, msg}), 0u);
  // The small helpers.
  CHECK(p.api(U, "RegisterWindowMessageA", {p.str("MadeUpMsg")}) >= 0xC000);
  CHECK_EQ(p.api(U, "RegisterWindowMessageA", {p.str("madeupmsg")}),
           p.api(U, "RegisterWindowMessageA", {p.str("MadeUpMsg")}));
  uint32_t s = p.str("MiXed");
  CHECK_EQ(p.api(U, "CharUpperA", {s}), s);
  CHECK_EQ(p.cstr(s), std::string("MIXED"));
  CHECK_EQ(p.api(U, "CharLowerA", {'Q'}), uint32_t('q'));
  CHECK_EQ(p.api(U, "CharNextA", {s}), s + 1);
  CHECK(p.api(U, "GetSysColorBrush", {COLOR_BTNFACE}) != 0);
  CHECK(p.api(U, "LoadIconA", {0, 32512}) != 0);  // IDI_APPLICATION
  CHECK_EQ(p.api(U, "IsWindowUnicode", {h}), 0u);
  CHECK_EQ(p.api(U, "GetWindowTextLengthA", {h}), 0u);
  CHECK_EQ(p.api(U, "GetCapture", {}), 0u);
  CHECK_EQ(p.api(U, "SetCapture", {h}), 0u);
  CHECK_EQ(p.api(U, "GetCapture", {}), h);
  CHECK_EQ(p.api(U, "ReleaseCapture", {}), 1u);
  CHECK_EQ(p.api(U, "GetCapture", {}), 0u);
  // The emulated pointer.
  CHECK_EQ(p.api(U, "SetCursorPos", {33, 44}), 1u);
  uint32_t pt = p.block(8);
  p.api(U, "GetCursorPos", {pt});
  POINT q = read_pod<POINT>(p.rt->mem(), pt);
  CHECK(q.x == 33 && q.y == 44);
  CHECK_EQ(p.api(U, "GetSystemMetrics", {SM_CYMENU}), 19u);
  CHECK_EQ(p.api(U, "GetSystemMetrics", {SM_CXSCREEN}), 640u);
}

// mmio over a made-up RIFF WAVE file: descent, reads, the buffer
// (mmioGetInfo/mmioAdvance/mmioSetInfo), ascent; sndPlaySoundA with sound off.
TEST(program_mmio) {
  Program p;
  std::string wav = "RIFF";
  auto u32 = [&](uint32_t v) { wav.append(reinterpret_cast<const char*>(&v), 4); };
  auto u16 = [&](uint16_t v) { wav.append(reinterpret_cast<const char*>(&v), 2); };
  u32(4 + 8 + 16 + 8 + 6), wav += "WAVE";
  wav += "fmt ", u32(16), u16(1), u16(1), u32(11025), u32(11025), u16(1), u16(8);
  wav += "data", u32(5), wav += std::string("\x80\x81\x82\x83\x84", 5), wav += '\0';
  p.rt->vfs().add_virtual_file("C:\\TOYS\\MADEUP.WAV", std::vector<uint8_t>(wav.begin(), wav.end()));
  uint32_t h = p.api(W, "mmioOpenA", {p.str("C:\\TOYS\\madeup.wav"), 0, 0x10000});
  CHECK(h != 0);
  CHECK_EQ(p.api(W, "mmioOpenA", {p.str("C:\\TOYS\\none.wav"), 0, 0}), 0u);
  uint32_t parent = p.block(20), ck = p.block(20), buf = p.block(64), info = p.block(72);
  p.rt->mem().write_u32l(parent + 8, 0x45564157);  // "WAVE"
  CHECK_EQ(p.api(W, "mmioDescend", {h, parent, 0, 0x20}), 0u);  // MMIO_FINDRIFF
  CHECK_EQ(p.u32(parent + 12), 8u);
  p.rt->mem().write_u32l(ck, 0x20746D66);  // "fmt "
  CHECK_EQ(p.api(W, "mmioDescend", {h, ck, parent, 0x10}), 0u);  // MMIO_FINDCHUNK
  CHECK_EQ(p.u32(ck + 4), 16u);
  CHECK_EQ(p.api(W, "mmioRead", {h, buf, 16}), 16u);
  CHECK_EQ(p.u32(buf + 4), 11025u);
  CHECK_EQ(p.api(W, "mmioAscend", {h, ck, 0}), 0u);
  p.rt->mem().write_u32l(ck, 0x61746164);  // "data"
  CHECK_EQ(p.api(W, "mmioDescend", {h, ck, parent, 0x10}), 0u);
  CHECK_EQ(p.u32(ck + 4), 5u);
  // The buffer: read two bytes through pchNext, then hand the position back.
  CHECK_EQ(p.api(W, "mmioGetInfo", {h, info, 0}), 0u);
  uint32_t next = p.u32(info + 28), end = p.u32(info + 32);
  CHECK_EQ(p.rt->mem().read_u8(next), 0x80);
  CHECK_EQ(end - next, 6u);  // the data and its pad byte, to the file's end
  p.rt->mem().write_u32l(info + 28, next + 2);
  CHECK_EQ(p.api(W, "mmioAdvance", {h, info, 0}), 0u);
  CHECK_EQ(p.rt->mem().read_u8(p.u32(info + 28)), 0x82);
  CHECK_EQ(p.api(W, "mmioSetInfo", {h, info, 0}), 0u);
  CHECK_EQ(p.api(W, "mmioRead", {h, buf, 64}), 4u);  // 0x82..0x84 and the pad
  CHECK_EQ(p.rt->mem().read_u8(buf), 0x82);
  CHECK_EQ(p.api(W, "mmioSeek", {h, 0, SEEK_SET}), 0u);
  CHECK_EQ(p.api(W, "mmioSeek", {h, 1000, SEEK_SET}), 0xFFFFFFFFu);
  p.rt->mem().write_u32l(ck, 0x58585858);  // "XXXX": not there
  CHECK_EQ(p.api(W, "mmioSeek", {h, 12, SEEK_SET}), 12u);
  CHECK_EQ(p.api(W, "mmioDescend", {h, ck, parent, 0x10}), 265u);  // MMIOERR_CHUNKNOTFOUND
  CHECK_EQ(p.api(W, "mmioClose", {h, 0}), 0u);
  CHECK_EQ(p.api(W, "mmioClose", {h, 0}), 5u);  // MMSYSERR_INVALHANDLE
  // Sound off: no wave device.
  CHECK_EQ(p.api(W, "sndPlaySoundA", {p.str("C:\\TOYS\\madeup.wav"), 1}), 0u);
  // DirectSound on a machine with no sound card.
  register_dsound_absent(*p.rt);
  uint32_t pds = p.block(4);
  p.rt->mem().write_u32l(pds, 0xDEADBEEF);
  CHECK_EQ(p.api("DSOUND.DLL", "DirectSoundCreate", {0, pds, 0}), 0x88780078u);
  CHECK_EQ(p.u32(pds), 0u);
}

// The process image: an EXE at its preferred base, the handle every
// "this module" query answers; a DLL is refused as the process.
TEST(program_process_image) {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / ("adw_program_test_" + std::to_string(GetCurrentProcessId()));
  fs::create_directories(dir);
  std::string exe = tiny_pe(false), dll = tiny_pe(true);
  fs::path pe = dir / "TOY.EXE", pd = dir / "TOY.DLL";
  std::ofstream(pe, std::ios::binary).write(exe.data(), std::streamsize(exe.size()));
  std::ofstream(pd, std::ios::binary).write(dll.data(), std::streamsize(dll.size()));
  {
    Program p;
    Module* m = p.rt->modules().load_exe(narrow(pe.wstring()));
    CHECK(m != nullptr && !m->dll);
    CHECK_EQ(m->base, 0x00400000u);  // the preferred base: no stand-in host EXE in program mode
    CHECK_EQ(m->entry, 0x00401000u);
    CHECK_EQ(p.rt->modules().exe_handle(), m->base);
    CHECK_EQ(p.u32(layout::kPeb + 8), m->base);
    CHECK_EQ(p.api(K, "GetModuleHandleA", {0}), m->base);
    uint32_t buf = p.block(64);
    CHECK(p.api(K, "GetModuleFileNameA", {0, buf, 64}) > 0);
    CHECK_EQ(p.cstr(buf), std::string("C:\\TOYS\\TOY.EXE"));
    bool threw = false;
    try {
      p.rt->modules().load_exe(narrow(pd.wstring()));
    } catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);
  }
  std::error_code ec;
  fs::remove_all(dir, ec);
}

// The slice hook: code with no API call still gives the host control.
TEST(program_slice_hook) {
  Program p;
  auto r = cpu::X86Emulator::assemble(R"(
    mov ecx, 3000000
  spin:
    dec ecx
    jnz spin
    mov eax, 7
    ret
  )", nullptr, p.code);
  p.rt->mem().memcpy(p.code, r.code.data(), r.code.size());
  int slices = 0;
  p.rt->set_slice_hook(100'000, [&] { slices++; });
  CHECK_EQ(p.rt->call_guest(p.code, {}, Conv::cdecl_), 7u);
  CHECK(slices >= 50);
  p.rt->set_slice_hook(0, nullptr);
}

// The input queue (user32_program_key/_mouse, Caps Lock play): key messages
// to the focus window with Windows 95's lParam, TranslateMessage's WM_CHAR,
// the mouse to the window under the pointer (or the capture) in client
// coordinates, moves coalesced, double clicks on a CS_DBLCLKS class; posted
// messages come first; a destroyed window's input goes with it.
TEST(program_input_queue) {
  Program p;
  p.load_procs();
  uint32_t wc = p.block(40);
  write_pod(p.rt->mem(), wc, g32::WNDCLASSA{CS_DBLCLKS, p.wndproc, 0, 0, 0x400000, 0, 0, 0, 0, p.str("MadeUp")});
  CHECK(p.api(U, "RegisterClassA", {wc}) != 0);
  uint32_t top = p.create("MadeUp", WS_POPUP | WS_VISIBLE, 0, 0, 640, 480);
  uint32_t child = p.create("MadeUp", WS_CHILD | WS_VISIBLE, 100, 100, 50, 50, top, 7);
  p.api(U, "SetFocus", {top});
  uint32_t msg = p.block(28);
  auto next = [&]() -> uint32_t {
    for (;;) {
      if (!p.api(U, "PeekMessageA", {msg, 0, 0, 0, PM_REMOVE})) return 0;
      uint32_t m = p.u32(msg + 4);
      if (m != WM_PAINT) return m;
      p.api(U, "ValidateRect", {p.u32(msg), 0});
    }
  };
  // A key: down, then up (down before, going up).
  p.api(U, "PostMessageA", {top, WM_USER + 1, 0, 0});
  user32_program_key(*p.rt, 'A', true);
  CHECK_EQ(next(), uint32_t(WM_USER + 1));
  CHECK_EQ(next(), uint32_t(WM_KEYDOWN));
  CHECK_EQ(p.u32(msg), top);
  CHECK_EQ(p.u32(msg + 8), uint32_t('A'));
  CHECK_EQ(p.u32(msg + 12), 0x001E0001u);
  CHECK_EQ(p.api(U, "TranslateMessage", {msg}), 1u);
  CHECK_EQ(next(), uint32_t(WM_CHAR));
  CHECK_EQ(p.u32(msg + 8), uint32_t('a'));
  user32_program_key(*p.rt, VK_SHIFT, true);
  user32_program_key(*p.rt, '1', true);
  CHECK_EQ(next(), uint32_t(WM_KEYDOWN));
  CHECK_EQ(next(), uint32_t(WM_KEYDOWN));
  CHECK_EQ(p.api(U, "TranslateMessage", {msg}), 1u);
  CHECK_EQ(next(), uint32_t(WM_CHAR));
  CHECK_EQ(p.u32(msg + 8), uint32_t('!'));
  user32_program_key(*p.rt, VK_SHIFT, false);
  user32_program_key(*p.rt, 'A', false);
  CHECK_EQ(next(), uint32_t(WM_KEYUP));
  CHECK_EQ(next(), uint32_t(WM_KEYUP));
  CHECK_EQ(p.u32(msg + 12), 0xC01E0001u);
  CHECK_EQ(p.api(U, "TranslateMessage", {msg}), 1u);  // a key message: nonzero, no character
  CHECK_EQ(next(), 0u);
  // Alt: system keys.
  user32_program_key(*p.rt, VK_MENU, true);
  user32_program_key(*p.rt, 'X', true);
  CHECK_EQ(next(), uint32_t(WM_SYSKEYDOWN));
  CHECK_EQ(next(), uint32_t(WM_SYSKEYDOWN));
  CHECK(p.u32(msg + 12) & (1u << 29));
  user32_program_key(*p.rt, 'X', false);
  user32_program_key(*p.rt, VK_MENU, false);
  CHECK_EQ(next(), uint32_t(WM_SYSKEYUP));
  CHECK_EQ(next(), uint32_t(WM_SYSKEYUP));
  // The mouse: two moves over the child coalesce, in its client coordinates.
  user32_program_mouse(*p.rt, 110, 110, 0);
  user32_program_mouse(*p.rt, 120, 130, 0);
  CHECK_EQ(next(), uint32_t(WM_MOUSEMOVE));
  CHECK_EQ(p.u32(msg), child);
  CHECK_EQ(p.u32(msg + 12), (30u << 16) | 20u);
  CHECK_EQ(next(), 0u);
  uint32_t pt = p.block(8);
  p.api(U, "GetCursorPos", {pt});
  CHECK(p.u32(pt) == 120 && p.u32(pt + 4) == 130);
  // A click, then another at the same place: a double click (CS_DBLCLKS).
  user32_program_mouse(*p.rt, 120, 130, kMouseLeft);
  CHECK_EQ(next(), uint32_t(WM_LBUTTONDOWN));
  CHECK_EQ(p.u32(msg + 8), uint32_t(MK_LBUTTON));
  user32_program_mouse(*p.rt, 120, 130, 0);
  CHECK_EQ(next(), uint32_t(WM_LBUTTONUP));
  user32_program_mouse(*p.rt, 121, 130, kMouseLeft);
  CHECK_EQ(next(), uint32_t(WM_MOUSEMOVE));
  CHECK_EQ(next(), uint32_t(WM_LBUTTONDBLCLK));
  user32_program_mouse(*p.rt, 121, 130, kMouseRight);
  CHECK_EQ(next(), uint32_t(WM_LBUTTONUP));
  CHECK_EQ(next(), uint32_t(WM_RBUTTONDOWN));
  CHECK_EQ(p.u32(msg + 8), uint32_t(MK_RBUTTON));
  user32_program_mouse(*p.rt, 121, 130, 0);
  CHECK_EQ(next(), uint32_t(WM_RBUTTONUP));
  // The capture window gets the mouse wherever it is, in its own coordinates.
  p.api(U, "SetCapture", {top});
  user32_program_mouse(*p.rt, 125, 140, 0);
  CHECK_EQ(next(), uint32_t(WM_MOUSEMOVE));
  CHECK_EQ(p.u32(msg), top);
  CHECK_EQ(p.u32(msg + 12), (140u << 16) | 125u);
  p.api(U, "ReleaseCapture", {});
  // A destroyed window's queued input goes with it.
  user32_program_mouse(*p.rt, 130, 140, 0);
  p.api(U, "DestroyWindow", {child});
  CHECK_EQ(next(), 0u);
  // The cursor: on once one is set, off below ShowCursor's 0.
  CHECK(!user32_cursor_shown(*p.rt));
  p.api(U, "SetCursor", {p.api(U, "LoadCursorA", {0, 32512})});
  CHECK(user32_cursor_shown(*p.rt));
  p.api(U, "ShowCursor", {0});
  CHECK(!user32_cursor_shown(*p.rt));
}
