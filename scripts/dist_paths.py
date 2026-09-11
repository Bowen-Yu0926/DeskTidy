"""Locate the packaged DeskTidy.exe (onedir first, legacy onefile fallback)."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def packaged_exe(root: Path | None = None) -> Path:
    base = root if root is not None else ROOT
    onedir = base / "dist" / "DeskTidy" / "DeskTidy.exe"
    if onedir.is_file():
        return onedir
    return base / "dist" / "DeskTidy.exe"
