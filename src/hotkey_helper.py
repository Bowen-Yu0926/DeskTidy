"""Elevated keyboard hotkey helper (headless sidecar).

The main (non-elevated) DeskTidy process launches this helper elevated via
``ShellExecuteW("runas")``. The helper owns a ``WH_KEYBOARD_LL`` hook for
modifier-combo chords plus a ``GetAsyncKeyState`` poll for bare F-keys. Only
when the foreground window belongs to an *elevated* process — where the
non-elevated main app's RegisterHotKey / LL hook / poll are all UIPI-suppressed
— does the helper relay the matched hotkey ACTION over a localhost socket.

This keeps the main process non-elevated (so Explorer OLE drag-drop still
works) while restoring global hotkeys over elevated fullscreen games such as
Hearthstone.
"""

from __future__ import annotations

import ctypes
import json
import os
import socket
import sys
import time
from ctypes import wintypes

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
advapi32 = ctypes.windll.advapi32

# ---------------------------------------------------------------------------
# Win32 signatures (explicit, to dodge the 32-bit pseudo-handle truncation trap)
# ---------------------------------------------------------------------------
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
advapi32.OpenProcessToken.argtypes = [
    wintypes.HANDLE,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.HANDLE),
]
advapi32.OpenProcessToken.restype = wintypes.BOOL
advapi32.GetTokenInformation.argtypes = [
    wintypes.HANDLE,
    ctypes.c_int,
    ctypes.c_void_p,
    wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD),
]
advapi32.GetTokenInformation.restype = wintypes.BOOL
user32.GetForegroundWindow.restype = wintypes.HWND
user32.GetWindowThreadProcessId.argtypes = [
    wintypes.HWND,
    ctypes.POINTER(wintypes.DWORD),
]
user32.GetWindowThreadProcessId.restype = wintypes.DWORD
user32.PeekMessageW.argtypes = [
    ctypes.POINTER(wintypes.MSG),
    wintypes.HWND,
    wintypes.UINT,
    wintypes.UINT,
    wintypes.UINT,
]
user32.PeekMessageW.restype = wintypes.BOOL
user32.TranslateMessage.argtypes = [ctypes.POINTER(wintypes.MSG)]
user32.DispatchMessageW.argtypes = [ctypes.POINTER(wintypes.MSG)]

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
TOKEN_QUERY = 0x0008
TokenElevation = 20
WM_QUIT = 0x0012
PM_REMOVE = 0x0001

# Hotkey chord modifiers (hotkey_manager MOD_* namespace).
MOD_WIN = 0x0008
VK_D = 0x44
# Relay action for Win+D: minimize the fullscreen cover so the desktop shows.
# The shell's own "show desktop" cannot push past a fullscreen elevated app.
_SHOW_DESKTOP_ACTION = "show_desktop"

# A short-lived localhost connect is essentially instant; this is only a safety
# net so a stalled relay can never wedge the LL hook.
_RELAY_TIMEOUT_S = 0.3
_PING_INTERVAL_S = 5.0
_PING_FAIL_LIMIT = 3


def _pid_elevated(pid: int) -> bool:
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return False
    try:
        token = wintypes.HANDLE()
        if not advapi32.OpenProcessToken(h, TOKEN_QUERY, ctypes.byref(token)):
            return False
        try:
            val = wintypes.DWORD()
            retlen = wintypes.DWORD()
            if not advapi32.GetTokenInformation(
                token,
                TokenElevation,
                ctypes.byref(val),
                ctypes.sizeof(val),
                ctypes.byref(retlen),
            ):
                return False
            return bool(val.value)
        finally:
            kernel32.CloseHandle(token)
    finally:
        kernel32.CloseHandle(h)


def _foreground_elevated() -> bool:
    fg = user32.GetForegroundWindow()
    if not fg:
        return False
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(fg, ctypes.byref(pid))
    if not pid.value:
        return False
    return _pid_elevated(pid.value)


def _connect(port: int) -> socket.socket | None:
    try:
        return socket.create_connection(("127.0.0.1", port), timeout=_RELAY_TIMEOUT_S)
    except OSError:
        return None


def _relay(port: int, action: str) -> None:
    s = _connect(port)
    if s is None:
        return
    try:
        s.sendall((action + "\n").encode("utf-8"))
    except OSError:
        pass
    finally:
        s.close()


def _ping(port: int) -> bool:
    s = _connect(port)
    if s is None:
        return False
    s.close()
    return True


def _make_chord_callback(port: int, action: str):
    """Return a chord callback that relays when the foreground is elevated.

    Returning False opts out of consuming the key (see
    ``desktop_ll_keyboard._dispatch``): when the foreground is non-elevated the
    main app's own hook handles it, so the helper must pass the key through.
    """

    def _cb() -> bool:
        if _foreground_elevated():
            _relay(port, action)
            return True
        return False

    return _cb


def _make_show_desktop_callback(port: int):
    """Win+D: relay a desktop reveal when the foreground is elevated.

    Always returns False — Win+D is a system shortcut and must keep working
    normally; the helper only *adds* the minimize over an elevated fullscreen
    app (which the shell's show-desktop cannot get past).
    """

    def _cb() -> bool:
        if _foreground_elevated():
            _relay(port, _SHOW_DESKTOP_ACTION)
        return False

    return _cb


def _poll_fkeys(port: int, fkeys: dict[str, int], prev_down: dict[str, bool]) -> None:
    for action, vk in fkeys.items():
        down = bool(user32.GetAsyncKeyState(vk) & 0x8000)
        was = prev_down.get(action, False)
        if down and not was and _foreground_elevated():
            _relay(port, action)
        prev_down[action] = down


def _load_mapping(path: str) -> dict[str, str]:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def _mtime(path: str) -> float | None:
    try:
        return os.stat(path).st_mtime
    except OSError:
        return None


def _apply_mapping(mapping: dict[str, str], port: int) -> dict[str, int]:
    from src.desktop_ll_keyboard import clear_chord_handlers, register_chord_handler
    from src.hotkey_manager import _parse_hotkey

    clear_chord_handlers()
    # Built-in passthrough: Win+D reveals the desktop over an elevated fullscreen
    # app. Registered here (after clear) so a mapping hot-reload keeps it.
    register_chord_handler(MOD_WIN, VK_D, _make_show_desktop_callback(port))
    fkeys: dict[str, int] = {}
    for action, key in mapping.items():
        key = str(key or "").strip()
        if not key:
            continue
        parsed = _parse_hotkey(key)
        if parsed is None:
            continue
        modifiers, vk = parsed
        if modifiers == 0:
            fkeys[action] = vk
        else:
            register_chord_handler(modifiers, vk, _make_chord_callback(port, action))
    return fkeys


def _parse_args(argv: list[str]) -> tuple[int, str]:
    port = 0
    mapping = ""
    for i, a in enumerate(argv):
        if a == "--port" and i + 1 < len(argv):
            port = int(argv[i + 1])
        elif a.startswith("--port="):
            port = int(a.split("=", 1)[1])
        elif a == "--mapping" and i + 1 < len(argv):
            mapping = argv[i + 1]
        elif a.startswith("--mapping="):
            mapping = a.split("=", 1)[1]
    return port, mapping


def run_hotkey_helper(argv: list[str]) -> int:
    port, mapping_path = _parse_args(argv)
    if not port or not mapping_path:
        return 2

    fkeys = _apply_mapping(_load_mapping(mapping_path), port)

    msg = wintypes.MSG()
    prev_down: dict[str, bool] = {}
    last_mtime = _mtime(mapping_path)
    last_ping = time.monotonic()
    ping_fail = 0

    while True:
        # Message pump drives the LL hook callbacks.
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
            if msg.message == WM_QUIT:
                return 0
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

        if fkeys:
            _poll_fkeys(port, fkeys, prev_down)

        now = time.monotonic()

        # Hot reload: the main app rewrites the mapping file on hotkey refresh.
        mt = _mtime(mapping_path)
        if mt is not None and mt != last_mtime:
            last_mtime = mt
            fkeys = _apply_mapping(_load_mapping(mapping_path), port)

        # Liveness: exit once the main app is gone (its listener closed).
        if now - last_ping >= _PING_INTERVAL_S:
            last_ping = now
            if _ping(port):
                ping_fail = 0
            else:
                ping_fail += 1
                if ping_fail >= _PING_FAIL_LIMIT:
                    return 0

        time.sleep(0.02)
