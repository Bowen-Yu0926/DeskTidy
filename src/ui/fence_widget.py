"""Desktop fence (zone) widget with resize and drag-drop support."""

from __future__ import annotations

from pathlib import Path

from PyQt6.QtCore import (
    QEvent,
    QObject,
    QPoint,
    QRect,
    QSize,
    QTimer,
    QUrl,
    Qt,
    pyqtSignal,
)
from PyQt6.QtGui import QCursor, QFont, QRegion
from PyQt6.QtWidgets import (
    QApplication,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QMenu,
    QPushButton,
    QRubberBand,
    QScrollArea,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from src.fence_pages import get_fence_pages
from src.fence_rules import (
    assign_paths_to_virtual_fence,
    get_virtual_items_for_fence,
    is_portal_fence,
    get_portal_path,
    list_portal_entries,
    resolve_fence_folder,
    set_virtual_item_order,
    unpin_paths_from_virtual_fence,
)
from src.settings import get_desktop_path, save_settings
from src.ui.fence_icon_item import (
    FenceIconItem,
    _commit_fence_item_selection_visual,
    after_shell_file_menu,
    apply_fence_click_selection,
    build_file_icon_shell_extras,
    cancel_pending_label_rename,
    display_name_for_path,
    fence_selection_batch,
    handle_fence_item_key,
    paths_for_fence_drag,
    paths_for_icon_shell_action,
    prepare_shell_item_invoke,
    select_fence_item,
    start_file_drag,
    start_virtual_item_drag,
    widget_is_under_desktidy_fence,
)
from src.ui.screen_snap import snap_geometry
from src.win_shell import (
    collect_drop_paths,
    configure_desktop_overlay,
    desktidy_source_fence_id,
    import_drop_to_folder,
    mime_has_droppable_items,
    move_item_to_folder,
    move_namespace_icon_to_folder,
    open_containing_folder,
    open_path,
    preferred_drop_action_for_mime,
    reveal_in_explorer,
)

# Match Explorer / Fences: light captions on the dark WeChat-style panel.
_DEFAULT_FENCE_TITLE_COLOR = "#FFFFFF"
_DEFAULT_FENCE_TEXT_COLOR = "#FFFFFF"
_DEFAULT_FENCE_MUTED_COLOR = "#CBD5E1"


class FenceItemLabel(QLabel):
    """Draggable, clickable file item inside a fence."""

    activated = pyqtSignal(Path)
    refresh_needed = pyqtSignal()
    virtual_unpinned = pyqtSignal(Path)

    def __init__(
        self,
        file_path: Path,
        parent: QWidget | None = None,
        *,
        virtual_mode: bool = False,
        fence_id: str = "",
    ):
        display_name = display_name_for_path(file_path)
        display = display_name[:14] + ("…" if len(display_name) > 14 else "")
        super().__init__(display, parent)
        self.file_path = file_path
        self._virtual_mode = virtual_mode
        self._fence_id = fence_id
        self._press_pos: QPoint | None = None
        self._press_was_selected = False
        self._selected = False
        self.setObjectName("fenceItem")
        self.setToolTip(str(file_path))
        self.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.setWordWrap(True)
        self.setCursor(Qt.CursorShape.ArrowCursor)
        # NoFocus: NOACTIVATE translucent fence — ClickFocus/setFocus flashes.
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.customContextMenuRequested.connect(self._show_menu)

    def apply_renamed_path(self, new_path: Path) -> None:
        self.file_path = Path(new_path)
        display_name = display_name_for_path(new_path)
        display = display_name[:14] + ("…" if len(display_name) > 14 else "")
        self.setText(display)
        self.setToolTip(str(new_path))

    def is_selected(self) -> bool:
        return bool(self._selected)

    def set_selected(self, selected: bool) -> None:
        selected = bool(selected)
        if self._selected == selected:
            return
        self._selected = selected
        _commit_fence_item_selection_visual(self)

    def paintEvent(self, event) -> None:  # noqa: N802
        if self._selected:
            from PyQt6.QtGui import QPainter

            from src.ui.fence_icon_item import paint_fence_item_selection

            painter = QPainter(self)
            paint_fence_item_selection(self, painter)
            painter.end()
        super().paintEvent(event)

    def resizeEvent(self, event) -> None:  # noqa: N802
        super().resizeEvent(event)

    def _apply_click_selection(self, event) -> None:
        apply_fence_click_selection(
            self,
            event.modifiers(),
            right_button=event.button() == Qt.MouseButton.RightButton,
        )

    def mouseDoubleClickEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            from src.ui.fence_icon_item import mark_item_opened_by_double_click

            mark_item_opened_by_double_click(self)
            self.activated.emit(self.file_path)
            event.accept()
            return
        super().mouseDoubleClickEvent(event)

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            cancel_pending_label_rename(self)
            self._press_was_selected = self.is_selected()
        if event.button() in (Qt.MouseButton.LeftButton, Qt.MouseButton.RightButton):
            self._apply_click_selection(event)
        if event.button() == Qt.MouseButton.LeftButton:
            self._press_pos = event.position().toPoint()
            # Accept so the press does not bubble to items_widget and clear selection.
            event.accept()
            return
        if event.button() == Qt.MouseButton.RightButton:
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if not (event.buttons() & Qt.MouseButton.LeftButton) or self._press_pos is None:
            return
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
                # Defer: rebuild must not run while this widget is still in mouseMove.
                batch = list(
                    getattr(self, "_desktidy_unpinned_paths", None) or drag_paths
                )
                fence = self
                while fence is not None and fence.__class__.__name__ != "FenceWidget":
                    fence = fence.parentWidget()
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
                return
            if result in (Qt.DropAction.CopyAction, Qt.DropAction.MoveAction):
                self.refresh_needed.emit()
            return
        result = start_file_drag(self, self.file_path)
        if result in (Qt.DropAction.MoveAction, Qt.DropAction.CopyAction):
            self.refresh_needed.emit()

    def mouseReleaseEvent(self, event) -> None:
        if (
            event.button() == Qt.MouseButton.LeftButton
            and self._press_pos is not None
        ):
            from src.ui.fence_icon_item import (
                finalize_plain_click_selection,
                try_inplace_rename_on_label_click,
            )

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
        from PyQt6.QtWidgets import QApplication

        from src.shell_file_menu import show_file_context_menu

        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
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

    def _delete_file(self) -> None:
        from src.ui.fence_icon_item import delete_selected_fence_items

        delete_selected_fence_items(self)


# One app-level filter for Alt+wheel zoom — N fences each installing
# QApplication.installEventFilter multiplies every Qt event by fence count.
# Desktop overlays often never receive QWheelEvent while Alt is held (SYSKEY /
# WorkerW hit-testing), so we also subscribe to the shared WH_MOUSE_LL hook.
_fence_alt_zoom_targets: list = []
_fence_alt_zoom_filter = None
_fence_zoom_ll_registered = False
_fence_zoom_bridge = None

# Qt / Win32: one traditional mouse notch. High-res wheels send many smaller deltas.
_WHEEL_ZOOM_NOTCH = 120
_VK_MENU = 0x12
_VK_CONTROL = 0x11


def _wheel_delta_y(event) -> int:
    """Prefer angleDelta; fall back to pixelDelta (trackpads / precision mice)."""
    try:
        dy = int(event.angleDelta().y())
    except Exception:
        dy = 0
    if dy != 0:
        return dy
    try:
        return int(event.pixelDelta().y())
    except Exception:
        return 0


def _zoom_modifier_held(event=None) -> bool:
    """True when Alt (or Ctrl) is held for icon zoom.

    On Windows, QWheelEvent.modifiers() often omits AltModifier while Alt is
    physically down (SYSKEY). Also check queryKeyboardModifiers + GetAsyncKeyState.
    Ctrl+wheel is accepted as a standard zoom alternate.
    """
    mods = Qt.KeyboardModifier.NoModifier
    if event is not None:
        try:
            mods |= event.modifiers()
        except Exception:
            pass
    try:
        mods |= QApplication.queryKeyboardModifiers()
    except Exception:
        pass
    zoom_mods = Qt.KeyboardModifier.AltModifier | Qt.KeyboardModifier.ControlModifier
    if mods & zoom_mods:
        return True
    try:
        from src.desktop_ll_mouse import zoom_modifier_physically_down

        if zoom_modifier_physically_down():
            return True
    except Exception:
        pass
    try:
        import ctypes

        user32 = ctypes.windll.user32
        if user32.GetAsyncKeyState(_VK_MENU) & 0x8000:
            return True
        if user32.GetAsyncKeyState(_VK_CONTROL) & 0x8000:
            return True
    except Exception:
        pass
    return False


def _zoom_notches_from_delta(remain: int, dy: int) -> tuple[int, int]:
    """Accumulate wheel delta into whole notches; return (notches, new_remain)."""
    if dy == 0:
        return 0, remain
    if remain and (remain > 0) != (dy > 0):
        remain = 0
    remain += dy
    notches = int(remain / _WHEEL_ZOOM_NOTCH)
    remain -= notches * _WHEEL_ZOOM_NOTCH
    return notches, remain


class _FenceZoomHookBridge(QObject):
    """Marshal LL-hook wheel zooms onto the Qt GUI thread."""

    zoom_at = pyqtSignal(int, int, int)  # x, y, wheel_delta


class _FenceAltZoomAppFilter(QObject):
    def eventFilter(self, obj: QObject, event: QEvent) -> bool:  # noqa: N802
        if event.type() != QEvent.Type.Wheel:
            return False
        if not _zoom_modifier_held(event):
            return False
        pos = QCursor.pos()
        for fence in tuple(_fence_alt_zoom_targets):
            try:
                if fence is None or not fence.isVisible():
                    continue
                if fence.frameGeometry().contains(pos):
                    if fence._handle_zoom_wheel(event):
                        return True
                    return False
            except RuntimeError:
                continue
        return False


def _fence_under_point(x: int, y: int):
    for fence in tuple(_fence_alt_zoom_targets):
        try:
            if fence is None or not fence.isVisible():
                continue
            if fence.frameGeometry().contains(int(x), int(y)):
                return fence
        except RuntimeError:
            continue
    return None


def _on_ll_fence_zoom_wheel(w_param: int, l_param: int) -> bool:
    """Eat Alt/Ctrl+wheel over a fence so Explorer/Qt cannot steal the gesture."""
    from src.desktop_ll_mouse import MSLLHOOKSTRUCT, WM_MOUSEWHEEL, wheel_delta_from_ll

    if int(w_param) != WM_MOUSEWHEEL:
        return False
    try:
        info = MSLLHOOKSTRUCT.from_address(int(l_param))
        x, y = int(info.pt.x), int(info.pt.y)
    except Exception:
        return False
    if _fence_under_point(x, y) is None:
        return False
    dy = wheel_delta_from_ll(l_param)
    if dy == 0:
        return False
    bridge = _fence_zoom_bridge
    if bridge is None:
        return False
    bridge.zoom_at.emit(x, y, dy)
    return True


def _apply_ll_fence_zoom(x: int, y: int, dy: int) -> None:
    fence = _fence_under_point(x, y)
    if fence is None:
        return
    fence._apply_zoom_wheel_delta(dy)


def _register_fence_alt_zoom(fence: "FenceWidget") -> None:
    global _fence_alt_zoom_filter, _fence_zoom_ll_registered, _fence_zoom_bridge
    if fence in _fence_alt_zoom_targets:
        return
    _fence_alt_zoom_targets.append(fence)
    app = QApplication.instance()
    if app is None:
        return
    if _fence_alt_zoom_filter is None:
        _fence_alt_zoom_filter = _FenceAltZoomAppFilter(app)
        app.installEventFilter(_fence_alt_zoom_filter)
    if _fence_zoom_bridge is None:
        _fence_zoom_bridge = _FenceZoomHookBridge(app)
        _fence_zoom_bridge.zoom_at.connect(
            _apply_ll_fence_zoom, Qt.ConnectionType.QueuedConnection
        )
    if not _fence_zoom_ll_registered:
        from src.desktop_ll_mouse import (
            register_ll_mouse_handler,
            set_wheel_interest_check,
        )

        set_wheel_interest_check(
            lambda x, y: _fence_under_point(x, y) is not None
        )
        register_ll_mouse_handler(_on_ll_fence_zoom_wheel)
        _fence_zoom_ll_registered = True


def _unregister_fence_alt_zoom(fence: "FenceWidget") -> None:
    global _fence_alt_zoom_filter, _fence_zoom_ll_registered, _fence_zoom_bridge
    try:
        _fence_alt_zoom_targets.remove(fence)
    except ValueError:
        pass
    if _fence_alt_zoom_targets:
        return
    app = QApplication.instance()
    if app is not None and _fence_alt_zoom_filter is not None:
        app.removeEventFilter(_fence_alt_zoom_filter)
    _fence_alt_zoom_filter = None
    if _fence_zoom_ll_registered:
        from src.desktop_ll_mouse import (
            set_wheel_interest_check,
            unregister_ll_mouse_handler,
        )

        unregister_ll_mouse_handler(_on_ll_fence_zoom_wheel)
        set_wheel_interest_check(None)
        _fence_zoom_ll_registered = False
    _fence_zoom_bridge = None


class FenceWidget(QWidget):
    """A draggable, resizable desktop zone displaying categorized files."""

    geometry_changed = pyqtSignal()
    files_changed = pyqtSignal()
    public_items_changed = pyqtSignal(object)  # Path | None — optional new public float
    hide_requested = pyqtSignal()
    page_move_requested = pyqtSignal(int)  # target page id
    dissolve_requested = pyqtSignal()

    RESIZE_MARGIN = 8
    MIN_WIDTH = 160
    MIN_HEIGHT = 180
    COLLAPSED_HEIGHT = 44
    HEADER_OVERLAY_HEIGHT = 40
    FOOTER_OVERLAY_HEIGHT = 22
    EDGE_SNAP_THRESHOLD = 24
    # Default desktop-medium-ish glyph (40px). Users can Alt+wheel / menu to enlarge.
    ICON_BASE_SIZE = 40
    ICON_BASE_WIDTH = 220
    ICON_BASE_HEIGHT = 320
    ICON_MIN_SIZE = 32
    ICON_MAX_SIZE = 128

    def __init__(
        self,
        config: dict,
        settings: dict,
        parent: QWidget | None = None,
    ):
        super().__init__(parent)
        self.config = config
        self.settings = settings
        self.fence_name = config.get("name", "分区")
        self.folder_name = config.get("folder") or self.fence_name
        self.sort_by = config.get("sort_by", "name")
        self._style = dict(config.get("style") or {})
        self._collapsed = False
        self._saved_height = int(config.get("height", 320))
        self._position_locked = bool(config.get("position_locked", False))
        self._drag_pos: QPoint | None = None
        # Deferred empty-panel press (icon fences): move past threshold → drag zone.
        self._panel_press_pending: tuple[QObject, QPoint, QPoint] | None = None
        self._PANEL_DRAG_THRESHOLD = 6
        self._resize_mode: str | None = None
        self._resize_start_geo = None
        self._resize_start_pos: QPoint | None = None
        self._chrome_btn_press = None  # QPushButton | None — title-bar chip press
        self._peek_mode = False
        self._hovered = False
        # Per-fence icon zoom (Alt+wheel / 右键菜单). Persisted as config["icon_zoom"].
        self._icon_zoom = 1.0
        self._load_icon_zoom_from_config()
        self._zoom_wheel_remain = 0
        self._last_icon_size = 0
        self._last_max_cols = 0
        self._force_refresh_pending = False
        self._refresh_debounce = QTimer(self)
        self._refresh_debounce.setSingleShot(True)
        self._refresh_debounce.setInterval(200)
        self._refresh_debounce.timeout.connect(self._refresh_impl)
        self._icon_scale_debounce = QTimer(self)
        self._icon_scale_debounce.setSingleShot(True)
        self._icon_scale_debounce.setInterval(90)
        self._icon_scale_debounce.timeout.connect(
            lambda: self._update_icon_scale(fast=True)
        )
        self._icon_scale_settle = QTimer(self)
        self._icon_scale_settle.setSingleShot(True)
        self._icon_scale_settle.setInterval(220)
        self._icon_scale_settle.timeout.connect(
            lambda: self._update_icon_scale(fast=False)
        )
        self._zoom_persist_timer = QTimer(self)
        self._zoom_persist_timer.setSingleShot(True)
        self._zoom_persist_timer.setInterval(400)
        self._zoom_persist_timer.timeout.connect(self._flush_icon_zoom_persist)
        self._zoom_hint_timer = QTimer(self)
        self._zoom_hint_timer.setSingleShot(True)
        self._zoom_hint_timer.setInterval(900)
        self._zoom_hint_timer.timeout.connect(self._restore_footer_text)
        # Explorer-like multi-select: Shift anchor + rubber-band on empty area.
        self._selection_anchor: QWidget | None = None
        self._marquee_origin: QPoint | None = None
        self._marquee_additive = False
        self._marquee_band: QRubberBand | None = None

        # Initial flags: never WindowStaysOnBottomHint (Win+D / geometry bugs).
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.Window
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
        self.setAcceptDrops(True)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setMinimumSize(self.MIN_WIDTH, self.COLLAPSED_HEIGHT)
        self._seed_geometry_from_config()

        self._build_ui()
        self._apply_style()
        self._update_lock_button()
        self.setWindowOpacity(self._target_opacity())
        _register_fence_alt_zoom(self)
        # Normal create: defer grid to keep ctor light. Page-switch first-visit
        # sets ``_desktidy_defer_refresh`` in ``_prepare_page_switch_new_fence``;
        # ``DeskTidyApp._refresh_fences_after_page_switch`` rebuilds after batch show.
        def _deferred_ctor_refresh() -> None:
            if bool(getattr(self, "_desktidy_defer_refresh", False)):
                return
            self._refresh_impl()

        QTimer.singleShot(0, _deferred_ctor_refresh)

    def closeEvent(self, event) -> None:
        _unregister_fence_alt_zoom(self)
        super().closeEvent(event)

    def _seed_geometry_from_config(self) -> None:
        """Restore saved size before the native HWND exists.

        ``winId()`` otherwise births the overlay at ``MIN_WIDTH`` (工作→文档
        first visit: one-column grid, or an empty-looking fence).
        """
        try:
            w = int((self.config or {}).get("width") or 0)
            h = int((self.config or {}).get("height") or 0)
        except (TypeError, ValueError):
            return
        if w < self.MIN_WIDTH:
            return
        self.resize(w, max(h, self.COLLAPSED_HEIGHT))

    def _icon_zoom_bounds(self) -> tuple[float, float]:
        return 0.7, 1.6

    def _load_icon_zoom_from_config(self) -> None:
        raw = (self.config or {}).get("icon_zoom", 1.0)
        try:
            zoom = float(raw)
        except (TypeError, ValueError):
            zoom = 1.0
        lo, hi = self._icon_zoom_bounds()
        self._icon_zoom = max(lo, min(hi, zoom))

    def _persist_icon_zoom(self) -> None:
        zoom = round(float(self._icon_zoom), 3)
        if isinstance(self.config, dict):
            self.config["icon_zoom"] = zoom
        # Coalesce disk/layout writes while the user is still scrolling.
        self._zoom_persist_timer.start()

    def _flush_icon_zoom_persist(self) -> None:
        self.geometry_changed.emit()

    def _set_icon_zoom(self, zoom: float) -> None:
        lo, hi = self._icon_zoom_bounds()
        new_zoom = max(lo, min(hi, float(zoom)))
        if abs(new_zoom - self._icon_zoom) < 1e-3:
            return
        self._icon_zoom = new_zoom
        self._persist_icon_zoom()
        self._show_zoom_hint()
        prev_cols = self._last_max_cols
        self._last_icon_size = 0
        _icon_size, max_cols, _label_width = self._icon_layout_metrics()
        if max_cols != prev_cols:
            self._last_max_cols = 0
            self.refresh()
        else:
            self._schedule_icon_scale_update()

    def _schedule_icon_scale_update(self) -> None:
        self._icon_scale_debounce.start()
        self._icon_scale_settle.start()

    def _footer_count_text(self) -> str:
        entries = self._get_entries()
        count = min(len(entries), 60)
        return f"共 {count} 项" if count else ""

    def _restore_footer_text(self) -> None:
        if hasattr(self, "footer_label"):
            self.footer_label.setText(self._footer_count_text())
            # Zoom hint may have forced the overlay visible — restore chrome rule.
            self._chrome_vis_sig = None
            self._update_chrome_visibility()

    def _show_zoom_hint(self) -> None:
        if not hasattr(self, "footer_label"):
            return
        percent = int(round(self._icon_zoom * 100))
        self.footer_label.setText(f"图标大小 {percent}%")
        if not self._collapsed:
            self._sync_footer_geometry()
            self.footer_label.show()
            self.footer_label.raise_()
            if hasattr(self, "header_widget"):
                self.header_widget.raise_()
        self._zoom_hint_timer.start()

    def _zoom_in(self) -> None:
        self._set_icon_zoom(self._icon_zoom + 0.15)

    def _zoom_out(self) -> None:
        self._set_icon_zoom(self._icon_zoom - 0.15)

    def _reset_icon_zoom(self) -> None:
        self._set_icon_zoom(1.0)

    def _build_ui(self) -> None:
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)

        self.container = QFrame()
        self.container.setObjectName("fenceContainer")
        # Translucent top-level: QSS background only paints with this attribute.
        self.container.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.container.setAcceptDrops(True)
        self.container.installEventFilter(self)
        container_layout = QVBoxLayout(self.container)
        container_layout.setContentsMargins(0, 0, 0, 0)
        container_layout.setSpacing(0)

        header = QHBoxLayout()
        # Inset past RESIZE_MARGIN so × / collapse sit outside the east/north
        # resize strip (narrow fences used to put the close button in that strip).
        edge = self.RESIZE_MARGIN + 4
        header.setContentsMargins(8, edge, edge, 6)
        header.setSpacing(6)

        self.accent_bar = QFrame()
        self.accent_bar.setObjectName("fenceAccent")
        self.accent_bar.setFixedWidth(4)
        self.accent_bar.setMinimumHeight(22)

        self.title_label = QLabel(self.fence_name)
        self.title_label.setObjectName("fenceTitle")
        self.title_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.title_label.setFont(QFont("Microsoft YaHei UI", 11, QFont.Weight.Bold))
        # Content width only — stretch=1 used to cover the whole top icon row
        # and steal hover/clicks (cursor stuck as arrow on icon upper half).
        self.title_label.setSizePolicy(
            QSizePolicy.Policy.Maximum, QSizePolicy.Policy.Preferred
        )

        self.collapse_btn = QPushButton("▾")
        self.collapse_btn.setObjectName("fenceBtn")
        self.collapse_btn.setFixedSize(26, 26)
        self.collapse_btn.setFlat(True)
        self.collapse_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.collapse_btn.setToolTip("折叠/展开")
        self.collapse_btn.clicked.connect(self._toggle_collapse)

        self.lock_btn = QPushButton()
        self.lock_btn.setObjectName("fenceBtn")
        self.lock_btn.setFixedSize(26, 26)
        self.lock_btn.setIconSize(QSize(14, 14))
        self.lock_btn.setFlat(True)
        self.lock_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.lock_btn.clicked.connect(self._toggle_position_lock)

        # Equal side slots keep the title visually centered (collapse + lock).
        side_w = 80
        self.left_slot = QWidget()
        self.left_slot.setFixedWidth(side_w)
        left_layout = QHBoxLayout(self.left_slot)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.setSpacing(6)
        left_layout.addWidget(self.accent_bar)
        left_layout.addStretch()

        self.right_slot = QWidget()
        self.right_slot.setFixedWidth(side_w)
        right_layout = QHBoxLayout(self.right_slot)
        right_layout.setContentsMargins(0, 0, 0, 0)
        right_layout.setSpacing(2)
        right_layout.addStretch()
        right_layout.addWidget(self.collapse_btn)
        right_layout.addWidget(self.lock_btn)

        header.addWidget(self.left_slot)
        header.addStretch(1)
        header.addWidget(self.title_label, stretch=0)
        header.addStretch(1)
        header.addWidget(self.right_slot)

        self.header_widget = QWidget(self.container)
        self.header_widget.setObjectName("fenceHeader")
        self.header_widget.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.header_widget.setAcceptDrops(True)
        header_layout = QVBoxLayout(self.header_widget)
        header_layout.setContentsMargins(0, 0, 0, 0)
        header_layout.addLayout(header)
        self.header_widget.installEventFilter(self)
        self.title_label.installEventFilter(self)
        self.left_slot.installEventFilter(self)
        self.right_slot.installEventFilter(self)
        self.title_label.setAcceptDrops(True)
        self.left_slot.setAcceptDrops(True)
        self.right_slot.setAcceptDrops(True)
        # Overlay must not steal icon hits: only title text + buttons capture mouse.
        self._apply_header_mouse_pass_through()
        # Overlay on top of the panel — does not consume a layout row.
        self.header_widget.raise_()
        self.header_widget.hide()

        body = QWidget()
        body.setObjectName("fenceBody")
        # Do NOT set WA_TranslucentBackground on children — on Windows that punches
        # a hole through the parent and makes the dark panel look fully transparent.
        body.setAutoFillBackground(False)
        self.body_widget = body
        body_layout = QVBoxLayout(body)
        # Top/bottom insets reserve overlay bands (title + count). Putting the
        # footer in the layout used to shrink the scroll viewport on hover and
        # clip the last icon row (「图标显示不全」).
        body_layout.setContentsMargins(
            10, self.HEADER_OVERLAY_HEIGHT, 10, self.FOOTER_OVERLAY_HEIGHT
        )
        body_layout.setSpacing(6)
        self._body_layout = body_layout

        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setObjectName("fenceScroll")
        self.scroll.setFrameShape(QFrame.Shape.NoFrame)
        self.scroll.setAutoFillBackground(False)
        self.scroll.viewport().setAutoFillBackground(False)
        # Qt delivers drops to the deepest child — enable drop on scroll/body.
        for drop_target in (body, self.scroll, self.scroll.viewport()):
            drop_target.setAcceptDrops(True)
            drop_target.installEventFilter(self)

        self.items_widget = QWidget()
        self.items_widget.setObjectName("fenceItems")
        self.items_widget.setAutoFillBackground(False)
        self.items_widget.setAcceptDrops(True)
        self.items_widget.installEventFilter(self)
        self.items_layout = QGridLayout(self.items_widget)
        self.items_layout.setSpacing(4)
        # Extra right/bottom gutter so the last column is not clipped by the panel edge.
        self.items_layout.setContentsMargins(2, 2, 8, 4)
        # Horizontally center the icon block; keep top-aligned so a short row
        # does not float in the middle of a tall fence (large blank above icons).
        self.items_layout.setAlignment(
            Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop
        )
        self.items_widget.setSizePolicy(
            QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding
        )
        self.scroll.setWidget(self.items_widget)
        self.content_widget = self.scroll
        self._marquee_band = QRubberBand(
            QRubberBand.Shape.Rectangle, self.items_widget
        )
        self._marquee_band.hide()

        # Insertion caret shown while reordering / dropping icons.
        self._drop_indicator = QFrame(self.items_widget)
        self._drop_indicator.setObjectName("fenceDropIndicator")
        self._drop_indicator.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self._drop_indicator.hide()
        body_layout.addWidget(self.scroll)

        # Count / zoom hint overlays the reserved bottom band — never a layout row.
        self.footer_label = QLabel("", self.container)
        self.footer_label.setObjectName("fenceEmpty")
        self.footer_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.footer_label.setAttribute(
            Qt.WidgetAttribute.WA_TransparentForMouseEvents, True
        )
        self.footer_label.hide()
        # Body fills the panel; title overlays on hover.
        container_layout.addWidget(body)

        outer.addWidget(self.container)
        self._sync_header_geometry()
        self._sync_footer_geometry()
        self._wire_fence_context_menus()

    def _wire_fence_context_menus(self) -> None:
        """Body blank → Explorer shell menu; header → DeskTidy zone actions."""
        header_chrome = [
            getattr(self, "header_widget", None),
            getattr(self, "title_label", None),
            getattr(self, "left_slot", None),
            getattr(self, "right_slot", None),
        ]
        body_chrome = [
            self,
            self.container,
            getattr(self, "body_widget", None),
            getattr(self, "scroll", None),
            getattr(self, "items_widget", None),
            getattr(self, "footer_label", None),
        ]
        scroll = getattr(self, "scroll", None)
        if scroll is not None:
            body_chrome.append(scroll.viewport())

        for widget in header_chrome:
            if widget is None:
                continue
            widget.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            try:
                widget.customContextMenuRequested.disconnect(self._on_header_context_menu)
            except TypeError:
                pass
            try:
                widget.customContextMenuRequested.disconnect(self._on_chrome_context_menu)
            except TypeError:
                pass
            widget.customContextMenuRequested.connect(self._on_header_context_menu)

        for widget in body_chrome:
            if widget is None:
                continue
            widget.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            try:
                widget.customContextMenuRequested.disconnect(self._on_chrome_context_menu)
            except TypeError:
                pass
            try:
                widget.customContextMenuRequested.disconnect(self._on_header_context_menu)
            except TypeError:
                pass
            widget.customContextMenuRequested.connect(self._on_chrome_context_menu)

    def _on_header_context_menu(self, pos: QPoint) -> None:
        """Title bar RMB: DeskTidy-only verbs (refresh / dissolve)."""
        from PyQt6.QtWidgets import QMenu

        from src.system_defaults import is_locked_fence

        sender = self.sender()
        global_pos = (
            sender.mapToGlobal(pos) if isinstance(sender, QWidget) else QCursor.pos()
        )
        menu = QMenu(self)
        menu.addAction("刷新分区", lambda: self.refresh(force=True))
        if self.is_position_locked():
            menu.addAction("解锁位置", lambda: self.set_position_locked(False, emit=True))
        else:
            menu.addAction("锁定位置", lambda: self.set_position_locked(True, emit=True))
        menu.addAction("隐藏分区", self._request_hide)
        if not is_locked_fence(self.config):
            menu.addAction("解散分区", lambda: self.dissolve_requested.emit())
        menu.exec(global_pos)
        self._ensure_alive_after_shell_menu()
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app else None
        if desk is not None:
            recover = getattr(desk, "_recover_overlays_after_desktop_shell_menu", None)
            if callable(recover):
                QTimer.singleShot(200, recover)

    def _on_chrome_context_menu(self, pos: QPoint) -> None:
        sender = self.sender()
        if isinstance(sender, QWidget):
            self._show_fence_context_menu(sender.mapToGlobal(pos))
        else:
            self._show_fence_context_menu(QCursor.pos())

    def _request_hide(self) -> None:
        self.hide()
        self.hide_requested.emit()

    def _base_opacity(self) -> float:
        try:
            return float(self._style.get("opacity", 0.85))
        except (TypeError, ValueError):
            return 0.85

    def _target_opacity(self) -> float:
        # Panel alpha lives in stylesheet; keep the window fully opaque so icons stay crisp.
        return 1.0

    def _panel_rgb(self) -> tuple[int, int, int]:
        """Background RGB from fence style (honors light and dark presets)."""
        from src.ui.styles import fence_theme_defaults

        defaults = fence_theme_defaults()
        bg = self._style.get("background") or defaults.get("background")
        if isinstance(bg, str) and bg.startswith("#") and len(bg) >= 7:
            try:
                return (
                    int(bg[1:3], 16),
                    int(bg[3:5], 16),
                    int(bg[5:7], 16),
                )
            except ValueError:
                pass
        return (32, 36, 44)

    def _panel_rgba(self) -> str:
        """Panel fill: style background × opacity (Fences-like translucency)."""
        r, g, b = self._panel_rgb()
        alpha = int(max(0.25, min(1.0, self._base_opacity())) * 255)
        return f"rgba({r}, {g}, {b}, {alpha})"

    def _chrome_visible(self) -> bool:
        return self._hovered or self._peek_mode or self._drag_pos is not None

    @staticmethod
    def _is_light_color(color: str) -> bool:
        if not (isinstance(color, str) and color.startswith("#") and len(color) >= 7):
            return False
        try:
            r, g, b = int(color[1:3], 16), int(color[3:5], 16), int(color[5:7], 16)
        except ValueError:
            return False
        # Relative luminance — light panels need dark ink.
        return (0.2126 * r + 0.7152 * g + 0.0722 * b) > 160

    def _apply_style(self) -> None:
        from src.ui.styles import fence_theme_defaults

        defaults = fence_theme_defaults()
        accent = self._style.get("accent") or defaults.get("accent") or "#07C160"
        if not isinstance(accent, str) or not accent.startswith("#"):
            accent = "#07C160"
        try:
            radius = int(self._style.get("border_radius", defaults.get("border_radius", 10)))
        except (TypeError, ValueError):
            radius = 10
        radius = max(0, min(28, radius))

        r, g, b = self._panel_rgb()
        bg_hex = f"#{r:02X}{g:02X}{b:02X}"
        light = self._is_light_color(bg_hex)
        panel_rgba = self._panel_rgba()

        if self._peek_mode:
            border = f"2px solid {accent}"
        elif light:
            border = "1px solid rgba(15, 23, 42, 48)"
        else:
            border = "1px solid rgba(255, 255, 255, 40)"

        accent_bg = accent
        # Header chrome adapts for light panels. Captions must too — fixed white
        # on mint/frost packs made filenames unreadable (「文件显示不全」).
        if light:
            title_css = "#0F172A"
            btn_css = "#334155"
            btn_bg = "rgba(15, 23, 42, 28)"
            btn_hover_bg = "rgba(15, 23, 42, 52)"
            btn_hover_fg = "#0F172A"
            item_css = "#0F172A"
            empty_css = "#475569"
        else:
            title_css = _DEFAULT_FENCE_TITLE_COLOR
            btn_css = _DEFAULT_FENCE_MUTED_COLOR
            btn_bg = "rgba(255, 255, 255, 38)"
            btn_hover_bg = "rgba(255, 255, 255, 72)"
            btn_hover_fg = "#ffffff"
            item_css = _DEFAULT_FENCE_TEXT_COLOR
            empty_css = _DEFAULT_FENCE_MUTED_COLOR
        self._chrome_btn_color = btn_css
        # WA_TranslucentBackground: transparent chips are click-through on Win32
        # layered hit-tests — only opaque painted pixels receive mouse events.
        # Same visual plane as the panel — a darker header strip looked like a
        # black "top border" and fought the body fill.
        header_bg = "transparent"

        self.container.setStyleSheet(
            f"""
            QFrame#fenceContainer {{
                background-color: {panel_rgba};
                border: {border};
                border-radius: {radius}px;
            }}
            QWidget#fenceHeader {{
                background-color: {header_bg};
                border: none;
            }}
            QFrame#fenceAccent {{
                background-color: transparent;
                border: none;
            }}
            QLabel#fenceTitle {{
                color: {title_css};
                font-weight: 700;
                font-size: 13px;
            }}
            QLabel#fenceItem {{
                color: {item_css};
            }}
            QLabel#fenceEmpty {{
                color: {empty_css};
            }}
            QPushButton#fenceBtn {{
                color: {btn_css};
                background-color: {btn_bg};
                border: none;
                border-radius: 4px;
                font-size: 14px;
                font-weight: 600;
                padding: 0px;
                min-width: 26px;
                min-height: 26px;
            }}
            QPushButton#fenceBtn:hover {{
                color: {btn_hover_fg};
                background-color: {btn_hover_bg};
            }}
            QScrollArea#fenceScroll {{
                background: transparent;
                border: none;
            }}
            QScrollArea#fenceScroll > QWidget > QWidget {{
                background: transparent;
            }}
            QWidget#fenceBody, QWidget#fenceItems {{
                /* Tiny non-zero alpha so empty panel still gets 框选 hits
                   (fully transparent pixels skip Win32 layered hit-tests).
                   Use 2 (0–255 scale), not 1.0 — rgba(...,1) can be read as
                   opaque black and paint a dark strip over icons. */
                background-color: rgba(0, 0, 0, 2);
            }}
            QFrame#fenceDropIndicator {{
                background-color: {accent_bg};
                border: none;
                border-radius: 2px;
            }}
            """
        )
        self._sync_body_top_inset()
        self._update_chrome_visibility()
        self._update_lock_button()
        self._flush_style_paint()

    def _sync_body_top_inset(self) -> None:
        """Keep the icon grid clear of title / count overlay bands."""
        layout = getattr(self, "_body_layout", None)
        if layout is None:
            return
        show_title = bool((self._style or {}).get("show_title", True))
        top = self.HEADER_OVERLAY_HEIGHT if (show_title or self._collapsed) else 8
        bottom = 8 if self._collapsed else self.FOOTER_OVERLAY_HEIGHT
        try:
            left, _old_top, right, _old_bottom = layout.getContentsMargins()
            if _old_top != top or _old_bottom != bottom:
                layout.setContentsMargins(left, top, right, bottom)
        except RuntimeError:
            pass

    def _flush_style_paint(self) -> None:
        """Force layered translucent HWND to show the latest QSS panel colors.

        ``ensure_live_fences_interactive`` raises with ``SWP_NOREDRAW`` (avoids
        Z-order flash). Do not restack after this flush — a raise that discarded
        client bits used to wipe mint panels to solid black / leave「分区外观
        选完没变化」until a later unrelated invalidate.

        Never use ``RDW_ERASE`` here — same rule as ``redraw_overlay_hwnd``: erase
        clears translucent fences to blank until the next desktop activation.
        """
        container = getattr(self, "container", None)
        if container is not None:
            try:
                container.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
                style = container.style()
                if style is not None:
                    style.unpolish(container)
                    style.polish(container)
            except RuntimeError:
                pass
            try:
                container.repaint()
            except RuntimeError:
                pass
        try:
            self.repaint()
        except RuntimeError:
            return
        # Do not call winId() here — that births an unattached HWND during ctor
        # before shell attach. Only redraw when a native window already exists.
        try:
            handle = self.windowHandle()
        except RuntimeError:
            return
        if handle is None:
            return
        try:
            hwnd = int(handle.winId())
        except Exception:
            return
        if not hwnd:
            return
        try:
            from src.desktop_shell_host import redraw_overlay_hwnd

            redraw_overlay_hwnd(hwnd)
        except Exception:
            pass

    def _apply_header_mouse_pass_through(self) -> None:
        """Keep title chips mouse-opaque; hit-mask lets icons through elsewhere.

        Qt skips a widget *and its entire subtree* when
        ``WA_TransparentForMouseEvents`` is set on an ancestor — so the header
        itself must stay opaque for collapse / lock.

        Expanded fences with icons: ``_sync_header_hit_mask`` limits the header
        HWND mask to the title text + buttons. Empty header pixels over the top
        icon row then fall through to the icons (file drag), instead of starting
        a whole-fence move — common when hovering a zone while other apps are
        open and dragging toward them.
        """
        chrome = (
            getattr(self, "header_widget", None),
            getattr(self, "left_slot", None),
            getattr(self, "right_slot", None),
            getattr(self, "accent_bar", None),
            getattr(self, "title_label", None),
            getattr(self, "collapse_btn", None),
            getattr(self, "lock_btn", None),
        )
        for widget in chrome:
            if widget is None:
                continue
            try:
                widget.setAttribute(
                    Qt.WidgetAttribute.WA_TransparentForMouseEvents, False
                )
            except RuntimeError:
                pass
        self._sync_header_hit_mask()

    def _sync_header_hit_mask(self) -> None:
        """Mask the overlay title bar to title + chips so icons stay draggable."""
        header = getattr(self, "header_widget", None)
        if header is None:
            return
        try:
            if header.isHidden():
                header.clearMask()
                return
        except RuntimeError:
            return
        # Empty / collapsed: full title bar is the move grip.
        if self._collapsed or not self.has_icon_widgets():
            try:
                header.clearMask()
            except RuntimeError:
                pass
            return
        # Side-slot width changes must settle before we sample geometries.
        try:
            layout = header.layout()
            if layout is not None:
                layout.activate()
        except RuntimeError:
            pass
        region = QRegion()

        def _add_in_header(widget: QWidget | None) -> None:
            nonlocal region
            if widget is None:
                return
            try:
                if widget.isHidden():
                    return
                size = widget.size()
                if size.width() <= 0 or size.height() <= 0:
                    size = widget.sizeHint()
                if size.width() <= 0 or size.height() <= 0:
                    return
                # Children may sit under an intermediate layout wrapper — always
                # map into header coordinates before building the mask.
                top_left = widget.mapTo(header, QPoint(0, 0))
                region = region.united(QRegion(QRect(top_left, size)))
            except RuntimeError:
                return

        _add_in_header(getattr(self, "left_slot", None))
        _add_in_header(getattr(self, "title_label", None))
        # Real chips only — not the whole right_slot (stretch covered icons).
        for btn in (
            getattr(self, "collapse_btn", None),
            getattr(self, "lock_btn", None),
        ):
            _add_in_header(btn)
        try:
            if region.isEmpty():
                header.clearMask()
            else:
                header.setMask(region)
        except RuntimeError:
            pass

    def _sync_header_geometry(self) -> None:
        """Pin the title bar as an overlay on the panel (no layout row)."""
        if not hasattr(self, "header_widget") or not hasattr(self, "container"):
            return
        w = max(self.container.width(), 1)
        self._sync_header_side_slots(w)
        if self._collapsed:
            h = max(self.container.height(), self.COLLAPSED_HEIGHT)
        else:
            h = self.HEADER_OVERLAY_HEIGHT
        self.header_widget.setGeometry(0, 0, w, h)
        self.header_widget.raise_()
        self._sync_header_hit_mask()

    def _sync_footer_geometry(self) -> None:
        """Pin the count / zoom hint to the reserved bottom band (no layout row)."""
        footer = getattr(self, "footer_label", None)
        container = getattr(self, "container", None)
        if footer is None or container is None:
            return
        w = max(container.width(), 1)
        h = self.FOOTER_OVERLAY_HEIGHT
        y = max(0, container.height() - h)
        footer.setGeometry(0, y, w, h)
        if not footer.isHidden():
            footer.raise_()
            header = getattr(self, "header_widget", None)
            if header is not None and not header.isHidden():
                header.raise_()

    def _header_chrome_buttons(self) -> tuple:
        """Title-bar widgets that must win over resize-edge hit testing."""
        return (
            getattr(self, "collapse_btn", None),
            getattr(self, "lock_btn", None),
            getattr(self, "title_label", None),
        )

    def _pos_on_header_chrome(self, pos: QPoint) -> bool:
        """True when *pos* (fence-local) is on collapse / lock / title."""
        header = getattr(self, "header_widget", None)
        # isHidden(): this widget was explicitly hidden. isVisible() is also
        # false when an ancestor (unshown fence in tests) is hidden.
        if header is None or header.isHidden():
            return False
        for widget in self._header_chrome_buttons():
            if widget is None:
                continue
            try:
                if widget.isHidden():
                    continue
                top_left = widget.mapTo(self, QPoint(0, 0))
                if QRect(top_left, widget.size()).contains(pos):
                    return True
            except RuntimeError:
                continue
        return False

    def _header_event_on_chrome_button(self, obj: QObject, event: QEvent) -> bool:
        """True when a header-filtered mouse event is over collapse/lock."""
        if not isinstance(obj, QWidget):
            return False
        try:
            local = event.position().toPoint()  # type: ignore[attr-defined]
            fence_pos = self.mapFromGlobal(obj.mapToGlobal(local))
        except Exception:
            return False
        return self._chrome_button_at_pos(fence_pos) is not None

    def _chrome_button_at_pos(self, pos: QPoint):
        """Return the visible title-bar chip under fence-local *pos*, else None."""
        header = getattr(self, "header_widget", None)
        if header is None or header.isHidden():
            return None
        buttons = (
            getattr(self, "collapse_btn", None),
            getattr(self, "lock_btn", None),
        )
        for btn in buttons:
            if btn is None:
                continue
            try:
                if btn.isHidden():
                    continue
                top_left = btn.mapTo(self, QPoint(0, 0))
                # Slightly inflate — translucent hit pixels are easy to miss.
                hit = QRect(top_left, btn.size()).adjusted(-2, -2, 2, 2)
                if hit.contains(pos):
                    return btn
            except RuntimeError:
                continue
        return None

    def _dispatch_chrome_button_mouse(self, obj: QObject, event: QEvent) -> bool:
        """Route title-bar chip clicks even when Win32 alpha lands on body/icons.

        With ``WA_TranslucentBackground``, fully transparent painted regions are
        click-through. Presses then arrive on icons/body under the header; map
        fence-local coords back onto collapse/lock and synthesize click.
        """
        etype = event.type()
        if etype not in (
            QEvent.Type.MouseButtonPress,
            QEvent.Type.MouseMove,
            QEvent.Type.MouseButtonRelease,
        ):
            return False
        header_buttons = {
            getattr(self, "collapse_btn", None),
            getattr(self, "lock_btn", None),
        }
        # Native delivery to the chip — let Qt emit clicked(); do not arm pending.
        if obj in header_buttons and etype == QEvent.Type.MouseButtonPress:
            self._chrome_btn_press = None
            return False
        if not isinstance(obj, QWidget):
            return False
        try:
            local = event.position().toPoint()  # type: ignore[attr-defined]
            fence_pos = self.mapFromGlobal(obj.mapToGlobal(local))
        except Exception:
            return False

        if etype == QEvent.Type.MouseButtonPress:
            try:
                if event.button() != Qt.MouseButton.LeftButton:  # type: ignore[attr-defined]
                    return False
            except Exception:
                return False
            btn = self._chrome_button_at_pos(fence_pos)
            if btn is None:
                return False
            self._chrome_btn_press = btn
            self._drag_pos = None
            self._resize_mode = None
            event.accept()
            return True

        if etype == QEvent.Type.MouseMove:
            if self._chrome_btn_press is None:
                return False
            event.accept()
            return True

        if etype == QEvent.Type.MouseButtonRelease:
            btn = self._chrome_btn_press
            if btn is None:
                return False
            self._chrome_btn_press = None
            try:
                if not btn.isHidden():
                    btn.click()
            except RuntimeError:
                pass
            event.accept()
            return True
        return False

    def _sync_header_side_slots(self, panel_width: int) -> None:
        """Keep title + button row inside narrow fences without clipping chips."""
        if not hasattr(self, "left_slot") or not hasattr(self, "right_slot"):
            return
        btn_w, gap = 26, 2
        inset = self.RESIZE_MARGIN
        wanted: list = []
        if getattr(self, "collapse_btn", None) is not None and self.collapse_btn.isVisible():
            wanted.append(self.collapse_btn)
        if getattr(self, "lock_btn", None) is not None and self.lock_btn.isVisible():
            wanted.append(self.lock_btn)

        def _slot_w(count: int) -> int:
            if count <= 0:
                return inset + btn_w
            return count * btn_w + max(0, count - 1) * gap + inset

        title_min = 24
        left_min = 8
        margins = 8 + inset
        while len(wanted) > 1:
            right_need = _slot_w(len(wanted))
            if left_min + right_need + margins + title_min <= panel_width:
                break
            # Prefer keeping lock; drop collapse first on very narrow fences.
            drop = self.collapse_btn if self.collapse_btn in wanted else wanted[0]
            drop.hide()
            wanted.remove(drop)

        right_w = min(100, max(_slot_w(len(wanted)), btn_w + inset))
        left_w = min(90, max(left_min, panel_width - margins - title_min - right_w))
        if left_w + right_w + margins + title_min > panel_width:
            left_w = max(left_min, panel_width - margins - title_min - right_w)
        # Cap the slot so the HBox cannot push chips past the east resize strip.
        inner = max(1, panel_width - 8 - inset)
        if left_w + right_w > inner - title_min:
            left_w = max(left_min, inner - title_min - right_w)
        if left_w + right_w > inner - 16:
            right_w = max(btn_w + inset, inner - left_w - 16)
        self.left_slot.setFixedWidth(max(left_min, left_w))
        self.right_slot.setFixedWidth(max(btn_w + inset, right_w))
        try:
            self.right_slot.layout().setContentsMargins(0, 0, inset, 0)
        except Exception:
            pass
        if getattr(self, "title_label", None) is not None:
            title_max = max(16, inner - self.left_slot.width() - self.right_slot.width())
            self.title_label.setMaximumWidth(title_max)

    def _update_chrome_visibility(self) -> None:
        chrome_on = self._chrome_visible()
        show_title = self._style.get("show_title", True)
        collapsible = self._style.get("collapsible", True)

        # Collapsed: always show title overlay (even if show_title is off — otherwise trapped).
        # Expanded: overlay title only on hover / peek — icons stay full-bleed underneath.
        if self._collapsed:
            show_header = True
        else:
            show_header = bool(show_title) and chrome_on
        has_icons = self.has_icon_widgets()
        # has_icons must be in the sig — empty→populated while hovered used to keep
        # a full-header mask and steal top-row file drags.
        sig = (show_header, chrome_on, self._collapsed, bool(collapsible), has_icons)
        if sig != getattr(self, "_chrome_vis_sig", None):
            self._chrome_vis_sig = sig
            self.header_widget.setVisible(show_header)
            self.title_label.setVisible(True)
            # No accent strip — title + buttons only over the panel.
            self.accent_bar.setVisible(False)

            self.collapse_btn.setVisible(show_header and collapsible)
            self.lock_btn.setVisible(show_header)
            self._sync_header_geometry()
            # Re-assert after show/hide — Qt can reset child attributes with parent.
            if show_header:
                self._apply_header_mouse_pass_through()
                # Layout may settle after setVisible; rebuild mask once more so the
                # title grip is included (first pass can see size 0).
                self._sync_header_hit_mask()

            self.footer_label.setVisible(
                bool(show_header)
                and (not self._collapsed)
                and bool(self.footer_label.text().strip())
            )
            if not self.footer_label.isHidden():
                self._sync_footer_geometry()

            policy = (
                Qt.ScrollBarPolicy.ScrollBarAsNeeded
                if chrome_on and not self._collapsed
                else Qt.ScrollBarPolicy.ScrollBarAlwaysOff
            )
            self.scroll.setVerticalScrollBarPolicy(policy)
            self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        elif show_header:
            # Geometry / icon layout may have changed without a chrome sig flip.
            self._sync_header_hit_mask()

    def set_collapsed(
        self,
        collapsed: bool,
        *,
        emit: bool = False,
        saved_height: int | None = None,
        layout: bool = True,
    ) -> None:
        """Collapse or expand the fence body."""
        collapsed = bool(collapsed)
        if saved_height is not None:
            self._saved_height = max(int(saved_height), self.MIN_HEIGHT)

        if collapsed == self._collapsed:
            if not layout:
                return
            if collapsed and self.height() != self.COLLAPSED_HEIGHT:
                self.resize(self.width(), self.COLLAPSED_HEIGHT)
            elif not collapsed and self.height() != self._saved_height:
                self.resize(self.width(), self._saved_height)
            return

        if collapsed:
            if saved_height is None:
                self._saved_height = max(self.height(), self.MIN_HEIGHT)
            self._collapsed = True
            self.body_widget.hide()
            self.content_widget.hide()
            self.footer_label.hide()
            self.collapse_btn.setText("▸")
            self.collapse_btn.setToolTip("展开")
            self.setMinimumHeight(self.COLLAPSED_HEIGHT)
            self.resize(self.width(), self.COLLAPSED_HEIGHT)
            self._update_chrome_visibility()
        else:
            self._collapsed = False
            self.body_widget.show()
            self.content_widget.show()
            self.footer_label.hide()
            self.collapse_btn.setText("▾")
            self.collapse_btn.setToolTip("折叠")
            self.setMinimumHeight(self.MIN_HEIGHT)
            self.resize(self.width(), max(self._saved_height, self.MIN_HEIGHT))
            self._update_icon_scale()
            self._update_chrome_visibility()

        if emit:
            self.geometry_changed.emit()

    def _toggle_collapse(self) -> None:
        self.set_collapsed(not self._collapsed, emit=True)

    def is_position_locked(self) -> bool:
        """User lock: fixed geometry (drag/resize off). Not system ``locked``."""
        return bool(self._position_locked)

    def _update_lock_button(self) -> None:
        btn = getattr(self, "lock_btn", None)
        if btn is None:
            return
        from src.ui.action_icons import make_fence_lock_icon

        color = getattr(self, "_chrome_btn_color", None) or _DEFAULT_FENCE_MUTED_COLOR
        locked = bool(self._position_locked)
        btn.setText("")
        btn.setIcon(make_fence_lock_icon(locked=locked, color=color, size=14))
        if locked:
            btn.setToolTip("解锁位置（可拖动）")
        else:
            btn.setToolTip("锁定位置（不可拖动）")

    def set_position_locked(self, locked: bool, *, emit: bool = False) -> None:
        locked = bool(locked)
        if locked == self._position_locked:
            self._update_lock_button()
            return
        self._position_locked = locked
        try:
            self.config["position_locked"] = locked
        except Exception:
            pass
        # Abort any in-progress move/resize.
        self._drag_pos = None
        self._panel_press_pending = None
        self._resize_mode = None
        self._resize_start_geo = None
        self._resize_start_pos = None
        try:
            self.releaseMouse()
        except Exception:
            pass
        self._update_chrome_visibility()
        self._update_lock_button()
        if emit:
            self.geometry_changed.emit()

    def _toggle_position_lock(self) -> None:
        self.set_position_locked(not self._position_locked, emit=True)

    def _target_folder(self) -> Path:
        portal = get_portal_path(self.config)
        if portal is not None:
            try:
                portal.mkdir(parents=True, exist_ok=True)
            except OSError:
                pass
            return portal
        return resolve_fence_folder(self.config)

    def _is_portal_mode(self) -> bool:
        return is_portal_fence(self.config)

    def _is_virtual_mode(self) -> bool:
        # Folder Portal mirrors a real directory (drop = FS move/copy).
        return not self._is_portal_mode()

    def _get_entries(self) -> list[Path]:
        if self._is_portal_mode():
            entries = list_portal_entries(self.config, self.settings)
            if self.config.get("sort_by") == "manual" or self.sort_by == "manual":
                # Portal has no pin order — fall back to name.
                return self._sort_entries(entries) if self.sort_by != "name" else sorted(
                    entries, key=lambda p: p.name.lower()
                )
            return self._sort_entries(entries)
        entries = get_virtual_items_for_fence(
            self.config,
            self.settings,
            all_pinned=getattr(self, "_all_pinned_cache", None),
            apply_sort=False,
        )
        if self.config.get("sort_by") == "manual" or self.sort_by == "manual":
            return entries
        return self._sort_entries(entries)

    def _sort_entries(self, entries: list[Path]) -> list[Path]:
        if self.sort_by == "date":
            from src.path_stat_cache import path_mtime

            return sorted(entries, key=path_mtime, reverse=True)
        if self.sort_by == "size":
            from src.path_stat_cache import path_size

            return sorted(entries, key=path_size, reverse=True)
        if self.sort_by == "type":
            return sorted(
                entries,
                key=lambda p: (p.suffix.lower(), p.name.lower()),
            )
        return sorted(entries, key=lambda p: p.name.lower())

    def _clear_items(self) -> None:
        while self.items_layout.count():
            child = self.items_layout.takeAt(0)
            if child.widget():
                child.widget().deleteLater()

    @staticmethod
    def _entry_key(path: Path) -> str:
        try:
            return str(path).casefold()
        except OSError:
            return str(path).casefold()

    def _available_items_width(self) -> int:
        """Width the icon grid can actually use.

        Do not trust a leftover QScrollArea viewport from a min-size HWND.
        First-visit fences now seed saved width before ``winId()``; this
        fallback still covers a resizeEvent that fires before layout catches up.
        Explorer sizes icon columns from the view's client width.
        """
        try:
            live = int(self.width())
        except RuntimeError:
            live = 0
        try:
            cfg_w = int((self.config or {}).get("width") or 0)
        except (TypeError, ValueError):
            cfg_w = 0
        min_w = int(self.MIN_WIDTH)
        # First page-switch HWND is created at MIN_WIDTH; saved/config width
        # is the real client. After the user resizes, live width wins.
        if live > min_w + 8:
            outer = live
        else:
            outer = max(live, min_w, cfg_w)
        scroll = getattr(self, "scroll", None)
        if scroll is not None:
            try:
                vp = int(scroll.viewport().width())
            except (RuntimeError, TypeError, ValueError):
                vp = 0
            # Laid-out viewport tracks the frame. A min-size leftover must not
            # win over the saved/client width.
            if vp >= 40 and vp >= outer - 64:
                return vp
        return max(80, outer - 28)

    @staticmethod
    def _fences_min_label_width(icon_size: int) -> int:
        """Fences-like caption shelf: wide enough for 2–4 wrapped lines, not max columns."""
        icon_size = max(16, int(icon_size))
        # ~96px at medium (48) icons; scales gently when zoomed.
        return max(96, icon_size + 28)

    @staticmethod
    def _fit_grid_columns(
        usable: int,
        icon_size: int,
        *,
        h_spacing: int = 4,
        min_label: int = 48,
    ) -> tuple[int, int]:
        """Return (columns, label_width) so every cell fits inside ``usable`` px."""
        usable = max(1, int(usable))
        icon_size = max(16, int(icon_size))
        h_spacing = max(0, int(h_spacing))
        min_label = max(int(min_label), FenceWidget._fences_min_label_width(icon_size))
        # Fences prefers fewer, wider cells over packing an extra skinny column.
        ideal_cell = max(icon_size + 20, min_label + 10)
        max_cols = max(1, (usable + h_spacing) // (ideal_cell + h_spacing))
        while max_cols >= 1:
            label_width = max(
                min_label,
                (usable - h_spacing * (max_cols - 1)) // max_cols - 10,
            )
            cell_w = max(icon_size + 20, label_width + 10)
            total = max_cols * cell_w + h_spacing * max(0, max_cols - 1)
            if total <= usable:
                return max_cols, label_width
            if max_cols == 1:
                # Shrink label so a single column stays within the viewport;
                # icon itself may still be larger than a tiny shelf.
                label_width = max(16, min(label_width, usable - 10))
                return 1, label_width
            max_cols -= 1
        return 1, max(16, min(min_label, usable - 10))

    def _view_mode(self) -> str:
        from src.i18n import normalize_view_mode

        return normalize_view_mode((self._style or {}).get("view_mode", "grid"))

    def _icon_layout_metrics(self) -> tuple[int, int, int]:
        """Return icon size, column count, and label width for the current fence size."""
        view_mode = self._view_mode()
        outer_w = max(self.width(), self.MIN_WIDTH)

        if view_mode == "list":
            return 0, 1, outer_w

        # Icon size is user-controlled (Alt+wheel / menu), not tied to fence geometry.
        icon_size = int(self.ICON_BASE_SIZE * self._icon_zoom)
        icon_size = max(self.ICON_MIN_SIZE, min(self.ICON_MAX_SIZE, icon_size))

        # Fit columns to the scroll viewport — using outer fence width packed one
        # extra column and clipped the last icon against the right edge.
        spacing = 4
        try:
            spacing = int(self.items_layout.horizontalSpacing())
            if spacing < 0:
                spacing = int(self.items_layout.spacing())
        except Exception:
            spacing = 4
        margins = self.items_layout.contentsMargins()
        gutter = margins.left() + margins.right()
        usable = max(80, self._available_items_width() - gutter)
        # Label at least as wide as the icon so caption centers under the glyph.
        max_cols, label_width = self._fit_grid_columns(
            usable,
            icon_size,
            h_spacing=max(0, spacing),
            min_label=self._fences_min_label_width(icon_size),
        )
        return icon_size, max_cols, label_width

    def _update_icon_scale(self, *, fast: bool = False) -> None:
        if self._collapsed:
            return

        view_mode = self._view_mode()
        if view_mode == "list":
            return

        icon_size, max_cols, label_width = self._icon_layout_metrics()
        if not fast:
            if icon_size == self._last_icon_size and max_cols == self._last_max_cols:
                return
        elif icon_size == self._last_icon_size:
            return

        if max_cols != self._last_max_cols:
            self._icon_scale_debounce.stop()
            self._icon_scale_settle.stop()
            self.refresh()
            return

        if not fast:
            self._last_icon_size = icon_size

        host = getattr(self, "items_widget", None)
        if host is not None:
            host.setUpdatesEnabled(False)
        try:
            for index in range(self.items_layout.count()):
                item = self.items_layout.itemAt(index)
                if not item or not item.widget():
                    continue
                widget = item.widget()
                if isinstance(widget, FenceIconItem):
                    widget.set_icon_size(icon_size, label_width, fast=fast)
        finally:
            if host is not None:
                host.setUpdatesEnabled(True)
                if fast:
                    host.update()
                else:
                    host.adjustSize()
                    host.update()

    def refresh(self, *, force: bool = False, shell_heal: bool = True) -> None:
        """Rebuild the icon grid. ``force`` bypasses signature skip and reloads icons.

        ``shell_heal=False`` skips deferred post-refresh shell attach — used by
        boot paint heal so force-refresh cannot re-enter force-attach forever.
        """
        if force:
            self._force_refresh_pending = True
            self._last_refresh_sig = None
            self._refresh_debounce.stop()
            self._refresh_impl()
            # Menu actions run while desktop-popup freeze is still armed; defer
            # one coalesced soft heal (not per-fence force attach).
            if shell_heal:
                app = QApplication.instance()
                desk = getattr(app, "_desktidy_app", None) if app is not None else None
                if desk is not None:
                    heal = getattr(desk, "_schedule_post_refresh_shell_heal", None)
                    if callable(heal):
                        heal()
            return
        self._refresh_debounce.start()

    def reload_icons(self) -> None:
        """Re-apply shell icons on existing items without rebuilding the grid."""
        if self._collapsed:
            return
        view_mode = self._view_mode()
        if view_mode == "list":
            return
        icon_size, _max_cols, label_width = self._icon_layout_metrics()
        for index in range(self.items_layout.count()):
            item = self.items_layout.itemAt(index)
            widget = item.widget() if item else None
            if isinstance(widget, FenceIconItem):
                widget.set_icon_size(icon_size, label_width)

    def flush_icon_grid_paint(self) -> None:
        """Sync-paint icon cells after Win32 batch SWP_SHOWWINDOW + SWP_NOREDRAW."""
        if self._collapsed:
            return
        try:
            self.repaint()
            hwnd = int(self.winId()) if self.winId() else 0
            if hwnd:
                from src.desktop_shell_host import redraw_overlay_hwnd

                redraw_overlay_hwnd(hwnd)
        except RuntimeError:
            pass

    def _refresh_impl(self) -> None:
        force = bool(getattr(self, "_force_refresh_pending", False))
        self._force_refresh_pending = False
        entries = self._get_entries()
        view_mode = self._view_mode()
        if view_mode == "list":
            max_cols = 1
            use_icons = False
            icon_size, label_width = 0, self.width()
        else:
            use_icons = True
            icon_size, max_cols, label_width = self._icon_layout_metrics()

        wanted_keys = tuple(self._entry_key(p) for p in entries[:60])
        refresh_sig = (wanted_keys, use_icons, icon_size, max_cols, label_width, view_mode)
        if not force and refresh_sig == getattr(self, "_last_refresh_sig", None):
            count = len(wanted_keys)
            self.footer_label.setText("" if count == 0 else f"共 {count} 项")
            self._update_chrome_visibility()
            return

        if force:
            from src.icon_utils import invalidate_file_icon_cache

            # Only drop icons for this fence's visible entries — global clear
            # forced every other fence to re-extract shell icons on UI thread.
            for entry in entries[:60]:
                invalidate_file_icon_cache(entry)

        existing: dict[str, QWidget] = {}
        leftovers: list[QWidget] = []
        while self.items_layout.count():
            child = self.items_layout.takeAt(0)
            widget = child.widget() if child else None
            if widget is None:
                continue
            # Force rebuild: discard old icon widgets so labels/icons reload.
            if force:
                leftovers.append(widget)
                continue
            if use_icons and isinstance(widget, FenceIconItem):
                existing[self._entry_key(widget.file_path)] = widget
            elif (not use_icons) and isinstance(widget, FenceItemLabel):
                existing[self._entry_key(widget.file_path)] = widget
            else:
                leftovers.append(widget)

        self._last_icon_size = icon_size
        self._last_max_cols = max_cols
        if use_icons and not force:
            new_entries = [
                entry
                for entry in entries
                if self._entry_key(entry) not in existing
            ]
            if new_entries:
                from src.icon_utils import prefetch_shell_icon_lookups

                prefetch_shell_icon_lookups(new_entries[:24])
        row, col, count = 0, 0, 0
        new_icon_stagger = 0
        skip_stagger = bool(getattr(self, "_desktidy_page_switch_refresh", False))
        virtual = self._is_virtual_mode()
        fence_id = str(self.config.get("id") or "")

        for entry in entries:
            key = self._entry_key(entry)
            item = existing.pop(key, None)
            if item is None:
                icon_delay = 0
                if use_icons and new_icon_stagger >= 5 and not skip_stagger:
                    icon_delay = (new_icon_stagger - 4) * 14
                if use_icons:
                    item = FenceIconItem(
                        entry,
                        show_label=True,
                        icon_size=icon_size,
                        label_max_width=label_width,
                        virtual_mode=virtual,
                        fence_id=fence_id,
                        icon_load_delay_ms=icon_delay,
                    )
                    new_icon_stagger += 1
                else:
                    item = FenceItemLabel(entry, virtual_mode=virtual, fence_id=fence_id)
                item.activated.connect(open_path)
                item.refresh_needed.connect(self.refresh)
                item.virtual_unpinned.connect(self._unpin_virtual_item)
                item.setAcceptDrops(True)
                item.installEventFilter(self)
            elif use_icons and isinstance(item, FenceIconItem):
                item.set_icon_size(icon_size, label_width)

            self.items_layout.addWidget(
                item, row, col, Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter
            )
            col += 1
            count += 1
            if col >= max_cols:
                col = 0
                row += 1
            if count >= 60:
                break

        for widget in list(existing.values()) + leftovers:
            widget.hide()
            widget.deleteLater()

        if count == 0:
            if self._collapsed:
                empty_text = "暂无文件"
            else:
                empty_text = "暂无文件\n拖入图标，或使用一键整理"
            empty = QLabel(empty_text)
            empty.setObjectName("fenceEmpty")
            empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.items_layout.addWidget(empty, 0, 0)
            self.footer_label.setText("")
        else:
            self.footer_label.setText(f"{count} 项")
        self._last_refresh_sig = refresh_sig
        self._update_chrome_visibility()

    def note_item_path_changed(self, old: Path, new: Path) -> None:
        """Keep the refresh signature in sync after an in-place rename."""
        sig = getattr(self, "_last_refresh_sig", None)
        if not isinstance(sig, tuple) or not sig:
            return
        keys = sig[0]
        if not isinstance(keys, tuple):
            return
        try:
            old_k = self._entry_key(old)
            new_k = self._entry_key(new)
        except Exception:
            return
        self._last_refresh_sig = (
            tuple(new_k if k == old_k else k for k in keys),
            *sig[1:],
        )

    def _sync_fence_virtual_items(self) -> None:
        fid = self.config.get("id")
        for fence in self.settings.get("fences", []):
            if fence.get("id") == fid:
                fence["virtual_items"] = list(self.config.get("virtual_items") or [])
                fence["sort_by"] = self.config.get("sort_by", fence.get("sort_by"))
                break

    def _remove_virtual_icon_widget(self, path: Path) -> bool:
        """Drop one grid cell without rebuilding the whole fence (avoids drag-out hitch)."""
        return self._remove_virtual_icon_widgets([Path(path)])

    def _remove_virtual_icon_widgets(self, paths: list[Path]) -> bool:
        """Drop many grid cells with a single compact (multi-drag out)."""
        if not paths:
            return False
        targets: set[str] = set()
        for path in paths:
            try:
                targets.add(self._entry_key(Path(path)))
            except Exception:
                targets.add(str(path).casefold())
        if not targets:
            return False
        keep: list[QWidget] = []
        removed = 0
        while self.items_layout.count():
            child = self.items_layout.takeAt(0)
            w = child.widget() if child else None
            if w is None:
                continue
            fp = getattr(w, "file_path", None)
            if fp is None:
                w.hide()
                w.deleteLater()
                continue
            try:
                key = self._entry_key(Path(fp))
            except Exception:
                key = str(fp).casefold()
            if key in targets:
                w.hide()
                w.deleteLater()
                removed += 1
                continue
            keep.append(w)
        if removed == 0:
            # Put widgets back if nothing matched (caller may refresh).
            view_mode = self._view_mode()
            max_cols = 1 if view_mode == "list" else self._icon_layout_metrics()[1]
            row = col = 0
            for w in keep:
                self.items_layout.addWidget(
                    w,
                    row,
                    col,
                    Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter,
                )
                col += 1
                if col >= max_cols:
                    col = 0
                    row += 1
            return False
        view_mode = self._view_mode()
        if view_mode == "list":
            max_cols = 1
            use_icons = False
            icon_size, label_width = 0, self.width()
        else:
            use_icons = True
            icon_size, max_cols, label_width = self._icon_layout_metrics()
        row = col = 0
        keys: list[str] = []
        for w in keep:
            self.items_layout.addWidget(
                w, row, col, Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter
            )
            fp = getattr(w, "file_path", None)
            if fp is not None:
                keys.append(self._entry_key(Path(fp)))
            col += 1
            if col >= max_cols:
                col = 0
                row += 1
        count = len(keys)
        if count == 0:
            empty = QLabel(
                "暂无匹配文件\n拖入桌面图标到此分区（不移动文件）"
                if not self._collapsed
                else "暂无文件"
            )
            empty.setObjectName("fenceEmpty")
            empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self.items_layout.addWidget(empty, 0, 0)
            self.footer_label.setText("")
        else:
            self.footer_label.setText(f"共 {count} 项")
        self._last_refresh_sig = (
            tuple(keys[:60]),
            use_icons,
            icon_size,
            max_cols,
            label_width,
            view_mode,
        )
        self._update_chrome_visibility()
        return True

    def _relayout_icons_to_virtual_order(self) -> bool:
        """Reorder existing icon widgets to match pin order — no rebuild / no flash.

        Used for same-fence drag reorder. Returns False when the live grid cannot
        cover the ordered set (caller should fall back to ``_refresh_impl``).
        """
        if self._collapsed:
            return False
        ordered = self._get_entries()
        if not ordered:
            return False
        by_key: dict[str, QWidget] = {}
        while self.items_layout.count():
            child = self.items_layout.takeAt(0)
            w = child.widget() if child else None
            if w is None:
                continue
            fp = getattr(w, "file_path", None)
            if fp is None:
                w.hide()
                w.deleteLater()
                continue
            try:
                key = self._entry_key(Path(fp))
            except Exception:
                key = str(fp).casefold()
            # Keep first widget for a key; drop duplicates / empty chrome.
            if key in by_key:
                w.hide()
                w.deleteLater()
            else:
                by_key[key] = w
        view_mode = self._view_mode()
        if view_mode == "list":
            max_cols = 1
            use_icons = False
            icon_size, label_width = 0, self.width()
        else:
            use_icons = True
            icon_size, max_cols, label_width = self._icon_layout_metrics()
        row = col = 0
        keys: list[str] = []
        missing = False
        for entry in ordered[:60]:
            try:
                key = self._entry_key(entry)
            except Exception:
                key = str(entry).casefold()
            w = by_key.pop(key, None)
            if w is None:
                missing = True
                continue
            self.items_layout.addWidget(
                w, row, col, Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignHCenter
            )
            keys.append(key)
            col += 1
            if col >= max_cols:
                col = 0
                row += 1
        for w in by_key.values():
            w.hide()
            w.deleteLater()
        if missing or not keys:
            # Incomplete grid — rebuild rather than leave holes.
            while self.items_layout.count():
                child = self.items_layout.takeAt(0)
                w = child.widget() if child else None
                if w is not None:
                    w.hide()
                    w.deleteLater()
            return False
        self.footer_label.setText(f"共 {len(keys)} 项")
        self._last_refresh_sig = (
            tuple(keys[:60]),
            use_icons,
            icon_size,
            max_cols,
            label_width,
            view_mode,
        )
        self._update_chrome_visibility()
        return True

    def _abort_item_interaction(self) -> None:
        """Drop marquee + selection so leftover chrome cannot eat clicks."""
        self._release_marquee_grab()
        self._marquee_origin = None
        band = getattr(self, "_marquee_band", None)
        if band is not None:
            try:
                band.hide()
            except RuntimeError:
                pass
        self.clear_item_selection()

    def _release_marquee_grab(self) -> None:
        """Release mouse grab taken for rubber-band selection."""
        try:
            if QWidget.mouseGrabber() is self:
                self.releaseMouse()
        except RuntimeError:
            pass

    def _unpin_virtual_item(self, path: Path, *, place_on_public: bool = True) -> None:
        # Defer: caller is often still inside mouseMove after QDrag.exec; rebuilding
        # icons synchronously deletes that widget and freezes/crashes the UI thread.
        self._unpin_virtual_paths([Path(path)], place_on_public=place_on_public)

    def _unpin_virtual_paths(
        self, paths: list[Path], *, place_on_public: bool = True
    ) -> None:
        """Batch unpin (multi-drag out) — one settings write, one grid compact."""
        batch = [Path(p) for p in paths if p is not None]
        if not batch:
            return
        QTimer.singleShot(
            0,
            lambda items=list(batch), pub=place_on_public: self._unpin_virtual_paths_impl(
                items, place_on_public=pub
            ),
        )

    def _unpin_virtual_paths_impl(
        self, paths: list[Path], *, place_on_public: bool = True
    ) -> None:
        # Portal fences mirror a real folder — never rewrite virtual_items / floats.
        if self._is_portal_mode():
            return
        from src.fence_rules import unpin_paths_from_virtual_fence

        paths = [Path(p) for p in paths]
        if not paths:
            return
        unpin_paths_from_virtual_fence(self.config, paths)
        self._sync_fence_virtual_items()

        if place_on_public:
            from src.public_desktop import (
                add_fence_unpin_public_item,
                live_fence_rects,
            )
            from src.ui.fence_icon_item import last_virtual_unpin_pos

            drop = last_virtual_unpin_pos()
            page_id = int(self.settings.get("current_page", 0))
            rects = live_fence_rects(self.settings, page_id)
            for path in paths:
                add_fence_unpin_public_item(
                    self.settings,
                    path,
                    page_id=page_id,
                    fence_rects=rects,
                    monitor_hint_x=int(drop.x()),
                    monitor_hint_y=int(drop.y()),
                )
            try:
                save_settings(self.settings)
            except OSError:
                pass
            if not self._remove_virtual_icon_widgets(paths):
                self.refresh()
            # Clear press/grab chrome left from the drag that triggered unpin.
            self._abort_item_interaction()
            for path in paths:
                self.public_items_changed.emit(Path(path))
            # Soft-add restacks the public host; re-assert this fence is clickable.
            self._ensure_drop_target_interactive()
            return

        try:
            save_settings(self.settings)
        except OSError:
            pass
        self._abort_item_interaction()
        if not self._remove_virtual_icon_widgets(paths):
            self.refresh()

    def _unpin_virtual_item_impl(
        self, path: Path, *, place_on_public: bool = True
    ) -> None:
        """Compatibility wrapper — prefer ``_unpin_virtual_paths_impl`` for batches."""
        self._unpin_virtual_paths_impl([Path(path)], place_on_public=place_on_public)

    def _insert_index_at(self, widget: QWidget, pos: QPoint) -> int | None:
        """Return insert index among icon items for a drop position in widget coords."""
        items: list[QWidget] = []
        for i in range(self.items_layout.count()):
            child = self.items_layout.itemAt(i)
            w = child.widget() if child else None
            if isinstance(w, (FenceIconItem, FenceItemLabel)):
                items.append(w)
        if not items:
            return 0
        # Map to items_widget coordinates. Prefer global mapping — after shell
        # reparent, mapFrom(widget, …) can warn "parent must be in hierarchy".
        if widget is self.items_widget:
            local = pos
        else:
            try:
                local = self.items_widget.mapFromGlobal(widget.mapToGlobal(pos))
            except RuntimeError:
                local = self.items_widget.mapFrom(widget, pos)
        for idx, w in enumerate(items):
            geo = w.geometry()
            if local.y() < geo.center().y() or (
                local.y() <= geo.bottom() and local.x() < geo.center().x()
            ):
                if geo.contains(local) or local.y() <= geo.bottom():
                    # Drop on/before this cell.
                    if geo.contains(local) and local.x() >= geo.center().x():
                        return idx + 1
                    return idx
        return len(items)

    def _icon_item_widgets(self) -> list[QWidget]:
        items: list[QWidget] = []
        for i in range(self.items_layout.count()):
            child = self.items_layout.itemAt(i)
            w = child.widget() if child else None
            if isinstance(w, (FenceIconItem, FenceItemLabel)):
                items.append(w)
        return items

    def clear_item_selection(self) -> None:
        items = self._icon_item_widgets()
        if not items:
            self._selection_anchor = None
            return
        with fence_selection_batch(items[0]):
            for item in items:
                setter = getattr(item, "set_selected", None)
                if callable(setter):
                    setter(False)
        self._selection_anchor = None
        try:
            from src.ui.fence_icon_item import mark_desktop_selection_live

            mark_desktop_selection_live()
        except Exception:
            pass

    def keyPressEvent(self, event) -> None:
        # When focus is on fence chrome/empty area, still support Select-All / Paste.
        from src.ui.fence_icon_item import (
            dispatch_desktop_icon_chord,
            ensure_desktop_icon_key_hook,
        )

        ensure_desktop_icon_key_hook()
        key = event.key()
        mods = event.modifiers()
        ctrl = bool(mods & Qt.KeyboardModifier.ControlModifier)
        if ctrl and key == Qt.Key.Key_A:
            if dispatch_desktop_icon_chord("select_all", self):
                event.accept()
                return
        if ctrl and key == Qt.Key.Key_C:
            if dispatch_desktop_icon_chord("copy", self):
                event.accept()
                return
        if ctrl and key == Qt.Key.Key_X:
            if dispatch_desktop_icon_chord("cut", self):
                event.accept()
                return
        if ctrl and key == Qt.Key.Key_V:
            if dispatch_desktop_icon_chord("paste", self):
                event.accept()
                return
        if key == Qt.Key.Key_Delete and not ctrl:
            anchor = self._selection_anchor
            if anchor is None:
                selected = [
                    w
                    for w in self._icon_item_widgets()
                    if getattr(w, "is_selected", lambda: False)()
                ]
                anchor = selected[0] if selected else None
            if anchor is not None and dispatch_desktop_icon_chord("delete", anchor):
                event.accept()
                return
        super().keyPressEvent(event)

    def _map_pos_to_items_widget(self, obj: QObject, pos: QPoint) -> QPoint | None:
        items = getattr(self, "items_widget", None)
        if items is None:
            return None
        if obj is items:
            return QPoint(pos)
        if isinstance(obj, QWidget):
            try:
                return items.mapFrom(obj, pos)
            except Exception:
                return None
        return None

    def _marquee_host_objects(self) -> set[object]:
        hosts: set[object] = set()
        items = getattr(self, "items_widget", None)
        body = getattr(self, "body_widget", None)
        scroll = getattr(self, "scroll", None)
        if items is not None:
            hosts.add(items)
        if body is not None:
            hosts.add(body)
        if scroll is not None:
            hosts.add(scroll)
            try:
                hosts.add(scroll.viewport())
            except Exception:
                pass
        return hosts

    def _begin_marquee(
        self, obj: QObject, pos: QPoint, *, additive: bool
    ) -> bool:
        origin = self._map_pos_to_items_widget(obj, pos)
        if origin is None:
            return False
        band = getattr(self, "_marquee_band", None)
        items = getattr(self, "items_widget", None)
        if band is None or items is None:
            return False
        self._panel_press_pending = None
        self._drag_pos = None
        self._marquee_origin = QPoint(origin)
        self._marquee_additive = bool(additive)
        if not additive:
            self.clear_item_selection()
        band.setGeometry(QRect(origin, origin))
        band.show()
        band.raise_()
        # Keep MouseMove/Release after the cursor leaves the click mask / fence.
        try:
            self.grabMouse()
        except RuntimeError:
            pass
        return True

    def _update_marquee(self, obj: QObject, pos: QPoint) -> bool:
        origin = getattr(self, "_marquee_origin", None)
        band = getattr(self, "_marquee_band", None)
        if origin is None or band is None:
            return False
        current = self._map_pos_to_items_widget(obj, pos)
        if current is None:
            return False
        rect = QRect(origin, current).normalized()
        band.setGeometry(rect)
        band.show()
        additive = bool(getattr(self, "_marquee_additive", False))
        items = self._icon_item_widgets()
        anchor = items[0] if items else None
        with fence_selection_batch(anchor):
            if not additive:
                for item in items:
                    setter = getattr(item, "set_selected", None)
                    if callable(setter):
                        setter(False)
            last_hit: QWidget | None = None
            for item in items:
                if not rect.intersects(item.geometry()):
                    continue
                setter = getattr(item, "set_selected", None)
                if callable(setter):
                    setter(True)
                last_hit = item
        if last_hit is not None:
            self._selection_anchor = last_hit
            try:
                from src.ui.fence_icon_item import mark_desktop_selection_live

                mark_desktop_selection_live(True)
            except Exception:
                pass
        return True

    def _finish_marquee(self, obj: QObject, pos: QPoint) -> bool:
        origin = getattr(self, "_marquee_origin", None)
        band = getattr(self, "_marquee_band", None)
        if origin is None:
            return False
        current = self._map_pos_to_items_widget(obj, pos) or QPoint(origin)
        moved = (QPoint(current) - QPoint(origin)).manhattanLength()
        additive = bool(getattr(self, "_marquee_additive", False))
        if moved < 6 and not additive:
            self._marquee_origin = None
            if band is not None:
                band.hide()
            self.clear_item_selection()
            self._release_marquee_grab()
            return True
        self._update_marquee(obj, current)
        self._marquee_origin = None
        if band is not None:
            band.hide()
        self._release_marquee_grab()
        try:
            from src.ui.fence_icon_item import mark_desktop_selection_live

            mark_desktop_selection_live()
        except Exception:
            pass
        return True

    def _hide_drop_indicator(self) -> None:
        indicator = getattr(self, "_drop_indicator", None)
        if indicator is not None:
            indicator.hide()

    def _update_drop_indicator(
        self, source_widget: QWidget | None, pos: QPoint
    ) -> None:
        """Show a caret between icons so the user sees before/after placement."""
        if not self._is_virtual_mode():
            self._hide_drop_indicator()
            return
        indicator = getattr(self, "_drop_indicator", None)
        if indicator is None:
            return
        insert_at = self._insert_index_at(
            source_widget if isinstance(source_widget, QWidget) else self.items_widget,
            pos,
        )
        if insert_at is None:
            self._hide_drop_indicator()
            return

        items = self._icon_item_widgets()
        list_mode = self._view_mode() == "list"
        thickness = 3

        if not items:
            indicator.setGeometry(4, 4, thickness if not list_mode else 48, 48 if not list_mode else thickness)
            indicator.show()
            indicator.raise_()
            return

        if insert_at <= 0:
            geo = items[0].geometry()
            if list_mode:
                indicator.setGeometry(geo.left(), max(0, geo.top() - 2), max(24, geo.width()), thickness)
            else:
                indicator.setGeometry(max(0, geo.left() - 2), geo.top(), thickness, geo.height())
        elif insert_at >= len(items):
            geo = items[-1].geometry()
            if list_mode:
                indicator.setGeometry(geo.left(), geo.bottom() - 1, max(24, geo.width()), thickness)
            else:
                indicator.setGeometry(geo.right() - 1, geo.top(), thickness, geo.height())
        else:
            geo = items[insert_at].geometry()
            prev = items[insert_at - 1].geometry()
            # Same row → vertical bar between prev and current; new row → bar at left of current.
            same_row = abs(geo.center().y() - prev.center().y()) < max(8, geo.height() // 3)
            if list_mode or not same_row:
                if list_mode:
                    y = (prev.bottom() + geo.top()) // 2 - thickness // 2
                    indicator.setGeometry(geo.left(), y, max(24, geo.width()), thickness)
                else:
                    indicator.setGeometry(max(0, geo.left() - 2), geo.top(), thickness, geo.height())
            else:
                x = (prev.right() + geo.left()) // 2 - thickness // 2
                indicator.setGeometry(x, geo.top(), thickness, geo.height())

        indicator.show()
        indicator.raise_()

    def _reorder_virtual_items(self, paths: list[Path], insert_at: int | None) -> None:
        current = self._visible_entry_paths() or list(self._get_entries())
        moving_keys = set()
        for p in paths:
            try:
                moving_keys.add(str(p).casefold())
            except OSError:
                moving_keys.add(str(p).casefold())

        remaining: list[Path] = []
        for p in current:
            try:
                key = str(p).casefold()
            except OSError:
                key = str(p).casefold()
            if key not in moving_keys:
                remaining.append(p)

        idx = len(remaining) if insert_at is None else max(0, min(insert_at, len(remaining)))
        # Adjust insert index: _insert_index_at uses full list including moving item.
        # Recompute against remaining by counting how many non-moving were before insert_at.
        if insert_at is not None:
            before = 0
            for i, p in enumerate(current):
                if i >= insert_at:
                    break
                try:
                    key = str(p).casefold()
                except OSError:
                    key = str(p).casefold()
                if key not in moving_keys:
                    before += 1
            idx = before

        ordered = remaining[:idx] + list(paths) + remaining[idx:]
        set_virtual_item_order(self.config, ordered)
        self.sort_by = "manual"
        self._sync_fence_virtual_items()

    def _move_dropped_files(self, urls: list[QUrl]) -> None:
        # Kept for compatibility; prefer import_drop_to_folder via dropEvent.
        from PyQt6.QtCore import QMimeData

        mime = QMimeData()
        mime.setUrls(urls)
        self._import_dropped_mime(mime)

    def _visible_entry_paths(self) -> list[Path]:
        """Paths currently shown in the fence grid (no desktop re-scan)."""
        paths: list[Path] = []
        for index in range(self.items_layout.count()):
            item = self.items_layout.itemAt(index)
            widget = item.widget() if item else None
            if isinstance(widget, (FenceIconItem, FenceItemLabel)):
                paths.append(widget.file_path)
        return paths

    def _import_virtual_drop(self, mime, insert_at: int | None = None) -> bool:
        """Accept a virtual drop; apply pin/reorder after OLE returns.

        Doing assign/sync inside ``dropEvent`` / ``QDrag.exec`` can deadlock the
        desktop-band overlay (drag begins, never ends — public icon stays hidden).
        Paths are copied out immediately because ``QMimeData`` dies with the event.
        """
        paths = collect_drop_paths(mime)
        if not paths:
            return False
        source_id = desktidy_source_fence_id(mime) or ""
        path_list = [Path(p) for p in paths]
        insert = insert_at
        QTimer.singleShot(
            0,
            lambda: self._apply_virtual_drop_paths(
                path_list, source_id=source_id, insert_at=insert
            ),
        )
        return True

    def _apply_virtual_drop_paths(
        self,
        paths: list[Path],
        *,
        source_id: str = "",
        insert_at: int | None = None,
        quiet_finish: bool = False,
    ) -> bool:
        """Pin / reorder paths now that we are outside QDrag.exec / dropEvent."""
        if not paths:
            return False
        from src.ui.fence_icon_item import PUBLIC_SOURCE_FENCE_ID

        self_id = str(self.config.get("id") or "")
        from_public = source_id == PUBLIC_SOURCE_FENCE_ID
        try:
            if source_id and source_id == self_id:
                self._reorder_virtual_items(paths, insert_at)
                # Same-fence reorder: move existing cells only. Full refresh +
                # broadcasting to other fences / public layer flashes desktop.
                if not self._relayout_icons_to_virtual_order():
                    self._last_refresh_sig = None
                    try:
                        self._refresh_debounce.stop()
                        self._refresh_impl()
                    except RuntimeError:
                        return False
                try:
                    save_settings(self.settings)
                except OSError:
                    pass
                return True
            else:
                # Cross-fence / public / Explorer drop: membership is pin-only.
                # Always switch to manual so restart cannot re-absorb via old rule scan.
                # Seed from the full pin list — painted widgets alone can be a
                # partial subset and would wipe the rest of the fence.
                if self.config.get("sort_by") != "manual":
                    current = [
                        Path(str(p))
                        for p in (self.config.get("virtual_items") or [])
                    ]
                    if not current:
                        current = self._visible_entry_paths()
                    set_virtual_item_order(self.config, current)
                added = assign_paths_to_virtual_fence(
                    self.config, self.settings, paths, insert_at=insert_at
                )
                if not added:
                    # Exact pin keys only — basename would false-success when
                    # another folder's same-named file is already in the fence.
                    from src.fence_rules import _norm_virtual_key

                    def _vk(raw: Path | str) -> str:
                        try:
                            return (
                                _norm_virtual_key(Path(raw))
                                .casefold()
                                .replace("/", "\\")
                            )
                        except OSError:
                            return str(raw).casefold().replace("/", "\\")

                    already = {
                        _vk(str(x))
                        for x in (self.config.get("virtual_items") or [])
                    }
                    if not any(_vk(p) in already for p in paths):
                        return False
                self.config["sort_by"] = "manual"
                self.sort_by = "manual"
                self._sync_fence_virtual_items()
                self._sync_all_fence_configs_from_settings()
            self._finish_virtual_drop_ui(from_public=from_public, quiet=quiet_finish)
            # Quiet paste skips public refresh; still scrub live floats/peer cells
            # whose settings membership already moved here (Explorer cut-paste UX).
            try:
                from src.ui.fence_icon_item import scrub_live_icons_for_claimed_paths

                scrub_live_icons_for_claimed_paths([Path(p) for p in paths])
            except Exception:
                pass
            return True
        except RuntimeError:
            return False
        except Exception:
            try:
                from src.app_logging import get_logger

                get_logger().exception("virtual drop apply failed")
            except Exception:
                pass
            return False

    def _sync_all_fence_configs_from_settings(self) -> None:
        """Rebind live FenceWidget configs to the settings-owned fence dicts."""
        by_id = {
            f.get("id"): f
            for f in (self.settings.get("fences") or [])
            if isinstance(f, dict) and f.get("id")
        }
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app else None
        live = list(getattr(desk, "fences", []) or []) if desk is not None else []
        for widget in live:
            cfg = getattr(widget, "config", None)
            if not isinstance(cfg, dict):
                continue
            stored = by_id.get(cfg.get("id"))
            if stored is None:
                continue
            # Prefer the settings object as the single source of truth.
            widget.config = stored
            if stored.get("sort_by"):
                try:
                    widget.sort_by = stored.get("sort_by")
                except Exception:
                    pass

    def _finish_virtual_drop_ui(
        self, *, from_public: bool, quiet: bool = False
    ) -> None:
        try:
            save_settings(self.settings)
        except OSError:
            pass
        self._sync_all_fence_configs_from_settings()
        if quiet:
            # Membership may have changed; never reuse a stale signature. Still
            # skip peer/public broadcast (that is what quiet means).
            self._last_refresh_sig = None
            try:
                self._refresh_debounce.stop()
                self._refresh_impl()
            except RuntimeError:
                return
            self._ensure_drop_target_interactive()
            return
        self._last_refresh_sig = None
        try:
            self._refresh_debounce.stop()
            self._refresh_impl()
        except RuntimeError:
            return
        # Peers may have lost pins (cross-fence move). Refresh them once —
        # do NOT emit files_changed (that re-refreshed *this* fence + public hitch).
        self._soft_refresh_peer_fences()
        if not from_public:
            app = QApplication.instance()
            desk = getattr(app, "_desktidy_app", None) if app is not None else None
            refresh = getattr(desk, "refresh_public_desktop", None) if desk else None
            if callable(refresh):
                try:
                    refresh(relayout=False)
                except Exception:
                    pass
        # Explorer→fence / public refresh can restack pet/host above this fence
        # so new icons paint but cannot be selected — same owner as page switch.
        self._ensure_drop_target_interactive()

    def _ensure_drop_target_interactive(self) -> None:
        """After virtual drop UI: fences opaque + above pet/public host."""
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        ensure = getattr(desk, "ensure_live_fences_interactive", None) if desk else None
        if callable(ensure):
            try:
                ensure()
            except Exception:
                pass

    def _soft_refresh_peer_fences(self) -> None:
        """Debounced refresh of other live fences after pins moved here."""
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        live = list(getattr(desk, "fences", []) or []) if desk is not None else []
        self_id = str(self.config.get("id") or "")
        for widget in live:
            if widget is self:
                continue
            cfg = getattr(widget, "config", None)
            if not isinstance(cfg, dict):
                continue
            if self_id and str(cfg.get("id") or "") == self_id:
                continue
            try:
                widget.refresh()
            except RuntimeError:
                pass

    def _import_dropped_mime(self, mime) -> None:
        if self._is_virtual_mode():
            self._import_virtual_drop(mime)
            return
        target = self._target_folder()
        desktop = get_desktop_path()
        imported = import_drop_to_folder(mime, target, desktop)
        if imported:
            self.refresh()
            self.files_changed.emit()

    def _accept_file_drop(self, event) -> None:
        if not mime_has_droppable_items(event.mimeData()):
            return
        if self._is_virtual_mode():
            # Virtual: copy-link semantics (file stays on desktop).
            event.setDropAction(Qt.DropAction.CopyAction)
            event.accept()
            return
        action = preferred_drop_action_for_mime(
            event.mimeData(), target_dir=self._target_folder()
        )
        event.setDropAction(action)
        event.accept()

    def dragEnterEvent(self, event) -> None:
        self._accept_file_drop(event)
        if event.isAccepted() and self._is_virtual_mode():
            self._update_drop_indicator(self, event.position().toPoint())

    def dragMoveEvent(self, event) -> None:
        self._accept_file_drop(event)
        if event.isAccepted() and self._is_virtual_mode():
            self._update_drop_indicator(self, event.position().toPoint())

    def dragLeaveEvent(self, event) -> None:
        self._hide_drop_indicator()
        super().dragLeaveEvent(event)

    def dropEvent(self, event) -> None:
        mime = event.mimeData()
        self._hide_drop_indicator()
        if not mime_has_droppable_items(mime):
            return
        if self._is_virtual_mode():
            from src.ui.fence_icon_item import PUBLIC_SOURCE_FENCE_ID

            src_id = desktidy_source_fence_id(mime) or ""
            if src_id == PUBLIC_SOURCE_FENCE_ID:
                # Sync pin after QDrag.exec owns public→fence mutation.
                event.setDropAction(Qt.DropAction.CopyAction)
                event.accept()
                return
            insert_at = self._insert_index_at(self, event.position().toPoint())
            if self._import_virtual_drop(mime, insert_at=insert_at):
                event.setDropAction(Qt.DropAction.CopyAction)
                event.accept()
            return
        action = preferred_drop_action_for_mime(mime, target_dir=self._target_folder())
        self._import_dropped_mime(mime)
        event.setDropAction(action)
        event.accept()

    def _set_sort_by(self, sort_by: str) -> None:
        if self.sort_by == sort_by:
            self.refresh()
            return
        self.sort_by = sort_by
        self.config["sort_by"] = sort_by
        if sort_by == "manual":
            # Snapshot current visible order into virtual_items / keep pins.
            if self._is_virtual_mode():
                set_virtual_item_order(self.config, list(self._get_entries()))
                self._sync_fence_virtual_items()
                save_settings(self.settings)
        self.refresh()
        self.geometry_changed.emit()

    def contextMenuEvent(self, event) -> None:
        self._show_fence_context_menu(event.globalPos())
        event.accept()

    def _shell_background_folder(self) -> Path:
        """Folder whose Explorer background menu New/Paste should target."""
        if self._is_portal_mode():
            return self._target_folder()
        return get_desktop_path()

    def _desktop_child_keys(self) -> set[str]:
        desk = get_desktop_path()
        keys: set[str] = set()
        try:
            for child in desk.iterdir():
                # Avoid Path.resolve() — network / reparse points stall the UI.
                try:
                    keys.add(str(child).casefold())
                except OSError:
                    continue
        except OSError:
            pass
        return keys

    def _pin_new_desktop_children(self, before: set[str]) -> None:
        """After shell New/Paste on a virtual fence, pin newly landed desktop files."""
        if not self._is_virtual_mode():
            return
        if not isinstance(before, set):
            return
        desk = get_desktop_path()
        landed: list[Path] = []
        try:
            for child in desk.iterdir():
                try:
                    key = str(child).casefold()
                except OSError:
                    continue
                if key not in before:
                    landed.append(child)
        except OSError:
            return
        if not landed:
            return
        assign_paths_to_virtual_fence(self.config, self.settings, landed)
        try:
            save_settings(self.settings)
        except OSError:
            pass
        self.refresh(force=True)
        self.files_changed.emit()

    def _schedule_pin_new_desktop_children(self, before: set[str]) -> None:
        """Shell New/Paste often finishes after InvokeCommand — retry briefly."""
        token = int(getattr(self, "_pin_new_token", 0) or 0) + 1
        self._pin_new_token = token
        # Keep a strong ref so the set isn't GC'd before delayed slots run.
        self._pin_new_before = set(before)

        def _try(t: int = token) -> None:
            if int(getattr(self, "_pin_new_token", 0) or 0) != t:
                return
            snap = getattr(self, "_pin_new_before", None)
            if isinstance(snap, set):
                self._pin_new_desktop_children(snap)

        for delay in (350, 900, 1800):
            QTimer.singleShot(int(delay), _try)

    def _fence_shell_extra_commands(self):
        """DeskTidy actions prepended above the Explorer blank-area menu."""
        from src.shell_file_menu import ShellMenuCommand
        from src.system_defaults import is_locked_fence

        name = str(self.config.get("name") or "分区")
        locked = is_locked_fence(self.config)
        return [
            ShellMenuCommand(
                f"解散分区：{name}",
                lambda: self.dissolve_requested.emit(),
                enabled=not locked,
            )
        ]

    def _show_fence_context_menu(self, global_pos: QPoint) -> None:
        """Zone blank RMB: Explorer folder background plus minimal DeskTidy verbs."""
        from src.shell_file_menu import show_folder_background_menu

        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        begin = getattr(desk, "_begin_desktop_popup", None) if desk is not None else None
        end_later = (
            getattr(desk, "_end_desktop_popup_later", None) if desk is not None else None
        )
        if callable(begin):
            begin()
        snap = getattr(desk, "_snapshot_explorer_defview_host", None) if desk else None
        if callable(snap):
            snap()

        folder = self._shell_background_folder()
        shell_state: dict[str, object] = {"invoked": False, "before": None}

        def _before_shell_invoke() -> None:
            shell_state["invoked"] = True
            if self._is_virtual_mode():
                # Snapshot immediately before InvokeCommand — not before menu open —
                # so unrelated desktop creates during the menu don't get pinned.
                shell_state["before"] = self._desktop_child_keys()

        try:
            try:
                hwnd = int(self.winId())
            except Exception:
                hwnd = 0
            shown = show_folder_background_menu(
                folder,
                x=int(global_pos.x()),
                y=int(global_pos.y()),
                hwnd=hwnd,
                extra_commands=self._fence_shell_extra_commands(),
                on_before_shell_invoke=_before_shell_invoke,
            )
        finally:
            if callable(end_later):
                end_later(250)
            elif desk is not None:
                end = getattr(desk, "_end_desktop_popup", None)
                if callable(end):
                    end()

        if not shown:
            self._ensure_alive_after_shell_menu()
            return

        if not shell_state.get("invoked"):
            self._ensure_alive_after_shell_menu()
            return

        if self._is_portal_mode():
            self.refresh(force=True)
            self.files_changed.emit()
            self._ensure_alive_after_shell_menu()
            return

        before = shell_state.get("before")
        if isinstance(before, set):
            self._schedule_pin_new_desktop_children(before)
        self._ensure_alive_after_shell_menu()
        if shell_state.get("invoked") and desk is not None:
            recover = getattr(desk, "_recover_overlays_after_desktop_shell_menu", None)
            if callable(recover):
                QTimer.singleShot(350, recover)

    def _ensure_alive_after_shell_menu(self) -> None:
        """Recover if shell-menu host DestroyWindow / HideWindow hit this overlay.

        Qt ``isVisible`` can stay True while Win32 is SW_HIDE'd after the first
        TrackPopupMenu — check native visibility and remount.
        """
        try:
            import win32gui
        except Exception:
            win32gui = None  # type: ignore[assignment]
        try:
            hwnd = int(self.winId())
        except Exception:
            hwnd = 0
        alive = False
        if hwnd and win32gui is not None:
            try:
                alive = bool(win32gui.IsWindow(hwnd))
            except Exception:
                alive = False
        win32_on = False
        if alive:
            try:
                from src.win_shell import overlay_win32_visible

                win32_on = bool(overlay_win32_visible(self))
            except Exception:
                win32_on = False
        if alive and self.isVisible() and win32_on:
            pass
        else:
            try:
                if not self.isVisible():
                    self.show()
                from src.desktop_shell_host import (
                    is_attached_to_desktop,
                    set_overlay_hwnd_visible,
                )
                from src.win_shell import configure_desktop_overlay

                if hwnd and is_attached_to_desktop(hwnd):
                    set_overlay_hwnd_visible(hwnd, True)
                else:
                    configure_desktop_overlay(self, peek=False)
            except Exception:
                pass
        # Sibling fences can share the same Win32 hide; heal the whole page.
        try:
            app = QApplication.instance()
            desk = getattr(app, "_desktidy_app", None) if app is not None else None
            heal = getattr(desk, "_heal_overlays_after_shell_menu", None)
            if callable(heal):
                heal()
        except Exception:
            pass

    def _host_system_icon(self, clsid: str, name: str) -> None:
        if self._is_virtual_mode() or self._is_portal_mode():
            return
        dest = move_namespace_icon_to_folder(self._target_folder(), f"::{clsid}", name)
        if dest is not None:
            self.refresh()
            self.files_changed.emit()

    def _open_portal_folder(self) -> None:
        portal = get_portal_path(self.config)
        if portal is None:
            return
        try:
            from src.win_shell import open_path

            open_path(portal)
        except Exception:
            pass

    def _pick_portal_folder(self) -> None:
        from PyQt6.QtWidgets import QFileDialog

        from src.fence_rules import set_portal_path
        from src.settings import save_settings

        start = get_portal_path(self.config)
        start_dir = str(start) if start is not None else str(Path.home())
        chosen = QFileDialog.getExistingDirectory(self, "选择要镜像的文件夹", start_dir)
        if not chosen:
            return
        set_portal_path(self.config, chosen)
        # Sync into settings-owned fence dict.
        fid = self.config.get("id")
        for fence in self.settings.get("fences") or []:
            if isinstance(fence, dict) and fence.get("id") == fid:
                set_portal_path(fence, chosen)
                break
        try:
            save_settings(self.settings)
        except OSError:
            pass
        self._last_refresh_sig = None
        self.refresh(force=True)
        self.files_changed.emit()
        # Ask app to reschedule portal watchers.
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        restart = getattr(desk, "_start_watcher", None) if desk is not None else None
        if callable(restart):
            try:
                restart()
            except Exception:
                pass

    def _clear_portal_folder(self) -> None:
        from src.fence_rules import set_portal_path
        from src.settings import save_settings

        set_portal_path(self.config, None)
        fid = self.config.get("id")
        for fence in self.settings.get("fences") or []:
            if isinstance(fence, dict) and fence.get("id") == fid:
                set_portal_path(fence, None)
                break
        try:
            save_settings(self.settings)
        except OSError:
            pass
        self._last_refresh_sig = None
        self.refresh(force=True)
        self.files_changed.emit()
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        restart = getattr(desk, "_start_watcher", None) if desk is not None else None
        if callable(restart):
            try:
                restart()
            except Exception:
                pass

    def _hit_test_resize(self, pos: QPoint) -> str | None:
        if self._collapsed or self.is_position_locked():
            return None
        # Collapse / lock own their pixels — a MIN_WIDTH fence puts
        # chips on the east/north strip, which used to steal hover and clicks.
        if self._pos_on_header_chrome(pos):
            return None
        x, y = pos.x(), pos.y()
        w, h = self.width(), self.height()
        m = self.RESIZE_MARGIN
        on_right = x >= w - m
        on_bottom = y >= h - m
        on_left = x <= m
        on_top = y <= m
        if on_bottom and on_right:
            return "se"
        if on_bottom:
            return "s"
        if on_right:
            return "e"
        if on_left:
            return "w"
        if on_top:
            return "n"
        return None

    def _update_cursor(self, pos: QPoint) -> None:
        mode = self._hit_test_resize(pos)
        # Top-row icons sit under the hover title overlay — never show resize
        # arrows while the pointer is on an icon cell.
        if mode is not None:
            try:
                walk = self.childAt(pos)
                while walk is not None and walk is not self:
                    if isinstance(walk, (FenceIconItem, FenceItemLabel)):
                        mode = None
                        break
                    walk = walk.parentWidget()
                if mode == "n":
                    under = QApplication.widgetAt(self.mapToGlobal(pos))
                    walk = under
                    while walk is not None:
                        if isinstance(walk, (FenceIconItem, FenceItemLabel)):
                            mode = None
                            break
                        if walk is self:
                            break
                        walk = walk.parentWidget()
            except RuntimeError:
                pass
        if mode == getattr(self, "_last_cursor_mode", object()):
            return
        self._last_cursor_mode = mode
        cursors = {
            "se": Qt.CursorShape.SizeFDiagCursor,
            "s": Qt.CursorShape.SizeVerCursor,
            "e": Qt.CursorShape.SizeHorCursor,
            "n": Qt.CursorShape.SizeVerCursor,
            "w": Qt.CursorShape.SizeHorCursor,
        }
        self.setCursor(QCursor(cursors.get(mode, Qt.CursorShape.ArrowCursor)))

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            pos = event.position().toPoint()
            chrome = self._chrome_button_at_pos(pos)
            if chrome is not None:
                self._chrome_btn_press = chrome
                self._drag_pos = None
                self._resize_mode = None
                event.accept()
                return
            self._resize_mode = self._hit_test_resize(pos)
            if self._resize_mode:
                self._resize_start_geo = self.geometry()
                self._resize_start_pos = event.globalPosition().toPoint()
            elif self.is_position_locked():
                self._drag_pos = None
                self.clear_item_selection()
            else:
                self._drag_pos = event.globalPosition().toPoint() - self.frameGeometry().topLeft()
                # Clicking empty chrome/panel clears icon selection.
                self.clear_item_selection()
            event.accept()

    def _drop_filter_targets(self) -> tuple:
        """Widgets that forward file drops; safe during partial UI construction."""
        targets: list = []
        container = getattr(self, "container", None)
        header = getattr(self, "header_widget", None)
        title = getattr(self, "title_label", None)
        left = getattr(self, "left_slot", None)
        right = getattr(self, "right_slot", None)
        body = getattr(self, "body_widget", None)
        scroll = getattr(self, "scroll", None)
        items = getattr(self, "items_widget", None)
        if container is not None:
            targets.append(container)
        if header is not None:
            targets.append(header)
        if title is not None:
            targets.append(title)
        if left is not None:
            targets.append(left)
        if right is not None:
            targets.append(right)
        if body is not None:
            targets.append(body)
        if scroll is not None:
            targets.append(scroll)
            try:
                targets.append(scroll.viewport())
            except Exception:
                pass
        if items is not None:
            targets.append(items)
        return tuple(targets)

    def _owns_drop_object(self, obj: QObject | None) -> bool:
        """True only for widgets that belong to THIS fence.

        Each FenceWidget installs an app-level event filter. Without this guard,
        every fence would treat any FenceIconItem as its own drop target — so
        dragging onto another fence's icon was stolen by the source fence and
        looked like cross-fence drag "did nothing".
        """
        if obj is None:
            return False
        if obj is self:
            return True
        if obj in self._drop_filter_targets():
            return True
        if isinstance(obj, (FenceIconItem, FenceItemLabel)):
            own_id = str(self.config.get("id") or "")
            obj_id = str(getattr(obj, "_fence_id", "") or "")
            if own_id and obj_id:
                return own_id == obj_id
            try:
                return self.isAncestorOf(obj)  # type: ignore[arg-type]
            except Exception:
                return False
        # Nested chrome inside this fence (scroll children etc.)
        if isinstance(obj, QWidget):
            try:
                return self.isAncestorOf(obj)
            except Exception:
                return False
        return False

    def _apply_zoom_wheel_delta(self, dy: int) -> bool:
        """Apply accumulated wheel delta as icon zoom. Shared by Qt + LL paths."""
        if self._view_mode() == "list":
            return False
        if dy == 0:
            return False
        notches, self._zoom_wheel_remain = _zoom_notches_from_delta(
            self._zoom_wheel_remain, dy
        )
        if notches == 0:
            return True
        self._set_icon_zoom(self._icon_zoom + 0.15 * notches)
        return True

    def _handle_zoom_wheel(self, event) -> bool:
        """Alt/Ctrl + wheel zooms icons for this fence (Qt event path)."""
        if event.type() != QEvent.Type.Wheel:
            return False
        if not _zoom_modifier_held(event):
            return False
        dy = _wheel_delta_y(event)
        if dy == 0:
            return False
        if not self._apply_zoom_wheel_delta(dy):
            return False
        event.accept()
        return True
    def eventFilter(self, obj: QObject, event: QEvent) -> bool:
        # Resize handles must remain clickable even when a child (scroll
        # viewport / icon widget) is covering the corner. We detect resize
        # margin presses here and divert the gesture onto this fence.
        try:
            # Title-bar chips win over drag / resize / marquee — including when
            # layered alpha click-through delivers the press to body/icons.
            if self._dispatch_chrome_button_mouse(obj, event):
                return True

            header_buttons = {
                getattr(self, "collapse_btn", None),
                getattr(self, "lock_btn", None),
            }
            if obj in header_buttons:
                return super().eventFilter(obj, event)

            # Title / empty chrome drag — never steal clicks that land on the
            # right-side buttons (or their direct chrome children).
            header_drag_targets = {
                getattr(self, "header_widget", None),
                getattr(self, "title_label", None),
                getattr(self, "left_slot", None),
                getattr(self, "right_slot", None),
            }
            if obj in header_drag_targets:
                if event.type() in (
                    QEvent.Type.MouseButtonPress,
                    QEvent.Type.MouseMove,
                    QEvent.Type.MouseButtonRelease,
                ) and self._header_event_on_chrome_button(obj, event):
                    return super().eventFilter(obj, event)
                if event.type() == QEvent.Type.MouseButtonPress:
                    return self._handle_header_press(event)
                if event.type() == QEvent.Type.MouseMove:
                    return self._handle_header_move(event)
                if event.type() == QEvent.Type.MouseButtonRelease:
                    return self._handle_header_release(event)
                return super().eventFilter(obj, event)

            if (
                event.type() == QEvent.Type.MouseButtonPress
                and getattr(event, "button", lambda: None)() == Qt.MouseButton.LeftButton  # type: ignore[attr-defined]
            ):
                # Map click point into this fence's local coordinates.
                p_self = None
                if isinstance(obj, QWidget):
                    try:
                        p_local = event.position().toPoint()  # type: ignore[attr-defined]
                        p_global = obj.mapToGlobal(p_local)
                        p_self = self.mapFromGlobal(p_global)
                    except Exception:
                        p_self = None
                if p_self is not None:
                    mode = self._hit_test_resize(p_self)
                    # Icons and title-bar buttons own their clicks.
                    if mode and not isinstance(obj, (FenceIconItem, FenceItemLabel)):
                        # Top strip on an empty title band → move, don't resize.
                        if (
                            mode == "n"
                            and p_self.y() <= self.HEADER_OVERLAY_HEIGHT
                            and not self.has_icon_widgets()
                        ):
                            mode = None
                        if mode:
                            self._resize_mode = mode
                            self._resize_start_geo = self.geometry()
                            self._resize_start_pos = (
                                event.globalPosition().toPoint()  # type: ignore[attr-defined]
                            )
                            self.grabMouse()
                            event.accept()
                            return True
        except Exception:
            pass

        # Child-owned events only — app-level Alt+wheel is handled by the
        # shared _FenceAltZoomAppFilter (one filter for all fences).
        if self._handle_zoom_wheel(event):
            return True
        # Empty-area rubber-band selection (Explorer-like). Icons handle their own.
        etype = event.type()
        if (
            etype
            in (
                QEvent.Type.MouseButtonPress,
                QEvent.Type.MouseMove,
                QEvent.Type.MouseButtonRelease,
            )
            and obj in self._marquee_host_objects()
            and not isinstance(obj, (FenceIconItem, FenceItemLabel))
        ):
            try:
                button = event.button()  # type: ignore[attr-defined]
            except Exception:
                button = None
            try:
                buttons = event.buttons()  # type: ignore[attr-defined]
            except Exception:
                buttons = Qt.MouseButton.NoButton
            try:
                pos = event.position().toPoint()  # type: ignore[attr-defined]
            except Exception:
                pos = None
            over_icon = False
            if pos is not None and isinstance(obj, QWidget):
                child = obj.childAt(pos)
                while child is not None:
                    if isinstance(child, (FenceIconItem, FenceItemLabel)):
                        over_icon = True
                        break
                    child = child.parentWidget()
                    if child is obj:
                        break
            if (
                etype == QEvent.Type.MouseButtonPress
                and button == Qt.MouseButton.LeftButton
                and pos is not None
                and not over_icon
            ):
                # Panel empty drag = rubber-band select (Explorer-like).
                # Moving the fence is title-bar only (_handle_header_press).
                mods = Qt.KeyboardModifier.NoModifier
                try:
                    mods = event.modifiers()  # type: ignore[attr-defined]
                except Exception:
                    pass
                additive = bool(mods & Qt.KeyboardModifier.ControlModifier)
                if additive:
                    if self._begin_marquee(obj, pos, additive=True):
                        event.accept()
                        return True
                else:
                    try:
                        gpos = event.globalPosition().toPoint()  # type: ignore[attr-defined]
                    except Exception:
                        gpos = None
                    if gpos is not None:
                        self._panel_press_pending = (obj, QPoint(pos), QPoint(gpos))
                        event.accept()
                        return True
            if (
                etype == QEvent.Type.MouseMove
                and getattr(self, "_panel_press_pending", None) is not None
                and pos is not None
                and (buttons & Qt.MouseButton.LeftButton)
            ):
                pending = self._panel_press_pending
                if pending is not None and isinstance(obj, QWidget):
                    _pobj, origin, press_global = pending
                    try:
                        current_global = obj.mapToGlobal(pos)
                    except RuntimeError:
                        current_global = press_global
                    if (
                        QPoint(current_global) - QPoint(press_global)
                    ).manhattanLength() >= self._PANEL_DRAG_THRESHOLD:
                        mods = Qt.KeyboardModifier.NoModifier
                        try:
                            mods = event.modifiers()  # type: ignore[attr-defined]
                        except Exception:
                            pass
                        if self._begin_marquee(
                            _pobj,
                            origin,
                            additive=bool(mods & Qt.KeyboardModifier.ControlModifier),
                        ):
                            self._update_marquee(obj, pos)
                            event.accept()
                            return True
                        self._panel_press_pending = None
            if (
                etype == QEvent.Type.MouseMove
                and getattr(self, "_marquee_origin", None) is not None
                and pos is not None
                and (buttons & Qt.MouseButton.LeftButton)
            ):
                if self._update_marquee(obj, pos):
                    event.accept()
                    return True
            if (
                etype == QEvent.Type.MouseMove
                and getattr(self, "_drag_pos", None) is not None
                and (buttons & Qt.MouseButton.LeftButton)
            ):
                return self._handle_header_move(event)
            if (
                etype == QEvent.Type.MouseButtonRelease
                and button == Qt.MouseButton.LeftButton
                and getattr(self, "_panel_press_pending", None) is not None
            ):
                self._panel_press_pending = None
                self.clear_item_selection()
                event.accept()
                return True
            if (
                etype == QEvent.Type.MouseButtonRelease
                and button == Qt.MouseButton.LeftButton
                and getattr(self, "_marquee_origin", None) is not None
            ):
                if pos is not None and self._finish_marquee(obj, pos):
                    event.accept()
                    return True
            if (
                etype == QEvent.Type.MouseButtonRelease
                and button == Qt.MouseButton.LeftButton
                and getattr(self, "_drag_pos", None) is not None
            ):
                return self._handle_header_release(event)
        # Only handle drops for widgets that belong to this fence.
        if self._owns_drop_object(obj):
            et = event.type()
            if et in (QEvent.Type.DragEnter, QEvent.Type.DragMove):
                if mime_has_droppable_items(event.mimeData()):
                    if self._is_virtual_mode():
                        event.setDropAction(Qt.DropAction.CopyAction)
                        pos = (
                            event.position().toPoint()
                            if hasattr(event, "position")
                            else QPoint(0, 0)
                        )
                        from src.ui.fence_icon_item import PUBLIC_SOURCE_FENCE_ID

                        src_id = desktidy_source_fence_id(event.mimeData()) or ""
                        # Public→fence: never raise_() desktop-band HWNDs during
                        # OLE (deadlocks Explorer). In-fence unpin still needs it.
                        if src_id != PUBLIC_SOURCE_FENCE_ID:
                            try:
                                self.raise_()
                            except Exception:
                                pass
                        self._update_drop_indicator(
                            obj if isinstance(obj, QWidget) else self.items_widget,
                            pos,
                        )
                    else:
                        action = preferred_drop_action_for_mime(
                            event.mimeData(), target_dir=self._target_folder()
                        )
                        event.setDropAction(action)
                    event.accept()
                    return True
            if et == QEvent.Type.DragLeave:
                # Ignore noisy leave events while moving between sibling icons.
                if not self.frameGeometry().contains(QCursor.pos()):
                    self._hide_drop_indicator()
                return False
            if et == QEvent.Type.Drop:
                mime = event.mimeData()
                self._hide_drop_indicator()
                if mime_has_droppable_items(mime):
                    if self._is_virtual_mode():
                        from src.ui.fence_icon_item import PUBLIC_SOURCE_FENCE_ID

                        src_id = desktidy_source_fence_id(mime) or ""
                        # Public→fence pins are applied synchronously after
                        # QDrag.exec by geometry. Accept OLE here but do not
                        # schedule a second assign (race → wrong fence / vanish).
                        if src_id == PUBLIC_SOURCE_FENCE_ID:
                            event.setDropAction(Qt.DropAction.CopyAction)
                            event.accept()
                            return True
                        pos = event.position().toPoint() if hasattr(event, "position") else QPoint(0, 0)
                        insert_at = self._insert_index_at(
                            obj if isinstance(obj, QWidget) else self.items_widget, pos
                        )
                        if self._import_virtual_drop(mime, insert_at=insert_at):
                            event.setDropAction(Qt.DropAction.CopyAction)
                            event.accept()
                            return True
                        event.ignore()
                        return True
                    action = preferred_drop_action_for_mime(
                        mime, target_dir=self._target_folder()
                    )
                    self._import_dropped_mime(mime)
                    event.setDropAction(action)
                    event.accept()
                    return True
        return super().eventFilter(obj, event)

    def _start_fence_drag(self, global_press: QPoint) -> bool:
        """Begin moving the whole fence from a global press point."""
        if self.is_position_locked():
            return False
        self._panel_press_pending = None
        self._marquee_origin = None
        band = getattr(self, "_marquee_band", None)
        if band is not None:
            try:
                band.hide()
            except RuntimeError:
                pass
        self._release_marquee_grab()
        self._resize_mode = None
        self._resize_start_geo = None
        self._resize_start_pos = None
        self._drag_pos = global_press - self.frameGeometry().topLeft()
        self._update_chrome_visibility()
        self._ensure_drop_target_interactive()
        try:
            self.grabMouse()
        except RuntimeError:
            pass
        return True

    def _handle_header_press(self, event) -> bool:
        if event.button() != Qt.MouseButton.LeftButton:
            return False
        if self.is_position_locked():
            self._drag_pos = None
            event.accept()
            return True
        self._start_fence_drag(event.globalPosition().toPoint())
        event.accept()
        return True

    def _handle_header_move(self, event) -> bool:
        if self.is_position_locked():
            return False
        if self._drag_pos is None or not (event.buttons() & Qt.MouseButton.LeftButton):
            return False
        top_left = event.globalPosition().toPoint() - self._drag_pos
        geo = self.geometry()
        snapped = snap_geometry(
            QRect(top_left.x(), top_left.y(), geo.width(), geo.height()),
            self.EDGE_SNAP_THRESHOLD,
        )
        self.move(snapped.topLeft())
        event.accept()
        return True

    def _handle_header_release(self, event) -> bool:
        if self.is_position_locked() and self._drag_pos is None:
            event.accept()
            return True
        if self._drag_pos is None:
            return False
        geo = self.geometry()
        snapped = snap_geometry(geo, self.EDGE_SNAP_THRESHOLD)
        if snapped.topLeft() != geo.topLeft():
            self.move(snapped.topLeft())
        self._drag_pos = None
        self._panel_press_pending = None
        try:
            self.releaseMouse()
        except RuntimeError:
            pass
        self._update_chrome_visibility()
        self.geometry_changed.emit()
        event.accept()
        return True

    def mouseMoveEvent(self, event) -> None:
        pos = event.position().toPoint()
        # grabMouse routes rubber-band moves here (not child eventFilter).
        if self._marquee_origin is not None and (
            event.buttons() & Qt.MouseButton.LeftButton
        ):
            if self._update_marquee(self, pos):
                event.accept()
                return
        if (
            getattr(self, "_panel_press_pending", None) is not None
            and (event.buttons() & Qt.MouseButton.LeftButton)
        ):
            pending = self._panel_press_pending
            if pending is not None:
                _pobj, origin, press_global = pending
                if (
                    event.globalPosition().toPoint() - press_global
                ).manhattanLength() >= self._PANEL_DRAG_THRESHOLD:
                    mods = event.modifiers()
                    if self._begin_marquee(
                        _pobj,
                        origin,
                        additive=bool(mods & Qt.KeyboardModifier.ControlModifier),
                    ):
                        self._update_marquee(self, pos)
                        event.accept()
                        return
                    self._panel_press_pending = None
        if self.is_position_locked():
            self._drag_pos = None
            self._resize_mode = None
            self._update_cursor(pos)
            return
        if self._resize_mode and self._resize_start_geo and self._resize_start_pos:
            delta = event.globalPosition().toPoint() - self._resize_start_pos
            geo = self._resize_start_geo
            x, y, w, h = geo.x(), geo.y(), geo.width(), geo.height()
            min_h = self.COLLAPSED_HEIGHT if self._collapsed else 180
            if "e" in self._resize_mode:
                w = max(self.MIN_WIDTH, w + delta.x())
            if "s" in self._resize_mode:
                h = max(min_h, h + delta.y())
            if "w" in self._resize_mode:
                new_w = max(self.MIN_WIDTH, w - delta.x())
                x = x + (w - new_w)
                w = new_w
            if "n" in self._resize_mode:
                new_h = max(min_h, h - delta.y())
                y = y + (h - new_h)
                h = new_h
            snapped = snap_geometry(QRect(x, y, w, h), self.EDGE_SNAP_THRESHOLD)
            self.setGeometry(snapped)
            if not self._collapsed:
                self._saved_height = snapped.height()
            event.accept()
            return
        if self._drag_pos and event.buttons() & Qt.MouseButton.LeftButton:
            top_left = event.globalPosition().toPoint() - self._drag_pos
            geo = self.geometry()
            snapped = snap_geometry(
                QRect(top_left.x(), top_left.y(), geo.width(), geo.height()),
                self.EDGE_SNAP_THRESHOLD,
            )
            self.move(snapped.topLeft())
            event.accept()
            return
        self._update_cursor(pos)

    def mouseReleaseEvent(self, event) -> None:
        if self._chrome_btn_press is not None:
            btn = self._chrome_btn_press
            self._chrome_btn_press = None
            try:
                if not btn.isHidden():
                    btn.click()
            except RuntimeError:
                pass
            event.accept()
            return
        if event.button() == Qt.MouseButton.LeftButton:
            if getattr(self, "_marquee_origin", None) is not None:
                if self._finish_marquee(self, event.position().toPoint()):
                    event.accept()
                    return
            if getattr(self, "_panel_press_pending", None) is not None:
                self._panel_press_pending = None
                self.clear_item_selection()
                event.accept()
                return
        was_resizing = self._resize_mode is not None
        moved = self._drag_pos is not None or was_resizing
        if self._drag_pos is not None:
            geo = self.geometry()
            snapped = snap_geometry(geo, self.EDGE_SNAP_THRESHOLD)
            if snapped.topLeft() != geo.topLeft():
                self.move(snapped.topLeft())
        elif was_resizing:
            snapped = snap_geometry(self.geometry(), self.EDGE_SNAP_THRESHOLD)
            self.setGeometry(snapped)
            if not self._collapsed:
                self._saved_height = snapped.height()
        self._drag_pos = None
        self._resize_mode = None
        self._resize_start_geo = None
        self._resize_start_pos = None
        if was_resizing:
            # Ensure we release captured mouse so Qt can route future clicks.
            try:
                self.releaseMouse()
            except Exception:
                pass
        if was_resizing and not self._collapsed:
            self._last_max_cols = 0
            self._last_icon_size = 0
            self.refresh()
        if moved:
            self.geometry_changed.emit()

    def wheelEvent(self, event) -> None:
        if self._handle_zoom_wheel(event):
            return
        super().wheelEvent(event)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._sync_header_geometry()
        self._sync_footer_geometry()
        # Defer column reflow while the user is dragging resize handles — avoids
        # shell icon rescale storms and full grid rebuilds on every mouse move.
        if not self._collapsed and not self._resize_mode:
            self._update_icon_scale()

    def enterEvent(self, event) -> None:
        self._hovered = True
        self._update_chrome_visibility()
        self._update_cursor(self.mapFromGlobal(QCursor.pos()))
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:
        if self._drag_pos is not None:
            super().leaveEvent(event)
            return
        self._hovered = False
        self._update_chrome_visibility()
        self._last_cursor_mode = None
        self.setCursor(QCursor(Qt.CursorShape.ArrowCursor))
        super().leaveEvent(event)

    def get_config_update(self) -> dict:
        pos = self.pos()
        size = self.size()
        return {
            "x": pos.x(),
            "y": pos.y(),
            "width": size.width(),
            "height": self._saved_height if self._collapsed else size.height(),
            "collapsed": self._collapsed,
            "icon_zoom": round(float(self._icon_zoom), 3),
            "position_locked": self.is_position_locked(),
        }

    def set_peek_mode(self, peek: bool) -> None:
        """Raise fences above all windows during peek; re-attach to desktop on exit."""
        if self._peek_mode == peek:
            return
        self._peek_mode = peek
        from src.win_shell import apply_overlay_stack_mode

        apply_overlay_stack_mode(self, desktop_layer=not peek, peek=peek)
        # Style rim / header for peek feedback — keep windowOpacity at 1.0 so icons stay crisp.
        self.setWindowOpacity(1.0)
        self._apply_style()

    def showEvent(self, event) -> None:
        super().showEvent(event)
        try:
            if self.testAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen):
                # Qt↔Win32 sync after batch SHOW uses DontShowOnScreen so
                # show() does not remap. configure_desktop_overlay here would flash.
                return
            # First-visit / parked restore: batch commit owns the first map.
            if bool(getattr(self, "_desktidy_batch_reveal", False)):
                return
        except RuntimeError:
            return
        try:
            from src.desktop_shell_host import (
                is_attached_to_desktop,
                set_overlay_hwnd_visible,
            )
            from src.win_shell import overlay_win32_visible

            hwnd = int(self.winId()) if self.winId() else 0
            # Parked fences stay DefView-owned. Only ShowWindow — never
            # configure/SetWindowPos here (that multi-flashed on every page click).
            if hwnd and is_attached_to_desktop(hwnd):
                if self.isVisible() and not overlay_win32_visible(self):
                    set_overlay_hwnd_visible(hwnd, True)
                return
        except Exception:
            pass
        if not self._peek_mode:
            configure_desktop_overlay(self, peek=False)
        else:
            configure_desktop_overlay(self, peek=True)

    def has_icon_widgets(self) -> bool:
        """True when the grid already has icon/label cells (not just empty hint)."""
        layout = getattr(self, "items_layout", None)
        if layout is None:
            return False
        for index in range(layout.count()):
            item = layout.itemAt(index)
            widget = item.widget() if item else None
            if isinstance(widget, (FenceIconItem, FenceItemLabel)):
                return True
        return False

    def apply_config(
        self,
        config: dict,
        *,
        refresh_icons: bool = True,
        apply_style: bool = True,
        defer_icon_refresh: bool = False,
    ) -> None:
        """Hot-reload config without recreating widget."""
        self.config = config
        self.fence_name = config.get("name", self.fence_name)
        self.folder_name = config.get("folder") or self.fence_name
        self.sort_by = config.get("sort_by", self.sort_by)
        self._style = dict(config.get("style") or self._style or {})
        self.title_label.setText(self.fence_name)
        prev_zoom = self._icon_zoom
        self._load_icon_zoom_from_config()
        zoom_changed = abs(prev_zoom - self._icon_zoom) >= 1e-3
        if zoom_changed:
            self._last_icon_size = 0
        self.set_position_locked(bool(config.get("position_locked", False)), emit=False)
        if not self._style.get("collapsible", True) and self._collapsed:
            self.set_collapsed(False, emit=False)
        if not self._style.get("show_title", True) and self._collapsed:
            # Avoid trapping a collapsed fence with no title chrome.
            self.set_collapsed(False, emit=False)
        if apply_style:
            self._apply_style()
            self.setWindowOpacity(self._target_opacity())
        if not defer_icon_refresh and (refresh_icons or zoom_changed):
            self.refresh(force=zoom_changed)
