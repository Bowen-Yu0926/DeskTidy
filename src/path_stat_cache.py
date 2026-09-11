"""Short-TTL cache for Path.stat() used in fence sorting / presence hot paths."""

from __future__ import annotations

import stat as stat_mod
import time
from pathlib import Path

_STAT_TTL_S = 2.0
_MAX_STAT_CACHE_ENTRIES = 4096
# key -> (mtime, size, present, cached_at)
_cache: dict[str, tuple[float, int, bool, float]] = {}
_cache_order: list[str] = []


def _trim_stat_cache() -> None:
    while len(_cache_order) > _MAX_STAT_CACHE_ENTRIES:
        old = _cache_order.pop(0)
        _cache.pop(old, None)


def _remember_stat_key(key: str) -> None:
    try:
        _cache_order.remove(key)
    except ValueError:
        pass
    _cache_order.append(key)
    _trim_stat_cache()


def _key(path: Path) -> str:
    try:
        return str(path).casefold()
    except OSError:
        return str(path)


def _fetch(path: Path) -> tuple[float, int, bool]:
    key = _key(path)
    now = time.monotonic()
    hit = _cache.get(key)
    if hit is not None and now - hit[3] < _STAT_TTL_S:
        return hit[0], hit[1], hit[2]
    try:
        st = path.stat()
        mtime = float(st.st_mtime)
        is_reg = stat_mod.S_ISREG(st.st_mode)
        is_dir = stat_mod.S_ISDIR(st.st_mode)
        size = int(st.st_size) if is_reg else 0
        present = is_reg or is_dir
    except OSError:
        mtime = 0.0
        size = 0
        present = False
    _cache[key] = (mtime, size, present, now)
    _remember_stat_key(key)
    return mtime, size, present


def path_mtime(path: Path) -> float:
    return _fetch(path)[0]


def path_size(path: Path) -> int:
    return _fetch(path)[1]


def path_present(path: Path) -> bool:
    """True when path exists as a regular file or directory (one cached stat).

    Prefer this over ``exists() and (is_file() or is_dir())`` on UI hot paths —
    those are up to three kernel round-trips per pin.
    """
    return _fetch(path)[2]


def invalidate_path_stat_cache(path: Path | str | None = None) -> None:
    if path is None:
        _cache.clear()
        _cache_order.clear()
        return
    try:
        key = _key(Path(path))
    except OSError:
        key = str(path).casefold()
    _cache.pop(key, None)
    try:
        _cache_order.remove(key)
    except ValueError:
        pass
