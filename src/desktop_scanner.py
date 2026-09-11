"""Desktop scanning utilities."""

from __future__ import annotations

import fnmatch
import stat as stat_mod
import time
from dataclasses import dataclass, field
from pathlib import Path

from src.settings import get_desktop_paths

_SCAN_CACHE_TTL_S = 2.5
_scan_cache: dict = {}


_OFFICE_TEMP_SUFFIXES = (
    ".wbk",  # Word backup while editing
    ".asd",  # Word auto-recovery
    ".xlk",  # Excel backup
    ".lck",  # Generic lock
    ".temp",
    ".part",
    ".partial",
)


def is_temp_desktop_file(name: str) -> bool:
    """Office lock files (~$...) and similar desktop temp artifacts."""
    if name.startswith("~$"):
        return True
    # Office / WPS scratch files beside the document (~WRL*.tmp, ~foo.docx, …).
    if name.startswith("~"):
        return True
    lower = name.lower()
    if ".tmp." in lower:
        return True
    if lower.endswith(".tmp") or lower.endswith(".download") or lower.endswith(".crdownload"):
        return True
    if any(lower.endswith(suffix) for suffix in _OFFICE_TEMP_SUFFIXES):
        return True
    return False


def _name_matches_exclude(name: str, pattern: str) -> bool:
    """Match exclude entry against a desktop basename.

    Plain patterns (``DeskTidy``, ``desktop.ini``) match that exact name or the
    same stem with an extension (``DeskTidy.lnk``), **not** substrings like
    ``DeskTidy_20260908_100919.mp4`` (screen recordings). Glob wildcards use
    ``fnmatch``.
    """
    raw = (pattern or "").strip()
    if not raw:
        return False
    n = name.casefold()
    p = raw.casefold()
    if any(ch in p for ch in "*?["):
        return fnmatch.fnmatchcase(n, p)
    if n == p:
        return True
    stem, sep, _ext = n.rpartition(".")
    if sep and stem == p:
        return True
    return False


def is_ignored_desktop_entry(name: str, exclude: list[str] | None = None) -> bool:
    """True when a desktop entry should be hidden from scans, fences, and organize."""
    if is_temp_desktop_file(name):
        return True
    if name.startswith("."):
        return True
    exclude = exclude or []
    return any(_name_matches_exclude(name, pattern) for pattern in exclude)


@dataclass
class DesktopItem:
    path: Path
    name: str
    is_dir: bool
    extension: str
    size: int


@dataclass
class DesktopScanResult:
    items: list[DesktopItem] = field(default_factory=list)
    total_files: int = 0
    total_folders: int = 0
    total_size: int = 0

    @property
    def loose_files(self) -> list[DesktopItem]:
        return [item for item in self.items if not item.is_dir]


def invalidate_desktop_scan_cache() -> None:
    """Drop cached desktop listings (after file create/delete/move)."""
    _scan_cache.clear()
    try:
        from src.path_stat_cache import invalidate_path_stat_cache

        invalidate_path_stat_cache()
    except Exception:
        pass
    try:
        from src.public_desktop import invalidate_loose_sync_cache

        invalidate_loose_sync_cache()
    except Exception:
        pass


def scan_desktop(exclude: list[str] | None = None) -> DesktopScanResult:
    exclude = exclude or []
    key = tuple(sorted(exclude))
    now = time.monotonic()
    cached = _scan_cache.get(key)
    if cached is not None and now - cached[0] < _SCAN_CACHE_TTL_S:
        return cached[1]

    result = _scan_desktop_uncached(exclude)
    _scan_cache[key] = (now, result)
    return result


def _scan_desktop_uncached(exclude: list[str]) -> DesktopScanResult:
    result = DesktopScanResult()
    seen_keys: set[str] = set()

    for desktop in get_desktop_paths():
        try:
            if not desktop.is_dir():
                continue
        except OSError:
            continue

        try:
            entries = list(desktop.iterdir())
        except OSError:
            continue

        for entry in entries:
            if is_ignored_desktop_entry(entry.name, exclude):
                continue

            # abspath/normcase is enough for desktop dedupe and cheaper than resolve().
            try:
                key = str(entry).casefold()
            except OSError:
                key = entry.name.casefold()
            if key in seen_keys:
                continue
            seen_keys.add(key)

            try:
                st = entry.stat()
                is_dir = stat_mod.S_ISDIR(st.st_mode)
                item = DesktopItem(
                    path=entry,
                    name=entry.name,
                    is_dir=is_dir,
                    extension="" if is_dir else entry.suffix.lower(),
                    size=0 if is_dir else int(st.st_size),
                )
                result.items.append(item)
                if item.is_dir:
                    result.total_folders += 1
                else:
                    result.total_files += 1
                    result.total_size += item.size
            except OSError:
                continue

    result.items.sort(key=lambda x: (not x.is_dir, x.name.lower()))
    return result


def purge_temp_from_settings(settings: dict) -> list[Path]:
    """Remove temp-file paths from fence pins and the public desktop area."""
    removed: list[Path] = []
    for fence in settings.get("fences", []):
        items = list(fence.get("virtual_items") or [])
        kept: list[str] = []
        for raw in items:
            name = Path(str(raw)).name
            if is_temp_desktop_file(name):
                removed.append(Path(str(raw)))
            else:
                kept.append(str(raw))
        if len(kept) != len(items):
            fence["virtual_items"] = kept

    from src.public_desktop import get_public_items

    public = get_public_items(settings)
    kept_public: list[dict] = []
    for entry in public:
        if not isinstance(entry, dict):
            continue
        raw = entry.get("path")
        if raw and is_temp_desktop_file(Path(str(raw)).name):
            removed.append(Path(str(raw)))
            continue
        kept_public.append(entry)
    if len(kept_public) != len(public):
        settings["public_desktop_items"] = kept_public
    return removed


def delete_temp_files_on_desktop() -> list[Path]:
    """Delete temp files on configured desktop paths."""
    deleted: list[Path] = []
    for desktop in get_desktop_paths():
        if not desktop.exists():
            continue
        for entry in desktop.iterdir():
            if not entry.is_file() or not is_temp_desktop_file(entry.name):
                continue
            try:
                entry.unlink()
                deleted.append(entry)
            except OSError:
                continue
    invalidate_desktop_scan_cache()
    return deleted


def format_size(size: int) -> str:
    for unit in ("字节", "KB", "MB", "GB"):
        if size < 1024:
            return f"{size:.1f} {unit}" if unit != "字节" else f"{size} {unit}"
        size /= 1024
    return f"{size:.1f} TB"
