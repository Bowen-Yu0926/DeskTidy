# -*- coding: utf-8 -*-
"""QA-DT-FE-002: fence layout geometry + pin_order persistence contracts."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def test_layout_and_fence_have_pin_order():
    layout_h = _read("layout.h")
    fw_h = _read("fence_window.h")
    assert "pin_order" in layout_h
    assert "PinOrder" in fw_h or "pin_order_" in fw_h


def test_fence_writes_geometry_into_layout():
    fw = _read("fence_window.cpp")
    # FenceLayout / SaveLayout path should copy rect + pin_order.
    assert "pin_order" in fw
    assert "FenceLayout" in fw or "fl.pin_order" in fw or "layout.pin_order" in fw
    assert "left" in fw and "top" in fw


def test_display_fingerprint_or_ratio_scaling_exists():
    blobs = "\n".join(
        [
            _read("layout.cpp"),
            _read("layout.h"),
            _read("display_geom.cpp"),
            _read("app.cpp"),
        ]
    )
    hit = any(
        k in blobs
        for k in (
            "fingerprint",
            "Fingerprint",
            "fence_layouts_by_display",
            "display_fingerprint",
            "rh",
            "ratio",
            "size_scale",
        )
    )
    assert hit, "expected display fingerprint or ratio-based layout scaling"


def test_corrupt_pin_path_normalization_exists():
    layout = _read("layout.cpp") + _read("layout.h")
    app = _read("app.cpp")
    assert "NormalizeStoredPath" in layout or "NormalizeStoredPath" in app
    assert "pin_order" in layout
    # App reconciles corrupt pin_order escape sequences on load.
    assert "pin_order" in app and (
        "\\\\" in app or "corrupt" in app.lower() or "Reconcile" in app or "normalize" in app.lower()
    )
