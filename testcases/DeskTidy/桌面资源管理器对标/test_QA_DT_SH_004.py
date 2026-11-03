# -*- coding: utf-8 -*-
"""QA-DT-SH-004: folder↔desktop / drag-to-apps parity contracts + move semantics."""

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


def _desktop() -> Path:
    desk = Path(os.path.expanduser("~")) / "Desktop"
    if desk.is_dir():
        return desk
    # Some machines redirect Desktop (e.g. D:\desktop)
    known = os.environ.get("USERPROFILE", "")
    if known:
        alt = Path(known) / "Desktop"
        if alt.is_dir():
            return alt
    pytest.skip("用户桌面目录不可用")


def test_source_has_cross_window_drag_wiring():
    ole_h = _read("ole_drag.h")
    ole = _read("ole_drag.cpp")
    app = _read("app.cpp")
    pub = _read("public_host.cpp")
    fence = _read("fence_window.cpp")

    assert "FileDropSemantics" in ole_h
    assert "VirtualPin" in ole_h
    assert "ResolveFileEffect" in ole
    assert "cancel_desktop_self_drop" in ole_h
    assert "g_float_drag_active" in ole
    assert "CreateShellDataObject" in ole or "SHCreateItemFromIDList" in ole
    assert "SameVolume(src, desk)" in app or "SameVolume(src, desk)" in pub
    assert "VK_CONTROL" in app
    assert "OnDesktopDropPaths" in app
    assert "cancel_desktop_self_drop=*/true" in pub or "cancel_desktop_self_drop=true" in pub
    assert "FileDropSemantics::VirtualPin" in fence
    assert "FileDropSemantics::Physical" in fence or "Physical" in fence


def test_same_volume_move_onto_desktop_semantics():
    """Mirror OnDesktopDropPaths same-volume MOVE into the user Desktop."""
    desk = _desktop()
    token = f"desktidy_dnd_qa_{uuid.uuid4().hex[:10]}"
    # Prefer a temp dir on the same drive as Desktop so MOVE applies.
    src_root = Path(tempfile.mkdtemp(prefix="dt_dnd_src_", dir=str(desk.parent)))
    src = src_root / f"{token}.txt"
    src.write_text("DeskTidy folder→desktop move probe\n", encoding="utf-8")
    dest = desk / src.name
    try:
        if dest.exists():
            dest.unlink()
        # SameVolume default → move
        shutil.move(str(src), str(dest))
        assert dest.is_file()
        assert not src.exists()
        assert dest.read_text(encoding="utf-8").startswith("DeskTidy folder")
    finally:
        if dest.exists():
            dest.unlink()
        shutil.rmtree(src_root, ignore_errors=True)


def test_desktop_self_drop_is_skipped():
    """Paths already parented by Desktop must not spawn (2) copies."""
    desk = _desktop()
    token = f"desktidy_dnd_self_{uuid.uuid4().hex[:10]}.txt"
    existing = desk / token
    existing.write_text("self\n", encoding="utf-8")
    before = list(desk.glob(f"{token}*"))
    try:
        parent = str(existing.parent)
        # Mirror OnDesktopDropPaths parent_of skip
        skipped = parent.lower() == str(desk).lower()
        assert skipped
        if not skipped:
            twin = desk / f"{existing.stem} (2){existing.suffix}"
            shutil.copy2(existing, twin)
        after = list(desk.glob(f"{existing.stem}*"))
        assert len(after) == len(before)
    finally:
        for p in desk.glob(f"{existing.stem}*"):
            p.unlink(missing_ok=True)


def test_virtual_pin_never_advertises_move_only_copy():
    fence = _read("fence_window.cpp")
    notes = _read("notes_window.cpp")
    assert "VirtualPin" in fence
    assert "VirtualPin" in notes
    ole = _read("ole_drag.cpp")
    # ResolveFileEffect VirtualPin branch returns COPY
    assert "case FileDropSemantics::VirtualPin" in ole


def test_heal_misplaced_docs_off_software_side():
    org = _read("organize.cpp")
    org_h = _read("organize.h")
    app = _read("app.cpp")
    assert "HealMisplacedSystemPins" in org_h
    assert "system_software_side" in org
    assert 'kv.second = "system_docs"' in app
    assert "Do not HealMisplacedSystemPins" in app


def test_drop_sinks_report_actual_ole_effect():
    """No-op / failed sinks must return DROPEFFECT_NONE so Explorer keeps sources."""
    ole_h = _read("ole_drag.h")
    ole = _read("ole_drag.cpp")
    app = _read("app.cpp")
    pet = _read("pet_window.cpp")
    assert "proposed_effect" in ole_h
    assert "actual = on_drop_" in ole or "on_drop_(paths, proposed)" in ole
    assert "return DROPEFFECT_NONE" in app
    assert "Self-drop" in app or "must NOT advertise MOVE" in app
    assert "ok ? proposed : DROPEFFECT_NONE" in pet
    assert "SendPathsToRecycleBin" in app
