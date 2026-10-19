cmake_minimum_required(VERSION 3.20)
project(DeskTidyNative LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(desktidy_native WIN32
  src/main.cpp
  src/app.cpp
  src/desktop_shell.cpp
  src/fence_window.cpp
  src/settings_window.cpp
  src/layout.cpp
  src/desktop_items.cpp
  src/pin_store.cpp
  src/rule_store.cpp
  src/ole_drag.cpp
  src/organize.cpp
  src/dock_window.cpp
  src/pet_window.cpp
  src/notes_window.cpp
  src/page_bar.cpp
  src/desktop_watch.cpp
  src/prompt_text.cpp
  src/autostart.cpp
  src/instance_lock.cpp
  src/paths_util.cpp
  src/snapshot.cpp
  src/shell_verbs.cpp
  src/public_host.cpp
  src/todo_window.cpp
  src/search_window.cpp
  src/vault_window.cpp
  src/tools_misc.cpp
  src/tool_paths.cpp
  src/capture_overlay.cpp
  src/pinned_image.cpp
  src/first_run.cpp
  src/hotkey_help.cpp
  src/hotkey_store.cpp
  src/fence_style.cpp
  src/whitelist.cpp
  src/snapshot_ui.cpp
  src/display_geom.cpp
  src/minutes.cpp
  src/version.rc
)

target_include_directories(desktidy_native PRIVATE src)
target_compile_definitions(desktidy_native PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(desktidy_native PRIVATE
  user32 gdi32 shell32 ole32 oleaut32 uuid comctl32 advapi32
  gdiplus comdlg32 crypt32
)

if(MSVC)
  target_compile_options(desktidy_native PRIVATE /W4 /permissive- /utf-8)
endif()
