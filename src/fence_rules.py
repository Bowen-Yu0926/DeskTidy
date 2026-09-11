"""Fence-based organize rules: each fence picks icon / document kinds."""

from __future__ import annotations

import time
from pathlib import Path

from src.desktop_scanner import DesktopItem

_CLAIMED_CACHE: dict = {
    "sid": 0,
    "t": 0.0,
    "all_pinned": set(),
    "public": set(),
}
_CLAIMED_CACHE_TTL_S = 0.8

# Shortcut / desktop-icon extensions (「软件」).
ICON_EXTENSIONS = frozenset({".lnk", ".url"})

# Only two live kinds: software shortcuts vs everything else (文档).
ORGANIZE_KINDS: tuple[str, ...] = ("icon", "file")
ORGANIZE_KIND_LABELS: dict[str, str] = {
    "icon": "软件",
    "file": "文档",
}


def invalidate_claimed_keys_cache() -> None:
    _CLAIMED_CACHE["sid"] = 0
    _CLAIMED_CACHE["t"] = 0.0
    _CLAIMED_CACHE["all_pinned"] = set()
    _CLAIMED_CACHE["public"] = set()


# Kept for older UI imports / migration only — organize no longer uses presets.
PRESET_EXTENSION_GROUPS: dict[str, list[str]] = {
    "文档": [".doc", ".docx", ".pdf", ".txt", ".xls", ".xlsx", ".ppt", ".pptx", ".md", ".rtf"],
    "图片": [".jpg", ".jpeg", ".png", ".gif", ".bmp", ".svg", ".webp", ".ico", ".tiff"],
    "视频": [".mp4", ".avi", ".mkv", ".mov", ".wmv", ".flv", ".webm"],
    "音频": [".mp3", ".wav", ".flac", ".aac", ".ogg", ".wma"],
    "压缩包": [".zip", ".rar", ".7z", ".tar", ".gz", ".bz2"],
    "程序": [".exe", ".msi", ".bat", ".cmd", ".lnk"],
    "代码": [
        ".py",
        ".js",
        ".ts",
        ".html",
        ".css",
        ".java",
        ".cpp",
        ".c",
        ".h",
        ".json",
        ".xml",
        ".yaml",
        ".yml",
    ],
}

ALL_PRESET_EXTENSIONS: list[str] = sorted(
    {ext for exts in PRESET_EXTENSION_GROUPS.values() for ext in exts}
)


def normalize_extension(ext: str) -> str:
    ext = ext.strip().lower()
    if ext and not ext.startswith("."):
        ext = "." + ext
    return ext


def normalize_organize_kinds(kinds: list[str] | None) -> list[str]:
    """Stable unique kinds in canonical order.

    Legacy ``folder`` is folded into ``file`` (文档): all non-icon items
    match document rules.
    """
    if not kinds:
        return []
    wanted = {str(k).strip().lower() for k in kinds if k}
    if "folder" in wanted:
        wanted.add("file")
        wanted.discard("folder")
    return [k for k in ORGANIZE_KINDS if k in wanted]


def get_fence_extensions(fence: dict) -> list[str]:
    return [normalize_extension(e) for e in fence.get("extensions", []) if e]


def get_fence_filename_patterns(fence: dict) -> list[str]:
    return list(fence.get("filename_patterns", []))


def item_organize_kind(item: DesktopItem) -> str:
    """Classify a desktop item as icon or document.

    ``.lnk`` / ``.url`` / ``.exe`` are icons (programs). Folders and every
    other file type are「文档」for rule matching — raw ``deskNote.exe`` must
    not land in the docs fence beside spreadsheets.
    """
    if not item.is_dir:
        ext = normalize_extension(item.extension or "")
        if ext in ICON_EXTENSIONS or ext == ".exe":
            return "icon"
        # Some scanners leave extension empty for odd shortcuts.
        name = (item.name or "").lower()
        if name.endswith(".lnk") or name.endswith(".url") or name.endswith(".exe"):
            return "icon"
    return "file"


def path_organize_kind(path: Path | str) -> str:
    """Classify a filesystem path as ``icon`` (.lnk/.url/.exe) or ``file`` (文档)."""
    p = Path(path)
    try:
        if p.is_dir():
            return "file"
    except OSError:
        pass
    ext = normalize_extension(p.suffix or "")
    if ext in ICON_EXTENSIONS or ext == ".exe":
        return "icon"
    name = p.name.lower()
    if name.endswith(".lnk") or name.endswith(".url") or name.endswith(".exe"):
        return "icon"
    return "file"


def kinds_from_legacy_fence(fence: dict) -> list[str]:
    """Infer organize_kinds from legacy extensions / filename patterns."""
    kinds: list[str] = []
    exts = get_fence_extensions(fence)
    if any(e in ICON_EXTENSIONS for e in exts):
        kinds.append("icon")
    if any(e and e not in ICON_EXTENSIONS for e in exts):
        kinds.append("file")
    if get_fence_filename_patterns(fence) and "file" not in kinds:
        kinds.append("file")
    return normalize_organize_kinds(kinds)


def get_fence_organize_kinds(fence: dict) -> list[str]:
    """Kinds this fence accepts for one-click organize."""
    raw = fence.get("organize_kinds")
    if isinstance(raw, list) and raw:
        return normalize_organize_kinds(raw)
    return kinds_from_legacy_fence(fence)


def get_page_organize_kinds(page: dict | None) -> list[str]:
    """Optional page-level kinds (legacy / tests). Live UI owns kinds on fences.

    Empty list means「no page rule」— empty pages still auto-create a document
    fence for file-kind items via ``resolve_organize_target``.
    """
    if not isinstance(page, dict):
        return []
    raw = page.get("organize_kinds")
    if isinstance(raw, list) and raw:
        return normalize_organize_kinds(raw)
    return []


def page_rules_summary(page: dict | None) -> str:
    """Short label for page organize kinds when present."""
    kinds = get_page_organize_kinds(page)
    if not kinds:
        return ""
    return "、".join(ORGANIZE_KIND_LABELS.get(k, k) for k in kinds)


def get_page_by_id(settings: dict, page_id: int) -> dict | None:
    for page in settings.get("desktop_pages") or []:
        if not isinstance(page, dict):
            continue
        try:
            if int(page.get("id", -1)) == int(page_id):
                return page
        except (TypeError, ValueError):
            continue
    return None


def fences_on_page(settings: dict, page_id: int) -> list[dict]:
    from src.fence_pages import fence_on_page

    return [
        fence
        for fence in settings.get("fences", [])
        if isinstance(fence, dict) and fence_on_page(fence, page_id)
    ]


def item_matches_fence(item: DesktopItem, fence: dict) -> bool:
    kinds = get_fence_organize_kinds(fence)
    if not kinds:
        return False
    return item_organize_kind(item) in kinds


def item_matches_page_rules(item: DesktopItem, page: dict | None) -> bool:
    """True when *page* declares organize kinds that include *item*."""
    kinds = get_page_organize_kinds(page)
    if not kinds:
        return False
    return item_organize_kind(item) in kinds


def match_item_to_fence(
    item: DesktopItem,
    settings: dict,
    *,
    page_id: int | None = None,
) -> dict | None:
    """Return the first matching fence on ``page_id``.

    When the page has fences, only those fences are considered (page rules
    are ignored by the caller). When the page has no fences, returns None.
    """
    if page_id is None:
        try:
            page_id = int(settings.get("current_page", 0))
        except (TypeError, ValueError):
            page_id = 0
    page_fences = fences_on_page(settings, page_id)
    if not page_fences:
        return None
    for fence in page_fences:
        if item_matches_fence(item, fence):
            return fence
    return None


def _resolve_organize_target_on_page(
    item: DesktopItem,
    settings: dict,
    page_id: int,
) -> tuple[str, dict] | None:
    """Resolve against fences on one page; empty pages can claim file items."""
    page_fences = fences_on_page(settings, page_id)
    if page_fences:
        for fence in page_fences:
            if item_matches_fence(item, fence):
                return ("fence", fence)
        return None
    page = get_page_by_id(settings, page_id)
    if page is None:
        return None
    kinds = get_page_organize_kinds(page)
    # Migrated settings strip page kinds — empty pages still absorb「文档」
    # so one-click organize materializes a real fence (not a lost page float).
    if not kinds:
        kinds = ["file"]
    if item_organize_kind(item) in kinds:
        return ("page", page)
    return None


def ensure_organize_fence_for_page(settings: dict, page: dict) -> dict:
    """Reuse or create a document fence on ``page`` (fence-owned kinds only).

    Empty「文档」pages used to only get free-floating icons. Users looking at
   「工作」fences then thought the documents vanished. Always materialize a
    real fence so one-click organize lands in a visible partition.
    """
    import uuid

    try:
        page_id = int(page.get("id", 0))
    except (TypeError, ValueError):
        page_id = 0
    kinds = ["file"]
    existing = fences_on_page(settings, page_id)
    for fence in existing:
        if "file" in get_fence_organize_kinds(fence) or not get_fence_organize_kinds(
            fence
        ):
            # Prefer an existing file-capable fence on this page.
            if "file" in get_fence_organize_kinds(fence):
                return fence
    for fence in existing:
        # Single fence on the page with empty kinds — adopt it for docs.
        if not get_fence_organize_kinds(fence):
            fence["organize_kinds"] = list(kinds)
            return fence

    name = str(page.get("name") or "文档").strip() or "文档"
    # Avoid duplicate names when possible.
    used = {str(f.get("name") or "") for f in (settings.get("fences") or [])}
    fence_name = name
    if fence_name in used:
        fence_name = f"{name}区"
    fence = {
        "id": str(uuid.uuid4()),
        "name": fence_name,
        "folder": fence_name,
        "organize_kinds": list(kinds),
        "extensions": [],
        "filename_patterns": [],
        "page": page_id,
        "pages": [page_id],
        "sort_by": "name",
        "x": 80,
        "y": 80,
        "width": 360,
        "height": 420,
        "visible": True,
        "collapsed": False,
        "virtual_items": [],
        "style": {
            "opacity": 0.94,
            "background": "#FFFFFF",
            "accent": "#0F766E",
            "border_radius": 14,
            "show_title": True,
            "view_mode": "grid",
            "collapsible": True,
        },
    }
    settings.setdefault("fences", []).append(fence)
    return fence


def migrate_page_local_floats_to_organize_fences(settings: dict) -> bool:
    """One-shot: page-local floats on file-rule pages become fence pins."""
    from src.public_desktop import get_public_items, is_shared_public_entry
    from src.public_desktop import is_system_namespace_entry, is_loose_desktop_entry

    changed = False
    by_page: dict[int, list[Path]] = {}
    for entry in list(get_public_items(settings)):
        if not isinstance(entry, dict) or not entry.get("path"):
            continue
        if is_system_namespace_entry(entry) or is_loose_desktop_entry(entry):
            continue
        if is_shared_public_entry(entry):
            continue
        try:
            pid = int(entry.get("page"))
        except (TypeError, ValueError):
            continue
        page = get_page_by_id(settings, pid)
        if page is None:
            continue
        # Only auto-migrate when the page has no file fence yet (or none at all).
        page_fences = fences_on_page(settings, pid)
        if any("file" in get_fence_organize_kinds(f) for f in page_fences):
            # Already has a doc fence — still collect orphans into it below.
            pass
        try:
            path = Path(str(entry["path"]))
        except OSError:
            continue
        by_page.setdefault(pid, []).append(path)

    for pid, paths in by_page.items():
        page = get_page_by_id(settings, pid)
        if page is None or not paths:
            continue
        fence = ensure_organize_fence_for_page(settings, page)
        added = assign_paths_to_virtual_fence(fence, settings, paths)
        if added:
            changed = True
    return changed


def resolve_organize_target(
    item: DesktopItem,
    settings: dict,
    *,
    page_id: int | None = None,
) -> tuple[str, dict] | None:
    """Decide where an item goes for one-click organize.

    Returns ``("fence", fence)``, ``("page", page)`` (empty page → auto fence
    on apply), or None. Prefer the current page, then other pages.
    """
    if page_id is None:
        try:
            page_id = int(settings.get("current_page", 0))
        except (TypeError, ValueError):
            page_id = 0
    hit = _resolve_organize_target_on_page(item, settings, page_id)
    if hit is not None:
        return hit

    for page in settings.get("desktop_pages") or []:
        if not isinstance(page, dict):
            continue
        try:
            pid = int(page.get("id", -1))
        except (TypeError, ValueError):
            continue
        if pid == page_id:
            continue
        hit = _resolve_organize_target_on_page(item, settings, pid)
        if hit is not None:
            return hit
    return None


def organize_accepts_folders(
    settings: dict, *, page_id: int | None = None
) -> bool:
    """True when any fence document rule can accept folders."""
    _ = page_id  # kept for call-site compatibility
    for fence in settings.get("fences") or []:
        if isinstance(fence, dict) and "file" in get_fence_organize_kinds(fence):
            return True
    return False


def any_fence_organizes_folders(settings: dict) -> bool:
    """Backward-compatible: folders accepted on the current page's rules."""
    return organize_accepts_folders(settings)


def resolve_fence_folder(fence: dict) -> Path:
    from src.settings import resolve_fence_storage_path

    name = (fence.get("name") or fence.get("folder") or "").strip() or "未命名"
    path = resolve_fence_storage_path(name)
    path.mkdir(parents=True, exist_ok=True)
    return path


def migrate_fence_storage(settings: dict) -> None:
    """Move files from legacy desktop category folders into app storage."""
    import shutil

    from src.settings import get_desktop_path, get_fence_storage_root

    desktop = get_desktop_path()
    storage_root = get_fence_storage_root()

    for fence in settings.get("fences", []):
        name = (fence.get("name") or fence.get("folder") or "").strip()
        if not name:
            continue

        legacy_names = {name}
        folder = (fence.get("folder") or "").strip()
        if folder:
            legacy_names.add(folder)

        dest_dir = storage_root / name
        dest_dir.mkdir(parents=True, exist_ok=True)

        for folder_name in legacy_names:
            old_dir = desktop / folder_name
            if not old_dir.is_dir():
                continue
            try:
                if old_dir.resolve() == dest_dir.resolve():
                    continue
            except OSError:
                continue

            for item in list(old_dir.iterdir()):
                if item.name.startswith("."):
                    continue
                target = dest_dir / item.name
                if target.exists():
                    continue
                try:
                    shutil.move(str(item), str(target))
                except OSError:
                    pass

            try:
                if old_dir.is_dir() and not any(old_dir.iterdir()):
                    old_dir.rmdir()
            except OSError:
                pass


def fence_rules_summary(fence: dict, max_items: int = 3) -> str:
    del max_items  # kinds are always ≤3
    if is_portal_fence(fence):
        portal = get_portal_path(fence)
        if portal is not None:
            return f"门户：{portal.name}"
        return "门户"
    kinds = get_fence_organize_kinds(fence)
    if not kinds:
        return "—"
    return "、".join(ORGANIZE_KIND_LABELS[k] for k in kinds)


def _norm_virtual_key(path: Path) -> str:
    """Stable path key without symlink resolve (resolve can stall on .lnk)."""
    try:
        p = Path(path)
        if p.is_absolute():
            return str(p)
        return str(p.resolve())
    except OSError:
        return str(path)


def all_fence_pinned_keys(settings: dict) -> set[str]:
    """Case-folded path keys pinned in any fence virtual_items list."""
    keys: set[str] = set()
    for fence in settings.get("fences", []):
        for raw in fence.get("virtual_items") or []:
            try:
                keys.add(_norm_virtual_key(Path(str(raw))).casefold())
            except OSError:
                keys.add(str(raw).casefold())
    return keys


def path_in_pinned_keys(path: Path | str, pinned_keys: set[str]) -> bool:
    """True when ``path`` is already pinned, including desktop stand-in remap.

    Folders dragged into a fence are stored as the real off-desktop path
    (``_prefer_real_folder_pin``). The desktop child (e.g. ``D:\\desktop\\Foo``)
    must still count as claimed so「待整理列表」and loose-float sync hide it.

    Exact / prefer-remap keys only — never unique-basename matching (two
    ``report.xlsx`` in different folders must not collide).
    """
    if not pinned_keys:
        return False
    try:
        p = Path(path)
    except OSError:
        return str(path).casefold() in pinned_keys

    candidates: list[str] = []
    try:
        candidates.append(_norm_virtual_key(p).casefold())
    except OSError:
        candidates.append(str(p).casefold())
    try:
        preferred = _prefer_real_folder_pin(p)
        prefer_key = _norm_virtual_key(preferred).casefold()
        if prefer_key not in candidates:
            candidates.append(prefer_key)
    except OSError:
        pass
    # Slash-normalized variants (settings may mix / and \).
    expanded: list[str] = []
    for key in candidates:
        expanded.append(key)
        slash = key.replace("/", "\\")
        if slash not in expanded:
            expanded.append(slash)
    for key in expanded:
        if key in pinned_keys:
            return True
        # pinned_keys may also use mixed separators.
        if key.replace("\\", "/") in pinned_keys:
            return True
    return False


def _pinned_elsewhere_keys(fence: dict, settings: dict) -> set[str]:
    """Paths explicitly pinned to a different fence (case-insensitive)."""
    fid = fence.get("id")
    keys: set[str] = set()
    for other in settings.get("fences", []):
        if other is fence or other.get("id") == fid:
            continue
        for raw in other.get("virtual_items") or []:
            try:
                keys.add(_norm_virtual_key(Path(str(raw))).casefold())
            except OSError:
                keys.add(str(raw).casefold())
    return keys


def _claimed_key_sets(settings: dict) -> tuple[set[str], set[str]]:
    """Return (all fence pins, public pins) with a short TTL across fence refreshes."""
    now = time.monotonic()
    sid = id(settings)
    if (
        _CLAIMED_CACHE["sid"] == sid
        and now - float(_CLAIMED_CACHE["t"]) < _CLAIMED_CACHE_TTL_S
    ):
        return set(_CLAIMED_CACHE["all_pinned"]), set(_CLAIMED_CACHE["public"])
    from src.public_desktop import public_claimed_keys

    all_pinned = all_fence_pinned_keys(settings)
    public = public_claimed_keys(settings)
    _CLAIMED_CACHE["sid"] = sid
    _CLAIMED_CACHE["t"] = now
    _CLAIMED_CACHE["all_pinned"] = set(all_pinned)
    _CLAIMED_CACHE["public"] = set(public)
    return all_pinned, public


def _prefer_real_folder_pin(path: Path) -> Path:
    """Desktop stand-in folders → real off-desktop path for pins.

    Only remaps direct desktop children that are real directories (or folder
    shortcuts with an absolute filesystem target). Never touches hosted
    namespace icons (This PC / Recycle Bin) or arbitrary .lnk files.
    """
    try:
        from src.win_shell import is_desktop_loose_item, resolve_folder_drop_target

        if not is_desktop_loose_item(path):
            return path
        if path.is_dir():
            return resolve_folder_drop_target(path)
        if path.suffix.lower() == ".lnk":
            from src.win_shell import _lnk_target_dir

            target = _lnk_target_dir(path)
            if target is not None:
                return target
    except OSError:
        pass
    return path


# WScript.Shell CreateShortCut is expensive; cache by path + mtime.
_LNK_TARGET_FILE_CACHE: dict[str, tuple[int, Path | None]] = {}
_MAX_LNK_TARGET_FILE_CACHE = 512

# Desktop exe→unique .lnk map (OLE Baidu-style remap). Rebuild rarely.
_EXE_COVER_CACHE: dict = {"t": 0.0, "map": {}}
_EXE_COVER_TTL_S = 3.0


def _lnk_target_file(path: Path) -> Path | None:
    """Absolute filesystem target of a .lnk (file or dir), or None."""
    if path.suffix.lower() != ".lnk":
        return None
    try:
        key = str(path).casefold()
    except OSError:
        key = str(path).casefold()
    try:
        mtime = int(path.stat().st_mtime_ns)
    except OSError:
        mtime = -1
    cached = _LNK_TARGET_FILE_CACHE.get(key)
    if cached is not None and cached[0] == mtime:
        return cached[1]
    result: Path | None = None
    try:
        import win32com.client

        raw = str(
            win32com.client.Dispatch("WScript.Shell")
            .CreateShortCut(str(path))
            .Targetpath
            or ""
        ).strip()
        if raw and raw not in {".", ".."}:
            target = Path(raw)
            if target.is_absolute():
                result = target
    except Exception:
        result = None
    _LNK_TARGET_FILE_CACHE[key] = (mtime, result)
    if len(_LNK_TARGET_FILE_CACHE) > _MAX_LNK_TARGET_FILE_CACHE:
        # Drop an arbitrary older entry (insertion order in 3.7+).
        _LNK_TARGET_FILE_CACHE.pop(next(iter(_LNK_TARGET_FILE_CACHE)), None)
    return result


def invalidate_exe_cover_cache() -> None:
    """Drop the desktop exe→lnk map (after New shortcut / desktop change)."""
    _EXE_COVER_CACHE["t"] = 0.0
    _EXE_COVER_CACHE["map"] = {}


def _exe_to_unique_desktop_lnk_map() -> dict[str, Path]:
    """Map casefolded exe Targetpath → unique desktop .lnk covering it."""
    now = time.monotonic()
    cached_map = _EXE_COVER_CACHE.get("map")
    if (
        isinstance(cached_map, dict)
        and cached_map
        and now - float(_EXE_COVER_CACHE.get("t", 0.0)) < _EXE_COVER_TTL_S
    ):
        return cached_map
    from src.settings import get_desktop_paths

    counts: dict[str, list[Path]] = {}
    for desk in get_desktop_paths():
        try:
            children = list(desk.iterdir())
        except OSError:
            continue
        for child in children:
            if child.suffix.lower() != ".lnk":
                continue
            dest = _lnk_target_file(child)
            if dest is None or dest.suffix.lower() != ".exe":
                continue
            try:
                key = str(dest).casefold()
            except OSError:
                continue
            counts.setdefault(key, []).append(child)
    mapping: dict[str, Path] = {
        key: hits[0] for key, hits in counts.items() if len(hits) == 1
    }
    _EXE_COVER_CACHE["t"] = now
    _EXE_COVER_CACHE["map"] = mapping
    return mapping


def _desktop_shortcut_covering_target(target: Path) -> Path | None:
    """Desktop ``.lnk`` whose Targetpath is *target* (prefer localized name).

    OLE handoff resolves ``百度网盘.lnk`` → ``BaiduNetdisk.exe``; pinning the
    exe shows an English label and fails to unpin the original shortcut.

    Only used for ``.exe`` remaps — never scan the desktop for Office docs.
    """
    from src.win_shell import is_desktop_loose_item

    try:
        want = str(Path(target)).casefold()
    except OSError:
        return None
    if not want:
        return None
    # Prefer an already-desktop path; never invent shortcuts.
    if is_desktop_loose_item(Path(target)) and Path(target).suffix.lower() == ".lnk":
        return Path(target)
    if Path(target).suffix.lower() != ".exe":
        return None
    return _exe_to_unique_desktop_lnk_map().get(want)


def _canonicalize_virtual_pin_path(path: Path) -> Path:
    """Pin desktop shortcuts by their .lnk path, not the resolved target.

    OLE may hand off ``百度网盘.lnk`` as ``BaiduNetdisk.exe``. Remap only
    executable targets back to a unique desktop shortcut. Scanning every
    desktop ``.lnk`` via COM for Office documents (xlsx/pptx/…) froze the UI
    when dragging docs out of a fence (``get_virtual_items_for_fence``).
    """
    try:
        p = Path(path)
    except OSError:
        return path
    try:
        suffix = p.suffix.lower()
        if suffix == ".lnk":
            return _prefer_real_folder_pin(p)
        # Only exe remaps need a desktop-wide shortcut scan.
        if suffix == ".exe":
            cover = _desktop_shortcut_covering_target(p)
            if cover is not None:
                return cover
        return _prefer_real_folder_pin(p)
    except OSError:
        return p


def _strip_resolved_target_pins(
    settings: dict, *, target: Path | None
) -> None:
    """Remove exe/target pins that duplicate a desktop shortcut pin."""
    if target is None:
        return
    try:
        target_cf = str(Path(target)).casefold()
    except OSError:
        return
    if not target_cf:
        return
    for fence in settings.get("fences") or []:
        if not isinstance(fence, dict):
            continue
        items = list(fence.get("virtual_items") or [])
        filtered = [x for x in items if str(x).casefold() != target_cf]
        if len(filtered) != len(items):
            fence["virtual_items"] = filtered


def _heal_fence_virtual_pins(fence: dict, settings: dict) -> bool:
    """Rewrite OLE / cross-fence pin damage once (write path, not every display).

    Remaps bare ``.exe`` pins to a unique desktop ``.lnk``, drops resolved-target
    dupes beside their shortcut, and strips pins owned by another fence.
    """
    rewritten: list[str] = []
    pins_changed = False
    for raw in fence.get("virtual_items") or []:
        if not raw:
            continue
        try:
            path = _canonicalize_virtual_pin_path(Path(str(raw)))
            key = _norm_virtual_key(path)
            rewritten.append(key)
            raw_cf = str(raw).casefold().replace("/", "\\")
            if key.casefold() != raw_cf:
                pins_changed = True
        except OSError:
            rewritten.append(str(raw))
    if not rewritten:
        return False

    lnk_targets: set[str] = set()
    for key in rewritten:
        if Path(key).suffix.lower() != ".lnk":
            continue
        resolved = _lnk_target_file(Path(key))
        if resolved is None:
            continue
        try:
            lnk_targets.add(str(resolved).casefold())
        except OSError:
            pass
    owned_elsewhere = _pinned_elsewhere_keys(fence, settings)
    deduped: list[str] = []
    seen_keys: set[str] = set()
    for key in rewritten:
        cf = key.casefold()
        if cf in seen_keys or cf in lnk_targets:
            pins_changed = True
            continue
        if cf in owned_elsewhere:
            pins_changed = True
            continue
        seen_keys.add(cf)
        deduped.append(key)
    if deduped != list(rewritten):
        pins_changed = True
        rewritten = deduped
    for key in rewritten:
        if Path(key).suffix.lower() != ".lnk":
            continue
        _strip_resolved_target_pins(settings, target=_lnk_target_file(Path(key)))
    if not pins_changed:
        return False
    fence["virtual_items"] = rewritten
    stored = _settings_fence_for(fence, settings)
    if stored is not fence:
        stored["virtual_items"] = list(rewritten)
    invalidate_claimed_keys_cache()
    return True


def get_virtual_items_for_fence(
    fence: dict,
    settings: dict,
    *,
    all_pinned: set[str] | None = None,
    apply_sort: bool = True,
) -> list[Path]:
    """Files shown in this fence: explicit `virtual_items` pins only.

    Rule matching is applied by ``organize_desktop`` (一键整理) and optional
    watcher auto-pin (``try_auto_pin_desktop_paths``), which write pins.
    Auto-including rule matches here used to pull icons back into their
    original fence after the user dragged them elsewhere — looking like a
    "restore on open".

    Explicit pins in this fence always win: do NOT hide them just because the
    path is still listed under public_desktop_items (stale during public→fence
    drag). That dual-list filter made icons vanish from both places.

    Display is intentionally cheap: no per-pin COM canonicalize / desktop
    shortcut scan. OLE ``.exe`` remaps run on assign via ``_heal_fence_virtual_pins``,
    or once here when a legacy bare ``.exe`` pin is still present.
    """
    from src.desktop_scanner import is_temp_desktop_file

    raw_items = list(fence.get("virtual_items") or [])
    # Legacy OLE damage only — never heal Office docs / normal pins on refresh.
    if any(str(raw).lower().endswith(".exe") for raw in raw_items if raw):
        _heal_fence_virtual_pins(fence, settings)
        raw_items = list(fence.get("virtual_items") or [])

    result: list[Path] = []
    seen: set[str] = set()
    mine: set[str] = set()
    for raw in raw_items:
        if not raw:
            continue
        try:
            mine.add(_norm_virtual_key(Path(str(raw))).casefold())
        except OSError:
            mine.add(str(raw).casefold())

    if all_pinned is None:
        others = all_fence_pinned_keys(settings) - mine
    else:
        others = all_pinned - mine

    def _add(path: Path) -> None:
        try:
            key = _norm_virtual_key(path).casefold()
        except OSError:
            key = str(path).casefold()
        if key in seen or key in others:
            return
        from src.path_stat_cache import path_present

        try:
            if not path_present(path):
                return
        except OSError:
            return
        seen.add(key)
        result.append(path)

    for raw in raw_items:
        if not raw:
            continue
        try:
            path = Path(str(raw))
            if is_temp_desktop_file(path.name):
                continue
            _add(path)
        except OSError:
            continue

    sort_by = str(fence.get("sort_by") or "name")
    if not apply_sort or sort_by == "manual":
        return result
    if sort_by == "date":
        from src.path_stat_cache import path_mtime

        return sorted(result, key=path_mtime, reverse=True)
    if sort_by == "size":
        from src.path_stat_cache import path_size

        return sorted(result, key=path_size, reverse=True)
    return sorted(result, key=lambda p: p.name.lower())


def _settings_fence_for(fence: dict, settings: dict) -> dict:
    """Return the settings-owned fence dict (same id), or ``fence`` as fallback."""
    fid = fence.get("id")
    for stored in settings.get("fences") or []:
        if not isinstance(stored, dict):
            continue
        if stored is fence or (fid and stored.get("id") == fid):
            return stored
    return fence


def assign_paths_to_virtual_fence(
    fence: dict,
    settings: dict,
    paths: list[Path],
    *,
    insert_at: int | None = None,
) -> list[Path]:
    """Pin desktop files to a fence without moving them. Returns newly pinned paths.

    Always writes pins onto the settings-owned fence dict. Live widget configs can
    be a different object with the same id; mutating only the widget used to clear
    public floats while leaving settings.virtual_items empty — icons vanished.
    """
    target = _settings_fence_for(fence, settings)
    # Prefer live widget order when the two dicts have drifted apart.
    if target is not fence:
        live_items = list(fence.get("virtual_items") or [])
        if live_items:
            target["virtual_items"] = live_items
        if fence.get("sort_by"):
            target["sort_by"] = fence.get("sort_by")

    pinned = [_norm_virtual_key(Path(p)) for p in (target.get("virtual_items") or [])]
    pinned_set = {p.casefold() for p in pinned}
    added: list[Path] = []
    new_keys: list[str] = []
    fid = target.get("id")

    from src.desktop_scanner import is_temp_desktop_file
    from src.public_desktop import remove_public_paths

    for path in paths:
        try:
            path = Path(path)
            if is_temp_desktop_file(path.name):
                continue
            path = _canonicalize_virtual_pin_path(path)
            if not path.exists():
                continue
            key = _norm_virtual_key(path)
        except OSError:
            continue
        key_cf = key.casefold()
        # Pinning a desktop .lnk must drop any prior pin of its resolved target
        # (OLE used to leave BaiduNetdisk.exe beside 百度网盘.lnk).
        if Path(key).suffix.lower() == ".lnk":
            resolved = _lnk_target_file(Path(key))
            _strip_resolved_target_pins(settings, target=resolved)
            if resolved is not None:
                try:
                    resolved_cf = str(resolved).casefold()
                except OSError:
                    resolved_cf = ""
                if resolved_cf:
                    pinned = [p for p in pinned if p.casefold() != resolved_cf]
                    pinned_set.discard(resolved_cf)
        for other in settings.get("fences", []):
            if other is target or other is fence or other.get("id") == fid:
                continue
            other_items = list(other.get("virtual_items") or [])
            filtered = [x for x in other_items if str(x).casefold() != key_cf]
            if len(filtered) != len(other_items):
                other["virtual_items"] = filtered
        if key_cf in pinned_set:
            pinned = [p for p in pinned if p.casefold() != key_cf]
            pinned_set.discard(key_cf)
        new_keys.append(key)
        pinned_set.add(key_cf)
        added.append(path)
        # AIGC START — allow This PC / Recycle Bin into fences; keep Explorer copy hidden
        try:
            from src.win_shell import get_lnk_namespace_clsid, host_namespace_icon_in_fence

            ns_clsid = get_lnk_namespace_clsid(Path(key))
            if ns_clsid:
                host_namespace_icon_in_fence(ns_clsid, Path(key).stem, Path(key))
        except Exception:
            pass
        # AIGC END

    if insert_at is None:
        pinned.extend(new_keys)
    else:
        idx = max(0, min(insert_at, len(pinned)))
        pinned[idx:idx] = new_keys
    # Write pins BEFORE clearing public — reverse order used to leave icons in
    # neither list when a later step failed (classic public→fence vanish).
    target["virtual_items"] = pinned
    # Keep the caller's dict in sync when it is a separate live config object.
    if fence is not target:
        fence["virtual_items"] = list(pinned)
        if target.get("sort_by"):
            fence["sort_by"] = target.get("sort_by")
    if added:
        remove_public_paths(settings, added)
        invalidate_claimed_keys_cache()
        try:
            from src.public_desktop import invalidate_loose_sync_cache

            invalidate_loose_sync_cache()
        except Exception:
            pass
        # One write-path heal: remap bare .exe / drop OLE dupes (not on display).
        _heal_fence_virtual_pins(target, settings)
        if fence is not target:
            fence["virtual_items"] = list(target.get("virtual_items") or [])
    return added


def set_virtual_item_order(fence: dict, paths: list[Path]) -> None:
    """Persist a manual icon order (drag-reorder inside a fence)."""
    fence["virtual_items"] = [_norm_virtual_key(p) for p in paths]
    fence["sort_by"] = "manual"
    invalidate_claimed_keys_cache()


def unpin_paths_from_virtual_fence(fence: dict, paths: list[Path]) -> bool:
    """Remove pins so icons leave this fence (files stay on desktop)."""
    if not paths:
        return False
    remove: set[str] = set()
    for path in paths:
        try:
            remove.add(_norm_virtual_key(path).casefold())
        except OSError:
            remove.add(str(path).casefold())
    before = list(fence.get("virtual_items") or [])
    after = [x for x in before if str(x).casefold() not in remove]
    if len(after) == len(before):
        return False
    fence["virtual_items"] = after
    invalidate_claimed_keys_cache()
    return True


def rewrite_pinned_path(settings: dict, old: Path | str, new: Path | str) -> bool:
    """Update fence virtual_items (and public lists) after a filesystem rename."""
    try:
        old_key = _norm_virtual_key(Path(old)).casefold()
        new_path = _norm_virtual_key(Path(new))
    except OSError:
        return False
    changed = False
    for fence in settings.get("fences", []) or []:
        if not isinstance(fence, dict):
            continue
        items = list(fence.get("virtual_items") or [])
        if not items:
            continue
        updated: list[str] = []
        local = False
        for raw in items:
            if not raw:
                continue
            try:
                key = _norm_virtual_key(Path(str(raw))).casefold()
            except OSError:
                key = str(raw).casefold()
            if key == old_key:
                updated.append(new_path)
                local = True
            else:
                updated.append(str(raw))
        if local:
            fence["virtual_items"] = updated
            changed = True
    if changed:
        invalidate_claimed_keys_cache()
    try:
        from src.public_desktop import rewrite_public_path

        if rewrite_public_path(settings, old, new):
            changed = True
    except Exception:
        pass
    return changed


_PRUNE_VIRTUAL_TTL_S = 12.0
# Fences-like sticky pins: keep membership through Office save delete→recreate
# and only drop after the path stays missing for this long.
_STICKY_MISSING_S = 45.0
_prune_virtual_cache: dict[str, float] = {"t": 0.0}
_missing_since: dict[str, float] = {}


def clear_virtual_missing_tracking() -> None:
    """Reset sticky-miss stamps (tests / settings reload)."""
    _missing_since.clear()
    _prune_virtual_cache["t"] = 0.0


def prune_missing_virtual_items(
    settings: dict,
    *,
    force: bool = False,
    sticky_missing_s: float | None = None,
) -> bool:
    """Persist-drop fence pins whose files have been missing long enough.

    Display already skips missing paths. Pins stay sticky across brief gaps
    (Excel/Word save) — only sustained absence removes membership, matching
    Fences-class organizers. ``force`` bypasses the call throttle only; it does
    not skip the sticky-miss window.
    """
    now = time.monotonic()
    if not force and now - float(_prune_virtual_cache.get("t", 0.0)) < _PRUNE_VIRTUAL_TTL_S:
        return False
    grace = float(_STICKY_MISSING_S if sticky_missing_s is None else sticky_missing_s)
    grace = max(0.0, grace)
    changed = False
    for fence in settings.get("fences", []):
        if not isinstance(fence, dict):
            continue
        items = list(fence.get("virtual_items") or [])
        if not items:
            continue
        kept: list[str] = []
        from src.path_stat_cache import path_present

        for raw in items:
            if not raw:
                continue
            try:
                path = Path(str(raw))
                key = str(path).casefold()
                ok = path_present(path)
            except OSError:
                path = Path(str(raw))
                key = str(path).casefold()
                ok = False
            if ok:
                _missing_since.pop(key, None)
                kept.append(str(raw))
                continue
            first = _missing_since.get(key)
            if first is None:
                _missing_since[key] = now
                kept.append(str(raw))
                continue
            if now - float(first) < grace:
                kept.append(str(raw))
                continue
            # Sustained absence — drop pin (Fences-like cleanup).
        if len(kept) != len(items):
            fence["virtual_items"] = kept
            changed = True
    live_keys: set[str] = set()
    for fence in settings.get("fences", []):
        if not isinstance(fence, dict):
            continue
        for raw in fence.get("virtual_items") or []:
            if not raw:
                continue
            try:
                live_keys.add(str(Path(str(raw))).casefold())
            except OSError:
                live_keys.add(str(raw).casefold())
    for stale in list(_missing_since):
        if stale not in live_keys:
            _missing_since.pop(stale, None)
    if changed or not force:
        _prune_virtual_cache["t"] = now
    if changed:
        invalidate_claimed_keys_cache()
    return changed


def _purge_fence_layouts(settings: dict, fence_id: str) -> None:
    """Drop stored per-page geometries for a deleted fence."""
    if not fence_id:
        return
    keys = (str(fence_id), fence_id)
    by_display = settings.get("fence_layouts_by_display")
    if isinstance(by_display, dict):
        for pages in by_display.values():
            if not isinstance(pages, dict):
                continue
            for layout in pages.values():
                if not isinstance(layout, dict):
                    continue
                for key in keys:
                    layout.pop(key, None)
    by_page = settings.get("fence_layouts_by_page")
    if isinstance(by_page, dict):
        for layout in by_page.values():
            if not isinstance(layout, dict):
                continue
            for key in keys:
                layout.pop(key, None)


def dissolve_fence_to_page(
    settings: dict, fence: dict, page_id: int
) -> list[Path]:
    """Delete a fence and place its pins as page-local floats on ``page_id``.

    Files stay on the real desktop; only virtual pins and the fence config move.
    Always page-local (``shared=False``), even when the public area is enabled.
    """
    from src.organize_targets import remove_fence_mappings
    from src.public_desktop import add_public_item, find_auto_slot, live_fence_rects

    paths = list(get_virtual_items_for_fence(fence, settings))
    fid = fence.get("id")
    hint_x = int(fence.get("x", 80) or 80)
    hint_y = int(fence.get("y", 80) or 80)

    fences = settings.setdefault("fences", [])
    settings["fences"] = [f for f in fences if f.get("id") != fid]
    if fid:
        remove_fence_mappings(settings, str(fid))
        _purge_fence_layouts(settings, str(fid))
    invalidate_claimed_keys_cache()

    rects = live_fence_rects(settings, int(page_id))
    placed: list[Path] = []
    for index, path in enumerate(paths):
        sx, sy = find_auto_slot(
            settings,
            page_id=int(page_id),
            hint_x=hint_x + (index % 6) * 12,
            hint_y=hint_y + (index // 6) * 12,
            exclude_path=path,
            fence_rects=rects,
            prefer_nearest=True,
        )
        add_public_item(
            settings,
            path,
            x=sx,
            y=sy,
            page_id=int(page_id),
            fence_rects=rects,
            auto_arrange=False,
            shared=False,
        )
        placed.append(path)
    return placed


def migrate_simplify_fences(settings: dict) -> None:
    """Normalize fence records; keep Folder Portal paths (Fences-like mirrors)."""
    cleaned: list[dict] = []
    for fence in settings.get("fences", []):
        if not isinstance(fence, dict):
            continue
        name = (fence.get("name") or "新分区").strip() or "新分区"
        portal = str(fence.get("portal_path") or "").strip()
        # Legacy: type=portal without path was incomplete — drop only those.
        if fence.get("type") == "portal" and not portal:
            continue
        if portal:
            fence["type"] = "portal"
            fence["portal_path"] = portal
            # Portals are live FS mirrors — pin list is unused for listing.
            fence.setdefault("virtual_items", [])
        else:
            fence.pop("type", None)
            fence.pop("portal_path", None)
        if not fence.get("folder"):
            fence["folder"] = name
        cleaned.append(fence)
    settings["fences"] = cleaned


def is_portal_fence(fence: dict | None) -> bool:
    """True when the fence mirrors a real folder (Folder Portal)."""
    if not isinstance(fence, dict):
        return False
    path = str(fence.get("portal_path") or "").strip()
    if path:
        return True
    return str(fence.get("type") or "").strip().lower() == "portal"


def get_portal_path(fence: dict | None) -> Path | None:
    """Absolute directory mirrored by a portal fence, if configured."""
    if not is_portal_fence(fence):
        return None
    raw = str((fence or {}).get("portal_path") or "").strip()
    if not raw:
        return None
    try:
        path = Path(raw).expanduser()
        if not path.is_absolute():
            path = path.resolve()
        return path
    except OSError:
        return None


def set_portal_path(fence: dict, folder: Path | str | None) -> None:
    """Enable/disable Folder Portal on *fence* (empty clears portal mode)."""
    if folder is None or str(folder).strip() == "":
        fence.pop("portal_path", None)
        fence.pop("type", None)
        return
    try:
        path = Path(folder).expanduser().resolve()
    except OSError:
        path = Path(str(folder))
    fence["portal_path"] = str(path)
    fence["type"] = "portal"


def list_portal_entries(
    fence: dict,
    settings: dict | None = None,
    *,
    limit: int = 200,
) -> list[Path]:
    """Top-level children of the portal folder (non-recursive, Fences-like)."""
    root = get_portal_path(fence)
    if root is None:
        return []
    try:
        if not root.is_dir():
            return []
    except OSError:
        return []
    exclude = list((settings or {}).get("exclude_patterns") or [])
    from src.desktop_scanner import is_ignored_desktop_entry

    entries: list[Path] = []
    try:
        children = list(root.iterdir())
    except OSError:
        return []
    for entry in children:
        try:
            if is_ignored_desktop_entry(entry.name, exclude):
                continue
            # Skip hidden/system clutter in portals.
            if entry.name.startswith("."):
                continue
            entries.append(entry)
        except OSError:
            continue
        if len(entries) >= max(1, int(limit)):
            break
    return entries


def collect_portal_watch_paths(settings: dict) -> list[Path]:
    """Unique existing portal directories to watch for live refresh."""
    seen: set[str] = set()
    out: list[Path] = []
    for fence in settings.get("fences") or []:
        if not isinstance(fence, dict):
            continue
        path = get_portal_path(fence)
        if path is None:
            continue
        try:
            if not path.is_dir():
                continue
            key = str(path.resolve()).casefold()
        except OSError:
            continue
        if key in seen:
            continue
        seen.add(key)
        out.append(path)
    return out


def migrate_fence_rules(settings: dict) -> None:
    """Merge legacy organize_rules + category_fence_map into fence.extensions."""
    from src.fence_layout import ensure_fence_ids
    from src.organize_targets import get_category_fence_map, get_fence_by_id

    ensure_fence_ids(settings)
    organize_rules = settings.get("organize_rules", {})
    filename_rules = settings.get("filename_rules", {})
    fence_map = get_category_fence_map(settings)

    by_folder: dict[str, dict] = {}
    by_name: dict[str, dict] = {}
    for fence in settings.get("fences", []):
        folder = fence.get("folder") or fence.get("name", "")
        if folder:
            by_folder.setdefault(folder, fence)
        by_name.setdefault(fence.get("name", ""), fence)

    for category in organize_rules:
        if category in fence_map:
            continue
        fence = by_folder.get(category) or by_name.get(category)
        if fence and fence.get("id"):
            fence_map[category] = fence["id"]

    for category, fence_id in list(fence_map.items()):
        fence = get_fence_by_id(settings, fence_id)
        if fence is None:
            continue
        exts = list(fence.get("extensions") or [])
        rule_exts = organize_rules.get(category) or []
        for ext in rule_exts:
            next_ext = normalize_extension(ext)
            if next_ext and next_ext not in exts:
                exts.append(next_ext)
        patterns = list(fence.get("filename_patterns") or [])
        for pat in filename_rules.get(category) or []:
            if pat and pat not in patterns:
                patterns.append(pat)
        fence["extensions"] = exts
        fence["filename_patterns"] = patterns

    # Materialize the only supported rule model: icon / file (文档).
    for fence in settings.get("fences", []):
        if not isinstance(fence, dict):
            continue
        kinds = get_fence_organize_kinds(fence)
        fence["organize_kinds"] = kinds
        # Drop legacy per-extension / filename rules from the live config.
        fence["extensions"] = []
        fence["filename_patterns"] = []

    for page in settings.get("desktop_pages") or []:
        if not isinstance(page, dict):
            continue
        page.pop("organize_kinds", None)

    settings["category_fence_map"] = fence_map
    settings["organize_rules"] = {}
    settings["filename_rules"] = {}
