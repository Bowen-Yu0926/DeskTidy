"""deskNote — standalone notepad entry (separate from DeskTidy.exe)."""

from __future__ import annotations

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))


def _sanitize_qt_platform() -> None:
    if os.environ.get("DESKTIDY_SELFTEST") == "1":
        return
    plat = (os.environ.get("QT_QPA_PLATFORM") or "").strip().lower()
    if plat in {"offscreen", "minimal", "null"}:
        os.environ.pop("QT_QPA_PLATFORM", None)
        os.environ.pop("QT_QPA_PLATFORM_PLUGIN_PATH", None)


def _entry() -> int:
    _sanitize_qt_platform()
    from src.app_logging import (
        get_logger,
        install_exception_hooks,
        install_qt_message_handler,
        setup_logging,
    )
    from src.desknote_launch import run_desknote_process

    setup_logging()
    install_exception_hooks()
    install_qt_message_handler()
    logger = get_logger()
    logger.info("deskNote entry argv=%s", sys.argv)
    return int(run_desknote_process(sys.argv[1:]) or 0)


if __name__ == "__main__":
    sys.exit(_entry())
