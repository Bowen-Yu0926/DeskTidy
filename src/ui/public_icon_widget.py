"""Free-floating public-desktop icon overlays (visible on every page)."""

from __future__ import annotations

from pathlib import Path

from PyQt6.QtCore import QPoint, QRect, QTimer, Qt, pyqtSignal
from PyQt6.QtGui import QColor, QPainter, QPixmap
from PyQt6.QtWidgets import QLabel, QVBoxLayout, QWidget

from src.desktop_caption import (
    caption_box_height,
    clear_desktop_caption_caches,
    icon_title_qfont,
    render_desktop_caption,
    screen_device_pixel_ratio,
)
from src.desktop_icon_metrics import (
    clear_desktop_icon_metrics_cache,
    desktop_icon_footprint_height,
    system_desktop_icon_cell,
)
from src.icon_utils import display_file_icon_pixmap
from src.ui.fence_icon_item import (
    _commit_public_item_selection_visual,
    after_shell_file_menu,
    apply_cut_ghost_visual,
    apply_public_click_selection,
    display_name_for_path,
    last_public_relocate_batch,
    last_virtual_unpin_pos,
    paint_public_item_selection,
    paths_for_public_drag,
    prepare_shell_item_invoke,
    public_selection_chrome_rect,
    start_public_item_drag,
)
from src.win_shell import (
    configure_desktop_overlay,
    open_path,
)

# Defaults mirror Explorer; live widgets re-read ``system_desktop_icon_cell()``.
_cell0 = system_desktop_icon_cell()
_ICON_SIZE = int(_cell0.icon_size)
# Side inset scales with cell (not a fixed 14px that eats narrow DPI cells).
_SIDE_INSET = max(4, min(10, int(_cell0.width) // 12))
_LABEL_WIDTH = max(32, int(_cell0.width) - 2 * _SIDE_INSET)


def _default_label_height(cell=None) -> int:
    cell = cell or system_desktop_icon_cell()
    try:
        from PyQt6.QtWidgets import QApplication

        if QApplication.instance() is not None:
            return max(
                16,
                caption_box_height(
                    icon_title_qfont(),
                    max_lines=int(cell.caption_lines),
                    extra_pad=6,
                ),
            )
    except Exception:
        pass
    # No QApplication yet (import-time / selftests): font-relative fallback.
    return max(16, int(round(18 * 2 + 3)))


_LABEL_HEIGHT = _default_label_height(_cell0)
_CAPTION_MAX_LINES = int(_cell0.caption_lines)
_WIDGET_W = int(_cell0.width)
_WIDGET_H = max(
    48,
    desktop_icon_footprint_height(
        int(_cell0.icon_size), int(_cell0.caption_lines), scale=1.0
    ),
)


def clear_label_pixmap_cache() -> None:
    """Drop cached shell caption font / cell metrics after DPI / theme changes."""
    clear_desktop_caption_caches()
    clear_desktop_icon_metrics_cache()
    global _ICON_SIZE, _LABEL_WIDTH, _LABEL_HEIGHT, _CAPTION_MAX_LINES, _WIDGET_W, _WIDGET_H
    global _SIDE_INSET
    cell = system_desktop_icon_cell()
    _ICON_SIZE = int(cell.icon_size)
    _SIDE_INSET = max(4, min(10, int(cell.width) // 12))
    _LABEL_WIDTH = max(32, int(cell.width) - 2 * _SIDE_INSET)
    _LABEL_HEIGHT = _default_label_height(cell)
    _CAPTION_MAX_LINES = int(cell.caption_lines)
    _WIDGET_W = int(cell.width)
    _WIDGET_H = max(
        48,
        desktop_icon_footprint_height(
            int(cell.icon_size), int(cell.caption_lines), scale=1.0
        ),
    )
    try:
        from src.public_desktop import refresh_public_grid_metrics

        refresh_public_grid_metrics()
    except Exception:
        pass


class PublicIconWidget(QWidget):
    """Desktop icon living outside all fences; shown on every page."""

    activated = pyqtSignal(Path)
    moved = pyqtSignal(Path, int, int)
    pinned_to_fence = pyqtSignal(Path)
    removed = pyqtSignal(Path)
    refresh_needed = pyqtSignal()

    def __init__(
        self,
        file_path: Path,
        x: int,
        y: int,
        parent: QWidget | None = None,
        *,
        icon_load_delay_ms: int = 0,
        icon_size: int = _ICON_SIZE,
    ):
        super().__init__(parent)
        self.file_path = Path(file_path)
        cell = system_desktop_icon_cell()
        base_icon = max(16, int(cell.icon_size))
        self._icon_size = max(16, min(256, int(icon_size)))
        scale = float(self._icon_size) / float(base_icon)
        # Footprint width from comfort cell; height follows 2-line shelf (not leftover space).
        self._widget_w = max(48, int(round(cell.width * scale)))
        side_inset = max(4, min(10, int(round(self._widget_w / 12))))
        self._label_width = max(32, self._widget_w - 2 * side_inset)
        self._caption_max_lines = min(2, max(1, int(cell.caption_lines)))
        self._icon_caption_gap = max(2, int(round(3 * scale)))
        # Tight 2-line shelf (ink only). Extra cell pitch is centered, not a
        # blank strip under the caption.
        shelf = caption_box_height(
            icon_title_qfont(),
            max_lines=self._caption_max_lines,
            extra_pad=max(6, int(round(6 * scale))),
        )
        self._label_height = max(16, int(round(shelf * scale)) if abs(scale - 1.0) > 0.01 else shelf)
        self._margin_top = 0
        self._margin_bot = 0
        # Prefer shared footprint so grid CELL_H and widget stay identical.
        self._widget_h_min = max(
            48,
            desktop_icon_footprint_height(
                self._icon_size, self._caption_max_lines, scale=scale
            ),
            int(round(cell.height * scale)),
        )
        self._press_pos: QPoint | None = None
        self._press_was_selected = False
        self._dragging_window = False
        self._drag_offset = QPoint()
        self._caption_text = ""
        self._selected = False

        self.setObjectName("publicDesktopIcon")
        # Hosted under PublicIconHost: child widgets share one shell HWND.
        # Standalone (tests / legacy): keep a top-level tool window.
        if parent is None:
            self.setWindowFlags(
                Qt.WindowType.FramelessWindowHint | Qt.WindowType.Window
            )
            self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
            self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
            self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setFixedSize(self._widget_w, self._widget_h_min)
        self.setToolTip(str(self.file_path))
        self.setCursor(Qt.CursorShape.ArrowCursor)
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.customContextMenuRequested.connect(self._show_menu)

        layout = QVBoxLayout(self)
        # Side inset only; equal stretch above/below centers glyph+caption in the cell.
        layout.setContentsMargins(5, 0, 5, 0)
        layout.setSpacing(self._icon_caption_gap)
        layout.addStretch(1)

        self.icon_label = QLabel()
        self.icon_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.icon_label.setFixedSize(self._icon_size, self._icon_size)
        self.icon_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        layout.addWidget(self.icon_label, 0, Qt.AlignmentFlag.AlignHCenter)

        self.text_label = QLabel()
        self.text_label.setObjectName("publicDesktopLabel")
        self.text_label.setAlignment(
            Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop
        )
        # No contentsMargins: they shrink the pixmap rect and clip caption ink.
        self.text_label.setContentsMargins(0, 0, 0, 0)
        self.text_label.setFixedWidth(self._label_width)
        # Caption is a pre-rasterized pixmap; WordWrap on the QLabel is unused.
        self.text_label.setWordWrap(False)
        self.text_label.setTextFormat(Qt.TextFormat.PlainText)
        self.text_label.setFont(icon_title_qfont())
        self.text_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        layout.addWidget(self.text_label, 0, Qt.AlignmentFlag.AlignHCenter)
        layout.addStretch(1)

        self.setStyleSheet(
            "#publicDesktopIcon { background: transparent; }"
            "QLabel#publicDesktopLabel {"
            "  background: transparent;"
            "  border: none;"
            "  padding: 0px;"
            "  font-weight: normal;"
            "}"
            "QLabel { background: transparent; border: none; }"
        )
        self._reload_label()
        delay = max(0, int(icon_load_delay_ms))
        if delay > 0:
            QTimer.singleShot(delay, self._reload_icon)
        else:
            self._reload_icon()
        # Settings store screen (virtual-desktop) coordinates.
        self.move_to_screen(int(x), int(y))
        apply_cut_ghost_visual(self)

    def is_hosted(self) -> bool:
        """True when painted into PublicIconHost (no per-icon HWND)."""
        return self.parentWidget() is not None and not self.isWindow()

    def screen_pos(self) -> QPoint:
        try:
            return self.mapToGlobal(QPoint(0, 0))
        except RuntimeError:
            return QPoint(int(self.x()), int(self.y()))

    def move_to_screen(self, x: int, y: int) -> None:
        parent = self.parentWidget()
        if parent is not None and not self.isWindow():
            try:
                self.move(parent.mapFromGlobal(QPoint(int(x), int(y))))
            except RuntimeError:
                return
        else:
            self.move(int(x), int(y))
        self._notify_host_geometry()

    def _notify_host_geometry(self) -> None:
        parent = self.parentWidget()
        schedule = getattr(parent, "schedule_mask_refresh", None)
        if callable(schedule):
            try:
                schedule()
            except RuntimeError:
                pass

    def _host_or_self(self) -> QWidget:
        """Shell-attach target: the shared host when nested, else this window."""
        if self.is_hosted():
            win = self.window()
            if win is not None:
                return win
        return self

    def _caption_font(self):
        font = icon_title_qfont()
        try:
            font.setBold(False)
            font.setWeight(400)
        except Exception:
            pass
        return font

    def _caption_height(self) -> int:
        # Always a fixed 2-line shelf — selected does not grow to 3+ lines.
        return max(16, int(self._label_height))

    def _reload_icon(self) -> None:
        # Scale from the canonical shell cache (same as fence cells) so
        # fence→public unpin reuses the already-fetched Office glyph.
        pix = display_file_icon_pixmap(self.file_path, self._icon_size)
        self.icon_label.setScaledContents(False)
        self.icon_label.setPixmap(pix)
        self.icon_label.setFixedSize(self._icon_size, self._icon_size)

    def _fit_caption(self) -> None:
        h = self._caption_height()
        # Lock min=max so pixmap / sizeHint cannot shrink short names (selection
        # chrome would then look shorter than long filenames in the same column).
        from PyQt6.QtWidgets import QSizePolicy

        self.text_label.setSizePolicy(
            QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed
        )
        self.text_label.setFixedSize(self._label_width, h)
        self.text_label.setMinimumSize(self._label_width, h)
        self.text_label.setMaximumSize(self._label_width, h)
        try:
            # Fixed Explorer cell pitch — caption stays a 2-line shelf when selected.
            self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)
            self.setFixedSize(self._widget_w, self._widget_h_min)
            self.setMinimumSize(self._widget_w, self._widget_h_min)
            self.setMaximumSize(self._widget_w, self._widget_h_min)
        except Exception:
            self.setFixedSize(self._widget_w, self._widget_h_min)
        self._sync_content_hit_mask()

    def _reload_label(self) -> None:
        name = display_name_for_path(self.file_path)
        self._caption_text = name
        self.text_label.setToolTip(name)
        self._fit_caption()
        # Use the shell caption renderer, but normalize to regular weight so the
        # public-area title does not look artificially bold.
        font = self._caption_font()
        self.text_label.setFont(font)
        max_lines = max(1, int(self._caption_max_lines))
        pix = render_desktop_caption(
            name,
            self._label_width,
            self.text_label.height(),
            dpr=screen_device_pixel_ratio(),
            font=font,
            max_lines=max_lines,
            elide=True,
        )
        self.text_label.setPixmap(pix)
        self.text_label.setScaledContents(False)
        self.text_label.setText("")
        self._sync_content_hit_mask()
        self._notify_host_geometry()

    def apply_renamed_path(self, new_path: Path) -> None:
        self.file_path = Path(new_path)
        self._reload_icon()
        self._reload_label()

    def showEvent(self, event) -> None:
        super().showEvent(event)
        self._sync_content_hit_mask()
        self._notify_host_geometry()
        # First map: re-rasterize so idle caption is already the 2-line ellipsis
        # (avoids needing a click to refresh after font/DPR settle).
        if not getattr(self, "_caption_mapped_once", False):
            self._caption_mapped_once = True
            try:
                self._reload_label()
            except Exception:
                pass
        if self.is_hosted():
            # Host owns DefView bind. Calling ensure on every child show() during
            # page switch restacked the whole desktop (multi-flash).
            return
        # Standalone float (tests): batch attach; skip Win32 work when healthy.
        try:
            from src.desktop_shell_host import (
                is_attached_to_desktop,
                set_overlay_hwnd_visible,
            )
            from src.win_shell import overlay_win32_visible

            hwnd = int(self.winId())
            if hwnd and is_attached_to_desktop(hwnd):
                if self.isVisible() and not overlay_win32_visible(self):
                    set_overlay_hwnd_visible(hwnd, True)
                return
        except Exception:
            pass
        configure_desktop_overlay(self)

    def hideEvent(self, event) -> None:  # noqa: N802
        super().hideEvent(event)
        self._notify_host_geometry()

    def moveEvent(self, event) -> None:  # noqa: N802
        super().moveEvent(event)
        self._notify_host_geometry()

    def is_selected(self) -> bool:
        return bool(self._selected)

    def set_selected(self, selected: bool) -> None:
        selected = bool(selected)
        if self._selected == selected:
            if selected:
                try:
                    from src.ui.fence_icon_item import mark_desktop_selection_live

                    mark_desktop_selection_live(True)
                except Exception:
                    pass
            return
        self._selected = selected
        # Keep 2-line ellipsis shelf when selection toggles (no height expand).
        try:
            self._reload_label()
        except Exception:
            pass
        _commit_public_item_selection_visual(self)
        try:
            from src.ui.fence_icon_item import mark_desktop_selection_live

            mark_desktop_selection_live(True if selected else None)
        except Exception:
            pass

    def paintEvent(self, event) -> None:  # noqa: N802
        # Layered DefView HWNDs hit-test by pixel alpha more than Qt masks.
        # Host plate carves this chrome out (alpha=0); without ink here the first
        # click falls through to DefView / blank-clear — feels like「要点两次」.
        painter = QPainter(self)
        try:
            chrome = public_selection_chrome_rect(self)
            if chrome.isValid() and not chrome.isEmpty():
                painter.fillRect(chrome, QColor(0, 0, 0, 1))
            if self._selected:
                paint_public_item_selection(self, painter)
        finally:
            if painter.isActive():
                painter.end()
        super().paintEvent(event)

    def resizeEvent(self, event) -> None:  # noqa: N802
        super().resizeEvent(event)
        self._sync_content_hit_mask()
        self._notify_host_geometry()

    def _content_hit_region(self):
        """One rectangular block around glyph + caption (not a T-shaped ink union).

        ``QRegion.united`` of icon + caption rects created a stepped hit silhouette
        matching only the blue ink lobes — clicks in the AABB corners missed.
        Use the same padded AABB as selection chrome so the whole plate selects.
        Shelf margins outside that block stay open for host 框选.
        """
        from PyQt6.QtGui import QRegion

        from src.ui.fence_icon_item import public_selection_chrome_rect

        rect = public_selection_chrome_rect(self)
        if not rect.isValid() or rect.isEmpty():
            fallback = self.rect().adjusted(12, 6, -12, -6)
            return QRegion(fallback) if fallback.isValid() else QRegion()
        return QRegion(rect)

    def _content_hit_rect(self) -> QRect:
        """Bounding rect of content hit region (tests / diagnostics)."""
        return self._content_hit_region().boundingRect()

    def _pos_on_content(self, pos: QPoint) -> bool:
        return self._content_hit_region().contains(pos)

    def _sync_content_hit_mask(self) -> None:
        """Limit hit-testing to the content plate so host can 框选 in shelf gaps.

        Fixed 124×108 shelves overlap neighbors; without a mask the transparent
        margins steal every press and rubber-band never starts.
        """
        region = self._content_hit_region()
        try:
            if region.isEmpty():
                self.clearMask()
            else:
                self.setMask(region)
        except RuntimeError:
            pass

    def mouseDoubleClickEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            from src.ui.fence_icon_item import mark_item_opened_by_double_click

            mark_item_opened_by_double_click(self)
            self.activated.emit(self.file_path)
            open_path(self.file_path)
            event.accept()
            return
        super().mouseDoubleClickEvent(event)

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            from src.ui.fence_icon_item import cancel_pending_label_rename

            cancel_pending_label_rename(self)
            self._press_was_selected = self.is_selected()
        if event.button() in (Qt.MouseButton.LeftButton, Qt.MouseButton.RightButton):
            apply_public_click_selection(
                self,
                event.modifiers(),
                right_button=event.button() == Qt.MouseButton.RightButton,
            )
        if event.button() == Qt.MouseButton.LeftButton:
            self._press_pos = event.position().toPoint()
            self._dragging_window = False
            self._drag_offset = (
                event.globalPosition().toPoint() - self.frameGeometry().topLeft()
            )
            event.accept()
            return
        if event.button() == Qt.MouseButton.RightButton:
            event.accept()
            return
        super().mousePressEvent(event)

    def keyPressEvent(self, event) -> None:
        from src.ui.fence_icon_item import (
            ensure_desktop_icon_key_hook,
            handle_fence_item_key,
        )

        ensure_desktop_icon_key_hook()
        if handle_fence_item_key(self, event):
            event.accept()
            return
        super().keyPressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if not (event.buttons() & Qt.MouseButton.LeftButton) or self._press_pos is None:
            return
        delta = event.position().toPoint() - self._press_pos
        # Higher threshold: plain clicks must not start a drag that hides the icon.
        if not self._dragging_window and delta.manhattanLength() < 24:
            return

        from src.ui.fence_icon_item import cancel_pending_label_rename

        cancel_pending_label_rename(self)
        # Prefer Qt drag (into fences / public relocate) over window move.
        if not self._dragging_window:
            press_local = QPoint(self._press_pos)
            self._press_pos = None
            path = self.file_path
            drag_paths = paths_for_public_drag(self)
            # Drag helper already defers shell re-attach; keep emits deferred too
            # so we never mutate overlays while still inside mouseMove.
            result, relocated = start_public_item_drag(
                self, path, press_local=press_local, paths=drag_paths
            )
            if relocated:
                batch = last_public_relocate_batch()
                if batch:
                    for p, x, y in batch:
                        QTimer.singleShot(
                            0, lambda pp=p, xx=x, yy=y: self.moved.emit(pp, xx, yy)
                        )
                else:
                    pos = last_virtual_unpin_pos()
                    QTimer.singleShot(
                        0,
                        lambda p=path, x=pos.x(), y=pos.y(): self.moved.emit(p, x, y),
                    )
                return
            if result == Qt.DropAction.CopyAction:
                # Deferred pin / confirm owns the pinned_to_fence emit — do not
                # clear the flag here (that raced the timer and skipped cleanup).
                if getattr(self, "_desktidy_pin_deferred", False):
                    return
                try:
                    still_hidden = not self.isVisible()
                except RuntimeError:
                    still_hidden = False
                if still_hidden:
                    QTimer.singleShot(0, lambda p=path: self.pinned_to_fence.emit(p))
                return
            # Safety: if still hidden after a failed drop, show on next tick.
            QTimer.singleShot(0, self._ensure_visible_after_drag)
            return

    def _ensure_visible_after_drag(self) -> None:
        try:
            if self.isVisible():
                return
            self.show()
        except RuntimeError:
            return
        try:
            target = self._host_or_self()
            ensure = getattr(target, "ensure_shell_attached", None)
            if callable(ensure):
                ensure(force=False)
            else:
                configure_desktop_overlay(target)
        except Exception:
            pass

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
                self._dragging_window = False
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
                self._dragging_window = False
                return
        self._press_pos = None
        self._dragging_window = False
        super().mouseReleaseEvent(event)

    def _show_menu(self, pos: QPoint) -> None:
        from PyQt6.QtWidgets import QApplication

        from src.ui.fence_icon_item import (
            build_file_icon_shell_extras,
            paths_for_icon_shell_action,
        )
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
            anchor=self,
            action_paths=action_paths,
        )

        global_pos = self.mapToGlobal(pos)
        try:
            hwnd = (
                int(self.window().winId())
                if self.window() is not None
                else int(self.winId())
            )
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
        from src.ui.fence_icon_item import delete_selected_public_items

        delete_selected_public_items(self)
