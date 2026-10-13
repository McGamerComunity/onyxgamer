cmake_minimum_required(VERSION 3.20)
project(onyx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "Onyx builds only on Windows (MSVC, Windows SDK 10.0.17134+).")
endif()

add_compile_definitions(NOMINMAX UNICODE _UNICODE)
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
endif()

add_executable(onyx_service src/service/main.cpp src/service/supervisor.cpp)
target_link_libraries(onyx_service PRIVATE wtsapi32 userenv advapi32 shell32)

add_executable(onyx_agent src/agent/main.cpp)
target_link_libraries(onyx_agent PRIVATE advapi32 shell32)

add_executable(sessionprobe tools/sessionprobe/main.cpp)
target_link_libraries(sessionprobe PRIVATE d3d11 dxgi wtsapi32 ole32 user32 windowsapp)
