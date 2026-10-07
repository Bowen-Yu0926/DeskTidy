"""Notepad++-style elevated (UAC) saves for system-protected paths.

When saving a protected path (e.g. the hosts file) fails with
PermissionError, DeskNote offers to relaunch itself elevated. The elevated
instance then runs its *own* write code on the pending payload, so every
later save in that session also works — exactly how Notepad++ handles
protected files.

The relaunch goes through ``ShellExecuteW("runas")`` rather than spawning
``powershell.exe -Verb RunAs``: the shell handles the UAC prompt natively,
so no interpreter has to be launched first (CreateProcess for powershell is
denied for the frozen GUI build on some machines, which silently skipped the
whole handoff).
"""
from __future__ import annotations

import sys
from pathlib import Path

_ELEVATED_SAVE_ARG = "--elevated-save"
_SW_SHOWNORMAL = 1
# ShellExecuteW returns an HINSTANCE on success; SE_ERR_* failures are <= 32.
_SE_SUCCESS_THRESHOLD = 32


def _desknote_launch_parts() -> tuple[str, str, str]:
    """``(exe, script_or_empty, working_dir)`` for a deskNote relaunch.

    Frozen: the installed ``deskNote.exe``. Source: pythonw.exe running
    ``desknote_main.py`` next to this repo root.
    """
    from src.desknote_launch import desknote_exe_path

    if getattr(sys, "frozen", False):
        exe = desknote_exe_path()
        return str(exe), "", str(exe.parent)
    exe = Path(sys.executable)
    windowed = exe.with_name("pythonw.exe")
    launcher = str(windowed if windowed.is_file() else exe)
    script = desknote_exe_path()  # <repo root>/desknote_main.py
    return launcher, str(script), str(script.parent)


def _shell_execute_runas(file_: str, params: str, cwd: str) -> int:
    """``ShellExecuteW(None, "runas", ...)``; > 32 means the shell accepted it."""
    import ctypes
    import ctypes.wintypes as wt

    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    shell32.ShellExecuteW.argtypes = [
        wt.HWND,
        wt.LPCWSTR,
        wt.LPCWSTR,
        wt.LPCWSTR,
        wt.LPCWSTR,
        wt.INT,
    ]
    shell32.ShellExecuteW.restype = ctypes.c_ssize_t
    handle = shell32.ShellExecuteW(None, "runas", file_, params, cwd, _SW_SHOWNORMAL)
    return int(handle or 0)


def relaunch_desknote_elevated(target: Path | str) -> bool:
    """Ask UAC to relaunch DeskNote with ``--elevated-save <target>``.

    Returns False when the user declines UAC or the shell refuses the launch
    (the current process should then stay put). The elevated child starts
    asynchronously; we never wait for it to exit.
    """
    from src.app_logging import get_logger

    logger = get_logger()
    exe, script, cwd = _desknote_launch_parts()
    params = (
        f'"{script}" {_ELEVATED_SAVE_ARG} "{target}"'
        if script
        else f'{_ELEVATED_SAVE_ARG} "{target}"'
    )
    logger.info(
        "elevated relaunch: verb=runas file=%r params=%r cwd=%r", exe, params, cwd
    )
    try:
        code = _shell_execute_runas(exe, params, cwd)
        logger.info("elevated relaunch ShellExecuteW rc=%s", code)
        return code > _SE_SUCCESS_THRESHOLD
    except OSError as exc:
        logger.warning(
            "elevated relaunch failed: errno=%s winerror=%s msg=%s",
            getattr(exc, "errno", None),
            getattr(exc, "winerror", None),
            exc,
        )
        return False
    except Exception:
        logger.exception("elevated relaunch failed")
        return False
