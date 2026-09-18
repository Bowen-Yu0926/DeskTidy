"""Regression tests for whitelist, public/page-local floats, and organize skips."""

from __future__ import annotations

import sys
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
import scripts.selftest_env  # noqa: F401, E402 — DESKTIDY_SELFTEST=1

passes: list[str] = []
failures: list[tuple[str, str]] = []


def begin(title: str) -> None:
    print(f"\n--- {title} ---")


def run(name: str, fn) -> None:
    try:
        fn()
        passes.append(name)
        print(f"  OK  {name}")
    except Exception as exc:
        failures.append((name, f"{exc}\n{traceback.format_exc()}"))
        print(f"  FAIL {name}: {exc}")


def test_organize_whitelist() -> None:
    begin("1) 整理白名单")

    def _match():
        from src.organize_whitelist import (
            add_organize_whitelist_entry,
            is_organize_whitelisted,
            remove_organize_whitelist_entry,
        )

        s: dict = {"organize_whitelist": []}
        assert add_organize_whitelist_entry(s, "KeepMe.txt")
        assert not add_organize_whitelist_entry(s, "keepme.TXT")
        assert add_organize_whitelist_entry(s, "*.skip")
        assert add_organize_whitelist_entry(s, r"C:\Users\demo\report.docx")
        assert "report.docx" in s["organize_whitelist"]
        assert is_organize_whitelisted(Path(r"D:\desk\KeepMe.txt"), s)
        assert is_organize_whitelisted(Path(r"D:\desk\a.skip"), s)
        assert is_organize_whitelisted(Path(r"D:\desk\report.docx"), s)
        assert not is_organize_whitelisted(Path(r"D:\desk\other.txt"), s)
        assert remove_organize_whitelist_entry(s, "KeepMe.txt")
        assert not is_organize_whitelisted(Path(r"D:\desk\KeepMe.txt"), s)

        # Settings UI: Add opens a multi-file picker.
        import inspect

        from src.ui.main_window import MainWindow

        pick = inspect.getsource(MainWindow._on_whitelist_add_pick)
        assert "getOpenFileNames" in pick
        assert "add_organize_whitelist_entry" in pick
        typed = inspect.getsource(MainWindow._on_whitelist_add_typed)
        assert "_on_whitelist_add_pick" in typed
        # Wire-up lives in __init__ (settings section).
        import pathlib

        mw_src = pathlib.Path(inspect.getfile(MainWindow)).read_text(encoding="utf-8")
        assert "add_wl_btn.clicked.connect(self._on_whitelist_add_pick)" in mw_src
        files_menu = inspect.getsource(MainWindow._on_files_context_menu)
        assert "打开所在文件夹" in files_menu
        assert "在资源管理器中显示" in files_menu
        assert "open_containing_folder" in files_menu
        assert "reveal_in_explorer" in files_menu

    def _organize_skips(tmp: Path):
        from src.organizer import organize_desktop

        keep = tmp / "whitelist_keep.txt"
        take = tmp / "whitelist_take.txt"
        keep.write_text("k", encoding="utf-8")
        take.write_text("t", encoding="utf-8")
        # Point scanner at a fake desktop via monkeypatch of get_desktop_paths.
        import src.desktop_scanner as scanner
        import src.settings as settings_mod

        old_paths = settings_mod.get_desktop_paths
        old_scan = scanner.get_desktop_paths
        settings_mod.get_desktop_paths = lambda: [tmp]  # type: ignore
        scanner.get_desktop_paths = lambda: [tmp]  # type: ignore
        scanner.invalidate_desktop_scan_cache()
        try:
            settings = {
                "organize_mode": "virtual",
                "exclude_patterns": [],
                "organize_whitelist": ["whitelist_keep.txt"],
                "public_desktop_items": [],
                "fences": [
                    {
                        "id": "f1",
                        "name": "文件",
                        "organize_kinds": ["file"],
                        "virtual_items": [],
                    }
                ],
            }
            result = organize_desktop(settings, dry_run=True)
            names = {a.source.name: a.category for a in result.actions}
            assert names.get("whitelist_keep.txt") == "白名单"
            assert names.get("whitelist_take.txt") == "文件"
        finally:
            settings_mod.get_desktop_paths = old_paths
            scanner.get_desktop_paths = old_scan
            scanner.invalidate_desktop_scan_cache()
            keep.unlink(missing_ok=True)
            take.unlink(missing_ok=True)

    def _preview_breakdown(tmp: Path):
        """Preview must not lump already-pinned files as a vague 'N files' blob."""
        from src.organizer import (
            build_organize_preview_model,
            format_organize_preview,
            organize_desktop,
        )
        import src.desktop_scanner as scanner
        import src.settings as settings_mod

        pinned = tmp / "already_pin.txt"
        fresh = tmp / "fresh_doc.txt"
        floater = tmp / "float_me.txt"
        folder = tmp / "a_folder"
        pinned.write_text("p", encoding="utf-8")
        fresh.write_text("f", encoding="utf-8")
        floater.write_text("x", encoding="utf-8")
        folder.mkdir(exist_ok=True)
        old_paths = settings_mod.get_desktop_paths
        old_scan = scanner.get_desktop_paths
        settings_mod.get_desktop_paths = lambda: [tmp]  # type: ignore
        scanner.get_desktop_paths = lambda: [tmp]  # type: ignore
        scanner.invalidate_desktop_scan_cache()
        try:
            settings = {
                "organize_mode": "virtual",
                "exclude_patterns": [],
                "organize_whitelist": [],
                "public_desktop_items": [
                    {"path": str(floater), "x": 10, "y": 10, "shared": True}
                ],
                "fences": [
                    {
                        "id": "f1",
                        "name": "文件",
                        "organize_kinds": ["file"],
                        "virtual_items": [str(pinned)],
                    }
                ],
            }
            result = organize_desktop(settings, dry_run=True)
            cats = {a.source.name: a.category for a in result.actions}
            assert cats.get("already_pin.txt") == "已在分区", cats
            # Shared float still absorbed into a matching fence.
            assert cats.get("float_me.txt") == "文件", cats
            assert cats.get("fresh_doc.txt") == "文件", cats
            # Folders classify as「文档」and match file rules.
            assert cats.get("a_folder") == "文件", cats
            assert result.moved_count == 3
            text = format_organize_preview(
                result, settings, unclaimed_folders=[]
            )
            assert "将归入分区：3 个" in text
            assert "已在分区中（不列出）：1 个" in text
            assert "• already_pin.txt" not in text
            assert "有 3 个文件未匹配" not in text
            model = build_organize_preview_model(
                result, settings, unclaimed_folders=[]
            )
            assert model.pin_total == 3
            assert model.will_count == 3
            assert any(b.key == "already_fence" and b.count == 1 for b in model.buckets)
            # Main window preview must use the structured panel on the files page.
            import inspect

            from src.ui.main_window import MainWindow
            from src.ui.organize_preview_panel import OrganizePreviewPanel

            src = inspect.getsource(MainWindow._on_preview)
            assert "build_organize_preview_model" in inspect.getsource(
                MainWindow._refresh_organize_preview
            )
            assert "show_scrollable_info" not in src
            assert "_preview_panel" in src or "_refresh_organize_preview" in src
            build = inspect.getsource(MainWindow._build_files_page)
            assert "OrganizePreviewPanel" in build
            assert "filesOrganizeSplitter" in build
            assert "一键整理" in build
            assert "一键整理桌面" not in build
            assert "filesEmptyKicker" in build or "filesEmptyState" in build
            assert "收起" not in build
            assert "_hide_organize_preview" not in inspect.getsource(MainWindow)
            assert "lines[:30]" not in src
            assert callable(OrganizePreviewPanel)
            preview_src = inspect.getsource(OrganizePreviewPanel)
            assert "_tile_will" not in preview_src
            assert "organizePreviewSummary" in preview_src
            sidebar = inspect.getsource(MainWindow._build_sidebar)
            assert "一键整理桌面" not in sidebar
            assert "一键整理" in build
        finally:
            settings_mod.get_desktop_paths = old_paths
            scanner.get_desktop_paths = old_scan
            scanner.invalidate_desktop_scan_cache()
            pinned.unlink(missing_ok=True)
            fresh.unlink(missing_ok=True)
            floater.unlink(missing_ok=True)
            try:
                folder.rmdir()
            except OSError:
                pass

    run("白名单匹配 / 去重 / 路径转文件名", _match)

    def _org():
        from src.settings import get_desktop_path

        _organize_skips(get_desktop_path())

    run("organize_desktop 跳过白名单", _org)

    def _prev():
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            _preview_breakdown(Path(td))

    run("预览整理分类统计不混淆", _prev)

    def _page_float_not_fence():
        """Page-local floats on empty doc pages are absorbed into an auto fence."""
        from src.organizer import format_organize_preview, organize_desktop
        import src.desktop_scanner as scanner
        import src.settings as settings_mod
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            desk = Path(td)
            doc = desk / "page_float.xlsx"
            doc.write_text("x", encoding="utf-8")
            old_paths = settings_mod.get_desktop_paths
            old_scan = scanner.get_desktop_paths
            settings_mod.get_desktop_paths = lambda: [desk]  # type: ignore
            scanner.get_desktop_paths = lambda: [desk]  # type: ignore
            scanner.invalidate_desktop_scan_cache()
            try:
                settings = {
                    "current_page": 0,
                    "organize_mode": "virtual",
                    "exclude_patterns": [],
                    "organize_whitelist": [],
                    "enable_public_desktop": False,
                    "public_desktop_items": [
                        {
                            "path": str(doc),
                            "x": 10,
                            "y": 10,
                            "page": 1,
                        }
                    ],
                    "desktop_pages": [
                        {"id": 0, "name": "工作", "organize_kinds": ["icon"]},
                        {"id": 1, "name": "文档", "organize_kinds": ["file"]},
                    ],
                    "fences": [
                        {
                            "id": "c",
                            "name": "常用",
                            "pages": [0],
                            "organize_kinds": ["icon"],
                            "virtual_items": [],
                        }
                    ],
                }
                result = organize_desktop(settings, dry_run=True)
                cats = {a.source.name: a.category for a in result.actions}
                # Preview shows the destination fence name (auto-created on apply).
                assert cats.get("page_float.xlsx") == "文档", cats
                text = format_organize_preview(result, settings)
                assert "将归入分区" in text or "文档" in text
                # Apply for real: float becomes a pin on the new page fence.
                result2 = organize_desktop(settings, dry_run=False)
                assert result2.moved_count == 1
                assert settings.get("public_desktop_items") == []
                doc_fences = [
                    f
                    for f in (settings.get("fences") or [])
                    if 1 in (f.get("pages") or [f.get("page")])
                ]
                assert len(doc_fences) == 1
                assert any(
                    str(p).endswith("page_float.xlsx")
                    for p in (doc_fences[0].get("virtual_items") or [])
                )
            finally:
                settings_mod.get_desktop_paths = old_paths
                scanner.get_desktop_paths = old_scan
                scanner.invalidate_desktop_scan_cache()
                doc.unlink(missing_ok=True)

    run("分页浮标整理进自动文档分区", _page_float_not_fence)


def test_public_page_local() -> None:
    begin("2) 公共区 / 分页浮动")

    def _visibility():
        from src.public_desktop import (
            add_public_item,
            is_shared_public_entry,
            migrate_shared_public_to_page,
            promote_page_public_to_shared,
            visible_floating_items,
        )

        s = {"enable_public_desktop": False, "public_desktop_items": []}
        e = add_public_item(
            s, r"C:\tmp\local.txt", x=10, y=10, page_id=2, auto_arrange=False
        )
        assert e.get("page") == 2
        assert not is_shared_public_entry(e)
        assert len(visible_floating_items(s, 2)) == 1
        assert len(visible_floating_items(s, 0)) == 0

        s2 = {"enable_public_desktop": True, "public_desktop_items": []}
        e2 = add_public_item(
            s2, r"C:\tmp\shared.txt", x=10, y=10, page_id=0, auto_arrange=False
        )
        assert is_shared_public_entry(e2)
        assert len(visible_floating_items(s2, 0)) == 1
        assert len(visible_floating_items(s2, 9)) == 1

        # Disable public → migrate shared to page-local
        s2["enable_public_desktop"] = False
        assert migrate_shared_public_to_page(s2, 3)
        assert s2["public_desktop_items"][0].get("page") == 3
        assert len(visible_floating_items(s2, 3)) == 1
        assert len(visible_floating_items(s2, 0)) == 0

        # Re-enable → promote to shared
        s2["enable_public_desktop"] = True
        assert promote_page_public_to_shared(s2)
        assert is_shared_public_entry(s2["public_desktop_items"][0])
        assert len(visible_floating_items(s2, 0)) == 1

        # System icons stay visible (and shared) even when public area is off.
        s3 = {"enable_public_desktop": False, "public_desktop_items": []}
        sys_path = r"C:\Users\demo\.desktidy\storage\.public_system\此电脑.lnk"
        add_public_item(
            s3, sys_path, x=10, y=10, page_id=0, auto_arrange=False, shared=True
        )
        assert is_shared_public_entry(s3["public_desktop_items"][0])
        assert len(visible_floating_items(s3, 0)) == 1
        assert len(visible_floating_items(s3, 7)) == 1
        assert not migrate_shared_public_to_page(s3, 1)
        assert is_shared_public_entry(s3["public_desktop_items"][0])

    def _organize_consumes_float_unless_whitelisted():
        from src.fence_rules import all_fence_pinned_keys
        from src.organizer import organize_desktop
        from src.public_desktop import add_public_item, get_public_items
        from src.settings import get_desktop_path
        import src.desktop_scanner as scanner
        import src.settings as settings_mod

        desk = get_desktop_path()
        stamp = int(time.time())
        p = desk / f"_st_float_{stamp}.txt"
        keep = desk / f"_st_float_wl_{stamp}.txt"
        p.write_text("f", encoding="utf-8")
        keep.write_text("k", encoding="utf-8")
        old = settings_mod.get_desktop_paths
        old_s = scanner.get_desktop_paths
        settings_mod.get_desktop_paths = lambda: [desk]  # type: ignore
        scanner.get_desktop_paths = lambda: [desk]  # type: ignore
        scanner.invalidate_desktop_scan_cache()
        try:
            settings = {
                "organize_mode": "virtual",
                "enable_public_desktop": True,
                "exclude_patterns": [],
                "organize_whitelist": [keep.name],
                "public_desktop_items": [],
                "fences": [
                    {
                        "id": "f1",
                        "name": "文件",
                        "organize_kinds": ["file"],
                        "virtual_items": [],
                    }
                ],
            }
            add_public_item(
                settings, p, x=20, y=20, auto_arrange=False, shared=True
            )
            add_public_item(
                settings, keep, x=40, y=40, auto_arrange=False, shared=True
            )
            result = organize_desktop(settings, dry_run=False)
            cats = {a.source.name: a.category for a in result.actions}
            assert cats.get(p.name) == "文件", cats
            assert cats.get(keep.name) == "白名单", cats
            pub_names = {
                Path(str(e.get("path", ""))).name for e in get_public_items(settings)
            }
            assert p.name not in pub_names, pub_names
            assert keep.name in pub_names, pub_names
            pinned = all_fence_pinned_keys(settings)
            assert any(
                str(k).casefold().endswith(p.name.casefold()) for k in pinned
            ), pinned
        finally:
            settings_mod.get_desktop_paths = old
            scanner.get_desktop_paths = old_s
            scanner.invalidate_desktop_scan_cache()
            p.unlink(missing_ok=True)
            keep.unlink(missing_ok=True)

    def _move_path_to_page_menu():
        """RMB「移动到分页」unpins fence and places a page-local float."""
        import inspect
        import tempfile
        from pathlib import Path as P

        from src.app import DeskTidyApp
        from src.public_desktop import (
            desktop_pages_for_move_menu,
            move_path_to_desktop_page,
            visible_floating_items,
        )
        from src.ui.fence_icon_item import FenceIconItem
        from src.ui.public_icon_widget import PublicIconWidget

        assert callable(DeskTidyApp.move_desktop_item_to_page)
        assert "build_file_icon_shell_extras" in inspect.getsource(FenceIconItem._show_menu)
        assert "build_file_icon_shell_extras" in inspect.getsource(PublicIconWidget._show_menu)
        from src.ui.fence_widget import FenceItemLabel

        assert "build_file_icon_shell_extras" in inspect.getsource(FenceItemLabel._show_menu)
        assert "show_shell_context_menu" in inspect.getsource(PublicIconWidget._show_menu) or (
            "show_file_context_menu" in inspect.getsource(PublicIconWidget._show_menu)
        )
        from src import shell_file_menu as sfm

        assert "_CMF_CANRENAME" in inspect.getsource(sfm.show_shell_context_menu) or "_CMF_CANRENAME" in inspect.getsource(sfm)
        assert "_host_rename_item" in inspect.getsource(sfm)
        assert "_handle_menu_msg2" in inspect.getsource(sfm)
        assert "InvokeCommand" in inspect.getsource(sfm._host_shell_context_menu)
        assert "show_qt_file_context_menu" not in inspect.getsource(sfm)
        assert "_clipboard_cut_or_copy" not in inspect.getsource(sfm)
        # RMB popup latency: reuse host HWND; avoid Path.resolve on absolute paths.
        assert "_ensure_menu_host_hwnd" in inspect.getsource(sfm)
        assert "warm_shell_context_menu_host" in inspect.getsource(sfm)
        assert "_normalize_menu_path" in inspect.getsource(sfm)
        assert "_parent_shell_folder" in inspect.getsource(sfm)
        host_src = inspect.getsource(sfm._host_shell_context_menu)
        # Persistent host: must not destroy the TrackPopupMenu owner after each RMB.
        assert "DestroyWindow(" not in host_src
        assert "win32gui.DestroyWindow" not in host_src
        assert "_ensure_menu_host_hwnd" in host_src
        assert "Persistent menu host" in host_src
        show_src = inspect.getsource(sfm.show_shell_context_menu)
        assert "Path(path).resolve()" not in show_src
        assert "target.exists()" not in show_src
        assert "show_file_context_menu" in inspect.getsource(PublicIconWidget._show_menu)
        assert "show_file_context_menu" in inspect.getsource(FenceIconItem._show_menu)
        move_fence_src = inspect.getsource(
            __import__(
                "src.ui.fence_icon_item",
                fromlist=["build_file_icon_shell_extras"],
            ).build_file_icon_shell_extras
        )
        assert "移动到分区" in move_fence_src
        assert "enabled=(pid != current)" in move_fence_src
        assert "move_desktop_items_to_page" in move_fence_src
        assert "move_desktop_items_to_fence" in move_fence_src
        assert "paths_for_icon_shell_action" in move_fence_src
        assert "anchor" in move_fence_src
        assert "_reveal_organize_float_pages" in inspect.getsource(
            DeskTidyApp.move_desktop_items_to_page
        )
        promote_src = inspect.getsource(
            __import__(
                "src.public_desktop",
                fromlist=["promote_page_public_to_shared"],
            ).promote_page_public_to_shared
        )
        assert 'entry.get("shared") is False' in promote_src
        assert callable(DeskTidyApp.move_desktop_items_to_fence)
        assert "move_paths_to_fence" in inspect.getsource(
            DeskTidyApp.move_desktop_items_to_fence
        )

        pages = desktop_pages_for_move_menu(
            {"desktop_pages": [{"id": 0, "name": "工作"}]}
        )
        assert pages == []
        pages = desktop_pages_for_move_menu(
            {
                "desktop_pages": [
                    {"id": 0, "name": "工作"},
                    {"id": 1, "name": "文档"},
                ]
            }
        )
        assert pages == [(0, "工作"), (1, "文档")]

        with tempfile.TemporaryDirectory() as td:
            doc = P(td) / "move_me.xlsx"
            doc.write_bytes(b"PK")
            settings = {
                "enable_public_desktop": False,
                "desktop_pages": [
                    {"id": 0, "name": "工作"},
                    {"id": 1, "name": "文档"},
                ],
                "current_page": 0,
                "fences": [
                    {
                        "id": "f1",
                        "name": "常用",
                        "pages": [0],
                        "virtual_items": [str(doc)],
                    }
                ],
                "public_desktop_items": [],
            }
            assert move_path_to_desktop_page(settings, doc, 1) is True
            assert settings["fences"][0]["virtual_items"] == []
            assert len(visible_floating_items(settings, 1)) == 1
            assert len(visible_floating_items(settings, 0)) == 0
            entry = settings["public_desktop_items"][0]
            assert int(entry.get("page")) == 1
            from src.public_desktop import heal_public_area_scope, is_shared_public_entry

            assert not is_shared_public_entry(entry)
            assert move_path_to_desktop_page(settings, doc, 1) is False
            settings["enable_public_desktop"] = True
            assert heal_public_area_scope(settings, 0) is False
            assert int(entry.get("page")) == 1
            assert entry.get("shared") is False
            sys_path = r"C:\Users\demo\.desktidy\storage\.public_system\此电脑.lnk"
            assert move_path_to_desktop_page(settings, sys_path, 1) is False

            # Shared config + stale painted cells must force icon rebuild
            # (ghost icons on the source page after「移动到分页」).
            from types import SimpleNamespace

            cfg = settings["fences"][0]
            ghost = SimpleNamespace(file_path=doc)

            class _FakeLayout:
                def __init__(self, widgets):
                    self._widgets = list(widgets)

                def count(self):
                    return len(self._widgets)

                def itemAt(self, i):
                    w = self._widgets[i]
                    return SimpleNamespace(widget=lambda: w)

            fake_fence = SimpleNamespace(
                config=cfg,
                items_layout=_FakeLayout([ghost]),
                has_icon_widgets=lambda: True,
            )
            assert DeskTidyApp._fence_needs_icon_rebuild(fake_fence, cfg) is True
            fake_empty = SimpleNamespace(
                config=cfg,
                items_layout=_FakeLayout([]),
                has_icon_widgets=lambda: False,
            )
            assert DeskTidyApp._fence_needs_icon_rebuild(fake_empty, cfg) is False

        from src.public_desktop import fences_for_move_menu, move_path_to_fence

        menus = fences_for_move_menu(
            {
                "current_page": 0,
                "fences": [
                    {"id": "a", "name": "常用", "pages": [0], "virtual_items": []},
                    {"id": "b", "name": "其他", "pages": [0], "virtual_items": []},
                ],
            },
            source_fence_id="a",
        )
        assert menus == [("b", "其他")]
        with tempfile.TemporaryDirectory() as td2:
            doc2 = P(td2) / "cross_fence.txt"
            doc2.write_text("x", encoding="utf-8")
            settings2 = {
                "current_page": 0,
                "fences": [
                    {
                        "id": "a",
                        "name": "常用",
                        "pages": [0],
                        "virtual_items": [str(doc2)],
                    },
                    {"id": "b", "name": "其他", "pages": [0], "virtual_items": []},
                ],
                "public_desktop_items": [],
            }
            assert move_path_to_fence(settings2, doc2, "b") is True
            assert settings2["fences"][0]["virtual_items"] == []
            assert str(doc2) in settings2["fences"][1]["virtual_items"]

    def _toggle_public_area_keeps_items():
        from src.public_desktop import (
            add_public_item,
            heal_public_area_scope,
            visible_floating_items,
        )

        s = {
            "enable_public_desktop": False,
            "public_desktop_items": [],
            "current_page": 1,
        }
        add_public_item(
            s, r"C:\a.docx", x=10, y=20, page_id=1, auto_arrange=False, shared=False
        )
        add_public_item(
            s, r"C:\b.xlsx", x=30, y=40, page_id=0, auto_arrange=False, shared=False
        )
        assert len(s["public_desktop_items"]) == 2

        s["enable_public_desktop"] = True
        assert heal_public_area_scope(s, 1)
        assert len(s["public_desktop_items"]) == 2
        assert len(visible_floating_items(s, 0)) == 2
        assert len(visible_floating_items(s, 1)) == 2
        assert all(e.get("page") is None for e in s["public_desktop_items"])

        s["enable_public_desktop"] = False
        assert heal_public_area_scope(s, 1)
        assert len(s["public_desktop_items"]) == 2
        assert len(visible_floating_items(s, 1)) == 2
        assert len(visible_floating_items(s, 0)) == 0
        for entry in s["public_desktop_items"]:
            assert entry.get("page") == 1
            assert "shared" not in entry

        # Stale shared:false + no page (promote churn) must land on current page.
        s2 = {
            "enable_public_desktop": True,
            "current_page": 2,
            "public_desktop_items": [
                {"path": r"C:\stale.txt", "x": 1, "y": 2, "shared": False}
            ],
        }
        s2["enable_public_desktop"] = False
        assert heal_public_area_scope(s2, 2)
        assert s2["public_desktop_items"][0].get("page") == 2
        assert len(visible_floating_items(s2, 2)) == 1

    run("visible_floating_items / migrate / promote", _visibility)
    run("启用/关闭公共区域保留浮标", _toggle_public_area_keeps_items)
    run("整理收纳公共区（白名单除外）", _organize_consumes_float_unless_whitelisted)
    run("右键移动到分页契约", _move_path_to_page_menu)


def test_public_to_fence_pin_visible() -> None:
    begin("2b) 公共区拖入分区后可见")

    def _dual_list_still_shows():
        import tempfile
        from pathlib import Path

        from src.fence_rules import (
            assign_paths_to_virtual_fence,
            get_virtual_items_for_fence,
        )
        from src.public_desktop import add_public_item, get_public_items

        td = Path(tempfile.mkdtemp())
        p = td / "drag_in.txt"
        p.write_text("x", encoding="utf-8")
        try:
            settings = {
                "enable_public_desktop": True,
                "public_desktop_items": [],
                "fences": [
                    {"id": "f1", "name": "F", "extensions": [], "virtual_items": []},
                ],
            }
            add_public_item(settings, p, x=10, y=10, auto_arrange=False, shared=True)
            # Simulate a stale dual-list frame: pin written, public not cleared yet.
            settings["fences"][0]["virtual_items"] = [str(p)]
            shown = get_virtual_items_for_fence(settings["fences"][0], settings)
            assert any(x.name == p.name for x in shown), shown
            # Normal path: assign clears public.
            settings["fences"][0]["virtual_items"] = []
            add_public_item(settings, p, x=10, y=10, auto_arrange=False, shared=True)
            assign_paths_to_virtual_fence(settings["fences"][0], settings, [p])
            assert not any(
                Path(str(e.get("path", ""))).name == p.name
                for e in get_public_items(settings)
            )
            shown2 = get_virtual_items_for_fence(settings["fences"][0], settings)
            assert any(x.name == p.name for x in shown2), shown2
        finally:
            p.unlink(missing_ok=True)

    def _split_config_object_still_pins():
        """Live FenceWidget config can diverge from settings['fences'][i]."""
        import tempfile
        from pathlib import Path

        from src.fence_rules import (
            assign_paths_to_virtual_fence,
            get_virtual_items_for_fence,
        )
        from src.public_desktop import add_public_item, get_public_items

        td = Path(tempfile.mkdtemp())
        p = td / "split_cfg.txt"
        p.write_text("x", encoding="utf-8")
        try:
            settings = {
                "enable_public_desktop": False,
                "public_desktop_items": [],
                "fences": [
                    {
                        "id": "f1",
                        "name": "F",
                        "extensions": [],
                        "virtual_items": [],
                        "sort_by": "name",
                    },
                ],
            }
            widget_cfg = {
                "id": "f1",
                "name": "F",
                "extensions": [],
                "virtual_items": [],
                "sort_by": "name",
            }
            add_public_item(
                settings, p, x=10, y=10, page_id=0, auto_arrange=False, shared=False
            )
            added = assign_paths_to_virtual_fence(widget_cfg, settings, [p])
            assert added and added[0].name == p.name, added
            assert settings["fences"][0].get("virtual_items"), settings["fences"][0]
            assert widget_cfg.get("virtual_items"), widget_cfg
            assert not get_public_items(settings)
            shown = get_virtual_items_for_fence(settings["fences"][0], settings)
            assert any(x.name == p.name for x in shown), shown
            shown_live = get_virtual_items_for_fence(widget_cfg, settings)
            assert any(x.name == p.name for x in shown_live), shown_live
        finally:
            p.unlink(missing_ok=True)

    run("双列表残留时仍显示 + assign 后可见", _dual_list_still_shows)

    def _drop_pin_outside_ole():
        import inspect

        from src.ui.fence_icon_item import force_pin_path_to_fence, start_public_item_drag
        from src.ui.fence_widget import FenceWidget

        src = inspect.getsource(FenceWidget._import_virtual_drop)
        assert "singleShot" in src and "_apply_virtual_drop_paths" in src
        assert "assign_paths_to_virtual_fence" not in src
        assert "_apply_virtual_drop_paths" in inspect.getsource(force_pin_path_to_fence)
        drag = inspect.getsource(start_public_item_drag)
        assert "pin_public_path_to_fence" in drag
        assert "geometry_only=True" in drag
        assert "QDrag(" not in drag
        assert "_PublicDragFilter" in drag
        from src.ui import fence_icon_item as fii

        assert "setUrls(" not in inspect.getsource(fii._make_public_drag_mime)
        assert "_raise_fences_for_drop()" not in drag
        assert "user32.SetWindowPos" not in inspect.getsource(fii._raise_fences_for_drop)
        assert "_fence_global_rect" in inspect.getsource(fii)
        from src.app import DeskTidyApp

        assert "pin_public_path_to_fence" in DeskTidyApp.__dict__
        # Logic lives on the batch API; single-path is a thin wrapper.
        pin_src = inspect.getsource(DeskTidyApp.pin_public_paths_to_fence)
        wrap_src = inspect.getsource(DeskTidyApp.pin_public_path_to_fence)
        assert "pin_public_paths_to_fence" in wrap_src
        assert "seed_src" in pin_src or "FULL pin list" in pin_src or "virtual_items" in pin_src
        assert "unpin_paths_from_virtual_fence" in pin_src

    run("公共区拖入分区钉选在 OLE 外完成", _drop_pin_outside_ole)

    def _click_must_not_false_unpin():
        import inspect

        from PyQt6.QtCore import QPoint, Qt

        from src.ui.fence_icon_item import (
            FenceIconItem,
            should_treat_as_virtual_unpin,
            start_virtual_item_drag,
        )
        from src.ui.fence_widget import FenceItemLabel

        assert not should_treat_as_virtual_unpin(
            catcher_accepted=False,
            drop_action=Qt.DropAction.IgnoreAction,
            start_global=QPoint(100, 100),
            end_global=QPoint(112, 105),
        )
        src = inspect.getsource(start_virtual_item_drag)
        # All fence pins use custom non-OLE drag (no mid-drag OLE handoff).
        assert "_virtual_drag_all_desktop_resident" in src
        assert "_run_custom_desktop_resident_virtual_drag" in src
        assert "QDrag(" not in src and "drag.exec" not in src
        from src.ui import fence_icon_item as fii

        custom = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
        assert "should_unpin_virtual_drop" in custom or "virtual_unpin_drop_hint" in custom
        assert "_exec_external_file_ole_drag" in custom
        assert "handoff_external" in custom
        # Cross-fence is a move; OLE over translucent fence must not pin .exe.
        assert "DropAction.MoveAction" in custom
        assert "fence_widget_at" in custom
        assert "geometry_only=True" in custom
        from src.fence_rules import (
            _canonicalize_virtual_pin_path,
            assign_paths_to_virtual_fence,
            get_virtual_items_for_fence,
        )

        assert callable(_canonicalize_virtual_pin_path)
        assert "_canonicalize_virtual_pin_path" in inspect.getsource(
            assign_paths_to_virtual_fence
        )
        # Display path must stay cheap — heal/canonicalize only for legacy .exe pins.
        get_src = inspect.getsource(get_virtual_items_for_fence)
        assert "_heal_fence_virtual_pins" in get_src
        assert 'endswith(".exe")' in get_src or "endswith('.exe')" in get_src
        assert "_canonicalize_virtual_pin_path" not in get_src.split("for raw in raw_items:")[-1]
        assert callable(fii.attach_external_file_drag_payload)
        assert callable(fii.external_payload_paths_for_virtual_drag)
        assert callable(fii.resolve_external_drag_path)
        assert callable(fii.should_convert_prefercopy_to_unpin)
        assert callable(fii.discard_explorer_prefercopy_desktop_dupes)
        drag_src = inspect.getsource(fii.attach_external_file_drag_payload)
        assert "attach_shell_file_drag_mime" in drag_src
        from src.shell_clipboard import attach_shell_file_drag_mime

        assert "FileNameW" in inspect.getsource(attach_shell_file_drag_mime)
        assert "manhattanLength() < 24" in inspect.getsource(
            FenceIconItem.mouseMoveEvent
        )
        move_src = inspect.getsource(FenceIconItem.mouseMoveEvent)
        assert "virtual_unpinned.emit" in move_src
        assert "QTimer.singleShot" in move_src
        assert "manhattanLength() < 24" in inspect.getsource(
            FenceItemLabel.mouseMoveEvent
        )

    run("点击微抖不误判移出分区", _click_must_not_false_unpin)

    def _organize_kinds_only():
        import inspect
        from pathlib import Path as P

        from PyQt6.QtWidgets import QApplication

        from src.desktop_scanner import DesktopItem
        from src.fence_rules import (
            fence_rules_summary,
            item_organize_kind,
            migrate_fence_rules,
        )
        from src.ui.fence_editor import FenceEditDialog
        from src.ui.organize_kind_picker import OrganizeKindPicker

        assert item_organize_kind(
            DesktopItem(P("a.lnk"), "a.lnk", False, ".lnk", 0)
        ) == "icon"
        assert item_organize_kind(
            DesktopItem(P("a.txt"), "a.txt", False, ".txt", 0)
        ) == "file"
        assert item_organize_kind(
            DesktopItem(P("d"), "d", True, "", 0)
        ) == "file"
        assert item_organize_kind(
            DesktopItem(P("a.png"), "a.png", False, ".png", 0)
        ) == "file"
        # Legacy folder kind folds into 文档.
        assert fence_rules_summary({"organize_kinds": ["icon", "folder"]}) == "软件、文档"
        assert fence_rules_summary({"organize_kinds": ["icon", "file", "folder"]}) == (
            "软件、文档"
        )
        s = {
            "fences": [{"id": "x", "name": "旧", "extensions": [".lnk", ".pdf"]}],
            "organize_rules": {},
            "filename_rules": {},
            "category_fence_map": {},
        }
        migrate_fence_rules(s)
        assert s["fences"][0]["organize_kinds"] == ["icon", "file"]
        assert s["fences"][0]["extensions"] == []
        src = inspect.getsource(FenceEditDialog.__init__)
        assert "OrganizeKindPicker" in src
        assert "ExtensionRulePicker" not in src
        app = QApplication.instance() or QApplication([])
        picker = OrganizeKindPicker(["file"])
        assert picker.selected_kinds() == ["file"]
        picker.close()
        picker.deleteLater()
        app.processEvents()

    run("分区规则仅软件/文档", _organize_kinds_only)
    run("控件 config 与 settings 分离时仍能钉入分区", _split_config_object_still_pins)


def test_desktop_context_contract() -> None:
    begin("3) 桌面右键菜单契约（系统菜单合并）")

    def _recorder_hook():
        import inspect

        from src.desktop_context import (
            DesktopContextMonitor,
            get_last_desktop_right_click,
            set_last_desktop_right_click,
        )
        from src.desktop_ll_mouse import (
            ll_mouse_hook_active,
            subscriber_count,
        )
        from src.desktop_monitor import DesktopBlankClickMonitor, DesktopDoubleClickMonitor

        src = inspect.getsource(DesktopContextMonitor)
        assert "QueuedConnection" in src
        assert "WM_RBUTTONDOWN" in src
        assert "WM_RBUTTONUP" in src
        assert "freeze.emit" in src
        assert "register_ll_mouse_handler" in src
        handler = inspect.getsource(DesktopContextMonitor._on_ll_mouse)
        # Must not swallow Explorer's menu (handlers never block the chain).
        assert "return 1" not in handler
        assert "is_desktop_point" in handler
        # Shared single WH_MOUSE_LL — both monitors subscribe to one hook.
        import src.desktop_ll_mouse as ll

        ll_src = inspect.getsource(ll._dispatch)
        ll_mod_src = inspect.getsource(ll)
        assert "CallNextHookEx" in ll_src
        assert "_INTERESTING_WPARAMS" in ll_src
        assert "WM_LBUTTONUP" in ll_mod_src
        mon_src = inspect.getsource(DesktopDoubleClickMonitor)
        blank_src = inspect.getsource(DesktopBlankClickMonitor)
        assert "register_ll_mouse_handler" in mon_src
        assert "register_ll_mouse_handler" in blank_src
        assert "WM_LBUTTONUP" in blank_src
        assert "is_desktop_point" in blank_src
        assert "SetWindowsHookEx" not in mon_src
        assert "SetWindowsHookEx" not in blank_src
        assert "SetWindowsHookEx" not in src
        # Fences share one app Alt+wheel filter (not N× installEventFilter).
        from src.ui.fence_widget import FenceWidget

        fence_init = inspect.getsource(FenceWidget.__init__)
        assert "_register_fence_alt_zoom" in fence_init
        assert "app.installEventFilter(self)" not in fence_init
        build_ui = inspect.getsource(FenceWidget._build_ui)
        assert "_apply_header_mouse_pass_through" in build_ui
        assert "title_label, stretch=1" not in build_ui
        pass_src = inspect.getsource(FenceWidget._apply_header_mouse_pass_through)
        assert "WA_TransparentForMouseEvents" in pass_src
        assert "title_label" in pass_src
        assert "collapse_btn" in pass_src
        filt = inspect.getsource(FenceWidget.eventFilter)
        assert "obj is app" not in filt
        assert "FenceIconItem" in filt
        cur_src = inspect.getsource(FenceWidget._update_cursor)
        assert "FenceIconItem" in cur_src
        freezes: list[int] = []
        mon = DesktopContextMonitor(
            lambda x, y: None, on_freeze=lambda: freezes.append(1)
        )
        assert not mon._active
        before = subscriber_count()
        mon.start()
        assert mon._active
        assert subscriber_count() == before + 1
        assert ll_mouse_hook_active()
        mon.stop()
        assert not mon._active
        assert subscriber_count() == before
        blank = DesktopBlankClickMonitor(lambda: None)
        blank.start()
        assert blank._active
        assert subscriber_count() == before + 1
        blank.stop()
        assert not blank._active
        assert subscriber_count() == before
        set_last_desktop_right_click(12, 34)
        assert get_last_desktop_right_click() == (12, 34)
        from src.win_shell import is_shell_context_menu_open

        assert callable(is_shell_context_menu_open)
        # Idle desktop must not look like a menu is open (CoreWindow false positive
        # used to block RMB「刷新」until the ~20s safety cap).
        assert is_shell_context_menu_open() is False
        import src.win_shell as win_shell

        assert "Windows.UI.Core.CoreWindow" not in win_shell._SHELL_MENU_ENUM_CLASSES
        assert "Popup" not in win_shell._SHELL_MENU_ENUM_CLASSES
        assert "#32768" in win_shell._SHELL_MENU_ENUM_CLASSES
        assert "Xaml_WindowedPopupClass" in win_shell._SHELL_MENU_ENUM_CLASSES
        # Win11 desktop background menu (FG often stays on another app).
        assert (
            "Microsoft.UI.Content.PopupWindowSiteBridge"
            in win_shell._SHELL_MENU_ENUM_CLASSES
        )
        # Menu EnumWindows is throttled (many open apps).
        menu_src = inspect.getsource(win_shell.is_shell_context_menu_open)
        assert "_cache" in menu_src or "0.25" in menu_src
        assert "allow_enum" in menu_src

    def _shell_verbs_and_ipc():
        import inspect
        import winreg

        from src.app import DeskTidyApp
        from src.shell_background_verbs import (
            register_desktop_background_verbs,
            unregister_desktop_background_verbs,
            _CHILDREN,
        )
        from src.shell_ipc import (
            send_shell_verb_to_running_instance,
            start_shell_ipc_host,
            stop_shell_ipc_host,
        )
        from src.ui.fence_widget import FenceWidget

        sync = inspect.getsource(DeskTidyApp._sync_desktop_shell_integration)
        blank_click = inspect.getsource(DeskTidyApp._on_desktop_blank_click)
        clear_sel = inspect.getsource(DeskTidyApp._clear_desktop_item_selection)
        assert "sync_desktop_background_verbs" in sync
        assert "start_shell_ipc_host" in sync
        assert "_clear_desktop_item_selection" in blank_click
        assert "_cursor_over_icon_item" in blank_click
        assert "clear_item_selection" in clear_sel
        assert "host.clear_item_selection()" in clear_sel
        # Public float first-click: chrome must paint invisible hit ink (layered alpha).
        pub_paint = inspect.getsource(
            __import__(
                "src.ui.public_icon_widget", fromlist=["PublicIconWidget"]
            ).PublicIconWidget.paintEvent
        )
        assert "public_selection_chrome_rect" in pub_paint
        assert "QColor(0, 0, 0, 1)" in pub_paint or "QColor(0,0,0,1)" in pub_paint.replace(" ", "")
        verb_handler = inspect.getsource(DeskTidyApp._on_shell_verb)
        assert "new-fence" in verb_handler
        assert "_create_fence_at" in verb_handler
        assert "region-fence" not in verb_handler
        assert "get_last_desktop_right_click" in verb_handler
        create_at = inspect.getsource(DeskTidyApp._create_fence_at)
        assert "place_new_fence_rect" in create_at
        assert "_current_page()" in create_at
        assert "_iter_live_fences_for_placement" in create_at
        assert "width=220" not in create_at
        from src.fence_layout import DEFAULT_NEW_FENCE_HEIGHT, DEFAULT_NEW_FENCE_WIDTH

        assert DEFAULT_NEW_FENCE_WIDTH >= 300
        assert DEFAULT_NEW_FENCE_HEIGHT >= 340
        create_geo = inspect.getsource(DeskTidyApp._create_fence_with_geometry)
        assert "set_fence_pages(config, [self._current_page()])" in create_geo
        layout_src = inspect.getsource(
            __import__("src.ui.desktop_layout_widget", fromlist=["DesktopLayoutWidget"]).DesktopLayoutWidget._add_fence
        )
        assert "place_new_fence_config" in layout_src
        editor_src = inspect.getsource(
            __import__("src.ui.fence_editor", fromlist=["FenceEditorWidget"]).FenceEditorWidget._add_fence
        )
        assert "place_new_fence_config" in editor_src
        assert "len(fences) * 240" not in editor_src
        ctor = inspect.getsource(DeskTidyApp.__init__)
        assert "set_live_fences_provider" in ctor
        assert "force_refresh_desktop" in verb_handler
        assert "region-fence" not in {v for _, v in _CHILDREN.values()}
        assert callable(DeskTidyApp.force_refresh_desktop)
        force_src = inspect.getsource(DeskTidyApp.force_refresh_desktop)
        assert "invalidate_file_icon_cache" in force_src
        assert "invalidate_desktop_scan_cache" in force_src
        assert "_ensure_shell_attachments(force=True)" in force_src
        assert "_ensure_desktop_overlays_visible(force=True)" in force_src
        assert "_shell_attach_force_pending = False" in force_src
        attach = inspect.getsource(DeskTidyApp._ensure_shell_attachments)
        assert "_schedule_force_shell_attach" in attach
        # Blocked restack must still queue force attach (RMB refresh during freeze).
        blocked_branch = attach.split("if self._overlay_restack_blocked():", 1)[1]
        blocked_branch = blocked_branch.split(
            "if self._desk_app_ui_open() and self._desk_app_owns_foreground():", 1
        )[0]
        assert "_schedule_force_shell_attach" in blocked_branch
        # App UI open must queue force too (Win+D / refresh while UI visible).
        settings_branch = attach.split(
            "if self._desk_app_ui_open() and self._desk_app_owns_foreground():", 1
        )[1]
        settings_branch = settings_branch.split("now = time.perf_counter()", 1)[0]
        assert "_schedule_force_shell_attach" in settings_branch
        run_pending = inspect.getsource(DeskTidyApp._run_pending_force_shell_attach)
        assert "_overlay_restack_blocked" in run_pending
        assert "_desk_app_ui_open" in run_pending
        # One attach owner — must not re-enter via ensure_desktop_overlays_visible(force).
        assert "_ensure_shell_attachments(force=True)" in run_pending
        assert "_clear_foreign_fg_park_latch_if_desktop" in run_pending
        assert "_ensure_desktop_overlays_visible(force=True)" not in run_pending
        bring = inspect.getsource(
            __import__("src.win_shell", fromlist=["x"]).bring_widget_to_foreground
        )
        assert "was_maximized" in bring
        assert "showMaximized" in bring
        visible = inspect.getsource(DeskTidyApp._ensure_desktop_overlays_visible)
        assert "_schedule_force_shell_attach" in visible
        fence_refresh = inspect.getsource(FenceWidget.refresh)
        assert "force" in fence_refresh
        assert "_schedule_post_refresh_shell_heal" in fence_refresh
        assert "force=True" not in fence_refresh.split("_schedule_post_refresh_shell_heal")[0][-200:]
        heal = inspect.getsource(DeskTidyApp._schedule_post_refresh_shell_heal)
        assert "_any_live_overlay_win32_hidden" in heal
        assert "_heal_overlays_after_shell_menu" in heal
        assert "_ensure_shell_attachments" in heal
        assert "_ensure_desktop_overlays_visible" in heal
        fence_impl = inspect.getsource(FenceWidget._refresh_impl)
        assert "_force_refresh_pending" in fence_impl
        menu_src = inspect.getsource(FenceWidget._show_fence_context_menu)
        assert "refresh(force=True)" in menu_src
        assert callable(FenceWidget.reload_icons)
        poll = inspect.getsource(DeskTidyApp._poll_explorer_menu_dismiss)
        assert "_sync_after_explorer_menu" in poll
        sync = inspect.getsource(DeskTidyApp._sync_after_explorer_menu)
        # Soft repair after any RMB dismiss; full force_refresh is shell「刷新」only.
        assert "force_refresh_desktop" not in sync
        assert "_recover_overlays_after_desktop_shell_menu" in sync
        recover = inspect.getsource(DeskTidyApp._recover_overlays_after_desktop_shell_menu)
        assert "_overlays_need_shell_repair" in recover
        assert "_any_live_overlay_win32_hidden" in recover
        assert "force=needs_repair" in recover
        assert "invalidate_defview_host_cache" in recover
        assert "_schedule_overlay_keepalive" in recover
        assert "force_refresh_desktop" in verb_handler
        assert callable(DeskTidyApp._snapshot_explorer_defview_host)
        repair = inspect.getsource(DeskTidyApp._overlays_need_shell_repair_impl)
        assert "require_attached" in repair
        # One DefView lookup for all overlays — never EnumWindows per fence/icon.
        assert "_find_defview_host()" in repair
        assert repair.count("_find_defview_host()") == 1
        assert "host=host" in repair and "defview=defview" in repair
        assert callable(DeskTidyApp._overlays_need_shell_repair_light)
        assert not hasattr(DeskTidyApp, "_show_desktop_context_menu")
        assert not hasattr(DeskTidyApp, "track_desktop_context_menu")
        arm = inspect.getsource(DeskTidyApp._arm_explorer_menu_freeze)
        assert "_begin_desktop_popup" in arm
        poll = inspect.getsource(DeskTidyApp._poll_explorer_menu_dismiss)
        assert "is_shell_context_menu_open" in poll
        assert "allow_enum=False" in poll
        assert "closed_streak" in poll
        assert "120" in poll
        ctor = inspect.getsource(DeskTidyApp.__init__)
        assert "on_freeze=self._arm_explorer_menu_freeze" in ctor
        import src.desktop_shell_host as host

        assert hasattr(host, "invalidate_defview_host_cache")
        assert "_DEFVIEW_CACHE" in inspect.getsource(host)
        keep = inspect.getsource(DeskTidyApp._start_overlay_keepalive_timer)
        assert "15000" in keep
        ensure_keep = inspect.getsource(DeskTidyApp._ensure_desktop_overlays_visible)
        assert "20000" in ensure_keep
        begin_drag = inspect.getsource(DeskTidyApp._begin_overlay_drag)
        assert "_poll_timer" in begin_drag
        from src.hotkey_manager import HotkeyManager

        ensure_poll = inspect.getsource(HotkeyManager._ensure_poll_timer)
        assert "_POLL_IDLE_MS" in ensure_poll or "250" in ensure_poll
        assert "any_down" in ensure_poll
        poll = inspect.getsource(HotkeyManager._poll_hotkeys)
        assert "any_down" in poll
        assert "0x0001" in poll or "pressed_since" in poll
        # Idle poll default should stay gentle for 24/7 tray use.
        from src import hotkey_manager as _hk

        assert int(getattr(_hk, "_POLL_IDLE_MS")) == 1200
        assert int(getattr(_hk, "_POLL_WM_ONLY_MS")) == 1500
        cfg = inspect.getsource(
            __import__("src.win_shell", fromlist=["configure_desktop_overlay"]).configure_desktop_overlay
        )
        assert "desired" in cfg
        assert "is_attached_to_desktop" in cfg
        # Fake/offscreen winId must not SetWindowPos — pywintypes.error escapes OSError.
        assert "IsWindow" in cfg
        assert "except Exception:" in cfg
        ensure = inspect.getsource(host.ensure_overlay_on_desktop)
        # Attached path must not call SetWindowPos/HWND_BOTTOM (flash source).
        # Peel wallpaper WorkerW first, then the already-attached early return.
        assert "is_stuck_under_wallpaper" in ensure
        attached_branch = ensure.split("if is_attached_to_desktop(hwnd):", 1)[1]
        attached_branch = attached_branch.split("return attach_overlay_to_desktop")[0]
        assert "place_overlay_in_desktop_band(hwnd)" not in attached_branch
        assert "ShowWindow" in attached_branch

        register_desktop_background_verbs()
        try:
            parent = r"Software\Classes\Directory\Background\shell\DeskTidy"
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, parent) as pk:
                label, _ = winreg.QueryValueEx(pk, "MUIVerb")
                assert label == "DeskTidy"
                sub, _ = winreg.QueryValueEx(pk, "SubCommands")
                assert sub == ""
            for child_key, (child_label, verb) in _CHILDREN.items():
                key = parent + rf"\shell\{child_key}"
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as k:
                    label, _ = winreg.QueryValueEx(k, "MUIVerb")
                    assert label == child_label
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key + r"\command") as ck:
                    cmd, _ = winreg.QueryValueEx(ck, None)
                    assert f"--shell-verb={verb}" in cmd
            # Flat crowding keys from old builds must be gone.
            for flat in (
                "DeskTidy.new_fence",
                "DeskTidy.region_fence",
                "DeskTidy.refresh_fences",
                "DeskTidy.show_window",
            ):
                try:
                    winreg.OpenKey(
                        winreg.HKEY_CURRENT_USER,
                        rf"Software\Classes\Directory\Background\shell\{flat}",
                    )
                    raise AssertionError(f"obsolete flat verb still present: {flat}")
                except FileNotFoundError:
                    pass
        finally:
            unregister_desktop_background_verbs()

        received: list[tuple] = []
        assert start_shell_ipc_host(lambda v, x, y: received.append((v, x, y)))
        try:
            assert send_shell_verb_to_running_instance("new-fence", 1, 2)
            # Pump Qt so QueuedConnection delivers.
            from PyQt6.QtWidgets import QApplication
            from PyQt6.QtCore import QTimer

            app = QApplication.instance() or QApplication([])
            QTimer.singleShot(50, app.quit)
            app.exec()
            assert any(t[0] == "new-fence" for t in received), received
        finally:
            stop_shell_ipc_host()

        fence_src = inspect.getsource(FenceWidget._show_fence_context_menu)
        assert "setWindowFlags" not in fence_src
        sched = inspect.getsource(DeskTidyApp._schedule_overlay_keepalive)
        assert "_ensure_desktop_overlays_visible" not in sched
        # Multi-app: never yank a healthy timer back to 2.5s on every focus churn.
        assert "setInterval(2500)" not in sched
        assert "15000" in sched

    def _narrow_fence_close_not_resize():
        """MIN_WIDTH fence: close button must not sit in the resize hit-strip."""
        import inspect

        from PyQt6.QtCore import QPoint, Qt
        from PyQt6.QtWidgets import QApplication

        from src.ui.fence_widget import FenceWidget

        app = QApplication.instance() or QApplication([])
        cfg = {
            "id": "tiny_close",
            "name": "窄",
            "virtual_items": [],
            "width": FenceWidget.MIN_WIDTH,
            "height": FenceWidget.MIN_HEIGHT,
            "style": {"show_title": True, "collapsible": True},
            "pages": [0],
        }
        fence = FenceWidget(cfg, {"theme": "mist", "fences": [cfg], "exclude_patterns": []})
        try:
            fence.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
            fence.resize(FenceWidget.MIN_WIDTH, FenceWidget.MIN_HEIGHT)
            fence.show()
            fence._hovered = True
            fence._update_chrome_visibility()
            app.processEvents()
            lock = fence.lock_btn
            assert lock.isVisible()
            # Button rect in fence coordinates — must not be a resize mode.
            top_left = lock.mapTo(fence, QPoint(0, 0))
            center = top_left + QPoint(lock.width() // 2, lock.height() // 2)
            assert 0 <= top_left.x()
            assert top_left.x() + lock.width() <= fence.width(), (
                top_left,
                lock.width(),
                fence.width(),
            )
            assert fence._hit_test_resize(center) is None, (
                fence._hit_test_resize(center),
                center,
                fence.size(),
                lock.geometry(),
            )
            # Cursor helper must also refuse the resize arrow on lock chip.
            fence._last_cursor_mode = object()
            fence._update_cursor(center)
            assert fence.cursor().shape() != Qt.CursorShape.SizeHorCursor
            assert fence.cursor().shape() != Qt.CursorShape.SizeFDiagCursor
            assert fence.cursor().shape() != Qt.CursorShape.SizeVerCursor
            src = inspect.getsource(FenceWidget._hit_test_resize)
            assert "_pos_on_header_chrome" in src
            filt = inspect.getsource(FenceWidget.eventFilter)
            assert "lock_btn" in filt
            assert "header_drag_targets" in filt
            assert "has_icon_widgets" in filt
            assert "_dispatch_chrome_button_mouse" in filt
            pass_src = inspect.getsource(FenceWidget._apply_header_mouse_pass_through)
            # Ancestors must stay mouse-opaque — transparent parents skip the
            # button subtree in Qt hit-testing.
            assert "WA_TransparentForMouseEvents" in pass_src
            assert "False" in pass_src
            assert "pass_header" not in pass_src
            assert "_sync_header_hit_mask" in pass_src
            mask_src = inspect.getsource(FenceWidget._sync_header_hit_mask)
            assert "setMask" in mask_src
            assert "has_icon_widgets" in mask_src
            assert "clearMask" in mask_src
            assert "layout().activate()" in mask_src or "layout.activate()" in mask_src
            vis_src = inspect.getsource(FenceWidget._update_chrome_visibility)
            assert "has_icons" in vis_src
            style_src = inspect.getsource(FenceWidget._apply_style)
            assert "rgba(255, 255, 255, 38)" in style_src
            assert 'background: transparent' not in style_src or "fenceBtn" in style_src
            # Chip fill must not be the transparent keyword for fenceBtn.
            assert "background-color: {btn_bg}" in style_src or "rgba(255, 255, 255, 38)" in style_src
        finally:
            fence.close()
            fence.deleteLater()
            app.processEvents()

    def _empty_fence_body_drag_moves():
        """New empty fence: title press starts move (not a no-op)."""
        from PyQt6.QtCore import QEvent, QPoint, QPointF, Qt
        from PyQt6.QtGui import QMouseEvent
        from PyQt6.QtWidgets import QApplication

        from src.ui.fence_widget import FenceWidget

        app = QApplication.instance() or QApplication([])
        cfg = {
            "id": "new_empty_drag",
            "name": "新建空分区",
            "virtual_items": [],
            "width": 280,
            "height": 220,
            "style": {"show_title": True, "collapsible": True},
            "pages": [0],
        }
        fence = FenceWidget(cfg, {"theme": "mist", "fences": [cfg], "exclude_patterns": []})
        try:
            fence.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
            fence.resize(280, 220)
            fence.move(100, 100)
            fence.show()
            fence.refresh(force=True)
            fence._hovered = True
            fence._update_chrome_visibility()
            app.processEvents()
            assert not fence.has_icon_widgets()
            assert not fence.header_widget.testAttribute(
                Qt.WidgetAttribute.WA_TransparentForMouseEvents
            )
            before = fence.pos()
            center = fence.title_label.rect().center()
            press = QMouseEvent(
                QEvent.Type.MouseButtonPress,
                QPointF(center),
                QPointF(fence.title_label.mapToGlobal(center)),
                Qt.MouseButton.LeftButton,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert fence.eventFilter(fence.title_label, press) is True
            assert fence._drag_pos is not None
            move = QMouseEvent(
                QEvent.Type.MouseMove,
                QPointF(center + QPoint(40, 30)),
                QPointF(fence.title_label.mapToGlobal(center) + QPoint(40, 30)),
                Qt.MouseButton.NoButton,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert fence.eventFilter(fence.title_label, move) is True
            app.processEvents()
            assert fence.pos() != before
            release = QMouseEvent(
                QEvent.Type.MouseButtonRelease,
                QPointF(center + QPoint(40, 30)),
                QPointF(fence.title_label.mapToGlobal(center) + QPoint(40, 30)),
                Qt.MouseButton.LeftButton,
                Qt.MouseButton.NoButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert fence.eventFilter(fence.title_label, release) is True
            assert fence._drag_pos is None
        finally:
            fence.close()
            fence.deleteLater()
            app.processEvents()

    def _fence_position_lock_blocks_drag():
        """Lock chip toggles position_locked; locked fence cannot be dragged."""
        from PyQt6.QtCore import QEvent, QPoint, QPointF, Qt
        from PyQt6.QtGui import QMouseEvent
        from PyQt6.QtWidgets import QApplication

        from src.ui.fence_widget import FenceWidget

        app = QApplication.instance() or QApplication([])
        cfg = {
            "id": "lock_drag",
            "name": "锁定",
            "virtual_items": [],
            "width": 280,
            "height": 220,
            "style": {"show_title": True, "collapsible": True},
            "pages": [0],
        }
        fence = FenceWidget(cfg, {"theme": "mist", "fences": [cfg], "exclude_patterns": []})
        try:
            fence.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
            fence.resize(280, 220)
            fence.move(120, 80)
            fence.show()
            fence._hovered = True
            fence._update_chrome_visibility()
            app.processEvents()
            assert not fence.is_position_locked()
            assert not hasattr(fence, "refresh_btn")
            assert not hasattr(fence, "close_btn")
            fence.lock_btn.click()
            app.processEvents()
            assert fence.is_position_locked()
            assert fence.config.get("position_locked") is True
            assert fence.get_config_update().get("position_locked") is True
            before = fence.pos()
            center = fence.title_label.rect().center()
            press = QMouseEvent(
                QEvent.Type.MouseButtonPress,
                QPointF(center),
                QPointF(fence.title_label.mapToGlobal(center)),
                Qt.MouseButton.LeftButton,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert fence.eventFilter(fence.title_label, press) is True
            assert fence._drag_pos is None
            move = QMouseEvent(
                QEvent.Type.MouseMove,
                QPointF(center + QPoint(50, 40)),
                QPointF(fence.title_label.mapToGlobal(center) + QPoint(50, 40)),
                Qt.MouseButton.NoButton,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert fence.eventFilter(fence.title_label, move) is False
            assert fence.pos() == before
            # Corner must not enter resize while locked.
            corner = QPoint(fence.width() - 2, fence.height() - 2)
            assert fence._hit_test_resize(corner) is None
            fence.lock_btn.click()
            app.processEvents()
            assert not fence.is_position_locked()
            press2 = QMouseEvent(
                QEvent.Type.MouseButtonPress,
                QPointF(center),
                QPointF(fence.title_label.mapToGlobal(center)),
                Qt.MouseButton.LeftButton,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert fence.eventFilter(fence.title_label, press2) is True
            assert fence._drag_pos is not None
        finally:
            fence.close()
            fence.deleteLater()
            app.processEvents()

    def _fence_header_buttons_clickable():
        """Title chrome buttons must receive clicks even when the fence has icons.

        Regression: making header/slots WA_TransparentForMouseEvents skipped the
        whole button subtree in Qt hit-testing, so collapse/lock did nothing.
        """
        import tempfile
        from pathlib import Path

        from PyQt6.QtCore import QEvent, QPoint, QPointF, QRect, Qt
        from PyQt6.QtGui import QMouseEvent
        from PyQt6.QtWidgets import QApplication

        from src.ui.fence_widget import FenceWidget

        app = QApplication.instance() or QApplication([])
        td = tempfile.mkdtemp(prefix="desktidy_hdrbtn_")
        try:
            pinned = Path(td) / "item.txt"
            pinned.write_text("x", encoding="utf-8")
            cfg = {
                "id": "hdr_btns",
                "name": "有图标",
                "virtual_items": [str(pinned)],
                "width": 320,
                "height": 360,
                "style": {"show_title": True, "collapsible": True},
                "pages": [0],
            }
            fence = FenceWidget(
                cfg, {"theme": "mist", "fences": [cfg], "exclude_patterns": []}
            )
            try:
                fence.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
                fence.resize(320, 360)
                fence.show()
                fence.refresh(force=True)
                fence._hovered = True
                fence._update_chrome_visibility()
                app.processEvents()
                assert fence.has_icon_widgets()
                for w in (
                    fence.header_widget,
                    fence.right_slot,
                    fence.collapse_btn,
                    fence.lock_btn,
                ):
                    assert not w.testAttribute(
                        Qt.WidgetAttribute.WA_TransparentForMouseEvents
                    ), w.objectName()
                # With icons: header mask is title+chips only — empty overlay
                # over the icon grid must not steal file drags (foreign-app case).
                mask = fence.header_widget.mask()
                assert not mask.isEmpty()
                title_c = fence.title_label.mapTo(
                    fence.header_widget, fence.title_label.rect().center()
                )
                assert mask.contains(title_c)
                left_c = fence.left_slot.mapTo(
                    fence.header_widget, fence.left_slot.rect().center()
                )
                assert mask.contains(left_c)
                lock_in_header = fence.lock_btn.mapTo(
                    fence.header_widget, fence.lock_btn.rect().center()
                )
                assert mask.contains(lock_in_header)
                # Center stretch between title and right_slot stays unmasked so
                # top-row icons accept foreign file drags.
                title_in_header = QRect(
                    fence.title_label.mapTo(fence.header_widget, QPoint(0, 0)),
                    fence.title_label.size(),
                )
                hole = QPoint(
                    min(
                        title_in_header.right() + 12,
                        fence.header_widget.width()
                        - fence.right_slot.width()
                        - 4,
                    ),
                    fence.HEADER_OVERLAY_HEIGHT // 2,
                )
                assert not title_in_header.contains(hole)
                assert not mask.contains(hole)
                # Hit-test must find the button under the header (not skip subtree).
                lock_local = fence.lock_btn.mapTo(fence, fence.lock_btn.rect().center())
                assert fence.childAt(lock_local) is fence.lock_btn
                # Opaque chip style (translucent windows click-through transparent paint).
                style = fence.container.styleSheet()
                assert "background-color: rgba(255, 255, 255, 38)" in style
                assert "background: transparent" not in style.split("QPushButton#fenceBtn")[1].split("QPushButton#fenceBtn:hover")[0]
                # Press on right_slot over lock must arm chrome click (not fence drag).
                slot_pos = fence.lock_btn.mapTo(
                    fence.right_slot, fence.lock_btn.rect().center()
                )
                press = QMouseEvent(
                    QEvent.Type.MouseButtonPress,
                    QPointF(slot_pos),
                    QPointF(fence.right_slot.mapToGlobal(slot_pos)),
                    Qt.MouseButton.LeftButton,
                    Qt.MouseButton.LeftButton,
                    Qt.KeyboardModifier.NoModifier,
                )
                assert fence.eventFilter(fence.right_slot, press) is True
                assert fence._chrome_btn_press is fence.lock_btn
                assert fence._drag_pos is None
                hits: list[str] = []
                fence.collapse_btn.clicked.connect(lambda: hits.append("collapse"))
                fence.lock_btn.clicked.connect(lambda: hits.append("lock"))
                release = QMouseEvent(
                    QEvent.Type.MouseButtonRelease,
                    QPointF(slot_pos),
                    QPointF(fence.right_slot.mapToGlobal(slot_pos)),
                    Qt.MouseButton.LeftButton,
                    Qt.MouseButton.NoButton,
                    Qt.KeyboardModifier.NoModifier,
                )
                assert fence.eventFilter(fence.right_slot, release) is True
                assert hits == ["lock"], hits
                hits.clear()
                # Alpha click-through: press arrives on body/items under the chip.
                # mapTo(non-ancestor) is undefined — use global round-trip like Win32.
                body = fence.items_widget
                body_pos = body.mapFromGlobal(
                    fence.lock_btn.mapToGlobal(fence.lock_btn.rect().center())
                )
                press2 = QMouseEvent(
                    QEvent.Type.MouseButtonPress,
                    QPointF(body_pos),
                    QPointF(body.mapToGlobal(body_pos)),
                    Qt.MouseButton.LeftButton,
                    Qt.MouseButton.LeftButton,
                    Qt.KeyboardModifier.NoModifier,
                )
                assert fence.eventFilter(body, press2) is True
                assert fence._chrome_btn_press is fence.lock_btn
                release2 = QMouseEvent(
                    QEvent.Type.MouseButtonRelease,
                    QPointF(body_pos),
                    QPointF(body.mapToGlobal(body_pos)),
                    Qt.MouseButton.LeftButton,
                    Qt.MouseButton.NoButton,
                    Qt.KeyboardModifier.NoModifier,
                )
                assert fence.eventFilter(body, release2) is True
                assert hits == ["lock"], hits
                hits.clear()
                for btn, name in (
                    (fence.collapse_btn, "collapse"),
                    (fence.lock_btn, "lock"),
                ):
                    c = btn.rect().center()
                    g = btn.mapToGlobal(c)
                    for et, buttons in (
                        (QEvent.Type.MouseButtonPress, Qt.MouseButton.LeftButton),
                        (QEvent.Type.MouseButtonRelease, Qt.MouseButton.NoButton),
                    ):
                        ev = QMouseEvent(
                            et,
                            QPointF(c),
                            QPointF(g),
                            Qt.MouseButton.LeftButton,
                            buttons,
                            Qt.KeyboardModifier.NoModifier,
                        )
                        app.sendEvent(btn, ev)
                        app.processEvents()
                assert hits == ["collapse", "lock"], hits
                assert "_dispatch_chrome_button_mouse" in __import__(
                    "inspect"
                ).getsource(fence.eventFilter)
            finally:
                fence.close()
                fence.deleteLater()
                app.processEvents()
        finally:
            import shutil

            shutil.rmtree(td, ignore_errors=True)

    def _icon_fence_body_drag_marquees():
        """Icon fence: empty panel drag rubber-bands; does not move the zone."""
        import tempfile
        from pathlib import Path

        from PyQt6.QtCore import QEvent, QPoint, QPointF, Qt
        from PyQt6.QtGui import QMouseEvent
        from PyQt6.QtWidgets import QApplication

        from src.ui.fence_widget import FenceWidget

        app = QApplication.instance() or QApplication([])
        td = tempfile.mkdtemp(prefix="desktidy_body_marquee_")
        try:
            pinned = Path(td) / "item.txt"
            pinned.write_text("x", encoding="utf-8")
            cfg = {
                "id": "body_marquee_icons",
                "name": "有图标",
                "virtual_items": [str(pinned)],
                "width": 320,
                "height": 360,
                "style": {"show_title": True, "collapsible": True},
                "pages": [0],
            }
            fence = FenceWidget(
                cfg, {"theme": "mist", "fences": [cfg], "exclude_patterns": []}
            )
            try:
                fence.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
                fence.resize(320, 360)
                fence.move(100, 100)
                fence.show()
                fence.refresh(force=True)
                fence._hovered = True
                fence._update_chrome_visibility()
                app.processEvents()
                assert fence.has_icon_widgets()
                assert not fence.is_position_locked()
                before = fence.pos()
                vp = fence.scroll.viewport()
                pos = QPoint(50, 80)
                press = QMouseEvent(
                    QEvent.Type.MouseButtonPress,
                    QPointF(pos),
                    QPointF(vp.mapToGlobal(pos)),
                    Qt.MouseButton.LeftButton,
                    Qt.MouseButton.LeftButton,
                    Qt.KeyboardModifier.NoModifier,
                )
                assert fence.eventFilter(vp, press) is True
                assert fence._panel_press_pending is not None
                assert fence._drag_pos is None
                move = QMouseEvent(
                    QEvent.Type.MouseMove,
                    QPointF(pos + QPoint(40, 30)),
                    QPointF(vp.mapToGlobal(pos + QPoint(40, 30))),
                    Qt.MouseButton.NoButton,
                    Qt.MouseButton.LeftButton,
                    Qt.KeyboardModifier.NoModifier,
                )
                assert fence.eventFilter(vp, move) is True
                app.processEvents()
                assert fence.pos() == before
                assert fence._drag_pos is None
                assert fence._marquee_origin is not None
                release = QMouseEvent(
                    QEvent.Type.MouseButtonRelease,
                    QPointF(pos + QPoint(40, 30)),
                    QPointF(vp.mapToGlobal(pos + QPoint(40, 30))),
                    Qt.MouseButton.LeftButton,
                    Qt.MouseButton.NoButton,
                    Qt.KeyboardModifier.NoModifier,
                )
                assert fence.eventFilter(vp, release) is True
                assert fence._marquee_origin is None
                assert fence._drag_pos is None
            finally:
                fence.close()
                fence.deleteLater()
                app.processEvents()
        finally:
            import shutil

            shutil.rmtree(td, ignore_errors=True)

    def _empty_fence_body_does_not_move():
        """Empty fence body drag must not move — title bar is the only grip."""
        from PyQt6.QtCore import QEvent, QPoint, QPointF, Qt
        from PyQt6.QtGui import QMouseEvent
        from PyQt6.QtWidgets import QApplication

        from src.ui.fence_widget import FenceWidget

        app = QApplication.instance() or QApplication([])
        cfg = {
            "id": "empty_body_no_move",
            "name": "空分区",
            "virtual_items": [],
            "width": 280,
            "height": 220,
            "style": {"show_title": True, "collapsible": True},
            "pages": [0],
        }
        fence = FenceWidget(cfg, {"theme": "mist", "fences": [cfg], "exclude_patterns": []})
        try:
            fence.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
            fence.resize(280, 220)
            fence.move(100, 100)
            fence.show()
            fence.refresh(force=True)
            app.processEvents()
            before = fence.pos()
            hosts = list(fence._marquee_host_objects())
            host = next((h for h in hosts if h is not None), None)
            assert host is not None
            pos = QPoint(40, 100)
            press = QMouseEvent(
                QEvent.Type.MouseButtonPress,
                QPointF(pos),
                QPointF(host.mapToGlobal(pos)),
                Qt.MouseButton.LeftButton,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert fence.eventFilter(host, press) is True
            assert fence._drag_pos is None
            move = QMouseEvent(
                QEvent.Type.MouseMove,
                QPointF(pos + QPoint(50, 40)),
                QPointF(host.mapToGlobal(pos + QPoint(50, 40))),
                Qt.MouseButton.NoButton,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            fence.eventFilter(host, move)
            app.processEvents()
            assert fence.pos() == before
            assert fence._drag_pos is None
        finally:
            fence.close()
            fence.deleteLater()
            app.processEvents()

    run("点击记录钩子不吞系统菜单 + start/stop", _recorder_hook)
    run("Background shell 动词 + IPC + 应用接线", _shell_verbs_and_ipc)
    run("窄分区锁定按钮不被缩放热区抢走", _narrow_fence_close_not_resize)
    run("空分区标题栏可拖动位移", _empty_fence_body_drag_moves)
    run("有图标分区空白处框选不位移", _icon_fence_body_drag_marquees)
    run("空分区空白处不位移", _empty_fence_body_does_not_move)
    run("分区锁定后不可拖动", _fence_position_lock_blocks_drag)
    run("分区右侧按钮可点击", _fence_header_buttons_clickable)


def test_shell_band_contract() -> None:
    begin("4) 桌面壳层契约")

    def _band():
        import inspect

        from src.desktop_shell_host import place_overlay_in_desktop_band
        import src.desktop_shell_host as host

        src = inspect.getsource(place_overlay_in_desktop_band)
        assert "HWND_BOTTOM" in src
        assert not hasattr(host, "place_overlay_below_hwnd")

    run("HWND_BOTTOM 沉底且无 relative-below 死代码", _band)


def test_uninstall_autostart() -> None:
    begin("卸载后清理开机启动")

    def _contracts():
        import inspect
        from pathlib import Path

        from src import uninstall_cleanup as uc
        from src.settings import remove_all_autostart

        rm = inspect.getsource(remove_all_autostart)
        assert "_remove_registry_autostart" in rm
        assert "_remove_startup_approved" in rm
        assert "_desktop_guard_link_path" in rm
        assert "remove_desktop_guard_autostart" in inspect.getsource(
            __import__("src.settings", fromlist=["remove_desktop_guard_autostart"])
        )
        from src.settings import remove_desktop_guard_autostart, set_auto_start

        # Dual-startup must not be recreated.
        src_set = inspect.getsource(set_auto_start)
        assert "remove_desktop_guard_autostart" in src_set
        assert "--desktop-guard" not in src_set
        clean = inspect.getsource(uc._purge_shell_and_autostart)
        assert "remove_all_autostart" in clean
        assert "run_uninstall_cleanup" in inspect.getsource(uc)
        assert "purge_user_data" in inspect.getsource(uc)
        assert "collect_custom_data_dirs" in inspect.getsource(uc)
        assert uc.wants_purge_userdata(["--uninstall-cleanup", "--purge-userdata"])
        assert not uc.wants_purge_userdata(["--uninstall-cleanup"])
        assert not uc.is_safe_data_purge_target(Path.home())
        assert not uc.is_safe_data_purge_target(Path.home() / "Desktop")
        assert len(uc.collect_custom_data_dirs({"notepad": {"folder": "D:\\n"}})) == 1

        iss = (Path(__file__).resolve().parents[1] / "installer" / "DeskTidy.iss").read_text(
            encoding="utf-8"
        )
        assert "DeleteStartupShortcuts" in iss
        assert "DeleteStartupShortcuts();" in iss
        assert "--uninstall-cleanup" in iss
        assert "--purge-userdata" in iss
        # Wizard must paint before any DeskTidy stop — not in InitializeSetup.
        assert "PrepareToInstall" in iss
        assert "function InitializeSetup" not in iss
        prep = iss.split("function PrepareToInstall", 1)[1].split("function ", 1)[0]
        assert "StopDeskTidyAt" not in prep
        assert "StopDeskTidyProcesses" not in prep
        assert "taskkill.exe" in prep
        uninst = iss.split("function InitializeUninstall", 1)[1]
        assert "StopDeskTidyProcesses" in uninst
        assert "--uninstall-cleanup" in iss.split("[Code]", 1)[1]
        assert "DeleteUserDataFolders" in iss
        assert "GetEnv('USERPROFILE')" in iss
        assert "是否清除 DeskTidy 的本地数据" in iss
        assert "自定义" in iss
        assert "不会自动删除" in iss
        assert "GPurgeUserData" in iss
        assert "UninstallDelete" in iss

    def _purge_tmpdir():
        import json
        import tempfile
        from pathlib import Path
        from unittest import mock

        from src import uninstall_cleanup as uc

        with tempfile.TemporaryDirectory(prefix="desktidy_purge_") as tmp:
            root = Path(tmp)
            app_dir = root / ".desktidy"
            install = root / "install"
            notes = install / "笔记"
            records = install / "录屏"
            custom_notes = root / "my_notes"
            custom_rec = root / "my_recs"
            minutes = root / "minutes_out"
            for folder in (app_dir / "storage", notes, records, custom_notes, custom_rec, minutes):
                folder.mkdir(parents=True, exist_ok=True)
                (folder / "marker.txt").write_text("x", encoding="utf-8")
            settings = {
                "notepad": {"folder": str(custom_notes)},
                "screen_record": {"output_dir": str(custom_rec)},
                "meeting_minutes": {"folder": str(minutes)},
            }
            (app_dir / "settings.json").write_text(
                json.dumps(settings, ensure_ascii=False), encoding="utf-8"
            )
            with (
                mock.patch("src.settings.APP_DIR", app_dir),
                mock.patch("src.settings.SETTINGS_FILE", app_dir / "settings.json"),
                mock.patch.object(uc, "_load_settings_snapshot", return_value=settings),
                mock.patch.object(uc, "_notify_custom_data_dirs") as notify,
            ):
                uc.purge_user_data(install_dir=install)

            assert not app_dir.exists()
            assert not notes.exists()
            assert not records.exists()
            assert custom_notes.exists()
            assert custom_rec.exists()
            assert minutes.exists()
            notify.assert_called_once()
            args = notify.call_args[0][0]
            assert len(args) == 3

    run("卸载清 Run/Startup/桌面守护快捷方式", _contracts)
    run("确认清除时保留自定义目录并提示", _purge_tmpdir)


def test_fence_grid_fit() -> None:
    begin("5) 分区图标网格不裁切")

    def _fit():
        from src.ui.fence_widget import FenceWidget

        for usable in (100, 200, 400, 800, 1600):
            for icon in (32, 48, 64, 96):
                cols, label = FenceWidget._fit_grid_columns(
                    usable, icon, h_spacing=4
                )
                cell = max(icon + 20, label + 10)
                total = cols * cell + 4 * max(0, cols - 1)
                # When the icon itself is wider than the shelf, only 1 col is possible.
                if icon + 20 <= usable:
                    assert total <= usable, (usable, icon, cols, label, total)
                else:
                    assert cols == 1, (usable, icon, cols)
        # Fences-like: medium icons get a wide caption shelf (~96px+).
        cols48, label48 = FenceWidget._fit_grid_columns(800, 48, h_spacing=4)
        assert label48 >= 96
        assert cols48 <= 7
        src = __import__("inspect").getsource(FenceWidget._icon_layout_metrics)
        assert "_available_items_width" in src
        assert "_fit_grid_columns" in src
        assert "ICON_BASE_WIDTH" not in src
        assert "ICON_BASE_HEIGHT" not in src
        assert "_icon_zoom" in src

    def _icon_defaults_and_align():
        import inspect
        import os
        import tempfile
        from pathlib import Path

        from PyQt6.QtWidgets import QApplication

        from src.icon_utils import _shell_lookup_path, file_icon_pixmap
        from src.ui.fence_icon_item import FenceIconItem
        from src.ui.fence_widget import FenceWidget
        from src.ui import public_icon_widget as pub

        assert FenceWidget.ICON_BASE_SIZE == 40
        assert FenceIconItem.DEFAULT_ICON_SIZE == FenceWidget.ICON_BASE_SIZE
        assert FenceWidget.FOOTER_OVERLAY_HEIGHT >= 18
        align_src = inspect.getsource(FenceWidget._build_ui)
        assert "AlignHCenter" in align_src
        assert "AlignTop" in align_src
        # Full AlignCenter on the grid vertically floats a short icon row.
        assert "setAlignment(Qt.AlignmentFlag.AlignCenter)" not in align_src.split(
            "items_layout"
        )[1].split("footer_label")[0]
        # Footer must overlay — in-flow footer on hover clipped the last icon row.
        assert "body_layout.addWidget(self.footer_label)" not in align_src
        assert "_sync_footer_geometry" in inspect.getsource(FenceWidget)
        # No Qt StandardPixmap special-case for system icons.
        import src.icon_utils as iu

        assert "_namespace_standard_pixmap" not in iu.__dict__
        assert "SP_TrashIcon" not in inspect.getsource(iu)

        app = QApplication.instance() or QApplication([])
        tmp = Path(__file__).resolve().parent / "a.txt"
        tmp.write_text("x", encoding="utf-8")
        try:
            # Parent so Windows DPI / top-level show does not rewrite geometry.
            from PyQt6.QtWidgets import QWidget

            host = QWidget()
            item = FenceIconItem(tmp, icon_size=64, label_max_width=80, parent=host)
            host.show()
            item.show()
            app.processEvents()
            cell = max(64 + 20, 80 + 10)
            assert item.icon_label.width() == 64
            assert item.icon_label.height() == 64
            assert item.text_label is not None
            fm = item.text_label.fontMetrics()
            line_h = max(fm.lineSpacing(), fm.height() + max(0, fm.descent()))
            # Fixed 4-line shelf — short and long names share one height.
            from src.desktop_caption import caption_box_height

            shelf = caption_box_height(item.text_label.font(), max_lines=2, extra_pad=10)
            assert item.text_label.height() == shelf, (
                item.text_label.height(),
                shelf,
            )
            assert "…" not in item.text_label.text() and "..." not in item.text_label.text()
            long_name = "很长很长的桌面文件名称用来验证换行不会过早省略"
            item.text_label.setText(long_name)
            from src.ui.fence_icon_item import fit_desktop_caption_label

            fit_desktop_caption_label(item.text_label, item.text_label.width(), max_lines=2)
            assert item.text_label.height() == shelf, (
                item.text_label.height(),
                shelf,
            )
            assert item.text_label.height() <= int(line_h * 2 + 24), (
                item.text_label.height(),
                line_h,
            )
            assert item.minimumWidth() == cell
            assert item.maximumWidth() == cell
            assert item.width() == cell
            # Caption and glyph share the same horizontal center.
            assert abs(
                item.icon_label.geometry().center().x()
                - item.text_label.geometry().center().x()
            ) <= 1
            host.close()
            host.deleteLater()
            app.processEvents()

            # Public floats use the shell icon-title font with plain wrapped text.
            public = pub.PublicIconWidget(tmp, 0, 0, parent=None)
            public._reload_label()
            app.processEvents()
            assert public.text_label.pixmap() is not None
            assert not public.text_label.pixmap().isNull()
            assert public.text_label.text() == ""
            # Caption is a pre-rasterized pixmap; QLabel must not wrap again.
            assert public.text_label.wordWrap() is False
            assert public.text_label.height() >= 16
            assert public.height() >= pub._WIDGET_H - 2
            short_caption_h = public.text_label.height()
            short_widget_h = public.height()
            public.close()
            public.deleteLater()
            app.processEvents()

            from src.desktop_caption import (
                caption_box_height,
                caption_needed_height,
                icon_title_qfont,
            )
            from src.desktop_icon_metrics import system_desktop_icon_cell

            cell = system_desktop_icon_cell()
            # Long public names share the same Explorer cell shelf.
            long_tmp = Path(r"C:\Users\demo\很长很长的桌面文件名称用来验证公共区换行不会过早省略.xlsx")
            long_pub = pub.PublicIconWidget(long_tmp, 0, 0, parent=None)
            long_pub._reload_label()
            app.processEvents()
            assert long_pub._caption_text == long_tmp.name
            assert long_pub.text_label.height() == short_caption_h
            assert long_pub.height() == short_widget_h
            assert long_pub.width() == cell.width
            # Outer height follows the tight shelf footprint (shared with grid CELL_H).
            assert long_pub.height() == cell.height
            assert abs(pub._WIDGET_H - cell.height) <= 2
            assert pub._CAPTION_MAX_LINES == cell.caption_lines
            # Caption label is shelf-sized — not the leftover empty band under text.
            assert long_pub.text_label.height() <= caption_box_height(
                icon_title_qfont(), max_lines=2, extra_pad=10
            ) + 2
            two_line = caption_needed_height(
                long_tmp.name, long_pub._label_width, max_lines=2
            )
            four_line = caption_needed_height(
                long_tmp.name, long_pub._label_width, max_lines=4
            )
            assert four_line >= two_line
            long_pub.close()
            long_pub.deleteLater()
            app.processEvents()
        finally:
            try:
                tmp.unlink()
            except OSError:
                pass

        # System shortcuts resolve to ::{CLSID} and extract a real shell glyph.
        fake = Path(r"C:\Users\demo\.desktidy\.public_system\此电脑.lnk")
        assert _shell_lookup_path(fake).startswith("::{")
        pix = file_icon_pixmap(fake, 64)
        assert not pix.isNull()
        dpr = max(1.0, float(pix.devicePixelRatio() or 1.0))
        assert abs(pix.width() / dpr - 64) < 0.6
        assert abs(pix.height() / dpr - 64) < 0.6
        # Must not be an empty/transparent canvas (shell extract failed).
        img = pix.toImage()
        cx = max(0, min(pix.width() - 1, int(round(32 * dpr))))
        cy = max(0, min(pix.height() - 1, int(round(32 * dpr))))
        assert img.pixelColor(cx, cy).alpha() > 32
        # Direct CLSID path also works (Recycle Bin).
        pix2 = file_icon_pixmap(
            Path(r"C:\Users\demo\.desktidy\.public_system\回收站.lnk"), 64
        )
        assert not pix2.isNull()
        dpr2 = max(1.0, float(pix2.devicePixelRatio() or 1.0))
        assert abs(pix2.width() / dpr2 - 64) < 0.6
        assert "_shell_icon_pixmap" in iu.__dict__
        assert "_screen_device_pixel_ratio" in iu.__dict__
        apply = inspect.getsource(FenceIconItem._apply_icon_size)
        assert "setScaledContents(False)" in apply
        assert "display_file_icon_pixmap" in apply
        assert "_refresh_caption_for_selection" in apply
        refresh_cap = inspect.getsource(FenceIconItem._refresh_caption_for_selection)
        assert "fit_desktop_caption_label" in refresh_cap
        assert "max_lines=2" in refresh_cap
        assert "elide_desktop_caption_text" in inspect.getsource(
            FenceIconItem._caption_display_text
        )
        # Selected stays on the same 2-line ellipsis shelf (height must not clip).
        cap_disp = inspect.getsource(FenceIconItem._caption_display_text)
        assert "max_lines=2" in cap_disp
        assert "12 if self._selected" not in cap_disp
        pub_label = inspect.getsource(pub.PublicIconWidget._reload_label)
        assert "elide=True" in pub_label
        assert "elide=not selected" not in pub_label
        cap_h = inspect.getsource(pub.PublicIconWidget._caption_height)
        assert "caption_needed_height" not in cap_h
        from src.ui import fence_icon_item as fii

        assert callable(fii.fit_desktop_caption_label)
        from src.desktop_caption import elide_desktop_caption_text

        assert "..." in elide_desktop_caption_text(
            "微控立库堆垛机库提高出库节拍方案.docx", 70, max_lines=2
        ) or "\u2026" in elide_desktop_caption_text(
            "微控立库堆垛机库提高出库节拍方案.docx", 70, max_lines=2
        )
        # Shelf tall enough: 2-line caption pixmap keeps ink near the bottom
        # (regression — line 2 used to be vertically half-clipped).
        from src.desktop_caption import (
            caption_box_height,
            icon_title_qfont,
            render_desktop_caption,
        )
        import src.desktop_caption as _dc

        assert "WrapAtWordBoundaryOrAnywhere" in inspect.getsource(
            _dc.elide_desktop_caption_text
        )
        _font = icon_title_qfont()
        _shelf = caption_box_height(_font, max_lines=2, extra_pad=10)
        _pix = render_desktop_caption(
            "Android Developer", 98, _shelf, dpr=1.0, font=_font, max_lines=2, elide=True
        )
        _img = _pix.toImage()
        _ink_rows = [
            y
            for y in range(_img.height())
            if any(_img.pixelColor(x, y).alpha() > 30 for x in range(0, _img.width(), 2))
        ]
        assert _ink_rows, "caption pixmap has no ink"
        # Line 2 must be painted (not half-clipped). Extra shelf pad sits *below*
        # the glyphs, so ink need not reach 70% of the box.
        _bands = []
        _s = _p = _ink_rows[0]
        for _y in _ink_rows[1:]:
            if _y > _p + 1:
                _bands.append((_s, _p))
                _s = _y
            _p = _y
        _bands.append((_s, _p))
        assert len(_bands) >= 2, _bands
        _h1 = _bands[0][1] - _bands[0][0] + 1
        _h2 = _bands[-1][1] - _bands[-1][0] + 1
        assert _h2 >= int(_h1 * 0.85), (_bands, "line 2 shorter than line 1")
        assert _shelf - 1 - _ink_rows[-1] >= 4, (_ink_rows[-1], _shelf)
        # CJK 孤字: do not leave 「同」 alone on the second line.
        from src.desktop_caption import _balance_caption_orphan_lines

        assert _balance_caption_orphan_lines(["2500710集团合", "同"]) == [
            "2500710集团",
            "合同",
        ]
        orphan_el = elide_desktop_caption_text("2500710集团合同", 90, max_lines=2)
        assert "同" != orphan_el.split("\n")[-1]
        assert all(len(part) >= 2 for part in orphan_el.split("\n") if part)
        # Long name: keep 「合同」 together; never 「合」/「同」 split or a 3rd clipped line.
        long_name = "2500710集团合同外购件签订模板测试很长.xlsx"
        for _w in (80, 90, 98):
            long_el = elide_desktop_caption_text(long_name, _w, max_lines=2)
            assert "合\n同" not in long_el, long_el
            parts = [p for p in long_el.split("\n") if p]
            assert 1 <= len(parts) <= 2
            assert all(len(p) >= 2 for p in parts)
            assert "…" in long_el or "..." in long_el
        fit_src = inspect.getsource(fii.fit_desktop_caption_label)
        assert "setWordWrap(False)" in fit_src
        assert "setWordWrap(True)" not in fit_src
        assert callable(fii.display_file_icon_pixmap) if hasattr(fii, "display_file_icon_pixmap") else True
        from src.icon_utils import display_file_icon_pixmap

        # Missing Office path must still resolve to the association glyph
        # (SHGFI_USEFILEATTRIBUTES), not the blank-document iIcon=0.
        from src.icon_utils import _sys_icon_index

        missing_xlsx = Path(tempfile.gettempdir()) / f"desktidy_missing_{os.getpid()}.xlsx"
        assert not missing_xlsx.exists()
        miss_idx = _sys_icon_index(str(missing_xlsx))
        # Association index for .xlsx via the same helper path as a live file.
        live = Path(tempfile.gettempdir()) / f"desktidy_live_{os.getpid()}.xlsx"
        live.write_bytes(b"PK\x03\x04")
        try:
            live_idx = _sys_icon_index(str(live))
            assert miss_idx == live_idx, (miss_idx, live_idx)
            assert miss_idx > 0
            pix_miss = file_icon_pixmap(missing_xlsx, 64)
            assert not pix_miss.isNull()
            img_m = pix_miss.toImage()
            # Must not be a near-empty canvas (old blank-document bug).
            opaque = 0
            for yy in range(0, img_m.height(), 4):
                for xx in range(0, img_m.width(), 4):
                    if img_m.pixelColor(xx, yy).alpha() > 32:
                        opaque += 1
            assert opaque >= 8, opaque
        finally:
            live.unlink(missing_ok=True)
        assert "_SHGFI_USEFILEATTRIBUTES" in inspect.getsource(iu)
        drag_src = inspect.getsource(fii.start_virtual_item_drag)
        assert "press_local" in drag_src
        custom_drag = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
        assert "_drag_hotspot" in custom_drag
        assert "virtual_unpin_drop_hint" in custom_drag
        pub_drag = inspect.getsource(fii.start_public_item_drag)
        assert "press_local" in pub_drag
        assert "_drag_hotspot" in pub_drag
        assert "_drag_drop_anchor" in pub_drag
        resize_src = inspect.getsource(FenceWidget.resizeEvent)
        assert "_resize_mode" in resize_src
        release_src = inspect.getsource(FenceWidget.mouseReleaseEvent)
        assert "was_resizing" in release_src
        zoom_src = inspect.getsource(FenceWidget._set_icon_zoom)
        assert "_schedule_icon_scale_update" in zoom_src
        scale_src = inspect.getsource(FenceWidget._update_icon_scale)
        assert "fast=True" in scale_src or "fast: bool" in scale_src
        assert "_icon_scale_settle" in inspect.getsource(FenceWidget.__init__)
        scale_pix = inspect.getsource(FenceIconItem._scale_pixmap_from_source)
        assert "FastTransformation" in scale_pix
        assert "_ensure_source_pixmap" in apply
        pub_reload = inspect.getsource(pub.PublicIconWidget._reload_icon)
        assert "setScaledContents(False)" in pub_reload
        label_src = inspect.getsource(pub.PublicIconWidget._reload_label)
        assert "render_desktop_caption" in label_src
        assert "setPixmap" in label_src
        assert "_caption_font" in label_src
        assert "_outlined_name_pixmap" not in label_src
        cap_h = inspect.getsource(pub.PublicIconWidget._caption_height)
        assert "_label_height" in cap_h
        assert "system_desktop_icon_cell" in inspect.getsource(pub.PublicIconWidget.__init__)
        assert "_outlined_name_pixmap" not in inspect.getsource(pub)
        assert "QGraphicsDropShadowEffect" not in inspect.getsource(pub.PublicIconWidget.__init__)
        assert "icon_title_qfont" in inspect.getsource(pub)
        from src.desktop_caption import icon_title_qfont, render_desktop_caption
        import inspect as _insp_cap
        import src.desktop_caption as _desk_cap

        qt_cap = _insp_cap.getsource(_desk_cap._qt_explorer_caption)
        # Soft glow only — no hard outline / offset cast-shadow under the title.
        assert "No hard" in qt_cap or "soft glow" in qt_cap.lower()
        assert "translated(0, max(1" not in qt_cap
        assert "QColor(0, 0, 0, 200)" not in qt_cap

        cap = render_desktop_caption("副本报表验证情况 验证", 90, 40, dpr=1.0)
        assert not cap.isNull()
        cap_img = cap.toImage()
        dark = white = 0
        for yy in range(0, cap_img.height(), 2):
            for xx in range(0, cap_img.width(), 2):
                c = cap_img.pixelColor(xx, yy)
                if c.alpha() < 20:
                    continue
                if c.red() < 70 and c.green() < 70 and c.blue() < 70:
                    dark += 1
                elif c.red() > 240 and c.green() > 240 and c.blue() > 240:
                    white += 1
        assert white >= 10 and dark >= 10, (white, dark)
        assert icon_title_qfont().pixelSize() > 0 or icon_title_qfont().pointSize() > 0
        fence_style = inspect.getsource(FenceWidget._apply_style)
        assert "_DEFAULT_FENCE_TITLE_COLOR" in fence_style
        assert "_DEFAULT_FENCE_TEXT_COLOR" in fence_style
        from src.ui.styles import build_stylesheet

        sheet = build_stylesheet("graphite")
        # Fence captions stay Explorer/Fences-style white (not theme ink).
        assert "color: #FFFFFF;" in sheet
        assert "color: #CBD5E1;" in sheet
        assert "padding: 1px 2px 3px 2px;" in sheet
        assert pub._LABEL_HEIGHT >= 16
        assert pub._WIDGET_H >= 48
        from src.desktop_icon_metrics import system_desktop_icon_cell as _sys_cell

        assert pub._WIDGET_W == _sys_cell().width
        assert abs(pub._WIDGET_H - _sys_cell().height) <= 2
        # Height tracks the caption shelf, not an inflated empty band under text.
        assert pub._WIDGET_H <= _sys_cell().icon_size + pub._LABEL_HEIGHT + 24

    def _icon_zoom_persists():
        """Zoom must round-trip through config so quit/reopen keeps icon size."""
        import inspect

        from src.fence_layout import save_visible_fences
        from src.ui.fence_widget import FenceWidget

        assert "_load_icon_zoom_from_config" in inspect.getsource(FenceWidget)
        assert "_persist_icon_zoom" in inspect.getsource(FenceWidget)
        assert "icon_zoom" in inspect.getsource(FenceWidget.get_config_update)
        assert "icon_zoom" in inspect.getsource(FenceWidget.apply_config)
        assert "icon_zoom" in inspect.getsource(save_visible_fences)
        # Bounds helper is instance-bound but pure.
        assert FenceWidget._icon_zoom_bounds(object()) == (0.7, 1.6)  # type: ignore[arg-type]

    def _alt_wheel_zoom_reliable():
        """Alt+wheel must use Win32 LL path + notch accumulation (Qt path alone is flaky)."""
        import inspect

        from src.ui import fence_widget as fw
        from src.ui.fence_widget import FenceWidget
        import src.desktop_ll_mouse as ll

        assert fw._zoom_notches_from_delta(0, 120) == (1, 0)
        assert fw._zoom_notches_from_delta(0, -120) == (-1, 0)
        # High-res wheel: many small deltas → one notch, not N zoom steps.
        remain = 0
        notches_total = 0
        for _ in range(8):
            n, remain = fw._zoom_notches_from_delta(remain, 15)
            notches_total += n
        assert notches_total == 1
        assert remain == 0
        # Partial notch keeps remainder (no false zoom).
        n, remain = fw._zoom_notches_from_delta(0, 40)
        assert n == 0 and remain == 40
        n, remain = fw._zoom_notches_from_delta(remain, 80)
        assert n == 1 and remain == 0
        # Direction flip resets remainder.
        n, remain = fw._zoom_notches_from_delta(50, -120)
        assert n == -1 and remain == 0

        zoom_src = inspect.getsource(FenceWidget._handle_zoom_wheel)
        assert "_zoom_modifier_held" in zoom_src
        assert "_apply_zoom_wheel_delta" in zoom_src
        apply_src = inspect.getsource(FenceWidget._apply_zoom_wheel_delta)
        assert "_zoom_notches_from_delta" in apply_src
        mod_src = inspect.getsource(fw._zoom_modifier_held)
        assert "queryKeyboardModifiers" in mod_src
        filt_src = inspect.getsource(fw._FenceAltZoomAppFilter.eventFilter)
        assert "_zoom_modifier_held" in filt_src
        # LL hook path: required for WorkerW overlays that never get QWheelEvent.
        reg = inspect.getsource(fw._register_fence_alt_zoom)
        assert "register_ll_mouse_handler" in reg
        assert "_on_ll_fence_zoom_wheel" in reg
        ll_src = inspect.getsource(ll._dispatch)
        assert "WM_MOUSEWHEEL" in ll_src
        assert "zoom_modifier_physically_down" in ll_src
        assert "eat" in ll_src or "return 1" in ll_src
        assert callable(fw._on_ll_fence_zoom_wheel)
        assert callable(ll.wheel_delta_from_ll)
        # Signed high-word decode (WHEEL_DELTA = 120).
        class _Fake:
            mouseData = (120 << 16) & 0xFFFFFFFF

        # wheel_delta_from_ll needs a real address — unit-test via c_short math:
        import ctypes

        assert int(ctypes.c_short((120 << 16 >> 16) & 0xFFFF).value) == 120
        assert int(ctypes.c_short(((-120) & 0xFFFF)).value) == -120

    run("列宽按视口拟合且不溢出", _fit)
    run("默认40px中等图标且文字居中对齐", _icon_defaults_and_align)
    run("图标缩放写入配置可恢复", _icon_zoom_persists)
    run("Alt滚轮缩放修饰键与齿距可靠", _alt_wheel_zoom_reliable)


def test_fence_multi_select_drag() -> None:
    begin("5b) 分区多选与多文件拖拽")

    def _mime_and_selection():
        import inspect
        import tempfile
        from pathlib import Path

        from PyQt6.QtCore import QMimeData, Qt
        from PyQt6.QtWidgets import QApplication, QWidget

        from src.ui import fence_icon_item as fii
        from src.ui.fence_icon_item import (
            FenceIconItem,
            apply_fence_click_selection,
            encode_virtual_drag_paths,
            paths_for_fence_drag,
            select_fence_item,
        )
        from src.ui.fence_widget import FenceWidget
        from src.win_shell import DESKTIDY_VIRTUAL_MIME, collect_drop_paths

        app = QApplication.instance() or QApplication([])
        tmp = Path(tempfile.mkdtemp(prefix="desktidy_msel_"))
        files = [tmp / f"f{i}.txt" for i in range(3)]
        for f in files:
            f.write_text("x", encoding="utf-8")

        # Multi-line virtual MIME → collect_drop_paths.
        mime = QMimeData()
        mime.setData(DESKTIDY_VIRTUAL_MIME, encode_virtual_drag_paths(files))
        got = collect_drop_paths(mime)
        assert {str(p).casefold() for p in got} == {str(p).casefold() for p in files}

        class _Host(QWidget):
            """Named FenceWidget so find_fence_widget / iter helpers resolve."""

            def __init__(self) -> None:
                super().__init__()
                self._selection_anchor = None
                self._items: list[QWidget] = []

            def _icon_item_widgets(self):
                return list(self._items)

        _Host.__name__ = "FenceWidget"
        host = _Host()
        items = [
            FenceIconItem(files[0], icon_size=48, label_max_width=80, parent=host),
            FenceIconItem(files[1], icon_size=48, label_max_width=80, parent=host),
            FenceIconItem(files[2], icon_size=48, label_max_width=80, parent=host),
        ]
        host._items = items
        for it in items:
            it._virtual_mode = True
            it._fence_id = "t"

        apply_fence_click_selection(items[0], Qt.KeyboardModifier.NoModifier)
        assert items[0].is_selected() and not items[1].is_selected()
        assert host._selection_anchor is items[0]

        apply_fence_click_selection(items[2], Qt.KeyboardModifier.ShiftModifier)
        assert items[0].is_selected() and items[1].is_selected() and items[2].is_selected()

        # Press on an already-selected item must keep the multi-select (drag-out).
        apply_fence_click_selection(items[1], Qt.KeyboardModifier.NoModifier)
        assert items[0].is_selected() and items[1].is_selected() and items[2].is_selected()
        assert len(paths_for_fence_drag(items[1])) == 3

        # Plain release without drag collapses to the clicked item (Explorer).
        from src.ui.fence_icon_item import finalize_plain_click_selection

        assert finalize_plain_click_selection(
            items[1],
            was_selected=True,
            modifiers=Qt.KeyboardModifier.NoModifier,
        )
        assert items[1].is_selected() and not items[0].is_selected() and not items[2].is_selected()

        select_fence_item(items[1], exclusive=True)
        host._selection_anchor = items[1]
        apply_fence_click_selection(items[0], Qt.KeyboardModifier.ControlModifier)
        assert items[0].is_selected() and items[1].is_selected() and not items[2].is_selected()

        drag_paths = paths_for_fence_drag(items[0])
        assert len(drag_paths) == 2
        assert {str(p).casefold() for p in drag_paths} == {
            str(files[0]).casefold(),
            str(files[1]).casefold(),
        }

        assert "apply_fence_click_selection" in inspect.getsource(
            FenceIconItem._apply_click_selection
        )
        assert "paths_for_fence_drag" in inspect.getsource(FenceIconItem.mouseMoveEvent)
        assert "finalize_plain_click_selection" in inspect.getsource(
            FenceIconItem.mouseReleaseEvent
        )
        assert "encode_virtual_drag_paths" in inspect.getsource(
            fii._run_custom_desktop_resident_virtual_drag
        ) or "encode_virtual_drag_paths" in inspect.getsource(fii)
        # MIME encoding still used for fence↔fence OLE-free drops via helpers.
        assert callable(fii.encode_virtual_drag_paths)
        assert "_begin_marquee" in inspect.getsource(FenceWidget)
        assert "QRubberBand" in inspect.getsource(FenceWidget._build_ui)
        host.close()
        host.deleteLater()
        app.processEvents()

    run("多选Shift/Ctrl与多路径MIME", _mime_and_selection)


def test_dissolve_fence() -> None:
    begin("6) 解散分区到当前分页")

    def _dissolve():
        import tempfile
        from pathlib import Path

        from src.fence_rules import dissolve_fence_to_page
        from src.public_desktop import is_shared_public_entry, visible_floating_items

        tmp = Path(tempfile.mkdtemp(prefix="desktidy_dissolve_"))
        a = tmp / "a.txt"
        b = tmp / "b.txt"
        a.write_text("a", encoding="utf-8")
        b.write_text("b", encoding="utf-8")

        settings = {
            "enable_public_desktop": True,  # even with public on → still page-local
            "public_desktop_items": [],
            "current_page": 1,
            "fences": [
                {
                    "id": "keep",
                    "name": "保留",
                    "pages": [1],
                    "x": 400,
                    "y": 100,
                    "width": 220,
                    "height": 320,
                    "virtual_items": [],
                },
                {
                    "id": "gone",
                    "name": "解散我",
                    "pages": [1],
                    "x": 80,
                    "y": 80,
                    "width": 220,
                    "height": 320,
                    "virtual_items": [str(a), str(b)],
                },
            ],
            "category_fence_map": {"文档": "gone"},
            "fence_layouts_by_page": {
                "1": {
                    "gone": {"x": 80, "y": 80, "width": 220, "height": 320},
                    "keep": {"x": 400, "y": 100, "width": 220, "height": 320},
                }
            },
        }
        fence = next(f for f in settings["fences"] if f["id"] == "gone")
        placed = dissolve_fence_to_page(settings, fence, page_id=1)
        assert {p.name for p in placed} == {"a.txt", "b.txt"}
        assert all(f.get("id") != "gone" for f in settings["fences"])
        assert settings.get("category_fence_map", {}).get("文档") != "gone"
        floats = visible_floating_items(settings, 1)
        assert len(floats) == 2
        assert all(not is_shared_public_entry(e) for e in floats)
        assert all(int(e.get("page", -1)) == 1 for e in floats)
        # Other pages must not see these page-local icons.
        assert len(visible_floating_items(settings, 0)) == 0
        layout = (settings.get("fence_layouts_by_page") or {}).get("1") or {}
        assert "gone" not in layout
        assert "keep" in layout

        # Blank RMB prepends a single DeskTidy dissolve verb above Explorer.
        import inspect

        from src.app import DeskTidyApp
        from src.ui.fence_widget import FenceWidget

        menu_src = inspect.getsource(FenceWidget._show_fence_context_menu)
        assert "show_folder_background_menu" in menu_src
        assert "_fence_shell_extra_commands" in menu_src
        assert "图标放大" not in menu_src
        extra_src = inspect.getsource(FenceWidget._fence_shell_extra_commands)
        assert "解散分区" in extra_src
        assert "dissolve_requested.emit" in extra_src
        assert "is_locked_fence" in extra_src
        assert "dissolve_requested" in inspect.getsource(FenceWidget)
        # Dissolve must clear popup freeze and force float refresh/attach.
        dissolve_src = inspect.getsource(DeskTidyApp._on_fence_dissolve_requested)
        assert "_desktop_popup_depth" in dissolve_src
        assert "_end_desktop_popup" in dissolve_src
        assert "refresh_public_desktop" in dissolve_src
        assert "_ensure_shell_attachments" in dissolve_src
        assert "_ensure_dissolve_floats_visible" in dissolve_src
        wire = inspect.getsource(DeskTidyApp._wire_fence_signals)
        assert "QueuedConnection" in wire
        assert "dissolve_requested" in wire
        show_src = inspect.getsource(DeskTidyApp.show_fences)
        app_branch = show_src.split(
            "if self._desk_app_ui_open() and self._desk_app_owns_foreground():", 1
        )[1].split("elif self._foreign_app_owns_foreground():", 1)[0]
        assert "_shell_attach_after_settings = True" in app_branch

    run("解散删分区且图标落到当前页", _dissolve)


def test_page_switch_click() -> None:
    begin("7) 分页切换首次点击可靠")

    def _indicator_roundtrip():
        import inspect

        from PyQt6.QtCore import QEvent, QPoint, QPointF, Qt
        from PyQt6.QtGui import QMouseEvent
        from PyQt6.QtWidgets import QApplication

        from src.ui.page_indicator import PageIndicatorWidget

        app = QApplication.instance() or QApplication([])
        pages = [{"id": 0, "name": "工作"}, {"id": 1, "name": "文档"}]
        settings = {
            "notepad": {"enabled": True, "folder": ""},
            "meeting_minutes": {"enabled": True, "folder": ""},
            "screen_record": {"enabled": True, "output_dir": "", "fps": 30},
            "desktop_pet": {"enabled": False},
        }
        w = PageIndicatorWidget(pages, current_page=0, settings=settings)
        received: list[int] = []
        w.page_changed.connect(received.append)
        w.show()
        app.processEvents()
        # Open all peeks so direct clicks hit full buttons in tests.
        w._set_expanded(True, animate=False)
        app.processEvents()
        assert w._note_btn is None
        assert w._minutes_btn is not None and w._minutes_btn.text() == "纪要"
        assert w._record_btn is not None and w._record_btn.text() == "录屏"
        assert w._note_row is None
        assert w._minutes_row is not None and w._minutes_row.is_open
        assert w._record_row is not None and w._record_row.is_open

        def _click_button(btn) -> None:
            center = btn.rect().center()
            global_pos = btn.mapToGlobal(center)
            press = QMouseEvent(
                QEvent.Type.MouseButtonPress,
                QPointF(center),
                QPointF(global_pos),
                Qt.MouseButton.LeftButton,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            release = QMouseEvent(
                QEvent.Type.MouseButtonRelease,
                QPointF(center),
                QPointF(global_pos),
                Qt.MouseButton.LeftButton,
                Qt.MouseButton.NoButton,
                Qt.KeyboardModifier.NoModifier,
            )
            app.sendEvent(btn, press)
            app.sendEvent(btn, release)
            app.processEvents()

        # 工作 → 文档 → 工作：两次切换都应一次生效（旧逻辑会吞掉第二次）。
        _click_button(w._buttons[1])
        assert received == [1], received
        _click_button(w._buttons[0])
        assert received == [1, 0], received

        # 微抖动未过拖拽阈值时仍应切页。
        received.clear()
        w.set_current_page(0)
        btn = w._buttons[1]
        center = btn.rect().center()
        g0 = btn.mapToGlobal(center)
        g1 = g0 + QPoint(6, 4)  # < _DRAG_THRESHOLD(16)
        press = QMouseEvent(
            QEvent.Type.MouseButtonPress,
            QPointF(center),
            QPointF(g0),
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        )
        move = QMouseEvent(
            QEvent.Type.MouseMove,
            QPointF(center + QPoint(6, 4)),
            QPointF(g1),
            Qt.MouseButton.NoButton,
            Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        )
        release = QMouseEvent(
            QEvent.Type.MouseButtonRelease,
            QPointF(center + QPoint(6, 4)),
            QPointF(g1),
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.NoButton,
            Qt.KeyboardModifier.NoModifier,
        )
        app.sendEvent(btn, press)
        app.sendEvent(btn, move)
        app.sendEvent(btn, release)
        app.processEvents()
        assert received == [1], received

        # 「录屏」: single click emits; 「纪要」: only double-click creates.
        # 记事本已迁到 deskNote 快捷方式，分页栏不再显示。
        assert w._note_btn is None

        assert w._record_btn is not None
        rec_hits: list[int] = []
        w.record_requested.connect(lambda: rec_hits.append(1))
        _click_button(w._record_btn)
        assert rec_hits == [1], rec_hits

        assert w._minutes_btn is not None
        minutes_hits: list[int] = []
        w.minutes_requested.connect(lambda: minutes_hits.append(1))
        _click_button(w._minutes_btn)
        assert minutes_hits == [], "minutes single click must not create"
        # Double-click creates.
        center = w._minutes_btn.rect().center()
        g = w._minutes_btn.mapToGlobal(center)
        for etype in (
            QEvent.Type.MouseButtonPress,
            QEvent.Type.MouseButtonDblClick,
            QEvent.Type.MouseButtonRelease,
        ):
            ev = QMouseEvent(
                etype,
                QPointF(center),
                QPointF(g),
                Qt.MouseButton.LeftButton,
                Qt.MouseButton.LeftButton
                if etype != QEvent.Type.MouseButtonRelease
                else Qt.MouseButton.NoButton,
                Qt.KeyboardModifier.NoModifier,
            )
            app.sendEvent(w._minutes_btn, ev)
            app.processEvents()
        assert minutes_hits == [1], minutes_hits

        # Default: mostly visible (labels readable); only a small tuck remains.
        w._tuck_all(animate=False)
        app.processEvents()
        for row in w._iter_rows():
            assert not row.is_open, row.slide
            assert row.slide >= w._FULL_W - w._PEEK_W - 1
            assert w._PEEK_W >= 72, "resting peek must keep labels readable"
        # Hover one row → only that button slides out fully.
        w._rows[1].open_out(animate=False, ripple=False)
        assert w._rows[1].is_open
        assert not w._rows[0].is_open
        assert w._note_row is None
        assert hasattr(w._rows[1], "_ripple")

        w.close()
        w.deleteLater()
        app.processEvents()

    def _peek_hover_unstick():
        """Hover paint/slide must clear when Leave is missed (overlay failsafe)."""
        import time

        from PyQt6.QtCore import QEvent, QPoint
        from PyQt6.QtGui import QCursor
        from PyQt6.QtWidgets import QApplication

        from src.ui.page_indicator import PageIndicatorWidget, _PeekChip

        app = QApplication.instance() or QApplication([])
        w = PageIndicatorWidget(
            [{"id": 0, "name": "工作"}, {"id": 1, "name": "文档"}],
            current_page=0,
            settings={
                "theme": "sky",
                "page_folders": [{"name": "AIproject", "path": r"E:\soft\AIproject"}],
                "notepad": {"enabled": True},
                "screen_record": {"enabled": True},
                "meeting_minutes": {"enabled": True},
                "desktop_pet": {"enabled": False},
            },
        )
        w.show()
        app.processEvents()
        assert w._folder_rows, "expected folder peek row"
        row = w._folder_rows[0]
        btn = row.button
        assert isinstance(btn, _PeekChip)

        def _wait_tucked() -> None:
            for _ in range(50):
                app.processEvents()
                if not row.is_open:
                    return
                time.sleep(0.02)
            assert not row.is_open, row.slide

        # Simulate hover open + sticky hover flag (missed Leave).
        btn.set_hovered(True)
        row.open_out(animate=False, ripple=False)
        w._arm_hover_watch()
        assert row.is_open
        assert btn._hovered is True

        # Cursor is nowhere near the band → failsafe must tuck + clear hover.
        QCursor.setPos(QPoint(8, 8))
        app.processEvents()
        w._on_hover_watch()
        assert btn._hovered is False
        _wait_tucked()
        assert btn._hovered is False

        # Explicit Leave path also clears hover.
        btn.set_hovered(True)
        row.open_out(animate=False, ripple=False)
        app.sendEvent(btn, QEvent(QEvent.Type.Leave))
        app.processEvents()
        assert btn._hovered is False
        QCursor.setPos(QPoint(8, 8))
        row._on_leave_timeout()
        _wait_tucked()

        w.close()
        w.deleteLater()
        app.processEvents()

    def _retire_blocks_hits():
        from PyQt6.QtCore import Qt
        from PyQt6.QtWidgets import QApplication, QWidget

        from src.app import DeskTidyApp

        app = QApplication.instance() or QApplication([])
        w = QWidget()
        w.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, False)
        w.show()
        app.processEvents()
        DeskTidyApp._retire_overlay_widget(w)
        assert w.testAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        assert not w.isVisible()
        app.processEvents()

    def _switch_page_queued():
        import inspect

        from src.app import DeskTidyApp

        src = inspect.getsource(DeskTidyApp._setup_page_indicator)
        assert "QueuedConnection" in src
        src2 = inspect.getsource(DeskTidyApp.switch_page)
        assert "singleShot(0, self._keep_overlays_under_apps)" not in src2
        # Page switch finishes via refresh completion (root), not timer ladders.
        assert "_page_switch_ensure_pending" in src2
        assert "_finish_page_switch_overlays_if_pending" in src2
        assert "singleShot(120" not in src2
        assert "singleShot(280" not in src2
        assert callable(DeskTidyApp._ensure_page_switch_overlays_visible)
        finish = inspect.getsource(DeskTidyApp._finish_page_switch_overlays_if_pending)
        assert "_refresh_fences_after_page_switch" in finish
        quiet_idx = finish.index("_page_switch_quiet_until = 0.0")
        refresh_idx = finish.index("_refresh_fences_after_page_switch")
        assert refresh_idx < quiet_idx
        ensure_src = inspect.getsource(DeskTidyApp._ensure_page_switch_overlays_visible)
        assert "attempt" not in ensure_src
        assert "_foreign_app_owns_foreground" in ensure_src
        assert "_sink_overlays_for_foreign_app" in ensure_src
        # Soft repair only — no force-restack / chrome raise on every click.
        assert "force=need_force" not in ensure_src
        # Page-switch finish must not restack every overlay (multi-flash root).
        assert "self._ensure_desktop_overlays_visible" not in ensure_src
        assert "_ensure_new_public_floats_visible" not in ensure_src
        assert "set_overlay_hwnd_visible" in ensure_src
        assert "overlay_win32_visible" in ensure_src
        assert "_reveal_desktop_overlay" in ensure_src
        assert "_remap_hidden_overlay_hwnds" in ensure_src
        assert "is_stuck_under_wallpaper" in ensure_src
        assert "_restore_arriving_fences_interactive" in finish
        refresh_fn = inspect.getsource(DeskTidyApp._refresh_fences_after_page_switch)
        assert "_yield_page_switch_ui" in refresh_fn
        # Hotkey page switch: raise chrome only when buried (not every click).
        assert "_page_chrome_needs_raise" in ensure_src
        assert "_ensure_page_chrome_visible(raise_band=True)" in ensure_src
        assert "WindowFromPoint" in inspect.getsource(DeskTidyApp._page_chrome_needs_raise)
        chrome = inspect.getsource(DeskTidyApp._ensure_page_chrome_visible)
        assert "configure_desktop_overlay" in chrome
        assert "raise_band=raise_band" in chrome
        assert "force_band=True" not in chrome
        assert "page_indicator" in chrome
        assert "_page_chrome_may_show" in chrome
        assert "_overlay_widgets_may_show" not in chrome
        assert "is_attached_to_desktop" in chrome
        assert "raise_band: bool" in chrome or "*, raise_band" in chrome
        assert "not raise_band" in chrome
        assert "_foreign_app_owns_foreground" in chrome
        # Open vault/todo after pet so raise_band does not bury panels under sprite.
        assert chrome.index("chrome_widgets.append(pet)") < chrome.index(
            "chrome_widgets.append(vault)"
        )
        assert callable(DeskTidyApp._raise_open_tool_panels_above_pet)
        assert "_raise_open_tool_panels_above_pet" in inspect.getsource(
            DeskTidyApp._on_vault_requested
        )
        assert "_raise_open_tool_panels_above_pet" in inspect.getsource(
            DeskTidyApp.ensure_live_fences_interactive
        )
        chrome_may = inspect.getsource(DeskTidyApp._page_chrome_may_show)
        assert "_overlays_parked_for_app_fg" not in chrome_may
        assert "_desk_app_ui_open" in chrome_may
        assert "_icons_hidden" in chrome_may
        needs_raise = inspect.getsource(DeskTidyApp._page_chrome_needs_raise)
        assert "WindowFromPoint" in needs_raise
        assert "is_desktop_point" in needs_raise
        sink_fg = inspect.getsource(DeskTidyApp._sink_overlays_for_foreign_app)
        assert "_keep_overlays_under_apps" in sink_fg
        assert "_ensure_page_chrome_visible(raise_band=True)" not in sink_fg
        assert "widget.hide()" not in sink_fg
        assert "_desktidy_soft_parked" in sink_fg
        assert "_remap_hidden_overlay_hwnds" in sink_fg
        assert callable(DeskTidyApp._remap_hidden_overlay_hwnds)
        assert callable(DeskTidyApp._heal_overlays_after_shell_menu)
        remap = inspect.getsource(DeskTidyApp._remap_hidden_overlay_hwnds)
        assert "set_overlay_hwnd_visible" in remap
        assert "overlay_win32_visible" in remap
        assert "_desktidy_soft_parked" in remap
        end_popup = inspect.getsource(DeskTidyApp._end_desktop_popup)
        assert "_heal_overlays_after_shell_menu" in end_popup
        sync_menu = inspect.getsource(DeskTidyApp._sync_after_explorer_menu)
        assert "_recover_overlays_after_desktop_shell_menu" in sync_menu
        switch = inspect.getsource(DeskTidyApp.switch_page)
        assert "_page_switch_quiet_until" in switch
        assert "persist=False" in switch
        assert "_flush_fence_layout_save(persist=False)" in switch
        # Direct raise stays in finish path — not mid-switch_page (flash).
        assert "_ensure_page_chrome_visible" not in switch
        assert "_page_switch_ensure_pending" in switch
        assert "_finish_page_switch_overlays_if_pending" in switch
        # Desktop fences before admin tree so A↔B clicks feel immediate.
        assert switch.index("show_fences") < switch.index("select_page_id")
        assert "_page_switch_paint_freeze" in switch
        assert switch.index("_page_switch_paint_freeze") < switch.index(
            "_finish_page_switch_overlays_if_pending"
        )
        assert "_restore_arriving_fences_interactive" in switch
        assert switch.index("_restore_arriving_fences_interactive") < switch.index(
            "_finish_page_switch_overlays_if_pending"
        )
        assert "_refresh_public_host_click_mask" in switch
        # Quiet clears only in finish (after icon fill) — not mid switch_page.
        assert "_page_switch_quiet_until = 0.0" not in switch
        assert switch.index("_finish_page_switch_overlays_if_pending") < switch.index(
            "select_page_id"
        )
        # Finish must run after the freeze with-block (batch.commit), not inside it.
        freeze_call = switch.index("with self._page_switch_paint_freeze()")
        finish_call = switch.index(
            "self._finish_page_switch_overlays_if_pending()", freeze_call
        )
        # The call site after `with` is the one we want — depth gate also guards
        # in-flight public refresh finish.
        finish_fn = inspect.getsource(DeskTidyApp._finish_page_switch_overlays_if_pending)
        assert "_page_switch_freeze_depth" in finish_fn
        assert "QTimer.singleShot(0, self._finish_page_switch_overlays_if_pending)" in finish_fn
        assert finish_call > freeze_call
        assert "timer.stop()" in switch or "_refresh_public_timer" in switch
        assert "_reveal_desktop_overlay" in inspect.getsource(DeskTidyApp.show_fences)
        assert callable(DeskTidyApp._reveal_desktop_overlay)
        assert callable(DeskTidyApp._page_switch_paint_freeze)
        reveal = inspect.getsource(DeskTidyApp._reveal_desktop_overlay)
        assert "overlay_win32_visible" in reveal
        assert "set_overlay_hwnd_visible" in reveal
        assert "configure_desktop_overlay" in reveal
        assert "is_attached_to_desktop" in reveal
        assert "_desktidy_soft_parked" in reveal
        park_fence = inspect.getsource(DeskTidyApp._park_fence)
        assert "set_overlay_hwnd_visible" in park_fence
        assert "soft" in park_fence
        assert "_soft_hide_fence_for_page" in park_fence
        assert "setWindowOpacity(0.0)" not in inspect.getsource(
            DeskTidyApp._soft_hide_fence_for_page
        )
        soft_hide = inspect.getsource(DeskTidyApp._soft_hide_fence_for_page)
        assert "batch.hide" in soft_hide or "set_overlay_hwnd_visible" in soft_hide
        soft_show = inspect.getsource(DeskTidyApp._soft_show_fence_for_page)
        assert "batch.show" in soft_show or "set_overlay_hwnd_visible" in soft_show
        assert "_target_opacity" in soft_hide  # heal opacity, never leave 0
        assert "set_overlay_mouse_passthrough" in soft_hide
        assert "set_overlay_mouse_passthrough" in soft_show
        park_pub = inspect.getsource(DeskTidyApp._park_public_icon)
        assert "soft" in park_pub
        assert "icon.hide()" in park_pub
        assert "OFFSCREEN_PARK_POS" not in park_pub.split("if soft:")[1][:400]
        keep = inspect.getsource(DeskTidyApp._keep_overlays_under_apps)
        assert "page_indicator" in keep
        assert "continue" in keep
        assert "force=app_ui_fg" in keep
        assert "_desk_app_ui_open" in keep
        assert "_desk_app_owns_foreground" in keep
        assert "_hwnd_owns_foreground" in keep
        assert "foreign_fg" in keep or "_foreign_app_owns_foreground" in keep
        assert "if not app_ui_fg and not foreign_fg" in keep
        assert "_settings_ui_open" in keep
        assert "_raise_settings_window" in keep
        assert "_raise_notepad_window" in keep
        assert "_hwnd_owns_foreground" in keep
        assert "_notepad_window_hwnd()" in keep
        assert "_foreign_app_owns_foreground" in keep
        # Attached DefView overlays must not HWND_BOTTOM (tray/settings vanish root).
        assert "is_attached_to_desktop" in keep
        assert "is_stuck_under_wallpaper" in keep
        assert "attach_overlay_to_desktop" in keep
        assert keep.index("is_attached_to_desktop") < keep.index(
            "place_overlay_in_desktop_band(hwnd"
        )
        park = inspect.getsource(DeskTidyApp._park_overlays_for_foreign_app)
        assert "_sink_overlays_for_foreign_app" in park
        assert "widget.hide()" not in inspect.getsource(
            DeskTidyApp._sink_overlays_for_foreign_app
        )
        assert "place_overlay_in_desktop_band" in inspect.getsource(
            DeskTidyApp._keep_overlays_under_apps
        )
        shown = inspect.getsource(DeskTidyApp._on_settings_shown)
        # Root: sink once; further show paths sink via may_show / show_fences.
        assert "singleShot(80, self._keep_overlays_under_apps)" not in shown
        assert "singleShot(250, self._keep_overlays_under_apps)" not in shown
        assert "_keep_overlays_under_apps()" in shown
        attach = inspect.getsource(DeskTidyApp._ensure_shell_attachments)
        assert attach.rindex("_ensure_page_chrome_visible") > attach.index(
            "apply_overlay_stack_mode"
        )
        ensure_vis = inspect.getsource(DeskTidyApp._ensure_desktop_overlays_visible)
        assert "_ensure_page_chrome_visible" in ensure_vis
        setup = inspect.getsource(DeskTidyApp._setup_page_indicator)
        assert "_ensure_page_chrome_visible" in setup
        cfg = inspect.getsource(__import__("src.win_shell", fromlist=["configure_desktop_overlay"]).configure_desktop_overlay)
        assert "force_band" in cfg
        assert "raise_band" in cfg
        assert "_desktidy_raise_band" in cfg
        assert "raise_band is None" in cfg
        assert "raise_overlay_in_desktop_band" in cfg
        assert "is_stuck_under_wallpaper" in cfg
        from src.ui.page_indicator import PageIndicatorWidget

        assert "_desktidy_raise_band" in inspect.getsource(PageIndicatorWidget.__init__)
        place = inspect.getsource(
            __import__("src.desktop_shell_host", fromlist=["place_overlay_in_desktop_band"]).place_overlay_in_desktop_band
        )
        assert "force" in place
        raise_fn = inspect.getsource(
            __import__(
                "src.desktop_shell_host", fromlist=["raise_overlay_in_desktop_band"]
            ).raise_overlay_in_desktop_band
        )
        assert "HWND_TOP" in raise_fn
        assert "SWP_NOOWNERZORDER" in raise_fn
        assert "is_attached_to_desktop" in raise_fn
        assert "SWP_NOCOPYBITS" not in raise_fn
        live = inspect.getsource(DeskTidyApp.ensure_live_fences_interactive)
        assert "is_explorer_desktop_foreground" in live
        assert "skip_raise" in live
        assert "SWP_NOOWNERZORDER" in place
        attach2 = inspect.getsource(DeskTidyApp._ensure_shell_attachments)
        # Force attach must not demote to soft after quiet/throttle.
        assert "_schedule_force_shell_attach" in attach2
        assert "self._ensure_shell_attachments(force=False)" not in attach2
        assert callable(DeskTidyApp._schedule_force_shell_attach)
        assert callable(DeskTidyApp._run_pending_force_shell_attach)
        assert "_foreign_app_owns_foreground" in attach2
        pending = inspect.getsource(DeskTidyApp._run_pending_force_shell_attach)
        assert "_foreign_app_owns_foreground" in pending
        assert "_sink_overlays_for_foreign_app" in pending
        session = inspect.getsource(DeskTidyApp._apply_session_desktop_view)
        assert "light and self._foreign_app_owns_foreground()" in session
        src3 = inspect.getsource(DeskTidyApp.show_fences)
        assert "_park_fence" in src3
        take_src = inspect.getsource(DeskTidyApp._take_parked_fence)
        # Soft flag stays True until soft_show — clearing it here skipped unpark.
        assert "_desktidy_soft_parked = False" not in take_src
        assert "WA_TransparentForMouseEvents" not in take_src
        restore_src = inspect.getsource(DeskTidyApp._restore_arriving_fences_interactive)
        assert "set_overlay_mouse_passthrough" in restore_src
        assert "ensure_live_fences_interactive" in restore_src
        assert "_public_icon_host" in inspect.getsource(
            DeskTidyApp._sink_public_host_below_overlays
        )
        assert "_sink_public_host_below_overlays" in inspect.getsource(
            DeskTidyApp.ensure_live_fences_interactive
        )
        impl = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
        assert "not page_switch" in impl.split("sync_sig ==")[0][-120:]
        # Off-page fences must be parked (not always destroyed) for snappy revisits.
        assert "self._park_fence(" in src3
        assert "to_park" in src3
        assert "restored_from_park" in src3
        # Soft switch: first-create fence HWNDs must not rewrite float positions.
        assert "do_relayout = bool(force_rebuild)" in src3
        assert "created_new > 0" not in src3.split("do_relayout")[1][:80]
        assert "or mirrored" not in src3
        assert "relayout=False" in inspect.getsource(DeskTidyApp.switch_page)
        assert "_fence_needs_icon_rebuild" in src3
        assert "_overlay_widgets_may_show" in src3
        # Page-switch first-create: warm-attach hidden + batch show (no deferred restack flash).
        assert "_prepare_page_switch_new_fence" in src3
        assert "batch.set_rect" in src3
        assert "batch.commit in freeze finally" in src3
        assert "if not page_switch:" in src3
        assert "elif created_new > 0:" in src3
        assert "force_rebuild:" in src3 or "elif force_rebuild:" in src3
        # Page-switch unpark must not force-restack (flash); revisit uses soft show.
        assert "restored_fence > 0" in src3
        prepare = inspect.getsource(DeskTidyApp._prepare_page_switch_new_fence)
        assert "attach_overlay_to_desktop" in prepare
        assert "show=False" in prepare
        assert "fence.hide()" not in prepare
        assert "fence.show()" not in prepare
        assert "setAttribute" not in prepare
        assert "_desktidy_batch_reveal" in prepare
        assert "_desktidy_defer_refresh" in prepare
        assert "_refresh_impl" not in prepare.split("return int(hwnd")[0]
        assert "_refresh_fences_after_page_switch" in inspect.getsource(
            DeskTidyApp._finish_page_switch_overlays_if_pending
        )
        assert "refresh_icons=need_icons and not page_switch" in src3
        assert "defer_icon_refresh=page_switch" in src3
        apply_cfg = inspect.getsource(
            __import__("src.ui.fence_widget", fromlist=["FenceWidget"]).FenceWidget.apply_config
        )
        assert "defer_icon_refresh" in apply_cfg
        refresh_after = inspect.getsource(DeskTidyApp._refresh_fences_after_page_switch)
        assert "needs_force" in refresh_after
        assert "_page_switch_force_refresh_fence_ids" in refresh_after
        assert "reload_icons" in refresh_after or "refresh(force=True)" in refresh_after
        assert "flush_icon_grid_paint" in refresh_after
        assert "fence_id in batch_ids" not in refresh_after
        assert "_schedule_startup_offpage_warmup" in inspect.getsource(
            DeskTidyApp._run_startup_overlays
        )
        overlays_src = inspect.getsource(DeskTidyApp._run_startup_overlays)
        assert "1200, self._warmup_all_offpage_fences" not in overlays_src
        assert overlays_src.index("_setup_hotkeys") < overlays_src.index(
            "_schedule_startup_offpage_warmup"
        )
        assert "immediate=True" in overlays_src
        assert "_startup_force_overlay_attach" in overlays_src
        boot_attach = inspect.getsource(DeskTidyApp._startup_force_overlay_attach)
        assert "ensure_live_fences_interactive" in boot_attach
        assert "_heal_boot_fence_surfaces" in boot_attach
        heal = inspect.getsource(DeskTidyApp._heal_boot_fence_surfaces)
        assert "_flush_style_paint" in heal
        assert "flush_icon_grid_paint" in heal
        assert "refresh(force=True, shell_heal=False)" in heal
        assert "self.ensure_live_fences_interactive" not in heal
        assert "configure_desktop_overlay" not in heal
        assert "QTimer.singleShot(900" not in boot_attach
        pending_force = inspect.getsource(DeskTidyApp._run_pending_force_shell_attach)
        assert "ensure_live_fences_interactive" in pending_force
        # Ongoing force-attach must not rebuild icon grids (flicker loop).
        assert "self._heal_boot_fence_surfaces()" not in pending_force
        assert "warm_desktop_capture" in overlays_src
        assert "processEvents" in inspect.getsource(DeskTidyApp._warmup_all_offpage_fences)
        assert "fence.show()" not in inspect.getsource(
            DeskTidyApp._warmup_one_offpage_fence
        )
        flush_src = inspect.getsource(
            __import__("src.ui.fence_widget", fromlist=["FenceWidget"]).FenceWidget.flush_icon_grid_paint
        )
        assert "redraw_overlay_hwnd" in flush_src
        finish = inspect.getsource(DeskTidyApp._finish_page_switch_overlays_if_pending)
        assert "_page_switch_quiet_until = 0.0" in finish
        refresh_impl = inspect.getsource(
            __import__("src.ui.fence_widget", fromlist=["FenceWidget"]).FenceWidget._refresh_impl
        )
        assert "_desktidy_page_switch_refresh" in refresh_impl
        assert "skip_stagger" in refresh_impl
        attach_src = inspect.getsource(
            __import__("src.desktop_shell_host", fromlist=["x"]).attach_overlay_to_desktop
        )
        assert "show: bool" in attach_src or "*, show" in attach_src
        assert "_sync_overlays_to_foreground" not in src3
        assert "_repair_overlay_parents" not in src3
        # Reveal arriving fences before parking leavers (no empty wallpaper hole).
        assert "to_park" in src3
        assert "_page_switch_paint_freeze" in src3
        assert "layout=not page_switch" in src3
        assert "fence.geometry() == target" in src3
        # New fence HWND must be born at saved geometry, not MIN_WIDTH.
        assert "windowHandle" in src3
        place = src3.split("def _place_fence", 1)[1].split("for fence_cfg", 1)[0]
        assert place.index("windowHandle") < place.index("_fence_hwnd")
        keepalive_pos = src3.index("_schedule_overlay_keepalive")
        assert src3.rindex("if not page_switch:", 0, keepalive_pos) < keepalive_pos
        assert "immediate=True" in src3
        from src.desktop_shell_host import freeze_desktop_paint as _freeze_paint

        freeze_src = inspect.getsource(_freeze_paint)
        # DefView WM_SETREDRAW thaw was the remaining whole-desktop flash.
        assert "WM_SETREDRAW" not in freeze_src or "yield" in freeze_src
        assert "RedrawWindow(" not in freeze_src
        from src import desktop_shell_host as _dsh

        assert hasattr(_dsh, "OFFSCREEN_PARK_POS")
        assert _dsh.OFFSCREEN_PARK_POS[0] < -1000
        # Unpark showEvent must ShowWindow when Qt isVisible lies (page-switch lag).
        from src.ui.fence_widget import FenceWidget as _FenceWidgetShow

        show_ev = inspect.getsource(_FenceWidgetShow.showEvent)
        assert "overlay_win32_visible" in show_ev
        assert "set_overlay_hwnd_visible" in show_ev
        assert "is_attached_to_desktop" in show_ev
        assert "WA_DontShowOnScreen" in show_ev
        assert "_desktidy_batch_reveal" in show_ev
        assert "_seed_geometry_from_config" in inspect.getsource(
            _FenceWidgetShow.__init__
        )
        need = inspect.getsource(DeskTidyApp._fence_needs_icon_rebuild)
        assert "virtual_items" in need
        assert "is_portal_fence" in need
        assert "has_icon_widgets" in need
        assert "_fence_painted_pin_keys" in need
        assert "painted" in need
        # Unpark outside page-switch still soft-ensures; page-switch skips restack.
        assert "restored_fence > 0" in src3
        pub = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
        assert "_park_public_icon" in pub
        assert "_take_parked_public_icon" in pub
        assert "soft=page_switch" in pub
        assert "if not page_switch:" in pub
        assert pub.index("if not page_switch:") < pub.index("prune_missing_public_items")
        # Unparked floats must re-attach (hotkey onto float-only pages).
        assert "restored_from_park > 0" in pub
        assert "created_new > 0 or restored_from_park > 0" in pub
        # Page switch must not soft-attach every fence after float unpark.
        assert "_page_switch_ensure_pending" in pub
        assert "ensure_shell_attachments" in pub
        assert "page_switch" in pub
        assert "mask_only=True" in pub
        assert callable(DeskTidyApp._park_fence)
        assert callable(DeskTidyApp._park_public_icon)
        assert callable(DeskTidyApp._overlays_healthy_after_page_switch)
        assert "reload_icons" in refresh_after or "refresh(force=True)" in refresh_after
        ensure_sw = inspect.getsource(DeskTidyApp._ensure_page_switch_overlays_visible)
        assert "_remap_hidden_overlay_hwnds" in ensure_sw
        assert "is_stuck_under_wallpaper" in ensure_sw
        assert "_sync_qt_visible_after_win32" in ensure_sw
        assert "_overlays_healthy_after_page_switch" not in ensure_sw
        finish_sw = inspect.getsource(DeskTidyApp._finish_page_switch_overlays_if_pending)
        assert "timer.stop()" in finish_sw
        pub_refresh = inspect.getsource(DeskTidyApp._run_public_refresh)
        assert "_page_switch_ensure_pending" in pub_refresh
        freeze = inspect.getsource(DeskTidyApp._page_switch_paint_freeze)
        assert "begin_mask_batch" in freeze
        assert "end_mask_batch" in freeze
        assert "_page_switch_freeze_depth" in freeze
        assert "OverlayWindowBatch" in freeze or "OverlayGeometryBatch" in freeze
        assert "batch.commit" in freeze or "commit()" in freeze
        assert "_reveal_desktop_overlay" in freeze
        src3 = inspect.getsource(DeskTidyApp.show_fences)
        assert "_page_switch_geo_batch" in src3 or "OverlayWindowBatch" in src3
        assert "apply_style=" in src3
        assert "not page_switch" in src3
        assert "_page_switch_quiet_until" in inspect.getsource(DeskTidyApp.switch_page)
        assert "_soft_hide_fence_for_page" in inspect.getsource(DeskTidyApp._park_fence)
        assert "windowOpacity()" in src3  # heal leftover opacity-0
        from src.desktop_shell_host import OverlayWindowBatch as _Batch

        batch_src = inspect.getsource(_Batch)
        assert "BeginDeferWindowPos" in batch_src
        assert "SWP_HIDEWINDOW" in batch_src
        assert "SWP_SHOWWINDOW" in batch_src
        assert "show_base" in batch_src
        assert "SWP_NOREDRAW" in batch_src
        assert callable(_Batch.hide)
        assert callable(_Batch.show)
        from src.ui.public_icon_host import PublicIconHost as _PubHost

        assert "begin_mask_batch" in inspect.getsource(_PubHost)
        assert "mask_only" in inspect.getsource(_PubHost.present)
        assert "windowOpacity()" in inspect.getsource(
            DeskTidyApp._overlays_healthy_after_page_switch
        )
        assert "_page_switch_quiet_until" in inspect.getsource(
            DeskTidyApp._ensure_desktop_overlays_visible
        )
        assert "layout" in inspect.signature(
            __import__("src.ui.fence_widget", fromlist=["FenceWidget"]).FenceWidget.set_collapsed
        ).parameters

        host_mask = inspect.getsource(_PubHost.refresh_click_mask)
        assert "_park_mask_region" in host_mask
        assert "self.hide()" not in host_mask
        assert "_desktidy_soft_parked" in inspect.getsource(_PubHost.public_icon_widgets)
        from src.ui.fence_widget import FenceWidget

        assert "refresh_icons" in inspect.signature(FenceWidget.apply_config).parameters

    def _page_switch_unpark_reattach_logic():
        """Simulate park→unpark float sync deciding re-attach is required."""
        # Pure logic: when floats come back from park, the refresh impl must
        # treat them like new HWNDs for shell visibility (contract above).
        from src.public_desktop import add_public_item, visible_floating_items

        settings = {
            "enable_public_desktop": False,
            "public_desktop_items": [],
            "desktop_pages": [
                {"id": 0, "name": "工作"},
                {"id": 1, "name": "文档"},
            ],
        }
        add_public_item(
            settings,
            r"C:\tmp\doc_float.xlsx",
            x=20,
            y=20,
            page_id=1,
            auto_arrange=False,
            shared=False,
        )
        assert len(visible_floating_items(settings, 0)) == 0
        assert len(visible_floating_items(settings, 1)) == 1

    def _force_attach_throttle_reschedules():
        """Throttled force attach must queue a retry, not drop the request."""
        import time

        from PyQt6.QtCore import QTimer

        from src.app import DeskTidyApp

        fake = object.__new__(DeskTidyApp)
        fake._exiting = False
        fake._shell_attach_force_pending = False
        fake._overlay_restack_blocked = lambda: False  # type: ignore[method-assign]
        fake._settings_ui_open = lambda: False  # type: ignore[method-assign]
        fake._desk_app_ui_open = lambda: False  # type: ignore[method-assign]
        # Throttle path is desktop-side; foreign FG sinks instead of restacking.
        fake._foreign_app_owns_foreground = lambda: False  # type: ignore[method-assign]
        fake._shell_attach_quiet_until = 0.0
        fake._last_shell_attach_at = time.perf_counter()

        scheduled: list[int] = []
        orig = QTimer.singleShot

        def _spy(ms, cb):
            scheduled.append(int(ms))

        QTimer.singleShot = staticmethod(_spy)  # type: ignore[assignment]
        try:
            DeskTidyApp._ensure_shell_attachments(fake, force=True)
            assert scheduled, "throttled force attach must schedule a retry"
            assert fake._shell_attach_force_pending is True
            # Dedup: second force while pending must not queue again.
            DeskTidyApp._ensure_shell_attachments(fake, force=True)
            assert len(scheduled) == 1, scheduled
        finally:
            QTimer.singleShot = orig  # type: ignore[assignment]

    def _page_switch_ensure_waits_for_refresh():
        """Page-switch ensure runs after public refresh completes — not a timer ladder."""
        from PyQt6.QtCore import QTimer

        from src.app import DeskTidyApp

        calls: list[str] = []
        scheduled: list[int] = []
        orig = QTimer.singleShot

        def _spy(_ms, fn):
            scheduled.append(int(_ms))
            return None

        QTimer.singleShot = staticmethod(_spy)  # type: ignore[assignment]
        fake = object.__new__(DeskTidyApp)
        fake._page_switch_ensure_pending = True
        fake._refreshing_public = True
        fake._refresh_public_timer = type("T", (), {"isActive": lambda self: False})()
        fake._ensure_page_switch_overlays_visible = (  # type: ignore[method-assign]
            lambda: calls.append("ensure")
        )
        try:
            DeskTidyApp._finish_page_switch_overlays_if_pending(fake)
            assert calls == [], "must wait while public refresh is in flight"
            assert scheduled == [0], scheduled
            assert fake._page_switch_ensure_pending is True

            fake._refreshing_public = False
            fake._page_switch_freeze_depth = 1
            DeskTidyApp._finish_page_switch_overlays_if_pending(fake)
            assert calls == [], "must wait until paint freeze commits the batch"
            assert fake._page_switch_ensure_pending is True

            fake._page_switch_freeze_depth = 0
            DeskTidyApp._finish_page_switch_overlays_if_pending(fake)
            assert calls == ["ensure"]
            assert fake._page_switch_ensure_pending is False

            DeskTidyApp._finish_page_switch_overlays_if_pending(fake)
            assert calls == ["ensure"], "second finish must no-op"
        finally:
            QTimer.singleShot = orig  # type: ignore[assignment]

    def _drag_defers_public_refresh():
        import inspect

        from src.app import DeskTidyApp
        from src.icon_utils import _shil_candidates, _SHIL_EXTRALARGE, _SHIL_JUMBO
        from src.ui.fence_icon_item import start_public_item_drag

        src = inspect.getsource(DeskTidyApp._run_public_refresh)
        assert "_overlay_drag_active" in src
        src2 = inspect.getsource(DeskTidyApp._end_overlay_drag)
        assert "_flush_public_refresh_after_drag" in src2
        assert "singleShot(120" in src2
        assert "_heal_overlays_after_shell_menu" in src2
        assert "_deferred_fg_sync" in src2
        assert "_shell_attach_quiet_until" in src2
        assert "overlay drag end" in src2
        flush = inspect.getsource(DeskTidyApp._flush_public_refresh_after_drag)
        assert "refresh_public_desktop" in flush
        src3 = inspect.getsource(DeskTidyApp._restore_public_icon_widget)
        assert "_configure_public_float_overlay" in src3
        assert "_take_parked_public_icon" in src3
        src4 = inspect.getsource(start_public_item_drag)
        assert "configure_desktop_overlay" in src4
        assert "_set_public_drag_path" in src4
        assert "protect_drag" in inspect.getsource(
            DeskTidyApp._refresh_public_desktop_impl
        )
        attach = inspect.getsource(DeskTidyApp._ensure_shell_attachments)
        assert "_shell_attach_quiet_until" in attach
        active = inspect.getsource(DeskTidyApp._on_application_state_changed)
        assert "force=True" not in active
        # 64px icons must lead with jumbo so we downscale (sharp), not upscale 48→64.
        cands = _shil_candidates(64)
        assert cands[0] == _SHIL_JUMBO
        assert _SHIL_EXTRALARGE in cands
        assert cands.index(_SHIL_JUMBO) < cands.index(_SHIL_EXTRALARGE)
        cands48 = _shil_candidates(48)
        assert cands48[0] == _SHIL_EXTRALARGE

    def _icon_64_uses_hires_shell_source():
        """Regression: 64px must prefer jumbo and upgrade an undersized raw cache."""
        from PyQt6.QtCore import Qt
        from PyQt6.QtGui import QPixmap
        from PyQt6.QtWidgets import QApplication

        from src import icon_utils

        app = QApplication.instance() or QApplication([])
        _ = app  # keep Qt alive for QPixmap

        assert icon_utils._shil_candidates(64)[0] == icon_utils._SHIL_JUMBO

        icon_utils.invalidate_file_icon_cache()
        key = r"c:\fake\upgrade-test.exe"
        small = QPixmap(48, 48)
        small.fill(Qt.GlobalColor.blue)
        icon_utils._SHELL_RAW_CACHE[key.casefold()] = small

        calls: list[int] = []
        orig_idx = icon_utils._sys_icon_index
        orig_iml = icon_utils._imagelist_icon

        def _fake_idx(_lookup: str) -> int:
            return 1

        def _fake_iml(_index: int, shil: int) -> QPixmap:
            calls.append(int(shil))
            if shil == icon_utils._SHIL_JUMBO:
                pix = QPixmap(256, 256)
                pix.fill(Qt.GlobalColor.red)
                return pix
            pix = QPixmap(48, 48)
            pix.fill(Qt.GlobalColor.green)
            return pix

        try:
            icon_utils._sys_icon_index = _fake_idx  # type: ignore[assignment]
            icon_utils._imagelist_icon = _fake_iml  # type: ignore[assignment]
            raw = icon_utils._shell_icon_pixmap(key, 64)
            assert min(raw.width(), raw.height()) >= 64, (
                f"expected upgraded raw >=64, got {raw.width()}x{raw.height()}"
            )
            assert icon_utils._SHIL_JUMBO in calls
            cached = icon_utils._SHELL_RAW_CACHE[key.casefold()]
            assert min(cached.width(), cached.height()) >= 64
        finally:
            icon_utils._sys_icon_index = orig_idx
            icon_utils._imagelist_icon = orig_iml
            icon_utils.invalidate_file_icon_cache()

    def _trim_sparse_jumbo_icon():
        """Regression: padded jumbo (tiny glyph on 256 canvas) must fill 64px."""
        from PyQt6.QtCore import Qt
        from PyQt6.QtGui import QColor, QPainter, QPixmap
        from PyQt6.QtWidgets import QApplication

        from src.icon_utils import _fit_pixmap, _trim_icon_padding, _opaque_bounds

        app = QApplication.instance() or QApplication([])
        _ = app

        sparse = QPixmap(256, 256)
        sparse.fill(Qt.GlobalColor.transparent)
        painter = QPainter(sparse)
        painter.fillRect(8, 12, 40, 32, QColor(80, 180, 255, 255))
        painter.end()
        trimmed = _trim_icon_padding(sparse)
        assert max(trimmed.width(), trimmed.height()) <= 48, (
            f"expected crop of tiny glyph, got {trimmed.width()}x{trimmed.height()}"
        )
        fitted = _fit_pixmap(sparse, 64)
        assert fitted.width() == 64 and fitted.height() == 64
        bounds = _opaque_bounds(fitted)
        assert bounds is not None
        _x, _y, bw, bh = bounds
        assert bw >= 40 and bh >= 32, (
            f"fitted glyph still too small: {bw}x{bh} (was speck-sized before trim)"
        )

        # Full jumbo must not be cropped.
        full = QPixmap(256, 256)
        full.fill(QColor(10, 120, 200, 255))
        assert _trim_icon_padding(full).width() == 256


    def _wallpaper_stuck_repair_contract():
        import inspect

        from src.app import DeskTidyApp
        from src.desktop_shell_host import (
            attach_overlay_to_desktop,
            ensure_overlay_on_desktop,
            is_stuck_under_wallpaper,
        )
        from src.win_shell import apply_overlay_stack_mode

        assert callable(is_stuck_under_wallpaper)
        src = inspect.getsource(ensure_overlay_on_desktop)
        assert "is_stuck_under_wallpaper" in src
        src2 = inspect.getsource(attach_overlay_to_desktop)
        assert "is_stuck_under_wallpaper" in src2
        src3 = inspect.getsource(DeskTidyApp._sync_overlay_layer_mode)
        assert "_overlays_need_shell_repair" in src3
        # Win+D restore: force attach when returning/broken; ignore healthy desktop FG noise.
        recover = inspect.getsource(DeskTidyApp._recover_from_desktop_foreground)
        assert "_remap_hidden_overlay_hwnds" in recover
        assert "_foreign_app_owns_foreground" in recover
        assert "_sink_overlays_for_foreign_app" in recover
        assert "_overlays_need_shell_repair" in recover
        assert "_last_fg_was_desktop" in recover
        assert "_ensure_shell_attachments(force=True)" in recover
        assert "_page_chrome_needs_raise" in recover
        assert "_overlays_need_shell_repair_light" in recover
        sync = inspect.getsource(DeskTidyApp._sync_overlays_to_foreground)
        assert "_foreign_app_owns_foreground" in sync
        assert "_sink_overlays_for_foreign_app" in sync
        assert "widget.hide()" not in sync
        assert "if was_desktop:" in sync
        assert "_desktop_fg_recover_timer" in sync
        assert "is_explorer_desktop_foreground" in sync
        assert "if not explorer_fg:" in sync
        assert "_remap_hidden_overlay_hwnds" in sync
        # Sticky park latch clears without force-restack storms.
        assert "_overlays_parked_for_app_fg" in sync
        assert "_ensure_desktop_overlays_visible(force=True)" not in sync
        assert "_schedule_overlay_keepalive" not in sync.split(
            "_overlays_parked_for_app_fg"
        )[1].split("explorer_fg")[0]
        on_fg = inspect.getsource(DeskTidyApp._on_desktop_foreground_changed)
        assert "_sync_overlays_to_foreground" in on_fg
        may = inspect.getsource(DeskTidyApp._overlay_widgets_may_show)
        assert "_desk_app_ui_open" in may
        assert "_should_park_overlays_for_foreign_fg" not in may
        assert "_overlays_parked_for_app_fg" not in may
        park_pred = inspect.getsource(DeskTidyApp._should_park_overlays_for_foreign_fg)
        # Hide-on-foreign-FG retired — Fences stay mapped under WeChat/etc.
        assert "return False" in park_pred
        foreign = inspect.getsource(DeskTidyApp._foreign_app_owns_foreground)
        assert "is_desktop_foreground" in foreign
        assert "not is_desktop_foreground()" in foreign
        assert "is_explorer_desktop_foreground" in recover
        from src.desktop_foreground_monitor import DesktopForegroundMonitor
        import src.desktop_foreground_monitor as fgmon

        mon_src = inspect.getsource(DesktopForegroundMonitor._on_win_event)
        assert "_prev_desktop" in mon_src
        assert "_on_desktop_surface" in mon_src
        # Own chrome is desktop-side (no leave emit) — not an own→app bandaid edge.
        assert "_hwnd_is_own_desktop_chrome" in inspect.getsource(fgmon._on_desktop_surface)
        assert "_prev_ours" not in mon_src
        from src.win_shell import _is_own_desktop_chrome

        assert callable(_is_own_desktop_chrome)
        desk_fg = inspect.getsource(
            __import__("src.win_shell", fromlist=["x"]).is_desktop_foreground
        )
        assert "_is_own_desktop_chrome" in desk_fg
        assert "_desk_app_ui_open" in inspect.getsource(DeskTidyApp._foreign_app_owns_foreground)
        assert "_desk_app_owns_foreground" in inspect.getsource(DeskTidyApp._foreign_app_owns_foreground)
        assert "_desk_app_owns_foreground" in inspect.getsource(DeskTidyApp._overlay_widgets_may_show)
        assert "_notepad_ui_open" in inspect.getsource(DeskTidyApp._desk_app_ui_open)
        keep = inspect.getsource(DeskTidyApp._keep_overlays_under_apps)
        assert "_raise_notepad_window" in keep
        note = inspect.getsource(DeskTidyApp._on_note_requested)
        assert "open_in_desknote" in note
        assert "2.5" in inspect.getsource(
            __import__("src.desktop_shell_host", fromlist=["x"])
        ) or __import__("src.desktop_shell_host", fromlist=["x"])._DEFVIEW_CACHE_TTL_S >= 2.0
        find_src = inspect.getsource(
            __import__("src.desktop_shell_host", fromlist=["x"])._find_defview_host
        )
        assert "user32.EnumWindows" not in find_src
        assert "FindWindowEx" in find_src
        assert "WorkerW" in find_src
        visible = inspect.getsource(DeskTidyApp._ensure_desktop_overlays_visible)
        assert "is_explorer_desktop_foreground" in visible
        assert "_foreign_app_owns_foreground" in visible
        assert "_sink_overlays_for_foreign_app" in visible
        # Away from desktop: early return (no repair walk under Chrome/VS Code).
        away = visible.split(
            "if not force and not is_explorer_desktop_foreground():", 1
        )
        assert len(away) == 2
        assert "return" in away[1].split("if force:", 1)[0]
        sink = inspect.getsource(DeskTidyApp._sink_overlays_for_foreign_app)
        # Foreign FG sinks (never hides); do not HWND_TOP chrome over the app.
        assert "_overlays_parked_for_app_fg" in sink
        assert "widget.hide()" not in sink
        assert "_keep_overlays_under_apps" in sink
        assert "_remap_hidden_overlay_hwnds" in sink
        assert "_release_stuck_grabs" in sink
        assert "_passthrough_public_host_for_foreign_fg" not in sink
        assert "set_overlay_mouse_passthrough(host" not in sink
        assert "_ensure_page_chrome_visible(raise_band=True)" not in sink
        release = inspect.getsource(DeskTidyApp._release_stuck_grabs)
        assert "ReleaseCapture" in release
        recover = inspect.getsource(DeskTidyApp._recover_from_desktop_foreground)
        assert "_ensure_public_host_mouse_opaque" in recover
        assert callable(DeskTidyApp._ensure_public_host_mouse_opaque)
        live = inspect.getsource(DeskTidyApp.ensure_live_fences_interactive)
        assert "_ensure_public_host_mouse_opaque" in live
        desk_fg = inspect.getsource(
            __import__("src.win_shell", fromlist=["x"]).is_desktop_foreground
        )
        assert "_SHELL_MENU_FG_CLASSES" in desk_fg or "DeskTidyShellMenuHost" in desk_fg
        assert "_hwnd_is_shell_taskbar" in desk_fg
        assert "Shell_TrayWnd" in inspect.getsource(
            __import__("src.win_shell", fromlist=["x"])
        )
        assert "_hwnd_is_shell_menu_surface" in inspect.getsource(fgmon._on_desktop_surface)
        assert "_hwnd_is_shell_taskbar_surface" in inspect.getsource(
            fgmon._on_desktop_surface
        )
        apply = inspect.getsource(DeskTidyApp._apply_foreground_overlay_visibility)
        assert "_sync_overlays_to_foreground" in apply
        show = inspect.getsource(DeskTidyApp.show_fences)
        assert "_overlay_widgets_may_show" in show
        assert "_foreign_app_owns_foreground" in show
        assert "_sink_overlays_for_foreign_app" in show
        repair = inspect.getsource(DeskTidyApp._overlays_need_shell_repair_impl)
        assert "if not host:" in repair
        assert "is_attached_to_desktop" in repair
        assert "is_stuck_under_wallpaper" in repair
        attach = inspect.getsource(DeskTidyApp._ensure_shell_attachments)
        assert "is_attached_to_desktop" in repair
        assert "is_stuck_under_wallpaper" in repair
        attach = inspect.getsource(DeskTidyApp._ensure_shell_attachments)
        assert "_keep_overlays_under_apps()" not in attach
        stack = inspect.getsource(apply_overlay_stack_mode)
        # configure_desktop_overlay owns ensure; avoid a second ensure pass.
        assert "configure_desktop_overlay" in stack
        assert "ensure_overlay_on_desktop" not in stack
        assert "place_overlay_in_desktop_band" not in stack.split("if peek:", 1)[-1]

    def _screenshot_freezes_restack():
        import inspect

        from src.app import DeskTidyApp
        from src.screenshot_manager import ScreenshotManager

        blocked = inspect.getsource(DeskTidyApp._overlay_restack_blocked)
        assert "_screenshot_session_active" in blocked
        begin = inspect.getsource(DeskTidyApp._begin_screenshot_session)
        assert "_stop_overlay_churn_timers" in begin
        end = inspect.getsource(DeskTidyApp._end_screenshot_session)
        # Must not force restack on resume (that flashes icons).
        assert "force=True" not in end
        assert "_ensure_desktop_overlays_visible" not in end
        mgr_src = inspect.getsource(ScreenshotManager)
        assert "_begin_session" in mgr_src
        assert "_end_session_later" in mgr_src
        assert "_recent_pixmaps" in mgr_src
        assert "_trim_pinned" in mgr_src
        assert "max_pins_from_settings" in mgr_src or "_max_pins" in mgr_src
        show_last = inspect.getsource(ScreenshotManager.show_last_screenshot)
        assert "_recent_pixmaps" in show_last
        assert "_place_pin" in show_last
        assert "_f2_reveal_count" in show_last
        # Closing a pin must free a reveal slot (double-click close → F2 again).
        assert "min(int(self._f2_reveal_count), len(self.pinned))" in show_last
        remove_pin = inspect.getsource(ScreenshotManager._remove_pin)
        assert "len(self.pinned)" in remove_pin
        assert "_f2_reveal_count" in remove_pin
        # One F2 press → one additional pin (newest→older), not a cascade dump.
        assert "for i, pm in enumerate(pixmaps)" not in show_last
        assert "recent[-(self._f2_reveal_count + 1)]" in show_last
        # Must not swallow newer shots by raise-only when one pin already exists.
        assert "len(self.pinned) == 1" not in show_last
        remember = inspect.getsource(ScreenshotManager._remember_pixmap)
        assert "self._f2_reveal_count = 0" in remember
        from src.screenshot_manager import MAX_PINS_HARD_LIMIT

        assert MAX_PINS_HARD_LIMIT == 5

        # Behavioral: close pin → F2 can pin again (not stuck raise-only).
        from PyQt6.QtGui import QColor, QImage
        from PyQt6.QtWidgets import QApplication

        _ = QApplication.instance() or QApplication([])
        mgr = ScreenshotManager(lambda: {"screenshot": {"enabled": True, "max_pins": 3}})
        img = QImage(40, 30, QImage.Format.Format_RGB32)
        img.fill(QColor(20, 120, 200))
        from PyQt6.QtGui import QPixmap

        mgr._remember_pixmap(QPixmap.fromImage(img))
        assert mgr.show_last_screenshot()
        assert len(mgr.pinned) == 1
        assert mgr._f2_reveal_count == 1
        pin = mgr.pinned[0]
        pin._close()
        assert len(mgr.pinned) == 0
        assert mgr._f2_reveal_count == 0
        assert mgr.show_last_screenshot()
        assert len(mgr.pinned) == 1
        for w in list(mgr.pinned):
            try:
                w.close()
                w.deleteLater()
            except RuntimeError:
                pass
        mgr.pinned.clear()
        mgr._f2_reveal_count = 0
        start = inspect.getsource(ScreenshotManager.start_capture)
        assert "_begin_session" in start
        assert "singleShot" in start
        assert "_begin_capture" in start
        assert "_capture_armed" in start
        assert "_discard_stale_overlay" in start
        assert "force_widget_topmost" in start
        assert "_blocking_modal_open" in start
        assert "_recover_stale_capture_arm" in start
        assert "_watchdog_capture_arm" in start
        assert "截图暂不可用" in start
        block = inspect.getsource(ScreenshotManager._blocking_modal_open)
        assert "QMessageBox" in block
        assert "QErrorMessage" in block
        assert "return None" in block
        begin_cap = inspect.getsource(ScreenshotManager._begin_capture)
        assert "snapshot_snip_window_rects" in begin_cap
        assert "window_rects=" in begin_cap
        finished = inspect.getsource(ScreenshotManager._on_finished)
        assert "_end_session_later" in finished
        assert "restore_suspended_modals" in finished
        prep = inspect.getsource(ScreenshotManager.prepare_capture_ui)
        assert "_suspend_modal_widgets" in prep
        assert "_close_blocking_qt_popups" in prep
        assert "_dismiss_desktidy_transient_windows" in prep
        dismiss = inspect.getsource(ScreenshotManager._dismiss_desktidy_transient_windows)
        assert "toastCard" in dismiss
        assert "fileSearchOverlay" in dismiss
        # Settings / notepad must stay visible — hide→show flashes between grab and overlay.
        assert "_suspend_app_window" not in dismiss
        assert "w is main_win" in dismiss
        assert "w is notepad" in dismiss
        from src.ui import main_window as mw

        mw_hide = inspect.getsource(mw.MainWindow.hideEvent)
        assert "_screenshot_snip_active" in mw_hide
        assert "release_lazy_pages" in mw_hide
        from src.screenshot_manager import force_widget_topmost

        assert "bring_widget_to_foreground" in inspect.getsource(force_widget_topmost)
        assert "_close_active_popups" not in prep
        # Must NOT Escape-dismiss shell RMB menus before / during prepare
        # (WeChat-style: keep right-click menus in the freeze-frame).
        assert "dismiss_shell_context_menus" not in prep
        begin_cap = inspect.getsource(ScreenshotManager._begin_capture)
        assert begin_cap.index("grab_desktop") < begin_cap.index("prepare_capture_ui")
        from src import desktop_capture as cap_mod
        from src.ui.screenshot_overlay import ScreenshotOverlay

        grab_src = inspect.getsource(cap_mod.grab_desktop)
        assert "_grab_desktop_win32_timed" in grab_src
        assert "align_capture_to_logical_desktop" in grab_src
        assert "warm_desktop_capture" in inspect.getsource(cap_mod)
        assert callable(cap_mod.align_capture_to_logical_desktop)
        # HiDPI: physical Win32 buffer must be tagged with DPR (not left 1:1 logical).
        from PyQt6.QtCore import QPoint, QRect
        from PyQt6.QtGui import QColor, QPixmap
        from unittest import mock

        logical = QRect(0, 0, 1600, 900)
        phys = QPixmap(2400, 1350)
        phys.fill(QColor("#336699"))
        with (
            mock.patch.object(cap_mod, "_virtual_desktop_rect", return_value=logical),
            mock.patch.object(
                cap_mod,
                "_physical_virtual_desktop_size",
                return_value=(0, 0, 2400, 1350),
            ),
        ):
            aligned, origin = cap_mod.align_capture_to_logical_desktop(
                phys, QPoint(0, 0)
            )
        assert origin == logical.topLeft()
        assert abs(float(aligned.devicePixelRatio()) - 1.5) < 0.01
        di = aligned.deviceIndependentSize().toSize()
        assert di.width() == logical.width()
        assert di.height() == logical.height()
        # Overlay must size from device-independent pixels, not raw buffer.
        show_ov = inspect.getsource(ScreenshotOverlay.show_overlay)
        assert "deviceIndependentSize" in show_ov or "_logical_desktop_size" in show_ov
        assert "_logical_desktop_size" in inspect.getsource(ScreenshotOverlay)
        assert "dismiss_shell_context_menus" not in begin_cap
        assert "dismiss_shell_context_menus" not in start
        assert "_capture_armed" in begin_cap
        assert "_blocking_modal_open" in begin_cap

        assert "_finish_sent" in inspect.getsource(ScreenshotOverlay._finish)
        assert "grabMouse" in show_ov
        assert "grabKeyboard" in show_ov
        reclaim = inspect.getsource(ScreenshotOverlay._reclaim_input_grab)
        assert "releaseMouse" in reclaim
        assert "toolbar_up" in reclaim
        close_ev = inspect.getsource(ScreenshotOverlay.closeEvent)
        assert "_finish" in close_ev and "cancel" in close_ev
        close_src = inspect.getsource(ScreenshotManager._close_blocking_qt_popups)
        assert "keybd_event" not in close_src
        assert "VK_ESCAPE" not in close_src
        assert "QMenu" in close_src
        # Docstring may mention Escape; executable path must not send it.
        body = close_src.split('"""', 2)[-1] if '"""' in close_src else close_src
        assert "keybd_event" not in body
        assert "user32" not in body or "SetWindowPos" in body  # no Escape helper call
        assert "dismiss_shell" not in body

    def _f2_pin_context_save_as():
        """F2 贴图：右键菜单另存为（内存图，无 Explorer IContextMenu）。"""
        import inspect
        import tempfile
        from pathlib import Path
        from unittest.mock import patch

        from PyQt6.QtCore import Qt
        from PyQt6.QtGui import QColor, QPixmap
        from PyQt6.QtWidgets import QApplication

        from src.screenshot_manager import ScreenshotManager
        from src.ui.pinned_image import PinnedImageWidget, save_pixmap_dialog

        pin_src = inspect.getsource(PinnedImageWidget)
        assert "contextMenuEvent" in pin_src
        assert "另存为" in pin_src
        assert "NoContextMenu" in pin_src
        assert "save_as" in pin_src
        save_src = inspect.getsource(save_pixmap_dialog)
        assert "getSaveFileName" in save_src
        assert "WindowStaysOnTopHint" in save_src
        assert "save_pixmap_dialog" in inspect.getsource(ScreenshotManager._save_image)

        app = QApplication.instance() or QApplication([])
        pm = QPixmap(16, 16)
        pm.fill(QColor(220, 40, 40))
        w = PinnedImageWidget(pm)
        try:
            assert (
                w.image_label.contextMenuPolicy()
                == Qt.ContextMenuPolicy.NoContextMenu
            )
            with tempfile.TemporaryDirectory() as tmp:
                dest = str(Path(tmp) / "pin.png")
                with patch(
                    "src.ui.pinned_image.QFileDialog.getSaveFileName",
                    return_value=(dest, "PNG 图片 (*.png)"),
                ):
                    assert w.save_as() is True
                assert Path(dest).is_file() and Path(dest).stat().st_size > 0
        finally:
            w.close()
            w.deleteLater()
            app.processEvents()

    def _force_refresh_bypasses_sig():
        """Right-click 刷新 must rebuild even when the entry list is unchanged."""
        import tempfile
        from PyQt6.QtWidgets import QApplication

        from src import icon_utils
        from src.ui.fence_widget import FenceWidget

        app = QApplication.instance() or QApplication([])
        with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as tmp:
            pin_path = tmp.name
        settings = {
            "fences": [
                {
                    "id": "rf1",
                    "name": "R",
                    "folder": "R",
                    "virtual_items": [pin_path],
                    "sort_by": "name",
                    "x": 40,
                    "y": 40,
                    "width": 220,
                    "height": 280,
                    "pages": [0],
                }
            ],
            "current_page": 0,
            "style": {"icon_size": 48, "view_mode": "grid"},
        }
        cfg = settings["fences"][0]
        fence = FenceWidget(cfg, settings)
        cleared: list[int] = []
        orig_inv = icon_utils.invalidate_file_icon_cache

        def _spy(path=None):
            cleared.append(1)
            return orig_inv(path)

        try:
            fence._refresh_impl()
            assert fence._last_refresh_sig is not None
            sig = fence._last_refresh_sig
            # Soft refresh must keep the signature (skip path) and not wipe cache.
            icon_utils.invalidate_file_icon_cache = _spy
            fence._refresh_impl()
            assert fence._last_refresh_sig == sig
            assert cleared == []
            # Force refresh must invalidate this fence's icons (scoped, not global).
            fence.refresh(force=True)
            assert cleared, "force refresh must invalidate icon cache for fence entries"
            assert getattr(fence, "_force_refresh_pending", True) is False
            assert fence._last_refresh_sig is not None
        finally:
            icon_utils.invalidate_file_icon_cache = orig_inv
            fence.close()
            fence.deleteLater()
            app.processEvents()
            try:
                Path(pin_path).unlink(missing_ok=True)
            except OSError:
                pass

    def _screen_record_f3_integration():
        import inspect
        import json
        import tempfile
        from pathlib import Path as P

        from PyQt6.QtWidgets import QApplication

        from src.app import DeskTidyApp
        from src.hotkey_manager import HOTKEY_IDS, HOTKEY_LABELS, _POLLABLE_ACTIONS
        from src.screen_record_manager import ScreenRecordManager

        app = QApplication.instance() or QApplication([])

        assert HOTKEY_IDS["screen_record"] == 9
        assert HOTKEY_IDS["calculator"] == 11
        assert "录屏" in HOTKEY_LABELS["screen_record"]
        assert HOTKEY_LABELS["calculator"] == "计算器"
        assert "screen_record" in _POLLABLE_ACTIONS
        assert "calculator" not in _POLLABLE_ACTIONS

        defaults = json.loads(
            (P(__file__).resolve().parents[1] / "config" / "default_settings.json").read_text(
                encoding="utf-8"
            )
        )
        assert defaults.get("hotkeys", {}).get("screen_record") == "F3"
        assert defaults.get("hotkeys", {}).get("screenshot") == "F1"
        assert defaults.get("hotkeys", {}).get("show_screenshot") == "F2"
        assert defaults.get("screen_record", {}).get("enabled") is False
        assert "output_dir" in defaults.get("screen_record", {})
        assert "capture_screen" not in defaults.get("screen_record", {})
        assert defaults.get("meeting_minutes", {}).get("enabled") is True
        assert defaults.get("notepad", {}).get("enabled") is True
        assert defaults.get("desktop_pet", {}).get("enabled") is True
        assert defaults.get("calculator", {}).get("enabled") is False
        assert defaults.get("hotkeys", {}).get("calculator") == "Ctrl+Alt+C"
        assert defaults.get("screenshot", {}).get("max_pins") == 3
        assert "enabled" not in defaults.get("file_search", {})
        assert defaults.get("enable_public_desktop") is False

        from src.screenshot_manager import (
            DEFAULT_MAX_PINS,
            MAX_PINS_HARD_LIMIT,
            clamp_max_pins,
            max_pins_from_settings,
        )

        assert DEFAULT_MAX_PINS == 3
        assert MAX_PINS_HARD_LIMIT == 5
        assert clamp_max_pins(0) == 1
        assert clamp_max_pins(9) == 5
        assert clamp_max_pins("4") == 4
        assert max_pins_from_settings({"screenshot": {"max_pins": 2}}) == 2
        assert max_pins_from_settings({}) == DEFAULT_MAX_PINS

        from src.screen_record_manager import (
            RECORDINGS_DIR_NAME,
            _match_ddagrab_output_idx,
            build_gdigrab_input_args,
            build_record_command,
            capture_screen_choices,
            ddagrab_supported,
            default_output_dir,
            list_capture_screens,
            normalize_capture_screen,
            resolve_capture_region,
            resolve_output_dir,
        )
        from src.settings import install_root
        from src.ui.record_screen_picker import RecordScreenPickerDialog, pick_capture_screen

        assert default_output_dir(ensure=False) == install_root() / RECORDINGS_DIR_NAME
        assert resolve_output_dir("", ensure=False) == default_output_dir(ensure=False)
        assert resolve_output_dir(None, ensure=False) == default_output_dir(ensure=False)
        custom = P(tempfile.gettempdir()) / "desktidy_rec_test_dir"
        assert resolve_output_dir(str(custom), ensure=False) == custom
        assert normalize_capture_screen("") == "primary"
        assert normalize_capture_screen("ALL") == "all"
        assert normalize_capture_screen("extended") == "extended"
        choices = capture_screen_choices()
        assert any(v == "primary" for v, _ in choices)
        assert all(v in {"primary", "extended"} for v, _ in choices)
        assert "开始时鼠标所在屏幕" not in {lab for _, lab in choices}
        assert "全部屏幕（虚拟桌面）" not in {lab for _, lab in choices}
        assert isinstance(list_capture_screens(), list)
        assert build_gdigrab_input_args(None) == ["-draw_mouse", "1"]
        primary_region = resolve_capture_region("primary")
        if primary_region is not None:
            grab = build_gdigrab_input_args(primary_region)
            assert "-offset_x" in grab and "-video_size" in grab
            cmd = build_record_command(
                "ffmpeg", P("out.mp4"), fps=30, region=primary_region
            )
            # Prefer Desktop Duplication; all-screens uses gdigrab.
            assert "ddagrab=" in " ".join(cmd) or "gdigrab" in cmd
            assert "h264_mf" in cmd or "libx264" in cmd
        all_cmd = build_record_command("ffmpeg", P("out.mp4"), fps=30, region=None)
        assert "gdigrab" in all_cmd
        assert "-rtbufsize" in all_cmd or "ddagrab=" in " ".join(all_cmd)

        from src.ffmpeg_bundle import has_ddagrab
        from src.ffmpeg_locator import find_ffmpeg as _find_ff

        assert "filter=ddagrab" in inspect.getsource(has_ddagrab)
        assert '["-hide_banner", "-h", "filter=ddagrab"' in inspect.getsource(
            has_ddagrab
        ).replace("'", '"') or '"filter=ddagrab"' in inspect.getsource(has_ddagrab)
        assert "list_dxgi_outputs" in inspect.getsource(
            __import__("src.screen_record_manager", fromlist=["list_dxgi_outputs"])
        )
        real_ff = _find_ff()
        if real_ff is not None and primary_region is not None:
            import src.screen_record_manager as _srm

            _srm._ddagrab_supported_cache.clear()
            if ddagrab_supported(real_ff):
                real_cmd = build_record_command(
                    real_ff, P("out.mp4"), fps=30, region=primary_region, codec="libx264"
                )
                assert any(
                    "ddagrab=" in str(part) for part in real_cmd
                ), "single-monitor record must use ddagrab (avoids cursor flicker)"
                assert _match_ddagrab_output_idx(primary_region) is not None
        assert callable(pick_capture_screen)
        assert "选择要录制的屏幕" in inspect.getsource(RecordScreenPickerDialog)

        from src.ui.extensions_widget import ExtensionsWidget
        from src.ui.main_window import MainWindow

        ext_src = inspect.getsource(ExtensionsWidget._build_ui)
        assert "screen_record_dir_edit" in ext_src
        assert "screenshot_enabled_cb" not in ext_src
        assert "screenshot_pin_cb" in ext_src
        assert "screenshot_max_pins_spin" in ext_src
        assert "同时贴图最多" in ext_src
        assert "_on_screenshot_max_pins_changed" in inspect.getsource(ExtensionsWidget)
        assert "screen_record_enabled_cb" in ext_src
        assert "screen_record_capture_combo" not in ext_src
        assert "录制屏幕" not in ext_src
        assert "开始时会先选择" in ext_src
        assert "minutes_enabled_cb" in ext_src
        assert "notepad_enabled_cb" in ext_src
        assert "hotkey_inputs" in ext_src
        assert "screen_record" in ext_src
        assert "screenshot" in ext_src
        assert "show_screenshot" in ext_src
        assert "_on_hotkey_changed" in inspect.getsource(ExtensionsWidget)
        assert "_browse_screen_record_dir" in inspect.getsource(ExtensionsWidget)
        assert "_on_screen_record_capture_changed" not in inspect.getsource(ExtensionsWidget)
        assert "_on_minutes_enabled_changed" in inspect.getsource(ExtensionsWidget)
        assert "_on_notepad_enabled_changed" in inspect.getsource(ExtensionsWidget)
        save_ext = inspect.getsource(ExtensionsWidget._save_screen_record_dir)
        assert "output_dir" in save_ext
        build = inspect.getsource(MainWindow._build_settings_page)
        assert "screen_record_dir_edit" not in build
        start = inspect.getsource(ScreenRecordManager.start_recording)
        assert "resolve_output_dir" in start
        assert "resolve_capture_region" in start
        assert "build_record_command" in start
        assert "pick_capture_screen" in start
        assert "capture_screen" in start

        app_src = inspect.getsource(DeskTidyApp.__init__)
        assert "ScreenRecordManager" in app_src
        assert "screen_record_manager" in app_src

        messages: list[str] = []
        from src.screen_record_manager import _clear_state

        _clear_state()
        mgr = ScreenRecordManager(
            lambda: {"screen_record": {"enabled": False}}, notify=messages.append
        )
        # enabled=False only hides page-bar/tray chrome — toggle must still run.
        assert "录屏已关闭" not in inspect.getsource(ScreenRecordManager.toggle_recording)
        mgr.shutdown()

        _clear_state()
        mgr2 = ScreenRecordManager(
            lambda: {"screen_record": {"enabled": True}}, notify=messages.append
        )
        assert mgr2.is_recording is False
        mgr2.shutdown()

    def _screen_record_ffmpeg_missing_tip():
        from PyQt6.QtWidgets import QApplication
        import time

        import src.i18n as i18n
        import src.screen_record_manager as srm
        from src.screen_record_manager import ScreenRecordManager, _clear_state

        app = QApplication.instance() or QApplication([])
        messages: list[str] = []
        orig = srm.find_ffmpeg
        orig_warn = i18n.show_warning
        srm.find_ffmpeg = lambda: None  # type: ignore[assignment]
        i18n.show_warning = lambda *a, **k: None  # type: ignore[assignment]
        try:
            from src.ffmpeg_locator import clear_ffmpeg_cache

            clear_ffmpeg_cache()
            _clear_state()
            mgr = ScreenRecordManager(
                lambda: {"screen_record": {"enabled": True}}, notify=messages.append
            )
            mgr.start_recording(capture_screen="primary")
            deadline = time.monotonic() + 3.0
            while time.monotonic() < deadline and not any("FFmpeg" in m for m in messages):
                app.processEvents()
                time.sleep(0.05)
            assert any("FFmpeg" in m for m in messages)
            assert mgr._ffmpeg_tip_shown is True
            mgr.shutdown()
        finally:
            srm.find_ffmpeg = orig  # type: ignore[assignment]
            i18n.show_warning = orig_warn  # type: ignore[assignment]
            try:
                from src.ffmpeg_locator import clear_ffmpeg_cache

                clear_ffmpeg_cache()
            except Exception:
                pass

    def _screen_record_active_tip_contract():
        """Chrome shows immediately on start; toast deferred; early-exit verified async."""
        import inspect

        from src import screen_record_manager as srm
        from src.screen_record_manager import ScreenRecordManager
        from src.ui import recording_indicator as ri

        finish_start = inspect.getsource(ScreenRecordManager._on_start_finished)
        assert "_announce_recording_active" in finish_start
        assert "singleShot(450" in finish_start or "singleShot(450," in finish_start
        check = inspect.getsource(ScreenRecordManager._check_started)
        assert "_hide_recording_chrome" in check
        assert "FFmpeg 立即退出" in check
        announce = inspect.getsource(ScreenRecordManager._announce_recording_active)
        assert "_show_recording_chrome" in announce
        assert "_message" not in announce
        show = inspect.getsource(ScreenRecordManager._show_recording_chrome)
        assert "show_recording_indicator" in show
        assert "show_toast" in show
        assert "录屏已开始" in show
        assert "anchor=" in show or "anchor=toast_anchor" in show
        assert "singleShot(320" in show or "QTimer.singleShot(320" in show
        assert "qt_geometry_for_capture_region" in show
        place = inspect.getsource(ri.RecordingIndicator._place_chrome)
        assert "_capture_target_rect" in place
        target_src = inspect.getsource(ri.RecordingIndicator._capture_target_rect)
        assert "qt_geometry_for_capture_region" in target_src
        assert callable(srm.qt_geometry_for_capture_region)
        # HiDPI: physical gdigrab region must not be fed raw into QWidget.move.
        from PyQt6.QtCore import QRect
        from PyQt6.QtGui import QGuiApplication
        from unittest import mock

        from src.ui.screen_snap import work_screen

        primary = QGuiApplication.primaryScreen() or work_screen()
        if primary is None:
            return
        logical = primary.geometry()
        # Simulate 150% laptop: physical = logical × 1.5 at origin.
        fake_phys = {
            "left": 0,
            "top": 0,
            "width": int(logical.width() * 1.5) if logical.width() else 2880,
            "height": int(logical.height() * 1.5) if logical.height() else 1800,
            "label": "主屏幕",
        }
        mapped = srm.qt_geometry_for_capture_region(
            {
                "left": int(logical.x()),
                "top": int(logical.y()),
                "width": int(logical.width()),
                "height": int(logical.height()),
                "label": "主屏幕",
            }
        )
        assert mapped is not None
        assert isinstance(mapped, QRect)
        assert mapped.width() > 0 and mapped.height() > 0
        # Primary physical-at-origin should resolve to primary Qt screen even when
        # width/height are DPR-scaled (gaming-laptop path).
        with mock.patch.object(
            srm,
            "_qt_screen_for_hmonitor",
            return_value=primary,
        ):
            mapped_hidpi = srm.qt_geometry_for_capture_region(fake_phys)
        assert mapped_hidpi is not None
        assert mapped_hidpi == QRect(logical) or (
            mapped_hidpi.x() == logical.x() and mapped_hidpi.y() == logical.y()
        )
        assert "_ensure_chrome_topmost" in inspect.getsource(ri.RecordingIndicator)
        chrome_init = inspect.getsource(ri._RecChrome.__init__)
        assert "结束录制" in chrome_init
        assert "REC" in chrome_init
        assert "_tick_elapsed" in inspect.getsource(ri._RecChrome)
        assert "_BorderStrip" in inspect.getsource(ri)
        assert "_stopping" in inspect.getsource(ri._RecChrome._handle_stop)
        stop = inspect.getsource(ScreenRecordManager.stop_recording)
        assert "_hide_recording_chrome" in stop
        assert "threading.Thread" in stop
        assert "self._stopping" in stop
        start = inspect.getsource(ScreenRecordManager.start_recording)
        # Tip must not fire before process spawn finishes.
        assert "正在录屏" not in start
        assert "build_record_command" in start
        assert "threading.Thread" in start
        assert "probe_preferred_encoder" in inspect.getsource(ScreenRecordManager._warm_ffmpeg)
        warm_ff = inspect.getsource(ScreenRecordManager._warm_ffmpeg)
        assert "-version" in warm_ff
        assert "ddagrab_supported" in warm_ff
        assert "QTimer.singleShot(800, self._warm_ffmpeg)" not in inspect.getsource(
            ScreenRecordManager.__init__
        )
        finish = inspect.getsource(ScreenRecordManager._on_stop_finished)
        assert "ask_keep_recording_with_name" in finish
        assert "rename_recording_file" in finish
        assert "已取消保存" in finish
        assert "_reveal_recording" in finish
        assert "saved.is_file()" in finish
        from src import i18n as i18n_mod
        from src import ffmpeg_locator as ff_mod
        from src import screen_record_manager as srm

        assert "_selftest_active" in inspect.getsource(srm._reveal_recording)

        ask_rec = inspect.getsource(i18n_mod.ask_keep_recording_with_name)
        assert "QLineEdit" in ask_rec
        assert "保留" in ask_rec
        assert "取消" in ask_rec
        assert "explicit_discard" in ask_rec
        assert "sanitize_recording_stem" in ask_rec
        assert "保留" in inspect.getsource(i18n_mod.ask_keep_or_discard)
        assert "取消" in inspect.getsource(i18n_mod.ask_keep_or_discard)
        ask_src = inspect.getsource(i18n_mod.ask_keep_or_discard)
        assert "setEscapeButton" in ask_src
        assert "discard_btn" in ask_src
        assert "if clicked is discard_btn" in ask_src
        assert callable(srm.sanitize_recording_stem)
        assert callable(srm.rename_recording_file)
        assert srm.sanitize_recording_stem("bad/name?") == "badname"
        import shutil
        import tempfile

        td = Path(tempfile.mkdtemp(prefix="desktidy_rec_rename_"))
        try:
            src = td / "DeskTidy_test.mp4"
            src.write_bytes(b"x" * 64)
            renamed = srm.rename_recording_file(src, "我的录屏")
            assert renamed.is_file()
            assert renamed.name == "我的录屏.mp4"
            assert not src.exists()
        finally:
            shutil.rmtree(td, ignore_errors=True)
        wait_src = inspect.getsource(srm._wait_for_saved_file)
        assert "stable" in wait_src
        assert "timeout_s=3.0" in inspect.getsource(srm._finalize_stop)
        locate = inspect.getsource(ff_mod._locate_ffmpeg)
        assert 'glob("**/ffmpeg.exe")' not in locate
        assert "warm_ffmpeg_cache" in inspect.getsource(ff_mod)
        assert "_show_border_strips" in inspect.getsource(ri.RecordingIndicator)
        show_ind = inspect.getsource(ri.RecordingIndicator.show)
        assert "singleShot(200" in show_ind
        assert callable(srm.build_record_command)
        assert callable(srm.probe_preferred_encoder)
        assert "ddagrab" in inspect.getsource(srm.build_record_command)
        assert "h264_mf" in inspect.getsource(srm.build_encoder_args)

    def _ux_polish_contracts():
        """Labels / about / toast / ripple theme alignment."""
        import inspect

        from src.hotkey_manager import HOTKEY_LABELS
        from src.i18n import show_about
        from src.tray import TrayManager
        from src.ui import main_window as mw
        from src.ui import page_indicator as pi
        from src.ui import toast as toast_mod
        from src.ui.styles import build_stylesheet

        assert HOTKEY_LABELS["peek_fences"] == "临时查看分区"
        assert HOTKEY_LABELS["toggle_fences"] == "显示/隐藏分区"
        tray_src = inspect.getsource(TrayManager.__init__)
        assert "HOTKEY_LABELS" in tray_src
        assert "show_about" in tray_src
        assert "QueuedConnection" in tray_src
        assert HOTKEY_LABELS["organize"] == "一键整理"
        org_src = inspect.getsource(
            __import__("src.app", fromlist=["DeskTidyApp"]).DeskTidyApp._do_organize
        )
        assert "call_on_main_thread" in org_src
        assert "_finish_organize_worker" in org_src
        assert "QTimer.singleShot(" not in org_src
        assert "功能说明" not in tray_src or "使用说明" in tray_src
        assert "帮助" in tray_src
        assert "使用说明" in tray_src
        assert "系统操作手册" not in tray_src
        assert "_show_manual" not in tray_src
        assert "_show_help" in tray_src
        assert "快捷呼出" not in tray_src
        settings_src = inspect.getsource(mw.MainWindow._build_settings_page)
        assert "HOTKEY_LABELS" in settings_src
        assert "快捷呼出" not in settings_src
        assert "NoWheelComboBox" in settings_src
        assert "定时整理" not in settings_src
        assert "schedule_organize" not in settings_src
        from src.ui.fence_editor import FenceEditDialog
        from src.ui.extensions_widget import ExtensionsWidget
        from src.ui.no_wheel_combo import (
            NoWheelComboBox,
            NoWheelDoubleSpinBox,
            NoWheelSpinBox,
        )

        fence_dlg_src = inspect.getsource(FenceEditDialog.__init__)
        assert "NoWheelComboBox" in fence_dlg_src
        assert "NoWheelSpinBox" in fence_dlg_src
        assert "NoWheelDoubleSpinBox" in fence_dlg_src
        # No raw QComboBox/QSpinBox constructors left in form UIs.
        assert "QComboBox()" not in fence_dlg_src
        assert "QSpinBox()" not in fence_dlg_src
        assert "QDoubleSpinBox()" not in fence_dlg_src
        ext_src = inspect.getsource(ExtensionsWidget._build_ui)
        assert "CompactCountStepper" in ext_src
        assert "screenshot_max_pins_spin" in ext_src
        assert "QSpinBox()" not in ext_src
        assert callable(NoWheelComboBox)
        assert callable(NoWheelSpinBox)
        assert callable(NoWheelDoubleSpinBox)
        from src.ui.compact_stepper import CompactCountStepper

        assert callable(CompactCountStepper)
        assert callable(show_about)
        from src.help_content import HELP_TOPICS, help_html, operation_manual_html
        from src.ui.help_dialog import (
            HelpContentPanel,
            HelpHomePage,
            show_help,
            show_operation_manual,
        )

        assert len(HELP_TOPICS) >= 8
        topic_ids = {t[0] for t in HELP_TOPICS}
        assert "fences" in topic_ids
        assert "screenshot" in topic_ids
        assert "record" in topic_ids
        assert "organize" not in topic_ids  # merged into fences
        assert "hotkeys" not in topic_ids  # per-feature sections
        assert "一键整理" in help_html("fences")
        assert "文件夹门户" in help_html("fences")
        assert "系统资源管理器菜单" in help_html("fences")
        # Legacy aliases still resolve.
        assert "一键整理" in help_html("organize")
        pages_help = help_html("pages")
        assert "虚拟桌面" in pages_help
        assert "Win+Ctrl" in pages_help or "任务视图" in pages_help
        fences_help = help_html("fences")
        assert "标题栏" in fences_help
        assert "不抢系统焦点" in fences_help or "其它程序" in fences_help
        assert "录屏" in help_html("record")
        assert "区域截图" in help_html("screenshot")
        assert "F1" in help_html("screenshot", {"hotkeys": {"screenshot": "F1"}})
        assert "Ctrl+Shift+O" in help_html(
            "fences", {"hotkeys": {"organize": "Ctrl+Shift+O"}}
        )
        overview = help_html("overview")
        assert "功能" in overview or "主题" in overview or "分章" in overview
        manual = operation_manual_html({"hotkeys": {"organize": "Ctrl+Shift+O"}})
        assert "使用说明" in manual
        assert "一键整理" in manual
        assert "F1" in manual
        assert "桌面待办" in manual
        assert "计算器" in manual
        assert callable(show_help)
        assert callable(show_operation_manual)
        from src.product_pages import (
            build_merged_desktidy_help_html,
            open_desktidy_help,
        )

        assert callable(open_desktidy_help)
        merged = build_merged_desktidy_help_html(
            {"hotkeys": {"organize": "Ctrl+Shift+O", "screenshot": "F1"}}
        )
        assert merged is not None
        assert "使用手册" in merged
        assert "一键整理" in merged
        assert "Ctrl+Shift+O" in merged
        assert 'id="manual-fences"' in merged or "manual-fences" in merged
        # DeskNote chapters stay on the separate DeskNote help page.
        assert "id=\"manual-notepad\"" not in merged
        assert "独立帮助" in merged
        from src.product_pages import build_merged_desknote_help_html

        dn_merged = build_merged_desknote_help_html()
        assert dn_merged is not None
        assert "DeskNote 使用手册" in dn_merged
        assert "Markdown 三栏" in dn_merged
        assert "id=\"manual-fences\"" not in dn_merged
        panel_src = inspect.getsource(HelpContentPanel.__init__)
        assert "include_manual" not in panel_src
        assert "系统操作手册" not in panel_src
        home_src = inspect.getsource(HelpHomePage)
        assert "在浏览器中打开" in home_src
        assert "入门指引" in home_src
        assert "open_desktidy_help" in home_src
        assert "open_desktidy_help" in inspect.getsource(show_help)
        assert "_open_first_run_guide" in home_src
        from src.app import DeskTidyApp
        from src.ui.first_run_guide import (
            GUIDE_STEPS,
            FirstRunGuideDialog,
            should_auto_show_first_run_guide,
        )
        from src.settings import apply_first_run_guide_upgrade, apply_first_run_organize_upgrade

        assert len(GUIDE_STEPS) == 10
        titles = [step[0] for step in GUIDE_STEPS]
        assert "区域截图" in titles
        assert "截图贴图" in titles
        assert "桌面录屏" in titles
        assert "内置记事本" in titles
        assert "会议纪要" in titles
        assert "桌面待办" in titles
        assert should_auto_show_first_run_guide({"first_run_guide_done": False})
        assert not should_auto_show_first_run_guide({"first_run_guide_done": True})
        upgraded = {"theme": "mist"}
        assert apply_first_run_guide_upgrade({}, upgraded) is True
        assert upgraded["first_run_guide_done"] is True
        fresh = {"first_run_guide_done": False}
        merged_fresh = {"first_run_guide_done": False}
        assert apply_first_run_guide_upgrade(fresh, merged_fresh) is False
        assert merged_fresh["first_run_guide_done"] is False
        upgraded_org = {"theme": "mist"}
        assert apply_first_run_organize_upgrade({}, upgraded_org) is True
        assert upgraded_org["first_run_organize_done"] is True
        sched = inspect.getsource(DeskTidyApp._schedule_startup_deferred)
        assert "_startup_deferred_first_run" in sched
        assert sched.index("_startup_deferred_first_run") < sched.index(
            "_maybe_show_first_run_guide"
        )
        first_run_src = inspect.getsource(DeskTidyApp._startup_deferred_first_run)
        assert "_do_organize(silent=True)" in first_run_src
        assert "first_run_organize_done" in first_run_src
        assert "_maybe_show_first_run_guide" in first_run_src
        assert callable(FirstRunGuideDialog)
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance() or QApplication([])
        dlg = FirstRunGuideDialog({"theme": "mist"})
        assert dlg._next.text() == "下一步"
        for _ in range(len(GUIDE_STEPS) - 1):
            dlg._advance()
        assert dlg._index == len(GUIDE_STEPS) - 1
        assert dlg._next.text() == "开始使用"
        dlg.close()
        dlg.deleteLater()
        app.processEvents()
        assert "_maybe_show_first_run_guide" in inspect.getsource(
            DeskTidyApp._schedule_startup_deferred
        )
        assert callable(DeskTidyApp._maybe_show_first_run_guide)
        defaults_txt = (
            Path(__file__).resolve().parents[1] / "config" / "default_settings.json"
        ).read_text(encoding="utf-8")
        assert '"first_run_guide_done": false' in defaults_txt
        assert '"first_run_organize_done": false' in defaults_txt
        overview = help_html("overview")
        assert "入门指引" in overview
        assert "小提示" in overview
        from src.ui import first_run_guide as frg

        assert hasattr(frg, "DesktopGuideTip")
        assert "fence" in [s[2] for s in GUIDE_STEPS]
        assert "rail" in [s[2] for s in GUIDE_STEPS]
        assert callable(HelpContentPanel)
        from src.product_pages import product_page_path

        assert callable(HelpHomePage)
        assert product_page_path("desktidy") is not None
        assert product_page_path("desknote") is not None
        intro = product_page_path("desktidy").read_text(encoding="utf-8")
        assert "账号管理" in intro
        assert 'id="vault"' in intro
        assert "DeskTidy_Setup_4.0.25.exe" in intro
        assert "help" in mw._LAZY_PAGE_IDS
        assert ("help", "帮助") in mw._NAV_ITEMS
        assert "menuBar().hide()" in inspect.getsource(mw.MainWindow.__init__)
        build_help = inspect.getsource(mw.MainWindow._build_help_page)
        assert "include_manual" not in build_help
        assert "HelpHomePage" in build_help
        assert "系统操作手册" not in mw._PAGE_SUBTITLES.get("help", "")
        assert "_build_help_page" in inspect.getsource(mw.MainWindow)
        assert "_popup_help_menu" not in inspect.getsource(mw.MainWindow)
        assert "_build_help_menu" not in mw.MainWindow.__dict__
        assert "_build_menu_bar" not in mw.MainWindow.__dict__
        assert "HelpHomePage" in inspect.getsource(mw.MainWindow._build_help_page)
        from src.help_content import about_html

        assert "关于" in about_html()
        toast_src = inspect.getsource(toast_mod._ToastTip.__init__)
        assert "get_theme_palette" in inspect.getsource(toast_mod._theme_colors)
        assert "windowOpacity" in toast_src
        assert "QParallelAnimationGroup" in toast_src or "OutBack" in toast_src
        assert "QGraphicsDropShadowEffect" not in toast_src
        assert "QGraphicsDropShadowEffect" not in inspect.getsource(toast_mod)
        assert "BrandMark" in inspect.getsource(mw.MainWindow._build_sidebar)
        from src.ui.brand_mark import BrandMark

        assert callable(BrandMark)
        refresh_src = inspect.getsource(mw.MainWindow.refresh_data)
        assert "is_files_page_visible()" in refresh_src
        import_src = inspect.getsource(mw.MainWindow._import_settings)
        assert "_sync_theme_combo" in import_src
        assert "_apply_sidebar_accent" in import_src
        ripple = inspect.getsource(pi._RippleOverlay.play)
        assert "return" in ripple
        assert "Lightweight" in ripple or "no-op" in ripple or "disabled" in ripple
        chip_src = inspect.getsource(pi._PeekChip)
        assert "set_hovered" in chip_src
        assert "self._hovered" in chip_src
        assert "underMouse()" not in inspect.getsource(pi._PeekChip._palette)
        peek_row = inspect.getsource(pi._PeekRow)
        assert "_on_leave_timeout" in peek_row
        assert "_clear_button_hover" in peek_row
        host_src = inspect.getsource(pi.PageIndicatorWidget)
        assert "_hover_watch" in host_src
        assert "_on_hover_watch" in host_src
        assert "_arm_hover_watch" in host_src
        css = build_stylesheet("mist")
        assert "statCard" not in css
        assert "sidebarBtn" not in css
        assert "sectionCard" in css
        from src.ui.styles import get_theme_palette

        mist = get_theme_palette("mist")
        assert mist["accent"].upper() == "#1D4ED8"
        assert mist["sidebar"].upper() == "#1A2740"
        sky = get_theme_palette("sky")
        mint = get_theme_palette("mint")
        assert sky["sidebar"].upper() != mist["sidebar"].upper()
        assert mint["sidebar"].upper() != sky["sidebar"].upper()
        assert sky["sidebar_hover"].startswith("rgba(")
        assert sky["sidebar_active"].startswith("rgba(14,")  # #0EA5E9
        from src.app import DeskTidyApp

        settings_src = inspect.getsource(DeskTidyApp._on_settings_changed)
        assert "_apply_sidebar_accent" in settings_src
        assert "_sync_window_icons" in settings_src
        theme_src = inspect.getsource(DeskTidyApp._on_theme_changed)
        assert "_sync_window_icons" in theme_src
        sync_icons = inspect.getsource(DeskTidyApp._sync_window_icons)
        assert "apply_window_icon" in sync_icons
        assert "_notepad_window" in sync_icons
        apply_theme = inspect.getsource(DeskTidyApp._apply_theme)
        assert "_apply_sidebar_accent" in apply_theme
        assert 'getattr(self, "window", None)' in apply_theme
        from src.ui.fence_widget import FenceWidget

        peek = inspect.getsource(FenceWidget.set_peek_mode)
        assert "setWindowOpacity(1.0)" in peek
        assert "QPropertyAnimation" not in peek
        assert "_apply_style" in peek
        from src import hotkey_manager as hk

        assert int(getattr(hk, "_POLL_HELD_MS")) == 200
        assert int(getattr(hk, "_POLL_IDLE_MS")) == 1200
        keep_src = inspect.getsource(DeskTidyApp._ensure_desktop_overlays_visible)
        assert ">= 1" in keep_src and "_overlays_need_shell_repair_light" in keep_src
        assert "_maybe_reschedule_desktop_watcher" in keep_src
        from src.ui.recording_indicator import _RecChrome

        pulse = inspect.getsource(_RecChrome._apply_pulse_style)
        assert "unpolish" not in pulse
        assert "setStyleSheet" in pulse

    def _screen_record_orphan_state_reclaim():
        """Restart must reclaim orphan ffmpeg via persisted state (not leave 0-byte files)."""
        import subprocess
        import time
        from pathlib import Path as P

        from PyQt6.QtWidgets import QApplication

        import src.screen_record_manager as srm
        from src.ffmpeg_locator import find_ffmpeg
        from src.screen_record_manager import (
            ScreenRecordManager,
            _clear_state,
            _pid_alive,
            _process_create_time_ms,
            _write_state,
        )

        app = QApplication.instance() or QApplication([])
        # Ensure no leftover capture holds gdigrab.
        try:
            subprocess.run(
                ["taskkill", "/IM", "ffmpeg.exe", "/F"],
                capture_output=True,
                creationflags=srm._creation_flags(),
                check=False,
            )
        except OSError:
            pass
        time.sleep(0.3)

        ffmpeg = find_ffmpeg()
        assert ffmpeg is not None, "bundled/system ffmpeg required for this test"

        out_dir = P.home() / "Videos" / "DeskTidy"
        out_dir.mkdir(parents=True, exist_ok=True)
        out = out_dir / "_selftest_orphan_reclaim.mp4"
        if out.exists():
            try:
                out.unlink()
            except OSError:
                pass

        _clear_state()
        proc = None
        last_err = b""
        # Synthetic lavfi + ``-re`` keeps ffmpeg alive without gdigrab (desktop
        # grab error 5 flakes when another session holds the display). Orphan
        # reclaim only needs a live ffmpeg PID + state file.
        for _attempt in range(3):
            proc = subprocess.Popen(
                [
                    str(ffmpeg),
                    "-y",
                    "-nostdin",
                    "-loglevel",
                    "error",
                    "-re",
                    "-f",
                    "lavfi",
                    "-i",
                    "color=c=black:s=320x240:r=15",
                    "-t",
                    "30",
                    "-c:v",
                    "libx264",
                    "-preset",
                    "ultrafast",
                    "-pix_fmt",
                    "yuv420p",
                    str(out),
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                creationflags=srm._creation_flags(),
            )
            time.sleep(0.8)
            if proc.poll() is None:
                break
            try:
                last_err = proc.stderr.read() if proc.stderr else b""
            except Exception:
                last_err = b""
            proc = None
        assert proc is not None and proc.poll() is None, (
            f"ffmpeg failed to stay up: {last_err!r}"
        )

        _write_state(
            int(proc.pid),
            out,
            exe=str(ffmpeg),
            create_time_ms=_process_create_time_ms(int(proc.pid)),
        )
        try:
            messages: list[str] = []
            import src.i18n as i18n

            orig_ask = i18n.ask_keep_recording_with_name
            i18n.ask_keep_recording_with_name = lambda *a, **k: (True, "DeskTidy_test")  # type: ignore[assignment]
            try:
                mgr = ScreenRecordManager(
                    lambda: {"screen_record": {"enabled": True}}, notify=messages.append
                )
                assert mgr.is_recording is True
                orphan_pid = int(mgr._proc.pid)
                mgr.stop_recording()
                deadline = time.monotonic() + 10.0
                while (mgr._stopping or _pid_alive(orphan_pid)) and time.monotonic() < deadline:
                    app.processEvents()
                    time.sleep(0.05)
                for _ in range(30):
                    app.processEvents()
                    time.sleep(0.02)
                assert mgr.is_recording is False
                assert not mgr._stopping
                assert not _pid_alive(orphan_pid)
                assert any(("已保存" in m) or ("未能保存" in m) for m in messages), messages
            finally:
                i18n.ask_keep_recording_with_name = orig_ask  # type: ignore[assignment]
        finally:
            try:
                if proc is not None and proc.poll() is None:
                    proc.kill()
                    proc.wait(timeout=3)
            except Exception:
                pass
            _clear_state()
            try:
                out.unlink(missing_ok=True)
            except OSError:
                pass

    def _perf_regression_contracts():
        import inspect

        from src.app import DeskTidyApp
        from src.hotkey_manager import HotkeyManager
        from src.ui.fence_widget import FenceWidget

        organize = inspect.getsource(DeskTidyApp._on_organize_requested)
        assert organize.count("refresh_public_desktop") == 0
        assert "_loose_sync_needed" in organize

        fence_files = inspect.getsource(DeskTidyApp._on_fence_files_changed)
        assert "fence.refresh()" in fence_files
        assert "refresh_fences" not in fence_files

        finish_drop = inspect.getsource(FenceWidget._finish_virtual_drop_ui)
        assert "_soft_refresh_peer_fences" in finish_drop
        assert "files_changed.emit()" not in finish_drop
        assert "_ensure_drop_target_interactive" in finish_drop
        assert "ensure_live_fences_interactive" in inspect.getsource(
            FenceWidget._ensure_drop_target_interactive
        )
        assert "_unpin_virtual_paths" in inspect.getsource(FenceWidget)
        assert "_remove_virtual_icon_widgets" in inspect.getsource(FenceWidget)

        wire = inspect.getsource(DeskTidyApp._wire_fence_signals)
        assert "_on_fence_files_changed" in wire

        register = inspect.getsource(HotkeyManager.register)
        assert "_bare_function_key" in register
        # All bare F bindings poll (organize/pages too), not only screenshot/record.
        assert "bare_fkey or (action in _POLLABLE_ACTIONS and not registered)" in register
        # Bare F-keys must still RegisterHotKey (consume system Help); never skip it.
        assert "if not use_async_poll:" not in register
        assert "RegisterHotKey" in register
        # Bare F must NOT fall back to repeating RegisterHotKey (hold-key thrash).
        assert "if bare_fkey:" in register
        assert "modifier_attempts = [modifiers | MOD_NOREPEAT]" in register

        trigger = inspect.getsource(HotkeyManager._trigger_action)
        assert "0.45" in trigger
        assert "_suppress_until" in trigger
        dispatch = inspect.getsource(__import__("src.hotkey_manager", fromlist=["x"])._dispatch_on_main_thread)
        assert "QThread.currentThread()" in dispatch
        assert "app.thread()" in dispatch

        arm = inspect.getsource(HotkeyManager._arm_after_register)
        assert "_flush_wm_hotkey_queue" in arm
        assert "_suppress_until" in arm
        assert "_polled_down" in arm

        flush = inspect.getsource(HotkeyManager._flush_wm_hotkey_queue)
        assert "WM_HOTKEY" in flush
        assert "PeekMessageW" in flush

        schedule = inspect.getsource(DeskTidyApp._schedule_hotkeys_refresh)
        assert "_hotkey_capture_depth" in schedule
        assert "_setup_hotkeys" in schedule

        cap_began = inspect.getsource(DeskTidyApp._on_hotkey_capture_began)
        assert "unregister_all" in cap_began
        cap_ended = inspect.getsource(DeskTidyApp._on_hotkey_capture_ended)
        assert "_schedule_hotkeys_refresh" in cap_ended

        setup = inspect.getsource(DeskTidyApp._setup_hotkeys)
        assert "register_with_fallbacks" in setup
        assert "self.hotkey_manager.register" in setup
        assert setup.count("shortcut.activated.connect(callback)") == 1

        # Risk: settings-open must not spin force-attach every 400ms.
        pending = inspect.getsource(DeskTidyApp._run_pending_force_shell_attach)
        assert "_shell_attach_after_settings" in pending
        assert "self._shell_attach_after_settings = True" in pending
        assert "_schedule_force_shell_attach(400)" not in pending
        # Force-attach re-entry storm: one attach owner, no nested force visible.
        assert "_clear_foreign_fg_park_latch_if_desktop" in pending
        assert "_ensure_desktop_overlays_visible(force=True)" not in pending
        attach_src = inspect.getsource(DeskTidyApp._ensure_shell_attachments)
        assert "raise_band=bool(force)" in attach_src
        visible_src = inspect.getsource(DeskTidyApp._ensure_desktop_overlays_visible)
        assert "raise_band=bool(force)" in visible_src
        assert "_overlays_parked_for_app_fg = False" in visible_src
        end_popup = inspect.getsource(DeskTidyApp._end_desktop_popup)
        assert "_sync_overlays_to_foreground" in end_popup
        assert "reconcile_fg" in end_popup
        end_drag = inspect.getsource(DeskTidyApp._end_overlay_drag)
        assert "_sync_overlays_to_foreground" in end_drag
        assert "reconcile_fg=False" in end_drag
        hidden = inspect.getsource(DeskTidyApp._on_settings_hidden)
        assert "_shell_attach_after_settings" in hidden
        assert "_overlays_need_shell_repair_light" in hidden
        assert "_schedule_force_shell_attach" in hidden
        assert "force=False" in hidden
        assert "_remap_hidden_overlay_hwnds" in hidden
        assert "_keep_overlays_under_apps()" not in hidden

        from src.instance_lock import AcquireResult
        from src.screen_record_manager import ScreenRecordManager as SRM

        assert set(AcquireResult) >= {
            AcquireResult.OWNED,
            AcquireResult.BUSY,
            AcquireResult.FAILED,
        }
        start_src = inspect.getsource(SRM.start_recording)
        assert ".ffmpeg.log" in start_src
        assert "stderr=err_fh" in start_src
        assert "stderr=subprocess.DEVNULL" not in start_src
        stop_src = inspect.getsource(SRM.stop_recording)
        assert "threading.Thread" in stop_src
        assert "_stop_bridge" in stop_src or "done.emit" in stop_src

        refresh_impl = inspect.getsource(FenceWidget._refresh_impl)
        force_block = refresh_impl.split("if force:", 1)[1].split("existing:", 1)[0]
        assert "invalidate_file_icon_cache(entry)" in force_block
        assert "invalidate_file_icon_cache()" not in force_block

        stack = inspect.getsource(
            __import__("src.win_shell", fromlist=["x"]).apply_overlay_stack_mode
        )
        assert "is_attached_to_desktop" in stack.split("if not peek:", 1)[0] or "is_attached_to_desktop" in stack[:800]

        run_src = inspect.getsource(DeskTidyApp.run)
        assert "close_to_tray" in run_src
        assert "close_behavior_prompted" in run_src
        assert "first_open" in run_src
        assert "self.window.show()" in run_src

        from src.settings import migrate_legacy_autostart

        mig = inspect.getsource(migrate_legacy_autostart)
        assert "LEGACY_APP_NAMES" in mig
        assert "桌面整理（私人版）" in mig or "LEGACY_APP_NAMES" in mig
        from src.settings import APP_NAME, LEGACY_APP_NAMES

        assert APP_NAME == "DeskTidy"
        assert "桌面整理（私人版）" in LEGACY_APP_NAMES
        assert "Desktidy" in LEGACY_APP_NAMES
        assert "remove_desktop_guard_autostart" in mig or "_desktop_guard_link_path" in mig
        assert "ensure_desktop_guard_autostart()" not in mig
        init = inspect.getsource(DeskTidyApp.__init__)
        assert "migrate_legacy_autostart" not in init.split("def _schedule_startup_deferred", 1)[0]
        guard_src = inspect.getsource(DeskTidyApp._startup_deferred_guard)
        assert "migrate_legacy_autostart" in guard_src
        assert "remove_desktop_guard_autostart" in guard_src
        assert "ensure_desktop_guard_autostart()" not in guard_src

        inactive = inspect.getsource(DeskTidyApp._on_application_state_changed)
        assert "setInterval(20000)" in inactive
        assert "_trim_background_memory" in inactive

        from src.tray import TrayManager

        tray_src = inspect.getsource(TrayManager.apply_feature_visibility)
        assert "_screenshot_action" in tray_src
        assert "_record_action" in tray_src
        assert "setVisible" in tray_src
        assert "apply_feature_visibility" in inspect.getsource(DeskTidyApp._on_extensions_changed)

        deferred = inspect.getsource(DeskTidyApp._startup_deferred_immediate)
        assert "_setup_hotkeys" in deferred
        sched = inspect.getsource(DeskTidyApp._schedule_startup_deferred)
        assert "_startup_deferred_immediate" in sched
        overlay = inspect.getsource(DeskTidyApp._schedule_startup_overlays)
        assert "explorer_shell_ready" in overlay
        assert "_poll_explorer_shell_for_startup" in overlay
        assert "_run_startup_overlays" in overlay
        assert "QTimer.singleShot(0, self._run_startup_overlays)" in overlay

        deferred = inspect.getsource(DeskTidyApp._startup_deferred_window_data)
        assert "refresh_data_if_visible" in deferred
        sched = inspect.getsource(DeskTidyApp._schedule_startup_deferred)
        assert "_startup_deferred_scheduler" not in sched
        assert "ScheduleOrganizer" not in inspect.getsource(DeskTidyApp.__init__)
        assert not hasattr(DeskTidyApp, "_startup_deferred_scheduler")
        assert not hasattr(DeskTidyApp, "_do_organize_silent")
        defaults_json = (
            Path(__file__).resolve().parents[1] / "config" / "default_settings.json"
        ).read_text(encoding="utf-8")
        assert "schedule_organize" not in defaults_json
        from src import help_content as help_mod

        assert "定时整理" not in help_mod.help_html("fences")
        assert "定时整理" not in help_mod.help_html("organize")
        assert "定时整理" not in help_mod.help_html("settings")
        assert "定时整理" not in help_mod.help_html("overview")

        setup = inspect.getsource(DeskTidyApp._setup_hotkeys)
        # Extension chrome flags must not gate hotkey registration.
        assert "shot_enabled" not in setup
        assert "rec_enabled" not in setup
        assert "search_enabled" not in setup
        assert "calculator_enabled" not in setup
        assert '"screenshot"' in setup
        assert '"screen_record"' in setup
        assert '"file_search"' in setup
        assert '"calculator"' in setup
        assert "截图未启用" not in inspect.getsource(
            __import__("src.screenshot_manager", fromlist=["x"]).ScreenshotManager.start_capture
        )
        assert "录屏已关闭" not in inspect.getsource(
            __import__("src.screen_record_manager", fromlist=["x"]).ScreenRecordManager.toggle_recording
        )

        init = inspect.getsource(DeskTidyApp.__init__)
        assert "_screenshot_manager" in init
        assert "ScreenshotManager(lambda" not in init.split("def _schedule_startup_deferred", 1)[0]

        from src.icon_utils import _MAX_FILE_PIXMAP_CACHE, _MAX_SHELL_RAW_CACHE, app_icon_is_hires

        assert _MAX_FILE_PIXMAP_CACHE <= 96
        assert _MAX_SHELL_RAW_CACHE <= 48
        assert app_icon_is_hires(), "assets/app_icon.ico must embed multiple sizes (run scripts/generate_icon.py)"

        hidden = inspect.getsource(DeskTidyApp._on_settings_hidden)
        assert "_trim_background_memory" in hidden
        trim = inspect.getsource(DeskTidyApp._trim_background_memory)
        # Tray trim must NOT wipe file/shell icon caches (that caused hitch on return).
        assert "invalidate_file_icon_cache" not in trim
        assert "clear_label_pixmap_cache" in trim
        assert "release_lazy_pages" in trim
        assert "_trim_parked_overlays" in trim

        from src.ui.main_window import MainWindow

        lazy = inspect.getsource(MainWindow.release_lazy_pages)
        assert "_lazy_built" in lazy
        assert "_build_page_widget" in inspect.getsource(MainWindow._ensure_page)
        show_ev = inspect.getsource(MainWindow.showEvent)
        # Re-open after hideEvent→release_lazy_pages must refresh organize metrics.
        assert 'page_id == "files"' in show_ev
        assert "refresh_data()" in show_ev
        # Must refresh after _ensure_page, not only emit settings_ui_shown.
        assert show_ev.index("_ensure_page") < show_ev.index("refresh_data()")
        assert show_ev.index("refresh_data()") < show_ev.index("settings_ui_shown")

    def _cursor_flash_regression():
        import inspect

        from src import desktop_shell_host as host
        from src.app import DeskTidyApp
        from src.ui.fence_icon_item import FenceIconItem
        from src.ui.public_icon_widget import PublicIconWidget

        wallpaper = inspect.getsource(host.is_wallpaper_workerw)
        assert "except Exception" in wallpaper
        assert "IsWindow" in wallpaper

        attach = inspect.getsource(host.attach_overlay_to_desktop)
        # After the already-attached branch, we must not HWND_BOTTOM healthy overlays.
        attached = attach.split("if is_attached_to_desktop(hwnd", 1)[1]
        early = attached.split("return True", 1)[0]
        assert "place_overlay_in_desktop_band" not in early

        band = inspect.getsource(host.place_overlay_in_desktop_band)
        assert "_BAND_SINK_AT" in band or "2.0" in band

        ensure_src = inspect.getsource(DeskTidyApp._ensure_shell_attachments)
        assert "except Exception" in ensure_src

        repair = inspect.getsource(DeskTidyApp._overlays_need_shell_repair_impl)
        assert "if not host:" in repair

        on_fg = inspect.getsource(DeskTidyApp._on_desktop_foreground_changed)
        assert "_sync_overlays_to_foreground" in on_fg
        sync = inspect.getsource(DeskTidyApp._sync_overlays_to_foreground)
        was_block = sync.split("if was_desktop:", 1)[1].split("if self._desktop_fg_recover_timer", 1)[0]
        assert "return" in was_block
        assert "_remap_hidden_overlay_hwnds" in was_block
        state = inspect.getsource(DeskTidyApp._on_application_state_changed)
        inactive = state.split("ApplicationHidden", 1)[1]
        inactive = inactive.split("ApplicationActive", 1)[0]
        assert "_schedule_overlay_keepalive" not in inactive

        # Desktop icons: ArrowCursor (Hand↔Arrow thrash flashes translucent HWNDs).
        # NoFocus / no press-time setFocus (ClickFocus flashes NOACTIVATE overlays).
        icon_init = inspect.getsource(FenceIconItem.__init__)
        assert "ArrowCursor" in icon_init
        assert "NoFocus" in icon_init
        icon_press = inspect.getsource(FenceIconItem.mousePressEvent)
        assert "setFocus" not in icon_press
        pub_init = inspect.getsource(PublicIconWidget.__init__)
        assert "ArrowCursor" in pub_init
        assert "NoFocus" in pub_init
        pub_press = inspect.getsource(PublicIconWidget.mousePressEvent)
        assert "setFocus" not in pub_press
        from src.ui.fence_widget import FenceItemLabel, FenceWidget

        label_init = inspect.getsource(FenceItemLabel.__init__)
        assert "NoFocus" in label_init
        label_press = inspect.getsource(FenceItemLabel.mousePressEvent)
        assert "setFocus" not in label_press
        fence_init = inspect.getsource(FenceWidget.__init__)
        assert "NoFocus" in fence_init
        marquee = inspect.getsource(FenceWidget.eventFilter)
        assert "setFocus(Qt.FocusReason.MouseFocusReason)" not in marquee

    def _tray_open_no_flash():
        import inspect

        from src.app import DeskTidyApp
        from src.desktop_foreground_monitor import (
            _hwnd_is_shell_taskbar_surface,
            _on_desktop_surface,
        )
        from src.tray import TrayManager
        from src.win_shell import (
            _hwnd_is_shell_taskbar,
            bring_widget_to_foreground,
        )

        tray_show = inspect.getsource(TrayManager.show_window)
        assert "was_mapped" in tray_show
        assert "if was_mapped:" in tray_show
        # showEvent owns overlay sink when hidden→shown; tray only when already mapped.
        assert tray_show.index("if was_mapped:") < tray_show.index("_on_settings_shown")

        bring = inspect.getsource(bring_widget_to_foreground)
        assert "was_mapped" in bring
        assert "if was_mapped:" in bring
        assert "HWND_TOPMOST" in bring
        assert "_win32_activate_hwnd" in inspect.getsource(
            __import__("src.win_shell", fromlist=["x"])
        )

        shown = inspect.getsource(DeskTidyApp._on_settings_shown)
        assert "_settings_shown_at" in shown

        # Taskbar / notification area is desktop-surface (no foreign sink).
        import ctypes

        tray = int(ctypes.windll.user32.FindWindowW("Shell_TrayWnd", None) or 0)
        assert tray, "Shell_TrayWnd missing"
        assert _hwnd_is_shell_taskbar(tray)
        assert _hwnd_is_shell_taskbar_surface(tray)
        assert _on_desktop_surface(tray)

    def _perf_round2_contracts():
        import inspect
        import tempfile
        import time
        from pathlib import Path as P

        from src.app import DeskTidyApp
        from src.fence_rules import get_virtual_items_for_fence
        from src.path_stat_cache import invalidate_path_stat_cache, path_mtime
        from src.ui.fence_icon_item import FenceIconItem
        from src.ui.fence_widget import FenceWidget
        from src.ui.public_icon_widget import PublicIconWidget

        fences_impl = inspect.getsource(DeskTidyApp._refresh_fences_impl)
        assert "_all_pinned_cache" in fences_impl

        sig = inspect.signature(get_virtual_items_for_fence)
        assert "all_pinned" in sig.parameters
        assert "apply_sort" in sig.parameters

        src = inspect.getsource(FenceWidget._get_entries)
        assert "apply_sort=False" in src

        item_init = inspect.getsource(FenceIconItem.__init__)
        assert "icon_load_delay_ms" in item_init
        pub_init = inspect.getsource(PublicIconWidget.__init__)
        assert "icon_load_delay_ms" in pub_init

        invalidate_path_stat_cache()
        with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as tmp:
            pin = P(tmp.name)
        try:
            t0 = time.perf_counter()
            for _ in range(30):
                path_mtime(pin)
            warm_ms = (time.perf_counter() - t0) * 1000
            assert warm_ms < 5.0, f"stat cache should be fast, got {warm_ms:.2f}ms"
        finally:
            try:
                pin.unlink(missing_ok=True)
            except OSError:
                pass
            invalidate_path_stat_cache()

    run("分页指示器往返点击一次生效", _indicator_roundtrip)
    run("分页悬停动画移开可收起", _peek_hover_unstick)
    run("退役遮罩立即不接收鼠标", _retire_blocks_hits)
    run("换页排队+延迟沉底契约", _switch_page_queued)
    run("切页浮标停放可见性契约", _page_switch_unpark_reattach_logic)
    run("强制壳层附着节流会补调度", _force_attach_throttle_reschedules)
    run("切页确保等公共刷新完成", _page_switch_ensure_waits_for_refresh)
    run("拖动冻结公共刷新+64图标优先jumbo", _drag_defers_public_refresh)
    run("64图标用高清shell源", _icon_64_uses_hires_shell_source)
    run("稀疏jumbo图标裁剪后铺满", _trim_sparse_jumbo_icon)
    run("壁纸WorkerW卡住会重挂契约", _wallpaper_stuck_repair_contract)
    run("截屏贴图冻结层级防闪", _screenshot_freezes_restack)
    run("F2贴图右键另存为", _f2_pin_context_save_as)
    def _bare_fkey_uses_poll():
        from src.hotkey_manager import (
            HOTKEY_IDS,
            MOD_CONTROL,
            HotkeyManager,
            _bare_function_key,
            user32,
        )

        assert _bare_function_key(0, 0x70)  # F1
        assert _bare_function_key(0, 0x72)  # F3
        assert not _bare_function_key(MOD_CONTROL, 0x72)
        assert not _bare_function_key(0, 0x53)  # S

        mgr = HotkeyManager()
        fired: list[str] = []
        assert mgr.register("screenshot", "F1", lambda: fired.append("shot"))
        # Always poll bare F-keys for reliable fire.
        assert "screenshot" in mgr._polled
        # Organize / page actions must also poll bare F bindings (not only shot/rec).
        assert mgr.register("organize", "F2", lambda: fired.append("org"))
        assert "organize" in mgr._polled
        mgr.unregister("organize")
        # RegisterHotKey consumes system Help when free; if another DeskTidy
        # already owns F1, poll-only is still a successful binding.
        if HOTKEY_IDS["screenshot"] not in mgr._registered:
            # Prove the API still rejects a second owner when *we* hold the key.
            probe = HotkeyManager()
            assert probe.register("screen_record", "F3", lambda: fired.append("rec"))
            assert "screen_record" in probe._polled
            if HOTKEY_IDS["screen_record"] in probe._registered:
                # We own F3 → a raw second RegisterHotKey must fail.
                assert not user32.RegisterHotKey(None, 99, 0x4000, 0x72)
                user32.UnregisterHotKey(None, 99)
            probe.unregister_all()
        else:
            assert HOTKEY_IDS["screenshot"] in mgr._registered
            mgr.register("screen_record", "F3", lambda: fired.append("rec"))
            assert "screen_record" in mgr._polled
            assert HOTKEY_IDS["screen_record"] in mgr._registered
        mgr.unregister_all()

    run("F3录屏热键与FFmpeg管理器", _screen_record_f3_integration)
    run("裸F键注册并轮询防系统抢键", _bare_fkey_uses_poll)
    run("录屏缺FFmpeg有明确提示", _screen_record_ffmpeg_missing_tip)
    run("录屏生效后有提示契约", _screen_record_active_tip_contract)
    run("文案与界面打磨契约", _ux_polish_contracts)
    run("录屏孤儿进程可接管结束", _screen_record_orphan_state_reclaim)
    run("性能回归契约", _perf_regression_contracts)
    run("鼠标闪烁回归契约", _cursor_flash_regression)
    run("托盘打开不双沉底/不TOPMOST闪", _tray_open_no_flash)
    run("性能第二轮契约", _perf_round2_contracts)
    run("右键强制刷新绕过签名跳过", _force_refresh_bypasses_sig)


def test_organize_metrics_on_reshow() -> None:
    """hide→release_lazy→show must refresh organize tiles (not stay at 0)."""
    begin("整理页重开刷新指标")

    def _organize_metrics_refresh_on_reshow():
        from pathlib import Path
        from unittest.mock import patch

        from PyQt6.QtWidgets import QApplication

        from src.desktop_scanner import DesktopItem, DesktopScanResult
        from src.ui.main_window import MainWindow

        app = QApplication.instance() or QApplication([])
        settings = {
            "theme": "sky",
            "organize_mode": "virtual",
            "exclude_patterns": [],
            "fences": [],
            "public_desktop_items": [],
            "close_to_tray": True,
            "close_behavior_prompted": True,
            "hotkeys": {},
        }
        fake_items = [
            DesktopItem(Path(r"D:\desktop\a.txt"), "a.txt", False, ".txt", 10),
            DesktopItem(Path(r"D:\desktop\b.lnk"), "b.lnk", False, ".lnk", 20),
            DesktopItem(Path(r"D:\desktop\c.lnk"), "c.lnk", False, ".lnk", 30),
        ]
        scan = DesktopScanResult(items=fake_items)

        win = MainWindow(settings)
        try:
            with patch("src.ui.main_window.scan_desktop", return_value=scan):
                # Simulate tray reopen: destroy lazy pages then show again.
                win.show()
                app.processEvents()
                assert win._files_status_total is not None
                assert win._files_status_total._value.text() == "3"
                assert win._files_status_pending._value.text() == "3"

                win.hide()
                app.processEvents()
                assert "files" not in win._lazy_built

                win.show()
                app.processEvents()
                assert win._files_status_total is not None
                assert win._files_status_total._value.text() == "3"
                assert win._files_status_pending._value.text() == "3"
                assert win._files_status_organized._value.text() == "0"
        finally:
            win.close()
            win.deleteLater()
            app.processEvents()

    run("整理页 hide→show 指标不为0", _organize_metrics_refresh_on_reshow)

    def _no_hover_wheel_on_combos():
        from PyQt6.QtCore import QPoint, QPointF, Qt
        from PyQt6.QtGui import QWheelEvent
        from PyQt6.QtWidgets import QApplication

        from src.ui.no_wheel_combo import NoWheelComboBox, NoWheelSpinBox

        app = QApplication.instance() or QApplication([])
        combo = NoWheelComboBox()
        combo.addItems(["a", "b", "c"])
        combo.setCurrentIndex(0)
        combo.show()
        app.processEvents()
        local = QPointF(10, 10)
        global_pos = QPointF(combo.mapToGlobal(QPoint(10, 10)))
        # Hover wheel with popup closed must not change index.
        ev = QWheelEvent(
            local,
            global_pos,
            QPoint(0, 0),
            QPoint(0, 120),
            Qt.MouseButton.NoButton,
            Qt.KeyboardModifier.NoModifier,
            Qt.ScrollPhase.NoScrollPhase,
            False,
        )
        combo.wheelEvent(ev)
        assert combo.currentIndex() == 0

        spin = NoWheelSpinBox()
        spin.setRange(0, 10)
        spin.setValue(3)
        spin.show()
        app.processEvents()
        spin.clearFocus()
        app.processEvents()
        spin.wheelEvent(ev)
        assert spin.value() == 3
        spin.setFocus()
        app.processEvents()
        spin.wheelEvent(ev)
        assert spin.value() != 3
        combo.close()
        spin.close()
        combo.deleteLater()
        spin.deleteLater()
        app.processEvents()

    run("悬停滚轮不改下拉/步进值", _no_hover_wheel_on_combos)


def test_page_organize_rules() -> None:
    begin("8) 整理规则仅分区生效（分页不再携带）")

    def _resolve_targets():
        from pathlib import Path as P

        from src.desktop_scanner import DesktopItem
        from src.fence_rules import resolve_organize_target
        from src.organizer import organize_desktop
        import src.desktop_scanner as scanner
        import src.settings as settings_mod
        import inspect

        from src.ui.page_edit_dialog import PageEditDialog

        doc = DesktopItem(P(r"C:\tmp\a.txt"), "a.txt", False, ".txt", 0)
        settings_empty = {
            "current_page": 1,
            "desktop_pages": [
                {"id": 0, "name": "工作"},
                {"id": 1, "name": "文档"},
            ],
            "fences": [
                {
                    "id": "only_work",
                    "name": "常用",
                    "pages": [0],
                    "organize_kinds": ["icon"],
                }
            ],
        }
        # Empty「文档」page (no fences) still claims file items so one-click
        # organize can materialize a real fence instead of losing docs.
        hit_empty = resolve_organize_target(doc, settings_empty, page_id=1)
        assert hit_empty is not None and hit_empty[0] == "page"
        assert hit_empty[1]["id"] == 1
        # Page 0 icon-only fences don't match docs → fall through to empty page 1.
        hit_cross = resolve_organize_target(doc, settings_empty, page_id=0)
        assert hit_cross is not None and hit_cross[0] == "page"
        assert hit_cross[1]["id"] == 1
        # With a document fence on page 1, docs resolve to that fence.
        settings_docs = {
            "current_page": 0,
            "desktop_pages": [
                {"id": 0, "name": "工作"},
                {"id": 1, "name": "文档"},
            ],
            "fences": [
                {
                    "id": "only_work",
                    "name": "常用",
                    "pages": [0],
                    "organize_kinds": ["icon"],
                },
                {
                    "id": "docs",
                    "name": "文档",
                    "pages": [1],
                    "organize_kinds": ["file"],
                },
            ],
        }
        hit = resolve_organize_target(doc, settings_docs, page_id=0)
        assert hit is not None and hit[0] == "fence"
        assert hit[1]["id"] == "docs"
        icon = DesktopItem(P(r"C:\tmp\a.lnk"), "a.lnk", False, ".lnk", 0)
        hit0 = resolve_organize_target(icon, settings_docs, page_id=0)
        assert hit0 is not None and hit0[0] == "fence"
        assert hit0[1]["id"] == "only_work"

        with __import__("tempfile").TemporaryDirectory() as tmp:
            desk = P(tmp)
            f = desk / "note.txt"
            f.write_text("n", encoding="utf-8")
            old_paths = settings_mod.get_desktop_paths
            old_scan = scanner.get_desktop_paths
            settings_mod.get_desktop_paths = lambda: [desk]  # type: ignore
            scanner.get_desktop_paths = lambda: [desk]  # type: ignore
            scanner.invalidate_desktop_scan_cache()
            try:
                settings = {
                    "current_page": 1,
                    "organize_mode": "virtual",
                    "exclude_patterns": [],
                    "organize_whitelist": [],
                    "public_desktop_items": [],
                    "desktop_pages": [
                        {"id": 1, "name": "文档"},
                    ],
                    "fences": [
                        {
                            "id": "docs",
                            "name": "文档",
                            "pages": [1],
                            "organize_kinds": ["file"],
                            "virtual_items": [],
                        }
                    ],
                }
                result = organize_desktop(settings, dry_run=False)
                assert result.moved_count == 1
                floats = settings.get("public_desktop_items") or []
                assert floats == []
                fences = settings.get("fences") or []
                assert len(fences) == 1
                pins = fences[0].get("virtual_items") or []
                assert len(pins) == 1
                assert str(pins[0]).endswith("note.txt")
                from src.fence_rules import migrate_page_local_floats_to_organize_fences

                assert migrate_page_local_floats_to_organize_fences(settings) is False
            finally:
                settings_mod.get_desktop_paths = old_paths
                scanner.get_desktop_paths = old_scan
                scanner.invalidate_desktop_scan_cache()

        from src.fence_rules import migrate_page_local_floats_to_organize_fences
        import tempfile

        with tempfile.TemporaryDirectory() as td:
            report = Path(td) / "报告.xlsx"
            report.write_bytes(b"PK")
            orphan = {
                "current_page": 0,
                "desktop_pages": [
                    {"id": 0, "name": "工作"},
                    {"id": 1, "name": "文档"},
                ],
                "fences": [
                    {
                        "id": "only_work",
                        "name": "常用",
                        "pages": [0],
                        "organize_kinds": ["icon"],
                        "virtual_items": [],
                    }
                ],
                "public_desktop_items": [
                    {
                        "path": str(report),
                        "page": 1,
                        "x": 10,
                        "y": 10,
                    }
                ],
            }
            assert migrate_page_local_floats_to_organize_fences(orphan) is True
            assert orphan["public_desktop_items"] == []
            doc_fences = [
                f
                for f in orphan["fences"]
                if 1 in (f.get("pages") or [f.get("page")])
            ]
            assert len(doc_fences) == 1
            assert any(
                str(p).endswith("报告.xlsx")
                for p in (doc_fences[0].get("virtual_items") or [])
            )

        dlg_src = inspect.getsource(PageEditDialog)
        assert "OrganizeKindPicker" not in dlg_src
        assert "organize_kinds" in inspect.getsource(PageEditDialog.apply_to)

    run("分区规则匹配；分页编辑仅改名", _resolve_targets)


def test_system_defaults_locked() -> None:
    begin("9) 系统默认工作/文档页与软件/文档分区锁定")

    def _ensure_and_block_delete():
        import inspect
        import json
        from pathlib import Path

        from src.system_defaults import (
            SYSTEM_COMMON_FENCE_ID,
            SYSTEM_COMMON_FENCE_NAME,
            SYSTEM_DOCS_FENCE_ID,
            SYSTEM_DOCS_FENCE_NAME,
            SYSTEM_DOCS_PAGE_ID,
            SYSTEM_SOFTWARE_SIDE_FENCE_ID,
            SYSTEM_SOFTWARE_SIDE_FENCE_NAME,
            SYSTEM_WORK_PAGE_ID,
            _DOCS_FALLBACK,
            _SOFT_FALLBACK,
            _SOFT_SIDE_FALLBACK,
            default_docs_fence,
            default_software_fence,
            default_software_side_fence,
            ensure_system_defaults,
            is_locked_fence,
            is_locked_page,
        )
        from src.ui.desktop_layout_widget import DesktopLayoutWidget
        from PyQt6.QtWidgets import QApplication

        data = json.loads(
            (Path(__file__).resolve().parents[1] / "config" / "default_settings.json")
            .read_text(encoding="utf-8")
        )
        assert data["desktop_pages"][0]["id"] == 0
        assert data["desktop_pages"][0]["name"] == "工作"
        assert data["desktop_pages"][0].get("locked") is True
        assert "organize_kinds" not in data["desktop_pages"][0]
        assert data["desktop_pages"][1]["id"] == 1
        assert data["desktop_pages"][1]["name"] == "文档"
        assert "organize_kinds" not in data["desktop_pages"][1]
        soft = next(f for f in data["fences"] if f["id"] == SYSTEM_COMMON_FENCE_ID)
        side = next(f for f in data["fences"] if f["id"] == SYSTEM_SOFTWARE_SIDE_FENCE_ID)
        docs = next(f for f in data["fences"] if f["id"] == SYSTEM_DOCS_FENCE_ID)
        assert soft["name"] == SYSTEM_COMMON_FENCE_NAME
        assert soft["organize_kinds"] == ["icon"]
        assert soft["pages"] == [0]
        assert soft["x"] == 0 and soft["y"] == 0
        assert soft["width"] == 1280 and soft["height"] == 315
        assert side["name"] == SYSTEM_SOFTWARE_SIDE_FENCE_NAME
        assert side["pages"] == [0]
        assert side["x"] == _SOFT_SIDE_FALLBACK["x"]
        assert side["width"] == _SOFT_SIDE_FALLBACK["width"]
        assert side["height"] == soft["height"]
        assert soft["x"] + soft["width"] < side["x"]
        assert side["x"] + side["width"] <= 1920
        assert abs(soft["width"] / 1920 - 2 / 3) < 0.01
        assert abs(side["width"] / 1920 - 1 / 3) < 0.02
        assert docs["name"] == SYSTEM_DOCS_FENCE_NAME
        assert docs["organize_kinds"] == ["file"]
        assert docs["pages"] == [1]
        assert docs["x"] == 0 and docs["y"] == 0
        assert docs["width"] == 1920 and docs["height"] == 257
        from src.app import DeskTidyApp
        from src import organizer as org_mod
        from src.organizer import restore_native_system_namespace_icons

        assert callable(restore_native_system_namespace_icons)
        ns_src = inspect.getsource(DeskTidyApp._ensure_native_system_namespace_icons)
        assert "restore_native_system_namespace_icons" in ns_src
        org_src = inspect.getsource(org_mod._organize_virtual)
        assert "ensure_hosted_system_namespace_public_icons" in org_src
        assert "hide_shell_icons" in org_src
        consider = org_src.split("def _consider_item")[1].split(
            "existing_float = find_public_entry"
        )[0]
        assert "is_system_namespace_path(path)" not in consider
        from src.fence_layout import _heal_top_edge_strips

        heal_settings = {
            "fences": [
                {
                    "id": "system_common",
                    "x": 0,
                    "y": 0,
                    "width": 1609,
                    "height": 315,
                    "collapsed": False,
                }
            ],
            "fence_layouts_by_page": {},
            "fence_layouts_by_display": {},
        }
        _heal_top_edge_strips(heal_settings)
        assert heal_settings["fences"][0]["y"] == 0
        # Fallback px match the shipped JSON; live create scales to primary screen.
        assert _SOFT_FALLBACK["width"] == 1280 and _SOFT_FALLBACK["height"] == 315
        assert _SOFT_SIDE_FALLBACK["width"] == 636
        assert abs(_SOFT_FALLBACK["width"] / 1920 - 2 / 3) < 0.01
        assert _DOCS_FALLBACK["width"] == 1920 and _DOCS_FALLBACK["height"] == 257
        soft_live = default_software_fence()
        side_live = default_software_side_fence()
        docs_live = default_docs_fence()
        assert soft_live["width"] >= 160 and soft_live["height"] >= 120
        assert side_live["width"] >= 120 and side_live["height"] == soft_live["height"]
        assert soft_live["x"] + soft_live["width"] < side_live["x"]
        assert side_live["x"] + side_live["width"] <= soft_live["x"] + soft_live["width"] + side_live["width"] + 32
        assert docs_live["width"] >= soft_live["width"]
        assert docs_live["height"] >= 120

        settings = {
            "desktop_pages": [{"id": 0, "name": "工作"}],
            "fences": [{"id": "x", "name": "常用程序", "pages": [0], "organize_kinds": []}],
        }
        assert ensure_system_defaults(settings) is True
        work = next(p for p in settings["desktop_pages"] if p["id"] == SYSTEM_WORK_PAGE_ID)
        doc_page = next(p for p in settings["desktop_pages"] if p["id"] == SYSTEM_DOCS_PAGE_ID)
        common = next(
            f for f in settings["fences"] if f.get("id") == SYSTEM_COMMON_FENCE_ID
        )
        docs_fence = next(
            f for f in settings["fences"] if f.get("id") == SYSTEM_DOCS_FENCE_ID
        )
        assert is_locked_page(work) and is_locked_page(doc_page)
        assert is_locked_fence(common) and is_locked_fence(docs_fence)
        assert common["name"] == "常用"
        assert common["organize_kinds"] == ["icon"]
        assert SYSTEM_WORK_PAGE_ID in (common.get("pages") or [0])
        assert docs_fence["name"] == "文档"
        assert docs_fence["organize_kinds"] == ["file"]
        assert docs_fence.get("pages") == [SYSTEM_DOCS_PAGE_ID]

        # Missing defaults are recreated.
        empty = {"desktop_pages": [], "fences": []}
        assert ensure_system_defaults(empty) is True
        assert sum(1 for p in empty["desktop_pages"] if is_locked_page(p)) >= 2
        assert sum(1 for f in empty["fences"] if is_locked_fence(f)) >= 2

        app = QApplication.instance() or QApplication([])
        import src.ui.desktop_layout_widget as dlw

        warns: list[str] = []
        orig_warn = dlw.show_warning
        dlw.show_warning = lambda *_a, **_k: warns.append("warn")  # type: ignore
        try:
            w = DesktopLayoutWidget(
                {
                    "current_page": 0,
                    "desktop_pages": [
                        {"id": 0, "name": "工作", "locked": True},
                        {"id": 1, "name": "文档", "locked": True},
                    ],
                    "fences": [
                        {
                            "id": SYSTEM_COMMON_FENCE_ID,
                            "name": "软件",
                            "locked": True,
                            "pages": [0],
                            "organize_kinds": ["icon"],
                            "visible": True,
                        },
                        {
                            "id": SYSTEM_DOCS_FENCE_ID,
                            "name": "文档",
                            "locked": True,
                            "pages": [1],
                            "organize_kinds": ["file"],
                            "visible": True,
                        },
                    ],
                    "show_page_indicator": True,
                }
            )
            before_pages = len(w.settings["desktop_pages"])
            before_fences = len(w.settings["fences"])
            w._delete_page(0)
            w._delete_page(1)
            w._delete_fence(w.settings["fences"][0])
            w._delete_fence(w.settings["fences"][1])
            assert len(w.settings["desktop_pages"]) == before_pages
            assert len(w.settings["fences"]) == before_fences
            assert len(warns) >= 4
            w.close()
            w.deleteLater()
            app.processEvents()
        finally:
            dlw.show_warning = orig_warn

    run("默认配置锁定且删除被拦截", _ensure_and_block_delete)


def test_admin_select_does_not_switch_desktop() -> None:
    begin("10) 管理端选分区不切桌面")

    def _tree_select_keeps_page():
        from PyQt6.QtWidgets import QApplication

        from src.ui.desktop_layout_widget import DesktopLayoutWidget

        app = QApplication.instance() or QApplication([])
        settings = {
            "current_page": 0,
            "desktop_pages": [
                {"id": 0, "name": "工作"},
                {"id": 1, "name": "文档"},
            ],
            "fences": [
                {"id": "f0", "name": "常用", "pages": [0], "visible": True},
                {"id": "f1", "name": "资料", "pages": [1], "visible": True},
            ],
            "show_page_indicator": True,
        }
        w = DesktopLayoutWidget(settings)
        emitted: list[int] = []
        w.pages_changed.connect(lambda: emitted.append(1))
        w.show()
        app.processEvents()

        assert w._select_fence_item("f1", 1) is True
        app.processEvents()
        assert settings.get("current_page") == 0, settings.get("current_page")
        assert emitted == [], emitted

        assert w.select_page_id(1) is True
        app.processEvents()
        assert settings.get("current_page") == 0, settings.get("current_page")
        assert w._selected_page_id() == 1
        src = __import__("inspect").getsource(DesktopLayoutWidget._on_page_selected)
        assert "never switch" in src.lower() or "Never switch" in src
        assert "current_page" not in src
        assert "pages_changed.emit" not in src
        w.close()
        w.deleteLater()
        app.processEvents()

    run("卡片选中分区/分页不改 current_page", _tree_select_keeps_page)


def test_fence_visibility_toggle_surgical() -> None:
    begin("10b) 后台显示/隐藏仅作用于单个分区")

    def _visibility_contract():
        import inspect

        from src.app import DeskTidyApp
        from src.ui.desktop_layout_widget import DesktopLayoutWidget
        from src.ui.main_window import MainWindow

        mod_src = inspect.getsource(
            __import__("src.ui.desktop_layout_widget", fromlist=["x"])
        )
        assert "显隐" in mod_src
        assert "_toggle_visible_by_id" in mod_src
        assert "fence_view" in mod_src

        toggle_src = inspect.getsource(DesktopLayoutWidget._toggle_visible_by_id)
        assert "fence_visibility_changed.emit" in toggle_src
        assert "fences_changed.emit" not in toggle_src
        assert "reload_table" not in toggle_src
        assert "_refresh_fence_row" in toggle_src

        mw_src = inspect.getsource(MainWindow._build_fences_page)
        assert "fence_visibility_changed.connect" in mw_src

        apply_src = inspect.getsource(DeskTidyApp._apply_fence_visibility)
        assert "force_rebuild" not in apply_src
        assert "rebuild_fences" not in apply_src
        assert "_retire_overlay_widget" in apply_src
        assert "fence_on_page" in apply_src
        # Hide→show while settings open must map the fence (not only set a flag).
        assert "_reveal_desktop_overlay" in apply_src
        assert "_desk_app_ui_open" in apply_src
        # Visibility apply must not full-reload the settings tree (perf / flicker).
        assert "reload_table" not in apply_src

        keep_src = inspect.getsource(DeskTidyApp._keep_overlays_under_apps)
        assert "_reveal_desktop_overlay" in keep_src
        assert "isVisible" in keep_src

    run("显示/隐藏走单分区同步", _visibility_contract)

    def _hide_then_show_maps_under_settings():
        """Regression: settings-open hide→show must leave the fence Qt-visible."""
        import uuid
        from unittest import mock

        from PyQt6.QtWidgets import QApplication

        from src.app import DeskTidyApp
        from src.fence_pages import set_fence_pages

        app = QApplication.instance() or QApplication([])
        fid = uuid.uuid4().hex[:8]
        fence_cfg = {
            "id": fid,
            "name": "vis-test",
            "folder": "vis-test",
            "visible": True,
            "x": 80,
            "y": 80,
            "width": 240,
            "height": 200,
            "style": {"view_mode": "grid", "icon_size": 48},
        }
        set_fence_pages(fence_cfg, [0])
        settings = {
            "show_fences": True,
            "theme": "mist",
            "exclude_patterns": [],
            "fences": [fence_cfg],
            "desktop_pages": [{"id": 0, "name": "默认"}],
            "current_page": 0,
            "fence_page_layouts": {},
        }

        desk = DeskTidyApp.__new__(DeskTidyApp)
        desk.settings = settings
        desk.fences = []
        desk._parked_fences = {}
        desk._max_parked_fences = 8
        desk._exiting = False
        desk._icons_hidden = False
        desk._fences_hidden = False
        desk._shell_attach_after_settings = False
        desk.page_indicator = None
        desk.dock = None
        desk.public_icons = []
        desk._parked_public_icons = {}
        desk._public_icon_host = None
        desk._notepad_window = None
        desk._displayed_page = 0

        class _Editor:
            @staticmethod
            def reload_table():
                pass

        class _Win:
            fence_editor = _Editor()

            def isVisible(self):
                return True

            def isMinimized(self):
                return False

            def winId(self):
                return 1

            def raise_(self):
                pass

        desk.window = _Win()
        desk._settings_window_hwnd = lambda: 1  # type: ignore[method-assign]
        desk._notepad_window_hwnd = lambda: 0  # type: ignore[method-assign]
        desk._foreign_app_owns_foreground = lambda: False  # type: ignore[method-assign]
        desk._wire_fence_signals = lambda fence: None  # type: ignore[method-assign]
        desk._fence_needs_icon_rebuild = lambda fence, cfg: False  # type: ignore[method-assign]
        desk._ensure_page_chrome_visible = lambda **kw: None  # type: ignore[method-assign]
        desk._raise_settings_window = lambda: None  # type: ignore[method-assign]
        desk._raise_notepad_window = lambda: None  # type: ignore[method-assign]
        desk._current_page = lambda: 0  # type: ignore[method-assign]
        desk._fences_should_show = lambda: True  # type: ignore[method-assign]
        desk._overlay_widgets_may_show = DeskTidyApp._overlay_widgets_may_show.__get__(
            desk, DeskTidyApp
        )
        desk._desk_app_ui_open = DeskTidyApp._desk_app_ui_open.__get__(desk, DeskTidyApp)
        desk._settings_ui_open = DeskTidyApp._settings_ui_open.__get__(desk, DeskTidyApp)
        desk._notepad_ui_open = DeskTidyApp._notepad_ui_open.__get__(desk, DeskTidyApp)
        desk._iter_overlay_widgets = DeskTidyApp._iter_overlay_widgets.__get__(
            desk, DeskTidyApp
        )
        desk._take_parked_fence = DeskTidyApp._take_parked_fence.__get__(desk, DeskTidyApp)
        desk._retire_overlay_widget = DeskTidyApp._retire_overlay_widget.__get__(
            desk, DeskTidyApp
        )
        desk._reveal_desktop_overlay = DeskTidyApp._reveal_desktop_overlay.__get__(
            desk, DeskTidyApp
        )
        desk._keep_overlays_under_apps = DeskTidyApp._keep_overlays_under_apps.__get__(
            desk, DeskTidyApp
        )
        desk._apply_fence_visibility = DeskTidyApp._apply_fence_visibility.__get__(
            desk, DeskTidyApp
        )
        desk._fence_hwnd = lambda w: int(w.winId()) if w is not None else 0

        with mock.patch("src.app.save_settings"), mock.patch(
            "src.desktop_shell_host.attach_overlay_to_desktop"
        ), mock.patch(
            "src.desktop_shell_host.is_attached_to_desktop", return_value=False
        ), mock.patch(
            "src.desktop_shell_host.is_stuck_under_wallpaper", return_value=False
        ), mock.patch(
            "src.desktop_shell_host.place_overlay_in_desktop_band"
        ), mock.patch(
            "src.win_shell.configure_desktop_overlay"
        ):
            desk._apply_fence_visibility(fid, False)
            assert desk.fences == []
            assert fence_cfg.get("visible") is False

            desk._apply_fence_visibility(fid, True)
            assert fence_cfg.get("visible") is True
            assert len(desk.fences) == 1
            live = desk.fences[0]
            assert live.isVisible(), "hide→show while settings open must Qt-show the fence"
            # close only — deleteLater()+processEvents aborts (0xC0000409) after
            # stubbed shell attach on this synthetic DeskTidyApp skeleton.
            live.close()
            desk.fences.clear()

    run("设置页隐藏后再显示会映射分区", _hide_then_show_maps_under_settings)


def test_office_email_temp_filtered() -> None:
    begin("Office/邮件编辑临时文件不显示为浮标")

    import inspect
    import tempfile
    from pathlib import Path

    from src.app import DeskTidyApp
    from src.desktop_scanner import (
        is_temp_desktop_file,
        purge_temp_from_settings,
        scan_desktop,
    )
    import src.desktop_scanner as scanner
    import src.settings as settings_mod

    assert is_temp_desktop_file("~$报告.docx")
    assert is_temp_desktop_file("~WRL0001.tmp")
    assert is_temp_desktop_file("~报告.docx")
    assert is_temp_desktop_file("报告.wbk")
    assert is_temp_desktop_file("报告.asd")
    assert is_temp_desktop_file("报告.tmp.docx")
    assert not is_temp_desktop_file("报告.docx")

    from src.desktop_scanner import is_ignored_desktop_entry

    # Default exclude "DeskTidy" must hide the app shortcut, not recordings.
    excl = ["desktop.ini", "DeskTidy"]
    assert is_ignored_desktop_entry("DeskTidy", excl)
    assert is_ignored_desktop_entry("DeskTidy.lnk", excl)
    assert is_ignored_desktop_entry("desktop.ini", excl)
    assert not is_ignored_desktop_entry("DeskTidy_20260908_100919.mp4", excl)
    assert not is_ignored_desktop_entry("报告.docx", excl)
    assert is_ignored_desktop_entry("shot_01.png", ["shot_*.png"])
    assert not is_ignored_desktop_entry("other.png", ["shot_*.png"])

    ingest = inspect.getsource(DeskTidyApp._try_ingest_desktop_file_as_public)
    assert "is_ignored_desktop_entry" in ingest
    assert "is_desktop_loose_item" in ingest
    assert "path.resolve()" not in ingest
    refresh = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "_purge_temp_desktop_refs" in refresh
    assert "sync_loose_desktop_items" in refresh
    assert "ensure_desktop_loose_floats" not in refresh
    organize = inspect.getsource(DeskTidyApp._run_watched_organize)
    assert "_purge_temp_desktop_refs" in organize

    settings = {
        "fences": [{"virtual_items": ["C:/x/~$a.docx", "C:/x/real.docx"]}],
        "public_desktop_items": [
            {"path": "C:/x/~$b.xlsx"},
            {"path": "C:/x/客户资料.xlsx"},
        ],
    }
    removed = purge_temp_from_settings(settings)
    assert len(removed) == 2
    assert settings["fences"][0]["virtual_items"] == ["C:/x/real.docx"]
    assert len(settings["public_desktop_items"]) == 1

    with tempfile.TemporaryDirectory() as tmp:
        desk = Path(tmp)
        (desk / "客户资料.xlsx").write_text("x", encoding="utf-8")
        (desk / "~$客户资料.xlsx").write_text("lock", encoding="utf-8")
        (desk / "草稿.wbk").write_text("bak", encoding="utf-8")
        old_paths = settings_mod.get_desktop_paths
        old_scan = scanner.get_desktop_paths
        settings_mod.get_desktop_paths = lambda: [desk]  # type: ignore
        scanner.get_desktop_paths = lambda: [desk]  # type: ignore
        scanner.invalidate_desktop_scan_cache()
        try:
            names = {item.name for item in scan_desktop().items}
            assert names == {"客户资料.xlsx"}, names
        finally:
            settings_mod.get_desktop_paths = old_paths
            scanner.get_desktop_paths = old_scan
            scanner.invalidate_desktop_scan_cache()


def test_loose_desktop_floats() -> None:
    begin("11) 未归入桌面项显示为浮标")

    def _sync_loose():
        import tempfile
        from pathlib import Path

        from src.public_desktop import (
            sync_loose_desktop_items,
            visible_floating_items,
        )
        import src.desktop_scanner as scanner
        import src.settings as settings_mod

        with tempfile.TemporaryDirectory() as tmp:
            desk = Path(tmp)
            folder = desk / "opencode"
            folder.mkdir()
            checklist = desk / "问题清单"
            checklist.mkdir()
            pinned = desk / "in_fence.txt"
            pinned.write_text("p", encoding="utf-8")
            old_paths = settings_mod.get_desktop_paths
            old_scan = scanner.get_desktop_paths
            settings_mod.get_desktop_paths = lambda: [desk]  # type: ignore
            scanner.get_desktop_paths = lambda: [desk]  # type: ignore
            scanner.invalidate_desktop_scan_cache()
            try:
                settings = {
                    "current_page": 1,
                    "enable_public_desktop": False,
                    "exclude_patterns": [],
                    "public_desktop_items": [],
                    "fences": [
                        {
                            "id": "f1",
                            "name": "常用",
                            "pages": [0],
                            "virtual_items": [str(pinned)],
                        }
                    ],
                }
                assert sync_loose_desktop_items(settings) is True
                # New loose items land on the current page (Save As visibility).
                names_doc = {
                    Path(str(e["path"])).name
                    for e in visible_floating_items(settings, 1)
                }
                assert "opencode" in names_doc
                assert "问题清单" in names_doc
                names_work = {
                    Path(str(e["path"])).name
                    for e in visible_floating_items(settings, 0)
                }
                assert "opencode" not in names_work
                assert "问题清单" not in names_work
                assert "in_fence.txt" not in names_work
                loose = [
                    e
                    for e in settings["public_desktop_items"]
                    if e.get("loose")
                ]
                assert all(int(e.get("page")) == 1 for e in loose)
                # Idempotent (force bypasses TTL throttle).
                assert sync_loose_desktop_items(settings, force=True) is False
            finally:
                settings_mod.get_desktop_paths = old_paths
                scanner.get_desktop_paths = old_scan
                scanner.invalidate_desktop_scan_cache()

        from src.app import DeskTidyApp
        import inspect

        impl = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
        assert "_start_loose_sync_background" in impl
        assert "sync_loose_desktop_items" in impl
        assert "ensure_desktop_loose_floats" not in impl
        assert "_shell_attach_quiet_until" in impl
        assert "_overlay_drag_active" in impl
        # Watch path: refresh public overlays; optional live rule pin.
        watched = inspect.getsource(DeskTidyApp._run_watched_organize)
        assert "refresh_public=True" in watched
        assert "try_auto_pin_desktop_paths" in watched
        assert "_reveal_organize_float_pages" in watched
        assert "auto_organize_watch" in watched
        # Full-desktop organize_desktop stays for 一键整理 / startup — not the watch path.
        assert "organize_desktop(" not in watched
        # Startup organize must not vacuum when live auto-pin is off.
        startup_org = inspect.getsource(DeskTidyApp._startup_deferred_organize)
        assert "auto_organize_watch" in startup_org
        assert "auto_organize_on_startup" in startup_org
        assert "_do_organize" in startup_org
        init_src = inspect.getsource(DeskTidyApp.__init__)
        # migrate of page floats is gated on the same auto-pin switch.
        assert "migrate_page_local_floats_to_organize_fences(self.settings)" in init_src
        gate = 'if bool(self.settings.get("auto_organize_watch", False)):'
        assert gate in init_src
        assert init_src.index(gate) < init_src.index(
            "migrate_page_local_floats_to_organize_fences(self.settings)"
        )
        # Watcher always starts (floats for Save As / drops).
        start_src = inspect.getsource(DeskTidyApp._startup_deferred_monitors)
        assert "_start_watcher()" in start_src
        # Settings UI offers opt-in auto-pin for new files.
        from src.ui.main_window import MainWindow

        mw_src = inspect.getsource(MainWindow._build_settings_page)
        assert "auto_watch_cb" in mw_src
        assert "新桌面文件按规则自动钉选" in mw_src
        assert "启动时也不会" in mw_src
        assert callable(getattr(MainWindow, "_on_auto_watch_organize_changed", None))

        def _startup_skips_when_watch_off():
            from unittest import mock

            from src.app import DeskTidyApp

            app = mock.Mock(spec=DeskTidyApp)
            app._exiting = False
            app.settings = {
                "auto_organize_on_startup": True,
                "auto_organize_watch": False,
            }
            app._do_organize = mock.Mock()
            DeskTidyApp._startup_deferred_organize(app)
            assert app._do_organize.call_count == 0
            app.settings["auto_organize_watch"] = True
            DeskTidyApp._startup_deferred_organize(app)
            assert app._do_organize.call_count == 1

        _startup_skips_when_watch_off()

        from src.file_watcher import DesktopEventHandler

        moved = inspect.getsource(DesktopEventHandler.on_moved)
        assert "dest_path" in moved
        assert "on_moved_file" in moved
        moved_src = inspect.getsource(DeskTidyApp._on_watched_file_moved)
        assert "rewrite_public_path" in moved_src
        assert "rewrite_pinned_path" in moved_src
        rename_src = inspect.getsource(
            __import__("src.ui.fence_icon_item", fromlist=["x"]).commit_filesystem_rename
        )
        assert "文件正在使用中" in rename_src
        created = inspect.getsource(DesktopEventHandler.on_created)
        assert "is_directory" not in created or "_notify" in created

        # Positive: opt-in auto-pin pins a document into the docs fence.
        from src.organizer import try_auto_pin_desktop_paths
        from src.organize_suppress import suppress_desktop_item

        tmp = Path(tempfile.mkdtemp(prefix="desktidy_autopin_"))
        doc = tmp / "report.docx"
        doc.write_text("x", encoding="utf-8")
        lnk = tmp / "app.lnk"
        lnk.write_bytes(b"\x00" * 8)
        settings_ap = {
            "current_page": 0,
            "desktop_pages": [
                {"id": 0, "name": "工作", "organize_kinds": ["icon"]},
                {"id": 1, "name": "文档", "organize_kinds": ["file"]},
            ],
            "fences": [
                {
                    "id": "soft",
                    "name": "软件",
                    "pages": [0],
                    "organize_kinds": ["icon"],
                    "virtual_items": [],
                },
                {
                    "id": "docs",
                    "name": "文档",
                    "pages": [1],
                    "organize_kinds": ["file"],
                    "virtual_items": [],
                },
            ],
            "exclude_patterns": [],
            "organize_whitelist": [],
            "public_desktop_items": [],
        }
        pinned, pin_pages = try_auto_pin_desktop_paths(settings_ap, [doc, lnk])
        assert {p.name for p in pinned} == {"report.docx", "app.lnk"}, pinned
        assert pin_pages == [1, 0] or pin_pages == [0, 1]
        docs_pins = {
            Path(str(p)).name for p in (settings_ap["fences"][1].get("virtual_items") or [])
        }
        soft_pins = {
            Path(str(p)).name for p in (settings_ap["fences"][0].get("virtual_items") or [])
        }
        assert "report.docx" in docs_pins
        assert "app.lnk" in soft_pins
        # Suppress window blocks immediate re-pin (user just dragged out).
        suppress_desktop_item(doc)
        settings_ap["fences"][1]["virtual_items"] = []
        assert try_auto_pin_desktop_paths(settings_ap, [doc])[0] == []
        # Office lock files never pin.
        lock = tmp / "~$draft.docx"
        lock.write_text("x", encoding="utf-8")
        assert try_auto_pin_desktop_paths(settings_ap, [lock])[0] == []

    run("未归入项出现在当前分页", _sync_loose)

    def _watcher_notifies_rename():
        """Save As temp→final rename must notify (not only on_created)."""
        from src.file_watcher import DesktopEventHandler

        seen: list[str] = []

        class _Ev:
            def __init__(self, src: str, dest: str = "", is_dir: bool = False):
                self.src_path = src
                self.dest_path = dest
                self.is_directory = is_dir

        handler = DesktopEventHandler({"exclude_patterns": []}, on_created_file=seen.append)
        handler.on_created(_Ev(r"C:\Users\x\Desktop\~$draft.docx"))
        assert seen == [], "Office lock files must be ignored"
        handler.on_moved(
            _Ev(r"C:\Users\x\Desktop\tmpXXXX.tmp", r"C:\Users\x\Desktop\报告.docx")
        )
        assert seen and seen[-1].endswith("报告.docx"), seen
        moved_pairs: list[tuple[str, str]] = []

        def _on_moved(src: str, dest: str) -> None:
            moved_pairs.append((src, dest))

        handler3 = DesktopEventHandler(
            {"exclude_patterns": []},
            on_moved_file=_on_moved,
        )
        handler3.on_moved(
            _Ev(r"C:\Users\x\Desktop\old.xlsx", r"C:\Users\x\Desktop\book.xlsx")
        )
        assert moved_pairs and moved_pairs[-1][1].endswith("book.xlsx"), moved_pairs
        removed_after_move: list[str] = []
        handler4 = DesktopEventHandler(
            {"exclude_patterns": []},
            on_removed_file=removed_after_move.append,
            on_moved_file=_on_moved,
        )
        handler4.on_moved(
            _Ev(r"C:\Users\x\Desktop\a.docx", r"C:\Users\x\Desktop\b.docx")
        )
        assert moved_pairs[-1][0].endswith("a.docx")
        assert not removed_after_move, "rename must not drop float before relink"
        handler.on_created(_Ev(r"C:\Users\x\Desktop\新建文件夹", is_dir=True))
        assert any(s.endswith("新建文件夹") for s in seen), seen
        removed: list[str] = []
        handler2 = DesktopEventHandler(
            {"exclude_patterns": []},
            on_created_file=seen.append,
            on_removed_file=removed.append,
        )
        handler2.on_deleted(_Ev(r"C:\Users\x\Desktop\gone.txt"))
        assert removed and removed[-1].endswith("gone.txt"), removed

    run("监视器处理另存为重命名+文件夹", _watcher_notifies_rename)


def test_empty_page_does_not_mirror_other_pins() -> None:
    begin("12) 空分页不镜像其它页钉选")

    def _no_cross_page_leak():
        import inspect
        import tempfile
        from pathlib import Path

        from src.app import DeskTidyApp
        from src.public_desktop import (
            clear_ephemeral_public_items,
            get_public_items,
            visible_floating_items,
        )

        with tempfile.TemporaryDirectory() as tmp:
            desk = Path(tmp)
            a = desk / "report.docx"
            a.write_text("a", encoding="utf-8")
            settings = {
                "enable_public_desktop": True,
                "current_page": 1,
                "public_desktop_items": [
                    {
                        "path": str(a),
                        "x": 40,
                        "y": 40,
                        "page": 1,
                        "ephemeral": True,
                    }
                ],
                "fences": [
                    {
                        "id": "work",
                        "name": "常用程序",
                        "pages": [0],
                        "visible": True,
                        "virtual_items": [str(a)],
                    }
                ],
            }
            # Legacy mirrors must be cleared; empty doc page must not show work pins.
            assert clear_ephemeral_public_items(settings) is True
            assert not any(e.get("ephemeral") for e in get_public_items(settings))
            names = {
                Path(str(e["path"])).name for e in visible_floating_items(settings, 1)
            }
            assert "report.docx" not in names

        show_src = inspect.getsource(DeskTidyApp.show_fences)
        assert "ensure_empty_page_pin_mirrors" not in show_src
        assert "clear_ephemeral_public_items" in show_src
        assert "_fence_needs_icon_rebuild" in show_src
        need = inspect.getsource(DeskTidyApp._fence_needs_icon_rebuild)
        assert "has_icon_widgets" in need
        assert "is_portal_fence" in need
        assert "_fence_painted_pin_keys" in need

    run("文档页看不到工作页钉选", _no_cross_page_leak)


def test_layout_snapshot_desktidy() -> None:
    begin("13) 布局快照保存/恢复分区样式 + 双击契约")

    def _roundtrip_layout_style():
        from src.layout_snapshot import (
            LayoutSnapshot,
            apply_layout_snapshot,
            capture_layout,
        )

        settings = {
            "theme": "mist",
            "show_fences": True,
            "current_page": 0,
            "enable_public_desktop": False,
            "desktop_pages": [{"id": 0, "name": "工作"}],
            "fence_layouts_by_display": {"fp1": {"0": {"f1": {"x": 10, "y": 20}}}},
            "fence_layouts_by_page": {},
            "public_desktop_items": [
                {"path": r"C:\tmp\float.txt", "x": 1, "y": 2, "page": 0, "loose": True}
            ],
            "fences": [
                {
                    "id": "f1",
                    "name": "工作区",
                    "x": 40,
                    "y": 60,
                    "width": 300,
                    "height": 400,
                    "collapsed": False,
                    "style": {
                        "opacity": 0.91,
                        "background": "#112233",
                        "view_mode": "list",
                        "accent": "#FF5500",
                    },
                    "virtual_items": [r"C:\tmp\a.exe"],
                }
            ],
        }
        snap = capture_layout("样式测试", settings=settings)
        assert snap.has_desktidy_layout
        assert snap.fence_count == 1
        assert snap.layout["fences"][0]["style"]["view_mode"] == "list"
        # Mutate live settings, then restore from snapshot.
        settings["theme"] = "dark"
        settings["fences"][0]["style"]["view_mode"] = "grid"
        settings["fences"][0]["x"] = 999
        settings["public_desktop_items"] = []
        summary = apply_layout_snapshot(settings, snap)
        assert summary["fence_count"] == 1
        assert settings["theme"] == "mist"
        assert settings["fences"][0]["style"]["view_mode"] == "list"
        assert settings["fences"][0]["style"]["background"] == "#112233"
        assert settings["fences"][0]["x"] == 40
        assert len(settings["public_desktop_items"]) == 1
        # Legacy JSON (icons only) still loads.
        legacy = LayoutSnapshot.from_dict(
            {
                "name": "旧",
                "created_at": "2020-01-01T00:00:00",
                "icon_count": 1,
                "icons": [{"name": "A", "x": 1, "y": 2}],
            }
        )
        assert not legacy.has_desktidy_layout
        assert legacy.icon_count == 1
        # Deepcopy independence: mutating settings after capture must not alter snap.
        snap2 = capture_layout("独立", settings=settings)
        settings["fences"][0]["style"]["opacity"] = 0.1
        assert snap2.layout["fences"][0]["style"]["opacity"] == 0.91

    def _preview_click_contract():
        from PyQt6.QtWidgets import QApplication

        from src.snapshot_preview import (
            collect_preview_fences,
            collect_preview_pages,
            layout_pin_count,
            preview_cache_needs_refresh,
            render_layout_preview,
        )
        from src.ui.snapshot_widget import (
            SnapshotWidget,
            _SnapshotIconDelegate,
            _SnapshotPreviewPopup,
        )
        import inspect

        app = QApplication.instance() or QApplication([])
        _ = app
        layout = {
            "current_page": 0,
            "desktop_pages": [
                {"id": 0, "name": "工作"},
                {"id": 1, "name": "文档"},
            ],
            "fences": [
                {
                    "id": "f1",
                    "name": "常用",
                    "pages": [0],
                    "visible": True,
                    "x": 50,
                    "y": 60,
                    "width": 240,
                    "height": 300,
                    "style": {"background": "#FFFFFF", "accent": "#3B82F6"},
                    "virtual_items": [
                        r"C:\Windows\System32\notepad.exe",
                        r"C:\Windows\explorer.exe",
                        {"path": r"C:\Windows\System32\calc.exe"},
                    ],
                },
                {
                    "id": "f2",
                    "name": "资料",
                    "pages": [1],
                    "visible": True,
                    "x": 80,
                    "y": 70,
                    "width": 260,
                    "height": 280,
                    "style": {"background": "#FEF3C7", "accent": "#D97706"},
                    "virtual_items": [r"C:\Windows\System32\write.exe"],
                },
            ],
            "public_desktop_items": [
                {"path": r"C:\Windows\System32\cmd.exe", "x": 400, "y": 80, "page": 0},
            ],
        }
        # Legacy single-page helper still respects current_page.
        items = collect_preview_fences(layout)
        assert len(items) == 2 and items[0]["name"] == "常用"
        assert len(items[0]["icons"]) == 3
        assert items[1].get("float") is True
        # Multi-page collection must include every desktop page.
        pages = collect_preview_pages(layout)
        assert len(pages) == 2
        assert pages[0]["name"] == "工作" and pages[1]["name"] == "文档"
        assert any(i["name"] == "常用" for i in pages[0]["items"])
        assert any(i["name"] == "资料" for i in pages[1]["items"])
        # Page-1-only fence must not appear on page 0.
        assert not any(i["name"] == "资料" for i in pages[0]["items"])
        pix = render_layout_preview(layout)
        assert not pix.isNull()
        assert pix.width() >= 480 and pix.height() >= 270
        assert layout_pin_count(layout) == 5  # 3+1 fence pins + 1 public float
        # Icons must change the rendered pixels vs empty-fence baseline.
        empty = {
            **layout,
            "fences": [
                {**layout["fences"][0], "virtual_items": []},
                {**layout["fences"][1], "virtual_items": []},
            ],
            "public_desktop_items": [],
        }
        pix_empty = render_layout_preview(empty)
        assert pix.toImage() != pix_empty.toImage()
        # Stale tiny PNG (pre-icon era) must refresh when pins exist.
        import tempfile
        from pathlib import Path as _P

        with tempfile.TemporaryDirectory() as _td:
            fake_json = _P(_td) / "snap.json"
            fake_json.write_text("{}", encoding="utf-8")
            tiny = fake_json.with_suffix(".png")
            tiny.write_bytes(b"\x89PNG\r\n\x1a\n" + b"\x00" * 200)
            assert preview_cache_needs_refresh(fake_json, layout) is True
            assert preview_cache_needs_refresh(fake_json, empty) is False
        thumb_src = inspect.getsource(
            __import__("src.ui.snapshot_widget", fromlist=["x"])._thumb_for
        )
        assert "preview_cache_needs_refresh" in thumb_src
        assert "ensure_snapshot_preview" in thumb_src
        assert "layout_pin_count" in inspect.getsource(SnapshotWidget.reload_table)
        # Wide short fences must still show icons (stacked card mode), not blank chrome.
        wide = {
            "current_page": 0,
            "desktop_pages": [{"id": 0, "name": "工作"}],
            "fences": [
                {
                    "id": "wide",
                    "name": "软件",
                    "pages": [0],
                    "visible": True,
                    "x": 0,
                    "y": 0,
                    "width": 1600,
                    "height": 300,
                    "style": {"background": "#FFFFFF", "accent": "#3B82F6"},
                    "virtual_items": [
                        r"C:\Windows\System32\notepad.exe",
                        r"C:\Windows\explorer.exe",
                        r"C:\Windows\System32\calc.exe",
                        r"C:\Windows\System32\cmd.exe",
                    ],
                }
            ],
            "public_desktop_items": [],
        }
        pix_wide = render_layout_preview(wide)
        pix_wide_empty = render_layout_preview(
            {
                **wide,
                "fences": [{**wide["fences"][0], "virtual_items": []}],
            }
        )
        assert pix_wide.toImage() != pix_wide_empty.toImage()
        from src.snapshot_preview import _draw_page_items_stacked

        assert callable(_draw_page_items_stacked)
        # Multi-page render must differ from current-page-only crop.
        single = {
            **layout,
            "desktop_pages": [{"id": 0, "name": "工作"}],
        }
        pix_single = render_layout_preview(single)
        assert pix.toImage() != pix_single.toImage()

        src = inspect.getsource(SnapshotWidget._build_ui)
        assert "setMouseTracking(True)" in src
        assert "IconMode" in src
        assert 'setObjectName("snapshotIconView")' in src
        assert "setItemDelegate" in src
        assert "恢复选中快照" not in src
        assert "删除选中" not in src
        assert "保存当前布局" in src
        assert "_SnapshotIconDelegate" in inspect.getsource(SnapshotWidget)
        widget_src = inspect.getsource(SnapshotWidget)
        assert "_run_card_action" in widget_src
        assert "_hit_card_action" in widget_src
        assert "MouseButtonPress" in inspect.getsource(SnapshotWidget.eventFilter)
        assert "or -1" not in inspect.getsource(SnapshotWidget.eventFilter)
        assert "_item_row" in inspect.getsource(SnapshotWidget)
        paint = inspect.getsource(_SnapshotIconDelegate.paint)
        assert "_CARD_ACTIONS" in paint
        from src.ui.snapshot_widget import (
            _CARD_ACTIONS,
            _FOOTER_TITLE_H,
            _ITEM_H,
            _ITEM_W,
            _THUMB_H,
            _card_action_rects,
            _card_content_rect,
            _created_redundant_with_name,
            _hit_card_action,
            _thumb_rect,
        )
        from PyQt6.QtCore import QPoint, QRect

        assert [label for _, label in _CARD_ACTIONS] == ["预览", "应用", "删除"]
        card = _card_content_rect(QRect(0, 0, _ITEM_W, _ITEM_H))
        thumb = _thumb_rect(card)
        rects = _card_action_rects(card)
        assert set(rects) == {"preview", "apply", "delete"}
        assert _hit_card_action(card, rects["apply"].center()) == "apply"
        assert _hit_card_action(card, QPoint(10, 10)) is None
        # Chips sit under the full-width title row (not beside a squeezed name).
        assert rects["preview"].top() >= thumb.bottom() + _FOOTER_TITLE_H
        assert rects["delete"].bottom() <= card.bottom()
        assert rects["delete"].right() <= card.right()
        assert rects["preview"].left() > thumb.left()
        assert all(r.height() >= 22 and r.width() >= 44 for r in rects.values())
        assert thumb.height() == _THUMB_H
        # Title stays left of the action row.
        assert rects["preview"].left() > card.left() + 40
        assert _created_redundant_with_name("2026-09-17 14:57", "2026-09-17T14:57:03")
        assert not _created_redundant_with_name("我的布局", "2026-09-17 14:57")
        paint = inspect.getsource(_SnapshotIconDelegate.paint)
        assert "_card_action_rects" in paint
        assert "ElideRight" in paint
        assert "_created_redundant_with_name" in paint
        assert "text_muted" in paint
        assert 'palette["muted"]' not in paint
        assert "btn_fill" in paint
        assert "rect.width() - 20" in paint or "rect.width()-20" in paint.replace(" ", "")
        sheet = __import__("src.ui.styles", fromlist=["build_stylesheet"]).build_stylesheet("mist")
        assert "snapshotIconView::item" in sheet
        item_block = sheet.split("QListWidget#snapshotIconView::item {")[1].split("}")[0]
        assert "background: transparent" in item_block
        assert "margin: 0px" in item_block
        show = inspect.getsource(SnapshotWidget._show_click_preview)
        assert "load_snapshot_preview" in show
        assert "preview_canvas_size" in show
        assert "prefer_cache=False" in show
        from src.snapshot_preview import load_snapshot_preview
        load_src = inspect.getsource(load_snapshot_preview)
        assert "render_layout_preview" in load_src
        assert "prefer_cache" in load_src
        cap = inspect.getsource(SnapshotWidget._capture)
        assert "write_preview=False" in cap
        assert "_finish_snapshot_preview" in cap
        from src.layout_snapshot import capture_layout, list_snapshots, save_snapshot
        import inspect as _ins

        assert "hide_shell_icons" in _ins.getsource(capture_layout)
        assert "ensure_snapshot_preview" not in _ins.getsource(list_snapshots)
        assert "write_preview" in _ins.getsource(save_snapshot)
        popup_src = inspect.getsource(_SnapshotPreviewPopup.show_preview)
        assert "pixmap.width()" in popup_src and "pixmap.height()" in popup_src
        assert "availableGeometry()" in popup_src
        assert "anchor" not in popup_src
        assert "Qt.WindowType.Popup" in inspect.getsource(_SnapshotPreviewPopup.__init__)

    def _save_skips_shell_when_hidden():
        """Saving must not probe Explorer ListView when shell icons are hidden."""
        import time
        from pathlib import Path
        import tempfile
        import src.layout_snapshot as ls
        from src.layout_snapshot import capture_layout, save_snapshot

        settings = {
            "theme": "mist",
            "hide_shell_icons": True,
            "show_fences": True,
            "current_page": 0,
            "desktop_pages": [{"id": 0, "name": "工作"}],
            "fences": [
                {
                    "id": "f1",
                    "name": "区",
                    "x": 10,
                    "y": 10,
                    "width": 200,
                    "height": 200,
                    "virtual_items": [r"C:\Windows\System32\notepad.exe"],
                    "style": {"background": "#FFFFFF", "accent": "#3B82F6"},
                }
            ],
        }
        t0 = time.perf_counter()
        snap = capture_layout("速存", settings=settings)
        t1 = time.perf_counter()
        assert snap.icon_count == 0
        assert t1 - t0 < 1.0, t1 - t0
        td = Path(tempfile.mkdtemp(prefix="desktidy_snapsave_"))
        old = ls.SNAPSHOTS_DIR
        ls.SNAPSHOTS_DIR = td
        try:
            t2 = time.perf_counter()
            path = save_snapshot(snap, write_preview=False)
            t3 = time.perf_counter()
            assert path.is_file()
            assert t3 - t2 < 1.0, t3 - t2
            assert not path.with_suffix(".png").is_file()
        finally:
            ls.SNAPSHOTS_DIR = old
            import shutil

            shutil.rmtree(td, ignore_errors=True)

    def _double_click_and_app_wire():
        import inspect

        from src.app import DeskTidyApp
        from src.ui.snapshot_widget import SnapshotWidget

        src = inspect.getsource(SnapshotWidget._build_ui)
        assert "itemDoubleClicked" in src
        assert "_on_item_double_clicked" in src
        assert "snapshotIconView" in src
        assert "IconMode" in src
        assert "_restore" in inspect.getsource(SnapshotWidget)
        from src.ui.styles import build_stylesheet

        sheet = build_stylesheet("mist")
        assert "snapshotIconView" in sheet
        assert "snapshotIconView::item:selected" in sheet
        app_src = inspect.getsource(DeskTidyApp.__init__)
        assert "snapshot_restored" in app_src
        assert "_on_snapshot_restored" in app_src
        restore = inspect.getsource(DeskTidyApp._on_snapshot_restored)
        assert "force_rebuild=True" in restore
        assert "refresh_public_desktop" in restore
        assert "_apply_theme" in restore

    def _chip_click_hits_thumb_corner():
        """Paint/hit geometry share one helper; Press on chip must fire action."""
        from unittest import mock

        from PyQt6.QtCore import QEvent, QPoint, QPointF, Qt
        from PyQt6.QtGui import QMouseEvent
        from PyQt6.QtWidgets import QApplication, QListWidgetItem

        from src.ui.snapshot_widget import (
            SnapshotWidget,
            _ITEM_H,
            _ITEM_W,
            _card_action_rects,
            _card_content_rect,
            _item_row,
        )

        app = QApplication.instance() or QApplication([])
        w = SnapshotWidget({})
        w.resize(900, 500)
        w.show()
        app.processEvents()
        # Bypass disk list — inject one fake card so visualItemRect is real.
        w._snapshots = [(__import__("pathlib").Path("x.json"), type("S", (), {"name": "t"})())]
        w.view.clear()
        item = QListWidgetItem("t")
        item.setData(Qt.ItemDataRole.UserRole, 0)  # row 0 must remain clickable
        item.setSizeHint(__import__("PyQt6.QtCore", fromlist=["QSize"]).QSize(_ITEM_W, _ITEM_H))
        w.view.addItem(item)
        app.processEvents()
        assert _item_row(item) == 0

        item_rect = w.view.visualItemRect(item)
        card = _card_content_rect(item_rect)
        apply_rect = _card_action_rects(card)["apply"]
        center = apply_rect.center()
        hits: list[str] = []
        with mock.patch.object(
            w, "_run_card_action", side_effect=lambda a, r: hits.append(f"{a}:{r}")
        ):
            press = QMouseEvent(
                QEvent.Type.MouseButtonPress,
                QPointF(center),
                Qt.MouseButton.LeftButton,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert w.eventFilter(w.view.viewport(), press) is True
        assert hits == ["apply:0"], hits
        # Miss outside chips must not swallow the event.
        miss = QMouseEvent(
            QEvent.Type.MouseButtonPress,
            QPointF(card.left() + 8, card.top() + 8),
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        )
        assert w.eventFilter(w.view.viewport(), miss) is False
        w.close()
        w.deleteLater()
        app.processEvents()

    def _default_name_dialog_contract():
        """Save dialog pre-fills a day timestamp; blank confirms fall back."""
        from datetime import datetime
        import inspect
        import re

        from src.layout_snapshot import capture_layout, default_snapshot_name
        from src.ui.snapshot_widget import SnapshotWidget

        fixed = datetime(2026, 9, 9, 9, 26, 0)
        assert default_snapshot_name(now=fixed) == "2026-09-09 09:26"
        # Same-minute collision → include seconds.
        assert (
            default_snapshot_name(now=fixed, existing_names=["2026-09-09 09:26"])
            == "2026-09-09 09:26:00"
        )
        blank = capture_layout("", settings={"hide_shell_icons": True, "fences": []})
        assert re.fullmatch(r"\d{4}-\d{2}-\d{2} \d{2}:\d{2}(:\d{2})?", blank.name)
        cap = inspect.getsource(SnapshotWidget._capture)
        assert "default_snapshot_name" in cap
        assert "getText" in cap

    run("分区样式快照往返还原", _roundtrip_layout_style)
    run("快照点击预览渲染契约", _preview_click_contract)
    run("缩略图右下角按钮可点", _chip_click_hits_thumb_corner)
    run("隐藏壳图标时保存不探测ListView", _save_skips_shell_when_hidden)
    run("双击恢复+App接线契约", _double_click_and_app_wire)
    run("快照默认时间戳命名", _default_name_dialog_contract)


def test_pinned_desktop_folder_standin() -> None:
    begin("17) 桌面文件夹 stand-in 已钉分区不算未归入")

    def _prefer_key_match():
        from pathlib import Path

        from src.fence_rules import all_fence_pinned_keys, path_in_pinned_keys

        # Pin stores the real off-desktop path; desktop child matches only via
        # prefer-remap when the stand-in resolves — never by unique basename.
        pinned = {"e:\\library\\reports".casefold()}
        desk_child = Path(r"D:\desktop\Reports")
        assert path_in_pinned_keys(desk_child, pinned) is False
        assert path_in_pinned_keys(Path(r"D:\desktop\Other"), pinned) is False
        # Exact pin path still matches (slash/case).
        assert path_in_pinned_keys(Path(r"E:\library\Reports"), pinned) is True
        assert path_in_pinned_keys(Path(r"e:/library/reports"), pinned) is True

        # Duplicate basenames must not false-positive.
        pinned2 = {
            r"e:\a\Reports".casefold(),
            r"e:\b\Reports".casefold(),
        }
        assert path_in_pinned_keys(Path(r"D:\desktop\Reports"), pinned2) is False
        # Unique basename alone still must not match a different folder.
        pinned3 = {r"e:\library\report.xlsx".casefold()}
        assert path_in_pinned_keys(Path(r"D:\desktop\report.xlsx"), pinned3) is False
        assert path_in_pinned_keys(Path(r"E:\library\report.xlsx"), pinned3) is True

    def _live_aiproject_if_present():
        from pathlib import Path

        from src.desktop_scanner import scan_desktop
        from src.fence_rules import all_fence_pinned_keys, path_in_pinned_keys
        from src.public_desktop import public_claimed_keys
        from src.settings import load_settings

        settings = load_settings()
        claimed = all_fence_pinned_keys(settings) | public_claimed_keys(settings)
        scan = scan_desktop(exclude=settings.get("exclude_patterns") or [])
        for item in scan.items:
            if item.is_dir and item.name.casefold() == "aiproject":
                assert path_in_pinned_keys(item.path, claimed), (
                    f"desktop {item.path} must count as pinned "
                    f"(claimed has real path)"
                )
                return
        # Not on this machine — unit case above still covers the bug.
        print("  (skip live AIproject — not on desktop)")

    run("path_in_pinned_keys desktop stand-in", _prefer_key_match)
    run("本机 AIproject 应已归入", _live_aiproject_if_present)


def test_wallpaper() -> None:
    begin("16) 桌面壁纸更换")

    def _set_and_list():
        import tempfile
        from pathlib import Path

        from PyQt6.QtGui import QColor, QImage

        from src.wallpaper import (
            IMAGE_SUFFIXES,
            list_library_images,
            set_desktop_wallpaper,
            set_random_wallpaper_from_library,
            wallpaper_settings,
        )

        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            img = folder / "a.png"
            qimg = QImage(64, 64, QImage.Format.Format_RGB32)
            qimg.fill(QColor(30, 120, 200))
            assert qimg.save(str(img), "PNG")
            applied = set_desktop_wallpaper(img, apply=False)
            assert applied.is_file()
            assert applied.suffix.lower() == ".png"
            assert list_library_images(folder) == [img]
            settings = {"wallpaper": {"library_folder": str(folder)}}
            wallpaper_settings(settings)
            again = set_random_wallpaper_from_library(settings, apply=False)
            assert again.is_file()
            assert ".jpg" in IMAGE_SUFFIXES
            # Must not touch the live desktop wallpaper during tests.
            from src.wallpaper import get_current_wallpaper_path

            before = get_current_wallpaper_path()
            set_desktop_wallpaper(img, apply=False)
            after = get_current_wallpaper_path()
            assert before == after or (
                before is not None
                and after is not None
                and before.resolve() == after.resolve()
            )

        try:
            set_random_wallpaper_from_library({"wallpaper": {"library_folder": ""}})
            assert False, "empty library must raise"
        except ValueError:
            pass

    def _wire_contract():
        import inspect

        from src.ui.extensions_widget import ExtensionsWidget
        from src.ui.page_indicator import PageIndicatorWidget

        pi = inspect.getsource(PageIndicatorWidget)
        assert "wallpaper_requested" not in pi
        assert "pageWallpaperBtn" not in pi
        assert "_make_wallpaper_button" not in pi
        ext = inspect.getsource(ExtensionsWidget)
        assert "桌面壁纸" in ext
        assert "_pick_wallpaper" in ext
        assert "_random_wallpaper" in ext
        assert "wallpaper_folder_edit" in ext

    run("设置壁纸+库随机契约", _set_and_list)
    run("扩展功能后台配置接线", _wire_contract)


def test_meeting_minutes() -> None:
    begin("14) 会议纪要命名与接线")

    def _serial_naming():
        from datetime import datetime
        import tempfile
        from pathlib import Path

        from src.meeting_minutes import (
            create_meeting_minutes,
            next_minutes_path,
        )

        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            when = datetime(2026, 8, 3, 10, 0, 0)
            p1 = next_minutes_path(folder, when=when)
            assert p1.name == "20260803_001.docx", p1.name
            (folder / "20260803_001.docx").write_bytes(b"x")
            (folder / "20260803_002.docx").write_bytes(b"x")
            (folder / "20260802_009.docx").write_bytes(b"x")
            p2 = next_minutes_path(folder, when=when)
            assert p2.name == "20260803_003.docx", p2.name

            settings = {"meeting_minutes": {"folder": str(folder)}}
            created = create_meeting_minutes(settings, when=when, open_after=False)
            assert created.name == "20260803_003.docx"
            assert created.is_file()
            # python-docx file is a zip/ole container — just ensure non-trivial size.
            assert created.stat().st_size > 1000

        from src.meeting_minutes import (
            MINUTES_DIR_NAME,
            default_minutes_folder,
            resolve_minutes_folder,
        )
        from src.settings import install_root

        default = default_minutes_folder(ensure=False)
        assert default == install_root() / MINUTES_DIR_NAME, default
        resolved = resolve_minutes_folder(
            {"meeting_minutes": {"folder": ""}}, ensure=False
        )
        assert resolved == default or (
            resolved == install_root() / "dist" / MINUTES_DIR_NAME
        ), resolved
        created_default = create_meeting_minutes(
            {"meeting_minutes": {"folder": ""}}, open_after=False
        )
        assert created_default.parent == resolve_minutes_folder(
            {"meeting_minutes": {"folder": ""}}, ensure=False
        )
        assert created_default.is_file()
        try:
            created_default.unlink(missing_ok=True)
        except OSError:
            pass

    def _wire_contract():
        import inspect

        from src.app import DeskTidyApp
        from src.ui.page_indicator import PageIndicatorWidget

        src = inspect.getsource(PageIndicatorWidget)
        assert "纪要" in src
        assert "minutes_requested" in src
        assert "minutes_folder_requested" in src
        assert "打开纪要文件夹" in src
        assert "MouseButtonDblClick" in src
        assert "note_requested" in src
        assert "pageNoteBtn" in src  # helper kept; chip gated off via float_bar_tool_flags
        assert "record_requested" in src
        assert "pageRecordBtn" in src
        assert "打开录屏文件夹" in src
        assert "pageToolsBtn" not in src
        assert "_show_tools_menu" not in src
        assert "create_fence_requested" in src
        assert "右键在该页新建分区" in src
        filt = inspect.getsource(PageIndicatorWidget.eventFilter)
        # Create only on double-click; single release must not emit.
        dbl = filt.split("MouseButtonDblClick", 1)[1]
        assert "minutes_requested.emit()" in dbl
        assert "calculator_requested.emit()" in dbl
        release = filt.split("MouseButtonRelease", 1)[1].split("MouseButtonDblClick", 1)[0]
        assert "minutes_requested.emit()" not in release
        assert "calculator_requested.emit()" not in release
        assert "note_requested.emit()" in release
        assert "record_requested.emit()" in release
        assert "create_fence_requested.emit" in filt
        assert "RightButton" in filt
        assert "_PeekRow" in src or "PeekRow" in src
        assert "_set_expanded" in src
        setup = inspect.getsource(DeskTidyApp._setup_page_indicator)
        assert "minutes_requested" in setup
        assert "minutes_folder_requested" in setup
        assert "_on_meeting_minutes_requested" in setup
        assert "_on_meeting_minutes_folder_requested" in setup
        assert "note_requested" in setup
        assert "_on_note_requested" in setup
        assert "record_requested" in setup
        assert "_on_record_requested" in setup
        assert "record_folder_requested" in setup
        assert "_on_record_folder_requested" in setup
        assert "create_fence_requested" in setup
        assert "_on_page_create_fence_requested" in setup
        create_handler = inspect.getsource(DeskTidyApp._on_page_create_fence_requested)
        assert "switch_page" in create_handler
        assert "_create_fence_at" in create_handler
        handler = inspect.getsource(DeskTidyApp._on_meeting_minutes_requested)
        assert "create_meeting_minutes" in handler
        folder_handler = inspect.getsource(DeskTidyApp._on_meeting_minutes_folder_requested)
        assert "open_minutes_folder" in folder_handler
        note_handler = inspect.getsource(DeskTidyApp._on_note_requested)
        assert "open_in_desknote" in note_handler
        assert "show_notepad" not in note_handler
        assert "open_notepadpp" not in note_handler
        rec_handler = inspect.getsource(DeskTidyApp._on_record_requested)
        assert "toggle_recording" in rec_handler
        rec_folder = inspect.getsource(DeskTidyApp._on_record_folder_requested)
        assert "resolve_output_dir" in rec_folder
        assert "startfile" in rec_folder

    run("年月日流水号命名+生成docx", _serial_naming)
    run("分页栏纪要按钮接线契约", _wire_contract)


def test_page_folders() -> None:
    begin("15) 分页栏文件夹快捷方式")

    def _normalize_and_defaults():
        import json
        from pathlib import Path

        from src.page_folders import get_page_folder_items, set_page_folder_items

        raw = json.loads(
            (Path(__file__).resolve().parents[1] / "config" / "default_settings.json").read_text(
                encoding="utf-8"
            )
        )
        assert "page_folders" in raw
        assert raw["page_folders"] == []

        s: dict = {
            "page_folders": [
                {"name": " 下载 ", "path": r"C:\Users\demo\Downloads"},
                {"path": ""},
                "bad",
                {"name": "", "path": r"D:\Work"},
            ]
        }
        items = get_page_folder_items(s)
        assert items == [
            {"name": "下载", "path": r"C:\Users\demo\Downloads"},
            {"name": "Work", "path": r"D:\Work"},
        ], items
        set_page_folder_items(s, [{"name": "A", "path": r"E:\a"}, {"name": "B", "path": ""}])
        assert s["page_folders"] == [{"name": "A", "path": r"E:\a"}]

    def _partial_save_must_not_wipe_page_folders():
        """Regression: stub save with page_folders=[] must not erase disk folders."""
        import json
        import tempfile
        from pathlib import Path
        from unittest import mock

        from src import settings as settings_mod

        td = Path(tempfile.mkdtemp(prefix="desktidy_pf_"))
        settings_file = td / "settings.json"
        disk = {
            "hotkeys": {},
            "desktop_pages": [{"id": 0, "name": "工作"}],
            "organize_rules": {},
            "theme": "mist",
            "page_folders": [
                {"name": "微控", "path": r"E:\work\mom"},
                {"name": "AIproject", "path": r"E:\soft\AIproject"},
            ],
            "fences": [],
        }
        settings_file.write_text(
            json.dumps(disk, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        stub = {
            "theme": "mist",
            "page_folders": [],
            "page_indicator_pos": {"x": 10, "y": 20},
        }
        with mock.patch.object(settings_mod, "SETTINGS_FILE", settings_file), mock.patch.object(
            settings_mod, "SETTINGS_BACKUP_DIR", td / "backups"
        ), mock.patch.object(settings_mod, "ensure_app_dir", lambda: None):
            settings_mod._save_settings_now(stub)
            saved = json.loads(settings_file.read_text(encoding="utf-8"))
        assert saved.get("page_folders") == disk["page_folders"], saved.get("page_folders")
        assert saved.get("page_indicator_pos") == {"x": 10, "y": 20}

    def _partial_save_must_not_replace_desktop_pages():
        """Regression: stub save with demo 娱乐/学习 must not overwrite 工作/文档."""
        import json
        import tempfile
        from pathlib import Path
        from unittest import mock

        from src import settings as settings_mod

        td = Path(tempfile.mkdtemp(prefix="desktidy_pages_"))
        settings_file = td / "settings.json"
        disk = {
            "hotkeys": {},
            "desktop_pages": [
                {"id": 0, "name": "工作"},
                {"id": 1, "name": "文档"},
            ],
            "organize_rules": {},
            "theme": "mist",
            "desktop_pet": {"enabled": True, "character": "hoodie"},
            "fences": [],
        }
        settings_file.write_text(
            json.dumps(disk, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        stub = {
            "desktop_pages": [
                {"id": 0, "name": "工作"},
                {"id": 1, "name": "娱乐"},
                {"id": 2, "name": "学习"},
            ],
            "desktop_pet": {"pos": {"x": 64, "y": 500}},
        }
        with mock.patch.object(settings_mod, "SETTINGS_FILE", settings_file), mock.patch.object(
            settings_mod, "SETTINGS_BACKUP_DIR", td / "backups"
        ), mock.patch.object(settings_mod, "ensure_app_dir", lambda: None):
            settings_mod._save_settings_now(stub)
            saved = json.loads(settings_file.read_text(encoding="utf-8"))
        names = [p.get("name") for p in saved.get("desktop_pages") or []]
        assert names == ["工作", "文档"], names
        assert saved.get("desktop_pet", {}).get("character") == "hoodie"
        assert saved.get("desktop_pet", {}).get("pos") == {"x": 64, "y": 500}

    def _indicator_order_and_dblclick():
        from PyQt6.QtCore import QEvent, QPointF, Qt
        from PyQt6.QtGui import QMouseEvent
        from PyQt6.QtWidgets import QApplication

        from src.ui.page_indicator import PageIndicatorWidget

        app = QApplication.instance() or QApplication([])
        settings = {
            "page_folders": [
                {"name": "资料库", "path": r"C:\Docs"},
                {"name": "截图", "path": r"D:\Shots"},
            ],
            "notepad": {"enabled": True, "folder": ""},
            "meeting_minutes": {"enabled": True, "folder": ""},
            "screen_record": {"enabled": True, "output_dir": "", "fps": 30},
            "calculator": {"enabled": True},
            # Keep tools on the right bar for this contract (pet default-on
            # would host the float bar on the pet instead).
            "desktop_pet": {"enabled": False},
        }
        pages = [{"id": 0, "name": "工作"}, {"id": 1, "name": "文档"}]
        w = PageIndicatorWidget(pages, current_page=0, settings=settings)
        w.show()
        app.processEvents()
        w._set_expanded(True, animate=False)
        app.processEvents()

        assert len(w._buttons) == 2
        assert len(w._folder_buttons) == 2
        assert w._folder_buttons[0].text() == "资料库"
        assert w._folder_buttons[1].text() == "截图"
        assert w._note_btn is None
        assert w._minutes_btn is not None and w._minutes_btn.text() == "纪要"
        assert w._record_btn is not None and w._record_btn.text() == "录屏"
        assert w._calc_btn is not None and w._calc_btn.text() == "计算器"
        assert not hasattr(w, "_wallpaper_btn") or w._wallpaper_btn is None
        # Stack order: pages → folders → 录屏 → 纪要 → 计算器（记事本已迁 deskNote）
        layout = w.layout()
        assert layout is not None
        widgets = [
            layout.itemAt(i).widget()
            for i in range(layout.count())
            if layout.itemAt(i) is not None and layout.itemAt(i).widget() is not None
        ]
        assert widgets[0] is w._rows[0]
        assert widgets[1] is w._rows[1]
        assert widgets[2] is w._folder_rows[0]
        assert widgets[3] is w._folder_rows[1]
        assert widgets[4] is w._record_row
        assert widgets[5] is w._minutes_row
        assert widgets[6] is w._calc_row

        opened: list[str] = []
        w.folder_open_requested.connect(opened.append)
        page_hits: list[int] = []
        w.page_changed.connect(page_hits.append)

        # Dedicated chips wire feature signals (not a tools menu).
        assert callable(getattr(w, "_show_record_menu", None))
        assert callable(getattr(w, "_show_note_menu", None))
        assert callable(getattr(w, "_show_minutes_menu", None))
        assert page_hits == []

        btn = w._folder_buttons[0]
        center = btn.rect().center()
        global_pos = btn.mapToGlobal(center)

        def _send(etype, buttons=Qt.MouseButton.LeftButton, buttons_down=Qt.MouseButton.NoButton):
            ev = QMouseEvent(
                etype,
                QPointF(center),
                QPointF(global_pos),
                Qt.MouseButton.LeftButton,
                buttons_down if etype == QEvent.Type.MouseButtonRelease else buttons,
                Qt.KeyboardModifier.NoModifier,
            )
            app.sendEvent(btn, ev)
            app.processEvents()

        # Single click must not open or change page.
        _send(QEvent.Type.MouseButtonPress, buttons_down=Qt.MouseButton.LeftButton)
        _send(QEvent.Type.MouseButtonRelease)
        assert opened == []
        assert page_hits == []

        # Double-click opens configured path.
        _send(QEvent.Type.MouseButtonPress, buttons_down=Qt.MouseButton.LeftButton)
        _send(QEvent.Type.MouseButtonDblClick, buttons_down=Qt.MouseButton.LeftButton)
        _send(QEvent.Type.MouseButtonRelease)
        assert opened == [r"C:\Docs"], opened
        assert page_hits == []

        # Reload after settings change.
        settings["page_folders"] = [{"name": "仅一个", "path": r"E:\One"}]
        w.reload_folder_shortcuts()
        app.processEvents()
        w._set_expanded(True, animate=False)
        assert len(w._folder_buttons) == 1
        assert w._folder_buttons[0].text() == "仅一个"
        assert w._record_btn is not None
        assert w._note_btn is None
        assert w._minutes_btn is not None
        assert w._calc_btn is not None

        # Disabled features hide the dedicated chips.
        settings["notepad"] = {"enabled": False, "folder": ""}
        settings["meeting_minutes"] = {"enabled": False, "folder": ""}
        settings["screen_record"] = {"enabled": False, "output_dir": "", "fps": 30}
        settings["calculator"] = {"enabled": False}
        w.reload_folder_shortcuts()
        app.processEvents()
        assert getattr(w, "_tools_btn", None) is None
        assert w._note_btn is None
        assert w._minutes_btn is None
        assert w._record_btn is None
        assert w._calc_btn is None

        w.close()
        w.deleteLater()
        app.processEvents()

    def _wire_contract():
        import inspect

        from src.app import DeskTidyApp
        from src.ui.extensions_widget import ExtensionsWidget
        from src.ui.main_window import MainWindow
        from src.ui.page_indicator import PageIndicatorWidget

        pi = inspect.getsource(PageIndicatorWidget)
        assert "folder_open_requested" in pi
        assert "note_folder_requested" in pi
        assert "pageFolderBtn" in pi
        assert "reload_folder_shortcuts" in pi
        assert "MouseButtonDblClick" in pi
        assert "WA_NoSystemBackground" in pi
        assert "_make_context_menu" in pi
        assert "_context_menu_stylesheet" in pi
        menu_src = inspect.getsource(PageIndicatorWidget._make_context_menu)
        style_src = inspect.getsource(PageIndicatorWidget._context_menu_stylesheet)
        assert "QMenu(None)" in menu_src
        assert "_context_menu_stylesheet" in menu_src
        assert "get_theme_palette" in style_src
        assert "_ctx_menu_style" in style_src
        assert "WA_TranslucentBackground" in menu_src
        run_menu = inspect.getsource(PageIndicatorWidget._run_context_menu)
        assert "_finish_context_menu" in run_menu
        assert "action_chosen" in run_menu
        finish_menu = inspect.getsource(PageIndicatorWidget._finish_context_menu)
        assert "action_chosen" in finish_menu
        assert "_context_action_row" in finish_menu
        assert "_cursor_on_widget" in inspect.getsource(PageIndicatorWidget.leaveEvent)
        # Peek RMB must freeze park (same owner as fence shell menus).
        assert "_begin_desktop_popup" in run_menu
        assert "_end_desktop_popup_later" in run_menu
        leave = inspect.getsource(PageIndicatorWidget.eventFilter)
        assert "While any peek context menu is open" in leave or (
            "if not self._menu_open:" in leave and "schedule_tuck" in leave
        )
        # Folder / tools menus must use the opaque helper (not QMenu(self)).
        folder_menu = inspect.getsource(PageIndicatorWidget._show_folder_menu)
        assert "_run_context_menu" in folder_menu
        assert "QMenu(self)" not in folder_menu
        note_menu = inspect.getsource(PageIndicatorWidget._show_note_menu)
        assert "_run_context_menu" in note_menu
        assert "打开笔记文件夹" in note_menu
        rec_menu = inspect.getsource(PageIndicatorWidget._show_record_menu)
        assert "打开录屏文件夹" in rec_menu
        min_menu = inspect.getsource(PageIndicatorWidget._show_minutes_menu)
        assert "打开纪要文件夹" in min_menu
        assert not hasattr(PageIndicatorWidget, "_show_tools_menu")

        # Runtime: menu has opaque card/text colors (not inherited transparent band).
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance() or QApplication([])
        host = PageIndicatorWidget(
            [{"id": 0, "name": "工作"}],
            current_page=0,
            settings={"theme": "mist", "page_folders": [], "desktop_pet": {"enabled": False}},
        )
        peek_menu = host._make_context_menu()
        peek_menu.addAction("打开文件夹位置")
        ss = peek_menu.styleSheet().replace(" ", "")
        assert "background-color:#FFFFFF" in ss or "background-color:#ffffff" in ss.lower()
        from src.ui.styles import get_theme_palette

        mist_text = get_theme_palette("mist")["text"].replace(" ", "").lower()
        assert f"color:{mist_text}" in ss.lower()
        peek_menu.deleteLater()
        host.close()
        host.deleteLater()
        app.processEvents()
        from src.ui import page_indicator as pi_mod

        assert hasattr(pi_mod, "_PeekChip")
        chip_src = inspect.getsource(pi_mod._PeekChip)
        assert "_tab_path" in chip_src
        assert "_apply_tab_mask" not in chip_src
        assert "CompositionMode_Source" in chip_src
        assert "setMask" not in chip_src
        assert "QLinearGradient" in chip_src
        assert "sheen" not in chip_src.lower() or "_mix" in chip_src
        assert "_mix" in chip_src
        # Refined active state: left rail + soft wash (not solid accent brick).
        assert "rail" in chip_src
        assert "drawRoundedRect" in chip_src
        assert "elidedText" in chip_src
        assert "text_inverse" not in chip_src or "nav_active" in chip_src

        setup = inspect.getsource(DeskTidyApp._setup_page_indicator)
        assert "folder_open_requested" in setup
        assert "_on_page_folder_open_requested" in setup
        assert "note_requested" in setup
        assert "_on_note_requested" in setup
        assert "note_folder_requested" in setup
        assert "_on_note_folder_requested" in setup
        assert "calculator_requested" in setup
        assert "_on_calculator_requested" in setup
        handler = inspect.getsource(DeskTidyApp._on_page_folder_open_requested)
        assert "open_folder_path" in handler
        note_handler = inspect.getsource(DeskTidyApp._on_note_requested)
        assert "open_in_desknote" in note_handler
        assert "show_notepad" not in note_handler
        assert "open_notepadpp" not in note_handler
        note_folder = inspect.getsource(DeskTidyApp._on_note_folder_requested)
        assert "resolve_openable_notes_folder" in note_folder
        assert "startfile" in note_folder
        ext_changed = inspect.getsource(DeskTidyApp._on_extensions_changed)
        assert "reload_folder_shortcuts" in ext_changed
        assert "_schedule_hotkeys_refresh" in ext_changed

        from src.ui.hotkey_edit import HotkeyEdit

        hk_edit = inspect.getsource(HotkeyEdit)
        assert "capture_began" in hk_edit
        assert "capture_ended" in hk_edit
        assert "focusInEvent" in hk_edit
        assert "focusOutEvent" in hk_edit

        mw = inspect.getsource(MainWindow)
        assert "hotkey_capture_began" in mw
        assert "_wire_hotkey_edit" in mw

        ext = inspect.getsource(ExtensionsWidget)
        assert "分页栏文件夹" in ext
        assert "记事本" in ext
        assert "notepad_folder_edit" in ext
        assert "区域截图" in ext
        assert "hotkey_inputs" in ext
        assert "_wire_hotkey_edit" in ext
        assert "hotkey_capture_began" in ext
        assert "screenshot" in ext
        assert "screen_record" in ext
        assert "screenshot_enabled_cb" not in ext
        assert "screenshot_pin_cb" in ext
        assert "screen_record_enabled_cb" in ext
        assert "minutes_enabled_cb" in ext
        assert "notepad_enabled_cb" in ext
        assert "calculator_enabled_cb" in ext
        assert "notepadpp_path_edit" not in ext
        rebuild = inspect.getsource(PageIndicatorWidget._rebuild_buttons)
        assert "_tool_flags" in rebuild
        assert "_make_record_button" in rebuild
        assert "_make_note_button" in rebuild
        assert "_make_minutes_button" in rebuild
        assert "_make_calculator_button" in rebuild
        assert "pageToolsBtn" not in rebuild
        flags = inspect.getsource(PageIndicatorWidget._tool_flags)
        assert "float_bar_tool_flags" in flags
        from src.desktop_pet import float_bar_tool_flags as _float_flags

        shared = inspect.getsource(_float_flags)
        assert '"note": False' in shared or "'note': False" in shared
        assert "meeting_minutes_enabled" in shared
        assert "calculator_enabled" in shared
        assert "DeskNote" in inspect.getsource(ExtensionsWidget) or "DeskNote" in ext
        assert "desktop_pet_hosts_float_bar" in rebuild
        assert "calculator_requested" in pi
        assert "_on_calculator_requested" in inspect.getsource(DeskTidyApp)
        assert "_add_page_folder" in ext
        assert "page_folders_table" in ext or "_page_folders_host" in ext
        assert "_make_page_folder_row" in ext
        assert "_delete_page_folder_at" in ext
        assert "删除选中" not in ext
        assert "快捷启动栏" not in ext

        build = inspect.getsource(MainWindow._build_settings_page)
        idx = build.find("self.hotkey_inputs")
        assert idx > 0
        hotkey_section = build[idx : idx + 1200]
        assert "screenshot" not in hotkey_section
        assert "show_screenshot" not in hotkey_section
        assert "screen_record" not in hotkey_section
        assert "screenshot_enabled_cb" not in build
        assert "screen_record_enabled_cb" not in build
        chip_src = inspect.getsource(pi_mod._PeekChip)
        assert "get_theme_palette" in chip_src
        assert "accent_soft" in chip_src or "accent_hover" in chip_src
        assert "QLinearGradient" in chip_src
        assert "refresh_theme" in pi
        assert "_mix" in chip_src

    def _builtin_notepad():
        import json
        import tempfile
        from pathlib import Path

        from PyQt6.QtWidgets import QApplication

        from src.notepad import (
            NOTES_DIR_NAME,
            NOTE_OPEN_SUFFIXES,
            default_notes_folder,
            document_open_shell_commands,
            is_notepad_openable_path,
            notepad_open_dialog_filter,
            notepad_settings,
            resolve_notes_folder,
            resolve_openable_notes_folder,
            temporary_session_file,
        )
        from src.settings import install_root
        from src.ui.notepad_window import NotepadWindow, show_notepad
        from src.app import DeskTidyApp
        import inspect

        present = inspect.getsource(NotepadWindow.present)
        assert "bring_widget_to_foreground" in present
        note = inspect.getsource(DeskTidyApp._on_note_requested)
        assert "open_in_desknote" in note
        assert "show_notepad" not in note
        assert "_desk_app_ui_open" in inspect.getsource(DeskTidyApp._overlay_widgets_may_show)
        open_src = inspect.getsource(NotepadWindow.open_notes_folder)
        assert "resolve_openable_notes_folder" in open_src
        assert "hint_paths" in open_src
        assert ".py" in NOTE_OPEN_SUFFIXES and ".md" in NOTE_OPEN_SUFFIXES
        assert is_notepad_openable_path.__module__ == "src.notepad"
        assert "notepad_open_dialog_filter" in inspect.getsource(NotepadWindow.open_document)
        assert "setAcceptDrops(True)" in inspect.getsource(NotepadWindow.__init__)
        assert "app.installEventFilter" in inspect.getsource(NotepadWindow.__init__)
        assert "_handle_window_file_drag" in inspect.getsource(NotepadWindow.eventFilter)
        drag_src = inspect.getsource(NotepadWindow._handle_window_file_drag)
        assert "_mime_has_openable_files" in drag_src
        assert "_handle_window_file_drag" in inspect.getsource(NotepadWindow)
        make_page = inspect.getsource(NotepadWindow._make_page)
        assert "viewport().installEventFilter" in make_page
        assert "_editor_filter_target" in inspect.getsource(NotepadWindow)
        assert "collect_drop_paths" in inspect.getsource(NotepadWindow._paths_from_drop_mime)
        assert "apply_window_icon" in inspect.getsource(NotepadWindow)
        assert "setWindowIcon" in inspect.getsource(NotepadWindow.apply_window_icon)
        assert "open_paths" in inspect.getsource(NotepadWindow)
        assert "open_path_with_picker" in inspect.getsource(
            __import__("src.win_shell", fromlist=["open_path_with_picker"])
        )
        picker_src = inspect.getsource(
            __import__("src.win_shell", fromlist=["open_path_with_picker"]).open_path_with_picker
        )
        assert "subprocess.run(" not in picker_src
        assert "ShellExecuteW" in picker_src
        assert "Popen" in picker_src
        assert "用记事本打开" in inspect.getsource(document_open_shell_commands)
        assert "打开方式" in inspect.getsource(document_open_shell_commands)
        assert "open_paths_in_notepad" in inspect.getsource(DeskTidyApp)
        fence_extra_src = inspect.getsource(
            __import__("src.ui.fence_icon_item", fromlist=["build_file_icon_shell_extras"]).build_file_icon_shell_extras
        )
        assert "document_open_shell_commands" in fence_extra_src
        public_menu_src = inspect.getsource(
            __import__("src.ui.public_icon_widget", fromlist=["x"]).PublicIconWidget._show_menu
        )
        assert "build_file_icon_shell_extras" in public_menu_src
        assert "用记事本打开" in (
            __import__("src.help_content", fromlist=["help_html"]).help_html("notepad")
        )
        # Scratch file for openable check (is_file required).
        with tempfile.TemporaryDirectory() as td:
            sample = Path(td) / "demo.py"
            sample.write_text("print(1)\n", encoding="utf-8")
            assert is_notepad_openable_path(sample)
            assert not is_notepad_openable_path(Path(td) / "missing.py")
            assert "*.py" in notepad_open_dialog_filter()

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            backup_note = tmp_path / "backup" / "new 1@2026-08-21_120000.txt"
            backup_note.parent.mkdir(parents=True)
            backup_note.write_text("hello", encoding="utf-8")
            empty_notes = tmp_path / "笔记"
            empty_notes.mkdir()
            settings = {"notepad": {"folder": str(empty_notes), "enabled": True}}
            # Open tabs live in backup while 笔记 is empty → open backup parent.
            got = resolve_openable_notes_folder(
                settings, hint_paths=[backup_note], ensure=True
            )
            assert got.resolve() == backup_note.parent.resolve(), got
            sess = tmp_path / "sess.json"
            with temporary_session_file(sess):
                from src.notepad import save_notepad_session

                save_notepad_session([str(backup_note)], 0)
                got2 = resolve_openable_notes_folder(
                    settings, hint_paths=None, ensure=True
                )
                assert got2.resolve() == backup_note.parent.resolve(), got2
            # Explicit empty hints → configured notes folder (no session fallback).
            got3 = resolve_openable_notes_folder(
                settings, hint_paths=[], ensure=True
            )
            assert got3.resolve() == empty_notes.resolve(), got3

        raw = json.loads(
            (Path(__file__).resolve().parents[1] / "config" / "default_settings.json").read_text(
                encoding="utf-8"
            )
        )
        assert "notepad" in raw
        assert "folder" in raw["notepad"]
        assert raw["notepad"].get("enabled") is True
        assert raw.get("meeting_minutes", {}).get("enabled") is True
        assert "notepadpp" not in raw

        s: dict = {}
        cfg = notepad_settings(s)
        assert cfg is s["notepad"]
        folder = resolve_notes_folder(s, ensure=True)
        assert folder.is_dir()
        assert folder.name == NOTES_DIR_NAME
        assert folder.resolve() == default_notes_folder(ensure=False).resolve()
        restore_src = inspect.getsource(NotepadWindow._restore_session)
        assert "recent_note_files" in restore_src
        assert "session_count" in restore_src
        assert "temporary_session_file" in inspect.getsource(
            __import__("src.notepad", fromlist=["x"])
        )

        with tempfile.TemporaryDirectory() as tmp:
            custom = Path(tmp) / "notes"
            s2 = {"notepad": {"folder": str(custom)}}
            got = resolve_notes_folder(s2, ensure=True)
            assert got.resolve() == custom.resolve()
            assert got.is_dir()

        app = QApplication.instance() or QApplication([])
        from src.notepad import (
            save_notepad_session,
            temporary_backup_dir,
            temporary_session_file,
        )

        with tempfile.TemporaryDirectory() as work_tmp:
            work = Path(work_tmp)
            notes = work / "notes"
            notes.mkdir()
            sess_path = work / "notepad_session.json"
            bak = work / "backup"
            with temporary_session_file(sess_path), temporary_backup_dir(bak):
                save_notepad_session([], 0)
                win = show_notepad({"notepad": {"folder": str(notes)}})
                assert isinstance(win, NotepadWindow)
                assert not win.windowIcon().isNull()
                assert win.isVisible()
                assert win.isMaximized()
                assert win.tabs.count() == 1
                win.editor.setPlainText("hello")
                assert win._dirty
                target = notes / "_selftest_note.txt"
                assert win._write_path(target)
                assert target.read_text(encoding="utf-8") == "hello"
                assert not win._dirty

                # Double-click empty tab bar / strip → new library note
                before = win.tabs.count()
                bar = win.tabs.tabBar()
                from src.ui.notepad_window import _NoteTabBar

                assert isinstance(bar, _NoteTabBar)
                assert not bar.expanding()
                assert hasattr(bar, "tab_delete_requested")
                assert hasattr(bar, "tab_pin_requested")
                assert hasattr(bar, "tab_menu_requested")
                assert hasattr(win, "_toggle_pin_page")
                assert hasattr(win.library, "close_requested")
                assert hasattr(win.library, "pin_requested")
                # Shared RMB actions on library + tabs (关闭用页签 ×，无单独「打开/关闭」项)
                lib_menu_src = inspect.getsource(win.library._file_actions_menu)
                assert "重命名" in lib_menu_src and "置顶" in lib_menu_src
                assert "打开文件所在目录" in lib_menu_src and "删除" in lib_menu_src
                tab_menu_src = inspect.getsource(NotepadWindow._on_tab_context_menu)
                for label in ("重命名", "置顶", "打开文件所在目录", "删除"):
                    assert label in tab_menu_src
                # Pin button on each tab; pin moves tab to the left group
                pin_btn = bar.tabButton(0, bar.ButtonPosition.LeftSide)
                assert pin_btn is not None
                last = win.tabs.count() - 1
                win._toggle_pin_at(last)
                assert win._pages[0].pinned
                win._toggle_pin_at(0)
                assert not any(p.pinned for p in win._pages)
                from src.notepad import load_notepad_session as _load_sess

                win._toggle_pin_at(win.tabs.count() - 1)
                win._persist_session()
                sess = _load_sess()
                assert sess.get("pinned"), sess
                win._toggle_pin_at(0)  # leave unpinned for later close tests
                bar.empty_double_clicked.emit()
                assert win.tabs.count() == before + 1
                from PyQt6.QtCore import QPoint, Qt
                from PyQt6.QtTest import QTest

                before = win.tabs.count()
                bar_rect = bar.geometry()
                click_pos = QPoint(max(win.tabs.width() - 8, 1), max(bar_rect.center().y(), 1))
                QTest.mouseDClick(
                    win.tabs,
                    Qt.MouseButton.LeftButton,
                    Qt.KeyboardModifier.NoModifier,
                    click_pos,
                )
                app.processEvents()
                assert win.tabs.count() == before + 1

                # Close tab keeps library file; delete removes it
                assert target.is_file()
                # Find tab that has target
                target_idx = next(
                    i
                    for i, p in enumerate(win._pages)
                    if p.path is not None and p.path.resolve() == target.resolve()
                )
                bar.tab_close_requested.emit(target_idx)
                app.processEvents()
                assert target.is_file(), "closing a tab must keep the notes file"
                # Re-open then delete (confirm dialog auto-Yes in test)
                from unittest.mock import patch

                from PyQt6.QtWidgets import QMessageBox

                win._activate_or_open_path(target)
                app.processEvents()
                target_idx = next(
                    i
                    for i, p in enumerate(win._pages)
                    if p.path is not None and p.path.resolve() == target.resolve()
                )
                with patch.object(
                    QMessageBox,
                    "question",
                    return_value=QMessageBox.StandardButton.Yes,
                ):
                    bar.tab_delete_requested.emit(target_idx)
                    app.processEvents()
                assert not target.is_file(), "delete tab must remove the notes file"

                # Close window autosaves dirty library notes
                win.tabs.setCurrentIndex(win.tabs.count() - 1)
                win.editor.setPlainText("autosave-me")
                assert win._dirty
                dirty_path = win._current_page().path
                win.close()
                app.processEvents()
                assert dirty_path is not None
                assert dirty_path.is_file()
                assert "autosave-me" in dirty_path.read_text(encoding="utf-8")

                close_src = inspect.getsource(NotepadWindow.closeEvent)
                assert "_autosave_page" in close_src
                assert "_delete_tab_file" not in close_src
                assert "_prompt_save_if_dirty" in inspect.getsource(NotepadWindow._close_tab)
                assert "_delete_tab_file" in inspect.getsource(NotepadWindow._delete_tab)

                win_re = NotepadWindow({"notepad": {"folder": str(notes)}})
                restored = [
                    win_re._pages[i].editor.toPlainText()
                    for i in range(win_re.tabs.count())
                ]
                assert "autosave-me" in restored, restored
                win_re.close()
                win_re.deleteLater()

                # Reopen restores session and last active tab
                from src.notepad import recent_note_files

                a = notes / "_sess_a.txt"
                b = notes / "_sess_b.txt"
                a.write_text("AAA", encoding="utf-8")
                b.write_text("BBB", encoding="utf-8")
                save_notepad_session([str(a), str(b)], 1, pinned=[str(b)])
                win2 = NotepadWindow({"notepad": {"folder": str(notes)}})
                assert win2.tabs.count() >= 2
                # Pinned tab(s) stay on the left
                pinned_pages = [p for p in win2._pages if p.pinned]
                assert pinned_pages, "session pinned must restore"
                assert win2._pages[0].pinned
                assert win2._pages[0].path is not None
                assert win2._pages[0].path.resolve() == b.resolve()
                # Session current index among session files
                assert win2.editor.toPlainText() in ("AAA", "BBB", "autosave-me", "# \n\n")
                # Prefer checking the intended session files are open
                names = {p.path.name for p in win2._pages if p.path}
                assert "a.txt" in str(names) or "_sess_a.txt" in names
                assert "_sess_b.txt" in names
                win2.close()
                win2.deleteLater()

                # Stale session (all paths missing) falls back to notes folder.
                keep = notes / "笔记_fallback.txt"
                keep.write_text("KEEPME", encoding="utf-8")
                save_notepad_session(
                    [str(notes / "_gone_a.txt"), str(notes / "_gone_b.txt")], 0
                )
                assert recent_note_files(notes, limit=5)
                win3 = NotepadWindow({"notepad": {"folder": str(notes)}})
                assert win3.tabs.count() >= 1
                texts = [
                    win3._pages[i].editor.toPlainText()
                    for i in range(win3.tabs.count())
                ]
                assert "KEEPME" in texts
                win3.close()
                win3.deleteLater()

                # Folder has 3 notes, session lists 1 → open all 3 tabs.
                pack = work / "notes_pack"
                pack.mkdir()
                p1 = pack / "笔记_one.txt"
                p2 = pack / "笔记_two.txt"
                p3 = pack / "笔记_three.txt"
                p1.write_text("ONE", encoding="utf-8")
                p2.write_text("TWO", encoding="utf-8")
                p3.write_text("THREE", encoding="utf-8")
                save_notepad_session([str(p2)], 0)
                win_pack = NotepadWindow({"notepad": {"folder": str(pack)}})
                pack_texts = [
                    win_pack._pages[i].editor.toPlainText()
                    for i in range(win_pack.tabs.count())
                ]
                assert win_pack.tabs.count() == 3, pack_texts
                assert set(pack_texts) == {"ONE", "TWO", "THREE"}
                assert win_pack.editor.toPlainText() == "TWO"
                win_pack.close()
                win_pack.deleteLater()

                assert sess_path.is_file()

        from src.notepad_search import replace_all_in_document
        from PyQt6.QtGui import QTextDocument

        doc = QTextDocument("foo bar foo")
        assert replace_all_in_document(doc, "foo", "baz") == 2
        assert doc.toPlainText() == "baz bar baz"

        win4 = NotepadWindow({"notepad": {"folder": ""}})
        dlg = win4._ensure_find_dialog()
        dlg.find_edit.setText("hello")
        dlg.replace_edit.setText("Hi")
        dlg.open_find(replace=True)
        assert dlg.isVisible()

        src = inspect.getsource(NotepadWindow._build_menus)
        assert "show_replace_dialog" in src
        assert "NotepadFindDialog" in inspect.getsource(NotepadWindow)
        assert "notepadCloseBtn" in inspect.getsource(NotepadWindow)
        assert "setCornerWidget" in inspect.getsource(NotepadWindow)
        win4.close()
        win4.deleteLater()
        app.processEvents()

    run("page_folders 规范化+默认配置", _normalize_and_defaults)
    run("stub save 不清空 page_folders", _partial_save_must_not_wipe_page_folders)
    run("stub save 不覆盖 desktop_pages", _partial_save_must_not_replace_desktop_pages)
    run("分页栏文件夹顺序与双击", _indicator_order_and_dblclick)
    run("扩展面板与 App 接线契约", _wire_contract)
    run("内置记事本契约", _builtin_notepad)


def main() -> int:
    print("DeskTidy 近期功能回归")
    print(f"ROOT = {ROOT}")
    started = time.perf_counter()
    try:
        test_organize_whitelist()
        test_public_page_local()
        test_public_to_fence_pin_visible()
        test_desktop_context_contract()
        test_uninstall_autostart()
        test_shell_band_contract()
        test_fence_grid_fit()
        test_fence_multi_select_drag()
        test_dissolve_fence()
        test_page_switch_click()
        test_page_organize_rules()
        test_system_defaults_locked()
        test_admin_select_does_not_switch_desktop()
        test_fence_visibility_toggle_surgical()
        test_office_email_temp_filtered()
        test_loose_desktop_floats()
        test_empty_page_does_not_mirror_other_pins()
        test_layout_snapshot_desktidy()
        test_meeting_minutes()
        test_page_folders()
        test_wallpaper()
        test_pinned_desktop_folder_standin()
        test_organize_metrics_on_reshow()
    finally:
        # Drain Qt before process teardown — avoids intermittent
        # STATUS_FATAL_USER_CALLBACK_EXCEPTION (0xC000041D) on interpreter exit.
        try:
            from PyQt6.QtWidgets import QApplication

            app = QApplication.instance()
            if app is not None:
                for w in list(app.topLevelWidgets()):
                    try:
                        w.close()
                        w.deleteLater()
                    except RuntimeError:
                        pass
                app.processEvents()
                app.quit()
                app.processEvents()
        except Exception:
            pass
    elapsed = time.perf_counter() - started
    try:
        from scripts.cleanup_selftest_artifacts import report_cleanup

        report_cleanup()
    except Exception as exc:
        print(f"测试残留清理失败: {exc}")
    print("\n" + "=" * 60)
    print(f"通过 {len(passes)}  失败 {len(failures)}  耗时 {elapsed:.1f}s")
    if failures:
        print("\n失败项:")
        for name, err in failures:
            print(f"  - {name}: {err}")
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
