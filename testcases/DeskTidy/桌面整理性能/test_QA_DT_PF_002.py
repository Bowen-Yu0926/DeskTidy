# -*- coding: utf-8 -*-
"""QA-DT-PF-002: icon reuse, sticky prune one-scan, record/trim throttle."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def test_fence_setitems_reuses_icons_and_short_circuits():
    fw = _read("fence_window.cpp")
    i = fw.find("void FenceWindow::SetItems")
    assert i > 0
    chunk = fw[i : i + 3500]
    assert "reuse" in chunk
    assert "LoadItemIcon" in chunk
    assert "DesktopItemListsIdentityEqual" in chunk
    assert "loaded_any" in chunk


def test_public_setitems_reuses_but_still_layouts():
    pub = _read("public_host.cpp")
    i = pub.find("void PublicHost::SetItems")
    assert i > 0
    chunk = pub[i : i + 2800]
    assert "reuse" in chunk
    assert "DesktopItemListsIdentityEqual" in chunk
    # Early path must still Layout (blocked fence rects).
    assert chunk.count("Layout()") >= 2


def test_sticky_prune_scans_desktop_once():
    app = _read("app.cpp")
    i = app.find("void App::PruneStickyMissingPins")
    assert i > 0
    chunk = app[i : i + 4500]
    assert "desk_scan_ready" in chunk
    assert "desk_items" in chunk
    assert chunk.count("InvalidateDesktopScanCache()") == 1


def test_record_poll_gated_and_trim_when_hidden():
    app = _read("app.cpp")
    assert "ScreenRecordSessionLive()" in app
    # Pet timer path should gate IsScreenRecording.
    pet = app.find("kTimerPet")
    assert pet > 0
    # Search near WM_TIMER pet handling
    idx = app.find("ScreenRecordSessionLive()")
    assert idx > 0
    window = app[max(0, idx - 200) : idx + 200]
    assert "IsScreenRecording()" in window

    trim_i = app.find("void App::TrimMemory")
    assert trim_i > 0
    trim = app[trim_i : trim_i + 900]
    assert "fences_hidden_" in trim
    assert "SetProcessWorkingSetSize" in trim
    # Must return early when fences visible / settings open.
    assert "return;" in trim
    assert trim.find("return;") < trim.find("SetProcessWorkingSetSize")
