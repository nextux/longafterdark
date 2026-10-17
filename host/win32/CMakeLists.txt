# adw_win32 — the Win32 guest runtime (address space, TEB/PEB, thunks, SEH,
# PE module table, call_guest) plus the API shims, one file per DLL family,
# and the host-level display model (display.hh) that the Win16 shims reuse.
# See host/win32/README.md for the layout and the extension recipe.

add_library(adw_win32 STATIC
  runtime.cc
  heap.cc
  shims.cc
  signatures.cc
  seh.cc
  modules.cc
  display.cc
  gdi_objects.cc
  vfs.cc
  ini_store.cc
  config_script.cc
  realui.cc
  # The host audio engine as the shims see it (audio.hh; AUDIO.md §7).
  audio.cc
  # API families (register_<dll>(ShimRegistry&)); see shim_families.hh.
  kernel32.cc
  user32.cc
  gdi32.cc
  winmm.cc
  msacm32.cc
  dsound.cc
  shell32.cc
  comdlg32.cc
  advapi32.cc
  user32_program.cc
  mmio.cc
  user32_real.cc
)
# Consumers include "win32/runtime.hh" etc.
target_include_directories(adw_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(adw_win32 PUBLIC adw_core adw_cpu adw_loader gdi32 user32 comdlg32 dwmapi)
target_compile_features(adw_win32 PUBLIC cxx_std_20)

# ---- tests ------------------------------------------------------------------
add_executable(adw_win32_tests tests/test_win32.cc tests/test_vfs.cc tests/test_interaction.cc tests/test_audio.cc
               tests/test_program.cc)
target_include_directories(adw_win32_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests)
target_link_libraries(adw_win32_tests PRIVATE adw_win32)
add_test(NAME win32.unit COMMAND adw_win32_tests)
