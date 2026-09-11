"""Locate FFmpeg for lightweight screen recording (LGPL open-source encoder)."""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

# Session cache: Path when found, False when missing, None when not searched.
_cached: Path | bool | None = None


def clear_ffmpeg_cache() -> None:
    """Drop cached path (tests / after install changes)."""
    global _cached
    _cached = None


def find_ffmpeg() -> Path | None:
    """Return path to ffmpeg executable, or None if not found.

    Result is cached for the process lifetime so hotkey start stays snappy
    (WinGet package scans can take seconds on a cold path).
    """
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

    found = _locate_ffmpeg()
    _cached = found if found is not None else False
    return found


def warm_ffmpeg_cache() -> None:
    """Resolve FFmpeg off the critical path (startup / idle)."""
    find_ffmpeg()


def _locate_ffmpeg() -> Path | None:
    candidates: list[Path] = []

    if getattr(sys, "frozen", False):
        # Installed/portable layout: ffmpeg beside DeskTidy.exe (not inside one-file bundle).
        candidates.append(
            Path(sys.executable).resolve().parent / "assets" / "ffmpeg" / "ffmpeg.exe"
        )

    candidates.append(_resource_base() / "assets" / "ffmpeg" / "ffmpeg.exe")

    # Also check the default Inno install dir (in case a portable/dev exe
    # is launched while the installed copy already has bundled FFmpeg).
    local = Path(os.environ.get("LOCALAPPDATA", "") or "")
    if local:
        candidates.append(local / "Programs" / "Desktidy" / "assets" / "ffmpeg" / "ffmpeg.exe")
        candidates.append(local / "Programs" / "DeskTidy" / "assets" / "ffmpeg" / "ffmpeg.exe")
        # Legacy Chinese install folder.
        candidates.append(
            local / "Programs" / "桌面整理（私人版）" / "assets" / "ffmpeg" / "ffmpeg.exe"
        )

    for candidate in candidates:
        if candidate.is_file():
            return candidate

    which = shutil.which("ffmpeg")
    if which:
        return Path(which)

    for candidate in (
        Path(os.environ.get("ProgramFiles", "")) / "ffmpeg" / "bin" / "ffmpeg.exe",
        Path(os.environ.get("ProgramFiles(x86)", "")) / "ffmpeg" / "bin" / "ffmpeg.exe",
        Path(os.environ.get("LOCALAPPDATA", "")) / "Microsoft" / "WinGet" / "Links" / "ffmpeg.exe",
    ):
        if candidate.is_file():
            return candidate

    # Version-agnostic WinGet package scan (folder name changes across releases).
    # Cap depth — recursive **/ffmpeg.exe under Packages can freeze the UI for seconds.
    winget_pkgs = (
        Path(os.environ.get("LOCALAPPDATA", ""))
        / "Microsoft"
        / "WinGet"
        / "Packages"
    )
    if winget_pkgs.is_dir():
        try:
            for pkg in winget_pkgs.glob("Gyan.FFmpeg*"):
                for exe in (
                    pkg.glob("ffmpeg-*/bin/ffmpeg.exe"),
                    pkg.glob("*/bin/ffmpeg.exe"),
                    pkg.glob("bin/ffmpeg.exe"),
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
