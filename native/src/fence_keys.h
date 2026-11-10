#pragma once

#include <Windows.h>

// Install/Remove are no-ops: fencing shortcuts ride the single App LL keyboard
// hook (typing latency — three WH_KEYBOARD_LL hooks was too expensive).
void InstallFenceKeyHook();
void RemoveFenceKeyHook();

// Called from App::LowLevelKeyboardProc. Returns true when the key was
// swallowed (PostMessage to a fence).
bool TryHandleFenceLlKey(WPARAM wparam, LPARAM lparam);
