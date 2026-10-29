# -*- coding: utf-8 -*-
"""QA-DT-FE-001: fence icon reorder + drag-out pin transfer contracts."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def test_manual_drag_and_commit_reorder_exist():
    fw = _read("fence_window.cpp")
    assert "CommitReorder" in fw
    assert "icon_dragging_" in fw
    assert "pin_order_" in fw
    assert "manual drag" in fw.lower() or "icon_dragging_ = true" in fw


def test_drag_out_hands_off_to_ole_pin_transfer():
    fw = _read("fence_window.cpp")
    assert "drag_left_" in fw
    # Leaving fence mid-drag should clear in-fence drag and start OLE path.
    assert "drag_left_ = true" in fw
    assert "icon_dragging_ = false" in fw
    ole = _read("ole_drag.cpp")
    assert "ExtractPinTransfer" in ole or "PinTransfer" in ole or "on_pin_drop_" in ole


def test_app_transfer_pins_and_pin_drop_wired():
    app = _read("app.cpp")
    assert "TransferPins" in app
    assert "OnPinDropped" in app
    assert "SetOnPinDrop" in app
    assert "SetOnReorder" in app


def test_portal_incoming_paths_distinct_from_plain_pin():
    app = _read("app.cpp")
    assert "HandleIncomingPaths" in app
    # Portal / import path must exist alongside pin transfer.
    assert "portal" in app.lower() or "Import" in app or "paste" in app
