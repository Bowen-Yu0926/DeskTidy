"""Regression: fence blank RMB hosts Explorer folder background IContextMenu."""

from __future__ import annotations

import inspect
import sys
import tempfile
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

_pass = 0
_fail = 0


def ok(msg: str) -> None:
    global _pass
    _pass += 1
    print(f"  OK  {msg}")


def fail(msg: str, err: BaseException | None = None) -> None:
    global _fail
    _fail += 1
    print(f"  FAIL  {msg}")
    if err is not None:
        traceback.print_exception(type(err), err, err.__traceback__)


def run(name: str, fn) -> None:
    try:
        fn()
        ok(name)
    except Exception as e:
        fail(name, e)


def test_create_view_object_builds_menu() -> None:
    import pythoncom
    import win32con
    import win32gui
    from win32com.shell import shell, shellcon

    td = Path(tempfile.mkdtemp(prefix="desktidy_bgmenu_"))
    pythoncom.CoInitialize()
    host = 0
    hmenu = 0
    try:
        from PyQt6.QtWidgets import QApplication, QWidget

        app = QApplication.instance() or QApplication([])
        w = QWidget()
        w.resize(40, 40)
        w.show()
        app.processEvents()
        host = int(w.winId())
        desktop = shell.SHGetDesktopFolder()
        _e, pidl, _a = desktop.ParseDisplayName(host, None, str(td))
        folder = desktop.BindToObject(pidl, None, shell.IID_IShellFolder)
        cm = folder.CreateViewObject(host, shell.IID_IContextMenu)
        hmenu = win32gui.CreatePopupMenu()
        cm.QueryContextMenu(
            hmenu,
            0,
            100,
            0x7FFF,
            shellcon.CMF_NORMAL | shellcon.CMF_EXPLORE,
        )
        count = win32gui.GetMenuItemCount(hmenu)
        assert count >= 3, f"expected New/Paste-style verbs, got {count}"
    finally:
        if hmenu:
            win32gui.DestroyMenu(hmenu)
        try:
            import shutil

            shutil.rmtree(td, ignore_errors=True)
        except Exception:
            pass


def test_api_and_fence_wiring() -> None:
    from src import shell_file_menu as sfm
    from src.ui.fence_widget import FenceWidget

    assert callable(sfm.show_folder_background_menu)
    src = inspect.getsource(sfm.show_folder_background_menu)
    assert "CreateViewObject" in src
    assert "on_before_shell_invoke" in src
    host_src = inspect.getsource(sfm._host_shell_context_menu)
    assert "QueryContextMenu" in host_src
    assert "InvokeCommand" in host_src
    assert "on_before_shell_invoke" in host_src
    # Persistent TrackPopupMenu owner — never DestroyWindow fence/borrowed HWND.
    assert "_ensure_menu_host_hwnd" in host_src
    assert "Persistent menu host" in host_src
    assert "DestroyWindow(host)" not in host_src
    assert "user32.DestroyWindow" not in host_src
    assert "CreateWindowExW.restype" in inspect.getsource(sfm)
    assert "_ensure_menu_host_hwnd" in inspect.getsource(sfm)

    menu_src = inspect.getsource(FenceWidget._show_fence_context_menu)
    assert "show_folder_background_menu" in menu_src
    assert "_shell_background_folder" in menu_src
    assert "_fence_shell_extra_commands" in menu_src
    assert "_schedule_pin_new_desktop_children" in menu_src
    assert "on_before_shell_invoke" in menu_src
    assert "_ensure_alive_after_shell_menu" in menu_src
    extra_src = inspect.getsource(FenceWidget._fence_shell_extra_commands)
    assert "ShellMenuCommand" in extra_src
    assert "解散分区" in extra_src
    assert "dissolve_requested.emit" in extra_src
    assert "is_locked_fence" in extra_src
    assert "图标放大" not in menu_src
    assert "刷新分区" not in menu_src

    ensure = inspect.getsource(FenceWidget._ensure_alive_after_shell_menu)
    assert "overlay_win32_visible" in ensure
    assert "set_overlay_hwnd_visible" in ensure
    assert "_heal_overlays_after_shell_menu" in ensure

    create = inspect.getsource(sfm._create_menu_host_hwnd)
    assert "WS_EX_TOOLWINDOW" in create
    assert "DeskTidyShellMenuHost" in create

    bg = inspect.getsource(FenceWidget._shell_background_folder)
    assert "get_desktop_path" in bg
    assert "portal" in bg.lower() or "_target_folder" in bg

    unpin = inspect.getsource(FenceWidget._unpin_virtual_paths_impl)
    assert "_is_portal_mode" in unpin


def test_menu_host_does_not_destroy_borrowed_hwnd() -> None:
    """Regression: shell menu must not DestroyWindow a FenceWidget HWND."""
    import win32gui
    from PyQt6.QtWidgets import QApplication, QWidget

    from src.shell_file_menu import _create_menu_host_hwnd

    app = QApplication.instance() or QApplication([])
    victim = QWidget()
    victim.resize(80, 60)
    victim.show()
    app.processEvents()
    victim_hwnd = int(victim.winId())
    assert win32gui.IsWindow(victim_hwnd)

    owned = _create_menu_host_hwnd()
    assert owned, "owned menu host must be creatable"
    assert owned != victim_hwnd
    assert win32gui.IsWindow(owned)
    win32gui.DestroyWindow(owned)
    # Victim must still be alive after destroying the owned host.
    assert win32gui.IsWindow(victim_hwnd)
    victim.close()
    victim.deleteLater()
    app.processEvents()


def test_menu_host_toolwindow_and_chrome_predicate() -> None:
    """First RMB SetForegroundWindow(host) must count as desktop chrome FG."""
    import win32gui
    from PyQt6.QtWidgets import QApplication

    from src.desktop_foreground_monitor import (
        _hwnd_is_own_desktop_chrome,
        _hwnd_is_shell_menu_surface,
        _on_desktop_surface,
    )
    from src.shell_file_menu import _create_menu_host_hwnd
    from src.win_shell import _is_own_desktop_chrome

    app = QApplication.instance() or QApplication([])
    _ = app
    host = _create_menu_host_hwnd()
    assert host
    assert win32gui.IsWindow(host)
    assert win32gui.GetClassName(host) == "DeskTidyShellMenuHost"
    GWL_EXSTYLE = -20
    WS_EX_TOOLWINDOW = 0x00000080
    ex = int(win32gui.GetWindowLong(host, GWL_EXSTYLE))
    assert ex & WS_EX_TOOLWINDOW, "menu host must be TOOLWINDOW chrome"
    assert _is_own_desktop_chrome(host)
    assert _hwnd_is_own_desktop_chrome(host)
    assert _hwnd_is_shell_menu_surface(host)
    assert _on_desktop_surface(host)
    win32gui.DestroyWindow(host)


def test_portal_paste_branch() -> None:
    src = inspect.getsource(
        __import__("src.ui.fence_icon_item", fromlist=["paste_files_into_fence"]).paste_files_into_fence
    )
    assert "is_portal_fence" in src
    assert "get_portal_path" in src

    del_src = inspect.getsource(
        __import__(
            "src.ui.fence_icon_item", fromlist=["delete_selected_fence_items"]
        ).delete_selected_fence_items
    )
    assert "is_portal_fence" in del_src
    assert "portal" in del_src.lower()


def main() -> int:
    print("DeskTidy fence background shell menu")
    run("CreateViewObject builds background menu", test_create_view_object_builds_menu)
    run("API + FenceWidget wiring", test_api_and_fence_wiring)
    run("menu host does not destroy borrowed HWND", test_menu_host_does_not_destroy_borrowed_hwnd)
    run("menu host TOOLWINDOW + chrome FG", test_menu_host_toolwindow_and_chrome_predicate)
    run("portal paste branch", test_portal_paste_branch)
    print()
    print(f"通过 {_pass}  失败 {_fail}")
    return 0 if _fail == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
