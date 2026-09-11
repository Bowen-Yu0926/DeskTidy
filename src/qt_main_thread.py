"""Marshal callables from worker threads onto the Qt GUI thread.

Posting timers from a raw ``threading.Thread`` is undefined in Qt (no event
loop on that thread). Folder moves used that pattern; completion never ran
→ ghost public floats → silent RMB failures.

Bridge installation must happen on the GUI thread (creating a ``QObject`` on a
worker is also undefined). Call ``ensure_main_thread_bridge()`` before starting
workers — ``_start_background_fs_move`` / paste do this.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from PyQt6.QtCore import QEvent, QObject, QThread
from PyQt6.QtWidgets import QApplication

_EVENT_TYPE = QEvent.Type(QEvent.registerEventType())
_filter: "_CallableFilter | None" = None


class _CallableEvent(QEvent):
    def __init__(self, fn: Callable[[], Any]) -> None:
        super().__init__(_EVENT_TYPE)
        self.fn = fn


class _CallableFilter(QObject):
    def eventFilter(self, obj, event) -> bool:  # noqa: N802
        if event.type() == _EVENT_TYPE:
            fn = getattr(event, "fn", None)
            if callable(fn):
                try:
                    fn()
                except Exception:
                    try:
                        from src.app_logging import get_logger

                        get_logger().exception("main-thread callable failed")
                    except Exception:
                        pass
            return True
        return False


def ensure_main_thread_bridge(app: QApplication | None = None) -> bool:
    """Install the QApplication event filter (must run on the GUI thread)."""
    global _filter
    app = app or QApplication.instance()
    if app is None:
        return False
    try:
        if QThread.currentThread() is not app.thread():
            return _filter is not None
    except Exception:
        return _filter is not None
    if _filter is not None:
        try:
            if _filter.thread() is app.thread():
                return True
        except RuntimeError:
            _filter = None
    filt = _CallableFilter(app)
    app.installEventFilter(filt)
    _filter = filt
    return True


def call_on_main_thread(fn: Callable[[], Any]) -> None:
    """Run *fn* on the Qt GUI thread (immediate if already there)."""
    app = QApplication.instance()
    if app is None:
        fn()
        return
    try:
        on_gui = QThread.currentThread() is app.thread()
    except Exception:
        on_gui = True
    if on_gui:
        ensure_main_thread_bridge(app)
        fn()
        return
    if _filter is None:
        # Bridge not installed yet — cannot safely create QObjects here.
        # Best-effort: still post; if no filter, the event is ignored and the
        # caller should have called ensure_main_thread_bridge on the GUI first.
        try:
            from src.app_logging import get_logger

            get_logger().warning(
                "call_on_main_thread: bridge missing; installing is a no-op from worker"
            )
        except Exception:
            pass
    QApplication.postEvent(app, _CallableEvent(fn))
