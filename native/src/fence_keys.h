#pragma once

#include <Windows.h>

// Install/Remove are no-ops: fencing shortcuts ride the single App LL keyboard
// hook (typing latency — three WH_KEYBOARD_LL hooks was too expensive).
void InstallFenceKeyHook();
void RemoveFenceKeyHook();

// Called from App::LowLevelKeyboardProc. Returns true when the key was
// swallowed (PostMessage to a fence / public host).
bool TryHandleFenceLlKey(WPARAM wparam, LPARAM lparam);

// Fences: last-clicked fence/public keeps keyboard chords (Ctrl+C/V, Del, …)
// until blank desktop click — even if the cursor has moved off the window
// (MA_NOACTIVATE means the fence never takes Win32 focus).
void SetFenceKeyTarget(HWND hwnd);
void ClearFenceKeyTarget();
HWND GetFenceKeyTarget();
