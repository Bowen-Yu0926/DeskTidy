"""Per-page fence layout persistence, keyed by display configuration.

Strategy (Fences-like):
1. Each monitor layout fingerprint has its own layout store.
2. Geometry is saved as pixels + relative ratios of the *host screen*
   (never the virtual-desktop union — that caused dual-monitor spanning).
3. If the current fingerprint has no layout, scale from the last / any profile,
   then clamp onto a single visible screen (primary when a rect spans monitors).
"""

from __future__ import annotations

import uuid
from typing import Any

from src.fence_pages import fence_on_page, get_fence_pages

_MIN_W = 160
_MIN_H = 120
# New-fence defaults: wide enough for title + collapse/refresh/close without clipping.
DEFAULT_NEW_FENCE_WIDTH = 320
DEFAULT_NEW_FENCE_HEIGHT = 360


def ensure_fence_ids(settings: dict) -> None:
    for fence in settings.get("fences", []):
        if not fence.get("id"):
            fence["id"] = uuid.uuid4().hex[:8]


def virtual_desktop_rect():
    """Bounding union of available geometries across all screens.

    Used for overlay *window* size. For mouse hit-testing prefer
    ``virtual_desktop_work_region`` — a bounding QRect can cover another
    monitor's taskbar strip when work-area heights differ.
    """
    from PyQt6.QtCore import QRect
    from PyQt6.QtGui import QGuiApplication

    screens = QGuiApplication.screens()
    if not screens:
        return QRect(0, 0, 1920, 1080)
    area = screens[0].availableGeometry()
    for screen in screens[1:]:
        area = area.united(screen.availableGeometry())
    return area


def virtual_desktop_work_region():
    """QRegion of every screen's availableGeometry (excludes taskbars).

    ``QRect.united`` of available areas is a bounding box: a taller secondary
    work area fills the primary taskbar gap, so a full-desktop click plate
    stole「屏幕底下的程序」. Region union keeps those strips click-through.
    """
    from PyQt6.QtCore import QRect
    from PyQt6.QtGui import QGuiApplication, QRegion

    screens = list(QGuiApplication.screens() or [])
    if not screens:
        return QRegion(QRect(0, 0, 1920, 1080))
    region = QRegion()
    for screen in screens:
        try:
            region = region.united(QRegion(screen.availableGeometry()))
        except RuntimeError:
            continue
    return region


def primary_desktop_rect():
    """Available geometry of the primary monitor."""
    from PyQt6.QtCore import QRect
    from PyQt6.QtGui import QGuiApplication

    screen = QGuiApplication.primaryScreen()
    if screen:
        return screen.availableGeometry()
    return virtual_desktop_rect()


def _target_screen_rect(x: int, y: int, w: int, h: int):
    """Pick a single screen for a fence — never the virtual-desktop union.

    Uses the screen with the largest overlap. If the rect spans multiple
    screens (the dual-monitor full-bleed bug), pin to the primary monitor.
    """
    from PyQt6.QtCore import QPoint, QRect
    from PyQt6.QtGui import QGuiApplication

    screens = list(QGuiApplication.screens() or [])
    primary = QGuiApplication.primaryScreen()
    if not screens:
        return QRect(0, 0, 1920, 1080)

    rect = QRect(int(x), int(y), max(1, int(w)), max(1, int(h)))
    overlaps: list[tuple[int, object]] = []
    for screen in screens:
        area = screen.availableGeometry()
        inter = rect.intersected(area)
        if inter.width() > 0 and inter.height() > 0:
            overlaps.append((inter.width() * inter.height(), area))

    if len(overlaps) >= 2:
        if primary:
            return primary.availableGeometry()
        overlaps.sort(key=lambda item: item[0], reverse=True)
        return overlaps[0][1]

    if overlaps:
        overlaps.sort(key=lambda item: item[0], reverse=True)
        return overlaps[0][1]

    screen = QGuiApplication.screenAt(QPoint(rect.center()))
    if screen:
        return screen.availableGeometry()
    if primary:
        return primary.availableGeometry()
    return screens[0].availableGeometry()


def current_display_fingerprint() -> str:
    """Stable key for the current monitor arrangement + resolutions + physical size.

    Logical resolution alone is not enough: a 15\" laptop and a 27\" external
    can both report 1920×1080. Physical size (mm) distinguishes them so each
    panel keeps its own fence layout.

    Use full screen geometry (not availableGeometry). Taskbar / work-area
    changes would otherwise create a new fingerprint and reset fence layouts.
    """
    from PyQt6.QtGui import QGuiApplication

    screens = list(QGuiApplication.screens() or [])
    if not screens:
        return "0,0,1920x1080@520x290mm"
    parts: list[str] = []
    for screen in sorted(
        screens,
        key=lambda s: (s.geometry().x(), s.geometry().y(), s.name()),
    ):
        g = screen.geometry()
        try:
            ps = screen.physicalSize()
            mm_w = max(1, int(round(float(ps.width()))))
            mm_h = max(1, int(round(float(ps.height()))))
        except Exception:
            mm_w, mm_h = 520, 290
        try:
            dpr = float(screen.devicePixelRatio())
        except Exception:
            dpr = 1.0
        # Bucket DPR so tiny float noise does not fork profiles.
        dpr_tag = f"{dpr:.2f}".rstrip("0").rstrip(".")
        parts.append(
            f"{g.x()},{g.y()},{g.width()}x{g.height()}@{mm_w}x{mm_h}mm@{dpr_tag}"
        )
    return "|".join(parts)


def fingerprint_monitor_count(fp: str) -> int:
    """How many panels a display fingerprint describes."""
    if not fp:
        return 0
    return len([p for p in str(fp).split("|") if p.strip()])


def fingerprint_is_transient(fp: str) -> bool:
    """True for plug/unplug intermediates and Qt offscreen fake screens.

    Offscreen / minimal Qt often reports a square ``800×800`` panel. Saving fence
    layouts under that fingerprint (and copying into the real dual profile on the
    next launch) permanently crushed user sizes — e.g. 1610→800.
    """
    if not fp:
        return False
    for part in str(fp).split("|"):
        part = part.strip()
        if not part:
            continue
        w, h = _fingerprint_primary_pixels(part)
        if 0 < w < 1280:
            return True
        if w > 0 and h > 0 and w == h and w <= 1024:
            return True
    return False


def layout_writes_blocked() -> bool:
    """Refuse to persist layouts while the Qt platform is headless / transient."""
    import os

    if os.environ.get("DESKTIDY_SELFTEST") == "1":
        return False
    plat = (os.environ.get("QT_QPA_PLATFORM") or "").strip().lower()
    if plat in {"offscreen", "minimal", "null"}:
        return True
    return fingerprint_is_transient(current_display_fingerprint())


def prepare_destination_profile_for_display_change(
    settings: dict, old_fp: str, new_fp: str
) -> None:
    """Adjust stored profiles when the monitor set changes.

    Multi→fewer: drop the destination profile so geometries scale from the
    layout the user was just looking at (``last_fp`` / donor). Keeping a stale
    single-screen profile made unplug restore a cramped leftover instead of the
    just-used dual arrangement.

    Fewer→multi / same count: only drop destination when it is empty/corrupt.
    """
    if not new_fp or new_fp == old_fp:
        return
    profiles = settings.setdefault("fence_layouts_by_display", {})
    if not isinstance(profiles, dict) or new_fp not in profiles:
        return
    lost_monitors = fingerprint_monitor_count(old_fp) > fingerprint_monitor_count(
        new_fp
    )
    if lost_monitors or not display_profile_is_healthy(profiles.get(new_fp)):
        profiles.pop(new_fp, None)


def purge_transient_display_profiles(settings: dict) -> None:
    """Drop tiny intermediate fingerprints Windows / offscreen Qt emit.

    Example: brief ``1024x768`` while settling, or Qt offscreen ``800x800``.
    Those keys are not real user layouts and must not win as donors or last_fp.
    """
    profiles = settings.get("fence_layouts_by_display")
    if not isinstance(profiles, dict):
        return
    for key in list(profiles.keys()):
        if fingerprint_is_transient(str(key)):
            profiles.pop(key, None)
    last = str(settings.get("last_display_fingerprint") or "")
    if not fingerprint_is_transient(last):
        return
    current = current_display_fingerprint()
    if current and not fingerprint_is_transient(current):
        settings["last_display_fingerprint"] = current
        return
    donors = [
        k
        for k in profiles
        if not fingerprint_is_transient(str(k)) and display_profile_is_healthy(profiles.get(k))
    ]
    if donors:
        settings["last_display_fingerprint"] = max(
            donors, key=lambda k: _page0_total_rw(profiles.get(k))
        )
    else:
        settings["last_display_fingerprint"] = ""


def _page0_total_rw(profile: dict | None) -> float:
    page = (profile or {}).get("0") or {}
    if not isinstance(page, dict):
        return 0.0
    total = 0.0
    for entry in page.values():
        if not isinstance(entry, dict):
            continue
        try:
            total += float(entry.get("rw") or 0)
        except (TypeError, ValueError):
            continue
    return total


def _heal_offscreen_layout_corruption(settings: dict) -> None:
    """Restore fence sizes crushed by a Qt-offscreen ``800×800`` save cycle.

    The fake screen wrote ~800-wide tiles into the live dual profile and top-level
    fence fields. Recover from the newest settings backup that still has a wide
    page-0 layout (common ≥1200px).

    Do **not** treat bare fence absolute placeholders (often ``width≈400``) as
    crushed — that falsely overwrote healthy multi-monitor profiles from backups
    whenever ``migrate_layouts`` ran on a fresh/test settings dict.
    """
    if settings.get("offscreen_layout_healed_v1"):
        return
    purge_transient_display_profiles(settings)

    def _page0_profile_span(profile: dict | None) -> int:
        page0 = (profile or {}).get("0") or {}
        if not isinstance(page0, dict) or not page0:
            return 0
        span = 0
        for entry in page0.values():
            if not isinstance(entry, dict):
                continue
            try:
                span = max(
                    span, int(entry.get("x") or 0) + int(entry.get("width") or 0)
                )
            except (TypeError, ValueError):
                continue
        return span

    crushed = False
    profiles = settings.get("fence_layouts_by_display") or {}
    if isinstance(profiles, dict):
        for key, pages in profiles.items():
            if fingerprint_is_transient(str(key)):
                continue
            span = _page0_profile_span(pages if isinstance(pages, dict) else None)
            if 0 < span <= 820:
                crushed = True
                break
    if not crushed:
        settings["offscreen_layout_healed_v1"] = True
        return

    def _page0_fence_span(data: dict) -> int:
        span = 0
        for fence in data.get("fences") or []:
            if not isinstance(fence, dict):
                continue
            if 0 not in get_fence_pages(fence):
                continue
            try:
                x = int(fence.get("x") or 0)
                w = int(fence.get("width") or 0)
            except (TypeError, ValueError):
                continue
            if w > 0:
                span = max(span, x + w)
        return span

    try:
        from src.settings import SETTINGS_BACKUP_DIR
    except Exception:
        settings["offscreen_layout_healed_v1"] = True
        return

    backups = sorted(
        SETTINGS_BACKUP_DIR.glob("settings_*.json"),
        key=lambda p: p.stat().st_mtime,
        reverse=True,
    )
    donor = None
    for path in backups[:40]:
        try:
            import json

            data = json.loads(path.read_text(encoding="utf-8"))
        except Exception:
            continue
        wide = _page0_fence_span(data) >= 1200
        if not wide:
            donor_profiles = data.get("fence_layouts_by_display") or {}
            if isinstance(donor_profiles, dict):
                for key, pages in donor_profiles.items():
                    if fingerprint_is_transient(str(key)):
                        continue
                    if (
                        _page0_profile_span(
                            pages if isinstance(pages, dict) else None
                        )
                        >= 1200
                    ):
                        wide = True
                        break
        if not wide:
            continue
        donor = data
        break
    if donor is None:
        settings["offscreen_layout_healed_v1"] = True
        return

    by_id = {
        str(f.get("id")): f
        for f in (donor.get("fences") or [])
        if isinstance(f, dict) and f.get("id")
    }
    for fence in settings.get("fences") or []:
        if not isinstance(fence, dict):
            continue
        src = by_id.get(str(fence.get("id")))
        if src is None:
            continue
        for key in ("x", "y", "width", "height", "collapsed"):
            if key in src:
                fence[key] = src[key]

    donor_profiles = donor.get("fence_layouts_by_display")
    if isinstance(donor_profiles, dict):
        profiles = settings.setdefault("fence_layouts_by_display", {})
        for key, pages in donor_profiles.items():
            if fingerprint_is_transient(str(key)):
                continue
            if not isinstance(pages, dict):
                continue
            # Prefer donor page maps when ours look packed.
            profiles[key] = pages

    donor_legacy = donor.get("fence_layouts_by_page")
    if isinstance(donor_legacy, dict):
        settings["fence_layouts_by_page"] = donor_legacy

    donor_last = str(donor.get("last_display_fingerprint") or "")
    if donor_last and not fingerprint_is_transient(donor_last):
        settings["last_display_fingerprint"] = donor_last

    settings["offscreen_layout_healed_v1"] = True


def _heal_stale_single_after_multi(settings: dict) -> None:
    """If single-screen profile is a cramped leftover vs multi, prefer multi donor.

    Users often arrange fences on an external primary, then unplug. The older
    laptop profile (narrow tiles at y=0) must not win over the just-used dual
    layout — clear it so ``get_fence_geometry`` scales from the multi donor.
    """
    fp = current_display_fingerprint()
    if fingerprint_monitor_count(fp) != 1:
        return
    profiles = settings.get("fence_layouts_by_display")
    if not isinstance(profiles, dict):
        return
    current = profiles.get(fp)
    if not isinstance(current, dict) or not current:
        return
    multis = [k for k in profiles if fingerprint_monitor_count(k) > 1]
    if not multis:
        return
    cur_rw = _page0_total_rw(current)
    best_multi = max(multis, key=lambda k: _page0_total_rw(profiles.get(k)))
    multi_rw = _page0_total_rw(profiles.get(best_multi))
    if cur_rw <= 0 or multi_rw <= 0:
        return
    # Single profile covers much less of the screen than the multi arrangement.
    if cur_rw >= multi_rw * 0.7:
        return
    settings["last_display_fingerprint"] = best_multi
    profiles.pop(fp, None)


def _fingerprint_primary_pixels(fp: str) -> tuple[int, int]:
    """Best-effort (width, height) of the primary-ish panel in a fingerprint."""
    if not fp:
        return (0, 0)
    # Prefer the segment with origin closest to (0,0).
    best = (0, 0)
    best_dist = 10**18
    for part in str(fp).split("|"):
        try:
            segs = part.split(",")
            x = int(segs[0])
            y = int(segs[1])
            wh = segs[2].split("@", 1)[0]
            w_s, h_s = wh.split("x", 1)
            w, h = int(w_s), int(h_s)
        except (TypeError, ValueError, IndexError):
            continue
        dist = abs(x) + abs(y)
        if dist < best_dist:
            best_dist = dist
            best = (w, h)
    return best


def _fingerprint_primary_mm(fp: str) -> tuple[int, int]:
    """Physical size (mm) of the primary-ish panel, or (0,0) for legacy keys."""
    if not fp or "@" not in fp:
        return (0, 0)
    best = (0, 0)
    best_dist = 10**18
    for part in str(fp).split("|"):
        try:
            segs = part.split(",")
            x = int(segs[0])
            y = int(segs[1])
            # WIDTHxHEIGHT@MMxMMmm@dpr
            body = segs[2]
            chunks = body.split("@")
            mm = chunks[1] if len(chunks) > 1 else ""
            if not mm.endswith("mm"):
                continue
            mw_s, mh_s = mm[:-2].split("x", 1)
            mw, mh = int(mw_s), int(mh_s)
        except (TypeError, ValueError, IndexError):
            continue
        dist = abs(x) + abs(y)
        if dist < best_dist:
            best_dist = dist
            best = (mw, mh)
    return best


def current_primary_physical_mm() -> tuple[int, int]:
    from PyQt6.QtGui import QGuiApplication

    screen = QGuiApplication.primaryScreen()
    if screen is None:
        return (0, 0)
    try:
        ps = screen.physicalSize()
        return (max(1, int(round(float(ps.width())))), max(1, int(round(float(ps.height())))))
    except Exception:
        return (0, 0)


def migrate_layouts(settings: dict) -> None:
    """Seed page layout store; lift legacy layouts into the current display profile."""
    from copy import deepcopy

    ensure_fence_ids(settings)
    layouts: dict[str, dict[str, dict]] = settings.setdefault("fence_layouts_by_page", {})
    for fence in settings.get("fences", []):
        fence_id = fence["id"]
        for page_id in get_fence_pages(fence):
            page_key = str(page_id)
            page_layout = layouts.setdefault(page_key, {})
            if fence_id not in page_layout:
                page_layout[fence_id] = _geometry_from_fence(fence)
            else:
                page_layout[fence_id].setdefault(
                    "collapsed", bool(fence.get("collapsed", False))
                )

    if not settings.get("first_run_organize_done", True):
        from src.system_defaults import apply_system_fence_geometry_from_display

        apply_system_fence_geometry_from_display(settings)

    profiles = settings.setdefault("fence_layouts_by_display", {})
    fp = current_display_fingerprint()
    first_install = not settings.get("first_run_organize_done", True)
    if fp not in profiles or first_install:
        # If other display profiles already exist, leave this fingerprint empty so
        # get_fence_geometry proportionally scales from a donor. Copying the legacy
        # page store here re-poisoned laptop/single-screen profiles after a switch.
        # First install always seeds from the current work area instead.
        if first_install or not any(k != fp for k in profiles):
            if layouts:
                profiles[fp] = _enrich_profile_with_ratios(deepcopy(layouts))
            else:
                # No page store yet — seed from fence absolute fields.
                seed: dict[str, dict[str, dict]] = {}
                for fence in settings.get("fences", []):
                    fence_id = fence["id"]
                    for page_id in get_fence_pages(fence):
                        seed.setdefault(str(page_id), {})[fence_id] = _geometry_from_fence(fence)
                if seed:
                    profiles[fp] = _enrich_profile_with_ratios(seed)
        elif fp not in profiles:
            profiles[fp] = {}
    settings["last_display_fingerprint"] = settings.get("last_display_fingerprint") or fp
    _heal_offscreen_layout_corruption(settings)
    _heal_top_edge_strips(settings)
    _purge_unhealthy_display_profiles(settings)
    purge_transient_display_profiles(settings)
    _heal_stale_single_after_multi(settings)


def _purge_unhealthy_display_profiles(settings: dict) -> None:
    """Drop overlapping / crushed display profiles so donors stay usable."""
    profiles = settings.get("fence_layouts_by_display")
    if not isinstance(profiles, dict) or not profiles:
        return
    current = current_display_fingerprint()
    for key in list(profiles.keys()):
        if key == current:
            continue
        if not display_profile_is_healthy(profiles.get(key)):
            profiles.pop(key, None)


def _heal_top_edge_strips(settings: dict) -> None:
    """Nudge wide fences stuck at y=0 (SetParent / dual-monitor clamp remnant).

    Runs once per settings file. Leaves smaller top tiles alone.
    """
    if settings.get("top_edge_strip_healed"):
        return
    area = primary_desktop_rect()
    new_y = area.y() + max(80, min(200, area.height() // 8))

    def _nudge(entry: dict | None) -> dict | None:
        if not isinstance(entry, dict):
            return entry
        try:
            x = int(entry.get("x", 0))
            y = int(entry.get("y", 0))
            w = int(entry.get("width", 0))
            h = int(entry.get("height", 320))
        except (TypeError, ValueError):
            return entry
        # Only lift crushed top strips (SetParent corruption), not intentional layouts.
        if y > area.y() + 4:
            return entry
        collapsed = bool(entry.get("collapsed", False))
        if not _is_crushed_geometry(
            {"x": x, "y": y, "width": w, "height": h, "collapsed": collapsed}
        ):
            return entry
        clamped = _clamp_geometry(x, new_y, w, h, collapsed)
        return {
            **entry,
            **clamped,
            **_ratios_for(
                clamped["x"], clamped["y"], clamped["width"], clamped["height"], area
            ),
        }

    for fence in settings.get("fences", []):
        if not isinstance(fence, dict):
            continue
        fixed = _nudge(
            {
                "x": fence.get("x", 50),
                "y": fence.get("y", 50),
                "width": fence.get("width", 220),
                "height": fence.get("height", 320),
                "collapsed": fence.get("collapsed", False),
            }
        )
        if fixed is None or int(fixed.get("y", -1)) == int(fence.get("y", -2)):
            continue
        fence["x"] = fixed["x"]
        fence["y"] = fixed["y"]
        fence["width"] = fixed["width"]
        fence["height"] = fixed["height"]

    for page_map in (settings.get("fence_layouts_by_page") or {}).values():
        if not isinstance(page_map, dict):
            continue
        for fid, geom in list(page_map.items()):
            fixed = _nudge(geom if isinstance(geom, dict) else None)
            if fixed is not None:
                page_map[fid] = fixed

    for pages in (settings.get("fence_layouts_by_display") or {}).values():
        if not isinstance(pages, dict):
            continue
        for fmap in pages.values():
            if not isinstance(fmap, dict):
                continue
            for fid, geom in list(fmap.items()):
                fixed = _nudge(geom if isinstance(geom, dict) else None)
                if fixed is not None:
                    fmap[fid] = fixed

    settings["top_edge_strip_healed"] = True


def ensure_display_layout_ready(settings: dict) -> str:
    """Guarantee a profile exists for the current monitors; return fingerprint."""
    migrate_layouts(settings)
    return current_display_fingerprint()


def _geometry_from_fence(fence: dict) -> dict:
    return {
        "x": int(fence.get("x", 50)),
        "y": int(fence.get("y", 50)),
        "width": int(fence.get("width", 220)),
        "height": int(fence.get("height", 320)),
        "collapsed": bool(fence.get("collapsed", False)),
    }


def _clamp_geometry(x: int, y: int, w: int, h: int, collapsed: bool = False) -> dict:
    area = _target_screen_rect(x, y, w, h)
    w = max(_MIN_W, min(int(w), area.width()))
    h = max(_MIN_H if not collapsed else 44, min(int(h), area.height()))
    x = max(area.x(), min(int(x), area.x() + area.width() - w))
    y = max(area.y(), min(int(y), area.y() + area.height() - h))
    return {"x": x, "y": y, "width": w, "height": h, "collapsed": bool(collapsed)}


_NEW_FENCE_GAP = 16
_NEW_FENCE_STEP = 32
_OFFSCREEN_PARK = -10000
_live_fences_provider = None


def set_live_fences_provider(provider) -> None:
    """Register the running app's fence widgets (visible + parked) for placement."""
    global _live_fences_provider
    _live_fences_provider = provider


def _provider_live_fences() -> list | None:
    fn = _live_fences_provider
    if fn is None:
        return None
    try:
        widgets = fn()
    except Exception:
        return None
    if widgets is None:
        return None
    return list(widgets)


def current_page_fence_rects(
    settings: dict,
    page_id: int,
    live_fences: list | None = None,
):
    """On-screen (preferred) or saved rects for visible fences on *page_id*."""
    from PyQt6.QtCore import QRect

    live_by_id: dict[str, object] = {}
    if live_fences:
        for fence in live_fences:
            try:
                cfg = getattr(fence, "config", None)
                if not isinstance(cfg, dict) or not fence_on_page(cfg, page_id):
                    continue
                geo = fence.frameGeometry()
                # Off-screen warmup park is not a desktop slot; use saved geometry.
                if geo.x() < _OFFSCREEN_PARK or geo.y() < _OFFSCREEN_PARK:
                    continue
                fid = str(cfg.get("id") or "")
                if not fid:
                    continue
                live_by_id[fid] = QRect(geo)
            except RuntimeError:
                continue

    rects: list = []
    seen: set[str] = set()
    for fence in settings.get("fences") or []:
        if not isinstance(fence, dict) or not fence.get("visible", True):
            continue
        if not fence_on_page(fence, page_id):
            continue
        fid = str(fence.get("id") or "")
        if fid:
            seen.add(fid)
        live_rect = live_by_id.get(fid) if fid else None
        if live_rect is not None:
            rects.append(live_rect)
            continue
        geom = get_fence_geometry(settings, fence, page_id)
        rects.append(
            QRect(
                int(geom["x"]),
                int(geom["y"]),
                int(geom["width"]),
                int(geom["height"]),
            )
        )
    for fid, live_rect in live_by_id.items():
        if fid not in seen:
            rects.append(live_rect)
    return rects


def place_new_fence_rect(
    settings: dict,
    page_id: int,
    *,
    width: int = DEFAULT_NEW_FENCE_WIDTH,
    height: int = DEFAULT_NEW_FENCE_HEIGHT,
    hint_x: int = 80,
    hint_y: int = 80,
    anchor_x: int | None = None,
    anchor_y: int | None = None,
    live_fences: list | None = None,
    occupied: list | None = None,
) -> tuple[int, int, int, int]:
    """Pick a free slot on the current page (Fences-like: click if empty, else search).

    Prefers *anchor_x/y* (drawn rect) or a click-centered rect at *hint_*.
    Other pages do not occupy space. Falls back to the nearest empty grid slot
    so a new fence does not cover existing ones on this page.
    """
    from PyQt6.QtCore import QPoint, QRect
    from PyQt6.QtGui import QGuiApplication

    w = max(_MIN_W, int(width))
    h = max(_MIN_H, int(height))
    hint_x = int(hint_x)
    hint_y = int(hint_y)
    screen = QGuiApplication.screenAt(QPoint(hint_x, hint_y))
    if screen is None:
        screen = QGuiApplication.primaryScreen()
    if screen is not None:
        area = screen.availableGeometry()
    else:
        area = QRect(0, 0, 1920, 1080)
    w = min(w, max(_MIN_W, area.width() - _NEW_FENCE_GAP * 2))
    h = min(h, max(_MIN_H, area.height() - _NEW_FENCE_GAP * 2))

    if occupied is not None:
        blockers = list(occupied)
    else:
        widgets = live_fences
        if widgets is None:
            widgets = _provider_live_fences()
        blockers = current_page_fence_rects(
            settings, page_id, live_fences=widgets
        )

    def _fits(x: int, y: int) -> bool:
        proposed = QRect(x, y, w, h)
        if not area.contains(proposed):
            return False
        padded = proposed.adjusted(
            -_NEW_FENCE_GAP, -_NEW_FENCE_GAP, _NEW_FENCE_GAP, _NEW_FENCE_GAP
        )
        for occ in blockers:
            try:
                if padded.intersects(occ):
                    return False
            except Exception:
                continue
        return True

    def _clamp_xy(x: int, y: int) -> tuple[int, int]:
        x = max(area.left(), min(int(x), area.x() + area.width() - w))
        y = max(area.top(), min(int(y), area.y() + area.height() - h))
        return x, y

    if anchor_x is not None and anchor_y is not None:
        x, y = _clamp_xy(anchor_x, anchor_y)
    else:
        x, y = _clamp_xy(hint_x - w // 2, hint_y - 48)
    if _fits(x, y):
        return x, y, w, h

    best: tuple[int, int, int] | None = None
    y_limit = area.y() + area.height() - h - _NEW_FENCE_GAP
    x_limit = area.x() + area.width() - w - _NEW_FENCE_GAP
    y_start = area.top() + _NEW_FENCE_GAP
    x_pos = area.left() + _NEW_FENCE_GAP
    while x_pos <= x_limit:
        y_pos = y_start
        while y_pos <= y_limit:
            if _fits(x_pos, y_pos):
                dist = (x_pos + w // 2 - hint_x) ** 2 + (y_pos + h // 2 - hint_y) ** 2
                if best is None or dist < best[0]:
                    best = (dist, x_pos, y_pos)
            y_pos += _NEW_FENCE_STEP
        x_pos += _NEW_FENCE_STEP
    if best is not None:
        return best[1], best[2], w, h

    for n in range(1, 24):
        x, y = _clamp_xy(hint_x - w // 2 + n * 24, hint_y - 48 + n * 24)
        if _fits(x, y):
            return x, y, w, h

    x, y = _clamp_xy(hint_x - w // 2, hint_y - 48)
    return x, y, w, h


def place_new_fence_config(
    settings: dict,
    config: dict,
    page_id: int,
    *,
    hint_x: int | None = None,
    hint_y: int | None = None,
    live_fences: list | None = None,
) -> tuple[int, int, int, int]:
    """Place a settings/tray (no click) fence into a free slot on *page_id*."""
    area = primary_desktop_rect()
    gap = _NEW_FENCE_GAP
    if hint_x is None:
        hint_x = area.x() + 80
    if hint_y is None:
        hint_y = area.y() + 80
    x, y, w, h = place_new_fence_rect(
        settings,
        int(page_id),
        width=int(config.get("width") or DEFAULT_NEW_FENCE_WIDTH),
        height=int(config.get("height") or DEFAULT_NEW_FENCE_HEIGHT),
        hint_x=int(hint_x),
        hint_y=int(hint_y),
        anchor_x=area.x() + gap,
        anchor_y=area.y() + gap,
        live_fences=live_fences,
    )
    config["x"] = x
    config["y"] = y
    config["width"] = w
    config["height"] = h
    return x, y, w, h


def _ratios_for(x: int, y: int, w: int, h: int, area=None) -> dict[str, float | int]:
    area = area or _target_screen_rect(x, y, w, h)
    aw = max(area.width(), 1)
    ah = max(area.height(), 1)
    return {
        "rx": (x - area.x()) / aw,
        "ry": (y - area.y()) / ah,
        "rw": w / aw,
        "rh": h / ah,
        "ref_x": area.x(),
        "ref_y": area.y(),
        "ref_w": aw,
        "ref_h": ah,
    }


def _enrich_entry(entry: dict) -> dict:
    """Ensure relative fields exist for an absolute geometry entry."""
    out = dict(entry)
    x = int(out.get("x", 50))
    y = int(out.get("y", 50))
    w = int(out.get("width", 220))
    h = int(out.get("height", 320))
    if "rx" not in out or "rw" not in out:
        if out.get("ref_w") and out.get("ref_h"):
            from PyQt6.QtCore import QRect

            area = QRect(
                int(out.get("ref_x", 0)),
                int(out.get("ref_y", 0)),
                int(out["ref_w"]),
                int(out["ref_h"]),
            )
        else:
            area = _target_screen_rect(x, y, w, h)
        out.update(_ratios_for(x, y, w, h, area))
    return out


def _enrich_profile_with_ratios(profile: dict) -> dict:
    enriched: dict = {}
    for page_key, fences in profile.items():
        enriched[page_key] = {
            fid: _enrich_entry(dict(geo)) for fid, geo in (fences or {}).items()
        }
    return enriched


def _spans_multiple_screens(x: int, y: int, w: int, h: int) -> bool:
    from PyQt6.QtCore import QRect
    from PyQt6.QtGui import QGuiApplication

    rect = QRect(int(x), int(y), max(1, int(w)), max(1, int(h)))
    hits = 0
    for screen in QGuiApplication.screens() or []:
        inter = rect.intersected(screen.availableGeometry())
        if inter.width() > 0 and inter.height() > 0:
            hits += 1
            if hits >= 2:
                return True
    return False


def _is_crushed_geometry(entry: dict | None) -> bool:
    """True for setWindowFlags / SetParent corruption (thin side or top strip)."""
    if not entry:
        return False
    try:
        w = int(entry.get("width", 0))
        h = int(entry.get("height", 0))
    except (TypeError, ValueError):
        return False
    if w <= _MIN_W + 8:
        return True
    # Expanded fence crushed to a header-thin top strip (icons stuck at screen top).
    collapsed = bool(entry.get("collapsed", False))
    if not collapsed and 0 < h < _MIN_H:
        return True
    return False


def _ratios_usable(entry: dict) -> bool:
    """True when rx/rw look like screen-relative fractions (not a botched save)."""
    try:
        rx = float(entry.get("rx"))
        ry = float(entry.get("ry", 0))
        rw = float(entry.get("rw"))
        rh = float(entry.get("rh", 0.3))
    except (TypeError, ValueError):
        return False
    if rw <= 0.01 or rh <= 0.01 or rw > 1.05 or rh > 1.05:
        return False
    if rx < -0.05 or ry < -0.05 or rx > 1.05 or ry > 1.05:
        return False
    return True


def _target_area_for_ratios(entry: dict):
    """Map ratio layout onto the primary work area (laptop / single-screen case).

    Multi-monitor donors still store ratios against one host screen; when the
    machine drops to a single panel we always place onto that panel.
    """
    return primary_desktop_rect()


def _scale_from_ratios(entry: dict, collapsed: bool) -> dict:
    area = _target_area_for_ratios(entry)
    rx = float(entry["rx"])
    ry = float(entry.get("ry", 0))
    rw = min(1.0, max(0.02, float(entry["rw"])))
    rh = min(1.0, max(0.02, float(entry.get("rh", 0.3))))
    x = area.x() + int(round(rx * area.width()))
    y = area.y() + int(round(ry * area.height()))
    w = int(round(rw * area.width()))
    h = int(round(rh * area.height()))
    return _clamp_geometry(x, y, w, h, collapsed)


def _scale_absolute_proportional(entry: dict, collapsed: bool) -> dict | None:
    """等比例 map absolute pixels from ``ref_*`` screen onto the current primary."""
    try:
        ref_w = int(entry.get("ref_w") or 0)
        ref_h = int(entry.get("ref_h") or 0)
        ref_x = int(entry.get("ref_x", 0))
        ref_y = int(entry.get("ref_y", 0))
        x0 = int(entry.get("x", 50))
        y0 = int(entry.get("y", 50))
        w0 = int(entry.get("width", 220))
        h0 = int(entry.get("height", 320))
    except (TypeError, ValueError):
        return None
    if ref_w <= 0 or ref_h <= 0 or w0 <= 0 or h0 <= 0:
        return None
    area = primary_desktop_rect()
    sx = area.width() / ref_w
    sy = area.height() / ref_h
    # Uniform scale so the layout shape matches the donor monitor.
    s = min(sx, sy)
    x = area.x() + int(round((x0 - ref_x) * s))
    y = area.y() + int(round((y0 - ref_y) * s))
    w = int(round(w0 * s))
    h = int(round(h0 * s))
    return _clamp_geometry(x, y, w, h, collapsed)


def _rects_overlap_heavily(a: dict, b: dict) -> bool:
    """True when two fence rects share a large fraction of the smaller one."""
    try:
        ax, ay = int(a["x"]), int(a["y"])
        aw, ah = int(a["width"]), int(a["height"])
        bx, by = int(b["x"]), int(b["y"])
        bw, bh = int(b["width"]), int(b["height"])
    except (KeyError, TypeError, ValueError):
        return False
    ix1, iy1 = max(ax, bx), max(ay, by)
    ix2, iy2 = min(ax + aw, bx + bw), min(ay + ah, by + bh)
    iw, ih = ix2 - ix1, iy2 - iy1
    if iw <= 0 or ih <= 0:
        return False
    inter = iw * ih
    smaller = max(1, min(aw * ah, bw * bh))
    return inter >= int(smaller * 0.35)


def _page_entries_overlap(entries: dict) -> bool:
    items = [e for e in entries.values() if isinstance(e, dict) and not _is_crushed_geometry(e)]
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            if _rects_overlap_heavily(items[i], items[j]):
                return True
    return False


def display_profile_is_healthy(profile: dict | None) -> bool:
    """False for empty / crushed / overlapping destination layouts."""
    if not isinstance(profile, dict) or not profile:
        return False
    any_entry = False
    for page_map in profile.values():
        if not isinstance(page_map, dict) or not page_map:
            continue
        resolved: dict[str, dict] = {}
        for fid, entry in page_map.items():
            if not isinstance(entry, dict):
                continue
            if _is_crushed_geometry(entry):
                return False
            any_entry = True
            # Evaluate the geometry we would actually show.
            geom = geometry_from_entry(entry)
            if _is_crushed_geometry(geom):
                return False
            resolved[fid] = geom
        if _page_entries_overlap(resolved):
            return False
    return any_entry


def _entry_fit_for_current_display(entry: dict | None) -> bool:
    """False for profiles that were clamped onto the wrong resolution (laptop mess)."""
    if not entry or _is_crushed_geometry(entry):
        return False
    if _ratios_usable(entry):
        return True
    try:
        ref_w = int(entry.get("ref_w") or 0)
        ref_h = int(entry.get("ref_h") or 0)
        w = int(entry.get("width", 0))
    except (TypeError, ValueError):
        return False
    area = primary_desktop_rect()
    # Wider than the current screen with broken ratios — only OK if ref is a
    # larger donor screen we can shrink from (not a botched same-size save).
    if w > area.width() + 24:
        return ref_w > area.width() + 24 and ref_h > 0
    if ref_w > 0 and abs(ref_w - area.width()) > max(48, int(area.width() * 0.1)):
        return ref_h > 0
    return entry.get("x") is not None and entry.get("width") is not None


def _donor_size_score(fp: str, cur_fp: str) -> int:
    """Higher when donor primary size (pixels + physical mm) matches current."""
    cw, ch = _fingerprint_primary_pixels(cur_fp)
    dw, dh = _fingerprint_primary_pixels(fp)
    score = 0
    if cw > 0 and dw > 0:
        # Prefer similar resolution class (laptop 1280 vs desktop 1920).
        pix_delta = abs(cw - dw) + abs(ch - dh)
        score += max(0, 50_000 - pix_delta)
    cm = _fingerprint_primary_mm(cur_fp)
    dm = _fingerprint_primary_mm(fp)
    if cm[0] > 0 and dm[0] > 0:
        mm_delta = abs(cm[0] - dm[0]) + abs(cm[1] - dm[1])
        # Physical size match beats pixel-only (same 1920, different panel).
        score += max(0, 200_000 - mm_delta * 80)
    elif cm[0] > 0 and dm[0] == 0:
        # Legacy donor without mm — still usable, mild penalty.
        score += 5_000
    return score


def _find_donor_entry(
    settings: dict, page_id: int, fence_id: str, current_fp: str
) -> dict | None:
    profiles: dict = settings.get("fence_layouts_by_display") or {}
    last_fp = settings.get("last_display_fingerprint")
    candidates: list[str] = []
    if last_fp and last_fp != current_fp and last_fp in profiles:
        candidates.append(last_fp)
    # Prefer multi-monitor profiles (usually hold the user's real layout).
    rest = [fp for fp in profiles if fp != current_fp and fp not in candidates]
    rest.sort(key=lambda fp: (_donor_size_score(fp, current_fp), fp.count("|")), reverse=True)
    candidates.extend(rest)

    best: dict | None = None
    best_score = -1
    for fp in candidates:
        entry = _profile_page_entry(profiles.get(fp) or {}, page_id, fence_id)
        if not entry or _is_crushed_geometry(entry):
            continue
        # Skip donors from pages that already overlap for this fence's neighbors.
        page_map = (profiles.get(fp) or {}).get(str(page_id)) or {}
        if isinstance(page_map, dict) and _page_entries_overlap(
            {
                k: geometry_from_entry(v) if isinstance(v, dict) else v
                for k, v in page_map.items()
                if isinstance(v, dict)
            }
        ):
            continue
        try:
            w = int(entry.get("width", 0))
        except (TypeError, ValueError):
            w = 0
        # Prefer entries with clean screen ratios (scale cleanly across sizes).
        score = w + _donor_size_score(fp, current_fp)
        if _ratios_usable(entry):
            score += 100_000
        elif int(entry.get("ref_w") or 0) > 0:
            score += 10_000
        else:
            continue
        if score > best_score:
            best = entry
            best_score = score

    if best:
        return best

    # Legacy flat store.
    legacy = (settings.get("fence_layouts_by_page") or {}).get(str(page_id), {}).get(fence_id)
    if isinstance(legacy, dict) and not _is_crushed_geometry(legacy):
        return dict(legacy)
    return None


def geometry_from_entry(entry: dict | None, *, collapsed_default: bool = False) -> dict:
    """Map a stored entry onto the current screen, scaling proportionally.

    Prefer relative ratios (rx/rw) so monitor → laptop keeps the same layout
    shape. Absolute pixels are only a last resort, and are scaled via ``ref_*``
    when the donor screen size differs.
    """
    if not entry or _is_crushed_geometry(entry):
        area = primary_desktop_rect()
        if not entry:
            return _clamp_geometry(area.x() + 50, area.y() + 50, 220, 320, collapsed_default)
        collapsed = bool(entry.get("collapsed", collapsed_default))
        return _clamp_geometry(area.x() + 50, area.y() + 50, 220, 320, collapsed)

    collapsed = bool(entry.get("collapsed", collapsed_default))

    # 1) Screen-relative ratios — primary path for display switches.
    if _ratios_usable(entry):
        return _scale_from_ratios(entry, collapsed)

    # 2) Absolute + ref screen → uniform proportional scale.
    scaled = _scale_absolute_proportional(entry, collapsed)
    if scaled is not None:
        area = primary_desktop_rect()
        ref_w = int(entry.get("ref_w") or 0)
        if ref_w > 0 and abs(ref_w - area.width()) > 24:
            return scaled
        # Same-ish screen: proportional scale still OK; fall through if identical.

    # 3) Same screen absolute clamp (no cross-resolution).
    if entry.get("x") is not None and entry.get("width") is not None:
        try:
            ref_w = int(entry.get("ref_w") or 0)
        except (TypeError, ValueError):
            ref_w = 0
        area = primary_desktop_rect()
        if ref_w <= 0 or abs(ref_w - area.width()) <= max(24, int(area.width() * 0.05)):
            return _clamp_geometry(
                int(entry.get("x", 50)),
                int(entry.get("y", 50)),
                int(entry.get("width", 220)),
                int(entry.get("height", 320)),
                collapsed,
            )
        if scaled is not None:
            return scaled

    # 4) Broken ratios (e.g. rx=2) — still try clamp after naive multiply.
    if "rx" in entry and "rw" in entry:
        try:
            return _scale_from_ratios(
                {
                    **entry,
                    "rx": min(1.0, max(0.0, float(entry["rx"]))),
                    "ry": min(1.0, max(0.0, float(entry.get("ry", 0)))),
                    "rw": min(1.0, max(0.02, float(entry["rw"]))),
                    "rh": min(1.0, max(0.02, float(entry.get("rh", 0.3)))),
                },
                collapsed,
            )
        except (TypeError, ValueError):
            pass

    if scaled is not None:
        return scaled

    return _clamp_geometry(
        int(entry.get("x", 50)),
        int(entry.get("y", 50)),
        int(entry.get("width", 220)),
        int(entry.get("height", 320)),
        collapsed,
    )


def _profile_page_entry(profile: dict, page_id: int, fence_id: str) -> dict | None:
    page = profile.get(str(page_id)) or {}
    saved = page.get(fence_id)
    return dict(saved) if isinstance(saved, dict) else None


def _geometry_needs_resave(saved: dict, geom: dict) -> bool:
    keys = ("x", "y", "width", "height")
    if any(int(saved.get(k, -1)) != int(geom.get(k, -2)) for k in keys):
        return True
    if bool(saved.get("collapsed")) != bool(geom.get("collapsed")):
        return True
    if _is_crushed_geometry(saved):
        return True
    # Rewrite legacy ratios keyed to the virtual-desktop union.
    ref_w = int(saved.get("ref_w") or 0)
    virtual = virtual_desktop_rect()
    if ref_w >= int(virtual.width() * 0.9) and virtual.width() > primary_desktop_rect().width():
        return True
    return False


def get_fence_geometry(settings: dict, fence_cfg: dict, page_id: int) -> dict:
    fence_id = fence_cfg.get("id") or fence_cfg.get("name", "")
    fp = ensure_display_layout_ready(settings)
    profiles = settings.setdefault("fence_layouts_by_display", {})
    profile = profiles.get(fp) or {}
    collapsed_default = bool(fence_cfg.get("collapsed", False))

    saved = _profile_page_entry(profile, page_id, fence_id)
    page_map = profile.get(str(page_id)) if isinstance(profile, dict) else None
    page_overlaps = False
    if isinstance(page_map, dict) and len(page_map) > 1:
        resolved = {
            k: geometry_from_entry(v)
            for k, v in page_map.items()
            if isinstance(v, dict)
        }
        page_overlaps = _page_entries_overlap(resolved)

    if saved and _entry_fit_for_current_display(saved) and not page_overlaps:
        geom = geometry_from_entry(saved, collapsed_default=collapsed_default)
        if not _is_crushed_geometry(geom):
            if _geometry_needs_resave(saved, geom):
                save_fence_geometry(settings, fence_cfg, page_id, geom, fingerprint=fp)
            return geom

    donor = _find_donor_entry(settings, page_id, fence_id, fp)
    if donor:
        geom = geometry_from_entry(donor, collapsed_default=collapsed_default)
        if not _is_crushed_geometry(geom):
            save_fence_geometry(settings, fence_cfg, page_id, geom, fingerprint=fp)
            return geom

    # Last resort: fence absolute fields, then a sensible default.
    seed = _geometry_from_fence(fence_cfg)
    if _is_crushed_geometry(seed):
        area = primary_desktop_rect()
        seed = {
            "x": area.x() + 50,
            "y": area.y() + 50,
            "width": min(720, max(360, area.width() // 2)),
            "height": 320,
            "collapsed": collapsed_default,
        }
    geom = geometry_from_entry(seed, collapsed_default=collapsed_default)
    save_fence_geometry(settings, fence_cfg, page_id, geom, fingerprint=fp)
    return geom


def save_fence_geometry(
    settings: dict,
    fence_cfg: dict,
    page_id: int,
    geometry: dict,
    fingerprint: str | None = None,
) -> None:
    # Never persist the corrupted 160px strip produced by setWindowFlags churn.
    if _is_crushed_geometry(geometry):
        return
    # Offscreen Qt / tiny intermediate fingerprints must not overwrite real layouts.
    if layout_writes_blocked():
        return
    fence_id = fence_cfg.get("id") or fence_cfg.get("name", "")
    ensure_display_layout_ready(settings)
    fp = fingerprint or current_display_fingerprint()
    if fingerprint_is_transient(fp):
        return
    x = int(geometry.get("x", 50))
    y = int(geometry.get("y", 50))
    w = int(geometry.get("width", 220))
    h = int(geometry.get("height", 320))
    collapsed = bool(geometry.get("collapsed", False))
    clamped = _clamp_geometry(x, y, w, h, collapsed)
    if _is_crushed_geometry(clamped):
        return
    area = _target_screen_rect(
        clamped["x"], clamped["y"], clamped["width"], clamped["height"]
    )
    entry = {
        **clamped,
        **_ratios_for(clamped["x"], clamped["y"], clamped["width"], clamped["height"], area),
    }

    profiles = settings.setdefault("fence_layouts_by_display", {})
    page_layout = profiles.setdefault(fp, {}).setdefault(str(page_id), {})
    page_layout[fence_id] = entry
    settings["last_display_fingerprint"] = fp

    # Keep legacy store in sync for older readers / editors.
    legacy = settings.setdefault("fence_layouts_by_page", {})
    legacy.setdefault(str(page_id), {})[fence_id] = {
        "x": entry["x"],
        "y": entry["y"],
        "width": entry["width"],
        "height": entry["height"],
        "collapsed": entry["collapsed"],
    }

    if fence_on_page(fence_cfg, page_id):
        fence_cfg.update(
            {
                "x": entry["x"],
                "y": entry["y"],
                "width": entry["width"],
                "height": entry["height"],
                "collapsed": entry["collapsed"],
            }
        )


def save_visible_fences(settings: dict, fences: list[Any], page_id: int) -> None:
    by_id = {cfg.get("id"): cfg for cfg in settings.get("fences", [])}
    for fence in fences:
        fence_cfg = fence.config
        geometry = fence.get_config_update()
        save_fence_geometry(settings, fence_cfg, page_id, geometry)
        stored = by_id.get(fence_cfg.get("id"))
        if stored is not None:
            stored["sort_by"] = fence_cfg.get("sort_by", stored.get("sort_by", "name"))
            zoom = geometry.get("icon_zoom", getattr(fence, "_icon_zoom", 1.0))
            try:
                stored["icon_zoom"] = round(float(zoom), 3)
            except (TypeError, ValueError):
                stored["icon_zoom"] = 1.0
            fence_cfg["icon_zoom"] = stored["icon_zoom"]
            locked = geometry.get(
                "position_locked", getattr(fence, "_position_locked", False)
            )
            stored["position_locked"] = bool(locked)
            fence_cfg["position_locked"] = stored["position_locked"]
            # Keep live widget config pointer in sync with settings entry.
            fence.config = stored


def apply_geometries_to_fences(settings: dict, fences: list[Any], page_id: int) -> None:
    """Re-apply stored/scaled geometries after a display change."""
    for fence in fences:
        geom = get_fence_geometry(settings, fence.config, page_id)
        fence.setGeometry(geom["x"], geom["y"], geom["width"], geom["height"])
        fence.set_collapsed(bool(geom.get("collapsed", False)), emit=False)
    _resolve_visible_fence_overlaps(settings, fences, page_id)


def _resolve_visible_fence_overlaps(
    settings: dict, fences: list[Any], page_id: int
) -> None:
    """If two visible fences heavily overlap after a size switch, pack them left→right."""
    if len(fences) < 2:
        return
    items: list[tuple[Any, dict]] = []
    for fence in fences:
        g = {
            "x": int(fence.x()),
            "y": int(fence.y()),
            "width": int(fence.width()),
            "height": int(fence.height()),
            "collapsed": bool(getattr(fence, "collapsed", False)),
        }
        items.append((fence, g))
    # Stable left-to-right order by current x, then width.
    items.sort(key=lambda it: (it[1]["x"], -it[1]["width"], it[0].config.get("id") or ""))
    changed = False
    for i in range(len(items)):
        for j in range(i + 1, len(items)):
            left_f, left_g = items[i]
            right_f, right_g = items[j]
            if not _rects_overlap_heavily(left_g, right_g):
                continue
            # Place the smaller/right tile just after the left one.
            area = primary_desktop_rect()
            new_x = left_g["x"] + left_g["width"] + 4
            if new_x + right_g["width"] > area.x() + area.width():
                # Shrink the left strip so both fit.
                room = max(_MIN_W, area.width() - right_g["width"] - 8)
                if left_g["width"] > room:
                    left_g["width"] = room
                    left_f.setGeometry(
                        left_g["x"], left_g["y"], left_g["width"], left_g["height"]
                    )
                    save_fence_geometry(settings, left_f.config, page_id, left_g)
                    changed = True
                new_x = left_g["x"] + left_g["width"] + 4
            right_g["x"] = new_x
            right_f.setGeometry(
                right_g["x"], right_g["y"], right_g["width"], right_g["height"]
            )
            save_fence_geometry(settings, right_f.config, page_id, right_g)
            changed = True
            items[j] = (right_f, right_g)
    if changed:
        pass
