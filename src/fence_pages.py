"""Fence multi-page membership helpers."""

from __future__ import annotations


def get_fence_pages(fence: dict) -> list[int]:
    """Return page ids for a fence. Supports legacy single `page` and `pages` list."""
    raw = fence.get("pages")
    if isinstance(raw, list) and raw:
        return [int(value) for value in raw]
    return [int(fence.get("page", 0))]


def set_fence_pages(fence: dict, page_ids: list[int] | None) -> None:
    """Persist page membership, keeping legacy `page` in sync."""
    ids = [int(value) for value in (page_ids or [])]
    if not ids:
        ids = [0]
    # de-dupe while preserving order
    seen: set[int] = set()
    unique: list[int] = []
    for page_id in ids:
        if page_id in seen:
            continue
        seen.add(page_id)
        unique.append(page_id)
    fence["pages"] = unique
    fence["page"] = unique[0]


def fence_on_page(fence: dict, page_id: int) -> bool:
    return int(page_id) in get_fence_pages(fence)
