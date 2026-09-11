"""Shimeji-style window ledges for desktop pet perching (Win32 EnumWindows)."""

from __future__ import annotations

from dataclasses import dataclass

from PyQt6.QtCore import QRect


@dataclass(frozen=True)
class WindowLedge:
    """Top edge of a visible top-level window the pet can sit on."""

    x: int
    y: int
    width: int
    hwnd: int


def list_window_ledges(
    *,
    work: QRect,
    exclude_hwnds: set[int] | None = None,
    min_width: int = 220,
    max_results: int = 24,
) -> list[WindowLedge]:
    """Return sit-able top edges inside the work area (market Shimeji approach)."""
    exclude = set(exclude_hwnds or ())
    try:
        import ctypes
        from ctypes import wintypes
    except ImportError:
        return []

    user32 = ctypes.windll.user32
    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    GW_OWNER = 4
    GWL_EXSTYLE = -20
    WS_EX_TOOLWINDOW = 0x00000080
    WS_EX_NOACTIVATE = 0x08000000
    found: list[WindowLedge] = []

    def _cb(hwnd: int, _lparam: int) -> bool:
        if len(found) >= max_results:
            return False
        hwnd_i = int(hwnd)
        if hwnd_i in exclude:
            return True
        if not user32.IsWindowVisible(hwnd):
            return True
        if user32.GetWindow(hwnd, GW_OWNER):
            return True
        try:
            ex = int(user32.GetWindowLongW(hwnd, GWL_EXSTYLE) or 0)
        except Exception:
            ex = 0
        if ex & WS_EX_TOOLWINDOW:
            return True
        if ex & WS_EX_NOACTIVATE:
            return True
        length = int(user32.GetWindowTextLengthW(hwnd) or 0)
        if length <= 0:
            return True
        rect = wintypes.RECT()
        if not user32.GetWindowRect(hwnd, ctypes.byref(rect)):
            return True
        left, top, right, bottom = int(rect.left), int(rect.top), int(rect.right), int(rect.bottom)
        w = right - left
        h = bottom - top
        if w < min_width or h < 80:
            return True
        # Skip near-fullscreen covers of the work area
        if w >= work.width() * 0.96 and h >= work.height() * 0.92:
            return True
        # Ledge must sit inside work band (not above monitor)
        if top < work.y() + 8 or top > work.y() + work.height() - 120:
            return True
        if right < work.x() + 40 or left > work.x() + work.width() - 40:
            return True
        sit_x = max(work.x() + 8, left + 12)
        sit_w = min(right - 12, work.x() + work.width() - 8) - sit_x
        if sit_w < 80:
            return True
        found.append(WindowLedge(x=sit_x, y=top, width=sit_w, hwnd=hwnd_i))
        return True

    user32.EnumWindows(EnumWindowsProc(_cb), 0)
    # Prefer lower (closer to taskbar) ledges first for natural sitting
    found.sort(key=lambda L: (-L.y, -L.width))
    return found
