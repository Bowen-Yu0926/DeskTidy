# -*- coding: utf-8 -*-
"""QA-DT-CAP-001: capture Esc hook + record exclude/session live."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def test_capture_esc_hook_install_uninstall():
    cap = _read("capture_overlay.cpp")
    assert "CaptureEscHookProc" in cap
    assert "InstallCaptureEscHook" in cap
    assert "UninstallCaptureEscHook" in cap
    assert "WH_KEYBOARD_LL" in cap
    assert "VK_ESCAPE" in cap


def test_record_exclude_from_capture():
    rec = _read("screen_record.cpp")
    # WDA_EXCLUDEFROMCAPTURE or SetWindowDisplayAffinity
    assert (
        "WDA_EXCLUDEFROMCAPTURE" in rec
        or "SetWindowDisplayAffinity" in rec
        or "ExcludeOverlayFromCapture" in rec
        or "ExcludeFromCapture" in rec
    )


def test_screen_record_session_live_api():
    hdr = _read("screen_record.h")
    src = _read("screen_record.cpp")
    assert "ScreenRecordSessionLive" in hdr
    assert "bool ScreenRecordSessionLive()" in src
    i = src.find("bool ScreenRecordSessionLive()")
    chunk = src[i : i + 200]
    assert "g_proc" in chunk and "g_paused" in chunk
