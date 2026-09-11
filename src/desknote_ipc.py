"""IPC so DeskTidy / second deskNote launches reach the running deskNote instance."""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from pathlib import Path
from typing import Callable

from PyQt6.QtCore import QObject, Qt, pyqtSignal

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

WM_COPYDATA = 0x004A
HWND_MESSAGE = -3
WS_POPUP = 0x80000000

_IPC_WINDOW_CLASS = "DeskNoteShellIpcWnd"
_IPC_WINDOW_TITLE = "DeskNoteShellIpc"
_MAGIC = 0x444E01  # 'DN\x01'
_PATH_SEP = "\x1f"

_bridge: "_IpcBridge | None" = None
_hwnd = 0
_wnd_proc_ref = None
_class_registered = False


class COPYDATASTRUCT(ctypes.Structure):
    _fields_ = [
        ("dwData", ctypes.c_size_t),
        ("cbData", wintypes.DWORD),
        ("lpData", ctypes.c_void_p),
    ]


WNDPROC = ctypes.WINFUNCTYPE(
    ctypes.c_ssize_t,
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
)


class WNDCLASSW(ctypes.Structure):
    _fields_ = [
        ("style", wintypes.UINT),
        ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", wintypes.HINSTANCE),
        ("hIcon", wintypes.HANDLE),
        ("hCursor", wintypes.HANDLE),
        ("hbrBackground", wintypes.HANDLE),
        ("lpszMenuName", wintypes.LPCWSTR),
        ("lpszClassName", wintypes.LPCWSTR),
    ]


user32.DefWindowProcW.argtypes = [
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
user32.CreateWindowExW.argtypes = [
    wintypes.DWORD,
    wintypes.LPCWSTR,
    wintypes.LPCWSTR,
    wintypes.DWORD,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    ctypes.c_int,
    wintypes.HWND,
    wintypes.HMENU,
    wintypes.HINSTANCE,
    wintypes.LPVOID,
]
user32.CreateWindowExW.restype = wintypes.HWND
user32.DestroyWindow.argtypes = [wintypes.HWND]
user32.DestroyWindow.restype = wintypes.BOOL
user32.IsWindow.argtypes = [wintypes.HWND]
user32.IsWindow.restype = wintypes.BOOL
user32.RegisterClassW.argtypes = [ctypes.POINTER(WNDCLASSW)]
user32.RegisterClassW.restype = wintypes.ATOM
user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
user32.FindWindowW.restype = wintypes.HWND
user32.SendMessageW.argtypes = [
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
]
user32.SendMessageW.restype = ctypes.c_ssize_t
kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
kernel32.GetModuleHandleW.restype = wintypes.HINSTANCE


class _IpcBridge(QObject):
    received = pyqtSignal(str, list)


def _encode_payload(verb: str, paths: list[str] | None = None) -> bytes:
    body = _PATH_SEP.join(paths or [])
    return f"{verb}\t{body}".encode("utf-8")


def _decode_payload(data: bytes) -> tuple[str, list[str]] | None:
    try:
        text = data.decode("utf-8").strip("\0")
        if "\t" not in text:
            verb = text.strip()
            return (verb, []) if verb else None
        verb, _, rest = text.partition("\t")
        verb = verb.strip()
        if not verb:
            return None
        paths = [p for p in rest.split(_PATH_SEP) if p.strip()]
        return verb, paths
    except Exception:
        return None


def _wnd_proc(hwnd, msg, wparam, lparam):
    if msg == WM_COPYDATA and _bridge is not None:
        try:
            cds = COPYDATASTRUCT.from_address(int(lparam))
            if int(cds.dwData) != _MAGIC or not cds.lpData or cds.cbData <= 0:
                return 0
            raw = ctypes.string_at(cds.lpData, int(cds.cbData))
            parsed = _decode_payload(raw)
            if parsed is not None:
                verb, paths = parsed
                _bridge.received.emit(verb, paths)
                return 1
        except Exception:
            return 0
    return int(user32.DefWindowProcW(hwnd, msg, wparam, lparam) or 0)


def start_desknote_ipc_host(on_verb: Callable[[str, list[str]], None]) -> bool:
    """Create message-only HWND that receives deskNote COPYDATA messages."""
    global _bridge, _hwnd, _wnd_proc_ref, _class_registered
    if _hwnd and user32.IsWindow(_hwnd):
        if _bridge is None:
            _bridge = _IpcBridge()
        try:
            _bridge.received.disconnect()
        except TypeError:
            pass
        _bridge.received.connect(on_verb, Qt.ConnectionType.QueuedConnection)
        return True
    _bridge = _IpcBridge()
    _bridge.received.connect(on_verb, Qt.ConnectionType.QueuedConnection)
    _wnd_proc_ref = WNDPROC(_wnd_proc)
    if not _class_registered:
        wc = WNDCLASSW()
        wc.lpfnWndProc = _wnd_proc_ref
        wc.hInstance = kernel32.GetModuleHandleW(None)
        wc.lpszClassName = _IPC_WINDOW_CLASS
        user32.RegisterClassW(ctypes.byref(wc))
        _class_registered = True
    hwnd = user32.CreateWindowExW(
        0,
        _IPC_WINDOW_CLASS,
        _IPC_WINDOW_TITLE,
        WS_POPUP,
        0,
        0,
        0,
        0,
        wintypes.HWND(HWND_MESSAGE),
        None,
        kernel32.GetModuleHandleW(None),
        None,
    )
    if not hwnd:
        return False
    _hwnd = int(hwnd)
    return True


def stop_desknote_ipc_host() -> None:
    global _hwnd
    if _hwnd and user32.IsWindow(_hwnd):
        user32.DestroyWindow(_hwnd)
    _hwnd = 0


def send_to_running_desknote(verb: str, paths: list[str] | Path | None = None) -> bool:
    """Send verb (+ optional paths) to running deskNote. Returns True if delivered."""
    hwnd = int(user32.FindWindowW(_IPC_WINDOW_CLASS, _IPC_WINDOW_TITLE) or 0)
    if not hwnd:
        return False
    path_list: list[str] = []
    if paths is None:
        path_list = []
    elif isinstance(paths, (str, Path)):
        path_list = [str(paths)]
    else:
        path_list = [str(p) for p in paths if p]
    payload = _encode_payload(verb, path_list)
    buf = ctypes.create_string_buffer(payload)
    cds = COPYDATASTRUCT()
    cds.dwData = _MAGIC
    cds.cbData = len(payload)
    cds.lpData = ctypes.cast(buf, ctypes.c_void_p)
    result = user32.SendMessageW(
        hwnd, WM_COPYDATA, 0, ctypes.cast(ctypes.byref(cds), ctypes.c_void_p).value
    )
    return bool(result)


def desknote_ipc_window_exists() -> bool:
    return bool(user32.FindWindowW(_IPC_WINDOW_CLASS, _IPC_WINDOW_TITLE))
