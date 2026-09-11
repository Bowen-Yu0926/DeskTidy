"""Host the Windows Explorer IContextMenu for a filesystem path.

Follows the standard shell-hosting pattern (Raymond Chen / MSDN):
QueryContextMenu → TrackPopupMenu on a host HWND that forwards
IContextMenu2/3 menu messages → InvokeCommand.

DeskTidy-only commands may be prepended; all Explorer verbs stay with the shell.
"""

from __future__ import annotations

import ctypes
import uuid
from collections.abc import Callable
from ctypes import (
    HRESULT,
    POINTER,
    Structure,
    WINFUNCTYPE,
    byref,
    c_ssize_t,
    c_void_p,
    cast,
    sizeof,
)
from ctypes import wintypes
from dataclasses import dataclass
from pathlib import Path

import pythoncom
import win32api
import win32con
import win32gui
from win32com.shell import shell, shellcon

user32 = ctypes.windll.user32
user32.DefWindowProcW.argtypes = [
    wintypes.HWND,
    wintypes.UINT,
    wintypes.WPARAM,
    wintypes.LPARAM,
]
user32.DefWindowProcW.restype = ctypes.c_ssize_t
# 64-bit Windows: default c_int restype truncates HWND and can make
# CreateWindowExW look like failure → fallback to overlay HWND → DestroyWindow
# then kills the fence (External WM_DESTROY on FenceWidget).
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
user32.RegisterClassW.argtypes = [ctypes.c_void_p]
user32.RegisterClassW.restype = wintypes.ATOM
user32.IsWindow.argtypes = [wintypes.HWND]
user32.IsWindow.restype = wintypes.BOOL


@dataclass(frozen=True)
class ShellMenuCommand:
    """Extra (DeskTidy) command prepended above the Explorer shell menu."""

    label: str
    callback: Callable[[], None]
    enabled: bool = True
    checked: bool = False


_SHELL_ID_FIRST = 100
_SHELL_ID_LAST = 0x7FFF
_CUSTOM_ID_FIRST = 1
_CMF_CANRENAME = int(getattr(shellcon, "CMF_CANRENAME", 0x00000010))

_IID_IContextMenu2 = "{000214F4-0000-0000-C000-000000000046}"
_IID_IContextMenu3 = "{BCFCE0A0-EC17-11D0-8D10-00A0C90F2719}"

# Active IContextMenu2/3 during TrackPopupMenu (host WndProc reads these).
_g_pcm2: int = 0
_g_pcm3: int = 0

# Reused TrackPopupMenu owner — Create/Destroy per RMB was a visible popup lag.
_menu_host_hwnd: int = 0
# Parent IShellFolder cache (desktop path is stable across icon RMBs).
_parent_isf_cache: dict[str, object] = {}
_PARENT_ISF_CACHE_MAX = 8

WNDPROC = WINFUNCTYPE(
    ctypes.c_ssize_t, wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM
)


class GUID(Structure):
    _fields_ = [
        ("Data1", ctypes.c_uint32),
        ("Data2", ctypes.c_uint16),
        ("Data3", ctypes.c_uint16),
        ("Data4", ctypes.c_ubyte * 8),
    ]

    def __init__(self, value: str = "00000000-0000-0000-0000-000000000000") -> None:
        super().__init__()
        u = uuid.UUID(value)
        self.Data1 = u.time_low
        self.Data2 = u.time_mid
        self.Data3 = u.time_hi_version
        for i, b in enumerate(u.bytes[8:]):
            self.Data4[i] = b


class _PyIHead(Structure):
    """CPython + pywin32 PyIBase layout: PyObject_HEAD then IUnknown*."""

    _fields_ = [
        ("ob_refcnt", c_ssize_t),
        ("ob_type", c_void_p),
        ("m_obj", c_void_p),
    ]


def show_file_context_menu(
    path: Path | str,
    *,
    x: int,
    y: int,
    hwnd: int = 0,
    extra_commands: list[ShellMenuCommand] | None = None,
    parent_widget=None,
    on_before_shell_invoke: Callable[..., None] | None = None,
    paths: list[Path | str] | None = None,
) -> bool:
    """Alias for the standard shell menu host (parent_widget unused; API stable)."""
    _ = parent_widget
    return show_shell_context_menu(
        path,
        x=x,
        y=y,
        hwnd=hwnd,
        extra_commands=extra_commands,
        on_before_shell_invoke=on_before_shell_invoke,
        paths=paths,
    )


def _normalize_menu_path(path: Path | str) -> Path:
    """Prefer absolute paths as-is — ``Path.resolve()`` can stall on cloud roots."""
    p = Path(path)
    if p.is_absolute():
        return p
    try:
        return p.resolve(strict=False)
    except OSError:
        return p


def _normalize_menu_paths(
    path: Path | str, paths: list[Path | str] | None = None
) -> list[Path]:
    """Deduped menu targets: *path* first, then any extra multi-select paths."""
    primary = _normalize_menu_path(path)
    out: list[Path] = [primary]
    seen = {str(primary).casefold()}
    for raw in paths or ():
        try:
            p = _normalize_menu_path(raw)
        except Exception:
            continue
        key = str(p).casefold()
        if key in seen:
            continue
        seen.add(key)
        out.append(p)
    return out


def _same_parent_menu_paths(targets: list[Path]) -> list[Path]:
    """Explorer multi-item menus require one IShellFolder parent."""
    if len(targets) <= 1:
        return targets
    primary = targets[0]
    try:
        parent_key = str(primary.parent).casefold()
    except OSError:
        return [primary]
    same: list[Path] = [primary]
    for p in targets[1:]:
        try:
            if str(p.parent).casefold() == parent_key:
                same.append(p)
        except OSError:
            continue
    return same


def warm_shell_context_menu_host() -> None:
    """Create the reusable TrackPopupMenu HWND early (first RMB then skips create)."""
    try:
        pythoncom.CoInitialize()
    except Exception:
        pass
    _ensure_menu_host_hwnd()


def show_folder_background_menu(
    folder: Path | str,
    *,
    x: int,
    y: int,
    hwnd: int = 0,
    extra_commands: list[ShellMenuCommand] | None = None,
    on_before_shell_invoke: Callable[[], None] | None = None,
) -> bool:
    """Display Explorer's *folder background* menu (New / Paste / View, …).

    Uses ``IShellFolder::CreateViewObject(..., IID_IContextMenu)`` — the same
    pattern Explorer uses for blank-area RMB inside a folder view.

    ``on_before_shell_invoke`` runs immediately before ``InvokeCommand`` (not
    for DeskTidy extra commands or cancel) so callers can snapshot the folder.
    """
    target = _normalize_menu_path(folder)
    if not target.is_dir():
        return False

    def _acquire(host: int):
        folder_isf = _parent_shell_folder(host, str(target))
        return folder_isf.CreateViewObject(host, shell.IID_IContextMenu), None

    return _host_shell_context_menu(
        acquire_context_menu=_acquire,
        x=x,
        y=y,
        hwnd=hwnd,
        extra_commands=extra_commands,
        cmf_flags=shellcon.CMF_NORMAL | shellcon.CMF_EXPLORE,
        log_label=f"folder={target}",
        on_before_shell_invoke=on_before_shell_invoke,
    )


def show_shell_context_menu(
    path: Path | str,
    *,
    x: int,
    y: int,
    hwnd: int,
    extra_commands: list[ShellMenuCommand] | None = None,
    on_before_shell_invoke: Callable[[], None] | None = None,
    paths: list[Path | str] | None = None,
) -> bool:
    """Display Explorer's context menu for *path* at screen (*x*, *y*).

    When *paths* has multiple same-folder items (DeskTidy multi-select), host a
    multi-PIDL ``IContextMenu`` so verbs like Delete apply to the whole set —
    same pattern as Explorer's DefView selection.

    Returns True if the shell menu was shown (even when the user cancels).
    Returns False only when the menu could not be built.
    """
    targets = _normalize_menu_paths(path, paths)
    primary = targets[0]
    multi = len(targets) > 1

    def _acquire(host: int):
        return _acquire_files_context_menu(host, targets), primary if not multi else None

    cmf = shellcon.CMF_NORMAL | shellcon.CMF_EXPLORE
    if not multi:
        cmf |= _CMF_CANRENAME

    return _host_shell_context_menu(
        acquire_context_menu=_acquire,
        x=x,
        y=y,
        hwnd=hwnd,
        extra_commands=extra_commands,
        cmf_flags=cmf,
        log_label=f"path={primary}" + (f" n={len(targets)}" if multi else ""),
        rename_target=primary if not multi else None,
        on_before_shell_invoke=on_before_shell_invoke,
    )


def _parent_shell_folder(host: int, parent_path: str):
    """Bind ``parent_path`` to IShellFolder, caching desktop (and similar) parents."""
    key = parent_path.casefold()
    cached = _parent_isf_cache.get(key)
    if cached is not None:
        return cached
    desktop = shell.SHGetDesktopFolder()
    _eaten, parent_pidl, _attr = desktop.ParseDisplayName(host, None, parent_path)
    parent_folder = desktop.BindToObject(parent_pidl, None, shell.IID_IShellFolder)
    if len(_parent_isf_cache) >= _PARENT_ISF_CACHE_MAX:
        _parent_isf_cache.clear()
    _parent_isf_cache[key] = parent_folder
    return parent_folder


def _acquire_file_context_menu(host: int, target: Path):
    """Resolve IContextMenu for a filesystem item (market shell-host path)."""
    # Prefer one-shot parsing when pywin32 exposes it (fewer Bind hops).
    try:
        create_item = getattr(shell, "SHCreateItemFromParsingName", None)
        bhid = getattr(shell, "BHID_SFUIObject", None) or getattr(
            shellcon, "BHID_SFUIObject", None
        )
        if create_item is not None and bhid is not None:
            item = create_item(str(target), None, shell.IID_IShellItem)
            return item.BindToHandler(None, bhid, shell.IID_IContextMenu)
    except Exception:
        pass

    parent_path = str(target.parent)
    name = target.name
    try:
        parent_folder = _parent_shell_folder(host, parent_path)
        _eaten, item_pidl, _attr = parent_folder.ParseDisplayName(host, None, name)
        return parent_folder.GetUIObjectOf(
            host, [item_pidl], shell.IID_IContextMenu, 0
        )[1]
    except Exception:
        # Stale cache after Explorer restart / folder rename.
        _parent_isf_cache.pop(parent_path.casefold(), None)
        parent_folder = _parent_shell_folder(host, parent_path)
        _eaten, item_pidl, _attr = parent_folder.ParseDisplayName(host, None, name)
        return parent_folder.GetUIObjectOf(
            host, [item_pidl], shell.IID_IContextMenu, 0
        )[1]


def _acquire_files_context_menu(host: int, targets: list[Path]):
    """Resolve IContextMenu for one or many same-folder items (multi-select)."""
    items = _same_parent_menu_paths(targets)
    if len(items) <= 1:
        return _acquire_file_context_menu(host, items[0] if items else targets[0])

    parent_path = str(items[0].parent)
    try:
        parent_folder = _parent_shell_folder(host, parent_path)
        pidls = []
        for item in items:
            _eaten, item_pidl, _attr = parent_folder.ParseDisplayName(
                host, None, item.name
            )
            pidls.append(item_pidl)
        return parent_folder.GetUIObjectOf(
            host, pidls, shell.IID_IContextMenu, 0
        )[1]
    except Exception:
        _parent_isf_cache.pop(parent_path.casefold(), None)
        parent_folder = _parent_shell_folder(host, parent_path)
        pidls = []
        for item in items:
            _eaten, item_pidl, _attr = parent_folder.ParseDisplayName(
                host, None, item.name
            )
            pidls.append(item_pidl)
        return parent_folder.GetUIObjectOf(
            host, pidls, shell.IID_IContextMenu, 0
        )[1]


def _host_shell_context_menu(
    *,
    acquire_context_menu,
    x: int,
    y: int,
    hwnd: int,
    extra_commands: list[ShellMenuCommand] | None,
    cmf_flags: int,
    log_label: str,
    rename_target: Path | None = None,
    on_before_shell_invoke: Callable[[], None] | None = None,
) -> bool:
    """Shared TrackPopupMenu host for item and folder-background IContextMenu."""
    global _g_pcm2, _g_pcm3

    pythoncom.CoInitialize()
    hmenu = 0
    host = 0
    pcm2 = 0
    pcm3 = 0
    try:
        host = _ensure_menu_host_hwnd()
        if not host:
            # Borrowed owner only — never DestroyWindow this HWND.
            host = _resolve_menu_owner_hwnd(hwnd)
        if not host:
            return False

        context_menu, _acquired_target = acquire_context_menu(host)
        if rename_target is None and _acquired_target is not None:
            rename_target = _acquired_target

        punk = _pycom_iunknown_ptr(context_menu)
        if punk:
            pcm3 = _query_interface(punk, _IID_IContextMenu3)
            pcm2 = _query_interface(punk, _IID_IContextMenu2) if not pcm3 else pcm3

        hmenu = win32gui.CreatePopupMenu()
        callbacks: dict[int, Callable[[], None]] = {}
        insert_at = 0
        next_id = _CUSTOM_ID_FIRST
        for cmd in extra_commands or []:
            label = (cmd.label or "").strip()
            if not label:
                continue
            flags = win32con.MF_BYPOSITION | win32con.MF_STRING
            if not cmd.enabled:
                flags |= win32con.MF_GRAYED
            if cmd.checked:
                flags |= win32con.MF_CHECKED
            win32gui.InsertMenu(hmenu, insert_at, flags, next_id, label)
            callbacks[next_id] = cmd.callback
            next_id += 1
            insert_at += 1

        if callbacks:
            win32gui.InsertMenu(
                hmenu,
                insert_at,
                win32con.MF_BYPOSITION | win32con.MF_SEPARATOR,
                0,
                "",
            )
            insert_at += 1

        flags = int(cmf_flags)
        try:
            if win32api_shift_down():
                flags |= getattr(shellcon, "CMF_EXTENDEDVERBS", 0x0200)
        except Exception:
            pass

        context_menu.QueryContextMenu(
            hmenu, insert_at, _SHELL_ID_FIRST, _SHELL_ID_LAST, flags
        )

        _g_pcm2 = int(pcm2 or 0)
        _g_pcm3 = int(pcm3 or 0)
        try:
            win32gui.SetForegroundWindow(host)
        except Exception:
            pass

        tpm = (
            win32con.TPM_LEFTALIGN
            | win32con.TPM_RIGHTBUTTON
            | win32con.TPM_RETURNCMD
        )
        cmd_id = win32gui.TrackPopupMenu(hmenu, tpm, int(x), int(y), 0, host, None)
        try:
            win32gui.PostMessage(host, win32con.WM_NULL, 0, 0)
        except Exception:
            pass
        finally:
            _g_pcm2 = 0
            _g_pcm3 = 0

        if cmd_id in callbacks:
            try:
                callbacks[cmd_id]()
            except Exception:
                pass
            return True

        if cmd_id and _SHELL_ID_FIRST <= cmd_id <= _SHELL_ID_LAST:
            verb_offset = int(cmd_id - _SHELL_ID_FIRST)
            verb = _canonical_shell_verb(context_menu, verb_offset)
            if callable(on_before_shell_invoke):
                try:
                    on_before_shell_invoke(verb)
                except TypeError:
                    try:
                        on_before_shell_invoke()
                    except Exception:
                        pass
                except Exception:
                    pass
            # MSDN: with CMF_CANRENAME, the host must present rename UI for "rename".
            if rename_target is not None and _is_rename_verb(context_menu, verb_offset):
                _host_rename_item(rename_target)
            else:
                context_menu.InvokeCommand(
                    (0, host, verb_offset, None, None, win32con.SW_SHOWNORMAL, 0, 0)
                )
        return True
    except Exception:
        try:
            from src.app_logging import get_logger

            get_logger().exception(
                "host shell context menu failed %s", log_label
            )
        except Exception:
            pass
        return False
    finally:
        _g_pcm2 = 0
        _g_pcm3 = 0
        if pcm3 and pcm3 != pcm2:
            _release(pcm3)
        if pcm2:
            _release(pcm2)
        if hmenu:
            try:
                win32gui.DestroyMenu(hmenu)
            except Exception:
                pass
        # Persistent menu host — do not DestroyWindow here.


def _ensure_menu_host_hwnd() -> int:
    """Return a long-lived popup owner HWND for TrackPopupMenu."""
    global _menu_host_hwnd
    if _menu_host_hwnd:
        try:
            if user32.IsWindow(wintypes.HWND(int(_menu_host_hwnd))):
                return int(_menu_host_hwnd)
        except Exception:
            pass
        _menu_host_hwnd = 0
    created = _create_menu_host_hwnd()
    _menu_host_hwnd = int(created or 0)
    return int(_menu_host_hwnd)


_menu_host_class = "DeskTidyShellMenuHost"
_menu_wndproc_ref = None
_menu_class_atom = 0


def _menu_host_wndproc(hwnd, msg, wparam, lparam):
    """Forward owner-draw / cascade messages to IContextMenu2/3 (MSDN pattern)."""
    global _g_pcm2, _g_pcm3
    wp = wparam & ((1 << (8 * sizeof(c_void_p))) - 1)
    lp = lparam & ((1 << (8 * sizeof(c_void_p))) - 1)
    try:
        if _g_pcm3:
            lres = ctypes.c_ssize_t(0)
            if _handle_menu_msg2(_g_pcm3, msg, wp, lp, byref(lres)) >= 0:
                return int(lres.value)
        elif _g_pcm2:
            if _handle_menu_msg(_g_pcm2, msg, wp, lp) >= 0:
                return 0
    except Exception:
        pass
    try:
        return int(user32.DefWindowProcW(hwnd, msg, wp, lp) or 0)
    except Exception:
        return 0


def _create_menu_host_hwnd() -> int:
    """Message-friendly top-level HWND for TrackPopupMenu + HandleMenuMsg2."""
    global _menu_wndproc_ref, _menu_class_atom
    try:
        if _menu_wndproc_ref is None:
            _menu_wndproc_ref = WNDPROC(_menu_host_wndproc)
        if not _menu_class_atom:
            class WNDCLASSW(Structure):
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

            wc = WNDCLASSW()
            wc.lpfnWndProc = _menu_wndproc_ref
            wc.hInstance = ctypes.windll.kernel32.GetModuleHandleW(None)
            wc.lpszClassName = _menu_host_class
            atom = user32.RegisterClassW(byref(wc))
            if not atom:
                # Already registered in this process.
                atom = 1
            _menu_class_atom = int(atom)

        # Prefer a real (hidden) popup owner — TrackPopupMenu is unreliable on
        # HWND_MESSAGE, and a failed create must not fall back to destroying
        # DeskTidy overlay HWNDs.
        #
        # WS_EX_TOOLWINDOW: SetForegroundWindow(host) must count as desktop
        # chrome FG. Without it, the first RMB looked like a foreign app and
        # the sink path left SW_HIDE'd fences unmapped (partitions "vanished").
        WS_EX_TOOLWINDOW = 0x00000080
        WS_EX_NOACTIVATE = 0x08000000
        ex_style = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
        hwnd = user32.CreateWindowExW(
            ex_style,
            _menu_host_class,
            "DeskTidyShellMenuHost",
            win32con.WS_POPUP,
            0,
            0,
            0,
            0,
            None,
            None,
            ctypes.windll.kernel32.GetModuleHandleW(None),
            None,
        )
        if not hwnd:
            hwnd = user32.CreateWindowExW(
                0,
                _menu_host_class,
                "DeskTidyShellMenuHost",
                0,
                0,
                0,
                0,
                0,
                wintypes.HWND(win32con.HWND_MESSAGE),
                None,
                ctypes.windll.kernel32.GetModuleHandleW(None),
                None,
            )
        return int(hwnd or 0)
    except Exception:
        return 0


def _pycom_iunknown_ptr(py_iface) -> int:
    try:
        return int(_PyIHead.from_address(id(py_iface)).m_obj or 0)
    except Exception:
        return 0


def _vtable(punk: int):
    return cast(cast(punk, POINTER(c_void_p)).contents, POINTER(c_void_p))


def _query_interface(punk: int, iid: str) -> int:
    if not punk:
        return 0
    QueryInterface = WINFUNCTYPE(
        HRESULT, c_void_p, POINTER(GUID), POINTER(c_void_p)
    )(_vtable(punk)[0])
    out = c_void_p()
    hr = QueryInterface(punk, byref(GUID(iid)), byref(out))
    if hr < 0 or not out.value:
        return 0
    return int(out.value)


def _release(punk: int) -> None:
    if not punk:
        return
    try:
        Release = WINFUNCTYPE(ctypes.c_ulong, c_void_p)(_vtable(punk)[2])
        Release(punk)
    except Exception:
        pass


def _handle_menu_msg(pcm2: int, msg: int, wparam: int, lparam: int) -> int:
    # IUnknown(0-2) + IContextMenu(3-5) + HandleMenuMsg = slot 6.
    HandleMenuMsg = WINFUNCTYPE(
        HRESULT, c_void_p, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM
    )(_vtable(pcm2)[6])
    return int(HandleMenuMsg(pcm2, msg, wparam, lparam))


def _handle_menu_msg2(
    pcm3: int, msg: int, wparam: int, lparam: int, plresult
) -> int:
    # IContextMenu2 + HandleMenuMsg2 = slot 7.
    HandleMenuMsg2 = WINFUNCTYPE(
        HRESULT,
        c_void_p,
        wintypes.UINT,
        wintypes.WPARAM,
        wintypes.LPARAM,
        POINTER(ctypes.c_ssize_t),
    )(_vtable(pcm3)[7])
    return int(HandleMenuMsg2(pcm3, msg, wparam, lparam, plresult))


def _resolve_menu_owner_hwnd(hwnd: int) -> int:
    candidates: list[int] = []
    try:
        if hwnd:
            candidates.append(int(hwnd))
    except Exception:
        pass
    try:
        from src.desktop_shell_host import _find_defview_host

        host, defview = _find_defview_host()
        if defview:
            candidates.append(int(defview))
        if host:
            candidates.append(int(host))
    except Exception:
        pass
    try:
        desk = int(win32gui.GetDesktopWindow() or 0)
        if desk:
            candidates.append(desk)
    except Exception:
        pass
    seen: set[int] = set()
    for h in candidates:
        if not h or h in seen:
            continue
        seen.add(h)
        try:
            if win32gui.IsWindow(h):
                return h
        except Exception:
            continue
    return 0


def _command_string(context_menu, verb_offset: int, gcs: int) -> str:
    try:
        raw = context_menu.GetCommandString(int(verb_offset), int(gcs))
    except Exception:
        return ""
    if raw is None:
        return ""
    if isinstance(raw, bytes):
        for enc in ("utf-8", "mbcs", "latin1"):
            try:
                return raw.decode(enc, errors="ignore").strip("\x00").strip()
            except Exception:
                continue
        return ""
    return str(raw).strip("\x00").strip()


def _canonical_shell_verb(context_menu, verb_offset: int) -> str:
    for gcs in (
        getattr(shellcon, "GCS_VERBW", 4),
        getattr(shellcon, "GCS_VERBA", 0),
    ):
        text = _command_string(context_menu, verb_offset, gcs)
        if text:
            return text.lower()
    return ""


def _is_rename_verb(context_menu, verb_offset: int) -> bool:
    return _canonical_shell_verb(context_menu, verb_offset) == "rename"


def _host_rename_item(path: Path) -> bool:
    """Host rename UI required by CMF_CANRENAME (non-DefView hosts).

    Prefer Explorer-like in-place edit on the visible icon; fall back to a
    name prompt when no overlay widget is found.
    """
    try:
        from src.ui.fence_icon_item import begin_inplace_rename, find_item_widget_for_path

        widget = find_item_widget_for_path(path)
        if widget is not None and begin_inplace_rename(widget):
            return True
    except Exception:
        pass

    try:
        from PyQt6.QtWidgets import QApplication, QInputDialog, QLineEdit
    except Exception:
        return False

    app = QApplication.instance()
    if app is None:
        return False
    old_name = path.name
    new_name, ok = QInputDialog.getText(
        None,
        "重命名",
        "新名称:",
        QLineEdit.EchoMode.Normal,
        old_name,
    )
    if not ok:
        return False
    try:
        from src.ui.fence_icon_item import commit_filesystem_rename

        return commit_filesystem_rename(path, new_name) is not None
    except Exception:
        return False


def win32api_shift_down() -> bool:
    try:
        return bool(win32api.GetAsyncKeyState(win32con.VK_SHIFT) & 0x8000)
    except Exception:
        return False
