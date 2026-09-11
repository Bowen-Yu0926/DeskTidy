"""Snapshot visible top-level window rects for WeChat-style snip hover.

Freeze-frame capture cannot use live ``WindowFromPoint`` (the overlay owns
the cursor). Market snip tools (WeChat / Snipaste) enumerate window bounds
at grab time, then hit-test those rects under the overlay.

Hit-testing follows **Z-order** (``EnumWindows`` topmost-first), not “smallest
rect wins”. Smallest-among-overlapping top-levels wrongly picks a window
*behind* the one under the cursor (e.g. AI助手 under DeskTidy settings).
"""

from __future__ import annotations

from PyQt6.QtCore import QPoint, QRect

# Skip chrome that is never a useful snip target.
_SKIP_CLASSES = frozenset(
    {
        "Shell_TrayWnd",
        "Shell_SecondaryTrayWnd",
        "Progman",
        "WorkerW",
        "ForegroundStaging",
        "Windows.UI.Core.CoreWindow",
    }
)

# DeskTidy desktop-band / overlay classes — not snip targets (keep settings).
_SKIP_OWN_CLASSES = frozenset(
    {
        "ScreenshotOverlay",
        "FenceWidget",
        "PublicIconHost",
        "PetWidget",
        "PageIndicator",
        "PinnedImageWidget",
    }
)

# DWMWA_CLOAKED — UWP / virtual-desktop ghosts stay IsWindowVisible=True.
_DWMWA_CLOAKED = 14


def _is_window_cloaked(hwnd: int) -> bool:
    """True when DWM has cloaked the window (looks empty on desktop)."""
    try:
        import ctypes
        from ctypes import wintypes

        value = wintypes.DWORD()
        hr = ctypes.windll.dwmapi.DwmGetWindowAttribute(
            int(hwnd),
            _DWMWA_CLOAKED,
            ctypes.byref(value),
            ctypes.sizeof(value),
        )
        return hr == 0 and int(value.value) != 0
    except Exception:
        return False


def _is_snip_window_candidate(hwnd: int) -> bool:
    """Whether *hwnd* should be a WeChat-style snip hover target."""
    try:
        import win32con
        import win32gui
    except ImportError:
        return False

    hwnd = int(hwnd)
    if not hwnd or not win32gui.IsWindowVisible(hwnd):
        return False
    if win32gui.IsIconic(hwnd):
        return False
    # Cloaked UWP / other-desktop windows look like empty wallpaper.
    if _is_window_cloaked(hwnd):
        return False
    try:
        ex = int(win32gui.GetWindowLong(hwnd, win32con.GWL_EXSTYLE))
    except OSError:
        ex = 0
    if ex & win32con.WS_EX_TRANSPARENT:
        return False
    if ex & win32con.WS_EX_TOOLWINDOW:
        # Keep some utility panels; skip pure tool tips / ghosts.
        if not (ex & win32con.WS_EX_APPWINDOW):
            return False
    # Fully transparent layered windows (alpha 0) are not visible targets.
    if ex & win32con.WS_EX_LAYERED:
        try:
            import ctypes
            from ctypes import wintypes

            color = wintypes.COLORREF()
            alpha = wintypes.BYTE()
            flags = wintypes.DWORD()
            ok = ctypes.windll.user32.GetLayeredWindowAttributes(
                hwnd, ctypes.byref(color), ctypes.byref(alpha), ctypes.byref(flags)
            )
            lwa_alpha = 0x2
            if ok and (int(flags.value) & lwa_alpha) and int(alpha.value) == 0:
                return False
        except Exception:
            pass
    try:
        class_name = win32gui.GetClassName(hwnd) or ""
    except OSError:
        class_name = ""
    if class_name in _SKIP_CLASSES:
        return False
    if class_name == "ApplicationFrameWindow":
        try:
            title = win32gui.GetWindowText(hwnd) or ""
        except OSError:
            title = ""
        if not title.strip():
            return False
    return True


def snapshot_snip_window_rects(
    *,
    desk_origin: QPoint | None = None,
    desk_size: tuple[int, int] | None = None,
) -> list[QRect]:
    """Return visible top-level window rects in *overlay-local* coordinates.

    ``desk_origin`` is the virtual-desktop top-left (same as ``grab_desktop``).
    When *desk_size* is set, rects are clipped to that logical desktop.

    Order is **Z-order topmost-first** (``EnumWindows``), so hover hit-tests
    the window actually under the cursor — not a smaller window behind it.
    """
    try:
        import win32gui
    except ImportError:
        return []

    origin = desk_origin if desk_origin is not None else QPoint(0, 0)
    ox, oy = int(origin.x()), int(origin.y())
    clip: QRect | None = None
    if desk_size is not None:
        clip = QRect(0, 0, int(desk_size[0]), int(desk_size[1]))

    own_skip = _snip_exclude_root_hwnds()
    rects: list[QRect] = []
    seen: set[tuple[int, int, int, int]] = set()

    def _enum(hwnd: int, _ctx) -> bool:
        try:
            hwnd = int(hwnd)
            if not _is_snip_window_candidate(hwnd):
                return True
            try:
                root = int(win32gui.GetAncestor(hwnd, 2) or hwnd)
            except OSError:
                root = hwnd
            if root in own_skip or hwnd in own_skip:
                return True
            left, top, right, bottom = win32gui.GetWindowRect(hwnd)
            w = int(right - left)
            h = int(bottom - top)
            if w < 24 or h < 24:
                return True
            try:
                class_name = win32gui.GetClassName(hwnd) or ""
            except OSError:
                class_name = ""
            # Full-screen desktop shells often match virtual desktop — skip
            # when they cover almost the entire capture area.
            if clip is not None and w >= clip.width() - 4 and h >= clip.height() - 4:
                if class_name in ("Progman", "WorkerW", "Shell_TrayWnd"):
                    return True
            local = QRect(int(left) - ox, int(top) - oy, w, h)
            if clip is not None:
                local = local.intersected(clip)
            if local.width() < 24 or local.height() < 24:
                return True
            key = (local.x(), local.y(), local.width(), local.height())
            if key in seen:
                return True
            seen.add(key)
            rects.append(local)
        except OSError:
            return True
        return True

    try:
        win32gui.EnumWindows(_enum, None)
    except OSError:
        return []

    # Keep EnumWindows order (topmost first). Do not sort by area.
    return rects


def hit_test_snip_window(
    rects: list[QRect], local_pos: QPoint
) -> QRect | None:
    """First Z-order rect in *rects* that contains *local_pos* (topmost wins)."""
    for rect in rects:
        if rect.contains(local_pos):
            return QRect(rect)
    return None


def expand_rect_to_nearby_window(
    selection: QRect,
    rects: list[QRect],
    *,
    edge_slop: int = 28,
) -> QRect:
    """If *selection* is mostly inside / near a window, expand to that window.

    Used on drag-release so a rough drag snaps to window bounds (一期「自动扩」).
    Prefer higher coverage; on a tie prefer earlier *rects* (Z-order topmost).
    """
    if selection.isNull() or selection.width() < 2 or selection.height() < 2:
        return QRect(selection)
    sel = selection.normalized()
    best: QRect | None = None
    best_score = 0.0
    sel_area = max(1, sel.width() * sel.height())
    for rect in rects:
        inter = sel.intersected(rect)
        if inter.isEmpty():
            # Near-edge: selection center inside window, or edges within slop.
            center = sel.center()
            if not rect.adjusted(-edge_slop, -edge_slop, edge_slop, edge_slop).contains(
                center
            ):
                continue
            # Prefer windows that almost contain the selection.
            padded = rect.adjusted(-edge_slop, -edge_slop, edge_slop, edge_slop)
            if not padded.contains(sel.topLeft()) and not padded.contains(
                sel.bottomRight()
            ):
                continue
            score = 0.35
        else:
            inter_area = inter.width() * inter.height()
            score = inter_area / float(sel_area)
            # Also reward selections that cover most of a small window.
            win_area = max(1, rect.width() * rect.height())
            score = max(score, inter_area / float(win_area) * 0.85)
        # Strictly better score wins; equal score keeps the earlier (topmost) hit.
        if score > best_score + 1e-6:
            best_score = score
            best = QRect(rect)
    if best is not None and best_score >= 0.45:
        return best
    return QRect(sel)


def _snip_exclude_root_hwnds() -> set[int]:
    """DeskTidy chrome to skip — settings / notepad stay snippable (WeChat-like)."""
    roots: set[int] = set()
    try:
        from PyQt6.QtCore import Qt
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance()
        if app is None:
            return roots
        for top in app.topLevelWidgets():
            try:
                if not top.isVisible():
                    continue
                name = top.__class__.__name__
                flags = top.windowFlags()
            except RuntimeError:
                continue
            # Capture overlay / desktop-band widgets are never snip targets.
            skip = name in _SKIP_OWN_CLASSES
            if not skip:
                is_tool = bool(flags & Qt.WindowType.Tool)
                is_popup = bool(flags & Qt.WindowType.Popup)
                is_splash = bool(flags & Qt.WindowType.SplashScreen)
                # Tool/Popup chrome (toasts, search, fences) — not MainWindow.
                skip = is_tool or is_popup or is_splash
            if not skip:
                continue
            try:
                hwnd = int(top.winId()) if top.winId() else 0
            except Exception:
                continue
            if not hwnd:
                continue
            try:
                import win32gui

                roots.add(int(win32gui.GetAncestor(hwnd, 2) or hwnd))
            except Exception:
                roots.add(hwnd)
            roots.add(hwnd)
    except Exception:
        pass
    return roots


# Back-compat alias for older call sites / tests.
_own_tool_root_hwnds = _snip_exclude_root_hwnds
