cmake_minimum_required(VERSION 3.24)

project(RC-ScreenShot VERSION 0.5.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(RC_ENABLE_ULTRAHDR "Build the Ultra HDR JPEG encoder" ON)
option(RC_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)
if(RC_ENABLE_ULTRAHDR)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_DEPS ON CACHE BOOL "" FORCE)
  set(UHDR_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(UHDR_ENABLE_GLES OFF CACHE BOOL "" FORCE)
  set(UHDR_ENABLE_HEIF OFF CACHE BOOL "" FORCE)
  set(UHDR_WRITE_XMP ON CACHE BOOL "" FORCE)
  set(UHDR_WRITE_ISO ON CACHE BOOL "" FORCE)
  FetchContent_Declare(
    libultrahdr
    GIT_REPOSITORY https://github.com/google/libultrahdr.git
    GIT_TAG b2aacb366e1542cfc29605cb0d8a0ebd06bb07f8
    GIT_SHALLOW FALSE
  )
  FetchContent_MakeAvailable(libultrahdr)
endif()

set(RC_SOURCES
  src/app.cpp
  src/capture.cpp
  src/config.cpp
  src/editor.cpp
  src/exporter.cpp
  src/main.cpp
  src/overlay.cpp
  src/unit_detector.cpp
  src/uia_detector.cpp
  src/resources.rc
)

add_executable(RC-ScreenShot WIN32 ${RC_SOURCES})
target_include_directories(RC-ScreenShot PRIVATE src)
target_compile_definitions(RC-ScreenShot PRIVATE
  UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0A00 NTDDI_VERSION=NTDDI_WIN10_19H1
  $<$<BOOL:${RC_ENABLE_ULTRAHDR}>:RC_ENABLE_ULTRAHDR=1>
)
target_compile_options(RC-ScreenShot PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
target_link_options(RC-ScreenShot PRIVATE /MANIFEST:NO)
target_link_libraries(RC-ScreenShot PRIVATE
  d3d11 dxgi d2d1 dwrite windowscodecs dwmapi shlwapi shell32 ole32 oleaut32 uuid comctl32 gdi32 msimg32 user32 advapi32 comdlg32 imm32 uiautomationcore
)
if(RC_ENABLE_ULTRAHDR)
  target_include_directories(RC-ScreenShot PRIVATE ${libultrahdr_SOURCE_DIR})
  if(TARGET uhdr-static)
    target_link_libraries(RC-ScreenShot PRIVATE uhdr-static)
  else()
    target_link_libraries(RC-ScreenShot PRIVATE uhdr)
  endif()
endif()

if(RC_BUILD_TESTS)
  enable_testing()
  add_executable(rc_core_tests
    tests/core_tests.cpp
    src/capture.cpp
    src/config.cpp
    src/editor.cpp
    src/exporter.cpp
    src/unit_detector.cpp
    src/uia_detector.cpp
  )
  target_include_directories(rc_core_tests PRIVATE src)
  target_compile_definitions(rc_core_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(rc_core_tests PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
  target_link_libraries(rc_core_tests PRIVATE
    d3d11 dxgi d2d1 dwrite windowscodecs dwmapi shlwapi shell32 ole32 uuid gdi32 user32 imm32 uiautomationcore
  )
  add_test(NAME rc_core_tests COMMAND rc_core_tests)

  add_executable(rc_capture_smoke EXCLUDE_FROM_ALL tests/capture_smoke.cpp src/capture.cpp)
  target_include_directories(rc_capture_smoke PRIVATE src)
  target_compile_definitions(rc_capture_smoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(rc_capture_smoke PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
  target_link_libraries(rc_capture_smoke PRIVATE d3d11 dxgi dwmapi ole32 gdi32 user32 imm32)

  add_executable(rc_overlay_smoke WIN32 EXCLUDE_FROM_ALL
    tests/overlay_smoke.cpp src/capture.cpp src/config.cpp src/editor.cpp src/overlay.cpp src/unit_detector.cpp src/uia_detector.cpp
  )
  target_include_directories(rc_overlay_smoke PRIVATE src)
  target_compile_definitions(rc_overlay_smoke PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_options(rc_overlay_smoke PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
  target_link_libraries(rc_overlay_smoke PRIVATE
    d3d11 dxgi d2d1 dwrite windowscodecs dwmapi shlwapi ole32 uuid gdi32 user32 comdlg32 comctl32 imm32 uiautomationcore
  )
endif()

install(TARGETS RC-ScreenShot RUNTIME DESTINATION .)
