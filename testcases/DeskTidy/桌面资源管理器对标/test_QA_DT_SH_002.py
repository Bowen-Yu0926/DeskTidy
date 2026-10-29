# -*- coding: utf-8 -*-
"""QA-DT-SH-002: fence + public float Explorer-like operation contracts."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def test_fence_has_explorer_ops():
    fw = _read("fence_window.cpp")
    needed = [
        "CopySelection",
        "CutSelection",
        "PasteClipboardFiles",
        "SelectAll",
        "DeleteSelection",
        "BeginInlineRename",
        "VK_DELETE",
        "VK_F2",
        "TrackShellContextMenu",
        "TrackShellFolderBackgroundMenu",
        "rubber_",
        "RegisterPathDropTarget",
    ]
    missing = [n for n in needed if n not in fw]
    assert missing == [], f"分区缺少资源管理器入口: {missing}"


def test_public_float_has_explorer_ops():
    pub = _read("public_host.cpp")
    needed = [
        "WM_LBUTTONDBLCLK",
        "DeleteSelection",
        "RenameSelection",
        "OpenSelection",
        "PasteClipboardFiles",
        "CopySelection",
        "CutSelection",
        "ShowSlotMenu",
        "ShowBackgroundMenu",
        "TrackShellContextMenu",
        "TrackShellFolderBackgroundMenu",
        "RegisterPathDropTarget",
        "rubber_",
        "VK_DELETE",
        "VK_F2",
        "L'C'",
        "L'X'",
        "L'V'",
        "L'A'",
    ]
    missing = [n for n in needed if n not in pub]
    assert missing == [], f"浮动层缺少资源管理器入口: {missing}"


def test_drop_into_fence_wired_in_app():
    app = _read("app.cpp")
    ole = _read("ole_drag.cpp")
    assert "RegisterPathDropTarget" in _read("fence_window.cpp")
    assert "OnPathsDropped" in app or "HandleIncomingPaths" in app
    assert "PathDropTarget" in ole
    # Virtual pin vs portal import both go through HandleIncomingPaths.
    assert "HandleIncomingPaths" in app
    assert "PortalPath" in app or "portal" in app.lower()
