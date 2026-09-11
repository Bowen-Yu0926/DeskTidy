"""Windows Calculator launcher (system calc / Calculator app)."""

from __future__ import annotations

import logging
import os
import subprocess
from typing import Any

logger = logging.getLogger(__name__)

_CALC_TITLES = ("计算器", "Calculator")


def calculator_settings(settings: dict[str, Any] | None) -> dict[str, Any]:
    if settings is None:
        return {"enabled": False}
    raw = settings.get("calculator")
    if not isinstance(raw, dict):
        raw = {"enabled": False}
        settings["calculator"] = raw
    return raw


def calculator_enabled(settings: dict[str, Any] | None) -> bool:
    return bool(calculator_settings(settings).get("enabled", False))


def _enum_calculator_hwnds() -> list[int]:
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
    found: list[int] = []

    def _cb(hwnd: int, _lparam: int) -> bool:
        if not user32.IsWindowVisible(hwnd):
            return True
        length = int(user32.GetWindowTextLengthW(hwnd) or 0)
        if length <= 0:
            return True
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        title = (buf.value or "").strip()
        if title in _CALC_TITLES or title.startswith("计算器") or title.startswith("Calculator"):
            found.append(int(hwnd))
        return True

    user32.EnumWindows(EnumWindowsProc(_cb), 0)
    return found


def _restore_hwnd(hwnd: int) -> None:
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    SW_RESTORE = 9
    user32.ShowWindow(hwnd, SW_RESTORE)
    user32.SetForegroundWindow(hwnd)
    try:
        rect = wintypes.RECT()
        if user32.GetWindowRect(hwnd, ctypes.byref(rect)):
            user32.SetCursorPos(
                (int(rect.left) + int(rect.right)) // 2,
                (int(rect.top) + int(rect.bottom)) // 2,
            )
    except Exception:
        pass


def _minimize_hwnd(hwnd: int) -> None:
    import ctypes

    user32 = ctypes.windll.user32
    SW_MINIMIZE = 6
    user32.ShowWindow(hwnd, SW_MINIMIZE)


def _launch_calculator() -> bool:
    windir = os.environ.get("WINDIR", r"C:\Windows")
    calc_exe = os.path.join(windir, "System32", "calc.exe")
    try:
        if os.path.isfile(calc_exe):
            subprocess.Popen([calc_exe], shell=False)
            return True
    except OSError as exc:
        logger.warning("calc.exe launch failed: %s", exc)
    try:
        # Win10+ Calculator app protocol
        os.startfile("calculator:")  # type: ignore[attr-defined]
        return True
    except OSError as exc:
        logger.warning("calculator: protocol failed: %s", exc)
    try:
        os.startfile("calc.exe")  # type: ignore[attr-defined]
        return True
    except OSError as exc:
        logger.warning("os.startfile calc failed: %s", exc)
        return False


def open_or_toggle_calculator() -> bool:
    """Show Calculator; if already foreground, minimize (toggle)."""
    try:
        import ctypes

        user32 = ctypes.windll.user32
        hwnds = _enum_calculator_hwnds()
        if hwnds:
            fg = int(user32.GetForegroundWindow() or 0)
            if fg in hwnds:
                _minimize_hwnd(fg)
                return True
            # Prefer non-minimized, else first
            target = hwnds[0]
            for hwnd in hwnds:
                if not user32.IsIconic(hwnd):
                    target = hwnd
                    break
            _restore_hwnd(target)
            return True
        return _launch_calculator()
    except Exception:
        logger.exception("open_or_toggle_calculator failed")
        return False
