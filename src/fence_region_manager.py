"""Manage desktop region selection for fence creation."""

from __future__ import annotations

from PyQt6.QtCore import QPoint, QRect, Qt, pyqtSignal, QObject

from src.desktop_capture import virtual_desktop_rect
from src.ui.fence_region_selector import FenceRegionSelector


class FenceRegionManager(QObject):
    region_confirmed = pyqtSignal(QRect)
    cancelled = pyqtSignal()
    _start_drag = pyqtSignal(int, int, int, int)
    _update_drag = pyqtSignal(int, int)
    _finish_drag = pyqtSignal(int, int)
    _start_point = pyqtSignal(int, int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._overlay: FenceRegionSelector | None = None
        self._start_drag.connect(self._start_and_update, Qt.ConnectionType.QueuedConnection)
        self._update_drag.connect(self.update_drag, Qt.ConnectionType.QueuedConnection)
        self._finish_drag.connect(self.finish_drag, Qt.ConnectionType.QueuedConnection)
        self._start_point.connect(
            lambda x, y: self.start(x, y, x, y), Qt.ConnectionType.QueuedConnection
        )

    def is_active(self) -> bool:
        return self._overlay is not None and self._overlay.isVisible()

    def _geo(self) -> QRect:
        return virtual_desktop_rect()

    def _to_local(self, screen_x: int, screen_y: int) -> QPoint:
        geo = self._geo()
        return QPoint(screen_x - geo.x(), screen_y - geo.y())

    def start(
        self,
        screen_x: int | None = None,
        screen_y: int | None = None,
        end_x: int | None = None,
        end_y: int | None = None,
    ) -> None:
        if self.is_active():
            return

        geo = self._geo()
        if geo.width() <= 0 or geo.height() <= 0:
            return

        self._overlay = FenceRegionSelector(geo.topLeft(), geo.size())
        self._overlay.confirmed.connect(self._on_confirmed)
        self._overlay.cancelled.connect(self._on_cancelled)
        if screen_x is not None and screen_y is not None:
            start = self._to_local(screen_x, screen_y)
            if end_x is not None and end_y is not None:
                end = self._to_local(end_x, end_y)
                self._overlay.begin_drag(start, end)
            else:
                self._overlay.begin_drag(start, start)
        self._overlay.show_overlay()

    def update_drag(self, screen_x: int, screen_y: int) -> None:
        if not self._overlay:
            return
        self._overlay.update_drag_end(self._to_local(screen_x, screen_y))

    def finish_drag(self, screen_x: int, screen_y: int) -> None:
        if not self._overlay:
            return
        self._overlay.finish_drag(self._to_local(screen_x, screen_y))

    def schedule_start(self, screen_x: int, screen_y: int) -> None:
        self._start_point.emit(screen_x, screen_y)

    def schedule_start_drag(
        self, start_x: int, start_y: int, end_x: int, end_y: int
    ) -> None:
        self._start_drag.emit(start_x, start_y, end_x, end_y)

    def schedule_update(self, screen_x: int, screen_y: int) -> None:
        self._update_drag.emit(screen_x, screen_y)

    def schedule_finish(self, screen_x: int, screen_y: int) -> None:
        self._finish_drag.emit(screen_x, screen_y)

    def _start_and_update(
        self, start_x: int, start_y: int, end_x: int, end_y: int
    ) -> None:
        if self.is_active():
            self.update_drag(end_x, end_y)
            return
        self.start(start_x, start_y, start_x, start_y)
        self.update_drag(end_x, end_y)

    def _on_confirmed(self, rect: QRect) -> None:
        self._overlay = None
        self.region_confirmed.emit(rect)

    def _on_cancelled(self) -> None:
        self._overlay = None
        self.cancelled.emit()
