"""Rule engine for desktop file categorization."""

from __future__ import annotations

import fnmatch
from pathlib import Path

from src.desktop_scanner import DesktopItem


def match_extension(extension: str, rules: dict[str, list[str]]) -> str | None:
    ext = extension.lower()
    if not ext.startswith("."):
        ext = f".{ext}"
    for category, extensions in rules.items():
        if ext in [e.lower() for e in extensions]:
            return category
    return None


def match_filename(name: str, filename_rules: dict[str, list[str]]) -> str | None:
    lower_name = name.lower()
    for category, patterns in filename_rules.items():
        for pattern in patterns:
            pat = pattern.lower()
            if fnmatch.fnmatch(lower_name, pat) or pat in lower_name:
                return category
    return None


def match_category(
    item: DesktopItem,
    extension_rules: dict[str, list[str]],
    filename_rules: dict[str, list[str]] | None = None,
) -> str | None:
    """Match a desktop item to a category. Extension rules take priority."""
    category = match_extension(item.extension, extension_rules)
    if category:
        return category
    if filename_rules:
        return match_filename(item.name, filename_rules)
    return None


def resolve_fence_folder(folder_name: str) -> Path:
    """Resolve fence data source path on desktop."""
    from src.settings import get_desktop_path

    return get_desktop_path() / folder_name


def get_virtual_fence_items(
    category: str,
    extension_rules: dict[str, list[str]],
    filename_rules: dict[str, list[str]] | None,
    exclude: list[str] | None,
    categories: list[str] | None = None,
) -> list[Path]:
    """Get desktop loose files matching categories (virtual organize mode)."""
    from src.desktop_scanner import scan_desktop

    allowed = {c for c in (categories or [category]) if c}
    scan = scan_desktop(exclude=exclude)
    result: list[Path] = []
    for item in scan.loose_files:
        cat = match_category(item, extension_rules, filename_rules)
        if cat in allowed:
            result.append(item.path)
    return sorted(result, key=lambda p: p.name.lower())
