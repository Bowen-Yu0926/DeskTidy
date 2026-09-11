"""Desktop region selector for creating fences."""

from __future__ import annotations

from PyQt6.QtCore import QPoint, QRect, Qt, pyqtSignal
from PyQt6.QtGui import QColor, QCursor, QFont, QPainter, QPen
from PyQt6.QtWidgets import QMenu, QWidget

_MASK = QColor(0, 0, 0, 90)
_BORDER = QColor(88, 166, 255)
_BORDER_OUTER = QColor(255, 255, 255, 180)
_HINT_BG = QColor(0, 0, 0, 170)
_HINT_TEXT = QColor(255, 255, 255)

MIN_WIDTH = 160
MIN_HEIGHT = 180


class FenceRegionSelector(QWidget):
    """Drag to select a rectangle, then right-click to create a fence."""

    confirmed = pyqtSignal(QRect)
    cancelled = pyqtSignal()

    def __init__(self, origin: QPoint, size, parent: QWidget | None = None):
        super().__init__(
            parent,
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool,
        )
        self._origin = origin
        self._start: QPoint | None = None
        self._current: QPoint | None = None
        self._selection: QRect | None = None
        self._menu_open = False
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.CrossCursor)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent, True)
        self.setGeometry(QRect(origin, size))

    def begin_drag(self, start_local: QPoint, end_local: QPoint) -> None:
        rect = QRect(start_local, end_local).normalized()
        if rect.width() >= MIN_WIDTH and rect.height() >= MIN_HEIGHT:
            self._selection = rect
            self._start = None
            self._current = None
        else:
            self._start = start_local
            self._current = end_local
            self._selection = None
        self.update()

    def finish_drag(self, end_local: QPoint) -> None:
        if self._start is None:
            # Already finalized via mouseRelease on the overlay itself.
            if self._selection is not None:
                self._prompt_create_menu()
            return
        self._current = end_local
        rect = self._selection_rect()
        self._start = None
        self._current = None
        if rect and rect.width() >= MIN_WIDTH and rect.height() >= MIN_HEIGHT:
            self._selection = rect
            self.update()
            self._prompt_create_menu()
        else:
            self._selection = None
            self._cancel()

    def update_drag_end(self, end_local: QPoint) -> None:
        if self._start is None or self._selection is not None:
            return
        self._current = end_local
        self.update()

    def show_overlay(self) -> None:
        self.show()
        self.raise_()
        self.activateWindow()
        self.setFocus()
        try:
            import ctypes

            hwnd = int(self.winId())
            ctypes.windll.user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0001 | 0x0002)
        except OSError:
            pass

    def _active_rect(self) -> QRect | None:
        if self._selection and not self._selection.isNull():
            return self._selection
        return self._selection_rect()

    def _selection_rect(self) -> QRect | None:
        if self._start is None or self._current is None:
            return None
        return QRect(self._start, self._current).normalized()

    def _draw_mask_outside(self, painter: QPainter, rect: QRect) -> None:
        full = self.rect()
        painter.fillRect(0, 0, full.width(), rect.top(), _MASK)
        painter.fillRect(0, rect.bottom() + 1, full.width(), full.height() - rect.bottom() - 1, _MASK)
        painter.fillRect(0, rect.top(), rect.left(), rect.height(), _MASK)
        painter.fillRect(rect.right() + 1, rect.top(), full.width() - rect.right() - 1, rect.height(), _MASK)

    def _draw_hint(self, painter: QPainter, rect: QRect) -> None:
        text = "松开后确认创建 · Esc 取消"
        font = QFont("Microsoft YaHei UI", 10)
        painter.setFont(font)
        metrics = painter.fontMetrics()
        pad_x, pad_y = 10, 6
        badge_w = metrics.horizontalAdvance(text) + pad_x * 2
        badge_h = metrics.height() + pad_y * 2
        badge_x = rect.left()
        badge_y = rect.bottom() + 8
        if badge_y + badge_h > self.height():
            badge_y = max(0, rect.top() - badge_h - 8)
        badge = QRect(badge_x, badge_y, badge_w, badge_h)
        painter.fillRect(badge, _HINT_BG)
        painter.setPen(_HINT_TEXT)
        painter.drawText(badge, Qt.AlignmentFlag.AlignCenter, text)

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.fillRect(self.rect(), _MASK)

        rect = self._active_rect()
        if rect and rect.width() > 0 and rect.height() > 0:
            painter.fillRect(rect, QColor(88, 166, 255, 35))
            self._draw_mask_outside(painter, rect)
            inner = rect.adjusted(1, 1, -1, -1)
            painter.setPen(QPen(_BORDER_OUTER, 1))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRect(rect)
            painter.setPen(QPen(_BORDER, 2))
            painter.drawRect(inner)
            self._draw_hint(painter, rect)

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._start = event.position().toPoint()
            self._current = self._start
            self._selection = None
            self.update()
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if self._start is not None and self._selection is None:
            self._current = event.position().toPoint()
            self.update()

    def mouseReleaseEvent(self, event) -> None:
        if event.button() != Qt.MouseButton.LeftButton or self._start is None:
            return
        self._current = event.position().toPoint()
        rect = self._selection_rect()
        self._start = None
        self._current = None
        if rect and rect.width() >= MIN_WIDTH and rect.height() >= MIN_HEIGHT:
            self._selection = rect
            self.update()
            self._prompt_create_menu()
        else:
            self._selection = None
            self._cancel()

    def _prompt_create_menu(self) -> None:
        if self._menu_open:
            return
        rect = self._active_rect()
        if rect is None or rect.width() < MIN_WIDTH or rect.height() < MIN_HEIGHT:
            self._cancel()
            return

        self._menu_open = True
        menu = QMenu(self)
        menu.setWindowFlags(
            menu.windowFlags()
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.FramelessWindowHint
        )
        create_action = menu.addAction("创建分区")
        menu.addAction("取消")
        try:
            chosen = menu.exec(QCursor.pos())
        finally:
            self._menu_open = False
        if chosen == create_action:
            global_rect = QRect(self.mapToGlobal(rect.topLeft()), rect.size())
            self.hide()
            self.confirmed.emit(global_rect)
        else:
            self._cancel()

    def contextMenuEvent(self, event) -> None:
        self._prompt_create_menu()
        event.accept()

    def keyPressEvent(self, event) -> None:
        if event.key() == Qt.Key.Key_Escape:
            self._cancel()
        else:
            super().keyPressEvent(event)

    def _cancel(self) -> None:
        self.hide()
        self.cancelled.emit()
