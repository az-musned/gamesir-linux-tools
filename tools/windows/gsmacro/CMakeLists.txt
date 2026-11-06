cmake_minimum_required(VERSION 3.16)
project(gsmacro CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(GSM_BUILD_GUI "Build the GUI application" ON)
option(GSM_BUILD_TESTS "Build the test runner" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra)
endif()
if(MINGW)
  # Self-contained exe: no libstdc++/winpthread DLLs to ship.
  add_link_options(-static -static-libgcc -static-libstdc++)
  add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

find_package(Threads REQUIRED)

# ── portable core: pipeline, scheduler, config, mock/demo backends ──
add_library(gsm_core STATIC
  core/gamepad.cpp
  core/macro.cpp
  core/config.cpp
  core/scheduler.cpp
  core/processor.cpp
  core/engine.cpp
  core/edge_timeline.cpp
  core/dualsense.cpp
  core/merged_source.cpp
  backends/demo_scenarios.cpp)
target_include_directories(gsm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gsm_core PUBLIC Threads::Threads)

# ── Windows backends (XInput input, ViGEm output, HidHide status, waiter) ──
if(WIN32)
  add_library(gsm_win STATIC
    backends/win/xinput_source.cpp
    backends/win/xinput_util.cpp
    backends/win/dualsense_source.cpp
    backends/win/vigem_sink.cpp
    backends/win/hidhide_status.cpp
    backends/win/win_waiter.cpp
    backends/win/win_platform.cpp)
  target_link_libraries(gsm_win PUBLIC gsm_core advapi32 avrt shell32 ole32 comdlg32 cfgmgr32 hid setupapi)
  target_compile_definitions(gsm_win PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ── tests ──
if(GSM_BUILD_TESTS)
  add_executable(gsm_tests tests/test_main.cpp tests/test_core.cpp tests/test_engine.cpp tests/test_edge_cases.cpp tests/test_edge_timeline.cpp tests/test_dualsense.cpp tests/test_merged.cpp)
  target_link_libraries(gsm_tests PRIVATE gsm_core)
  target_compile_definitions(gsm_tests PRIVATE GSM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  if(WIN32)
    target_link_libraries(gsm_tests PRIVATE winmm)  # timeBeginPeriod for the real-clock tests
  endif()
  enable_testing()
  add_test(NAME gsm_tests COMMAND gsm_tests)
endif()

# ── headless engine timing check (mock backends) ──
add_executable(gsm_bench tools/bench_main.cpp)
if(WIN32)
  target_link_libraries(gsm_bench PRIVATE gsm_win winmm)
else()
  target_link_libraries(gsm_bench PRIVATE gsm_core)
endif()

# ── GUI ──
if(GSM_BUILD_GUI)
  set(IMGUI ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
  set(IMGUI_CORE ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp)
  if(WIN32)
    add_executable(gsmacro WIN32 app/main_win32.cpp app/gui.cpp ${IMGUI_CORE}
      ${IMGUI}/backends/imgui_impl_win32.cpp ${IMGUI}/backends/imgui_impl_dx11.cpp)
    target_include_directories(gsmacro PRIVATE ${IMGUI} ${IMGUI}/backends)
    target_link_libraries(gsmacro PRIVATE gsm_win d3d11 dxgi d3dcompiler dwmapi)
    if(MINGW)
      target_link_options(gsmacro PRIVATE -municode)
    endif()
  else()
    find_package(glfw3 QUIET)
    find_package(OpenGL QUIET)
    if(glfw3_FOUND AND OpenGL_FOUND)
      add_executable(gsmacro app/main_glfw.cpp app/gui.cpp app/platform_posix.cpp ${IMGUI_CORE}
        ${IMGUI}/backends/imgui_impl_glfw.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp)
      target_include_directories(gsmacro PRIVATE ${IMGUI} ${IMGUI}/backends)
      target_link_libraries(gsmacro PRIVATE gsm_core glfw OpenGL::GL ${CMAKE_DL_LIBS})
    else()
      message(STATUS "GLFW/OpenGL not found: GUI not built on this host")
    endif()
  endif()
endif()
