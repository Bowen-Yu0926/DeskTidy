"""Organize whitelist: matching patterns skip pin/organize."""

from __future__ import annotations

import fnmatch
from pathlib import Path


def get_organize_whitelist(settings: dict | None) -> list[str]:
    if not isinstance(settings, dict):
        return []
    raw = settings.get("organize_whitelist")
    if not isinstance(raw, list):
        return []
    out: list[str] = []
    seen: set[str] = set()
    for item in raw:
        text = str(item).strip()
        if not text:
            continue
        key = text.casefold()
        if key in seen:
            continue
        seen.add(key)
        out.append(text)
    return out


def add_organize_whitelist_entry(settings: dict, pattern: str | Path) -> bool:
    """Add a filename or glob. Returns True when the list changed."""
    if isinstance(pattern, Path):
        text = pattern.name.strip()
    else:
        text = str(pattern).strip()
        # Pasted full paths → store basename; keep globs / plain names as-is.
        if text and ("\\" in text or "/" in text) and not any(
            ch in text for ch in "*?["
        ):
            text = Path(text).name.strip()
    if not text:
        return False
    items = get_organize_whitelist(settings)
    if any(x.casefold() == text.casefold() for x in items):
        return False
    items.append(text)
    settings["organize_whitelist"] = items
    return True


def remove_organize_whitelist_entry(settings: dict, pattern: str) -> bool:
    text = str(pattern).strip()
    if not text:
        return False
    items = get_organize_whitelist(settings)
    kept = [x for x in items if x.casefold() != text.casefold()]
    if len(kept) == len(items):
        return False
    settings["organize_whitelist"] = kept
    return True


def is_organize_whitelisted(path: Path | str, settings: dict | None) -> bool:
    """True when basename matches an exact name or glob in the whitelist."""
    patterns = get_organize_whitelist(settings)
    if not patterns:
        return False
    name = Path(path).name
    if not name:
        return False
    name_cf = name.casefold()
    for pattern in patterns:
        pat = pattern.strip()
        if not pat:
            continue
        if "*" in pat or "?" in pat or "[" in pat:
            if fnmatch.fnmatch(name_cf, pat.casefold()):
                return True
            continue
        if name_cf == pat.casefold():
            return True
        # Also allow full-path entries users may paste in.
        try:
            if Path(path).resolve().as_posix().casefold() == Path(pat).resolve().as_posix().casefold():
                return True
        except OSError:
            if str(path).casefold() == pat.casefold():
                return True
    return False
