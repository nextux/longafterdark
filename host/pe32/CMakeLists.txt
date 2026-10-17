# adw_lane_pe32 — the AD4 lane: an After Dark 4 module (PE32 .AD) with the real
# ADXPL510.DLL engine on the Win32 guest runtime (host/win32), driven as
# AFTERDAR.SCR drove it (docs/ABI.md §2). See lane.hh.

add_library(adw_lane_pe32 STATIC
  lane.cc
  program.cc
  controls.cc
  package.cc
)
target_include_directories(adw_lane_pe32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(adw_lane_pe32 PUBLIC adw_win32 adw_core)
target_compile_features(adw_lane_pe32 PUBLIC cxx_std_20)

# adhostwin links whatever library defines adw::make_pe32_lane() (core/README.md).
set_property(GLOBAL PROPERTY ADW_LANE_PE32_TARGET adw_lane_pe32)

# ---- tests ------------------------------------------------------------------
add_executable(adw_pe32_tests tests/test_pe32.cc)
target_include_directories(adw_pe32_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests)
target_link_libraries(adw_pe32_tests PRIVATE adw_lane_pe32)
add_test(NAME pe32.unit COMMAND adw_pe32_tests)
# Runs real modules from the imported assets; skips (77) when they are absent.
add_test(NAME pe32.assets COMMAND adw_pe32_tests --assets $<TARGET_FILE:adhostwin>)
set_tests_properties(pe32.assets PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
# Runs the pe32 modules of an imported After Dark 10th Anniversary package
# (win/packages/ad10 under AD_PE32_PKG_ROOT, else the installed assets);
# skips (77) when there is none.
add_test(NAME pe32.packages COMMAND adw_pe32_tests --packages $<TARGET_FILE:adhostwin>)
set_tests_properties(pe32.packages PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1200)
