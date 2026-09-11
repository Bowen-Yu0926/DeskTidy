"""Shared low-level keyboard hook — one WH_KEYBOARD_LL for DeskTidy.

Desktop overlays use WS_EX_NOACTIVATE so Explorer stays foreground; Qt
keyPressEvent on fence/public icons often never fires. Ctrl+C/V/X and friends
must be observed here, then marshalled onto the GUI thread.

Market note: Fences/DefView icons receive keys via Explorer ListView. DeskTidy
cannot host a real DefView per fence, so this LL hook is the minimal parallel
input path — gated by HWND / selection so other apps are not stolen from.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from typing import Callable

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

WH_KEYBOARD_LL = 13
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_SYSKEYDOWN = 0x0104
WM_SYSKEYUP = 0x0105
LLKHF_INJECTED = 0x10

VK_CONTROL = 0x11
VK_LCONTROL = 0xA2
VK_RCONTROL = 0xA3
VK_SHIFT = 0x10
VK_LSHIFT = 0xA0
VK_RSHIFT = 0xA1
VK_MENU = 0x12  # Alt
VK_LMENU = 0xA4
VK_RMENU = 0xA5
VK_A = 0x41
VK_C = 0x43
VK_V = 0x56
VK_X = 0x58
VK_DELETE = 0x2E
VK_F2 = 0x71

# Return True to consume the event (skip CallNextHookEx).
Handler = Callable[[int, int], bool | None]  # (vk, flags) -> eat?

_subscribers: list[Handler] = []
_hook_id = None
_proc = None

# Track modifiers from the hook stream itself. GetAsyncKeyState alone can miss
# very short Ctrl+C chords (C KEYDOWN processed while async state is briefly off).
_ctrl_vks_down: set[int] = set()
_shift_vks_down: set[int] = set()
_alt_vks_down: set[int] = set()
# After Ctrl up, keep a brief chord grace so a racing C KEYDOWN still counts
# (short tap: Ctrl↑ and C↓ can reorder by a few ms in the LL stream).
_ctrl_chord_grace_until = 0.0

_CTRL_VKS = frozenset({VK_CONTROL, VK_LCONTROL, VK_RCONTROL})
_SHIFT_VKS = frozenset({VK_SHIFT, VK_LSHIFT, VK_RSHIFT})
_ALT_VKS = frozenset({VK_MENU, VK_LMENU, VK_RMENU})
_CTRL_CHORD_GRACE_S = 0.06


class KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [
        ("vkCode", wintypes.DWORD),
        ("scanCode", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


LowLevelKeyboardProc = ctypes.WINFUNCTYPE(
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


def _track_modifier(vk: int, *, down: bool) -> None:
    global _ctrl_chord_grace_until
    import time

    if vk in _CTRL_VKS:
        if down:
            _ctrl_vks_down.add(vk)
            _ctrl_chord_grace_until = time.monotonic() + _CTRL_CHORD_GRACE_S
        else:
            _ctrl_vks_down.discard(vk)
            # Physical left/right up also clears the generic CONTROL bit.
            if vk in (VK_LCONTROL, VK_RCONTROL):
                _ctrl_vks_down.discard(VK_CONTROL)
            # Grace survives Ctrl↑ briefly so short Ctrl+C still matches.
            _ctrl_chord_grace_until = max(
                _ctrl_chord_grace_until, time.monotonic() + _CTRL_CHORD_GRACE_S
            )
    elif vk in _SHIFT_VKS:
        if down:
            _shift_vks_down.add(vk)
        else:
            _shift_vks_down.discard(vk)
            if vk in (VK_LSHIFT, VK_RSHIFT):
                _shift_vks_down.discard(VK_SHIFT)
    elif vk in _ALT_VKS:
        if down:
            _alt_vks_down.add(vk)
        else:
            _alt_vks_down.discard(vk)
            if vk in (VK_LMENU, VK_RMENU):
                _alt_vks_down.discard(VK_MENU)


def ctrl_physically_down() -> bool:
    """True when Ctrl is down for chord matching (hook tracker + grace + OS state)."""
    import time

    if _ctrl_vks_down:
        return True
    if time.monotonic() < float(_ctrl_chord_grace_until):
        return True
    try:
        # GetKeyState: message-sync state; GetAsyncKeyState: live physical state.
        if user32.GetKeyState(VK_CONTROL) & 0x8000:
            return True
        if user32.GetAsyncKeyState(VK_CONTROL) & 0x8000:
            return True
        if user32.GetAsyncKeyState(VK_LCONTROL) & 0x8000:
            return True
        if user32.GetAsyncKeyState(VK_RCONTROL) & 0x8000:
            return True
    except Exception:
        pass
    return False


def alt_physically_down() -> bool:
    if _alt_vks_down:
        return True
    try:
        return bool(user32.GetAsyncKeyState(VK_MENU) & 0x8000)
    except Exception:
        return False


def shift_physically_down() -> bool:
    if _shift_vks_down:
        return True
    try:
        return bool(user32.GetAsyncKeyState(VK_SHIFT) & 0x8000)
    except Exception:
        return False


def _dispatch(n_code: int, w_param: int, l_param: int) -> int:
    eat = False
    if n_code >= 0:
        wp = int(w_param)
        vk, flags = 0, 0
        try:
            info = KBDLLHOOKSTRUCT.from_address(int(l_param))
            vk = int(info.vkCode)
            flags = int(info.flags)
        except Exception:
            vk, flags = 0, 0

        # Always track modifiers (including injected) so short chords stay reliable.
        if vk and wp in (WM_KEYDOWN, WM_SYSKEYDOWN, WM_KEYUP, WM_SYSKEYUP):
            _track_modifier(vk, down=wp in (WM_KEYDOWN, WM_SYSKEYDOWN))

        if _subscribers and wp in (WM_KEYDOWN, WM_SYSKEYDOWN):
            interesting = vk in (
                VK_A,
                VK_C,
                VK_V,
                VK_X,
                VK_DELETE,
                VK_F2,
            )
            if vk and interesting and not (flags & LLKHF_INJECTED):
                for handler in tuple(_subscribers):
                    try:
                        if handler(vk, flags):
                            eat = True
                    except Exception:
                        pass
    if eat:
        return 1
    return int(user32.CallNextHookEx(_hook_id, n_code, w_param, l_param))


def register_ll_keyboard_handler(handler: Handler, *, force: bool = False) -> None:
    """Subscribe to the shared LL keyboard hook (idempotent)."""
    global _proc, _hook_id
    if handler not in _subscribers:
        _subscribers.append(handler)
    if _hook_id and not force:
        return
    if force and _hook_id:
        try:
            user32.UnhookWindowsHookEx(_hook_id)
        except Exception:
            pass
        _hook_id = None
    if _proc is None:
        _proc = LowLevelKeyboardProc(_dispatch)
    ctypes.set_last_error(0)
    hook = user32.SetWindowsHookExW(WH_KEYBOARD_LL, _proc, None, 0)
    if not hook:
        ctypes.set_last_error(0)
        hook = user32.SetWindowsHookExW(
            WH_KEYBOARD_LL, _proc, kernel32.GetModuleHandleW(None), 0
        )
    _hook_id = hook or None


def unregister_ll_keyboard_handler(handler: Handler) -> None:
    global _hook_id
    try:
        _subscribers.remove(handler)
    except ValueError:
        pass
    if _subscribers or not _hook_id:
        return
    user32.UnhookWindowsHookEx(_hook_id)
    _hook_id = None


def shutdown_ll_keyboard() -> None:
    """Force-unhook on app exit."""
    global _hook_id, _ctrl_chord_grace_until
    _subscribers.clear()
    _ctrl_vks_down.clear()
    _shift_vks_down.clear()
    _alt_vks_down.clear()
    _ctrl_chord_grace_until = 0.0
    if not _hook_id:
        return
    try:
        user32.UnhookWindowsHookEx(_hook_id)
    except Exception:
        pass
    _hook_id = None


def ll_keyboard_hook_active() -> bool:
    return bool(_hook_id)


def subscriber_count() -> int:
    return len(_subscribers)
