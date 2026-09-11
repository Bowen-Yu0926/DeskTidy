"""Locate sharkdp/fd for lightweight filename search (MIT/Apache-2.0)."""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

# Session cache: Path when found, False when missing, None when not searched.
_cached: Path | bool | None = None


def clear_fd_cache() -> None:
    """Drop cached path (tests / after install changes)."""
    global _cached
    _cached = None


def find_fd() -> Path | None:
    """Return path to fd executable, or None if not found."""
    global _cached
    if _cached is False:
        return None
    if isinstance(_cached, Path):
        try:
            if _cached.is_file():
                return _cached
        except OSError:
            pass
        _cached = None

    found = _locate_fd()
    _cached = found if found is not None else False
    return found


def warm_fd_cache() -> None:
    """Resolve fd off the critical path (startup / idle)."""
    find_fd()


def _locate_fd() -> Path | None:
    candidates: list[Path] = []

    if getattr(sys, "frozen", False):
        candidates.append(
            Path(sys.executable).resolve().parent / "assets" / "fd" / "fd.exe"
        )

    candidates.append(_resource_base() / "assets" / "fd" / "fd.exe")

    local = Path(os.environ.get("LOCALAPPDATA", "") or "")
    if local:
        candidates.append(local / "Programs" / "Desktidy" / "assets" / "fd" / "fd.exe")
        candidates.append(local / "Programs" / "DeskTidy" / "assets" / "fd" / "fd.exe")
        candidates.append(
            local / "Programs" / "桌面整理（私人版）" / "assets" / "fd" / "fd.exe"
        )

    for candidate in candidates:
        if candidate.is_file():
            return candidate

    which = shutil.which("fd") or shutil.which("fd.exe")
    if which:
        return Path(which)

    for candidate in (
        Path(os.environ.get("ProgramFiles", "")) / "fd" / "fd.exe",
        Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "WinGet" / "Links" / "fd.exe",
    ):
        if candidate.is_file():
            return candidate

    winget_pkgs = (
        Path(os.environ.get("LOCALAPPDATA", ""))
        / "Microsoft"
        / "WinGet"
        / "Packages"
    )
    if winget_pkgs.is_dir():
        try:
            for pkg in winget_pkgs.glob("sharkdp.fd*"):
                for exe in (
                    pkg.glob("fd.exe"),
                    pkg.glob("*/fd.exe"),
                ):
                    for path in exe:
                        if path.is_file():
                            return path
        except OSError:
            pass
    return None


def _resource_base() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys._MEIPASS)
    return Path(__file__).resolve().parent.parent
