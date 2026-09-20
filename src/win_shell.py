"""Windows shell integration: desktop icons, file operations."""

from __future__ import annotations

import ctypes
import os
import shutil
import subprocess
import sys
import time
from ctypes import wintypes
from pathlib import Path

import win32gui

user32 = ctypes.windll.user32
shell32 = ctypes.windll.shell32

SW_HIDE = 0
SW_SHOW = 5
WM_COMMAND = 0x0111
WM_CONTEXTMENU = 0x007B
TOGGLE_DESKTOP_ICONS = 0x7402

GWL_EXSTYLE = -20
WS_EX_APPWINDOW = 0x00040000
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_LAYERED = 0x00080000


def raise_pet_above_fences_in_band() -> None:
    """Raise the desktop pet above fence HWNDs in the DefView-owned band.

    Fences stay interactive (opaque, not sunk under wallpaper); the pet sprite
    paints on top of fence icons. Shaped ``QRegion`` mask on the pet keeps OLE
    folder→fence working outside the character silhouette.
    """
    from PyQt6.QtWidgets import QApplication

    from src.desktop_shell_host import raise_overlay_in_desktop_band

    app = QApplication.instance()
    if app is None:
        return
    desk = getattr(app, "_desktidy_app", None)
    pet = getattr(desk, "pet_widget", None) if desk is not None else None
    if pet is None:
        return
    try:
        if not pet.isVisible():
            return
        hwnd = int(pet.winId())
    except Exception:
        return
    if not hwnd:
        return
    try:
        raise_overlay_in_desktop_band(hwnd)
    except Exception:
        return


def raise_fences_above_pet_in_band() -> None:
    """Restack visible fences above the pet in the DefView-owned band.

    Legacy helper kept for drag/OLE regression tests. Normal pet show uses
    ``raise_pet_above_fences_in_band`` so the sprite stays above fence icons.
    """
    from PyQt6.QtWidgets import QApplication

    from src.desktop_shell_host import raise_overlay_in_desktop_band

    app = QApplication.instance()
    if app is None:
        return
    for top in app.topLevelWidgets():
        if top.__class__.__name__ != "FenceWidget":
            continue
        try:
            if not top.isVisible():
                continue
            if bool(getattr(top, "_desktidy_soft_parked", False)):
                continue
            hwnd = int(top.winId())
        except Exception:
            continue
        if not hwnd:
            continue
        try:
            raise_overlay_in_desktop_band(hwnd)
        except Exception:
            continue


def set_overlay_mouse_passthrough(widget, passthrough: bool) -> None:
    """Toggle click-through for a DefView overlay (Qt attr + WS_EX_TRANSPARENT).

    Soft-park used ``WA_TransparentForMouseEvents`` alone. Clearing that Qt
    attribute does not reliably restore hit-testing on a live layered HWND, so
    arriving fences stayed unclickable after the first page switch until a later
    restyle / keepalive. Pair the attribute with the native exstyle bit (market
    pattern for click-through overlays) and FRAMECHANGED so Win32 updates.
    """
    from PyQt6.QtCore import Qt

    want = bool(passthrough)
    try:
        widget.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, want)
    except RuntimeError:
        return
    try:
        hwnd = int(widget.winId()) if widget.winId() else 0
    except Exception:
        return
    if not hwnd:
        return
    try:
        ex = int(win32gui.GetWindowLong(hwnd, GWL_EXSTYLE))
        if want:
            desired = ex | WS_EX_TRANSPARENT | WS_EX_LAYERED
        else:
            desired = ex & ~WS_EX_TRANSPARENT
        if desired == ex:
            return
        win32gui.SetWindowLong(hwnd, GWL_EXSTYLE, desired)
        win32gui.SetWindowPos(
            hwnd,
            0,
            0,
            0,
            0,
            0,
            0x0001 | 0x0002 | 0x0004 | 0x0020,  # NOSIZE|NOMOVE|NOZORDER|FRAMECHANGED
        )
    except Exception:
        return

SHCNE_RENAMEITEM = 0x00000001
SHCNE_ASSOCCHANGED = 0x08000000
SHCNE_UPDATEDIR = 0x00001000
SHCNF_PATH = 0x0005
SHCNF_FLUSHNOWAIT = 0x3000
_EXPLORER_ADVANCED = r"Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced"


def _read_hide_icons_registry() -> bool | None:
    """Return True when Explorer Advanced HideIcons is set (icons hidden)."""
    try:
        import winreg

        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, _EXPLORER_ADVANCED) as key:
            val, _ = winreg.QueryValueEx(key, "HideIcons")
            return bool(int(val))
    except OSError:
        return None


def _write_hide_icons_registry(hidden: bool) -> None:
    try:
        import winreg

        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            _EXPLORER_ADVANCED,
            0,
            winreg.KEY_SET_VALUE,
        ) as key:
            winreg.SetValueEx(key, "HideIcons", 0, winreg.REG_DWORD, 1 if hidden else 0)
    except OSError:
        pass


def _win32_activate_hwnd(hwnd: int, *, show: bool = False) -> None:
    """Steal foreground without the TOPMOST→NOTOPMOST pulse (tray re-open flash)."""
    SWP_NOSIZE = 0x0001
    SWP_NOMOVE = 0x0002
    SWP_SHOWWINDOW = 0x0040
    flags = SWP_NOMOVE | SWP_NOSIZE
    if show:
        flags |= SWP_SHOWWINDOW
    try:
        foreground = user32.GetForegroundWindow()
        current_tid = ctypes.windll.kernel32.GetCurrentThreadId()
        foreground_tid = 0
        if foreground:
            foreground_tid = user32.GetWindowThreadProcessId(foreground, None)
        attached = False
        if foreground_tid and foreground_tid != current_tid:
            attached = bool(user32.AttachThreadInput(foreground_tid, current_tid, True))
        try:
            if show:
                user32.SetWindowPos(hwnd, 0, 0, 0, 0, 0, flags)
            user32.BringWindowToTop(hwnd)
            user32.SetForegroundWindow(hwnd)
            user32.SetActiveWindow(hwnd)
        finally:
            if attached:
                user32.AttachThreadInput(foreground_tid, current_tid, False)
    except OSError:
        return


def bring_widget_to_foreground(widget, *, was_mapped: bool | None = None) -> None:
    """Show a Qt window and force it above other windows on Windows.

    Plain show()/activateWindow() often leaves the main window behind until the
    user clicks the taskbar (focus-stealing prevention / overlay Z-order).
    Preserves maximized state — ``showNormal()`` must only clear minimized.

    When *was_mapped* is True (window already visible), skip the TOPMOST pulse —
    that brief topmost flip is what makes tray clicks flash the screen.
    """
    from PyQt6.QtCore import Qt

    if was_mapped is None:
        try:
            was_mapped = bool(widget.isVisible()) and not bool(widget.isMinimized())
        except Exception:
            was_mapped = False

    try:
        was_maximized = bool(widget.isMaximized())
    except Exception:
        was_maximized = False
    try:
        state = widget.windowState()
        widget.setWindowState(state & ~Qt.WindowState.WindowMinimized)
    except Exception:
        pass
    widget.show()
    try:
        if was_maximized:
            widget.showMaximized()
        elif bool(widget.isMinimized()):
            widget.showNormal()
    except Exception:
        pass
    widget.raise_()
    widget.activateWindow()
    try:
        widget.setFocus(Qt.FocusReason.ActiveWindowFocusReason)
    except Exception:
        pass

    try:
        hwnd = int(widget.winId())
    except Exception:
        return
    if not hwnd:
        return

    if was_mapped:
        _win32_activate_hwnd(hwnd)
        return

    SWP_NOSIZE = 0x0001
    SWP_NOMOVE = 0x0002
    SWP_SHOWWINDOW = 0x0040
    HWND_TOPMOST = -1
    HWND_NOTOPMOST = -2
    try:
        foreground = user32.GetForegroundWindow()
        current_tid = ctypes.windll.kernel32.GetCurrentThreadId()
        foreground_tid = 0
        if foreground:
            foreground_tid = user32.GetWindowThreadProcessId(foreground, None)
        attached = False
        if foreground_tid and foreground_tid != current_tid:
            attached = bool(user32.AttachThreadInput(foreground_tid, current_tid, True))
        try:
            user32.SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW,
            )
            user32.BringWindowToTop(hwnd)
            user32.SetForegroundWindow(hwnd)
            user32.SetActiveWindow(hwnd)
            user32.SetWindowPos(
                hwnd,
                HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW,
            )
        finally:
            if attached:
                user32.AttachThreadInput(foreground_tid, current_tid, False)
    except OSError:
        return


def configure_desktop_overlay(
    widget,
    *,
    peek: bool = False,
    force_band: bool = False,
    raise_band: bool | None = None,
) -> None:
    """Hide overlay from taskbar and attach it to the Explorer desktop shell.

    Market approach (Fences / Rainmeter On Desktop): own the overlay by the HWND
    that hosts SHELLDLL_DefView so Win+D shows overlays with the desktop, while
    they stay under normal apps. Never bind to the wallpaper-only WorkerW.

    force_band: re-sink with HWND_BOTTOM (floats/fences under sibling chrome).
    raise_band: None = honor ``_desktidy_raise_band`` on the widget; True/False
    force HWND_TOP on/off (False must win — foreign-FG must not lift chrome).
    Prefer raise_band for right-edge chrome after hotkey page switches — sinking
    can bury the bar under DefView so page buttons disappear.
    """
    from src.desktop_shell_host import (
        detach_overlay_from_desktop,
        ensure_overlay_on_desktop,
        is_attached_to_desktop,
        is_stuck_under_wallpaper,
        place_overlay_in_desktop_band,
        raise_overlay_in_desktop_band,
    )

    try:
        hwnd = int(widget.winId())
    except Exception:
        return
    if not hwnd:
        return
    # Offscreen / unrealized Qt can yield a non-zero fake winId (often 1).
    # SetWindowPos on it raises pywintypes.error — not OSError — and an uncaught
    # escape from showEvent aborts the process (0xC0000409) on this PyQt build.
    try:
        if not user32.IsWindow(hwnd):
            return
    except Exception:
        return
    try:
        geo = None
        try:
            geo = widget.geometry()
        except Exception:
            pass
        ex_style = win32gui.GetWindowLong(hwnd, GWL_EXSTYLE)
        desired = (ex_style | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW
        # Keepalive / FG recover call this for every overlay — skip FRAMECHANGED
        # when styles already match (SetWindowPos still flashes under load).
        if ex_style != desired:
            win32gui.SetWindowLong(hwnd, GWL_EXSTYLE, desired)
            win32gui.SetWindowPos(
                hwnd,
                0,
                0,
                0,
                0,
                0,
                0x0001 | 0x0002 | 0x0004 | 0x0020,  # NOSIZE|NOMOVE|NOZORDER|FRAMECHANGED
            )
        if peek:
            detach_overlay_from_desktop(hwnd)
        elif is_stuck_under_wallpaper(hwnd):
            ensure_overlay_on_desktop(hwnd)
        elif is_attached_to_desktop(hwnd) and user32.IsWindowVisible(hwnd):
            # Already healthy — avoid ensure's place thrash unless caller needs
            # a deliberate band re-order (page chrome after hotkey switch).
            pass
        else:
            ensure_overlay_on_desktop(hwnd)
        # Page bar / dock: HWND_TOP in the desktop band (with NOOWNERZORDER).
        # Explicit False must win — foreign-FG chrome raise used to lift fences
        # over VS Code / WeChat. None keeps the widget ``_desktidy_raise_band``.
        if raise_band is None:
            want_raise = bool(getattr(widget, "_desktidy_raise_band", False))
        else:
            want_raise = bool(raise_band)
        if want_raise:
            raise_overlay_in_desktop_band(hwnd)
        elif force_band:
            place_overlay_in_desktop_band(hwnd, force=True)
        if geo is not None:
            try:
                if widget.geometry() != geo:
                    widget.setGeometry(geo)
            except Exception:
                pass
    except Exception:
        # pywintypes.error is not an OSError — must not escape into Qt showEvent.
        return


def overlay_win32_visible(widget) -> bool:
    """True when the native HWND is actually shown (Qt isVisible can lie after Win+D)."""
    try:
        hwnd = int(widget.winId())
    except Exception:
        return bool(widget.isVisible())
    if not hwnd:
        return False
    if not user32.IsWindowVisible(hwnd):
        return False
    try:
        if user32.IsIconic(hwnd):
            return False
    except OSError:
        pass
    return True


def is_desktop_foreground() -> bool:
    """True when Explorer desktop *or* DeskTidy desktop chrome owns FG.

    Used to decide sink-vs-repair: foreign apps → desktop-band sink (overlays
    stay mapped underneath); Explorer FG → shell repair. Own *app* windows
    (settings / notepad) use ``_desk_app_ui_open`` instead.

    Shell context menus (#32768 / XAML popup / DeskTidyShellMenuHost) count as
    desktop-side so the first RMB cannot latch a foreign-FG sink while fences
    are still Win32-hidden after TrackPopupMenu.

    Taskbar / notification area also counts as desktop-side — clicking the
    tray must not HWND_BOTTOM partitions under the wallpaper.
    """
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return False
    if _is_own_desktop_chrome(hwnd):
        return True
    name = _window_class_name(int(hwnd))
    if name in _SHELL_MENU_FG_CLASSES:
        return True
    if _hwnd_is_shell_taskbar(int(hwnd)):
        return True
    try:
        root = int(win32gui.GetAncestor(int(hwnd), 2) or 0)
    except OSError:
        root = 0
    if root and _window_class_name(root) in _SHELL_MENU_FG_CLASSES:
        return True
    return is_explorer_desktop_foreground()


def is_explorer_desktop_foreground() -> bool:
    """True only when Explorer desktop (Progman/WorkerW) owns focus — not our chrome."""
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return False
    if _is_own_tool_window(hwnd):
        return False
    desktop_classes = {
        "Progman",
        "WorkerW",
        "SHELLDLL_DefView",
        "SysListView32",
    }
    cur = hwnd
    while cur:
        try:
            class_name = win32gui.GetClassName(cur)
        except OSError:
            break
        if class_name in desktop_classes:
            return True
        cur = win32gui.GetParent(cur)
    try:
        root = win32gui.GetAncestor(hwnd, 2)
        if root and win32gui.GetClassName(root) in desktop_classes:
            return True
    except OSError:
        pass
    return False


# Foreground-only: slightly broader (Win11 modern menu hosts).
_SHELL_MENU_FG_CLASSES = frozenset(
    {
        "#32768",  # classic TrackPopupMenu
        "Xaml_WindowedPopupClass",
        "XamlExplorerHostIslandWindow",
        "Microsoft.UI.Content.PopupWindowSiteBridge",
        "DeskTidyShellMenuHost",
    }
)
# Taskbar / notification area — shell chrome, not a foreign app.
_SHELL_TASKBAR_FG_CLASSES = frozenset(
    {
        "Shell_TrayWnd",
        "Shell_SecondaryTrayWnd",
        "NotifyIconOverflowWindow",
        "TopLevelWindowForOverflowXamlIsland",
    }
)
# EnumWindows: unambiguous menu classes only. Include Win11 desktop popup
# bridge (FG often stays on another app). Do NOT add Windows.UI.Core.CoreWindow
# — Input Experience keeps it visible and used to pin is_shell_context_menu_open.
_SHELL_MENU_ENUM_CLASSES = frozenset(
    {
        "#32768",
        "Xaml_WindowedPopupClass",
        "Microsoft.UI.Content.PopupWindowSiteBridge",
        "XamlExplorerHostIslandWindow",
    }
)
# Back-compat alias used by tests / callers.
_SHELL_MENU_CLASSES = _SHELL_MENU_FG_CLASSES


def _window_class_name(hwnd: int) -> str:
    if not hwnd:
        return ""
    try:
        return win32gui.GetClassName(int(hwnd)) or ""
    except OSError:
        return ""


def _hwnd_is_shell_taskbar(hwnd: int) -> bool:
    """True when FG is the taskbar / notification area (or a child of it)."""
    cur = int(hwnd or 0)
    hops = 0
    while cur and hops < 6:
        if _window_class_name(cur) in _SHELL_TASKBAR_FG_CLASSES:
            return True
        try:
            cur = int(win32gui.GetParent(cur) or 0)
        except OSError:
            break
        hops += 1
    try:
        root = int(win32gui.GetAncestor(int(hwnd or 0), 2) or 0)
    except OSError:
        root = 0
    return bool(root) and _window_class_name(root) in _SHELL_TASKBAR_FG_CLASSES


def is_shell_context_menu_open(*, allow_enum: bool = True) -> bool:
    """True when Explorer / Win11 desktop context menu appears to be showing.

    Covers modern XAML menu and classic「显示更多选项」(#32768). A brief gap
    between the two is handled by dismiss hysteresis in the app, not here.

    ``allow_enum=False`` skips the expensive EnumWindows fallback — use during
    high-frequency dismiss polling when many apps are open.
    """
    fg = int(user32.GetForegroundWindow() or 0)
    if fg:
        name = _window_class_name(fg)
        if name in _SHELL_MENU_FG_CLASSES:
            cache = getattr(is_shell_context_menu_open, "_cache", None)
            if not isinstance(cache, dict):
                cache = {"t": 0.0, "v": False}
                is_shell_context_menu_open._cache = cache  # type: ignore[attr-defined]
            cache["t"] = time.monotonic()
            cache["v"] = True
            return True
        try:
            root = int(win32gui.GetAncestor(fg, 2) or 0)
        except OSError:
            root = 0
        if root and _window_class_name(root) in _SHELL_MENU_FG_CLASSES:
            cache = getattr(is_shell_context_menu_open, "_cache", None)
            if not isinstance(cache, dict):
                cache = {"t": 0.0, "v": False}
                is_shell_context_menu_open._cache = cache  # type: ignore[attr-defined]
            cache["t"] = time.monotonic()
            cache["v"] = True
            return True

    # Full EnumWindows is expensive with many apps; throttle to ~250ms.
    now = time.monotonic()
    cache = getattr(is_shell_context_menu_open, "_cache", None)
    if not isinstance(cache, dict):
        cache = {"t": 0.0, "v": False}
        is_shell_context_menu_open._cache = cache  # type: ignore[attr-defined]
    if now - float(cache["t"]) < 0.25:
        return bool(cache["v"])

    if not allow_enum:
        # High-frequency dismiss poll: FG is not a menu class ⇒ treat as closed.
        # (EnumWindows is for rare cases where the menu exists but is not FG.)
        cache["t"] = now
        cache["v"] = False
        return False

    found = False

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def _enum(hwnd, _lparam):
        nonlocal found
        if found:
            return False
        try:
            if not user32.IsWindowVisible(hwnd):
                return True
            name = _window_class_name(int(hwnd))
            if name in _SHELL_MENU_ENUM_CLASSES:
                found = True
                return False
        except OSError:
            pass
        return True

    try:
        user32.EnumWindows(_enum, 0)
    except OSError:
        pass
    cache["t"] = now
    cache["v"] = found
    return found


def apply_overlay_stack_mode(
    widget,
    *,
    desktop_layer: bool = True,
    peek: bool = False,
) -> None:
    """Attach overlay to the desktop shell, or detach+TOPMOST for peek.

    `desktop_layer` is kept for call-site compatibility; shell ownership is the
    normal mode (Win+D visibility comes from the DefView host, not Z-order).
    """
    _ = desktop_layer
    from PyQt6.QtCore import Qt

    from src.desktop_shell_host import detach_overlay_from_desktop

    if widget is None:
        return
    if not peek:
        try:
            from src.desktop_shell_host import is_attached_to_desktop

            hwnd = int(widget.winId())
            if hwnd and is_attached_to_desktop(hwnd) and user32.IsWindowVisible(hwnd):
                ex_style = win32gui.GetWindowLong(hwnd, GWL_EXSTYLE)
                desired = (ex_style | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW
                current = widget.windowFlags()
                unwanted = Qt.WindowType.WindowStaysOnBottomHint
                top_bits = Qt.WindowType.WindowStaysOnTopHint
                flags = Qt.WindowType.FramelessWindowHint | Qt.WindowType.Window
                needs_flag_change = bool(current & unwanted) or (
                    (current & top_bits) != (flags & top_bits)
                )
                if ex_style == desired and not needs_flag_change:
                    return
        except Exception:
            pass
    try:
        geo = widget.geometry()
    except Exception:
        return

    flags = Qt.WindowType.FramelessWindowHint | Qt.WindowType.Window
    if peek:
        flags |= Qt.WindowType.WindowStaysOnTopHint

    try:
        current = widget.windowFlags()
        unwanted = Qt.WindowType.WindowStaysOnBottomHint
        top_bits = Qt.WindowType.WindowStaysOnTopHint
        needs_flag_change = bool(current & unwanted) or (
            (current & top_bits) != (flags & top_bits)
        )
        if needs_flag_change:
            # setWindowFlags recreates HWND — detach first so Explorer does not
            # keep a stale owner pointer.
            try:
                old_hwnd = int(widget.winId())
                if old_hwnd:
                    detach_overlay_from_desktop(old_hwnd)
            except Exception:
                pass
            widget.setWindowFlags(flags)
            widget.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
            widget.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
            widget.setGeometry(geo)
    except Exception:
        pass

    try:
        widget.show()
    except Exception:
        return
    try:
        if widget.geometry() != geo:
            widget.setGeometry(geo)
    except Exception:
        pass

    configure_desktop_overlay(widget, peek=peek)
    try:
        hwnd = int(widget.winId())
    except Exception:
        return
    if not hwnd:
        return

    SW_SHOWNOACTIVATE = 4
    SWP_NOSIZE = 0x0001
    SWP_NOMOVE = 0x0002
    SWP_NOACTIVATE = 0x0010
    HWND_TOPMOST = -1
    try:
        if not user32.IsWindowVisible(hwnd):
            user32.ShowWindow(hwnd, SW_SHOWNOACTIVATE)
    except OSError:
        pass

    if peek:
        try:
            if not user32.SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
            ):
                user32.SetWindowPos(
                    hwnd,
                    0,  # HWND_TOP
                    0,
                    0,
                    0,
                    0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | 0x0040,
                )
        except OSError:
            pass
        return

    # Ownership is handled once inside configure_desktop_overlay above.
    # Calling ensure again here doubled Win32 work on every attach.
    try:
        if widget.geometry() != geo:
            widget.setGeometry(geo)
    except Exception:
        pass


def _find_shell_def_view(*, force: bool = False) -> int:
    """Locate SHELLDLL_DefView under Progman or WorkerW."""
    try:
        from src.desktop_shell_host import _find_defview_host, invalidate_defview_host_cache

        if force:
            invalidate_defview_host_cache()
        _host, defview = _find_defview_host(force=force)
        if defview:
            return int(defview)
    except Exception:
        pass

    progman = win32gui.FindWindow("Progman", None)
    if progman:
        shell_view = win32gui.FindWindowEx(progman, 0, "SHELLDLL_DefView", None)
        if shell_view:
            return shell_view

    worker = 0
    desktop = win32gui.GetDesktopWindow()
    while True:
        worker = win32gui.FindWindowEx(desktop, worker, "WorkerW", None)
        if not worker:
            break
        shell_view = win32gui.FindWindowEx(worker, 0, "SHELLDLL_DefView", None)
        if shell_view:
            return shell_view
    return 0


def _find_desktop_listview(*, force: bool = False) -> int:
    shell_view = _find_shell_def_view(force=force)
    if not shell_view:
        return 0
    return win32gui.FindWindowEx(shell_view, 0, "SysListView32", None)


def request_desktop_background_menu(x: int, y: int) -> bool:
    """Ask Explorer DefView to open the real desktop background menu at screen (x, y).

    When DeskTidy owns empty-plate hit-testing (``hide_shell_icons`` + floats),
    hover ``WM_NCHITTEST`` already resolves to HTCLIENT, so RMB is delivered to
    our host instead of DefView — ``HTTRANSPARENT`` while VK_RBUTTON is down
    never runs. Post ``WM_CONTEXTMENU`` to DefView (same path Explorer uses for
    wallpaper RMB) so 查看 / 排序 / 粘贴 / 个性化 appear — not a thin
    ``CreateViewObject`` substitute.
    """
    defview = _find_shell_def_view()
    if not defview:
        return False
    # Prefer SysListView32 when present; fall back to DefView (icons may be hidden).
    target = _find_desktop_listview() or defview
    # lParam = screen coordinates (signed 16-bit each) — multi-mon safe.
    lx = ctypes.c_ushort(int(x) & 0xFFFF).value
    ly = ctypes.c_short(int(y)).value & 0xFFFF
    lparam = (ly << 16) | lx
    try:
        return bool(user32.PostMessageW(int(target), WM_CONTEXTMENU, int(target), lparam))
    except OSError:
        return False


def are_desktop_icons_visible() -> bool:
    """Return True if the desktop icon ListView is currently shown."""
    hidden = _read_hide_icons_registry()
    if hidden is not None:
        return not hidden
    listview = _find_desktop_listview()
    if not listview:
        return True
    return bool(user32.IsWindowVisible(listview))


def set_desktop_icons_visible(visible: bool, *, force: bool = False) -> None:
    """Show or hide Windows desktop icons via Explorer toggle.

    ``force=True`` re-queries DefView / ListView and *fixes* mismatch — it must
    NOT blindly toggle (a second force-show would hide icons again on quit).
    """
    want_hidden = not visible
    hidden = _read_hide_icons_registry()
    listview = _find_desktop_listview()
    currently_visible: bool | None = None
    if listview:
        try:
            currently_visible = bool(user32.IsWindowVisible(listview))
        except OSError:
            currently_visible = None

    already_ok = False
    if currently_visible is not None:
        already_ok = currently_visible == visible
    elif hidden is not None:
        already_ok = hidden == want_hidden

    if already_ok and not force:
        return

    if already_ok and force:
        # Desired visibility already — sync registry / refresh only, never toggle.
        if hidden is not None and hidden != want_hidden:
            _write_hide_icons_registry(want_hidden)
        if visible:
            refresh_desktop_namespace_icons()
        return

    shell_view = _find_shell_def_view(force=force)
    if shell_view:
        win32gui.SendMessage(shell_view, WM_COMMAND, TOGGLE_DESKTOP_ICONS, 0)

    hidden = _read_hide_icons_registry()
    if hidden is not None and hidden != want_hidden:
        _write_hide_icons_registry(want_hidden)
        refresh_desktop_namespace_icons()
    elif visible and not want_hidden:
        refresh_desktop_namespace_icons()


def ensure_desktop_icons_visible() -> bool:
    """Force Explorer desktop icons on (quit / uninstall / desktop guard).

    Safe to call repeatedly: never toggles icons off once they are visible.
    """
    set_desktop_icons_visible(True, force=True)
    if not are_desktop_icons_visible():
        # First pass missed (DefView race during overlay teardown) — try again.
        set_desktop_icons_visible(True, force=True)
    if _read_hide_icons_registry():
        _write_hide_icons_registry(False)
        refresh_desktop_namespace_icons()
        if not are_desktop_icons_visible():
            set_desktop_icons_visible(True, force=True)
    return are_desktop_icons_visible()


def toggle_desktop_icons() -> bool:
    """Toggle desktop icon visibility. Returns new visibility state."""
    visible = not are_desktop_icons_visible()
    set_desktop_icons_visible(visible)
    return visible


def open_path(path: Path) -> None:
    target = Path(path)
    # deskNote.lnk: open notepad in this process when main UI is running.
    # ShellExecute would spawn python(w).exe --notepad (console flash / IPC race).
    try:
        from src.desknote import try_open_desknote_in_running_app

        if try_open_desknote_in_running_app(target):
            return
    except Exception:
        pass
    try:
        if target.is_dir() or target.suffix.lower() == ".lnk":
            target = resolve_folder_drop_target(target)
    except OSError:
        pass
    os.startfile(str(target))


def open_path_with_picker(path: Path | str) -> None:
    """Show the Windows「打开方式」dialog (Explorer OpenAs) without blocking UI.

    Market path: ShellExecute ``openas`` (same as Explorer). Falls back to
    ``rundll32 shell32,OpenAs_RunDLL`` via detached Popen — never a blocking
    wait on the dialog (that froze DeskTidy until the picker closed).
    """
    target = Path(path)
    try:
        if not target.exists():
            return
    except OSError:
        return
    target_s = str(target)
    try:
        # ShellExecute returns immediately; OpenAs owns the dialog.
        rc = int(ctypes.windll.shell32.ShellExecuteW(None, "openas", target_s, None, None, 1))
        # Per MSDN, values > 32 mean success.
        if rc > 32:
            return
    except Exception:
        pass
    try:
        creationflags = 0
        if sys.platform == "win32":
            creationflags = int(getattr(subprocess, "DETACHED_PROCESS", 0)) | int(
                getattr(subprocess, "CREATE_NO_WINDOW", 0)
            )
        subprocess.Popen(  # noqa: S603
            ["rundll32", "shell32.dll,OpenAs_RunDLL", target_s],
            close_fds=True,
            creationflags=creationflags,
        )
    except OSError:
        return


def reveal_in_explorer(path: Path) -> None:
    subprocess.run(["explorer", "/select,", str(path)], check=False)


def open_containing_folder(path: Path) -> None:
    folder = path if path.is_dir() else path.parent
    try:
        folder = resolve_folder_drop_target(folder)
    except OSError:
        pass
    os.startfile(str(folder))


def _lnk_target_dir(path: Path) -> Path | None:
    if path.suffix.lower() != ".lnk":
        return None
    try:
        # Reuse fence_rules mtime cache — CreateShortCut on every folder open
        # / twin lookup was a UI hitch with many desktop shortcuts.
        from src.fence_rules import _lnk_target_file

        target = _lnk_target_file(path)
        if target is not None and target.is_dir():
            return target
    except Exception:
        return None
    return None


def _unique_drive_root_folder(name: str) -> Path | None:
    """Return ``X:\\name`` when exactly one fixed drive has that top-level folder."""
    if not name or name in {".", ".."} or "/" in name or "\\" in name:
        return None
    hits: list[Path] = []
    for code in range(ord("A"), ord("Z") + 1):
        candidate = Path(f"{chr(code)}:\\{name}")
        try:
            if candidate.is_dir():
                hits.append(candidate)
        except OSError:
            continue
    if len(hits) == 1:
        return hits[0]
    return None


def _desktop_folder_shortcut_twin(name: str) -> Path | None:
    """Desktop folder .lnk whose name matches *name* → off-desktop target."""
    from src.settings import get_desktop_paths

    name_cf = name.casefold()
    for desk in get_desktop_paths():
        try:
            if not desk.is_dir():
                continue
            children = list(desk.iterdir())
        except OSError:
            continue
        for child in children:
            if child.suffix.lower() != ".lnk":
                continue
            stem = child.stem.casefold()
            if not (
                stem == name_cf
                or stem.startswith(f"{name_cf} -")
                or stem.startswith(f"{name_cf}-")
            ):
                continue
            target = _lnk_target_dir(child)
            if target is not None:
                return target
    return None


def resolve_folder_drop_target(folder: Path, *, for_move: bool = False) -> Path:
    """Real folder for opens / pins / document moves.

    - Folder ``.lnk`` → its target (always).
    - Desktop stand-in (e.g. ``D:\\desktop\\AIproject``) → unique ``E:\\soft\\AIproject``
      or a matching desktop folder shortcut target, including nested paths
      (``...\\AIproject\\ppt`` → ``E:\\soft\\AIproject\\ppt``).

    *for_move*: when True (drag-into-folder), do **not** apply the stand-in twin
    remap. Explorer moves into the folder you dropped on; remapping to another
    drive made tiny files use slow ``FO_MOVE``, left the visible desktop folder
    empty, and triggered Shell「重命名」when the twin already held the name.

    Icons (.lnk/.url) never use this for swallow-into-folder; only documents do.
    """
    path = Path(folder)
    lnk_dir = _lnk_target_dir(path)
    if lnk_dir is not None:
        path = lnk_dir

    try:
        resolved = path.resolve()
    except OSError:
        resolved = path

    if for_move:
        return resolved

    from src.settings import get_desktop_paths

    for desk in get_desktop_paths():
        try:
            desk_res = desk.resolve()
        except OSError:
            desk_res = desk
        try:
            rel = resolved.relative_to(desk_res)
        except ValueError:
            continue
        parts = rel.parts
        if not parts:
            return resolved
        top = parts[0]
        twin = _desktop_folder_shortcut_twin(top) or _unique_drive_root_folder(top)
        if twin is None:
            return resolved
        try:
            twin_res = twin.resolve()
        except OSError:
            twin_res = twin
        try:
            twin_res.relative_to(desk_res)
            continue  # twin still under desktop — not a real off-desktop home
        except ValueError:
            pass
        # Same-drive twins (D:\desktop\opencode vs D:\OpenCode) are ambiguous;
        # only remap across drives (D:\desktop\AIproject → E:\soft\AIproject).
        if (twin_res.drive or "").casefold() == (desk_res.drive or "").casefold():
            continue
        if len(parts) == 1:
            return twin_res
        return twin_res.joinpath(*parts[1:])
    return resolved


# Back-compat alias
canonical_folder_drop_target = resolve_folder_drop_target

_EXPLORER_HWND_PATH_CACHE: dict[int, tuple[str, float]] = {}
_EXPLORER_HWND_CACHE_TTL_S = 2.5
_EXPLORER_COM_PROBE_MIN_S = 0.25
_explorer_com_last_probe = 0.0


def _hwnd_is_desktop_surface(hwnd: int) -> bool:
    """True when hwnd is Progman/DefView (not an open Explorer folder window)."""
    cur = int(hwnd or 0)
    for _ in range(12):
        if not cur:
            break
        try:
            cls = win32gui.GetClassName(cur) or ""
        except OSError:
            break
        if cls in {"CabinetWClass", "ExploreWClass"}:
            return False
        if cls in {
            "Progman",
            "WorkerW",
            "SHELLDLL_DefView",
            "SysListView32",
            "DesktopWindowContentBridge",
            "Windows.UI.Composition.DesktopWindowContentBridge",
        }:
            return True
        try:
            cur = int(win32gui.GetParent(cur) or 0)
        except OSError:
            break
    return False


def _explorer_path_from_com(target: int, pt: tuple[int, int], hwnd: int) -> Path | None:
    """Resolve Explorer folder path via Shell.Application (rate-limited + cached)."""
    global _explorer_com_last_probe
    import time

    now = time.monotonic()
    if target:
        cached = _EXPLORER_HWND_PATH_CACHE.get(int(target))
        if cached is not None and now - cached[1] < _EXPLORER_HWND_CACHE_TTL_S:
            try:
                path = Path(cached[0])
                if path.is_dir():
                    return path
            except OSError:
                pass

    if now - _explorer_com_last_probe < _EXPLORER_COM_PROBE_MIN_S:
        return None
    _explorer_com_last_probe = now

    try:
        from win32com.client import Dispatch

        shell_app = Dispatch("Shell.Application")
        if target:
            for window in shell_app.Windows():
                try:
                    if int(window.HWND) != target:
                        continue
                    raw = str(window.Document.Folder.Self.Path)
                    path = Path(raw)
                    if path.is_dir():
                        _EXPLORER_HWND_PATH_CACHE[int(target)] = (raw, now)
                        return path
                except Exception:
                    continue
        # Odd Explorer child HWNDs (no Cabinet seed): match by rect, but never
        # when the hit is DefView/desktop — that false-matches folder windows
        # sitting behind the desktop point.
        if (
            not target
            and hwnd
            and not _is_own_tool_window(hwnd)
            and not _hwnd_is_desktop_surface(hwnd)
        ):
            for window in shell_app.Windows():
                try:
                    wh = int(window.HWND)
                    left, top, right, bottom = win32gui.GetWindowRect(wh)
                    if not (left <= pt[0] < right and top <= pt[1] < bottom):
                        continue
                    raw = str(window.Document.Folder.Self.Path)
                    path = Path(raw)
                    if path.is_dir():
                        _EXPLORER_HWND_PATH_CACHE[wh] = (raw, now)
                        return path
                except Exception:
                    continue
    except Exception:
        return None
    return None


def _own_hwnd_is_pet_overlay(hwnd: int) -> bool:
    """True when hwnd is the desktop pet (or a child), not a fence/page bar.

    Fences are opaque OLE targets — looking *under* them for Explorer moves
    files into buried folder windows (e.g. ``E:\\``) while the user aimed at
    empty desktop / unpin. The pet is shaped + AcceptDrops off, so skipping
    only the pet still allows「文件夹在宠物身下」drops.
    """
    if not hwnd or not _is_own_tool_window(hwnd):
        return False
    try:
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance()
        if app is None:
            return False
        try:
            root = int(win32gui.GetAncestor(int(hwnd), 2) or hwnd)
        except OSError:
            root = int(hwnd)
        for top in app.topLevelWidgets():
            try:
                if top.__class__.__name__ != "DesktopPetWidget":
                    continue
                if not top.isVisible():
                    continue
                ph = int(top.winId()) if top.winId() else 0
            except Exception:
                continue
            if not ph:
                continue
            if ph in (int(hwnd), root):
                return True
    except Exception:
        return False
    return False


def explorer_folder_path_at(x: int, y: int) -> Path | None:
    """Return the filesystem path of an Explorer folder window under (x, y).

    Used by public/virtual (non-OLE) drags so dropping onto an open folder
    window still moves the file. Drag ghosts are permanently
    ``WS_EX_TRANSPARENT``, so ``WindowFromPoint`` already skips them — no
    mid-drag style thrash.

    Only the **pet** may be skipped to reveal an Explorer folder under the
    sprite. Opaque fences / page chrome must not see through — EnumWindows
    would otherwise hit a CabinetWClass whose rect contains the point even
    when that window is fully covered by the fence (files vanish into e.g. E:\\).
    """
    pt = (int(x), int(y))
    raw = int(win32gui.WindowFromPoint(pt) or 0)
    if raw and _own_hwnd_is_desktop_overlay(raw):
        if not _own_hwnd_is_pet_overlay(raw):
            return None
        hwnd = int(_hwnd_at_point_skip_desktop_chrome(pt[0], pt[1]) or 0)
    else:
        hwnd = raw

    target = 0
    cur = int(hwnd or 0)
    for _ in range(10):
        if not cur:
            break
        try:
            cls = win32gui.GetClassName(cur) or ""
        except OSError:
            break
        if cls in {"CabinetWClass", "ExploreWClass"}:
            target = cur
            break
        try:
            cur = int(win32gui.GetParent(cur) or 0)
        except OSError:
            break

    return _explorer_path_from_com(target, pt, hwnd)


_OWN_PID = os.getpid()


def _is_own_tool_window(hwnd: int) -> bool:
    """True if hwnd belongs to this process (any DeskTidy window)."""
    try:
        import win32process

        _, pid = win32process.GetWindowThreadProcessId(hwnd)
        return pid == _OWN_PID
    except OSError:
        return False


def _is_own_desktop_chrome(hwnd: int) -> bool:
    """True for DefView overlays (fence / page bar / float), not app MainWindows.

    Settings and notepad are normal APPWINDOW tops — they must *not* count as
    desktop FG, or overlays stay raised and cover those windows.

    ``DeskTidyShellMenuHost`` is chrome even before WS_EX_TOOLWINDOW is applied
    (first RMB used to SetForegroundWindow a non-TOOLWINDOW host → foreign FG).
    """
    if not hwnd or not _is_own_tool_window(hwnd):
        return False
    try:
        if win32gui.GetClassName(int(hwnd)) == "DeskTidyShellMenuHost":
            return True
        ex = int(win32gui.GetWindowLong(int(hwnd), GWL_EXSTYLE))
    except OSError:
        return False
    if ex & WS_EX_APPWINDOW:
        return False
    return bool(ex & WS_EX_TOOLWINDOW)


def is_desktop_point(x: int, y: int) -> bool:
    """Check if screen coordinates are over the desktop background (not another app)."""
    hwnd = win32gui.WindowFromPoint((x, y))
    if not hwnd:
        return False

    # Clicks on our fences/overlays are not "empty desktop".
    if _is_own_tool_window(hwnd):
        return False

    desktop_classes = {
        "Progman",
        "WorkerW",
        "SHELLDLL_DefView",
        "SysListView32",
        # Win11 / multi-desktop edge cases
        "DesktopWindowContentBridge",
        "Windows.UI.Composition.DesktopWindowContentBridge",
    }
    cur = hwnd
    while cur:
        try:
            class_name = win32gui.GetClassName(cur)
        except OSError:
            break
        if class_name in desktop_classes:
            return True
        cur = win32gui.GetParent(cur)

    # Fallback: some multi-monitor / wallpaper setups report atypical parents.
    try:
        root = win32gui.GetAncestor(hwnd, 2)  # GA_ROOT
        if root and win32gui.GetClassName(root) in desktop_classes:
            return True
    except OSError:
        pass
    return False


def _hwnd_at_point_skip_desktop_chrome(x: int, y: int) -> int:
    """Topmost HWND at (x,y), skipping DeskTidy DefView chrome (pet/floats/fences).

    ``WindowFromPoint`` stops on our TOOLWINDOW overlays (or their Qt children).
    For folder-drop and external-app checks we need the shell / foreign window
    underneath — EnumWindows is top-to-bottom Z-order among top-level HWNDs.
    """
    pt = (int(x), int(y))
    hwnd = int(win32gui.WindowFromPoint(pt) or 0)
    if not hwnd:
        return 0
    if not _own_hwnd_is_desktop_overlay(hwnd):
        return hwnd

    found: list[int] = []

    def _enum(wh: int, _ctx) -> bool:
        try:
            wh = int(wh)
            if not wh or not user32.IsWindowVisible(wh):
                return True
            if _own_hwnd_is_desktop_overlay(wh):
                return True
            try:
                ex = int(win32gui.GetWindowLong(wh, GWL_EXSTYLE))
            except OSError:
                ex = 0
            if ex & WS_EX_TRANSPARENT:
                return True
            left, top, right, bottom = win32gui.GetWindowRect(wh)
            if not (left <= pt[0] < right and top <= pt[1] < bottom):
                return True
            found.append(wh)
            return False
        except OSError:
            return True

    try:
        win32gui.EnumWindows(_enum, None)
    except OSError:
        return 0
    return int(found[0]) if found else 0


def _own_hwnd_is_desktop_overlay(hwnd: int) -> bool:
    """True when hwnd is DeskTidy DefView chrome or a child of it (not settings)."""
    if not hwnd or not _is_own_tool_window(hwnd):
        return False
    if _is_own_desktop_chrome(hwnd):
        return True
    try:
        root = int(win32gui.GetAncestor(int(hwnd), 2) or hwnd)
    except OSError:
        return False
    return bool(root and _is_own_desktop_chrome(root))


def _hwnd_under_drag_point(x: int, y: int) -> int:
    """HWND under cursor for OLE handoff / external release.

    Drag ghosts use permanent WS_EX_TRANSPARENT (skipped by WindowFromPoint).
    DeskTidy DefView chrome is skipped so an open folder under the pet still
    counts as Explorer, not as 「松在外部应用」.
    """
    return int(_hwnd_at_point_skip_desktop_chrome(int(x), int(y)) or 0)


def arm_nonactivating_drag_ghost(hwnd: int) -> None:
    """Permanent WS_EX_NOACTIVATE|TRANSPARENT|TOOLWINDOW on the drag ghost HWND.

    Root fix for VPN / tray-utility wake during fence→public drag: the ghost
    must never activate foreign topmost windows, and hit-tests must not thrash
    styles or ShowWindow around it.
    """
    if not hwnd:
        return
    WS_EX_TRANSPARENT = 0x00000020
    WS_EX_TOOLWINDOW = 0x00000080
    WS_EX_LAYERED = 0x00080000
    WS_EX_NOACTIVATE = 0x08000000
    try:
        ex = int(win32gui.GetWindowLong(hwnd, GWL_EXSTYLE))
        win32gui.SetWindowLong(
            hwnd,
            GWL_EXSTYLE,
            ex | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        )
    except OSError:
        pass


_DESKTOP_HIT_CLASSES = frozenset(
    {
        "Progman",
        "WorkerW",
        "SHELLDLL_DefView",
        "SysListView32",
        "DesktopWindowContentBridge",
        "Windows.UI.Composition.DesktopWindowContentBridge",
    }
)


def _is_desktop_or_explorer_hwnd(hwnd: int) -> bool:
    if not hwnd:
        return True
    if _is_own_tool_window(hwnd):
        return True
    cur = int(hwnd)
    for _ in range(12):
        if not cur:
            break
        try:
            cls = win32gui.GetClassName(cur) or ""
        except OSError:
            break
        if cls in _DESKTOP_HIT_CLASSES:
            return True
        if cls in {"CabinetWClass", "ExploreWClass"}:
            return True
        try:
            cur = int(win32gui.GetParent(cur) or 0)
        except OSError:
            break
    try:
        root = int(win32gui.GetAncestor(hwnd, 2) or 0)
        if root and (win32gui.GetClassName(root) or "") in _DESKTOP_HIT_CLASSES:
            return True
    except OSError:
        pass
    return False


def is_external_app_drop_point(x: int, y: int) -> bool:
    """True when (x,y) is over another app (WeChat / browser / Office…), not desktop or us.

    Sees *through* DeskTidy DefView chrome so a public-float release over WeChat
    (under a transparent plate) is not treated as desktop relocate. Mid-drag OLE
    handoff uses the stricter ``should_ole_file_handoff_at`` allowlist so VPN /
    utility panels are not woken as drop targets.

    Do **not** use this to gate fence→public unpin — IDE / Cubism windows behind
    the desktop band look 「external」 after chrome-skip and block every drag-out.
    Use ``is_visible_external_app_drop_point`` for unpin instead.
    """
    hwnd = _hwnd_under_drag_point(x, y)
    if not hwnd:
        return False
    return not _is_desktop_or_explorer_hwnd(hwnd)


def is_visible_external_app_drop_point(x: int, y: int) -> bool:
    """True when the *visible* top HWND is a foreign app (no chrome see-through).

    Fence→public unpin must not skip DefView overlays: after the first float is
    placed, chrome-skip often hits Cursor/Cubism under the band and the next
    unpin is rejected (log: ``skip unpin external app``) while the user still
    sees desktop.
    """
    try:
        hwnd = int(win32gui.WindowFromPoint((int(x), int(y))) or 0)
    except OSError:
        return False
    if not hwnd:
        return False
    return not _is_desktop_or_explorer_hwnd(hwnd)


def is_desknote_window_at(x: int, y: int) -> bool:
    """True when the visible window under (x,y) is DeskNote (frozen or source)."""
    try:
        hwnd = int(win32gui.WindowFromPoint((int(x), int(y))) or 0)
    except OSError:
        return False
    if not hwnd:
        return False
    try:
        root = int(win32gui.GetAncestor(hwnd, 2) or hwnd)
    except OSError:
        root = hwnd
    proc = (_process_name_for_hwnd(root) or _process_name_for_hwnd(hwnd) or "").casefold()
    if proc in {"desknote", "desknote.exe"}:
        return True
    # Source: pythonw + desknote_main — identify by window title.
    try:
        title = (win32gui.GetWindowText(root) or win32gui.GetWindowText(hwnd) or "")
    except OSError:
        title = ""
    t = title.casefold()
    return "desknote" in t or title.startswith("DeskNote")


def try_open_paths_in_desknote_at(x: int, y: int, paths: list[Path | str]) -> bool:
    """If release is over DeskNote, open openable paths there (no OLE / no Move).

    DeskTidy custom drag never starts OLE over DeskNote (not on the handoff
    allowlist). Without this, floats are restored and the file never opens.
    """
    if not is_desknote_window_at(x, y):
        return False
    try:
        from src.desknote_launch import open_in_desknote
        from src.notepad import is_notepad_openable_path
    except Exception:
        return False
    openable: list[Path] = []
    for raw in paths:
        try:
            path = Path(raw)
        except OSError:
            continue
        if is_notepad_openable_path(path):
            openable.append(path)
    if not openable:
        return False
    try:
        return bool(open_in_desknote(openable))
    except Exception:
        return False


# Mid-drag OLE handoff allowlist — only apps users intentionally drop files onto.
# Do NOT gate on WS_EX_ACCEPTFILES or generic Electron classes (Chrome_WidgetWin_1):
# VPN / tunnel / status panels often set AcceptFiles or share Electron chrome, and
# starting QDrag.exec over them wakes those dormant windows as OLE drop targets.
_OLE_HANDOFF_PROCESS_HINTS = frozenset(
    {
        "wechat",
        "weixin",
        "qq",
        "tim",
        "dingtalk",
        "feishu",
        "lark",
        "telegram",
        "discord",
        "slack",
        "teams",
        "outlook",
        "winword",
        "excel",
        "powerpnt",
        "foxmail",
        "chrome",
        "msedge",
        "firefox",
        "sogouexplorer",
        "quark",
        "360se",
        "360chrome",
    }
)
# Narrow, product-specific window classes only (never Chrome_WidgetWin_1 / Cef*).
_OLE_HANDOFF_CLASS_HINTS = frozenset(
    {
        "WeChatMainWndForPC",
        "TXGuiFoundation",  # QQ / TIM
        "ChatWnd",
        "OpusApp",  # Word
        "XLMAIN",  # Excel
        "PPTFrameClass",
        "rctrl_renwnd32",  # Outlook
    }
)

_process_name_cache: dict[int, tuple[float, str]] = {}
_PROCESS_NAME_TTL_S = 2.0


def _process_name_for_hwnd(hwnd: int) -> str:
    try:
        import win32process

        _tid, pid = win32process.GetWindowThreadProcessId(hwnd)
        pid = int(pid)
        if not pid:
            return ""
        now = time.perf_counter()
        cached = _process_name_cache.get(pid)
        if cached is not None and now - cached[0] < _PROCESS_NAME_TTL_S:
            return cached[1]
        handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED
        if not handle:
            return ""
        try:
            buf = ctypes.create_unicode_buffer(260)
            size = wintypes.DWORD(260)
            name = ""
            if ctypes.windll.kernel32.QueryFullProcessImageNameW(
                handle, 0, buf, ctypes.byref(size)
            ):
                name = Path(buf.value).stem.casefold()
            _process_name_cache[pid] = (now, name)
            return name
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    except Exception:
        pass
    return ""


def should_ole_file_handoff_at(x: int, y: int) -> bool:
    """True only when mid-drag OLE to a real file-drop app is appropriate.

    Crossing a dormant VPN / status / utility window while dragging a fence icon
    must NOT start ``QDrag.exec`` — that wakes the window as an OLE drop target.

    Gate on process-name / narrow product class allowlists only. Never treat
    ``WS_EX_ACCEPTFILES`` or generic Electron classes as enough.
    """
    hwnd = _hwnd_under_drag_point(x, y)
    if not hwnd or _is_desktop_or_explorer_hwnd(hwnd):
        return False
    try:
        root = int(win32gui.GetAncestor(hwnd, 2) or hwnd)
    except OSError:
        root = hwnd
    proc = _process_name_for_hwnd(root) or _process_name_for_hwnd(hwnd)
    if proc and (
        proc in _OLE_HANDOFF_PROCESS_HINTS
        or any(proc.startswith(h) for h in _OLE_HANDOFF_PROCESS_HINTS)
    ):
        return True
    try:
        cls = (win32gui.GetClassName(root) or "").strip()
    except OSError:
        cls = ""
    if cls in _OLE_HANDOFF_CLASS_HINTS:
        return True
    try:
        leaf = (win32gui.GetClassName(hwnd) or "").strip()
    except OSError:
        leaf = ""
    if leaf in _OLE_HANDOFF_CLASS_HINTS:
        return True
    return False


_WM_DROPFILES = 0x0233
_VK_CONTROL = 0x11
_VK_V = 0x56
_KEYEVENTF_KEYUP = 0x0002


def _focus_external_window(hwnd: int) -> bool:
    """Bring a foreign HWND to FG (AttachThreadInput for SetForegroundWindow)."""
    try:
        import ctypes

        user32 = ctypes.windll.user32
        target = int(hwnd or 0)
        if not target:
            return False
        fg = int(user32.GetForegroundWindow() or 0)
        if fg == target:
            return True
        tid_target = user32.GetWindowThreadProcessId(target, None)
        tid_fg = user32.GetWindowThreadProcessId(fg, None) if fg else 0
        attached = False
        if tid_fg and tid_target and tid_fg != tid_target:
            attached = bool(user32.AttachThreadInput(tid_fg, tid_target, True))
        try:
            user32.ShowWindow(target, 9)  # SW_RESTORE
            return bool(user32.SetForegroundWindow(target))
        finally:
            if attached:
                user32.AttachThreadInput(tid_fg, tid_target, False)
    except Exception:
        return False


def _send_ctrl_v() -> None:
    """Synthetic Ctrl+V for chat apps after clipboard_set_files."""
    import ctypes

    user32 = ctypes.windll.user32
    user32.keybd_event(_VK_CONTROL, 0, 0, 0)
    user32.keybd_event(_VK_V, 0, 0, 0)
    user32.keybd_event(_VK_V, 0, _KEYEVENTF_KEYUP, 0)
    user32.keybd_event(_VK_CONTROL, 0, _KEYEVENTF_KEYUP, 0)


def deliver_files_to_external_chat(
    x: int, y: int, paths: list[Path | str]
) -> bool:
    """Put files on the clipboard and paste into WeChat / QQ (release handoff)."""
    if not should_ole_file_handoff_at(x, y):
        return False
    try:
        import win32gui

        from src.shell_clipboard import clipboard_set_files

        hwnd = _hwnd_under_drag_point(x, y)
        if not hwnd:
            return False
        root = int(win32gui.GetAncestor(int(hwnd), 2) or int(hwnd))
        if not clipboard_set_files(paths, cut=False):
            return False
        try:
            from PyQt6.QtWidgets import QApplication

            app = QApplication.instance()
            if app is not None:
                app.processEvents()
        except Exception:
            pass
        if not _focus_external_window(root):
            _focus_external_window(int(hwnd))
        _send_ctrl_v()
        return True
    except Exception:
        return False


def deliver_files_to_external_window(
    x: int, y: int, paths: list[Path | str]
) -> bool:
    """Post ``WM_DROPFILES`` when ``QDrag`` returns ``IgnoreAction`` (WeChat fallback)."""
    if not should_ole_file_handoff_at(x, y):
        return False
    hwnd = _hwnd_under_drag_point(x, y)
    if not hwnd:
        return False
    try:
        from src.shell_clipboard import _normalize_paths, global_hdrop_handle

        abs_paths = _normalize_paths(
            [Path(p) if not isinstance(p, Path) else p for p in paths]
        )
        if not abs_paths:
            return False
        hdrop = global_hdrop_handle(abs_paths)
        if not hdrop:
            return False
        import ctypes

        user32 = ctypes.windll.user32
        # PostMessage transfers HDROP ownership to the receiver.
        if not user32.PostMessageW(int(hwnd), _WM_DROPFILES, int(hdrop), 0):
            ctypes.windll.kernel32.GlobalFree(hdrop)
            return False
        return True
    except Exception:
        return False


def delete_to_trash(path: Path) -> None:
    from send2trash import send2trash

    clsid = get_lnk_namespace_clsid(path) if path.suffix.lower() == ".lnk" else None
    send2trash(str(path))
    if clsid:
        return_namespace_icon_to_desktop(clsid)


def is_desktop_loose_item(path: Path, desktop: Path | None = None) -> bool:
    """Return True if path is a direct child of any Windows desktop folder.

    Uses casefolded parent-string compare against cached desktop roots — never
    ``Path.resolve()`` (that stalls the UI on cloud / reparse points and used
    to hitch folder-pin / covering checks).
    """
    from src.settings import get_desktop_paths

    try:
        p = Path(path)
        parent = p.parent if p.is_absolute() else Path(str(p)).absolute().parent
        parent_cf = str(parent).casefold().rstrip("\\/")
    except OSError:
        return False
    if not parent_cf:
        return False

    roots: list[Path] = []
    if desktop is not None:
        roots.append(Path(desktop))
    else:
        roots.extend(get_desktop_paths())
    for desktop_path in roots:
        try:
            desk_cf = str(desktop_path).casefold().rstrip("\\/")
        except OSError:
            continue
        if desk_cf and parent_cf == desk_cf:
            return True
    return False


_desktop_path_cache: Path | None = None


def _get_desktop_path_cached() -> Path:
    global _desktop_path_cache
    if _desktop_path_cache is None:
        from src.settings import get_desktop_path

        _desktop_path_cache = get_desktop_path()
    return _desktop_path_cache


def notify_shell_item_moved(src: Path, dest: Path) -> None:
    """Tell Explorer one item moved — no SHCNE_ASSOCCHANGED (that flashes DefView)."""
    try:
        shell32.SHChangeNotify(
            SHCNE_RENAMEITEM,
            SHCNF_PATH | SHCNF_FLUSHNOWAIT,
            str(src),
            str(dest),
        )
    except OSError:
        pass


def refresh_desktop(*paths: Path) -> None:
    """Notify Explorer that desktop contents changed."""
    for path in paths:
        try:
            shell32.SHChangeNotify(
                SHCNE_UPDATEDIR,
                SHCNF_PATH | SHCNF_FLUSHNOWAIT,
                str(path),
                None,
            )
        except OSError:
            pass
    shell32.SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_FLUSHNOWAIT, None, None)


def unique_dest_path(dest: Path) -> Path:
    if not dest.exists():
        return dest
    stem, suffix = dest.stem, dest.suffix
    counter = 1
    while dest.exists():
        dest = dest.parent / f"{stem}_{counter}{suffix}"
        counter += 1
    return dest


FO_MOVE = 0x0001
FO_COPY = 0x0002
FOF_SILENT = 0x0004
FOF_NOCONFIRMATION = 0x0010
FOF_ALLOWUNDO = 0x0040
FOF_NOCONFIRMMKDIR = 0x0200
FOF_NOERRORUI = 0x0400
# Never set FOF_RENAMEONCOLLISION — that creates 「opencode_1」 instead of a real move.
# Never omit FOF_NOCONFIRMATION when dest exists — Shell「重命名/替换」is not our UX.


class _SHFILEOPSTRUCTW(ctypes.Structure):
    _fields_ = [
        ("hwnd", wintypes.HWND),
        ("wFunc", wintypes.UINT),
        ("pFrom", ctypes.c_wchar_p),
        ("pTo", ctypes.c_wchar_p),
        ("fFlags", wintypes.WORD),
        ("fAnyOperationsAborted", wintypes.BOOL),
        ("hNameMappings", ctypes.c_void_p),
        ("lpszProgressTitle", ctypes.c_wchar_p),
    ]


def _same_volume(a: Path, b: Path) -> bool:
    try:
        return os.path.splitdrive(str(a))[0].casefold() == os.path.splitdrive(str(b))[0].casefold()
    except OSError:
        return False


def keyboard_fs_drop_modifiers() -> tuple[bool, bool]:
    """Return ``(ctrl, shift)`` for Explorer-like drop overrides."""
    try:
        from PyQt6.QtCore import Qt
        from PyQt6.QtWidgets import QApplication

        mods = QApplication.keyboardModifiers()
        ctrl = bool(mods & Qt.KeyboardModifier.ControlModifier)
        shift = bool(mods & Qt.KeyboardModifier.ShiftModifier)
        return ctrl, shift
    except Exception:
        return False, False


def default_fs_drop_is_move(
    src: Path,
    target_dir: Path,
    *,
    ctrl: bool | None = None,
    shift: bool | None = None,
) -> bool:
    """Explorer drag-into-folder rule: Shift→move, Ctrl→copy, else same-vol move.

    When *ctrl*/*shift* are omitted, reads the live keyboard modifiers.
    Ctrl+Shift together follows Ctrl (copy) — shortcut creation is out of scope.
    """
    if ctrl is None or shift is None:
        k_ctrl, k_shift = keyboard_fs_drop_modifiers()
        if ctrl is None:
            ctrl = k_ctrl
        if shift is None:
            shift = k_shift
    if ctrl:
        return False
    if shift:
        return True
    return _same_volume(Path(src), Path(target_dir))


def move_path_into_folder(src: Path, target_dir: Path) -> Path:
    """Move *src* into *target_dir*, keeping the original name (no ``_1`` copies).

    - Same volume + free name → ``os.rename`` / ``shutil.move`` (instant, no Shell UI).
    - Destination already occupied → ``FileExistsError`` (never Shell「重命名」dialog).
    - Cross volume + free name → Shell ``FO_MOVE`` with ``FOF_NOCONFIRMATION``;
      tiny files stay silent (no progress dialog).

    Raises ``OSError`` / ``FileExistsError`` when the move fails.
    """
    src = Path(src)
    target_dir = Path(target_dir)
    if not src.exists():
        raise FileNotFoundError(str(src))
    target_dir.mkdir(parents=True, exist_ok=True)
    dest = target_dir / src.name
    if src.parent == target_dir or src == dest:
        return src

    try:
        if dest.exists() and src.exists() and os.path.samefile(str(src), str(dest)):
            return dest
    except OSError:
        pass

    # Idempotent: source already gone, destination present (prior success).
    if dest.exists() and not src.exists():
        return dest

    if dest.exists():
        # Do not invite Shell Rename/Replace — that is the「提示重命名」bug.
        raise FileExistsError(str(dest))

    # Fast path: same volume → metadata rename (or shutil) without Shell.
    if _same_volume(src, target_dir):
        try:
            os.rename(str(src), str(dest))
            notify_shell_item_moved(src, dest)
            return dest
        except OSError:
            try:
                shutil.move(str(src), str(dest))
                notify_shell_item_moved(src, dest)
                return dest
            except OSError:
                pass

    # Cross-volume (or stubborn same-vol lock): Shell move, never rename-on-collision.
    from_buf = str(src) + "\0\0"
    to_buf = str(target_dir) + "\0\0"
    op = _SHFILEOPSTRUCTW()
    op.hwnd = None
    op.wFunc = FO_MOVE
    op.pFrom = from_buf
    op.pTo = to_buf
    flags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR | FOF_NOCONFIRMATION
    try:
        is_dir = src.is_dir()
        size = 0 if is_dir else int(src.stat().st_size)
    except OSError:
        is_dir, size = False, 0
    # Progress UI only for large / directory cross-drive work.
    if (not is_dir) and size < 2 * 1024 * 1024:
        flags |= FOF_SILENT | FOF_NOERRORUI
    op.fFlags = flags
    op.fAnyOperationsAborted = False
    op.hNameMappings = None
    op.lpszProgressTitle = None

    ole32 = ctypes.windll.ole32
    hr_init = ole32.CoInitialize(None)
    try:
        result = int(shell32.SHFileOperationW(ctypes.byref(op)))
    finally:
        # S_OK / S_FALSE from CoInitialize — only uninit if we initialized.
        if hr_init in (0, 1):
            try:
                ole32.CoUninitialize()
            except Exception:
                pass

    if result != 0 or op.fAnyOperationsAborted:
        raise OSError(
            f"Shell move failed code={result} aborted={bool(op.fAnyOperationsAborted)} "
            f"src={src} target={target_dir}"
        )
    if dest.exists() or not src.exists():
        return dest if dest.exists() else target_dir / src.name
    raise OSError(f"Shell move reported success but dest missing: {dest}")


def copy_path_into_folder(src: Path, target_dir: Path) -> Path:
    """Copy *src* into *target_dir* (Explorer cross-drive / Ctrl-drag default).

    Name collision uses ``unique_dest_path`` (no Shell replace/rename dialog).
    Directories use ``shutil.copytree``; large single files use ``shutil.copy2``.
    """
    src = Path(src)
    target_dir = Path(target_dir)
    if not src.exists():
        raise FileNotFoundError(str(src))
    target_dir.mkdir(parents=True, exist_ok=True)
    dest = unique_dest_path(target_dir / src.name)
    if src.is_dir():
        shutil.copytree(str(src), str(dest), dirs_exist_ok=False)
    else:
        shutil.copy2(str(src), str(dest))
    return dest


def transfer_path_into_folder(
    src: Path, target_dir: Path, *, move: bool | None = None
) -> Path:
    """Move or copy *src* into *target_dir* using Explorer-like *move* default."""
    src = Path(src)
    target_dir = Path(target_dir)
    if move is None:
        move = default_fs_drop_is_move(src, target_dir)
    if move:
        return move_path_into_folder(src, target_dir)
    return copy_path_into_folder(src, target_dir)


def _is_under_fence_storage(path: Path) -> bool:
    """True if path lives inside DeskTidy fence storage (cross-fence moves)."""
    from src.settings import get_fence_storage_root

    try:
        path.resolve().relative_to(get_fence_storage_root().resolve())
        return True
    except (OSError, ValueError):
        return False


def move_item_to_folder(src: Path, target_dir: Path, desktop: Path | None = None) -> Path | None:
    """Transfer a file into target_dir (Explorer same-vol move / cross-vol copy).

    Keyboard Ctrl/Shift overrides match drag-into-folder. Directories are not
    handled here (custom drag paths cover folder trees).
    """
    from src.settings import (
        get_desktop_path,
        get_desktop_paths,
        record_storage_origin,
        remap_storage_origin,
    )

    target_dir.mkdir(parents=True, exist_ok=True)
    try:
        src = src.resolve()
        target_dir = target_dir.resolve()
    except OSError:
        return None

    if not src.exists() or src.is_dir():
        return None
    if src.parent == target_dir:
        return src

    from_desktop = is_desktop_loose_item(src, desktop)
    from_storage = _is_under_fence_storage(src)
    do_move = default_fs_drop_is_move(src, target_dir)

    try:
        if do_move:
            dest = move_path_into_folder(src, target_dir)
            if from_desktop:
                record_storage_origin(dest, src.parent)
                refresh_desktop(*get_desktop_paths(), target_dir)
            elif from_storage:
                remap_storage_origin(src, dest)
                # Update hosted namespace shortcut path if this was a system icon lnk.
                try:
                    clsid = get_lnk_namespace_clsid(dest)
                    if clsid:
                        hosted = _hosted_namespace_store()
                        entry = hosted.get(_normalize_clsid(clsid))
                        if entry is not None:
                            entry["lnk"] = str(dest)
                            _save_hosted_namespace_store(hosted)
                except Exception:
                    pass
        else:
            dest = copy_path_into_folder(src, target_dir)
            # External / cross-drive copies restore to the primary user desktop by default.
            record_storage_origin(dest, desktop or get_desktop_path())
        return dest
    except OSError:
        return None


def preferred_drop_action_for_mime(mime, target_dir: Path | None = None):
    """Explorer-like Move/Copy for OLE drops into a known *target_dir*.

    Without *target_dir*, keeps the legacy desktop/fence-storage heuristic so
    virtual pin paths (copy-link) are unchanged.
    """
    from PyQt6.QtCore import Qt

    if mime is None:
        return Qt.DropAction.CopyAction

    ctrl, shift = keyboard_fs_drop_modifiers()
    if target_dir is not None and mime.hasUrls():
        decisions: list[bool] = []
        for url in mime.urls():
            local = url.toLocalFile()
            if not local:
                continue
            src = Path(local)
            if not src.exists():
                continue
            decisions.append(
                default_fs_drop_is_move(src, Path(target_dir), ctrl=ctrl, shift=shift)
            )
        if decisions:
            # One OLE effect for the whole drag: all-move → Move; else Copy
            # (safer — never delete sources when mixed volumes).
            if all(decisions):
                return Qt.DropAction.MoveAction
            return Qt.DropAction.CopyAction

    move_ok = False
    copy_needed = False
    if mime.hasUrls():
        for url in mime.urls():
            local = url.toLocalFile()
            if not local:
                continue
            src = Path(local)
            if not src.exists() or not src.is_file():
                continue
            if is_desktop_loose_item(src) or _is_under_fence_storage(src):
                move_ok = True
            else:
                copy_needed = True
    # Shell namespace icons / IDList drops are treated as moves onto the fence.
    for fmt in mime.formats():
        if "Shell IDList Array" in fmt:
            move_ok = True
            break

    if ctrl:
        return Qt.DropAction.CopyAction
    if shift and move_ok and not copy_needed:
        return Qt.DropAction.MoveAction
    if copy_needed:
        return Qt.DropAction.CopyAction
    if move_ok:
        return Qt.DropAction.MoveAction
    return Qt.DropAction.CopyAction


# Desktop namespace icons (not real files) — This PC, Recycle Bin, etc.
_NAMESPACE_FALLBACK_NAMES: dict[str, str] = {
    "{20D04FE0-3AEA-1069-A2D8-08002B30309D}": "此电脑",
    "{645FF040-5081-101B-9F08-00AA002F954E}": "回收站",
    "{F02C1A0D-BE21-4350-88B0-7367FC96EF3C}": "网络",
    "{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}": "控制面板",
    "{59031A47-3F72-44A7-89C5-5595FE6B30EE}": "用户文件夹",
}

_SHELL_IDLIST_FORMATS = (
    'application/x-qt-windows-mime;value="Shell IDList Array"',
    "application/x-qt-windows-mime;value=Shell IDList Array",
)

SIGDN_NORMALDISPLAY = 0
SIGDN_DESKTOPABSOLUTEPARSING = 0x80028000


def _extract_clsid(text: str) -> str | None:
    import re

    match = re.search(r"\{[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}\}", text)
    if not match:
        return None
    return match.group(0).upper()


DESKTIDY_VIRTUAL_MIME = "application/x-desktidy-virtual-item"
DESKTIDY_SOURCE_FENCE_MIME = "application/x-desktidy-source-fence"


def mime_has_droppable_items(mime) -> bool:
    """True if the mime payload can be imported into a fence."""
    if mime is None:
        return False
    if mime.hasFormat(DESKTIDY_VIRTUAL_MIME):
        return True
    if mime.hasUrls():
        return True
    for fmt in mime.formats():
        if "Shell IDList Array" in fmt:
            return True
    return False


def collect_drop_paths(mime) -> list[Path]:
    """Local filesystem paths from a drop (URLs + Shell IDList + DeskTidy virtual)."""
    paths: list[Path] = []
    seen: set[str] = set()

    def _add(path: Path, *, light: bool = False) -> None:
        try:
            if light:
                # Virtual/public drag path is already known-good; avoid resolve()
                # and type probes that can stall on namespace .lnk files.
                if not path.exists():
                    return
                key = str(path).casefold()
            else:
                if not path.exists() or not (path.is_file() or path.is_dir()):
                    return
                key = str(path).casefold()
        except OSError:
            return
        if key in seen:
            return
        seen.add(key)
        paths.append(path)

    if mime is None:
        return paths

    if mime.hasFormat(DESKTIDY_VIRTUAL_MIME):
        try:
            raw = bytes(mime.data(DESKTIDY_VIRTUAL_MIME).data()).decode("utf-8")
        except Exception:
            raw = ""
        for line in raw.splitlines():
            text = line.strip()
            if text:
                _add(Path(text), light=True)
        # Internal DeskTidy drags only carry the virtual payload.
        if paths:
            return paths

    if mime.hasUrls():
        for url in mime.urls():
            local = url.toLocalFile()
            if local:
                _add(Path(local))

    for fmt in list(mime.formats()):
        if "Shell IDList Array" not in fmt:
            continue
        try:
            raw = bytes(mime.data(fmt).data())
        except Exception:
            continue
        try:
            items = _iter_shell_idlist_items(raw)
        except Exception:
            # Keep URL-derived paths if Shell IDList parsing fails on one drop.
            break
        for _display, parsing in items:
            if not parsing:
                continue
            try:
                as_path = Path(parsing)
                if as_path.exists() and (as_path.is_file() or as_path.is_dir()):
                    _add(as_path)
            except OSError:
                continue
        break

    return paths


def desktidy_source_fence_id(mime) -> str:
    if mime is None or not mime.hasFormat(DESKTIDY_SOURCE_FENCE_MIME):
        return ""
    try:
        return bytes(mime.data(DESKTIDY_SOURCE_FENCE_MIME).data()).decode("utf-8")
    except Exception:
        return ""


_sh_name_from_idlist_ready = False


def _ensure_sh_name_from_idlist_api() -> None:
    """Bind SHGetNameFromIDList with 64-bit-safe pointer/SIGDN types."""
    global _sh_name_from_idlist_ready
    if _sh_name_from_idlist_ready:
        return
    # Without these, ctypes defaults to c_int and OverflowError on 64-bit PIDLs
    # (and SIGDN_* high-bit flags). Seen when Explorer drops Shell IDList Array.
    shell32.SHGetNameFromIDList.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_wchar_p),
    ]
    shell32.SHGetNameFromIDList.restype = ctypes.HRESULT
    _sh_name_from_idlist_ready = True


def _sh_get_name_from_pidl(pidl_ptr, sigdn: int) -> str:
    if not pidl_ptr:
        return ""
    _ensure_sh_name_from_idlist_api()
    ppsz = ctypes.c_wchar_p()
    hr = shell32.SHGetNameFromIDList(
        ctypes.c_void_p(pidl_ptr),
        ctypes.c_uint32(sigdn & 0xFFFFFFFF),
        ctypes.byref(ppsz),
    )
    if hr != 0 or not ppsz:
        return ""
    try:
        return ppsz.value or ""
    finally:
        ctypes.windll.ole32.CoTaskMemFree(ppsz)


def _iter_shell_idlist_items(data: bytes) -> list[tuple[str, str]]:
    """Parse CFSTR_SHELLIDLIST bytes into (display_name, parsing_name) pairs."""
    import struct

    if len(data) < 8:
        return []
    cidl = struct.unpack_from("<I", data, 0)[0]
    if cidl < 1:
        return []
    header_size = 4 + 4 * (cidl + 1)
    if header_size > len(data):
        return []
    offsets = [struct.unpack_from("<I", data, 4 + 4 * i)[0] for i in range(cidl + 1)]
    if any(off >= len(data) for off in offsets):
        return []

    buf = ctypes.create_string_buffer(data)
    base = ctypes.addressof(buf)
    parent = base + offsets[0]
    shell32.ILCombine.restype = ctypes.c_void_p
    shell32.ILCombine.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    shell32.ILFree.argtypes = [ctypes.c_void_p]

    results: list[tuple[str, str]] = []
    for index in range(1, cidl + 1):
        relative = base + offsets[index]
        absolute = shell32.ILCombine(ctypes.c_void_p(parent), ctypes.c_void_p(relative))
        if not absolute:
            continue
        try:
            display = _sh_get_name_from_pidl(absolute, SIGDN_NORMALDISPLAY)
            parsing = _sh_get_name_from_pidl(absolute, SIGDN_DESKTOPABSOLUTEPARSING)
            if not parsing:
                # SIGDN_FILESYSPATH
                parsing = _sh_get_name_from_pidl(absolute, 0x80058000)
            if display or parsing:
                results.append((display or Path(parsing).name, parsing or display))
        finally:
            shell32.ILFree(ctypes.c_void_p(absolute))
    return results


def create_namespace_shortcut(target_dir: Path, parsing_name: str, display_name: str = "") -> Path | None:
    """Create a .lnk to a shell namespace item (This PC, Recycle Bin, …)."""
    target_dir.mkdir(parents=True, exist_ok=True)
    clsid = _extract_clsid(parsing_name)
    if not display_name:
        if clsid and clsid in _NAMESPACE_FALLBACK_NAMES:
            display_name = _NAMESPACE_FALLBACK_NAMES[clsid]
        else:
            display_name = parsing_name.strip(":{}")[:32] or "快捷方式"
    # Sanitize Windows filename
    safe = "".join("_" if ch in '<>:"/\\|?*' else ch for ch in display_name).strip() or "快捷方式"
    dest = unique_dest_path(target_dir / f"{safe}.lnk")
    if clsid:
        target = f"::{{{clsid.strip('{}')}}}"
    else:
        target = parsing_name

    try:
        import win32com.client

        shell = win32com.client.Dispatch("WScript.Shell")
        shortcut = shell.CreateShortCut(str(dest))
        shortcut.Targetpath = target
        if clsid:
            # Marker so we can restore the desktop system icon on remove.
            shortcut.Description = f"DeskTidyNS:{clsid}"
        shortcut.save()
        return dest if dest.exists() else None
    except Exception:
        return None


def _normalize_clsid(clsid: str) -> str:
    extracted = _extract_clsid(clsid) or clsid
    extracted = extracted.strip().upper()
    if not extracted.startswith("{"):
        extracted = "{" + extracted.strip("{}") + "}"
    return extracted


def _hide_desktop_icon_reg_paths() -> list[str]:
    root = r"Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons"
    return [rf"{root}\NewStartPanel", rf"{root}\ClassicStartMenu"]


def set_desktop_namespace_icon_visible(clsid: str, visible: bool) -> None:
    """Show/hide a system desktop icon (This PC, Recycle Bin, …) via registry."""
    import winreg

    key_name = _normalize_clsid(clsid)
    # 0 = show, 1 = hide
    value = 0 if visible else 1
    for subkey in _hide_desktop_icon_reg_paths():
        try:
            with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, subkey, 0, winreg.KEY_SET_VALUE) as key:
                winreg.SetValueEx(key, key_name, 0, winreg.REG_DWORD, value)
        except OSError:
            continue
    refresh_desktop_namespace_icons()


def refresh_desktop_namespace_icons() -> None:
    """Ask Explorer to re-read desktop icon visibility."""
    try:
        from src.settings import get_desktop_paths

        refresh_desktop(*get_desktop_paths())
    except Exception:
        refresh_desktop()
    try:
        shell32.SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, None, None)
    except Exception:
        pass
    # Nudge desktop ListView to redraw.
    listview = _find_desktop_listview()
    if listview:
        try:
            win32gui.InvalidateRect(listview, None, True)
            win32gui.UpdateWindow(listview)
        except OSError:
            pass
    try:
        # SPI_SETDESKWALLPAPER-style soft refresh used by many shell tools.
        ctypes.windll.user32.SystemParametersInfoW(0x0014, 0, None, 0x01 | 0x02)
    except Exception:
        pass


def get_lnk_namespace_clsid(path: Path) -> str | None:
    """Return CLSID only for DeskTidy-created namespace shortcuts (DeskTidyNS:).

    Do not treat arbitrary user shortcuts to This PC / Recycle Bin as hosted —
    that caused false restores and accidental desktop .lnk deletion.
    """
    if path.suffix.lower() != ".lnk":
        return None
    try:
        key = str(path)
        mtime = path.stat().st_mtime_ns
    except OSError:
        key = str(path)
        mtime = -1
    cache = getattr(get_lnk_namespace_clsid, "_cache", None)
    if not isinstance(cache, dict):
        cache = {}
        get_lnk_namespace_clsid._cache = cache  # type: ignore[attr-defined]
    hit = cache.get(key)
    if isinstance(hit, tuple) and len(hit) == 2 and hit[0] == mtime:
        return hit[1]
    result: str | None = None
    try:
        import win32com.client

        shell = getattr(get_lnk_namespace_clsid, "_shell", None)
        if shell is None:
            shell = win32com.client.Dispatch("WScript.Shell")
            get_lnk_namespace_clsid._shell = shell  # type: ignore[attr-defined]
        shortcut = shell.CreateShortCut(str(path))
        desc = str(getattr(shortcut, "Description", "") or "")
        if desc.startswith("DeskTidyNS:"):
            result = _normalize_clsid(desc.split(":", 1)[1])
    except Exception:
        result = None
    cache[key] = (mtime, result)
    if len(cache) > 256:
        # Drop an arbitrary old entry to bound memory.
        try:
            cache.pop(next(iter(cache)))
        except StopIteration:
            pass
    return result


def _hosted_namespace_store() -> dict:
    from src.settings import load_settings

    settings = load_settings()
    hosted = settings.get("hosted_namespace_icons")
    return hosted if isinstance(hosted, dict) else {}


def _save_hosted_namespace_store(hosted: dict) -> None:
    """Persist hosted map and sync live in-memory app settings if present."""
    from src.settings import load_settings, save_settings

    settings = load_settings()
    settings["hosted_namespace_icons"] = hosted
    save_settings(settings)
    try:
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance()
        bridge = getattr(app, "_desktidy_settings", None) if app is not None else None
        if isinstance(bridge, dict):
            bridge["hosted_namespace_icons"] = dict(hosted)
    except Exception:
        pass


def host_namespace_icon_in_fence(clsid: str, display_name: str, lnk_path: Path) -> None:
    """Hide the desktop system icon and remember it lives in a fence."""
    key = _normalize_clsid(clsid)
    hosted = _hosted_namespace_store()
    hosted[key] = {
        "name": display_name or _NAMESPACE_FALLBACK_NAMES.get(key, key),
        "lnk": str(lnk_path),
    }
    _save_hosted_namespace_store(hosted)
    set_desktop_namespace_icon_visible(key, False)


def return_namespace_icon_to_desktop(clsid: str | None = None, lnk_path: Path | None = None) -> None:
    """Show the desktop system icon again (Fences-style move back)."""
    key = None
    if clsid:
        key = _normalize_clsid(clsid)
    elif lnk_path is not None:
        key = get_lnk_namespace_clsid(lnk_path)
    if not key:
        return
    hosted = _hosted_namespace_store()
    hosted.pop(key, None)
    _save_hosted_namespace_store(hosted)
    set_desktop_namespace_icon_visible(key, True)


def reapply_hosted_namespace_hides() -> None:
    """Ensure hosted system icons stay hidden while DeskTidy is running."""
    hosted = _hosted_namespace_store()
    changed = False
    for clsid, entry in list(hosted.items()):
        lnk = entry.get("lnk") if isinstance(entry, dict) else None
        if lnk and not Path(lnk).exists():
            hosted.pop(clsid, None)
            changed = True
            continue
        set_desktop_namespace_icon_visible(clsid, False)
    if changed:
        _save_hosted_namespace_store(hosted)


def reveal_hosted_namespace_icons() -> int:
    """Show hosted system icons without forgetting fence ownership.

    Used when DeskTidy exits so the desktop looks normal until next launch,
    while settings still remember which icons belong in fences.

    Always also unhides the stock system set (此电脑 / 回收站 / …). The hosted
    store alone can miss entries after crashes or partial saves — that left
    「此电脑」hidden in HideDesktopIcons after quit while other icons returned.
    """
    hosted = _hosted_namespace_store()
    count = 0
    seen: set[str] = set()
    for clsid in list(hosted.keys()):
        key = _normalize_clsid(clsid)
        set_desktop_namespace_icon_visible(key, True)
        seen.add(key)
        count += 1
    for clsid in _NAMESPACE_FALLBACK_NAMES:
        key = _normalize_clsid(clsid)
        if key in seen:
            continue
        set_desktop_namespace_icon_visible(key, True)
        count += 1
    return count


def ensure_system_namespace_icons_visible() -> int:
    """Force-show stock Explorer namespace icons (This PC, Recycle Bin, …)."""
    count = 0
    for clsid in _NAMESPACE_FALLBACK_NAMES:
        try:
            set_desktop_namespace_icon_visible(clsid, True)
            count += 1
        except Exception:
            pass
    return count


def restore_all_hosted_namespace_icons() -> int:
    """Unhide every system icon we moved into fences (used on exit)."""
    hosted = _hosted_namespace_store()
    count = 0
    seen: set[str] = set()
    for clsid in list(hosted.keys()):
        key = _normalize_clsid(clsid)
        set_desktop_namespace_icon_visible(key, True)
        seen.add(key)
        count += 1
    for clsid in _NAMESPACE_FALLBACK_NAMES:
        key = _normalize_clsid(clsid)
        if key in seen:
            continue
        set_desktop_namespace_icon_visible(key, True)
        count += 1
    _save_hosted_namespace_store({})
    return count


def move_namespace_icon_to_folder(
    target_dir: Path,
    parsing_name: str,
    display_name: str = "",
) -> Path | None:
    """Create a namespace .lnk in ``target_dir`` and hide the Explorer original."""
    # AIGC START
    clsid = _extract_clsid(parsing_name)
    if not clsid:
        return None
    clsid = _normalize_clsid(clsid)
    if not display_name:
        display_name = _NAMESPACE_FALLBACK_NAMES.get(clsid, "")
    # Avoid duplicates in the same fence folder.
    for existing in target_dir.glob("*.lnk") if target_dir.exists() else []:
        if get_lnk_namespace_clsid(existing) == clsid:
            host_namespace_icon_in_fence(clsid, display_name or existing.stem, existing)
            return existing
    dest = create_namespace_shortcut(
        target_dir, f"::{{{clsid.strip('{}')}}}", display_name
    )
    if dest is not None:
        host_namespace_icon_in_fence(clsid, display_name or dest.stem, dest)
    return dest
    # AIGC END


def import_drop_to_folder(mime, target_dir: Path, desktop: Path | None = None) -> list[Path]:
    """Import filesystem files and shell namespace icons from a drop into target_dir."""
    imported: list[Path] = []
    seen: set[str] = set()

    def _norm_path(path: Path) -> str:
        try:
            p = path if path.is_absolute() else path.absolute()
            return str(p).casefold()
        except OSError:
            return str(path).casefold()

    def _import_namespace(clsid: str, display: str, parsing: str) -> None:
        key = _normalize_clsid(clsid)
        if key in seen:
            return
        seen.add(key)
        dest = move_namespace_icon_to_folder(target_dir, parsing or f"::{key}", display)
        if dest is not None:
            imported.append(dest)

    if mime.hasUrls():
        for url in mime.urls():
            local = url.toLocalFile()
            text = url.toString()
            if local:
                src = Path(local)
                if src.exists() and src.is_file():
                    path_key = _norm_path(src)
                    if path_key in seen:
                        continue
                    # If user drops a previously hosted ns lnk, still host/hide.
                    ns = get_lnk_namespace_clsid(src)
                    if ns:
                        _import_namespace(ns, src.stem, f"::{ns}")
                        seen.add(path_key)
                        try:
                            if src.resolve() != imported[-1].resolve():
                                src.unlink(missing_ok=True)
                        except (OSError, IndexError):
                            pass
                        continue
                    dest = move_item_to_folder(src, target_dir, desktop)
                    seen.add(path_key)
                    if dest is not None:
                        imported.append(dest)
                        seen.add(_norm_path(dest))
                    continue
            clsid = _extract_clsid(local or text)
            if clsid:
                display = _NAMESPACE_FALLBACK_NAMES.get(_normalize_clsid(clsid), "")
                _import_namespace(clsid, display, f"::{{{clsid.strip('{}')}}}")

    for fmt in list(mime.formats()):
        if "Shell IDList Array" not in fmt:
            continue
        try:
            raw = bytes(mime.data(fmt).data())
        except Exception:
            continue
        try:
            idlist_items = _iter_shell_idlist_items(raw)
        except Exception:
            break
        for display, parsing in idlist_items:
            clsid = _extract_clsid(parsing) or _extract_clsid(display)
            key = (clsid or parsing).upper()
            if key in seen:
                continue
            try:
                as_path = Path(parsing)
                if as_path.exists() and as_path.is_file():
                    path_key = _norm_path(as_path)
                    if path_key in seen:
                        continue
                    seen.add(key)
                    seen.add(path_key)
                    dest = move_item_to_folder(as_path, target_dir, desktop)
                    if dest is not None:
                        imported.append(dest)
                        seen.add(_norm_path(dest))
                    continue
            except OSError:
                pass
            if not clsid and not parsing.startswith("::") and "shell:" not in parsing.lower():
                # Match by Chinese/English display name for stubborn shell drops.
                name = (display or "").strip().lower()
                mapped = None
                if name in {"此电脑", "我的电脑", "computer", "this pc", "my computer"}:
                    mapped = "{20D04FE0-3AEA-1069-A2D8-08002B30309D}"
                elif name in {"回收站", "recycle bin", "recyclebin"}:
                    mapped = "{645FF040-5081-101B-9F08-00AA002F954E}"
                if mapped:
                    _import_namespace(mapped, display, f"::{mapped}")
                continue
            if clsid:
                _import_namespace(clsid, display, parsing)
        break

    return imported
