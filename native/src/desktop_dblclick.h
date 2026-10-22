#pragma once

#include <Windows.h>

// Low-level mouse hook: notify when the user double-clicks blank desktop
// (Progman / DefView / SysListView32), excluding DeskTidy native overlays.
void StartDesktopDblClickMonitor(HWND notify_hwnd, UINT notify_msg);
void StopDesktopDblClickMonitor();
bool IsDesktopBlankPoint(int screen_x, int screen_y);

// Last desktop-background right-click (screen coords), recorded by the same
// low-level mouse hook. Used by "new-fence" to place a fence at the click.
// Returns false when nothing has been recorded yet (caller falls back to the
// current cursor position).
bool GetLastDesktopRightClick(POINT* out);
