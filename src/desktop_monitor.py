"""Monitor desktop blank-click gestures via the shared LL mouse hook."""

from __future__ import annotations

from typing import Callable

from PyQt6.QtCore import QObject, QTimer, pyqtSignal

from src.desktop_ll_mouse import (
    MSLLHOOKSTRUCT,
    register_ll_mouse_handler,
    unregister_ll_mouse_handler,
)
from src.win_shell import is_desktop_point

WM_LBUTTONUP = 0x0202
WM_LBUTTONDBLCLK = 0x0203


class _HookBridge(QObject):
    requested = pyqtSignal()


class DesktopDoubleClickMonitor:
    """Desktop double-click detection via the shared LL mouse hook."""

    def __init__(self, on_double_click: Callable[[], None], debounce_ms: int = 400):
        self._callback = on_double_click
        self._debounce_ms = debounce_ms
        self._pending = False
        self._active = False
        self._bridge = _HookBridge()
        self._bridge.requested.connect(self._arm_timer)
        self._timer = QTimer()
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(self._fire)

    def _on_ll_mouse(self, w_param: int, l_param: int) -> None:
        if w_param != WM_LBUTTONDBLCLK:
            return
        try:
            ms = MSLLHOOKSTRUCT.from_address(l_param)
            if is_desktop_point(ms.pt.x, ms.pt.y):
                self._bridge.requested.emit()
        except Exception:
            pass

    def _arm_timer(self) -> None:
        self._pending = True
        self._timer.start(self._debounce_ms)

    def _fire(self) -> None:
        if self._pending:
            self._pending = False
            self._callback()

    def start(self) -> None:
        if self._active:
            return
        register_ll_mouse_handler(self._on_ll_mouse)
        self._active = True

    def stop(self) -> None:
        if not self._active:
            return
        unregister_ll_mouse_handler(self._on_ll_mouse)
        self._active = False


class DesktopBlankClickMonitor:
    """Detect a left-click release on blank desktop and notify on the Qt thread."""

    def __init__(self, on_blank_click: Callable[[], None]):
        self._callback = on_blank_click
        self._active = False
        self._bridge = _HookBridge()
        self._bridge.requested.connect(self._fire)

    def _on_ll_mouse(self, w_param: int, l_param: int) -> None:
        if w_param != WM_LBUTTONUP:
            return
        try:
            ms = MSLLHOOKSTRUCT.from_address(l_param)
            if is_desktop_point(ms.pt.x, ms.pt.y):
                self._bridge.requested.emit()
        except Exception:
            pass

    def _fire(self) -> None:
        try:
            self._callback()
        except Exception:
            pass

    def start(self) -> None:
        if self._active:
            return
        register_ll_mouse_handler(self._on_ll_mouse)
        self._active = True

    def stop(self) -> None:
        if not self._active:
            return
        unregister_ll_mouse_handler(self._on_ll_mouse)
        self._active = False
