"""Desktop layout: per-fence opacity slider + two-column snapshot-like cards."""

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


def test_opacity_slider_in_editor() -> None:
    from PyQt6.QtWidgets import QApplication, QSlider

    from src.ui.fence_editor import FenceEditDialog

    app = QApplication.instance() or QApplication([])
    src = inspect.getsource(FenceEditDialog.__init__)
    assert "opacity_slider" in src
    assert "QSlider" in src or "NoWheelSlider" in src
    dlg = FenceEditDialog(
        {
            "id": "t1",
            "name": "测",
            "style": {"opacity": 0.72, "background": "#C8EBD6"},
            "pages": [0],
        },
        [{"id": 0, "name": "工作"}],
    )
    assert hasattr(dlg, "opacity_slider")
    assert isinstance(dlg.opacity_slider, QSlider)
    assert dlg.opacity_slider.value() == 72
    dlg.opacity_slider.setValue(55)
    cfg = dlg.get_fence_config()
    assert abs(float(cfg["style"]["opacity"]) - 0.55) < 0.001
    dlg.close()
    dlg.deleteLater()
    app.processEvents()


def test_two_column_card_shell() -> None:
    from PyQt6.QtCore import Qt
    from PyQt6.QtWidgets import QApplication, QListWidget

    from src.ui.desktop_layout_widget import (
        DesktopLayoutWidget,
        _PAGE_ROW_H,
        _page_list_action_rects,
    )
    from PyQt6.QtCore import QRect

    app = QApplication.instance() or QApplication([])
    build = inspect.getsource(DesktopLayoutWidget._build_ui)
    assert "page_view" in build
    assert "fence_view" in build
    assert "PageFenceTreeWidget" not in build
    assert "虚拟桌面" in build
    assert "_configure_page_list_view" in build
    assert "_configure_fence_icon_view" in build
    assert "desktopPageListView" in build
    assert "desktopFenceIconView" in build
    assert "primaryBtn" not in build
    assert "_add_page_btn = None" in build
    assert "add_fence" in inspect.getsource(DesktopLayoutWidget._reload_fence_cards)

    settings = {
        "current_page": 0,
        "desktop_pages": [
            {"id": 0, "name": "工作", "organize_kinds": ["icon"]},
            {"id": 1, "name": "文档", "organize_kinds": ["file"]},
        ],
        "fences": [
            {
                "id": "f0",
                "name": "常用",
                "pages": [0],
                "visible": True,
                "style": {"opacity": 0.66, "background": "#2A2E35", "view_mode": "grid"},
            },
            {
                "id": "f0b",
                "name": "其他",
                "pages": [0],
                "visible": True,
                "style": {"opacity": 0.55, "background": "#F8D5B5", "view_mode": "grid"},
            },
            {
                "id": "f1",
                "name": "资料",
                "pages": [1],
                "visible": True,
                "style": {"opacity": 0.8, "background": "#C8EBD6"},
            },
        ],
        "show_page_indicator": True,
        "theme": "sky",
    }
    w = DesktopLayoutWidget(settings)
    assert isinstance(w.page_view, QListWidget)
    assert isinstance(w.fence_view, QListWidget)
    assert w.page_view.viewMode() == QListWidget.ViewMode.ListMode
    assert w.fence_view.viewMode() == QListWidget.ViewMode.IconMode
    from PyQt6.QtWidgets import QFrame

    assert w.page_view.frameShape() == QFrame.Shape.NoFrame
    from src.ui.styles import build_stylesheet

    sheet = build_stylesheet("sky")
    page_block = sheet.split("QListWidget#desktopPageListView {")[1].split("}")[0]
    assert "background: transparent" in page_block
    assert "border: none" in page_block
    assert (
        w.page_view.verticalScrollBarPolicy()
        == Qt.ScrollBarPolicy.ScrollBarAsNeeded
    )
    assert (
        w.fence_view.verticalScrollBarPolicy()
        == Qt.ScrollBarPolicy.ScrollBarAsNeeded
    )
    assert w.page_view.count() == 3  # 2 pages + compact add_page row
    assert w.page_view.item(0).sizeHint().height() == _PAGE_ROW_H
    from src.ui.desktop_layout_widget import _ROLE_OPACITY, _ROLE_OPACITY_PCT, _ROLE_TYPE

    assert w.page_view.item(2).data(_ROLE_TYPE) == "add_page"
    hits = _page_list_action_rects(QRect(0, 0, 240, _PAGE_ROW_H))
    assert set(hits) == {"edit", "delete"}
    assert hits["edit"].right() < hits["delete"].left()
    assert hits["edit"].width() == hits["delete"].width() == 26
    assert w._add_page_btn is None
    assert w._add_fence_btn is None
    # Compact 「+」 is a real QPushButton in the add_page row (not full-row paint).
    add_item = w.page_view.item(2)
    add_host = w.page_view.itemWidget(add_item)
    assert add_host is not None
    from PyQt6.QtWidgets import QPushButton

    add_btns = add_host.findChildren(QPushButton, "desktopLayoutAddBtn")
    assert len(add_btns) == 1
    add_btn = add_btns[0]
    assert add_btn.toolTip() == "新建分页"
    assert add_btn.width() == 28 and add_btn.height() == 28
    w.resize(960, 640)
    w.show()
    app.processEvents()
    row = w.page_view.visualItemRect(add_item)
    btn_tl = add_btn.mapTo(w.page_view.viewport(), add_btn.rect().topLeft())
    # Button stays on the left of the row — not shifted into empty chrome.
    assert btn_tl.x() <= row.left() + 8
    assert btn_tl.x() + add_btn.width() < row.center().x()
    # Empty area to the right of the button must not be treated as the control.
    filt = inspect.getsource(DesktopLayoutWidget.eventFilter)
    add_block = filt.split('== "add_page"')[1].split("if self._handle_card_click")[0]
    assert "_add_page()" not in add_block
    assert "return True" in add_block
    mount = inspect.getsource(DesktopLayoutWidget._mount_add_page_button)
    assert "setItemWidget" in mount
    assert "desktopLayoutAddPageHost" in mount
    assert w.fence_view.flow() == QListWidget.Flow.LeftToRight
    assert w.fence_view.isWrapping()
    assert w.fence_view.gridSize().width() >= 196
    from src.ui.desktop_layout_widget import _paint_action_icon_btn, _ACTION_ICON_KIND

    assert _ACTION_ICON_KIND["delete"] == "trash"
    assert _ACTION_ICON_KIND["toggle"] == "eye"
    assert callable(_paint_action_icon_btn)
    w.select_page_id(0)
    app.processEvents()

    # Two fences + trailing 「新建分区」 card.
    assert w.fence_view.count() == 3
    assert w.fence_view.item(2).data(_ROLE_TYPE) == "add_fence"
    assert w.fence_view.item(0).data(_ROLE_OPACITY_PCT) == "66%"
    # Selecting page 1 must not switch live desktop page.
    w.select_page_id(1)
    app.processEvents()
    assert settings["current_page"] == 0
    assert w.fence_view.count() == 2
    assert w.fence_view.item(1).data(_ROLE_TYPE) == "add_fence"
    assert w._selected_page_id() == 1
    # User click path: selection change must refresh right pane (not stale cache).
    sel_src = inspect.getsource(DesktopLayoutWidget._selected_page_id)
    assert sel_src.find("selectedItems") < sel_src.find("_selected_page is not None")
    w.select_page_id(0)
    app.processEvents()
    assert w.fence_view.count() == 3
    assert "工作" in w._fence_column_title.text()
    w.page_view.setCurrentItem(w.page_view.item(1))
    app.processEvents()
    assert w._selected_page_id() == 1
    assert w.fence_view.count() == 2
    assert "文档" in w._fence_column_title.text()
    paint = inspect.getsource(
        __import__("src.ui.desktop_layout_widget", fromlist=["x"])._LayoutCardDelegate.paint
    )
    assert "add_fence" in paint
    assert "新建分区" in paint
    page_paint = inspect.getsource(
        __import__("src.ui.desktop_layout_widget", fromlist=["x"])._PageListDelegate.paint
    )
    assert "add_page" in page_paint
    assert "drawRoundedRect(btn" not in page_paint
    assert "setItemWidget" in inspect.getsource(
        DesktopLayoutWidget._mount_add_page_button
    )
    cfg = inspect.getsource(DesktopLayoutWidget._configure_fence_icon_view)
    assert "LeftToRight" in cfg
    assert "setGridSize" in cfg
    assert "_paint_opacity_slider" in paint
    assert "fence_opacity_changed" in inspect.getsource(DesktopLayoutWidget)
    # Card slider under style chips updates preview data + emits live signal.
    from src.ui.desktop_layout_widget import (
        _THUMB_H,
        _THUMB_W,
        _card_content_rect,
        _fence_opacity_track_rect,
        _thumb_rect,
    )
    from PyQt6.QtCore import QPoint

    seen: list[tuple[str, float]] = []
    w.fence_opacity_changed.connect(lambda fid, op: seen.append((fid, op)))
    # Ensure page 0 selected with fence f0 first.
    w.select_page_id(0)
    w.resize(960, 640)
    w.show()
    app.processEvents()
    item0 = w.fence_view.item(0)
    assert str(item0.data(_ROLE_TYPE)) == "fence"
    card = _card_content_rect(w.fence_view.visualItemRect(item0))
    thumb = _thumb_rect(card, thumb_w=_THUMB_W, thumb_h=_THUMB_H)
    track = _fence_opacity_track_rect(thumb)
    assert track.width() > 20
    # Map to viewport-local pos (_apply_opacity_at uses visualItemRect).
    pos = QPoint(track.right() - 2, track.center().y())
    assert w._begin_opacity_drag(item0, pos) is True
    assert abs(float(item0.data(_ROLE_OPACITY)) - 1.0) < 0.05
    assert settings["fences"][0]["style"]["opacity"] > 0.9
    assert seen and seen[-1][0] == "f0"
    w._finish_opacity_drag(persist=True)
    assert "_on_fence_context_menu" in inspect.getsource(DesktopLayoutWidget) or (
        "移动到分页" in inspect.getsource(DesktopLayoutWidget)
    )
    from src.ui.main_window import MainWindow

    fences_src = inspect.getsource(MainWindow._build_fences_page)
    assert "QScrollArea" in fences_src
    assert "setWidgetResizable(True)" in fences_src
    w.close()
    w.deleteLater()
    app.processEvents()


def main() -> int:
    print("DeskTidy desktop layout cards / opacity")
    run("编辑分区透明度滑条", test_opacity_slider_in_editor)
    run("双栏分页列表/分区图标", test_two_column_card_shell)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err.splitlines()[0]}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
