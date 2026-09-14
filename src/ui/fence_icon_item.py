"""Fence item with Windows file icon and label."""

from __future__ import annotations

import struct
import time
from contextlib import contextmanager
from pathlib import Path

from PyQt6.QtCore import (
    QByteArray,
    QEvent,
    QEventLoop,
    QMimeData,
    QObject,
    QPoint,
    QRect,
    QTimer,
    Qt,
    QUrl,
    pyqtSignal,
)
from PyQt6.QtGui import QColor, QCursor, QDrag, QKeySequence, QPainter, QPen, QPixmap
from PyQt6.QtWidgets import (
    QApplication,
    QFrame,
    QLabel,
    QLineEdit,
    QMenu,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)
from src.icon_utils import display_file_icon_pixmap, file_icon_pixmap
from src.win_shell import (
    DESKTIDY_SOURCE_FENCE_MIME,
    DESKTIDY_VIRTUAL_MIME,
    delete_to_trash,
    open_containing_folder,
    open_path,
    preferred_drop_action_for_mime,
    reveal_in_explorer,
)


def fit_desktop_caption_label(label: QLabel, width: int, *, max_lines: int = 2) -> None:
    """Lock caption to a fixed ``max_lines`` shelf (default 2 = Explorer desktop).

    Every unselected icon cell shares one height; text is pre-elided with
    explicit ``\\n`` breaks — do **not** WordWrap again (that recreates 孤字
    and clips a third line). Selected icons may pass a larger ``max_lines``.
    """
    from src.desktop_caption import caption_box_height

    width = max(16, int(width))
    label.setFixedWidth(width)
    # Pre-elided captions already contain line breaks; wrapping again at a
    # slightly different width produced 「同」 alone + a clipped third row.
    label.setWordWrap(False)
    label.setAlignment(Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop)
    max_lines = max(1, int(max_lines))
    caption_h = caption_box_height(label.font(), max_lines=max_lines, extra_pad=10)
    # No contentsMargins here — stylesheet padding on ``#fenceItem`` already
    # reserves descent room; stacking margins clipped line 2 inside FixedHeight.
    label.setContentsMargins(0, 0, 0, 0)
    label.setMinimumHeight(caption_h)
    label.setMaximumHeight(caption_h)
    label.setFixedHeight(caption_h)

_DROPEFFECT_MOVE = 2
_DROPEFFECT_COPY = 1
DRAG_PIXMAP_PAD = 3
_last_virtual_unpin_pos: QPoint | None = None
PUBLIC_SOURCE_FENCE_ID = "__public__"
# GUI-thread flag read by the LL keyboard hook (copy/cut without cursor-on-HWND).
_desktop_selection_live = False
# Explorer-like cut ghost: paths currently Preferred DropEffect=MOVE on the clipboard.
_CUT_GHOST_OPACITY = 0.42
_cut_path_keys: set[str] = set()
_cut_clipboard_hooked = False


def _norm_cut_path_key(path: Path | str) -> str:
    try:
        return str(Path(path)).casefold().replace("/", "\\")
    except OSError:
        return str(path).casefold().replace("/", "\\")


def path_is_clipboard_cut(path: Path | str) -> bool:
    """True when *path* is armed for cut (clipboard MOVE effect)."""
    return _norm_cut_path_key(path) in _cut_path_keys


def apply_cut_ghost_visual(widget: QWidget) -> None:
    """Dim an icon like Explorer after Ctrl+X; clear when not cut."""
    from PyQt6.QtWidgets import QGraphicsOpacityEffect

    path = getattr(widget, "file_path", None)
    cut = path is not None and path_is_clipboard_cut(path)
    try:
        current = widget.graphicsEffect()
    except RuntimeError:
        return
    if cut:
        if not isinstance(current, QGraphicsOpacityEffect):
            effect = QGraphicsOpacityEffect(widget)
            widget.setGraphicsEffect(effect)
            current = effect
        current.setOpacity(_CUT_GHOST_OPACITY)
        return
    if isinstance(current, QGraphicsOpacityEffect):
        widget.setGraphicsEffect(None)


def _iter_live_desktop_icon_widgets() -> list[QWidget]:
    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app is not None else None
    out: list[QWidget] = []
    if desk is None:
        return out
    for icon in list(getattr(desk, "public_icons", []) or []):
        out.append(icon)
    parked = getattr(desk, "_parked_public_icons", None)
    if isinstance(parked, dict):
        out.extend(list(parked.values()))
    for fence in list(getattr(desk, "fences", []) or []):
        try:
            out.extend(iter_fence_selectable_items(fence))
        except Exception:
            continue
    return out


def refresh_clipboard_cut_ghost_visuals() -> None:
    """Re-apply / clear cut dimming on every live public + fence icon."""
    for widget in _iter_live_desktop_icon_widgets():
        try:
            apply_cut_ghost_visual(widget)
        except RuntimeError:
            continue


def set_clipboard_cut_paths(paths: list[Path] | None) -> None:
    """Remember cut paths for Explorer-like ghosting; None/[] clears."""
    global _cut_path_keys
    if not paths:
        next_keys: set[str] = set()
    else:
        next_keys = {_norm_cut_path_key(p) for p in paths}
    if next_keys == _cut_path_keys:
        return
    _cut_path_keys = next_keys
    refresh_clipboard_cut_ghost_visuals()


def sync_cut_ghost_from_clipboard() -> None:
    """Match ghost state to CF_HDROP + Preferred DropEffect (DeskTidy or Explorer)."""
    try:
        from src.shell_clipboard import DROPEFFECT_MOVE, clipboard_get_files_with_effect

        files, effect = clipboard_get_files_with_effect()
        if effect == DROPEFFECT_MOVE and files:
            set_clipboard_cut_paths(files)
        else:
            set_clipboard_cut_paths(None)
    except Exception:
        set_clipboard_cut_paths(None)


def ensure_cut_ghost_clipboard_sync() -> None:
    """Hook clipboard changes once so Explorer Ctrl+X also dims our floats."""
    global _cut_clipboard_hooked
    if _cut_clipboard_hooked:
        return
    app = QApplication.instance()
    if app is None:
        return
    try:
        app.clipboard().dataChanged.connect(sync_cut_ghost_from_clipboard)
    except Exception:
        return
    _cut_clipboard_hooked = True


def scrub_live_icons_for_claimed_paths(paths: list[Path]) -> None:
    """Drop stale public floats + peer fence cells after pins claim *paths*.

    ``assign_paths_to_virtual_fence`` already clears settings membership; quiet
    paste used to skip UI scrub and left desktop ghosts until sticky prune.
    Only removes fence cells that are no longer pinned on that fence (keeps
    the destination fence's newly claimed icons).
    """
    if not paths:
        return
    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app is not None else None
    if desk is None:
        return
    rem_pub = getattr(desk, "_remove_public_icon_widget", None)
    if callable(rem_pub):
        for path in paths:
            try:
                rem_pub(Path(path))
            except Exception:
                continue
    from src.fence_rules import _norm_virtual_key

    def _pk(raw: Path | str) -> str:
        try:
            return _norm_virtual_key(Path(raw)).casefold().replace("/", "\\")
        except OSError:
            return str(raw).casefold().replace("/", "\\")

    claimed = {_pk(p) for p in paths}
    for fence in list(getattr(desk, "fences", []) or []):
        cfg = getattr(fence, "config", None)
        if not isinstance(cfg, dict):
            continue
        pin_keys = {_pk(str(x)) for x in (cfg.get("virtual_items") or [])}
        drop = [Path(p) for p in paths if _pk(p) not in pin_keys]
        if not drop:
            continue
        remove = getattr(fence, "_remove_virtual_icon_widgets", None)
        if not callable(remove):
            continue
        try:
            remove(drop)
        except Exception:
            continue
    # Cut ghost: clear keys that left the public surface (still may live in fence).
    try:
        if claimed & _cut_path_keys:
            # Keep cut ghost on fence icons until demote / clipboard change.
            refresh_clipboard_cut_ghost_visuals()
    except Exception:
        pass

# Qt / Windows clipboard formats many third-party apps probe for file drops.
_WIN_FILENAME_W = 'application/x-qt-windows-mime;value="FileNameW"'
_WIN_FILENAME = 'application/x-qt-windows-mime;value="FileName"'


def resolve_external_drag_path(path: Path) -> Path | None:
    """Filesystem path to offer external drop targets (chat, Office, browsers…).

    Resolves ``.lnk`` to the real target when possible. Namespace shortcuts
    (This PC / Recycle Bin) cannot be dropped into other apps — returns None.
    """
    try:
        p = Path(path)
        if not p.exists():
            return None
        if p.suffix.lower() != ".lnk":
            return p
        from src.win_shell import get_lnk_namespace_clsid

        if get_lnk_namespace_clsid(p):
            return None
        try:
            from src.fence_rules import _lnk_target_file

            target = _lnk_target_file(p)
            if target is not None:
                try:
                    if target.exists():
                        return target
                except OSError:
                    pass
        except Exception:
            pass
        # Fall back to the shortcut file itself.
        return p
    except OSError:
        return None


def attach_external_file_drag_payload(
    mime: QMimeData, path: Path | list[Path]
) -> bool:
    """Attach Explorer/WeChat drag formats (CF_HDROP + companions, no URL mime)."""
    paths = [path] if isinstance(path, Path) else [p for p in path if p]
    resolved: list[Path] = []
    for raw in paths:
        p = resolve_external_drag_path(Path(raw))
        if p is not None:
            resolved.append(p)
    if not resolved:
        return False
    from src.shell_clipboard import attach_shell_file_drag_mime

    return attach_shell_file_drag_mime(mime, resolved, copy=True)


def last_virtual_unpin_pos() -> QPoint:
    """Global top-left anchor from the most recent fence/public drag release."""
    if _last_virtual_unpin_pos is not None:
        return QPoint(_last_virtual_unpin_pos)
    return QCursor.pos()


def _drag_hotspot(widget: QWidget, press_local: QPoint | None = None) -> QPoint:
    """Hot spot within the outlined drag pixmap (matches ``make_outlined_drag_pixmap``)."""
    if press_local is None:
        local = QPoint(widget.width() // 2, widget.height() // 2)
    else:
        local = QPoint(press_local)
    local.setX(max(0, min(widget.width(), local.x())))
    local.setY(max(0, min(widget.height(), local.y())))
    return QPoint(local.x() + DRAG_PIXMAP_PAD, local.y() + DRAG_PIXMAP_PAD)


def _drag_drop_anchor(drop_global: QPoint, hotspot: QPoint) -> QPoint:
    """Top-left of the drag ghost — used for custom public-drag ghost positioning."""
    return QPoint(drop_global.x() - hotspot.x(), drop_global.y() - hotspot.y())


def virtual_unpin_drop_hint(cursor: QPoint) -> QPoint:
    """Monitor hint for fence→public unpin (left-edge grid picks the cell).

    The release point selects which screen's work area to use; placement itself
    is column-major from the left margin, skipping fence frames.
    """
    return QPoint(int(cursor.x()), int(cursor.y()))


def display_name_for_path(file_path: Path) -> str:
    """Label text for fence items; hide .lnk extension like Windows desktop."""
    name = file_path.name
    if name.lower().endswith(".lnk"):
        return file_path.stem
    return name


def make_outlined_drag_pixmap(widget: QWidget, accent: str = "#07c160") -> QPixmap:
    """Semi-transparent grab of the icon with a clear outline (Fences-style ghost)."""
    try:
        grab = widget.grab()
    except Exception:
        grab = QPixmap()
    if grab.isNull():
        return QPixmap()

    pad = DRAG_PIXMAP_PAD
    out = QPixmap(grab.width() + pad * 2, grab.height() + pad * 2)
    out.fill(Qt.GlobalColor.transparent)
    painter = QPainter(out)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    painter.setOpacity(0.82)
    painter.drawPixmap(pad, pad, grab)
    painter.setOpacity(1.0)
    color = QColor(accent)
    if not color.isValid():
        color = QColor("#07c160")
    pen = QPen(color, 2.0)
    pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
    painter.setPen(pen)
    painter.setBrush(Qt.BrushStyle.NoBrush)
    painter.drawRoundedRect(
        pad - 1,
        pad - 1,
        grab.width() + 2,
        grab.height() + 2,
        8,
        8,
    )
    painter.end()
    return out


def clear_fence_drop_indicators() -> None:
    for top in QApplication.topLevelWidgets():
        hide = getattr(top, "_hide_drop_indicator", None)
        if callable(hide):
            try:
                hide()
            except Exception:
                pass


def iter_fence_selectable_items(anchor: QWidget) -> list[QWidget]:
    """Sibling fence icon/list items under the same FenceWidget."""
    # Start at *anchor* itself — Ctrl+A / paste pass the FenceWidget directly.
    parent: QWidget | None = anchor
    while parent is not None:
        fn = getattr(parent, "_icon_item_widgets", None)
        if callable(fn):
            try:
                return list(fn())
            except Exception:
                break
        parent = parent.parentWidget()
    return [anchor] if getattr(anchor, "file_path", None) is not None else []


def find_fence_widget(anchor: QWidget) -> QWidget | None:
    """Nearest ancestor FenceWidget, if any."""
    parent: QWidget | None = anchor
    while parent is not None:
        if parent.__class__.__name__ == "FenceWidget":
            return parent
        parent = parent.parentWidget()
    return None


_selection_batch_depth = 0
_selection_batch_dirty: set[QWidget] = set()


def fence_selection_batch_active() -> bool:
    return _selection_batch_depth > 0


@contextmanager
def fence_selection_batch(anchor: QWidget | None = None):
    """Coalesce N icon selection paints into one fence-region update.

    Exclusive click used to call ``update()`` on every sibling — each pass
    recomposited the translucent top-level HWND and flashed the desktop.

    Flush while updates are still disabled, then re-enable once — enabling
    first then flushing painted twice and flashed the wallpaper.
    """
    global _selection_batch_depth
    fence = find_fence_widget(anchor) if anchor is not None else None
    _selection_batch_depth += 1
    if fence is not None:
        try:
            fence.setUpdatesEnabled(False)
        except RuntimeError:
            fence = None
    try:
        yield
    finally:
        _selection_batch_depth -= 1
        if _selection_batch_depth == 0:
            _flush_fence_selection_visuals()
        if fence is not None:
            try:
                fence.setUpdatesEnabled(True)
            except RuntimeError:
                pass


def _selection_ring_radius(widget: QWidget) -> int:
    name = widget.__class__.__name__
    if name in ("FenceIconItem", "PublicIconWidget"):
        return 6
    return 4


def _selection_colors_for(widget: QWidget) -> tuple[QColor, QColor]:
    if widget.__class__.__name__ == "PublicIconWidget":
        return public_item_selection_colors()
    return fence_item_selection_colors(widget)


def _retire_legacy_selection_ring(widget: QWidget) -> None:
    """Drop leftover QFrame rings from older builds (ghost chrome on translucent fences)."""
    ring = getattr(widget, "_selection_ring", None)
    if ring is None:
        return
    try:
        ring.hide()
        ring.setParent(None)
        ring.deleteLater()
    except RuntimeError:
        pass
    try:
        widget._selection_ring = None  # type: ignore[attr-defined]
    except Exception:
        pass


def paint_fence_item_selection(widget: QWidget, painter: QPainter) -> None:
    """Draw selection chrome via QPainter — child QFrame rings ghost on translucent fences."""
    if not bool(getattr(widget, "_selected", False)):
        return
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    fill, border = _selection_colors_for(widget)
    painter.setBrush(fill)
    pen = QPen(border, 1.5)
    pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
    painter.setPen(pen)
    radius = _selection_ring_radius(widget)
    rect = fence_selection_chrome_rect(widget)
    if not rect.isValid() or rect.isEmpty():
        return
    painter.drawRoundedRect(rect, radius, radius)


def fence_selection_chrome_rect(widget: QWidget) -> QRect:
    """Equal-pad AABB around glyph + caption ink (not the empty 2-line shelf).

    Full ``widget.rect()`` left a large empty band under one-line names because
    captions reserve a 2-line shelf — selection looked top-heavy.
    """
    try:
        bounds = widget.rect().adjusted(1, 1, -2, -2)
    except RuntimeError:
        return QRect()
    parts: list[QRect] = []
    icon = getattr(widget, "icon_label", None)
    if icon is not None:
        try:
            if not icon.isHidden():
                parts.append(QRect(icon.geometry()))
        except RuntimeError:
            pass
    text = getattr(widget, "text_label", None)
    if text is not None:
        try:
            if not text.isHidden():
                ink = _caption_text_hit_rect(widget, text)
                if ink.isValid() and not ink.isEmpty():
                    # Keep shelf width (centered column) but clip height to ink
                    # so short names do not leave a dead band under the glyphs.
                    geo = text.geometry()
                    parts.append(QRect(geo.x(), ink.y(), geo.width(), ink.height()))
                else:
                    parts.append(QRect(text.geometry()))
        except RuntimeError:
            pass
    if not parts:
        return bounds
    content = parts[0]
    for part in parts[1:]:
        content = content.united(part)
    # Want equal pad; if the cell edge clips one side, shrink the other to match
    # so icon+caption stay vertically centered in the green plate.
    pad = 4
    want = content.adjusted(-pad, -pad, pad, pad)
    rect = want.intersected(bounds)
    if not rect.isValid() or rect.isEmpty():
        return bounds
    pad_top = max(0, content.top() - rect.top())
    pad_bot = max(0, rect.bottom() - content.bottom())
    if pad_top != pad_bot:
        use = min(pad_top, pad_bot)
        rect.setTop(content.top() - use)
        rect.setBottom(content.bottom() + use)
        rect = rect.intersected(bounds)
    return rect


def sync_fence_item_selection_ring(widget: QWidget) -> None:
    """Compat no-op: selection is painted in paintEvent (rings retired)."""
    _retire_legacy_selection_ring(widget)


def _fence_item_paint_host(widget: QWidget) -> QWidget | None:
    """Widget whose update() should repaint *widget* selection chrome.

    Prefer the immediate parent (``items_widget``) — child ``geometry()`` is
    already in that space. Falling back to FenceWidget requires mapTo.
    """
    parent = widget.parentWidget()
    if parent is not None:
        return parent
    return find_fence_widget(widget)


def _item_rect_for_host(widget: QWidget, host: QWidget) -> QRect:
    if widget.parentWidget() is host:
        return QRect(widget.geometry())
    try:
        top_left = widget.mapTo(host, QPoint(0, 0))
        return QRect(top_left, widget.size())
    except RuntimeError:
        return QRect(widget.geometry())


def _invalidate_fence_item_selection(widget: QWidget) -> None:
    """One owner for selection invalidate — single host-region update.

    Child ``widget.update()`` + parent ``fence.update()`` double-composited the
    translucent HWND (click flash). One host-region update repaints children.
    """
    _retire_legacy_selection_ring(widget)
    host = _fence_item_paint_host(widget)
    if host is not None:
        try:
            host.update(_item_rect_for_host(widget, host))
        except RuntimeError:
            pass
        return
    try:
        widget.update()
    except RuntimeError:
        return


def _commit_fence_item_selection_visual(widget: QWidget) -> None:
    if _selection_batch_depth > 0:
        _selection_batch_dirty.add(widget)
        return
    _invalidate_fence_item_selection(widget)


def _flush_fence_selection_visuals() -> None:
    dirty = set(_selection_batch_dirty)
    _selection_batch_dirty.clear()
    if not dirty:
        return
    by_host: dict[QWidget, list[QWidget]] = {}
    orphans: list[QWidget] = []
    for widget in dirty:
        _retire_legacy_selection_ring(widget)
        host = _fence_item_paint_host(widget)
        if host is not None:
            by_host.setdefault(host, []).append(widget)
        else:
            orphans.append(widget)
    for host, items in by_host.items():
        try:
            region = QRect()
            for widget in items:
                region = region.united(_item_rect_for_host(widget, host))
            if not region.isNull():
                host.update(region)
        except RuntimeError:
            continue
    for widget in orphans:
        try:
            widget.update()
        except RuntimeError:
            continue


def desktop_selection_batch_active() -> bool:
    """True while fence or public multi-select is coalescing paints."""
    return fence_selection_batch_active() or public_selection_batch_active()


def select_fence_item(item: QWidget, *, exclusive: bool = True) -> None:
    """Mark *item* selected; optionally clear other items in the same fence."""
    with fence_selection_batch(item):
        if exclusive:
            for other in iter_fence_selectable_items(item):
                if other is item:
                    continue
                setter = getattr(other, "set_selected", None)
                if callable(setter):
                    setter(False)
        setter = getattr(item, "set_selected", None)
        if callable(setter):
            setter(True)
    mark_desktop_selection_live()


def selected_fence_items(anchor: QWidget) -> list[QWidget]:
    """Currently selected siblings in the same fence (grid order)."""
    out: list[QWidget] = []
    for item in iter_fence_selectable_items(anchor):
        checker = getattr(item, "is_selected", None)
        if callable(checker) and checker():
            out.append(item)
    return out


def apply_fence_click_selection(
    item: QWidget,
    modifiers: Qt.KeyboardModifier,
    *,
    right_button: bool = False,
) -> None:
    """Explorer-like click selection inside one fence (Ctrl / Shift / exclusive)."""
    fence = find_fence_widget(item)
    items = iter_fence_selectable_items(item)
    ctrl = bool(modifiers & Qt.KeyboardModifier.ControlModifier)
    shift = bool(modifiers & Qt.KeyboardModifier.ShiftModifier)

    if right_button:
        checker = getattr(item, "is_selected", None)
        if not (callable(checker) and checker()):
            select_fence_item(item, exclusive=True)
            if fence is not None:
                fence._selection_anchor = item  # type: ignore[attr-defined]
        mark_desktop_selection_live()
        return

    if shift and fence is not None and items:
        anchor = getattr(fence, "_selection_anchor", None)
        if anchor is None or anchor not in items:
            anchor = item
        try:
            i0 = items.index(anchor)
            i1 = items.index(item)
        except ValueError:
            select_fence_item(item, exclusive=True)
            fence._selection_anchor = item  # type: ignore[attr-defined]
            mark_desktop_selection_live()
            return
        lo, hi = (i0, i1) if i0 <= i1 else (i1, i0)
        with fence_selection_batch(item):
            for idx, other in enumerate(items):
                setter = getattr(other, "set_selected", None)
                if callable(setter):
                    setter(lo <= idx <= hi)
        mark_desktop_selection_live()
        return

    if ctrl:
        with fence_selection_batch(item):
            checker = getattr(item, "is_selected", None)
            setter = getattr(item, "set_selected", None)
            if callable(checker) and callable(setter):
                setter(not bool(checker()))
            if fence is not None:
                fence._selection_anchor = item  # type: ignore[attr-defined]
        mark_desktop_selection_live()
        return

    # Plain press on an already-selected item keeps the multi-select so a
    # following drag can unpin / move the whole set (Explorer). Collapse to
    # exclusive happens on mouse-release if no drag started.
    checker = getattr(item, "is_selected", None)
    if callable(checker) and checker():
        mark_desktop_selection_live()
        return

    select_fence_item(item, exclusive=True)
    if fence is not None:
        fence._selection_anchor = item  # type: ignore[attr-defined]
    mark_desktop_selection_live()


def finalize_plain_click_selection(
    item: QWidget,
    *,
    was_selected: bool,
    modifiers: Qt.KeyboardModifier = Qt.KeyboardModifier.NoModifier,
) -> bool:
    """Collapse multi-select to *item* after a plain click without drag.

    Explorer: press on a selected icon keeps the set for drag; release without
    moving then singles that icon. Returns True when selection was collapsed
    (caller should not start in-place rename on this same click).
    """
    if not was_selected:
        return False
    if modifiers & (
        Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier
    ):
        return False
    if is_public_icon_context(item):
        selected = selected_public_items(item)
        if len(selected) <= 1:
            return False
        select_public_item(item, exclusive=True)
        host = find_public_icon_host(item)
        if host is not None:
            host._selection_anchor = item  # type: ignore[attr-defined]
        mark_desktop_selection_live()
        return True
    selected = selected_fence_items(item)
    if len(selected) <= 1:
        return False
    select_fence_item(item, exclusive=True)
    fence = find_fence_widget(item)
    if fence is not None:
        fence._selection_anchor = item  # type: ignore[attr-defined]
    mark_desktop_selection_live()
    return True


def paths_for_fence_drag(widget: QWidget) -> list[Path]:
    """Paths to drag: whole selection if *widget* is selected, else only itself."""
    selected = selected_fence_items(widget)
    if any(item is widget for item in selected):
        paths = []
        for item in selected:
            path = getattr(item, "file_path", None)
            if path is not None:
                paths.append(Path(path))
        if paths:
            return paths
    select_fence_item(widget, exclusive=True)
    fence = find_fence_widget(widget)
    if fence is not None:
        fence._selection_anchor = widget  # type: ignore[attr-defined]
    path = getattr(widget, "file_path", None)
    return [Path(path)] if path is not None else []


def find_public_icon_host(anchor: QWidget) -> QWidget | None:
    """Nearest PublicIconHost ancestor, or the hosted top-level window."""
    parent: QWidget | None = anchor
    while parent is not None:
        if parent.__class__.__name__ == "PublicIconHost":
            return parent
        parent = parent.parentWidget()
    win = anchor.window() if anchor is not None else None
    if win is not None and win.__class__.__name__ == "PublicIconHost":
        return win
    return None


def iter_public_selectable_items(anchor: QWidget) -> list[QWidget]:
    """Visible public floats sharing the same host (screen order: top→bottom, left→right)."""
    host = find_public_icon_host(anchor)
    items: list[QWidget] = []
    if host is not None:
        cached = getattr(host, "public_icon_widgets", None)
        if callable(cached):
            try:
                return list(cached())
            except Exception:
                pass
        for child in host.findChildren(QWidget):
            if child.parentWidget() is not host:
                continue
            if child.__class__.__name__ != "PublicIconWidget":
                continue
            try:
                if child.isHidden():
                    continue
            except RuntimeError:
                continue
            items.append(child)
    elif anchor.__class__.__name__ == "PublicIconWidget":
        items = [anchor]
    else:
        return []
    items.sort(key=lambda w: (int(w.y()), int(w.x())))
    return items


_public_selection_batch_depth = 0
_public_selection_batch_dirty: set[QWidget] = set()


def public_selection_batch_active() -> bool:
    return _public_selection_batch_depth > 0


@contextmanager
def public_selection_batch(anchor: QWidget | None = None):
    """Coalesce public-float selection paints into one host update."""
    global _public_selection_batch_depth
    host = find_public_icon_host(anchor) if anchor is not None else None
    _public_selection_batch_depth += 1
    if host is not None:
        try:
            host.setUpdatesEnabled(False)
        except RuntimeError:
            host = None
    try:
        yield
    finally:
        _public_selection_batch_depth -= 1
        if host is not None:
            try:
                host.setUpdatesEnabled(True)
            except RuntimeError:
                pass
        if _public_selection_batch_depth == 0:
            _flush_public_selection_visuals()


def _commit_public_item_selection_visual(widget: QWidget) -> None:
    """Repaint public float selection via paintEvent — not a child QFrame.

    Public icons sit on a fully transparent DefView host. A stylesheet QFrame
    ring does not composite there (looks like「选不中」), while QPainter in
    ``paintEvent`` does. Keep batching so exclusive click still one host update.
    Prefer host-region update only — dual widget+host update forced two DWM
    compositions per select on translucent overlays.
    """
    if _public_selection_batch_depth > 0:
        _public_selection_batch_dirty.add(widget)
        return
    host = find_public_icon_host(widget)
    if host is not None:
        try:
            host.update(widget.geometry())
        except RuntimeError:
            pass
        return
    try:
        widget.update()
    except RuntimeError:
        return


def _flush_public_selection_visuals() -> None:
    dirty = set(_public_selection_batch_dirty)
    _public_selection_batch_dirty.clear()
    if not dirty:
        return
    by_host: dict[QWidget, list[QWidget]] = {}
    orphans: list[QWidget] = []
    for widget in dirty:
        host = find_public_icon_host(widget)
        if host is not None:
            by_host.setdefault(host, []).append(widget)
        else:
            orphans.append(widget)
    for host, items in by_host.items():
        try:
            region = QRect()
            for widget in items:
                region = region.united(widget.geometry())
            if not region.isNull():
                host.update(region)
        except RuntimeError:
            continue
    for widget in orphans:
        try:
            widget.update()
        except RuntimeError:
            continue


def select_public_item(item: QWidget, *, exclusive: bool = True) -> None:
    with public_selection_batch(item):
        if exclusive:
            for other in iter_public_selectable_items(item):
                if other is item:
                    continue
                setter = getattr(other, "set_selected", None)
                if callable(setter):
                    setter(False)
        setter = getattr(item, "set_selected", None)
        if callable(setter):
            setter(True)
    mark_desktop_selection_live()


def selected_public_items(anchor: QWidget) -> list[QWidget]:
    out: list[QWidget] = []
    for item in iter_public_selectable_items(anchor):
        checker = getattr(item, "is_selected", None)
        if callable(checker) and checker():
            out.append(item)
    return out


def clear_public_selection(anchor: QWidget) -> None:
    host = find_public_icon_host(anchor)
    items = iter_public_selectable_items(anchor if host is None else host)
    batch_anchor = items[0] if items else host
    with public_selection_batch(batch_anchor):
        for item in items:
            setter = getattr(item, "set_selected", None)
            if callable(setter):
                setter(False)
    if host is not None:
        host._selection_anchor = None  # type: ignore[attr-defined]
    mark_desktop_selection_live(False)


def apply_public_click_selection(
    item: QWidget,
    modifiers: Qt.KeyboardModifier,
    *,
    right_button: bool = False,
) -> None:
    """Explorer-like click selection among public floats (Ctrl / Shift / exclusive)."""
    host = find_public_icon_host(item)
    items = iter_public_selectable_items(item)
    ctrl = bool(modifiers & Qt.KeyboardModifier.ControlModifier)
    shift = bool(modifiers & Qt.KeyboardModifier.ShiftModifier)

    if right_button:
        checker = getattr(item, "is_selected", None)
        if not (callable(checker) and checker()):
            select_public_item(item, exclusive=True)
            if host is not None:
                host._selection_anchor = item  # type: ignore[attr-defined]
        mark_desktop_selection_live()
        return

    if shift and items:
        anchor = getattr(host, "_selection_anchor", None) if host is not None else None
        if anchor is None or anchor not in items:
            anchor = item
        try:
            i0 = items.index(anchor)
            i1 = items.index(item)
        except ValueError:
            select_public_item(item, exclusive=True)
            if host is not None:
                host._selection_anchor = item  # type: ignore[attr-defined]
            mark_desktop_selection_live()
            return
        lo, hi = (i0, i1) if i0 <= i1 else (i1, i0)
        with public_selection_batch(item):
            for idx, other in enumerate(items):
                setter = getattr(other, "set_selected", None)
                if callable(setter):
                    setter(lo <= idx <= hi)
        mark_desktop_selection_live()
        return

    if ctrl:
        with public_selection_batch(item):
            checker = getattr(item, "is_selected", None)
            setter = getattr(item, "set_selected", None)
            if callable(checker) and callable(setter):
                setter(not bool(checker()))
            if host is not None:
                host._selection_anchor = item  # type: ignore[attr-defined]
        mark_desktop_selection_live()
        return

    # Plain press on already-selected float keeps multi-select for drag.
    checker = getattr(item, "is_selected", None)
    if callable(checker) and checker():
        mark_desktop_selection_live()
        return

    select_public_item(item, exclusive=True)
    if host is not None:
        host._selection_anchor = item  # type: ignore[attr-defined]
    mark_desktop_selection_live()


def paths_for_public_drag(widget: QWidget) -> list[Path]:
    """Paths to drag: whole public selection if *widget* is selected, else itself."""
    selected = selected_public_items(widget)
    if any(item is widget for item in selected):
        paths = []
        for item in selected:
            path = getattr(item, "file_path", None)
            if path is not None:
                paths.append(Path(path))
        if paths:
            return paths
    select_public_item(widget, exclusive=True)
    host = find_public_icon_host(widget)
    if host is not None:
        host._selection_anchor = widget  # type: ignore[attr-defined]
    path = getattr(widget, "file_path", None)
    return [Path(path)] if path is not None else []


def public_item_selection_colors() -> tuple[QColor, QColor]:
    """Selection chrome for public floats — system Highlight, Explorer translucency."""
    from PyQt6.QtGui import QGuiApplication, QPalette

    hl = QColor(0, 120, 215)
    try:
        cand = QGuiApplication.palette().color(QPalette.ColorRole.Highlight)
        if cand.isValid():
            hl = QColor(cand)
    except Exception:
        pass
    fill = QColor(hl)
    # Match desktop ListviewAlphaSelect fill (see-through, not a solid plate).
    fill.setAlpha(72)
    border = QColor(hl)
    border.setAlpha(210)
    return fill, border


def public_caption_ink_rect(widget: QWidget) -> QRect:
    """Caption plate: fixed shelf size (uniform width + height for every icon).

    Prefer locked ``_label_width`` / ``_label_height`` over live ``QLabel``
    sizeHint — after ``setPixmap`` / layout lag, geometry height can briefly
    follow ink (1-line vs 2-line) and make selection chrome look uneven.
    """
    label = getattr(widget, "text_label", None)
    if label is None:
        return QRect()
    try:
        if label.isHidden():
            return QRect()
        geo = QRect(label.geometry())
    except RuntimeError:
        return QRect()
    if not geo.isValid() or geo.isEmpty():
        return QRect()
    shelf_w = int(getattr(widget, "_label_width", 0) or 0)
    shelf_h = int(getattr(widget, "_label_height", 0) or 0)
    if shelf_w > 0:
        # Keep label's laid-out X (centered); force shelf width.
        x = geo.x() + max(0, (geo.width() - shelf_w) // 2) if geo.width() > shelf_w else geo.x()
        geo.setLeft(x)
        geo.setWidth(shelf_w)
    if shelf_h > 0:
        geo.setHeight(max(int(geo.height()), shelf_h))
    return geo


def public_selection_chrome_rect(widget: QWidget) -> QRect:
    """Win11-style selection: one padded AABB around glyph + fixed caption shelf."""
    parts: list[QRect] = []
    icon = getattr(widget, "icon_label", None)
    if icon is not None:
        try:
            if not icon.isHidden():
                parts.append(icon.geometry().adjusted(-2, -2, 2, 2))
        except RuntimeError:
            pass
    cap = public_caption_ink_rect(widget)
    if cap.isValid() and not cap.isEmpty():
        parts.append(QRect(cap))
    try:
        bounds = widget.rect()
    except RuntimeError:
        return QRect()
    if not parts:
        return bounds.adjusted(12, 6, -12, -6)
    rect = parts[0]
    for part in parts[1:]:
        rect = rect.united(part)
    # Width follows the fixed caption shelf (uniform across icons). Light vertical
    # pad only — large horizontal pad used to inflate ink-tight captions to the
    # cell edge and made short vs long names look different widths.
    if cap.isValid() and not cap.isEmpty():
        rect.setLeft(cap.left())
        rect.setRight(cap.right())
    rect = rect.adjusted(0, -3, 0, 3)
    return rect.intersected(bounds)


def paint_public_item_selection(widget: QWidget, painter: QPainter) -> None:
    """Explorer/DefView-like selection chrome for public desktop floats."""
    if not bool(getattr(widget, "_selected", False)):
        return
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    fill, border = public_item_selection_colors()
    painter.setBrush(fill)
    pen = QPen(border, 1.0)
    pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
    painter.setPen(pen)
    rect = public_selection_chrome_rect(widget)
    if not rect.isValid() or rect.isEmpty():
        return
    painter.drawRoundedRect(rect, 4, 4)


def encode_virtual_drag_paths(paths: list[Path]) -> QByteArray:
    """Newline-separated absolute paths for ``DESKTIDY_VIRTUAL_MIME``."""
    lines = [str(Path(p)) for p in paths if p]
    return QByteArray("\n".join(lines).encode("utf-8"))


def fence_item_selection_colors(widget: QWidget) -> tuple[QColor, QColor]:
    """Fill/border colors for selected fence icons (accent on dark panel)."""
    accent = "#07c160"
    parent = widget.parent()
    while parent is not None:
        style = getattr(parent, "_style", None)
        if isinstance(style, dict):
            raw = style.get("accent")
            if isinstance(raw, str) and raw.startswith("#") and len(raw) >= 7:
                accent = raw[:7]
            break
        parent = parent.parent()
    fill = QColor(accent)
    if not fill.isValid():
        fill = QColor("#07c160")
    fill.setAlpha(110)
    border = QColor(fill)
    border.setAlpha(230)
    return fill, border


def destroy_unpin_catchers() -> None:
    """Force-remove any leftover fullscreen drag catchers (they block desktop RMB)."""
    for top in list(QApplication.topLevelWidgets()):
        try:
            if top.objectName() != "desktidyUnpinCatcher":
                continue
        except RuntimeError:
            continue
        _destroy_catcher(top)


def _destroy_catcher(catcher: QWidget | None) -> None:
    if catcher is None:
        return
    try:
        catcher.setAcceptDrops(False)
        catcher.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        catcher.hide()
        catcher.setGeometry(0, 0, 0, 0)
        catcher.setParent(None)
        catcher.close()
        catcher.deleteLater()
    except RuntimeError:
        pass


def _begin_overlay_drag_session() -> None:
    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app else None
    begin = getattr(desk, "_begin_overlay_drag", None) if desk is not None else None
    if callable(begin):
        try:
            begin()
        except Exception:
            pass


def _end_overlay_drag_session(*, reconcile: bool = True) -> None:
    destroy_unpin_catchers()
    clear_fence_drop_indicators()
    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app else None
    end = getattr(desk, "_end_overlay_drag", None) if desk is not None else None
    if callable(end):
        try:
            end(reconcile_fg=reconcile)
        except TypeError:
            try:
                end()
            except Exception:
                pass
        except Exception:
            pass


def path_pinned_in_settings(settings: dict | None, path: Path | str) -> bool:
    """True when *path* is an explicit fence virtual pin (exact key only).

    Uses the same ``_norm_virtual_key`` as fence membership — never unique
    basename fallback. Basename-only matching used to treat a different folder's
    ``report.xlsx`` as pinned when exactly one fence listed that name — public
    restore then skipped ``add_public_item`` and the float vanished.
    """
    if not isinstance(settings, dict):
        return False
    from src.fence_rules import _norm_virtual_key

    try:
        key = _norm_virtual_key(Path(path)).casefold().replace("/", "\\")
    except OSError:
        key = str(path).casefold().replace("/", "\\")
    for fence in settings.get("fences") or []:
        if not isinstance(fence, dict):
            continue
        for raw in fence.get("virtual_items") or []:
            try:
                raw_s = _norm_virtual_key(Path(str(raw))).casefold().replace("/", "\\")
            except OSError:
                continue
            if raw_s == key:
                return True
    return False


def start_file_drag(widget: QWidget, file_path: Path) -> Qt.DropAction:
    """Drag a local file, preferring Move for Explorer / desktop drops."""
    from src.app_logging import get_logger
    from src.organize_suppress import suppress_desktop_item
    from src.settings import get_desktop_paths, iter_fence_storage_lnk_files
    from src.win_shell import (
        get_lnk_namespace_clsid,
        return_namespace_icon_to_desktop,
    )

    ns_clsid = get_lnk_namespace_clsid(file_path)
    ns_name = file_path.stem

    _begin_overlay_drag_session()
    result = Qt.DropAction.IgnoreAction
    try:
        drag = QDrag(_ole_drag_source_widget(widget))
        mime = QMimeData()
        if not attach_external_file_drag_payload(mime, file_path):
            return Qt.DropAction.IgnoreAction
        # Tell Windows Shell to treat this as a move by default (not copy).
        mime.setData("Preferred DropEffect", QByteArray(struct.pack("<I", _DROPEFFECT_MOVE)))
        drag.setMimeData(mime)
        watchdog = QTimer()
        watchdog.setSingleShot(True)
        timed_out = False

        def _on_ole_watchdog() -> None:
            nonlocal timed_out
            timed_out = True
            drag.cancel()

        watchdog.timeout.connect(_on_ole_watchdog)
        watchdog.start(30000)
        try:
            result = drag.exec(
                Qt.DropAction.MoveAction | Qt.DropAction.CopyAction,
                Qt.DropAction.MoveAction,
            )
        finally:
            watchdog.stop()
        if timed_out:
            get_logger().warning(
                "file drag: OLE timed out after 30s path=%s", file_path
            )
            return Qt.DropAction.IgnoreAction
    finally:
        _end_overlay_drag_session(reconcile=True)

    if ns_clsid and result == Qt.DropAction.MoveAction:
        # If the shortcut still lives under fence storage, it was moved to another fence.
        still_in_storage = None
        try:
            for lnk in iter_fence_storage_lnk_files():
                if get_lnk_namespace_clsid(lnk) == ns_clsid:
                    still_in_storage = lnk
                    break
        except OSError:
            still_in_storage = None
        if still_in_storage is not None:
            pass
        else:
            return_namespace_icon_to_desktop(ns_clsid)
            # Only remove the dragged shortcut itself if it landed on the desktop.
            for desk in get_desktop_paths():
                try:
                    candidate = desk / file_path.name
                    if candidate.exists() and get_lnk_namespace_clsid(candidate) == ns_clsid:
                        candidate.unlink(missing_ok=True)
                except OSError:
                    pass
        return result
    if result in (Qt.DropAction.MoveAction, Qt.DropAction.CopyAction):
        suppress_desktop_item(file_path)
    return result


def _desktop_prefercopy_dupe_candidates(src: Path, desks: list[Path]) -> list[Path]:
    """Likely names Explorer creates when PreferCopy lands on the desktop."""
    name = src.name
    stem = src.stem
    suffix = src.suffix
    out: list[Path] = []
    for desk in desks:
        out.append(desk / name)
        # zh-CN / en-US Explorer copy naming.
        out.append(desk / f"{stem} - 副本{suffix}")
        out.append(desk / f"{stem} - Copy{suffix}")
        out.append(desk / f"{stem} (1){suffix}")
        for n in range(2, 6):
            out.append(desk / f"{stem} - 副本 ({n}){suffix}")
            out.append(desk / f"{stem} - Copy ({n}){suffix}")
            out.append(desk / f"{stem} ({n}){suffix}")
    return out


def discard_explorer_prefercopy_desktop_dupes(
    paths: list[Path],
    *,
    not_before: float,
    skew_sec: float = 5.0,
) -> list[Path]:
    """Remove fresh desktop copies Explorer made when CF_HDROP PreferCopy stole the drop.

    Virtual pins must stay on disk at their original path; a blank-desktop OLE
    Copy would otherwise leave a real duplicate while the icon remains in the fence.
    Only deletes paths under known desktop folders whose mtime is >= *not_before*.
    """
    from src.settings import get_desktop_paths

    try:
        desks = [Path(p) for p in get_desktop_paths()]
    except Exception:
        return []
    if not desks:
        return []
    return discard_fresh_prefercopy_dupes(
        paths, desks, not_before=not_before, skew_sec=skew_sec
    )


def discard_fresh_prefercopy_dupes(
    paths: list[Path],
    dirs: list[Path],
    *,
    not_before: float,
    skew_sec: float = 5.0,
) -> list[Path]:
    """Delete freshly created PreferCopy collisions under *dirs* (mtime-gated)."""
    removed: list[Path] = []
    targets = [Path(d) for d in dirs if d]
    if not targets:
        return removed
    floor = float(not_before) - float(skew_sec)
    for raw in paths:
        src = Path(raw)
        try:
            if not src.exists():
                continue
            src_key = str(src.resolve(strict=False)).casefold()
        except OSError:
            continue
        for dest in _desktop_prefercopy_dupe_candidates(src, targets):
            try:
                if not dest.exists():
                    continue
                dest_key = str(dest.resolve(strict=False)).casefold()
                if dest_key == src_key:
                    continue
                if float(dest.stat().st_mtime) < floor:
                    continue
                if dest.is_dir():
                    import shutil

                    shutil.rmtree(dest, ignore_errors=True)
                else:
                    dest.unlink(missing_ok=True)
                if not dest.exists():
                    removed.append(dest)
            except OSError:
                continue
    return removed


def should_convert_prefercopy_to_unpin(
    *,
    drop_action: Qt.DropAction,
    end_global: QPoint | None,
    primary: Path | None = None,
) -> bool:
    """True when blank-desktop OLE Copy should become 「移出分区」 instead of a FS copy."""
    if drop_action != Qt.DropAction.CopyAction:
        return False
    end = end_global if end_global is not None else QCursor.pos()
    if not should_unpin_virtual_drop(end):
        return False
    if folder_drop_target_at(end, exclude=primary) is not None:
        return False
    try:
        from src.win_shell import is_external_app_drop_point

        if is_external_app_drop_point(int(end.x()), int(end.y())):
            return False
    except Exception:
        pass
    return True


def path_is_under_desktop(path: Path) -> bool:
    """True when *path* already lives directly in a desktop folder."""
    from src.settings import get_desktop_paths

    try:
        parent = Path(path).resolve(strict=False).parent
    except OSError:
        return False
    for root in get_desktop_paths():
        try:
            if parent == Path(root).resolve(strict=False):
                return True
        except OSError:
            continue
    return False


def _virtual_drag_all_desktop_resident(paths: list[Path]) -> bool:
    """Every path already on a desktop folder (common virtual-pin case)."""
    if not paths:
        return False
    return all(path_is_under_desktop(p) for p in paths)


def _fence_insert_index_at_global(fence, global_pos: QPoint) -> int | None:
    items_w = getattr(fence, "items_widget", None)
    insert_fn = getattr(fence, "_insert_index_at", None)
    if items_w is None or not callable(insert_fn):
        return None
    try:
        local = items_w.mapFromGlobal(global_pos)
        return insert_fn(items_w, local)
    except Exception:
        return None


def _apply_fence_virtual_drop(
    fence,
    paths: list[Path],
    *,
    source_id: str,
    drop_global: QPoint,
) -> bool:
    apply = getattr(fence, "_apply_virtual_drop_paths", None)
    if not callable(apply):
        return False
    insert_at = _fence_insert_index_at_global(fence, drop_global)
    try:
        return bool(apply(paths, source_id=source_id, insert_at=insert_at))
    except Exception:
        return False


def _run_custom_desktop_resident_virtual_drag(
    widget: QWidget,
    file_path: Path,
    fence_id: str,
    drag_paths: list[Path],
    *,
    press_local: QPoint | None = None,
) -> tuple[Qt.DropAction, bool]:
    """Non-OLE virtual drag for fence pins (desktop-resident or off-desktop).

    Avoids Explorer same-name dialogs and VPN/utility wake-ups from mid-gesture
    ``QDrag.exec``. Hovering an allowlisted chat / Office / browser hands off to
    OLE while LMB is still down (WeChat / QQ / …).
    """
    global _last_virtual_unpin_pos

    from src.app_logging import get_logger

    app = QApplication.instance()
    if app is None:
        return Qt.DropAction.IgnoreAction, False

    primary = Path(file_path) if file_path else drag_paths[0]
    start_global = QCursor.pos()
    source_id = str(fence_id or "")

    _begin_overlay_drag_session()
    get_logger().info(
        "virtual drag: custom (no OLE) desktop-resident count=%s path=%s",
        len(drag_paths),
        primary,
    )

    accent = "#07c160"
    try:
        fence = widget
        while fence is not None and fence.__class__.__name__ != "FenceWidget":
            fence = fence.parentWidget()
        if fence is not None:
            style = getattr(fence, "_style", None) or {}
            if isinstance(style.get("accent"), str):
                accent = style["accent"]
    except Exception:
        pass

    pix = make_outlined_drag_pixmap(widget, accent=accent)
    if pix.isNull():
        try:
            pix = file_icon_pixmap(primary, FenceIconItem.DEFAULT_ICON_SIZE)
        except Exception:
            pix = QPixmap()
    if len(drag_paths) > 1 and not pix.isNull():
        try:
            painter = QPainter(pix)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            badge = f"+{len(drag_paths) - 1}"
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(0, 120, 215, 230))
            painter.drawRoundedRect(pix.width() - 36, 4, 32, 18, 6, 6)
            painter.setPen(QColor("white"))
            painter.drawText(
                QRect(pix.width() - 36, 4, 32, 18),
                int(Qt.AlignmentFlag.AlignCenter),
                badge,
            )
            painter.end()
        except Exception:
            pass

    hotspot = _drag_hotspot(widget, press_local) if not pix.isNull() else QPoint(24, 24)
    ghost: QLabel | None = None
    if not pix.isNull():
        ghost = QLabel()
        ghost.setObjectName("desktidyPublicDragGhost")
        ghost.setWindowFlags(
            Qt.WindowType.Tool
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        ghost.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        ghost.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        _show_drag_ghost(ghost, pix, hotspot)

    drag_sources: list[QWidget] = []
    for item in selected_fence_items(widget):
        path = getattr(item, "file_path", None)
        if path is None:
            continue
        key = str(Path(path)).casefold()
        if key not in {str(p).casefold() for p in drag_paths}:
            continue
        drag_sources.append(item)
    if not drag_sources:
        drag_sources = [widget]
    concealed = _conceal_widgets_for_custom_drag(drag_sources)

    loop = QEventLoop()
    filt = _PublicDragFilter(loop, ghost, hotspot)
    app.installEventFilter(filt)
    filt.start()

    def _drag_timeout() -> None:
        filt.cancelled = True
        loop.quit()

    QTimer.singleShot(45000, _drag_timeout)

    drop_pos = QCursor.pos()
    cancelled = False
    handoff_external = False
    quiet_end = False
    try:
        loop.exec()
        drop_pos = QPoint(filt.drop_pos)
        cancelled = bool(filt.cancelled)
        handoff_external = bool(filt.handoff_external)
    finally:
        filt.stop()
        try:
            app.removeEventFilter(filt)
        except Exception:
            pass
        if ghost is not None:
            try:
                ghost.hide()
                ghost.setParent(None)
                ghost.deleteLater()
            except RuntimeError:
                pass

    try:
        if cancelled:
            return Qt.DropAction.IgnoreAction, False

        if handoff_external:
            payload = external_payload_paths_for_virtual_drag(drag_paths)
            if not payload:
                return Qt.DropAction.IgnoreAction, False
            get_logger().info(
                "virtual drag: OLE handoff to external app count=%s path=%s",
                len(payload),
                primary,
            )
            result = _exec_external_file_ole_drag(
                widget, payload, pixmap=pix, hotspot=hotspot
            )
            get_logger().info(
                "virtual drag: OLE handoff result=%s path=%s",
                result.name if hasattr(result, "name") else result,
                primary,
            )
            # If release landed on a fence (handoff was a false positive through
            # translucent chrome), pin the original .lnk paths — not OLE's
            # resolved English target.
            end_pos = QCursor.pos()
            fence = fence_widget_at(end_pos, geometry_only=True)
            if fence is not None:
                if _apply_fence_virtual_drop(
                    fence, drag_paths, source_id=source_id, drop_global=end_pos
                ):
                    try:
                        drop_id = str(
                            (getattr(fence, "config", None) or {}).get("id") or ""
                        )
                    except Exception:
                        drop_id = ""
                    if drop_id and drop_id == source_id:
                        quiet_end = True
                    return Qt.DropAction.MoveAction, False
            return result, False

        # DeskNote before pet trash (geometry-only pet hit overlapped DeskNote).
        desk = getattr(app, "_desktidy_app", None)
        settings = getattr(desk, "settings", None) if desk is not None else None
        try:
            from src.win_shell import try_open_paths_in_desknote_at

            if try_open_paths_in_desknote_at(drop_pos.x(), drop_pos.y(), drag_paths):
                get_logger().info(
                    "virtual drag: open in DeskNote count=%s drop=(%s,%s)",
                    len(drag_paths),
                    drop_pos.x(),
                    drop_pos.y(),
                )
                return Qt.DropAction.IgnoreAction, False
        except Exception:
            pass

        # Pet trash before folder: sprite owns the release (Explorer folders
        # under chrome / folder icons under the pet must not win).
        if _try_deliver_drag_to_pet_trash(
            desk,
            settings if isinstance(settings, dict) else None,
            drag_paths,
            drop_pos,
            fence_id=str(source_id or fence_id or ""),
            virtual_items=list(drag_sources),
        ):
            quiet_end = True
            return Qt.DropAction.MoveAction, False

        folder = folder_drop_target_at(drop_pos, exclude=primary)
        if folder is not None:
            from src.win_shell import resolve_folder_drop_target

            try:
                folder_resolved = Path(
                    resolve_folder_drop_target(Path(folder), for_move=True)
                )
            except Exception:
                folder_resolved = Path(folder)
            get_logger().info(
                "virtual drag: custom into folder=%s paths=%s",
                folder_resolved,
                len(drag_paths),
            )
            moved_keys: set[str] = set()
            copied_any = False
            for path in drag_paths:
                outcome = _move_virtual_into_folder(path, folder_resolved, fence_id)
                if outcome == "moved":
                    try:
                        moved_keys.add(str(Path(path)).casefold())
                    except OSError:
                        moved_keys.add(str(path).casefold())
                elif outcome == "copied":
                    copied_any = True
            # Hide only cells that actually started a document move. Icons /
            # copies / failed docs must stay visible.
            for item in drag_sources:
                path = getattr(item, "file_path", None)
                if path is None:
                    continue
                try:
                    key = str(Path(path)).casefold()
                except OSError:
                    key = str(path).casefold()
                if key in moved_keys:
                    try:
                        item.hide()
                    except RuntimeError:
                        pass
            if moved_keys:
                quiet_end = True
                return Qt.DropAction.MoveAction, False
            if copied_any:
                quiet_end = True
                return Qt.DropAction.CopyAction, False
            # Folder hit but nothing moved (.lnk / failed doc): fall through so
            # a release that is really "out to desktop" can still unpin. Only
            # block unpin while the pointer remains on that folder target.
            if not should_unpin_virtual_drop(drop_pos):
                get_logger().info(
                    "virtual drag: folder miss kept in fence drop=(%s,%s)",
                    drop_pos.x(),
                    drop_pos.y(),
                )
                return Qt.DropAction.IgnoreAction, False

        fence = fence_widget_at(drop_pos, geometry_only=True)
        # Only the icon grid accepts reorder/pin — title/empty body must unpin.
        if fence is not None and fence_accepts_virtual_drop_at(fence, drop_pos):
            if _apply_fence_virtual_drop(
                fence, drag_paths, source_id=source_id, drop_global=drop_pos
            ):
                try:
                    drop_id = str(
                        (getattr(fence, "config", None) or {}).get("id") or ""
                    )
                except Exception:
                    drop_id = ""
                if drop_id and drop_id == source_id:
                    # In-fence reorder: no FG heal / public refresh (desktop flash).
                    quiet_end = True
                return Qt.DropAction.MoveAction, False

        try:
            from src.win_shell import is_visible_external_app_drop_point

            # Visible top only — chrome-skip ``is_external_app_drop_point`` false-
            # positives IDE/Cubism under the desktop band after the first unpin.
            if is_visible_external_app_drop_point(drop_pos.x(), drop_pos.y()):
                get_logger().info(
                    "virtual drag: skip unpin external app drop=(%s,%s)",
                    drop_pos.x(),
                    drop_pos.y(),
                )
                return Qt.DropAction.IgnoreAction, False
        except Exception:
            pass

        # Outside icon grids → unpin to public area (no Explorer OLE involved).
        if (drop_pos - start_global).manhattanLength() < 36:
            return Qt.DropAction.IgnoreAction, False
        if not should_unpin_virtual_drop(drop_pos):
            get_logger().info(
                "virtual drag: skip unpin still on icon grid drop=(%s,%s)",
                drop_pos.x(),
                drop_pos.y(),
            )
            return Qt.DropAction.IgnoreAction, False

        _last_virtual_unpin_pos = QPoint(virtual_unpin_drop_hint(drop_pos))
        try:
            widget._desktidy_unpinned_paths = list(drag_paths)  # type: ignore[attr-defined]
        except Exception:
            pass
        get_logger().info(
            "virtual drag: custom unpin to public count=%s drop=(%s,%s)",
            len(drag_paths),
            drop_pos.x(),
            drop_pos.y(),
        )
        return Qt.DropAction.CopyAction, True
    finally:
        _reveal_widgets_after_custom_drag(concealed)
        _end_overlay_drag_session(reconcile=not quiet_end)
        try:
            widget._desktidy_drag_quiet = bool(quiet_end)  # type: ignore[attr-defined]
        except Exception:
            pass
        get_logger().info(
            "virtual drag: custom end cancelled=%s handoff=%s quiet=%s drop=(%s,%s) count=%s",
            cancelled,
            handoff_external,
            quiet_end,
            drop_pos.x(),
            drop_pos.y(),
            len(drag_paths),
        )


def external_payload_paths_for_virtual_drag(paths: list[Path]) -> list[Path]:
    """Resolve existing paths suitable for CF_HDROP / external file payloads.

    Used by allowlisted mid-drag OLE handoff (WeChat / QQ / Office / browsers)
    and PreferCopy recovery helpers.
    """
    out: list[Path] = []
    seen: set[str] = set()
    for raw in paths:
        p = Path(raw)
        try:
            key = str(p.resolve(strict=False)).casefold()
        except OSError:
            key = str(p).casefold()
        if key in seen:
            continue
        try:
            if not p.exists():
                continue
        except OSError:
            continue
        seen.add(key)
        out.append(p)
    return out


def start_virtual_item_drag(
    widget: QWidget,
    file_path: Path,
    fence_id: str,
    *,
    press_local: QPoint | None = None,
    paths: list[Path] | None = None,
) -> tuple[Qt.DropAction, bool]:
    """Internal virtual-mode drag: reorder / move between fences / unpin.

    Returns (drop_action, unpinned_to_desktop).
    When *paths* is omitted, drag uses Explorer rules via ``paths_for_fence_drag``.

    Always uses the custom non-OLE loop (same as public floats). A full-gesture
    ``QDrag.exec`` with CF_HDROP wakes every AcceptFiles / Electron panel under
    the cursor while crossing to the public area (VPN popups). OLE is only
    started mid-gesture via ``_PublicDragFilter`` when the cursor rests on an
    allowlisted chat / Office / browser process.
    """
    drag_paths = [Path(p) for p in (paths if paths is not None else paths_for_fence_drag(widget))]
    if not drag_paths:
        drag_paths = [Path(file_path)]
    primary = Path(file_path) if file_path else drag_paths[0]
    # Formerly branched: desktop-resident → custom, else → QDrag.exec.
    # Off-desktop pins now share the custom path so fence→public never OLE-wakes
    # background utilities. ``_virtual_drag_all_desktop_resident`` remains for tests.
    return _run_custom_desktop_resident_virtual_drag(
        widget,
        primary,
        fence_id,
        drag_paths,
        press_local=press_local,
    )


def _path_is_dir(path: Path) -> bool:
    try:
        return path.is_dir()
    except OSError:
        return False


# Cross-drive Shell FO_MOVE and directory trees block the UI thread when run sync.
_FS_BACKGROUND_COPY_BYTES = 2 * 1024 * 1024


def _fs_move_needs_background(src: Path, target_dir: Path) -> bool:
    """True when ``move_path_into_folder`` would block the UI (cross-vol / trees).

    Same-volume renames and destination-name collisions stay on the UI thread
    (instant rename, or immediate ``FileExistsError`` — never Shell「重命名」).
    """
    if _path_is_dir(src):
        return True
    if src.parent == target_dir:
        return False
    from src.win_shell import _same_volume

    dest = target_dir / src.name
    try:
        if dest.exists():
            return False
    except OSError:
        pass
    if _same_volume(src, target_dir):
        return False
    return True


def _fs_copy_needs_background(src: Path) -> bool:
    if _path_is_dir(src):
        return True
    try:
        return int(src.stat().st_size) >= _FS_BACKGROUND_COPY_BYTES
    except OSError:
        return False


def _safe_abs_path(path: Path) -> Path:
    """Absolute path without Path.resolve() (network roots can stall the UI)."""
    try:
        return path if path.is_absolute() else path.absolute()
    except OSError:
        return Path(path)


def _path_parent_casefold(path: Path) -> str:
    """Parent directory string for desktop/portal checks — no resolve()."""
    try:
        p = Path(path)
        parent = p.parent if p.is_absolute() else p.absolute().parent
        return str(parent).casefold().rstrip("\\/")
    except OSError:
        return ""


def _is_direct_child_of(path: Path, parent_dir: Path) -> bool:
    """True when *path* is a direct child of *parent_dir* (casefold, no resolve)."""
    parent_cf = _path_parent_casefold(path)
    if not parent_cf:
        return False
    try:
        dir_cf = str(parent_dir).casefold().rstrip("\\/")
    except OSError:
        return False
    return bool(dir_cf and parent_cf == dir_cf)


def _run_shutil_move(src: Path, dest: Path) -> None:
    """Legacy exact-path move (files only). Prefer ``move_path_into_folder``."""
    import shutil

    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(src), str(dest))


def _run_move_into_folder(src: Path, target_dir: Path) -> Path:
    """True move into *target_dir* keeping the original name (no ``_1`` copies)."""
    from src.win_shell import move_path_into_folder

    return move_path_into_folder(src, target_dir)


def _run_transfer_into_folder(src: Path, target_dir: Path, *, move: bool) -> Path:
    from src.win_shell import transfer_path_into_folder

    return transfer_path_into_folder(src, target_dir, move=move)


def _start_background_fs_transfer(
    src: Path,
    target_dir: Path,
    *,
    move: bool,
    on_success,
    on_failure,
) -> None:
    """Move or copy into *target_dir* on a daemon thread; complete on the UI thread."""
    import threading

    from src.app_logging import get_logger
    from src.qt_main_thread import call_on_main_thread, ensure_main_thread_bridge
    from src.ui.toast import show_toast

    ensure_main_thread_bridge()
    op = "move" if move else "copy"
    get_logger().info(
        "fs %s: background start src=%s target=%s", op, src, target_dir
    )

    result_dest: list[Path] = []

    def worker() -> None:
        err: BaseException | None = None
        try:
            result_dest.append(_run_transfer_into_folder(src, target_dir, move=move))
        except BaseException as exc:  # noqa: BLE001 — marshal any failure to UI
            err = exc

        def done() -> None:
            if err is None:
                dest = result_dest[0] if result_dest else target_dir / src.name
                get_logger().info(
                    "fs %s: background ok src=%s dest=%s", op, src, dest
                )
                try:
                    on_success(dest)
                except Exception:
                    get_logger().exception("fs %s: on_success failed", op)
            else:
                get_logger().exception(
                    "fs %s: background failed src=%s target=%s", op, src, target_dir
                )
                try:
                    on_failure(err)
                except Exception:
                    get_logger().exception("fs %s: on_failure failed", op)
                try:
                    show_toast(
                        "移动失败" if move else "复制失败",
                        (
                            f"「{target_dir.name}」里已有同名「{src.name}」"
                            if isinstance(err, FileExistsError)
                            else f"「{src.name}」未能{'移入' if move else '复制到'}目标文件夹"
                        ),
                        msec=4500,
                    )
                except Exception:
                    pass

        call_on_main_thread(done)

    threading.Thread(
        target=worker, name=f"desktidy-fs-{op}", daemon=True
    ).start()


def _start_background_fs_move(
    src: Path,
    target_dir: Path,
    *,
    on_success,
    on_failure,
) -> None:
    """Move into *target_dir* on a daemon thread; complete on the Qt UI thread."""
    _start_background_fs_transfer(
        src, target_dir, move=True, on_success=on_success, on_failure=on_failure
    )


def _start_background_fs_paste(
    work,
    *,
    on_success,
    on_failure,
    toast_title: str = "正在粘贴",
    toast_detail: str = "文件较多或较大，请稍候…",
) -> None:
    """Run a paste/copy batch on a daemon thread; finish on the Qt UI thread."""
    import threading

    from src.app_logging import get_logger
    from src.qt_main_thread import call_on_main_thread, ensure_main_thread_bridge
    from src.ui.toast import show_toast

    ensure_main_thread_bridge()
    show_toast(toast_title, toast_detail, msec=4500)
    get_logger().info("fs paste: background start")

    result: list = []
    err_box: list[BaseException] = []

    def worker() -> None:
        try:
            result.append(work())
        except BaseException as exc:  # noqa: BLE001
            err_box.append(exc)

        def done() -> None:
            if not err_box:
                get_logger().info("fs paste: background ok")
                try:
                    on_success(result[0])
                except Exception:
                    get_logger().exception("fs paste: on_success failed")
            else:
                get_logger().exception("fs paste: background failed")
                try:
                    on_failure(err_box[0])
                except Exception:
                    get_logger().exception("fs paste: on_failure failed")
                try:
                    show_toast("粘贴失败", "部分文件未能粘贴完成", msec=4500, level="warn")
                except Exception:
                    pass

        call_on_main_thread(done)

    threading.Thread(
        target=worker, name="desktidy-fs-paste", daemon=True
    ).start()


def _finalize_virtual_folder_move(
    file_path: Path, fence_id: str, moved_dest: Path | None = None
) -> None:
    """Unpin + save after a virtual item was moved into a folder."""
    from src.app_logging import get_logger
    from src.fence_rules import unpin_paths_from_virtual_fence
    from src.organize_suppress import suppress_desktop_item
    from src.public_desktop import remove_public_paths
    from src.settings import save_settings
    from src.ui.toast import show_toast

    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app is not None else None
    settings = getattr(desk, "settings", None) if desk is not None else None
    if not isinstance(settings, dict):
        return

    try:
        suppress_desktop_item(Path(file_path))
    except Exception:
        pass

    fid = str(fence_id or "")
    for fence in list(settings.get("fences") or []):
        if not isinstance(fence, dict):
            continue
        if fid and str(fence.get("id") or "") != fid:
            continue
        unpin_paths_from_virtual_fence(fence, [Path(file_path)])
        if fid:
            break
    if desk is not None:
        for fw in list(getattr(desk, "fences", []) or []):
            cfg = getattr(fw, "config", None)
            if not isinstance(cfg, dict):
                continue
            if fid and str(cfg.get("id") or "") != fid:
                continue
            unpin_paths_from_virtual_fence(cfg, [Path(file_path)])
            sync = getattr(fw, "_sync_fence_virtual_items", None)
            if callable(sync):
                try:
                    sync()
                except Exception:
                    pass
            # Surgical cell drop — full refresh() rebuilds the translucent HWND.
            remove = getattr(fw, "_remove_virtual_icon_widget", None)
            if callable(remove):
                try:
                    if remove(Path(file_path)):
                        break
                except Exception:
                    pass
            refresh = getattr(fw, "refresh", None)
            if callable(refresh):
                try:
                    refresh()
                except Exception:
                    pass
            break

    dropped_public = False
    try:
        dropped_public = bool(remove_public_paths(settings, [file_path]))
    except Exception:
        dropped_public = False
    if dropped_public:
        rem = getattr(desk, "_remove_public_icon_widget", None) if desk else None
        if callable(rem):
            try:
                rem(file_path)
            except Exception:
                pass

    try:
        save_settings(settings)
    except OSError:
        pass
    if moved_dest is not None:
        try:
            show_toast("已移入文件夹", f"「{file_path.name}」", msec=2800, level="success")
        except Exception:
            pass
    if dropped_public:
        pub_refresh = getattr(desk, "refresh_public_desktop", None) if desk else None
        if callable(pub_refresh):
            QTimer.singleShot(0, lambda: pub_refresh(relayout=False))
    get_logger().info(
        "virtual drag: folder move finalized path=%s dest=%s",
        file_path,
        moved_dest,
    )


def _restore_virtual_folder_move_failure(file_path: Path, fence_id: str) -> None:
    """Undo pre-move hide/suppress when a background folder move fails."""
    from src.app_logging import get_logger
    from src.organize_suppress import clear_organize_suppress

    try:
        clear_organize_suppress(Path(file_path))
    except Exception:
        pass
    try:
        key = str(Path(file_path)).casefold()
    except OSError:
        key = str(file_path).casefold()
    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app is not None else None
    fences = list(getattr(desk, "fences", None) or []) if desk is not None else []
    for fence in fences:
        try:
            if str((fence.config or {}).get("id") or "") != str(fence_id):
                continue
        except Exception:
            continue
        for i in range(fence.items_layout.count()):
            child = fence.items_layout.itemAt(i)
            w = child.widget() if child else None
            if w is None:
                continue
            fp = getattr(w, "file_path", None)
            if fp is None:
                continue
            try:
                same = str(Path(fp)).casefold() == key
            except OSError:
                same = False
            if not same:
                continue
            try:
                w.show()
            except RuntimeError:
                pass
            get_logger().info(
                "virtual drag: restored cell after folder move fail path=%s",
                file_path,
            )
            return


def _move_virtual_into_folder(file_path: Path, folder: Path, fence_id: str) -> str | None:
    """Transfer a document into *folder* (Explorer same-vol move / cross-vol copy).

    Returns ``\"moved\"``, ``\"copied\"``, or ``None`` (skipped / failed).

    「图标」(.lnk/.url) → ``None`` so the caller keeps the pin (no FS swallow).
    Copy leaves the virtual pin in place (source file stays); move unpins.
    """
    from src.app_logging import get_logger
    from src.fence_rules import path_organize_kind
    from src.organize_suppress import suppress_desktop_item
    from src.win_shell import default_fs_drop_is_move, resolve_folder_drop_target

    # Icons stay virtual: pin/unpin only, do not bury the .lnk inside a folder.
    if path_organize_kind(file_path) == "icon":
        get_logger().info(
            "virtual drag: skip folder move for icon path=%s", file_path
        )
        return None

    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app is not None else None
    settings = getattr(desk, "settings", None) if desk is not None else None
    if not isinstance(settings, dict):
        return None

    try:
        src = _safe_abs_path(Path(file_path))
        # Literal drop target (no desktop twin remap) — same for move and copy.
        target = _safe_abs_path(
            Path(resolve_folder_drop_target(Path(folder), for_move=True))
        )
        folder_abs = _safe_abs_path(Path(folder))
        if str(target).casefold() != str(folder_abs).casefold():
            get_logger().info(
                "virtual drag: folder remapped %s -> %s", folder, target
            )
    except OSError:
        return None
    if not src.exists() or not target.is_dir():
        return None
    if src == target:
        return None
    if _path_is_dir(src):
        try:
            target.relative_to(src)
            return None
        except ValueError:
            pass

    do_move = default_fs_drop_is_move(src, target)
    get_logger().info(
        "virtual drag: folder transfer move=%s src=%s target=%s",
        do_move,
        src,
        target,
    )

    if do_move:
        try:
            suppress_desktop_item(Path(file_path))
        except Exception:
            pass

    if src.parent == target:
        get_logger().info(
            "virtual drag: document already in folder=%s path=%s", target, src
        )
        if do_move:
            _finalize_virtual_folder_move(file_path, fence_id, moved_dest=src)
            return "moved"
        try:
            from src.ui.toast import show_toast

            show_toast("已在目标文件夹", f"「{file_path.name}」", msec=2800, level="success")
        except Exception:
            pass
        return "copied"

    needs_bg = (
        _fs_move_needs_background(src, target)
        if do_move
        else _fs_copy_needs_background(src)
    )
    if needs_bg:
        if do_move:
            # Eager unpin so a mid-move fence refresh cannot paint a ghost cell.
            _finalize_virtual_folder_move(file_path, fence_id, moved_dest=None)

        def _ok(dest: Path) -> None:
            from src.ui.toast import show_toast

            try:
                if do_move:
                    show_toast("已移入文件夹", f"「{file_path.name}」", msec=2800, level="success")
                else:
                    show_toast("已复制到文件夹", f"「{file_path.name}」", msec=2800, level="success")
            except Exception:
                pass
            get_logger().info(
                "virtual drag: folder %s finalized path=%s dest=%s",
                "move" if do_move else "copy",
                file_path,
                dest,
            )

        def _fail(_exc: BaseException) -> None:
            if not do_move:
                return
            _restore_virtual_folder_move_failure(file_path, fence_id)
            # Eager unpin already cleared settings — re-pin if the file still exists.
            try:
                if Path(file_path).exists():
                    from src.fence_rules import assign_paths_to_virtual_fence
                    from src.settings import save_settings

                    fence_dict = None
                    for fence in list(settings.get("fences") or []):
                        if isinstance(fence, dict) and str(fence.get("id") or "") == str(
                            fence_id
                        ):
                            fence_dict = fence
                            break
                    if fence_dict is not None:
                        assign_paths_to_virtual_fence(
                            fence_dict, settings, [Path(file_path)]
                        )
                        save_settings(settings)
                    if desk is not None:
                        for fw in list(getattr(desk, "fences", []) or []):
                            cfg = getattr(fw, "config", None)
                            if not isinstance(cfg, dict):
                                continue
                            if str(cfg.get("id") or "") != str(fence_id):
                                continue
                            refresh = getattr(fw, "refresh", None)
                            if callable(refresh):
                                refresh()
                            break
            except Exception:
                get_logger().exception(
                    "virtual drag: re-pin after folder move fail path=%s", file_path
                )

        _start_background_fs_transfer(
            src,
            target,
            move=do_move,
            on_success=_ok,
            on_failure=_fail,
        )
        return "moved" if do_move else "copied"

    try:
        dest = _run_transfer_into_folder(src, target, move=do_move)
        get_logger().info(
            "virtual drag: document %s dest=%s src=%s",
            "moved" if do_move else "copied",
            dest,
            src,
        )
    except FileExistsError:
        get_logger().warning(
            "virtual drag: dest name exists src=%s target=%s", src, target
        )
        try:
            from src.ui.toast import show_toast

            show_toast(
                "无法移入" if do_move else "无法复制",
                f"「{target.name}」里已有同名「{src.name}」",
                msec=4000,
            )
        except Exception:
            pass
        if do_move:
            _restore_virtual_folder_move_failure(file_path, fence_id)
        return None
    except OSError:
        get_logger().exception(
            "virtual drag: folder transfer failed src=%s target=%s", src, target
        )
        if do_move:
            _restore_virtual_folder_move_failure(file_path, fence_id)
        return None
    if do_move:
        _finalize_virtual_folder_move(file_path, fence_id, moved_dest=dest)
        return "moved"
    try:
        from src.ui.toast import show_toast

        show_toast("已复制到文件夹", f"「{file_path.name}」", msec=2800, level="success")
    except Exception:
        pass
    return "copied"


class _PublicDragFilter(QObject):
    """Drive a non-OLE public/fence icon drag until left button release / Escape.

    Desktop / fence / folder targets stay custom (no mid-drag CF_HDROP — that
    wakes VPN panels). Hovering an allowlisted chat / Office / browser arms
    ``handoff_pending``; on **release** over that target the caller delivers
    via clipboard paste / OLE (WeChat rejects post-release ``QDrag``).

    Ghost motion and OLE handoff probes run on a timer — not on every global
    ``MouseMove`` — so cold ``EnumWindows`` / fence scans do not hitch the drag.
    Fence hover uses cached ``fence_widget_at(..., geometry_only=True)`` rects.

    Win32 LMB polling in ``_on_tick`` ends the loop when Qt never delivers
    ``MouseButtonRelease`` (common after hiding DefView-owned float HWNDs).
    """

    _TICK_MS = 16
    _HANDOFF_MS = 50
    _VK_LBUTTON = 0x01

    def __init__(self, loop: QEventLoop, ghost: QLabel, hotspot: QPoint) -> None:
        super().__init__()
        self._loop = loop
        self._ghost = ghost
        self._hotspot = hotspot
        self.drop_pos = QCursor.pos()
        self.cancelled = False
        self.handoff_external = False
        self._handoff_pending = False
        self._external_streak = 0
        self._last_handoff_check_ms = 0
        self._last_ghost_global: QPoint | None = None
        self._fences_cache = self._build_fence_cache()
        self._tick = QTimer()
        self._tick.setInterval(self._TICK_MS)
        self._tick.timeout.connect(self._on_tick)

    def _build_fence_cache(self) -> list[tuple[QWidget, QRect]]:
        cache: list[tuple[QWidget, QRect]] = []
        for top in QApplication.topLevelWidgets():
            if top.__class__.__name__ != "FenceWidget":
                continue
            try:
                if not top.isVisible():
                    continue
                rect = _fence_global_rect(top)
                if rect is not None:
                    cache.append((top, rect))
            except (RuntimeError, Exception):
                continue
        return cache

    def _fence_at(self, pos: QPoint):
        hit = None
        hit_area: int | None = None
        for fence, rect in self._fences_cache:
            if not rect.contains(pos):
                continue
            area = max(1, rect.width() * rect.height())
            if hit is None or hit_area is None or area < hit_area:
                hit = fence
                hit_area = area
        return hit

    def start(self) -> None:
        self._tick.start()
        self._on_tick()

    def stop(self) -> None:
        try:
            self._tick.stop()
        except RuntimeError:
            pass

    def _commit_external_handoff_if_pending(self, pos: QPoint) -> None:
        if not self._handoff_pending:
            return
        self._handoff_pending = False
        try:
            if self._fence_at(pos) is not None:
                return
            try:
                from src.ui.pet_widget import find_pet_trash_target

                if find_pet_trash_target(pos) is not None:
                    return
            except Exception:
                pass
            try:
                import win32gui

                from src.win_shell import _own_hwnd_is_desktop_overlay

                under = int(
                    win32gui.WindowFromPoint((int(pos.x()), int(pos.y()))) or 0
                )
                if under and _own_hwnd_is_desktop_overlay(under):
                    return
            except Exception:
                pass
            from src.win_shell import should_ole_file_handoff_at

            if should_ole_file_handoff_at(pos.x(), pos.y()):
                self.handoff_external = True
        except Exception:
            pass

    def _on_tick(self) -> None:
        pos = QCursor.pos()
        self.drop_pos = pos
        try:
            import ctypes

            if not (ctypes.windll.user32.GetAsyncKeyState(self._VK_LBUTTON) & 0x8000):
                self._commit_external_handoff_if_pending(pos)
                self.drop_pos = pos
                self._loop.quit()
                return
        except Exception:
            pass
        if self._ghost is not None and (
            self._last_ghost_global is None or self._last_ghost_global != pos
        ):
            try:
                self._ghost.move(pos - self._hotspot)
            except RuntimeError:
                pass
            self._last_ghost_global = QPoint(pos)
        self._maybe_handoff_check(pos)

    def _maybe_handoff_check(self, pos: QPoint) -> None:
        now_ms = int(time.perf_counter() * 1000)
        if now_ms - self._last_handoff_check_ms < self._HANDOFF_MS:
            return
        self._last_handoff_check_ms = now_ms
        try:
            # Translucent fence HWNDs are skipped by WindowFromPoint — an
            # app underneath (Chrome/Edge) looked like an OLE target and
            # resolved .lnk → English .exe (百度网盘 → BaiduNetdisk.exe).
            if self._fence_at(pos) is not None:
                self._external_streak = 0
                self._handoff_pending = False
                return
            # Same for the pet sprite: skip-chrome hit-test sees apps under
            # the character and would false-handoff mid crumple aim.
            try:
                from src.ui.pet_widget import find_pet_trash_target

                if find_pet_trash_target(pos) is not None:
                    self._external_streak = 0
                    self._handoff_pending = False
                    return
            except Exception:
                pass
            try:
                import win32gui

                from src.win_shell import _own_hwnd_is_desktop_overlay

                under = int(
                    win32gui.WindowFromPoint((int(pos.x()), int(pos.y()))) or 0
                )
                if under and _own_hwnd_is_desktop_overlay(under):
                    self._external_streak = 0
                    self._handoff_pending = False
                    return
            except Exception:
                pass
            from src.win_shell import should_ole_file_handoff_at

            if should_ole_file_handoff_at(pos.x(), pos.y()):
                self._external_streak += 1
                if self._external_streak >= 3:
                    self._handoff_pending = True
            else:
                self._external_streak = 0
                self._handoff_pending = False
        except Exception:
            self._external_streak = 0

    def eventFilter(self, obj, event) -> bool:  # noqa: ANN001
        et = event.type()
        if et in (
            QEvent.Type.MouseButtonRelease,
            QEvent.Type.NonClientAreaMouseButtonRelease,
        ):
            try:
                if event.button() != Qt.MouseButton.LeftButton:
                    return False
            except Exception:
                return False
            self.drop_pos = QCursor.pos()
            self._commit_external_handoff_if_pending(self.drop_pos)
            self._loop.quit()
            return True
        if et == QEvent.Type.KeyPress:
            try:
                if event.key() == Qt.Key.Key_Escape:
                    self.cancelled = True
                    self.drop_pos = QCursor.pos()
                    self._loop.quit()
                    return True
            except Exception:
                pass
        return False


def _release_public_host_mouse_grab() -> None:
    """Marquee selection on PublicIconHost can steal the drag release."""
    app = QApplication.instance()
    if app is None:
        return
    for top in app.topLevelWidgets():
        if top.__class__.__name__ != "PublicIconHost":
            continue
        abort = getattr(top, "_abort_marquee", None)
        if callable(abort):
            try:
                abort()
            except RuntimeError:
                pass
            continue
        try:
            if top.mouseGrabber() is top:
                top.releaseMouse()
        except RuntimeError:
            pass


def _prepare_external_ole_handoff() -> None:
    """Release grabs and overlay freeze before OLE (WeChat / QQ handoff)."""
    _release_public_host_mouse_grab()
    app = QApplication.instance()
    if app is None:
        return
    for top in list(app.topLevelWidgets()):
        try:
            if top.mouseGrabber() is top:
                top.releaseMouse()
        except RuntimeError:
            pass
    try:
        import ctypes

        ctypes.windll.user32.ReleaseCapture()
    except Exception:
        pass
    try:
        app.processEvents()
    except Exception:
        pass
    _end_overlay_drag_session(reconcile=False)


def _ole_drag_source_widget(fallback: QWidget) -> QWidget:
    """OLE must not start from DefView-owned float/fence HWNDs (deadlocks Explorer)."""
    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app is not None else None
    win = getattr(desk, "window", None) if desk is not None else None
    if isinstance(win, QWidget):
        return win
    return fallback


def _exec_external_file_ole_drag(
    widget: QWidget,
    file_path: Path | list[Path],
    *,
    pixmap: QPixmap | None = None,
    hotspot: QPoint | None = None,
) -> Qt.DropAction:
    """Deliver files to WeChat / QQ / Office after custom public/fence drag."""
    from src.app_logging import get_logger
    from src.win_shell import (
        deliver_files_to_external_chat,
        deliver_files_to_external_window,
    )

    paths = (
        [file_path] if isinstance(file_path, Path) else [Path(p) for p in file_path if p]
    )
    if not paths:
        return Qt.DropAction.IgnoreAction
    primary = paths[0]
    gpos = QCursor.pos()
    gx, gy = int(gpos.x()), int(gpos.y())

    if deliver_files_to_external_chat(gx, gy, paths):
        get_logger().info(
            "external handoff: clipboard paste ok count=%s path=%s",
            len(paths),
            primary,
        )
        return Qt.DropAction.CopyAction

    _prepare_external_ole_handoff()
    lmb_down = False
    try:
        import ctypes

        lmb_down = bool(ctypes.windll.user32.GetAsyncKeyState(0x01) & 0x8000)
    except Exception:
        pass
    if not lmb_down:
        if deliver_files_to_external_window(gx, gy, paths):
            get_logger().info(
                "external handoff: WM_DROPFILES ok count=%s path=%s",
                len(paths),
                primary,
            )
            return Qt.DropAction.CopyAction
        return Qt.DropAction.IgnoreAction

    source = _ole_drag_source_widget(widget)
    drag = QDrag(source)
    mime = QMimeData()
    if not attach_external_file_drag_payload(mime, paths):
        return Qt.DropAction.IgnoreAction
    drag.setMimeData(mime)
    if pixmap is not None and not pixmap.isNull():
        drag.setPixmap(pixmap)
        if hotspot is not None:
            drag.setHotSpot(hotspot)
    watchdog = QTimer()
    watchdog.setSingleShot(True)
    timed_out = False

    def _on_ole_watchdog() -> None:
        nonlocal timed_out
        timed_out = True
        drag.cancel()

    watchdog.timeout.connect(_on_ole_watchdog)
    watchdog.start(30000)
    try:
        result = drag.exec(
            Qt.DropAction.CopyAction | Qt.DropAction.MoveAction,
            Qt.DropAction.CopyAction,
        )
    finally:
        watchdog.stop()
    if timed_out:
        get_logger().warning(
            "external OLE drag: timed out after 30s count=%s path=%s",
            len(paths),
            primary,
        )
        result = Qt.DropAction.IgnoreAction
    if result == Qt.DropAction.IgnoreAction:
        if deliver_files_to_external_window(gx, gy, paths):
            get_logger().info(
                "external OLE drag: WM_DROPFILES fallback ok count=%s path=%s",
                len(paths),
                primary,
            )
            return Qt.DropAction.CopyAction
    # Chat apps sometimes report Move while only copying — keep the desktop file.
    try:
        if result == Qt.DropAction.MoveAction and Path(primary).exists():
            return Qt.DropAction.CopyAction
    except OSError:
        pass
    return result


def _conceal_widgets_for_custom_drag(widgets) -> list[QWidget]:
    """Hide drag sources so only the ghost pixmap moves (no translucency echo)."""
    hidden: list[QWidget] = []
    for widget in widgets:
        try:
            if widget.isVisible():
                widget.hide()
                hidden.append(widget)
        except RuntimeError:
            pass
    return hidden


def _reveal_widgets_after_custom_drag(hidden) -> None:
    for widget in hidden:
        try:
            if not widget.isVisible():
                widget.show()
        except RuntimeError:
            pass


def _show_drag_ghost(ghost: QLabel, pix: QPixmap, hotspot: QPoint) -> None:
    """Show the drag pixmap without activating other topmost windows."""
    from src.win_shell import arm_nonactivating_drag_ghost

    ghost.setPixmap(pix)
    ghost.resize(pix.size())
    ghost.move(QCursor.pos() - hotspot)
    ghost.show()
    try:
        arm_nonactivating_drag_ghost(int(ghost.winId()))
    except Exception:
        pass


_last_public_relocate_batch: list[tuple[Path, int, int]] | None = None


def last_public_relocate_batch() -> list[tuple[Path, int, int]] | None:
    """Multi-float public relocate anchors from the last drag, if any."""
    if not _last_public_relocate_batch:
        return None
    return list(_last_public_relocate_batch)


def _public_widgets_for_paths(desk, paths: list[Path]) -> list[QWidget]:
    """Match live public icon widgets to *paths* (stable order of *paths*)."""
    icons = list(getattr(desk, "public_icons", None) or [])
    by_key: dict[str, QWidget] = {}
    for icon in icons:
        try:
            by_key[str(icon.file_path).casefold()] = icon
        except OSError:
            continue
    out: list[QWidget] = []
    for path in paths:
        try:
            key = str(path).casefold()
        except OSError:
            key = str(path).casefold()
        found = by_key.get(key)
        if found is not None:
            out.append(found)
    return out


def start_public_item_drag(
    widget: QWidget,
    file_path: Path,
    *,
    press_local: QPoint | None = None,
    paths: list[Path] | None = None,
) -> tuple[Qt.DropAction, bool]:
    """Drag public-area icon(s): into a fence, folder, or relocate on the desktop.

    Returns (drop_action, relocated_on_public).

    Market note (Fences / Explorer): ideally one OLE ``IDataObject`` drag end-to-end.
    On DeskTidy overlays owned by SHELLDLL_DefView, ``QDrag.exec`` deadlocks Explorer
    — verified in production. Therefore the desktop / fence / folder path stays a
    custom non-OLE loop. Hovering an allowlisted chat / Office / browser hands off
    to OLE while LMB is still down (WeChat / QQ / …).

    Do not “fix” by forcing OLE for desktop/fence drops.
    """
    global _last_virtual_unpin_pos, _last_public_relocate_batch

    from src.app_logging import get_logger

    app = QApplication.instance()
    if app is None:
        return Qt.DropAction.IgnoreAction, False
    desk = getattr(app, "_desktidy_app", None)

    drag_paths = [Path(p) for p in (paths if paths is not None else [file_path]) if p]
    if not drag_paths:
        drag_paths = [Path(file_path)]
    # Keep the pressed icon first so drop anchors / OLE primary stay stable.
    primary = Path(file_path)
    ordered: list[Path] = []
    seen: set[str] = set()
    for path in [primary, *drag_paths]:
        try:
            key = str(path).casefold()
        except OSError:
            key = str(path).casefold()
        if key in seen:
            continue
        seen.add(key)
        ordered.append(Path(path))
    drag_paths = ordered
    file_path = drag_paths[0]

    _release_public_host_mouse_grab()
    set_drag = getattr(desk, "_set_public_drag_paths", None) if desk else None
    if callable(set_drag):
        set_drag(drag_paths)
    else:
        set_one = getattr(desk, "_set_public_drag_path", None) if desk else None
        if callable(set_one):
            set_one(file_path)

    _begin_overlay_drag_session()
    get_logger().info(
        "public drag: custom (no OLE) begin count=%s path=%s",
        len(drag_paths),
        file_path,
    )

    pix = make_outlined_drag_pixmap(widget)
    if pix.isNull():
        try:
            pix = file_icon_pixmap(file_path, 72)
        except Exception:
            pix = QPixmap()
    if len(drag_paths) > 1 and not pix.isNull():
        try:
            painter = QPainter(pix)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            badge = f"+{len(drag_paths) - 1}"
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(0, 120, 215, 230))
            painter.drawRoundedRect(pix.width() - 36, 4, 32, 18, 6, 6)
            painter.setPen(QColor("white"))
            painter.drawText(
                QRect(pix.width() - 36, 4, 32, 18),
                int(Qt.AlignmentFlag.AlignCenter),
                badge,
            )
            painter.end()
        except Exception:
            pass

    hotspot = _drag_hotspot(widget, press_local) if not pix.isNull() else QPoint(24, 24)
    ghost: QLabel | None = None
    if not pix.isNull():
        ghost = QLabel()
        ghost.setObjectName("desktidyPublicDragGhost")
        ghost.setWindowFlags(
            Qt.WindowType.Tool
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        ghost.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        ghost.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        _show_drag_ghost(ghost, pix, hotspot)

    peer_widgets = _public_widgets_for_paths(desk, drag_paths)
    if widget not in peer_widgets:
        peer_widgets = [widget, *[w for w in peer_widgets if w is not widget]]
    origins: dict[str, QPoint] = {}
    for peer in peer_widgets:
        try:
            origins[str(peer.file_path).casefold()] = peer.mapToGlobal(QPoint(0, 0))
        except Exception:
            pass
    concealed = _conceal_widgets_for_custom_drag(peer_widgets)

    loop = QEventLoop()
    filt = _PublicDragFilter(loop, ghost, hotspot)
    app.installEventFilter(filt)
    filt.start()

    def _drag_timeout() -> None:
        filt.cancelled = True
        loop.quit()

    QTimer.singleShot(45000, _drag_timeout)

    drop_pos = QCursor.pos()
    drop_anchor = _drag_drop_anchor(drop_pos, hotspot)
    cancelled = False
    handoff_external = False
    quiet_end = False
    try:
        loop.exec()
        drop_pos = QPoint(filt.drop_pos)
        drop_anchor = _drag_drop_anchor(drop_pos, hotspot)
        cancelled = bool(filt.cancelled)
        handoff_external = bool(filt.handoff_external)
    finally:
        filt.stop()
        try:
            app.removeEventFilter(filt)
        except Exception:
            pass
        if ghost is not None:
            try:
                ghost.hide()
                ghost.setParent(None)
                ghost.deleteLater()
            except RuntimeError:
                pass
        _reveal_widgets_after_custom_drag(concealed)

    try:
        settings = getattr(desk, "settings", None) if desk is not None else None

        def _peer_for(path: Path) -> QWidget | None:
            key = str(path).casefold()
            for peer in peer_widgets:
                try:
                    if str(peer.file_path).casefold() == key:
                        return peer
                except OSError:
                    continue
            return widget if path == file_path else None

        def _restore_float(
            *, ensure_public: bool = True, place_at: QPoint | None = None
        ) -> None:
            anchor = place_at if place_at is not None else drop_anchor
            primary_origin = origins.get(str(file_path).casefold())
            for path in drag_paths:
                peer = _peer_for(path)
                if ensure_public and desk is not None and settings is not None:
                    if not path_pinned_in_settings(settings, path):
                        try:
                            from src.public_desktop import (
                                add_public_item,
                                find_public_entry,
                                live_fence_rects,
                            )

                            if find_public_entry(settings, path) is None:
                                page_id = 0
                                cur = getattr(desk, "_current_page", None)
                                if callable(cur):
                                    try:
                                        page_id = int(cur())
                                    except Exception:
                                        page_id = 0
                                origin = origins.get(str(path).casefold())
                                ax = int(anchor.x())
                                ay = int(anchor.y())
                                if path != file_path and origin is not None and primary_origin is not None:
                                    ax = int(anchor.x() + (origin.x() - primary_origin.x()))
                                    ay = int(anchor.y() + (origin.y() - primary_origin.y()))
                                add_public_item(
                                    settings,
                                    path,
                                    x=ax,
                                    y=ay,
                                    page_id=page_id,
                                    fence_rects=live_fence_rects(settings, page_id),
                                    auto_arrange=False,
                                    shared=False,
                                )
                        except Exception:
                            get_logger().exception(
                                "public drag: re-add public entry failed path=%s", path
                            )
                try:
                    if peer is not None:
                        peer.show()
                except RuntimeError:
                    pass

            def _deferred() -> None:
                for path in drag_paths:
                    peer = _peer_for(path)
                    try:
                        if peer is not None and not peer.isVisible():
                            peer.show()
                    except RuntimeError:
                        continue
                    try:
                        from src.win_shell import configure_desktop_overlay

                        if peer is not None:
                            target = peer.window() if peer.parentWidget() else peer
                            ensure = getattr(target, "ensure_shell_attached", None)
                            if callable(ensure):
                                ensure(force=False)
                            else:
                                configure_desktop_overlay(target)
                    except Exception:
                        pass
                    restore = (
                        getattr(desk, "_restore_public_icon_widget", None) if desk else None
                    )
                    if callable(restore):
                        try:
                            restore(path, icon=peer)
                        except Exception:
                            pass

            QTimer.singleShot(0, _deferred)

        def _mark_pinned_done() -> None:
            for peer in peer_widgets:
                try:
                    setattr(peer, "_desktidy_pin_deferred", True)
                except Exception:
                    pass

        def _original_place() -> QPoint:
            try:
                return widget.mapToGlobal(QPoint(0, 0))
            except Exception:
                return QPoint(drop_anchor)

        def _pin_all_to_fence(fence) -> bool:
            pin_many = getattr(desk, "pin_public_paths_to_fence", None) if desk else None
            if callable(pin_many):
                try:
                    return bool(pin_many(drag_paths, fence))
                except Exception:
                    get_logger().exception(
                        "public drag: sync pin failed count=%s", len(drag_paths)
                    )
                    return False
            pin_fn = getattr(desk, "pin_public_path_to_fence", None) if desk else None
            ok_all = True
            ok_any = False
            for path in drag_paths:
                ok = False
                if callable(pin_fn):
                    try:
                        ok = bool(pin_fn(path, fence))
                    except Exception:
                        get_logger().exception(
                            "public drag: sync pin failed path=%s", path
                        )
                else:
                    ok = bool(force_pin_path_to_fence(fence, path)) and path_pinned_in_settings(
                        settings, path
                    )
                ok_any = ok_any or ok
                ok_all = ok_all and ok
            return ok_all if ok_any and len(drag_paths) > 1 else ok_any

        if cancelled:
            _restore_float(ensure_public=True, place_at=_original_place())
            return Qt.DropAction.IgnoreAction, False

        if handoff_external:
            payload = external_payload_paths_for_virtual_drag(drag_paths)
            if not payload:
                _restore_float(ensure_public=True, place_at=_original_place())
                return Qt.DropAction.IgnoreAction, False
            get_logger().info(
                "public drag: OLE handoff to external app count=%s path=%s",
                len(payload),
                file_path,
            )
            result = _exec_external_file_ole_drag(
                widget, payload, pixmap=pix, hotspot=hotspot
            )
            _restore_float(ensure_public=False)
            get_logger().info(
                "public drag: OLE handoff result=%s path=%s",
                result.name if hasattr(result, "name") else result,
                file_path,
            )
            return result, False

        # DeskNote before pet trash: pet hit-test is geometry-only and used to
        # recycle files released over a DeskNote window that overlaps the sprite.
        try:
            from src.win_shell import try_open_paths_in_desknote_at

            if try_open_paths_in_desknote_at(drop_pos.x(), drop_pos.y(), drag_paths):
                get_logger().info(
                    "public drag: open in DeskNote count=%s drop=(%s,%s)",
                    len(drag_paths),
                    drop_pos.x(),
                    drop_pos.y(),
                )
                _restore_float(ensure_public=True, place_at=_original_place())
                return Qt.DropAction.IgnoreAction, False
        except Exception:
            get_logger().exception("public drag: DeskNote open check failed")

        fence = fence_widget_at(drop_pos, geometry_only=True)

        if all(path_pinned_in_settings(settings, p) for p in drag_paths):
            get_logger().info("public drag: already pinned count=%s", len(drag_paths))
            _mark_pinned_done()
            if fence is not None and desk is not None and _pin_all_to_fence(fence):
                return Qt.DropAction.CopyAction, False
            handler = getattr(desk, "_on_public_icon_pinned", None) if desk else None
            if callable(handler):
                try:
                    for path in drag_paths:
                        handler(path)
                    return Qt.DropAction.CopyAction, False
                except Exception:
                    get_logger().exception("public drag: pinned cleanup failed")
            _restore_float()
            return Qt.DropAction.IgnoreAction, False

        # Pet trash owns the sprite: ahead of folder/Explorer under chrome.
        # (folder_drop_target_at skips pet via geometry — Explorer under the
        # sprite must not steal crumple when the user aimed at the pet.)
        if _try_deliver_drag_to_pet_trash(
            desk,
            settings if isinstance(settings, dict) else None,
            drag_paths,
            drop_pos,
            public_peers=peer_widgets,
        ):
            quiet_end = True
            _mark_pinned_done()
            return Qt.DropAction.MoveAction, False

        # Folder before fence — dropping onto a folder icon inside a fence must
        # move into that folder, not pin beside it.
        folder = folder_drop_target_at(drop_pos, exclude=file_path)
        if folder is not None:
            get_logger().info(
                "public drag: into folder=%s count=%s drop=(%s,%s)",
                folder,
                len(drag_paths),
                drop_pos.x(),
                drop_pos.y(),
            )
            moved_any = False
            copied_any = False
            unmoved: list[Path] = []
            for path in drag_paths:
                peer = _peer_for(path)
                outcome = _move_public_into_folder(desk, settings, peer, path, folder)
                if outcome == "moved":
                    moved_any = True
                elif outcome == "copied":
                    copied_any = True
                else:
                    unmoved.append(path)
            if moved_any:
                for path in unmoved:
                    peer = _peer_for(path)
                    try:
                        if peer is not None:
                            peer.show()
                    except RuntimeError:
                        pass
                quiet_end = True
                _mark_pinned_done()
                return Qt.DropAction.MoveAction, False
            if copied_any:
                quiet_end = True
                _mark_pinned_done()
                return Qt.DropAction.CopyAction, False
            get_logger().warning(
                "public drag: folder move failed; restoring floats folder=%s",
                folder,
            )
            _restore_float(ensure_public=True)
            return Qt.DropAction.IgnoreAction, False

        if fence is not None and desk is not None:
            try:
                fid = (fence.config or {}).get("id")
            except Exception:
                fid = None
            get_logger().info(
                "public drag: sync pin fence=%s count=%s drop=(%s,%s)",
                fid,
                len(drag_paths),
                drop_pos.x(),
                drop_pos.y(),
            )
            ok = _pin_all_to_fence(fence)
            if ok:
                _mark_pinned_done()
                return Qt.DropAction.CopyAction, False
            if any(path_pinned_in_settings(settings, p) for p in drag_paths):
                handler = getattr(desk, "_on_public_icon_pinned", None)
                if callable(handler):
                    try:
                        for path in drag_paths:
                            if path_pinned_in_settings(settings, path):
                                handler(path)
                        _mark_pinned_done()
                        return Qt.DropAction.CopyAction, False
                    except Exception:
                        get_logger().exception("public drag: partial-pin cleanup failed")
            get_logger().warning(
                "public drag: pin failed; restoring floats count=%s", len(drag_paths)
            )
            _last_virtual_unpin_pos = QPoint(virtual_unpin_drop_hint(drop_pos))
            _restore_float(ensure_public=True)
            return Qt.DropAction.IgnoreAction, False

        try:
            from src.win_shell import is_external_app_drop_point

            if is_external_app_drop_point(drop_pos.x(), drop_pos.y()):
                get_logger().info(
                    "public drag: external release — keep floats in place count=%s",
                    len(drag_paths),
                )
                _restore_float(ensure_public=True, place_at=_original_place())
                return Qt.DropAction.IgnoreAction, False
        except Exception:
            get_logger().exception("public drag: external check failed")

        hint = QPoint(virtual_unpin_drop_hint(drop_pos))
        _last_virtual_unpin_pos = QPoint(hint)
        primary_origin = origins.get(str(file_path).casefold()) or _original_place()
        batch: list[tuple[Path, int, int]] = []
        for path in drag_paths:
            origin = origins.get(str(path).casefold())
            if origin is None:
                if path == file_path:
                    batch.append((path, int(hint.x()), int(hint.y())))
                continue
            nx = int(hint.x() + (origin.x() - primary_origin.x()))
            ny = int(hint.y() + (origin.y() - primary_origin.y()))
            batch.append((path, nx, ny))
        _last_public_relocate_batch = batch
        _restore_float(ensure_public=True)
        return Qt.DropAction.CopyAction, True
    finally:
        _end_overlay_drag_session(reconcile=not quiet_end)
        get_logger().info(
            "public drag: custom end cancelled=%s handoff=%s quiet=%s drop=(%s,%s) count=%s path=%s",
            cancelled,
            handoff_external,
            quiet_end,
            drop_pos.x(),
            drop_pos.y(),
            len(drag_paths),
            file_path,
        )


def _fence_global_rect(fence) -> QRect | None:
    """Screen-space rect for a fence (desktop-band geometry can drift)."""
    try:
        top_left = fence.mapToGlobal(QPoint(0, 0))
        return QRect(top_left, fence.size()).adjusted(-6, -6, 6, 6)
    except RuntimeError:
        return None
    except Exception:
        try:
            return fence.frameGeometry().adjusted(-6, -6, 6, 6)
        except Exception:
            return None


def fence_items_global_rect(fence) -> QRect | None:
    """Screen rect of the icon grid only (not title / empty chrome / margins)."""
    items = getattr(fence, "items_widget", None)
    if items is None:
        return None
    try:
        if not items.isVisible():
            return None
        top_left = items.mapToGlobal(QPoint(0, 0))
        return QRect(top_left, items.size())
    except RuntimeError:
        return None
    except Exception:
        return None


def fence_accepts_virtual_drop_at(fence, global_pos: QPoint | None = None) -> bool:
    """True when release is over the icon grid — reorder/pin, not chrome.

    Full-frame ``fence_widget_at`` + same-fence ``_apply_virtual_drop_paths``
    always returned True (reorder), so dragging out onto title/empty body
    never reached unpin — felt like「拖出一个后，再拖就不好用」on large
    文档 fences.
    """
    if fence is None:
        return False
    pos = global_pos if global_pos is not None else QCursor.pos()
    grid = fence_items_global_rect(fence)
    if grid is not None and grid.isValid() and not grid.isEmpty():
        return grid.contains(pos)
    # Collapsed / no items_widget: keep whole-frame behavior.
    rect = _fence_global_rect(fence)
    return bool(rect is not None and rect.contains(pos))


def fence_widget_at(
    global_pos: QPoint | None = None, *, geometry_only: bool = False
):
    """Return a visible FenceWidget under the cursor, if any.

    ``geometry_only`` skips ``widgetAt`` (unreliable for HWND_BOTTOM overlays)
    and hits fence screen rects — required for public→fence drops.
    """
    pos = global_pos if global_pos is not None else QCursor.pos()
    if not geometry_only:
        try:
            under = QApplication.widgetAt(pos)
        except Exception:
            under = None
        cur = under
        while cur is not None:
            if cur.__class__.__name__ == "FenceWidget":
                try:
                    if cur.isVisible():
                        return cur
                except RuntimeError:
                    return None
                return None
            cur = cur.parentWidget()
    # Prefer the smallest containing fence when frames overlap (last-in-list
    # used to pin into the wrong zone under stacked shelves).
    hit = None
    hit_area = None
    for top in QApplication.topLevelWidgets():
        if top.__class__.__name__ != "FenceWidget" or not top.isVisible():
            continue
        rect = _fence_global_rect(top)
        if rect is None or not rect.contains(pos):
            continue
        area = max(1, rect.width() * rect.height())
        if hit is None or area < hit_area:
            hit = top
            hit_area = area
    return hit


def _exclude_path_key(exclude: Path | None) -> str:
    if exclude is None:
        return ""
    try:
        return str(exclude).casefold().replace("/", "\\")
    except OSError:
        return str(exclude).casefold()


def _resolve_drop_folder(path: Path) -> Path | None:
    """Filesystem folder for a fence/public icon (dirs + folder .lnk → target).

    Uses ``for_move=True`` so drag-into-folder hits the visible folder, not a
    cross-drive desktop stand-in twin (that caused slow FO_MOVE + rename prompts).
    """
    from src.win_shell import resolve_folder_drop_target, _lnk_target_dir

    try:
        if path.is_dir():
            return resolve_folder_drop_target(path, for_move=True)
    except OSError:
        return None
    if path.suffix.lower() != ".lnk":
        return None
    target = _lnk_target_dir(path)
    if target is None:
        return None
    return resolve_folder_drop_target(target, for_move=True)


def public_folder_at(
    global_pos: QPoint | None = None, *, exclude: Path | None = None
) -> Path | None:
    """Return a public desktop folder float under the cursor, if any."""
    pos = global_pos if global_pos is not None else QCursor.pos()
    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app is not None else None
    icons = list(getattr(desk, "public_icons", None) or [])
    exclude_key = _exclude_path_key(exclude)

    hit: Path | None = None
    hit_area: int | None = None
    for icon in icons:
        try:
            if not icon.isVisible():
                continue
            path = Path(getattr(icon, "file_path", "") or "")
            if not path:
                continue
            key = str(path).casefold().replace("/", "\\")
            if exclude_key and key == exclude_key:
                continue
            folder = _resolve_drop_folder(path)
            if folder is None:
                continue
            top_left = icon.mapToGlobal(QPoint(0, 0))
            # Slightly inflate hit target — folder captions are easy to miss.
            rect = QRect(top_left, icon.size()).adjusted(-8, -8, 8, 8)
            if not rect.contains(pos):
                continue
            area = max(1, rect.width() * rect.height())
            if hit is None or area < hit_area:
                hit = folder
                hit_area = area
        except RuntimeError:
            continue
        except Exception:
            continue
    return hit


def fence_folder_at(
    global_pos: QPoint | None = None, *, exclude: Path | None = None
) -> Path | None:
    """Return a folder icon inside a visible fence under the cursor, if any."""
    pos = global_pos if global_pos is not None else QCursor.pos()
    exclude_key = _exclude_path_key(exclude)

    hit: Path | None = None
    hit_area: int | None = None
    for top in QApplication.topLevelWidgets():
        if top.__class__.__name__ != "FenceWidget" or not top.isVisible():
            continue
        layout = getattr(top, "items_layout", None)
        if layout is None:
            continue
        try:
            count = int(layout.count())
        except Exception:
            continue
        for index in range(count):
            try:
                child = layout.itemAt(index)
                widget = child.widget() if child is not None else None
                if widget is None or not widget.isVisible():
                    continue
                path = Path(getattr(widget, "file_path", "") or "")
                if not path:
                    continue
                key = str(path).casefold().replace("/", "\\")
                if exclude_key and key == exclude_key:
                    continue
                folder = _resolve_drop_folder(path)
                if folder is None:
                    continue
                top_left = widget.mapToGlobal(QPoint(0, 0))
                rect = QRect(top_left, widget.size()).adjusted(-8, -8, 8, 8)
                if not rect.contains(pos):
                    continue
                area = max(1, rect.width() * rect.height())
                if hit is None or area < hit_area:
                    hit = folder
                    hit_area = area
            except RuntimeError:
                continue
            except Exception:
                continue
    return hit


def folder_drop_target_at(
    global_pos: QPoint | None = None, *, exclude: Path | None = None
) -> Path | None:
    """Folder under cursor: fence icon, public float, then open Explorer.

    Returns None when the pet sprite owns the point — callers that still check
    folders before pet trash must not steal crumple into an Explorer folder
    under chrome / a folder icon under the character.
    """
    pos = global_pos if global_pos is not None else QCursor.pos()
    try:
        from src.ui.pet_widget import find_pet_trash_target

        if find_pet_trash_target(pos) is not None:
            return None
    except Exception:
        pass
    hit = fence_folder_at(pos, exclude=exclude)
    if hit is not None:
        return hit
    hit = public_folder_at(pos, exclude=exclude)
    if hit is not None:
        return hit
    try:
        from src.win_shell import explorer_folder_path_at

        return explorer_folder_path_at(pos.x(), pos.y())
    except Exception:
        return None


def _try_deliver_drag_to_pet_trash(
    desk,
    settings: dict | None,
    paths: list[Path],
    drop_pos: QPoint,
    *,
    public_peers: list[QWidget] | None = None,
    fence_id: str = "",
    virtual_items: list[QWidget] | None = None,
) -> bool:
    """If release is on the pet sprite, recycle files then drop DeskTidy pins.

    Public/fence icon drags are custom (non-OLE). The pet's Qt AcceptDrops only
    sees Explorer OLE — without this bridge, dropping a desktop float on the pet
    just relocates the icon with no crumple animation.

    Files are sent to the Recycle Bin *before* floats/pins are removed, so a
    failed delete cannot leave a hidden desktop file that later triggers
    Explorer「替换或跳过文件」when the same name is moved back.
    """
    from src.app_logging import get_logger
    from src.ui.pet_widget import deliver_paths_to_pet_trash

    clean = [Path(p) for p in paths if p]
    if not clean:
        return False
    recycled = deliver_paths_to_pet_trash(clean, global_pos=drop_pos)
    if not recycled:
        return False

    get_logger().info(
        "drag: pet trash recycled=%s drop=(%s,%s)",
        len(recycled),
        drop_pos.x(),
        drop_pos.y(),
    )

    recycled_keys = {str(p).casefold() for p in recycled}

    def _is_recycled(path: Path) -> bool:
        try:
            return str(path).casefold() in recycled_keys
        except OSError:
            return False

    if isinstance(settings, dict):
        try:
            from src.public_desktop import remove_public_paths

            remove_public_paths(settings, recycled)
        except Exception:
            pass
        try:
            from src.fence_rules import unpin_paths_from_virtual_fence

            fid = str(fence_id or "")
            for fence in list(settings.get("fences") or []):
                if not isinstance(fence, dict):
                    continue
                if fid and str(fence.get("id") or "") != fid:
                    continue
                unpin_paths_from_virtual_fence(fence, recycled)
                if fid:
                    break
        except Exception:
            pass
        try:
            from src.settings import save_settings

            save_settings(settings)
        except OSError:
            pass

    rem = getattr(desk, "_remove_public_icon_widget", None) if desk else None
    for peer in list(public_peers or []):
        try:
            path = Path(getattr(peer, "file_path", "") or "")
        except OSError:
            path = Path()
        if path and not _is_recycled(path):
            continue
        if callable(rem) and path:
            try:
                rem(path)
                continue
            except Exception:
                pass
        try:
            peer.hide()
        except RuntimeError:
            pass

    if desk is not None and virtual_items:
        fid = str(fence_id or "")
        for fw in list(getattr(desk, "fences", []) or []):
            cfg = getattr(fw, "config", None)
            if not isinstance(cfg, dict):
                continue
            if fid and str(cfg.get("id") or "") != fid:
                continue
            try:
                from src.fence_rules import unpin_paths_from_virtual_fence

                unpin_paths_from_virtual_fence(cfg, recycled)
            except Exception:
                pass
            remove = getattr(fw, "_remove_virtual_icon_widget", None)
            for item in list(virtual_items):
                path = getattr(item, "file_path", None)
                if path is None or not _is_recycled(Path(path)):
                    continue
                if callable(remove):
                    try:
                        remove(Path(path))
                        continue
                    except Exception:
                        pass
                try:
                    item.hide()
                except RuntimeError:
                    pass
            break
    elif virtual_items:
        for item in list(virtual_items):
            path = getattr(item, "file_path", None)
            if path is None or not _is_recycled(Path(path)):
                continue
            try:
                item.hide()
            except RuntimeError:
                pass

    return True


def _snapshot_public_float_entry(settings: dict, file_path: Path) -> dict | None:
    """Copy public-float fields needed to restore after a failed folder move."""
    from src.public_desktop import find_public_entry

    entry = find_public_entry(settings, file_path)
    if not isinstance(entry, dict):
        return None
    snap: dict = {"path": str(file_path)}
    for key in ("x", "y", "page", "shared", "loose"):
        if key in entry:
            snap[key] = entry[key]
    return snap


def _restore_public_float_snapshot(desk, settings: dict | None, snap: dict | None) -> None:
    """Re-pin a public float after a background folder move failed."""
    if not isinstance(settings, dict) or not isinstance(snap, dict):
        return
    raw = snap.get("path")
    if not raw:
        return
    from src.public_desktop import add_public_item
    from src.settings import save_settings

    try:
        path = Path(str(raw))
    except OSError:
        return
    x = snap.get("x")
    y = snap.get("y")
    page = snap.get("page", 0)
    shared = snap.get("shared")
    try:
        page_id = int(page) if page is not None else 0
    except (TypeError, ValueError):
        page_id = 0
    try:
        add_public_item(
            settings,
            path,
            int(x) if x is not None else None,
            int(y) if y is not None else None,
            page_id=page_id,
            auto_arrange=False if x is not None and y is not None else True,
            shared=bool(shared) if shared is not None else None,
        )
        if "loose" in snap:
            from src.public_desktop import find_public_entry

            entry = find_public_entry(settings, path)
            if isinstance(entry, dict):
                entry["loose"] = snap["loose"]
        save_settings(settings)
    except Exception:
        return
    refresh = getattr(desk, "refresh_public_desktop", None) if desk else None
    if callable(refresh):
        try:
            refresh(immediate=True, relayout=False)
        except Exception:
            pass


def _finalize_public_folder_move(
    desk,
    settings: dict | None,
    widget: QWidget | None,
    file_path: Path,
    *,
    moved_dest: Path | None = None,
    restore_on_fail: bool = False,
    restore_snap: dict | None = None,
) -> None:
    from src.organize_suppress import suppress_desktop_item
    from src.public_desktop import remove_public_paths
    from src.settings import save_settings
    from src.ui.toast import show_toast

    if restore_on_fail:
        try:
            if widget is not None:
                widget.show()
        except RuntimeError:
            pass
        if restore_snap is not None:
            _restore_public_float_snapshot(desk, settings, restore_snap)
        return

    try:
        suppress_desktop_item(file_path)
    except Exception:
        pass
    if isinstance(settings, dict):
        remove_public_paths(settings, [file_path])
        try:
            save_settings(settings)
        except OSError:
            pass
    rem = getattr(desk, "_remove_public_icon_widget", None) if desk else None
    if callable(rem):
        try:
            rem(file_path)
        except Exception:
            pass
    else:
        try:
            if widget is not None:
                widget.hide()
        except RuntimeError:
            pass
    if moved_dest is not None:
        try:
            show_toast("已移入文件夹", f"「{file_path.name}」", msec=2800, level="success")
        except Exception:
            pass


def _move_public_into_folder(
    desk,
    settings: dict | None,
    widget: QWidget,
    file_path: Path,
    folder: Path,
) -> str | None:
    """Transfer a public float into *folder* (Explorer same-vol move / cross-vol copy).

    Returns ``\"moved\"``, ``\"copied\"``, or ``None``.
    Move drops the public entry; copy leaves the float in place.
    Shortcuts stay as floats (virtual semantics).
    """
    from src.app_logging import get_logger
    from src.fence_rules import path_organize_kind
    from src.organize_suppress import suppress_desktop_item
    from src.win_shell import default_fs_drop_is_move, resolve_folder_drop_target

    if path_organize_kind(file_path) == "icon":
        get_logger().info(
            "public drag: skip folder move for icon path=%s", file_path
        )
        return None

    try:
        src = _safe_abs_path(Path(file_path))
        target = _safe_abs_path(
            Path(resolve_folder_drop_target(Path(folder), for_move=True))
        )
    except OSError:
        return None
    if not src.exists() or not target.is_dir():
        return None
    if src == target:
        return None
    # Don't move/copy a folder into itself / a descendant.
    if _path_is_dir(src):
        try:
            target.relative_to(src)
            return None
        except ValueError:
            pass

    do_move = default_fs_drop_is_move(src, target)
    get_logger().info(
        "public drag: folder transfer move=%s src=%s target=%s",
        do_move,
        src,
        target,
    )

    if do_move:
        try:
            suppress_desktop_item(file_path)
        except Exception:
            pass
    if src.parent == target:
        if do_move:
            _finalize_public_folder_move(
                desk, settings, widget, file_path, moved_dest=src
            )
            return "moved"
        try:
            from src.ui.toast import show_toast

            show_toast("已在目标文件夹", f"「{file_path.name}」", msec=2800, level="success")
        except Exception:
            pass
        return "copied"

    needs_bg = (
        _fs_move_needs_background(src, target)
        if do_move
        else _fs_copy_needs_background(src)
    )
    if needs_bg:
        snap = (
            _snapshot_public_float_entry(settings, file_path)
            if isinstance(settings, dict) and do_move
            else None
        )
        if do_move:
            # Eager drop: hide + scrub settings before Shell FO_MOVE returns.
            _finalize_public_folder_move(
                desk, settings, widget, file_path, moved_dest=None
            )

        def _ok(dest: Path) -> None:
            if do_move:
                _finalize_public_folder_move(
                    desk, settings, widget, file_path, moved_dest=dest
                )
            else:
                try:
                    from src.ui.toast import show_toast

                    show_toast("已复制到文件夹", f"「{file_path.name}」", msec=2800, level="success")
                except Exception:
                    pass

        def _fail(_exc: BaseException) -> None:
            if not do_move:
                return
            from src.organize_suppress import clear_organize_suppress

            try:
                clear_organize_suppress(file_path)
            except Exception:
                pass
            _finalize_public_folder_move(
                desk,
                settings,
                widget,
                file_path,
                restore_on_fail=True,
                restore_snap=snap,
            )

        _start_background_fs_transfer(
            src, target, move=do_move, on_success=_ok, on_failure=_fail
        )
        return "moved" if do_move else "copied"

    try:
        dest = _run_transfer_into_folder(src, target, move=do_move)
    except FileExistsError:
        from src.ui.toast import show_toast

        if do_move:
            from src.organize_suppress import clear_organize_suppress

            try:
                clear_organize_suppress(file_path)
            except Exception:
                pass
        try:
            show_toast(
                "无法移入" if do_move else "无法复制",
                f"「{target.name}」里已有同名「{src.name}」",
                msec=4000,
            )
        except Exception:
            pass
        return None
    except OSError:
        if do_move:
            from src.organize_suppress import clear_organize_suppress

            try:
                clear_organize_suppress(file_path)
            except Exception:
                pass
        return None
    if do_move:
        _finalize_public_folder_move(
            desk, settings, widget, file_path, moved_dest=dest
        )
        return "moved"
    try:
        from src.ui.toast import show_toast

        show_toast("已复制到文件夹", f"「{file_path.name}」", msec=2800, level="success")
    except Exception:
        pass
    return "copied"


def should_unpin_virtual_drop(global_pos: QPoint | None = None) -> bool:
    """True if release is not over any fence *icon grid* (chrome counts as desktop)."""
    pos = global_pos if global_pos is not None else QCursor.pos()
    fence = fence_widget_at(pos, geometry_only=True)
    if fence is None:
        return True
    return not fence_accepts_virtual_drop_at(fence, pos)


def should_treat_as_virtual_unpin(
    *,
    catcher_accepted: bool,
    drop_action: Qt.DropAction,
    start_global: QPoint | None,
    end_global: QPoint | None,
    min_unpin_distance: int = 36,
) -> bool:
    """Decide whether a finished in-fence drag should unpin to the public area.

    Qt often returns ``IgnoreAction`` for desktop-band overlays even when the
    user released on a fence icon. Treating every IgnoreAction as unpin made
    a plain click (tiny jitter) look like 「点一下图标就消失」.
    """
    if catcher_accepted:
        return True
    if drop_action != Qt.DropAction.IgnoreAction:
        return False
    end = end_global if end_global is not None else QCursor.pos()
    if start_global is not None:
        try:
            if (end - start_global).manhattanLength() < int(min_unpin_distance):
                return False
        except Exception:
            pass
    # Only unpin when the pointer is clearly outside every fence frame.
    return should_unpin_virtual_drop(end)


def _raise_fences_for_drop() -> None:
    """Qt-only raise for in-app z-order; leave native desktop-band stacking alone.

    Native topmost restacks during ``QDrag.exec`` deadlock Explorer OLE when
    overlays are owned by SHELLDLL_DefView.
    """
    for top in QApplication.topLevelWidgets():
        if top.__class__.__name__ != "FenceWidget" or not top.isVisible():
            continue
        try:
            top.raise_()
        except Exception:
            pass


def _make_public_drag_mime(file_path: Path) -> QMimeData:
    mime = QMimeData()
    mime.setData(DESKTIDY_VIRTUAL_MIME, QByteArray(str(file_path).encode("utf-8")))
    mime.setData(
        DESKTIDY_SOURCE_FENCE_MIME,
        QByteArray(PUBLIC_SOURCE_FENCE_ID.encode("utf-8")),
    )
    # Virtual formats only — file URL payloads make Explorer join the OLE drop
    # on DefView and deadlock with our desktop-band overlays.
    mime.setData("Preferred DropEffect", QByteArray(struct.pack("<I", _DROPEFFECT_COPY)))
    return mime


def force_pin_path_to_fence(fence, file_path: Path) -> bool:
    """Pin a path into a fence when Qt drop targeting missed the overlay.

    Applies synchronously (caller is already outside ``QDrag.exec``). Never use
    ``_import_virtual_drop`` here — that only schedules work and can lie True.
    """
    if fence is None:
        return False
    apply = getattr(fence, "_apply_virtual_drop_paths", None)
    if not callable(apply):
        return False
    try:
        return bool(
            apply(
                [Path(file_path)],
                source_id=PUBLIC_SOURCE_FENCE_ID,
                insert_at=None,
            )
        )
    except Exception:
        return False


def widget_is_under_desktidy_fence(widget: QWidget | None) -> bool:
    """True if widget is (inside) a FenceWidget overlay."""
    cur = widget
    while cur is not None:
        if cur.__class__.__name__ == "FenceWidget":
            return True
        cur = cur.parentWidget()
    return False


class _DesktopUnpinCatcher(QWidget):
    """Fullscreen underlay that accepts virtual-item drops as 「移出分区」."""

    def __init__(self) -> None:
        super().__init__(None)
        self.accepted_unpin = False
        self.drop_global_pos: QPoint | None = None
        self.setObjectName("desktidyUnpinCatcher")
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowStaysOnBottomHint
        )
        # Nearly invisible but still hit-testable (full WA_Translucent often misses drops).
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setWindowOpacity(0.01)
        self.setStyleSheet("background: #010101;")
        self.setAcceptDrops(True)

    def _cursor_over_fence(self) -> bool:
        """True when the pointer is over a fence — let the fence take the drop."""
        return not should_unpin_virtual_drop()

    def dragEnterEvent(self, event) -> None:
        mime = event.mimeData()
        if mime is not None and mime.hasFormat(DESKTIDY_VIRTUAL_MIME):
            if self._cursor_over_fence():
                event.ignore()
                return
            event.setDropAction(Qt.DropAction.CopyAction)
            event.accept()
            return
        event.ignore()

    def dragMoveEvent(self, event) -> None:
        self.dragEnterEvent(event)

    def dropEvent(self, event) -> None:
        mime = event.mimeData()
        if mime is not None and mime.hasFormat(DESKTIDY_VIRTUAL_MIME):
            # Never steal cross-fence drops if z-order briefly puts us on top.
            if self._cursor_over_fence():
                event.ignore()
                return
            self.accepted_unpin = True
            try:
                self.drop_global_pos = event.globalPosition().toPoint()
            except Exception:
                self.drop_global_pos = QCursor.pos()
            event.setDropAction(Qt.DropAction.CopyAction)
            event.accept()
            return
        event.ignore()


def select_all_fence_items(anchor: QWidget) -> None:
    items = iter_fence_selectable_items(anchor)
    with fence_selection_batch(items[0] if items else anchor):
        for item in items:
            setter = getattr(item, "set_selected", None)
            if callable(setter):
                setter(True)
    fence = find_fence_widget(anchor)
    if fence is not None and items:
        fence._selection_anchor = items[0]  # type: ignore[attr-defined]
    mark_desktop_selection_live(True if items else False)


def is_public_icon_context(anchor: QWidget | None) -> bool:
    if anchor is None:
        return False
    name = anchor.__class__.__name__
    if name in ("PublicIconWidget", "PublicIconHost"):
        return True
    return find_public_icon_host(anchor) is not None


def select_all_public_items(anchor: QWidget) -> None:
    items = iter_public_selectable_items(anchor)
    with public_selection_batch(items[0] if items else anchor):
        for item in items:
            setter = getattr(item, "set_selected", None)
            if callable(setter):
                setter(True)
    host = find_public_icon_host(anchor)
    if host is not None and items:
        host._selection_anchor = items[0]  # type: ignore[attr-defined]
    mark_desktop_selection_live(True if items else False)


def selected_paths_from_anchor(anchor: QWidget) -> list[Path]:
    if is_public_icon_context(anchor):
        selected = selected_public_items(anchor)
        if selected:
            return [
                Path(w.file_path)
                for w in selected
                if getattr(w, "file_path", None) is not None
            ]
        path = getattr(anchor, "file_path", None)
        return [Path(path)] if path is not None else []
    selected = selected_fence_items(anchor)
    if selected:
        return [Path(w.file_path) for w in selected if getattr(w, "file_path", None)]
    path = getattr(anchor, "file_path", None)
    return [Path(path)] if path is not None else []


def _suppress_desktop_trash(path: Path) -> None:
    """Briefly mute watcher noise for *this path* during recycle."""
    try:
        from src.organize_suppress import suppress_desktop_item

        suppress_desktop_item(Path(path))
    except Exception:
        pass


def _clear_desktop_trash_suppress(path: Path) -> None:
    """Drop path suppress after recycle UI is done (same-name re-drop must work)."""
    try:
        from src.organize_suppress import clear_organize_suppress

        clear_organize_suppress(Path(path))
    except Exception:
        pass


def _hide_fence_item_for_path(fence, path: Path) -> None:
    getter = getattr(fence, "_icon_item_widgets", None) if fence is not None else None
    if not callable(getter):
        return
    try:
        key = str(path).casefold()
    except OSError:
        key = str(path).casefold()
    for item in getter():
        fp = getattr(item, "file_path", None)
        if fp is None:
            continue
        try:
            same = str(fp).casefold() == key
        except OSError:
            same = Path(fp) == Path(path)
        if same:
            try:
                item.hide()
            except RuntimeError:
                pass
            return


def _drop_fence_item_after_trash(fence, path: Path, *, portal: bool) -> None:
    """Unpin / drop one cell after Recycle Bin — no full fence or desktop refresh."""
    _drop_fence_items_after_trash(fence, [Path(path)], portal=portal)


def _drop_fence_items_after_trash(fence, paths: list[Path], *, portal: bool) -> None:
    """Unpin / drop many cells after Recycle Bin — one settings write / compact."""
    if fence is None:
        return
    batch = [Path(p) for p in paths if p is not None]
    if not batch:
        return
    if portal:
        remove_many = getattr(fence, "_remove_virtual_icon_widgets", None)
        if callable(remove_many):
            try:
                if remove_many(batch):
                    return
            except Exception:
                pass
        remove = getattr(fence, "_remove_virtual_icon_widget", None)
        if callable(remove):
            ok = True
            for path in batch:
                try:
                    if not remove(path):
                        ok = False
                except Exception:
                    ok = False
            if ok:
                return
        refresh = getattr(fence, "refresh", None)
        if callable(refresh):
            try:
                refresh()
            except Exception:
                pass
        return
    impl_many = getattr(fence, "_unpin_virtual_paths_impl", None)
    if callable(impl_many):
        try:
            impl_many(batch, place_on_public=False)
            return
        except Exception:
            pass
    for path in batch:
        impl = getattr(fence, "_unpin_virtual_item_impl", None)
        if callable(impl):
            try:
                impl(path, place_on_public=False)
                continue
            except Exception:
                pass
        from src.fence_rules import unpin_paths_from_virtual_fence
        from src.settings import save_settings

        cfg = getattr(fence, "config", None)
        if isinstance(cfg, dict) and unpin_paths_from_virtual_fence(cfg, [Path(path)]):
            try:
                settings = getattr(fence, "settings", None)
                if isinstance(settings, dict):
                    save_settings(settings)
            except Exception:
                pass
            remove = getattr(fence, "_remove_virtual_icon_widget", None)
            if callable(remove):
                try:
                    remove(path)
                except Exception:
                    pass


def delete_selected_fence_items(anchor: QWidget) -> None:
    """Delete selected (or focused) items to the Recycle Bin and unpin."""
    from src.fence_rules import is_portal_fence

    if is_public_icon_context(anchor):
        delete_selected_public_items(anchor)
        return

    paths = selected_paths_from_anchor(anchor)
    if not paths:
        return
    fence = find_fence_widget(anchor)
    portal = False
    if fence is not None:
        cfg = getattr(fence, "config", None)
        portal = isinstance(cfg, dict) and is_portal_fence(cfg)

    recycled: list[Path] = []
    for path in paths:
        _suppress_desktop_trash(path)
        _hide_fence_item_for_path(fence, path)
        try:
            delete_to_trash(path)
        except Exception:
            _clear_desktop_trash_suppress(path)
            continue
        recycled.append(Path(path))

    if recycled:
        _drop_fence_items_after_trash(fence, recycled, portal=portal)
        for path in recycled:
            _clear_desktop_trash_suppress(path)

    if fence is not None and not portal:
        abort = getattr(fence, "_abort_item_interaction", None)
        if callable(abort):
            try:
                abort()
            except Exception:
                pass


def delete_selected_public_items(anchor: QWidget) -> None:
    """Trash selected public floats and drop their public_desktop_items entries."""
    items = list(selected_public_items(anchor) or [])
    if not items:
        path = getattr(anchor, "file_path", None)
        if path is None:
            return
        items = [anchor]
    recycled: list[Path] = []
    for item in items:
        raw = getattr(item, "file_path", None)
        if raw is None:
            continue
        path = Path(raw)
        _suppress_desktop_trash(path)
        try:
            item.hide()
        except RuntimeError:
            pass
        try:
            delete_to_trash(path)
        except Exception:
            _clear_desktop_trash_suppress(path)
            try:
                item.show()
            except RuntimeError:
                pass
            continue
        recycled.append(path)

    if not recycled:
        return

    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app is not None else None
    settings = getattr(desk, "settings", None) if desk is not None else None
    if isinstance(settings, dict):
        try:
            from src.public_desktop import remove_public_paths
            from src.settings import save_settings

            remove_public_paths(settings, recycled)
            try:
                save_settings(settings)
            except OSError:
                pass
        except Exception:
            pass
    rem = getattr(desk, "_remove_public_icon_widget", None) if desk else None
    if callable(rem):
        for path in recycled:
            try:
                rem(path)
            except Exception:
                pass
            _clear_desktop_trash_suppress(path)
        return

    # Headless / unit tests without DeskTidyApp — emit per surviving widget.
    by_key = {
        str(getattr(item, "file_path", "")).casefold(): item
        for item in items
        if getattr(item, "file_path", None) is not None
    }
    for path in recycled:
        item = by_key.get(str(path).casefold(), anchor)
        sig = getattr(item, "removed", None)
        if sig is not None:
            try:
                sig.emit(path)
            except Exception:
                pass
        _clear_desktop_trash_suppress(path)


_DELETE_SHELL_VERBS = frozenset({"delete", "recycle"})


def is_delete_shell_verb(verb: str | None) -> bool:
    return str(verb or "").strip().lower() in _DELETE_SHELL_VERBS


def prepare_shell_item_invoke(
    widget: QWidget,
    path: Path,
    verb: str = "",
    *,
    related_paths: list[Path] | None = None,
) -> None:
    """Before IContextMenu InvokeCommand: Recycle Bin must not rebuild overlays."""
    if not is_delete_shell_verb(verb):
        return
    paths = [Path(p) for p in (related_paths or [path]) if p is not None]
    if not paths:
        paths = [Path(path)]
    for p in paths:
        _suppress_desktop_trash(p)
    if is_public_icon_context(widget):
        selected = selected_public_items(widget)
        targets = selected if selected else [widget]
        keys = {str(p).casefold() for p in paths}
        for item in targets:
            fp = getattr(item, "file_path", None)
            if fp is None:
                continue
            if str(fp).casefold() not in keys and item is not widget:
                continue
            try:
                item.hide()
            except RuntimeError:
                pass
        return
    fence = find_fence_widget(widget)
    for p in paths:
        _hide_fence_item_for_path(fence, p)
    try:
        widget.hide()
    except RuntimeError:
        pass


def _path_exists_for_shell_cleanup(path: Path) -> bool:
    try:
        return bool(Path(path).exists())
    except OSError:
        return False


def _offer_missing_icon_cleanup(widget: QWidget, gone: list[Path]) -> bool:
    """Qt fallback when IContextMenu cannot bind a missing path.

    Market shell menu is impossible for deleted paths; DeskTidy-only「移除失效图标」
    clears the ghost float/pin. Returns True when the user confirmed removal.
    """
    from PyQt6.QtGui import QCursor
    from PyQt6.QtWidgets import QApplication, QMenu

    if not gone:
        return False
    label = (
        "移除失效图标"
        if len(gone) == 1
        else f"移除 {len(gone)} 个失效图标"
    )
    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app is not None else None
    begin = getattr(desk, "_begin_desktop_popup", None) if desk else None
    end_later = getattr(desk, "_end_desktop_popup_later", None) if desk else None
    if callable(begin):
        begin()
    menu = QMenu()
    try:
        action = menu.addAction(label)
        chosen = menu.exec(QCursor.pos())
        return chosen is action
    finally:
        menu.deleteLater()
        if callable(end_later):
            end_later(400)


def after_shell_file_menu(
    widget: QWidget,
    path: Path,
    *,
    shown: bool,
    related_paths: list[Path] | None = None,
) -> None:
    """After the shell menu: drop recycled cells; do not rebuild the desktop."""
    paths = [Path(p) for p in (related_paths or [path]) if p is not None]
    if not paths:
        paths = [Path(path)]
    # Preserve click order / dedupe.
    deduped: list[Path] = []
    seen: set[str] = set()
    for p in paths:
        key = str(p).casefold()
        if key in seen:
            continue
        seen.add(key)
        deduped.append(p)
    paths = deduped

    still = [p for p in paths if _path_exists_for_shell_cleanup(p)]
    gone = [p for p in paths if p not in still]
    if still:
        # Delete verb hid cells before InvokeCommand; restore if recycle failed.
        if is_public_icon_context(widget):
            selected = selected_public_items(widget)
            targets = selected if selected else [widget]
            keys = {str(p).casefold() for p in still}
            for item in targets:
                fp = getattr(item, "file_path", None)
                if fp is None or str(fp).casefold() not in keys:
                    continue
                try:
                    if not item.isVisible():
                        item.show()
                except RuntimeError:
                    pass
        else:
            fence = find_fence_widget(widget)
            getter = getattr(fence, "_icon_item_widgets", None) if fence else None
            if callable(getter):
                keys = {str(p).casefold() for p in still}
                for item in getter():
                    fp = getattr(item, "file_path", None)
                    if fp is None or str(fp).casefold() not in keys:
                        continue
                    try:
                        if not item.isVisible():
                            item.show()
                    except RuntimeError:
                        pass
            try:
                if not widget.isVisible() and Path(path) in still:
                    widget.show()
            except RuntimeError:
                pass
        for p in still:
            _clear_desktop_trash_suppress(p)
    if not gone:
        return
    if not shown:
        # Shell IContextMenu cannot attach to a missing path (ghost float after
        # folder move). Offer DeskTidy pin cleanup instead of silent no-op.
        if not _offer_missing_icon_cleanup(widget, gone):
            for p in gone:
                _clear_desktop_trash_suppress(p)
            return
    for p in gone:
        _suppress_desktop_trash(p)
    if is_public_icon_context(widget):
        # Batch drop by path — do not emit removed in a loop (first emit can
        # deleteLater sibling widgets and skip later paths).
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        settings = getattr(desk, "settings", None) if desk is not None else None
        if isinstance(settings, dict):
            try:
                from src.public_desktop import remove_public_paths
                from src.settings import save_settings

                remove_public_paths(settings, gone)
                try:
                    save_settings(settings)
                except OSError:
                    pass
            except Exception:
                pass
        rem = getattr(desk, "_remove_public_icon_widget", None) if desk else None
        if callable(rem):
            for p in gone:
                try:
                    rem(Path(p))
                except Exception:
                    pass
        else:
            selected = selected_public_items(widget)
            by_key: dict[str, QWidget] = {}
            for item in selected or ():
                fp = getattr(item, "file_path", None)
                if fp is not None:
                    by_key[str(fp).casefold()] = item
            by_key.setdefault(str(path).casefold(), widget)
            for p in gone:
                item = by_key.get(str(p).casefold(), widget)
                try:
                    item.hide()
                except RuntimeError:
                    pass
                sig = getattr(item, "removed", None)
                if sig is not None:
                    try:
                        sig.emit(Path(p))
                    except Exception:
                        pass
        for p in gone:
            _clear_desktop_trash_suppress(p)
        return
    fence = find_fence_widget(widget)
    portal = False
    if fence is not None:
        from src.fence_rules import is_portal_fence

        cfg = getattr(fence, "config", None)
        portal = isinstance(cfg, dict) and is_portal_fence(cfg)
    _drop_fence_items_after_trash(fence, gone, portal=portal)
    for p in gone:
        _clear_desktop_trash_suppress(p)


def _fs_copy_entry(src: Path, dest: Path) -> None:
    """Copy a file or directory tree to *dest* (dest must not exist)."""
    import shutil

    if src.is_dir():
        shutil.copytree(src, dest)
    else:
        shutil.copy2(src, dest)


def _paste_sources_need_background(
    sources: list[Path],
    target_dir: Path,
    *,
    effect,
    skip_fs=None,
) -> bool:
    """True when a paste batch would block the UI (dirs, FO_MOVE, large copies)."""
    from src.shell_clipboard import DROPEFFECT_MOVE

    is_move = effect == DROPEFFECT_MOVE
    for src in sources:
        try:
            if not src.exists():
                continue
        except OSError:
            continue
        if skip_fs is not None:
            try:
                if skip_fs(src):
                    continue
            except Exception:
                pass
        if is_move:
            if _fs_move_needs_background(src, target_dir):
                return True
        elif _fs_copy_needs_background(src):
            return True
    return False


def _land_paste_sources_to_folder(
    sources: list[Path],
    target_dir: Path,
    *,
    effect,
    skip_fs=None,
) -> tuple[list[Path], list[tuple[Path, Path]], bool]:
    """Copy/move clipboard sources into *target_dir* (worker-thread safe).

    Returns ``(landed, moved_pairs, moves_ok)``. On cut (MOVE), a failed move
    does **not** fall back to copy — that left duplicates on the desktop.
    """
    from src.shell_clipboard import DROPEFFECT_MOVE
    from src.win_shell import move_path_into_folder, unique_dest_path

    landed: list[Path] = []
    moved_pairs: list[tuple[Path, Path]] = []
    moves_ok = True
    for src in sources:
        try:
            if not src.exists():
                continue
            if skip_fs is not None and skip_fs(src):
                if effect == DROPEFFECT_MOVE:
                    landed.append(src)
                continue
            if effect == DROPEFFECT_MOVE:
                try:
                    dest = Path(move_path_into_folder(src, target_dir))
                    landed.append(dest)
                    moved_pairs.append((src, dest))
                    continue
                except Exception:
                    moves_ok = False
                    continue
            dest = unique_dest_path(target_dir / src.name)
            _fs_copy_entry(src, dest)
            landed.append(dest)
        except Exception:
            if effect == DROPEFFECT_MOVE:
                moves_ok = False
            continue
    if landed or moved_pairs:
        try:
            from src.path_stat_cache import invalidate_path_stat_cache

            # Drop negative present=False hits (OneDrive/AV/probe) so the
            # immediate fence refresh does not filter out just-landed pins.
            for path in landed:
                invalidate_path_stat_cache(path)
            for old, new in moved_pairs:
                invalidate_path_stat_cache(old)
                invalidate_path_stat_cache(new)
        except Exception:
            pass
    return landed, moved_pairs, moves_ok


def _unpack_paste_result(
    result: tuple,
) -> tuple[list[Path], list[tuple[Path, Path]], bool]:
    if len(result) >= 3:
        return result[0], result[1], bool(result[2])
    return result[0], result[1], True


def _rewrite_pins_after_fs_move(settings: dict, pairs: list[tuple[Path, Path]]) -> None:
    """Keep virtual/public pins pointing at the post-move path."""
    from src.fence_rules import rewrite_pinned_path

    for old, new in pairs:
        try:
            if str(Path(old)).casefold() == str(Path(new)).casefold():
                continue
            rewrite_pinned_path(settings, old, new)
        except Exception:
            continue


def _demote_clipboard_after_cut_paste(paths: list[Path]) -> None:
    """Explorer-like: after a successful cut-paste, drop MOVE so a second paste copies."""
    try:
        from src.shell_clipboard import clipboard_set_files

        existing = [Path(p) for p in paths if Path(p).exists()]
        if existing:
            clipboard_set_files(existing, cut=False)
        set_clipboard_cut_paths(None)
    except Exception:
        try:
            set_clipboard_cut_paths(None)
        except Exception:
            pass


def paste_files_into_fence(
    anchor: QWidget,
    *,
    sources: list[Path] | None = None,
    effect: int | None = None,
) -> bool:
    """Paste CF_HDROP files into this fence (portal folder or desktop+pin)."""
    from src.fence_rules import (
        assign_paths_to_virtual_fence,
        get_portal_path,
        is_portal_fence,
    )
    from src.settings import get_desktop_path, save_settings
    from src.shell_clipboard import DROPEFFECT_MOVE, clipboard_get_files_with_effect

    fence = find_fence_widget(anchor)
    if fence is None and anchor.__class__.__name__ == "FenceWidget":
        fence = anchor
    if fence is None:
        return False
    if sources is None or effect is None:
        sources, effect = clipboard_get_files_with_effect()
    if not sources:
        return False
    cfg = getattr(fence, "config", None)
    settings = getattr(fence, "settings", None)
    if not isinstance(settings, dict):
        app = QApplication.instance()
        desk_app = getattr(app, "_desktidy_app", None) if app else None
        settings = getattr(desk_app, "settings", None) if desk_app is not None else None

    # Folder Portal: paste is a real FS copy/move into the mirrored folder.
    if isinstance(cfg, dict) and is_portal_fence(cfg):
        portal = get_portal_path(cfg)
        if portal is None:
            return False
        try:
            portal.mkdir(parents=True, exist_ok=True)
        except OSError:
            return False

        def _skip_portal_fs(src: Path) -> bool:
            return _is_direct_child_of(src, portal) and effect == DROPEFFECT_MOVE

        def _finish_portal(result: tuple) -> None:
            landed, moved_pairs, moves_ok = _unpack_paste_result(result)
            if not landed:
                return
            try:
                from src.path_stat_cache import invalidate_path_stat_cache

                for path in landed:
                    invalidate_path_stat_cache(path)
                for old, new in moved_pairs:
                    invalidate_path_stat_cache(old)
                    invalidate_path_stat_cache(new)
            except Exception:
                pass
            if isinstance(settings, dict) and moved_pairs:
                _rewrite_pins_after_fs_move(settings, moved_pairs)
                try:
                    from src.public_desktop import remove_public_paths

                    remove_public_paths(
                        settings, [Path(old) for old, _new in moved_pairs]
                    )
                except Exception:
                    pass
                try:
                    save_settings(settings)
                except OSError:
                    pass
                # MOVE off desktop: drop public floats immediately (do not wait
                # for sticky watcher prune ~45s).
                scrub_live_icons_for_claimed_paths(
                    [Path(old) for old, _new in moved_pairs]
                )
            if effect == DROPEFFECT_MOVE and moves_ok:
                _demote_clipboard_after_cut_paste(landed)
            refresh = getattr(fence, "refresh", None)
            if callable(refresh):
                # Force: debounce soft refresh can skip while portal listing is
                # mid-update; membership changed so rebuild now.
                refresh(force=True)
            # Portal paste only updates this mirror folder — skip files_changed
            # (that re-refreshed this fence + public layer and flashed desktop).

        if _paste_sources_need_background(
            sources, portal, effect=effect, skip_fs=_skip_portal_fs
        ):
            _start_background_fs_paste(
                lambda: _land_paste_sources_to_folder(
                    sources, portal, effect=effect, skip_fs=_skip_portal_fs
                ),
                on_success=_finish_portal,
                on_failure=lambda _exc: None,
            )
            return True

        landed, moved_pairs, moves_ok = _land_paste_sources_to_folder(
            sources, portal, effect=effect, skip_fs=_skip_portal_fs
        )
        if not landed:
            return False
        _finish_portal((landed, moved_pairs, moves_ok))
        return True

    from src.win_shell import is_desktop_loose_item

    desk = get_desktop_path()

    def _on_desktop(path: Path) -> bool:
        return is_desktop_loose_item(path)

    def _skip_virtual_fs(src: Path) -> bool:
        return _on_desktop(src) and effect == DROPEFFECT_MOVE

    def _finish_virtual(result: tuple) -> None:
        landed, moved_pairs, moves_ok = _unpack_paste_result(result)
        if not landed:
            return
        try:
            from src.path_stat_cache import invalidate_path_stat_cache

            for path in landed:
                invalidate_path_stat_cache(path)
            for old, new in moved_pairs:
                invalidate_path_stat_cache(old)
                invalidate_path_stat_cache(new)
        except Exception:
            pass
        from src.organize_suppress import suppress_desktop_item

        for path in landed:
            try:
                suppress_desktop_item(Path(path), seconds=8.0)
            except Exception:
                pass
        if isinstance(cfg, dict) and isinstance(settings, dict):
            if moved_pairs:
                _rewrite_pins_after_fs_move(settings, moved_pairs)
            apply = getattr(fence, "_apply_virtual_drop_paths", None)
            if callable(apply):
                apply(
                    [Path(p) for p in landed],
                    source_id="",
                    insert_at=None,
                    quiet_finish=True,
                )
            else:
                assign_paths_to_virtual_fence(cfg, settings, landed)
                try:
                    save_settings(settings)
                except OSError:
                    pass
                refresh = getattr(fence, "refresh", None)
                if callable(refresh):
                    refresh(force=True)
        if effect == DROPEFFECT_MOVE and moves_ok:
            _demote_clipboard_after_cut_paste(landed)

    if _paste_sources_need_background(
        sources, Path(desk), effect=effect, skip_fs=_skip_virtual_fs
    ):
        _start_background_fs_paste(
            lambda: _land_paste_sources_to_folder(
                sources, Path(desk), effect=effect, skip_fs=_skip_virtual_fs
            ),
            on_success=_finish_virtual,
            on_failure=lambda _exc: None,
        )
        return True

    landed, moved_pairs, moves_ok = _land_paste_sources_to_folder(
        sources, Path(desk), effect=effect, skip_fs=_skip_virtual_fs
    )
    if not landed:
        return False
    _finish_virtual((landed, moved_pairs, moves_ok))
    return True


def drop_files_to_public_from_mime(
    mime,
    *,
    effect: int | None = None,
    drop_action: Qt.DropAction | None = None,
) -> bool:
    """Import an Explorer OLE drop onto the public desktop (copy/move + float)."""
    from src.shell_clipboard import DROPEFFECT_COPY, DROPEFFECT_MOVE
    from src.win_shell import DESKTIDY_VIRTUAL_MIME, collect_drop_paths

    if mime is None or mime.hasFormat(DESKTIDY_VIRTUAL_MIME):
        return False
    sources = collect_drop_paths(mime)
    if not sources:
        return False
    if effect is None:
        if drop_action == Qt.DropAction.MoveAction:
            effect = DROPEFFECT_MOVE
        elif drop_action == Qt.DropAction.CopyAction:
            effect = DROPEFFECT_COPY
        else:
            from src.settings import get_desktop_path

            pref = preferred_drop_action_for_mime(mime, target_dir=get_desktop_path())
            effect = (
                DROPEFFECT_MOVE
                if pref == Qt.DropAction.MoveAction
                else DROPEFFECT_COPY
            )
    return paste_files_to_public(None, sources=sources, effect=effect)


def paste_files_to_public(
    anchor: QWidget | None = None,
    *,
    sources: list[Path] | None = None,
    effect: int | None = None,
) -> bool:
    """Paste CF_HDROP files onto the public desktop (copy/move + float pins)."""
    from src.public_desktop import add_public_item
    from src.settings import get_desktop_path, save_settings
    from src.shell_clipboard import DROPEFFECT_MOVE, clipboard_get_files_with_effect

    if sources is None or effect is None:
        sources, effect = clipboard_get_files_with_effect()
    if not sources:
        return False
    app = QApplication.instance()
    desk_app = getattr(app, "_desktidy_app", None) if app else None
    settings = getattr(desk_app, "settings", None) if desk_app is not None else None
    if not isinstance(settings, dict):
        return False

    from src.win_shell import is_desktop_loose_item

    desk = get_desktop_path()

    def _on_desktop(path: Path) -> bool:
        return is_desktop_loose_item(path)

    # Cursor only picks the target monitor. New floats fill the left-edge
    # column-major grid (Windows-like) — never under the mouse.
    hint = QCursor.pos()
    try:
        page_id = int(settings.get("current_page", 0))
    except (TypeError, ValueError):
        page_id = 0

    def _skip_public_fs(src: Path) -> bool:
        return _on_desktop(src) and effect == DROPEFFECT_MOVE

    def _finish_public(result: tuple) -> None:
        landed, moved_pairs, moves_ok = _unpack_paste_result(result)
        if not landed:
            return
        try:
            from src.path_stat_cache import invalidate_path_stat_cache

            for path in landed:
                invalidate_path_stat_cache(path)
            for old, new in moved_pairs:
                invalidate_path_stat_cache(old)
                invalidate_path_stat_cache(new)
        except Exception:
            pass
        from src.organize_suppress import suppress_desktop_item

        for path in landed:
            try:
                suppress_desktop_item(Path(path), seconds=8.0)
            except Exception:
                pass
        for dest in landed:
            try:
                add_public_item(
                    settings,
                    dest,
                    hint.x(),
                    hint.y(),
                    page_id=page_id,
                    prefer_nearest=False,
                )
            except Exception:
                continue
        if moved_pairs:
            _rewrite_pins_after_fs_move(settings, moved_pairs)
        try:
            save_settings(settings)
        except OSError:
            pass
        if effect == DROPEFFECT_MOVE and moves_ok:
            _demote_clipboard_after_cut_paste(landed)
        refresh = getattr(desk_app, "refresh_public_desktop", None)
        if callable(refresh):
            try:
                refresh(relayout=True)
            except Exception:
                pass

    if _paste_sources_need_background(
        sources, Path(desk), effect=effect, skip_fs=_skip_public_fs
    ):
        _start_background_fs_paste(
            lambda: _land_paste_sources_to_folder(
                sources, Path(desk), effect=effect, skip_fs=_skip_public_fs
            ),
            on_success=_finish_public,
            on_failure=lambda _exc: None,
        )
        return True

    landed, moved_pairs, moves_ok = _land_paste_sources_to_folder(
        sources, Path(desk), effect=effect, skip_fs=_skip_public_fs
    )
    if not landed:
        return False
    _finish_public((landed, moved_pairs, moves_ok))
    return True


def paste_files_into_context(
    anchor: QWidget,
    *,
    sources: list[Path] | None = None,
    effect: int | None = None,
) -> bool:
    """Paste into a fence (portal/virtual) or onto the public desktop."""
    if find_fence_widget(anchor) is not None or anchor.__class__.__name__ == "FenceWidget":
        return paste_files_into_fence(anchor, sources=sources, effect=effect)
    if is_public_icon_context(anchor):
        return paste_files_to_public(anchor, sources=sources, effect=effect)
    return False


def _rename_destination(old: Path, new_name: str) -> Path | None:
    """Same-folder dest path. Stem-only edits keep the original extension."""
    new_name = (new_name or "").strip()
    if not new_name:
        return None
    if any(ch in new_name for ch in '<>:"/\\|?*'):
        return None
    if old.suffix and not Path(new_name).suffix:
        new_name = f"{new_name}{old.suffix}"
    if new_name == old.name:
        return None
    return old.with_name(new_name)


def commit_filesystem_rename(old: Path, new_name: str) -> Path | None:
    """Rename *old* to *new_name* in the same folder; update DeskTidy pins.

    Explorer-like: rewrite pins and return. Do **not** rebuild fence overlays —
    that flashes the desktop and can briefly re-show the old name from cache.
    """
    dest = _rename_destination(old, new_name)
    if dest is None:
        return None
    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app else None

    def _toast(title: str, body: str = "", *, msec: int = 3200) -> None:
        try:
            from src.ui.toast import show_toast

            show_toast(title, body, msec=msec)
        except Exception:
            pass

    if dest.exists():
        _toast("无法重命名", "该名称已存在")
        return None
    try:
        old.rename(dest)
    except OSError as exc:
        winerr = int(getattr(exc, "winerror", 0) or 0)
        if winerr in (32, 33):
            _toast("无法重命名", "文件正在使用中，请先关闭 Excel 或其它程序后重试")
        else:
            _toast("无法重命名", "请检查文件名或是否被占用")
        return None
    try:
        from src.path_stat_cache import invalidate_path_stat_cache

        invalidate_path_stat_cache(old)
        invalidate_path_stat_cache(dest)
    except Exception:
        pass
    try:
        from src.organize_suppress import suppress_desktop_item

        suppress_desktop_item(old, seconds=8.0)
        suppress_desktop_item(dest, seconds=8.0)
    except Exception:
        pass
    try:
        from src.win_shell import notify_shell_item_moved

        notify_shell_item_moved(old, dest)
    except Exception:
        pass
    settings = getattr(desk, "settings", None) if desk is not None else None
    if isinstance(settings, dict):
        try:
            from src.fence_rules import rewrite_pinned_path
            from src.settings import save_settings

            if rewrite_pinned_path(settings, old, dest):
                save_settings(settings)
        except Exception:
            pass
    return dest


def apply_item_renamed_path(item: QWidget, new_path: Path) -> None:
    """Update one overlay cell after a successful rename (no grid rebuild)."""
    old_path: Path | None = None
    try:
        fp = getattr(item, "file_path", None)
        if fp is not None:
            old_path = Path(fp)
    except Exception:
        old_path = None
    try:
        item.file_path = Path(new_path)
    except Exception:
        return
    apply = getattr(item, "apply_renamed_path", None)
    if callable(apply):
        try:
            apply(Path(new_path))
            try:
                item.update()
            except RuntimeError:
                pass
        except Exception:
            pass
    else:
        label = getattr(item, "text_label", None)
        if isinstance(label, QLabel):
            try:
                label.setText(display_name_for_path(Path(new_path)))
                try:
                    item.update()
                except RuntimeError:
                    pass
            except Exception:
                pass
    if old_path is not None:
        fence = find_fence_widget(item)
        if fence is not None:
            note = getattr(fence, "note_item_path_changed", None)
            if callable(note):
                try:
                    note(old_path, Path(new_path))
                except Exception:
                    pass


def _item_caption_name(item: QWidget) -> str:
    path = getattr(item, "file_path", None)
    if path is not None:
        try:
            return display_name_for_path(Path(path))
        except Exception:
            pass
    label = getattr(item, "text_label", None)
    if isinstance(label, QLabel):
        return str(label.text() or "")
    if isinstance(item, QLabel):
        return str(item.text() or "")
    return ""


def _caption_text_hit_rect(item: QWidget, label: QWidget) -> QRect:
    """Glyph box of the caption — not the full cell-width / 2-line shelf."""
    try:
        geo = label.geometry()
    except RuntimeError:
        return QRect()
    text = _item_caption_name(item)
    try:
        fm = label.fontMetrics()
    except RuntimeError:
        return geo
    flags = int(
        Qt.TextFlag.TextWordWrap
        | Qt.AlignmentFlag.AlignHCenter
        | Qt.AlignmentFlag.AlignTop
    )
    br = fm.boundingRect(
        QRect(0, 0, max(1, geo.width()), 4000),
        flags,
        text or " ",
    )
    # Pad the glyphs a little so the name is still easy to click, but leave
    # the icon and empty caption padding for double-click open.
    hit_w = min(geo.width(), max(int(br.width()) + 12, fm.averageCharWidth() * 4))
    hit_h = min(geo.height(), max(int(br.height()) + 2, fm.height()))
    x = geo.x() + max(0, (geo.width() - hit_w) // 2)
    return QRect(x, geo.y(), hit_w, hit_h)


def _point_in_item_label_zone(item: QWidget, local_pos: QPoint) -> bool:
    """True when *local_pos* is over the caption glyphs (Explorer rename target)."""
    icon = getattr(item, "icon_label", None)
    if icon is not None and isinstance(icon, QWidget):
        try:
            # Icon + the small gap under it is open, never rename.
            if icon.geometry().adjusted(-4, -4, 4, 6).contains(local_pos):
                return False
        except RuntimeError:
            pass
    label = getattr(item, "text_label", None)
    if label is not None and isinstance(label, QWidget):
        try:
            return _caption_text_hit_rect(item, label).contains(local_pos)
        except RuntimeError:
            return False
    # Text-only fence rows (list view) — the cell is the name.
    if item.__class__.__name__ == "FenceItemLabel":
        return item.rect().contains(local_pos)
    return False


def cancel_pending_label_rename(item: QWidget) -> None:
    """Drop a deferred caption-rename (double-click open won the race)."""
    timer = getattr(item, "_pending_rename_timer", None)
    if timer is None:
        return
    try:
        timer.stop()
        timer.deleteLater()
    except RuntimeError:
        pass
    try:
        item._pending_rename_timer = None  # type: ignore[attr-defined]
    except RuntimeError:
        pass


def mark_item_opened_by_double_click(item: QWidget) -> None:
    """Double-click opens: do not arm/fire caption rename on the trailing release."""
    cancel_pending_label_rename(item)
    try:
        item._skip_rename_on_release = True  # type: ignore[attr-defined]
    except RuntimeError:
        pass


def _arm_pending_label_rename(item: QWidget) -> bool:
    """Explorer: wait GetDoubleClickTime before rename so a double-click can open."""
    cancel_pending_label_rename(item)
    interval = 500
    app = QApplication.instance()
    if app is not None:
        try:
            interval = max(1, int(app.doubleClickInterval()))
        except Exception:
            interval = 500
    timer = QTimer(item)
    timer.setSingleShot(True)

    def _fire() -> None:
        try:
            item._pending_rename_timer = None  # type: ignore[attr-defined]
        except RuntimeError:
            return
        try:
            if not item.is_selected():
                return
        except Exception:
            return
        begin_inplace_rename(item)

    timer.timeout.connect(_fire)
    try:
        item._pending_rename_timer = timer  # type: ignore[attr-defined]
    except RuntimeError:
        return False
    timer.start(interval)
    return True


def try_inplace_rename_on_label_click(
    item: QWidget,
    *,
    was_selected: bool,
    press_pos: QPoint,
    release_pos: QPoint,
    modifiers: Qt.KeyboardModifier = Qt.KeyboardModifier.NoModifier,
) -> bool:
    """Explorer-like: slow second click on the caption of a selected item renames.

    Must not begin rename on mouseRelease of a double-click — that steals open.
    """
    if getattr(item, "_skip_rename_on_release", False):
        try:
            item._skip_rename_on_release = False  # type: ignore[attr-defined]
        except RuntimeError:
            pass
        return False
    if not was_selected or not item.is_selected():
        return False
    if modifiers & (
        Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier
    ):
        return False
    # Rename only when this icon is the sole selection (Explorer).
    if is_public_icon_context(item):
        if len(selected_public_items(item)) > 1:
            return False
    elif len(selected_fence_items(item)) > 1:
        return False
    if getattr(item, "_rename_edit", None) is not None:
        return False
    if (release_pos - press_pos).manhattanLength() > 12:
        return False
    if not _point_in_item_label_zone(item, press_pos):
        return False
    if not _point_in_item_label_zone(item, release_pos):
        return False
    return _arm_pending_label_rename(item)


def begin_inplace_rename(item: QWidget) -> bool:
    """Show an Explorer-like in-place name editor on a fence icon/label.

    The editor is a small top-level ``Tool`` window (not a child of the
    ``WS_EX_NOACTIVATE`` overlay). Child ``QLineEdit``s on NOACTIVATE hosts
    never receive real focus — ``grabKeyboard`` alone still blocks IME and
    often drops typed characters (「重命名打字打不进去」).
    """
    global _inplace_rename_active
    cancel_pending_label_rename(item)
    path = getattr(item, "file_path", None)
    if path is None:
        return False
    path = Path(path)
    # Tear down any existing editor on this item.
    old = getattr(item, "_rename_edit", None)
    if old is not None:
        try:
            old.hide()
            old.deleteLater()
        except RuntimeError:
            pass
        item._rename_edit = None  # type: ignore[attr-defined]

    target = getattr(item, "text_label", None)
    if target is None or not isinstance(target, QLabel):
        target = item if isinstance(item, QLabel) else item

    # Caption rect in global coordinates for the focusable popup.
    try:
        if target is item:
            local = item.rect().adjusted(2, 2, -2, -2)
            top_left = item.mapToGlobal(local.topLeft())
        else:
            local = target.geometry().adjusted(-1, -1, 1, 1)
            top_left = item.mapToGlobal(local.topLeft())
        edit_geo = QRect(top_left, local.size())
        if edit_geo.width() < 48:
            edit_geo.setWidth(48)
        if edit_geo.height() < 22:
            edit_geo.setHeight(22)
    except RuntimeError:
        return False

    edit = QLineEdit(None)
    edit.setObjectName("fenceInplaceRename")
    edit.setWindowFlags(
        Qt.WindowType.Tool
        | Qt.WindowType.FramelessWindowHint
        | Qt.WindowType.WindowStaysOnTopHint
    )
    # Must be allowed to activate — otherwise IME / typing still fail.
    edit.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, False)
    edit.setText(path.name)
    edit.setAlignment(Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignVCenter)
    edit.setGeometry(edit_geo)
    edit.setStyleSheet(
        "QLineEdit#fenceInplaceRename {"
        "  background: #ffffff;"
        "  color: #111827;"
        "  border: 1px solid #2563eb;"
        "  border-radius: 2px;"
        "  padding: 1px 2px;"
        "  selection-background-color: #2563eb;"
        "  selection-color: #ffffff;"
        "}"
    )
    edit.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
    edit.show()
    edit.raise_()
    item._rename_edit = edit  # type: ignore[attr-defined]

    finished = {"done": False}
    rename_ready = {"ok": False}
    keyboard_grabbed = {"on": False}
    armed_at = {"t": 0.0}
    app = QApplication.instance()
    outside_click_filter = {"obj": None}

    def _select_basename() -> None:
        # Explorer-like: select stem, keep extension unselected when present.
        stem = path.stem
        if path.suffix and stem:
            edit.setSelection(0, len(stem))
        else:
            edit.selectAll()

    def _arm_rename_input() -> None:
        """Activate the Tool popup so focus + IME work (NOACTIVATE child cannot)."""
        if finished["done"]:
            return
        try:
            import time

            edit.show()
            edit.raise_()
            edit.activateWindow()
            edit.setFocus(Qt.FocusReason.ActiveWindowFocusReason)
            # Keep grab as a belt-and-suspenders for hosts that still refuse focus.
            edit.grabKeyboard()
            keyboard_grabbed["on"] = True
            _select_basename()
            rename_ready["ok"] = True
            armed_at["t"] = time.monotonic()
        except RuntimeError:
            pass

    _select_basename()
    _inplace_rename_active = True
    _arm_rename_input()
    # Fallback: retry once on the next tick (menu dismiss / shell attach race).
    QTimer.singleShot(0, _arm_rename_input)
    QTimer.singleShot(50, _arm_rename_input)

    def _release_rename_input() -> None:
        if not keyboard_grabbed["on"]:
            return
        keyboard_grabbed["on"] = False
        try:
            edit.releaseKeyboard()
        except RuntimeError:
            pass

    def _sync_hover_after_rename() -> None:
        """Rename teardown can skip FenceWidget.leaveEvent; recompute hover now."""
        try:
            app = QApplication.instance()
            desk = getattr(app, "_desktidy_app", None) if app else None
            sync = getattr(desk, "_sync_fence_hover_from_cursor", None) if desk else None
            if callable(sync):
                sync()
                return
        except Exception:
            pass
        try:
            fence = find_fence_widget(item)
            if fence is None:
                return
            over = fence.frameGeometry().contains(QCursor.pos())
            if bool(getattr(fence, "_hovered", False)) != over:
                fence._hovered = over
                update = getattr(fence, "_update_chrome_visibility", None)
                if callable(update):
                    update()
        except Exception:
            pass

    def _teardown_rename_editor() -> None:
        """Remove the in-place editor and repaint the caption on NOACTIVATE HWNDs."""
        repaint_rect: QRect | None = None
        try:
            # Map popup rect back onto the item for a local repaint.
            top_left = item.mapFromGlobal(edit.geometry().topLeft())
            repaint_rect = QRect(top_left, edit.geometry().size())
        except RuntimeError:
            repaint_rect = None
        _release_rename_input()
        try:
            edit.blockSignals(True)
            edit.editingFinished.disconnect()
        except Exception:
            pass
        try:
            edit.returnPressed.disconnect()
        except Exception:
            pass
        try:
            edit.hide()
            edit.setParent(None)
            edit.deleteLater()
        except RuntimeError:
            pass
        item._rename_edit = None  # type: ignore[attr-defined]
        try:
            if repaint_rect is not None:
                item.update(repaint_rect.adjusted(-2, -2, 2, 2))
            else:
                item.update()
        except RuntimeError:
            pass

    def _finish(commit: bool) -> None:
        global _inplace_rename_active
        if finished["done"]:
            return
        finished["done"] = True
        rename_ready["ok"] = False
        _inplace_rename_active = False
        filt = outside_click_filter.get("obj")
        if app is not None and filt is not None:
            try:
                app.removeEventFilter(filt)
            except Exception:
                pass
        outside_click_filter["obj"] = None
        text = edit.text()
        _teardown_rename_editor()
        QTimer.singleShot(0, _sync_hover_after_rename)
        if not commit:
            return
        new_path = commit_filesystem_rename(path, text)
        if new_path is not None:
            apply_item_renamed_path(item, new_path)

    def _commit_from_editor(*, from_key: bool = False) -> None:
        if not rename_ready["ok"]:
            return
        # activateWindow/setFocus churn can emit editingFinished immediately —
        # ignore that and re-arm instead of committing an empty rename.
        # Explicit Enter/Return must always commit.
        if not from_key:
            try:
                import time

                if time.monotonic() - float(armed_at["t"]) < 0.15:
                    QTimer.singleShot(0, _arm_rename_input)
                    return
            except Exception:
                pass
        _finish(True)

    edit.editingFinished.connect(lambda: _commit_from_editor(from_key=False))
    edit.returnPressed.connect(lambda: _commit_from_editor(from_key=True))

    class _RenameOutsideClickFilter(QObject):
        def eventFilter(self, obj, event) -> bool:  # noqa: ANN001
            if finished["done"]:
                return False
            et = event.type()
            if et in (
                QEvent.Type.MouseMove,
                QEvent.Type.NonClientAreaMouseMove,
            ):
                _sync_hover_after_rename()
                return False
            if et not in (
                QEvent.Type.MouseButtonPress,
                QEvent.Type.NonClientAreaMouseButtonPress,
            ):
                return False
            try:
                pos = QCursor.pos()
                hit = app.widgetAt(pos) if app is not None else None
                cur = hit
                while cur is not None:
                    # Only ignore presses on the editor itself — clicks on the
                    # icon / cell outside the edit must commit (Explorer-like).
                    if cur is edit:
                        return False
                    cur = cur.parentWidget()
            except Exception:
                return False
            # Outside click: always commit (user deliberately left the field).
            if rename_ready["ok"]:
                _finish(True)
            return False

    if app is not None:
        try:
            filt = _RenameOutsideClickFilter(app)
            outside_click_filter["obj"] = filt
            app.installEventFilter(filt)
        except Exception:
            outside_click_filter["obj"] = None

    def _on_key(event) -> None:
        if event.key() == Qt.Key.Key_Escape:
            _finish(False)
            event.accept()
            return
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            _commit_from_editor(from_key=True)
            event.accept()
            return
        QLineEdit.keyPressEvent(edit, event)

    edit.keyPressEvent = _on_key  # type: ignore[method-assign]
    return True


def looks_like_shell_new_item(path: Path | str) -> bool:
    """True for Explorer「新建文件夹 / New folder / 新建*」default names."""
    try:
        name = Path(path).name.strip()
    except (TypeError, ValueError, OSError):
        return False
    if not name:
        return False
    lower = name.casefold()
    if lower in {"新建文件夹", "new folder"}:
        return True
    if name.startswith("新建"):
        return True
    if lower.startswith("new folder") or lower.startswith("new text document"):
        return True
    if lower.startswith("new microsoft"):
        return True
    return False


def maybe_begin_rename_for_shell_new_item(item: QWidget, *, max_age_s: float = 8.0) -> bool:
    """Auto-start in-place rename for a freshly created shell「新建…」item.

    When shell icons are hidden, Explorer still creates the folder and starts
    its own (invisible) rename; our float appears with the default name and
    keystrokes never reach it. Starting our Tool-popup rename restores typing.
    """
    path = getattr(item, "file_path", None)
    if path is None:
        return False
    path = Path(path)
    if not looks_like_shell_new_item(path):
        return False
    try:
        import time

        age = time.time() - float(path.stat().st_mtime)
    except OSError:
        return False
    if age < 0 or age > float(max_age_s):
        return False
    if getattr(item, "_rename_edit", None) is not None:
        return False
    if _inplace_rename_active:
        return False
    return begin_inplace_rename(item)


def find_item_widget_for_path(path: Path) -> QWidget | None:
    """Find a visible fence icon/label (or public float) for *path*."""
    key = str(Path(path)).casefold()
    for top in QApplication.topLevelWidgets():
        name = top.__class__.__name__
        if name == "FenceWidget":
            fn = getattr(top, "_icon_item_widgets", None)
            if not callable(fn):
                continue
            try:
                items = list(fn())
            except Exception:
                continue
            for item in items:
                fp = getattr(item, "file_path", None)
                if fp is not None and str(Path(fp)).casefold() == key:
                    return item
        if name == "PublicIconHost":
            for child in top.findChildren(QWidget):
                if child.__class__.__name__ != "PublicIconWidget":
                    continue
                fp = getattr(child, "file_path", None)
                if fp is not None and str(Path(fp)).casefold() == key:
                    return child
        if name == "PublicIconWidget":
            fp = getattr(top, "file_path", None)
            if fp is not None and str(Path(fp)).casefold() == key:
                return top
    return None


def _is_desktop_icon_widget(widget: QWidget | None) -> bool:
    if widget is None:
        return False
    return widget.__class__.__name__ in (
        "FenceIconItem",
        "FenceItemLabel",
        "FenceWidget",
        "PublicIconWidget",
        "PublicIconHost",
    )


def _fence_under_cursor(pos: QPoint) -> QWidget | None:
    """Smallest visible FenceWidget whose frame contains *pos*."""
    hits: list[tuple[int, QWidget]] = []
    for top in QApplication.topLevelWidgets():
        if top.__class__.__name__ != "FenceWidget":
            continue
        try:
            if not top.isVisible():
                continue
            geo = top.frameGeometry()
            if not geo.contains(pos):
                continue
            hits.append((int(geo.width() * geo.height()), top))
        except RuntimeError:
            continue
    if not hits:
        return None
    hits.sort(key=lambda t: t[0])
    return hits[0][1]


def _public_under_cursor(pos: QPoint) -> QWidget | None:
    """Public icon under *pos*, or PublicIconHost only when inside its click mask."""
    for top in QApplication.topLevelWidgets():
        if top.__class__.__name__ != "PublicIconHost":
            continue
        try:
            if not top.isVisible():
                continue
            if not top.frameGeometry().contains(pos):
                continue
            local = top.mapFromGlobal(pos)
            hit = getattr(top, "hit_test_region", None)
            if callable(hit):
                region = hit()
                if region is not None and not region.isEmpty() and not region.contains(local):
                    continue
            child = top.childAt(local)
            cur = child
            while cur is not None and cur is not top:
                if cur.__class__.__name__ == "PublicIconWidget":
                    return cur
                cur = cur.parentWidget()
            # Empty gap inside compact-cluster mask: paste / select-all target.
            return top
        except RuntimeError:
            continue
    return None


def _selected_in_fence(fence: QWidget) -> list[QWidget]:
    fn = getattr(fence, "_icon_item_widgets", None)
    if not callable(fn):
        return []
    try:
        items = list(fn())
    except Exception:
        return []
    return [
        w
        for w in items
        if callable(getattr(w, "is_selected", None)) and w.is_selected()
    ]


def _any_selected_desktop_anchor() -> QWidget | None:
    for top in QApplication.topLevelWidgets():
        name = top.__class__.__name__
        if name == "FenceWidget":
            selected = _selected_in_fence(top)
            if selected:
                return selected[0]
        if name == "PublicIconHost":
            selected = selected_public_items(top)
            if selected:
                return selected[0]
    return None


def _fence_item_at(fence: QWidget, global_pos: QPoint) -> QWidget | None:
    """Fence icon/label under *global_pos*, if any."""
    try:
        local = fence.mapFromGlobal(global_pos)
        child = fence.childAt(local)
    except RuntimeError:
        return None
    cur = child
    while cur is not None and cur is not fence:
        name = cur.__class__.__name__
        if name in ("FenceIconItem", "FenceItemLabel") and getattr(
            cur, "file_path", None
        ) is not None:
            return cur
        cur = cur.parentWidget()
    return None


def _active_public_icon_host() -> QWidget | None:
    """Visible PublicIconHost top-level, if any (empty-desktop paste surface)."""
    for top in QApplication.topLevelWidgets():
        if top.__class__.__name__ != "PublicIconHost":
            continue
        try:
            if top.isVisible():
                return top
        except RuntimeError:
            continue
    return None


def resolve_desktop_key_anchor(*, chord: str = "") -> QWidget | None:
    """Resolve copy/paste target without stealing keys from other apps.

    Rules (Explorer-like on NOACTIVATE overlays):
    - Prefer the smallest fence under the cursor over the fullscreen public host.
    - Public host only counts inside its click mask / icon footprints.
    - Copy/cut fall back to the icon under the cursor when click-selection was lost.
    - Paste can target the fence that owns the current selection even when the
      cursor has left the frame (NOACTIVATE often leaves FG on another app).
    - Empty wallpaper paste (shell icons hidden): public host — DeskTidy owns the
      desktop UI, so we must not rely on invisible Explorer DefView paste.
    """
    needs_paths = chord in ("copy", "cut", "delete", "rename")
    pos = QCursor.pos()
    fence = _fence_under_cursor(pos)
    public = _public_under_cursor(pos) if fence is None else None

    if fence is not None:
        selected = _selected_in_fence(fence)
        if needs_paths:
            if selected:
                return selected[0]
            under = _fence_item_at(fence, pos)
            if under is not None:
                return under
            # Cursor on fence chrome with no icon — try any live selection below.
        else:
            # paste / select_all on this fence
            return selected[0] if selected else fence

    if public is not None:
        selected = selected_public_items(public)
        if needs_paths:
            if selected:
                return selected[0]
            if getattr(public, "file_path", None) is not None:
                return public
            # Empty PublicIconHost plate (full-desktop 框选 surface when shell
            # icons are hidden) must not abort copy/cut — fall through so a
            # live fence/public selection still resolves. Returning None here
            # caused first Ctrl+C →「无法复制」with the cursor on wallpaper.
        else:
            # paste / select_all on the public host or icon
            return selected[0] if selected else public

    # Cursor not over a DeskTidy hit region (or only over an empty host plate).
    try:
        from src.win_shell import is_desktop_foreground, is_explorer_desktop_foreground

        fg = bool(is_desktop_foreground())
        explorer_desk = bool(is_explorer_desktop_foreground())
    except Exception:
        fg = False
        explorer_desk = False

    # Full-desktop PublicIconHost plate (shell icons hidden) owns the wallpaper
    # surface even when Explorer is not "foreground" (NOACTIVATE overlays).
    owned_empty_plate = (
        public is not None
        and getattr(public, "file_path", None) is None
        and public.__class__.__name__ == "PublicIconHost"
    )

    if needs_paths:
        if not fg and not _desktop_selection_live and not owned_empty_plate:
            return None
        return _any_selected_desktop_anchor()

    if chord == "paste":
        # Selection owner first (cursor on wallpaper after selecting a fence item).
        if fg or _desktop_selection_live:
            anchor = _any_selected_desktop_anchor()
            if anchor is not None:
                owned = find_fence_widget(anchor)
                return owned if owned is not None else anchor
        # Empty desktop: with shell icons hidden, Explorer paste is invisible —
        # land on the public float host (created while session overlays are up).
        if explorer_desk or fg:
            return _active_public_icon_host()
        return None

    # select_all without a hit target — do not steal.
    return None


def dispatch_desktop_icon_chord(chord: str, anchor: QWidget | None = None) -> bool:
    """Run a copy/cut/paste/… chord against *anchor* (or resolved context)."""
    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app is not None else None
    begin_popup = getattr(desk, "_begin_desktop_popup", None) if desk is not None else None
    end_popup_later = (
        getattr(desk, "_end_desktop_popup_later", None) if desk is not None else None
    )
    freeze_clipboard = chord in ("copy", "cut", "paste")
    if freeze_clipboard and callable(begin_popup):
        begin_popup()

    def _toast(title: str, body: str = "", *, msec: int = 1600) -> None:
        try:
            from src.ui.toast import show_toast

            show_toast(title, body, msec=msec)
        except Exception:
            pass

    try:
        target = anchor if anchor is not None else resolve_desktop_key_anchor(chord=chord)

        # Paste may land on the public desktop even when no host widget is resolved yet
        # (session mid-refresh). Other chords still need a concrete anchor.
        if target is None and chord != "paste":
            if chord == "copy":
                _toast("无法复制", "请先选中要复制的图标", msec=1800)
            elif chord == "cut":
                _toast("无法剪切", "请先选中要剪切的图标", msec=1800)
            return False
        if target is None and chord == "paste":
            try:
                from src.win_shell import is_explorer_desktop_foreground

                if not is_explorer_desktop_foreground():
                    return False
            except Exception:
                return False
            from src.shell_clipboard import clipboard_get_files

            sources = clipboard_get_files()
            if not sources:
                _toast("无法粘贴", "剪贴板里没有文件", msec=2200)
                return False
            ok = paste_files_to_public(None)
            if ok:
                _toast("已粘贴", "文件已放入目标位置")
            else:
                _toast("粘贴失败", "未能写入目标位置", msec=2200)
            return ok

        from src.shell_clipboard import clipboard_set_files

        def _paths_for_copy_cut() -> list[Path]:
            paths = selected_paths_from_anchor(target)
            if paths:
                return paths
            path = getattr(target, "file_path", None)
            if path is None:
                return []
            # Cursor landed on an icon that was not yet selected — promote it so
            # subsequent paste / LL chords still have a live DeskTidy target.
            try:
                if is_public_icon_context(target):
                    select_public_item(target, exclusive=True)
                else:
                    select_fence_item(target, exclusive=True)
                    fence = find_fence_widget(target)
                    if fence is not None:
                        fence._selection_anchor = target  # type: ignore[attr-defined]
            except Exception:
                mark_desktop_selection_live(True)
            return [Path(path)]

        if chord == "copy":
            paths = _paths_for_copy_cut()
            if not paths:
                _toast("无法复制", "请先选中要复制的图标", msec=1800)
                return False
            try:
                ok = bool(clipboard_set_files(paths, cut=False))
            except Exception:
                ok = False
            if ok:
                set_clipboard_cut_paths(None)
                mark_desktop_selection_live(True)
                _toast("已复制", f"{len(paths)} 个项目")
            else:
                missing = [p for p in paths if not Path(p).exists()]
                if missing:
                    _toast("复制失败", "选中的文件已不存在", msec=2200)
                else:
                    _toast("复制失败", "剪贴板被占用，请再试一次", msec=2200)
            return ok
        if chord == "cut":
            paths = _paths_for_copy_cut()
            if not paths:
                _toast("无法剪切", "请先选中要剪切的图标", msec=1800)
                return False
            try:
                ok = bool(clipboard_set_files(paths, cut=True))
            except Exception:
                ok = False
            if ok:
                set_clipboard_cut_paths(paths)
                mark_desktop_selection_live(True)
                _toast("已剪切", f"{len(paths)} 个项目")
            else:
                _toast("剪切失败", "剪贴板被占用或文件不存在", msec=2200)
            return ok
        if chord == "paste":
            from src.shell_clipboard import clipboard_get_files_with_effect

            sources, effect = clipboard_get_files_with_effect()
            if not sources:
                _toast("无法粘贴", "剪贴板里没有文件", msec=2200)
                return False
            ok = paste_files_into_context(target, sources=sources, effect=effect)
            if ok:
                _toast("已粘贴", "文件已放入目标位置")
            else:
                _toast("粘贴失败", "未能写入目标位置", msec=2200)
            return ok
        if chord == "select_all":
            if is_public_icon_context(target):
                select_all_public_items(target)
            else:
                select_all_fence_items(target)
            return True
        if chord == "delete":
            paths = selected_paths_from_anchor(target)
            if not paths:
                return False
            delete_selected_fence_items(target)
            return True
        if chord == "rename":
            item = target
            if not getattr(item, "file_path", None):
                selected = (
                    selected_public_items(item)
                    if is_public_icon_context(item)
                    else selected_fence_items(item)
                )
                item = selected[0] if selected else item
            if not getattr(item, "file_path", None):
                return False
            return begin_inplace_rename(item)
        return False
    finally:
        if freeze_clipboard and callable(end_popup_later):
            end_popup_later(180)


class _DesktopIconKeyBridge(QObject):
    """Marshal LL-hook chords onto the Qt GUI thread."""

    # chord, optional sip-wrapped QWidget id via int(winId) is fragile —
    # pass chord only and re-resolve on GUI thread with same rules (cheap).
    chord = pyqtSignal(str)


_desktop_key_bridge: _DesktopIconKeyBridge | None = None
_desktop_key_hook_registered = False
_last_desktop_chord_ms = 0.0
_last_desktop_chord = ""
_inplace_rename_active = False
_hook_ctx_timer: QTimer | None = None
_HOOK_CTX_TTL_S = 0.45
_hook_desktop_ctx: dict[str, object] = {
    "over_icon": False,
    "over_paste_surface": False,
    "desktop_fg": False,
    "explorer_desktop_fg": False,
    "updated_at": 0.0,
}


def inplace_rename_active() -> bool:
    """True while a fence/public in-place rename editor owns keyboard input."""
    return bool(_inplace_rename_active)

# Fast reject before GetAsyncKeyState / widget scans.
_LL_VK_CTRL_CHORDS = frozenset({0x41, 0x43, 0x56, 0x58})  # A C V X
_LL_VK_BARE_CHORDS = frozenset({0x2E, 0x71})  # Delete, F2


def mark_desktop_selection_live(active: bool | None = None) -> None:
    """Refresh the hook-thread selection flag (GUI thread only)."""
    global _desktop_selection_live
    if active is not None:
        _desktop_selection_live = bool(active)
    else:
        _desktop_selection_live = _any_selected_desktop_anchor() is not None
    _refresh_hook_desktop_ctx()


def _refresh_hook_desktop_ctx() -> None:
    """GUI thread: cache pointer/FG state for the LL keyboard hook (no widgetAt in hook)."""
    global _hook_desktop_ctx
    import time

    try:
        from src.win_shell import is_desktop_foreground, is_explorer_desktop_foreground

        _hook_desktop_ctx = {
            "over_icon": _cursor_over_icon_item(),
            "over_paste_surface": _cursor_over_desktop_paste_surface(),
            "desktop_fg": bool(is_desktop_foreground()),
            "explorer_desktop_fg": bool(is_explorer_desktop_foreground()),
            "updated_at": time.monotonic(),
        }
    except Exception:
        pass


def _hook_ctx_fresh() -> bool:
    import time

    try:
        updated = float(_hook_desktop_ctx.get("updated_at", 0.0))
    except (TypeError, ValueError):
        return False
    return (time.monotonic() - updated) < _HOOK_CTX_TTL_S


def _ensure_hook_ctx_timer() -> None:
    global _hook_ctx_timer
    app = QApplication.instance()
    if app is None:
        return
    if _hook_ctx_timer is None:
        _hook_ctx_timer = QTimer(app)
        _hook_ctx_timer.setInterval(120)
        _hook_ctx_timer.timeout.connect(_refresh_hook_desktop_ctx)
    if not _hook_ctx_timer.isActive():
        _hook_ctx_timer.start()
    _refresh_hook_desktop_ctx()


def _cursor_over_our_process_window() -> bool:
    """Hook-thread safe: cursor is over a HWND owned by this process."""
    try:
        import ctypes
        from ctypes import wintypes

        user32 = ctypes.windll.user32
        kernel32 = ctypes.windll.kernel32
        pt = wintypes.POINT()
        if not user32.GetCursorPos(ctypes.byref(pt)):
            return False
        hwnd = int(user32.WindowFromPoint(pt) or 0)
        if not hwnd:
            return False
        our_pid = int(kernel32.GetCurrentProcessId())
        pid = wintypes.DWORD()
        while hwnd:
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if int(pid.value) == our_pid:
                return True
            parent = int(user32.GetParent(hwnd) or 0)
            if not parent or parent == hwnd:
                break
            hwnd = parent
    except Exception:
        return False
    return False


def _cursor_over_icon_item() -> bool:
    """True when the pointer is on a selectable fence/public icon (not page chrome)."""
    try:
        from PyQt6.QtGui import QCursor
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance()
        if app is None:
            return False
        w = app.widgetAt(QCursor.pos())
        while w is not None:
            name = w.__class__.__name__
            if name in ("FenceIconItem", "FenceItemLabel", "PublicIconWidget"):
                return True
            if name in (
                "PageIndicatorWidget",
                "_PeekChip",
                "_PeekRow",
                "DockWidget",
            ):
                return False
            w = w.parentWidget()
    except Exception:
        return False
    return False


def _cursor_over_desktop_paste_surface() -> bool:
    """True on a real paste target: fence frame or public host/icon — not page chrome.

    Bare ``_cursor_over_our_process_window`` used to claim Ctrl+V on the dock /
    peek chips, eat the key, then resolve to None →「没反应」.
    """
    try:
        from PyQt6.QtGui import QCursor
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance()
        if app is None:
            return False
        w = app.widgetAt(QCursor.pos())
        while w is not None:
            name = w.__class__.__name__
            if name in (
                "FenceIconItem",
                "FenceItemLabel",
                "FenceWidget",
                "PublicIconWidget",
                "PublicIconHost",
            ):
                return True
            if name in (
                "PageIndicatorWidget",
                "_PeekChip",
                "_PeekRow",
                "DockWidget",
                "MainWindow",
                "NotepadWindow",
            ):
                return False
            w = w.parentWidget()
    except Exception:
        return False
    return False


def _ll_should_claim_chord_hook(chord: str) -> bool:
    """Hook-thread gate: Win32 HWND walk + cached GUI context — no widgetAt / FG probes."""
    if _inplace_rename_active:
        return False
    ours = _cursor_over_our_process_window()
    if chord == "select_all":
        return ours
    fresh = _hook_ctx_fresh()
    ctx = _hook_desktop_ctx
    if chord == "paste":
        if not fresh:
            return False
        if not bool(ctx.get("desktop_fg")):
            return False
        if bool(ctx.get("over_paste_surface")):
            return True
        if _desktop_selection_live:
            return True
        return bool(ctx.get("explorer_desktop_fg"))
    if _desktop_selection_live:
        if bool(ctx.get("desktop_fg")):
            return True
        if fresh and bool(ctx.get("over_icon")):
            return True
        return False
    if fresh:
        return bool(ctx.get("over_icon"))
    return False


def _ll_should_claim_chord(chord: str) -> bool:
    """Full GUI-thread claim check (widgetAt + FG)."""
    if _inplace_rename_active:
        return False
    ours = _cursor_over_our_process_window()
    if chord == "select_all":
        return ours
    if chord == "paste":
        # Never steal Ctrl+V from WeChat / Office / browsers. Cursor often rests
        # on a fence while chat owns FG — claiming then pastes onto the desktop
        # and the chat invents a stub a.txt from a half-delivered key.
        try:
            from src.win_shell import is_desktop_foreground

            if not is_desktop_foreground():
                return False
        except Exception:
            return False
        # Desktop FG: claim on a real paste surface, or Explorer wallpaper when
        # shell icons are hidden (DeskTidy owns empty-desktop paste).
        if _cursor_over_desktop_paste_surface():
            return True
        if _desktop_selection_live:
            return True
        try:
            from src.win_shell import is_explorer_desktop_foreground

            return bool(is_explorer_desktop_foreground())
        except Exception:
            return False
    # copy/cut/delete/rename:
    # - Live selection + desktop FG → claim
    # - Live selection + foreign FG → only when pointer is on a real icon
    #   (selection_live + empty fence chrome used to steal WeChat Ctrl+C)
    # - No selection → only claim when the pointer is on a real icon
    if _desktop_selection_live:
        try:
            from src.win_shell import is_desktop_foreground

            if is_desktop_foreground():
                return True
        except Exception:
            pass
        return _cursor_over_icon_item()
    return _cursor_over_icon_item()


def _on_ll_desktop_icon_key(vk: int, flags: int) -> bool:
    """Eat Ctrl+C/V/X/A, Del, F2 only for a real DeskTidy desktop context.

    Heavy resolve runs on the GUI thread — the hook only does VK / HWND checks.
    """
    vk = int(vk)
    if vk not in _LL_VK_CTRL_CHORDS and vk not in _LL_VK_BARE_CHORDS:
        return False

    from src.desktop_ll_keyboard import (
        VK_A,
        VK_C,
        VK_DELETE,
        VK_F2,
        VK_V,
        VK_X,
        alt_physically_down,
        ctrl_physically_down,
    )

    if alt_physically_down():
        return False
    ctrl = ctrl_physically_down()
    chord = ""
    if ctrl and vk == VK_C:
        chord = "copy"
    elif ctrl and vk == VK_X:
        chord = "cut"
    elif ctrl and vk == VK_V:
        chord = "paste"
    elif ctrl and vk == VK_A:
        chord = "select_all"
    elif (not ctrl) and vk == VK_DELETE:
        chord = "delete"
    elif (not ctrl) and vk == VK_F2:
        chord = "rename"
    else:
        return False

    if not _ll_should_claim_chord_hook(chord):
        return False

    bridge = _desktop_key_bridge
    if bridge is None:
        return False
    bridge.chord.emit(chord)
    # Claim already verified a real DeskTidy target — always swallow so Explorer
    # does not also handle the chord (which looked like「没反应」on our side).
    return True


def _apply_desktop_icon_chord(chord: str) -> None:
    """Apply a chord on the GUI thread.

    Debounce only *successful* dispatches, and only briefly (~60ms) to collapse
    true double-fires. A 280ms lock after success made the next short Ctrl+C
    look dead (hold-for-repeat after ~300ms was the only way through).
    Failed first attempts still do not latch — short taps may retry immediately.

    The LL hook already swallowed copy/cut/delete/rename/select_all — do not
    re-run the stricter GUI claim gate here (hook ctx vs widgetAt mismatch ate
    the chord with no copy).
    """
    global _last_desktop_chord_ms, _last_desktop_chord
    import time

    _refresh_hook_desktop_ctx()
    now = time.monotonic() * 1000.0
    if chord == _last_desktop_chord and (now - _last_desktop_chord_ms) < 60.0:
        return
    if _inplace_rename_active:
        return
    if chord == "paste" and not _ll_should_claim_chord(chord):
        return
    ok = bool(dispatch_desktop_icon_chord(chord))
    if ok:
        _last_desktop_chord = chord
        _last_desktop_chord_ms = now


def ensure_desktop_icon_key_hook() -> None:
    """Install the shared LL keyboard hook for overlay copy/paste (idempotent)."""
    global _desktop_key_bridge, _desktop_key_hook_registered
    app = QApplication.instance()
    if app is None:
        return
    if _desktop_key_bridge is None:
        _desktop_key_bridge = _DesktopIconKeyBridge(app)
        _desktop_key_bridge.chord.connect(
            _apply_desktop_icon_chord, Qt.ConnectionType.QueuedConnection
        )
    from src.desktop_ll_keyboard import (
        ll_keyboard_hook_active,
        register_ll_keyboard_handler,
    )

    if _desktop_key_hook_registered and ll_keyboard_hook_active():
        ensure_cut_ghost_clipboard_sync()
        return
    # Hook can die after Explorer / session churn — allow re-install.
    register_ll_keyboard_handler(
        _on_ll_desktop_icon_key, force=bool(_desktop_key_hook_registered)
    )
    _desktop_key_hook_registered = bool(ll_keyboard_hook_active())
    _ensure_hook_ctx_timer()
    ensure_cut_ghost_clipboard_sync()


def handle_fence_item_key(item: QWidget, event) -> bool:
    """Explorer-like accelerators on fence/public icons. Return True if handled."""
    ensure_desktop_icon_key_hook()
    key = event.key()
    mods = event.modifiers()
    ctrl = bool(mods & Qt.KeyboardModifier.ControlModifier)

    if key == Qt.Key.Key_F2 and not ctrl:
        return dispatch_desktop_icon_chord("rename", item)
    if key == Qt.Key.Key_Delete and not ctrl:
        return dispatch_desktop_icon_chord("delete", item)
    if ctrl and key == Qt.Key.Key_A:
        return dispatch_desktop_icon_chord("select_all", item)
    if ctrl and key == Qt.Key.Key_C:
        return dispatch_desktop_icon_chord("copy", item)
    if ctrl and key == Qt.Key.Key_X:
        return dispatch_desktop_icon_chord("cut", item)
    if ctrl and key == Qt.Key.Key_V:
        return dispatch_desktop_icon_chord("paste", item)
    if event.matches(QKeySequence.StandardKey.Copy):
        return dispatch_desktop_icon_chord("copy", item)
    if event.matches(QKeySequence.StandardKey.Cut):
        return dispatch_desktop_icon_chord("cut", item)
    if event.matches(QKeySequence.StandardKey.Paste):
        return dispatch_desktop_icon_chord("paste", item)
    if event.matches(QKeySequence.StandardKey.SelectAll):
        return dispatch_desktop_icon_chord("select_all", item)
    return False


def paths_for_icon_shell_action(
    anchor: QWidget | None, file_path: Path
) -> list[Path]:
    """Paths for RMB DeskTidy commands: whole multi-select when *anchor* is selected."""
    if anchor is not None:
        try:
            if is_public_icon_context(anchor):
                paths = paths_for_public_drag(anchor)
            else:
                paths = paths_for_fence_drag(anchor)
            if paths:
                return [Path(p) for p in paths]
        except Exception:
            pass
    return [Path(file_path)] if file_path is not None else []


def build_file_icon_shell_extras(
    *,
    file_path: Path,
    settings: dict | None,
    desk,
    virtual_mode: bool = False,
    source_fence_id: str = "",
    virtual_unpin_callback=None,
    action_paths: list[Path] | None = None,
    anchor: QWidget | None = None,
) -> list:
    """DeskTidy commands prepended above Explorer's file context menu.

    When the icon belongs to a multi-selection, 「移动到分区 / 分页 / 移出分区」
    apply to the whole selection (Explorer-like), not only the clicked path.

    Public floats with「启用公共区域」omit 「移动到分区 / 移动到分页」so
    membership stays public unless the user drags into a fence.
    """
    from src.organize_whitelist import (
        add_organize_whitelist_entry,
        is_organize_whitelisted,
        remove_organize_whitelist_entry,
    )
    from src.public_desktop import (
        desktop_pages_for_move_menu,
        fences_for_move_menu,
        is_public_desktop_enabled,
        is_system_namespace_path,
    )
    from src.settings import save_settings
    from src.shell_file_menu import ShellMenuCommand

    paths = [Path(p) for p in (action_paths or []) if p]
    if not paths:
        paths = paths_for_icon_shell_action(anchor, Path(file_path))
    if not paths:
        paths = [Path(file_path)]

    extras: list[ShellMenuCommand] = []
    if virtual_mode and callable(virtual_unpin_callback):
        def _unpin_selection() -> None:
            items = [Path(p) for p in paths]
            fence = find_fence_widget(anchor) if anchor is not None else None
            batch_unpin = (
                getattr(fence, "_unpin_virtual_paths", None) if fence else None
            )
            if callable(batch_unpin):
                batch_unpin(items, place_on_public=True)
                return
            for p in items:
                virtual_unpin_callback(p)

        extras.append(ShellMenuCommand("移出分区", _unpin_selection))
    try:
        from src.notepad import document_open_shell_commands

        extras.extend(document_open_shell_commands(file_path, settings, desk))
    except Exception:
        pass
    if not isinstance(settings, dict) or is_system_namespace_path(file_path):
        return extras

    if is_organize_whitelisted(file_path, settings):

        def _remove_wl() -> None:
            remove_organize_whitelist_entry(settings, file_path.name)
            try:
                save_settings(settings)
            except OSError:
                pass

        extras.append(ShellMenuCommand("移出整理白名单", _remove_wl))
    else:

        def _add_wl() -> None:
            add_organize_whitelist_entry(settings, file_path.name)
            try:
                save_settings(settings)
            except OSError:
                pass

        extras.append(ShellMenuCommand("加入整理白名单", _add_wl))

    try:
        current = int(settings.get("current_page", 0))
    except (TypeError, ValueError):
        current = 0

    # Public floats stay shared when「启用公共区域」is on — RMB must not pin
    # them to a fence or demote them to a page-local float. Drag into a fence
    # still re-pins. Fence icons keep cross-fence / move-to-page.
    allow_membership_move = bool(virtual_mode) or not is_public_desktop_enabled(
        settings
    )
    if allow_membership_move:
        for fid, label in fences_for_move_menu(
            settings, page_id=current, source_fence_id=source_fence_id or None
        ):
            extras.append(
                ShellMenuCommand(
                    f"移动到分区：{label}",
                    lambda f=fid, items=list(paths): (
                        desk.move_desktop_items_to_fence(items, f)
                        if desk is not None
                        else None
                    ),
                )
            )

        for pid, label in desktop_pages_for_move_menu(settings):
            extras.append(
                ShellMenuCommand(
                    f"移动到分页：{label}",
                    lambda p=pid, items=list(paths): (
                        desk.move_desktop_items_to_page(items, p)
                        if desk is not None
                        else None
                    ),
                    enabled=(pid != current),
                )
            )
    return extras


class FenceIconItem(QWidget):
    activated = pyqtSignal(Path)
    refresh_needed = pyqtSignal()
    virtual_unpinned = pyqtSignal(Path)

    DEFAULT_ICON_SIZE = 40

    def __init__(
        self,
        file_path: Path,
        show_label: bool = True,
        icon_size: int = DEFAULT_ICON_SIZE,
        label_max_width: int = 80,
        parent: QWidget | None = None,
        *,
        virtual_mode: bool = False,
        fence_id: str = "",
        icon_load_delay_ms: int = 0,
    ):
        super().__init__(parent)
        self.file_path = file_path
        self._icon_size = icon_size
        self._show_label = show_label
        self._label_max_width = label_max_width
        self._virtual_mode = virtual_mode
        self._fence_id = fence_id
        self._icon_load_delay_ms = max(0, int(icon_load_delay_ms))
        self._press_pos: QPoint | None = None
        self._press_was_selected = False
        self._selected = False
        self._source_pixmap: QPixmap | None = None
        self._source_fetch_size = self.DEFAULT_ICON_SIZE
        self.setToolTip(str(file_path))
        self.setCursor(Qt.CursorShape.ArrowCursor)
        # NoFocus: overlays are WS_EX_NOACTIVATE; ClickFocus/setFocus flashes the
        # translucent HWND. Selection is paint-state; chords use the LL keyboard hook.
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.customContextMenuRequested.connect(self._show_menu)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, False)
        self._rename_edit = None
        ensure_desktop_icon_key_hook()

        layout = QVBoxLayout(self)
        layout.setContentsMargins(5, 3, 5, 4)
        layout.setSpacing(4)
        layout.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter)

        self.icon_label = QLabel()
        self.icon_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        # Let parent receive press/move so dragging actually starts.
        self.icon_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        layout.addWidget(self.icon_label, 0, Qt.AlignmentFlag.AlignHCenter)

        if show_label:
            self.text_label = QLabel("")
            self.text_label.setObjectName("fenceItem")
            self.text_label.setAlignment(Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop)
            # Line breaks come from elide_desktop_caption_text; do not wrap again.
            self.text_label.setWordWrap(False)
            self.text_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
            layout.addWidget(self.text_label, 0, Qt.AlignmentFlag.AlignHCenter)
        else:
            self.text_label = None

        if self._icon_load_delay_ms > 0:
            cell_w = max(icon_size + 20, label_max_width + 10)
            self.icon_label.setFixedSize(icon_size, icon_size)
            self.setFixedWidth(cell_w)
            self.setMinimumWidth(cell_w)
            self.setMaximumWidth(cell_w)
            # Caption must be 2-line ellipsis immediately — do not wait for icon
            # delay / first click (raw full name used to wrap as 孤字 / 3 lines).
            if self.text_label is not None:
                self._refresh_caption_for_selection()

            def _deferred_apply(
                size=icon_size, width=label_max_width, item=self
            ) -> None:
                try:
                    # Boot heal / page switch can delete the item before delay fires.
                    _ = item.icon_label
                except RuntimeError:
                    return
                try:
                    item._apply_icon_size(size, width)
                except RuntimeError:
                    return

            QTimer.singleShot(self._icon_load_delay_ms, _deferred_apply)
        else:
            self._apply_icon_size(icon_size, label_max_width)

        apply_cut_ghost_visual(self)

    def paintEvent(self, event) -> None:  # noqa: N802
        # Invisible hit ink under glyph+caption so layered fence HWNDs still
        # receive the first click (alpha=0 holes used to pass through to DefView).
        painter = QPainter(self)
        try:
            painter.fillRect(self.rect(), QColor(0, 0, 0, 1))
            if self._selected:
                paint_fence_item_selection(self, painter)
        finally:
            if painter.isActive():
                painter.end()
        super().paintEvent(event)

    def resizeEvent(self, event) -> None:  # noqa: N802
        super().resizeEvent(event)

    @staticmethod
    def _short_name(name: str, icon_size: int) -> str:
        _ = icon_size
        return name

    def _caption_display_text(self) -> str:
        """Always at most 2 lines with ellipsis (full name stays on the tooltip)."""
        from src.desktop_caption import elide_desktop_caption_text

        name = display_name_for_path(self.file_path)
        # Elide for the real shelf width only — a wider measure then a narrower
        # paint width re-broke the second line into 「同」 + clipped overflow.
        width = max(16, int(getattr(self, "_label_max_width", 80) or 80))
        try:
            if self.text_label is not None and self.text_label.width() > 16:
                width = min(width, int(self.text_label.width()))
        except RuntimeError:
            pass
        font = self.text_label.font() if self.text_label is not None else None
        return elide_desktop_caption_text(name, width, font, max_lines=2) or name

    def _refresh_caption_for_selection(self) -> None:
        if self.text_label is None:
            return
        cell_w = max(
            int(getattr(self, "_icon_size", 48) or 48) + 20,
            int(getattr(self, "_label_max_width", 80) or 80) + 10,
        )
        inner_w = max(16, cell_w - 10)
        name = display_name_for_path(self.file_path)
        self.text_label.setToolTip(name)
        # Selected and idle share the same 2-line ellipsis shelf.
        fit_desktop_caption_label(self.text_label, inner_w, max_lines=2)
        self.text_label.setText(self._caption_display_text())
        self.adjustSize()
        self.setFixedHeight(max(1, int(self.sizeHint().height())))

    def _ensure_source_pixmap(self) -> QPixmap:
        if self._source_pixmap is not None and not self._source_pixmap.isNull():
            return self._source_pixmap
        fetch = self.DEFAULT_ICON_SIZE
        base = file_icon_pixmap(self.file_path, fetch)
        if base.isNull():
            base = display_file_icon_pixmap(self.file_path, fetch)
        self._source_fetch_size = fetch
        self._source_pixmap = base
        return base

    @staticmethod
    def _scale_pixmap_from_source(
        base: QPixmap, fetch_size: int, icon_size: int, *, fast: bool
    ) -> QPixmap:
        if base.isNull():
            return base
        if icon_size == fetch_size:
            return base
        dpr = max(1.0, float(base.devicePixelRatio() or 1.0))
        target_phys = max(icon_size, int(round(icon_size * dpr)))
        mode = (
            Qt.TransformationMode.FastTransformation
            if fast
            else Qt.TransformationMode.SmoothTransformation
        )
        scaled = base.scaled(
            target_phys,
            target_phys,
            Qt.AspectRatioMode.KeepAspectRatio,
            mode,
        )
        if not scaled.isNull():
            scaled.setDevicePixelRatio(dpr)
        return scaled

    def _apply_icon_size(
        self, icon_size: int, label_max_width: int, *, fast: bool = False
    ) -> None:
        try:
            _ = self.icon_label
        except RuntimeError:
            return
        self._icon_size = icon_size
        if fast:
            base = self._ensure_source_pixmap()
            pix = self._scale_pixmap_from_source(
                base, self._source_fetch_size, icon_size, fast=True
            )
            try:
                self.icon_label.setScaledContents(False)
                self.icon_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
                self.icon_label.setPixmap(pix)
                self.icon_label.setFixedSize(icon_size, icon_size)
            except RuntimeError:
                return
            # Keep 2-line ellipsis in sync on fast zoom/page refresh — skipping
            # this left the raw full name until the user clicked the icon.
            if self.text_label is not None:
                self._label_max_width = label_max_width
                try:
                    self._refresh_caption_for_selection()
                except RuntimeError:
                    return
            return

        self._label_max_width = label_max_width
        # One cell width for icon + caption so they share the same center line.
        cell_w = max(icon_size + 20, label_max_width + 10)
        base = self._ensure_source_pixmap()
        if icon_size <= self._source_fetch_size:
            pix = self._scale_pixmap_from_source(
                base, self._source_fetch_size, icon_size, fast=False
            )
        else:
            pix = display_file_icon_pixmap(self.file_path, icon_size)
            if not pix.isNull():
                self._source_pixmap = pix
                self._source_fetch_size = icon_size
        try:
            self.icon_label.setScaledContents(False)
            self.icon_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.icon_label.setPixmap(pix)
            # Logical size stays icon_size; HiDPI pixmap carries its own DPR.
            self.icon_label.setFixedSize(icon_size, icon_size)
            self.icon_label.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
            if self.text_label is not None:
                self.text_label.setSizePolicy(
                    QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed
                )
            self.setFixedWidth(cell_w)
            self.setMinimumWidth(cell_w)
            self.setMaximumWidth(cell_w)
            # Lock height to content so QGridLayout row stretch cannot inflate the
            # selection chrome with empty space under short captions.
            self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
        except RuntimeError:
            return
        if self.text_label is not None:
            try:
                self._refresh_caption_for_selection()
            except RuntimeError:
                return
        else:
            try:
                self.adjustSize()
                self.setFixedHeight(max(1, int(self.sizeHint().height())))
            except RuntimeError:
                return

    def apply_renamed_path(self, new_path: Path) -> None:
        self.file_path = Path(new_path)
        self._source_pixmap = None
        self._apply_icon_size(self._icon_size, self._label_max_width)

    def set_icon_size(
        self, icon_size: int, label_max_width: int, *, fast: bool = False
    ) -> None:
        self._apply_icon_size(icon_size, label_max_width, fast=fast)

    def is_selected(self) -> bool:
        return bool(self._selected)

    def set_selected(self, selected: bool) -> None:
        selected = bool(selected)
        if self._selected == selected:
            if selected:
                mark_desktop_selection_live(True)
            return
        self._selected = selected
        try:
            self._refresh_caption_for_selection()
        except Exception:
            pass
        _commit_fence_item_selection_visual(self)
        mark_desktop_selection_live(True if selected else None)

    def _apply_click_selection(self, event) -> None:
        apply_fence_click_selection(
            self,
            event.modifiers(),
            right_button=event.button() == Qt.MouseButton.RightButton,
        )

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            cancel_pending_label_rename(self)
            self._press_was_selected = self.is_selected()
        if event.button() in (Qt.MouseButton.LeftButton, Qt.MouseButton.RightButton):
            self._apply_click_selection(event)
        if event.button() == Qt.MouseButton.LeftButton:
            self._press_pos = event.position().toPoint()
            # Must accept: QWidget's default ignores the press, which bubbles to
            # items_widget / viewport and our empty-click handler clears selection.
            event.accept()
            return
        if event.button() == Qt.MouseButton.RightButton:
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if not (event.buttons() & Qt.MouseButton.LeftButton) or self._press_pos is None:
            return
        # Match public floats: plain clicks often jitter past 8px.
        if (event.position().toPoint() - self._press_pos).manhattanLength() < 24:
            return
        cancel_pending_label_rename(self)
        press_local = QPoint(self._press_pos)
        self._press_pos = None
        drag_paths = paths_for_fence_drag(self)
        if self._virtual_mode:
            result, unpinned = start_virtual_item_drag(
                self,
                self.file_path,
                self._fence_id,
                press_local=press_local,
                paths=drag_paths,
            )
            if unpinned:
                batch = list(
                    getattr(self, "_desktidy_unpinned_paths", None) or drag_paths
                )
                fence = find_fence_widget(self)
                batch_unpin = (
                    getattr(fence, "_unpin_virtual_paths", None) if fence else None
                )
                if callable(batch_unpin):
                    QTimer.singleShot(
                        0,
                        lambda paths=list(batch): batch_unpin(
                            [Path(p) for p in paths], place_on_public=True
                        ),
                    )
                else:
                    for path in batch:
                        QTimer.singleShot(
                            0, lambda p=Path(path): self.virtual_unpinned.emit(p)
                        )
            elif result in (Qt.DropAction.CopyAction, Qt.DropAction.MoveAction):
                # Pet trash / folder move / same-fence reorder already updated
                # cells — a full refresh_needed flash fights quiet_end.
                if not bool(getattr(self, "_desktidy_drag_quiet", False)):
                    self.refresh_needed.emit()
                try:
                    self._desktidy_drag_quiet = False
                except Exception:
                    pass
            return
        start_file_drag(self, self.file_path)
        self.refresh_needed.emit()

    def mouseDoubleClickEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            mark_item_opened_by_double_click(self)
            self.activated.emit(self.file_path)
        super().mouseDoubleClickEvent(event)

    def mouseReleaseEvent(self, event) -> None:
        if (
            event.button() == Qt.MouseButton.LeftButton
            and self._press_pos is not None
        ):
            release_pos = event.position().toPoint()
            if finalize_plain_click_selection(
                self,
                was_selected=self._press_was_selected,
                modifiers=event.modifiers(),
            ):
                event.accept()
                self._press_pos = None
                return
            if try_inplace_rename_on_label_click(
                self,
                was_selected=self._press_was_selected,
                press_pos=self._press_pos,
                release_pos=release_pos,
                modifiers=event.modifiers(),
            ):
                event.accept()
                self._press_pos = None
                return
        self._press_pos = None
        super().mouseReleaseEvent(event)

    def keyPressEvent(self, event) -> None:
        if handle_fence_item_key(self, event):
            event.accept()
            return
        super().keyPressEvent(event)

    def _show_menu(self, pos: QPoint) -> None:
        from src.shell_file_menu import show_file_context_menu

        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app else None
        settings = getattr(desk, "settings", None) if desk is not None else None
        begin = getattr(desk, "_begin_desktop_popup", None) if desk else None
        end_later = getattr(desk, "_end_desktop_popup_later", None) if desk else None
        if callable(begin):
            begin()

        action_paths = paths_for_icon_shell_action(self, Path(self.file_path))
        extras = build_file_icon_shell_extras(
            file_path=self.file_path,
            settings=settings,
            desk=desk,
            virtual_mode=self._virtual_mode,
            source_fence_id=self._fence_id,
            virtual_unpin_callback=lambda p: self.virtual_unpinned.emit(p),
            anchor=self,
            action_paths=action_paths,
        )

        global_pos = self.mapToGlobal(pos)
        try:
            hwnd = int(self.window().winId()) if self.window() is not None else int(self.winId())
        except Exception:
            hwnd = 0
        try:
            shown = show_file_context_menu(
                self.file_path,
                paths=action_paths,
                x=global_pos.x(),
                y=global_pos.y(),
                hwnd=hwnd,
                extra_commands=extras,
                parent_widget=self,
                on_before_shell_invoke=lambda verb="": prepare_shell_item_invoke(
                    self, self.file_path, verb, related_paths=action_paths
                ),
            )
        finally:
            if callable(end_later):
                end_later(400)

        after_shell_file_menu(
            self, self.file_path, shown=shown, related_paths=action_paths
        )
