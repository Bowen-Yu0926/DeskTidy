"""Application bootstrap: process entry (``main``) and fatal-error reporting.

``main`` and ``_show_fatal_error`` are re-exported from ``src.app`` for backwards
compatibility (``main.py`` still does ``from src.app import main``). The real
implementation lives here so ``src.app`` no longer owns startup / fatal handling.
"""

from __future__ import annotations

import sys

from PyQt6.QtWidgets import QApplication

from src.i18n import APP_NAME_ZH


def main(*, pending_shell_verb: str | None = None) -> int:
    # Deferred import to avoid a circular import: src.app re-exports this module
    # at its top, so we must not import src.app at module scope here.
    from src.app import DeskTidyApp

    try:
        app = DeskTidyApp()
        if pending_shell_verb:
            app._pending_shell_verb = pending_shell_verb
        return app.run()
    except Exception as exc:
        _show_fatal_error(exc)
        return 1


def _show_fatal_error(exc: Exception) -> None:
    import traceback

    from src.app_logging import log_exception

    detail = traceback.format_exc()
    log_exception("fatal startup/runtime error", exc)
    try:
        qt_app = QApplication.instance() or QApplication(sys.argv)
        from PyQt6.QtWidgets import QMessageBox

        QMessageBox.critical(None, f"{APP_NAME_ZH} - 启动失败", f"{exc}\n\n{detail[-1500:]}")
    except Exception:
        import ctypes

        ctypes.windll.user32.MessageBoxW(0, str(exc), APP_NAME_ZH, 0x10)
