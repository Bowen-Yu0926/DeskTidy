"""Configured folder shortcuts shown on the desktop page indicator."""

from __future__ import annotations

import os
from pathlib import Path


def get_page_folder_items(settings: dict) -> list[dict]:
    """Return normalized ``[{name, path}, ...]`` for the page-indicator strip."""
    raw = settings.get("page_folders")
    if not isinstance(raw, list):
        return []
    items: list[dict] = []
    for entry in raw:
        if not isinstance(entry, dict):
            continue
        path = str(entry.get("path") or "").strip()
        if not path:
            continue
        name = str(entry.get("name") or "").strip() or Path(path).name or path
        items.append({"name": name, "path": path})
    return items


def set_page_folder_items(settings: dict, items: list[dict]) -> None:
    settings["page_folders"] = [
        {"name": str(e.get("name") or "").strip() or Path(str(e.get("path"))).name,
         "path": str(e.get("path") or "").strip()}
        for e in items
        if isinstance(e, dict) and str(e.get("path") or "").strip()
    ]


def open_folder_path(path: str | Path) -> Path:
    """Open a folder in Explorer; create it if missing."""
    folder = Path(str(path))
    folder.mkdir(parents=True, exist_ok=True)
    if not folder.is_dir():
        raise ValueError(f"文件夹不可用：{folder}")
    os.startfile(str(folder))  # type: ignore[attr-defined]
    return folder
