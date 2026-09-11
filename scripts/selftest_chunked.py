"""Chunked DeskTidy self-test — avoids full app/native overlay crash."""

from __future__ import annotations

import sys
import tempfile
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

failures: list[tuple[str, str]] = []
passes: list[str] = []


def ok(name: str) -> None:
    passes.append(name)
    print(f"  PASS  {name}")


def fail(name: str, exc: object) -> None:
    failures.append((name, str(exc)))
    print(f"  FAIL  {name}: {exc}")


def section(title: str) -> None:
    print(f"\n=== {title} ===")


def test_display_layout() -> None:
    section("Display layout")
    from PyQt6.QtWidgets import QApplication

    app = QApplication.instance() or QApplication(sys.argv)
    from src.fence_layout import (
        current_display_fingerprint,
        geometry_from_entry,
        get_fence_geometry,
        migrate_layouts,
        save_fence_geometry,
        virtual_desktop_rect,
    )

    fp = current_display_fingerprint()
    assert fp and "x" in fp
    ok("fingerprint")
    area = virtual_desktop_rect()
    assert area.width() > 0
    ok(f"virtual_desktop {area.width()}x{area.height()}")

    from src.fence_layout import fingerprint_is_transient

    settings = {
        "fences": [
            {"id": "f1", "name": "软件", "x": 100, "y": 80, "width": 500, "height": 280}
        ],
        "fence_layouts_by_page": {},
        "fence_layouts_by_display": {},
    }
    migrate_layouts(settings)
    if fingerprint_is_transient(fp):
        # Offscreen / tiny Qt screens must not poison real layout profiles.
        assert fp not in settings["fence_layouts_by_display"]
        ok("migrate skips transient profile")
        return
    assert fp in settings["fence_layouts_by_display"]
    ok("migrate seeds profile")

    fence = settings["fences"][0]
    save_fence_geometry(
        settings,
        fence,
        0,
        {"x": 120, "y": 90, "width": 400, "height": 260, "collapsed": False},
    )
    entry = settings["fence_layouts_by_display"][fp]["0"]["f1"]
    assert "rx" in entry and "rw" in entry
    ok("relative ratios saved")

    settings["fence_layouts_by_display"][fp] = {}
    settings["fence_layouts_by_display"]["donor"] = {
        "0": {
            "f1": {
                "x": 100,
                "y": 50,
                "width": 400,
                "height": 300,
                "collapsed": False,
                "rx": 0.1,
                "ry": 0.1,
                "rw": 0.3,
                "rh": 0.4,
                "ref_w": 1920,
                "ref_h": 1080,
                "ref_x": 0,
                "ref_y": 0,
            }
        }
    }
    settings["last_display_fingerprint"] = "donor"
    geom = get_fence_geometry(settings, fence, 0)
    assert geom["width"] >= 160
    ok(f"scale fallback {geom}")

    g2 = geometry_from_entry(
        {"rx": 2.0, "ry": 2.0, "rw": 0.2, "rh": 0.2, "collapsed": False}
    )
    assert g2["x"] + g2["width"] <= area.x() + area.width() + 1
    ok("clamp on-screen")
    _ = app


def test_virtual_membership() -> None:
    section("Virtual membership")
    from src.fence_rules import (
        assign_paths_to_virtual_fence,
        get_virtual_items_for_fence,
        set_virtual_item_order,
        unpin_paths_from_virtual_fence,
    )
    from src.settings import get_desktop_path

    desk = get_desktop_path()
    tmp_files: list[Path] = []
    try:
        for i in range(3):
            p = desk / f"_desktidy_selftest_{i}.txt"
            p.write_text("t", encoding="utf-8")
            tmp_files.append(p)
        settings = {
            "fences": [
                {"id": "a", "name": "A", "extensions": [], "virtual_items": []},
                {"id": "b", "name": "B", "extensions": [], "virtual_items": []},
            ],
            "exclude_patterns": [],
            "organize_mode": "virtual",
        }
        fa, fb = settings["fences"]
        assign_paths_to_virtual_fence(fa, settings, tmp_files[:2])
        assert len(fa["virtual_items"]) == 2
        ok("assign pins")

        assign_paths_to_virtual_fence(fb, settings, [tmp_files[0]])
        assert all(
            str(tmp_files[0]).casefold() not in str(x).casefold()
            for x in fa["virtual_items"]
        )
        ok("exclusivity across fences")

        set_virtual_item_order(fa, [tmp_files[1], tmp_files[2]])
        assert fa["sort_by"] == "manual"
        names = [p.name for p in get_virtual_items_for_fence(fa, settings)]
        assert names.index("_desktidy_selftest_1.txt") < names.index(
            "_desktidy_selftest_2.txt"
        )
        ok("manual order")

        assert unpin_paths_from_virtual_fence(fa, [tmp_files[1]])
        ok("unpin")
    finally:
        for p in tmp_files:
            p.unlink(missing_ok=True)


def test_mime() -> None:
    section("Drop mime")
    from PyQt6.QtCore import QByteArray, QMimeData, QUrl
    from PyQt6.QtWidgets import QApplication

    _ = QApplication.instance() or QApplication(sys.argv)
    from src.win_shell import (
        DESKTIDY_SOURCE_FENCE_MIME,
        DESKTIDY_VIRTUAL_MIME,
        collect_drop_paths,
        desktidy_source_fence_id,
        mime_has_droppable_items,
    )

    p = Path(tempfile.gettempdir()) / "_desktidy_drop_test.txt"
    p.write_text("x", encoding="utf-8")
    try:
        mime = QMimeData()
        mime.setData(DESKTIDY_VIRTUAL_MIME, QByteArray(str(p).encode("utf-8")))
        mime.setData(DESKTIDY_SOURCE_FENCE_MIME, QByteArray(b"fid1"))
        assert mime_has_droppable_items(mime)
        assert collect_drop_paths(mime)[0].resolve() == p.resolve()
        assert desktidy_source_fence_id(mime) == "fid1"
        ok("virtual mime")

        mime2 = QMimeData()
        mime2.setUrls([QUrl.fromLocalFile(str(p))])
        assert collect_drop_paths(mime2)
        ok("url mime")
    finally:
        p.unlink(missing_ok=True)


def test_restore_dedupe() -> None:
    section("Restore dedupe")
    from src.organizer import restore_desktop_from_storage
    from src.settings import get_desktop_path, get_fence_storage_root

    storage = get_fence_storage_root()
    desk = get_desktop_path()
    fence_dir = storage / "_selftest_fence"
    fence_dir.mkdir(parents=True, exist_ok=True)
    name = "_desktidy_restore_dup_test.txt"
    storage_file = fence_dir / name
    desk_file = desk / name
    storage_file.write_text("from-storage", encoding="utf-8")
    desk_file.write_text("already-on-desktop", encoding="utf-8")
    try:
        restore_desktop_from_storage()
        assert not storage_file.exists()
        assert desk_file.read_text(encoding="utf-8") == "already-on-desktop"
        assert not any(
            p.name.startswith("_desktidy_restore_dup_test_")
            for p in desk.glob("_desktidy_restore_dup_test*")
        )
        ok("no name_1 duplicate")
    finally:
        desk_file.unlink(missing_ok=True)
        storage_file.unlink(missing_ok=True)
        try:
            if fence_dir.exists() and not any(fence_dir.iterdir()):
                fence_dir.rmdir()
        except OSError:
            pass


def test_icon_item_attrs() -> None:
    section("FenceIconItem (no overlay window)")
    from PyQt6.QtCore import Qt
    from PyQt6.QtWidgets import QApplication

    app = QApplication.instance() or QApplication(sys.argv)
    from src.ui.fence_icon_item import FenceIconItem, should_unpin_virtual_drop

    tmp = Path(tempfile.gettempdir()) / "_desktidy_icon_item.txt"
    tmp.write_text("1", encoding="utf-8")
    item = FenceIconItem(tmp, virtual_mode=True, fence_id="t1")
    try:
        assert item.icon_label.testAttribute(
            Qt.WidgetAttribute.WA_TransparentForMouseEvents
        )
        if item.text_label is not None:
            assert item.text_label.testAttribute(
                Qt.WidgetAttribute.WA_TransparentForMouseEvents
            )
        ok("mouse events reach parent for drag")
        # should_unpin callable
        assert callable(should_unpin_virtual_drop)
        ok("should_unpin_virtual_drop exists")
    finally:
        tmp.unlink(missing_ok=True)
        item.deleteLater()
        app.processEvents()


def test_settings() -> None:
    section("Settings")
    from src.settings import APP_DIR, load_settings

    s = load_settings()
    assert isinstance(s, dict)
    ok(f"organize_mode={s.get('organize_mode')!r}")
    ok(f"fences={len(s.get('fences') or [])}")
    ok(f"hide_shell_icons={s.get('hide_shell_icons')!r}")
    ok(f"app_dir={APP_DIR}")
    profiles = s.get("fence_layouts_by_display") or {}
    ok(f"display_profiles={len(profiles)}")


def test_mainwindow_light() -> None:
    section("MainWindow (light)")
    from PyQt6.QtWidgets import QApplication

    app = QApplication.instance() or QApplication(sys.argv)
    from src.settings import load_settings
    from src.ui.main_window import MainWindow

    win = MainWindow(load_settings())
    try:
        # Don't show() — avoid native shell quirks; just construct.
        # Settings chrome is lazy-built; assert shell chrome instead of mode combo.
        assert "DeskTidy" in win.windowTitle() or win.windowTitle()
        assert win._nav is not None
        assert win._stack is not None
        ok("MainWindow constructed")
        ok("sidebar/stack present")
    finally:
        win.close()
        win.deleteLater()
        app.processEvents()


def main() -> int:
    tests = [
        ("display", test_display_layout),
        ("virtual", test_virtual_membership),
        ("mime", test_mime),
        ("restore", test_restore_dedupe),
        ("icon", test_icon_item_attrs),
        ("settings", test_settings),
        ("mainwindow", test_mainwindow_light),
    ]
    for name, fn in tests:
        try:
            fn()
        except Exception as e:
            fail(name, e)
            traceback.print_exc()

    section("SUMMARY")
    print(f"passed={len(passes)} failed={len(failures)}")
    for n, e in failures:
        print(f" - {n}: {e}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
