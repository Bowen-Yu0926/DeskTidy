"""Stress / microbenchmark for DeskTidy desktop lag hotspots."""

from __future__ import annotations

import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def _count_toplevel_windows() -> int:
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    n = 0

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def _enum(_hwnd, _lp):
        nonlocal n
        n += 1
        return True

    user32.EnumWindows(_enum, 0)
    return n


def _enum_windows_ms() -> tuple[int, float]:
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.windll.user32
    n = 0

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def _enum(_hwnd, _lp):
        nonlocal n
        n += 1
        return True

    t0 = time.perf_counter()
    user32.EnumWindows(_enum, 0)
    return n, (time.perf_counter() - t0) * 1000


def test_defview_cache_and_repair_cost() -> None:
    from src.desktop_shell_host import (
        _find_defview_host,
        invalidate_defview_host_cache,
        is_attached_to_desktop,
    )

    tops = _count_toplevel_windows()
    invalidate_defview_host_cache()

    t0 = time.perf_counter()
    for _ in range(30):
        invalidate_defview_host_cache()
        _find_defview_host(force=True)
    cold_ms = (time.perf_counter() - t0) * 1000 / 30

    invalidate_defview_host_cache()
    _find_defview_host(force=True)
    t1 = time.perf_counter()
    for _ in range(200):
        _find_defview_host()
    warm_ms = (time.perf_counter() - t1) * 1000 / 200

    host, defview = _find_defview_host()
    # Simulated N overlay attach checks (repair loop).
    fake_hwnds = [host] * 40 if host else [0] * 40
    t2 = time.perf_counter()
    for hwnd in fake_hwnds:
        is_attached_to_desktop(hwnd, host=host, defview=defview)
    batch_ms = (time.perf_counter() - t2) * 1000

    t3 = time.perf_counter()
    for hwnd in fake_hwnds:
        is_attached_to_desktop(hwnd)  # uses cache, not N EnumWindows
    cached_batch_ms = (time.perf_counter() - t3) * 1000

    enum_n, enum_ms = _enum_windows_ms()
    overlays = 40
    old_repair_ms = enum_ms * overlays
    print(f"toplevel_windows={tops} enum_ms={enum_ms:.3f}")
    print(f"defview_cold_ms={cold_ms:.2f} warm_ms={warm_ms:.3f}")
    print(f"repair_40_with_host_ms={batch_ms:.2f} repair_40_cached_ms={cached_batch_ms:.2f}")
    print(
        f"OLD_per_overlay_EnumWindows_est_ms={old_repair_ms:.2f} "
        f"NEW_single_lookup_est_ms={enum_ms:.3f}"
    )

    assert warm_ms < cold_ms * 0.5 or warm_ms < 0.05
    # Cached path must stay cheap even with many overlays.
    assert cached_batch_ms < 15.0, cached_batch_ms
    assert enum_n == tops or abs(enum_n - tops) < 50


def test_many_apps_pressure() -> None:
    """Create ~200 extra HWNDs and confirm repair stays cheap."""
    import ctypes

    from src.desktop_shell_host import (
        _find_defview_host,
        invalidate_defview_host_cache,
        is_attached_to_desktop,
    )

    user32 = ctypes.windll.user32
    WS_POPUP = 0x80000000
    WS_EX_TOOLWINDOW = 0x00000080
    hwnds: list[int] = []
    try:
        for i in range(200):
            h = int(
                user32.CreateWindowExW(
                    WS_EX_TOOLWINDOW,
                    "STATIC",
                    f"desktidy_stress_{i}",
                    WS_POPUP,
                    0,
                    0,
                    1,
                    1,
                    0,
                    0,
                    0,
                    0,
                )
                or 0
            )
            if h:
                hwnds.append(h)
        n, enum_ms = _enum_windows_ms()
        invalidate_defview_host_cache()
        t0 = time.perf_counter()
        host, defview = _find_defview_host(force=True)
        cold = (time.perf_counter() - t0) * 1000
        t1 = time.perf_counter()
        for _ in range(40):
            is_attached_to_desktop(host or 0, host=host, defview=defview)
        repair = (time.perf_counter() - t1) * 1000
        print(
            f"pressure_windows={n} enum_ms={enum_ms:.3f} "
            f"defview_cold_ms={cold:.3f} repair40_ms={repair:.3f} "
            f"old40x_est_ms={enum_ms * 40:.2f}"
        )
        assert repair < 5.0, repair
        assert cold < 25.0, cold
    finally:
        for h in hwnds:
            user32.DestroyWindow(h)


def test_shared_ll_mouse_hook() -> None:
    from src.desktop_ll_mouse import (
        _INTERESTING_WPARAMS,
        _dispatch,
        ll_mouse_hook_active,
        register_ll_mouse_handler,
        subscriber_count,
        unregister_ll_mouse_handler,
    )
    import inspect

    src = inspect.getsource(_dispatch)
    assert "_INTERESTING_WPARAMS" in src
    assert 0x0200 not in _INTERESTING_WPARAMS  # WM_MOUSEMOVE excluded
    assert 0x0203 in _INTERESTING_WPARAMS
    assert "WM_MOUSEWHEEL" in src
    assert "zoom_modifier_physically_down" in src

    seen: list[int] = []

    def h1(w, _l):
        seen.append(w)

    def h2(w, _l):
        seen.append(w + 1000)

    register_ll_mouse_handler(h1)
    register_ll_mouse_handler(h2)
    assert subscriber_count() == 2
    assert ll_mouse_hook_active()
    unregister_ll_mouse_handler(h1)
    unregister_ll_mouse_handler(h2)
    assert subscriber_count() == 0
    assert not ll_mouse_hook_active()
    print("shared_ll_hook_ok")


def test_hotkey_and_fence_filter_contracts() -> None:
    import inspect

    from src.hotkey_manager import HotkeyManager
    from src.ui import fence_widget as fw
    from src.ui.fence_widget import FenceWidget

    ensure = inspect.getsource(HotkeyManager._ensure_poll_timer)
    # Idle/held/WM-only cadence (see _POLL_*_MS); keep light while keys up.
    assert "_POLL_IDLE_MS" in ensure or "1200" in ensure
    assert "_POLL_HELD_MS" in ensure or "200" in ensure
    assert "any_down" in ensure
    init = inspect.getsource(FenceWidget.__init__)
    assert "_register_fence_alt_zoom" in init
    assert "app.installEventFilter(self)" not in init
    assert callable(fw._register_fence_alt_zoom)
    assert callable(fw._unregister_fence_alt_zoom)
    cfg = inspect.getsource(
        __import__("src.win_shell", fromlist=["x"]).configure_desktop_overlay
    )
    assert "desired" in cfg
    print("hotkey_fence_filter_ok")


def test_menu_open_throttle() -> None:
    from src import win_shell

    t0 = time.perf_counter()
    for _ in range(40):
        win_shell.is_shell_context_menu_open()
    elapsed = (time.perf_counter() - t0) * 1000
    print(f"menu_open_40_calls_ms={elapsed:.2f}")
    # With throttle, 40 calls should not mean 40 EnumWindows.
    assert elapsed < 200.0, elapsed


def test_perf_regression_contracts() -> None:
    """Locks for clipboard / page-switch / force-refresh storm fixes."""
    import inspect

    from src.app import DeskTidyApp
    from src.shell_clipboard import _open_clipboard
    from src.ui.fence_widget import FenceWidget

    open_src = inspect.getsource(_open_clipboard)
    assert "attempts = 8 if allow_sleep else 3" in open_src
    assert "time.sleep(0.01)" in open_src
    refresh = inspect.getsource(FenceWidget.refresh)
    assert "_schedule_post_refresh_shell_heal" in refresh
    heal = inspect.getsource(DeskTidyApp._schedule_post_refresh_shell_heal)
    assert "_any_live_overlay_win32_hidden" in heal
    assert "force=force" in heal
    assert "force=True" not in heal
    finish = inspect.getsource(DeskTidyApp._finish_page_switch_overlays_if_pending)
    assert "_page_switch_freeze_depth" in finish
    keep = inspect.getsource(DeskTidyApp._keep_overlays_under_apps)
    assert "force=app_ui_fg" in keep

    warmup = inspect.getsource(DeskTidyApp._warmup_all_offpage_fences)
    assert "QTimer.singleShot(0, self._warmup_all_offpage_fences)" not in warmup
    assert "pending" in warmup
    assert "sync_icons=True" in warmup
    assert "processEvents" in warmup
    overlays = inspect.getsource(DeskTidyApp._run_startup_overlays)
    assert "QTimer.singleShot(0, self._warmup_all_offpage_fences)" not in overlays
    assert "1200, self._warmup_all_offpage_fences" not in overlays
    assert overlays.index("_setup_hotkeys") < overlays.index(
        "_schedule_startup_offpage_warmup"
    )
    assert "_schedule_startup_offpage_warmup" in overlays
    assert "_startup_force_overlay_attach" in overlays
    boot_attach = inspect.getsource(DeskTidyApp._startup_force_overlay_attach)
    assert "ensure_live_fences_interactive" in boot_attach
    assert "immediate=True" in overlays
    assert "show_overlays=False" in overlays
    one = inspect.getsource(DeskTidyApp._warmup_one_offpage_fence)
    assert "_desktidy_page_switch_refresh" in one
    assert "sync_icons" in one
    sched = inspect.getsource(DeskTidyApp._schedule_startup_overlays)
    assert "QTimer.singleShot(0, self._run_startup_overlays)" in sched

    from src.ui.fence_icon_item import start_file_drag
    from src.organizer import legacy_warehouse_has_files

    drag_src = inspect.getsource(start_file_drag)
    assert "rglob" not in drag_src
    assert "iter_fence_storage_lnk_files" in drag_src
    assert "rglob" not in inspect.getsource(legacy_warehouse_has_files)
    print("perf_regression_contracts_ok")


def test_storage_lnk_scan_is_shallow() -> None:
    import tempfile

    from src.settings import iter_fence_storage_lnk_files

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        (root / "a.lnk").write_bytes(b"")
        fence = root / "docs"
        fence.mkdir()
        (fence / "b.lnk").write_bytes(b"")
        nested = fence / "deep"
        nested.mkdir()
        (nested / "c.lnk").write_bytes(b"")
        found = {p.name for p in iter_fence_storage_lnk_files(root)}
    assert found == {"a.lnk", "b.lnk"}, found
    print("storage_lnk_scan_shallow_ok")


def test_onedir_packaging_contract() -> None:
    """Boot autostart must not extract a onefile archive into %TEMP% on login."""
    root = Path(__file__).resolve().parents[1]
    spec = (root / "DeskTidy.spec").read_text(encoding="utf-8")
    assert "exclude_binaries=True" in spec
    assert "COLLECT(" in spec
    assert "a.binaries" not in spec.split("exe = EXE(", 1)[1].split("exclude_binaries", 1)[0]
    iss = (root / "installer" / "DeskTidy.iss").read_text(encoding="utf-8")
    assert "dist\\DeskTidy\\*" in iss
    build = (root / "scripts" / "build.bat").read_text(encoding="utf-8")
    assert "dist\\DeskTidy\\DeskTidy.exe" in build
    print("onedir_packaging_ok")


def test_p3_perf_gate_contracts() -> None:
    """P3 gates: hook path no widgetAt; clipboard sleep only off-UI; watcher no resolve."""
    import inspect

    from src.app import DeskTidyApp
    from src.shell_clipboard import _open_clipboard
    from src.ui import fence_icon_item as fii
    from src.ui.pet_anim import trim_fit_cache
    from src.icon_utils import trim_display_icon_cache

    hook = inspect.getsource(fii._ll_should_claim_chord_hook)
    assert "widgetAt(" not in hook
    assert "QApplication.widgetAt" not in hook
    dispatch = inspect.getsource(
        __import__("src.desktop_ll_keyboard", fromlist=["x"])._dispatch
    )
    assert "widgetAt(" not in dispatch

    open_src = inspect.getsource(_open_clipboard)
    # Sleep is gated behind allow_sleep — UI default never blocks the event loop.
    assert "allow_sleep" in open_src
    assert "if allow_sleep and" in open_src
    assert "attempts = 8 if allow_sleep else 3" in open_src
    # Every sleep call must be indented (never module/function top-level).
    for ln in open_src.splitlines():
        if "time.sleep" in ln:
            assert ln[:1].isspace(), f"unconditional sleep: {ln!r}"
            assert "allow_sleep" in open_src.split("time.sleep", 1)[0][-80:]

    moved = inspect.getsource(DeskTidyApp._on_watched_file_moved)
    assert ".resolve(" not in moved
    assert "invalidate_path_stat_cache(src)" in moved
    assert "invalidate_path_stat_cache(dest)" in moved

    trim_bg = inspect.getsource(DeskTidyApp._trim_background_memory)
    assert "trim_display_icon_cache" in trim_bg
    assert "trim_fit_cache" in trim_bg
    assert callable(trim_display_icon_cache)
    assert callable(trim_fit_cache)

    from src import icon_utils as iu
    from src.ui import pet_anim as pa

    assert int(getattr(iu, "_MAX_DISPLAY_PIXMAP_CACHE")) == 256
    fit_src = inspect.getsource(pa._fit_sprite)
    assert "popitem" in fit_src
    assert "_FIT_CACHE.clear()" not in fit_src
    from src.ui.fence_icon_item import _FS_BACKGROUND_COPY_BYTES

    assert int(_FS_BACKGROUND_COPY_BYTES) == 2 * 1024 * 1024
    print("p3_perf_gate_contracts_ok")


def main() -> int:
    test_shared_ll_mouse_hook()
    test_hotkey_and_fence_filter_contracts()
    test_defview_cache_and_repair_cost()
    test_many_apps_pressure()
    test_menu_open_throttle()
    test_perf_regression_contracts()
    test_p3_perf_gate_contracts()
    test_storage_lnk_scan_is_shallow()
    test_onedir_packaging_contract()
    print("STRESS PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
