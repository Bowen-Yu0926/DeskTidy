"""Notepad++-style elevated (UAC) saves for system-protected paths.

When saving a protected path (e.g. the hosts file) fails with
PermissionError, DeskNote offers to relaunch itself elevated. The elevated
instance then runs its *own* write code on the pending payload, so every
later save in that session also works — exactly how Notepad++ handles
protected files.
"""
from __future__ import annotations

import sys
from pathlib import Path

_ELEVATED_SAVE_ARG = "--elevated-save"


def _ps_quote(value: str) -> str:
    """Return *value* as a PowerShell single-quoted string literal."""
    return "'" + str(value).replace("'", "''") + "'"


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


def relaunch_desknote_elevated(target: Path | str) -> bool:
    """Ask UAC to relaunch DeskNote with ``--elevated-save <target>``.

    Returns False when the user declines UAC or the launch fails (the current
    process should then stay put). The elevated child starts asynchronously;
    we never wait for it to exit.
    """
    import base64
    import subprocess

    from src.app_logging import get_logger

    logger = get_logger()
    exe, script, cwd = _desknote_launch_parts()
    t = _ps_quote(str(target))
    if script:
        inner = (
            f"$t={t}; "
            f"Start-Process -FilePath {_ps_quote(exe)} "
            f"-WorkingDirectory {_ps_quote(cwd)} -Verb RunAs "
            f"-ArgumentList @({_ps_quote(script)},'{_ELEVATED_SAVE_ARG}',$t)"
        )
    else:
        inner = (
            f"$t={t}; "
            f"Start-Process -FilePath {_ps_quote(exe)} "
            f"-WorkingDirectory {_ps_quote(cwd)} -Verb RunAs "
            f"-ArgumentList @('{_ELEVATED_SAVE_ARG}',$t)"
        )
    logger.info("elevated relaunch: exe=%r script=%r cwd=%r target=%r",
                exe, script, cwd, str(target))
    logger.debug("elevated relaunch inner ps: %s", inner)
    encoded = base64.b64encode(inner.encode("utf-16-le")).decode("ascii")
    try:
        proc = subprocess.run(
            ["powershell.exe", "-NoProfile", "-EncodedCommand", encoded],
            creationflags=subprocess.CREATE_NO_WINDOW,
            timeout=60,
        )
        logger.info("elevated relaunch powershell rc=%s", proc.returncode)
        return proc.returncode == 0
    except (OSError, subprocess.TimeoutExpired) as exc:
        logger.warning("elevated relaunch failed: %r", exc)
        return False
    except Exception:
        logger.exception("elevated relaunch unexpected error")
        return False