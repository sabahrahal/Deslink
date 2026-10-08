cmake_minimum_required(VERSION 3.16)
project(desklink_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "desklink-host sólo se compila en Windows (DXGI, Media Foundation).")
endif()

# Runtime de C++ enlazado de forma estática: el .exe no necesita instalar el
# paquete redistribuible de Visual C++ en el PC.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_subdirectory(../../protocol protocol)

add_executable(desklink-host
  src/main.cpp
  src/gpu.cpp
  src/capture.cpp
  src/converter.cpp
  src/encoder.cpp
  src/touch.cpp
  src/touch_gestures.cpp
  src/udp.cpp
  src/usb.cpp
  src/virtual_display.cpp)

target_compile_definitions(desklink-host PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(desklink-host PRIVATE dlp
  d3d11 dxgi mfplat mf mfuuid strmiids ole32 oleaut32 ws2_32 iphlpapi winmm user32 cfgmgr32)

if(MSVC)
  target_compile_options(desklink-host PRIVATE /W3 /utf-8)
endif()

# Pruebas de lo que no depende de Windows (gestos del modo ratón).
enable_testing()
add_executable(test_touch_gestures tests/test_touch_gestures.cpp src/touch_gestures.cpp)
if(MSVC)
  target_compile_options(test_touch_gestures PRIVATE /W3 /utf-8)
endif()
add_test(NAME touch_gestures COMMAND test_touch_gestures)
