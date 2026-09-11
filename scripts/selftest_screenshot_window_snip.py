"""Screenshot: modal snip + WeChat-style window hover/snap."""

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


def test_blocking_modal_allows_editor_dialogs() -> None:
    from PyQt6.QtWidgets import QApplication, QDialog, QLabel, QMessageBox, QVBoxLayout

    from src.screenshot_manager import ScreenshotManager

    app = QApplication.instance() or QApplication([])
    mgr = ScreenshotManager(lambda: {})

    dlg = QDialog()
    dlg.setWindowTitle("编辑分区")
    lay = QVBoxLayout(dlg)
    lay.addWidget(QLabel("test"))
    dlg.setModal(True)
    dlg.show()
    app.processEvents()
    # WindowModal editor must NOT block snip (一期).
    assert mgr._blocking_modal_open() is None
    dlg.close()
    dlg.deleteLater()

    box = QMessageBox()
    box.setText("confirm")
    box.setStandardButtons(QMessageBox.StandardButton.Ok)
    box.show()
    app.processEvents()
    # Nested MessageBox still blocked.
    assert mgr._blocking_modal_open() is box
    box.close()
    box.deleteLater()
    app.processEvents()

    block = inspect.getsource(ScreenshotManager._blocking_modal_open)
    assert "QMessageBox" in block


def test_snip_window_hit_and_expand() -> None:
    from PyQt6.QtCore import QPoint, QRect

    from src.snip_window_regions import (
        expand_rect_to_nearby_window,
        hit_test_snip_window,
    )

    big = QRect(10, 10, 400, 300)
    small = QRect(40, 40, 120, 80)
    # Z-order list: first = topmost. Point in both → topmost (big) wins.
    assert hit_test_snip_window([big, small], QPoint(50, 50)) == big
    # Small on top of big → small wins.
    assert hit_test_snip_window([small, big], QPoint(50, 50)) == small
    assert hit_test_snip_window([big, small], QPoint(15, 15)) == big
    assert hit_test_snip_window([big, small], QPoint(1, 1)) is None

    # Behind-window trap (DeskTidy over AI助手): smaller behind must not win.
    front = QRect(392, 115, 1136, 779)  # DeskTidy-like
    behind = QRect(234, 234, 996, 719)  # AI助手-like, overlaps lower area
    title_pt = QPoint(500, 140)  # upper chrome of front only
    body_pt = QPoint(600, 400)  # overlaps both
    assert hit_test_snip_window([front, behind], title_pt) == front
    assert hit_test_snip_window([front, behind], body_pt) == front
    # If behind were somehow topmost, body would hit behind — title still front-only.
    assert hit_test_snip_window([behind, front], body_pt) == behind
    assert hit_test_snip_window([behind, front], title_pt) == front

    rough = QRect(45, 45, 90, 60)
    # Drag fully inside both → expand to Z-order topmost (equal coverage tie).
    assert expand_rect_to_nearby_window(rough, [big, small]) == big
    assert expand_rect_to_nearby_window(rough, [small, big]) == small
    # Near-miss empty area: no expand.
    assert expand_rect_to_nearby_window(QRect(0, 0, 5, 5), [big, small]) == QRect(0, 0, 5, 5)

    from src.ui.screenshot_overlay import ScreenshotOverlay

    src = inspect.getsource(ScreenshotOverlay)
    assert "window_rects" in src
    assert "_update_hover_window" in src
    assert "_CLICK_DRAG_SLOP" in src
    assert "_hover_ready" in src
    assert "_draw_hover_chrome" in src

    # Idle paint path: no chrome until hover is armed + hit.
    paint = inspect.getsource(ScreenshotOverlay.paintEvent)
    assert "dim only" in paint or "Default: dim only" in paint
    assert "_draw_hover_chrome" in paint
    update = inspect.getsource(ScreenshotOverlay._update_hover_window)
    assert "manhattanLength" in update
    assert "_hover_ready" in update

    # Free drag must keep user rect — never snap=True on release.
    release = inspect.getsource(ScreenshotOverlay.mouseReleaseEvent)
    assert "snap=True" not in release
    assert "snap=False" in release


def test_snip_keeps_settings_window_snippable() -> None:
    """MainWindow must not be blanket-excluded (else behind windows steal hover)."""
    import inspect

    from src import snip_window_regions as swr

    src = inspect.getsource(swr._snip_exclude_root_hwnds)
    assert "_SKIP_OWN_CLASSES" in src
    assert "is_tool" in src
    # Settings stay snippable: no class-name exclude for the main settings window.
    assert '"MainWindow"' not in src and "'MainWindow'" not in src
    # Snapshot keeps EnumWindows order (no area sort of the result list).
    snap = inspect.getsource(swr.snapshot_snip_window_rects)
    assert "rects.sort" not in snap
    assert "topmost" in snap or "Z-order" in snap
    hit = inspect.getsource(swr.hit_test_snip_window)
    assert "hit_area" not in hit  # old smallest-wins path removed
    assert "for rect in rects" in hit


def test_snapshot_skips_cloaked_windows() -> None:
    """Cloaked UWP ghosts (e.g. closed 设置) must not become empty-area hits."""
    import win32gui
    from PyQt6.QtCore import QPoint, QRect
    from PyQt6.QtWidgets import QApplication

    from src.snip_window_regions import (
        _is_snip_window_candidate,
        _is_window_cloaked,
        snapshot_snip_window_rects,
    )

    QApplication.instance() or QApplication([])

    cloaked_rects: list[QRect] = []

    def _enum(hwnd, _ctx):
        hwnd = int(hwnd)
        if not win32gui.IsWindowVisible(hwnd) or win32gui.IsIconic(hwnd):
            return True
        if not _is_window_cloaked(hwnd):
            return True
        # Candidate filter must reject cloaked windows.
        assert _is_snip_window_candidate(hwnd) is False
        left, top, right, bottom = win32gui.GetWindowRect(hwnd)
        w, h = int(right - left), int(bottom - top)
        if w >= 24 and h >= 24:
            cloaked_rects.append(QRect(int(left), int(top), w, h))
        return True

    win32gui.EnumWindows(_enum, None)

    snap = snapshot_snip_window_rects(
        desk_origin=QPoint(0, 0), desk_size=(7680, 4320)
    )
    # No snapped rect may match a cloaked window's screen rect (ghost region).
    for ghost in cloaked_rects:
        local = QRect(ghost)  # origin 0,0
        for r in snap:
            assert r != local, f"cloaked ghost leaked into snapshot: {local}"


def test_hover_chrome_waits_for_mouse_move() -> None:
    from PyQt6.QtCore import QPoint, QRect, QSize
    from PyQt6.QtGui import QPixmap
    from PyQt6.QtWidgets import QApplication

    from src.ui.screenshot_overlay import ScreenshotOverlay

    app = QApplication.instance() or QApplication([])
    pm = QPixmap(QSize(200, 150))
    pm.fill()
    win = QRect(20, 20, 80, 60)
    overlay = ScreenshotOverlay(pm, QPoint(0, 0), window_rects=[win])
    # Before any move: no hover frame.
    assert overlay._hover_ready is False
    assert overlay._hover_rect is None
    overlay._update_hover_window(QPoint(40, 40))
    assert overlay._hover_ready is False
    assert overlay._hover_rect is None
    # Tiny jitter still disarmed.
    overlay._update_hover_window(QPoint(41, 40))
    assert overlay._hover_ready is False
    assert overlay._hover_rect is None
    # Real move → recognize window under cursor.
    overlay._update_hover_window(QPoint(50, 45))
    assert overlay._hover_ready is True
    assert overlay._hover_rect == win
    # Off window → clear frame again.
    overlay._update_hover_window(QPoint(5, 5))
    assert overlay._hover_rect is None
    overlay.close()
    overlay.deleteLater()
    app.processEvents()


def test_begin_capture_wires_window_rects() -> None:
    from src.screenshot_manager import ScreenshotManager

    begin = inspect.getsource(ScreenshotManager._begin_capture)
    assert "snapshot_snip_window_rects" in begin
    assert "window_rects=" in begin


def main() -> int:
    print("DeskTidy screenshot modal + window snip")
    run("编辑分区不挡截图 / MessageBox 仍挡", test_blocking_modal_allows_editor_dialogs)
    run("悬停命中与拖选吸附扩窗", test_snip_window_hit_and_expand)
    run("设置主窗可截（不误命中下层窗）", test_snip_keeps_settings_window_snippable)
    run("跳过 cloaked 幽灵窗（空地误识别）", test_snapshot_skips_cloaked_windows)
    run("默认无框，移动后才识别出框", test_hover_chrome_waits_for_mouse_move)
    run("begin_capture 传入 window_rects", test_begin_capture_wires_window_rects)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err.splitlines()[0]}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
