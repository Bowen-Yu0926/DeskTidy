"""Application and file icon helpers."""

from __future__ import annotations

import ctypes
import sys
import threading
import uuid
from collections import OrderedDict
from ctypes import wintypes
from pathlib import Path

from PyQt6.QtCore import QFileInfo, Qt
from PyQt6.QtGui import QIcon, QImage, QPainter, QPixmap
from PyQt6.QtWidgets import QFileIconProvider

_FILE_ICON_PROVIDER = QFileIconProvider()
_FILE_PIXMAP_CACHE: OrderedDict[tuple[str, int, float], QPixmap] = OrderedDict()
_SHELL_RAW_CACHE: OrderedDict[str, QPixmap] = OrderedDict()
_MAX_FILE_PIXMAP_CACHE = 96
_MAX_SHELL_RAW_CACHE = 48
_SHELL_INDEX_CACHE: dict[str, int] = {}
_SHELL_INDEX_LOCK = threading.Lock()
_MAX_SHELL_INDEX_CACHE = 512

# Known desktop namespace items → CLSID (same shell path every icon uses).
_NAMESPACE_NAME_CLSID = {
    "此电脑": "{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
    "我的电脑": "{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
    "computer": "{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
    "this pc": "{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
    "my pc": "{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
    "my computer": "{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
    "回收站": "{645FF040-5081-101B-9F08-00AA002F954E}",
    "recycle bin": "{645FF040-5081-101B-9F08-00AA002F954E}",
    "recyclebin": "{645FF040-5081-101B-9F08-00AA002F954E}",
    "网络": "{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}",
    "network": "{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}",
    "用户文件夹": "{59031A47-3F72-44A7-89C5-5595FE6B30EE}",
}

_SHGFI_ICON = 0x000000100
_SHGFI_LARGEICON = 0x000000000
_SHGFI_SYSICONINDEX = 0x000004000
_SHGFI_PIDL = 0x000000008
_SHGFI_USEFILEATTRIBUTES = 0x000000010
_FILE_ATTRIBUTE_NORMAL = 0x00000080
_SHIL_LARGE = 0x0
_SHIL_EXTRALARGE = 0x2
_SHIL_JUMBO = 0x4

_ULONG_PTR = ctypes.c_uint64 if ctypes.sizeof(ctypes.c_void_p) == 8 else ctypes.c_uint32
_shell_ready = False
_shell32 = None
_user32 = None
_gdi32 = None
_IID_IImageList = None


class _GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_ulong),
        ("Data2", ctypes.c_ushort),
        ("Data3", ctypes.c_ushort),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class _SHFILEINFOW(ctypes.Structure):
    _fields_ = [
        ("hIcon", ctypes.c_void_p),
        ("iIcon", ctypes.c_int),
        ("dwAttributes", wintypes.DWORD),
        ("szDisplayName", wintypes.WCHAR * 260),
        ("szTypeName", wintypes.WCHAR * 80),
    ]


def _ensure_shell() -> bool:
    global _shell_ready, _shell32, _user32, _gdi32, _IID_IImageList
    if _shell_ready:
        return True
    if sys.platform != "win32":
        return False
    try:
        _shell32 = ctypes.WinDLL("shell32", use_last_error=True)
        _user32 = ctypes.WinDLL("user32", use_last_error=True)
        _gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
        ole32 = ctypes.WinDLL("ole32")
        ole32.CoInitialize(None)

        u = uuid.UUID("46EB5926-582E-4017-9FDF-E8998DAA0950")
        _IID_IImageList = _GUID(
            u.time_low,
            u.time_mid,
            u.time_hi_version,
            (ctypes.c_ubyte * 8).from_buffer_copy(u.bytes[8:]),
        )

        _shell32.SHGetFileInfoW.argtypes = [
            ctypes.c_void_p,
            wintypes.DWORD,
            ctypes.POINTER(_SHFILEINFOW),
            wintypes.UINT,
            wintypes.UINT,
        ]
        _shell32.SHGetFileInfoW.restype = _ULONG_PTR
        _shell32.SHParseDisplayName.argtypes = [
            wintypes.LPCWSTR,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
        ]
        _shell32.SHParseDisplayName.restype = ctypes.HRESULT
        _shell32.SHGetImageList.argtypes = [
            ctypes.c_int,
            ctypes.POINTER(_GUID),
            ctypes.POINTER(ctypes.c_void_p),
        ]
        _shell32.SHGetImageList.restype = ctypes.HRESULT
        _shell32.ILFree.argtypes = [ctypes.c_void_p]

        _user32.DrawIconEx.argtypes = [
            wintypes.HDC,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            wintypes.UINT,
            ctypes.c_void_p,
            wintypes.UINT,
        ]
        _user32.DrawIconEx.restype = wintypes.BOOL
        _user32.DestroyIcon.argtypes = [ctypes.c_void_p]
        _user32.GetDC.argtypes = [ctypes.c_void_p]
        _user32.GetDC.restype = wintypes.HDC
        _user32.ReleaseDC.argtypes = [ctypes.c_void_p, wintypes.HDC]
        _user32.GetIconInfo.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        _user32.GetIconInfo.restype = wintypes.BOOL
        _gdi32.GetObjectW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
        _gdi32.GetObjectW.restype = ctypes.c_int
        _gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
        _gdi32.CreateCompatibleDC.restype = wintypes.HDC
        _gdi32.CreateDIBSection.argtypes = [
            wintypes.HDC,
            ctypes.c_void_p,
            wintypes.UINT,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_void_p,
            wintypes.DWORD,
        ]
        _gdi32.CreateDIBSection.restype = ctypes.c_void_p
        _gdi32.SelectObject.argtypes = [wintypes.HDC, ctypes.c_void_p]
        _gdi32.SelectObject.restype = ctypes.c_void_p
        _gdi32.DeleteObject.argtypes = [ctypes.c_void_p]
        _gdi32.DeleteDC.argtypes = [wintypes.HDC]

        _shell_ready = True
        return True
    except Exception:
        _shell_ready = False
        return False


def icon_path() -> Path:
    if getattr(sys, "frozen", False):
        base = Path(sys._MEIPASS)
    else:
        base = Path(__file__).resolve().parent.parent
    return base / "assets" / "app_icon.ico"


def desknote_icon_path() -> Path:
    """Standalone deskNote product icon (shortcuts / window / frozen EXE)."""
    if getattr(sys, "frozen", False):
        base = Path(sys._MEIPASS)
    else:
        base = Path(__file__).resolve().parent.parent
    return base / "assets" / "desknote_icon.ico"


def get_app_icon(accent: str | None = None) -> QIcon:
    """App / window icon — same BrandMark glyph as the settings sidebar."""
    from src.ui.brand_mark import brand_mark_icon

    icon = brand_mark_icon(accent)
    if not icon.isNull():
        return icon
    # Fallback: embedded ICO (shortcuts / frozen resource).
    from PyQt6.QtCore import QSize

    path = icon_path()
    if not path.is_file():
        return QIcon()
    icon = QIcon()
    for edge in (16, 24, 32, 48, 64, 128, 256):
        icon.addFile(str(path), QSize(edge, edge))
    if icon.isNull():
        icon = QIcon(str(path))
    return icon


def get_desknote_icon() -> QIcon:
    """deskNote window / shortcut icon (teal notepad + #)."""
    from PyQt6.QtCore import QSize

    path = desknote_icon_path()
    if not path.is_file():
        return get_app_icon()
    icon = QIcon()
    for edge in (16, 24, 32, 48, 64, 128, 256):
        icon.addFile(str(path), QSize(edge, edge))
    if icon.isNull():
        icon = QIcon(str(path))
    return icon


def _desknote_product_pixmap(size: int) -> QPixmap:
    """Render ``desknote_icon.ico`` at *size* without shell ImageList / trim.

    Source-mode DeskNote.lnk targets pythonw.exe; Shell sometimes returns the
    generic blank-document glyph for that link. Product float must always show
    the teal notepad icon.
    """
    size = max(8, int(size))
    icon = get_desknote_icon()
    if icon.isNull():
        return QPixmap()
    pm = icon.pixmap(size, size)
    if pm.isNull():
        return QPixmap()
    if pm.width() == size and pm.height() == size:
        pm.setDevicePixelRatio(1.0)
        return pm
    scaled = pm.scaled(
        size,
        size,
        Qt.AspectRatioMode.KeepAspectRatio,
        Qt.TransformationMode.SmoothTransformation,
    )
    if scaled.width() == size and scaled.height() == size:
        scaled.setDevicePixelRatio(1.0)
        return scaled
    canvas = QPixmap(size, size)
    canvas.fill(Qt.GlobalColor.transparent)
    painter = QPainter(canvas)
    painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)
    painter.drawPixmap((size - scaled.width()) // 2, (size - scaled.height()) // 2, scaled)
    painter.end()
    canvas.setDevicePixelRatio(1.0)
    return canvas


def get_tray_icon(accent: str | None = None) -> QIcon:
    """Notification-area icon: identical BrandMark to the main-window sidebar."""
    from src.ui.brand_mark import brand_mark_icon

    icon = brand_mark_icon(accent)
    if icon.isNull():
        return get_app_icon(accent)
    return icon


def app_icon_is_hires() -> bool:
    """True when assets/app_icon.ico embeds sizes beyond a lone 16×16."""
    path = icon_path()
    if not path.is_file():
        return False
    try:
        raw = path.read_bytes()
    except OSError:
        return False
    if len(raw) < 1024:
        return False
    if len(raw) < 6:
        return False
    import struct

    _reserved, itype, count = struct.unpack_from("<HHH", raw, 0)
    return itype == 1 and count >= 4


def _normalize_clsid(raw: str) -> str:
    text = (raw or "").strip().upper()
    if text.startswith("::"):
        text = text[2:]
    if not text.startswith("{"):
        text = "{" + text.strip("{}") + "}"
    return text


def _shell_lookup_path(path: Path) -> str:
    """Resolve namespace shortcuts to ``::{CLSID}`` so they use the real shell icon."""
    try:
        if path.suffix.lower() != ".lnk":
            return str(path)
    except OSError:
        return str(path)

    try:
        from src.win_shell import get_lnk_namespace_clsid

        clsid = get_lnk_namespace_clsid(path)
        if clsid:
            return f"::{_normalize_clsid(clsid)}"
    except Exception:
        pass

    try:
        stem = path.stem.casefold()
    except OSError:
        return str(path)
    for name, clsid in _NAMESPACE_NAME_CLSID.items():
        if name in stem:
            return f"::{clsid}"
    return str(path)


def _hicon_to_pixmap(hicon) -> QPixmap:
    """Convert HICON to QPixmap preserving alpha."""
    if not hicon or not _ensure_shell():
        return QPixmap()

    class ICONINFO(ctypes.Structure):
        _fields_ = [
            ("fIcon", wintypes.BOOL),
            ("xHotspot", wintypes.DWORD),
            ("yHotspot", wintypes.DWORD),
            ("hbmMask", ctypes.c_void_p),
            ("hbmColor", ctypes.c_void_p),
        ]

    class BITMAP(ctypes.Structure):
        _fields_ = [
            ("bmType", ctypes.c_long),
            ("bmWidth", ctypes.c_long),
            ("bmHeight", ctypes.c_long),
            ("bmWidthBytes", ctypes.c_long),
            ("bmPlanes", wintypes.WORD),
            ("bmBitsPixel", wintypes.WORD),
            ("bmBits", ctypes.c_void_p),
        ]

    class BITMAPINFOHEADER(ctypes.Structure):
        _fields_ = [
            ("biSize", wintypes.DWORD),
            ("biWidth", ctypes.c_long),
            ("biHeight", ctypes.c_long),
            ("biPlanes", wintypes.WORD),
            ("biBitCount", wintypes.WORD),
            ("biCompression", wintypes.DWORD),
            ("biSizeImage", wintypes.DWORD),
            ("biXPelsPerMeter", ctypes.c_long),
            ("biYPelsPerMeter", ctypes.c_long),
            ("biClrUsed", wintypes.DWORD),
            ("biClrImportant", wintypes.DWORD),
        ]

    class BITMAPINFO(ctypes.Structure):
        _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", wintypes.DWORD * 3)]

    if isinstance(hicon, ctypes.c_void_p):
        handle = hicon.value
    else:
        handle = int(hicon)
    if not handle:
        return QPixmap()
    ii = ICONINFO()
    if not _user32.GetIconInfo(handle, ctypes.byref(ii)):
        return QPixmap()
    try:
        src = ii.hbmColor or ii.hbmMask
        if not src:
            return QPixmap()
        bm = BITMAP()
        if _gdi32.GetObjectW(src, ctypes.sizeof(bm), ctypes.byref(bm)) == 0:
            return QPixmap()
        w = int(bm.bmWidth)
        h = int(bm.bmHeight if ii.hbmColor else bm.bmHeight // 2)
        if w <= 0 or h <= 0:
            return QPixmap()

        hdc = _user32.GetDC(None)
        hdc_mem = _gdi32.CreateCompatibleDC(hdc)
        bmi = BITMAPINFO()
        bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bmi.bmiHeader.biWidth = w
        bmi.bmiHeader.biHeight = -h  # top-down
        bmi.bmiHeader.biPlanes = 1
        bmi.bmiHeader.biBitCount = 32
        bits = ctypes.c_void_p()
        hbmp = _gdi32.CreateDIBSection(
            hdc_mem, ctypes.byref(bmi), 0, ctypes.byref(bits), None, 0
        )
        if not hbmp or not bits.value:
            if hdc_mem:
                _gdi32.DeleteDC(hdc_mem)
            if hdc:
                _user32.ReleaseDC(None, hdc)
            return QPixmap()
        old = _gdi32.SelectObject(hdc_mem, hbmp)
        ctypes.memset(bits, 0, w * h * 4)
        # DI_NORMAL = 0x3
        _user32.DrawIconEx(hdc_mem, 0, 0, handle, w, h, 0, None, 0x0003)
        buf = (ctypes.c_char * (w * h * 4)).from_address(bits.value)
        img = QImage(buf, w, h, w * 4, QImage.Format.Format_ARGB32).copy()
        pix = QPixmap.fromImage(img)
        _gdi32.SelectObject(hdc_mem, old)
        _gdi32.DeleteObject(hbmp)
        _gdi32.DeleteDC(hdc_mem)
        _user32.ReleaseDC(None, hdc)
        return pix
    finally:
        if ii.hbmColor:
            _gdi32.DeleteObject(ii.hbmColor)
        if ii.hbmMask:
            _gdi32.DeleteObject(ii.hbmMask)


def _imagelist_icon(index: int, shil: int) -> QPixmap:
    if not _ensure_shell() or index < 0:
        return QPixmap()
    iml = ctypes.c_void_p()
    hr = _shell32.SHGetImageList(shil, ctypes.byref(_IID_IImageList), ctypes.byref(iml))
    if hr != 0 or not iml:
        return QPixmap()
    pvtbl = ctypes.cast(iml, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    GetIcon = ctypes.CFUNCTYPE(
        ctypes.HRESULT,
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_uint,
        ctypes.POINTER(ctypes.c_void_p),
    )(pvtbl[10])
    Release = ctypes.CFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(pvtbl[2])
    hicon = ctypes.c_void_p()
    try:
        hr2 = GetIcon(iml, int(index), 0, ctypes.byref(hicon))
        if hr2 != 0 or not hicon:
            return QPixmap()
        return _hicon_to_pixmap(hicon)
    finally:
        if hicon:
            _user32.DestroyIcon(hicon)
        Release(iml)


def _sys_icon_index(lookup: str) -> int:
    """Return system image-list index for a filesystem path or ``::{CLSID}``."""
    if not _ensure_shell():
        return -1
    info = _SHFILEINFOW()
    if lookup.startswith("::"):
        pidl = ctypes.c_void_p()
        attrs = wintypes.DWORD()
        hr = _shell32.SHParseDisplayName(
            lookup, None, ctypes.byref(pidl), 0, ctypes.byref(attrs)
        )
        if hr != 0 or not pidl:
            return -1
        try:
            result = _shell32.SHGetFileInfoW(
                pidl,
                0,
                ctypes.byref(info),
                ctypes.sizeof(info),
                _SHGFI_SYSICONINDEX | _SHGFI_PIDL,
            )
            return int(info.iIcon) if result else -1
        finally:
            _shell32.ILFree(pidl)
    result = _shell32.SHGetFileInfoW(
        lookup, 0, ctypes.byref(info), ctypes.sizeof(info), _SHGFI_SYSICONINDEX
    )
    if result:
        return int(info.iIcon)
    # Missing / inaccessible path: SHGetFileInfo fails with iIcon=0 (blank
    # document). Fall back to the extension association — same as Explorer
    # for sticky pins and briefly-missing Office files.
    info2 = _SHFILEINFOW()
    result2 = _shell32.SHGetFileInfoW(
        lookup,
        _FILE_ATTRIBUTE_NORMAL,
        ctypes.byref(info2),
        ctypes.sizeof(info2),
        _SHGFI_SYSICONINDEX | _SHGFI_USEFILEATTRIBUTES,
    )
    return int(info2.iIcon) if result2 else -1


def _sys_icon_index_cached(lookup: str) -> int:
    key = lookup.casefold()
    with _SHELL_INDEX_LOCK:
        cached = _SHELL_INDEX_CACHE.get(key)
    if cached is not None:
        return cached
    idx = _sys_icon_index(lookup)
    with _SHELL_INDEX_LOCK:
        if len(_SHELL_INDEX_CACHE) >= _MAX_SHELL_INDEX_CACHE:
            try:
                _SHELL_INDEX_CACHE.pop(next(iter(_SHELL_INDEX_CACHE)))
            except StopIteration:
                pass
        _SHELL_INDEX_CACHE[key] = idx
    return idx


def prefetch_shell_icon_lookups(paths: list[Path | str]) -> None:
    """Warm shell icon indices off the UI thread (best-effort, non-blocking)."""
    if sys.platform != "win32" or not paths:
        return
    todo: list[str] = []
    for raw in paths:
        try:
            lookup = _shell_lookup_path(Path(raw))
        except (TypeError, ValueError, OSError):
            continue
        key = lookup.casefold()
        with _SHELL_INDEX_LOCK:
            if key in _SHELL_INDEX_CACHE:
                continue
        todo.append(lookup)
    if not todo:
        return

    def _worker() -> None:
        for lookup in todo:
            try:
                _sys_icon_index_cached(lookup)
            except Exception:
                continue

    threading.Thread(
        target=_worker, name="desktidy-icon-index", daemon=True
    ).start()


def _shil_candidates(size: int) -> tuple[int, ...]:
    """Pick ImageList sizes; prefer a source >= target to avoid soft upscales."""
    size = max(16, int(size or 48))
    if size <= 32:
        return (_SHIL_LARGE, _SHIL_EXTRALARGE, _SHIL_JUMBO)
    if size <= 48:
        return (_SHIL_EXTRALARGE, _SHIL_LARGE, _SHIL_JUMBO)
    # 64+ (fence/public default): downscale from jumbo — 48→64 upscale looks soft.
    return (_SHIL_JUMBO, _SHIL_EXTRALARGE, _SHIL_LARGE)


def _opaque_bounds(pixmap: QPixmap, alpha_min: int = 16) -> tuple[int, int, int, int] | None:
    """Return ``(x, y, w, h)`` of non-transparent content, or None if empty."""
    if pixmap.isNull():
        return None
    img = pixmap.toImage()
    if img.format() != QImage.Format.Format_ARGB32:
        img = img.convertToFormat(QImage.Format.Format_ARGB32)
    w = img.width()
    h = img.height()
    if w <= 0 or h <= 0:
        return None
    # Little-endian ARGB32 memory layout is BGRA (alpha at +3).
    data = img.constBits().asarray(img.sizeInBytes())
    bpl = img.bytesPerLine()
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        row = y * bpl
        for x in range(w):
            if data[row + x * 4 + 3] > alpha_min:
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    if maxx < 0:
        return None
    return (minx, miny, maxx - minx + 1, maxy - miny + 1)


def _trim_icon_padding(pixmap: QPixmap) -> QPixmap:
    """Crop sparse shell jumbo padding so the glyph fills the canvas.

    Some shortcuts (e.g. only a 32px .ico) expose a 256×256 ImageList glyph that
    is mostly transparent with a tiny bitmap in the corner — scaling that to 64px
    leaves a speck. Trim first, then scale.
    """
    if pixmap.isNull():
        return pixmap
    # Transparent shell padding can also be visible on "medium" (48px) icons
    # depending on the specific shortcut / ImageList variant.
    # We still keep the "glyph already fills most of the canvas" early-return
    # below, so this stays safe for icons that already look tight.
    if max(pixmap.width(), pixmap.height()) < 48:
        return pixmap
    bounds = _opaque_bounds(pixmap)
    if bounds is None:
        return pixmap
    x, y, bw, bh = bounds
    canvas_area = max(1, pixmap.width() * pixmap.height())
    # Skip when the glyph already fills most of the canvas.
    if bw * bh >= canvas_area * 0.40 and bw >= pixmap.width() * 0.70 and bh >= pixmap.height() * 0.70:
        return pixmap
    margin = 1
    x0 = max(0, x - margin)
    y0 = max(0, y - margin)
    x1 = min(pixmap.width(), x + bw + margin)
    y1 = min(pixmap.height(), y + bh + margin)
    cropped = pixmap.copy(x0, y0, max(1, x1 - x0), max(1, y1 - y0))
    cropped.setDevicePixelRatio(1.0)
    return cropped


def _shell_icon_pixmap(lookup: str, size: int = 64) -> QPixmap:
    """Best-effort shell icon; caches the largest *effective* glyph per lookup."""
    cache_key = lookup.casefold()
    target = max(16, int(size or 48))
    cached = _SHELL_RAW_CACHE.get(cache_key)
    if cached is not None and not cached.isNull():
        _SHELL_RAW_CACHE.move_to_end(cache_key)
        # Cached values are already trimmed; reuse when large enough.
        if min(cached.width(), cached.height()) >= target:
            return cached
        # else fall through and try to upgrade to a larger ImageList size

    pix = QPixmap()
    best_score = -1
    idx = _sys_icon_index_cached(lookup)
    if idx >= 0:
        for shil in _shil_candidates(target):
            candidate = _imagelist_icon(idx, shil)
            if candidate.isNull() or candidate.width() < 16:
                continue
            trimmed = _trim_icon_padding(candidate)
            score = min(trimmed.width(), trimmed.height())
            if score > best_score:
                pix = trimmed
                best_score = score
            # Stop once effective glyph is >= target (not the padded canvas).
            if score >= target:
                break
    if pix.isNull() and _ensure_shell():
        info = _SHFILEINFOW()
        flags = _SHGFI_ICON | _SHGFI_LARGEICON
        got_icon = False
        if lookup.startswith("::"):
            pidl = ctypes.c_void_p()
            attrs = wintypes.DWORD()
            hr = _shell32.SHParseDisplayName(
                lookup, None, ctypes.byref(pidl), 0, ctypes.byref(attrs)
            )
            if hr == 0 and pidl:
                try:
                    got_icon = bool(
                        _shell32.SHGetFileInfoW(
                            pidl,
                            0,
                            ctypes.byref(info),
                            ctypes.sizeof(info),
                            flags | _SHGFI_PIDL,
                        )
                    )
                finally:
                    _shell32.ILFree(pidl)
        else:
            got_icon = bool(
                _shell32.SHGetFileInfoW(
                    lookup, 0, ctypes.byref(info), ctypes.sizeof(info), flags
                )
            )
            if not got_icon:
                info = _SHFILEINFOW()
                got_icon = bool(
                    _shell32.SHGetFileInfoW(
                        lookup,
                        _FILE_ATTRIBUTE_NORMAL,
                        ctypes.byref(info),
                        ctypes.sizeof(info),
                        flags | _SHGFI_USEFILEATTRIBUTES,
                    )
                )
        if got_icon and info.hIcon:
            try:
                pix = _trim_icon_padding(_hicon_to_pixmap(info.hIcon))
            finally:
                _user32.DestroyIcon(info.hIcon)

    if not pix.isNull():
        pix = _trim_icon_padding(pix)
        old = _SHELL_RAW_CACHE.get(cache_key)
        if (
            old is None
            or old.isNull()
            or min(pix.width(), pix.height()) >= min(old.width(), old.height())
        ):
            _SHELL_RAW_CACHE[cache_key] = pix
        _SHELL_RAW_CACHE.move_to_end(cache_key)
        while len(_SHELL_RAW_CACHE) > _MAX_SHELL_RAW_CACHE:
            _SHELL_RAW_CACHE.popitem(last=False)
        return _SHELL_RAW_CACHE[cache_key]
    return pix


def _qt_provider_pixmap(lookup: str, size: int) -> QPixmap:
    """Fallback: QFileIconProvider, preferring the largest available glyph."""
    icon = _FILE_ICON_PROVIDER.icon(QFileInfo(lookup))
    if icon.isNull():
        return QPixmap()
    sizes = icon.availableSizes()
    if sizes:
        best = max(sizes, key=lambda s: s.width() * s.height())
        pix = icon.pixmap(best)
    else:
        pix = icon.pixmap(max(size, 256), max(size, 256))
    if pix.isNull():
        return QPixmap()
    # Drop HiDPI metadata so physical pixels are treated as the bitmap size.
    if pix.devicePixelRatio() != 1.0:
        normalized = QPixmap(pix)
        normalized.setDevicePixelRatio(1.0)
        return normalized
    return pix


def _fit_pixmap(pixmap: QPixmap, size: int) -> QPixmap:
    """Scale and center into a square ``size``×``size`` canvas (DPR 1)."""
    if pixmap.isNull():
        return pixmap
    if pixmap.devicePixelRatio() != 1.0:
        pixmap = QPixmap(pixmap)
        pixmap.setDevicePixelRatio(1.0)
    # Safety net for callers that bypass shell trim.
    pixmap = _trim_icon_padding(pixmap)
    if pixmap.width() == size and pixmap.height() == size:
        return pixmap
    # Prefer Fast when downscaling by a near-integer factor (sharper glyphs).
    src = max(pixmap.width(), pixmap.height())
    mode = Qt.TransformationMode.SmoothTransformation
    if src > size:
        ratio = src / float(size)
        if abs(ratio - round(ratio)) < 0.08:
            mode = Qt.TransformationMode.FastTransformation
    scaled = pixmap.scaled(
        size,
        size,
        Qt.AspectRatioMode.KeepAspectRatio,
        mode,
    )
    if scaled.width() == size and scaled.height() == size:
        scaled.setDevicePixelRatio(1.0)
        return scaled
    canvas = QPixmap(size, size)
    canvas.fill(Qt.GlobalColor.transparent)
    painter = QPainter(canvas)
    painter.setRenderHint(
        QPainter.RenderHint.SmoothPixmapTransform,
        mode == Qt.TransformationMode.SmoothTransformation,
    )
    x = (size - scaled.width()) // 2
    y = (size - scaled.height()) // 2
    painter.drawPixmap(x, y, scaled)
    painter.end()
    canvas.setDevicePixelRatio(1.0)
    return canvas


def _screen_device_pixel_ratio() -> float:
    """Primary-screen DPR for sharp fence icons on HiDPI displays."""
    try:
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance()
        if app is not None:
            screen = app.primaryScreen()
            if screen is not None:
                return max(1.0, float(screen.devicePixelRatio()))
    except Exception:
        pass
    return 1.0


def file_icon_pixmap(path: Path | str, size: int) -> QPixmap:
    """Return a cached Windows shell icon pixmap for a path.

    On HiDPI screens the bitmap is fetched at physical size and tagged with
    devicePixelRatio so QLabel shows a sharp glyph at the logical ``size``.
    """
    size = max(8, int(size))
    dpr = round(_screen_device_pixel_ratio(), 2)
    try:
        key_path = str(path).casefold()
    except OSError:
        key_path = str(path)
    cache_key = (key_path, size, dpr)
    cached = _FILE_PIXMAP_CACHE.get(cache_key)
    if cached is not None and not cached.isNull():
        _FILE_PIXMAP_CACHE.move_to_end(cache_key)
        return cached

    phys = max(size, int(round(size * dpr)))
    p = Path(path)
    # DeskNote.lnk → always product ICO (never pythonw / blank-document shell glyph).
    try:
        from src.desknote import is_desknote_shortcut

        if is_desknote_shortcut(p):
            pixmap = _desknote_product_pixmap(phys)
            if not pixmap.isNull():
                if dpr != 1.0:
                    pixmap.setDevicePixelRatio(dpr)
                _FILE_PIXMAP_CACHE[cache_key] = pixmap
                _FILE_PIXMAP_CACHE.move_to_end(cache_key)
                while len(_FILE_PIXMAP_CACHE) > _MAX_FILE_PIXMAP_CACHE:
                    _FILE_PIXMAP_CACHE.popitem(last=False)
                return pixmap
    except Exception:
        pass
    lookup = _shell_lookup_path(p)
    pixmap = _shell_icon_pixmap(lookup, phys)
    if pixmap.isNull():
        pixmap = _qt_provider_pixmap(lookup, phys)
    if pixmap.isNull() and lookup != str(p):
        pixmap = _qt_provider_pixmap(str(p), phys)
    pixmap = _fit_pixmap(pixmap, phys)
    if not pixmap.isNull() and dpr != 1.0:
        pixmap.setDevicePixelRatio(dpr)
    _FILE_PIXMAP_CACHE[cache_key] = pixmap
    _FILE_PIXMAP_CACHE.move_to_end(cache_key)
    while len(_FILE_PIXMAP_CACHE) > _MAX_FILE_PIXMAP_CACHE:
        _FILE_PIXMAP_CACHE.popitem(last=False)
    return pixmap


_CANONICAL_ICON_SIZES = (48, 64, 96, 128)
_DISPLAY_PIXMAP_CACHE: OrderedDict = OrderedDict()
# Keep modest for 24/7 tray residency; trim_display_icon_cache halves further in bg.
_MAX_DISPLAY_PIXMAP_CACHE = 256


def display_file_icon_pixmap(path: Path | str, size: int) -> QPixmap:
    """Return a pixmap at ``size`` using a cached canonical shell fetch + scale.

    Zoom and resize should call this instead of ``file_icon_pixmap`` with arbitrary
    sizes so we avoid repeated shell extraction on every pixel step.
    """
    size = max(8, int(size))
    dpr = round(_screen_device_pixel_ratio(), 2)
    try:
        key_path = str(path).casefold()
    except OSError:
        key_path = str(path)
    cache_key = (key_path, size, dpr)
    cached = _DISPLAY_PIXMAP_CACHE.get(cache_key)
    if cached is not None and not cached.isNull():
        _DISPLAY_PIXMAP_CACHE.move_to_end(cache_key)
        return cached

    fetch = _CANONICAL_ICON_SIZES[-1]
    for candidate in _CANONICAL_ICON_SIZES:
        if candidate >= size:
            fetch = candidate
            break
    base = file_icon_pixmap(path, fetch)
    if size == fetch or base.isNull():
        pixmap = base
    else:
        dpr_val = max(1.0, float(base.devicePixelRatio() or 1.0))
        target_phys = max(size, int(round(size * dpr_val)))
        scaled = base.scaled(
            target_phys,
            target_phys,
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        if not scaled.isNull():
            scaled.setDevicePixelRatio(dpr_val)
        pixmap = scaled
    if not pixmap.isNull():
        _DISPLAY_PIXMAP_CACHE[cache_key] = pixmap
        _DISPLAY_PIXMAP_CACHE.move_to_end(cache_key)
        while len(_DISPLAY_PIXMAP_CACHE) > _MAX_DISPLAY_PIXMAP_CACHE:
            _DISPLAY_PIXMAP_CACHE.popitem(last=False)
    return pixmap


def invalidate_file_icon_cache(path: Path | str | None = None) -> None:
    """Drop one path from the icon cache, or clear everything."""
    if path is None:
        _FILE_PIXMAP_CACHE.clear()
        _SHELL_RAW_CACHE.clear()
        _DISPLAY_PIXMAP_CACHE.clear()
        with _SHELL_INDEX_LOCK:
            _SHELL_INDEX_CACHE.clear()
        return
    try:
        key_path = str(path).casefold()
    except OSError:
        key_path = str(path)
    for key in list(_FILE_PIXMAP_CACHE.keys()):
        if key[0] == key_path:
            _FILE_PIXMAP_CACHE.pop(key, None)
    for key in list(_DISPLAY_PIXMAP_CACHE.keys()):
        if key[0] == key_path:
            _DISPLAY_PIXMAP_CACHE.pop(key, None)
    # Also drop shell raw cache entries for this path / namespace lookup.
    try:
        lookup = _shell_lookup_path(Path(path)).casefold()
    except Exception:
        lookup = key_path
    _SHELL_RAW_CACHE.pop(key_path, None)
    _SHELL_RAW_CACHE.pop(lookup, None)
    with _SHELL_INDEX_LOCK:
        _SHELL_INDEX_CACHE.pop(key_path, None)
        _SHELL_INDEX_CACHE.pop(lookup, None)


def trim_display_icon_cache(*, keep: int | None = None) -> None:
    """LRU-trim display pixmaps (tray/background memory pressure).

    Does not drop shell/file extraction caches — those are expensive to rebuild.
    """
    target = int(keep) if keep is not None else max(32, _MAX_DISPLAY_PIXMAP_CACHE // 2)
    while len(_DISPLAY_PIXMAP_CACHE) > target:
        _DISPLAY_PIXMAP_CACHE.popitem(last=False)
