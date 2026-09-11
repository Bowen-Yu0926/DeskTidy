"""Map file categories to desktop fence destinations."""

from __future__ import annotations

from pathlib import Path

from src.settings import get_desktop_path


def get_category_fence_map(settings: dict) -> dict[str, str]:
    return settings.setdefault("category_fence_map", {})


def get_fence_by_id(settings: dict, fence_id: str) -> dict | None:
    for fence in settings.get("fences", []):
        if fence.get("id") == fence_id:
            return fence
    return None


def fence_label(fence: dict) -> str:
    name = fence.get("name", "分区")
    return name


def list_organize_fences(settings: dict) -> list[dict]:
    """Fences that can receive organized desktop files."""
    return list(settings.get("fences", []))


def migrate_category_fence_map(settings: dict) -> None:
    """Link categories to fences when folder/name matches and no map exists yet."""
    from src.fence_layout import ensure_fence_ids

    ensure_fence_ids(settings)
    fence_map = get_category_fence_map(settings)
    fences = settings.get("fences", [])
    by_folder: dict[str, dict] = {}
    by_name: dict[str, dict] = {}
    for fence in fences:
        folder = fence.get("folder") or fence.get("name", "")
        if folder:
            by_folder.setdefault(folder, fence)
        by_name.setdefault(fence.get("name", ""), fence)

    for category in settings.get("organize_rules", {}):
        if category in fence_map:
            continue
        fence = by_folder.get(category) or by_name.get(category)
        if fence and fence.get("id"):
            fence_map[category] = fence["id"]

    for category, fence_id in list(fence_map.items()):
        if not get_fence_by_id(settings, fence_id):
            fence_map.pop(category, None)


def set_category_fence(settings: dict, category: str, fence_id: str | None) -> None:
    fence_map = get_category_fence_map(settings)
    if not fence_id:
        fence_map.pop(category, None)
        return
    fence_map[category] = fence_id


def remove_fence_mappings(settings: dict, fence_id: str) -> None:
    fence_map = get_category_fence_map(settings)
    for category, mapped_id in list(fence_map.items()):
        if mapped_id == fence_id:
            fence_map.pop(category, None)


def resolve_organize_folder(category: str, settings: dict) -> Path:
    """Legacy helper: returns a display folder path (files are no longer moved)."""
    from src.fence_rules import resolve_fence_folder

    desktop = get_desktop_path()
    for fence in settings.get("fences", []):
        if fence.get("name") == category or fence.get("folder") == category:
            return resolve_fence_folder(fence)

    fence_map = get_category_fence_map(settings)
    fence_id = fence_map.get(category)
    if fence_id:
        fence = get_fence_by_id(settings, fence_id)
        if fence:
            return resolve_fence_folder(fence)
    return desktop / category


def organize_destination_label(name: str, settings: dict) -> str:
    """Human label for organize preview — pin target fence or page."""
    for fence in settings.get("fences", []):
        if fence.get("name") == name or fence.get("folder") == name:
            return f"分区「{fence_label(fence)}」"

    fence_map = get_category_fence_map(settings)
    fence_id = fence_map.get(name)
    if fence_id:
        fence = get_fence_by_id(settings, fence_id)
        if fence:
            return f"分区「{fence_label(fence)}」"

    for page in settings.get("desktop_pages") or []:
        if isinstance(page, dict) and str(page.get("name") or "") == name:
            return f"分页「{name}」桌面"
    return f"分区「{name}」"
