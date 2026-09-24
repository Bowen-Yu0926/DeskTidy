"""deskNote shortcut / standalone process / Markdown preview contracts."""

from __future__ import annotations

import inspect
import json
import os
import sys
import tempfile
from copy import deepcopy
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def main() -> int:
    failures: list[str] = []

    def ok(name: str) -> None:
        print(f"  OK  {name}")

    def fail(name: str, err: object) -> None:
        failures.append(f"{name}: {err}")
        print(f"  FAIL  {name}: {err}")

    print("deskNote / standalone + markdown")

    try:
        from src.desknote import (
            DESKNOTE_NAME,
            OPEN_NOTEPAD_VERB,
            is_desknote_installed,
            is_desknote_shortcut,
            remove_all_desknote_shortcuts,
            sync_desknote_shortcuts,
            try_open_desknote_in_running_app,
            wants_notepad_cli,
        )
        from src.desknote_launch import desknote_exe_path, open_in_desknote

        assert DESKNOTE_NAME == "DeskNote"
        assert OPEN_NOTEPAD_VERB == "open-notepad"
        assert wants_notepad_cli(["--notepad"])
        assert wants_notepad_cli(["--shell-verb=open-notepad"])
        assert not wants_notepad_cli(["--desktop-guard"])
        assert is_desknote_shortcut(f"{DESKNOTE_NAME}.lnk")
        assert is_desknote_installed() is True  # source tree
        assert desknote_exe_path().name in {"desknote_main.py", "deskNote.exe"}
        assert try_open_desknote_in_running_app("DeskTidy.lnk") is False
        # open_in_desknote is callable (do not actually spawn UI in selftest)
        assert callable(open_in_desknote)
        # Product float must use desknote_icon.ico — not pythonw blank document.
        from src.icon_utils import _desknote_product_pixmap, file_icon_pixmap, invalidate_file_icon_cache

        assert "_desknote_product_pixmap" in inspect.getsource(
            __import__("src.icon_utils", fromlist=["x"]).file_icon_pixmap
        ) or "is_desknote_shortcut" in inspect.getsource(file_icon_pixmap)
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance() or QApplication([])
        prod = _desknote_product_pixmap(48)
        assert not prod.isNull()
        invalidate_file_icon_cache()
        lnk_pm = file_icon_pixmap(f"{DESKNOTE_NAME}.lnk", 48)
        assert not lnk_pm.isNull()
        # Teal tile must survive (not blank white document).
        img = lnk_pm.toImage()
        teal_hits = 0
        for y in range(0, img.height(), 2):
            for x in range(0, img.width(), 2):
                c = img.pixelColor(x, y)
                if c.alpha() < 16:
                    continue
                if c.green() > c.red() + 15 and c.green() > 80:
                    teal_hits += 1
        assert teal_hits >= 8, f"expected teal product icon, hits={teal_hits}"
        import src.desknote as dn
        from src.settings import get_desktop_path

        assert "get_desktop_path" in inspect.getsource(dn.desktop_link_path)
        assert dn.desktop_link_path() == get_desktop_path() / f"{DESKNOTE_NAME}.lnk"
        target, args, _workdir = dn._shortcut_target_and_args()
        # New shortcuts point at desknote_main / deskNote.exe (no --notepad required).
        assert "desknote_main" in target.lower() or "desknote" in Path(target).name.lower() or (
            "python" in Path(target).name.lower() and "desknote_main" in args.lower()
        )
        if not getattr(sys, "frozen", False):
            assert Path(target).name.lower() in {"pythonw.exe", "python.exe"}
            assert "desknote_main" in args
        open_src = inspect.getsource(
            __import__("src.win_shell", fromlist=["open_path"]).open_path
        )
        assert "try_open_desknote_in_running_app" in open_src
        ok("CLI / constants / shortcut target")

        with tempfile.TemporaryDirectory() as td:
            desk = Path(td) / "Desktop"
            start = Path(td) / "StartMenu"
            desk.mkdir()
            start.mkdir()
            desktop_lnk = desk / f"{DESKNOTE_NAME}.lnk"
            start_lnk = start / f"{DESKNOTE_NAME}.lnk"

            orig_desk = dn.desktop_link_path
            orig_start = dn.start_menu_link_path
            dn.desktop_link_path = lambda: desktop_lnk  # type: ignore[assignment]
            dn.start_menu_link_path = lambda: start_lnk  # type: ignore[assignment]
            try:
                sync_desknote_shortcuts({"notepad": {"enabled": True}})
                assert desktop_lnk.is_file(), desktop_lnk
                assert start_lnk.is_file(), start_lnk
                from win32com.client import Dispatch

                sh = Dispatch("WScript.Shell")
                sc = sh.CreateShortCut(str(desktop_lnk))
                assert "desknote_main" in (sc.Arguments or "").lower() or (
                    Path(sc.Targetpath).name.lower() == "desknote.exe"
                )
                assert (sc.IconLocation or "").strip(), "icon must be set"
                assert "desknote_icon" in (sc.IconLocation or "").lower() or (
                    Path(sc.Targetpath).name.lower() == "desknote.exe"
                )
                sync_desknote_shortcuts({"notepad": {"enabled": False}})
                assert not desktop_lnk.exists()
                assert not start_lnk.exists()
                # Disabling must also drop the public float (no sticky ghost).
                from src.public_desktop import get_public_items

                settings_pub = {
                    "notepad": {"enabled": True},
                    "public_desktop_items": [
                        {"path": str(desktop_lnk), "x": 1, "y": 2, "loose": True}
                    ],
                }
                sync_desknote_shortcuts(settings_pub)  # create again
                assert desktop_lnk.is_file()
                settings_pub["notepad"]["enabled"] = False
                sync_desknote_shortcuts(settings_pub)
                assert not desktop_lnk.exists()
                assert not any(
                    str(e.get("path", "")).casefold() == str(desktop_lnk).casefold()
                    for e in get_public_items(settings_pub)
                    if isinstance(e, dict)
                )
            finally:
                dn.desktop_link_path = orig_desk  # type: ignore[assignment]
                dn.start_menu_link_path = orig_start  # type: ignore[assignment]
                remove_all_desknote_shortcuts()
        ok("sync create/delete + icon + args")
    except Exception as e:
        fail("desknote module", e)

    try:
        from src.markdown_preview import extract_outline, is_markdown_path, render_html

        assert is_markdown_path("a.md")
        assert is_markdown_path("a.markdown")
        assert not is_markdown_path("a.txt")
        items = extract_outline("# Hello\n\n## World\ntext\n### Nested")
        assert [(i.level, i.title, i.line) for i in items] == [
            (1, "Hello", 0),
            (2, "World", 2),
            (3, "Nested", 4),
        ]
        # Hand-typed ``###body`` (no space) still outlines / renders as H3.
        loose = extract_outline("# Hello\n## World\n###body")
        assert [(i.level, i.title) for i in loose] == [
            (1, "Hello"),
            (2, "World"),
            (3, "body"),
        ]
        html = render_html("# Title\n\nHello **bold**\n\n- a\n- b")
        assert "Title" in html and "bold" in html
        html2 = render_html("###body\n\nok")
        assert "body" in html2 and "<h3" in html2.lower()
        ok("markdown outline + render")
    except Exception as e:
        fail("markdown_preview", e)

    try:
        from src.desktop_pet import float_bar_tool_flags

        flags = float_bar_tool_flags(
            {"notepad": {"enabled": True}, "screen_record": {"enabled": True}}
        )
        assert flags["note"] is False
        assert flags["record"] is True
        ok("float bar never shows note chip")
    except Exception as e:
        fail("float_bar_tool_flags", e)

    try:
        from src.app import DeskTidyApp
        from src.ui.extensions_widget import ExtensionsWidget

        note_src = inspect.getsource(DeskTidyApp._on_note_requested)
        assert "open_in_desknote" in note_src
        assert "show_notepad" not in note_src
        paths_src = inspect.getsource(DeskTidyApp.open_paths_in_notepad)
        assert "open_in_desknote" in paths_src
        verb_src = inspect.getsource(DeskTidyApp._on_shell_verb)
        assert "open-notepad" in verb_src
        assert "_on_note_requested" in verb_src
        ext = inspect.getsource(ExtensionsWidget)
        assert "DeskNote" in ext
        assert "在分页栏显示「记事本」" not in ext
        assert "offer_install_desknote" not in inspect.getsource(
            ExtensionsWidget._on_notepad_enabled_changed
        )
        assert "启用 DeskNote" in ext
        assert "一并安装" in ext
        assert "sync_desknote_shortcuts" in inspect.getsource(
            ExtensionsWidget._on_notepad_enabled_changed
        )
        assert "immediate=True" in inspect.getsource(
            ExtensionsWidget._on_notepad_enabled_changed
        )
        ext_chg = inspect.getsource(DeskTidyApp._on_extensions_changed)
        assert "refresh_public_desktop" in ext_chg
        assert "_settings_ui_open" in ext_chg
        assert "_keep_overlays_under_apps" in ext_chg
        sync_src = inspect.getsource(
            __import__("src.desknote", fromlist=["sync_desknote_shortcuts"]).sync_desknote_shortcuts
        )
        assert "invalidate_loose_sync_cache" in sync_src
        assert "force=True" in sync_src
        # Fence-pinned DeskNote.lnk must not also get a public-desktop float.
        assert "path_in_pinned_keys" in sync_src
        assert "all_fence_pinned_keys" in sync_src
        deferred = inspect.getsource(DeskTidyApp._startup_deferred_guard)
        assert "sync_desknote_shortcuts" in deferred
        ok("app IPC + extensions + startup sync")
    except Exception as e:
        fail("wiring", e)

    try:
        main_src = (ROOT / "main.py").read_text(encoding="utf-8")
        assert "wants_notepad_cli" in main_src
        assert "run_desknote_process" in main_src or "open_in_desknote" in main_src
        assert (ROOT / "desknote_main.py").is_file()
        iss = (ROOT / "installer" / "DeskTidy.iss").read_text(encoding="utf-8")
        assert 'Name: "desknote"' in iss
        assert "deskNote.exe" in iss
        assert "[Components]" not in iss
        icons = iss.split("[Icons]")[1].split("[Registry]")[0]
        assert "deskNote.exe" in icons
        assert 'Parameters: "--notepad"' not in icons
        assert "CloseApplicationsFilter=DeskTidy.exe,deskNote.exe" in iss
        ok("main.py + desknote_main + installer")

        from src.win_app_id import (
            DESKNOTE_AUMID,
            DESKTIDY_AUMID,
            get_current_process_app_user_model_id,
            set_current_process_app_user_model_id,
        )

        assert DESKTIDY_AUMID != DESKNOTE_AUMID
        assert "DeskNote" in DESKNOTE_AUMID
        assert set_current_process_app_user_model_id(DESKNOTE_AUMID) is True
        assert get_current_process_app_user_model_id() == DESKNOTE_AUMID
        set_current_process_app_user_model_id(DESKTIDY_AUMID)
        assert "DESKNOTE_AUMID" in inspect.getsource(
            __import__("src.desknote", fromlist=["x"]).create_desknote_shortcut
        )
        assert "DESKNOTE_AUMID" in inspect.getsource(
            __import__("src.desknote_launch", fromlist=["x"])._run_ui
        )
        assert "DESKTIDY_AUMID" in (ROOT / "main.py").read_text(encoding="utf-8")
        ok("AppUserModelID DeskTidy≠DeskNote")

        from src.desknote_open_with import (
            _PROGID,
            sync_desknote_open_with,
            unregister_desktidy_open_with,
        )
        import winreg
        import time

        t0 = time.perf_counter()
        sync_desknote_open_with(enabled=True, force=True)
        # Second sync must be cheap (process debounce + up-to-date early-out).
        assert callable(
            __import__("src.desknote_open_with", fromlist=["x"])._desknote_open_with_up_to_date
        )
        ow_mod = __import__("src.desknote_open_with", fromlist=["x"])
        assert getattr(ow_mod, "_SYNC_DEBOUNCE_S", 0) > 0
        t_mid = time.perf_counter()
        sync_desknote_open_with(enabled=True)
        assert (time.perf_counter() - t_mid) < 0.2
        ensure_src = inspect.getsource(
            ow_mod.ensure_source_open_with_launcher
        )
        assert ensure_src.index("shutil.copy2") < ensure_src.index("_try_apply_exe_icon")
        assert "if need_copy:" in ensure_src
        elapsed = time.perf_counter() - t0
        assert elapsed < 2.0, f"open-with sync too slow: {elapsed:.2f}s"
        # Open With: ProgId + DefaultIcon + Applications\\deskNote.exe (source launcher).
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            rf"Software\Classes\{_PROGID}\shell\open\command",
        ) as key:
            cmd, _ = winreg.QueryValueEx(key, None)
        assert "desknote" in cmd.lower() or "DeskNote" in cmd
        assert "%1" in cmd
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            rf"Software\Classes\{_PROGID}\DefaultIcon",
        ) as key:
            icon, _ = winreg.QueryValueEx(key, None)
        assert "desknote" in icon.lower()
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Classes\.md\OpenWithProgids",
        ) as key:
            # ProgId must be listed for .md
            winreg.QueryValueEx(key, _PROGID)
        # .txt must NOT list DeskNote.Document — that stole「新建文本文档」.
        try:
            with winreg.OpenKey(
                winreg.HKEY_CURRENT_USER,
                r"Software\Classes\.txt\OpenWithProgids",
            ) as key:
                try:
                    winreg.QueryValueEx(key, _PROGID)
                    raise AssertionError(".txt OpenWithProgids must not list DeskNote")
                except FileNotFoundError:
                    pass
        except FileNotFoundError:
            pass
        ow = __import__("src.desknote_open_with", fromlist=["x"])
        assert ".txt" not in ow._OPEN_WITH_PROGID_SUFFIXES
        assert ".md" in ow._OPEN_WITH_PROGID_SUFFIXES
        assert callable(ow._scrub_progid_from_non_owned_suffixes)
        assert callable(ow._ensure_txt_shellnew)
        assert callable(ow._windows_notepad_txt_progid)
        assert "PostSetup" in inspect.getsource(ow._ensure_txt_shellnew)
        # After sync, .txt must have a ProgId that can open (not empty txtfilelegacy).
        ow.register_desknote_open_with(force=True)
        txt_default = ""
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, r"Software\Classes\.txt"
        ) as key:
            txt_default = str(winreg.QueryValue(key, None) or "")
        assert txt_default, "HKCU .txt default ProgId required"
        assert txt_default != _PROGID
        assert ow._progid_has_open_command(txt_default), txt_default
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, r"Software\Classes\.txt\ShellNew"
        ) as key:
            null, _ = winreg.QueryValueEx(key, "NullFile")
            assert null == ""
        # DeskTidy.exe must not remain as an Open With application.
        unregister_desktidy_open_with()
        try:
            winreg.OpenKey(
                winreg.HKEY_CURRENT_USER,
                r"Software\Classes\Applications\DeskTidy.exe",
            )
            raise AssertionError("DeskTidy.exe Applications key should be gone")
        except FileNotFoundError:
            pass
        # Applications\\deskNote.exe must exist (frozen EXE or source launcher shim).
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Classes\Applications\deskNote.exe",
        ) as key:
            name, _ = winreg.QueryValueEx(key, "FriendlyAppName")
        assert name == "DeskNote"
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Classes\Applications\deskNote.exe\DefaultIcon",
        ) as key:
            app_icon, _ = winreg.QueryValueEx(key, None)
        assert "desknote" in app_icon.lower()
        if not getattr(sys, "frozen", False):
            from src.desknote_open_with import source_open_with_launcher_path

            assert source_open_with_launcher_path().is_file()
            # Source must not keep DeskNote as the default .txt handler (Python icons).
            try:
                with winreg.OpenKey(
                    winreg.HKEY_CURRENT_USER,
                    r"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.txt\UserChoice",
                ) as key:
                    progid, _ = winreg.QueryValueEx(key, "ProgId")
                assert str(progid) != _PROGID, progid
            except FileNotFoundError:
                pass
        assert "sync_desknote_open_with" in inspect.getsource(
            __import__("src.desknote_launch", fromlist=["x"])._run_ui
        )
        assert "_release_script_launcher_userchoice" in inspect.getsource(
            __import__("src.desknote_open_with", fromlist=["x"])
        )
        ok("Open With DeskNote ProgId / no DeskTidy")
    except Exception as e:
        fail("main/installer", e)

    try:
        from src.ui.notepad_window import NotepadWindow

        src = inspect.getsource(NotepadWindow)
        assert "Markdown 预览" in src
        assert "_refresh_markdown_panes" in src
        assert "md_preview" in src
        assert "show_help" in src
        assert "open_desknote_help" in inspect.getsource(
            __import__("src.desknote_help", fromlist=["x"]).show_desknote_help
        )
        # Whole-window + editor drops: app filter + collect_drop_paths (no path paste).
        assert "viewport().installEventFilter" in inspect.getsource(NotepadWindow._make_page)
        assert "app.installEventFilter" in inspect.getsource(NotepadWindow.__init__)
        assert "collect_drop_paths" in inspect.getsource(NotepadWindow._paths_from_drop_mime)
        assert "_handle_window_file_drag" in src
        assert "_accept_file_drop" in src
        assert "_editor_filter_target" in src
        assert 'mode="open"' in inspect.getsource(NotepadWindow._handle_window_file_drag)
        accept_src = inspect.getsource(NotepadWindow._accept_file_drop)
        assert "CopyAction" in accept_src
        assert "IgnoreAction" in accept_src  # Move-only fallback
        from src.desknote_help import DESKNOTE_HELP_TOPICS, desknote_help_html

        assert len(DESKNOTE_HELP_TOPICS) >= 5
        html = desknote_help_html("markdown")
        assert "大纲" in html and "预览" in html
        ok("notepad_window markdown UI + help")
    except Exception as e:
        fail("notepad_window", e)

    try:
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PyQt6.QtCore import QMimeData, QPointF, QUrl, Qt
        from PyQt6.QtGui import QDragEnterEvent, QDropEvent
        from PyQt6.QtWidgets import QApplication

        from src.ui.notepad_window import NotepadWindow

        app = QApplication.instance() or QApplication([])
        with tempfile.TemporaryDirectory(prefix="desknote_drop_") as td:
            note = Path(td) / "hello.txt"
            note.write_text("hello desknote\n", encoding="utf-8")
            win = NotepadWindow({})
            win.show()
            app.processEvents()
            page = win._current_page()
            assert page is not None
            mime = QMimeData()
            mime.setUrls([QUrl.fromLocalFile(str(note))])
            assert win._editor_filter_target(page.editor.viewport(), page)
            assert win._widget_in_this_window(page.editor.viewport())
            pos = QPointF(8, 8)
            acts = Qt.DropAction.CopyAction | Qt.DropAction.MoveAction
            drag_enter = QDragEnterEvent(
                pos.toPoint(),
                acts,
                mime,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert win.eventFilter(page.editor.viewport(), drag_enter) is True
            assert drag_enter.isAccepted()
            # Prefer Copy cursor on enter; Drop must Ignore so source keeps icon.
            assert drag_enter.dropAction() == Qt.DropAction.CopyAction
            opened: list[Path] = []
            win._open_path_in_new_tab = lambda p: opened.append(Path(p))  # type: ignore[method-assign]
            drop = QDropEvent(
                pos,
                acts,
                mime,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert win.eventFilter(page.editor.viewport(), drop) is True
            # Opened in place; Ignore so source keeps the desktop icon.
            assert drop.dropAction() == Qt.DropAction.IgnoreAction
            assert opened and opened[0].resolve() == note.resolve()
            # Move-only offer: still open, Ignore so source keeps the icon.
            opened.clear()
            drop_move_only = QDropEvent(
                pos,
                Qt.DropAction.MoveAction,
                mime,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert win.eventFilter(page.editor.viewport(), drop_move_only) is True
            assert drop_move_only.dropAction() == Qt.DropAction.IgnoreAction
            assert opened and opened[0].resolve() == note.resolve()
            # Drop on chrome (tab bar) must also open — not only the editor viewport.
            opened.clear()
            chrome = win.tabs.tabBar()
            assert win._widget_in_this_window(chrome)
            drop2 = QDropEvent(
                pos,
                acts,
                mime,
                Qt.MouseButton.LeftButton,
                Qt.KeyboardModifier.NoModifier,
            )
            assert win.eventFilter(chrome, drop2) is True
            assert drop2.dropAction() == Qt.DropAction.IgnoreAction
            assert opened and opened[0].resolve() == note.resolve()
            win.close()
        ok("window-wide drop opens file (viewport + chrome)")
    except Exception as e:
        fail("editor drop", e)

    try:
        from src.ui import fence_icon_item as fii
        from src.ui.pet_widget import find_pet_trash_target
        from src.win_shell import try_open_paths_in_desknote_at

        # Both custom drag finishers must open in DeskNote (no OLE to DeskNote).
        fii_src = inspect.getsource(fii)
        assert fii_src.count("try_open_paths_in_desknote_at") >= 2
        assert "open in DeskNote" in fii_src
        assert callable(try_open_paths_in_desknote_at)
        # DeskNote open must run before pet trash (overlap used to recycle files).
        pub = inspect.getsource(fii.start_public_item_drag)
        assert pub.index("try_open_paths_in_desknote_at") < pub.index(
            "_try_deliver_drag_to_pet_trash"
        )
        trash_src = inspect.getsource(find_pet_trash_target)
        assert "is_desknote_window_at" in trash_src
        assert "is_visible_external_app_drop_point" in trash_src
        ok("DeskTidy drag → DeskNote open handoff")
    except Exception as e:
        fail("desknote drag handoff", e)

    try:
        from src.ui.markdown_pane import CollapsibleMdPane
        from src.ui.notepad_window import NotepadWindow

        assert "CollapsibleMdPane" in inspect.getsource(NotepadWindow._make_page)
        assert "outline_pane" in inspect.getsource(NotepadWindow._apply_markdown_layout)
        assert "preview_pane" in inspect.getsource(NotepadWindow._apply_markdown_layout)
        assert "expanded_changed" in inspect.getsource(CollapsibleMdPane)
        pane_src = inspect.getsource(CollapsibleMdPane)
        assert "notepadPaneStrip" in pane_src
        assert "handle_on_right" in pane_src
        assert "notepadPaneHeader" not in pane_src  # no up/down accordion header
        assert "_redistribute_md_splitter" in inspect.getsource(NotepadWindow)

        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        from PyQt6.QtWidgets import QApplication, QLabel, QSplitter

        app = QApplication.instance() or QApplication([])
        body = QLabel("body")
        pane = CollapsibleMdPane("大纲", body, expanded=True, handle_on_right=True)
        assert pane.is_expanded()
        seen: list[bool] = []
        pane.expanded_changed.connect(seen.append)
        pane._toggle()
        assert not pane.is_expanded()
        assert seen == [False]
        assert pane.width() == 28 or pane.maximumWidth() == 28
        assert pane._body_host.isHidden()
        pane._toggle()
        assert pane.is_expanded() and seen[-1] is True
        assert not pane._body_host.isHidden()

        # Splitter must grow the middle when sides collapse.
        win = NotepadWindow({})
        win.show()
        app.processEvents()
        page = win._current_page()
        # Force markdown layout chrome.
        page.path = Path("dummy.md")  # type: ignore[assignment]
        win._md_outline_enabled = True
        win._md_preview_enabled = True
        win._apply_markdown_layout(page)
        sp = page.splitter
        sp.resize(900, 400)
        app.processEvents()
        sp.setSizes([180, 400, 320])
        app.processEvents()
        page.outline_pane._saved_width = 180
        page.preview_pane._saved_width = 320
        page.outline_pane.set_expanded(False, emit=False)
        win._redistribute_md_splitter(page)
        app.processEvents()
        sizes = sp.sizes()
        assert sizes[0] <= 32, sizes
        assert sizes[1] >= 400, sizes  # middle grew
        page.preview_pane.set_expanded(False, emit=False)
        win._redistribute_md_splitter(page)
        app.processEvents()
        sizes = sp.sizes()
        assert sizes[2] <= 32, sizes
        assert sizes[1] >= 500, sizes
        win.close()
        ok("collapsible outline/preview panes")
    except Exception as e:
        fail("collapsible panes", e)

    try:
        from src import settings as settings_mod
        from src.ui.notepad_window import NotepadWindow

        persist_src = inspect.getsource(NotepadWindow._persist_md_settings)
        assert "patch_notepad_settings" in persist_src
        assert "immediate=True" in persist_src
        close_src = inspect.getsource(NotepadWindow.closeEvent)
        assert "_persist_md_settings" in close_src
        assert "flush_settings" in close_src

        td = Path(tempfile.mkdtemp(prefix="desktidy_md_pref_"))
        settings_file = td / "settings.json"
        disk = {
            "hotkeys": {},
            "desktop_pages": [{"id": 0, "name": "工作"}],
            "organize_rules": {},
            "theme": "mist",
            "notepad": {
                "enabled": True,
                "md_outline": True,
                "md_preview": True,
                "folder": "",
            },
            "fences": [],
        }
        settings_file.write_text(
            json.dumps(disk, ensure_ascii=False, indent=2), encoding="utf-8"
        )
        with mock.patch.object(settings_mod, "SETTINGS_FILE", settings_file), mock.patch.object(
            settings_mod, "SETTINGS_BACKUP_DIR", td / "backups"
        ), mock.patch.object(settings_mod, "ensure_app_dir", lambda: None):
            settings_mod.patch_notepad_settings(
                {"md_outline": False, "md_preview": False}, immediate=True
            )
            saved = json.loads(settings_file.read_text(encoding="utf-8"))
            assert saved["notepad"]["md_outline"] is False
            assert saved["notepad"]["md_preview"] is False
            # DeskTidy full save with stale True must not stomp DeskNote collapse.
            stale = deepcopy(disk)
            stale["notepad"] = {
                "enabled": True,
                "md_outline": True,
                "md_preview": True,
                "folder": "",
            }
            settings_mod._save_settings_now(stale)
            saved2 = json.loads(settings_file.read_text(encoding="utf-8"))
            assert saved2["notepad"]["md_outline"] is False, saved2["notepad"]
            assert saved2["notepad"]["md_preview"] is False, saved2["notepad"]
        ok("outline/preview collapse remembered (patch + no stomp)")
    except Exception as e:
        fail("md pane memory", e)

    try:
        from src.ui.notepad_window import NotepadWindow

        finish = inspect.getsource(NotepadWindow._finish_remove_tab)
        assert "new_document()" not in finish
        assert "self.close()" not in finish
        assert "_persist_session()" in finish
        lib_del = inspect.getsource(NotepadWindow._on_library_delete)
        after = lib_del.split("if not self._pages:", 1)[1][:420]
        assert "new_document()" not in after
        assert "self.close()" not in after
        ok("last tab close keeps window (no auto 未命名)")
    except Exception as e:
        fail("last tab close", e)

    if failures:
        print(f"\n{len(failures)} failure(s)")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("\nAll deskNote checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
