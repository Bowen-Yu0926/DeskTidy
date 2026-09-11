"""Regression: Folder Portal (real-folder mirror fences)."""

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


def test_portal_helpers_and_listing() -> None:
    from src.fence_rules import (
        get_portal_path,
        is_portal_fence,
        list_portal_entries,
        migrate_simplify_fences,
        set_portal_path,
    )

    td = Path(tempfile.mkdtemp(prefix="desktidy_portal_"))
    (td / "a.txt").write_text("a", encoding="utf-8")
    (td / "b.txt").write_text("b", encoding="utf-8")
    (td / ".hidden").write_text("h", encoding="utf-8")

    fence: dict = {"id": "p1", "name": "下载", "virtual_items": []}
    assert not is_portal_fence(fence)
    set_portal_path(fence, td)
    assert is_portal_fence(fence)
    assert get_portal_path(fence) == td.resolve()
    names = {p.name for p in list_portal_entries(fence, {})}
    assert names == {"a.txt", "b.txt"}

    # Migration keeps portals with a path.
    settings = {"fences": [dict(fence)]}
    migrate_simplify_fences(settings)
    assert is_portal_fence(settings["fences"][0])
    assert settings["fences"][0].get("portal_path")

    # Incomplete type=portal without path is dropped.
    settings2 = {"fences": [{"id": "x", "name": "坏", "type": "portal"}]}
    migrate_simplify_fences(settings2)
    assert settings2["fences"] == []

    set_portal_path(fence, None)
    assert not is_portal_fence(fence)


def test_portal_widget_mode() -> None:
    from PyQt6.QtWidgets import QApplication

    from src.fence_rules import set_portal_path
    from src.ui.fence_widget import FenceWidget

    app = QApplication.instance() or QApplication([])
    td = Path(tempfile.mkdtemp(prefix="desktidy_portal_w_"))
    (td / "x.txt").write_text("x", encoding="utf-8")

    settings = {
        "theme": "mist",
        "fences": [],
        "current_page": 0,
        "exclude_patterns": [],
    }
    cfg = {
        "id": "portal1",
        "name": "门户",
        "pages": [0],
        "sort_by": "name",
        "style": {},
        "virtual_items": [],
        "x": 40,
        "y": 40,
        "width": 240,
        "height": 280,
    }
    set_portal_path(cfg, td)
    settings["fences"] = [cfg]
    w = FenceWidget(cfg, settings)
    assert w._is_portal_mode() is True
    assert w._is_virtual_mode() is False
    assert w._target_folder() == td.resolve()
    entries = w._get_entries()
    assert {p.name for p in entries} == {"x.txt"}

    # Virtual fence still virtual.
    cfg2 = {
        "id": "v1",
        "name": "虚拟",
        "pages": [0],
        "sort_by": "name",
        "style": {},
        "virtual_items": [],
    }
    w2 = FenceWidget(cfg2, settings)
    assert w2._is_virtual_mode() is True
    assert w2._is_portal_mode() is False

    src = inspect.getsource(FenceWidget._import_dropped_mime)
    assert "import_drop_to_folder" in src
    assert "portal_path" in inspect.getsource(
        __import__("src.ui.fence_editor", fromlist=["FenceEditDialog"]).FenceEditDialog
    )

    w.close()
    w.deleteLater()
    w2.close()
    w2.deleteLater()
    app.processEvents()


def test_watcher_schedules_portals() -> None:
    import inspect

    from src.app import DeskTidyApp
    from src.file_watcher import DesktopWatcher, desktop_watch_fingerprint

    src = inspect.getsource(DesktopWatcher.start)
    assert "collect_portal_watch_paths" in src
    assert "on_portal_changed" in src
    assert "PortalEventHandler" in inspect.getsource(
        __import__("src.file_watcher", fromlist=["x"])
    )
    fp = desktop_watch_fingerprint({"fences": []})
    assert isinstance(fp, tuple)

    rebuild = inspect.getsource(DeskTidyApp.rebuild_fences)
    assert "_start_watcher" in rebuild
    need = inspect.getsource(DeskTidyApp._fence_needs_icon_rebuild)
    assert "is_portal_fence" in need
    assert "get_portal_path" in need


def test_portal_delete_skips_virtual_unpin() -> None:
    import inspect

    from src.ui.fence_icon_item import delete_selected_fence_items
    from src.ui.fence_widget import FenceWidget

    del_src = inspect.getsource(delete_selected_fence_items)
    assert "is_portal_fence" in del_src
    unpin = inspect.getsource(FenceWidget._unpin_virtual_paths_impl)
    assert "_is_portal_mode" in unpin


def test_virtual_delete_does_not_place_public_float() -> None:
    """Delete must not reuse drag-out unpin (ghost float steals fence clicks)."""
    import inspect

    from src.ui.fence_icon_item import (
        _drop_fence_item_after_trash,
        _drop_fence_items_after_trash,
        delete_selected_fence_items,
    )
    from src.ui.fence_widget import FenceWidget

    del_src = inspect.getsource(delete_selected_fence_items)
    drop_src = inspect.getsource(_drop_fence_items_after_trash)
    wrap_src = inspect.getsource(_drop_fence_item_after_trash)
    assert "_drop_fence_items_after_trash" in del_src
    assert "_drop_fence_items_after_trash" in wrap_src
    assert "place_on_public=False" in drop_src
    assert "unpin_paths_from_virtual_fence" in drop_src
    # Must not emit the drag-out signal (that places a public float).
    assert "virtual_unpinned.emit" not in del_src
    assert "virtual_unpinned.emit" not in drop_src

    impl = inspect.getsource(FenceWidget._unpin_virtual_paths_impl)
    assert "place_on_public" in impl
    assert "if place_on_public:" in impl
    assert "add_fence_unpin_public_item" in impl
    assert "_abort_item_interaction" in impl

    abort = inspect.getsource(FenceWidget._abort_item_interaction)
    assert "_marquee_origin" in abort
    assert "clear_item_selection" in abort


def test_delete_does_not_rebuild_desktop() -> None:
    """Recycle Bin must drop one cell — not refresh_fences / force overlay restack."""
    import inspect

    from src.app import DeskTidyApp
    from src.file_watcher import PortalEventHandler
    from src.organize_suppress import suppress_desktop_item
    from src.ui.fence_icon_item import (
        FenceIconItem,
        after_shell_file_menu,
        delete_selected_fence_items,
        delete_selected_public_items,
        prepare_shell_item_invoke,
    )
    from src.ui.fence_widget import FenceItemLabel
    from src.ui.public_icon_widget import PublicIconWidget
    from src.shell_file_menu import show_file_context_menu

    del_src = inspect.getsource(delete_selected_fence_items)
    assert "_suppress_desktop_trash" in del_src
    assert "_drop_fence_items_after_trash" in del_src
    assert "refresh(force=True)" not in del_src
    assert "files_changed" not in del_src
    assert "refresh_desktop" not in del_src

    pub_src = inspect.getsource(delete_selected_public_items)
    assert "refresh_public_desktop" not in pub_src
    assert "_suppress_desktop_trash" in pub_src

    after = inspect.getsource(after_shell_file_menu)
    assert "_drop_fence_items_after_trash" in after
    assert "refresh_needed" not in after

    prep = inspect.getsource(prepare_shell_item_invoke)
    assert "is_delete_shell_verb" in prep
    assert "_suppress_desktop_trash" in prep

    for src in (
        inspect.getsource(FenceIconItem._show_menu),
        inspect.getsource(FenceItemLabel._show_menu),
        inspect.getsource(PublicIconWidget._show_menu),
    ):
        assert "after_shell_file_menu" in src
        assert "prepare_shell_item_invoke" in src
        assert "refresh_needed.emit" not in src

    menu_api = inspect.getsource(show_file_context_menu)
    assert "on_before_shell_invoke" in menu_api

    removed = inspect.getsource(DeskTidyApp._on_watched_file_removed)
    assert "is_organize_suppressed" in removed
    # Suppress must skip the debounce that calls refresh_fences.
    assert removed.index("is_organize_suppressed") < removed.index("_watch_debounce")

    portal_fn = inspect.getsource(DeskTidyApp._refresh_portal_fences)
    assert "force=True" not in portal_fn
    assert "_portal_dirty_dirs" in portal_fn

    fired: list[int] = []
    handler = PortalEventHandler(lambda _path="": fired.append(1))

    class _Ev:
        def __init__(self, name: str) -> None:
            self.src_path = f"C:/desktidy_del_test/{name}"

    suppress_desktop_item(r"C:/desktidy_del_test/gone.txt")
    handler.on_deleted(_Ev("gone.txt"))
    assert fired == []
    handler.on_deleted(_Ev("keep.txt"))
    assert fired == [1]


def main() -> int:
    print("DeskTidy Folder Portal")
    run("门户路径与列表", test_portal_helpers_and_listing)
    run("FenceWidget 门户模式", test_portal_widget_mode)
    run("监视器调度门户目录", test_watcher_schedules_portals)
    run("门户删除不走虚拟卸钉", test_portal_delete_skips_virtual_unpin)
    run("虚拟分区删除不挂公共浮动", test_virtual_delete_does_not_place_public_float)
    run("删除不整桌重绘", test_delete_does_not_rebuild_desktop)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
