"""DeskTidy smoke self-test (no interactive GUI)."""

from __future__ import annotations

import os
import sys
import tempfile
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

failures: list[tuple[str, str]] = []
passes: list[str] = []


def ok(name: str) -> None:
    passes.append(name)
    print(f"  PASS  {name}")


def fail(name: str, exc: object) -> None:
    failures.append((name, str(exc)))
    print(f"  FAIL  {name}: {exc}")


def main() -> int:
    print("=== 1) Imports ===")
    try:
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance() or QApplication(sys.argv)
        ok("QApplication")
    except Exception as e:
        fail("QApplication", e)
        return 1

    for m in (
        "src.app",
        "src.fence_layout",
        "src.fence_rules",
        "src.win_shell",
        "src.organizer",
        "src.settings",
        "src.ui.fence_widget",
        "src.ui.fence_icon_item",
        "src.ui.main_window",
    ):
        try:
            __import__(m)
            ok(f"import {m}")
        except Exception as e:
            fail(f"import {m}", e)

    print("\n=== 2) Display layout adapt ===")
    try:
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
        ok(f"fingerprint ok")
        area = virtual_desktop_rect()
        assert area.width() > 0 and area.height() > 0
        ok(f"virtual_desktop={area.width()}x{area.height()}")

        settings = {
            "fences": [
                {
                    "id": "f1",
                    "name": "软件",
                    "x": 100,
                    "y": 80,
                    "width": 500,
                    "height": 280,
                }
            ],
            "fence_layouts_by_page": {},
            "fence_layouts_by_display": {},
        }
        migrate_layouts(settings)
        fp = current_display_fingerprint()
        from src.fence_layout import fingerprint_is_transient

        if fingerprint_is_transient(fp):
            # Offscreen Qt 800×800 must not keep a profile (would crush real sizes).
            assert fp not in settings["fence_layouts_by_display"]
            ok("migrate skips transient offscreen fingerprint")
        else:
            assert fp in settings["fence_layouts_by_display"]
            ok("migrate_layouts seeds current profile")

        fence = settings["fences"][0]
        save_fence_geometry(
            settings,
            fence,
            0,
            {"x": 120, "y": 90, "width": 400, "height": 260, "collapsed": False},
        )
        # Under offscreen, writes are blocked — skip ratio asserts.
        from src.fence_layout import layout_writes_blocked

        if layout_writes_blocked() or fingerprint_is_transient(fp):
            ok("save blocked on transient/offscreen (expected)")
        else:
            entry = settings["fence_layouts_by_display"][fp]["0"]["f1"]
            assert "rx" in entry and "rw" in entry
            ok("save stores relative ratios")

        settings["fence_layouts_by_display"].pop(fp, None)
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
        assert geom["width"] >= 160 and geom["height"] >= 120
        ok(f"scale fallback geom={geom}")

        g2 = geometry_from_entry(
            {"rx": 2.0, "ry": 2.0, "rw": 0.2, "rh": 0.2, "collapsed": False}
        )
        assert g2["x"] + g2["width"] <= area.x() + area.width() + 1
        assert g2["y"] + g2["height"] <= area.y() + area.height() + 1
        ok("clamp keeps geometry on-screen")
    except Exception as e:
        fail("display layout", e)
        traceback.print_exc()

    print("\n=== 3) Virtual fence membership ===")
    try:
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
            added = assign_paths_to_virtual_fence(fa, settings, tmp_files[:2])
            assert len(fa["virtual_items"]) == 2
            ok(f"assign pin count={len(added)}")

            assign_paths_to_virtual_fence(fb, settings, [tmp_files[0]])
            assert all(
                str(tmp_files[0]).casefold() not in str(x).casefold()
                for x in fa["virtual_items"]
            )
            assert any(
                str(tmp_files[0]).casefold() == str(x).casefold()
                for x in fb["virtual_items"]
            )
            ok("move between fences exclusivity")

            set_virtual_item_order(fa, [tmp_files[1], tmp_files[2]])
            assert fa["sort_by"] == "manual"
            items = get_virtual_items_for_fence(fa, settings)
            names = [p.name for p in items]
            assert names.index("_desktidy_selftest_1.txt") < names.index(
                "_desktidy_selftest_2.txt"
            )
            ok("manual order preserved")

            assert unpin_paths_from_virtual_fence(fa, [tmp_files[1]])
            assert not any("_desktidy_selftest_1" in str(x) for x in fa["virtual_items"])
            ok("unpin works")
        finally:
            for p in tmp_files:
                try:
                    p.unlink(missing_ok=True)
                except OSError:
                    pass
    except Exception as e:
        fail("virtual membership", e)
        traceback.print_exc()

    print("\n=== 4) Drop mime helpers ===")
    try:
        from PyQt6.QtCore import QByteArray, QMimeData, QUrl

        from src.win_shell import (
            DESKTIDY_SOURCE_FENCE_MIME,
            DESKTIDY_VIRTUAL_MIME,
            collect_drop_paths,
            desktidy_source_fence_id,
            mime_has_droppable_items,
        )

        mime = QMimeData()
        assert not mime_has_droppable_items(mime)
        p = Path(tempfile.gettempdir()) / "_desktidy_drop_test.txt"
        p.write_text("x", encoding="utf-8")
        try:
            mime2 = QMimeData()
            mime2.setData(DESKTIDY_VIRTUAL_MIME, QByteArray(str(p).encode("utf-8")))
            mime2.setData(DESKTIDY_SOURCE_FENCE_MIME, QByteArray(b"fid1"))
            assert mime_has_droppable_items(mime2)
            paths = collect_drop_paths(mime2)
            assert paths and paths[0].resolve() == p.resolve()
            assert desktidy_source_fence_id(mime2) == "fid1"
            ok("virtual mime collect_drop_paths")

            mime3 = QMimeData()
            mime3.setUrls([QUrl.fromLocalFile(str(p))])
            assert mime_has_droppable_items(mime3)
            assert collect_drop_paths(mime3)
            ok("url mime collect_drop_paths")

            # Explorer→fence drops use Shell IDList; 64-bit PIDL must not OverflowError.
            import ctypes

            from src.win_shell import (
                SIGDN_DESKTOPABSOLUTEPARSING,
                _sh_get_name_from_pidl,
            )

            ole32 = ctypes.windll.ole32
            shell32 = ctypes.windll.shell32
            ole32.CoInitialize(None)
            pidl = ctypes.c_void_p()
            try:
                shell32.SHParseDisplayName.argtypes = [
                    ctypes.c_wchar_p,
                    ctypes.c_void_p,
                    ctypes.POINTER(ctypes.c_void_p),
                    ctypes.c_ulong,
                    ctypes.POINTER(ctypes.c_ulong),
                ]
                shell32.SHParseDisplayName.restype = ctypes.HRESULT
                shell32.ILFree.argtypes = [ctypes.c_void_p]
                sfgao = ctypes.c_ulong()
                hr = shell32.SHParseDisplayName(
                    str(p), None, ctypes.byref(pidl), 0, ctypes.byref(sfgao)
                )
                assert hr == 0 and pidl.value, f"SHParseDisplayName hr={hr}"
                assert int(pidl.value) > 0x7FFFFFFF, "need 64-bit PIDL to cover OverflowError path"
                name = _sh_get_name_from_pidl(pidl.value, SIGDN_DESKTOPABSOLUTEPARSING)
                assert name and Path(name).resolve() == p.resolve(), name
                ok("64-bit Shell IDList PIDL name resolve")
            finally:
                if pidl.value:
                    shell32.ILFree(pidl)
                ole32.CoUninitialize()
        finally:
            p.unlink(missing_ok=True)
    except Exception as e:
        fail("mime helpers", e)
        traceback.print_exc()

    print("\n=== 5) Restore no-duplicate logic ===")
    try:
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
        n = restore_desktop_from_storage()
        assert not storage_file.exists(), "storage copy should be removed"
        assert desk_file.exists() and desk_file.read_text(encoding="utf-8") == (
            "already-on-desktop"
        )
        after = list(desk.glob("_desktidy_restore_dup_test*"))
        assert not any(p.name.startswith("_desktidy_restore_dup_test_") for p in after)
        ok(f"restore skips duplicate (restored_count={n})")
        desk_file.unlink(missing_ok=True)
        try:
            if fence_dir.exists() and not any(fence_dir.iterdir()):
                fence_dir.rmdir()
        except OSError:
            pass
    except Exception as e:
        fail("restore dedupe", e)
        traceback.print_exc()

    print("\n=== 6) Widget construct (offscreen) ===")
    try:
        from PyQt6.QtCore import Qt

        from src.ui.fence_icon_item import FenceIconItem
        from src.ui.fence_widget import FenceWidget

        settings = {
            "organize_mode": "virtual",
            "exclude_patterns": [],
            "fences": [],
            "theme": "dark",
        }
        cfg = {
            "id": "t1",
            "name": "测试分区",
            "x": 40,
            "y": 40,
            "width": 280,
            "height": 220,
            "virtual_items": [],
            "extensions": [],
            "style": {},
        }
        settings["fences"] = [cfg]
        # FenceWidget.show() under QT_QPA_PLATFORM=offscreen can abort on some hosts.
        w = FenceWidget(cfg, settings)
        app.processEvents()
        assert w._is_virtual_mode()
        ok("FenceWidget construct + virtual mode")

        tmp = Path(tempfile.gettempdir()) / "_desktidy_icon_item.txt"
        tmp.write_text("1", encoding="utf-8")
        item = None
        try:
            item = FenceIconItem(tmp, virtual_mode=True, fence_id="t1")
            assert item.icon_label.testAttribute(
                Qt.WidgetAttribute.WA_TransparentForMouseEvents
            )
            ok("icon label transparent for mouse (drag can start)")
            assert not item.is_selected()
            item.set_selected(True)
            assert item.is_selected()
            # Selected paint must draw a non-transparent highlight (regression:
            # clicking fence icons used to have no visible selection).
            from PyQt6.QtGui import QColor, QImage

            item.resize(96, 110)
            item.show()
            app.processEvents()
            img = item.grab().toImage().convertToFormat(QImage.Format.Format_ARGB32)
            assert not img.isNull() and img.width() > 0
            tinted = 0
            for y in range(0, img.height(), 4):
                for x in range(0, img.width(), 4):
                    c = QColor(img.pixel(x, y))
                    if c.alpha() < 20:
                        continue
                    # Selection fill uses accent green with alpha ~110.
                    if c.green() > c.red() + 15 and c.green() > c.blue() + 15:
                        tinted += 1
            assert tinted >= 8, f"expected visible selection tint pixels, got {tinted}"
            ok("FenceIconItem selected highlight is visible")
            item.set_selected(False)
            assert not item.is_selected()
        finally:
            tmp.unlink(missing_ok=True)
            if item is not None:
                item.deleteLater()
        w.close()
        w.deleteLater()
        app.processEvents()
    except Exception as e:
        fail("widget construct", e)
        traceback.print_exc()

    print("\n=== 6b) Fence icon click selection ===")
    try:
        from PyQt6.QtCore import Qt

        from src.ui.fence_icon_item import FenceIconItem, select_fence_item
        from src.ui.fence_widget import FenceItemLabel, FenceWidget

        settings = {
            "organize_mode": "virtual",
            "exclude_patterns": [],
            "fences": [],
            "theme": "dark",
        }
        cfg = {
            "id": "sel1",
            "name": "选中测试",
            "x": 40,
            "y": 40,
            "width": 320,
            "height": 260,
            "virtual_items": [],
            "extensions": [],
            "style": {"accent": "#07c160"},
        }
        settings["fences"] = [cfg]
        fence = FenceWidget(cfg, settings)
        app.processEvents()

        tmp_a = Path(tempfile.gettempdir()) / "_desktidy_sel_a.txt"
        tmp_b = Path(tempfile.gettempdir()) / "_desktidy_sel_b.txt"
        tmp_a.write_text("a", encoding="utf-8")
        tmp_b.write_text("b", encoding="utf-8")
        a = FenceIconItem(tmp_a, virtual_mode=True, fence_id="sel1", parent=fence.items_widget)
        b = FenceIconItem(tmp_b, virtual_mode=True, fence_id="sel1", parent=fence.items_widget)
        fence.items_layout.addWidget(a, 0, 0)
        fence.items_layout.addWidget(b, 0, 1)
        app.processEvents()

        class _Press:
            def modifiers(self):
                return Qt.KeyboardModifier.NoModifier

            def button(self):
                return Qt.MouseButton.LeftButton

        a._apply_click_selection(_Press())
        assert a.is_selected() and not b.is_selected()
        ok("click selects icon A only")

        select_fence_item(b, exclusive=True)
        assert b.is_selected() and not a.is_selected()
        ok("exclusive select switches to B")

        fence.clear_item_selection()
        assert not a.is_selected() and not b.is_selected()
        ok("clear_item_selection clears both")

        # Regression: QWidget default mousePress ignores → event bubbles to
        # items_widget → empty-click clearer wiped selection immediately.
        from PyQt6.QtCore import QEvent, QPointF
        from PyQt6.QtGui import QMouseEvent

        center = a.rect().center()
        gp = a.mapToGlobal(QPointF(center))
        press_ev = QMouseEvent(
            QEvent.Type.MouseButtonPress,
            QPointF(center),
            QPointF(center),
            gp,
            Qt.MouseButton.LeftButton,
            Qt.MouseButton.LeftButton,
            Qt.KeyboardModifier.NoModifier,
        )
        app.sendEvent(a, press_ev)
        app.processEvents()
        assert a.is_selected() and not b.is_selected(), (
            "selection must survive sendEvent (no bubble-clear)"
        )
        ok("sendEvent selection survives parent bubble")

        label = FenceItemLabel(tmp_a, virtual_mode=True, fence_id="sel1")
        label.set_selected(True)
        assert label.is_selected()
        label.set_selected(False)
        assert not label.is_selected()
        ok("FenceItemLabel selection toggles")
        label.deleteLater()

        fence.close()
        fence.deleteLater()
        tmp_a.unlink(missing_ok=True)
        tmp_b.unlink(missing_ok=True)
        app.processEvents()
    except Exception as e:
        fail("fence icon selection", e)
        traceback.print_exc()

    print("\n=== 7) Settings load ===")
    try:
        from src.settings import APP_DIR, load_settings

        s = load_settings()
        assert isinstance(s, dict)
        ok(f"load_settings organize_mode={s.get('organize_mode')!r} app_dir={APP_DIR}")
    except Exception as e:
        fail("settings", e)

    print("\n=== 8) MainWindow construct ===")
    try:
        from src.settings import load_settings
        from src.ui.main_window import MainWindow

        s = load_settings()
        win = MainWindow(s)
        win.show()
        app.processEvents()
        ok("MainWindow show")
        win.close()
        win.deleteLater()
        app.processEvents()
    except Exception as e:
        fail("MainWindow", e)
        traceback.print_exc()

    print("\n=== 9) Screenshot modal dismiss ===")
    try:
        from PyQt6.QtCore import Qt
        from PyQt6.QtWidgets import QMessageBox

        from src.i18n import _prepare_message_box
        from src.screenshot_manager import ScreenshotManager

        mgr = ScreenshotManager(lambda: {"screenshot": {"enabled": True}})
        box = QMessageBox()
        box.setText("blocking")
        box.addButton("ok", QMessageBox.ButtonRole.AcceptRole)
        _prepare_message_box(box)
        box.show()
        app.processEvents()
        assert app.activeModalWidget() is box
        mgr.prepare_capture_ui()
        app.processEvents()
        assert app.activeModalWidget() is None
        assert not box.isVisible()
        assert len(mgr._suspended_modals) == 1
        ok("prepare_capture_ui suspends ApplicationModal dialog")
        mgr.restore_suspended_modals()
        app.processEvents()
        assert box.isVisible()
        assert box.windowModality() == Qt.WindowModality.ApplicationModal
        ok("restore_suspended_modals brings dialog back")
        box.close()
    except Exception as e:
        fail("screenshot modal dismiss", e)
        traceback.print_exc()

    print("\n=== 10) Shell context menu build ===")
    try:
        import pythoncom
        import win32con
        import win32gui
        from win32com.shell import shell, shellcon

        from src.shell_file_menu import ShellMenuCommand, show_shell_context_menu
        from src.ui.fence_icon_item import FenceIconItem

        tmp = Path(tempfile.gettempdir()) / "_desktidy_shell_menu_build.txt"
        tmp.write_text("shell-menu", encoding="utf-8")
        host = None
        try:
            # Build the shell menu the same way as production (without TrackPopupMenu).
            pythoncom.CoInitialize()
            host = __import__("PyQt6.QtWidgets", fromlist=["QWidget"]).QWidget()
            host.resize(80, 40)
            host.show()
            app.processEvents()
            hwnd = int(host.winId())
            desktop = shell.SHGetDesktopFolder()
            _e, parent_pidl, _a = desktop.ParseDisplayName(hwnd, None, str(tmp.parent))
            parent = desktop.BindToObject(parent_pidl, None, shell.IID_IShellFolder)
            _e, item_pidl, _a = parent.ParseDisplayName(hwnd, None, tmp.name)
            cm = parent.GetUIObjectOf(hwnd, [item_pidl], shell.IID_IContextMenu, 0)[1]
            hmenu = win32gui.CreatePopupMenu()
            win32gui.InsertMenu(
                hmenu, 0, win32con.MF_BYPOSITION | win32con.MF_STRING, 1, "移出分区"
            )
            win32gui.InsertMenu(
                hmenu, 1, win32con.MF_BYPOSITION | win32con.MF_SEPARATOR, 0, ""
            )
            cmf = (
                shellcon.CMF_NORMAL
                | shellcon.CMF_EXPLORE
                | getattr(shellcon, "CMF_CANRENAME", 0x10)
            )
            cm.QueryContextMenu(hmenu, 2, 100, 0x7FFF, cmf)
            count = win32gui.GetMenuItemCount(hmenu)
            assert count >= 5, f"expected shell verbs, got {count}"
            ok(f"shell QueryContextMenu items={count}")
            win32gui.DestroyMenu(hmenu)

            import inspect as _inspect

            import src.shell_file_menu as sfm

            menu_src = _inspect.getsource(sfm.show_shell_context_menu)
            host_src = _inspect.getsource(sfm._host_shell_context_menu)
            mod_src = _inspect.getsource(sfm)
            assert "CMF_CANRENAME" in menu_src or "_CMF_CANRENAME" in menu_src or "_CMF_CANRENAME" in mod_src
            assert "HandleMenuMsg2" in mod_src or "_handle_menu_msg2" in mod_src
            assert "InvokeCommand" in host_src
            assert "_host_rename_item" in mod_src
            # Must not invent a parallel Qt Explorer menu / clipboard verbs.
            assert "show_qt_file_context_menu" not in mod_src
            assert "_clipboard_cut_or_copy" not in mod_src
            ok("shell menu hosts IContextMenu the standard way")

            from src.ui.fence_icon_item import FenceIconItem as _FII
            from src.ui.public_icon_widget import PublicIconWidget as _PIW

            assert "show_file_context_menu" in _inspect.getsource(_FII._show_menu)
            assert "show_file_context_menu" in _inspect.getsource(_PIW._show_menu)
            ok("icon widgets use show_file_context_menu")

            from src.ui.fence_widget import FenceWidget as _FW

            fw_menu = _inspect.getsource(_FW._show_fence_context_menu)
            assert "show_folder_background_menu" in fw_menu
            assert "CreateViewObject" in _inspect.getsource(sfm.show_folder_background_menu)
            ok("fence blank uses folder background CreateViewObject menu")

            src = _inspect.getsource(FenceIconItem._show_menu)
            assert "build_file_icon_shell_extras" in src
            ok("FenceIconItem uses shell context menu")
            assert callable(show_shell_context_menu)
            assert ShellMenuCommand("x", lambda: None).label == "x"
            ok("shell_file_menu exports ok")
        finally:
            tmp.unlink(missing_ok=True)
            if host is not None:
                host.close()
                host.deleteLater()
            app.processEvents()
    except Exception as e:
        fail("shell context menu", e)
        traceback.print_exc()

    print("\n=== SUMMARY ===")
    print(f"passed={len(passes)} failed={len(failures)}")
    for name, err in failures:
        print(f" - {name}: {err}")
    try:
        from scripts.cleanup_selftest_artifacts import report_cleanup

        report_cleanup()
    except Exception as exc:
        print(f"cleanup failed: {exc}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
