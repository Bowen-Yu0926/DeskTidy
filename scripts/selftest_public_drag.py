"""Regression: public/page-local float dragged onto a fence must stay visible in the fence."""

from __future__ import annotations

import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

passes: list[str] = []
failures: list[tuple[str, str]] = []


def run(name: str, fn) -> None:
    try:
        fn()
        passes.append(name)
        print(f"  OK  {name}")
    except Exception as exc:
        failures.append((name, str(exc)))
        print(f"  FAIL {name}: {exc}")


def test_move_off_desktop_drops_public_float() -> None:
    """Folder/file moved from Desktop into another folder must drop the public float.

    Old bug: rewrite_public_path(old→new) then sticky-prune on *src* — dest still
    exists so the float stayed as a desktop ghost.
    """
    import tempfile
    from unittest.mock import patch

    from src.app import DeskTidyApp
    from src.public_desktop import get_public_items

    root = Path(tempfile.mkdtemp(prefix="desktidy_leave_desk_"))
    desk = root / "Desktop"
    other = root / "Documents"
    desk.mkdir()
    other.mkdir()
    folder = desk / "项目"
    folder.mkdir()
    dest = other / "项目"

    settings = {
        "enable_public_desktop": True,
        "current_page": 0,
        "public_desktop_items": [
            {"path": str(folder), "x": 10, "y": 20, "page": 0},
        ],
        "fences": [],
        "exclude_patterns": [],
    }

    class Fake:
        pass

    app = Fake()
    app.settings = settings
    app.public_icons = []
    app._parked_public_icons = {}
    app._loose_sync_needed = False
    app._watch_debounce = type("T", (), {"start": lambda self, *_a, **_k: None})()
    app._remove_public_icon_widget = lambda p: app.public_icons.clear()  # type: ignore
    # Pretend a live widget was tracking the desktop folder.
    class Icon:
        file_path = folder

    app.public_icons = [Icon()]

    with patch("src.settings.get_desktop_paths", return_value=[desk]):
        with patch("src.app.save_settings", lambda *_a, **_k: None):
            with patch("src.app.invalidate_desktop_scan_cache", lambda: None):
                DeskTidyApp._on_watched_file_moved(app, str(folder), str(dest))

    items = get_public_items(settings)
    assert items == [], items
    assert app.public_icons == []

    # Same-desktop rename must keep (rewrite) the public entry.
    folder2 = desk / "a"
    folder2.mkdir()
    renamed = desk / "b"
    settings["public_desktop_items"] = [
        {"path": str(folder2), "x": 1, "y": 2, "page": 0},
    ]
    app.settings = settings
    app.public_icons = []
    with patch("src.settings.get_desktop_paths", return_value=[desk]):
        with patch("src.app.save_settings", lambda *_a, **_k: None):
            with patch("src.app.invalidate_desktop_scan_cache", lambda: None):
                DeskTidyApp._on_watched_file_moved(app, str(folder2), str(renamed))
    items2 = get_public_items(settings)
    assert len(items2) == 1
    assert Path(str(items2[0]["path"])).name == "b"

    import shutil

    shutil.rmtree(root, ignore_errors=True)


def test_loose_sync_throttled() -> None:
    """Repeated sync_loose must not rescan every public refresh."""
    import time

    from src.public_desktop import invalidate_loose_sync_cache, sync_loose_desktop_items

    invalidate_loose_sync_cache()
    settings = {
        "exclude_patterns": [],
        "public_desktop_items": [],
        "fences": [],
    }
    sync_loose_desktop_items(settings, force=True)
    t0 = time.perf_counter()
    # Within TTL — must be a no-op regardless of desktop contents.
    assert sync_loose_desktop_items(settings) is False
    assert time.perf_counter() - t0 < 0.2


def test_drag_avoids_explorer_ole_deadlock() -> None:
    """Public drag must not use QDrag/OLE for desktop/fence (deadlock / vanish)."""
    import inspect

    from src.ui import fence_icon_item as fii
    from src.ui.fence_widget import FenceWidget

    drag = inspect.getsource(fii.start_public_item_drag)
    assert "_PublicDragFilter" in drag
    assert "QEventLoop" in drag
    assert "custom (no OLE)" in drag
    # Market rationale locked: OLE for desktop/fence deadlocks DefView overlays.
    assert "deadlocks Explorer" in drag or "deadlock" in drag.lower()
    assert "Do not" in drag and "forcing OLE" in drag
    # Desktop path stays custom; OLE only when handing off to WeChat/etc.
    assert "handoff_external" in drag
    assert "_exec_external_file_ole_drag" in drag
    assert "is_external_app_drop_point" in drag
    assert "user32.SetWindowPos" not in drag
    assert "_raise_fences_for_drop()" not in drag
    # Folder drops are handled manually (non-OLE) on mouse release.
    assert "folder_drop_target_at" in drag
    assert "_move_public_into_folder" in drag
    # Folder before fence pin (match virtual drag) — else folder icons in fences
    # only get pinned beside, never moved into.
    assert drag.index("public drag: into folder") < drag.index("public drag: sync pin fence")
    # Pet trash owns the sprite — ahead of folder/Explorer under chrome.
    assert "_try_deliver_drag_to_pet_trash" in drag
    assert drag.index("_try_deliver_drag_to_pet_trash") < drag.index(
        "public drag: into folder"
    )
    assert drag.index("_try_deliver_drag_to_pet_trash") < drag.index(
        "public drag: sync pin fence"
    )
    custom_v = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
    assert "_try_deliver_drag_to_pet_trash" in custom_v
    assert custom_v.index("_try_deliver_drag_to_pet_trash") < custom_v.index(
        "folder_drop_target_at"
    )
    assert "deliver_paths_to_pet_trash" in inspect.getsource(fii._try_deliver_drag_to_pet_trash)
    pet_bridge = inspect.getsource(fii._try_deliver_drag_to_pet_trash)
    # Recycle before removing floats — otherwise desktop file can linger hidden.
    assert pet_bridge.index("deliver_paths_to_pet_trash") < pet_bridge.index(
        "remove_public_paths"
    )
    assert "if not recycled" in pet_bridge or "if not recycled:" in pet_bridge
    folder_at = inspect.getsource(fii.folder_drop_target_at)
    assert "find_pet_trash_target" in folder_at
    assert "raise_fences_above_pet_in_band" in inspect.getsource(
        __import__("src.win_shell", fromlist=["raise_fences_above_pet_in_band"])
    )
    raise_src = inspect.getsource(fii._raise_fences_for_drop)
    assert "user32.SetWindowPos" not in raise_src
    assert "HWND_TOP" not in raise_src
    mime_src = inspect.getsource(fii._make_public_drag_mime)
    assert "setUrls(" not in mime_src
    assert "fromLocalFile" not in mime_src
    filt = inspect.getsource(FenceWidget.eventFilter)
    assert "PUBLIC_SOURCE_FENCE_ID" in filt
    drop = inspect.getsource(FenceWidget.dropEvent)
    assert "PUBLIC_SOURCE_FENCE_ID" in drop
    # Failed pin restore must not report relocated (stale moved coords).
    assert "relocated — avoid moved" in drag or "IgnoreAction, False" in drag
    filt_src = inspect.getsource(fii._PublicDragFilter)
    assert "handoff_external" in filt_src
    assert "_handoff_pending" in filt_src
    assert "_commit_external_handoff_if_pending" in filt_src
    assert "should_ole_file_handoff_at" in filt_src
    attach_src = inspect.getsource(fii.attach_external_file_drag_payload)
    assert "setUrls(" not in attach_src
    assert "attach_shell_file_drag_mime" in attach_src
    # Translucent fence geometry must block OLE (WindowFromPoint skips overlays).
    assert "fence_widget_at" in filt_src
    assert "geometry_only=True" in filt_src
    assert "_on_tick" in filt_src or "_TICK_MS" in filt_src
    # Pet sprite: same class of skip-chrome false handoff under the character.
    assert "find_pet_trash_target" in filt_src
    assert "GetAsyncKeyState" in filt_src
    assert "_own_hwnd_is_desktop_overlay" in filt_src
    assert "_release_public_host_mouse_grab" in inspect.getsource(fii.start_public_item_drag)
    ole_src = inspect.getsource(fii._exec_external_file_ole_drag)
    assert "_ole_drag_source_widget" in ole_src
    assert "_prepare_external_ole_handoff" in ole_src
    assert "deliver_files_to_external_chat" in ole_src
    assert "deliver_files_to_external_window" in ole_src
    assert "ensure_public=False" in inspect.getsource(fii.start_public_item_drag)
    prep_src = inspect.getsource(fii._prepare_external_ole_handoff)
    assert "_end_overlay_drag_session" in prep_src
    file_drag_src = inspect.getsource(fii.start_file_drag)
    assert "_ole_drag_source_widget" in file_drag_src
    assert "_begin_overlay_drag_session" in file_drag_src
    assert "timed out" in ole_src.lower()
    # Must not OLE-handoff on every foreign HWND (wakes VPN / utility panels).
    assert "is_external_app_drop_point" not in filt_src
    assert "QDrag(" not in filt_src and "drag.exec" not in filt_src
    assert "arm_nonactivating_drag_ghost" in inspect.getsource(fii._show_drag_ghost)
    ext = inspect.getsource(
        __import__("src.win_shell", fromlist=["is_external_app_drop_point"]).is_external_app_drop_point
    )
    assert "WeChat" in ext or "external" in ext.lower()
    assert "_is_desktop_or_explorer_hwnd" in ext or "_is_own_tool_window" in ext
    assert "CabinetWClass" in inspect.getsource(
        __import__("src.win_shell", fromlist=["_is_desktop_or_explorer_hwnd"])._is_desktop_or_explorer_hwnd
    )
    import src.win_shell as ws

    assert callable(ws.should_ole_file_handoff_at)
    handoff = inspect.getsource(ws.should_ole_file_handoff_at)
    assert "_OLE_HANDOFF_PROCESS_HINTS" in handoff or "wechat" in handoff.lower()
    # Docstring may mention ACCEPTFILES; must not gate on the style bit.
    assert "ex & WS_EX_ACCEPTFILES" not in handoff
    assert "WS_EX_ACCEPTFILES =" not in handoff
    assert "VPN" in handoff or "must NOT" in handoff
    assert "wechat" in ws._OLE_HANDOFF_PROCESS_HINTS
    assert "WeChatMainWndForPC" in ws._OLE_HANDOFF_CLASS_HINTS
    # Hit-test: skip DefView chrome under the cursor (pet must not hide folders).
    peek = inspect.getsource(ws._hwnd_under_drag_point)
    assert "_hwnd_at_point_skip_desktop_chrome" in peek
    assert "SetWindowLong(" not in peek
    assert "ShowWindow" not in peek
    skip_src = inspect.getsource(ws._hwnd_at_point_skip_desktop_chrome)
    assert "WindowFromPoint" in skip_src
    assert "EnumWindows" in skip_src
    assert "_own_hwnd_is_desktop_overlay" in skip_src
    folder_peek = inspect.getsource(ws.explorer_folder_path_at)
    assert "SetWindowLong(" not in folder_peek
    # Pet may see through; opaque fences must not (buried Explorer false move).
    assert "_own_hwnd_is_pet_overlay" in folder_peek
    assert "_hwnd_at_point_skip_desktop_chrome" in folder_peek
    assert "return None" in folder_peek
    assert callable(ws._own_hwnd_is_pet_overlay)
    assert "_EXPLORER_HWND_PATH_CACHE" in inspect.getsource(ws._explorer_path_from_com)
    assert "_EXPLORER_COM_PROBE_MIN_S" in inspect.getsource(ws._explorer_path_from_com)
    assert callable(ws.arm_nonactivating_drag_ghost)
    arm = inspect.getsource(ws.arm_nonactivating_drag_ghost)
    assert "WS_EX_TRANSPARENT" in arm or "0x00000020" in arm
    assert "WS_EX_NOACTIVATE" in arm or "0x08000000" in arm


def test_custom_drag_hides_source_not_translucent() -> None:
    """Only the ghost pixmap should move — no semi-transparent source echo."""
    import inspect

    from src.ui import fence_icon_item as fii

    pub = inspect.getsource(fii.start_public_item_drag)
    virt = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
    conceal = inspect.getsource(fii._conceal_widgets_for_custom_drag)
    assert "_conceal_widgets_for_custom_drag" in pub
    assert "_conceal_widgets_for_custom_drag" in virt
    assert "setOpacity(0.35)" not in pub
    assert "setOpacity(0.35)" not in virt
    assert "widget.hide()" in conceal


def test_external_release_does_not_relocate() -> None:
    """Contract: release over foreign HWND must return relocated=False."""
    import inspect

    from src.ui import fence_icon_item as fii

    src = inspect.getsource(fii.start_public_item_drag)
    # The external branch must return IgnoreAction, False (not CopyAction, True).
    assert 'return Qt.DropAction.IgnoreAction, False' in src
    assert "external release — keep float" in src
    # Relocate (CopyAction, True) only after external check.
    assert src.index("external release — keep float") < src.rindex(
        "return Qt.DropAction.CopyAction, True"
    )

def test_public_folder_drop_moves_file() -> None:
    """Dropping a public float onto a folder float must move the file in."""
    import os
    import shutil
    import tempfile

    from PyQt6.QtWidgets import QApplication

    os.environ.setdefault("QT_QPA_PLATFORM", "windows")
    app = QApplication.instance() or QApplication([])

    from src.public_desktop import add_public_item, find_public_entry
    from src.ui.fence_icon_item import (
        _move_public_into_folder,
        folder_drop_target_at,
        public_folder_at,
    )
    from src.ui.public_icon_widget import PublicIconWidget

    root = Path(tempfile.mkdtemp(prefix="desktidy_folder_drop_"))
    folder = root / "目标文件夹"
    folder.mkdir()
    src = root / "笔记.txt"
    src.write_text("hello", encoding="utf-8")

    class _Desk:
        def __init__(self) -> None:
            self.public_icons: list = []
            self.settings = {
                "public_desktop_items": [],
                "exclude_patterns": [],
                "fences": [],
            }
            self._removed: list[Path] = []

        def _remove_public_icon_widget(self, path: Path) -> None:
            self._removed.append(Path(path))

        def refresh_public_desktop(self, relayout: bool = False) -> None:
            return None

    desk = _Desk()
    add_public_item(desk.settings, src, x=10, y=10, page_id=0, auto_arrange=False)
    add_public_item(desk.settings, folder, x=200, y=200, page_id=0, auto_arrange=False)
    assert find_public_entry(desk.settings, src) is not None

    folder_icon = PublicIconWidget(folder, 200, 200)
    file_icon = PublicIconWidget(src, 10, 10)
    desk.public_icons = [folder_icon, file_icon]
    app._desktidy_app = desk  # type: ignore[attr-defined]

    folder_icon.setGeometry(200, 200, 96, 110)
    folder_icon.show()
    file_icon.setGeometry(10, 10, 96, 110)
    file_icon.show()
    app.processEvents()

    center = folder_icon.mapToGlobal(folder_icon.rect().center())
    hit = public_folder_at(center, exclude=src)
    assert hit is not None and hit.resolve() == folder.resolve()
    assert folder_drop_target_at(center, exclude=src) is not None
    assert public_folder_at(center, exclude=folder) is None

    ok = _move_public_into_folder(desk, desk.settings, file_icon, src, folder)
    assert ok == "moved"
    assert not src.exists()
    assert (folder / "笔记.txt").exists()
    assert find_public_entry(desk.settings, src) is None
    assert desk._removed and desk._removed[0].name == "笔记.txt"

    file_icon.close()
    folder_icon.close()
    shutil.rmtree(root, ignore_errors=True)


def test_virtual_drag_moves_into_folder() -> None:
    """Fence virtual drag onto a folder must move the file (not unpin to public)."""
    import inspect
    import os
    import shutil
    import tempfile

    from PyQt6.QtCore import QPoint
    from PyQt6.QtWidgets import QApplication, QWidget

    os.environ.setdefault("QT_QPA_PLATFORM", "windows")
    app = QApplication.instance() or QApplication([])

    from src.ui import fence_icon_item as fii

    src_fn = inspect.getsource(fii.start_virtual_item_drag)
    custom_fn = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
    assert "_run_custom_desktop_resident_virtual_drag" in src_fn
    assert "QDrag(" not in src_fn and "drag.exec" not in src_fn
    assert "folder_drop_target_at" in custom_fn
    assert "_move_virtual_into_folder" in custom_fn
    assert "quiet_end" in custom_fn
    assert "reconcile=not quiet_end" in custom_fn
    assert "_desktidy_drag_quiet" in custom_fn
    move_src = inspect.getsource(fii.FenceIconItem.mouseMoveEvent)
    assert "_desktidy_drag_quiet" in move_src
    # Same-fence reorder must quiet-end (no FG heal / public refresh flash).
    assert "drop_id == source_id" in custom_fn
    assert "item.hide()" in custom_fn
    assert "fence_folder_at" in inspect.getsource(fii.folder_drop_target_at)

    root = Path(tempfile.mkdtemp(prefix="desktidy_virtual_folder_"))
    folder = root / "ppt"
    folder.mkdir()
    src = root / "产装大屏指标.xlsx"
    src.write_text("xlsx", encoding="utf-8")

    class _Desk:
        def __init__(self) -> None:
            self.settings = {
                "fences": [
                    {
                        "id": "f1",
                        "name": "文档",
                        "virtual_items": [str(src)],
                    }
                ],
                "public_desktop_items": [],
                "exclude_patterns": [],
            }
            self.fences: list = []

    desk = _Desk()
    app._desktidy_app = desk  # type: ignore[attr-defined]

    ok = fii._move_virtual_into_folder(src, folder, "f1")
    assert ok == "moved"
    assert not src.exists()
    assert (folder / "产装大屏指标.xlsx").exists()
    pins = desk.settings["fences"][0]["virtual_items"]
    assert not any(str(src).casefold() == str(p).casefold() for p in pins)

    # Directory drops must not invent opencode_1 / block the UI with shutil.
    move_src = inspect.getsource(fii._move_virtual_into_folder)
    assert "_start_background_fs_transfer" in move_src
    assert "unique_dest_path" not in move_src
    assert "_run_transfer_into_folder" in move_src
    assert "suppress_desktop_item" in move_src
    assert "refresh_desktop" not in move_src
    fin_src = inspect.getsource(fii._finalize_virtual_folder_move)
    assert "_remove_virtual_icon_widget" in fin_src
    assert "refresh_desktop" not in fin_src
    assert "fw.refresh()" not in fin_src
    from src.ui.fence_icon_item import FenceIconItem

    assert "CopyAction, Qt.DropAction.MoveAction" in inspect.getsource(
        FenceIconItem.mouseMoveEvent
    ) or "MoveAction, Qt.DropAction.CopyAction" in inspect.getsource(
        FenceIconItem.mouseMoveEvent
    )
    assert "move_path_into_folder" in inspect.getsource(fii._run_move_into_folder)
    drag_src = inspect.getsource(fii.start_virtual_item_drag)
    custom_src = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
    assert "QDrag(" not in drag_src and "drag.exec" not in drag_src
    assert "DropAction.MoveAction" in custom_src
    assert "folder_drop_target_at" in custom_src
    from src.win_shell import move_path_into_folder

    shell_src = inspect.getsource(move_path_into_folder)
    assert "FO_MOVE" in shell_src
    assert "FOF_RENAMEONCOLLISION" not in shell_src
    assert "FOF_NOCONFIRMATION" in shell_src
    assert "FileExistsError" in shell_src
    assert "os.rename" in shell_src
    assert "notify_shell_item_moved" in shell_src
    bg_src = inspect.getsource(fii._start_background_fs_move)
    assert "threading.Thread" in bg_src or "_start_background_fs_transfer" in bg_src
    assert "call_on_main_thread" in inspect.getsource(fii._start_background_fs_transfer)
    assert "ensure_main_thread_bridge" in inspect.getsource(
        fii._start_background_fs_transfer
    )
    transfer_src = inspect.getsource(fii._start_background_fs_transfer)
    body = transfer_src.split('"""', 2)[-1] if '"""' in transfer_src else transfer_src
    assert "QTimer.singleShot" not in body
    assert 'show_toast("正在移动"' not in transfer_src
    assert 'show_toast(\'正在移动\'' not in transfer_src
    paste_bg = inspect.getsource(fii._start_background_fs_paste)
    paste_body = paste_bg.split('"""', 2)[-1] if '"""' in paste_bg else paste_bg
    assert "call_on_main_thread" in paste_bg
    assert "ensure_main_thread_bridge" in paste_bg
    assert "QTimer.singleShot" not in paste_body
    pub_move = inspect.getsource(fii._move_public_into_folder)
    assert "_snapshot_public_float_entry" in pub_move
    assert "restore_snap" in pub_move
    assert "_finalize_public_folder_move" in pub_move
    toast_src = inspect.getsource(
        __import__("src.ui.toast", fromlist=["show_toast"])
    )
    assert "dismiss_toast" in toast_src
    assert "_active_toast" in toast_src
    assert "_fs_move_needs_background" in move_src
    needs_bg = inspect.getsource(fii._fs_move_needs_background)
    assert "FileExistsError" in needs_bg or "dest.exists()" in needs_bg
    paste_src = inspect.getsource(fii.paste_files_into_fence)
    assert "_start_background_fs_paste" in paste_src
    assert "_paste_sources_need_background" in paste_src

    big = root / "huge_project"
    big.mkdir()
    (big / "a.txt").write_text("a", encoding="utf-8")
    called: list[tuple] = []

    def _fake_bg(src_p, target_p, *, move, on_success, on_failure):
        called.append((Path(src_p), Path(target_p), bool(move)))
        # Do not run the real move / callbacks — only prove we offloaded.

    orig = fii._start_background_fs_transfer
    fii._start_background_fs_transfer = _fake_bg  # type: ignore[assignment]
    try:
        ok_dir = fii._move_virtual_into_folder(big, folder, "f1")
        assert ok_dir == "moved"
        assert called and called[0][0] == big
        assert called[0][1] == folder
        assert called[0][2] is True
        assert big.exists(), "source must remain until background move finishes"
    finally:
        fii._start_background_fs_transfer = orig  # type: ignore[assignment]

    # Same-volume rename keeps the original name (no _1).
    same_root = Path(tempfile.mkdtemp(prefix="desktidy_samevol_"))
    try:
        src_dir = same_root / "项目"
        src_dir.mkdir()
        (src_dir / "x.txt").write_text("x", encoding="utf-8")
        dest_parent = same_root / "目标"
        dest_parent.mkdir()
        out = move_path_into_folder(src_dir, dest_parent)
        assert out == dest_parent / "项目"
        assert out.is_dir()
        assert not src_dir.exists()
        assert not (dest_parent / "项目_1").exists()

        # Collision must raise FileExistsError — never Shell rename dialog.
        again = same_root / "项目"
        again.mkdir()
        (again / "y.txt").write_text("y", encoding="utf-8")
        try:
            move_path_into_folder(again, dest_parent)
            raise AssertionError("expected FileExistsError on name collision")
        except FileExistsError:
            pass
        assert again.exists(), "source must remain when dest name is taken"
    finally:
        shutil.rmtree(same_root, ignore_errors=True)

    shutil.rmtree(root, ignore_errors=True)


def test_fence_folder_hit_beats_explorer_underneath() -> None:
    """Dropping on a fence folder icon must target that folder, not Explorer behind."""
    import os
    import shutil
    import tempfile

    from PyQt6.QtCore import Qt
    from PyQt6.QtWidgets import QApplication, QGridLayout, QWidget

    os.environ.setdefault("QT_QPA_PLATFORM", "windows")
    app = QApplication.instance() or QApplication([])

    from src.ui.fence_icon_item import (
        FenceIconItem,
        fence_folder_at,
        folder_drop_target_at,
    )
    from src.win_shell import explorer_folder_path_at

    root = Path(tempfile.mkdtemp(prefix="desktidy_fence_folder_hit_"))
    folder = root / "myProjiect"
    folder.mkdir()
    doc = root / "协议.docx"
    doc.write_text("doc", encoding="utf-8")

    class FenceWidget(QWidget):
        pass

    shell = FenceWidget()
    shell.setWindowFlags(Qt.WindowType.Tool | Qt.WindowType.FramelessWindowHint)
    layout = QGridLayout(shell)
    shell.items_layout = layout  # type: ignore[attr-defined]
    icon = FenceIconItem(folder, shell, virtual_mode=True, fence_id="f1")
    layout.addWidget(icon, 0, 0)
    shell.setGeometry(80, 80, 220, 180)
    shell.show()
    shell.raise_()
    shell.activateWindow()
    for _ in range(8):
        app.processEvents()
        if shell.isVisible() and icon.isVisible():
            break

    center = icon.mapToGlobal(icon.rect().center())
    hit = fence_folder_at(center, exclude=doc)
    assert hit is not None and hit.resolve() == folder.resolve(), hit
    # Prefer fence folder even if Explorer would otherwise match the screen point.
    assert folder_drop_target_at(center, exclude=doc).resolve() == folder.resolve()
    # Own fence HWND must not resolve as Explorer; if another window covers the
    # test widget under suite load, folder_drop_target_at still winning is enough.
    exp = explorer_folder_path_at(center.x(), center.y())
    if exp is not None:
        assert Path(exp).resolve() != folder.resolve(), exp

    shell.close()
    shutil.rmtree(root, ignore_errors=True)


def test_document_moves_into_folder_icon_stays_virtual() -> None:
    """Documents real-move into remapped folders; .lnk icons skip folder swallow."""
    import os
    import shutil
    import tempfile
    from unittest import mock

    from PyQt6.QtWidgets import QApplication

    os.environ.setdefault("QT_QPA_PLATFORM", "windows")
    app = QApplication.instance() or QApplication([])

    from src.fence_rules import path_organize_kind
    from src.ui import fence_icon_item as fii

    assert path_organize_kind(Path("a.docx")) == "file"
    assert path_organize_kind(Path("b.lnk")) == "icon"
    assert path_organize_kind(Path("deskNote.exe")) == "icon"

    root = Path(tempfile.mkdtemp(prefix="desktidy_kind_folder_"))
    stand_in = root / "desktop_myProjiect"
    stand_in.mkdir()
    real = root / "E_myProjiect"
    real.mkdir()
    doc = root / "协议.docx"
    doc.write_text("doc", encoding="utf-8")
    icon = root / "应用.lnk"
    icon.write_text("x", encoding="utf-8")

    class _Desk:
        def __init__(self) -> None:
            self.settings = {
                "fences": [
                    {
                        "id": "f1",
                        "name": "文档",
                        "virtual_items": [str(doc), str(icon)],
                    }
                ],
                "public_desktop_items": [],
                "exclude_patterns": [],
            }
            self.fences: list = []

    desk_app = _Desk()
    app._desktidy_app = desk_app  # type: ignore[attr-defined]

    with mock.patch(
        "src.win_shell.resolve_folder_drop_target", return_value=real
    ):
        assert fii._move_virtual_into_folder(icon, stand_in, "f1") is None
        assert icon.exists()
        assert fii._move_virtual_into_folder(doc, stand_in, "f1") == "moved"
        assert not doc.exists()
        assert (real / "协议.docx").exists()
        assert not (stand_in / "协议.docx").exists()

    shutil.rmtree(root, ignore_errors=True)


def test_empty_namespace_lnk_not_cwd() -> None:
    """Namespace .lnk with empty Targetpath must not resolve to process cwd."""
    import shutil
    import tempfile
    from unittest import mock

    from src.win_shell import _lnk_target_dir, resolve_folder_drop_target

    root = Path(tempfile.mkdtemp(prefix="desktidy_ns_lnk_"))
    lnk = root / "回收站.lnk"
    lnk.write_bytes(b"x")

    class _SC:
        Targetpath = ""

    class _Shell:
        def CreateShortCut(self, _path: str):
            return _SC()

    with mock.patch("win32com.client.Dispatch", return_value=_Shell()):
        assert _lnk_target_dir(lnk) is None
        got = resolve_folder_drop_target(lnk)
        assert got.resolve() != Path.cwd().resolve()

    shutil.rmtree(root, ignore_errors=True)


def test_cross_drive_desktop_twin_remap() -> None:
    """Open/pin remaps D:\\desktop\\name → E:\\…; drag-move stays on the stand-in."""
    from src.win_shell import resolve_folder_drop_target

    desk = Path(r"D:\desktop\AIproject")
    real = Path(r"E:\soft\AIproject")
    if desk.is_dir() and real.is_dir():
        assert resolve_folder_drop_target(desk).resolve() == real.resolve()
        # Drag-into-folder must not cross-drive remap (slow FO_MOVE + rename UI).
        assert (
            resolve_folder_drop_target(desk, for_move=True).resolve()
            == desk.resolve()
        )
    oc = Path(r"D:\desktop\opencode")
    if oc.is_dir() and Path(r"D:\OpenCode").is_dir():
        # Same drive — do not remap.
        assert resolve_folder_drop_target(oc).resolve() == oc.resolve()
        assert resolve_folder_drop_target(oc, for_move=True).resolve() == oc.resolve()

    # Contract: move helpers pass for_move=True.
    import inspect

    from src.ui import fence_icon_item as fii

    assert "for_move=True" in inspect.getsource(fii._move_public_into_folder)
    assert "for_move=True" in inspect.getsource(fii._move_virtual_into_folder)
    assert "for_move=True" in inspect.getsource(fii._resolve_drop_folder)


def test_basename_remove_only_when_unique() -> None:
    """remove_public_paths matches exact keys only (slash/case normalized)."""
    from src.public_desktop import get_public_items, remove_public_paths

    settings = {
        "public_desktop_items": [
            {"path": r"D:\desktop\a\report.txt", "x": 1, "y": 1, "page": 0},
            {"path": r"D:\desktop\b\report.txt", "x": 2, "y": 2, "page": 0},
        ]
    }
    # Exact path removes only one — same basename sibling stays.
    remove_public_paths(settings, [r"D:\desktop\a\report.txt"])
    left = [str(e["path"]) for e in get_public_items(settings)]
    assert left == [r"D:\desktop\b\report.txt"], left
    # Slash/case variants of the same path still match via _norm_key.
    settings["public_desktop_items"] = [
        {"path": r"D:/desktop/b/report.txt", "x": 2, "y": 2, "page": 0},
    ]
    remove_public_paths(settings, [r"D:\desktop\b\report.txt"])
    assert not get_public_items(settings)
    # Unique basename alone must NOT wipe an unrelated spelling of another file.
    settings["public_desktop_items"] = [
        {"path": r"D:\desktop\c\report.txt", "x": 3, "y": 3, "page": 0},
    ]
    remove_public_paths(settings, [r"E:\other\report.txt"])
    assert len(get_public_items(settings)) == 1


def test_drop_apply_is_deferred() -> None:
    """dropEvent must not assign inside QDrag.exec (OLE deadlock / vanish)."""
    import inspect

    from src.ui import fence_icon_item as fii
    from src.ui.fence_widget import FenceWidget

    src = inspect.getsource(FenceWidget._import_virtual_drop)
    assert "singleShot" in src
    assert "_apply_virtual_drop_paths" in src
    assert "assign_paths_to_virtual_fence" not in src
    apply_src = inspect.getsource(FenceWidget._apply_virtual_drop_paths)
    assert "assign_paths_to_virtual_fence" in apply_src
    force_src = inspect.getsource(fii.force_pin_path_to_fence)
    assert "_apply_virtual_drop_paths" in force_src
    drag_src = inspect.getsource(fii.start_public_item_drag)
    assert "pin_public_path_to_fence" in drag_src
    assert "geometry_only=True" in drag_src
    assert "sync pin" in drag_src
    assert "QDrag(" not in drag_src
    assert "_PublicDragFilter" in drag_src
    mime_src = inspect.getsource(fii._make_public_drag_mime)
    assert "setUrls(" not in mime_src
    assert "fromLocalFile" not in mime_src
    assert "DESKTIDY_VIRTUAL_MIME" in mime_src
    raise_src = inspect.getsource(fii._raise_fences_for_drop)
    assert "user32.SetWindowPos" not in raise_src
    assert "_raise_fences_for_drop()" not in drag_src


def test_pin_seed_keeps_existing_pins() -> None:
    """Converting sort_by→manual must not drop existing virtual_items."""
    from PyQt6.QtWidgets import QApplication

    from src.app import DeskTidyApp
    from src.public_desktop import add_public_item, get_public_items
    from src.ui.fence_widget import FenceWidget

    app = QApplication.instance() or QApplication([])
    td = Path(tempfile.mkdtemp())
    existing = [td / f"keep_{i}.txt" for i in range(5)]
    for p in existing:
        p.write_text("x", encoding="utf-8")
    newbie = td / "new_pin.txt"
    newbie.write_text("n", encoding="utf-8")
    settings = {
        "enable_public_desktop": False,
        "current_page": 0,
        "public_desktop_items": [],
        "fences": [
            {
                "id": "f1",
                "name": "F",
                "virtual_items": [str(p) for p in existing],
                "sort_by": "name",
                "pages": [0],
                "style": {},
            }
        ],
        "theme": "mist",
    }
    live = {
        "id": "f1",
        "name": "F",
        # Simulate a live config that has drifted / not yet painted all icons.
        "virtual_items": [str(existing[0])],
        "sort_by": "name",
        "pages": [0],
        "style": {},
    }
    add_public_item(
        settings, newbie, x=10, y=10, page_id=0, auto_arrange=False, shared=False
    )

    class Fake:
        pass

    fake = Fake()
    fake.settings = settings
    fake.fences = []
    fake.public_icons = []
    fake._exiting = False
    fake._last_public_sync_sig = None
    fake._public_drag_settling = False
    fake._parked_public_icons = {}
    fake._max_parked_public_icons = 80
    fake._retire_overlay_widget = DeskTidyApp._retire_overlay_widget.__get__(fake)
    fake._park_public_icon = DeskTidyApp._park_public_icon.__get__(fake)
    fake._remove_public_icon_widget = DeskTidyApp._remove_public_icon_widget.__get__(fake)
    fake.refresh_public_desktop = lambda **kw: None
    fake._current_page = lambda: 0

    fence = FenceWidget(live, settings)
    fence.setGeometry(40, 40, 280, 320)
    fence.show()
    app.processEvents()
    fake.fences = [fence]

    assert DeskTidyApp.pin_public_path_to_fence(fake, newbie, fence)
    pins = [Path(str(x)).name for x in settings["fences"][0]["virtual_items"]]
    for p in existing:
        assert p.name in pins, pins
    assert newbie.name in pins
    assert not get_public_items(settings)

    fence.close()
    fence.deleteLater()
    app.processEvents()
    for p in existing + [newbie]:
        p.unlink(missing_ok=True)


def test_pin_public_path_atomic() -> None:
    """App helper must leave the icon in the fence (not neither list)."""
    from PyQt6.QtWidgets import QApplication

    from src.app import DeskTidyApp
    from src.fence_rules import get_virtual_items_for_fence
    from src.public_desktop import add_public_item, get_public_items
    from src.ui.fence_widget import FenceWidget

    app = QApplication.instance() or QApplication([])
    td = Path(tempfile.mkdtemp())
    p = td / "atomic_pin.txt"
    p.write_text("a", encoding="utf-8")
    settings = {
        "enable_public_desktop": False,
        "current_page": 0,
        "public_desktop_items": [],
        "fences": [
            {
                "id": "f1",
                "name": "F",
                "virtual_items": [],
                "sort_by": "name",
                "pages": [0],
                "style": {},
            }
        ],
        "theme": "mist",
    }
    live = {
        "id": "f1",
        "name": "F",
        "virtual_items": [],
        "sort_by": "name",
        "pages": [0],
        "style": {},
    }
    add_public_item(settings, p, x=10, y=10, page_id=0, auto_arrange=False, shared=False)

    class Fake:
        pass

    fake = Fake()
    fake.settings = settings
    fake.fences = []
    fake.public_icons = []
    fake._exiting = False
    fake._last_public_sync_sig = None
    fake._public_drag_settling = False
    fake._parked_public_icons = {}
    fake._max_parked_public_icons = 80
    fake._retire_overlay_widget = DeskTidyApp._retire_overlay_widget.__get__(fake)
    fake._park_public_icon = DeskTidyApp._park_public_icon.__get__(fake)
    fake._remove_public_icon_widget = DeskTidyApp._remove_public_icon_widget.__get__(fake)
    fake.refresh_public_desktop = lambda **kw: None
    fake._current_page = lambda: 0

    fence = FenceWidget(live, settings)
    fence.setGeometry(40, 40, 280, 320)
    fence.show()
    app.processEvents()
    fake.fences = [fence]

    assert DeskTidyApp.pin_public_path_to_fence(fake, p, fence)
    assert not get_public_items(settings), get_public_items(settings)
    shown = get_virtual_items_for_fence(settings["fences"][0], settings)
    assert any(x.name == p.name for x in shown), shown
    assert fence.has_icon_widgets(), "fence grid must show the pinned icon"

    fence.close()
    fence.deleteLater()
    app.processEvents()
    p.unlink(missing_ok=True)


def test_assign_writes_pin_before_clearing_public() -> None:
    import inspect

    from src.fence_rules import assign_paths_to_virtual_fence

    src = inspect.getsource(assign_paths_to_virtual_fence)
    pin_at = src.index('target["virtual_items"] = pinned')
    clear_at = src.index("remove_public_paths(settings, added)")
    assert pin_at < clear_at


def test_import_virtual_drop_pins() -> None:
    from PyQt6.QtCore import QByteArray, QMimeData
    from PyQt6.QtWidgets import QApplication

    from src.fence_rules import get_virtual_items_for_fence
    from src.public_desktop import add_public_item, get_public_items
    from src.ui.fence_icon_item import (
        DESKTIDY_SOURCE_FENCE_MIME,
        DESKTIDY_VIRTUAL_MIME,
        PUBLIC_SOURCE_FENCE_ID,
        force_pin_path_to_fence,
        path_pinned_in_settings,
    )
    from src.ui.fence_widget import FenceWidget
    from src.win_shell import DESKTIDY_SOURCE_FENCE_MIME as _SF
    from src.win_shell import DESKTIDY_VIRTUAL_MIME as _VM

    assert DESKTIDY_VIRTUAL_MIME == _VM
    assert DESKTIDY_SOURCE_FENCE_MIME == _SF

    app = QApplication.instance() or QApplication([])

    td = Path(tempfile.mkdtemp())
    p = td / "drag_me.txt"
    p.write_text("hello", encoding="utf-8")
    p2 = td / "drag_me2.txt"
    p2.write_text("hello2", encoding="utf-8")

    settings = {
        "enable_public_desktop": False,
        "current_page": 0,
        "public_desktop_items": [],
        "fences": [
            {
                "id": "f1",
                "name": "测试分区",
                "extensions": [],
                "virtual_items": [],
                "sort_by": "name",
                "pages": [0],
                "style": {},
            }
        ],
        "theme": "default",
    }
    # Live widget config is a *different* dict (the real-world drift case).
    live_cfg = {
        "id": "f1",
        "name": "测试分区",
        "extensions": [],
        "virtual_items": [],
        "sort_by": "name",
        "pages": [0],
        "style": {},
    }
    add_public_item(settings, p, x=10, y=10, page_id=0, auto_arrange=False, shared=False)
    assert get_public_items(settings)

    fence = FenceWidget(live_cfg, settings)
    fence.setGeometry(100, 100, 280, 360)
    fence.show()
    app.processEvents()

    mime = QMimeData()
    mime.setData(DESKTIDY_VIRTUAL_MIME, QByteArray(str(p).encode("utf-8")))
    mime.setData(
        DESKTIDY_SOURCE_FENCE_MIME,
        QByteArray(PUBLIC_SOURCE_FENCE_ID.encode("utf-8")),
    )

    # Simulate Qt drop miss → force pin (the production fallback).
    assert force_pin_path_to_fence(fence, p)
    app.processEvents()
    # Flush deferred finish UI.
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 0.5:
        app.processEvents()
        time.sleep(0.02)

    assert path_pinned_in_settings(settings, p), settings["fences"][0]
    assert not get_public_items(settings), get_public_items(settings)
    shown = get_virtual_items_for_fence(settings["fences"][0], settings)
    assert any(x.name == p.name for x in shown), shown
    # Live config must also see the pin (rebound or copied).
    shown_live = get_virtual_items_for_fence(fence.config, settings)
    assert any(x.name == p.name for x in shown_live), (fence.config, shown_live)

    # Deferred import path (what dropEvent uses): not pinned until next tick.
    add_public_item(settings, p2, x=20, y=20, page_id=0, auto_arrange=False, shared=False)
    mime2 = QMimeData()
    mime2.setData(DESKTIDY_VIRTUAL_MIME, QByteArray(str(p2).encode("utf-8")))
    mime2.setData(
        DESKTIDY_SOURCE_FENCE_MIME,
        QByteArray(PUBLIC_SOURCE_FENCE_ID.encode("utf-8")),
    )
    assert fence._import_virtual_drop(mime2)
    assert not path_pinned_in_settings(settings, p2)
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < 0.5:
        app.processEvents()
        time.sleep(0.02)
        if path_pinned_in_settings(settings, p2):
            break
    assert path_pinned_in_settings(settings, p2), settings["fences"][0]
    assert not any(
        Path(str(e.get("path", ""))).name == p2.name for e in get_public_items(settings)
    )

    fence.close()
    fence.deleteLater()
    app.processEvents()
    p.unlink(missing_ok=True)
    p2.unlink(missing_ok=True)


def test_split_assign_writes_settings() -> None:
    import tempfile
    from pathlib import Path

    from src.fence_rules import assign_paths_to_virtual_fence, get_virtual_items_for_fence
    from src.public_desktop import add_public_item, get_public_items

    td = Path(tempfile.mkdtemp())
    p = td / "x.txt"
    p.write_text("x", encoding="utf-8")
    settings = {
        "enable_public_desktop": False,
        "public_desktop_items": [],
        "fences": [{"id": "a", "name": "A", "virtual_items": [], "sort_by": "name"}],
    }
    widget_cfg = {"id": "a", "name": "A", "virtual_items": [], "sort_by": "name"}
    add_public_item(settings, p, x=1, y=1, page_id=0, auto_arrange=False, shared=False)
    assign_paths_to_virtual_fence(widget_cfg, settings, [p])
    assert settings["fences"][0]["virtual_items"]
    assert not get_public_items(settings)
    assert get_virtual_items_for_fence(settings["fences"][0], settings)
    p.unlink(missing_ok=True)


def test_drag_out_must_not_park_on_own_fg() -> None:
    """Dragging a pin to the desktop must not hide fences via foreign-FG park."""
    import inspect

    from src.app import DeskTidyApp
    from src.desktop_foreground_monitor import (
        _on_desktop_surface,
        _hwnd_is_own_desktop_chrome,
    )
    from src.ui import fence_icon_item as fii

    pred = inspect.getsource(DeskTidyApp._should_park_overlays_for_foreign_fg)
    assert "return False" in pred
    foreign = inspect.getsource(DeskTidyApp._foreign_app_owns_foreground)
    assert "is_desktop_foreground" in foreign
    assert "_desk_app_ui_open" in foreign
    sync = inspect.getsource(DeskTidyApp._sync_overlays_to_foreground)
    assert "is_explorer_desktop_foreground" in sync
    assert "if not explorer_fg:" in sync
    assert "_remap_hidden_overlay_hwnds" in sync
    assert "_sink_overlays_for_foreign_app" in sync
    assert "widget.hide()" not in sync
    # FG monitor: own chrome stays on desktop surface (Explorer↔fence silent).
    mon = inspect.getsource(
        __import__(
            "src.desktop_foreground_monitor", fromlist=["DesktopForegroundMonitor"]
        ).DesktopForegroundMonitor._on_win_event
    )
    assert "_on_desktop_surface" in mon
    assert callable(_on_desktop_surface) and callable(_hwnd_is_own_desktop_chrome)
    # Single public float must not bypass the may-show gate while parked.
    single = inspect.getsource(DeskTidyApp._ensure_single_public_float)
    assert "_overlay_widgets_may_show" in single
    take = inspect.getsource(DeskTidyApp._take_parked_public_icon)
    assert "icon.show()" not in take
    refresh = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "may_show = bool(self._overlay_widgets_may_show())" in refresh
    assert "if may_show:" in refresh
    for name in (
        "_ensure_new_public_floats_visible",
        "_ensure_dissolve_floats_visible",
        "_restore_public_icon_widget",
    ):
        src = inspect.getsource(getattr(DeskTidyApp, name))
        assert "_overlay_widgets_may_show" in src, name
    end = inspect.getsource(DeskTidyApp._end_overlay_drag)
    assert "_sync_overlays_to_foreground" in end
    assert "_heal_overlays_after_shell_menu" in end
    assert "_deferred_fg_sync" in end
    assert "singleShot(180" not in end
    custom = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
    assert "_end_overlay_drag_session" in custom

def test_click_jitter_must_not_unpin() -> None:
    """IgnoreAction + tiny cursor jitter must not unpin (click-vanish bug)."""
    from PyQt6.QtCore import QPoint, Qt

    from src.ui.fence_icon_item import should_treat_as_virtual_unpin

    start = QPoint(200, 200)
    # Plain click jitter — previously treated as unpin when Qt returned IgnoreAction.
    assert not should_treat_as_virtual_unpin(
        catcher_accepted=False,
        drop_action=Qt.DropAction.IgnoreAction,
        start_global=start,
        end_global=QPoint(210, 208),
    )
    # Explicit catcher accept still unpins.
    assert should_treat_as_virtual_unpin(
        catcher_accepted=True,
        drop_action=Qt.DropAction.CopyAction,
        start_global=start,
        end_global=QPoint(800, 600),
    )


def test_prefercopy_desktop_steal_becomes_unpin() -> None:
    """Blank-desktop OLE PreferCopy must unpin + drop the fresh FS duplicate."""
    import inspect
    import tempfile
    import time
    from unittest.mock import patch

    from PyQt6.QtCore import QPoint, Qt

    from src.ui import fence_icon_item as fii

    drag_src = inspect.getsource(fii.start_virtual_item_drag)
    assert "_virtual_drag_all_desktop_resident" in drag_src
    assert "_run_custom_desktop_resident_virtual_drag" in drag_src
    # Full-gesture QDrag.exec retired — PreferCopy steal no longer applies mid-drag.
    assert "QDrag(" not in drag_src and "drag.exec" not in drag_src
    custom_src = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
    assert "_PublicDragFilter" in custom_src
    assert "should_unpin_virtual_drop" in custom_src
    # Allowlisted mid-drag OLE handoff for WeChat/QQ — not full-gesture OLE.
    assert "_exec_external_file_ole_drag" in custom_src
    assert "handoff_external" in custom_src
    assert "QDrag(" not in custom_src and "drag.exec" not in custom_src
    # PreferCopy helpers remain for regression / legacy recovery paths.
    assert callable(fii.should_convert_prefercopy_to_unpin)
    assert callable(fii.discard_explorer_prefercopy_desktop_dupes)
    assert callable(fii.external_payload_paths_for_virtual_drag)
    assert callable(fii.attach_external_file_drag_payload)

    td = Path(tempfile.mkdtemp(prefix="desktidy_deskpin_"))
    desk = td / "Desktop"
    desk.mkdir()
    on_desk = desk / "book.xlsx"
    on_desk.write_text("x", encoding="utf-8")
    off = td / "Docs" / "other.xlsx"
    off.parent.mkdir()
    off.write_text("y", encoding="utf-8")
    missing = desk / "gone.xlsx"
    try:
        with patch("src.settings.get_desktop_paths", return_value=[desk]):
            assert fii.path_is_under_desktop(on_desk)
            assert not fii.path_is_under_desktop(off)
            # Both on-desktop and off-desktop pins are exported for OLE apps.
            got = fii.external_payload_paths_for_virtual_drag([on_desk, off, missing])
            assert got == [on_desk, off], got
            assert fii.external_payload_paths_for_virtual_drag([on_desk]) == [on_desk]
    finally:
        import shutil

        shutil.rmtree(td, ignore_errors=True)

    # Outside fences + Copy → convert (folder / external excluded).
    with (
        patch.object(fii, "should_unpin_virtual_drop", return_value=True),
        patch.object(fii, "folder_drop_target_at", return_value=None),
        patch(
            "src.win_shell.is_external_app_drop_point",
            return_value=False,
        ),
    ):
        assert fii.should_convert_prefercopy_to_unpin(
            drop_action=Qt.DropAction.CopyAction,
            end_global=QPoint(900, 500),
            primary=None,
        )
    with (
        patch.object(fii, "should_unpin_virtual_drop", return_value=True),
        patch.object(fii, "folder_drop_target_at", return_value=None),
        patch(
            "src.win_shell.is_external_app_drop_point",
            return_value=True,
        ),
    ):
        assert not fii.should_convert_prefercopy_to_unpin(
            drop_action=Qt.DropAction.CopyAction,
            end_global=QPoint(900, 500),
            primary=None,
        )
    with patch.object(fii, "should_unpin_virtual_drop", return_value=True):
        assert not fii.should_convert_prefercopy_to_unpin(
            drop_action=Qt.DropAction.IgnoreAction,
            end_global=QPoint(900, 500),
            primary=None,
        )

    td = Path(tempfile.mkdtemp(prefix="desktidy_prefercopy_"))
    desk = td / "Desktop"
    desk.mkdir()
    src = td / "notes.txt"
    src.write_text("src", encoding="utf-8")
    dupe = desk / "notes.txt"
    dupe.write_text("copy", encoding="utf-8")
    stale = desk / "notes - 副本.txt"
    stale.write_text("old", encoding="utf-8")
    older = time.time() - 120
    import os

    os.utime(stale, (older, older))
    t0 = time.time()
    try:
        with patch("src.settings.get_desktop_paths", return_value=[desk]):
            removed = fii.discard_explorer_prefercopy_desktop_dupes(
                [src], not_before=t0
            )
        assert not dupe.exists(), "fresh PreferCopy dupe must be removed"
        assert src.exists(), "original must stay"
        assert stale.exists(), "pre-existing 副本 must stay"
        assert any(p.name == "notes.txt" for p in removed)
    finally:
        import shutil

        shutil.rmtree(td, ignore_errors=True)


def test_canonicalize_skips_office_docs() -> None:
    """Office docs must not trigger a desktop-wide COM shortcut scan.

    Remapping ``BaiduNetdisk.exe`` → ``百度网盘.lnk`` is correct; doing the same
    scan for every xlsx/pptx pin froze fence→public drag-out.
    """
    from pathlib import Path as P

    from src import fence_rules as fr

    calls: list[str] = []

    def spy(target: P) -> P | None:
        calls.append(str(target))
        return P(r"D:\desktop\cover.lnk")

    orig = fr._desktop_shortcut_covering_target
    fr._desktop_shortcut_covering_target = spy  # type: ignore[assignment]
    try:
        xlsx = P(r"D:\desktop\report.xlsx")
        pptx = P(r"C:\Users\x\Doc\deck.pptx")
        assert fr._canonicalize_virtual_pin_path(xlsx) == xlsx
        assert fr._canonicalize_virtual_pin_path(pptx) == pptx
        assert calls == []

        exe = P(r"C:\Program Files\Foo\Foo.exe")
        remapped = fr._canonicalize_virtual_pin_path(exe)
        assert remapped == P(r"D:\desktop\cover.lnk")
        assert calls == [str(exe)]

        lnk = P(r"D:\desktop\app.lnk")
        # .lnk path stays on the prefer-folder path; covering spy must not run.
        out = fr._canonicalize_virtual_pin_path(lnk)
        assert out == lnk or isinstance(out, P)
        assert calls == [str(exe)]
    finally:
        fr._desktop_shortcut_covering_target = orig


def test_fence_display_skips_canonicalize() -> None:
    """Fence refresh must not COM-canonicalize every pin (docs + shortcuts)."""
    import inspect
    import tempfile
    from pathlib import Path as P
    from unittest.mock import patch

    from src import fence_rules as fr
    from src.fence_rules import get_virtual_items_for_fence

    get_src = inspect.getsource(get_virtual_items_for_fence)
    assert "_heal_fence_virtual_pins" in get_src
    # Hot loop uses Path(str(raw)) — not canonicalize per pin.
    display_loop = get_src.split("for raw in raw_items:")[-1]
    assert "_canonicalize_virtual_pin_path" not in display_loop
    assert "path_present" in get_src

    td = Path(tempfile.mkdtemp())
    docs = [td / f"doc{i}.xlsx" for i in range(12)]
    for p in docs:
        p.write_text("x", encoding="utf-8")
    settings = {
        "fences": [
            {
                "id": "docs",
                "virtual_items": [str(p) for p in docs],
                "sort_by": "manual",
            }
        ],
        "public_desktop_items": [],
    }
    calls = {"n": 0}

    def boom(_path: P) -> P:
        calls["n"] += 1
        raise AssertionError("display must not canonicalize")

    try:
        with patch.object(fr, "_canonicalize_virtual_pin_path", boom):
            shown = get_virtual_items_for_fence(settings["fences"][0], settings)
        assert len(shown) == 12
        assert calls["n"] == 0
    finally:
        import shutil

        shutil.rmtree(td, ignore_errors=True)


def test_hot_path_no_resolve_storms() -> None:
    """Interactive helpers must not Path.resolve / full free-list on drop."""
    import inspect

    from src.path_stat_cache import path_present
    from src.public_desktop import find_auto_slot, prune_missing_public_items
    from src.win_shell import is_desktop_loose_item

    loose_src = inspect.getsource(is_desktop_loose_item)
    # Docstring may mention the old pitfall; body must not call it.
    body = loose_src.split('"""', 2)[-1]
    assert "path.resolve()" not in body
    assert ".samefile(" not in body
    assert "casefold()" in body

    assert callable(path_present)
    assert "path_present" in inspect.getsource(prune_missing_public_items)
    pub_prune = inspect.getsource(prune_missing_public_items)
    assert "_missing_since" in pub_prune
    assert "sticky_missing_s" in pub_prune

    from src.win_shell import import_drop_to_folder, move_item_to_folder

    norm_block = inspect.getsource(import_drop_to_folder).split("def _norm_path")[1].split(
        "\n    def "
    )[0]
    assert "path.resolve()" not in norm_block
    assert "move_path_into_folder" in inspect.getsource(move_item_to_folder)

    slot_src = inspect.getsource(find_auto_slot)
    assert "free.append" not in slot_src
    assert "prefer_nearest" in slot_src

    from src.ui import fence_icon_item as fii
    from src.app import DeskTidyApp

    virt_paste = inspect.getsource(fii.paste_files_into_fence)
    pub_paste = inspect.getsource(fii.paste_files_to_public)
    assert "path.resolve()" not in virt_paste
    assert "path.resolve()" not in pub_paste
    assert "is_desktop_loose_item" in virt_paste
    assert "is_desktop_loose_item" in pub_paste
    # Paste lands on left-edge grid — not under the mouse (prefer_nearest).
    assert "prefer_nearest=False" in pub_paste
    assert "prefer_nearest=True" not in pub_paste
    ingest_src = inspect.getsource(DeskTidyApp._try_ingest_desktop_file_as_public)
    assert "prefer_nearest=False" in ingest_src


def test_folder_drop_does_not_unpin_icons() -> None:
    """Dropping a .lnk onto a folder must not fall through while still on that folder."""
    import inspect

    from src.ui import fence_icon_item as fii

    custom = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
    # Folder miss: Ignore only when still on a drop target; else allow unpin.
    folder_block = custom.split("folder_drop_target_at")[1].split(
        "fence_widget_at(drop_pos"
    )[0]
    assert "IgnoreAction" in folder_block
    assert "moved_keys" in folder_block
    assert "copied_any" in folder_block
    assert "should_unpin_virtual_drop" in folder_block
    assert "skip folder move for icon" in inspect.getsource(
        fii._move_virtual_into_folder
    )

    pub = inspect.getsource(fii._move_public_into_folder)
    assert "path_organize_kind" in pub
    assert "unique_dest_path" not in pub
    assert "_run_transfer_into_folder" in pub
    assert "_restore_virtual_folder_move_failure" in inspect.getsource(
        fii._move_virtual_into_folder
    )


def test_fence_chrome_drop_unpins_not_reorder() -> None:
    """Release on fence title/empty body must unpin — not same-fence reorder.

    Regression: geometry-only fence hit + always-True reorder made the second
    drag-out on large 文档 partitions appear broken.
    """
    import inspect

    from src.ui import fence_icon_item as fii

    assert callable(fii.fence_accepts_virtual_drop_at)
    assert callable(fii.fence_items_global_rect)
    custom = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
    assert "fence_accepts_virtual_drop_at" in custom
    unpin = inspect.getsource(fii.should_unpin_virtual_drop)
    assert "fence_accepts_virtual_drop_at" in unpin
    assert "fence_widget_at(global_pos) is None" not in unpin
    # Contract: apply only when accepts grid.
    apply_gate = custom.split("fence_widget_at(drop_pos, geometry_only=True)")[1].split(
        "is_visible_external_app_drop_point"
    )[0]
    assert "fence_accepts_virtual_drop_at" in apply_gate
    assert "is_visible_external_app_drop_point" in custom
    # Unpin gate must call visible-top check, not chrome-skip helper as the call.
    assert "is_visible_external_app_drop_point(drop_pos" in custom
    assert "is_external_app_drop_point(drop_pos" not in custom
    shell = inspect.getsource(
        __import__("src.win_shell", fromlist=["x"]).is_visible_external_app_drop_point
    )
    assert "WindowFromPoint" in shell
    assert "_hwnd_under_drag_point" not in shell


def test_path_pinned_exact_only() -> None:
    """Same basename in another folder must not count as pinned."""
    from src.fence_rules import path_in_pinned_keys
    from src.ui.fence_icon_item import path_pinned_in_settings

    settings = {
        "fences": [
            {
                "id": "f1",
                "virtual_items": [r"D:\desktop\report.xlsx"],
            }
        ]
    }
    assert path_pinned_in_settings(settings, r"D:\desktop\report.xlsx")
    assert not path_pinned_in_settings(settings, r"E:\other\report.xlsx")
    # Slash / case variants of the exact path still match via _norm_virtual_key.
    assert path_pinned_in_settings(settings, r"d:/desktop/report.xlsx")

    pinned = {r"d:\desktop\report.xlsx".casefold()}
    assert path_in_pinned_keys(r"D:\desktop\report.xlsx", pinned)
    assert path_in_pinned_keys(r"d:/desktop/report.xlsx", pinned)
    assert not path_in_pinned_keys(r"E:\other\report.xlsx", pinned)


def test_settings_atomic_write_contract() -> None:
    """Debounced/immediate saves must serialize and use temp+replace."""
    import inspect

    from src import settings as st

    now_src = inspect.getsource(st._save_settings_now)
    assert "os.replace" in now_src
    assert "mkstemp" in now_src
    flush_src = inspect.getsource(st._flush_pending_settings)
    # Write stays under the save lock (no race with immediate=True).
    assert "_save_settings_now(settings)" in flush_src
    assert flush_src.index("with _SAVE_LOCK") < flush_src.index("_save_settings_now")
    save_src = inspect.getsource(st.save_settings)
    imm = save_src.split("if immediate:")[1].split("return")[0]
    assert "_save_settings_now(snapshot)" in imm
    assert "with _SAVE_LOCK" in imm
    # Snapshot must be taken under the lock (not before acquiring it).
    assert imm.index("with _SAVE_LOCK") < imm.index("deepcopy(settings)")
    assert imm.index("deepcopy(settings)") < imm.index("_save_settings_now(snapshot)")
    deb = save_src.split("with _SAVE_LOCK:")[1]
    assert "deepcopy(settings)" in deb.split("next_timer.start()")[0]


def test_public_overlay_ends_after_ole_and_timeout_cancels() -> None:
    """Public drag must keep overlay freeze through OLE; 45s timeout = cancel."""
    import inspect

    from src.ui import fence_icon_item as fii

    pub = inspect.getsource(fii.start_public_item_drag)
    virt = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
    # Overlay end is in the post-handoff finally — after OLE / folder settle.
    assert pub.index("_exec_external_file_ole_drag") < pub.rindex(
        "_end_overlay_drag_session"
    )
    assert "quiet_end = True" in pub
    assert "reconcile=not quiet_end" in pub
    for src in (pub, virt):
        timeout = src.split("def _drag_timeout")[1].split("QTimer.singleShot")[0]
        assert "filt.cancelled = True" in timeout
        assert "loop.quit()" in timeout
        assert "45000" in src


def test_find_auto_slot_taken_is_exact_set() -> None:
    """Occupied slots use O(1) set membership, not a linear fuzzy scan."""
    import inspect

    from src.public_desktop import find_auto_slot

    src = inspect.getsource(find_auto_slot)
    taken = src.split("def _taken")[1].split("\n    if prefer_nearest")[0]
    assert " in occupied" in taken
    assert "CELL_W // 2" not in taken
    assert "for ox, oy in occupied" not in taken


def test_unpin_snaps_left_edge_and_forces_show() -> None:
    """fence→public must snap left-edge grid + deferred overlay show (no vanish)."""
    import inspect
    import tempfile

    from PyQt6.QtCore import QPoint, QRect

    from src.app import DeskTidyApp
    from src.public_desktop import (
        CELL_W,
        GAP_X,
        MARGIN_X,
        MARGIN_Y,
        _work_area,
        add_fence_unpin_public_item,
        find_auto_slot,
        get_public_items,
    )
    from src.ui import fence_icon_item as fii
    from src.ui.fence_widget import FenceWidget

    unpin_src = inspect.getsource(FenceWidget._unpin_virtual_paths_impl)
    assert "last_virtual_unpin_pos" in unpin_src
    assert "add_fence_unpin_public_item" in unpin_src
    assert "path.resolve()" not in unpin_src
    # Soft unpin: pin-list edit only (no get_entries / exists / COM canonicalize).
    assert "unpin_paths_from_virtual_fence" in unpin_src
    assert "for p in self._get_entries()" not in unpin_src
    assert "set_virtual_item_order" not in unpin_src
    assert "_remove_virtual_icon_widgets" in unpin_src
    assert "invalidate_loose_sync_cache(" not in unpin_src
    assert "public_items_changed.emit" in unpin_src
    # Single-path wrapper stays for delete/recycle call sites.
    wrap = inspect.getsource(FenceWidget._unpin_virtual_item_impl)
    assert "_unpin_virtual_paths_impl" in wrap

    single_src = inspect.getsource(DeskTidyApp._ensure_single_public_float)
    # Surgical float must keep sync sig so post-drag refresh early-returns —
    # but only AFTER host.present + sibling reconcile (second unpin must not
    # blank float #1 while sync_sig pretends everything is healthy).
    after_append = single_src.split("self.public_icons.append")[1]
    assert "_last_public_sync_sig = None" not in after_append
    assert "_last_public_sync_sig = tuple" in after_append
    assert "visible_floating_items" in single_src or "_reconcile_live_public_floats" in single_src
    assert "host.present" in after_append
    present_at = after_append.find("host.present")
    sig_at = after_append.find("_last_public_sync_sig = tuple")
    assert present_at >= 0 and sig_at > present_at
    assert "_reconcile_live_public_floats" in after_append
    recon_at = after_append.find("_reconcile_live_public_floats")
    assert recon_at > present_at and sig_at > recon_at
    recon_src = inspect.getsource(DeskTidyApp._reconcile_live_public_floats)
    assert "move_to_screen" in recon_src
    assert "visible_floating_items" in recon_src
    assert "return None" in recon_src or "complete = False" in recon_src
    refresh_early = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "have_keys == wanted_keys" in refresh_early
    from src.ui.public_icon_widget import PublicIconWidget

    assert "display_file_icon_pixmap" in inspect.getsource(PublicIconWidget._reload_icon)
    drag_src = inspect.getsource(fii.start_virtual_item_drag)
    custom_src = inspect.getsource(fii._run_custom_desktop_resident_virtual_drag)
    assert "_run_custom_desktop_resident_virtual_drag" in drag_src
    assert "virtual_unpin_drop_hint" in custom_src
    assert "_drag_drop_anchor" not in custom_src.split("_last_virtual_unpin_pos")[1].split("return")[0]

    wire_src = inspect.getsource(DeskTidyApp._wire_fence_signals)
    assert "_on_fence_public_items_changed" in wire_src
    assert "relayout=True" not in wire_src

    handler_src = inspect.getsource(DeskTidyApp._on_fence_public_items_changed)
    assert "_ensure_single_public_float" in handler_src
    assert "_public_drag_settling" in handler_src
    assert "_remap_hidden_overlay_hwnds" in handler_src
    assert "relayout=False" in handler_src
    assert "_last_public_sync_sig = None" not in handler_src.split("refresh_public")[0]
    assert "_ensure_single_public_float" in inspect.getsource(DeskTidyApp)
    soft_src = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "force=False" in soft_src
    assert "_ensure_shell_attachments(force=True)" not in soft_src.split(
        "if created_new > 0"
    )[1].split("elif structure_changed")[0]

    show_src = inspect.getsource(DeskTidyApp._ensure_new_public_floats_visible)
    assert "_configure_public_float_overlay" in show_src
    assert "force=True" not in show_src
    assert "_ensure_desktop_overlays_visible" in show_src

    refresh_src = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "_ensure_new_public_floats_visible" in refresh_src
    # Missing-path floats must be pruned even when relayout=False (ghost file icon).
    assert "prune_missing_public_items" in refresh_src.split("if relayout:")[0]
    fin = inspect.getsource(fii._finalize_virtual_folder_move)
    assert "remove_public_paths" in fin
    assert "_remove_public_icon_widget" in fin
    assert "_remove_virtual_icon_widget" in fin
    assert "refresh_desktop" not in fin
    end_src = inspect.getsource(DeskTidyApp._end_overlay_drag)
    assert "reconcile_fg" in end_src
    assert "_heal_overlays_after_shell_menu" in end_src
    assert "_deferred_fg_sync" in end_src
    removed = inspect.getsource(DeskTidyApp._on_watched_file_removed)
    assert "is_organize_suppressed" in removed
    assert "remove_public_paths(" not in removed
    assert "_remove_public_icon_widget(" not in removed
    assert "prune_missing_public_items" in removed
    assert "_schedule_sticky_missing_reprune" in removed

    moved_src = inspect.getsource(DeskTidyApp._on_watched_file_moved)
    assert "leaving_desktop" in moved_src
    assert "remove_public_paths(self.settings, [old, new])" in moved_src
    assert "_remove_public_icon_widget(old)" in moved_src
    # Must not rewrite public→off-desktop before dropping (ghost float).
    leave_block = moved_src.split("if leaving_desktop:")[1].split("changed = False")[0]
    assert "rewrite_public_path" not in leave_block

    pin_src = inspect.getsource(DeskTidyApp._on_public_icon_pinned)
    assert "_norm_virtual_key" in pin_src
    assert "Path(raw_s).name" not in pin_src
    assert "Basename fallback" not in pin_src

    refresh_match = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "wanted_ns_names" in refresh_match
    assert "wanted_names =" not in refresh_match
    assert "drag_names" not in refresh_match

    apply_src = inspect.getsource(FenceWidget._apply_virtual_drop_paths)
    assert "already_names" not in apply_src
    assert "_norm_virtual_key" in apply_src

    slot_src = inspect.getsource(find_auto_slot)
    assert "slot.contains(hint)" in slot_src
    assert "free.append" not in slot_src
    assert "materializes every free cell" in slot_src or "Streams the grid once" in slot_src

    hint_src = inspect.getsource(fii.virtual_unpin_drop_hint)
    assert "cursor.x()" in hint_src
    assert "- 48" not in hint_src

    # Left-edge snap: monitor hint only; first free column-major cell.
    fii._last_virtual_unpin_pos = QPoint(640, 420)
    td = Path(tempfile.mkdtemp())
    p = td / "unpin_drop.txt"
    p.write_text("u", encoding="utf-8")
    try:
        settings = {
            "enable_public_desktop": True,
            "current_page": 0,
            "public_desktop_items": [],
            "fences": [],
            "exclude_patterns": [],
        }
        area = _work_area(640, 420)
        entry = add_fence_unpin_public_item(
            settings,
            p,
            page_id=0,
            fence_rects=[],
            monitor_hint_x=640,
            monitor_hint_y=420,
        )
        items = get_public_items(settings)
        assert items, "unpin must create a public entry"
        assert int(entry["x"]) == area.left() + MARGIN_X, entry
        assert int(entry["y"]) == area.top() + MARGIN_Y, entry

        # Left column blocked by a fence → next column to the right.
        left_col_x = area.left() + MARGIN_X
        fence_rect = QRect(left_col_x, area.top(), CELL_W, area.height())
        sx, sy = find_auto_slot(
            settings,
            hint_x=640,
            hint_y=420,
            exclude_path=p,
            fence_rects=[fence_rect],
            prefer_nearest=False,
        )
        assert sx == left_col_x + CELL_W + GAP_X, (sx, sy, left_col_x)
        assert sy == area.top() + MARGIN_Y, (sx, sy)
    finally:
        p.unlink(missing_ok=True)
        fii._last_virtual_unpin_pos = None


def test_sequential_unpin_reconciles_before_sync_sig() -> None:
    """Second fence→public soft-add must re-place siblings before stamping sync_sig.

    Regression: host.present remasks the shared HWND; stamping sync_sig first let
    post-drag refresh early-return while float #1 stayed blank until a later drag.
    """
    import inspect

    from src.app import DeskTidyApp

    single = inspect.getsource(DeskTidyApp._ensure_single_public_float)
    body = single.split("self.public_icons.append")[1]
    assert "host.present" in body
    assert "_reconcile_live_public_floats" in body
    assert body.find("host.present") < body.find("_reconcile_live_public_floats")
    assert body.find("_reconcile_live_public_floats") < body.find(
        "_last_public_sync_sig = tuple"
    )
    # After soft-add present(), fences must regain click/drag (host can sit on top).
    assert "ensure_live_fences_interactive" in body
    assert body.find("host.present") < body.find("ensure_live_fences_interactive")
    # Boot force-attach remounts host last; without ensure_live the first
    # fence→public drag never starts (no virtual-drag log lines).
    boot = inspect.getsource(DeskTidyApp._startup_force_overlay_attach)
    assert "ensure_live_fences_interactive" in boot
    assert boot.find("_ensure_shell_attachments") < boot.find(
        "ensure_live_fences_interactive"
    )
    pending_force = inspect.getsource(DeskTidyApp._run_pending_force_shell_attach)
    assert "ensure_live_fences_interactive" in pending_force
    assert pending_force.find("_ensure_shell_attachments(force=True)") < pending_force.find(
        "ensure_live_fences_interactive"
    )
    unpin_impl = inspect.getsource(
        __import__("src.ui.fence_widget", fromlist=["FenceWidget"]).FenceWidget._unpin_virtual_paths_impl
    )
    # place_on_public branch must clear grab chrome and re-assert interactivity.
    assert "_abort_item_interaction" in unpin_impl
    assert "_ensure_drop_target_interactive" in unpin_impl
    recon = inspect.getsource(DeskTidyApp._reconcile_live_public_floats)
    assert "for ent in wanted" in recon or "for ent in" in recon
    assert "move_to_screen" in recon
    assert "icon.show()" in recon
    # Incomplete sibling set must not stamp a full sync signature.
    assert "complete = False" in recon
    assert "return None" in recon or "if complete else None" in recon
    refresh = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "have_keys == wanted_keys" in refresh
    assert "len(self.public_icons) == len(wanted)" not in refresh.split(
        "if ("
    )[1].split("):")[0]


def test_fence_public_roundtrip_exclusive() -> None:
    """fence→public→fence must leave the path only in virtual_items."""
    import tempfile

    from src.fence_rules import (
        assign_paths_to_virtual_fence,
        get_virtual_items_for_fence,
        set_virtual_item_order,
    )
    from src.public_desktop import (
        add_public_item,
        get_public_items,
        remove_public_paths,
    )

    td = Path(tempfile.mkdtemp())
    p = td / "roundtrip.txt"
    p.write_text("r", encoding="utf-8")
    try:
        settings = {
            "enable_public_desktop": True,
            "public_desktop_items": [],
            "fences": [
                {
                    "id": "f1",
                    "name": "F",
                    "extensions": [],
                    "virtual_items": [str(p)],
                    "sort_by": "manual",
                }
            ],
        }
        # Unpin to public (path spelling drift on purpose).
        set_virtual_item_order(settings["fences"][0], [])
        add_public_item(
            settings, p, x=10, y=10, auto_arrange=False, shared=True
        )
        # Simulate stored public path with different string form.
        settings["public_desktop_items"][0]["path"] = str(p).replace("\\", "/")
        assert get_public_items(settings)

        assign_paths_to_virtual_fence(settings["fences"][0], settings, [p])
        # Basename fallback must clear drifted public entries.
        remove_public_paths(settings, [p])
        assert not get_public_items(settings), get_public_items(settings)
        shown = get_virtual_items_for_fence(settings["fences"][0], settings)
        assert any(x.name == p.name for x in shown), shown
    finally:
        p.unlink(missing_ok=True)


def test_public_drag_refresh_keeps_hidden_folder() -> None:
    """Refresh during/after drag must not park a still-hidden folder float away."""
    import inspect
    import tempfile

    from PyQt6.QtWidgets import QApplication

    from src.app import DeskTidyApp
    from src.public_desktop import add_public_item
    from src.ui.fence_icon_item import start_public_item_drag
    from src.ui.public_icon_widget import PublicIconWidget

    app = QApplication.instance() or QApplication([])
    src = inspect.getsource(DeskTidyApp._end_overlay_drag)
    assert "_flush_public_refresh_after_drag" in src
    assert "singleShot(120" in src
    impl = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "protect_drag" in impl
    assert "_public_drag_path" in impl
    drag_src = inspect.getsource(start_public_item_drag)
    assert "_set_public_drag_path" in drag_src
    assert "_restore_public_icon_widget" in drag_src
    assert "pin_public_path_to_fence" in drag_src
    assert "geometry_only=True" in drag_src
    assert "QDrag(" not in drag_src
    assert "_PublicDragFilter" in drag_src
    assert "QEventLoop" in drag_src
    flush_src = inspect.getsource(DeskTidyApp._flush_public_refresh_after_drag)
    assert "_public_drag_settling" in flush_src
    restore_src = inspect.getsource(DeskTidyApp._restore_public_icon_widget)
    assert "immediate=True" not in restore_src
    refresh_src = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "processEvents" not in refresh_src

    td = Path(tempfile.mkdtemp())
    folder = td / "drag_folder"
    folder.mkdir()
    settings = {
        "enable_public_desktop": False,
        "current_page": 0,
        "public_desktop_items": [],
        "fences": [],
        "theme": "mist",
    }
    add_public_item(
        settings, folder, x=40, y=40, page_id=0, auto_arrange=False, shared=False
    )

    class Fake:
        pass

    fake = Fake()
    fake._exiting = False
    fake._icons_hidden = False
    fake._public_relayout_pending = False
    fake._public_force_relayout = False
    fake._last_public_sync_sig = None
    fake._last_public_fence_key = None
    fake._parked_public_icons = {}
    fake._max_parked_public_icons = 80
    fake._public_drag_path = str(folder).casefold()
    fake._public_icon_host = None
    fake.settings = settings
    fake.public_icons = []
    fake._retire_overlay_widget = DeskTidyApp._retire_overlay_widget.__get__(fake)
    fake._park_public_icon = DeskTidyApp._park_public_icon.__get__(fake)
    fake._take_parked_public_icon = DeskTidyApp._take_parked_public_icon.__get__(fake)
    fake._ensure_public_icon_host = DeskTidyApp._ensure_public_icon_host.__get__(fake)
    fake._configure_public_float_overlay = (
        DeskTidyApp._configure_public_float_overlay.__get__(fake)
    )
    fake._ensure_shell_attachments = lambda **kw: None
    fake._ensure_desktop_overlays_visible = lambda **kw: None
    fake.ensure_live_fences_interactive = lambda: None
    fake._schedule_overlay_keepalive = lambda: None
    fake._current_page = lambda: 0
    fake._on_public_icon_moved = lambda *a, **k: None
    fake._on_public_icon_pinned = lambda *a, **k: None
    fake._on_public_icon_removed = lambda *a, **k: None
    fake._overlay_widgets_may_show = lambda: True
    fake._public_drop_surface_needed = lambda host: False
    fake._purge_temp_desktop_refs = lambda: None

    host = DeskTidyApp._ensure_public_icon_host(fake)
    widget = PublicIconWidget(folder, 40, 40, parent=host)
    widget.hide()  # drag hide
    fake.public_icons = [widget]
    # Avoid scanning the real desktop (would create extra floats on Fake).
    orig_sync = getattr(fake, "_start_loose_sync_background", None)
    fake._start_loose_sync_background = lambda: None
    try:
        DeskTidyApp._refresh_public_desktop_impl(fake, relayout=False)
    finally:
        if orig_sync is not None:
            fake._start_loose_sync_background = orig_sync
        elif hasattr(fake, "_start_loose_sync_background"):
            delattr(fake, "_start_loose_sync_background")
    assert widget in fake.public_icons, "hidden drag folder must stay listed"
    assert str(folder).casefold() not in fake._parked_public_icons
    # After drag path cleared, restore path works from parked/active.
    fake._public_drag_path = None
    widget.hide()
    DeskTidyApp._set_public_drag_path(fake, None)
    DeskTidyApp._restore_public_icon_widget(fake, folder, icon=widget)
    assert widget.isVisible()
    widget.close()
    widget.deleteLater()
    if fake._public_icon_host is not None:
        fake._public_icon_host.close()
        fake._public_icon_host.deleteLater()
    app.processEvents()


def test_same_fence_reorder_no_flash() -> None:
    """In-fence icon reorder must not rebuild icons or broadcast files_changed."""
    import inspect
    import tempfile
    from pathlib import Path

    from PyQt6.QtWidgets import QApplication

    from src.ui.fence_widget import FenceWidget

    apply_src = inspect.getsource(FenceWidget._apply_virtual_drop_paths)
    assert "_relayout_icons_to_virtual_order" in apply_src
    # Same-fence branch returns before files_changed / full finish UI.
    same = apply_src.split("source_id == self_id", 1)[1].split("else:", 1)[0]
    assert "_relayout_icons_to_virtual_order" in same
    assert "files_changed" not in same
    assert "_finish_virtual_drop_ui" not in same
    assert "save_settings" in same

    app = QApplication.instance() or QApplication([])
    tmp = Path(tempfile.mkdtemp(prefix="desktidy_reorder_"))
    files = [tmp / f"r{i}.txt" for i in range(3)]
    for f in files:
        f.write_text("x", encoding="utf-8")

    settings = {
        "fences": [
            {
                "id": "reord",
                "name": "测",
                "sort_by": "manual",
                "virtual_items": [str(p) for p in files],
                "x": 80,
                "y": 80,
                "width": 360,
                "height": 280,
            }
        ],
        "theme": "light",
        "current_page": 0,
        "desktop_pages": [{"id": 0, "name": "默认"}],
    }
    cfg = settings["fences"][0]
    fence = FenceWidget(cfg, settings)
    fence.show()
    app.processEvents()
    fence.refresh(force=True)
    app.processEvents()

    before_ids = [id(w) for w in fence._icon_item_widgets()]
    assert len(before_ids) == 3

    # Move first item to the end.
    ok = fence._apply_virtual_drop_paths(
        [files[0]], source_id="reord", insert_at=3
    )
    assert ok
    app.processEvents()

    after = fence._icon_item_widgets()
    after_ids = [id(w) for w in after]
    # Same widget instances, new grid order — not deleteLater + recreate.
    assert after_ids == [before_ids[1], before_ids[2], before_ids[0]]
    assert [Path(w.file_path) for w in after] == [files[1], files[2], files[0]]
    assert [Path(p) for p in (cfg.get("virtual_items") or [])] == [
        files[1],
        files[2],
        files[0],
    ]

    fence.close()
    fence.deleteLater()
    app.processEvents()


def test_drag_copy_folder_fence_chain() -> None:
    """E2E chain: copy → into folder → folder file into fence → fence out to public.

    Covers the gestures the user cares about after the pet/folder OLE fixes:
    file copy, drag into folder, drag from a folder path into a fence, and
    drag out of a fence onto the public desktop.
    """
    import os
    import shutil
    from unittest.mock import patch

    from PyQt6.QtWidgets import QApplication, QWidget

    os.environ.setdefault("QT_QPA_PLATFORM", "windows")
    _ = QApplication.instance() or QApplication([])

    from src.fence_rules import (
        assign_paths_to_virtual_fence,
        get_virtual_items_for_fence,
        unpin_paths_from_virtual_fence,
    )
    from src.public_desktop import (
        add_fence_unpin_public_item,
        add_public_item,
        find_public_entry,
        remove_public_paths,
    )
    from src.ui import fence_icon_item as fii
    from src.ui.fence_icon_item import (
        _move_public_into_folder,
        _move_virtual_into_folder,
        path_pinned_in_settings,
        should_treat_as_virtual_unpin,
        should_unpin_virtual_drop,
    )
    from src.ui.public_icon_widget import PublicIconWidget
    from PyQt6.QtCore import QPoint, Qt

    root = Path(tempfile.mkdtemp(prefix="desktidy_drag_chain_"))
    desk = root / "Desktop"
    desk.mkdir()
    folder = desk / "项目资料"
    folder.mkdir()
    src = desk / "协议.docx"
    src.write_text("doc-v1", encoding="utf-8")

    settings = {
        "enable_public_desktop": True,
        "public_desktop_items": [],
        "exclude_patterns": [],
        "fences": [
            {
                "id": "f_docs",
                "name": "文档",
                "extensions": [],
                "virtual_items": [],
                "sort_by": "manual",
            }
        ],
    }

    class _Fence(QWidget):
        def __init__(self) -> None:
            super().__init__()
            self.config = settings["fences"][0]
            self.settings = settings
            self.refresh_calls = 0

        def refresh(self, force: bool = False) -> None:  # noqa: ARG002
            self.refresh_calls += 1

    class _Desk:
        def __init__(self) -> None:
            self.settings = settings
            self.public_icons: list = []
            self.fences: list = []
            self._removed: list[Path] = []

        def _remove_public_icon_widget(self, path: Path) -> None:
            self._removed.append(Path(path))

        def refresh_public_desktop(self, relayout: bool = False) -> None:
            return None

    app = QApplication.instance()
    desk_app = _Desk()
    fence = _Fence()
    desk_app.fences = [fence]
    app._desktidy_app = desk_app  # type: ignore[attr-defined]

    try:
        # --- 1) File copy into fence (Ctrl+C / Ctrl+V COPY) ---
        def _sync_bg(work, *, on_success, on_failure, **kwargs):  # noqa: ARG001
            try:
                on_success(work())
            except BaseException as exc:  # noqa: BLE001
                on_failure(exc)

        with (
            patch.object(fii, "find_fence_widget", return_value=fence),
            patch.object(fii, "_start_background_fs_paste", side_effect=_sync_bg),
            patch(
                "src.shell_clipboard.clipboard_get_files_with_effect",
                return_value=([src], 1),  # DROPEFFECT_COPY
            ),
            patch("src.settings.get_desktop_path", return_value=desk),
            patch("src.settings.get_desktop_paths", return_value=[desk]),
            patch("src.settings.save_settings"),
        ):
            assert fii.paste_files_into_fence(fence) is True

        twins = sorted(desk.glob("协议*.docx"))
        assert len(twins) >= 2, twins
        assert src.exists() and src.read_text(encoding="utf-8") == "doc-v1"
        copy_pin = next(p for p in twins if p.resolve() != src.resolve())
        assert path_pinned_in_settings(settings, copy_pin)
        assert not find_public_entry(settings, copy_pin)

        # --- 2) Drag public float into folder ---
        public_doc = desk / "待归类.txt"
        public_doc.write_text("loose", encoding="utf-8")
        add_public_item(
            settings, public_doc, x=20, y=20, page_id=0, auto_arrange=False
        )
        file_icon = PublicIconWidget(public_doc, 20, 20)
        desk_app.public_icons = [file_icon]
        assert _move_public_into_folder(
            desk_app, settings, file_icon, public_doc, folder
        ) == "moved"
        landed = folder / "待归类.txt"
        assert landed.exists() and not public_doc.exists()
        assert find_public_entry(settings, public_doc) is None

        # Virtual (fence) doc into same folder
        fence_doc = desk / "分区内.docx"
        fence_doc.write_text("in-fence", encoding="utf-8")
        settings["fences"][0]["virtual_items"] = [str(fence_doc), str(copy_pin)]
        assert _move_virtual_into_folder(fence_doc, folder, "f_docs") == "moved"
        assert (folder / "分区内.docx").exists() and not fence_doc.exists()
        pins_after = [str(p).casefold() for p in settings["fences"][0]["virtual_items"]]
        assert str(fence_doc).casefold() not in pins_after
        assert str(copy_pin).casefold() in pins_after

        # --- 3) From folder path → pin into fence (「从文件夹拖到分区」) ---
        from_folder = folder / "待归类.txt"
        assert from_folder.exists()
        add_public_item(
            settings, from_folder, x=40, y=40, page_id=0, auto_arrange=False
        )
        assert find_public_entry(settings, from_folder) is not None
        assign_paths_to_virtual_fence(settings["fences"][0], settings, [from_folder])
        remove_public_paths(settings, [from_folder])
        assert find_public_entry(settings, from_folder) is None
        assert path_pinned_in_settings(settings, from_folder)
        shown = get_virtual_items_for_fence(settings["fences"][0], settings)
        assert any(p.resolve() == from_folder.resolve() for p in shown), shown
        # File stays where it lived (virtual pin — no forced move onto desktop).
        assert from_folder.exists()

        # --- 4) Fence → outside (unpin to public) ---
        assert should_unpin_virtual_drop(None) is True  # no visible FenceWidget frames
        assert should_treat_as_virtual_unpin(
            catcher_accepted=False,
            drop_action=Qt.DropAction.IgnoreAction,
            start_global=QPoint(0, 0),
            end_global=QPoint(400, 400),
            min_unpin_distance=36,
        )
        # Tiny click jitter must NOT unpin.
        assert not should_treat_as_virtual_unpin(
            catcher_accepted=False,
            drop_action=Qt.DropAction.IgnoreAction,
            start_global=QPoint(100, 100),
            end_global=QPoint(105, 105),
            min_unpin_distance=36,
        )

        assert unpin_paths_from_virtual_fence(settings["fences"][0], [from_folder])
        add_fence_unpin_public_item(
            settings,
            from_folder,
            page_id=0,
            monitor_hint_x=120,
            monitor_hint_y=120,
        )
        assert not path_pinned_in_settings(settings, from_folder)
        assert find_public_entry(settings, from_folder) is not None
        assert from_folder.exists()
        # Exclusive: not still listed as a fence pin.
        left = get_virtual_items_for_fence(settings["fences"][0], settings)
        assert not any(p.resolve() == from_folder.resolve() for p in left)

        # Public drag still prefers folder over fence pin order.
        drag_src = __import__("inspect").getsource(fii.start_public_item_drag)
        assert drag_src.index("public drag: into folder") < drag_src.index(
            "public drag: sync pin fence"
        )

        file_icon.close()
    finally:
        try:
            fence.close()
        except Exception:
            pass
        shutil.rmtree(root, ignore_errors=True)


def test_explorer_like_folder_drop_modifiers() -> None:
    """Same-vol move / Ctrl copy / cross-vol copy; copy keeps pin/float."""
    import os
    import shutil
    import tempfile
    from unittest import mock

    from PyQt6.QtWidgets import QApplication

    os.environ.setdefault("QT_QPA_PLATFORM", "windows")
    app = QApplication.instance() or QApplication([])

    from src.public_desktop import add_public_item, find_public_entry
    from src.ui import fence_icon_item as fii
    from src.ui.public_icon_widget import PublicIconWidget
    from src.win_shell import default_fs_drop_is_move

    assert default_fs_drop_is_move(Path("C:/a"), Path("C:/b"), ctrl=False, shift=False)
    assert not default_fs_drop_is_move(Path("C:/a"), Path("C:/b"), ctrl=True, shift=False)
    assert default_fs_drop_is_move(Path("C:/a"), Path("C:/b"), ctrl=False, shift=True)
    # Ctrl wins over Shift (shortcut creation out of scope).
    assert not default_fs_drop_is_move(Path("C:/a"), Path("C:/b"), ctrl=True, shift=True)
    with mock.patch("src.win_shell._same_volume", return_value=False):
        assert not default_fs_drop_is_move(
            Path("C:/a"), Path("D:/b"), ctrl=False, shift=False
        )
        assert default_fs_drop_is_move(
            Path("C:/a"), Path("D:/b"), ctrl=False, shift=True
        )

    root = Path(tempfile.mkdtemp(prefix="desktidy_fs_drop_mod_"))
    try:
        folder = root / "箱"
        folder.mkdir()
        src = root / "报告.txt"
        src.write_text("v1", encoding="utf-8")

        class _Desk:
            def __init__(self) -> None:
                self.settings = {
                    "fences": [
                        {
                            "id": "f1",
                            "name": "文档",
                            "virtual_items": [str(src)],
                        }
                    ],
                    "public_desktop_items": [],
                    "exclude_patterns": [],
                }
                self.fences: list = []
                self._removed: list = []

            def remove_public_icon(self, path):
                self._removed.append(path)

        desk = _Desk()
        app._desktidy_app = desk  # type: ignore[attr-defined]

        with mock.patch(
            "src.win_shell.default_fs_drop_is_move", return_value=False
        ):
            assert fii._move_virtual_into_folder(src, folder, "f1") == "copied"
        assert src.exists() and src.read_text(encoding="utf-8") == "v1"
        copies = list(folder.glob("报告*.txt"))
        assert len(copies) == 1
        pins = [str(p).casefold() for p in desk.settings["fences"][0]["virtual_items"]]
        assert str(src).casefold() in pins

        # Public float: Ctrl/cross-vol copy must leave the float.
        pub = root / "浮标.txt"
        pub.write_text("float", encoding="utf-8")
        add_public_item(
            desk.settings, pub, x=10, y=10, page_id=0, auto_arrange=False
        )
        icon = PublicIconWidget(pub, 10, 10)
        desk.public_icons = [icon]
        with mock.patch(
            "src.win_shell.default_fs_drop_is_move", return_value=False
        ):
            assert (
                fii._move_public_into_folder(desk, desk.settings, icon, pub, folder)
                == "copied"
            )
        assert pub.exists()
        assert find_public_entry(desk.settings, pub) is not None
        assert list(folder.glob("浮标*.txt"))
    finally:
        shutil.rmtree(root, ignore_errors=True)


def main() -> int:
    print("DeskTidy public→fence drag regression")
    run("assign 分离 config 写入 settings", test_split_assign_writes_settings)
    run("assign 先写钉选再清公共区", test_assign_writes_pin_before_clearing_public)
    run("钉选不冲掉分区原有图标", test_pin_seed_keeps_existing_pins)
    run("pin_public 原子钉选可见", test_pin_public_path_atomic)
    run("loose sync 节流", test_loose_sync_throttled)
    run("同名公共项不误删", test_basename_remove_only_when_unique)
    run("移出桌面立刻去掉公共浮标", test_move_off_desktop_drops_public_float)
    run("拖拽避开 Explorer OLE 死锁", test_drag_avoids_explorer_ole_deadlock)
    run("拖拽时隐藏源图标不重影", test_custom_drag_hides_source_not_translucent)
    run("拖到外部门窗不挪位消失", test_external_release_does_not_relocate)
    run("公共图标拖进文件夹", test_public_folder_drop_moves_file)
    run("分区图标拖进文件夹", test_virtual_drag_moves_into_folder)
    run("分区内重排不闪屏", test_same_fence_reorder_no_flash)
    run("分区文件夹命中优先于背后资源管理器", test_fence_folder_hit_beats_explorer_underneath)
    run("文档真实移入文件夹/图标保持虚拟", test_document_moves_into_folder_icon_stays_virtual)
    run("进文件夹同资源管理器规则(Ctrl/跨盘复制)", test_explorer_like_folder_drop_modifiers)
    run("空目标命名空间快捷方式不映射到cwd", test_empty_namespace_lnk_not_cwd)
    run("跨盘桌面替身才映射到真实路径", test_cross_drive_desktop_twin_remap)
    run("drop 钉选延后到 OLE 外", test_drop_apply_is_deferred)
    run("force_pin + FenceWidget 可见", test_import_virtual_drop_pins)
    run("拖出分区不因本进程 FG 停放", test_drag_out_must_not_park_on_own_fg)
    run("点击微抖不误判移出分区", test_click_jitter_must_not_unpin)
    run("PreferCopy 桌面偷拖改为移出", test_prefercopy_desktop_steal_becomes_unpin)
    run("文档钉选不做桌面 COM 扫描", test_canonicalize_skips_office_docs)
    run("分区展示不做逐钉 canonicalize", test_fence_display_skips_canonicalize)
    run("热路径无 resolve/全格列表", test_hot_path_no_resolve_storms)
    run("拖进文件夹不误卸钉图标", test_folder_drop_does_not_unpin_icons)
    run("分区空白处松手应卸钉到公共区", test_fence_chrome_drop_unpins_not_reorder)
    run("钉选匹配仅精确路径", test_path_pinned_exact_only)
    run("settings 原子写入契约", test_settings_atomic_write_contract)
    run("公共拖拽 OLE 后结束/超时取消", test_public_overlay_ends_after_ole_and_timeout_cancels)
    run("find_auto_slot 精确占用集合", test_find_auto_slot_taken_is_exact_set)
    run("移出分区左缘吸附显示不消失", test_unpin_snaps_left_edge_and_forces_show)
    run("连续移出两个浮标须先 reconcile 再盖章", test_sequential_unpin_reconciles_before_sync_sig)
    run("分区与公共区往返互斥成员", test_fence_public_roundtrip_exclusive)
    run("拖拽中刷新不丢掉隐藏文件夹浮标", test_public_drag_refresh_keeps_hidden_folder)
    run("复制/进文件夹/进分区/拖出分区链路", test_drag_copy_folder_fence_chain)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
