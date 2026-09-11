"""Desktop right-click position recorder for shell-verb actions.

DeskTidy items are injected into Explorer's own desktop menu via
``shell_background_verbs`` (Directory\\Background\\shell). This module records
click coordinates and signals a freeze so overlays do not restack while the
system menu is open (restack causes visible flash).
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes
from typing import Callable, TYPE_CHECKING

from PyQt6.QtCore import QObject, Qt, pyqtSignal

from src.app_logging import get_logger
from src.desktop_ll_mouse import MSLLHOOKSTRUCT, register_ll_mouse_handler, unregister_ll_mouse_handler
from src.win_shell import is_desktop_point

if TYPE_CHECKING:
    from src.fence_region_manager import FenceRegionManager

user32 = ctypes.windll.user32

WM_RBUTTONDOWN = 0x0204
WM_RBUTTONUP = 0x0205

_last_desktop_right_click: tuple[int, int] | None = None


class _HookBridge(QObject):
    recorded = pyqtSignal(int, int)
    freeze = pyqtSignal()


def get_last_desktop_right_click() -> tuple[int, int]:
    """Last desktop-background right-click, or current cursor, or (0, 0)."""
    global _last_desktop_right_click
    if _last_desktop_right_click is not None:
        return _last_desktop_right_click
    pt = wintypes.POINT()
    if user32.GetCursorPos(ctypes.byref(pt)):
        return int(pt.x), int(pt.y)
    return 0, 0


def set_last_desktop_right_click(x: int, y: int) -> None:
    global _last_desktop_right_click
    _last_desktop_right_click = (int(x), int(y))


class DesktopContextMonitor:
    """Record desktop right-clicks without blocking Explorer's context menu."""

    def __init__(
        self,
        on_right_click: Callable[[int, int], None] | None = None,
        fence_region_manager: FenceRegionManager | None = None,
        is_marquee_enabled: Callable[[], bool] | None = None,
        on_freeze: Callable[[], None] | None = None,
        debounce_ms: int = 0,
    ):
        _ = on_right_click, debounce_ms
        self._on_freeze = on_freeze
        self._region_mgr = fence_region_manager
        self._is_enabled = is_marquee_enabled or (lambda: True)
        self._active = False
        self._bridge = _HookBridge()
        # Queued: hook runs on an arbitrary thread; freeze/record on Qt main thread.
        self._bridge.freeze.connect(self._emit_freeze, Qt.ConnectionType.QueuedConnection)
        self._bridge.recorded.connect(
            self._on_recorded, Qt.ConnectionType.QueuedConnection
        )

    def _feature_enabled(self) -> bool:
        try:
            return bool(self._is_enabled())
        except Exception:
            return True

    def _on_ll_mouse(self, w_param: int, l_param: int) -> None:
        try:
            if w_param not in (WM_RBUTTONDOWN, WM_RBUTTONUP) or not self._feature_enabled():
                return
            ms = MSLLHOOKSTRUCT.from_address(l_param)
            x, y = ms.pt.x, ms.pt.y
            region_active = (
                self._region_mgr is not None and self._region_mgr.is_active()
            )
            if is_desktop_point(x, y) and not region_active:
                # Freeze ASAP on DOWN so FG recover cannot restack before menu.
                self._bridge.freeze.emit()
                if w_param == WM_RBUTTONUP:
                    self._bridge.recorded.emit(x, y)
        except Exception:
            pass

    def _emit_freeze(self) -> None:
        if self._on_freeze is not None:
            try:
                self._on_freeze()
            except Exception:
                get_logger().exception("desktop menu freeze callback failed")

    def _on_recorded(self, x: int, y: int) -> None:
        set_last_desktop_right_click(x, y)

    def start(self) -> None:
        if self._active:
            return
        register_ll_mouse_handler(self._on_ll_mouse)
        self._active = True
        get_logger().info("desktop click recorder subscribed to shared LL mouse hook")

    def stop(self) -> None:
        if not self._active:
            return
        unregister_ll_mouse_handler(self._on_ll_mouse)
        self._active = False
