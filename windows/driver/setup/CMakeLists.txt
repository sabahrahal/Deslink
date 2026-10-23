cmake_minimum_required(VERSION 3.16)
project(extendesk_driver_setup CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(extendesk-driver setup.cpp)
target_compile_definitions(extendesk-driver PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(extendesk-driver PRIVATE setupapi newdev crypt32 shell32 cfgmgr32)
if(MSVC)
  target_compile_options(extendesk-driver PRIVATE /W3 /utf-8)
  # Pide permisos de administrador al abrirlo (UAC).
  target_link_options(extendesk-driver PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()
