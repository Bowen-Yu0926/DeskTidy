"""Shared low-level mouse hook — one WH_MOUSE_LL for all DeskTidy subscribers.

Two separate SetWindowsHookEx chains (double-click + RMB) doubled the global
hook cost when many apps also install LL hooks. Fan-out from a single hook.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from typing import Callable

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

WH_MOUSE_LL = 14

# Only messages DeskTidy subscribers care about. Skipping WM_MOUSEMOVE (and
# plain wheel) avoids Python fan-out on every system-wide cursor pixel.
WM_LBUTTONDBLCLK = 0x0203
WM_LBUTTONUP = 0x0202
WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205
WM_MOUSEWHEEL = 0x020A
WM_MOUSEHWHEEL = 0x020E
_VK_MENU = 0x12
_VK_CONTROL = 0x11
_INTERESTING_WPARAMS = frozenset(
    {WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_RBUTTONDOWN, WM_RBUTTONUP}
)

# Return True to consume the event (skip CallNextHookEx).
Handler = Callable[[int, int], bool | None]  # (w_param, l_param) -> eat?

_subscribers: list[Handler] = []
_hook_id = None
_proc = None


class MSLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [
        ("pt", wintypes.POINT),
        ("mouseData", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


LowLevelMouseProc = ctypes.WINFUNCTYPE(
    ctypes.c_ssize_t, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM
)

user32.SetWindowsHookExW.argtypes = [
    ctypes.c_int,
    ctypes.c_void_p,
    wintypes.HINSTANCE,
    wintypes.DWORD,
]
user32.SetWindowsHookExW.restype = wintypes.HHOOK
user32.CallNextHookEx.argtypes = [
    wintypes.HHOOK,
    ctypes.c_int,
    wintypes.WPARAM,
    wintypes.LPARAM,
]
user32.CallNextHookEx.restype = ctypes.c_ssize_t
user32.UnhookWindowsHookEx.argtypes = [wintypes.HHOOK]
user32.UnhookWindowsHookEx.restype = wintypes.BOOL


def zoom_modifier_physically_down() -> bool:
    """Alt or Ctrl down via Win32 (reliable while Qt modifiers lag on wheel)."""
    try:
        if user32.GetAsyncKeyState(_VK_MENU) & 0x8000:
            return True
        if user32.GetAsyncKeyState(_VK_CONTROL) & 0x8000:
            return True
    except Exception:
        pass
    return False


def wheel_delta_from_ll(l_param: int) -> int:
    """Signed wheel delta from MSLLHOOKSTRUCT.mouseData (high word)."""
    try:
        ms = MSLLHOOKSTRUCT.from_address(int(l_param))
        return int(ctypes.c_short((ms.mouseData >> 16) & 0xFFFF).value)
    except Exception:
        return 0


# Optional: skip wheel fan-out unless cursor is over a DeskTidy zoom target.
# Set by fence_widget when Alt-zoom targets are registered.
_wheel_interest_check: Callable[[int, int], bool] | None = None


def set_wheel_interest_check(fn: Callable[[int, int], bool] | None) -> None:
    """(x, y) -> True if wheel over a fence that cares about Alt/Ctrl zoom."""
    global _wheel_interest_check
    _wheel_interest_check = fn


def _dispatch(n_code: int, w_param: int, l_param: int) -> int:
    eat = False
    if n_code >= 0 and _subscribers:
        wp = int(w_param)
        # Fast path: do not enter Python handlers for move/plain-wheel noise.
        interesting = wp in _INTERESTING_WPARAMS
        if not interesting and wp == WM_MOUSEWHEEL:
            # Point-in-fence first — avoid GetAsyncKeyState on every notch elsewhere.
            over_target = True
            check = _wheel_interest_check
            if check is not None:
                try:
                    info = MSLLHOOKSTRUCT.from_address(int(l_param))
                    over_target = bool(check(int(info.pt.x), int(info.pt.y)))
                except Exception:
                    over_target = False
            if over_target and zoom_modifier_physically_down():
                interesting = True
        if interesting:
            # Copy so a subscriber can unregister mid-dispatch safely.
            for handler in tuple(_subscribers):
                try:
                    if handler(wp, int(l_param)):
                        eat = True
                except Exception:
                    pass
    if eat:
        return 1
    return int(user32.CallNextHookEx(_hook_id, n_code, w_param, l_param))


def register_ll_mouse_handler(handler: Handler) -> None:
    """Subscribe to the shared LL mouse hook (idempotent)."""
    global _proc, _hook_id
    if handler not in _subscribers:
        _subscribers.append(handler)
    if _hook_id:
        return
    if _proc is None:
        _proc = LowLevelMouseProc(_dispatch)
    ctypes.set_last_error(0)
    hook = user32.SetWindowsHookExW(WH_MOUSE_LL, _proc, None, 0)
    if not hook:
        ctypes.set_last_error(0)
        hook = user32.SetWindowsHookExW(
            WH_MOUSE_LL, _proc, kernel32.GetModuleHandleW(None), 0
        )
    _hook_id = hook or None


def unregister_ll_mouse_handler(handler: Handler) -> None:
    global _hook_id
    try:
        _subscribers.remove(handler)
    except ValueError:
        pass
    if _subscribers or not _hook_id:
        return
    user32.UnhookWindowsHookEx(_hook_id)
    _hook_id = None


def shutdown_ll_mouse() -> None:
    """Force-unhook on app exit so one-file _MEI cleanup is not blocked."""
    global _hook_id
    _subscribers.clear()
    if not _hook_id:
        return
    try:
        user32.UnhookWindowsHookEx(_hook_id)
    except Exception:
        pass
    _hook_id = None


def ll_mouse_hook_active() -> bool:
    return bool(_hook_id)


def subscriber_count() -> int:
    return len(_subscribers)
