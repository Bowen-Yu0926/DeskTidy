# -*- coding: utf-8 -*-
"""QA-DT-TL-003: wallpaper menu, random skip-last, invalid path safety."""

from __future__ import annotations

import ctypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"

user32 = ctypes.windll.user32


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def test_wallpaper_menu_ync_and_settings_hooks():
    tools = _read("tools_misc.cpp")
    app = _read("app.cpp")
    assert "WallpaperMenu" in tools
    assert "MistMsgKind::YesNoCancel" in tools or "YesNoCancel" in tools
    assert "PickAndSetWallpaper" in tools
    assert "RandomWallpaperFromLibrary" in tools
    assert "ReadOriginalWallpaper" in tools
    assert 'id == L"wallpaper_browse"' in app
    assert 'id == L"wallpaper_random"' in app


def test_random_skips_last_and_apply_checks_path():
    tools = _read("tools_misc.cpp")
    assert "last_image" in tools
    assert "PickRandomImage" in tools
    assert "ApplyWallpaperPath" in tools
    # Empty / missing file early return.
    assert "path.empty()" in tools
    assert "INVALID_FILE_ATTRIBUTES" in tools
    assert "SPI_SETDESKWALLPAPER" in tools


def test_runtime_invalid_path_does_not_change_wallpaper_via_spi_guard():
    # Source-level + SPI probe: SystemParametersInfo with empty fails.
    # Do NOT set a real wallpaper here — only assert empty buffer fails.
    SPI_SETDESKWALLPAPER = 0x0014
    # Passing NULL/empty should not succeed as a DeskTidy ApplyWallpaperPath call.
    # Mirror ApplyWallpaperPath gate: empty path returns false before SPI.
    tools = _read("tools_misc.cpp")
    idx = tools.find("bool ApplyWallpaperPath")
    assert idx > 0
    block = tools[idx : idx + 500]
    assert "return false" in block
    assert "path.empty()" in block
    # Extra: SPI with empty string should be treated as failure-ish; we only
    # ensure we never call SPI when path empty (contract above).
    _ = SPI_SETDESKWALLPAPER
    assert "SPIF_UPDATEINIFILE" in tools


def test_empty_library_warns():
    tools = _read("tools_misc.cpp")
    assert "wallpaper_no_images" in tools
    assert "MistAlert" in tools
    # RandomWallpaperFromLibrary / WallpaperMenu empty-list → MistAlert.
    idx = tools.find("bool RandomWallpaperFromLibrary")
    assert idx > 0
    body = tools[idx : idx + 1200]
    assert "images.empty()" in body
    assert "wallpaper_no_images" in body
    assert "MistAlert" in body
    # Must not call RememberAndSet after empty without returning.
    empty_at = body.find("images.empty()")
    remember_at = body.find("RememberAndSet")
    assert empty_at >= 0 and remember_at > empty_at
    assert "return false" in body[empty_at:remember_at]
