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

# Opus (sonido del PC en la tablet), compilado desde el código fuente y enlazado de forma estática.
include(FetchContent)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
  URL https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz
  URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1)
FetchContent_MakeAvailable(opus)

add_executable(desklink-host
  src/main.cpp
  src/audio.cpp
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
target_link_libraries(desklink-host PRIVATE dlp opus
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
