"""DeskTidy - Desktop Organization Tool"""

import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))


def _reexec_with_pythonw_if_needed() -> None:
    """Source runs under ``python.exe`` open a console (长方形黑框) then look hung.

    Other PCs often double-click ``main.py`` or type ``python main.py``. Re-launch
    with sibling ``pythonw.exe`` so startup matches the intended windowed mode.
    Opt out: ``DESKTIDY_ALLOW_CONSOLE=1`` (debug) or ``DESKTIDY_SELFTEST=1``.
    """
    if getattr(sys, "frozen", False):
        return
    if os.environ.get("DESKTIDY_SELFTEST") == "1":
        return
    if os.environ.get("DESKTIDY_ALLOW_CONSOLE") == "1":
        return
    if os.environ.get("DESKTIDY_PYTHONW_REEXEC") == "1":
        return
    exe = Path(sys.executable)
    if exe.name.lower() != "python.exe":
        return
    windowed = exe.with_name("pythonw.exe")
    if not windowed.is_file():
        return
    script = Path(sys.argv[0]).resolve() if sys.argv else Path(__file__).resolve()
    cmd = [str(windowed), "-u", str(script), *sys.argv[1:]]
    env = os.environ.copy()
    env["DESKTIDY_PYTHONW_REEXEC"] = "1"
    creationflags = 0
    try:
        creationflags = int(getattr(subprocess, "DETACHED_PROCESS", 0)) | int(
            getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        )
    except Exception:
        creationflags = 0
    try:
        subprocess.Popen(  # noqa: S603
            cmd,
            cwd=os.getcwd(),
            env=env,
            close_fds=True,
            creationflags=creationflags,
        )
    except OSError:
        return
    raise SystemExit(0)


def _sanitize_qt_platform() -> None:
    """Drop headless Qt platforms inherited from selftest / CI shells.

    ``QT_QPA_PLATFORM=offscreen`` makes the process run with no tray, no
    desktop overlays, and ``SetWindowPos`` 1400 errors — looks like「没启动」.
    Selftests that need offscreen set ``DESKTIDY_SELFTEST=1``.
    """
    if os.environ.get("DESKTIDY_SELFTEST") == "1":
        return
    plat = (os.environ.get("QT_QPA_PLATFORM") or "").strip().lower()
    if plat in {"offscreen", "minimal", "null"}:
        os.environ.pop("QT_QPA_PLATFORM", None)
        os.environ.pop("QT_QPA_PLATFORM_PLUGIN_PATH", None)


def _parse_shell_verb(argv: list[str]) -> str | None:
    for arg in argv[1:]:
        if arg.startswith("--shell-verb="):
            verb = arg.split("=", 1)[1].strip()
            return verb or None
        if arg == "--shell-verb" and len(argv) > argv.index(arg) + 1:
            # Support `--shell-verb name` form as well.
            idx = argv.index(arg)
            nxt = argv[idx + 1].strip() if idx + 1 < len(argv) else ""
            return nxt or None
    return None


def _bring_existing_window_to_front(*, title_substr: str | None = None) -> None:
    try:
        import ctypes
        from ctypes import wintypes

        from src.i18n import APP_NAME_ZH

        needle = title_substr or APP_NAME_ZH
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
            if needle in title:
                windows.append(int(hwnd))
            return True

        user32.EnumWindows(EnumWindowsProc(_enum), 0)
        if not windows:
            return
        hwnd = windows[0]
        SW_RESTORE = 9
        SW_SHOW = 5
        user32.ShowWindow(hwnd, SW_RESTORE)
        user32.ShowWindow(hwnd, SW_SHOW)
        user32.SetForegroundWindow(hwnd)
    except Exception:
        return


def _run_notepad_entry(logger) -> int:
    """Legacy DeskTidy.exe --notepad / open-notepad → standalone deskNote process."""
    from src.desknote_launch import is_desknote_installed, open_in_desknote, run_desknote_process

    # Prefer sibling deskNote.exe when frozen (true separate product).
    if getattr(sys, "frozen", False) and is_desknote_installed():
        logger.info("redirecting --notepad to deskNote.exe")
        if open_in_desknote():
            return 0
        logger.warning("deskNote launch failed; falling back to in-process notepad")

    logger.info("starting deskNote process (via DeskTidy --notepad compat)")
    return int(run_desknote_process(sys.argv[1:]) or 0)


def _entry() -> int:
    _reexec_with_pythonw_if_needed()
    _sanitize_qt_platform()

    # Elevated hotkey helper is a headless sidecar: no Qt, no logging, no mutex.
    if "--hotkey-helper" in sys.argv:
        from src.hotkey_helper import run_hotkey_helper

        return int(run_hotkey_helper(sys.argv) or 0)

    from src.app_logging import (
        get_logger,
        install_exception_hooks,
        install_qt_message_handler,
        setup_logging,
    )

    setup_logging()
    install_exception_hooks()
    install_qt_message_handler()
    logger = get_logger()
    logger.info("entry argv=%s", sys.argv)

    # Uninstall / silent-quit must run before Qt UI and instance mutex.
    if any(a in {"--uninstall-cleanup", "--quit-silent"} for a in sys.argv[1:]):
        from src.uninstall_cleanup import run_uninstall_cleanup

        logger.info("running uninstall cleanup argv=%s", sys.argv[1:])
        return int(run_uninstall_cleanup() or 0)

    if "--desktop-guard" in sys.argv:
        from src.desktop_guard import run_desktop_guard

        logger.info("starting desktop guard")
        return run_desktop_guard()

    from src.desknote import wants_notepad_cli

    if wants_notepad_cli(sys.argv):
        return _run_notepad_entry(logger)

    shell_verb = _parse_shell_verb(sys.argv)

    from src.instance_lock import AcquireResult, try_acquire_main_instance
    from src.win_app_id import DESKTIDY_AUMID, set_current_process_app_user_model_id

    # Before any Qt window: distinct from deskNote so taskbar icons stay separate
    # when both run under pythonw.exe.
    set_current_process_app_user_model_id(DESKTIDY_AUMID)

    acquire = try_acquire_main_instance()
    if acquire is AcquireResult.BUSY:
        if shell_verb:
            from src.shell_ipc import send_shell_verb_to_running_instance

            logger.info("forwarding shell verb to running instance: %s", shell_verb)
            if send_shell_verb_to_running_instance(shell_verb):
                return 0
            logger.warning("shell IPC deliver failed; bringing window to foreground")
        else:
            logger.info("existing instance detected; bringing window to foreground")
        _bring_existing_window_to_front()
        return 0
    if acquire is AcquireResult.FAILED:
        # Do not pretend another UI exists — continue with a warning so the
        # user is not locked out when the mutex API fails.
        logger.error(
            "single-instance mutex create failed; continuing without exclusivity"
        )

    from src.app import main

    logger.info("starting main application")
    return main(pending_shell_verb=shell_verb)


if __name__ == "__main__":
    sys.exit(_entry())
