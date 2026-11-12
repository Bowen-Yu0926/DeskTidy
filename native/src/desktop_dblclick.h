#pragma once

#include <Windows.h>

// Low-level mouse hook: blank-desktop clicks (Progman / DefView / …), excluding
// DeskTidy native overlays.
// notify_msg      — WM_LBUTTONDBLCLK on blank desktop
// notify_down_msg — WM_LBUTTONDOWN on blank desktop (0 = ignore); used to clear
//                   fence icon selection like Explorer / Fences.
// notify_paste_msg — Ctrl+V / Ctrl+Shift+V while foreground is the real
//                   desktop shell (Progman/DefView/…); 0 = ignore. wParam=1
//                   means pastelink. Used when Explorer icons are hidden so
//                   DefView no longer handles file paste.
// notify_blank_rclick_msg — organized-mode blank wallpaper RMB (0 = ignore);
//                   lParam = MAKELPARAM(x,y). Hook swallows DefView's menu.
void StartDesktopDblClickMonitor(HWND notify_hwnd, UINT notify_msg,
                                 UINT notify_down_msg = 0,
                                 UINT notify_paste_msg = 0,
                                 UINT notify_blank_rclick_msg = 0);
void StopDesktopDblClickMonitor();
// Organized (icons hidden): blank RMB → our Desktop background menu (Paste).
void SetDesktopOrganizedBlankMenu(bool organized);
bool IsDesktopBlankPoint(int screen_x, int screen_y);

// Ctrl+V on real desktop shell — called from App's single LL keyboard hook.
// Returns true when the key was swallowed.
bool TryHandleDesktopPasteLlKey(WPARAM wparam, LPARAM lparam);

// True when the foreground window is Progman/WorkerW/DefView (not Explorer
// folders, editors, or DeskTidy chrome that owns its own paste).
bool ForegroundIsDesktopShell();

// Last desktop-background right-click (screen coords), recorded by the same
// low-level mouse hook. Used by "new-fence" to place a fence at the click.
// Returns false when nothing has been recorded yet (caller falls back to the
// current cursor position). Optional *tick_ms receives GetTickCount64 of the
// click (DefView「新建」→ suppress watch-organize within a short window).
bool GetLastDesktopRightClick(POINT* out, ULONGLONG* tick_ms = nullptr);
