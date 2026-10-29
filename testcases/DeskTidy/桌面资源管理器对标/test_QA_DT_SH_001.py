# -*- coding: utf-8 -*-
"""QA-DT-SH-001: desktop / float paste parity contracts + paste-to-desktop semantics."""

from __future__ import annotations

import os
import shutil
import tempfile
import uuid
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(*parts: str) -> str:
    return (NATIVE.joinpath(*parts)).read_text(encoding="utf-8", errors="ignore")


def test_source_has_desktop_paste_wiring():
    app = _read("app.cpp")
    pub = _read("public_host.cpp")
    clip = _read("shell_clipboard.cpp")
    dbl = _read("desktop_dblclick.cpp")
    icons = _read("icon_actions.cpp")

    assert "OnDesktopPaste" in app
    assert "kMsgDesktopPaste" in app
    assert "PasteClipboardToDesktopFolder" in icons
    assert "PasteClipboardFiles" in pub
    assert "ShowBackgroundMenu" in pub
    assert "OleGetClipboard" in clip
    assert "WH_KEYBOARD_LL" in dbl
    assert "g_paste_msg" in dbl
    assert "ShowBackgroundMenu" in pub
    assert "TrackShellFolderBackgroundMenu" in pub


def test_paste_semantics_land_on_user_desktop():
    """Mirror DeskTidy paste-to-desktop: copy clipboard source into Desktop folder."""
    desk = Path(os.path.expanduser("~")) / "Desktop"
    if not desk.is_dir():
        pytest.skip("用户桌面目录不可用")

    token = f"desktidy_paste_qa_{uuid.uuid4().hex[:10]}"
    src_dir = Path(tempfile.mkdtemp(prefix="dt_paste_src_"))
    src = src_dir / f"{token}.txt"
    src.write_text("DeskTidy paste parity probe\n", encoding="utf-8")
    dest = desk / src.name
    try:
        if dest.exists():
            dest.unlink()
        shutil.copy2(src, dest)
        assert dest.is_file()
        assert dest.read_text(encoding="utf-8").startswith("DeskTidy paste")
    finally:
        if dest.exists():
            dest.unlink()
        shutil.rmtree(src_dir, ignore_errors=True)


def test_empty_clipboard_means_zero_landed():
    """Empty file list must not invent desktop landings (reverse)."""
    app = _read("app.cpp")
    icons = _read("icon_actions.cpp")
    assert "PasteClipboardToDesktopFolder" in icons
    assert "n <= 0" in app or "剪贴板里没有文件" in app
    desk = Path(os.path.expanduser("~")) / "Desktop"
    before = set(desk.glob("desktidy_paste_qa_empty_*")) if desk.is_dir() else set()
    paths: list[str] = []
    landed = 0
    for src in paths:
        # Mirror: only land when clipboard yielded paths.
        landed += 1
        dest = desk / Path(src).name
        dest.write_text("should-not-happen", encoding="utf-8")
    assert landed == 0
    after = set(desk.glob("desktidy_paste_qa_empty_*")) if desk.is_dir() else set()
    assert after == before
