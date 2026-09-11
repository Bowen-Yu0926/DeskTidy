"""Regression: desktop chrome follows work screen, not primary-only."""

from __future__ import annotations

import inspect
import sys
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

_pass = 0
_fail = 0


def ok(msg: str) -> None:
    global _pass
    _pass += 1
    print(f"  OK  {msg}")


def fail(msg: str, err: BaseException | None = None) -> None:
    global _fail
    _fail += 1
    print(f"  FAIL  {msg}")
    if err is not None:
        traceback.print_exception(type(err), err, err.__traceback__)


def run(name: str, fn) -> None:
    try:
        fn()
        ok(name)
    except Exception as e:
        fail(name, e)


def test_work_screen_helpers() -> None:
    from PyQt6.QtCore import QPoint, QRect
    from PyQt6.QtWidgets import QApplication

    from src.ui import screen_snap as ss

    app = QApplication.instance() or QApplication([])
    primary = app.primaryScreen()
    assert primary is not None

    assert callable(ss.work_screen)
    assert ss.work_screen() is not None
    # Explicit point on primary → that screen.
    center = primary.availableGeometry().center()
    assert ss.work_screen(center) is primary

    geo = primary.availableGeometry()
    assert ss.rect_visible_on_any_screen(geo)
    assert not ss.rect_visible_on_any_screen(QRect(-50000, -50000, 40, 40))

    # Orphan snap lands on a real work area (not left at -50000),
    # and uses cursor work_screen() rather than the stale rect center.
    orphan = QRect(-50000, -50000, 80, 80)
    snapped = ss.snap_geometry(orphan)
    assert ss.rect_visible_on_any_screen(snapped)
    snap_src = inspect.getsource(ss.snap_geometry)
    assert "work_screen()" in snap_src
    assert "work_screen(geo.center())" not in snap_src


def test_chrome_uses_work_screen() -> None:
    from src.ui import dock_widget, page_indicator, recording_indicator, toast
    from src.ui import screen_snap as ss

    pi_src = inspect.getsource(page_indicator.PageIndicatorWidget._default_position)
    assert "work_screen" in pi_src
    assert "primaryScreen" not in pi_src

    restore = inspect.getsource(
        page_indicator.PageIndicatorWidget._restore_or_default_position
    )
    assert "rect_visible_on_any_screen" in restore

    toast_src = inspect.getsource(toast._ToastTip._place_top_center)
    assert "work_screen" in toast_src
    assert "_anchor" in toast_src
    # Recording toast may pin to capture rect; default still follows work screen.
    show_toast = inspect.getsource(toast.show_toast)
    assert "anchor" in show_toast

    rec_place = inspect.getsource(recording_indicator.RecordingIndicator._place_chrome)
    assert "_capture_target_rect" in rec_place
    assert "work_screen" in rec_place  # fallback when region missing
    assert "primaryScreen" not in rec_place
    strips = inspect.getsource(recording_indicator.RecordingIndicator._target_geometries)
    assert "_capture_target_rect" in strips

    dock_src = inspect.getsource(dock_widget.DockWidget._position)
    assert "work_screen" in dock_src
    assert "primaryScreen" not in dock_src

    assert callable(ss.work_screen)
    assert callable(ss.rect_visible_on_any_screen)


def test_page_indicator_orphan_saved_pos() -> None:
    from PyQt6.QtWidgets import QApplication

    from src.ui.page_indicator import PageIndicatorWidget
    from src.ui.screen_snap import rect_visible_on_any_screen

    app = QApplication.instance() or QApplication([])
    settings = {
        "theme": "mist",
        "page_folders": [],
        # Far off any monitor — must fall back to work-screen default.
        "page_indicator_pos": {"x": -80000, "y": -80000},
    }
    pages = [{"id": 0, "name": "默认"}]
    w = PageIndicatorWidget(pages, current_page=0, settings=settings)
    w.show()
    app.processEvents()
    assert rect_visible_on_any_screen(w.geometry())
    saved = settings.get("page_indicator_pos")
    assert isinstance(saved, dict)
    assert int(saved["x"]) > -10000


def main() -> int:
    print("DeskTidy multi-monitor chrome")
    run("work_screen + orphan snap", test_work_screen_helpers)
    run("chrome placement uses work_screen", test_chrome_uses_work_screen)
    run("page bar salvages orphan saved pos", test_page_indicator_orphan_saved_pos)
    print()
    print(f"通过 {_pass}  失败 {_fail}")
    return 0 if _fail == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
