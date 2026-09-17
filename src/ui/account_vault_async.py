"""Background vault API jobs so UI stays responsive on slow hosts."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from PyQt6.QtCore import QObject, QThread, pyqtSignal, pyqtSlot
from PyQt6.QtWidgets import QApplication

from src.account_vault_api import VaultApiError


class VaultApiJob(QThread):
    """Run a callable off the UI thread; emit result or error."""

    succeeded = pyqtSignal(object)
    failed = pyqtSignal(object)

    def __init__(self, fn: Callable[[], Any], parent: QObject | None = None):
        super().__init__(parent)
        self._fn = fn

    def run(self) -> None:  # noqa: D401
        try:
            self.succeeded.emit(self._fn())
        except VaultApiError as exc:
            self.failed.emit(exc)
        except Exception as exc:  # noqa: BLE001 — surface to UI
            self.failed.emit(exc)


class VaultAsyncRunner(QObject):
    """Serialize vault network work for one host widget; show busy while running."""

    busy_changed = pyqtSignal(bool, str)  # busy, status_hint

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self._job: VaultApiJob | None = None
        self._busy = False
        self._pending_ok: Callable[[Any], None] | None = None
        self._pending_err: Callable[[BaseException], None] | None = None
        self._queued: tuple[
            Callable[[], Any],
            Callable[[Any], None],
            Callable[[BaseException], None],
            str,
        ] | None = None

    @property
    def busy(self) -> bool:
        return self._busy

    def run(
        self,
        fn: Callable[[], Any],
        *,
        on_ok: Callable[[Any], None],
        on_err: Callable[[BaseException], None],
        busy_text: str = "正在同步…",
    ) -> bool:
        """Start a job. If busy, replace any queued follow-up and return False."""
        if self._busy:
            self._queued = (fn, on_ok, on_err, busy_text)
            return False
        self._busy = True
        self._pending_ok = on_ok
        self._pending_err = on_err
        self.busy_changed.emit(True, busy_text)
        # Parent the job to QApplication (process lifetime) — NOT to self.
        # When the host widget is destroyed (release_lazy_pages / page switch),
        # a self-parented QThread is killed mid-run: callbacks never fire,
        # the settings checkbox stays disabled, and Qt prints
        # "QThread: Destroyed while thread is still running".
        # Parenting to the app lets the job finish naturally; Qt auto-
        # disconnects the slots (receiver destroyed) so nothing unsafe runs.
        app = QApplication.instance()
        job = VaultApiJob(fn, app if app is not None else None)
        job.succeeded.connect(self._on_job_succeeded)
        job.failed.connect(self._on_job_failed)
        job.finished.connect(job.deleteLater)
        self._job = job
        job.start()
        return True

    @pyqtSlot(object)
    def _on_job_succeeded(self, result: object) -> None:
        cb = self._pending_ok
        self._finish()
        if cb is not None:
            cb(result)
        self._pump_queue()

    @pyqtSlot(object)
    def _on_job_failed(self, exc: object) -> None:
        cb = self._pending_err
        self._finish()
        if cb is not None:
            if isinstance(exc, BaseException):
                cb(exc)
            else:
                cb(RuntimeError(str(exc)))
        self._pump_queue()

    def _pump_queue(self) -> None:
        queued = self._queued
        self._queued = None
        if queued is None:
            return
        fn, on_ok, on_err, busy_text = queued
        self.run(fn, on_ok=on_ok, on_err=on_err, busy_text=busy_text)

    def _finish(self) -> None:
        self._busy = False
        self._job = None
        self._pending_ok = None
        self._pending_err = None
        self.busy_changed.emit(False, "")
