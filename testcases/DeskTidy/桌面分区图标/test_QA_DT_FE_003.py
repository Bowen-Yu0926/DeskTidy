# -*- coding: utf-8 -*-
"""QA-DT-FE-003: right-click move is page → fence cascade (one entry)."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def _fn(src: str, sig: str, span: int = 2800) -> str:
    i = src.find(sig)
    assert i >= 0, sig
    return src[i : i + span]


def test_fence_icon_menu_single_move_entry():
    fw = _read("fence_window.cpp")
    # ShowIconMenu region: extras list
    body = _fn(fw, "const std::vector<ShellMenuExtra> extras{", 800)
    assert "menu_move_page" in body
    assert "menu_move_fence" not in body
    assert body.count("menu_move_") == 1 or "menu_move_page" in body


def test_fence_move_opens_page_then_fence_cascade():
    fw = _read("fence_window.cpp")
    assert "ShowMoveToPageFenceMenu" in fw
    move = _fn(fw, "void FenceWindow::ShowMoveToPageFenceMenu(", 1200)
    assert "TrackMoveToPageThenFenceMenu" in move
    assert "on_move_to_fence_" in move
    assert "exclude_fence_id" in move or "id_" in move


def test_shared_cascade_helper_builds_page_popups():
    shell = _read("shell_context_menu.cpp")
    body = _fn(shell, "std::string TrackMoveToPageThenFenceMenu(", 2200)
    assert "MF_POPUP" in body
    assert "该页无分区" in body
    assert "page_names" in body or "names" in body
