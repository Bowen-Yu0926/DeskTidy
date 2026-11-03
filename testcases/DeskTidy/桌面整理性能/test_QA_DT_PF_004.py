# -*- coding: utf-8 -*-
"""QA-DT-PF-004: same-seat Present skip, portal excludes, grid cache."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def _fn(src: str, sig: str, span: int = 3500) -> str:
    i = src.find(sig)
    assert i >= 0, sig
    return src[i : i + span]


def test_setitems_skips_present_when_same_seats_clean():
    fw = _read("fence_window.cpp")
    body = _fn(fw, "void FenceWindow::SetItems(", 4200)
    assert "same_seats" in body
    assert "layer_dirty_" in body
    # Early return before PresentLayered when same + clean
    present_i = body.rfind("PresentLayered()")
    assert present_i > 0
    before = body[:present_i]
    assert "same_seats && !layer_dirty_" in before.replace(" ", "") or (
        "same_seats" in before and "!layer_dirty_" in before and "return;" in before
    )


def test_refresh_uses_prefs_excludes_not_load_app_prefs_in_loop():
    app = _read("app.cpp")
    body = _fn(app, "void App::RefreshDesktopIcons()", 5200)
    assert "prefs_.exclude_patterns" in body
    assert "DefaultExcludePatterns" in body
    assert "ScanPortalItems(portal, excludes)" in body
    # Must not call LoadExcludePatterns / LoadAppPrefs inside the fence loop.
    loop = body[body.find("for (auto& fence : fences_)") :]
    loop = loop[: loop.find("RebuildDockItems()")]
    assert "LoadExcludePatterns()" not in loop
    assert "LoadAppPrefs()" not in loop


def test_off_desktop_pins_skip_displayname_for_files():
    app = _read("app.cpp")
    body = _fn(app, "void App::RefreshDesktopIcons()", 4200)
    # SHGetFileInfo DISPLAYNAME gated on is_link for extras
    assert "extra.is_link" in body
    assert "SHGFI_DISPLAYNAME" in body
    link_block = body[body.find("if (extra.is_link)") :]
    assert "SHGFI_DISPLAYNAME" in link_block[:400]


def test_icon_grid_metrics_cached_and_invalidated():
    geom = _read("display_geom.cpp")
    hdr = _read("display_geom.h")
    assert "InvalidateIconGridMetricsCache" in hdr
    assert "InvalidateIconGridMetricsCache" in geom
    assert "kGridCacheTtlMs" in geom
    assert "g_grid_cache" in geom
    query = _fn(geom, "IconGridMetrics QueryIconGridMetrics()", 900)
    assert "g_grid_valid" in query
    assert "g_grid_cache" in query
    app = _read("app.cpp")
    grids = _fn(app, "void App::RefreshIconGrids()", 500)
    assert "InvalidateIconGridMetricsCache()" in grids


def test_content_refresh_presents_only_when_dirty_or_buried():
    app = _read("app.cpp")
    body = _fn(app, "void App::OnDesktopContentRefresh()", 1600)
    assert "LayerDirty()" in body
    assert "IsOverlayAboveDefView" in body
    assert "RefreshLayer()" in body
    # Must not unconditionally RefreshLayer on mere visibility
    refresh_block = body[body.find("for (auto& fence : fences_)") :]
    # Guard precedes RefreshLayer
    assert refresh_block.find("LayerDirty()") < refresh_block.find("RefreshLayer()")
