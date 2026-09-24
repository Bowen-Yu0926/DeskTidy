"""Native Win32 file OLE drag (DoDragDrop + shell IDataObject).

Qt ``QDrag.exec`` hangs against Electron drop targets (Cursor / VS Code /
Chrome) and can wedge the process OLE drag state. Explorer-class handoff uses
``ole32.DoDragDrop`` with a shell ``IDataObject`` and a ctypes ``IDropSource``
(Python COM gateways deadlock the GIL inside ``DoDragDrop``).
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import re
import threading
import time
from pathlib import Path

import pythoncom
from win32com.shell import shell, shellcon

from src.shell_clipboard import _normalize_paths

_COPY = shellcon.DROPEFFECT_COPY
_MOVE = shellcon.DROPEFFECT_MOVE
_LINK = getattr(shellcon, "DROPEFFECT_LINK", 4)

HRESULT = ctypes.c_long
S_OK = 0
DRAGDROP_S_DROP = 0x00040100
DRAGDROP_S_CANCEL = 0x00040101
DRAGDROP_S_USEDEFAULTCURSORS = 0x00040102
MK_LBUTTON = 0x0001


class _GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wt.DWORD),
        ("Data2", wt.WORD),
        ("Data3", wt.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]

    @staticmethod
    def from_str(value: str) -> "_GUID":
        import uuid

        u = uuid.UUID(value)
        g = _GUID()
        g.Data1 = u.time_low
        g.Data2 = u.time_mid
        g.Data3 = u.time_hi_version
        for i in range(8):
            g.Data4[i] = u.bytes[8 + i]
        return g


_IID_IUnknown = _GUID.from_str("00000000-0000-0000-C000-000000000046")
_IID_IDropSource = _GUID.from_str("00000121-0000-0000-C000-000000000046")


def _guid_eq(riid, expected: _GUID) -> bool:
    got = ctypes.cast(riid, ctypes.POINTER(_GUID)).contents
    return (
        got.Data1 == expected.Data1
        and got.Data2 == expected.Data2
        and got.Data3 == expected.Data3
        and bytes(got.Data4) == bytes(expected.Data4)
    )


def _com_ptr_from_py(obj) -> int:
    """Extract the native IUnknown* from a pywin32 interface wrapper."""
    m = re.search(r"obj at (0x[0-9A-Fa-f]+)", repr(obj))
    if not m:
        raise RuntimeError(f"cannot extract COM pointer from {obj!r}")
    return int(m.group(1), 16)


def _shell_data_object(abs_paths: list[str]):
    """Build Explorer ``IDataObject`` for existing filesystem paths.

    ``SHCreateDataObject`` with a ``None`` parent and absolute PIDLs raises in
    pywin32 — use parent folder + relative PIDLs (same-folder batch) or
    ``BHID_DataObject`` for a single item.
    """
    if not abs_paths:
        return None
    if len(abs_paths) == 1:
        try:
            item = shell.SHCreateItemFromParsingName(
                abs_paths[0], None, shell.IID_IShellItem
            )
            return item.BindToHandler(
                None, shell.BHID_DataObject, pythoncom.IID_IDataObject
            )
        except Exception:
            pass

    # Group by parent folder; one SHCreateDataObject per parent, prefer first
    # parent that yields a data object covering all paths when co-located.
    parsed: list[tuple[str, object]] = []
    for path in abs_paths:
        try:
            pidl, _flags = shell.SHParseDisplayName(path, 0)
        except Exception:
            continue
        if pidl:
            parsed.append((path, pidl))
    if not parsed:
        return None

    by_parent: dict[tuple, list] = {}
    for _path, pidl in parsed:
        parent = tuple(pidl[:-1])
        by_parent.setdefault(parent, []).append(pidl)

    # Prefer a single parent that holds every file.
    for parent_t, pidls in by_parent.items():
        if len(pidls) != len(parsed):
            continue
        try:
            return shell.SHCreateDataObject(
                list(parent_t),
                [p[-1:] for p in pidls],
                None,
                pythoncom.IID_IDataObject,
            )
        except Exception:
            continue

    # Multi-parent batch: absolute PIDLs via IShellItemArray → BHID_DataObject.
    # SHCreateDataObject needs one relative parent; first-file-only dropped the rest.
    try:
        from win32com.shell.shell import SHCreateShellItemArrayFromIDLists

        array = SHCreateShellItemArrayFromIDLists([pidl for _p, pidl in parsed])
        return array.BindToHandler(
            None, shell.BHID_DataObject, pythoncom.IID_IDataObject
        )
    except Exception:
        pass

    # Last resort: first file only via BindToHandler.
    try:
        item = shell.SHCreateItemFromParsingName(
            parsed[0][0], None, shell.IID_IShellItem
        )
        return item.BindToHandler(
            None, shell.BHID_DataObject, pythoncom.IID_IDataObject
        )
    except Exception:
        return None


class _CtypesDropSource:
    """STA-safe ``IDropSource`` implemented in ctypes (no GIL gateway)."""

    def __init__(self, *, deadline: float) -> None:
        self._deadline = float(deadline)
        self._refs = 1
        self.queries = 0
        self._keep: list = []

        @ctypes.WINFUNCTYPE(
            HRESULT,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
        )
        def query_interface(this, riid, ppv):
            if not ppv:
                return 0x80004003
            ppv[0] = None
            if _guid_eq(riid, _IID_IUnknown) or _guid_eq(riid, _IID_IDropSource):
                ppv[0] = this
                self._refs += 1
                return S_OK
            return 0x80004002

        @ctypes.WINFUNCTYPE(wt.ULONG, ctypes.c_void_p)
        def add_ref(_this):
            self._refs += 1
            return self._refs

        @ctypes.WINFUNCTYPE(wt.ULONG, ctypes.c_void_p)
        def release(_this):
            self._refs = max(self._refs - 1, 0)
            return self._refs

        @ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wt.BOOL, wt.DWORD)
        def query_continue_drag(_this, escape_pressed, key_state):
            self.queries += 1
            if escape_pressed:
                return DRAGDROP_S_CANCEL
            if time.perf_counter() >= self._deadline:
                return DRAGDROP_S_CANCEL
            if not (int(key_state) & MK_LBUTTON):
                return DRAGDROP_S_DROP
            return S_OK

        @ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wt.DWORD)
        def give_feedback(_this, _effect):
            return DRAGDROP_S_USEDEFAULTCURSORS

        class VTable(ctypes.Structure):
            _fields_ = [
                ("QueryInterface", type(query_interface)),
                ("AddRef", type(add_ref)),
                ("Release", type(release)),
                ("QueryContinueDrag", type(query_continue_drag)),
                ("GiveFeedback", type(give_feedback)),
            ]

        vtable = VTable(
            query_interface, add_ref, release, query_continue_drag, give_feedback
        )

        class COMObject(ctypes.Structure):
            _fields_ = [("lpVtbl", ctypes.POINTER(VTable))]

        obj = COMObject()
        obj.lpVtbl = ctypes.pointer(vtable)
        self._vtable = vtable
        self._obj = obj
        self._keep.extend(
            [query_interface, add_ref, release, query_continue_drag, give_feedback]
        )
        self.pointer = ctypes.addressof(obj)


def _escape_watchdog(deadline: float, stop: threading.Event) -> None:
    """After *deadline*, synthesize input so ``QueryContinueDrag`` can cancel.

    Windows mainly invokes ``IDropSource`` on input changes — a silent timeout
    inside ``QueryContinueDrag`` never runs if the cursor is frozen.
    """
    user32 = ctypes.windll.user32
    if stop.wait(max(0.0, deadline - time.perf_counter())):
        return
    pt = wt.POINT()
    for _ in range(8):
        if stop.is_set():
            return
        if user32.GetCursorPos(ctypes.byref(pt)):
            user32.SetCursorPos(int(pt.x) + 2, int(pt.y) + 1)
            user32.SetCursorPos(int(pt.x), int(pt.y))
        # Relative move also injects WM_MOUSEMOVE into the OLE drag loop.
        user32.mouse_event(0x0001, 2, 1, 0, 0)  # MOUSEEVENTF_MOVE
        user32.keybd_event(0x1B, 0, 0, 0)
        user32.keybd_event(0x1B, 0, 2, 0)
        if stop.wait(0.15):
            return



def do_file_ole_drag(
    paths: list[Path | str],
    *,
    copy: bool = True,
    timeout_s: float = 12.0,
) -> int:
    """Run native ``DoDragDrop`` for *paths*. Returns ``DROPEFFECT_*`` (0 = none).

    Call while the physical left button is still down (mid-gesture handoff).
    """
    abs_paths = _normalize_paths(paths)
    if not abs_paths:
        return 0

    ole_owned = False
    try:
        hr = int(ctypes.windll.ole32.OleInitialize(None) or 0)
        ole_owned = hr == 0  # S_OK — we own uninit; S_FALSE means host owns it
    except Exception:
        try:
            pythoncom.CoInitialize()
        except Exception:
            pass

    data = _shell_data_object(abs_paths)
    if data is None:
        if ole_owned:
            try:
                ctypes.windll.ole32.OleUninitialize()
            except Exception:
                pass
        return 0

    try:
        data_ptr = _com_ptr_from_py(data)
    except Exception:
        if ole_owned:
            try:
                ctypes.windll.ole32.OleUninitialize()
            except Exception:
                pass
        return 0

    deadline = time.perf_counter() + float(timeout_s)
    source = _CtypesDropSource(deadline=deadline)
    effects = _COPY if copy else (_COPY | _MOVE)

    stop = threading.Event()
    watcher = threading.Thread(
        target=_escape_watchdog,
        args=(deadline, stop),
        name="ole-drag-watchdog",
        daemon=True,
    )
    watcher.start()

    DoDragDrop = ctypes.windll.ole32.DoDragDrop
    DoDragDrop.argtypes = [
        ctypes.c_void_p,
        ctypes.c_void_p,
        wt.DWORD,
        ctypes.POINTER(wt.DWORD),
    ]
    DoDragDrop.restype = HRESULT
    effect = wt.DWORD(0)
    result = 0
    try:
        # Keep pywin32 data object + ctypes source alive for the modal loop.
        _alive = (data, source)
        hr = int(
            DoDragDrop(
                ctypes.c_void_p(data_ptr),
                ctypes.c_void_p(source.pointer),
                int(effects),
                ctypes.byref(effect),
            )
        )
        hr_u = hr & 0xFFFFFFFF
        # DROPEFFECT_NONE (0) with DRAGDROP_S_DROP means "released over a
        # non-target" — must not report Copy or callers skip chat/WM_DROPFILES.
        if hr_u in (DRAGDROP_S_DROP, S_OK):
            result = int(effect.value or 0)
        else:
            result = 0
        _ = _alive
    finally:
        stop.set()
        if ole_owned:
            try:
                ctypes.windll.ole32.OleUninitialize()
            except Exception:
                pass
    return int(result or 0)


def drop_effect_to_qt_name(effect: int) -> str:
    if effect & _MOVE:
        return "MoveAction"
    if effect & _COPY:
        return "CopyAction"
    if effect & _LINK:
        return "LinkAction"
    return "IgnoreAction"
