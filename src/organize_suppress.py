"""Temporarily skip auto-organize / watcher noise for paths DeskTidy just touched.

Keys are **full path** (casefolded), not basename. Basename keys collided when
pet trash / fence delete / folder-move / paste shared a filename — a new desktop
file with the same name was ignored for ~8–12s.
"""

from __future__ import annotations

import time
from pathlib import Path

# casefolded absolute-or-as-given path -> unix expire time
_SUPPRESSED: dict[str, float] = {}


def _suppress_key(path: Path | str) -> str:
    raw = str(path).strip()
    if not raw:
        return ""
    try:
        return str(Path(raw)).casefold()
    except OSError:
        return raw.casefold()


def suppress_desktop_item(path: Path | str, seconds: float = 12.0) -> None:
    """Ignore this path during auto-organize / watcher for a short window."""
    key = _suppress_key(path)
    if not key:
        return
    _SUPPRESSED[key] = time.monotonic() + max(1.0, float(seconds))


def clear_organize_suppress(path: Path | str | None = None) -> None:
    """Drop one path from the suppress map, or clear everything."""
    if path is None:
        _SUPPRESSED.clear()
        return
    key = _suppress_key(path)
    if key:
        _SUPPRESSED.pop(key, None)


def is_organize_suppressed(path: Path | str) -> bool:
    key = _suppress_key(path)
    if not key:
        return False
    now = time.monotonic()
    expired = [k for k, until in _SUPPRESSED.items() if until <= now]
    for k in expired:
        _SUPPRESSED.pop(k, None)
    until = _SUPPRESSED.get(key)
    return until is not None and until > now
