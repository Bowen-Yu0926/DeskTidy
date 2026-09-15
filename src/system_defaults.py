"""Built-in locked work/docs pages + software/docs fences (cannot be deleted)."""

from __future__ import annotations

from src.fence_pages import get_fence_pages, set_fence_pages

SYSTEM_WORK_PAGE_ID = 0
SYSTEM_WORK_PAGE_NAME = "工作"
SYSTEM_DOCS_PAGE_ID = 1
SYSTEM_DOCS_PAGE_NAME = "文档"

SYSTEM_COMMON_FENCE_ID = "system_common"
SYSTEM_COMMON_FENCE_NAME = "常用"
SYSTEM_COMMON_FENCE_LEGACY_NAMES = frozenset({"常用程序", "软件"})

SYSTEM_SOFTWARE_SIDE_FENCE_ID = "system_software_side"
SYSTEM_SOFTWARE_SIDE_FENCE_NAME = "其他"
SYSTEM_SOFTWARE_SIDE_FENCE_LEGACY_NAMES = frozenset({"常用"})

SYSTEM_DOCS_FENCE_ID = "system_docs"
SYSTEM_DOCS_FENCE_NAME = "文档"

# Product default sizes — 常用 ~2/3 + gap + 其他 ~1/3 of the primary work width
# (docs full-width on page 1). Height ratios still keyed to a 1920×1032 canvas.
_REF_WORK_W = 1920
_REF_WORK_H = 1032
_REF_WORK_GAP = 4
_SOFT_FALLBACK_W = int(round(_REF_WORK_W * 2 / 3))  # 1280
_SOFT_FALLBACK = {"x": 0, "y": 0, "width": _SOFT_FALLBACK_W, "height": 315}
_SOFT_SIDE_FALLBACK = {
    "x": _SOFT_FALLBACK["x"] + _SOFT_FALLBACK["width"] + _REF_WORK_GAP,
    "y": 0,
    "width": _REF_WORK_W - _SOFT_FALLBACK["width"] - _REF_WORK_GAP,
    "height": 315,
}
_DOCS_FALLBACK = {"x": 0, "y": 0, "width": 1920, "height": 257}
_SOFT_RW = _SOFT_FALLBACK["width"] / _REF_WORK_W
_SOFT_RH = _SOFT_FALLBACK["height"] / _REF_WORK_H
_DOCS_RW = _DOCS_FALLBACK["width"] / _REF_WORK_W
_DOCS_RH = _DOCS_FALLBACK["height"] / _REF_WORK_H
# Pre-2/3 layout used ~84% / 16% (1609 / 307 on 1920) — heal still recognizes it.
_LEGACY_SOFT_RW = 1609 / 1920
_LEGACY_SOFT_W = 1609
_LEGACY_SIDE_X = 1613

_FENCE_STYLE = {
    "opacity": 0.94,
    "background": "#FFFFFF",
    "accent": "#3B82F6",
    "border_radius": 14,
    "show_title": True,
    "view_mode": "grid",
    "collapsible": True,
}


def _default_fence_geometry(*, rw: float, rh: float, fallback: dict) -> dict:
    """Scale first-install fence size to the primary work area; else use fallback px."""
    try:
        from PyQt6.QtGui import QGuiApplication

        screen = QGuiApplication.primaryScreen()
        if screen is not None:
            area = screen.availableGeometry()
            aw = max(1, int(area.width()))
            ah = max(1, int(area.height()))
            w = max(160, min(int(round(rw * aw)), aw))
            h = max(120, min(int(round(rh * ah)), ah))
            return {
                "x": int(area.x()),
                "y": int(area.y()),
                "width": w,
                "height": h,
            }
    except Exception:
        pass
    return {
        "x": int(fallback["x"]),
        "y": int(fallback["y"]),
        "width": int(fallback["width"]),
        "height": int(fallback["height"]),
    }


def _default_work_row_geometries() -> tuple[dict, dict]:
    """Left 常用 + right 其他 side-by-side on the work page (fills work width)."""
    try:
        from PyQt6.QtGui import QGuiApplication

        screen = QGuiApplication.primaryScreen()
        if screen is not None:
            area = screen.availableGeometry()
            aw = max(1, int(area.width()))
            ah = max(1, int(area.height()))
            gap = max(4, int(round(_REF_WORK_GAP * aw / _REF_WORK_W)))
            h = max(120, min(int(round(_SOFT_RH * ah)), ah))
            left_w = max(160, min(int(round(_SOFT_RW * aw)), aw - gap - 120))
            right_w = max(120, aw - left_w - gap)
            x0 = int(area.x())
            y0 = int(area.y())
            return (
                {"x": x0, "y": y0, "width": left_w, "height": h},
                {"x": x0 + left_w + gap, "y": y0, "width": right_w, "height": h},
            )
    except Exception:
        pass
    return (
        {
            "x": int(_SOFT_FALLBACK["x"]),
            "y": int(_SOFT_FALLBACK["y"]),
            "width": int(_SOFT_FALLBACK["width"]),
            "height": int(_SOFT_FALLBACK["height"]),
        },
        {
            "x": int(_SOFT_SIDE_FALLBACK["x"]),
            "y": int(_SOFT_SIDE_FALLBACK["y"]),
            "width": int(_SOFT_SIDE_FALLBACK["width"]),
            "height": int(_SOFT_SIDE_FALLBACK["height"]),
        },
    )


def is_locked_page(page: dict | None) -> bool:
    if not isinstance(page, dict):
        return False
    if page.get("locked"):
        return True
    try:
        return int(page.get("id", -1)) in {SYSTEM_WORK_PAGE_ID, SYSTEM_DOCS_PAGE_ID}
    except (TypeError, ValueError):
        return False


def is_locked_fence(fence: dict | None) -> bool:
    if not isinstance(fence, dict):
        return False
    if fence.get("locked"):
        return True
    return str(fence.get("id") or "") in {SYSTEM_COMMON_FENCE_ID, SYSTEM_DOCS_FENCE_ID}


def is_locked_page_id(page_id: int | str | None) -> bool:
    try:
        return int(page_id) in {SYSTEM_WORK_PAGE_ID, SYSTEM_DOCS_PAGE_ID}
    except (TypeError, ValueError):
        return False


def locked_fence_home_page(fence: dict | None) -> int | None:
    """Home page a locked system fence must stay on."""
    if not isinstance(fence, dict):
        return None
    fid = str(fence.get("id") or "")
    if fid == SYSTEM_COMMON_FENCE_ID:
        return SYSTEM_WORK_PAGE_ID
    if fid == SYSTEM_DOCS_FENCE_ID:
        return SYSTEM_DOCS_PAGE_ID
    return None


def default_software_fence() -> dict:
    geom, _side = _default_work_row_geometries()
    return {
        "id": SYSTEM_COMMON_FENCE_ID,
        "name": SYSTEM_COMMON_FENCE_NAME,
        "folder": SYSTEM_COMMON_FENCE_NAME,
        "locked": True,
        "organize_kinds": ["icon"],
        "extensions": [],
        "filename_patterns": [],
        "page": SYSTEM_WORK_PAGE_ID,
        "pages": [SYSTEM_WORK_PAGE_ID],
        "sort_by": "name",
        "x": geom["x"],
        "y": geom["y"],
        "width": geom["width"],
        "height": geom["height"],
        "visible": True,
        "collapsed": False,
        "virtual_items": [],
        "style": dict(_FENCE_STYLE),
    }


def default_software_side_fence() -> dict:
    _main, geom = _default_work_row_geometries()
    return {
        "id": SYSTEM_SOFTWARE_SIDE_FENCE_ID,
        "name": SYSTEM_SOFTWARE_SIDE_FENCE_NAME,
        "folder": SYSTEM_SOFTWARE_SIDE_FENCE_NAME,
        "locked": False,
        "organize_kinds": [],
        "extensions": [],
        "filename_patterns": [],
        "page": SYSTEM_WORK_PAGE_ID,
        "pages": [SYSTEM_WORK_PAGE_ID],
        "sort_by": "name",
        "x": geom["x"],
        "y": geom["y"],
        "width": geom["width"],
        "height": geom["height"],
        "visible": True,
        "collapsed": False,
        "virtual_items": [],
        "style": {
            **_FENCE_STYLE,
            "accent": "#6366F1",
            "background": "#F8FAFC",
        },
    }


def default_docs_fence() -> dict:
    geom = _default_fence_geometry(
        rw=_DOCS_RW, rh=_DOCS_RH, fallback=_DOCS_FALLBACK
    )
    return {
        "id": SYSTEM_DOCS_FENCE_ID,
        "name": SYSTEM_DOCS_FENCE_NAME,
        "folder": SYSTEM_DOCS_FENCE_NAME,
        "locked": True,
        "organize_kinds": ["file"],
        "extensions": [],
        "filename_patterns": [],
        "page": SYSTEM_DOCS_PAGE_ID,
        "pages": [SYSTEM_DOCS_PAGE_ID],
        "sort_by": "name",
        "x": geom["x"],
        "y": geom["y"],
        "width": geom["width"],
        "height": geom["height"],
        "visible": True,
        "collapsed": False,
        "virtual_items": [],
        "style": {
            **_FENCE_STYLE,
            "accent": "#D97706",
            "background": "#FFFBEB",
        },
    }


# Back-compat alias used by older imports / tests.
def default_common_fence() -> dict:
    return default_software_fence()


def _find_page(pages: list, page_id: int) -> dict | None:
    for page in pages:
        if not isinstance(page, dict):
            continue
        try:
            if int(page.get("id", -1)) == page_id:
                return page
        except (TypeError, ValueError):
            continue
    return None


def _ensure_page(
    pages: list,
    *,
    page_id: int,
    name: str,
    kinds: list[str],
    insert_at: int | None = None,
) -> tuple[dict, bool]:
    # ``kinds`` kept for call-site compatibility; pages no longer store rules.
    _ = kinds
    changed = False
    page = _find_page(pages, page_id)
    if page is None:
        page = {
            "id": page_id,
            "name": name,
            "locked": True,
        }
        if insert_at is None or insert_at >= len(pages):
            pages.append(page)
        else:
            pages.insert(insert_at, page)
        return page, True
    if not page.get("locked"):
        page["locked"] = True
        changed = True
    if not str(page.get("name") or "").strip():
        page["name"] = name
        changed = True
    if "organize_kinds" in page:
        page.pop("organize_kinds", None)
        changed = True
    return page, changed


def _find_fence(
    fences: list,
    fence_id: str,
    legacy_names: frozenset[str] | None = None,
    *,
    skip_ids: frozenset[str] | None = None,
) -> dict | None:
    skip = skip_ids or frozenset()
    for fence in fences:
        if not isinstance(fence, dict):
            continue
        if str(fence.get("id") or "") == fence_id:
            return fence
    if legacy_names:
        for fence in fences:
            if not isinstance(fence, dict):
                continue
            fid = str(fence.get("id") or "")
            if fid in skip or fid == fence_id:
                continue
            if str(fence.get("name") or "").strip() in legacy_names:
                return fence
    return None


def _heal_system_fence(
    fence: dict,
    *,
    fence_id: str,
    name: str,
    home_page: int,
    kinds: list[str],
    old_default_kinds: tuple[tuple[str, ...], ...] = (),
) -> bool:
    changed = False
    if str(fence.get("id") or "") != fence_id:
        fence["id"] = fence_id
        changed = True
    if not fence.get("locked"):
        fence["locked"] = True
        changed = True
    current_name = str(fence.get("name") or "").strip()
    if fence_id == SYSTEM_COMMON_FENCE_ID and current_name in {"", "常用程序", "软件"}:
        if current_name != name:
            fence["name"] = name
            fence["folder"] = name
            changed = True
    elif not current_name:
        fence["name"] = name
        fence["folder"] = name
        changed = True
    page_ids = get_fence_pages(fence)
    if home_page not in page_ids or page_ids != [home_page]:
        # System fences are exclusive to their home page.
        set_fence_pages(fence, [home_page])
        changed = True
    raw_kinds = fence.get("organize_kinds")
    kind_tuple = tuple(raw_kinds) if isinstance(raw_kinds, list) else ()
    if not kind_tuple or kind_tuple in old_default_kinds:
        if list(raw_kinds or []) != list(kinds):
            fence["organize_kinds"] = list(kinds)
            changed = True
    return changed


def apply_system_fence_geometry_from_display(settings: dict) -> bool:
    """Snap locked system fences to the primary work area (first install / fresh display)."""
    changed = False
    fences = settings.get("fences")
    if not isinstance(fences, list):
        return False

    left_geom, right_geom = _default_work_row_geometries()
    row_specs = (
        (SYSTEM_COMMON_FENCE_ID, SYSTEM_COMMON_FENCE_LEGACY_NAMES, left_geom),
        (SYSTEM_SOFTWARE_SIDE_FENCE_ID, SYSTEM_SOFTWARE_SIDE_FENCE_LEGACY_NAMES, right_geom),
    )
    for fence_id, legacy_names, geom in row_specs:
        fence = _find_fence(fences, fence_id, legacy_names)
        if fence is None:
            continue
        for key in ("x", "y", "width", "height"):
            if int(fence.get(key, -1)) != int(geom[key]):
                fence[key] = geom[key]
                changed = True

    docs_geom = _default_fence_geometry(
        rw=_DOCS_RW, rh=_DOCS_RH, fallback=_DOCS_FALLBACK
    )
    docs_fence = _find_fence(
        fences, SYSTEM_DOCS_FENCE_ID, frozenset({SYSTEM_DOCS_FENCE_NAME})
    )
    if docs_fence is not None:
        for key in ("x", "y", "width", "height"):
            if int(docs_fence.get(key, -1)) != int(docs_geom[key]):
                docs_fence[key] = docs_geom[key]
                changed = True
    return changed


def _clear_system_work_row_profiles(settings: dict) -> None:
    """Drop saved 常用/其他 entries so the next apply re-seeds from fence cfg."""
    profiles = settings.get("fence_layouts_by_display")
    if not isinstance(profiles, dict):
        return
    ids = {SYSTEM_COMMON_FENCE_ID, SYSTEM_SOFTWARE_SIDE_FENCE_ID}
    for profile in profiles.values():
        if not isinstance(profile, dict):
            continue
        for page_key in (str(SYSTEM_WORK_PAGE_ID), SYSTEM_WORK_PAGE_ID):
            page_map = profile.get(page_key)
            if not isinstance(page_map, dict):
                continue
            for fid in list(page_map.keys()):
                if str(fid) in ids:
                    page_map.pop(fid, None)
    legacy = settings.get("fence_layouts_by_page")
    if isinstance(legacy, dict):
        page_map = legacy.get(str(SYSTEM_WORK_PAGE_ID))
        if isinstance(page_map, dict):
            for fid in ids:
                page_map.pop(fid, None)


def _heal_crowded_work_row_geometries(settings: dict, fences: list) -> bool:
    """Rewrite 常用/其他 when crowded, legacy 84/16 stock, or overflow on laptop."""
    software = _find_fence(fences, SYSTEM_COMMON_FENCE_ID, SYSTEM_COMMON_FENCE_LEGACY_NAMES)
    side = _find_fence(
        fences,
        SYSTEM_SOFTWARE_SIDE_FENCE_ID,
        SYSTEM_SOFTWARE_SIDE_FENCE_LEGACY_NAMES,
        skip_ids=frozenset({SYSTEM_COMMON_FENCE_ID}),
    )
    if software is None or side is None:
        return False

    try:
        from src.fence_layout import (
            _geometry_from_fence,
            _rects_overlap_heavily,
            geometry_from_entry,
            primary_desktop_rect,
        )
    except Exception:
        return False

    area = primary_desktop_rect()
    left = geometry_from_entry(_geometry_from_fence(software))
    right = geometry_from_entry(_geometry_from_fence(side))
    needs = _rects_overlap_heavily(left, right)
    try:
        raw_left_w = int(software.get("width") or 0)
        raw_side_x = int(side.get("x") or 0)
        raw_rw = float(software.get("rw") or 0)
    except (TypeError, ValueError):
        raw_left_w, raw_side_x, raw_rw = 0, 0, 0.0
    # Stock defaults (w≈1609, side x≈1613) on a narrower logical work area.
    if raw_left_w > area.width() + 24 or raw_side_x > area.width():
        needs = True
    # Already-clamped mess: left ate nearly the full width while side remains.
    if left["width"] >= int(area.width() * 0.92) and right["width"] >= 80:
        needs = True
    # Migrate pre-2/3 product default (~84% / 16%) to 常用 2/3 + 其他 1/3.
    left_ratio = left["width"] / max(1, area.width())
    if (
        raw_left_w == _LEGACY_SOFT_W
        or raw_side_x == _LEGACY_SIDE_X
        or abs(raw_rw - _LEGACY_SOFT_RW) < 0.03
        or abs(left_ratio - _LEGACY_SOFT_RW) < 0.04
    ):
        needs = True
    if not needs:
        return False

    left_g, right_g = _default_work_row_geometries()
    software.update(
        {
            "x": left_g["x"],
            "y": left_g["y"],
            "width": left_g["width"],
            "height": left_g["height"],
            "rx": 0.0,
            "ry": 0.0,
            "rw": _SOFT_RW,
            "rh": _SOFT_RH,
            "ref_x": 0,
            "ref_y": 0,
            "ref_w": _REF_WORK_W,
            "ref_h": _REF_WORK_H,
        }
    )
    side.update(
        {
            "x": right_g["x"],
            "y": right_g["y"],
            "width": right_g["width"],
            "height": right_g["height"],
            "rx": (_SOFT_FALLBACK["width"] + _REF_WORK_GAP) / _REF_WORK_W,
            "ry": 0.0,
            "rw": _SOFT_SIDE_FALLBACK["width"] / _REF_WORK_W,
            "rh": _SOFT_RH,
            "ref_x": 0,
            "ref_y": 0,
            "ref_w": _REF_WORK_W,
            "ref_h": _REF_WORK_H,
        }
    )
    _clear_system_work_row_profiles(settings)
    return True


def ensure_system_defaults(settings: dict) -> bool:
    """Guarantee locked 工作/文档 pages + 常用/文档 fences. Returns True if mutated."""
    changed = False

    pages = settings.get("desktop_pages")
    if not isinstance(pages, list):
        pages = []
        settings["desktop_pages"] = pages
        changed = True

    _work, work_changed = _ensure_page(
        pages,
        page_id=SYSTEM_WORK_PAGE_ID,
        name=SYSTEM_WORK_PAGE_NAME,
        kinds=["icon"],
        insert_at=0,
    )
    changed = changed or work_changed

    _docs, docs_changed = _ensure_page(
        pages,
        page_id=SYSTEM_DOCS_PAGE_ID,
        name=SYSTEM_DOCS_PAGE_NAME,
        kinds=["file"],
        insert_at=1 if len(pages) > 0 else 0,
    )
    changed = changed or docs_changed
    # Prefer canonical names for the two system pages when still empty/legacy.
    work_page = _find_page(pages, SYSTEM_WORK_PAGE_ID)
    if work_page is not None and str(work_page.get("name") or "").strip() in {"", "默认"}:
        work_page["name"] = SYSTEM_WORK_PAGE_NAME
        changed = True
    docs_page = _find_page(pages, SYSTEM_DOCS_PAGE_ID)
    if docs_page is not None and not str(docs_page.get("name") or "").strip():
        docs_page["name"] = SYSTEM_DOCS_PAGE_NAME
        changed = True

    fences = settings.get("fences")
    if not isinstance(fences, list):
        fences = []
        settings["fences"] = fences
        changed = True

    software = _find_fence(fences, SYSTEM_COMMON_FENCE_ID, SYSTEM_COMMON_FENCE_LEGACY_NAMES)
    if software is None:
        fences.insert(0, default_software_fence())
        software = fences[0]
        changed = True
    else:
        changed = (
            _heal_system_fence(
                software,
                fence_id=SYSTEM_COMMON_FENCE_ID,
                name=SYSTEM_COMMON_FENCE_NAME,
                home_page=SYSTEM_WORK_PAGE_ID,
                kinds=["icon"],
                old_default_kinds=(("icon", "file"), ("file", "icon")),
            )
            or changed
        )

    def _other_work_page_fences() -> list[dict]:
        out: list[dict] = []
        for fence in fences:
            if not isinstance(fence, dict):
                continue
            if str(fence.get("id") or "") == SYSTEM_COMMON_FENCE_ID:
                continue
            if SYSTEM_WORK_PAGE_ID in get_fence_pages(fence):
                out.append(fence)
        return out

    side = _find_fence(
        fences,
        SYSTEM_SOFTWARE_SIDE_FENCE_ID,
        SYSTEM_SOFTWARE_SIDE_FENCE_LEGACY_NAMES,
        skip_ids=frozenset({SYSTEM_COMMON_FENCE_ID}),
    )
    if side is not None:
        side_name = str(side.get("name") or "").strip()
        if side_name == "常用":
            side["name"] = SYSTEM_SOFTWARE_SIDE_FENCE_NAME
            side["folder"] = SYSTEM_SOFTWARE_SIDE_FENCE_NAME
            changed = True
    if side is None and not _other_work_page_fences():
        insert_at = fences.index(software) + 1 if software in fences else len(fences)
        fences.insert(insert_at, default_software_side_fence())
        changed = True

    docs_fence = _find_fence(fences, SYSTEM_DOCS_FENCE_ID, frozenset({SYSTEM_DOCS_FENCE_NAME}))
    # Only adopt a same-named fence on the docs page as the system docs fence.
    if docs_fence is None:
        for fence in fences:
            if not isinstance(fence, dict):
                continue
            if str(fence.get("name") or "").strip() != SYSTEM_DOCS_FENCE_NAME:
                continue
            if SYSTEM_DOCS_PAGE_ID in get_fence_pages(fence):
                docs_fence = fence
                break
    if docs_fence is None:
        # Place after software fence when present.
        insert_at = 1 if software in fences else len(fences)
        fences.insert(insert_at, default_docs_fence())
        changed = True
    else:
        changed = (
            _heal_system_fence(
                docs_fence,
                fence_id=SYSTEM_DOCS_FENCE_ID,
                name=SYSTEM_DOCS_FENCE_NAME,
                home_page=SYSTEM_DOCS_PAGE_ID,
                kinds=["file"],
                old_default_kinds=(("icon", "file"), ("file", "icon")),
            )
            or changed
        )

    if _heal_crowded_work_row_geometries(settings, fences):
        changed = True

    return changed
