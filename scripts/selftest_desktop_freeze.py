"""Runtime probe: desktop freeze regressions (pet click / owned plate / restack).

Exercises the hot paths that used to freeze the GUI thread (and WH_MOUSE_LL)
until the user switched displays. Pass = timings stay under budgets and
contracts hold; does not require a live DeskTidy process.
"""

from __future__ import annotations

import inspect
import os
import sys
import time
import traceback
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
import scripts.selftest_env  # noqa: F401, E402

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

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


def test_source_contracts() -> None:
    from src.app import DeskTidyApp
    from src.ui import pet_widget as pet_mod
    from src.ui.pet_widget import DesktopPetWidget
    from src.ui.public_icon_host import PublicIconHost

    ensure = inspect.getsource(DeskTidyApp.ensure_live_fences_interactive)
    skip = ensure.split("if skip_raise:", 1)[1].split("return", 1)[0]
    assert "_seat_live_overlays_under_apps" not in skip
    assert "_raise_live_overlays_above_defview" not in skip
    assert "_sink_public_host_below_overlays" in skip

    # Single owner for Win+D / reveal band repair.
    restore = inspect.getsource(DeskTidyApp._restore_desktop_band_overlays)
    assert "_raise_live_overlays_above_defview" in restore
    show = inspect.getsource(DeskTidyApp._on_show_desktop_relayed)
    assert "_schedule_desktop_band_restore" in show
    assert "_reveal_desktop_for_page_nav" not in show

    restack_body = inspect.getsource(pet_mod._restack_fences_after_pet_raise).split(
        '"""', 2
    )[-1]
    assert "ensure_live_fences_interactive" not in restack_body

    end = inspect.getsource(DesktopPetWidget._end_drag)
    assert "did_drag" in end and "_raise_pet_if_safe" in end
    assert "_remap_hidden_overlay_hwnds" not in end
    drag = inspect.getsource(DesktopPetWidget._update_drag)
    assert "_restack_fences_after_pet_raise" not in drag

    refresh = inspect.getsource(PublicIconHost.refresh_click_mask)
    assert "huge_plate" in refresh and "clearMask" in refresh
    hit = inspect.getsource(PublicIconHost.hit_test_region)
    assert "_hit_region_cache" in hit


def test_owned_plate_refresh_budget() -> None:
    """Pet-move mask churn on a full-desktop plate must stay cheap."""
    from PyQt6.QtCore import QPoint, QRect
    from PyQt6.QtWidgets import QApplication, QWidget

    from src.ui.public_icon_host import PublicIconHost

    app = QApplication.instance() or QApplication([])
    host = PublicIconHost()
    # Large enough that owned hit region is a "huge plate".
    host.setGeometry(QRect(0, 0, 1920, 1080))
    host.show()

    icon = QWidget(host)
    icon.setObjectName("probeIcon")
    icon.__class__.__name__ = "PublicIconWidget"  # type: ignore[misc]
    # public_icon_widgets filters by class name string — use a real class.
    from src.ui.public_icon_widget import PublicIconWidget

    # Prefer a real float if constructible; otherwise stub host cache.
    try:
        float_icon = PublicIconWidget(
            {
                "path": str(ROOT / "README.md"),
                "name": "probe",
                "x": 80,
                "y": 80,
            },
            parent=host,
        )
        float_icon.move(80, 80)
        float_icon.show()
    except Exception:
        host._icon_cache = [icon]  # type: ignore[attr-defined]
        icon.setGeometry(80, 80, 72, 90)
        icon.show()

    fake = SimpleNamespace(
        _exiting=False,
        _icons_hidden=False,
        settings={"hide_shell_icons": True},
        fences=[],
        pet_widget=None,
    )
    app._desktidy_app = fake  # type: ignore[attr-defined]

    setmask_n = {"n": 0}
    orig_set = host.setMask

    def _spy_set(region):
        setmask_n["n"] += 1
        return orig_set(region)

    host.setMask = _spy_set  # type: ignore[method-assign]

    with patch("src.win_shell.are_desktop_icons_visible", return_value=False):
        assert host.session_owns_desktop_drops() is True
        # Warm once.
        host.invalidate_icon_cache()
        host.refresh_click_mask()
        app.processEvents()
        assert host._desktop_plate_unmasked is True
        setmask_n["n"] = 0

        # Simulate pet hopping: invalidate + refresh many times.
        t0 = time.perf_counter()
        for i in range(40):
            fake.pet_widget = SimpleNamespace(
                isVisible=lambda: True,
                frameGeometry=lambda i=i: QRect(400 + i * 3, 500, 180, 240),
                mask=lambda: None,
                mapToGlobal=lambda p, i=i: QPoint(400 + i * 3 + p.x(), 500 + p.y()),
            )
            host.invalidate_icon_cache()
            host.schedule_mask_refresh()
            # Drive the coalesced timer immediately for the probe.
            host._mask_timer.stop()
            host.refresh_click_mask()
        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        app.processEvents()

    # Full-desktop plate must not thrash SetWindowRgn.
    assert setmask_n["n"] == 0, f"huge plate setMask count={setmask_n['n']}"
    # 40 refreshes of a 1920×1080 plate must not stall the GUI thread.
    assert elapsed_ms < 800.0, f"owned plate refresh too slow: {elapsed_ms:.1f}ms"
    print(f"      owned-plate ×40 = {elapsed_ms:.1f}ms (setMask={setmask_n['n']})")


def test_hit_region_cache_avoids_rebuild() -> None:
    from PyQt6.QtCore import QRect
    from PyQt6.QtWidgets import QApplication

    from src.ui.public_icon_host import PublicIconHost

    app = QApplication.instance() or QApplication([])
    host = PublicIconHost()
    host.setGeometry(QRect(0, 0, 1280, 720))
    fake = SimpleNamespace(
        _exiting=False,
        _icons_hidden=False,
        settings={"hide_shell_icons": True},
        fences=[],
        pet_widget=None,
    )
    app._desktidy_app = fake  # type: ignore[attr-defined]

    builds = {"n": 0}
    orig = host._build_hit_test_region

    def _spy():
        builds["n"] += 1
        return orig()

    host._build_hit_test_region = _spy  # type: ignore[method-assign]

    with patch("src.win_shell.are_desktop_icons_visible", return_value=False):
        host._icon_cache = [SimpleNamespace()]  # type: ignore[attr-defined]
        # Force has_visible_icons path via cache — public_icon_widgets uses cache.
        host.public_icon_widgets = lambda: [SimpleNamespace()]  # type: ignore[method-assign]
        host.hit_test_region()
        host.hit_test_region()
        host.hit_test_region()
        assert builds["n"] == 1, builds
        host.invalidate_icon_cache()
        host.public_icon_widgets = lambda: [SimpleNamespace()]  # type: ignore[method-assign]
        host.hit_test_region()
        assert builds["n"] == 2, builds


def test_ensure_live_skips_seat_when_not_explorer() -> None:
    from src.app import DeskTidyApp

    seats: list[str] = []
    raises: list[str] = []

    fake = object.__new__(DeskTidyApp)
    fake.fences = []
    fake.pet_widget = None
    fake._public_icon_host = None
    fake._exiting = False
    fake._overlay_mgmt_suspended = False
    fake._ensure_public_host_mouse_opaque = lambda: None  # type: ignore[method-assign]
    fake._sink_public_host_below_overlays = lambda *a, **k: None  # type: ignore[method-assign]
    fake._seat_live_overlays_under_apps = lambda: seats.append("seat")  # type: ignore[method-assign]
    fake._raise_live_overlays_above_defview = lambda: raises.append("raise")  # type: ignore[method-assign]
    fake._foreign_app_owns_foreground = lambda: True  # type: ignore[method-assign]

    with patch("src.win_shell.is_explorer_desktop_foreground", return_value=False):
        with patch("src.win_shell.set_overlay_mouse_passthrough", lambda *a, **k: None):
            DeskTidyApp.ensure_live_fences_interactive(fake)

    assert seats == [], seats
    assert raises == [], raises


def test_pet_click_does_not_restack_band() -> None:
    """mouseRelease without drag must not call restack (freeze trigger)."""
    from src.ui.pet_widget import DesktopPetWidget

    src = inspect.getsource(DesktopPetWidget.mouseReleaseEvent)
    # Release always calls _end_drag; freeze guard is inside _end_drag.
    assert "_end_drag" in src
    end = inspect.getsource(DesktopPetWidget._end_drag)
    assert "if did_drag:" in end
    assert end.index("if did_drag:") < end.index("_raise_pet_if_safe")


def main() -> int:
    print("DeskTidy desktop-freeze probes")
    run("source contracts", test_source_contracts)
    run("owned plate refresh budget", test_owned_plate_refresh_budget)
    run("hit region cache", test_hit_region_cache_avoids_rebuild)
    run("ensure_live no seat off explorer FG", test_ensure_live_skips_seat_when_not_explorer)
    run("pet click skips band restack", test_pet_click_does_not_restack_band)
    print()
    print(f"通过 {len(passes)}  失败 {len(failures)}")
    for name, detail in failures:
        print(f"--- {name} ---")
        print(detail)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
