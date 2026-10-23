#pragma once

#include <string>

// Elevated sidecar (P1-28): owns WH_KEYBOARD_LL + bare F-key poll; relays
// matched actions to the non-elevated main app over 127.0.0.1:<port>.
// Invoked as: desktidy_native.exe --hotkey-helper --port N --mapping path
int RunHotkeyHelper(int port, const std::wstring& mapping_path);
int RunHotkeyHelperFromCmd(const std::wstring& cmd_line);
