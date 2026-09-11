"""Launch / focus the standalone deskNote process and run its UI entry."""

from __future__ import annotations

import os
import subprocess
import sys
import time
from pathlib import Path

from src.desknote_ipc import (
    desknote_ipc_window_exists,
    send_to_running_desknote,
    start_desknote_ipc_host,
)
from src.instance_lock import (
    AcquireResult,
    is_notepad_instance_running,
    release_notepad_instance,
    try_acquire_notepad_instance,
)

OPEN_VERB = "open"
OPEN_FILES_VERB = "open-files"


def desknote_exe_path() -> Path:
    """Frozen: sibling deskNote.exe; source: desknote_main.py next to main.py."""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent / "deskNote.exe"
    return Path(__file__).resolve().parent.parent / "desknote_main.py"


def is_desknote_installed() -> bool:
    """True when deskNote can be launched (always in source; exe present when frozen)."""
    if not getattr(sys, "frozen", False):
        return True
    return desknote_exe_path().is_file()


def _launch_command(paths: list[Path | str] | None = None) -> list[str]:
    clean = [str(Path(p)) for p in (paths or []) if p]
    target = desknote_exe_path()
    if getattr(sys, "frozen", False):
        return [str(target), *clean]
    # Prefer pythonw so DeskTidy→deskNote does not flash a console.
    exe = Path(sys.executable)
    windowed = exe.with_name("pythonw.exe")
    launcher = str(windowed if windowed.is_file() else exe)
    return [launcher, "-u", str(target), *clean]


def _bring_desknote_window_to_front() -> None:
    try:
        import ctypes
        from ctypes import wintypes

        user32 = ctypes.windll.user32
        windows: list[int] = []
        EnumWindowsProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

        def _enum(hwnd, _lparam):
            if not user32.IsWindowVisible(hwnd):
                return True
            length = user32.GetWindowTextLengthW(hwnd)
            if length <= 0:
                return True
            buf = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, buf, len(buf))
            title = buf.value or ""
            if "desknote" in title.casefold() or "笔记" in title:
                windows.append(int(hwnd))
            return True

        user32.EnumWindows(EnumWindowsProc(_enum), 0)
        if not windows:
            return
        hwnd = windows[0]
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
        user32.ShowWindow(hwnd, 5)  # SW_SHOW
        user32.SetForegroundWindow(hwnd)
    except Exception:
        return


def open_in_desknote(paths: list[Path | str] | None = None) -> bool:
    """Focus running deskNote or start it; optionally open filesystem paths.

    Returns False when deskNote is not installed (frozen without deskNote.exe).
    """
    if not is_desknote_installed():
        return False
    clean = [str(Path(p)) for p in (paths or []) if p]
    if is_notepad_instance_running() or desknote_ipc_window_exists():
        verb = OPEN_FILES_VERB if clean else OPEN_VERB
        if send_to_running_desknote(verb, clean):
            return True
        _bring_desknote_window_to_front()
        return True

    cmd = _launch_command(clean)
    try:
        creationflags = 0
        if sys.platform == "win32":
            # Avoid console window when launching from DeskTidy.
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
            if cmd and Path(cmd[0]).name.lower() == "pythonw.exe":
                creationflags = 0
        subprocess.Popen(  # noqa: S603
            cmd,
            cwd=str(desknote_exe_path().parent),
            close_fds=True,
            creationflags=creationflags,
            env=os.environ.copy(),
        )
    except OSError:
        return False

    # Brief wait so a follow-up IPC from the same click path can succeed.
    for _ in range(40):
        if desknote_ipc_window_exists() or is_notepad_instance_running():
            if clean:
                send_to_running_desknote(OPEN_FILES_VERB, clean)
            return True
        time.sleep(0.05)
    return True


def _parse_cli_paths(argv: list[str]) -> list[Path]:
    out: list[Path] = []
    for arg in argv:
        if not arg or arg.startswith("-"):
            continue
        p = Path(arg)
        try:
            if p.exists() and p.is_file():
                out.append(p.resolve())
        except OSError:
            continue
    return out


def run_desknote_process(argv: list[str] | None = None) -> int:
    """Single-instance deskNote UI; argv may contain file paths to open."""
    from src.app_logging import get_logger

    logger = get_logger()
    args = list(argv if argv is not None else sys.argv[1:])
    paths = _parse_cli_paths(args)

    acquire = try_acquire_notepad_instance()
    if acquire is AcquireResult.BUSY:
        logger.info("deskNote already running; forwarding via IPC")
        verb = OPEN_FILES_VERB if paths else OPEN_VERB
        if send_to_running_desknote(verb, [str(p) for p in paths]):
            return 0
        _bring_desknote_window_to_front()
        return 0
    if acquire is AcquireResult.FAILED:
        logger.error("deskNote mutex failed; continuing without exclusivity")

    try:
        return _run_ui(paths)
    finally:
        release_notepad_instance()


def _run_ui(initial_paths: list[Path]) -> int:
    from PyQt6.QtCore import Qt
    from PyQt6.QtWidgets import QApplication

    from src.icon_utils import get_desknote_icon
    from src.i18n import setup_chinese
    from src.notepad_app import attach_desknote_ipc
    from src.settings import load_settings
    from src.ui.notepad_window import show_notepad
    from src.ui.styles import build_stylesheet, get_theme_palette, normalize_theme
    from src.win_app_id import DESKNOTE_AUMID, set_current_process_app_user_model_id

    # Must run before QApplication / top-level windows (taskbar identity).
    set_current_process_app_user_model_id(DESKNOTE_AUMID)

    try:
        from src.desknote_open_with import sync_desknote_open_with

        sync_desknote_open_with(enabled=True)
    except Exception:
        pass

    qt_app = QApplication.instance() or QApplication(sys.argv)
    qt_app.setQuitOnLastWindowClosed(True)
    setup_chinese(qt_app)
    qt_app.setApplicationName("DeskNote")

    settings = load_settings()
    theme = normalize_theme(settings.get("theme"))
    settings["theme"] = theme
    palette = get_theme_palette(theme)
    qt_app.setStyleSheet(build_stylesheet(theme))
    icon = get_desknote_icon()
    if not icon.isNull():
        qt_app.setWindowIcon(icon)

    win = show_notepad(settings, parent=None)
    win.setWindowTitle("DeskNote")
    win.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, True)
    win.destroyed.connect(qt_app.quit)

    attach_desknote_ipc(win)
    if initial_paths:
        try:
            win.open_paths(initial_paths)
        except Exception:
            pass

    return int(qt_app.exec())
