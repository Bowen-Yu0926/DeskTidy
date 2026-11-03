# -*- coding: utf-8 -*-
"""QA-DT-PUB-003: public float marquee OLE drag contracts."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def _fn(src: str, sig: str, span: int = 2200) -> str:
    i = src.find(sig)
    assert i >= 0, sig
    return src[i : i + span]


def test_public_begin_ole_cancels_desktop_self_drop():
    host = _read("public_host.cpp")
    body = _fn(host, "void PublicHost::BeginOleDrag()", 2000)
    assert "ShellDoDragFiles" in body
    assert "cancel_desktop_self_drop" in body
    assert "true" in body[body.find("ShellDoDragFiles") : body.find("ShellDoDragFiles") + 120]


def test_drop_back_on_public_treated_as_cancel():
    host = _read("public_host.cpp")
    body = _fn(host, "void PublicHost::BeginOleDrag()", 2400)
    assert "WindowFromPoint" in body
    assert "dropped = false" in body.replace(" ", "") or "dropped = false" in body
    assert "marquee_hwnd_" in body


def test_fence_shell_drag_does_not_force_cancel_flag():
    fw = _read("fence_window.cpp")
    # Fence drag uses ShellDoDragFiles(paths) without forcing cancel=true.
    assert "ShellDoDragFiles(paths)" in fw or "ShellDoDragFiles(paths);" in fw
    # Public path must still uniquely request cancel=true
    host = _read("public_host.cpp")
    assert "cancel_desktop_self_drop=*/true" in host or "cancel_desktop_self_drop = true" in host
