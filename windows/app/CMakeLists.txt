cmake_minimum_required(VERSION 3.16)
project(desklink_app CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "desklink (app de bandeja) sólo se compila en Windows.")
endif()

# Igual que el host: runtime estático, sin redistribuible de Visual C++.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(desklink WIN32
  src/main.cpp
  src/host_process.cpp
  src/settings.cpp
  src/system_info.cpp
  res/desklink.rc)

target_compile_definitions(desklink PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(desklink PRIVATE gdiplus dwmapi iphlpapi dxgi cfgmgr32 shell32 ole32 ws2_32 user32 gdi32 advapi32)

if(MSVC)
  target_compile_options(desklink PRIVATE /W3 /utf-8)
  # El manifiesto va en desklink.rc; sin esto el enlazador añade otro y choca.
  target_link_options(desklink PRIVATE /MANIFEST:NO)
endif()
