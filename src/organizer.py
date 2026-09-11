"""Desktop organization — virtual pin-only (Fences-like).

Files stay on the real Desktop. Organize writes fence `virtual_items` pins.
`restore_desktop_from_storage` remains only for one-time legacy warehouse migration.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from src.desktop_scanner import invalidate_desktop_scan_cache, scan_desktop
from src.fence_pages import get_fence_pages
from src.fence_rules import resolve_organize_target
from src.organize_suppress import is_organize_suppressed
from src.organize_whitelist import is_organize_whitelisted


@dataclass
class OrganizeAction:
    source: Path
    destination: Path
    category: str
    success: bool
    message: str = ""


@dataclass
class OrganizeResult:
    actions: list[OrganizeAction] = field(default_factory=list)
    moved_count: int = 0
    skipped_count: int = 0
    error_count: int = 0
    virtual: bool = True
    # Page ids that received new page-local floats (caller may switch UI there).
    float_page_ids: list[int] = field(default_factory=list)

    @property
    def summary(self) -> str:
        return (
            f"已归入分区/分页 {self.moved_count} 个（未移动原文件），"
            f"跳过 {self.skipped_count} 个"
        )


def _page_float_category(settings: dict, page_id: int) -> str:
    """Label for an existing page-local float (not a fence pin)."""
    from src.fence_rules import get_page_by_id

    page = get_page_by_id(settings, page_id)
    name = ""
    if isinstance(page, dict):
        name = str(page.get("name") or "").strip()
    if name:
        return f"已在「{name}」页"
    return "已在分页浮动"


_SYSTEM_NAMESPACE_CLSIDS = (
    "{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
    "{645FF040-5081-101B-9F08-00AA002F954E}",
)

_SYSTEM_NAMESPACE_NAMES = {
    "{20D04FE0-3AEA-1069-A2D8-08002B30309D}": "此电脑",
    "{645FF040-5081-101B-9F08-00AA002F954E}": "回收站",
}


def _public_system_dir() -> Path:
    from src.settings import get_fence_storage_root

    path = get_fence_storage_root() / ".public_system"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _strip_namespace_from_fences(settings: dict) -> bool:
    """Remove This PC / Recycle Bin pins from fences (hosted as floats instead)."""
    from src.public_desktop import is_system_namespace_path

    changed = False
    for fence in settings.get("fences") or []:
        if not isinstance(fence, dict):
            continue
        pinned = list(fence.get("virtual_items") or [])
        if not pinned:
            continue
        filtered: list[str] = []
        for raw in pinned:
            if not raw:
                continue
            try:
                if is_system_namespace_path(raw):
                    changed = True
                    continue
            except Exception:
                pass
            filtered.append(str(raw))
        if len(filtered) != len(pinned):
            fence["virtual_items"] = filtered
            changed = True
    return changed


def restore_native_system_namespace_icons(
    settings: dict,
    *,
    apply: bool = True,
) -> bool:
    """Reveal Explorer native This PC / Recycle Bin and drop DeskTidy copies.

    Use when shell desktop icons are visible (DeskTidy not owning the plate).
    """
    from src.fence_rules import invalidate_claimed_keys_cache
    from src.public_desktop import get_public_items, is_system_namespace_entry
    from src.win_shell import restore_all_hosted_namespace_icons, set_desktop_namespace_icon_visible

    changed = False

    items = get_public_items(settings)
    kept = [entry for entry in items if not is_system_namespace_entry(entry)]
    if len(kept) != len(items):
        items[:] = kept
        changed = True

    if _strip_namespace_from_fences(settings):
        changed = True

    if apply:
        hosted = settings.get("hosted_namespace_icons")
        if isinstance(hosted, dict) and hosted:
            settings["hosted_namespace_icons"] = {}
            changed = True
        try:
            if restore_all_hosted_namespace_icons():
                changed = True
        except Exception:
            pass
        for clsid in _SYSTEM_NAMESPACE_CLSIDS:
            try:
                set_desktop_namespace_icon_visible(clsid, True)
            except Exception:
                pass
        try:
            invalidate_claimed_keys_cache()
        except Exception:
            pass

    return changed


def ensure_hosted_system_namespace_public_icons(
    settings: dict,
    *,
    apply: bool = True,
) -> bool:
    """Host This PC / Recycle Bin while shell icons stay hidden.

    Prefer an existing fence pin. If neither icon is pinned yet, pin both into
    the locked「常用」fence (first-install / organize expectation). Only fall
    back to a shared public float when no common fence exists.
    Never strip fence pins (users may drag system icons into other zones).
    """
    from src.fence_rules import (
        assign_paths_to_virtual_fence,
        invalidate_claimed_keys_cache,
    )
    from src.public_desktop import remove_public_paths
    from src.system_defaults import SYSTEM_COMMON_FENCE_ID, ensure_system_defaults
    from src.win_shell import (
        create_namespace_shortcut,
        get_lnk_namespace_clsid,
        host_namespace_icon_in_fence,
    )

    changed = False
    if apply:
        try:
            if ensure_system_defaults(settings):
                changed = True
        except Exception:
            pass

    system_dir = _public_system_dir()
    existing_by_clsid: dict[str, Path] = {}
    try:
        for lnk in system_dir.glob("*.lnk"):
            clsid = get_lnk_namespace_clsid(lnk)
            if clsid:
                existing_by_clsid[clsid.upper()] = lnk
    except OSError:
        pass

    # Also discover namespace pins already living in fences.
    pinned_by_clsid: dict[str, Path] = {}
    for fence in settings.get("fences") or []:
        if not isinstance(fence, dict):
            continue
        for raw in fence.get("virtual_items") or []:
            try:
                path = Path(str(raw))
            except OSError:
                continue
            clsid = get_lnk_namespace_clsid(path)
            if clsid:
                pinned_by_clsid[clsid.upper()] = path

    common_fence = None
    for fence in settings.get("fences") or []:
        if isinstance(fence, dict) and fence.get("id") == SYSTEM_COMMON_FENCE_ID:
            common_fence = fence
            break

    for clsid in _SYSTEM_NAMESPACE_CLSIDS:
        key = clsid.upper()
        name = _SYSTEM_NAMESPACE_NAMES.get(key, key)
        lnk = pinned_by_clsid.get(key) or existing_by_clsid.get(key)
        if lnk is None or not lnk.exists():
            if not apply:
                continue
            created = create_namespace_shortcut(
                system_dir, f"::{{{key.strip('{}')}}}", name
            )
            if created is None:
                continue
            lnk = created
            changed = True
            existing_by_clsid[key] = lnk
        if apply:
            try:
                host_namespace_icon_in_fence(key, name, lnk)
            except Exception:
                pass
        if key in pinned_by_clsid:
            # Already in a fence — drop any leftover public float for this path.
            if apply:
                try:
                    if remove_public_paths(settings, [lnk]):
                        changed = True
                except Exception:
                    pass
            continue
        if not apply:
            continue
        # Default home:「常用」partition (not a loose float).
        if common_fence is not None:
            added = assign_paths_to_virtual_fence(common_fence, settings, [lnk])
            if added:
                changed = True
                pinned_by_clsid[key] = lnk
            try:
                if remove_public_paths(settings, [lnk]):
                    changed = True
            except Exception:
                pass
            continue
        # No common fence — last-resort shared float so the icon is not lost.
        from src.public_desktop import add_public_item, get_public_items, is_system_namespace_entry

        items = get_public_items(settings)
        present = {
            str(Path(str(e.get("path")))).casefold()
            for e in items
            if isinstance(e, dict) and e.get("path") and is_system_namespace_entry(e)
        }
        for e in items:
            if not isinstance(e, dict) or not e.get("path"):
                continue
            try:
                entry_clsid = get_lnk_namespace_clsid(Path(str(e["path"])))
            except Exception:
                entry_clsid = None
            if entry_clsid and entry_clsid.upper() == key:
                present.add(str(Path(str(e["path"]))).casefold())
        if str(lnk).casefold() in present:
            continue
        add_public_item(
            settings,
            lnk,
            shared=True,
            auto_arrange=True,
        )
        changed = True

    try:
        invalidate_claimed_keys_cache()
    except Exception:
        pass
    return changed


def ensure_system_namespace_in_software_fence(
    settings: dict,
    *,
    apply: bool = True,
) -> list[Path]:
    """Compat: host system icons (fence pin preferred) while shell icons are hidden."""
    if settings.get("hide_shell_icons"):
        ensure_hosted_system_namespace_public_icons(settings, apply=apply)
    else:
        restore_native_system_namespace_icons(settings, apply=apply)
    return []


def _organize_virtual(
    settings: dict,
    *,
    exclude: list[str],
    dry_run: bool,
    persist: bool = True,
) -> OrganizeResult:
    """Pin loose desktop files using page / fence organize rules.

    Prefer the current page; if it has no match, fall through to other pages
    (so documents can land on an empty「文档」page while viewing「工作」).
    Public floats are included unless whitelisted (or system icons).
    """
    from src.fence_rules import (
        all_fence_pinned_keys,
        assign_paths_to_virtual_fence,
        organize_accepts_folders,
        path_in_pinned_keys,
    )
    from src.public_desktop import (
        add_public_item,
        find_public_entry,
        is_shared_public_entry,
        live_fence_rects,
    )

    result = OrganizeResult(virtual=True)
    try:
        page_id = int(settings.get("current_page", 0))
    except (TypeError, ValueError):
        page_id = 0
    fences_by_id = {
        f.get("id"): f for f in settings.get("fences", []) if f.get("id")
    }
    matched_fences: dict[str, list[Path]] = {fid: [] for fid in fences_by_id}
    # (path, target_page_id) for page-rule floats.
    matched_page_paths: list[tuple[Path, int]] = []
    already_pinned = all_fence_pinned_keys(settings)
    seen: set[str] = set()

    def _key(path: Path) -> str:
        try:
            return str(path.resolve()).casefold()
        except OSError:
            return str(path).casefold()

    def _consider_item(item) -> None:
        path = item.path
        key = _key(path)
        if key in seen:
            return
        seen.add(key)
        if is_organize_suppressed(path):
            result.skipped_count += 1
            return
        if is_organize_whitelisted(path, settings):
            result.skipped_count += 1
            result.actions.append(
                OrganizeAction(
                    source=path,
                    destination=path,
                    category="白名单",
                    success=False,
                    message="整理白名单跳过",
                )
            )
            return
        # Namespace shortcuts (此电脑 / 回收站) follow icon rules like other .lnk files.

        existing_float = find_public_entry(settings, path)

        if path_in_pinned_keys(path, already_pinned):
            result.skipped_count += 1
            if dry_run:
                result.actions.append(
                    OrganizeAction(
                        source=path,
                        destination=path,
                        category="已在分区",
                        success=False,
                        message="已在分区",
                    )
                )
            return
        # path_in_pinned_keys covers desktop stand-in ↔ real-folder remap.

        target = resolve_organize_target(item, settings, page_id=page_id)

        def _skip_as_float() -> None:
            nonlocal result
            if is_shared_public_entry(existing_float):
                cat = "已在公共区"
                msg = "已在公共区"
            else:
                try:
                    float_page = int(existing_float.get("page"))
                except (TypeError, ValueError):
                    float_page = page_id
                cat = _page_float_category(settings, float_page)
                msg = "已在分页浮动"
            result.skipped_count += 1
            if dry_run:
                result.actions.append(
                    OrganizeAction(
                        source=path,
                        destination=path,
                        category=cat,
                        success=False,
                        message=msg,
                    )
                )

        if target is None:
            # Already floating with no further rule — never call this「已在分区」.
            if existing_float is not None:
                _skip_as_float()
                return
            result.skipped_count += 1
            result.actions.append(
                OrganizeAction(
                    source=path,
                    destination=path,
                    category="未分类",
                    success=False,
                    message="无匹配整理规则",
                )
            )
            return

        kind, payload = target
        target_page = page_id
        if kind == "page":
            try:
                target_page = int(payload.get("id", page_id))
            except (TypeError, ValueError):
                target_page = page_id
            page_label = str(payload.get("name") or "分页")
            if dry_run:
                # Preview uses the page name; apply creates/reuses a real fence.
                result.moved_count += 1
                if target_page not in result.float_page_ids:
                    result.float_page_ids.append(target_page)
                result.actions.append(
                    OrganizeAction(
                        source=path,
                        destination=path,
                        category=page_label,
                        success=True,
                    )
                )
                return
            # Materialize a fence on the empty page so docs are not "lost" as
            # free floats while the user is staring at another page's fences.
            from src.fence_rules import ensure_organize_fence_for_page

            fence = ensure_organize_fence_for_page(settings, payload)
            fences_by_id[str(fence.get("id"))] = fence
            if target_page not in result.float_page_ids:
                result.float_page_ids.append(target_page)
            kind = "fence"
            payload = fence

        if kind == "fence":
            fid = payload.get("id")
            if not fid:
                result.skipped_count += 1
                return
            label = payload.get("name", "分区")
            matched_fences.setdefault(str(fid), []).append(path)
            for pid in get_fence_pages(payload):
                if pid not in result.float_page_ids:
                    result.float_page_ids.append(pid)
        else:
            label = payload.get("name", "分页")
            matched_page_paths.append((path, target_page))
            if target_page not in result.float_page_ids:
                result.float_page_ids.append(target_page)
        result.moved_count += 1
        result.actions.append(
            OrganizeAction(
                source=path,
                destination=path,
                category=label,
                success=True,
            )
        )

    scan = scan_desktop(exclude=exclude)
    # Files + shortcuts always; folders only when current-page rules allow.
    for item in scan.loose_files:
        _consider_item(item)
    if organize_accepts_folders(settings, page_id=page_id):
        for item in scan.items:
            if item.is_dir:
                _consider_item(item)

    if dry_run:
        return result

    # Virtual organize must not strip hosted This PC / Recycle Bin while shell
    # icons stay hidden — that left natives invisible and nothing in「常用」.
    if settings.get("hide_shell_icons"):
        ensure_hosted_system_namespace_public_icons(settings, apply=True)
    else:
        restore_native_system_namespace_icons(settings, apply=True)

    for fid, paths in matched_fences.items():
        if not paths:
            continue
        fence = fences_by_id.get(fid)
        if fence is None:
            continue
        assign_paths_to_virtual_fence(fence, settings, paths)

    if matched_page_paths:
        rects_by_page: dict[int, list] = {}
        for path, target_page in matched_page_paths:
            if target_page not in rects_by_page:
                rects_by_page[target_page] = live_fence_rects(settings, target_page)
            add_public_item(
                settings,
                path,
                page_id=target_page,
                fence_rects=rects_by_page[target_page],
                auto_arrange=True,
                shared=False,
            )

    invalidate_desktop_scan_cache()

    if not dry_run and persist:
        from src.settings import looks_like_full_settings, load_settings, save_settings

        try:
            if looks_like_full_settings(settings):
                save_settings(settings)
            else:
                full = load_settings()
                by_id = {
                    f.get("id"): f
                    for f in settings.get("fences", [])
                    if isinstance(f, dict) and f.get("id")
                }
                for fence in full.get("fences", []):
                    src = by_id.get(fence.get("id"))
                    if not src:
                        continue
                    if "virtual_items" in src:
                        fence["virtual_items"] = list(src.get("virtual_items") or [])
                if isinstance(settings.get("public_desktop_items"), list):
                    full["public_desktop_items"] = list(settings["public_desktop_items"])
                save_settings(full)
        except OSError:
            pass
    return result


def organize_desktop(
    settings: dict,
    exclude: list[str] | None = None,
    dry_run: bool = False,
    organize_mode: str | None = None,
    *,
    persist: bool = True,
) -> OrganizeResult:
    """Pin matching desktop files into fences. `organize_mode` is ignored (virtual-only)."""
    del organize_mode  # retained for call-site compatibility
    exclude = exclude if exclude is not None else settings.get("exclude_patterns", [])
    settings["organize_mode"] = "virtual"
    return _organize_virtual(
        settings, exclude=exclude, dry_run=dry_run, persist=persist
    )


def desktop_item_from_path(path: Path) -> "DesktopItem | None":
    """Build a ``DesktopItem`` for a single on-disk path (watcher / live pin)."""
    from src.desktop_scanner import DesktopItem, is_temp_desktop_file

    try:
        path = Path(path)
        if not path.exists():
            return None
        if is_temp_desktop_file(path.name):
            return None
        is_dir = path.is_dir()
        size = 0 if is_dir else int(path.stat().st_size)
        return DesktopItem(
            path=path,
            name=path.name,
            is_dir=is_dir,
            extension="" if is_dir else path.suffix.lower(),
            size=size,
        )
    except OSError:
        return None


def try_auto_pin_desktop_paths(
    settings: dict,
    paths: list[Path | str],
    *,
    page_id: int | None = None,
) -> tuple[list[Path], list[int]]:
    """Pin newly arrived desktop paths that match organize rules (Fences-like).

    Returns ``(newly_pinned_paths, affected_page_ids)``.
    """
    from src.desktop_scanner import is_ignored_desktop_entry
    from src.fence_rules import (
        all_fence_pinned_keys,
        assign_paths_to_virtual_fence,
        ensure_organize_fence_for_page,
        path_in_pinned_keys,
        resolve_organize_target,
    )
    from src.public_desktop import is_system_namespace_path

    if page_id is None:
        try:
            page_id = int(settings.get("current_page", 0))
        except (TypeError, ValueError):
            page_id = 0

    exclude = list(settings.get("exclude_patterns") or [])
    already_pinned = all_fence_pinned_keys(settings)
    pinned_now: list[Path] = []
    affected_pages: set[int] = set()
    seen: set[str] = set()

    for raw in paths:
        try:
            path = Path(raw)
        except (TypeError, ValueError, OSError):
            continue
        try:
            key = str(path).casefold()
        except OSError:
            key = str(path).casefold()
        if key in seen:
            continue
        seen.add(key)

        if is_ignored_desktop_entry(path.name, exclude):
            continue
        if is_organize_suppressed(path):
            continue
        if is_organize_whitelisted(path, settings):
            continue
        if is_system_namespace_path(path):
            continue
        if path_in_pinned_keys(path, already_pinned):
            continue

        item = desktop_item_from_path(path)
        if item is None:
            continue

        target = resolve_organize_target(item, settings, page_id=page_id)
        if target is None:
            continue
        kind, payload = target
        fence = None
        if kind == "fence":
            fence = payload
        elif kind == "page":
            fence = ensure_organize_fence_for_page(settings, payload)
        if fence is None:
            continue
        added = assign_paths_to_virtual_fence(fence, settings, [item.path])
        if added:
            pinned_now.extend(added)
            for pid in get_fence_pages(fence):
                affected_pages.add(int(pid))
            for p in added:
                try:
                    already_pinned.add(str(p).casefold())
                except OSError:
                    pass

    if pinned_now:
        invalidate_desktop_scan_cache()
    return pinned_now, sorted(affected_pages)


@dataclass
class OrganizePreviewPin:
    name: str
    category: str
    target: str


@dataclass
class OrganizePreviewBucket:
    key: str
    title: str
    count: int
    names: list[str] = field(default_factory=list)
    list_names: bool = True
    hint: str = ""


@dataclass
class OrganizePreviewModel:
    """Structured dry-run report for the organize preview panel."""

    pins: list[OrganizePreviewPin]
    buckets: list[OrganizePreviewBucket]
    scan_total: int
    will_count: int
    skip_count: int
    pin_total: int
    pin_omitted: int = 0


def build_organize_preview_model(
    result: OrganizeResult,
    settings: dict,
    *,
    unclaimed_folders: list[str] | None = None,
    max_names: int = 40,
) -> OrganizePreviewModel:
    """Build counts + sample names for UI / text preview (same breakdown rules)."""
    from collections import defaultdict

    from src.organize_targets import organize_destination_label

    by_cat: dict[str, list[str]] = defaultdict(list)
    for action in result.actions:
        by_cat[action.category].append(action.source.name)

    matched = [a for a in result.actions if a.success]
    pins = [
        OrganizePreviewPin(
            name=action.source.name,
            category=action.category,
            target=organize_destination_label(action.category, settings),
        )
        for action in matched[:max_names]
    ]
    pin_omitted = max(0, len(matched) - len(pins))

    buckets: list[OrganizePreviewBucket] = []

    def _add(
        key: str,
        title: str,
        names: list[str],
        *,
        list_names: bool = True,
        hint: str = "",
    ) -> None:
        if not names:
            return
        show = list(names[:max_names]) if list_names else []
        buckets.append(
            OrganizePreviewBucket(
                key=key,
                title=title,
                count=len(names),
                names=show,
                list_names=list_names,
                hint=hint,
            )
        )

    # Already-organized: count only — no name dump.
    _add("already_fence", "已在分区中", by_cat.get("已在分区", []), list_names=False)
    _add("already_public", "已在公共区", by_cat.get("已在公共区", []), list_names=False)
    page_float_names: list[str] = []
    page_float_titles: list[str] = []
    for cat, names in by_cat.items():
        if cat.startswith("已在「") or cat == "已在分页浮动":
            page_float_names.extend(names)
            page_float_titles.append(cat)
    if page_float_names:
        title = (
            page_float_titles[0]
            if len(set(page_float_titles)) == 1
            else "已在分页浮动"
        )
        _add("page_float", title, page_float_names, list_names=False)

    _add("whitelist", "整理白名单跳过", by_cat.get("白名单", []))
    _add("unmatched", "未匹配任何整理规则", by_cat.get("未分类", []))

    folders = list(unclaimed_folders or [])
    if folders:
        from src.fence_rules import organize_accepts_folders

        if organize_accepts_folders(settings):
            _add("folders", "未归入的文件夹", folders)
        else:
            _add(
                "folders_blocked",
                "桌面文件夹（未勾选「文档」规则）",
                folders,
                hint="在分区整理规则中勾选「文档」，或手动拖入分区。",
            )

    return OrganizePreviewModel(
        pins=pins,
        buckets=buckets,
        scan_total=result.moved_count + result.skipped_count,
        will_count=result.moved_count,
        skip_count=result.skipped_count,
        pin_total=len(matched),
        pin_omitted=pin_omitted,
    )


def format_organize_preview(
    result: OrganizeResult,
    settings: dict,
    *,
    unclaimed_folders: list[str] | None = None,
    max_names: int = 40,
) -> str:
    """Human-readable dry-run report with clear counts (not a misleading lump sum)."""
    model = build_organize_preview_model(
        result,
        settings,
        unclaimed_folders=unclaimed_folders,
        max_names=max_names,
    )
    lines: list[str] = [f"将归入分区：{model.pin_total} 个"]
    if model.pins:
        for pin in model.pins:
            lines.append(f"  • {pin.name} → [{pin.category}] → {pin.target}")
        if model.pin_omitted:
            lines.append(f"  …另有 {model.pin_omitted} 个")
    else:
        lines.append("  （没有可按规则钉入的新文件）")

    for bucket in model.buckets:
        if bucket.list_names:
            lines.append(f"{bucket.title}：{bucket.count} 个")
            for name in bucket.names:
                lines.append(f"  • {name}")
            omitted = bucket.count - len(bucket.names)
            if omitted > 0:
                lines.append(f"  …另有 {omitted} 个")
            if bucket.hint:
                lines.append(f"  提示：{bucket.hint}")
        else:
            lines.append(f"{bucket.title}（不列出）：{bucket.count} 个")

    lines.append("")
    lines.append(
        f"合计扫描文件 {model.scan_total} 个"
        f"（将整理 {model.will_count} / 跳过 {model.skip_count}）"
    )
    return "\n".join(lines)


def get_fence_stats(settings: dict, exclude: list[str] | None = None) -> dict[str, int]:
    """Count loose files that organize would consider (same skip rules)."""
    from src.fence_rules import (
        all_fence_pinned_keys,
        fences_on_page,
        get_page_by_id,
        organize_accepts_folders,
        path_in_pinned_keys,
        resolve_organize_target,
    )
    from src.public_desktop import is_system_namespace_path

    exclude = exclude if exclude is not None else settings.get("exclude_patterns", [])
    scan = scan_desktop(exclude=exclude)
    stats: dict[str, int] = {}

    try:
        page_id = int(settings.get("current_page", 0))
    except (TypeError, ValueError):
        page_id = 0
    page_fences = fences_on_page(settings, page_id)
    if page_fences:
        for fence in page_fences:
            stats[fence.get("name", "分区")] = 0
    else:
        page = get_page_by_id(settings, page_id)
        if page is not None:
            stats[page.get("name", "分页")] = 0

    stats["未分类"] = 0
    pinned = all_fence_pinned_keys(settings)

    candidates = list(scan.loose_files)
    if organize_accepts_folders(settings, page_id=page_id):
        candidates.extend(item for item in scan.items if item.is_dir)

    for item in candidates:
        if path_in_pinned_keys(item.path, pinned):
            continue
        if is_system_namespace_path(item.path):
            continue
        if is_organize_whitelisted(item.path, settings):
            continue
        if is_organize_suppressed(item.path):
            continue
        target = resolve_organize_target(item, settings, page_id=page_id)
        if target is None:
            name = "未分类"
        else:
            _kind, payload = target
            name = payload.get("name", "分区")
        stats[name] = stats.get(name, 0) + 1
    return stats


def restore_desktop_from_storage() -> int:
    """One-time / recovery: move leftover warehouse files back to the desktop.

    Not part of the normal virtual product path. Used when migrating off physical
    mode or when leftover storage copies exist after a legacy physical install.
    Shell namespace shortcuts under storage are removed; real icons are restored
    separately via registry.
    """
    from src.settings import (
        clear_storage_origins,
        get_desktop_path,
        get_fence_storage_root,
        get_storage_origin,
    )
    from src.win_shell import (
        get_lnk_namespace_clsid,
        refresh_desktop,
        unique_dest_path,
    )

    default_desktop = get_desktop_path()
    try:
        default_desktop.mkdir(parents=True, exist_ok=True)
    except OSError:
        pass

    storage_root = get_fence_storage_root()
    if not storage_root.exists():
        clear_storage_origins()
        return 0

    restored = 0
    for fence_dir in list(storage_root.iterdir()):
        if not fence_dir.is_dir():
            continue
        # Keep .public_system for hosted namespace fake shortcuts.
        if fence_dir.name.startswith("."):
            continue
        for item in list(fence_dir.iterdir()):
            if item.name.startswith("."):
                continue
            if not item.is_file():
                continue
            try:
                if get_lnk_namespace_clsid(item):
                    item.unlink(missing_ok=True)
                    restored += 1
                    continue
            except OSError:
                pass

            origin = get_storage_origin(item)
            desktop = origin if origin is not None else default_desktop
            try:
                desktop.mkdir(parents=True, exist_ok=True)
            except OSError:
                desktop = default_desktop

            candidate = desktop / item.name
            if candidate.exists():
                try:
                    item.unlink(missing_ok=True)
                except OSError:
                    pass
                restored += 1
                continue

            dest = unique_dest_path(desktop / item.name)
            try:
                import shutil

                shutil.move(str(item), str(dest))
                restored += 1
            except OSError:
                pass

        # Remove empty fence dirs after restore.
        try:
            if fence_dir.exists() and not any(fence_dir.iterdir()):
                fence_dir.rmdir()
        except OSError:
            pass

    clear_storage_origins()
    try:
        refresh_desktop()
    except Exception:
        pass
    invalidate_desktop_scan_cache()
    return restored


def legacy_warehouse_has_files() -> bool:
    """True when non-hidden fence warehouse files remain (pre-virtual leftovers)."""
    from src.settings import get_fence_storage_root

    root = get_fence_storage_root()
    if not root.exists():
        return False
    try:
        # Depth-limited: leftovers sit in ``storage/<fence>/file``. A full
        # recursive walk of user copies stalled the 2s startup migrate tick.
        for child in root.iterdir():
            if child.name.startswith("."):
                continue
            try:
                if child.is_file():
                    return True
                if not child.is_dir():
                    continue
                for nested in child.iterdir():
                    if nested.name.startswith("."):
                        continue
                    if nested.is_file():
                        return True
            except OSError:
                continue
    except OSError:
        return False
    return False
