// The API families, one file per DLL. Each file defines register_<dll>(), which
// binds handlers to the names in its DLL (signatures come from
// signatures.cc). register_all_shims() is what a lane calls.
//
// Ownership (parallel work): a family file owns its handlers and its own
// RuntimeState struct (rt.state<T>()); shared object models live in their own
// headers — gdi_objects.hh (GDI handles, used by USER32 and GDI32),
// display.hh (the screen and palette), heap.hh (memory), modules.hh (images),
// vfs.hh (paths), seh.hh (exceptions). Host-facing helpers a lane (or another
// family) needs from a family are declared here. README.md has the recipe.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "loader/pe.hh"
#include "win32/shims.hh"

namespace adw::win32 {

class Runtime;
class IniStore;

void register_kernel32(ShimRegistry& r);
void register_user32(ShimRegistry& r);
void register_gdi32(ShimRegistry& r);
void register_winmm(ShimRegistry& r);
void register_msacm32(ShimRegistry& r);
void register_shell32(ShimRegistry& r);
void register_comdlg32(ShimRegistry& r);
// Real-window routing over the USER32/COMDLG32 handlers (realui.hh); after them.
void register_user32_real(ShimRegistry& r);

inline void register_all_shims(ShimRegistry& r) {
  register_kernel32(r);
  register_user32(r);
  register_gdi32(r);
  register_winmm(r);
  register_msacm32(r);
  register_shell32(r);
  register_comdlg32(r);
  register_user32_real(r);
}

// A whole program's (pe32/program.cc, ABI.md §2.15), after register_all_shims:
// the emulated registry (advapi32.cc), the window manager a program needs
// (user32_program.cc: in program mode it replaces some of USER32's handlers,
// which it hands the call to otherwise), WINMM's mmio and sndPlaySoundA, and
// KERNEL32's additions, over the signature rows only a program has
// (program_signatures()). Never registered for a module, so
// ADVAPI32.DLL stays "not found" for After Dark's LoadLibrary, as before.
void register_advapi32(ShimRegistry& r);
void register_user32_program(ShimRegistry& r);
// WINMM's multimedia file I/O and sndPlaySoundA (mmio.cc).
void register_mmio(ShimRegistry& r);
// KERNEL32's atoms, strings, paths and the rest MFC's startup calls (kernel32.cc).
void register_kernel32_program(ShimRegistry& r);
inline void register_program_shims(ShimRegistry& r) {
  r.add_signatures(program_signatures());
  register_kernel32_program(r);
  register_advapi32(r);
  register_user32_program(r);
  register_mmio(r);
}

// ---- host-facing helpers ----

// USER32: the saver window every module draws in (AD_MODULE32 +0x10), created
// on first use with the display's size as its client area.
uint32_t host_window(Runtime& rt);

// USER32: a class the guest registered (RegisterClassA/ExA), by name
// (case-insensitive). False when there is none.
struct GuestClassInfo {
  std::string name;
  uint32_t wndproc = 0, style = 0, background = 0;
  int32_t cls_extra = 0, wnd_extra = 0;
};
bool guest_class(Runtime& rt, std::string_view name, GuestClassInfo* out);
// USER32: a class-name argument (a string pointer, or an atom in the low word).
std::string guest_class_arg(Runtime& rt, uint32_t p);

// USER32: the module posted (or sent) WM_CLOSE / WM_SYSCOMMAND(SC_CLOSE) to
// the saver window (INTERACTION.md §5.1: ADWS_WAKE).
bool saver_wake_requested(Runtime& rt);

// USER32: PostMessageA from the host (MCI and waveOut notifications): the
// message joins the thread's queue with the time of the post. False for a
// window that does not exist (HWND_BROADCAST is accepted and dropped).
bool post_guest_message(Runtime& rt, uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);

// USER32: wsprintfA's formatter over a guest format string and a guest
// argument list (dwords at `args`) — for any shim that formats like USER does.
std::string guest_wsprintf(Runtime& rt, const std::string& fmt, uint32_t args);

// KERNEL32: the date and time the guest sees, as a FILETIME (100 ns since
// 1601; the guest's UTC is the host's local time, bias 0). Headless runs
// (fixed-step clock) start at a fixed moment so they stay reproducible;
// streamed runs start at the host's local time. Each call is a clock read.
uint64_t guest_local_filetime(Runtime& rt);

// KERNEL32: the process's profile store (ini_store.hh) over rt.vfs(), with
// WIN.INI's [Berkeley Systems] seeds; what Get/WritePrivateProfileStringA use.
IniStore& profile_store(Runtime& rt);
// KERNEL32: closes every file the guest left open (each commits to its
// overlay's upper layer), as process exit would.
void close_guest_files(Runtime& rt);

// USER32: a whole program (pe32/program.cc, ABI.md §2.15) owns its windows and
// its message loop: windows get the messages Windows 95 sent them
// (user32_program.cc), GetMessageA/WaitMessage with nothing to deliver wait
// in Runtime::yield_frame, and the screen is the desktop the program's
// windows lie on. Off (every module): nothing changes.
void user32_set_program(Runtime& rt, bool on);
bool user32_program(Runtime& rt);
// USER32: the last MessageBoxA's caption and text ("" when none was shown).
std::string user32_last_message_box(Runtime& rt);
// USER32: a program's input while the lane lets it play (pe32/program.cc,
// Caps Lock): a KEY line (vk, down) and a MOUSE line (screen coordinates,
// the MOUSE button bitmask) become the messages Windows 95's input queue
// held — WM_KEYDOWN/WM_KEYUP (WM_SYSKEY* with Alt or F10) to the focus
// window, WM_MOUSEMOVE and the button messages (double clicks for a
// CS_DBLCLKS class) to the capture window or the window under the pointer,
// in client coordinates —, read after posted messages; the mouse moves the
// emulated pointer. Queued only: no guest code runs. Nothing in program
// mode queues input otherwise (ghost mode).
void user32_program_key(Runtime& rt, uint32_t vk, bool down);
void user32_program_mouse(Runtime& rt, int32_t x, int32_t y, uint32_t buttons);
// USER32: whether the program's cursor is on (a cursor set, ShowCursor's count
// not below 0).
bool user32_cursor_shown(Runtime& rt);

// ADVAPI32: the emulated registry (advapi32.cc). A REG_SZ value the
// program's installer wrote ("HKEY_LOCAL_MACHINE\SOFTWARE\…", name, value);
// the file (a guest path) the guest's writes go to, REGEDIT4 text read back
// over the seeds now; a value as it stands (false when absent; REG_SZ
// without its NUL).
void registry_seed(Runtime& rt, std::string_view key_path, std::string_view name, std::string_view value);
void registry_attach_file(Runtime& rt, std::string_view guest_path);
bool registry_get(Runtime& rt, std::string_view key_path, std::string_view name, std::string* value);

// KERNEL32: a resource of a loaded image, preferring US English, then
// neutral, then any language (ABI.md §2.10.1). Null when absent.
const loader::pe::Resource* find_module_resource(Runtime& rt, uint32_t hmod, const loader::ResId& type,
                                                 const loader::ResId& name);

}  // namespace adw::win32
