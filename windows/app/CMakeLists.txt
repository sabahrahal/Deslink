cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Lo que enseña la ventana (view_model) y los argumentos del host no dependen de Windows: se prueban en
# cualquier sistema con `cmake -S windows/app -B build && ctest --test-dir build`.
if(NOT WIN32)
  project(desklink_app_tests CXX)
  enable_testing()
  add_executable(test_view_model tests/test_view_model.cpp src/view_model.cpp src/settings_args.cpp)
  add_test(NAME test_view_model COMMAND test_view_model)
  return()
endif()

project(desklink_app CXX RC)
enable_testing()
add_executable(test_view_model tests/test_view_model.cpp src/view_model.cpp src/settings_args.cpp)
target_compile_definitions(test_view_model PRIVATE NOMINMAX)
if(MSVC)
  target_compile_options(test_view_model PRIVATE /utf-8)
endif()
add_test(NAME test_view_model COMMAND test_view_model)

# Igual que el host: runtime estático, sin redistribuible de Visual C++.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(desklink WIN32
  src/main.cpp
  src/host_process.cpp
  src/settings.cpp
  src/settings_args.cpp
  src/system_info.cpp
  src/view_model.cpp
  res/desklink.rc)

target_compile_definitions(desklink PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(desklink PRIVATE gdiplus dwmapi iphlpapi dxgi cfgmgr32 shell32 ole32 ws2_32 user32 gdi32 advapi32)

if(MSVC)
  target_compile_options(desklink PRIVATE /W3 /utf-8)
  # El manifiesto va en desklink.rc; sin esto el enlazador añade otro y choca.
  target_link_options(desklink PRIVATE /MANIFEST:NO)
endif()
