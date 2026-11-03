# -*- coding: utf-8 -*-
"""QA-DT-PUB-002: public floats cross-page + move-to-page (page_local)."""

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


def test_float_placement_page_local_field_and_set():
    hdr = _read("float_placements.h")
    assert "page_local" in hdr
    cpp = _read("float_placements.cpp")
    set_fn = _fn(cpp, "void SetFloatPage(")
    assert "page_local" in set_fn
    # Persist round-trip
    assert '"page_local"' in cpp or "page_local" in cpp
    assert "FindBoolNear" in cpp and "page_local" in cpp


def test_rebuild_public_respects_shared_and_page_local():
    app = _read("app.cpp")
    body = _fn(app, "void App::RebuildPublicItems()", 3200)
    assert "enable_public_desktop" in body
    assert "page_local" in body
    assert "page_bound" in body
    # New loose icons default shared (page_local false) when public on
    assert "page_local = false" in body.replace(" ", "") or "page_local = false" in body
    # Skip when page-bound and page mismatch
    assert "fit->second.page != current_page_" in body or "page != current_page_" in body


def test_move_to_page_clears_pin_and_sets_page_local():
    app = _read("app.cpp")
    body = _fn(app, "void App::MoveDesktopItemsToPage(", 1600)
    assert "ClearPin" in body
    assert "SetFloatPage" in body
    assert "page_local" in body
    assert "SuppressOrganizePath" in body
    assert "RefreshDesktopIcons" in body


def test_public_menu_keeps_multiselect_and_move_page():
    host = _read("public_host.cpp")
    menu = _fn(host, "void PublicHost::ShowSlotMenu(", 2800)
    # Do not collapse selection when clicking an already-selected slot
    assert "IsSelected(index)" in menu
    assert "SelectOnly(index)" in menu
    assert "menu_move_page" in menu or "kExtPublicMovePage" in menu
    assert "SelectedPaths()" in menu
    # Cascading page → fence (not flat move-to-page only)
    move = _fn(host, "void PublicHost::ShowMoveToPageFenceMenu(", 900)
    assert "TrackMoveToPageThenFenceMenu" in move
    assert "on_move_to_fence_" in move


def test_public_desktop_defaults_on():
    prefs_h = _read("app_prefs.h")
    assert "enable_public_desktop = true" in prefs_h
    prefs_cpp = _read("app_prefs.cpp")
    # Unmigrated prefs heal to ON
    assert "public_desktop_pref_migrated_v1" in prefs_cpp
    assert "enable_public_desktop" in prefs_cpp
