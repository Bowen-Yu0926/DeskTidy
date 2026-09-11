"""Regression: public float multi-select + multi-drag."""

from __future__ import annotations

import inspect
import sys
import tempfile
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


def test_public_multi_select_drag() -> None:
    from PyQt6.QtCore import QPoint, Qt
    from PyQt6.QtGui import QRegion
    from PyQt6.QtWidgets import QApplication

    from src.ui.fence_icon_item import (
        apply_public_click_selection,
        paths_for_public_drag,
        select_public_item,
        start_public_item_drag,
    )
    from src.ui.public_icon_host import PublicIconHost
    from src.ui.public_icon_widget import PublicIconWidget

    app = QApplication.instance() or QApplication([])
    td = Path(tempfile.mkdtemp(prefix="desktidy_pubsel_"))
    files = [td / f"p{i}.txt" for i in range(3)]
    for f in files:
        f.write_text("x", encoding="utf-8")

    host = PublicIconHost()
    icons = [
        PublicIconWidget(files[i], 80 + i * 110, 100, parent=host) for i in range(3)
    ]
    for icon in icons:
        icon.show()
    host.present()
    app.processEvents()
    apply_public_click_selection(icons[0], Qt.KeyboardModifier.NoModifier)
    assert icons[0].is_selected() and not icons[1].is_selected()
    assert host._selection_anchor is icons[0]

    apply_public_click_selection(icons[2], Qt.KeyboardModifier.ShiftModifier)
    assert all(ic.is_selected() for ic in icons)

    select_public_item(icons[1], exclusive=True)
    host._selection_anchor = icons[1]
    apply_public_click_selection(icons[0], Qt.KeyboardModifier.ControlModifier)
    assert icons[0].is_selected() and icons[1].is_selected() and not icons[2].is_selected()

    drag_paths = paths_for_public_drag(icons[0])
    assert len(drag_paths) == 2
    assert {str(p).casefold() for p in drag_paths} == {
        str(files[0]).casefold(),
        str(files[1]).casefold(),
    }

    assert "paths_for_public_drag" in inspect.getsource(PublicIconWidget.mouseMoveEvent)
    assert "apply_public_click_selection" in inspect.getsource(
        PublicIconWidget.mousePressEvent
    )
    assert "paths:" in inspect.getsource(start_public_item_drag)
    assert "drag_paths" in inspect.getsource(start_public_item_drag)
    assert "_begin_marquee" in inspect.getsource(PublicIconHost)
    assert "hit_test_region" in inspect.getsource(PublicIconHost)
    hit = inspect.getsource(PublicIconHost._icon_hit_region)
    # Sparse layouts must keep padded corridors — footprint-only blocked marquee.
    assert "adjusted(-pad" in hit or "pad =" in hit
    assert "paintEvent" in inspect.getsource(PublicIconHost)
    host_mod = inspect.getsource(PublicIconHost.paintEvent)
    # Qt6: QRegion is not iterable — paint via QPainterPath.addRegion + drawPath
    # (fillRect over full plate used to freeze MainThread on keepalive update).
    assert "_HIT_PLATE" in host_mod
    assert "drawPath" in host_mod or "fillRect" in host_mod
    assert "addRegion" in host_mod or "fillRect" in host_mod
    assert "_hit_plate_region" in host_mod
    plate_mod = inspect.getsource(PublicIconHost._hit_plate_region)
    # Content carve-out keeps 框选 gaps; must not use full child.geometry() only.
    assert "_content_hit_region" in plate_mod and "subtracted" in plate_mod
    assert "child.geometry()" not in plate_mod
    assert "grabMouse" in inspect.getsource(PublicIconHost._begin_marquee)
    assert "releaseMouse" in inspect.getsource(PublicIconHost._finish_marquee) or (
        "releaseMouse" in inspect.getsource(PublicIconHost._abort_marquee)
        and "_abort_marquee" in inspect.getsource(PublicIconHost._finish_marquee)
    )
    assert "releaseMouse" in inspect.getsource(PublicIconHost._abort_marquee)
    assert "_sync_content_hit_mask" in inspect.getsource(PublicIconWidget)
    assert "setMask" in inspect.getsource(PublicIconWidget._sync_content_hit_mask)

    # Content mask: outer widget margin (outside caption shelf) must not hit.
    icons[0]._sync_content_hit_mask()
    app.processEvents()
    # Far-left of the 124px cell sits outside the centered 116px caption shelf.
    side = QPoint(1, 20)
    assert not icons[0]._pos_on_content(side), icons[0]._content_hit_rect()
    assert host.childAt(icons[0].mapTo(host, side)) is None
    mid_local = icons[0].icon_label.geometry().center()
    assert host.childAt(icons[0].mapTo(host, mid_local)) is icons[0]
    # Hit region is one rectangle (AABB), not a T-shaped ink union.
    hit = icons[0]._content_hit_region()
    br = hit.boundingRect()
    assert hit == QRegion(br), (hit.boundingRect(), "expected rectangular plate")
    # Corners of that plate must select (regression: T-mask missed AABB corners).
    corner = QPoint(br.left() + 2, br.bottom() - 2)
    assert icons[0]._pos_on_content(corner), (corner, br)

    # Selection chrome = fixed caption shelf (not ink-tight to short names).
    from src.ui.fence_icon_item import public_selection_chrome_rect

    short_file = td / "ai-coder.txt"
    long_file = td / ("很长很长很长很长很长的文件名测试用.xlsx")
    short_file.write_text("x", encoding="utf-8")
    long_file.write_text("x", encoding="utf-8")
    icons[0].apply_renamed_path(short_file)
    app.processEvents()
    chrome_short = public_selection_chrome_rect(icons[0])
    assert chrome_short.isValid() and not chrome_short.isEmpty()
    icons[0].apply_renamed_path(long_file)
    app.processEvents()
    chrome_long = public_selection_chrome_rect(icons[0])
    assert chrome_short.width() == chrome_long.width(), (chrome_short, chrome_long)
    assert chrome_short.height() == chrome_long.height(), (
        chrome_short,
        chrome_long,
        "selection chrome height must not follow caption ink",
    )
    assert chrome_short.width() >= icons[0]._label_width - 4, (
        chrome_short,
        icons[0]._label_width,
    )
    # Selected: still the same plate (no Explorer-style expand by filename length).
    icons[0].set_selected(True)
    app.processEvents()
    chrome_sel_long = public_selection_chrome_rect(icons[0])
    icons[0].apply_renamed_path(short_file)
    app.processEvents()
    chrome_sel_short = public_selection_chrome_rect(icons[0])
    assert chrome_sel_short.height() == chrome_sel_long.height(), (
        chrome_sel_short,
        chrome_sel_long,
    )
    assert icons[0].height() == icons[0]._widget_h_min
    assert icons[0].text_label.height() == icons[0]._label_height
    # Restore a short name for later layout checks.
    icons[0].apply_renamed_path(short_file)
    app.processEvents()
    icons[0]._sync_content_hit_mask()
    assert icons[0]._content_hit_rect() == public_selection_chrome_rect(icons[0])

    # Column layout: overlapping 124×108 shelves. Marquee starts in margins that
    # sit inside a child rect but outside content mask — plate must still ink them.
    host.setGeometry(0, 0, 800, 900)
    for i, icon in enumerate(icons):
        icon.move(100, 80 + i * 72)
        icon.show()
        icon._sync_content_hit_mask()
    host.invalidate_icon_cache()
    app.processEvents()
    shelf_gap = None
    for ic in icons:
        for lx in range(0, ic.width(), 4):
            for ly in range(0, min(ic.height(), 40), 4):
                lp = QPoint(lx, ly)
                if ic._pos_on_content(lp):
                    continue
                hp = ic.mapTo(host, lp)
                if host.childAt(hp) is not None:
                    continue
                if host.hit_test_region().contains(hp):
                    shelf_gap = hp
                    break
            if shelf_gap is not None:
                break
        if shelf_gap is not None:
            break
    assert shelf_gap is not None, "expected open shelf margin between stacked floats"
    assert host._hit_plate_region().contains(shelf_gap), shelf_gap
    # Glyph center must stay unplated (alpha=0) so layered host does not steal
    # clicks from a fence stacked underneath the same screen pixels.
    glyph = icons[0].mapTo(host, icons[0].icon_label.geometry().center())
    assert not host._hit_plate_region().contains(glyph), glyph

    # Sparse dual-monitor-sized host: without owning desktop, gaps only near icons.
    host.setGeometry(0, 0, 3840, 1080)
    for i, icon in enumerate(icons):
        icon.setGeometry(80 + i * 120, 100, 96, 112)
        icon.show()
    host.invalidate_icon_cache()
    region = host.hit_test_region()
    assert not region.isEmpty()
    # Midpoint between first two icons must be hittable (marquee start).
    mid = QPoint(80 + 96 + 12, 100 + 56)
    assert region.contains(mid), mid
    # Far empty wallpaper stays click-through when shell icons are visible / unowned.
    assert not region.contains(QPoint(3000, 900))
    # Owning the desktop (shell icons hidden) expands the plate for blank 框选.
    from unittest.mock import patch
    from types import SimpleNamespace

    fake = SimpleNamespace(_exiting=False, _icons_hidden=False, settings={"hide_shell_icons": True})
    app._desktidy_app = fake  # type: ignore[attr-defined]
    try:
        with patch("src.win_shell.are_desktop_icons_visible", return_value=False):
            assert host.session_owns_desktop_drops() is True
            # Host may sit on a negative virtual origin (multi-mon). Use a point
            # inside the work-area plate, not a hardcoded screen coord.
            host.sync_desktop_geometry()
            owned = host.hit_test_region()
            br = owned.boundingRect()
            assert not br.isEmpty(), "owned plate empty"
            far = QPoint(br.x() + max(40, br.width() - 80), br.y() + br.height() // 2)
            assert owned.contains(far), f"blank wallpaper must 框选 when owning desktop ({far})"
    finally:
        if hasattr(app, "_desktidy_app"):
            delattr(app, "_desktidy_app")
    assert "nativeEvent" in inspect.getsource(PublicIconHost)
    assert "HTTRANSPARENT" in inspect.getsource(PublicIconHost)
    assert "_hit_public_icon_at" in inspect.getsource(PublicIconHost)
    assert "_show_desktop_background_menu" not in inspect.getsource(PublicIconHost)
    assert "_forward_empty_plate_rmb" in inspect.getsource(PublicIconHost)
    # PyQt6 sip: must not call super().nativeEvent with voidptr (fatal crash).
    ne_src = inspect.getsource(PublicIconHost.nativeEvent)
    assert "super().nativeEvent" not in ne_src
    assert "from_address" in ne_src
    ef_src = inspect.getsource(PublicIconHost.eventFilter)
    assert "request_desktop_background_menu" in inspect.getsource(
        PublicIconHost._forward_empty_plate_rmb
    )
    assert "show_folder_background_menu" not in ef_src

    for icon in icons:
        icon.close()
        icon.deleteLater()
    host.close()
    host.deleteLater()
    app.processEvents()
    for f in files:
        f.unlink(missing_ok=True)


def test_app_protects_multi_drag_keys() -> None:
    import inspect

    from src.app import DeskTidyApp

    assert callable(DeskTidyApp._set_public_drag_paths)
    src = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "_public_drag_keys" in src
    flush = inspect.getsource(DeskTidyApp._flush_public_refresh_after_drag)
    assert "_public_drag_keys" in flush


def test_multi_select_move_to_fence() -> None:
    """Public floats + public desktop: no RMB「移动到分区」; fence→fence still works."""
    import inspect

    from PyQt6.QtCore import Qt
    from PyQt6.QtWidgets import QApplication

    from src.app import DeskTidyApp
    from src.fence_rules import get_virtual_items_for_fence
    from src.public_desktop import add_public_item, get_public_items, move_paths_to_fence
    from src.ui.fence_icon_item import (
        apply_public_click_selection,
        build_file_icon_shell_extras,
        paths_for_icon_shell_action,
    )
    from src.ui.public_icon_host import PublicIconHost
    from src.ui.public_icon_widget import PublicIconWidget

    app = QApplication.instance() or QApplication([])
    td = Path(tempfile.mkdtemp(prefix="desktidy_movefence_"))
    files = [td / f"m{i}.txt" for i in range(2)]
    for f in files:
        f.write_text("x", encoding="utf-8")

    settings = {
        "current_page": 1,
        "enable_public_desktop": True,
        "public_desktop_items": [],
        "fences": [
            {
                "id": "system_docs",
                "name": "文档",
                "pages": [1],
                "virtual_items": [],
                "sort_by": "manual",
                "organize_kinds": ["file"],
            },
            {
                "id": "work_box",
                "name": "工作箱",
                "pages": [1],
                "virtual_items": [],
                "sort_by": "manual",
                "organize_kinds": ["file"],
            },
        ],
        "desktop_pages": [
            {"id": 0, "name": "工作"},
            {"id": 1, "name": "文档"},
        ],
    }
    for i, f in enumerate(files):
        add_public_item(
            settings, f, x=20 + i * 100, y=40, page_id=1, auto_arrange=False, shared=False
        )

    host = PublicIconHost()
    icons = [
        PublicIconWidget(files[i], 80 + i * 110, 100, parent=host) for i in range(2)
    ]
    for icon in icons:
        icon.show()
    host.present()
    app.processEvents()

    apply_public_click_selection(icons[0], Qt.KeyboardModifier.NoModifier)
    apply_public_click_selection(icons[1], Qt.KeyboardModifier.ControlModifier)
    assert all(ic.is_selected() for ic in icons)

    action_paths = paths_for_icon_shell_action(icons[0], files[0])
    assert len(action_paths) == 2

    extras = build_file_icon_shell_extras(
        file_path=files[0],
        settings=settings,
        desk=None,
        anchor=icons[0],
        action_paths=action_paths,
    )
    labels = [str(getattr(cmd, "label", "")) for cmd in extras]
    assert not any(lab.startswith("移动到分区：") for lab in labels), labels
    assert not any(lab.startswith("移动到分页：") for lab in labels), labels

    class _FakeDesk:
        def __init__(self) -> None:
            self.settings = settings
            self._exiting = False
            self.window = type(
                "W",
                (),
                {
                    "fence_editor": type(
                        "E", (), {"reload_table": lambda self: None}
                    )()
                },
            )()

        def _fences_should_show(self) -> bool:
            return False

        def refresh_public_desktop(self, **_kw) -> None:
            return None

    fake = _FakeDesk()
    assert (
        DeskTidyApp.move_desktop_items_to_fence(fake, action_paths, "system_docs")
        is False
    )
    assert DeskTidyApp.move_desktop_items_to_page(fake, action_paths, 0) is False
    assert get_public_items(settings)
    assert not get_virtual_items_for_fence(settings["fences"][0], settings)

    fence_files = [td / f"f{i}.txt" for i in range(2)]
    for f in fence_files:
        f.write_text("y", encoding="utf-8")
    settings["fences"][0]["virtual_items"] = [str(p) for p in fence_files]
    fence_extras = build_file_icon_shell_extras(
        file_path=fence_files[0],
        settings=settings,
        desk=None,
        virtual_mode=True,
        source_fence_id="system_docs",
        action_paths=fence_files,
    )
    fence_labels = [str(getattr(cmd, "label", "")) for cmd in fence_extras]
    assert any(lab.startswith("移动到分区：工作箱") for lab in fence_labels), fence_labels
    move_cmd = next(
        cmd for cmd in fence_extras if str(cmd.label).startswith("移动到分区：")
    )
    closed = inspect.getsource(move_cmd.callback)
    assert "items" in closed or "move_desktop_items_to_fence" in inspect.getsource(
        build_file_icon_shell_extras
    )
    assert move_paths_to_fence(settings, fence_files, "work_box") is True
    pins = get_virtual_items_for_fence(settings["fences"][1], settings)
    assert {p.name for p in pins} == {"f0.txt", "f1.txt"}

    for icon in icons:
        icon.close()
        icon.deleteLater()
    host.close()
    host.deleteLater()
    app.processEvents()
    for f in files + fence_files:
        f.unlink(missing_ok=True)


def test_multi_select_delete_and_shell_menu() -> None:
    """框选多文件删除 / 右键删除 must act on the whole selection."""
    import inspect
    from unittest.mock import patch

    from PyQt6.QtCore import Qt
    from PyQt6.QtWidgets import QApplication

    from src.shell_file_menu import (
        _acquire_files_context_menu,
        _normalize_menu_paths,
        _same_parent_menu_paths,
        show_file_context_menu,
        show_shell_context_menu,
    )
    from src.ui.fence_icon_item import (
        apply_public_click_selection,
        delete_selected_fence_items,
        delete_selected_public_items,
        paths_for_icon_shell_action,
    )
    from src.ui.fence_widget import FenceIconItem, FenceWidget
    from src.ui.public_icon_host import PublicIconHost
    from src.ui.public_icon_widget import PublicIconWidget

    # Shell host must accept multi paths and build multi-PIDL menus.
    menu_src = inspect.getsource(show_shell_context_menu)
    assert "paths" in menu_src
    assert "_acquire_files_context_menu" in menu_src
    assert "_same_parent_menu_paths" in inspect.getsource(_same_parent_menu_paths)
    assert "paths=action_paths" in inspect.getsource(PublicIconWidget._show_menu)
    assert "paths=action_paths" in inspect.getsource(FenceIconItem._show_menu)
    assert "related_paths" in inspect.getsource(show_file_context_menu) or (
        "paths" in inspect.getsource(show_file_context_menu)
    )

    app = QApplication.instance() or QApplication([])
    td = Path(tempfile.mkdtemp(prefix="desktidy_multidel_"))
    files = [td / f"d{i}.txt" for i in range(2)]
    for f in files:
        f.write_text("x", encoding="utf-8")

    norms = _normalize_menu_paths(files[0], files)
    assert len(norms) == 2
    assert _same_parent_menu_paths(norms) == norms

    # Multi-PIDL acquire must request GetUIObjectOf with 2 pidls (same folder).
    class _FakeFolder:
        def __init__(self) -> None:
            self.pidl_names: list[str] = []

        def ParseDisplayName(self, host, bind, name):  # noqa: N802
            self.pidl_names.append(str(name))
            return 0, ("pidl", str(name)), 0

        def GetUIObjectOf(self, host, pidls, iid, reserved):  # noqa: N802
            self.last_pidls = list(pidls)
            return (0, "fake-cm")

    fake = _FakeFolder()
    with patch(
        "src.shell_file_menu._parent_shell_folder", return_value=fake
    ), patch(
        "src.shell_file_menu._acquire_file_context_menu",
        side_effect=AssertionError("multi must not fall back to single"),
    ):
        cm = _acquire_files_context_menu(0, norms)
    assert cm == "fake-cm"
    assert len(fake.last_pidls) == 2

    # Del / chord delete: fence batch trash + one unpin.
    settings = {
        "current_page": 0,
        "fences": [
            {
                "id": "f1",
                "name": "F",
                "pages": [0],
                "virtual_items": [str(files[0]), str(files[1])],
                "sort_by": "manual",
                "style": {},
            }
        ],
        "public_desktop_items": [],
    }
    fence = FenceWidget(dict(settings["fences"][0]), settings)
    fence.setGeometry(40, 40, 400, 400)
    fence.show()
    app.processEvents()
    fence.refresh()
    app.processEvents()
    items = fence._icon_item_widgets()
    assert len(items) == 2
    for w in items:
        w.set_selected(True)

    unpin_calls: list[list] = []

    def _capture_unpin(paths, *, place_on_public=True):
        unpin_calls.append([Path(p) for p in paths])
        # Still drop pins for realism.
        from src.fence_rules import unpin_paths_from_virtual_fence

        unpin_paths_from_virtual_fence(fence.config, [Path(p) for p in paths])
        fence._sync_fence_virtual_items()
        fence._remove_virtual_icon_widgets([Path(p) for p in paths])

    with patch(
        "src.ui.fence_icon_item.delete_to_trash",
        side_effect=lambda p: Path(p).unlink(missing_ok=True),
    ), patch.object(fence, "_unpin_virtual_paths_impl", side_effect=_capture_unpin):
        delete_selected_fence_items(items[0])
    assert len(unpin_calls) == 1, unpin_calls
    assert {p.name for p in unpin_calls[0]} == {"d0.txt", "d1.txt"}
    assert not files[0].exists() and not files[1].exists()

    # Public multi-delete recycles both without depending on removed.emit loops.
    files2 = [td / f"p{i}.txt" for i in range(2)]
    for f in files2:
        f.write_text("y", encoding="utf-8")
    host = PublicIconHost()
    icons = [
        PublicIconWidget(files2[i], 80 + i * 110, 100, parent=host) for i in range(2)
    ]
    for icon in icons:
        icon.show()
    host.present()
    app.processEvents()
    apply_public_click_selection(icons[0], Qt.KeyboardModifier.NoModifier)
    apply_public_click_selection(icons[1], Qt.KeyboardModifier.ControlModifier)
    assert len(paths_for_icon_shell_action(icons[0], files2[0])) == 2

    with patch(
        "src.ui.fence_icon_item.delete_to_trash",
        side_effect=lambda p: Path(p).unlink(missing_ok=True),
    ):
        delete_selected_public_items(icons[0])
    assert not files2[0].exists() and not files2[1].exists()

    del_src = inspect.getsource(delete_selected_fence_items)
    assert "_drop_fence_items_after_trash" in del_src
    pub_src = inspect.getsource(delete_selected_public_items)
    assert "remove_public_paths" in pub_src

    for icon in icons:
        icon.close()
        icon.deleteLater()
    host.close()
    host.deleteLater()
    fence.close()
    fence.deleteLater()
    app.processEvents()


def main() -> int:
    print("DeskTidy public multi-select / multi-drag")
    run("Ctrl/Shift 选中与多路径拖拽", test_public_multi_select_drag)
    run("刷新保护多选拖拽路径", test_app_protects_multi_drag_keys)
    run("多选移动到分区", test_multi_select_move_to_fence)
    run("多选删除与多PIDL菜单", test_multi_select_delete_and_shell_menu)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
