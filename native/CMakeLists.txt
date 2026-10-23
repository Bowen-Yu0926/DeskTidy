cmake_minimum_required(VERSION 3.20)
project(DeskTidyNative LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(desktidy_native WIN32
  src/main.cpp
  src/log.cpp
  src/app.cpp
  src/desktop_shell.cpp
  src/fence_window.cpp
  src/settings_window.cpp
  src/layout.cpp
  src/python_layout_sync.cpp
  src/desktop_items.cpp
  src/namespace_icons.cpp
  src/brand_mark.cpp
  src/pin_store.cpp
  src/rule_store.cpp
  src/rules_editor.cpp
  src/ole_drag.cpp
  src/organize.cpp
  src/dock_window.cpp
  src/pet_window.cpp
  src/desknote_launch.cpp
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
  src/hotkey_helper.cpp
  src/hotkey_relay.cpp
  src/hotkey_store.cpp
  src/fence_style.cpp
  src/whitelist.cpp
  src/snapshot_ui.cpp
  src/mist_webview.cpp
  src/display_geom.cpp
  src/minutes.cpp
  src/i18n.cpp
  src/vault_http.cpp
  src/vault_crypto.cpp
  src/shell_icons.cpp
  src/page_folders.cpp
  src/organize_suppress.cpp
  src/app_prefs.cpp
  src/ui_theme.cpp
  src/desktop_dblclick.cpp
  src/vault_launcher.cpp
  src/screen_record.cpp
  src/ffmpeg_fetch.cpp
  src/fence_region_overlay.cpp
  src/update_check.cpp
  src/shell_clipboard.cpp
  src/icon_actions.cpp
  src/shell_context_menu.cpp
  src/toast.cpp
  src/help_center.cpp
  src/native_cleanup.cpp
  src/fence_keys.cpp
  src/layout_editor.cpp
  src/pet_ledges.cpp
  src/pet_lines.cpp
  src/cli_ipc.cpp
  src/file_assoc.cpp
  src/version.rc
)

# Standalone DeskNote (sibling of desktidy_native.exe).
add_executable(deskNote WIN32
  src/desknote_main.cpp
  src/desknote_launch.cpp
  src/elevated_save.cpp
  src/file_assoc.cpp
  src/notes_window.cpp
  src/notes_webview.cpp
  src/notes_library.cpp
  src/markdown_preview.cpp
  src/desknote_help.cpp
  src/mist_webview.cpp
  src/paths_util.cpp
  src/prompt_text.cpp
  src/i18n.cpp
  src/app_prefs.cpp
  src/ui_theme.cpp
  src/tool_paths.cpp
  src/desktop_shell.cpp
  src/display_geom.cpp
  src/log.cpp
  src/version.rc
)

target_include_directories(deskNote PRIVATE
  src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2-include
)
target_compile_definitions(deskNote PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(deskNote PRIVATE
  user32 gdi32 shell32 ole32 oleaut32 uuid comctl32 advapi32
  gdiplus comdlg32 uxtheme version propsys
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2-x64/WebView2LoaderStatic.lib
)
set_target_properties(deskNote PROPERTIES OUTPUT_NAME "deskNote")
if(MSVC)
  target_compile_options(deskNote PRIVATE /W4 /permissive- /utf-8)
endif()

# Ensure deskNote.exe lands next to desktidy_native.exe.
add_custom_command(TARGET deskNote POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:deskNote>
          $<TARGET_FILE_DIR:desktidy_native>/deskNote.exe
  COMMENT "Copy deskNote.exe next to desktidy_native"
)

target_include_directories(desktidy_native PRIVATE
  src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2-include
)
target_compile_definitions(desktidy_native PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(desktidy_native PRIVATE
  user32 gdi32 shell32 ole32 oleaut32 uuid comctl32 advapi32
  gdiplus comdlg32 crypt32 winhttp bcrypt msimg32 version propsys
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2-x64/WebView2LoaderStatic.lib
)

add_custom_command(TARGET desktidy_native POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/assets/pets
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/assets/settings
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/assets/vault
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/assets/overlays
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/docs
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/wysiwyg_native.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/wysiwyg_native.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/toastui-editor-all.min.js
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/toastui-editor-all.min.js
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/toastui-editor.min.css
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/toastui-editor.min.css
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/viewer_native.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/viewer_native.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/bridge_native.js
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/bridge_native.js
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/theme-light.css
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/theme-light.css
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/../assets/md_preview/vendor
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/settings/settings.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/settings/settings.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/vault/vault.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/vault/vault.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/whitelist.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/whitelist.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/rules.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/rules.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/snapshot.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/snapshot.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/prompt.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/prompt.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/pick_list.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/pick_list.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/page_folders.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/page_folders.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/record_cfg.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/record_cfg.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/help.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/help.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/notes_find.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/notes_find.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/desknote_help_zh.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/desknote_help_zh.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/desknote_help_en.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/desknote_help_en.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/help_manual_chapters.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/help_manual_chapters.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/../docs/desktidy.html
          $<TARGET_FILE_DIR:desktidy_native>/docs/desktidy.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/../docs/desknote.html
          $<TARGET_FILE_DIR:desktidy_native>/docs/desknote.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/overlays/search.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/overlays/search.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/overlays/todo.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/overlays/todo.html
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/pets
          $<TARGET_FILE_DIR:desktidy_native>/assets/pets
)

if(MSVC)
  target_compile_options(desktidy_native PRIVATE /W4 /permissive- /utf-8)
endif()
