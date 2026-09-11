"""Live: empty-plate RMB on owned DeskTidy host must open Explorer menu.

Requires a running DeskTidy session with hide_shell_icons (full public plate).
Finds a screen point where WindowFromPoint is the public host (not Excel/Chrome),
SendInput right-clicks it, asserts a shell popup appears, then Esc dismisses.
"""

from __future__ import annotations

import ctypes
import sys
import time
from ctypes import wintypes

import win32gui

sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parents[1]))

user32 = ctypes.windll.user32

INPUT_MOUSE = 0
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_VIRTUALDESK = 0x4000
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
VK_ESCAPE = 0x1B

_MENU_CLASSES = frozenset(
    {
        "#32768",
        "Xaml_WindowedPopupClass",
        "Microsoft.UI.Content.PopupWindowSiteBridge",
        "XamlExplorerHostIslandWindow",
        "DeskTidyShellMenuHost",
    }
)


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", ctypes.c_long),
        ("dy", ctypes.c_long),
        ("mouseData", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT)]

    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


def _dismiss_menus() -> None:
    for _ in range(5):
        user32.keybd_event(VK_ESCAPE, 0, 0, 0)
        user32.keybd_event(VK_ESCAPE, 0, 2, 0)
        time.sleep(0.06)


def _menu_hwnds() -> set[int]:
    found: set[int] = set()

    def _cb(hwnd, _):
        try:
            if user32.IsWindowVisible(hwnd) and win32gui.GetClassName(hwnd) in _MENU_CLASSES:
                found.add(int(hwnd))
        except OSError:
            pass
        return True

    win32gui.EnumWindows(_cb, None)
    return found


def _find_public_host() -> int:
    candidates: list[tuple[int, int]] = []

    def _cb(hwnd, _):
        try:
            if not user32.IsWindowVisible(hwnd):
                return True
            cls = win32gui.GetClassName(hwnd)
            if not cls.startswith("Qt"):
                return True
            title = win32gui.GetWindowText(hwnd)
            # Source runs use pythonw.exe → Qt title "pythonw"; frozen → "DeskTidy".
            if title not in ("DeskTidy", "python", "pythonw") and not title.startswith(
                "DeskTidy"
            ):
                return True
            left, top, right, bottom = win32gui.GetWindowRect(hwnd)
            w, h = right - left, bottom - top
            if w >= 1600 and h >= 700:
                candidates.append((w * h, int(hwnd)))
        except OSError:
            pass
        return True

    win32gui.EnumWindows(_cb, None)
    if not candidates:
        return 0
    candidates.sort(reverse=True)
    return candidates[0][1]


def _find_empty_host_point(host: int) -> tuple[int, int] | None:
    """Point where WindowFromPoint is *host* (uncovered by Excel/Chrome/…)."""
    left, top, right, bottom = win32gui.GetWindowRect(host)
    ranges: list[range] = []
    if left < 0:
        ranges.append(range(left + 40, min(-40, right - 40), 80))
    if right > 40:
        ranges.append(range(max(0, left + 40), min(right - 40, 1800), 80))
    for xs in ranges:
        for y in range(max(top + 60, 60), min(bottom - 60, top + 1000), 50):
            for x in xs:
                pt = POINT(int(x), int(y))
                hwnd = int(user32.WindowFromPoint(pt) or 0)
                if hwnd == host:
                    return int(x), int(y)
    return None


def _send_rmb(x: int, y: int) -> None:
    user32.SetCursorPos(int(x), int(y))
    time.sleep(0.05)
    user32.mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0)
    time.sleep(0.08)
    user32.mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0)


def main() -> int:
    print("DeskTidy live empty-plate RMB")
    host = _find_public_host()
    if not host:
        print("FAIL  no DeskTidy public host HWND (is DeskTidy running?)")
        return 1
    point = _find_empty_host_point(host)
    if point is None:
        print("FAIL  no uncovered empty point on public host (other apps cover it?)")
        return 1
    x, y = point
    print(f"  host={hex(host)} point=({x},{y})")
    _dismiss_menus()
    time.sleep(0.25)
    before = _menu_hwnds()
    _send_rmb(x, y)
    opened_at: float | None = None
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 2.5:
        if _menu_hwnds() - before:
            opened_at = time.perf_counter() - t0
            break
        time.sleep(0.05)
    _dismiss_menus()
    if opened_at is None:
        print("FAIL  empty-plate RMB did not open Explorer menu")
        return 1
    print(f"OK  menu opened in {opened_at:.2f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
