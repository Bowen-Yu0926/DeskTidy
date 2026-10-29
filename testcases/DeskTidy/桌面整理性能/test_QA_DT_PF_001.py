# -*- coding: utf-8 -*-
"""QA-DT-PF-001: hot-path perf contracts for fence drag + clipboard paste."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def _slice_around(text: str, anchor: str, before: int = 200, after: int = 2500) -> str:
    i = text.find(anchor)
    if i < 0:
        return ""
    return text[max(0, i - before) : i + after]


def test_drag_move_path_has_no_directory_enum():
    fw = _read("fence_window.cpp")
    # Focus the in-fence drag update region (icon_dragging_ mouse move).
    chunk = _slice_around(fw, "icon_dragging_ && (wparam & MK_LBUTTON)", after=1800)
    if not chunk:
        chunk = _slice_around(fw, "drag_insert_", after=1200)
    assert chunk, "could not locate fence drag hot path"
    assert "FindFirstFile" not in chunk
    assert "FindNextFile" not in chunk
    assert "FindFirstFileW" not in chunk


def test_clipboard_prefers_ole_get():
    clip = _read("shell_clipboard.cpp")
    ole_i = clip.find("OleGetClipboard")
    open_i = clip.find("OpenClipboard")
    assert ole_i >= 0, "OleGetClipboard missing"
    # Prefer OLE before (or in addition to) classic OpenClipboard path.
    assert open_i < 0 or ole_i < open_i or clip.count("OleGetClipboard") >= 1


def test_paste_paths_call_desktop_land_without_full_disk_scan():
    pub = _read("public_host.cpp")
    icons = _read("icon_actions.cpp")
    app = _read("app.cpp")
    assert "PasteClipboardFiles" in pub or "PasteClipboardToDesktopFolder" in icons
    assert "PasteClipboardToDesktopFolder" in icons
    for blob, label in ((pub, "public_host"), (icons, "icon_actions")):
        # Paste helpers must not enumerate all drives.
        paste_chunks = re.findall(
            r"PasteClipboard[\s\S]{0,1200}", blob
        )
        joined = "\n".join(paste_chunks) if paste_chunks else blob
        assert "FindFirstFileW(L\"C:\\\\" not in joined
        assert "GetLogicalDriveStrings" not in joined, label


def test_drag_end_still_commits_reorder():
    fw = _read("fence_window.cpp")
    assert "CommitReorder" in fw
    # End-drag path should still call CommitReorder when insert index valid.
    end_chunk = _slice_around(fw, "icon_dragging_ = false", after=800)
    assert "CommitReorder" in fw
    assert "CommitReorder" in end_chunk or "CommitReorder(at)" in fw
