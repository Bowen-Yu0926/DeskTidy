# -*- coding: utf-8 -*-
"""QA-DT-SH-003: public float copy/cut + drop target now present."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"
DOCS = ROOT / "docs"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def test_public_float_has_copy_cut_hotkeys():
    pub = _read(NATIVE / "public_host.cpp")
    assert "CopySelection" in pub
    assert "CutSelection" in pub
    assert "L'C'" in pub
    assert "L'X'" in pub
    assert "ClipboardSetFiles" in pub


def test_public_float_has_drop_target_wired():
    pub = _read(NATIVE / "public_host.cpp")
    app = _read(NATIVE / "app.cpp")
    assert "RegisterPathDropTarget" in pub
    assert "EnsureDropTarget" in pub
    assert "PathDropTarget" in pub
    assert "OnDesktopDropPaths" in app
    assert "SetOnPathsDropped" in app


def test_docs_state_virtual_pin_vs_explorer_copy():
    text = _read(DOCS / "cpp-parity-gaps.md")
    assert "钉选" in text or "虚拟钉选" in text
    assert "不复制" in text or "只钉选" in text
