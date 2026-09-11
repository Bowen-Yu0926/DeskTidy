"""Regression for fence keyboard verbs + shell file clipboard."""

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

passes: list[str] = []
failures: list[tuple[str, str]] = []


def run(name: str, fn) -> None:
    try:
        fn()
        passes.append(name)
        print(f"  OK  {name}")
    except Exception as exc:
        failures.append((name, f"{exc}\n{traceback.format_exc()}"))
        print(f"  FAIL {name}: {exc}")


def test_clipboard_owner_not_overlay() -> None:
    """OpenClipboard must not borrow FenceWidget HWNDs (VPN-like flash)."""
    import inspect

    from PyQt6.QtWidgets import QApplication, QWidget

    from src import shell_clipboard as sc

    src = inspect.getsource(sc._clipboard_owner_hwnd)
    assert "topLevelWidgets" not in src
    assert "_create_message_only_clipboard_hwnd" in src
    assert "CreateWindowExW" in inspect.getsource(sc._create_message_only_clipboard_hwnd)
    assert "HWND_MESSAGE" in inspect.getsource(sc._create_message_only_clipboard_hwnd)
    assert "CreateWindowExW.restype" in inspect.getsource(sc._user32_hwnd_api)

    app = QApplication.instance() or QApplication([])
    # A visible top-level must not become the clipboard owner.
    decoy = QWidget()
    decoy.setObjectName("fakeFence")
    decoy.resize(200, 120)
    decoy.show()
    app.processEvents()
    decoy_hwnd = int(decoy.winId())
    assert decoy_hwnd

    sc._owner_hwnd = 0
    sc._owner_widget = None
    owner = sc._clipboard_owner_hwnd()
    assert owner, "dedicated clipboard owner must be creatable"
    assert owner != decoy_hwnd
    decoy.close()
    decoy.deleteLater()
    app.processEvents()


def test_clipboard_roundtrip() -> None:
    from unittest.mock import patch

    from src.shell_clipboard import clipboard_set_files

    tmp = Path(tempfile.mkdtemp(prefix="desktidy_clip_"))
    a = tmp / "a.txt"
    b = tmp / "b.txt"
    a.write_text("a", encoding="utf-8")
    b.write_text("b", encoding="utf-8")
    ok = clipboard_set_files([a, b], cut=False)
    if ok:
        import ctypes
        from ctypes import wintypes

        from src.shell_clipboard import (
            DROPEFFECT_COPY,
            DROPEFFECT_MOVE,
            clipboard_get_files,
            clipboard_preferred_effect,
        )

        got = clipboard_get_files()
        assert {str(p).casefold() for p in got} == {
            str(a).casefold(),
            str(b).casefold(),
        }
        assert clipboard_preferred_effect() == DROPEFFECT_COPY

        # WeChat / QQ need FileNameW (+ Shell IDList) beyond bare CF_HDROP.
        # Qt setUrls also adds UniformResourceLocator — WeChat turns that into a.txt.
        import time

        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.OpenClipboard.argtypes = [wintypes.HWND]
        user32.OpenClipboard.restype = wintypes.BOOL
        user32.CloseClipboard.restype = wintypes.BOOL
        user32.IsClipboardFormatAvailable.argtypes = [wintypes.UINT]
        user32.IsClipboardFormatAvailable.restype = wintypes.BOOL
        user32.RegisterClipboardFormatW.argtypes = [wintypes.LPCWSTR]
        user32.RegisterClipboardFormatW.restype = wintypes.UINT
        user32.GetClipboardData.argtypes = [wintypes.UINT]
        user32.GetClipboardData.restype = wintypes.HANDLE
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GlobalLock.argtypes = [wintypes.HGLOBAL]
        kernel32.GlobalLock.restype = ctypes.c_void_p
        kernel32.GlobalUnlock.argtypes = [wintypes.HGLOBAL]

        opened = False
        for _ in range(16):
            if user32.OpenClipboard(None):
                opened = True
                break
            time.sleep(0.03)
        assert opened, "OpenClipboard denied (another app holding clipboard?)"
        try:
            fmt_w = user32.RegisterClipboardFormatW("FileNameW")
            fmt_ida = user32.RegisterClipboardFormatW("Shell IDList Array")
            fmt_url = user32.RegisterClipboardFormatW("UniformResourceLocator")
            fmt_url_w = user32.RegisterClipboardFormatW("UniformResourceLocatorW")
            assert user32.IsClipboardFormatAvailable(15)  # CF_HDROP
            assert user32.IsClipboardFormatAvailable(fmt_w)
            assert user32.IsClipboardFormatAvailable(fmt_ida)
            assert not user32.IsClipboardFormatAvailable(fmt_url)
            assert not user32.IsClipboardFormatAvailable(fmt_url_w)
            handle = user32.GetClipboardData(fmt_w)
            assert handle
            ptr = kernel32.GlobalLock(handle)
            assert ptr
            name = ctypes.wstring_at(ptr)
            kernel32.GlobalUnlock(handle)
            assert name.casefold() == str(a.resolve(strict=False)).casefold()
            assert "\\" in name or (len(name) >= 3 and name[1] == ":")
        finally:
            user32.CloseClipboard()

        assert clipboard_set_files([a], cut=True)
        assert clipboard_preferred_effect() == DROPEFFECT_MOVE
    else:
        # Agent/CI clipboard lock (OpenClipboard ACCESS_DENIED) — still verify fallback.
        with patch("src.shell_clipboard._open_clipboard", return_value=False):
            with patch("src.shell_clipboard._set_files_qt", return_value=True) as qt:
                assert clipboard_set_files([a], cut=True) is True
                qt.assert_called()

    # Contract: companion formats stay wired for WeChat paste; Win32 preferred.
    import inspect

    from src import shell_clipboard as sc

    src = inspect.getsource(sc.clipboard_set_files)
    assert "FileNameW" in src or "_companion_shell_format_bytes" in src
    assert "_write_win32_file_formats" in src
    assert "UniformResourceLocator" in src
    assert "FileNameW" in inspect.getsource(sc._companion_shell_format_bytes)
    assert "Shell IDList Array" in inspect.getsource(sc._companion_shell_format_bytes)
    assert "replace(\"/\", \"\\\\\")" in inspect.getsource(sc._normalize_paths)
    open_src = inspect.getsource(sc._open_clipboard)
    assert "OpenClipboard(None)" in open_src
    assert open_src.index("OpenClipboard(None)") < open_src.index(
        "_clipboard_owner_hwnd"
    )
    assert "allow_sleep" in open_src
    assert "time.sleep(0.01)" in open_src
    assert "attempts = 8 if allow_sleep else 3" in open_src
    assert "_try_win32" in inspect.getsource(sc.clipboard_set_files)
    assert "for attempt in range(8):" in inspect.getsource(sc.clipboard_set_files)
    assert callable(sc.clipboard_get_files_with_effect)
    with_fx = inspect.getsource(sc.clipboard_get_files_with_effect)
    assert "Preferred DropEffect" in with_fx
    assert "CF_HDROP" in with_fx or "GetClipboardData(CF_HDROP)" in with_fx
    from src.ui import fence_icon_item as fii

    paste_fence = inspect.getsource(fii.paste_files_into_fence)
    assert "clipboard_get_files_with_effect" in paste_fence
    assert "clipboard_preferred_effect" not in paste_fence
    paste_pub = inspect.getsource(fii.paste_files_to_public)
    assert "clipboard_get_files_with_effect" in paste_pub


def test_keyboard_and_rename_contracts() -> None:
    import inspect

    from src.shell_file_menu import _host_rename_item
    from src.ui import fence_icon_item as fii
    from src.ui.fence_icon_item import FenceIconItem, handle_fence_item_key
    from src.ui.fence_widget import FenceWidget
    from src.ui.public_icon_widget import PublicIconWidget

    assert "begin_inplace_rename" in inspect.getsource(_host_rename_item)
    assert "find_item_widget_for_path" in inspect.getsource(_host_rename_item)
    assert "commit_filesystem_rename" in inspect.getsource(_host_rename_item)
    assert "try_inplace_rename_on_label_click" in inspect.getsource(FenceIconItem.mouseReleaseEvent)
    assert "try_inplace_rename_on_label_click" in inspect.getsource(PublicIconWidget.mouseReleaseEvent)
    assert "mark_item_opened_by_double_click" in inspect.getsource(FenceIconItem.mouseDoubleClickEvent)
    assert "mark_item_opened_by_double_click" in inspect.getsource(PublicIconWidget.mouseDoubleClickEvent)
    assert "doubleClickInterval" in inspect.getsource(fii._arm_pending_label_rename)
    assert "begin_inplace_rename" not in inspect.getsource(fii.try_inplace_rename_on_label_click)
    assert "Key_F2" in inspect.getsource(handle_fence_item_key)
    assert "Key_Delete" in inspect.getsource(handle_fence_item_key)
    assert "dispatch_desktop_icon_chord" in inspect.getsource(handle_fence_item_key)
    assert "paste_files_into_context" in inspect.getsource(fii.dispatch_desktop_icon_chord)
    assert "ensure_desktop_icon_key_hook" in inspect.getsource(fii)
    assert "register_ll_keyboard_handler" in inspect.getsource(fii.ensure_desktop_icon_key_hook)
    assert "is_desktop_foreground" in inspect.getsource(fii.resolve_desktop_key_anchor)
    assert "hit_test_region" in inspect.getsource(fii._public_under_cursor)
    assert "_LL_VK_CTRL_CHORDS" in inspect.getsource(fii)
    assert "_ll_should_claim_chord" in inspect.getsource(fii._on_ll_desktop_icon_key)
    assert "resolve_desktop_key_anchor" not in inspect.getsource(fii._on_ll_desktop_icon_key)
    assert "QueuedConnection" in inspect.getsource(fii.ensure_desktop_icon_key_hook)
    assert "handle_fence_item_key" in inspect.getsource(FenceIconItem.keyPressEvent)
    assert "handle_fence_item_key" in inspect.getsource(PublicIconWidget.keyPressEvent)
    assert "keyPressEvent" in inspect.getsource(FenceWidget)
    assert "dissolve_requested.emit" in inspect.getsource(FenceWidget._on_header_context_menu)
    # Blank RMB prepends DeskTidy dissolve above Explorer (not bare None).
    menu_src = inspect.getsource(FenceWidget._show_fence_context_menu)
    assert "_fence_shell_extra_commands()" in menu_src
    assert "extra_commands=None" not in menu_src
    assert "rewrite_pinned_path" in inspect.getsource(fii.commit_filesystem_rename)


def test_ll_keyboard_hook_contract() -> None:
    from src.desktop_ll_keyboard import (
        register_ll_keyboard_handler,
        shutdown_ll_keyboard,
        subscriber_count,
        unregister_ll_keyboard_handler,
    )

    seen: list[int] = []

    def handler(vk: int, flags: int) -> bool:
        seen.append(vk)
        return False

    register_ll_keyboard_handler(handler)
    assert subscriber_count() >= 1
    unregister_ll_keyboard_handler(handler)
    shutdown_ll_keyboard()
    assert subscriber_count() == 0


def test_dispatch_copy_paths() -> None:
    from unittest.mock import patch

    from PyQt6.QtWidgets import QApplication, QWidget

    from src.ui import fence_icon_item as fii
    from src.ui.fence_icon_item import dispatch_desktop_icon_chord

    _ = QApplication.instance() or QApplication([])
    tmp = Path(tempfile.mkdtemp(prefix="desktidy_keyclip_"))
    a = tmp / "clip_a.txt"
    a.write_text("a", encoding="utf-8")

    class _FakeIcon(QWidget):
        def __init__(self) -> None:
            super().__init__()
            self.file_path = a
            self._selected = True

        def is_selected(self) -> bool:
            return True

    icon = _FakeIcon()
    with patch("src.shell_clipboard.clipboard_set_files", return_value=True) as mocked:
        with patch("src.ui.toast.show_toast"):
            with patch("src.ui.fence_icon_item.set_clipboard_cut_paths") as ghost:
                assert dispatch_desktop_icon_chord("copy", icon)
                mocked.assert_called()
                assert mocked.call_args.kwargs.get("cut") is False
                ghost.assert_called_with(None)
                assert dispatch_desktop_icon_chord("cut", icon)
                assert mocked.call_args.kwargs.get("cut") is True
                ghost.assert_called()
                assert list(ghost.call_args.args[0]) == [a]
    # Failed apply must not latch debounce — immediate retry stays allowed.
    fii._last_desktop_chord = ""
    fii._last_desktop_chord_ms = 0.0
    with patch("src.ui.fence_icon_item._ll_should_claim_chord", return_value=True):
        with patch("src.ui.fence_icon_item.dispatch_desktop_icon_chord", return_value=False):
            fii._apply_desktop_icon_chord("copy")
            assert fii._last_desktop_chord_ms == 0.0
            fii._apply_desktop_icon_chord("copy")  # second call not blocked
        with patch(
            "src.ui.fence_icon_item.dispatch_desktop_icon_chord", return_value=True
        ) as ok_dispatch:
            fii._apply_desktop_icon_chord("copy")
            assert ok_dispatch.call_count == 1
            assert fii._last_desktop_chord == "copy"
            assert fii._last_desktop_chord_ms > 0.0
            fii._apply_desktop_icon_chord("copy")  # within 60ms — blocked
            assert ok_dispatch.call_count == 1


def test_scrub_removes_public_not_dest_fence() -> None:
    """After pin claim, public float is dropped; destination fence keeps the cell."""
    from types import SimpleNamespace
    from unittest.mock import MagicMock

    from PyQt6.QtWidgets import QApplication

    from src.ui import fence_icon_item as fii

    _ = QApplication.instance() or QApplication([])
    tmp = Path(tempfile.mkdtemp(prefix="desktidy_scrub_"))
    doc = tmp / "doc.txt"
    doc.write_text("x", encoding="utf-8")

    rem_pub = MagicMock()
    dest_remove = MagicMock(return_value=True)
    peer_remove = MagicMock(return_value=True)
    dest = SimpleNamespace(
        config={"id": "dest", "virtual_items": [str(doc)]},
        _remove_virtual_icon_widgets=dest_remove,
    )
    peer = SimpleNamespace(
        config={"id": "peer", "virtual_items": []},
        _remove_virtual_icon_widgets=peer_remove,
    )
    desk = SimpleNamespace(
        _remove_public_icon_widget=rem_pub,
        fences=[dest, peer],
        public_icons=[],
        _parked_public_icons={},
    )
    app = QApplication.instance()
    prev = getattr(app, "_desktidy_app", None)
    app._desktidy_app = desk  # type: ignore[attr-defined]
    try:
        fii._cut_path_keys = {fii._norm_cut_path_key(doc)}
        fii.scrub_live_icons_for_claimed_paths([doc])
        rem_pub.assert_called()
        assert any(str(c.args[0]) == str(doc) for c in rem_pub.call_args_list)
        dest_remove.assert_not_called()
        peer_remove.assert_called()
        dropped = peer_remove.call_args.args[0]
        assert [str(p) for p in dropped] == [str(doc)]
    finally:
        fii._cut_path_keys = set()
        if prev is None:
            try:
                delattr(app, "_desktidy_app")
            except Exception:
                app._desktidy_app = None  # type: ignore[attr-defined]
        else:
            app._desktidy_app = prev  # type: ignore[attr-defined]
        import shutil

        shutil.rmtree(tmp, ignore_errors=True)


def test_copy_resolve_and_selection_live_contracts() -> None:
    """Ctrl+C must resolve icon-under-cursor and keep LL selection_live after marquee."""
    import inspect

    from src.ui import fence_icon_item as fii
    from src.ui.fence_widget import FenceWidget

    resolve = inspect.getsource(fii.resolve_desktop_key_anchor)
    assert "_fence_item_at" in resolve
    assert "_desktop_selection_live" in resolve
    assert "owned_empty_plate" in resolve
    assert "_any_selected_desktop_anchor" in resolve
    # Must not hard-fail on fence chrome when another selection exists.
    assert "Cursor over fence chrome with no selection — no copy target" not in resolve
    # Empty PublicIconHost plate must fall through (not return None immediately).
    assert "Empty PublicIconHost plate" in resolve or "full-desktop 框选 surface" in resolve

    iter_src = inspect.getsource(fii.iter_fence_selectable_items)
    assert "parent: QWidget | None = anchor" in iter_src or "parent = anchor" in iter_src.replace(
        " ", ""
    )
    # Start at anchor (FenceWidget), not anchor.parent().
    assert "anchor.parent()" not in iter_src.split("while")[0]

    finish = inspect.getsource(FenceWidget._finish_marquee)
    assert "mark_desktop_selection_live" in finish
    update = inspect.getsource(FenceWidget._update_marquee)
    assert "mark_desktop_selection_live" in update

    keys = inspect.getsource(FenceWidget.keyPressEvent)
    assert 'dispatch_desktop_icon_chord("copy"' in keys
    assert 'dispatch_desktop_icon_chord("cut"' in keys

    ll = inspect.getsource(fii._on_ll_desktop_icon_key)
    assert "_ll_should_claim_chord_hook" in ll
    assert "return True" in ll
    claim = inspect.getsource(fii._ll_should_claim_chord)
    assert "is_desktop_foreground" in claim
    assert "_cursor_over_icon_item" in claim
    assert "_desktop_selection_live" in claim
    hook_claim = inspect.getsource(fii._ll_should_claim_chord_hook)
    assert "widgetAt(" not in hook_claim
    assert "is_desktop_foreground(" not in hook_claim
    assert "_hook_desktop_ctx" in hook_claim
    assert "_refresh_hook_desktop_ctx" in inspect.getsource(fii.mark_desktop_selection_live)
    # Must not claim Ctrl+C solely because the cursor is on page chrome.
    assert "_cursor_over_our_process_window()" in claim
    # Paste must not claim bare "ours" (dock/page chrome ate Ctrl+V).
    assert "_cursor_over_desktop_paste_surface" in claim
    assert "is_explorer_desktop_foreground" in claim
    # Copy under foreign FG only when pointer is on an icon (not empty fence).
    copy_branch = claim.split("copy/cut", 1)[1]
    assert "_cursor_over_icon_item" in copy_branch
    assert "is_desktop_foreground" in copy_branch
    icon_hit = inspect.getsource(fii._cursor_over_icon_item)
    assert "FenceIconItem" in icon_hit
    assert "PageIndicatorWidget" in icon_hit
    paste_surf = inspect.getsource(fii._cursor_over_desktop_paste_surface)
    assert "FenceWidget" in paste_surf
    assert "PageIndicatorWidget" in paste_surf
    assert "PublicIconHost" in paste_surf
    dispatch = inspect.getsource(fii.dispatch_desktop_icon_chord)
    assert "复制失败" in dispatch
    assert "无法复制" in dispatch
    assert "select_fence_item" in dispatch
    assert "无法粘贴" in dispatch
    assert "粘贴失败" in dispatch
    apply = inspect.getsource(fii._apply_desktop_icon_chord)
    # Failed copy must not arm the debounce lock (short-tap / key-repeat bug).
    # Success debounce is brief (~60ms) — 280ms blocked the next short Ctrl+C.
    assert "if ok:" in apply
    assert '_refresh_hook_desktop_ctx()' in apply
    assert 'chord == "paste"' in apply and "_ll_should_claim_chord(" in apply
    assert "_last_desktop_chord_ms = now" in apply
    assert apply.index("dispatch_desktop_icon_chord") < apply.index("if ok:")
    assert "60.0" in apply
    assert "280.0" not in apply
    ctrl = inspect.getsource(
        __import__("src.desktop_ll_keyboard", fromlist=["x"]).ctrl_physically_down
    )
    assert "_ctrl_chord_grace_until" in ctrl or "_ctrl_vks_down" in ctrl
    assert "GetKeyState" in ctrl
    track = inspect.getsource(
        __import__("src.desktop_ll_keyboard", fromlist=["x"])._track_modifier
    )
    assert "_ctrl_chord_grace_until" in track
    open_clip = inspect.getsource(
        __import__("src.shell_clipboard", fromlist=["x"])._open_clipboard
    )
    assert "attempts = 8 if allow_sleep else 3" in open_clip
    set_files = inspect.getsource(
        __import__("src.shell_clipboard", fromlist=["x"]).clipboard_set_files
    )
    assert "_try_win32" in set_files
    assert "for attempt in range(8):" in set_files
    llk = inspect.getsource(__import__("src.desktop_ll_keyboard", fromlist=["x"]))
    assert "_track_modifier" in llk
    assert "_ctrl_vks_down" in llk
    assert "WM_KEYUP" in llk
    resolve = inspect.getsource(fii.resolve_desktop_key_anchor)
    assert "_active_public_icon_host" in resolve
    assert "is_explorer_desktop_foreground" in resolve


def test_inplace_rename_commit() -> None:
    import inspect

    from PyQt6.QtWidgets import QApplication

    from src.fence_rules import rewrite_pinned_path
    from src.ui import fence_icon_item as fii
    from src.ui.fence_icon_item import commit_filesystem_rename

    _ = QApplication.instance() or QApplication([])
    tmp = Path(tempfile.mkdtemp(prefix="desktidy_ren_"))
    src = tmp / "old name.txt"
    src.write_text("x", encoding="utf-8")
    settings = {"fences": [{"id": "f1", "virtual_items": [str(src)]}], "public_desktop_items": []}
    dest = commit_filesystem_rename(src, "new name.txt")
    assert dest is not None
    assert dest.exists() and dest.name == "new name.txt"
    assert not src.exists()
    # Stem-only edit must keep the original extension (Explorer-like).
    xlsx = tmp / "Sheet.xlsx"
    xlsx.write_text("x", encoding="utf-8")
    dest2 = commit_filesystem_rename(xlsx, "报表")
    assert dest2 is not None and dest2.name == "报表.xlsx"
    assert dest2.exists() and not xlsx.exists()
    src_commit = inspect.getsource(commit_filesystem_rename)
    assert "refresh_fences" not in src_commit
    assert "refresh_public_desktop" not in src_commit
    assert "notify_shell_item_moved" in src_commit
    assert "suppress_desktop_item" in src_commit
    assert "_sync_fence_virtual_items" not in src_commit
    assert "note_item_path_changed" not in src_commit
    finish = inspect.getsource(fii.begin_inplace_rename)
    assert "apply_item_renamed_path" in finish
    assert "grabKeyboard" in finish
    assert "releaseKeyboard" in finish
    assert "keyboard_grabbed" in finish
    assert "keyboardGrabber" not in finish
    assert "WindowType.Tool" in finish
    assert "activateWindow" in finish
    assert "_teardown_rename_editor" in finish
    assert "rename_ready" in finish
    assert "cur is edit" in finish
    assert "cur is item" not in finish
    assert "Key_Return" in finish
    assert "from_key" in finish
    assert "refresh_needed.emit" not in finish
    assert callable(fii.looks_like_shell_new_item)
    assert fii.looks_like_shell_new_item("新建文件夹")
    assert fii.looks_like_shell_new_item(Path(r"D:\desktop\New folder"))
    assert not fii.looks_like_shell_new_item("报告.docx")
    assert callable(fii.maybe_begin_rename_for_shell_new_item)
    apply_src = inspect.getsource(fii.apply_item_renamed_path)
    assert "note_item_path_changed" in apply_src
    assert "find_fence_widget" in apply_src
    # Direct pin rewrite helper.
    other = tmp / "pin.txt"
    other.write_text("p", encoding="utf-8")
    settings["fences"][0]["virtual_items"] = [str(other)]
    renamed = other.with_name("pin2.txt")
    other.rename(renamed)
    assert rewrite_pinned_path(settings, other, renamed) is True
    assert settings["fences"][0]["virtual_items"] == [str(renamed)]


def test_paste_move_rewrites_pins_and_copies_dirs() -> None:
    """Cut-paste of off-desktop files must rewrite pins; copy must support folders."""
    import inspect
    import shutil
    from unittest.mock import patch

    from PyQt6.QtWidgets import QApplication, QWidget

    from src.ui import fence_icon_item as fii

    paste_src = inspect.getsource(fii.paste_files_into_fence)
    assert "clipboard_get_files_with_effect" in paste_src
    assert "_unpack_paste_result" in paste_src
    assert "suppress_desktop_item" in paste_src
    assert "_rewrite_pins_after_fs_move" in paste_src
    assert "_land_paste_sources_to_folder" in paste_src
    assert "_demote_clipboard_after_cut_paste" in paste_src
    assert "quiet_finish=True" in paste_src
    assert "invalidate_path_stat_cache" in paste_src
    from src.ui.fence_widget import FenceWidget

    # Quiet paste must still scrub live public floats (settings-only used to leave ghosts).
    apply_src = inspect.getsource(FenceWidget._apply_virtual_drop_paths)
    assert "scrub_live_icons_for_claimed_paths" in apply_src
    assert "quiet_finish" in apply_src
    assert callable(fii.scrub_live_icons_for_claimed_paths)
    assert callable(fii.set_clipboard_cut_paths)
    assert callable(fii.apply_cut_ghost_visual)
    dispatch = inspect.getsource(fii.dispatch_desktop_icon_chord)
    assert "set_clipboard_cut_paths(paths)" in dispatch
    assert "set_clipboard_cut_paths(None)" in dispatch
    demote = inspect.getsource(fii._demote_clipboard_after_cut_paste)
    assert "set_clipboard_cut_paths(None)" in demote
    land_src = inspect.getsource(fii._land_paste_sources_to_folder)
    assert "moves_ok" in land_src
    assert "fall back to copy" in land_src or "does **not** fall back to copy" in land_src
    assert "_fs_copy_entry" in land_src
    assert "invalidate_path_stat_cache" in land_src
    portal_finish = paste_src.split("def _finish_portal", 1)[1].split(
        "desk = get_desktop_path()", 1
    )[0]
    assert "files_changed.emit" not in portal_finish
    # Paste must rebuild immediately (stale path_present cache used to hide icons).
    assert "refresh(force=True)" in portal_finish
    assert "scrub_live_icons_for_claimed_paths" in portal_finish
    assert "remove_public_paths" in portal_finish
    finish_ui = inspect.getsource(FenceWidget._finish_virtual_drop_ui)
    assert "quiet: bool" in finish_ui
    # Quiet paste still clears signature so new pins paint immediately.
    quiet_branch = finish_ui.split("if quiet:", 1)[1]
    assert "_last_refresh_sig = None" in quiet_branch[:350]
    assert "_refresh_impl()" in quiet_branch[:500]
    # Copy of an already-desktop file must create a new path (not silent no-op).
    assert "_on_desktop(src) and effect == DROPEFFECT_MOVE" in paste_src
    assert "_land_paste_sources_to_folder" in inspect.getsource(fii.paste_files_to_public)
    resolve = inspect.getsource(fii.resolve_desktop_key_anchor)
    assert 'chord == "paste"' in resolve
    assert "_active_public_icon_host" in resolve
    claim = inspect.getsource(fii._ll_should_claim_chord)
    assert "_inplace_rename_active" in claim
    assert 'chord == "paste"' in claim
    assert "_cursor_over_desktop_paste_surface" in claim
    assert "is_explorer_desktop_foreground" in claim
    # Must not claim paste on any DeskTidy HWND (page chrome / dock).
    paste_branch = claim.split('chord == "paste"', 1)[1].split("copy/cut", 1)[0]
    assert "if ours:" not in paste_branch
    # Foreign FG (WeChat/Office) must keep Ctrl+V — do not steal from chat paste.
    assert "is_desktop_foreground" in paste_branch
    assert paste_branch.index("is_desktop_foreground") < paste_branch.index(
        "_cursor_over_desktop_paste_surface"
    )
    assert "已粘贴" in inspect.getsource(fii.dispatch_desktop_icon_chord)
    assert "无法粘贴" in inspect.getsource(fii.dispatch_desktop_icon_chord)

    tmp = Path(tempfile.mkdtemp(prefix="desktidy_paste_"))
    desk = tmp / "Desktop"
    desk.mkdir()
    docs = tmp / "Documents"
    docs.mkdir()
    src = docs / "report.txt"
    src.write_text("r", encoding="utf-8")
    folder = docs / "photos"
    folder.mkdir()
    (folder / "a.jpg").write_text("j", encoding="utf-8")

    settings = {
        "fences": [
            {
                "id": "f1",
                "name": "文档",
                "virtual_items": [str(src)],
                "extensions": [],
            }
        ],
        "public_desktop_items": [],
        "exclude_patterns": [],
    }

    class _Fence(QWidget):
        def __init__(self) -> None:
            super().__init__()
            self.config = settings["fences"][0]
            self.settings = settings
            self.refresh_calls = 0

        def refresh(self, force: bool = False) -> None:  # noqa: ARG002
            self.refresh_calls += 1

    _ = QApplication.instance() or QApplication([])

    def _sync_background_paste(work, *, on_success, on_failure, **kwargs):  # noqa: ARG001
        try:
            on_success(work())
        except BaseException as exc:  # noqa: BLE001
            on_failure(exc)

    fence = _Fence()

    with (
        patch.object(fii, "find_fence_widget", return_value=fence),
        patch.object(fii, "_start_background_fs_paste", side_effect=_sync_background_paste),
        patch(
            "src.shell_clipboard.clipboard_get_files_with_effect",
            return_value=([src], 2),  # DROPEFFECT_MOVE
        ),
        patch("src.settings.get_desktop_path", return_value=desk),
        patch("src.settings.get_desktop_paths", return_value=[desk]),
        patch("src.settings.save_settings"),
        patch.object(fii, "_demote_clipboard_after_cut_paste") as demote,
    ):
        assert fii.paste_files_into_fence(fence) is True
        demote.assert_called()
    dest = desk / "report.txt"
    assert dest.exists() and not src.exists()
    pins = [str(p).casefold() for p in settings["fences"][0]["virtual_items"]]
    assert str(dest).casefold() in pins
    assert str(src).casefold() not in pins

    # Directory copy into fence.
    with (
        patch.object(fii, "find_fence_widget", return_value=fence),
        patch.object(fii, "_start_background_fs_paste", side_effect=_sync_background_paste),
        patch(
            "src.shell_clipboard.clipboard_get_files_with_effect",
            return_value=([folder], 1),  # COPY
        ),
        patch("src.settings.get_desktop_path", return_value=desk),
        patch("src.settings.get_desktop_paths", return_value=[desk]),
        patch("src.settings.save_settings"),
    ):
        assert fii.paste_files_into_fence(fence) is True
    copied = desk / "photos"
    assert copied.is_dir() and (copied / "a.jpg").exists()

    # Copy of already-desktop file must create a sibling (not silent pin no-op).
    on_desk = desk / "note.txt"
    on_desk.write_text("n", encoding="utf-8")
    settings["fences"][0]["virtual_items"] = [str(on_desk)]
    with (
        patch.object(fii, "find_fence_widget", return_value=fence),
        patch.object(fii, "_start_background_fs_paste", side_effect=_sync_background_paste),
        patch(
            "src.shell_clipboard.clipboard_get_files_with_effect",
            return_value=([on_desk], 1),  # COPY
        ),
        patch("src.settings.get_desktop_path", return_value=desk),
        patch("src.settings.get_desktop_paths", return_value=[desk]),
        patch("src.settings.save_settings"),
    ):
        assert fii.paste_files_into_fence(fence) is True
    twins = list(desk.glob("note*.txt"))
    assert len(twins) >= 2, twins
    assert on_desk.exists()
    shutil.rmtree(tmp, ignore_errors=True)

    # Stale path_present=False must not hide a just-pasted pin from the grid.
    from PyQt6.QtCore import Qt
    from src import path_stat_cache as psc
    from src.shell_clipboard import DROPEFFECT_COPY
    from src.ui.fence_widget import FenceWidget as LiveFence

    td2 = Path(tempfile.mkdtemp(prefix="desktidy_paste_ui_"))
    try:
        desk2 = td2 / "Desktop"
        desk2.mkdir()
        src2 = desk2 / "note.txt"
        src2.write_text("n", encoding="utf-8")
        poison = desk2 / "note_1.txt"
        psc.invalidate_path_stat_cache()
        psc._fetch(poison)  # present=False cached
        assert psc.path_present(poison) is False
        cfg2 = {
            "id": "f_ui",
            "name": "测",
            "virtual_items": [str(src2)],
            "sort_by": "manual",
            "width": 320,
            "height": 360,
            "style": {"show_title": True, "collapsible": True},
            "pages": [0],
        }
        settings2 = {
            "theme": "mist",
            "fences": [cfg2],
            "exclude_patterns": [],
            "public_desktop_items": [],
            "current_page": 0,
        }
        live = LiveFence(cfg2, settings2)
        try:
            live.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
            live.resize(320, 360)
            live.show()
            live.refresh(force=True)
            app = QApplication.instance() or QApplication([])
            app.processEvents()
            assert live.items_layout.count() == 1
            with (
                patch("src.settings.get_desktop_path", return_value=desk2),
                patch("src.settings.get_desktop_paths", return_value=[desk2]),
                patch("src.settings.save_settings"),
            ):
                assert (
                    fii.paste_files_into_fence(
                        live, sources=[src2], effect=DROPEFFECT_COPY
                    )
                    is True
                )
            app.processEvents()
            assert live.items_layout.count() == 2, [
                Path(p).name for p in live._get_entries()
            ]
            assert psc.path_present(desk2 / "note_1.txt") is True
        finally:
            live.close()
            live.deleteLater()
            app = QApplication.instance()
            if app is not None:
                app.processEvents()
    finally:
        shutil.rmtree(td2, ignore_errors=True)


def test_label_click_rename_gates() -> None:
    from unittest.mock import patch

    from PyQt6.QtCore import QPoint
    from PyQt6.QtWidgets import QApplication, QLabel, QWidget

    from src.ui.fence_icon_item import (
        cancel_pending_label_rename,
        mark_item_opened_by_double_click,
        try_inplace_rename_on_label_click,
    )

    _ = QApplication.instance() or QApplication([])
    w = QWidget()
    w.resize(100, 108)
    icon = QLabel(w)
    icon.setGeometry(26, 4, 48, 48)
    w.icon_label = icon
    label = QLabel("x.txt", w)
    label.setGeometry(0, 56, 100, 44)
    w.text_label = label
    w.is_selected = lambda: True
    w.file_path = Path("x.txt")

    on_label = QPoint(50, 64)
    on_icon = QPoint(50, 20)
    on_caption_pad = QPoint(6, 92)

    with patch("src.ui.fence_icon_item.begin_inplace_rename", return_value=True) as bip:
        assert try_inplace_rename_on_label_click(
            w, was_selected=True, press_pos=on_label, release_pos=on_label
        )
        # Explorer: wait out the double-click interval — do not steal open.
        bip.assert_not_called()
        timer = getattr(w, "_pending_rename_timer", None)
        assert timer is not None
        timer.timeout.emit()
        bip.assert_called_once_with(w)

        bip.reset_mock()
        assert not try_inplace_rename_on_label_click(
            w, was_selected=True, press_pos=on_icon, release_pos=on_icon
        )
        bip.assert_not_called()
        assert getattr(w, "_pending_rename_timer", None) is None

        assert not try_inplace_rename_on_label_click(
            w, was_selected=True, press_pos=on_caption_pad, release_pos=on_caption_pad
        )
        bip.assert_not_called()

        bip.reset_mock()
        assert not try_inplace_rename_on_label_click(
            w, was_selected=False, press_pos=on_label, release_pos=on_label
        )
        bip.assert_not_called()

        # Double-click open cancels a pending caption rename.
        assert try_inplace_rename_on_label_click(
            w, was_selected=True, press_pos=on_label, release_pos=on_label
        )
        mark_item_opened_by_double_click(w)
        assert getattr(w, "_pending_rename_timer", None) is None
        assert not try_inplace_rename_on_label_click(
            w, was_selected=True, press_pos=on_label, release_pos=on_label
        )
        bip.assert_not_called()

        cancel_pending_label_rename(w)


def test_copy_empty_host_plate_uses_live_selection() -> None:
    """Wallpaper cursor on full-desktop PublicIconHost must still copy fence selection.

    After shell icons are hidden, the public host click-mask covers the whole
    monitor. Cursor-on-empty-plate used to ``return None`` →「无法复制」even when
    a fence icon was already selected.
    """
    from unittest.mock import patch

    from PyQt6.QtWidgets import QApplication, QWidget

    from src.ui import fence_icon_item as fii

    _ = QApplication.instance() or QApplication([])

    class _FakeFenceIcon(QWidget):
        def __init__(self) -> None:
            super().__init__()
            self.file_path = Path("C:/fake/selected.txt")
            self._selected = True

        def is_selected(self) -> bool:
            return True

    class PublicIconHost(QWidget):
        """Empty plate — no file_path (matches PublicIconHost shape)."""

    host = PublicIconHost()
    selected = _FakeFenceIcon()

    with patch.object(fii, "_fence_under_cursor", return_value=None):
        with patch.object(fii, "_public_under_cursor", return_value=host):
            with patch.object(fii, "selected_public_items", return_value=[]):
                with patch.object(
                    fii, "_any_selected_desktop_anchor", return_value=selected
                ):
                    with patch(
                        "src.win_shell.is_desktop_foreground", return_value=False
                    ):
                        with patch(
                            "src.win_shell.is_explorer_desktop_foreground",
                            return_value=False,
                        ):
                            # Stale / False selection_live must still resolve via
                            # owned empty plate fallthrough.
                            prev = fii._desktop_selection_live
                            fii._desktop_selection_live = False
                            try:
                                got = fii.resolve_desktop_key_anchor(chord="copy")
                                assert got is selected
                            finally:
                                fii._desktop_selection_live = prev

    # No selection anywhere → still None (toast path).
    with patch.object(fii, "_fence_under_cursor", return_value=None):
        with patch.object(fii, "_public_under_cursor", return_value=host):
            with patch.object(fii, "selected_public_items", return_value=[]):
                with patch.object(fii, "_any_selected_desktop_anchor", return_value=None):
                    with patch(
                        "src.win_shell.is_desktop_foreground", return_value=False
                    ):
                        with patch(
                            "src.win_shell.is_explorer_desktop_foreground",
                            return_value=False,
                        ):
                            prev = fii._desktop_selection_live
                            fii._desktop_selection_live = False
                            try:
                                assert (
                                    fii.resolve_desktop_key_anchor(chord="copy") is None
                                )
                            finally:
                                fii._desktop_selection_live = prev


def test_inplace_rename_widget_teardown() -> None:
    """Enter commits rename, clears editor, updates caption (offscreen Qt)."""
    import os
    import shutil
    import tempfile

    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

    from PyQt6.QtCore import Qt
    from PyQt6.QtTest import QTest
    from PyQt6.QtWidgets import QApplication, QWidget

    from src.ui.fence_icon_item import FenceIconItem, begin_inplace_rename

    app = QApplication.instance() or QApplication([])
    tmp = Path(tempfile.mkdtemp(prefix="desktidy_ren_ui_"))
    src = tmp / "before.txt"
    src.write_text("x", encoding="utf-8")
    try:
        host = QWidget()
        host.resize(120, 120)
        item = FenceIconItem(src, parent=host)
        host.show()
        item.show()
        assert begin_inplace_rename(item)
        edit = getattr(item, "_rename_edit", None)
        assert edit is not None
        assert edit.parent() is None  # top-level Tool popup
        edit.setText("after.txt")
        # Allow arming grace to elapse so editingFinished cannot cancel Enter.
        QTest.qWait(180)
        QTest.keyClick(edit, Qt.Key.Key_Return)
        for _ in range(5):
            app.processEvents()
        assert getattr(item, "_rename_edit", None) is None
        dest = tmp / "after.txt"
        assert dest.exists() and not src.exists()
        assert item.file_path == dest
        assert item.text_label is not None
        assert "after" in item.text_label.text()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main() -> int:
    print("DeskTidy fence keyboard / clipboard regression")
    run("剪贴板宿主不借用分区 HWND", test_clipboard_owner_not_overlay)
    run("CF_HDROP 剪切复制", test_clipboard_roundtrip)
    run("快捷键与就地重命名契约", test_keyboard_and_rename_contracts)
    run("LL 键盘钩子契约", test_ll_keyboard_hook_contract)
    run("dispatch 复制剪切路径", test_dispatch_copy_paths)
    run("剪切粘贴后清理公共浮标", test_scrub_removes_public_not_dest_fence)
    run("复制锚点与选中闸门契约", test_copy_resolve_and_selection_live_contracts)
    run("空白宿主板仍用分区选中复制", test_copy_empty_host_plate_uses_live_selection)
    run("重命名提交与钉选改写", test_inplace_rename_commit)
    run("就地重命名 UI 提交收尾", test_inplace_rename_widget_teardown)
    run("名称区二次点击重命名", test_label_click_rename_gates)
    run("粘贴移动改写钉选/复制目录", test_paste_move_rewrites_pins_and_copies_dirs)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
