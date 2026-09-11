"""Explorer-style file clipboard (CF_HDROP + shell companion formats).

Cut/Copy/Paste for desktop organizers should use the shell clipboard formats
so Explorer and other apps interoperate — not plain-text paths.

WeChat / QQ / DingTalk often need ``FileNameW`` (and prefer ``Shell IDList
Array``) in addition to ``CF_HDROP``. CF_HDROP alone can make WeChat paste a
generic ``a.txt`` instead of the real file.
"""

from __future__ import annotations

import ctypes
import struct
import sys
import time
from ctypes import wintypes
from pathlib import Path

CF_HDROP = 15
GMEM_MOVEABLE = 0x0002
GMEM_ZEROINIT = 0x0040
GHND = GMEM_MOVEABLE | GMEM_ZEROINIT
DROPEFFECT_COPY = 1
DROPEFFECT_MOVE = 2

_owner_hwnd = 0
_owner_widget = None  # Qt fallback keep-alive

# Qt mime aliases for Win32 shell formats (same as fence drag payload).
_QT_CF_HDROP = 'application/x-qt-windows-mime;value="CF_HDROP"'
_QT_FILENAME_W = 'application/x-qt-windows-mime;value="FileNameW"'
_QT_FILENAME = 'application/x-qt-windows-mime;value="FileName"'
_QT_SHELL_IDLIST = 'application/x-qt-windows-mime;value="Shell IDList Array"'


def _user32_hwnd_api():
    """user32 with 64-bit-safe HWND restypes (truncated c_int breaks CreateWindow)."""
    lib = ctypes.WinDLL("user32", use_last_error=True)
    lib.IsWindow.argtypes = [wintypes.HWND]
    lib.IsWindow.restype = wintypes.BOOL
    lib.DestroyWindow.argtypes = [wintypes.HWND]
    lib.DestroyWindow.restype = wintypes.BOOL
    lib.CreateWindowExW.argtypes = [
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
    lib.CreateWindowExW.restype = wintypes.HWND
    return lib


def _create_message_only_clipboard_hwnd() -> int:
    """HWND_MESSAGE owner — never paints, never joins Z-order."""
    user32 = _user32_hwnd_api()
    HWND_MESSAGE = wintypes.HWND(-3)
    try:
        hwnd = user32.CreateWindowExW(
            0,
            "STATIC",
            "DeskTidyClipboardOwner",
            0,
            0,
            0,
            0,
            0,
            HWND_MESSAGE,
            wintypes.HMENU(0),
            wintypes.HINSTANCE(0),
            None,
        )
    except Exception:
        return 0
    return int(hwnd or 0)


def _create_qt_hidden_clipboard_owner() -> int:
    """Last-resort hidden Qt owner when message-only HWND is unavailable."""
    global _owner_widget
    try:
        from PyQt6.QtCore import Qt as _Qt
        from PyQt6.QtWidgets import QApplication, QWidget
    except Exception:
        return 0
    app = QApplication.instance()
    if app is None:
        return 0
    w = QWidget()
    w.setObjectName("desktidyClipboardOwner")
    w.setWindowFlags(
        _Qt.WindowType.Tool
        | _Qt.WindowType.FramelessWindowHint
        | _Qt.WindowType.WindowDoesNotAcceptFocus
    )
    w.setAttribute(_Qt.WidgetAttribute.WA_DontShowOnScreen, True)
    w.setAttribute(_Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
    w.setAttribute(_Qt.WidgetAttribute.WA_QuitOnClose, False)
    w.resize(1, 1)
    w.show()
    try:
        app.processEvents()
    except Exception:
        pass
    try:
        hwnd = int(w.winId())
    except Exception:
        hwnd = 0
    if not hwnd:
        try:
            w.close()
            w.deleteLater()
        except Exception:
            pass
        return 0
    _owner_widget = w
    try:
        app._desktidy_clipboard_owner = w  # type: ignore[attr-defined]
    except Exception:
        pass
    return hwnd


def _clipboard_owner_hwnd() -> int:
    """HWND for OpenClipboard — dedicated hidden owner only.

    Never borrow FenceWidget / PublicIconHost / page chrome. OpenClipboard on a
    visible translucent overlay raises/flashes that HWND (users: VPN window
    open/close on Ctrl+C / Ctrl+V).
    """
    global _owner_hwnd
    if _owner_hwnd:
        try:
            if _user32_hwnd_api().IsWindow(wintypes.HWND(int(_owner_hwnd))):
                return int(_owner_hwnd)
        except Exception:
            pass
        _owner_hwnd = 0
    hwnd = _create_message_only_clipboard_hwnd()
    if not hwnd:
        hwnd = _create_qt_hidden_clipboard_owner()
    _owner_hwnd = int(hwnd or 0)
    return int(_owner_hwnd)


class POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class DROPFILES(ctypes.Structure):
    _fields_ = [
        ("pFiles", wintypes.DWORD),
        ("pt", POINT),
        ("fNC", wintypes.BOOL),
        ("fWide", wintypes.BOOL),
    ]


_user32_lib = None
_kernel32_lib = None
_shell32_lib = None


def _user32():
    global _user32_lib
    if _user32_lib is not None:
        return _user32_lib
    lib = ctypes.WinDLL("user32", use_last_error=True)
    lib.OpenClipboard.argtypes = [wintypes.HWND]
    lib.OpenClipboard.restype = wintypes.BOOL
    lib.EmptyClipboard.argtypes = []
    lib.EmptyClipboard.restype = wintypes.BOOL
    lib.CloseClipboard.argtypes = []
    lib.CloseClipboard.restype = wintypes.BOOL
    lib.SetClipboardData.argtypes = [wintypes.UINT, wintypes.HANDLE]
    lib.SetClipboardData.restype = wintypes.HANDLE
    lib.GetClipboardData.argtypes = [wintypes.UINT]
    lib.GetClipboardData.restype = wintypes.HANDLE
    lib.RegisterClipboardFormatW.argtypes = [wintypes.LPCWSTR]
    lib.RegisterClipboardFormatW.restype = wintypes.UINT
    _user32_lib = lib
    return lib


def _kernel32():
    global _kernel32_lib
    if _kernel32_lib is not None:
        return _kernel32_lib
    lib = ctypes.WinDLL("kernel32", use_last_error=True)
    lib.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
    lib.GlobalAlloc.restype = wintypes.HGLOBAL
    lib.GlobalLock.argtypes = [wintypes.HGLOBAL]
    lib.GlobalLock.restype = ctypes.c_void_p
    lib.GlobalUnlock.argtypes = [wintypes.HGLOBAL]
    lib.GlobalUnlock.restype = wintypes.BOOL
    lib.GlobalFree.argtypes = [wintypes.HGLOBAL]
    lib.GlobalFree.restype = wintypes.HGLOBAL
    _kernel32_lib = lib
    return lib


def _shell32():
    global _shell32_lib
    if _shell32_lib is not None:
        return _shell32_lib
    lib = ctypes.WinDLL("shell32", use_last_error=True)
    lib.DragQueryFileW.argtypes = [
        wintypes.HANDLE,
        wintypes.UINT,
        wintypes.LPWSTR,
        wintypes.UINT,
    ]
    lib.DragQueryFileW.restype = wintypes.UINT
    lib.ILCreateFromPathW.argtypes = [wintypes.LPCWSTR]
    lib.ILCreateFromPathW.restype = ctypes.c_void_p
    lib.ILFree.argtypes = [ctypes.c_void_p]
    lib.ILFree.restype = None
    lib.ILGetSize.argtypes = [ctypes.c_void_p]
    lib.ILGetSize.restype = wintypes.UINT
    _shell32_lib = lib
    return lib


def _open_clipboard(user32, *, allow_sleep: bool = False) -> bool:
    """Open the clipboard for writing.

    Prefer ``OpenClipboard(None)`` first. A message-only owner HWND is a
    fallback for flash-free EmptyClipboard; some chat apps (WeChat) are picky
    about who owns the clipboard when pasting files.

    Default: a few immediate retries only — never ``sleep`` on the Qt UI thread.
    Background callers may pass ``allow_sleep=True`` for clipboard-owner races.
    """
    attempts = 8 if allow_sleep else 3
    for i in range(attempts):
        if user32.OpenClipboard(None):
            return True
        hwnd = _clipboard_owner_hwnd()
        if hwnd and user32.OpenClipboard(hwnd):
            return True
        if allow_sleep and i + 1 < attempts:
            time.sleep(0.01)
    return False


def _normalize_paths(paths: list[Path | str]) -> list[str]:
    """Absolute filesystem paths suitable for CF_HDROP / WeChat paste."""
    resolve_external = None
    try:
        from src.ui.fence_icon_item import resolve_external_drag_path as resolve_external
    except Exception:
        resolve_external = None

    abs_paths: list[str] = []
    seen: set[str] = set()
    for raw in paths:
        try:
            p = Path(raw)
            if resolve_external is not None:
                resolved = resolve_external(p)
                if resolved is None:
                    continue
                p = resolved
            elif not p.exists():
                continue
            # WeChat rejects forward-slash paths and invents a generic a.txt.
            abs_path = str(p.resolve(strict=False)).replace("/", "\\")
        except OSError:
            continue
        key = abs_path.casefold()
        if key in seen:
            continue
        seen.add(key)
        abs_paths.append(abs_path)
    return abs_paths


def _set_hglobal(user32, kernel32, fmt: int, data: bytes) -> bool:
    """Set one clipboard format from raw bytes (caller holds OpenClipboard)."""
    if not fmt or not data:
        return False
    hmem = kernel32.GlobalAlloc(GHND, len(data))
    if not hmem:
        return False
    ptr = kernel32.GlobalLock(hmem)
    if not ptr:
        kernel32.GlobalFree(hmem)
        return False
    ctypes.memmove(ptr, data, len(data))
    kernel32.GlobalUnlock(hmem)
    if not user32.SetClipboardData(int(fmt), hmem):
        kernel32.GlobalFree(hmem)
        return False
    return True


def _build_shell_idlist_bytes(abs_paths: list[str]) -> bytes | None:
    """CFSTR_SHELLIDLIST (CIDA) for absolute filesystem paths.

    Parent PIDL is empty (desktop). Children are absolute PIDLs from
    ``ILCreateFromPathW`` — the same layout Explorer uses for multi-folder
    file copies.
    """
    if not abs_paths:
        return None
    shell32 = _shell32()
    pidls: list[int] = []
    try:
        for path in abs_paths:
            pidl = shell32.ILCreateFromPathW(path)
            if not pidl:
                return None
            pidls.append(int(pidl))
        parent = b"\x00\x00"
        children: list[bytes] = []
        for pidl in pidls:
            size = int(shell32.ILGetSize(ctypes.c_void_p(pidl)))
            if size <= 0:
                return None
            children.append(ctypes.string_at(pidl, size))
        cidl = len(children)
        offset_count = cidl + 1
        header_size = 4 + 4 * offset_count
        offsets = [0] * offset_count
        pos = header_size
        offsets[0] = pos
        pos += len(parent)
        blobs = [parent]
        for i, child in enumerate(children):
            offsets[i + 1] = pos
            pos += len(child)
            blobs.append(child)
        raw = struct.pack("<I", cidl)
        raw += struct.pack(f"<{offset_count}I", *offsets)
        raw += b"".join(blobs)
        return raw
    except Exception:
        return None
    finally:
        for pidl in pidls:
            try:
                shell32.ILFree(ctypes.c_void_p(pidl))
            except Exception:
                pass


def build_cfhdrop_bytes(abs_paths: list[str]) -> bytes:
    """Raw ``CF_HDROP`` / ``DROPFILES`` payload for drag or ``WM_DROPFILES``."""
    listing = ("\0".join(abs_paths) + "\0\0").encode("utf-16-le")
    header = DROPFILES(
        pFiles=ctypes.sizeof(DROPFILES),
        pt=POINT(0, 0),
        fNC=0,
        fWide=1,
    )
    return (
        ctypes.string_at(ctypes.addressof(header), ctypes.sizeof(header)) + listing
    )


def global_hdrop_handle(abs_paths: list[str]) -> int:
    """Allocate a moveable global ``HDROP`` for ``WM_DROPFILES`` (caller transfers)."""
    if not abs_paths:
        return 0
    raw = build_cfhdrop_bytes(abs_paths)
    kernel32 = _kernel32()
    hmem = kernel32.GlobalAlloc(GHND, len(raw))
    if not hmem:
        return 0
    ptr = kernel32.GlobalLock(hmem)
    if not ptr:
        kernel32.GlobalFree(hmem)
        return 0
    ctypes.memmove(ptr, raw, len(raw))
    kernel32.GlobalUnlock(hmem)
    return int(hmem)


def attach_shell_file_drag_mime(
    mime,
    paths: list[Path | str],
    *,
    copy: bool = True,
) -> bool:
    """Explorer/WeChat drag formats without Qt ``setUrls`` (no UniformResourceLocator)."""
    try:
        from PyQt6.QtCore import QByteArray
    except Exception:
        return False
    abs_paths = _normalize_paths(paths)
    if not abs_paths:
        return False
    try:
        mime.setData(_QT_CF_HDROP, QByteArray(build_cfhdrop_bytes(abs_paths)))
        effect = DROPEFFECT_COPY if copy else DROPEFFECT_MOVE
        mime.setData("Preferred DropEffect", QByteArray(struct.pack("<I", effect)))
        for name, payload in _companion_shell_format_bytes(abs_paths).items():
            if name == "Shell IDList Array":
                qt_name = _QT_SHELL_IDLIST
            elif name == "FileNameW":
                qt_name = _QT_FILENAME_W
            elif name == "FileName":
                qt_name = _QT_FILENAME
            else:
                qt_name = f'application/x-qt-windows-mime;value="{name}"'
            mime.setData(qt_name, QByteArray(payload))
        return True
    except Exception:
        return False


def _companion_shell_format_bytes(abs_paths: list[str]) -> dict[str, bytes]:
    """Extra formats WeChat/QQ expect alongside CF_HDROP."""
    out: dict[str, bytes] = {}
    if not abs_paths:
        return out
    first = abs_paths[0]
    out["FileNameW"] = (first + "\0").encode("utf-16-le")
    try:
        out["FileName"] = first.encode("mbcs", errors="replace") + b"\0"
    except Exception:
        pass
    cida = _build_shell_idlist_bytes(abs_paths)
    if cida:
        out["Shell IDList Array"] = cida
    return out


def _set_files_qt(abs_paths: list[str], *, cut: bool) -> bool:
    """Qt fallback when Win32 OpenClipboard is denied."""
    try:
        from PyQt6.QtCore import QByteArray, QMimeData, QUrl
        from PyQt6.QtWidgets import QApplication
    except Exception:
        return False
    app = QApplication.instance()
    if app is None:
        return False
    mime = QMimeData()
    mime.setUrls([QUrl.fromLocalFile(p) for p in abs_paths])
    effect = DROPEFFECT_MOVE if cut else DROPEFFECT_COPY
    mime.setData("Preferred DropEffect", QByteArray(struct.pack("<I", effect)))
    for name, payload in _companion_shell_format_bytes(abs_paths).items():
        if name == "Shell IDList Array":
            qt_name = 'application/x-qt-windows-mime;value="Shell IDList Array"'
        elif name == "FileNameW":
            qt_name = _QT_FILENAME_W
        elif name == "FileName":
            qt_name = _QT_FILENAME
        else:
            qt_name = f'application/x-qt-windows-mime;value="{name}"'
        try:
            mime.setData(qt_name, QByteArray(payload))
        except Exception:
            pass
    try:
        app.clipboard().setMimeData(mime)
        return True
    except Exception:
        return False


def _get_files_qt() -> list[Path]:
    try:
        from PyQt6.QtWidgets import QApplication
    except Exception:
        return []
    app = QApplication.instance()
    if app is None:
        return []
    try:
        mime = app.clipboard().mimeData()
        if mime is None or not mime.hasUrls():
            return []
        out: list[Path] = []
        for url in mime.urls():
            if url.isLocalFile():
                out.append(Path(url.toLocalFile()))
        return out
    except Exception:
        return []


def _write_win32_file_formats(
    user32,
    kernel32,
    abs_paths: list[str],
    *,
    cut: bool,
) -> bool:
    """EmptyClipboard + CF_HDROP / FileNameW / Shell IDList (caller holds open)."""
    raw = build_cfhdrop_bytes(abs_paths)
    user32.EmptyClipboard()
    if not _set_hglobal(user32, kernel32, CF_HDROP, raw):
        return False
    fmt_effect = user32.RegisterClipboardFormatW("Preferred DropEffect")
    effect_val = DROPEFFECT_MOVE if cut else DROPEFFECT_COPY
    _set_hglobal(
        user32,
        kernel32,
        int(fmt_effect),
        struct.pack("<I", int(effect_val)),
    )
    for name, payload in _companion_shell_format_bytes(abs_paths).items():
        fmt = user32.RegisterClipboardFormatW(name)
        _set_hglobal(user32, kernel32, int(fmt), payload)
    return True


def clipboard_set_files(paths: list[Path | str], *, cut: bool = False) -> bool:
    """Put filesystem paths on the clipboard as Explorer-compatible file data.

    Always sets CF_HDROP + Preferred DropEffect. Also sets FileNameW / FileName
    and Shell IDList Array so WeChat and similar apps paste the real file
    instead of inventing a generic ``a.txt``.

    Prefer the Win32 path: Qt ``setUrls`` also registers UniformResourceLocator,
    which WeChat often turns into a 1-byte ``a.txt`` stub.
    """
    if sys.platform != "win32":
        return False
    abs_paths = _normalize_paths(paths)
    if not abs_paths:
        return False

    user32 = _user32()
    kernel32 = _kernel32()

    def _try_win32() -> bool:
        if not _open_clipboard(user32, allow_sleep=True):
            return False
        try:
            return bool(_write_win32_file_formats(user32, kernel32, abs_paths, cut=cut))
        finally:
            user32.CloseClipboard()

    # Clipboard-owner races (WeChat / Office) — brief retries on the GUI thread.
    for attempt in range(8):
        if _try_win32():
            return True
        if attempt + 1 < 8:
            time.sleep(0.012)
            try:
                from PyQt6.QtWidgets import QApplication

                app = QApplication.instance()
                if app is not None:
                    app.processEvents()
            except Exception:
                pass

    # Qt can unlock a stuck clipboard, but leaves URL formats WeChat misreads —
    # immediately rewrite with Win32-only payload when possible.
    if not _set_files_qt(abs_paths, cut=cut):
        return False
    if _try_win32():
        return True
    return True


def clipboard_preferred_effect() -> int:
    """Return DROPEFFECT_COPY or DROPEFFECT_MOVE from the clipboard (default COPY)."""
    if sys.platform != "win32":
        return DROPEFFECT_COPY
    user32 = _user32()
    kernel32 = _kernel32()
    if not _open_clipboard(user32):
        try:
            from PyQt6.QtWidgets import QApplication

            app = QApplication.instance()
            mime = app.clipboard().mimeData() if app is not None else None
            if mime is not None and mime.hasFormat("Preferred DropEffect"):
                data = bytes(mime.data("Preferred DropEffect"))
                if len(data) >= 4:
                    value = struct.unpack("<I", data[:4])[0]
                    if int(value) == DROPEFFECT_MOVE:
                        return DROPEFFECT_MOVE
        except Exception:
            pass
        return DROPEFFECT_COPY
    try:
        fmt = user32.RegisterClipboardFormatW("Preferred DropEffect")
        handle = user32.GetClipboardData(fmt)
        if not handle:
            return DROPEFFECT_COPY
        ptr = kernel32.GlobalLock(handle)
        if not ptr:
            return DROPEFFECT_COPY
        try:
            value = ctypes.cast(ptr, ctypes.POINTER(wintypes.DWORD)).contents.value
        finally:
            kernel32.GlobalUnlock(handle)
        if int(value) == DROPEFFECT_MOVE:
            return DROPEFFECT_MOVE
        return DROPEFFECT_COPY
    finally:
        user32.CloseClipboard()


def clipboard_get_files() -> list[Path]:
    """Read CF_HDROP paths from the clipboard."""
    files, _effect = clipboard_get_files_with_effect()
    return files


def clipboard_get_files_with_effect() -> tuple[list[Path], int]:
    """One OpenClipboard: CF_HDROP paths + Preferred DropEffect (default COPY)."""
    if sys.platform != "win32":
        return [], DROPEFFECT_COPY
    user32 = _user32()
    kernel32 = _kernel32()
    shell32 = _shell32()
    if not _open_clipboard(user32):
        files = _get_files_qt()
        effect = DROPEFFECT_COPY
        try:
            from PyQt6.QtWidgets import QApplication

            app = QApplication.instance()
            mime = app.clipboard().mimeData() if app is not None else None
            if mime is not None and mime.hasFormat("Preferred DropEffect"):
                data = bytes(mime.data("Preferred DropEffect"))
                if len(data) >= 4 and struct.unpack("<I", data[:4])[0] == DROPEFFECT_MOVE:
                    effect = DROPEFFECT_MOVE
        except Exception:
            pass
        return files, effect
    try:
        effect = DROPEFFECT_COPY
        fmt = user32.RegisterClipboardFormatW("Preferred DropEffect")
        handle_fx = user32.GetClipboardData(fmt)
        if handle_fx:
            ptr = kernel32.GlobalLock(handle_fx)
            if ptr:
                try:
                    value = ctypes.cast(
                        ptr, ctypes.POINTER(wintypes.DWORD)
                    ).contents.value
                finally:
                    kernel32.GlobalUnlock(handle_fx)
                if int(value) == DROPEFFECT_MOVE:
                    effect = DROPEFFECT_MOVE
        handle = user32.GetClipboardData(CF_HDROP)
        if not handle:
            return _get_files_qt(), effect
        count = int(shell32.DragQueryFileW(handle, 0xFFFFFFFF, None, 0))
        out: list[Path] = []
        buf = ctypes.create_unicode_buffer(32768)
        for i in range(count):
            shell32.DragQueryFileW(handle, i, buf, len(buf))
            text = buf.value
            if text:
                out.append(Path(text))
        return (out or _get_files_qt()), effect
    finally:
        user32.CloseClipboard()
