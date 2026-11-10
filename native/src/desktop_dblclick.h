#pragma once

#include <Windows.h>

// Low-level mouse hook: blank-desktop clicks (Progman / DefView / …), excluding
// DeskTidy native overlays.
// notify_msg      — WM_LBUTTONDBLCLK on blank desktop
// notify_down_msg — WM_LBUTTONDOWN on blank desktop (0 = ignore); used to clear
//                   fence icon selection like Explorer / Fences.
// notify_paste_msg — Ctrl+V while foreground is the real desktop shell
//                   (Progman/DefView/…); 0 = ignore. Used when Explorer icons
//                   are hidden so DefView no longer handles file paste.
void StartDesktopDblClickMonitor(HWND notify_hwnd, UINT notify_msg,
                                 UINT notify_down_msg = 0,
                                 UINT notify_paste_msg = 0);
void StopDesktopDblClickMonitor();
bool IsDesktopBlankPoint(int screen_x, int screen_y);

// Ctrl+V on real desktop shell — called from App's single LL keyboard hook.
// Returns true when the key was swallowed.
bool TryHandleDesktopPasteLlKey(WPARAM wparam, LPARAM lparam);

// Last desktop-background right-click (screen coords), recorded by the same
// low-level mouse hook. Used by "new-fence" to place a fence at the click.
// Returns false when nothing has been recorded yet (caller falls back to the
// current cursor position).
bool GetLastDesktopRightClick(POINT* out);
