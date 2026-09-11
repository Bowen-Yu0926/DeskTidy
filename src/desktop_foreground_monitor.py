"""Watch Windows foreground changes (Win+D, Alt+Tab back to desktop)."""

from __future__ import annotations

import ctypes
from ctypes import wintypes

from PyQt6.QtCore import QObject, pyqtSignal

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

EVENT_SYSTEM_FOREGROUND = 0x0003
WINEVENT_OUTOFCONTEXT = 0x0000

_OWN_PID = int(kernel32.GetCurrentProcessId())

_DESKTOP_FG_CLASSES = frozenset(
    {
        "Progman",
        "WorkerW",
        "SHELLDLL_DefView",
        "SysListView32",
    }
)
# Shell / DeskTidy context menus stay on the desktop surface so RMB does not
# emit leave-desktop → foreign sink while partitions may still be Win32-hidden.
_SHELL_MENU_SURFACE_CLASSES = frozenset(
    {
        "DeskTidyShellMenuHost",
        "#32768",
        "Xaml_WindowedPopupClass",
        "XamlExplorerHostIslandWindow",
        "Microsoft.UI.Content.PopupWindowSiteBridge",
    }
)
# Taskbar / tray — clicking the notification area must not leave-desktop sink.
_SHELL_TASKBAR_SURFACE_CLASSES = frozenset(
    {
        "Shell_TrayWnd",
        "Shell_SecondaryTrayWnd",
        "NotifyIconOverflowWindow",
        "TopLevelWindowForOverflowXamlIsland",
    }
)

WinEventProcType = ctypes.WINFUNCTYPE(
    None,
    wintypes.HANDLE,
    wintypes.DWORD,
    wintypes.HWND,
    wintypes.LONG,
    wintypes.LONG,
    wintypes.DWORD,
    wintypes.DWORD,
)


def _class_name(hwnd: int) -> str:
    if not hwnd:
        return ""
    try:
        buf = ctypes.create_unicode_buffer(256)
        if user32.GetClassNameW(int(hwnd), buf, 256):
            return buf.value or ""
    except Exception:
        pass
    return ""


def _hwnd_looks_desktop(hwnd: int) -> bool:
    """Cheap class walk — used in the WinEvent hook to skip app↔app noise."""
    cur = int(hwnd or 0)
    hops = 0
    while cur and hops < 6:
        name = _class_name(cur)
        if name in _DESKTOP_FG_CLASSES:
            return True
        try:
            cur = int(user32.GetParent(cur) or 0)
        except Exception:
            break
        hops += 1
    return False


def _hwnd_is_own_desktop_chrome(hwnd: int) -> bool:
    """True for fence/page-bar TOOLWINDOWs — not settings/notepad MainWindows."""
    if not hwnd:
        return False
    try:
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(int(hwnd), ctypes.byref(pid))
        if int(pid.value) != _OWN_PID:
            return False
        if _class_name(int(hwnd)) == "DeskTidyShellMenuHost":
            return True
        GWL_EXSTYLE = -20
        WS_EX_TOOLWINDOW = 0x00000080
        WS_EX_APPWINDOW = 0x00040000
        ex = int(user32.GetWindowLongW(int(hwnd), GWL_EXSTYLE))
        if ex & WS_EX_APPWINDOW:
            return False
        return bool(ex & WS_EX_TOOLWINDOW)
    except Exception:
        return False


def _hwnd_is_shell_menu_surface(hwnd: int) -> bool:
    """Classic / Win11 shell menus and DeskTidy TrackPopupMenu host."""
    name = _class_name(int(hwnd or 0))
    if name in _SHELL_MENU_SURFACE_CLASSES:
        return True
    try:
        root = int(user32.GetAncestor(int(hwnd or 0), 2) or 0)  # GA_ROOT
    except Exception:
        root = 0
    return bool(root) and _class_name(root) in _SHELL_MENU_SURFACE_CLASSES


def _hwnd_is_shell_taskbar_surface(hwnd: int) -> bool:
    """Taskbar / notification area (and children)."""
    cur = int(hwnd or 0)
    hops = 0
    while cur and hops < 6:
        if _class_name(cur) in _SHELL_TASKBAR_SURFACE_CLASSES:
            return True
        try:
            cur = int(user32.GetParent(cur) or 0)
        except Exception:
            break
        hops += 1
    try:
        root = int(user32.GetAncestor(int(hwnd or 0), 2) or 0)
    except Exception:
        root = 0
    return bool(root) and _class_name(root) in _SHELL_TASKBAR_SURFACE_CLASSES


def _on_desktop_surface(hwnd: int) -> bool:
    """Explorer DefView, desktop chrome, shell menu, or taskbar — not app MainWindows."""
    return (
        _hwnd_looks_desktop(hwnd)
        or _hwnd_is_own_desktop_chrome(hwnd)
        or _hwnd_is_shell_menu_surface(hwnd)
        or _hwnd_is_shell_taskbar_surface(hwnd)
    )


class DesktopForegroundMonitor(QObject):
    """Emit on desktop-surface enter/leave edges (foreign app ↔ desktop side).

    Desktop side = Explorer Progman/WorkerW/DefView **or** DeskTidy chrome.
    App↔app Alt-Tab and Explorer↔own-chrome churn are silent. Foreign FG sinks
    overlays under windows (stay visible); shell repair still gates on Explorer
    FG in app.py.
    """

    foreground_changed = pyqtSignal()

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._hook = None
        self._proc = WinEventProcType(self._on_win_event)
        self._prev_desktop = False

    def _on_win_event(
        self,
        _hook,
        event: int,
        hwnd,
        _id_object,
        _id_child,
        _thread,
        _time,
    ) -> None:
        if event != EVENT_SYSTEM_FOREGROUND:
            return
        on_surface = _on_desktop_surface(int(hwnd or 0))
        # No edge: still on surface, or still in foreign apps.
        if on_surface == self._prev_desktop:
            return
        if not on_surface and not self._prev_desktop:
            return
        self._prev_desktop = on_surface
        self.foreground_changed.emit()

    def start(self) -> None:
        if self._hook:
            return
        self._hook = user32.SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            None,
            self._proc,
            0,
            0,
            WINEVENT_OUTOFCONTEXT,
        )
        if not self._hook:
            self._hook = None

    def stop(self) -> None:
        if self._hook:
            user32.UnhookWinEvent(self._hook)
            self._hook = None
