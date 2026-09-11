"""DeskTidy 虚拟分区回归测试（仅 virtual，无物理搬家）。

覆盖：
  1. 默认/强制 organize_mode=virtual
  2. 一键整理只钉选、不移动文件
  3. pin / 跨区迁移 / unpin
  4. 公共桌面与规则互斥
  5. 主窗无模式切换控件
  6. FenceWidget 恒虚拟
  7. 退出契约（保留钉选）
  8. 遗留仓库迁移钩子
  9. dry-run / 统计
 10. 打包版活体冒烟（可选，无进程冲突时）

运行：
  python scripts\\selftest_virtual.py
  scripts\\selftest_virtual.bat
"""

from __future__ import annotations

import inspect
import json
import os
import sys
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.dist_paths import packaged_exe

os.environ.setdefault("QT_QPA_PLATFORM", "windows")

passes: list[str] = []
failures: list[tuple[str, str]] = []
section = ""


def begin(title: str) -> None:
    global section
    section = title
    print(f"\n=== {title} ===")


def ok(name: str) -> None:
    passes.append(f"{section}: {name}" if section else name)
    print(f"  PASS  {name}")


def fail(name: str, exc: object) -> None:
    label = f"{section}: {name}" if section else name
    failures.append((label, str(exc)))
    print(f"  FAIL  {name}: {exc}")


def run(name: str, fn) -> None:
    try:
        fn()
        ok(name)
    except Exception as exc:
        fail(name, exc)
        traceback.print_exc()


def test_defaults_and_coerce() -> None:
    begin("1) 默认与强制 virtual")

    def _default_json():
        data = json.loads((ROOT / "config" / "default_settings.json").read_text(encoding="utf-8"))
        assert data.get("organize_mode") == "virtual"

    run("default_settings.json 为 virtual", _default_json)

    def _load_coerce():
        from src.settings import load_settings

        s = load_settings()
        assert s.get("organize_mode") == "virtual"

    run("load_settings 纠正为 virtual", _load_coerce)

    def _organize_forces_virtual():
        from src.organizer import organize_desktop

        settings = {
            "organize_mode": "physical",  # legacy junk
            "exclude_patterns": [],
            "public_desktop_items": [],
            "fences": [],
        }
        organize_desktop(settings, dry_run=True)
        assert settings.get("organize_mode") == "virtual"

    run("organize_desktop 强制写成 virtual", _organize_forces_virtual)


def test_organize_pin_only() -> None:
    begin("2) 一键整理只钉选")

    def _pin_no_move():
        from src.fence_rules import get_virtual_items_for_fence
        from src.organizer import organize_desktop
        from src.settings import get_desktop_path

        desk = get_desktop_path()
        marker = f"_vt_org_{int(time.time())}"
        src = desk / f"{marker}.docx"
        try:
            src.write_text("x", encoding="utf-8")
            settings = {
                "organize_mode": "virtual",
                "exclude_patterns": ["desktop.ini"],
                "public_desktop_items": [],
                "fences": [
                    {
                        "id": "doc1",
                        "name": "文件",
                        "organize_kinds": ["file"],
                        "virtual_items": [],
                        "pages": [0],
                    }
                ],
            }
            result = organize_desktop(settings, dry_run=False)
            assert result.virtual is True
            assert result.moved_count >= 1, result.summary
            assert src.is_file(), "文件不应被移走"
            pinned = get_virtual_items_for_fence(settings["fences"][0], settings)
            assert any(p.name == src.name for p in pinned)
        finally:
            src.unlink(missing_ok=True)

    run("匹配规则钉选且文件仍在桌面", _pin_no_move)

    def _dry_run():
        from src.organizer import organize_desktop
        from src.settings import load_settings

        s = load_settings()
        result = organize_desktop(s, dry_run=True)
        assert result.virtual is True
        assert result.moved_count >= 0

    run("dry_run 可调用", _dry_run)


def test_pin_migrate_unpin() -> None:
    begin("3) pin / 跨区 / unpin")

    def _flow():
        from src.fence_rules import (
            assign_paths_to_virtual_fence,
            get_virtual_items_for_fence,
            unpin_paths_from_virtual_fence,
        )
        from src.settings import get_desktop_path

        desk = get_desktop_path()
        files = []
        try:
            for i in range(2):
                p = desk / f"_vt_pin_{i}_{int(time.time())}.txt"
                p.write_text("1", encoding="utf-8")
                files.append(p)
            settings = {
                "organize_mode": "virtual",
                "exclude_patterns": [],
                "fences": [
                    {"id": "a", "name": "A", "extensions": [], "virtual_items": []},
                    {"id": "b", "name": "B", "extensions": [], "virtual_items": []},
                ],
            }
            fa, fb = settings["fences"]
            assign_paths_to_virtual_fence(fa, settings, files)
            assert len(get_virtual_items_for_fence(fa, settings)) == 2
            assign_paths_to_virtual_fence(fb, settings, [files[0]])
            assert len(get_virtual_items_for_fence(fa, settings)) == 1
            assert len(get_virtual_items_for_fence(fb, settings)) >= 1
            unpin_paths_from_virtual_fence(fb, [files[0]])
            assert files[0].is_file()
        finally:
            for p in files:
                p.unlink(missing_ok=True)

    run("跨区钉选与 unpin", _flow)

    def _no_rule_resorb():
        """Rule match alone must not show an icon already pinned elsewhere."""
        from src.fence_rules import (
            assign_paths_to_virtual_fence,
            get_virtual_items_for_fence,
        )
        from src.settings import get_desktop_path

        desk = get_desktop_path()
        p = desk / f"_vt_resorb_{int(time.time())}.lnk"
        try:
            p.write_bytes(b"x")
            settings = {
                "organize_mode": "virtual",
                "exclude_patterns": [],
                "public_desktop_items": [],
                "fences": [
                    {
                        "id": "apps",
                        "name": "常用",
                        "extensions": [".lnk"],
                        "virtual_items": [],
                        "sort_by": "name",
                    },
                    {
                        "id": "other",
                        "name": "其他",
                        "extensions": [".lnk"],
                        "virtual_items": [],
                        "sort_by": "name",
                    },
                ],
            }
            apps, other = settings["fences"]
            # Not pinned → neither fence shows it (rules only via organize).
            assert get_virtual_items_for_fence(apps, settings) == []
            assign_paths_to_virtual_fence(other, settings, [p])
            assert any(x.name == p.name for x in get_virtual_items_for_fence(other, settings))
            assert get_virtual_items_for_fence(apps, settings) == []
        finally:
            p.unlink(missing_ok=True)

    run("跨区后规则不再吸回原分区", _no_rule_resorb)


def test_public_vs_fence() -> None:
    begin("4) 公共桌面与分区互斥")

    def _claim():
        from PyQt6.QtCore import QRect

        from src.fence_rules import assign_paths_to_virtual_fence, all_fence_pinned_keys
        from src.public_desktop import (
            add_public_item,
            get_public_items,
            public_claimed_keys,
            remove_public_paths,
        )
        from src.settings import get_desktop_path

        desk = get_desktop_path()
        p = desk / f"_vt_pub_{int(time.time())}.txt"
        try:
            p.write_text("p", encoding="utf-8")
            settings = {
                "organize_mode": "virtual",
                "enable_public_desktop": True,
                "public_desktop_items": [],
                "fences": [
                    {"id": "f1", "name": "F", "extensions": [], "virtual_items": []},
                ],
            }
            add_public_item(
                settings,
                p,
                x=10,
                y=10,
                auto_arrange=False,
                fence_rects=[QRect(0, 0, 50, 50)],
            )
            assert p.name in " ".join(str(x) for x in public_claimed_keys(settings)) or any(
                Path(str(e.get("path", ""))).name == p.name for e in get_public_items(settings)
            )
            assign_paths_to_virtual_fence(settings["fences"][0], settings, [p])
            remove_public_paths(settings, [p])
            pub_names = {
                Path(str(e.get("path", ""))).name for e in get_public_items(settings)
            }
            assert p.name not in pub_names
            pinned_keys = all_fence_pinned_keys(settings)
            assert any(
                Path(str(k)).name == p.name or str(k).casefold().endswith(p.name.casefold())
                for k in pinned_keys
            )
            assert p.is_file()
        finally:
            p.unlink(missing_ok=True)

    run("公共项钉入分区后从 public 移除", _claim)


def test_ui_no_mode_combo() -> None:
    begin("5) UI 无模式切换")

    def _main_window():
        from PyQt6.QtWidgets import QApplication

        from src.settings import load_settings
        from src.ui.main_window import MainWindow

        app = QApplication.instance() or QApplication(sys.argv)
        globals()["qt_app"] = app
        src = Path(inspect.getfile(MainWindow)).read_text(encoding="utf-8")
        assert "mode_combo" not in src
        assert "物理整理" not in src
        assert "混合模式" not in src
        win = MainWindow(load_settings())
        assert not hasattr(win, "mode_combo")
        win.hide()
        win.close()
        win.deleteLater()
        app.processEvents()

    run("MainWindow 无整理模式下拉", _main_window)

    def _fence_always_virtual():
        from PyQt6.QtWidgets import QApplication

        from src.ui.fence_widget import FenceWidget

        app = QApplication.instance() or QApplication(sys.argv)
        cfg = {
            "id": "v1",
            "name": "V",
            "x": 40,
            "y": 40,
            "width": 320,
            "height": 240,
            "extensions": [],
            "virtual_items": [],
            "style": {},
            "pages": [0],
        }
        settings = {"organize_mode": "physical", "fences": [cfg], "theme": "mist", "exclude_patterns": []}
        w = FenceWidget(cfg, settings)
        assert w._is_virtual_mode() is True
        w.close()
        w.deleteLater()
        app.processEvents()

    run("FenceWidget 恒为虚拟模式", _fence_always_virtual)

    def _cross_fence_drop_ownership():
        """Each fence must only claim its own icon drop targets (app-level filter)."""
        from PyQt6.QtWidgets import QApplication

        from src.ui.fence_icon_item import FenceIconItem
        from src.ui.fence_widget import FenceWidget

        app = QApplication.instance() or QApplication(sys.argv)
        globals()["qt_app"] = app

        def _cfg(fid: str) -> dict:
            return {
                "id": fid,
                "name": fid,
                "x": 40,
                "y": 40,
                "width": 320,
                "height": 240,
                "extensions": [],
                "virtual_items": [],
                "style": {},
                "pages": [0],
            }

        settings = {
            "organize_mode": "virtual",
            "fences": [],
            "theme": "mist",
            "exclude_patterns": [],
        }
        cfg_a = _cfg("fa")
        cfg_b = _cfg("fb")
        settings["fences"] = [cfg_a, cfg_b]
        a = FenceWidget(cfg_a, settings)
        b = FenceWidget(cfg_b, settings)
        icon_a = FenceIconItem(Path("."), virtual_mode=True, fence_id="fa")
        icon_b = FenceIconItem(Path("."), virtual_mode=True, fence_id="fb")
        assert a._owns_drop_object(icon_a) is True
        assert a._owns_drop_object(icon_b) is False
        assert b._owns_drop_object(icon_b) is True
        assert b._owns_drop_object(icon_a) is False
        assert a._owns_drop_object(a.items_widget) is True
        assert a._owns_drop_object(b.items_widget) is False
        a.close()
        b.close()
        icon_a.deleteLater()
        icon_b.deleteLater()
        a.deleteLater()
        b.deleteLater()
        app.processEvents()

    run("跨分区拖放归属（不抢对方图标）", _cross_fence_drop_ownership)


def test_quit_contracts() -> None:
    begin("6) 退出契约")

    def _quit_copy():
        from src.app import DeskTidyApp

        src = inspect.getsource(DeskTidyApp._quit_impl)
        assert "退出并还原" not in src
        assert "分区钉选与布局会保留" in src
        assert "是否退出" in src
        # Single exit path — soft park only.
        assert "restore_desktop" not in src

    run("退出仅保留钉选文案", _quit_copy)

    def _tray_label():
        from src import tray

        src = Path(inspect.getfile(tray)).read_text(encoding="utf-8")
        assert "退出并还原系统桌面" not in src
        assert 'QAction("退出"' in src or "QAction('退出'" in src

    run("托盘仅保留退出", _tray_label)

    def _migrate_hook():
        from src.app import DeskTidyApp

        src = inspect.getsource(DeskTidyApp._startup_deferred_legacy_storage_migrate)
        assert "legacy_warehouse_has_files" in src
        sched = inspect.getsource(DeskTidyApp._schedule_startup_deferred)
        assert "_startup_deferred_legacy_storage_migrate" in sched

    run("遗留仓库迁移钩子", _migrate_hook)


def test_no_physical_move_path() -> None:
    begin("7) 无物理搬家路径")

    def _organizer_src():
        from src import organizer

        src = inspect.getsource(organizer.organize_desktop)
        assert "_organize_virtual" in src
        body = Path(inspect.getfile(organizer)).read_text(encoding="utf-8")
        # Physical move loop should be gone from organize_desktop path.
        assert "shutil.move" not in inspect.getsource(organizer.organize_desktop)
        assert "shutil.move" not in inspect.getsource(organizer._organize_virtual)
        # restore may still shutil.move for legacy — OK
        assert "restore_desktop_from_storage" in body
        assert "legacy_warehouse_has_files" in body

    run("organize 路径无 shutil.move", _organizer_src)


def test_packaged_optional() -> None:
    begin("8) 打包产物")

    def _dist():
        exe = packaged_exe(ROOT)
        assert exe.is_file()
        assert exe.stat().st_size > 100_000
        ver = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        assert ver

    run("DeskTidy.exe 存在", _dist)


def main() -> int:
    print("DeskTidy 虚拟分区回归测试")
    print(f"ROOT = {ROOT}")
    started = time.perf_counter()

    test_defaults_and_coerce()
    test_organize_pin_only()
    test_pin_migrate_unpin()
    test_public_vs_fence()
    test_ui_no_mode_combo()
    test_quit_contracts()
    test_no_physical_move_path()
    test_packaged_optional()

    elapsed = time.perf_counter() - started
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
