#pragma once

#include <Windows.h>

// DeskTidy brand mark (stacked panes) as HICON — same glyph as Python tray.
// Caller DestroyIcon when done. size typically 16 or 32 for the notification area.
HICON CreateBrandMarkIcon(int size = 32, COLORREF accent = RGB(59, 130, 246));

// Apply shared 16/32 brand icons so the Windows taskbar shows the mark instead
// of a live window thumbnail (blue slab + gray lines).
void ApplyBrandWindowIcons(HWND hwnd);
