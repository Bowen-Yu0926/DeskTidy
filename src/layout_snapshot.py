"""DeskTidy layout snapshot (backup & restore).

Primary payload is DeskTidy fence layout + style (virtual mode hides Explorer
icons). Shell icon positions are captured best-effort when available.
"""

from __future__ import annotations

import ctypes
import json
from copy import deepcopy
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

import win32con
import win32gui

from src.settings import APP_DIR

SNAPSHOTS_DIR = APP_DIR / "snapshots"

LVM_GETITEMCOUNT = 0x1004
LVM_GETITEMPOSITION = 0x1010
LVM_SETITEMPOSITION = 0x100F
LVM_GETITEMTEXTW = 0x1073
LVIF_TEXT = 0x0001

# Settings keys that define the visible DeskTidy desktop layout / style.
_LAYOUT_KEYS: tuple[str, ...] = (
    "fences",
    "fence_layouts_by_display",
    "fence_layouts_by_page",
    "desktop_pages",
    "current_page",
    "public_desktop_items",
    "enable_public_desktop",
    "theme",
    "show_fences",
)


class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class LVITEMW(ctypes.Structure):
    _fields_ = [
        ("mask", ctypes.c_uint),
        ("iItem", ctypes.c_int),
        ("iSubItem", ctypes.c_int),
        ("state", ctypes.c_uint),
        ("stateMask", ctypes.c_uint),
        ("pszText", ctypes.c_void_p),
        ("cchTextMax", ctypes.c_int),
        ("iImage", ctypes.c_int),
        ("lParam", ctypes.c_void_p),
        ("iIndent", ctypes.c_int),
        ("iGroupId", ctypes.c_int),
        ("cColumns", ctypes.c_uint),
        ("puColumns", ctypes.c_void_p),
    ]


@dataclass
class IconPosition:
    name: str
    x: int
    y: int


@dataclass
class LayoutSnapshot:
    name: str
    created_at: str
    icons: list[IconPosition] = field(default_factory=list)
    icon_count: int = 0
    layout: dict[str, Any] = field(default_factory=dict)
    fence_count: int = 0

    def to_dict(self) -> dict:
        data = {
            "name": self.name,
            "created_at": self.created_at,
            "icon_count": self.icon_count,
            "fence_count": self.fence_count,
            "icons": [{"name": i.name, "x": i.x, "y": i.y} for i in self.icons],
        }
        if self.layout:
            data["layout"] = self.layout
        return data

    @classmethod
    def from_dict(cls, data: dict) -> LayoutSnapshot:
        icons = [
            IconPosition(name=i["name"], x=i["x"], y=i["y"])
            for i in data.get("icons", [])
            if isinstance(i, dict) and i.get("name")
        ]
        layout = data.get("layout")
        if not isinstance(layout, dict):
            layout = {}
        fences = layout.get("fences") if layout else None
        fence_count = int(data.get("fence_count") or 0)
        if fence_count <= 0 and isinstance(fences, list):
            fence_count = len(fences)
        return cls(
            name=data.get("name", ""),
            created_at=data.get("created_at", ""),
            icons=icons,
            icon_count=int(data.get("icon_count", len(icons)) or len(icons)),
            layout=layout,
            fence_count=fence_count,
        )

    @property
    def has_desktidy_layout(self) -> bool:
        return bool(self.layout)


def capture_desktidy_layout(settings: dict) -> dict[str, Any]:
    """Deep-copy the DeskTidy settings that define on-desktop layout/style."""
    bundle: dict[str, Any] = {}
    for key in _LAYOUT_KEYS:
        if key in settings:
            bundle[key] = deepcopy(settings[key])
    return bundle


def _capture_shell_icons() -> list[IconPosition]:
    """Best-effort shell ListView positions; empty when icons are hidden.

    Cross-process ``LVM_GETITEMTEXT`` with a *local* buffer is unreliable on
    x64 and can stall for seconds per item. Use short timeouts and abort when
    the ListView is not answering with real names (typical when DeskTidy has
    already hidden shell icons).
    """
    hwnd = _find_desktop_listview()
    if not hwnd:
        return []
    try:
        count = int(win32gui.SendMessage(hwnd, LVM_GETITEMCOUNT, 0, 0))
    except Exception:
        return []
    if count <= 0:
        return []
    icons: list[IconPosition] = []
    empty_streak = 0
    for i in range(count):
        try:
            text = _get_item_text(hwnd, i)
            if not text:
                empty_streak += 1
                if empty_streak >= 3 and not icons:
                    return []
                continue
            empty_streak = 0
            x, y = _get_item_position(hwnd, i)
            icons.append(IconPosition(name=text, x=x, y=y))
        except Exception:
            empty_streak += 1
            if empty_streak >= 3 and not icons:
                return []
            continue
    return icons


def _find_desktop_listview() -> int | None:
    """Find the SysListView32 hwnd that hosts desktop icons."""
    result: list[int] = []

    def enum_callback(hwnd, _):
        if win32gui.GetClassName(hwnd) != "WorkerW":
            return True
        shell_view = win32gui.FindWindowEx(hwnd, None, "SHELLDLL_DefView", None)
        if not shell_view:
            return True
        list_view = win32gui.FindWindowEx(shell_view, None, "SysListView32", None)
        if list_view:
            result.append(list_view)
        return True

    progman = win32gui.FindWindow("Progman", None)
    # Prefer an already-present DefView — avoid poking Progman (0x052C) first;
    # that message can block up to the timeout even when a ListView exists.
    if progman:
        shell_view = win32gui.FindWindowEx(progman, None, "SHELLDLL_DefView", None)
        if shell_view:
            list_view = win32gui.FindWindowEx(shell_view, None, "SysListView32", None)
            if list_view:
                return list_view

    win32gui.EnumWindows(enum_callback, None)
    if result:
        return result[0]

    if progman:
        try:
            win32gui.SendMessageTimeout(
                progman, 0x052C, 0, 0, win32con.SMTO_ABORTIFHUNG, 200
            )
        except TypeError:
            try:
                win32gui.SendMessageTimeout(
                    progman, 0x052C, 0, 0, win32con.SMTO_NORMAL, 200
                )
            except Exception:
                pass
        except Exception:
            pass
        win32gui.EnumWindows(enum_callback, None)
        if result:
            return result[0]
        shell_view = win32gui.FindWindowEx(progman, None, "SHELLDLL_DefView", None)
        if shell_view:
            list_view = win32gui.FindWindowEx(shell_view, None, "SysListView32", None)
            if list_view:
                return list_view
    return None


def _send_listview(
    hwnd: int, msg: int, wparam: int, lparam: int, *, timeout_ms: int = 80
) -> bool:
    """Send a ListView message with a short hung-abort timeout. False on timeout."""
    try:
        ok, _ = win32gui.SendMessageTimeout(
            hwnd, msg, wparam, lparam, win32con.SMTO_ABORTIFHUNG, timeout_ms
        )
        return bool(ok)
    except TypeError:
        try:
            win32gui.SendMessage(hwnd, msg, wparam, lparam)
            return True
        except Exception:
            return False
    except Exception:
        return False


def _get_item_text(hwnd: int, index: int) -> str:
    buf = ctypes.create_unicode_buffer(512)
    item = LVITEMW()
    item.mask = LVIF_TEXT
    item.iItem = index
    item.pszText = ctypes.addressof(buf)
    item.cchTextMax = 512
    if not _send_listview(hwnd, LVM_GETITEMTEXTW, index, ctypes.addressof(item)):
        return ""
    return buf.value


def _get_item_position(hwnd: int, index: int) -> tuple[int, int]:
    point = POINT()
    if not _send_listview(hwnd, LVM_GETITEMPOSITION, index, ctypes.addressof(point)):
        return 0, 0
    return point.x, point.y


def _set_item_position(hwnd: int, index: int, x: int, y: int) -> None:
    point = POINT(x=x, y=y)
    win32gui.SendMessage(hwnd, LVM_SETITEMPOSITION, index, ctypes.addressof(point))


def default_snapshot_name(
    *,
    now: datetime | None = None,
    existing_names: list[str] | tuple[str, ...] | set[str] | None = None,
) -> str:
    """Default save-dialog title: today's timestamp (editable by the user).

    Minute precision first; if that label already exists, include seconds.
    """
    dt = now or datetime.now()
    base = dt.strftime("%Y-%m-%d %H:%M")
    names = {
        str(n).strip().casefold()
        for n in (existing_names or ())
        if str(n).strip()
    }
    if base.casefold() not in names:
        return base
    return dt.strftime("%Y-%m-%d %H:%M:%S")


def capture_layout(name: str = "", settings: dict | None = None) -> LayoutSnapshot:
    """Capture DeskTidy layout/style (+ optional shell icon positions)."""
    cleaned = str(name or "").strip()
    if not cleaned:
        cleaned = default_snapshot_name()

    layout: dict[str, Any] = {}
    fence_count = 0
    if isinstance(settings, dict):
        layout = capture_desktidy_layout(settings)
        fences = layout.get("fences")
        if isinstance(fences, list):
            fence_count = len(fences)

    # Virtual / hidden-shell mode: DeskTidy layout is the source of truth.
    # Probing Explorer ListView here used to stall several seconds per save.
    icons: list[IconPosition] = []
    if not (isinstance(settings, dict) and settings.get("hide_shell_icons") is True):
        icons = _capture_shell_icons()
    return LayoutSnapshot(
        name=cleaned,
        created_at=datetime.now().isoformat(timespec="seconds"),
        icons=icons,
        icon_count=len(icons),
        layout=layout,
        fence_count=fence_count,
    )


def restore_layout(snapshot: LayoutSnapshot) -> tuple[int, int]:
    """Restore shell icon positions from snapshot. Returns (restored, missing)."""
    if not snapshot.icons:
        return 0, 0
    hwnd = _find_desktop_listview()
    if not hwnd:
        return 0, len(snapshot.icons)

    count = win32gui.SendMessage(hwnd, LVM_GETITEMCOUNT, 0, 0)
    name_to_index: dict[str, int] = {}
    for i in range(count):
        text = _get_item_text(hwnd, i)
        if text:
            name_to_index[text] = i

    restored = 0
    missing = 0
    for icon in snapshot.icons:
        idx = name_to_index.get(icon.name)
        if idx is None:
            missing += 1
            continue
        _set_item_position(hwnd, idx, icon.x, icon.y)
        restored += 1

    win32gui.InvalidateRect(hwnd, None, True)
    return restored, missing


def apply_layout_snapshot(settings: dict, snapshot: LayoutSnapshot) -> dict[str, int]:
    """Apply DeskTidy layout bundle into ``settings``.

    Returns a small summary: fence_count, shell_restored, shell_missing.
    Shell icon restore is best-effort and never raises.
    """
    fence_count = 0
    if snapshot.has_desktidy_layout:
        for key, value in snapshot.layout.items():
            if key in _LAYOUT_KEYS:
                settings[key] = deepcopy(value)
        fences = settings.get("fences")
        if isinstance(fences, list):
            fence_count = len(fences)
    shell_restored, shell_missing = 0, 0
    try:
        shell_restored, shell_missing = restore_layout(snapshot)
    except Exception:
        shell_missing = len(snapshot.icons)
    return {
        "fence_count": fence_count,
        "shell_restored": shell_restored,
        "shell_missing": shell_missing,
    }


def save_snapshot(snapshot: LayoutSnapshot, *, write_preview: bool = True) -> Path:
    SNAPSHOTS_DIR.mkdir(parents=True, exist_ok=True)
    safe_name = snapshot.name.replace("/", "-").replace("\\", "-").replace(":", "-")
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    path = SNAPSHOTS_DIR / f"{timestamp}_{safe_name}.json"
    with open(path, "w", encoding="utf-8") as f:
        json.dump(snapshot.to_dict(), f, ensure_ascii=False, indent=2)
    if write_preview and snapshot.has_desktidy_layout:
        try:
            from src.snapshot_preview import ensure_snapshot_preview

            ensure_snapshot_preview(path, snapshot.layout)
        except Exception:
            pass
    return path


def list_snapshots() -> list[tuple[Path, LayoutSnapshot]]:
    """List snapshots newest-first. Does not render previews (keeps UI snappy)."""
    if not SNAPSHOTS_DIR.exists():
        return []
    results: list[tuple[Path, LayoutSnapshot]] = []
    for path in sorted(SNAPSHOTS_DIR.glob("*.json"), reverse=True):
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            snap = LayoutSnapshot.from_dict(data)
            results.append((path, snap))
        except (json.JSONDecodeError, OSError, TypeError, KeyError):
            continue
    return results


def delete_snapshot(path: Path) -> None:
    path.unlink(missing_ok=True)
    try:
        from src.snapshot_preview import preview_path_for

        preview_path_for(path).unlink(missing_ok=True)
    except Exception:
        pass
