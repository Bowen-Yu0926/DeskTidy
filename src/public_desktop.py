"""Floating desktop icons outside fences.

- Shared / public area (`enable_public_desktop`): no `page` field → visible on
  every page.
- Page-local floats: `page` set → only that page sees them (public area off).

Icons auto-arrange on a Windows-like left-edge grid (column-major) and skip
slots that would sit under fence frames.
"""

from __future__ import annotations

import time
from pathlib import Path

from src.desktop_icon_metrics import system_desktop_icon_cell

# Grid pitch = Explorer DefView item spacing (not a homemade larger shelf).
_cell = system_desktop_icon_cell()
CELL_W = int(_cell.width)
CELL_H = int(_cell.height)
MARGIN_X = 20
MARGIN_Y = 20
# Spacing already includes the gap between icons — do not add a second gutter.
GAP_X = 0
GAP_Y = 0
FENCE_PAD = 16


def refresh_public_grid_metrics() -> None:
    """Re-read Explorer cell size (DPI / icon-size change)."""
    global CELL_W, CELL_H, _cell
    from src.desktop_icon_metrics import clear_desktop_icon_metrics_cache

    clear_desktop_icon_metrics_cache()
    _cell = system_desktop_icon_cell()
    CELL_W = int(_cell.width)
    CELL_H = int(_cell.height)

# sync_loose scans the real desktop; calling it on every public refresh hitchs UI.
_LOOSE_SYNC_TTL_S = 12.0
_PRUNE_MISSING_TTL_S = 8.0
# Match fence virtual pins: brief Office save gaps should not drop floats instantly.
_STICKY_MISSING_S = 45.0
_prune_missing_cache: dict[str, float] = {"t": 0.0}
_missing_since: dict[str, float] = {}
_loose_sync_cache: dict = {"sid": 0, "t": 0.0, "changed": False}


def clear_public_missing_tracking() -> None:
    """Reset sticky-miss stamps (tests / settings reload)."""
    _missing_since.clear()
    _prune_missing_cache["t"] = 0.0


def _norm_key(path: Path | str) -> str:
    """Stable path key without symlink resolve (resolve can stall on .lnk)."""
    try:
        p = Path(str(path))
        if p.is_absolute():
            return str(p)
        return str(p.resolve())
    except OSError:
        return str(path)


def live_fence_rects(settings: dict, page_id: int):
    """Prefer on-screen fence frames; fall back to saved geometry."""
    from PyQt6.QtWidgets import QApplication

    live = None
    qt_app = QApplication.instance()
    desk = getattr(qt_app, "_desktidy_app", None) if qt_app else None
    if desk is not None:
        live = list(getattr(desk, "fences", []) or [])
    return collect_fence_rects(settings, page_id, live_fences=live or None)


def is_public_desktop_enabled(settings: dict | None) -> bool:
    """Shared public area (cross-page). Page-local floats work either way."""
    if not isinstance(settings, dict):
        return False
    return bool(settings.get("enable_public_desktop", False))


def is_shared_public_entry(entry: dict) -> bool:
    """True when the icon belongs to the cross-page public area."""
    return entry.get("page") is None


def is_system_namespace_path(path: Path | str | None) -> bool:
    """True for DeskTidy-hosted This PC / Recycle Bin shortcuts."""
    if not path:
        return False
    try:
        raw = str(path)
    except OSError:
        return False
    folded = raw.casefold().replace("/", "\\")
    if ".public_system" in folded:
        return True
    try:
        from src.win_shell import get_lnk_namespace_clsid

        return get_lnk_namespace_clsid(Path(raw)) is not None
    except Exception:
        return False


def is_system_namespace_entry(entry: dict) -> bool:
    return isinstance(entry, dict) and is_system_namespace_path(entry.get("path"))


def is_loose_desktop_entry(entry: dict | None) -> bool:
    """Unclaimed desktop file/folder hosted as an overlay (all pages)."""
    return isinstance(entry, dict) and bool(entry.get("loose"))


def get_public_items(settings: dict) -> list[dict]:
    raw = settings.get("public_desktop_items")
    if not isinstance(raw, list):
        settings["public_desktop_items"] = []
        return settings["public_desktop_items"]
    return raw


def visible_floating_items(settings: dict, page_id: int) -> list[dict]:
    """Icons that should be drawn on the given desktop page."""
    from src.system_defaults import SYSTEM_WORK_PAGE_ID

    visible: list[dict] = []
    shared_ok = is_public_desktop_enabled(settings)
    for entry in get_public_items(settings):
        if not isinstance(entry, dict) or not entry.get("path"):
            continue
        # This PC / Recycle Bin stay visible on every page.
        if is_system_namespace_entry(entry):
            visible.append(entry)
            continue
        # Unclaimed desktop items stay on the page they were synced to.
        if is_loose_desktop_entry(entry):
            try:
                loose_page = int(entry.get("page", SYSTEM_WORK_PAGE_ID))
            except (TypeError, ValueError):
                loose_page = SYSTEM_WORK_PAGE_ID
            if loose_page == int(page_id):
                visible.append(entry)
            continue
        if is_shared_public_entry(entry):
            if shared_ok:
                visible.append(entry)
            continue
        try:
            if int(entry.get("page")) == int(page_id):
                visible.append(entry)
        except (TypeError, ValueError):
            continue
    return visible


def iter_public_paths(settings: dict) -> list[Path]:
    """Paths stored as floating icons (shared or page-local)."""
    paths: list[Path] = []
    for entry in get_public_items(settings):
        if not isinstance(entry, dict):
            continue
        raw = entry.get("path")
        if raw:
            paths.append(Path(str(raw)))
    return paths


def public_claimed_keys(settings: dict) -> set[str]:
    """Case-folded path keys for floating icons (any page / shared).

    Used by the files list / fence refresh to treat floats as already placed.
    One-click organize still considers these paths unless whitelisted.
    """
    keys: set[str] = set()
    for entry in get_public_items(settings):
        if not isinstance(entry, dict):
            continue
        path = entry.get("path")
        if not path:
            continue
        try:
            keys.add(_norm_key(path).casefold())
        except OSError:
            keys.add(str(path).casefold())
    return keys


def migrate_shared_public_to_page(settings: dict, page_id: int) -> bool:
    """When turning off cross-page public area, keep icons as page-local floats."""
    try:
        target_page = int(page_id)
    except (TypeError, ValueError):
        target_page = 0
    changed = False
    for entry in get_public_items(settings):
        if not isinstance(entry, dict) or not entry.get("path"):
            continue
        if not is_shared_public_entry(entry):
            continue
        # Keep hosted system icons cross-page; loose items stay on work page.
        if is_system_namespace_entry(entry) or is_loose_desktop_entry(entry):
            continue
        entry["page"] = target_page
        entry.pop("shared", None)
        changed = True
    return changed


def promote_page_public_to_shared(settings: dict) -> bool:
    """When enabling public area, promote page-local floats to cross-page.

    Skips floats explicitly pinned to a page (``shared is False``), e.g. RMB
    「移动到分页」— those must stay on their target page even with public on.
    """
    changed = False
    for entry in get_public_items(settings):
        if not isinstance(entry, dict) or not entry.get("path"):
            continue
        if is_shared_public_entry(entry):
            continue
        if is_loose_desktop_entry(entry):
            continue
        if is_system_namespace_entry(entry):
            continue
        if entry.get("shared") is False:
            continue
        before = (entry.get("page"), entry.get("shared"), entry.get("loose"))
        entry.pop("page", None)
        entry.pop("shared", None)
        entry.pop("loose", None)
        after = (entry.get("page"), entry.get("shared"), entry.get("loose"))
        if before != after:
            changed = True
    return changed


def heal_public_area_scope(settings: dict, page_id: int) -> bool:
    """Reconcile floats with the public-area toggle (idempotent heal)."""
    if is_public_desktop_enabled(settings):
        return promote_page_public_to_shared(settings)
    return migrate_shared_public_to_page(settings, page_id)


def invalidate_loose_sync_cache() -> None:
    """Force the next sync_loose_desktop_items call to rescan."""
    _loose_sync_cache["sid"] = 0
    _loose_sync_cache["t"] = 0.0


def scan_loose_desktop_wanted(settings: dict) -> dict[str, Path]:
    """Scan desktop for paths that should appear as loose floats (read-only)."""
    from src.desktop_scanner import scan_desktop
    from src.fence_rules import all_fence_pinned_keys, path_in_pinned_keys

    scan = scan_desktop(exclude=settings.get("exclude_patterns") or [])
    pinned = all_fence_pinned_keys(settings)
    wanted: dict[str, Path] = {}
    for item in scan.items:
        path = item.path
        if is_system_namespace_path(path):
            continue
        try:
            key = _norm_key(path).casefold()
        except OSError:
            key = str(path).casefold()
        if path_in_pinned_keys(path, pinned):
            continue
        wanted[key] = path
    return wanted


def apply_loose_desktop_scan(settings: dict, wanted: dict[str, Path]) -> bool:
    """Merge a loose-desktop scan into ``public_desktop_items`` (GUI thread)."""
    from src.system_defaults import SYSTEM_WORK_PAGE_ID

    work_page = SYSTEM_WORK_PAGE_ID
    try:
        home_page = int(settings.get("current_page", work_page))
    except (TypeError, ValueError):
        home_page = work_page

    items = get_public_items(settings)
    changed = False
    kept: list[dict] = []
    covered: set[str] = set()

    for entry in items:
        if not isinstance(entry, dict) or not entry.get("path"):
            kept.append(entry)
            continue
        try:
            key = _norm_key(str(entry["path"])).casefold()
        except OSError:
            key = str(entry["path"]).casefold()

        if is_loose_desktop_entry(entry):
            if key in wanted:
                try:
                    int(entry.get("page"))
                except (TypeError, ValueError):
                    entry["page"] = home_page
                    changed = True
                kept.append(entry)
                covered.add(key)
            else:
                changed = True
            continue

        if key in wanted:
            kept.append(entry)
            covered.add(key)
            continue

        kept.append(entry)

    items[:] = kept
    fence_rects = live_fence_rects(settings, home_page)
    for key, path in wanted.items():
        if key in covered:
            continue
        sx, sy = find_auto_slot(
            settings,
            page_id=home_page,
            fence_rects=fence_rects,
            prefer_nearest=False,
        )
        if is_public_desktop_enabled(settings):
            items.append({"path": _norm_key(path), "x": sx, "y": sy})
        else:
            items.append(
                {
                    "path": _norm_key(path),
                    "x": sx,
                    "y": sy,
                    "page": home_page,
                    "loose": True,
                }
            )
        covered.add(key)
        changed = True
    return changed


def ensure_desktop_loose_floats(settings: dict) -> bool:
    """Mirror unclaimed desktop files as public floats when shell icons are hidden."""
    if not settings.get("hide_shell_icons"):
        return False
    wanted = scan_loose_desktop_wanted(settings)
    return apply_loose_desktop_scan(settings, wanted)


def sync_loose_desktop_items(settings: dict, *, force: bool = False) -> bool:
    """Host unclaimed desktop files/folders as page-local floats.

    Virtual mode hides Explorer icons; unclaimed items would otherwise vanish.
    New items land on the *current* page so Save As / drops are visible
    immediately; they stay there until the user drags them or organize pins
    them into a fence.

    Throttled: repeated public refreshes (page switch / keepalive) must not
    rescan the desktop on the UI thread every time.
    """
    now = time.monotonic()
    sid = id(settings)
    if (
        not force
        and _loose_sync_cache["sid"] == sid
        and now - float(_loose_sync_cache["t"]) < _LOOSE_SYNC_TTL_S
    ):
        return False

    wanted = scan_loose_desktop_wanted(settings)
    changed = apply_loose_desktop_scan(settings, wanted)
    _loose_sync_cache["sid"] = sid
    _loose_sync_cache["t"] = time.monotonic()
    _loose_sync_cache["changed"] = changed
    return changed


def find_public_entry(settings: dict, path: Path | str) -> dict | None:
    key = _norm_key(path).casefold()
    for entry in get_public_items(settings):
        if not isinstance(entry, dict):
            continue
        raw = entry.get("path")
        if raw and _norm_key(str(raw)).casefold() == key:
            return entry
    return None


def _work_area(hint_x: int | None = None, hint_y: int | None = None):
    """Available geometry of the target screen (primary, or screen at hint)."""
    from PyQt6.QtCore import QPoint
    from PyQt6.QtGui import QGuiApplication

    screen = None
    if hint_x is not None and hint_y is not None:
        screen = QGuiApplication.screenAt(QPoint(int(hint_x), int(hint_y)))
    if screen is None:
        screen = QGuiApplication.primaryScreen()
    if screen is None:
        from PyQt6.QtCore import QRect

        return QRect(0, 0, 1920, 1080)
    return screen.availableGeometry()


def collect_fence_rects(
    settings: dict,
    page_id: int,
    live_fences: list | None = None,
):
    """QRect list for fences that currently cover the public desktop."""
    from PyQt6.QtCore import QRect

    from src.fence_layout import get_fence_geometry
    from src.fence_pages import fence_on_page

    rects: list = []
    if live_fences:
        for fence in live_fences:
            try:
                if fence.isVisible():
                    geo = fence.frameGeometry()
                    rects.append(
                        QRect(geo).adjusted(-FENCE_PAD, -FENCE_PAD, FENCE_PAD, FENCE_PAD)
                    )
            except RuntimeError:
                continue
        if rects:
            return rects

    for fence in settings.get("fences", []):
        if not isinstance(fence, dict) or not fence.get("visible", True):
            continue
        if not fence_on_page(fence, page_id):
            continue
        geom = get_fence_geometry(settings, fence, page_id)
        rects.append(
            QRect(
                int(geom["x"]) - FENCE_PAD,
                int(geom["y"]) - FENCE_PAD,
                int(geom["width"]) + FENCE_PAD * 2,
                int(geom["height"]) + FENCE_PAD * 2,
            )
        )
    return rects


def iter_grid_slots(area):
    """Windows-like: fill a column top→bottom, then next column to the right."""
    from PyQt6.QtCore import QRect

    x = area.left() + MARGIN_X
    right_limit = area.right() - MARGIN_X
    bottom_limit = area.bottom() - MARGIN_Y
    while x + CELL_W <= right_limit:
        y = area.top() + MARGIN_Y
        while y + CELL_H <= bottom_limit:
            yield QRect(x, y, CELL_W, CELL_H)
            y += CELL_H + GAP_Y
        x += CELL_W + GAP_X


def _slot_blocked(slot, fence_rects) -> bool:
    for rect in fence_rects:
        if slot.intersects(rect):
            return True
    return False


def _occupied_slots(
    settings: dict,
    *,
    page_id: int = 0,
    exclude_path: Path | str | None = None,
) -> set[tuple[int, int]]:
    """Snap currently visible float positions onto the grid for occupancy."""
    exclude = _norm_key(exclude_path).casefold() if exclude_path is not None else None
    occupied: set[tuple[int, int]] = set()
    for entry in visible_floating_items(settings, page_id):
        if exclude and _norm_key(str(entry["path"])).casefold() == exclude:
            continue
        try:
            x = int(entry.get("x", 0))
            y = int(entry.get("y", 0))
        except (TypeError, ValueError):
            continue
        occupied.add((x, y))
        # Snap relative to this float's work-area origin (multi-mon safe).
        area = _work_area(x, y)
        occupied.add(
            (
                _snap_coord(x, CELL_W + GAP_X, area.left() + MARGIN_X),
                _snap_coord(y, CELL_H + GAP_Y, area.top() + MARGIN_Y),
            )
        )
    return occupied


def _snap_coord(value: int, step: int, margin: int) -> int:
    if step <= 0:
        return value
    # Relative to margin origin on the screen is handled by comparing slot tops.
    return int(round((value - margin) / step) * step + margin)


def find_auto_slot(
    settings: dict,
    *,
    page_id: int = 0,
    hint_x: int | None = None,
    hint_y: int | None = None,
    exclude_path: Path | str | None = None,
    fence_rects: list | None = None,
    prefer_nearest: bool = False,
) -> tuple[int, int]:
    """Pick a free grid cell outside fences.

    Default: first free slot in column-major order (auto-arrange).
    prefer_nearest: closest free slot to hint (public-area drag snap).

    Streams the grid once — never materializes every free cell (large
    multi-monitor work areas used to hitch fence→public unpin).
    """
    from PyQt6.QtCore import QPoint

    area = _work_area(hint_x, hint_y)
    if fence_rects is None:
        fence_rects = collect_fence_rects(settings, page_id)
    occupied = _occupied_slots(
        settings, page_id=page_id, exclude_path=exclude_path
    )
    fallback = (area.left() + MARGIN_X, area.top() + MARGIN_Y)

    def _taken(slot) -> bool:
        # Occupied already stores raw + snapped cell tops — O(1) membership.
        return (int(slot.x()), int(slot.y())) in occupied

    if prefer_nearest and hint_x is not None and hint_y is not None:
        hint = QPoint(int(hint_x), int(hint_y))
        best = None
        best_dist: int | None = None
        for slot in iter_grid_slots(area):
            if _taken(slot) or _slot_blocked(slot, fence_rects):
                continue
            if slot.contains(hint):
                return slot.x(), slot.y()
            dist = (slot.center() - hint).manhattanLength()
            if best_dist is None or dist < best_dist:
                best_dist = dist
                best = slot
        if best is not None:
            return best.x(), best.y()
        return fallback

    for slot in iter_grid_slots(area):
        if _taken(slot) or _slot_blocked(slot, fence_rects):
            continue
        return slot.x(), slot.y()
    return fallback


def add_fence_unpin_public_item(
    settings: dict,
    path: Path | str,
    *,
    page_id: int = 0,
    fence_rects: list | None = None,
    monitor_hint_x: int | None = None,
    monitor_hint_y: int | None = None,
) -> dict:
    """Place a file dragged out of a fence on the left-edge desktop grid.

    Fills column-major from the screen's left margin (Windows-like). Slots that
    intersect fence frames are skipped so floats never cover partitions.
    *monitor_hint_x/y* only pick the target monitor — not the cell position.
    """
    return add_public_item(
        settings,
        path,
        x=monitor_hint_x,
        y=monitor_hint_y,
        page_id=page_id,
        fence_rects=fence_rects,
        auto_arrange=True,
        prefer_nearest=False,
    )


def add_public_item(
    settings: dict,
    path: Path | str,
    x: int | None = None,
    y: int | None = None,
    *,
    page_id: int = 0,
    fence_rects: list | None = None,
    auto_arrange: bool = True,
    shared: bool | None = None,
    prefer_nearest: bool = False,
) -> dict:
    """Pin a desktop path as a floating icon outside fences.

    `shared=True` (default when public area is enabled): visible on every page.
    `shared=False`: page-local float for `page_id` only.
    `prefer_nearest`: when auto-arranging with a hint, snap to closest free slot
    (manual public-area drag). Fence→public unpin uses left-edge via
    ``add_fence_unpin_public_item``.
    """
    if shared is None:
        shared = is_public_desktop_enabled(settings)
    key = _norm_key(path)
    items = get_public_items(settings)
    items[:] = [
        e
        for e in items
        if not (
            isinstance(e, dict)
            and e.get("path")
            and _norm_key(str(e["path"])).casefold() == key.casefold()
        )
    ]
    if auto_arrange or x is None or y is None:
        sx, sy = find_auto_slot(
            settings,
            page_id=page_id,
            hint_x=x,
            hint_y=y,
            exclude_path=path,
            fence_rects=fence_rects,
            prefer_nearest=prefer_nearest,
        )
    else:
        sx, sy = int(x), int(y)
    entry: dict = {"path": key, "x": sx, "y": sy}
    if not shared:
        entry["page"] = int(page_id)
    items.append(entry)
    return entry


def snap_public_item(
    settings: dict,
    path: Path | str,
    hint_x: int,
    hint_y: int,
    *,
    page_id: int = 0,
    fence_rects: list | None = None,
) -> tuple[int, int] | None:
    """Snap an existing public icon to the nearest free grid slot."""
    entry = find_public_entry(settings, path)
    if entry is None:
        return None
    sx, sy = find_auto_slot(
        settings,
        page_id=page_id,
        hint_x=hint_x,
        hint_y=hint_y,
        exclude_path=path,
        fence_rects=fence_rects,
        prefer_nearest=True,
    )
    entry["x"] = sx
    entry["y"] = sy
    return sx, sy


def relayout_public_items(
    settings: dict,
    *,
    page_id: int = 0,
    fence_rects: list | None = None,
) -> bool:
    """Re-pack floating icons visible on `page_id`; leave other pages untouched."""
    visible = visible_floating_items(settings, page_id)
    if not visible:
        return False
    if fence_rects is None:
        fence_rects = collect_fence_rects(settings, page_id)

    area = _work_area()
    occupied: set[tuple[int, int]] = set()
    rebuilt_visible: list[dict] = []
    changed = False
    visible_keys: set[str] = set()

    for old in visible:
        path = str(old["path"])
        try:
            visible_keys.add(_norm_key(path).casefold())
        except OSError:
            visible_keys.add(path.casefold())
        placed = None
        for slot in iter_grid_slots(area):
            key = (slot.x(), slot.y())
            if key in occupied:
                continue
            if _slot_blocked(slot, fence_rects):
                continue
            placed = slot
            occupied.add(key)
            break
        if placed is None:
            x = area.left() + MARGIN_X
            y = area.top() + MARGIN_Y
        else:
            x, y = placed.x(), placed.y()
        try:
            old_x, old_y = int(old.get("x", 0)), int(old.get("y", 0))
        except (TypeError, ValueError):
            old_x, old_y = -1, -1
        if old_x != x or old_y != y:
            changed = True
        # Keep flags (ephemeral / shared page) — only refresh geometry.
        entry = dict(old) if isinstance(old, dict) else {}
        entry["path"] = _norm_key(path)
        entry["x"] = x
        entry["y"] = y
        if not is_shared_public_entry(old):
            try:
                entry["page"] = int(old.get("page"))
            except (TypeError, ValueError):
                entry["page"] = int(page_id)
        rebuilt_visible.append(entry)

    others: list[dict] = []
    for old in get_public_items(settings):
        if not isinstance(old, dict) or not old.get("path"):
            others.append(old)
            continue
        try:
            key = _norm_key(str(old["path"])).casefold()
        except OSError:
            key = str(old["path"]).casefold()
        if key in visible_keys:
            continue
        others.append(old)

    get_public_items(settings)[:] = others + rebuilt_visible
    return changed

def clear_ephemeral_public_items(
    settings: dict, *, page_id: int | None = None
) -> bool:
    """Remove auto-mirrored floats used on pages that have no fences."""
    items = get_public_items(settings)
    before = len(items)
    kept: list[dict] = []
    for entry in items:
        if not isinstance(entry, dict):
            kept.append(entry)
            continue
        if entry.get("ephemeral"):
            if page_id is None:
                continue
            try:
                if int(entry.get("page")) == int(page_id):
                    continue
            except (TypeError, ValueError):
                continue
        kept.append(entry)
    items[:] = kept
    return len(items) != before


def rewrite_public_path(settings: dict, old: Path | str, new: Path | str) -> bool:
    """Update a public/page-local float path after filesystem rename."""
    try:
        old_key = _norm_key(old).casefold().replace("/", "\\")
        new_key = _norm_key(new)
    except OSError:
        return False
    items = get_public_items(settings)
    changed = False
    for entry in items:
        if not isinstance(entry, dict) or not entry.get("path"):
            continue
        try:
            key = _norm_key(str(entry["path"])).casefold().replace("/", "\\")
        except OSError:
            key = str(entry["path"]).casefold().replace("/", "\\")
        if key == old_key:
            entry["path"] = new_key
            changed = True
    return changed


def remove_public_paths(settings: dict, paths: list[Path | str]) -> bool:
    """Drop public entries by exact path key (slash/case normalized).

    Never match by basename — two ``report.txt`` floats must not collide.
    """
    if not paths:
        return False
    remove: set[str] = set()
    for p in paths:
        text = str(p)
        remove.add(text.casefold().replace("/", "\\"))
        try:
            remove.add(_norm_key(text).casefold().replace("/", "\\"))
        except OSError:
            pass
    items = get_public_items(settings)
    before = len(items)
    kept: list[dict] = []
    for e in items:
        if not isinstance(e, dict) or not e.get("path"):
            kept.append(e)
            continue
        raw = str(e["path"])
        key = raw.casefold().replace("/", "\\")
        try:
            key_norm = _norm_key(raw).casefold().replace("/", "\\")
        except OSError:
            key_norm = key
        if key in remove or key_norm in remove:
            continue
        kept.append(e)
    items[:] = kept
    return len(items) != before


def move_path_to_desktop_page(
    settings: dict,
    path: Path | str,
    page_id: int,
    *,
    hint_x: int | None = None,
    hint_y: int | None = None,
) -> bool:
    """Unpin from fences and place as a page-local float on ``page_id``.

    Used by icon RMB「移动到分页」. Returns True when settings changed.
    """
    from src.fence_rules import unpin_paths_from_virtual_fence

    try:
        target = Path(path)
    except OSError:
        return False
    try:
        page_id = int(page_id)
    except (TypeError, ValueError):
        return False
    if is_system_namespace_path(target):
        # System icons stay shared / fence-hosted — not page-local.
        return False

    changed = False
    for fence in list(settings.get("fences") or []):
        if not isinstance(fence, dict):
            continue
        if unpin_paths_from_virtual_fence(fence, [target]):
            changed = True

    entry = find_public_entry(settings, target)
    if entry is not None:
        before_page = entry.get("page")
        before_shared = bool(entry.get("shared"))
        entry["page"] = page_id
        entry["shared"] = False
        entry.pop("loose", None)
        if hint_x is not None and hint_y is not None:
            entry["x"] = int(hint_x)
            entry["y"] = int(hint_y)
        if before_page != page_id or before_shared:
            changed = True
        elif hint_x is not None:
            changed = True
        return changed

    add_public_item(
        settings,
        target,
        x=hint_x,
        y=hint_y,
        page_id=page_id,
        fence_rects=live_fence_rects(settings, page_id),
        auto_arrange=True,
        shared=False,
        prefer_nearest=hint_x is not None and hint_y is not None,
    )
    return True


def desktop_pages_for_move_menu(settings: dict) -> list[tuple[int, str]]:
    """``(page_id, label)`` pairs for RMB move submenu (empty if ≤1 page)."""
    pages = settings.get("desktop_pages") or []
    if not isinstance(pages, list) or len(pages) <= 1:
        return []
    out: list[tuple[int, str]] = []
    for page in pages:
        if not isinstance(page, dict):
            continue
        try:
            pid = int(page.get("id", 0))
        except (TypeError, ValueError):
            continue
        label = str(page.get("name") or f"分页 {pid}").strip() or f"分页 {pid}"
        out.append((pid, label))
    return out if len(out) > 1 else []


def fences_for_move_menu(
    settings: dict,
    *,
    page_id: int | None = None,
    source_fence_id: str | None = None,
) -> list[tuple[str, str]]:
    """``(fence_id, label)`` pairs for RMB「移动到分区」on the given page."""
    from src.fence_pages import get_fence_pages
    from src.fence_rules import is_portal_fence

    if page_id is None:
        try:
            page_id = int(settings.get("current_page", 0))
        except (TypeError, ValueError):
            page_id = 0
    source = str(source_fence_id or "").strip()
    out: list[tuple[str, str]] = []
    for fence in settings.get("fences") or []:
        if not isinstance(fence, dict):
            continue
        fid = str(fence.get("id") or "").strip()
        if not fid or fid == source:
            continue
        if is_portal_fence(fence):
            continue
        try:
            pages = get_fence_pages(fence)
        except Exception:
            pages = []
        if int(page_id) not in pages:
            continue
        label = str(fence.get("name") or "分区").strip() or "分区"
        out.append((fid, label))
    return out


def move_paths_to_fence(
    settings: dict, paths: list[Path | str], fence_id: str
) -> bool:
    """Pin *paths* into *fence_id*, unpinning other fences / public floats.

    Used by icon RMB「移动到分区」so multi-select moves together.
    """
    from src.fence_rules import assign_paths_to_virtual_fence, is_portal_fence

    batch: list[Path] = []
    for raw in paths or []:
        try:
            target = Path(raw)
        except OSError:
            continue
        if is_system_namespace_path(target):
            continue
        batch.append(target)
    if not batch:
        return False
    fence_id = str(fence_id or "").strip()
    if not fence_id:
        return False
    fence = None
    for item in settings.get("fences") or []:
        if isinstance(item, dict) and str(item.get("id") or "") == fence_id:
            fence = item
            break
    if fence is None or is_portal_fence(fence):
        return False
    added = assign_paths_to_virtual_fence(fence, settings, batch)
    return bool(added)


def move_path_to_fence(settings: dict, path: Path | str, fence_id: str) -> bool:
    """Pin *path* into *fence_id*, unpinning other fences / public floats."""
    return move_paths_to_fence(settings, [path], fence_id)


def move_paths_to_desktop_page(
    settings: dict,
    paths: list[Path | str],
    page_id: int,
    *,
    hint_x: int | None = None,
    hint_y: int | None = None,
) -> bool:
    """Unpin *paths* from fences and place as page-local floats on ``page_id``."""
    changed = False
    for i, raw in enumerate(paths or []):
        hx = hint_x
        hy = hint_y
        if hx is not None and hy is not None and i > 0:
            # Fan out slightly so multi-moved floats are not stacked.
            hx = int(hx) + i * 16
            hy = int(hy) + i * 16
        if move_path_to_desktop_page(
            settings, raw, page_id, hint_x=hx, hint_y=hy
        ):
            changed = True
    return changed


def set_public_item_pos(settings: dict, path: Path | str, x: int, y: int) -> bool:
    entry = find_public_entry(settings, path)
    if entry is None:
        return False
    entry["x"] = int(x)
    entry["y"] = int(y)
    return True


def prune_missing_public_items(
    settings: dict,
    *,
    force: bool = False,
    sticky_missing_s: float | None = None,
) -> bool:
    """Drop public entries whose files have been missing long enough.

    Throttled: Path.exists on every float every refresh is expensive for a
    lightweight tray app. Watcher / force still runs immediately.

    Floats stay sticky across brief gaps (Excel/Word save) — only sustained
    absence removes the entry, matching fence virtual pins.
    """
    now = time.monotonic()
    if not force and now - float(_prune_missing_cache.get("t", 0.0)) < _PRUNE_MISSING_TTL_S:
        return False
    grace = float(_STICKY_MISSING_S if sticky_missing_s is None else sticky_missing_s)
    grace = max(0.0, grace)
    items = get_public_items(settings)
    before = len(items)
    kept: list[dict] = []
    live_keys: set[str] = set()
    for entry in items:
        if not isinstance(entry, dict):
            continue
        raw = entry.get("path")
        if not raw:
            continue
        p = Path(str(raw))
        from src.path_stat_cache import path_present

        try:
            key = str(p).casefold()
            ok = path_present(p)
        except OSError:
            key = str(p).casefold()
            ok = False
        if ok:
            _missing_since.pop(key, None)
            kept.append(entry)
            live_keys.add(key)
            continue
        first = _missing_since.get(key)
        if first is None:
            _missing_since[key] = now
            kept.append(entry)
            live_keys.add(key)
            continue
        if now - float(first) < grace:
            kept.append(entry)
            live_keys.add(key)
            continue
        # Sustained absence — drop float.
    for stale in list(_missing_since):
        if stale not in live_keys:
            _missing_since.pop(stale, None)
    items[:] = kept
    changed = len(items) != before
    # Stamp throttle only when we actually dropped something, or on a normal
    # (non-force) scan. A force scan that finds everything still present must
    # not block a soon-after real delete from being pruned.
    if changed or not force:
        _prune_missing_cache["t"] = now
    return changed
