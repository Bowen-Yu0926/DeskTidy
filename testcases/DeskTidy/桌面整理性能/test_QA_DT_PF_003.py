# -*- coding: utf-8 -*-
"""QA-DT-PF-003: layer buffer reuse, lean scan, gated restore Present."""

from __future__ import annotations

import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def _fn(src: str, sig: str, span: int = 4000) -> str:
    i = src.find(sig)
    assert i >= 0, sig
    return src[i : i + span]


def test_fence_present_reuses_layer_buffer():
    fw = _read("fence_window.cpp")
    assert "bool FenceWindow::EnsureLayerBuffer" in fw
    assert "void FenceWindow::ReleaseLayerBuffer" in fw
    present = _fn(fw, "void FenceWindow::PresentLayered()", 5200)
    assert "EnsureLayerBuffer" in present
    # Must not free the pooled dib at the end of every Present.
    tail = present[present.find("UpdateOverlayLayered") :]
    assert "DeleteObject(dib)" not in tail
    assert "DeleteDC(mem)" not in tail
    destroy = _fn(fw, "void FenceWindow::Destroy()", 800)
    assert "ReleaseLayerBuffer" in destroy


def test_public_present_reuses_buffer():
    pub = _read("public_host.cpp")
    assert "EnsurePresentBuffer" in pub
    present = _fn(pub, "void PublicHost::Present()", 2200)
    assert "EnsurePresentBuffer" in present
    assert "present_dc_" in present
    tail = present[present.find("UpdateOverlayLayered") :]
    assert "DeleteObject(dib)" not in tail


def test_scan_skips_shell_displayname_for_normal_files():
    items = _read("desktop_items.cpp")
    append = _fn(items, "void AppendFolder(", 2800)
    assert "item.is_link" in append
    assert "SHGetFileInfoW" in append
    # SHGetFileInfo only inside is_link branch
    link_block = append[append.find("if (item.is_link)") :]
    assert "SHGetFileInfoW" in link_block
    before = append[: append.find("if (item.is_link)")]
    assert "SHGetFileInfoW" not in before
    scan = _fn(items, "std::vector<DesktopItem> ScanDesktopItems()", 2200)
    assert "unordered_set" in scan
    assert "ScanDedupKey" in scan


def test_restore_timer_gates_refresh_layer():
    app = _read("app.cpp")
    body = _fn(app, "if (wparam == kTimerRestore)", 1600)
    assert "LayerDirty()" in body
    assert "DesktopChromeHealthy()" in body or "unhealthy" in body
    assert "IsOverlayAboveDefView" in body
    # Must not RefreshLayer with only IsWindowVisible gate
    assert "fence->RefreshLayer()" in body
    # Conditional path: RefreshLayer appears after a dirty/unhealthy check
    refresh_i = body.find("fence->RefreshLayer()")
    gate = body[:refresh_i]
    assert "LayerDirty" in gate or "unhealthy" in gate


def test_microbench_scan_desktop_cache_hit_is_fast():
    """Runtime: second Scan within TTL should be near-instant via cache.

    We can't call C++ ScanDesktopItems from pytest; instead assert the TTL
    constant and that the cache early-return still exists (contract).
    Optional: time a Python-side FindFirst of the same folders as a floor.
    """
    items = _read("desktop_items.cpp")
    assert "kScanCacheTtlMs = 2500" in items
    scan = _fn(items, "std::vector<DesktopItem> ScanDesktopItems()", 900)
    assert "g_scan_cache" in scan
    assert "g_scan_tick" in scan
    # Sanity: desktop FindFirst of a few entries stays cheap on this machine.
    desk = Path.home() / "Desktop"
    if not desk.is_dir():
        return
    t0 = time.perf_counter()
    _ = list(desk.iterdir())
    elapsed_ms = (time.perf_counter() - t0) * 1000
    assert elapsed_ms < 500.0, f"desktop iterdir too slow: {elapsed_ms:.1f}ms"
