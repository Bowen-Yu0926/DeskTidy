"""Regression: page-switch soft park must not leave opacity-0 HWNDs compositing.

Opacity-0 soft park caused every fence-icon selection paint to flash the desktop.
Market fix: batch SWP_HIDEWINDOW / SWP_SHOWWINDOW; keep opacity at target.
"""

from __future__ import annotations

import inspect
import sys
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

passes: list[str] = []
failures: list[tuple[str, str]] = []


def run(name: str, fn) -> None:
    try:
        fn()
        passes.append(name)
        print(f"  OK  {name}")
    except Exception as exc:
        failures.append((name, f"{exc}\n{traceback.format_exc()}"))
        print(f"FAIL  {name}: {exc}")


def test_soft_park_contracts() -> None:
    from src.app import DeskTidyApp
    from src.desktop_shell_host import OverlayWindowBatch

    hide = inspect.getsource(DeskTidyApp._soft_hide_fence_for_page)
    show = inspect.getsource(DeskTidyApp._soft_show_fence_for_page)
    assert "setWindowOpacity(0.0)" not in hide
    assert "setWindowOpacity(0)" not in hide.replace("0.0", "0")
    assert "_target_opacity" in hide
    assert "batch.hide" in hide
    assert "batch.show" in show
    assert "_target_opacity" in show
    # Soft park/unpark must toggle native click-through, not Qt attr alone.
    assert "set_overlay_mouse_passthrough" in hide
    assert "set_overlay_mouse_passthrough" in show
    take = inspect.getsource(DeskTidyApp._take_parked_fence)
    # Take must keep soft-parked so reveal → soft_show owns mouse + Win32 show.
    assert "_desktidy_soft_parked = False" not in take
    assert "set_overlay_mouse_passthrough" not in take
    assert "set_overlay_mouse_passthrough" in hide
    assert "set_overlay_mouse_passthrough" in show
    take = inspect.getsource(DeskTidyApp._take_parked_fence)
    # Mouse/soft flags stay for soft_show via reveal — do not clear in take.
    assert "_desktidy_soft_parked = False" not in take
    assert "WA_TransparentForMouseEvents" not in take
    assert "set_overlay_mouse_passthrough" not in take

    prepare = inspect.getsource(DeskTidyApp._prepare_page_switch_new_fence)
    assert "show=False" in prepare
    # First-visit must keep the warmed HWND — Qt hide detached it (fence missing).
    assert "fence.hide()" not in prepare
    # Do not Qt-show / DontShowOnScreen in prepare — that desynced 文档 first visit.
    assert "fence.show()" not in prepare
    assert "setAttribute" not in prepare
    assert "_desktidy_batch_reveal" in prepare
    assert "_desktidy_defer_refresh" in prepare
    assert "_refresh_impl" not in prepare.split("return int(hwnd")[0]
    refresh_after = inspect.getsource(DeskTidyApp._refresh_fences_after_page_switch)
    assert "needs_force" in refresh_after
    assert "and not defer" in refresh_after
    assert "_page_switch_force_refresh_fence_ids" in refresh_after
    assert "reload_icons" in refresh_after or "refresh(force=True)" in refresh_after
    assert "has_icon_widgets" in refresh_after
    assert "flush_icon_grid_paint" in refresh_after
    assert "_refresh_impl" in refresh_after
    # First-visit must not wipe icon cache (Office extract hitch).
    assert "fence_id in batch_ids" not in refresh_after
    warmup = inspect.getsource(DeskTidyApp._warmup_all_offpage_fences)
    assert "_warmup_one_offpage_fence" in warmup
    # Boot first-map of every off-page HWND — not one-per-tick (click flash).
    assert "QTimer.singleShot(0, self._warmup_all_offpage_fences)" not in warmup
    assert "pending" in warmup
    assert "sync_icons=True" in warmup
    one = inspect.getsource(DeskTidyApp._warmup_one_offpage_fence)
    assert "show=False" in one
    assert "fence.show()" not in one
    assert "_park_fence" in one
    assert "prime_hidden_overlay" in one
    assert "_refresh_impl" in one
    assert "sync_icons" in one
    assert "flush_icon_grid_paint" in one
    assert "on_shown" in one
    overlays = inspect.getsource(DeskTidyApp._run_startup_overlays)
    assert "_schedule_startup_offpage_warmup" in overlays
    assert "QTimer.singleShot(0, self._warmup_all_offpage_fences)" not in overlays
    assert "1200, self._warmup_all_offpage_fences" not in overlays
    assert overlays.index("_setup_hotkeys") < overlays.index(
        "_schedule_startup_offpage_warmup"
    )
    assert "_schedule_startup_offpage_warmup" in overlays
    assert "_startup_force_overlay_attach" in overlays
    boot_attach = inspect.getsource(DeskTidyApp._startup_force_overlay_attach)
    assert "ensure_live_fences_interactive" in boot_attach
    assert "_heal_boot_fence_surfaces" in boot_attach
    heal = inspect.getsource(DeskTidyApp._heal_boot_fence_surfaces)
    assert "_flush_style_paint" in heal
    assert "flush_icon_grid_paint" in heal
    assert "refresh(force=True, shell_heal=False)" in heal
    # Paint-only: restack after flush used to wipe layered panels black.
    assert "self.ensure_live_fences_interactive" not in heal
    assert "configure_desktop_overlay" not in heal
    assert "QTimer.singleShot(900" not in boot_attach
    pending_force = inspect.getsource(DeskTidyApp._run_pending_force_shell_attach)
    assert "ensure_live_fences_interactive" in pending_force
    assert "self._heal_boot_fence_surfaces()" not in pending_force
    assert "immediate=True" in overlays
    assert "show_overlays=False" in overlays
    assert "processEvents" in warmup
    switch = inspect.getsource(DeskTidyApp.switch_page)
    assert "_warmup_fences_for_page" in switch
    assert switch.index("_warmup_fences_for_page") < switch.index(
        "with self._page_switch_paint_freeze()"
    )
    assert "processEvents" in switch
    assert "ExcludeUserInputEvents" in switch
    overlays = inspect.getsource(DeskTidyApp._run_startup_overlays)
    assert "_schedule_startup_offpage_warmup" in overlays
    sched = inspect.getsource(DeskTidyApp._schedule_startup_deferred)
    assert "_warmup_offpage_fences" not in sched
    assert "QTimer.singleShot(0, self._run_startup_overlays)" in inspect.getsource(
        DeskTidyApp._schedule_startup_overlays
    )
    assert "_refresh_fences_after_page_switch" in inspect.getsource(
        DeskTidyApp._finish_page_switch_overlays_if_pending
    )
    show_fences = inspect.getsource(DeskTidyApp.show_fences)
    assert "refresh_icons=need_icons and not page_switch" in show_fences
    assert "defer_icon_refresh=page_switch" in show_fences
    assert "_mark_page_switch_fence_force_refresh" in show_fences
    assert "_desktidy_defer_refresh" in show_fences.split("_mark_page_switch_fence_force_refresh")[0]
    assert "Hide leavers before" in show_fences or "to_park.clear()" in show_fences
    assert "batch SWP_SHOWWINDOW" in prepare or "_sync_qt_visible_after_win32" in prepare
    show_src = inspect.getsource(DeskTidyApp.show_fences)
    assert "batch.set_rect" in show_src
    assert "batch.show" in show_src
    assert "_prepare_page_switch_new_fence" in show_src
    show_fences = show_src
    assert "if not page_switch:" in show_fences
    assert "elif created_new > 0:" in show_fences

    batch_src = inspect.getsource(OverlayWindowBatch)
    assert "SWP_HIDEWINDOW" in batch_src
    assert "SWP_SHOWWINDOW" in batch_src
    assert "SWP_NOREDRAW" in batch_src
    assert "show_base" in batch_src
    assert "BeginDeferWindowPos" in batch_src
    assert "hide leavers before showing arrivers" in batch_src or "_order" in batch_src
    show_flag_line = [
        ln for ln in batch_src.splitlines() if "show_base =" in ln and "SWP_" in ln
    ]
    assert show_flag_line, batch_src
    assert "SWP_NOCOPYBITS" not in show_flag_line[0]
    # Incomplete DeferWindowPos must still apply SHOW (文档 first-visit).
    assert "_apply_one" in batch_src
    redraw_src = inspect.getsource(
        __import__("src.desktop_shell_host", fromlist=["x"]).redraw_overlay_hwnd
    )
    assert "RedrawWindow" in redraw_src
    assert "RDW_ERASE |" not in redraw_src
    flush_src = inspect.getsource(
        __import__("src.ui.fence_widget", fromlist=["FenceWidget"]).FenceWidget.flush_icon_grid_paint
    )
    assert "redraw_overlay_hwnd" in flush_src
    from src.desktop_shell_host import prime_hidden_overlay as _prime

    prime_src = inspect.getsource(_prime)
    assert "OFFSCREEN_PARK_POS" in prime_src
    assert "SWP_SHOWWINDOW" in prime_src
    assert "SWP_HIDEWINDOW" in prime_src
    assert "on_shown" in prime_src

    # Active fences after page switch heal opacity-0 leftovers.
    show_fences = inspect.getsource(DeskTidyApp.show_fences)
    assert "windowOpacity()" in show_fences
    assert "_target_opacity" in show_fences

    freeze = inspect.getsource(DeskTidyApp._page_switch_paint_freeze)
    assert "parked_fences" in freeze
    assert "Include fences created mid-switch" in freeze
    assert "_sync_qt_visible_after_win32" in freeze
    assert "overlay_win32_visible" in freeze
    # First-visit: re-attach via reveal, never a bare show() after batch.
    assert "_reveal_desktop_overlay" in freeze
    assert "_desktidy_batch_reveal" in freeze
    assert "is_attached_to_desktop" in freeze
    assert "testAttribute" in freeze
    assert "WA_DontShowOnScreen" in inspect.getsource(
        DeskTidyApp._sync_qt_visible_after_win32
    )
    finish = inspect.getsource(DeskTidyApp._finish_page_switch_overlays_if_pending)
    assert "_page_switch_freeze_depth" in finish
    switch = inspect.getsource(DeskTidyApp.switch_page)
    # Ensure after freeze with-block (batch.commit), not inside the body.
    assert switch.index("with self._page_switch_paint_freeze()") < switch.index(
        "self._finish_page_switch_overlays_if_pending()"
    )
    assert "persist=False" in switch
    raise_src = inspect.getsource(
        __import__("src.desktop_shell_host", fromlist=["x"]).raise_overlay_in_desktop_band
    )
    assert "SWP_NOREDRAW" in raise_src
    assert "SWP_NOOWNERZORDER" in raise_src
    # Z-order raise must keep layered client bits (boot black fences).
    assert "SWP_NOCOPYBITS" not in raise_src
    place_src = inspect.getsource(
        __import__("src.desktop_shell_host", fromlist=["x"]).place_overlay_in_desktop_band
    )
    assert "SWP_NOOWNERZORDER" in place_src
    ensure_src = inspect.getsource(DeskTidyApp._ensure_page_switch_overlays_visible)
    assert "_remap_hidden_overlay_hwnds" in ensure_src
    assert "is_stuck_under_wallpaper" in ensure_src
    assert "_ensure_page_chrome_visible(raise_band=True)" in ensure_src
    assert "_reveal_desktop_overlay" in ensure_src
    assert "_sync_qt_visible_after_win32" in ensure_src
    finish_src = inspect.getsource(DeskTidyApp._finish_page_switch_overlays_if_pending)
    assert "timer.stop()" in finish_src
    assert "_restore_arriving_fences_interactive" in finish_src
    assert "_refresh_fences_after_page_switch" in finish_src
    restore_src = inspect.getsource(DeskTidyApp._restore_arriving_fences_interactive)
    assert "set_overlay_mouse_passthrough" in restore_src
    assert "ensure_live_fences_interactive" in restore_src
    ensure_live = inspect.getsource(DeskTidyApp.ensure_live_fences_interactive)
    assert "raise_overlay_in_desktop_band" in ensure_live
    assert "pet_widget" in ensure_live
    assert "_public_icon_host" in ensure_live
    assert "_soft_show_fence_for_page" in ensure_live
    assert "set_overlay_mouse_passthrough" in ensure_live
    # Tray organize / refresh_public: raise only on Explorer desktop FG.
    assert "is_explorer_desktop_foreground" in ensure_live
    assert "skip_raise" in ensure_live
    raise_src = inspect.getsource(
        __import__("src.desktop_shell_host", fromlist=["x"]).raise_overlay_in_desktop_band
    )
    assert "is_attached_to_desktop" in raise_src
    passthrough = inspect.getsource(
        __import__("src.win_shell", fromlist=["x"]).set_overlay_mouse_passthrough
    )
    assert "WS_EX_TRANSPARENT" in passthrough
    assert "FRAMECHANGED" in passthrough or "0x0020" in passthrough
    refresh_src = inspect.getsource(DeskTidyApp._refresh_fences_after_page_switch)
    assert "_yield_page_switch_ui" in refresh_src
    needs = inspect.getsource(DeskTidyApp._page_chrome_needs_raise)
    assert "is_desktop_point" in needs
    switch_src = inspect.getsource(DeskTidyApp.switch_page)
    # Quiet must stay until finish (after icon fill), not clear before finish.
    assert "_page_switch_quiet_until = 0.0" not in switch_src
    finish_src2 = inspect.getsource(DeskTidyApp._finish_page_switch_overlays_if_pending)
    assert finish_src2.index("_refresh_fences_after_page_switch") < finish_src2.index(
        "_page_switch_quiet_until = 0.0"
    )


def test_icon_click_still_nofocus() -> None:
    from src.ui.fence_icon_item import FenceIconItem
    from src.ui.fence_widget import FenceItemLabel, FenceWidget

    for cls in (FenceIconItem, FenceItemLabel, FenceWidget):
        init = inspect.getsource(cls.__init__)
        assert "NoFocus" in init, cls.__name__


def test_icon_selection_batch_contracts() -> None:
    from src.ui import fence_icon_item as fii
    from src.ui.fence_icon_item import FenceIconItem
    from src.ui.fence_widget import FenceItemLabel, FenceWidget
    from src.ui.public_icon_widget import PublicIconWidget

    select_src = inspect.getsource(fii.select_fence_item)
    assert "fence_selection_batch" in select_src
    click_src = inspect.getsource(fii.apply_fence_click_selection)
    assert "fence_selection_batch" in click_src
    assert "fence_selection_batch" in inspect.getsource(fii.select_all_fence_items)

    set_sel = inspect.getsource(FenceIconItem.set_selected)
    assert "self.update()" not in set_sel
    assert "_commit_fence_item_selection_visual" in set_sel
    # Translucent fence HWNDs: paintEvent selection (QFrame rings ghost after clear).
    paint = inspect.getsource(FenceIconItem.paintEvent)
    assert "paint_fence_item_selection" in paint
    assert "_selection_ring" not in inspect.getsource(FenceIconItem.__init__)
    assert "QGraphicsDropShadowEffect" not in inspect.getsource(FenceIconItem.__init__)

    label_paint = inspect.getsource(FenceItemLabel.paintEvent)
    assert "paint_fence_item_selection" in label_paint
    assert "_selection_ring" not in inspect.getsource(FenceItemLabel.__init__)
    clear_src = inspect.getsource(FenceWidget.clear_item_selection)
    assert "fence_selection_batch" in clear_src

    commit = inspect.getsource(fii._commit_fence_item_selection_visual)
    assert "_invalidate_fence_item_selection" in commit
    inv = inspect.getsource(fii._invalidate_fence_item_selection)
    # One host-region update — not child+parent dual update (click flash).
    assert "_fence_item_paint_host" in inv
    assert "host.update(_item_rect_for_host" in inv
    hosted_inv = inv.split("if host is not None:", 1)[1].split("try:", 1)[0]
    assert "widget.update()" not in hosted_inv
    flush = inspect.getsource(fii._flush_fence_selection_visuals)
    hosted = flush.split("for host, items in by_host.items():", 1)[1]
    hosted = hosted.split("for widget in orphans:", 1)[0]
    assert "widget.update()" not in hosted
    assert "host.update(region)" in hosted
    batch = inspect.getsource(fii.fence_selection_batch)
    # Flush before re-enabling updates (enable-then-flush double-painted).
    assert batch.index("_flush_fence_selection_visuals") < batch.index(
        "setUpdatesEnabled(True)"
    )
    assert "_apply_selection_ring" not in inspect.getsource(fii)

    # Public floats share the same storm class on PublicIconHost.
    assert "public_selection_batch" in inspect.getsource(fii.select_public_item)
    assert "public_selection_batch" in inspect.getsource(fii.apply_public_click_selection)
    assert "public_selection_batch" in inspect.getsource(fii.select_all_public_items)
    pub_sel = inspect.getsource(PublicIconWidget.set_selected)
    assert "_commit_public_item_selection_visual" in pub_sel
    pub_paint = inspect.getsource(PublicIconWidget.paintEvent)
    assert "paint_public_item_selection" in pub_paint
    assert "_selection_ring" not in inspect.getsource(PublicIconWidget.__init__)
    chrome = inspect.getsource(fii.public_selection_chrome_rect)
    assert "public_caption_ink_rect" in chrome
    assert "icon_label" in chrome
    # Full-widget paint was the oversized blue plate; chrome follows caption shelf.
    assert "widget.rect().adjusted(1, 1, -2, -2)" not in pub_paint
    pub_commit = inspect.getsource(fii._commit_public_item_selection_visual)
    assert "_apply_selection_ring" not in pub_commit
    # Host-region update only when hosted — dual widget+host update was a flash source.
    assert "host.update(widget.geometry())" in pub_commit
    flush = inspect.getsource(fii._flush_public_selection_visuals)
    assert "host.update(region)" in flush
    # Orphans may still widget.update; hosted path must not dual-update every dirty.
    hosted_flush = flush.split("for host, items in by_host.items():", 1)[1]
    hosted_flush = hosted_flush.split("for widget in orphans:", 1)[0]
    assert "widget.update()" not in hosted_flush

    # Copy/cut only promote selection via batched select_*; paste/drag use refresh.
    dispatch = inspect.getsource(fii.dispatch_desktop_icon_chord)
    assert "select_fence_item" in dispatch
    assert "select_public_item" in dispatch
    paste = inspect.getsource(fii.paste_files_into_fence)
    assert "refresh" in paste

    from src.app import DeskTidyApp

    blocked = inspect.getsource(DeskTidyApp._overlay_restack_blocked)
    assert "desktop_selection_batch_active" in blocked


def test_copy_paste_drag_not_selection_storm() -> None:
    """Copy/cut/paste/drag must not reintroduce per-icon update storms."""
    from src.ui import fence_icon_item as fii

    # Copy/cut: clipboard only (+ optional one batched select promotion).
    dispatch = inspect.getsource(fii.dispatch_desktop_icon_chord)
    assert "freeze_clipboard" in dispatch
    assert "_begin_desktop_popup" in dispatch
    assert "_end_desktop_popup_later" in dispatch
    assert 'chord == "copy"' in dispatch
    assert "clipboard_set_files" in dispatch
    assert "set_selected(False)" not in dispatch

    # Drag begin freezes overlay restack (not selection paint).
    start_virt = inspect.getsource(fii.start_virtual_item_drag)
    assert "_run_custom_desktop_resident_virtual_drag" in start_virt
    custom = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
    assert "_begin_overlay_drag_session" in custom
    start_pub = inspect.getsource(fii.start_public_item_drag)
    assert "_begin_overlay_drag_session" in start_pub

    # Paste rebuilds icons once — different flash class, but must not loop set_selected.
    paste = inspect.getsource(fii.paste_files_into_fence)
    assert "set_selected" not in paste
    assert paste.count("refresh") >= 1


def test_batch_hide_show_ops() -> None:
    from src.desktop_shell_host import OverlayWindowBatch

    batch = OverlayWindowBatch()
    batch.hide(0)  # ignored
    batch.show(0)
    batch.set_rect(0, 1, 2, 3, 4)
    assert batch._items == []
    # commit empty is fine
    batch.commit()


def test_first_visit_seeds_saved_geometry() -> None:
    """New FenceWidget must not be born at MIN_WIDTH."""
    from PyQt6.QtWidgets import QApplication

    from src.ui.fence_widget import FenceWidget

    app = QApplication.instance() or QApplication([])
    cfg = {
        "id": "system_docs",
        "name": "文档",
        "virtual_items": [],
        "width": 1920,
        "height": 252,
        "style": {"view_mode": "grid"},
        "pages": [1],
    }
    w = FenceWidget(cfg, {"theme": "mist", "fences": [cfg], "exclude_patterns": []})
    try:
        assert w.width() >= 1800, w.width()
        assert w.windowHandle() is None
        _icon, cols, _label = w._icon_layout_metrics()
        assert cols >= 8, (cols, w.width())
        src = inspect.getsource(FenceWidget._seed_geometry_from_config)
        assert "resize" in src
        show_ev = inspect.getsource(FenceWidget.showEvent)
        assert "WA_DontShowOnScreen" in show_ev
        from src.app import DeskTidyApp

        place = inspect.getsource(DeskTidyApp.show_fences).split("def _place_fence", 1)[
            1
        ].split("for fence_cfg", 1)[0]
        assert "windowHandle" in place
        assert place.index("windowHandle") < place.index("_fence_hwnd")
        # First-visit: Qt geometry only — HWND is born in prepare, not here.
        native_none = place.split("if native is None:", 1)[1]
        assert "if page_switch:" in native_none
        assert native_none.index("if page_switch:") < native_none.index("return")
    finally:
        w.close()
        w.deleteLater()
        app.processEvents()


def test_first_visit_grid_uses_saved_width() -> None:
    """If HWND was born at MIN_WIDTH, columns must still follow saved width."""
    from PyQt6.QtWidgets import QApplication

    from src.ui.fence_widget import FenceWidget

    app = QApplication.instance() or QApplication([])
    cfg = {
        "id": "system_docs",
        "name": "文档",
        "virtual_items": [],
        "width": 1920,
        "height": 252,
        "style": {"view_mode": "grid", "show_title": True, "collapsible": True},
        "icon_zoom": 0.7,
        "sort_by": "manual",
        "pages": [1],
    }
    settings = {
        "theme": "mist",
        "fences": [cfg],
        "current_page": 1,
        "exclude_patterns": [],
    }
    w = FenceWidget(cfg, settings)
    try:
        w.resize(FenceWidget.MIN_WIDTH, 252)
        app.processEvents()
        usable = w._available_items_width()
        assert usable >= 1800, usable
        _icon, cols, _label = w._icon_layout_metrics()
        assert cols >= 8, (cols, usable, w.width())
        # After the user shrinks the fence, live width wins over stale config.
        w.resize(800, 252)
        app.processEvents()
        usable_live = w._available_items_width()
        assert 700 <= usable_live <= 800, usable_live
        src = inspect.getsource(FenceWidget._available_items_width)
        assert "MIN_WIDTH" in src
        assert "viewport" in src
        assert "width" in src
    finally:
        w.close()
        w.deleteLater()
        app.processEvents()


def test_prepare_new_fence_keeps_qt_mapped() -> None:
    """工作→文档 first visit warms a hidden HWND — no Qt show before batch."""
    from unittest.mock import patch

    from PyQt6.QtCore import Qt
    from PyQt6.QtWidgets import QApplication

    from src.app import DeskTidyApp
    from src.ui.fence_widget import FenceWidget

    app = QApplication.instance() or QApplication([])
    cfg = {
        "id": "system_docs",
        "name": "文档",
        "virtual_items": [],
        "width": 1920,
        "height": 252,
        "style": {"view_mode": "grid"},
        "pages": [1],
    }
    w = FenceWidget(cfg, {"theme": "mist", "fences": [cfg], "exclude_patterns": []})
    try:
        with (
            patch("src.desktop_shell_host.attach_overlay_to_desktop", return_value=True),
            patch("src.desktop_shell_host.set_overlay_hwnd_visible") as hide_mock,
        ):
            hwnd = DeskTidyApp._prepare_page_switch_new_fence(object.__new__(DeskTidyApp), w)
        assert hwnd, "warm HWND must exist"
        assert not w.isVisible(), "Qt show() in prepare maps 文档 fence before leavers hide"
        assert not w.testAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen)
        assert bool(getattr(w, "_desktidy_batch_reveal", False))
        assert hide_mock.call_count >= 1
    finally:
        w._desktidy_batch_reveal = False  # type: ignore[attr-defined]
        w.close()
        w.deleteLater()
        app.processEvents()


def test_page_switch_defers_ctor_refresh() -> None:
    """First-visit fence must not fill icons until batch show completes."""
    init = inspect.getsource(
        __import__("src.ui.fence_widget", fromlist=["FenceWidget"]).FenceWidget.__init__
    )
    assert "_deferred_ctor_refresh" in init
    assert "_desktidy_defer_refresh" in init
    prepare = inspect.getsource(
        __import__("src.app", fromlist=["DeskTidyApp"]).DeskTidyApp._prepare_page_switch_new_fence
    )
    assert "_desktidy_defer_refresh" in prepare
    assert "_page_switch_batch_fence_ids" in prepare


def test_docs_first_visit_icons_after_switch() -> None:
    """工作→文档: empty first-visit fence fills after freeze (no cache wipe)."""
    import tempfile
    from pathlib import Path
    from unittest.mock import patch

    from PyQt6.QtWidgets import QApplication

    from src.app import DeskTidyApp
    from src.ui.fence_widget import FenceIconItem, FenceWidget

    app = QApplication.instance() or QApplication([])
    with tempfile.TemporaryDirectory() as tmp:
        desktop = Path(tmp)
        paths = []
        for name in ("a.txt", "b.txt", "c.txt"):
            p = desktop / name
            p.write_text("x", encoding="utf-8")
            paths.append(str(p))
        cfg = {
            "id": "system_docs",
            "name": "文档",
            "virtual_items": paths,
            "width": 640,
            "height": 252,
            "style": {"view_mode": "grid"},
            "pages": [1],
            "visible": True,
        }
        settings = {
            "theme": "mist",
            "fences": [cfg],
            "current_page": 0,
            "desktop_pages": [
                {"id": 0, "name": "工作"},
                {"id": 1, "name": "文档"},
            ],
            "exclude_patterns": [],
            "show_fences": True,
        }
        desk = object.__new__(DeskTidyApp)
        desk._exiting = False
        desk._icons_hidden = False
        desk.fences = []
        desk._parked_fences = {}
        desk._page_switch_batch_fence_ids = {"system_docs"}
        desk.settings = settings
        w = FenceWidget(cfg, settings)
        try:
            w.resize(640, 252)
            w._desktidy_defer_refresh = True  # type: ignore[attr-defined]
            app.processEvents()
            assert not w.has_icon_widgets(), "defer must block ctor grid"
            desk.fences = [w]
            desk._refresh_fences_after_page_switch()
            app.processEvents()
            assert w.has_icon_widgets(), "force refresh after switch must fill grid"
            count = sum(
                1
                for i in range(w.items_layout.count())
                if isinstance(
                    (w.items_layout.itemAt(i).widget() if w.items_layout.itemAt(i) else None),
                    FenceIconItem,
                )
            )
            assert count == 3, count
        finally:
            w.close()
            w.deleteLater()
            app.processEvents()


def test_parked_restore_uses_reload_not_force() -> None:
    """文档→工作: parked grid must reload in-place, not force rebuild."""
    import tempfile
    from pathlib import Path
    from unittest.mock import patch

    from PyQt6.QtWidgets import QApplication

    from src.app import DeskTidyApp
    from src.ui.fence_widget import FenceWidget

    app = QApplication.instance() or QApplication([])
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp) / "app.lnk"
        p.write_text("x", encoding="utf-8")
        cfg = {
            "id": "system_common",
            "name": "软件",
            "virtual_items": [str(p)],
            "width": 400,
            "height": 200,
            "style": {"view_mode": "grid"},
            "pages": [0],
            "visible": True,
        }
        settings = {"theme": "mist", "fences": [cfg], "exclude_patterns": []}
        w = FenceWidget(cfg, settings)
        try:
            w.resize(400, 200)
            app.processEvents()
            assert w.has_icon_widgets()
            desk = object.__new__(DeskTidyApp)
            desk._exiting = False
            desk._icons_hidden = False
            desk.fences = [w]
            desk._page_switch_batch_fence_ids = set()
            with patch.object(w, "refresh") as refresh_mock, patch.object(
                w, "reload_icons"
            ) as reload_mock, patch.object(
                w, "flush_icon_grid_paint"
            ) as flush_mock:
                DeskTidyApp._refresh_fences_after_page_switch(desk)
                app.processEvents()
                refresh_mock.assert_not_called()
                reload_mock.assert_not_called()
                flush_mock.assert_not_called()
        finally:
            w.close()
            w.deleteLater()
            app.processEvents()


def test_work_page_icons_after_restore_from_park() -> None:
    """文档→工作: parked fences must refresh after batch show, not in freeze."""
    import tempfile
    from pathlib import Path

    from PyQt6.QtWidgets import QApplication

    from src.app import DeskTidyApp
    from src.ui.fence_widget import FenceIconItem, FenceWidget

    app = QApplication.instance() or QApplication([])
    with tempfile.TemporaryDirectory() as tmp:
        desktop = Path(tmp)
        paths = [str(desktop / "app.lnk")]
        (desktop / "app.lnk").write_text("x", encoding="utf-8")
        cfg = {
            "id": "system_common",
            "name": "软件",
            "virtual_items": paths,
            "width": 800,
            "height": 200,
            "style": {"view_mode": "grid"},
            "pages": [0],
            "visible": True,
        }
        settings = {
            "theme": "mist",
            "fences": [cfg],
            "current_page": 0,
            "desktop_pages": [{"id": 0, "name": "工作"}, {"id": 1, "name": "文档"}],
            "exclude_patterns": [],
        }
        w = FenceWidget(cfg, settings)
        try:
            w.resize(800, 200)
            w._desktidy_soft_parked = True  # type: ignore[attr-defined]
            # Simulate freeze-time apply_config skip + empty grid after fast switch.
            app.processEvents()
            desk = object.__new__(DeskTidyApp)
            desk._exiting = False
            desk._icons_hidden = False
            desk.fences = [w]
            desk._page_switch_batch_fence_ids = set()
            desk._refresh_fences_after_page_switch()
            app.processEvents()
            assert w.has_icon_widgets()
            count = sum(
                1
                for i in range(w.items_layout.count())
                if isinstance(
                    (w.items_layout.itemAt(i).widget() if w.items_layout.itemAt(i) else None),
                    FenceIconItem,
                )
            )
            assert count == 1, count
        finally:
            w.close()
            w.deleteLater()
            app.processEvents()


def test_warmup_parks_offpage_fence() -> None:
    """Idle warmup parks 文档 fence hidden so first 工作→文档 is unpark."""
    from unittest.mock import patch

    from PyQt6.QtWidgets import QApplication

    from src.app import DeskTidyApp

    app = QApplication.instance() or QApplication([])
    cfg = {
        "id": "system_docs",
        "name": "文档",
        "virtual_items": [],
        "width": 640,
        "height": 252,
        "style": {"view_mode": "grid"},
        "pages": [1],
        "visible": True,
    }
    cfg2 = {
        "id": "system_docs_2",
        "name": "文档2",
        "virtual_items": [],
        "width": 640,
        "height": 252,
        "style": {"view_mode": "grid"},
        "pages": [1],
        "visible": True,
    }
    desk = object.__new__(DeskTidyApp)
    desk._exiting = False
    desk._icons_hidden = False
    desk._fences_hidden = False
    desk._page_switch_ensure_pending = False
    desk._page_switch_freeze_depth = 0
    desk._page_switch_geo_batch = None
    desk._max_parked_fences = 12
    desk.fences = []
    desk._parked_fences = {}
    desk.settings = {
        "show_fences": True,
        "current_page": 0,
        "fences": [cfg, cfg2],
        "exclude_patterns": [],
        "theme": "mist",
        "desktop_pages": [{"id": 0, "name": "工作"}, {"id": 1, "name": "文档"}],
    }
    w = None
    try:
        with (
            patch("src.desktop_shell_host.attach_overlay_to_desktop", return_value=True),
            patch("src.desktop_shell_host.set_overlay_hwnd_visible"),
            patch("src.desktop_shell_host.prime_hidden_overlay"),
        ):
            DeskTidyApp._warmup_all_offpage_fences(desk)
        assert "system_docs" in desk._parked_fences
        assert "system_docs_2" in desk._parked_fences
        assert set(desk._parked_fences) == {"system_docs", "system_docs_2"}
        w = desk._parked_fences["system_docs"]
        assert not w.isVisible()
        assert bool(getattr(w, "_desktidy_soft_parked", False))
    finally:
        for victim in list(desk._parked_fences.values()):
            try:
                victim.close()
                victim.deleteLater()
            except Exception:
                pass
        app.processEvents()


def test_show_event_skips_during_batch_reveal() -> None:
    """Qt sync after batch SHOW must not ShowWindow again (VPN-like flash)."""
    show_ev = inspect.getsource(
        __import__("src.ui.fence_widget", fromlist=["FenceWidget"]).FenceWidget.showEvent
    )
    assert "_desktidy_batch_reveal" in show_ev
    freeze = inspect.getsource(
        __import__("src.app", fromlist=["DeskTidyApp"]).DeskTidyApp._page_switch_paint_freeze
    )
    # First-visit mapped path: sync Qt while batch_reveal is still True.
    mapped = freeze.split("if attached and win32_on:", 1)[1]
    assert "_sync_qt_visible_after_win32" in mapped
    assert mapped.index("_sync_qt_visible_after_win32") < mapped.index(
        "_desktidy_batch_reveal = False"
    )


def test_arriving_fences_mouse_cleared_before_icon_fill() -> None:
    """工作→文档: arriving fence must not stay mouse-transparent during icon fill."""
    from PyQt6.QtCore import Qt
    from PyQt6.QtWidgets import QApplication, QWidget

    from src.app import DeskTidyApp

    app = QApplication.instance() or QApplication([])
    desk = object.__new__(DeskTidyApp)
    desk.pet_widget = None
    desk._public_icon_host = None
    desk._parked_fences = {}
    desk._fence_hwnd = lambda _f: 0  # type: ignore[method-assign]
    desk._soft_show_fence_for_page = (  # type: ignore[method-assign]
        lambda f: DeskTidyApp._soft_show_fence_for_page(desk, f)
    )

    arriving = QWidget()
    arriving.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
    arriving._desktidy_soft_parked = False  # type: ignore[attr-defined]
    desk.fences = [arriving]

    leaver = QWidget()
    leaver.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, False)
    leaver._desktidy_soft_parked = True  # type: ignore[attr-defined]
    desk._parked_fences = {"old": leaver}

    stuck = QWidget()
    stuck.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
    stuck._desktidy_soft_parked = True  # type: ignore[attr-defined]
    stuck._target_opacity = lambda: 1.0  # type: ignore[attr-defined]
    desk.fences.append(stuck)

    try:
        DeskTidyApp._restore_arriving_fences_interactive(desk)
        assert not arriving.testAttribute(
            Qt.WidgetAttribute.WA_TransparentForMouseEvents
        )
        assert leaver.testAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        # Stuck soft-arriver must be force soft-shown (mouse restored).
        assert not stuck.testAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        assert not bool(getattr(stuck, "_desktidy_soft_parked", True))

        finish = inspect.getsource(DeskTidyApp._finish_page_switch_overlays_if_pending)
        assert finish.index("_restore_arriving_fences_interactive") < finish.index(
            "_refresh_fences_after_page_switch"
        )
        refresh = inspect.getsource(DeskTidyApp._refresh_fences_after_page_switch)
        assert "did_heavy" in refresh and "_yield_page_switch_ui" in refresh
    finally:
        arriving.close()
        arriving.deleteLater()
        leaver.close()
        leaver.deleteLater()
        stuck.close()
        stuck.deleteLater()
        app.processEvents()


def main() -> int:
    print("DeskTidy page-switch / icon-flash contracts")
    run("soft park contracts (no opacity-0)", test_soft_park_contracts)
    run("icon click NoFocus preserved", test_icon_click_still_nofocus)
    run("icon selection batch (no paintEvent storm)", test_icon_selection_batch_contracts)
    run("copy/paste/drag not selection storm", test_copy_paste_drag_not_selection_storm)
    run("batch empty ops safe", test_batch_hide_show_ops)
    run("首次创建分区用保存尺寸", test_first_visit_seeds_saved_geometry)
    run("首次进入分页网格用保存宽度", test_first_visit_grid_uses_saved_width)
    run("首次切页分区不 Qt-hide", test_prepare_new_fence_keeps_qt_mapped)
    run("切页推迟 ctor 刷新", test_page_switch_defers_ctor_refresh)
    run("文档首次切页后显示图标", test_docs_first_visit_icons_after_switch)
    run("停放恢复走 reload 不全量重建", test_parked_restore_uses_reload_not_force)
    run("工作页从停放恢复后显示图标", test_work_page_icons_after_restore_from_park)
    run("空闲预热停放其它分页分区", test_warmup_parks_offpage_fence)
    run("batch reveal 时 showEvent 不抢先 ShowWindow", test_show_event_skips_during_batch_reveal)
    run("切页后分区立即可点", test_arriving_fences_mouse_cleared_before_icon_fill)
    print()
    print(f"通过 {len(passes)}  失败 {len(failures)}")
    for name, detail in failures:
        print(f"  - {name}: {detail}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
