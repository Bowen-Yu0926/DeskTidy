"""Application logging setup and exception hooks."""

from __future__ import annotations

import logging
import os
import sys
import threading
import traceback
from logging.handlers import TimedRotatingFileHandler
from pathlib import Path

_LOGGER_NAME = "desktidy"
_INITIALIZED = False


def _app_base_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent.parent


def log_file_path() -> Path:
    log_dir = _app_base_dir() / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    return log_dir / "latest.log"


def log_dir_path() -> Path:
    return log_file_path().parent


def _cleanup_old_rotated_logs() -> None:
    log_dir = log_dir_path()
    files = sorted(
        [p for p in log_dir.glob("latest.log*") if p.name != "latest.log"],
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    for old in files[1:]:
        try:
            old.unlink()
        except OSError:
            pass


def setup_logging() -> logging.Logger:
    global _INITIALIZED
    logger = logging.getLogger(_LOGGER_NAME)
    if _INITIALIZED:
        return logger

    path = log_file_path()
    _cleanup_old_rotated_logs()
    logger.setLevel(logging.INFO)
    logger.propagate = False

    handler = TimedRotatingFileHandler(
        path,
        when="midnight",
        interval=1,
        backupCount=1,
        encoding="utf-8",
    )
    handler.suffix = "%Y-%m-%d"
    handler.setFormatter(
        logging.Formatter(
            "%(asctime)s %(levelname)s [%(threadName)s] %(message)s",
            "%Y-%m-%d %H:%M:%S",
        )
    )
    logger.addHandler(handler)
    _INITIALIZED = True
    logger.info("logging initialized pid=%s log=%s", os.getpid(), path)
    return logger


def get_logger() -> logging.Logger:
    return setup_logging()


def log_exception(message: str, exc: BaseException) -> None:
    logger = get_logger()
    logger.error("%s: %s\n%s", message, exc, "".join(traceback.format_exception(exc)))


def _sys_excepthook(exc_type, exc, tb) -> None:
    logger = get_logger()
    logger.error(
        "uncaught exception: %s\n%s",
        exc,
        "".join(traceback.format_exception(exc_type, exc, tb)),
    )
    sys.__excepthook__(exc_type, exc, tb)


def _threading_excepthook(args) -> None:
    logger = get_logger()
    logger.error(
        "uncaught thread exception thread=%s exc=%s\n%s",
        getattr(args.thread, "name", "unknown"),
        args.exc_value,
        "".join(
            traceback.format_exception(args.exc_type, args.exc_value, args.exc_traceback)
        ),
    )


def install_exception_hooks() -> None:
    sys.excepthook = _sys_excepthook
    if hasattr(threading, "excepthook"):
        threading.excepthook = _threading_excepthook


def install_qt_message_handler() -> None:
    try:
        from PyQt6.QtCore import qInstallMessageHandler
    except Exception:
        return

    def _qt_handler(_mode, _ctx, msg) -> None:
        get_logger().warning("qt: %s", msg)

    qInstallMessageHandler(_qt_handler)

