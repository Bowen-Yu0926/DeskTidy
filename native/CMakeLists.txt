cmake_minimum_required(VERSION 3.20)
project(DeskTidyNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(desktidy_native WIN32
  src/main.cpp
  src/app.cpp
  src/desktop_shell.cpp
  src/fence_window.cpp
  src/settings_window.cpp
  src/layout.cpp
)

target_include_directories(desktidy_native PRIVATE src)
target_compile_definitions(desktidy_native PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(desktidy_native PRIVATE user32 gdi32 shell32 ole32 comctl32)

if(MSVC)
  target_compile_options(desktidy_native PRIVATE /W4 /permissive- /utf-8)
endif()
