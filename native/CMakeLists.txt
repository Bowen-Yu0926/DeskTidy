cmake_minimum_required(VERSION 3.20)
project(DeskTidyNative LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ARGON2_NO_THREADS: fill lanes sequentially; hash still matches Python
# argon2-cffi (parallelism=2 defines lanes, not a wire-format change).
set(ARGON2_SOURCES
  third_party/argon2/src/argon2.c
  third_party/argon2/src/core.c
  third_party/argon2/src/encoding.c
  third_party/argon2/src/ref.c
  third_party/argon2/src/blake2/blake2b.c
)

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
  src/desktop_item_identity.cpp
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
  src/mist_msg.cpp
  src/page_bar.cpp
  src/desktop_watch.cpp
  src/prompt_text.cpp
  src/mist_msg.cpp
  src/autostart.cpp
  src/instance_lock.cpp
  src/paths_util.cpp
  src/snapshot.cpp
  src/shell_verbs.cpp
  src/public_host.cpp
  src/float_placements.cpp
  src/todo_window.cpp
  src/search_window.cpp
  src/notes_cloud_sync.cpp
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
  src/screen_record.cpp
  src/ffmpeg_fetch.cpp
  src/fence_region_overlay.cpp
  src/update_check.cpp
  src/update_json.cpp
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
  src/docx_export.cpp
  src/version.rc
  src/desktidy_icon.rc
  ${ARGON2_SOURCES}
)

# Standalone DeskNote (sibling of desktidy_native.exe).
add_executable(deskNote WIN32
  src/desknote_main.cpp
  src/docx_export.cpp
  src/desknote_launch.cpp
  src/mist_msg.cpp
  src/elevated_save.cpp
  src/file_assoc.cpp
  src/notes_window.cpp
  src/notes_webview.cpp
  src/notes_library.cpp
  src/markdown_preview.cpp
  src/desknote_help.cpp
  src/mist_webview.cpp
  src/mist_msg.cpp
  src/ole_drag.cpp
  src/paths_util.cpp
  src/prompt_text.cpp
  src/i18n.cpp
  src/app_prefs.cpp
  src/ui_theme.cpp
  src/tool_paths.cpp
  src/desktop_shell.cpp
  src/display_geom.cpp
  src/vault_http.cpp
  src/vault_crypto.cpp
  src/online_notes_window.cpp
  src/notes_cloud_sync.cpp
  ${ARGON2_SOURCES}
  src/log.cpp
  src/version.rc
  src/toast.cpp
  src/desknote_icon.rc
)

target_include_directories(deskNote PRIVATE
  src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2-include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/src
)
target_compile_definitions(deskNote PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX ARGON2_NO_THREADS)
target_link_libraries(deskNote PRIVATE
  user32 gdi32 shell32 ole32 oleaut32 uuid comctl32 advapi32 crypt32
  gdiplus comdlg32 uxtheme version propsys winhttp bcrypt
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
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          $<TARGET_FILE:deskNote>
          $<TARGET_FILE_DIR:desktidy_native>/deskNote.exe
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/wysiwyg_native.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/wysiwyg_native.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/toastui-editor.min.js
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/toastui-editor.min.js
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/toastui-editor-all.min.js
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/toastui-editor-all.min.js
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/toastui-editor.min.css
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/toastui-editor.min.css
  # DeskNote WYSIWYG editor (Milkdown Crepe) — built by native/tools/build_crepe.js.
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/desknote-editor.js
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/desknote-editor.js
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/desknote-editor.css
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/desknote-editor.css
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/assets/online_notes
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/online_notes/online_notes.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/online_notes/online_notes.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/online_notes/notes_library.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/online_notes/notes_library.html
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/confirm.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/confirm.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/account_login.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/account_login.html
  COMMENT "Copy deskNote.exe + mist dialog assets next to desktidy_native"
)

target_include_directories(desktidy_native PRIVATE
  src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2-include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/src
)
target_compile_definitions(desktidy_native PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX ARGON2_NO_THREADS)
target_link_libraries(desktidy_native PRIVATE
  user32 gdi32 shell32 ole32 oleaut32 uuid comctl32 advapi32
  gdiplus comdlg32 crypt32 winhttp bcrypt msimg32 version propsys wtsapi32
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
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/assets/overlays
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/assets
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/../assets/desknote_icon.ico
          $<TARGET_FILE_DIR:desktidy_native>/assets/desknote_icon.ico
  COMMAND ${CMAKE_COMMAND} -E make_directory
          $<TARGET_FILE_DIR:desktidy_native>/docs
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/wysiwyg_native.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/wysiwyg_native.html
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/toastui-editor.min.js
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/toastui-editor.min.js
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/toastui-editor-all.min.js
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/toastui-editor-all.min.js
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/toastui-editor.min.css
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/toastui-editor.min.css
  # DeskNote WYSIWYG editor (Milkdown Crepe) — built by native/tools/build_crepe.js.
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/desknote-editor.js
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/desknote-editor.js
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/md_preview/vendor/desknote-editor.css
          $<TARGET_FILE_DIR:desktidy_native>/assets/md_preview/vendor/desknote-editor.css
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
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/confirm.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/confirm.html
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
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/record_done.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/record_done.html
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
          ${CMAKE_CURRENT_SOURCE_DIR}/assets/dialogs/account_login.html
          $<TARGET_FILE_DIR:desktidy_native>/assets/dialogs/account_login.html
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
  COMMAND ${CMAKE_COMMAND} -E touch
          $<TARGET_FILE_DIR:desktidy_native>/desktidy_dev.marker
)

if(MSVC)
  target_compile_options(desktidy_native PRIVATE /W4 /permissive- /utf-8)
endif()

# Headless geometry unit tests (no Win32 UI).
add_executable(test_layout_geom
  tests/test_layout_geom.cpp
  src/layout.cpp
  src/display_geom.cpp
  src/log.cpp
  src/paths_util.cpp
)
target_include_directories(test_layout_geom PRIVATE src)
target_compile_definitions(test_layout_geom PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(test_layout_geom PRIVATE
  user32 gdi32 shell32 ole32 advapi32)
if(MSVC)
  target_compile_options(test_layout_geom PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(test_desktop_item_identity
  tests/test_desktop_item_identity.cpp
  src/desktop_item_identity.cpp
)
target_include_directories(test_desktop_item_identity PRIVATE src)
target_compile_definitions(test_desktop_item_identity PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# Release-asset parsing (hand-rolled JSON scanner). No network: feeds the exact
# response shape GitHub returns, nested "uploader" object included.
add_executable(test_update_asset
  tests/test_update_asset.cpp
  src/update_json.cpp
)
target_include_directories(test_update_asset PRIVATE src)
target_compile_definitions(test_update_asset PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(test_update_asset PRIVATE user32)
if(MSVC)
  target_compile_options(test_update_asset PRIVATE /W4 /permissive- /utf-8)
endif()
if(MSVC)
  target_compile_options(test_desktop_item_identity PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(test_json_unescape
  tests/t_unesc.cpp
  src/paths_util.cpp
  src/log.cpp
)
target_include_directories(test_json_unescape PRIVATE src)
target_compile_definitions(test_json_unescape PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(test_json_unescape PRIVATE user32 shell32 ole32 advapi32)

add_executable(test_elevated_write
  tests/test_elevated_write.cpp
  src/elevated_save.cpp
  src/paths_util.cpp
  src/log.cpp
)
target_include_directories(test_elevated_write PRIVATE src)
target_compile_definitions(test_elevated_write PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(test_elevated_write PRIVATE user32 shell32 ole32 advapi32)
if(MSVC)
  target_compile_options(test_elevated_write PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(test_docx_export
  tests/test_docx_export.cpp
  src/docx_export.cpp
  src/paths_util.cpp
  src/log.cpp
)
target_include_directories(test_docx_export PRIVATE src)
target_compile_definitions(test_docx_export PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(test_docx_export PRIVATE user32 shell32 ole32 advapi32 gdiplus oleaut32)
if(MSVC)
  target_compile_options(test_docx_export PRIVATE /W4 /permissive- /utf-8)
endif()
