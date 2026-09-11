"""Run sharkdp/fd queries for DeskTidy file search (lightweight defaults)."""

from __future__ import annotations

import os
import string
import subprocess
import threading
import time
from pathlib import Path

from src.fd_locator import find_fd

DEFAULT_MAX_RESULTS = 50
DEFAULT_TIMEOUT_SEC = 1.8
DEFAULT_THREADS = 2
DEFAULT_MAX_DEPTH = 12  # project trees on D:/E: are often deeper than 6
MIN_QUERY_CHARS = 2
_ROOT_CACHE_TTL_S = 45.0

# Heavy trees — only needed when scanning home / whole drives.
_HEAVY_EXCLUDES = (
    "AppData",
    "Application Data",
    "Windows",
    "Program Files",
    "Program Files (x86)",
    "ProgramData",
    "$Recycle.Bin",
    "System Volume Information",
)
# Always skip these even on shallow Desktop/Docs/Downloads trees.
_SHALLOW_EXCLUDES = (
    "node_modules",
    ".git",
    ".svn",
    ".hg",
    "__pycache__",
    ".venv",
    "venv",
    ".tox",
    ".cache",
    ".npm",
    ".pnpm-store",
    "target",
)

_active_lock = threading.Lock()
_active_procs: set[subprocess.Popen] = set()
_roots_cache: dict[str, tuple[float, tuple[str, ...]]] = {}


def file_search_settings(settings: dict | None) -> dict:
    raw = (settings or {}).get("file_search")
    if not isinstance(raw, dict):
        raw = {}
    return raw


def search_config_slice(
    settings: dict | None,
    *,
    search_all_drives: bool | None = None,
) -> dict:
    """Tiny settings blob for workers — avoid copying the whole app settings."""
    cfg = dict(file_search_settings(settings))
    if search_all_drives is not None:
        cfg["search_all_drives"] = bool(search_all_drives)
    return {"file_search": cfg}


def invalidate_search_roots_cache() -> None:
    _roots_cache.clear()


def open_explorer_folder_paths(*, limit: int = 12) -> list[Path]:
    """Filesystem folders currently open in Explorer (not virtual namespaces)."""
    out: list[Path] = []
    seen: set[str] = set()
    try:
        from win32com.client import Dispatch

        shell_app = Dispatch("Shell.Application")
        for window in shell_app.Windows():
            try:
                raw = str(window.Document.Folder.Self.Path).strip()
            except Exception:
                continue
            if not raw or raw.startswith("::"):
                continue
            # Skip pure drive roots — too broad for the “open folder” bonus.
            if len(raw) <= 3 and raw[1:3] in {":\\", ":/"}:
                continue
            path = Path(raw)
            try:
                if not path.is_dir():
                    continue
            except OSError:
                continue
            key = _path_key(path)
            if key in seen:
                continue
            seen.add(key)
            out.append(path)
            if len(out) >= max(1, limit):
                break
    except Exception:
        pass
    return out


def resolve_search_roots(settings: dict | None) -> list[Path]:
    """Expand configured root tokens into existing directories (deduped / pruned).

    Non-global searches also include folders currently open in Explorer so a
    file visible in an Explorer window can be found without enabling 全局搜索.
    """
    cfg = file_search_settings(settings)
    all_drives = bool(cfg.get("search_all_drives", False))
    tokens = cfg.get("roots")
    if not isinstance(tokens, list) or not tokens:
        tokens = ["desktop", "documents", "downloads"]
    cache_key = f"{all_drives}|{'|'.join(str(t) for t in tokens)}"
    now = time.monotonic()
    hit = _roots_cache.get(cache_key)
    if hit is not None and now - hit[0] < _ROOT_CACHE_TTL_S:
        roots = [Path(p) for p in hit[1]]
    elif all_drives:
        roots = _fixed_drive_roots()
        _roots_cache[cache_key] = (now, tuple(str(p) for p in roots))
    else:
        roots = []
        seen: set[str] = set()
        for token in tokens:
            for path in _expand_root_token(str(token)):
                key = _path_key(path)
                if key in seen:
                    continue
                try:
                    if path.is_dir():
                        roots.append(path)
                        seen.add(key)
                except OSError:
                    continue
        roots = _prune_nested_roots(roots) or [
            _known_folder("Desktop", Path.home() / "Desktop")
        ]
        _roots_cache[cache_key] = (now, tuple(str(p) for p in roots))

    if all_drives or not bool(cfg.get("include_open_folders", True)):
        return roots

    # Fresh each call — Explorer navigation must be reflected immediately.
    seen = {_path_key(p) for p in roots}
    merged = list(roots)
    for path in open_explorer_folder_paths():
        key = _path_key(path)
        if key in seen:
            continue
        merged.append(path)
        seen.add(key)
    return _prune_nested_roots(merged) or roots


def _path_key(path: Path) -> str:
    try:
        return str(path).casefold()
    except OSError:
        return repr(path).casefold()


def _prune_nested_roots(roots: list[Path]) -> list[Path]:
    if len(roots) <= 1:
        return roots
    ranked = sorted(roots, key=lambda p: len(_path_key(p)))
    kept: list[Path] = []
    kept_keys: list[str] = []
    for path in ranked:
        key = _path_key(path)
        if any(
            key == parent
            or key.startswith(parent.rstrip("\\/") + "\\")
            or key.startswith(parent.rstrip("\\/") + "/")
            for parent in kept_keys
        ):
            continue
        kept.append(path)
        kept_keys.append(key)
    return kept


def _expand_root_token(token: str) -> list[Path]:
    t = token.strip().casefold()
    home = Path.home()
    if t in {"user_profile", "home", "~"}:
        return [home]
    if t == "desktop":
        # Prefer Explorer's real Desktop (may be D:\desktop), not ~\Desktop.
        try:
            from src.settings import get_desktop_paths

            paths = [p for p in get_desktop_paths() if p.is_dir()]
            if paths:
                return paths
        except Exception:
            pass
        return [_known_folder("Desktop", home / "Desktop")]
    if t == "documents":
        return _shell_user_folder("Personal", home / "Documents")
    if t == "downloads":
        return _shell_downloads_folder(home / "Downloads")
    if t:
        return [Path(os.path.expandvars(os.path.expanduser(token)))]
    return []


def _shell_user_folder(value_name: str, fallback: Path) -> list[Path]:
    """Resolve a User Shell Folders entry (e.g. Personal = Documents)."""
    try:
        import winreg

        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders",
        ) as key:
            raw, _ = winreg.QueryValueEx(key, value_name)
            path = Path(os.path.expandvars(str(raw)))
            if path.is_dir():
                return [path]
    except OSError:
        pass
    return [_known_folder(fallback.name, fallback)]


def _shell_downloads_folder(fallback: Path) -> list[Path]:
    # Downloads is stored under a GUID name in User Shell Folders.
    guid = "{374DE290-123F-4565-9164-39C4925E467B}"
    try:
        import winreg

        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders",
        ) as key:
            raw, _ = winreg.QueryValueEx(key, guid)
            path = Path(os.path.expandvars(str(raw)))
            if path.is_dir():
                return [path]
    except OSError:
        pass
    return [_known_folder("Downloads", fallback)]


def _known_folder(name: str, fallback: Path) -> Path:
    candidate = Path.home() / name
    try:
        if candidate.is_dir():
            return candidate
    except OSError:
        pass
    return fallback


def _fixed_drive_roots() -> list[Path]:
    """Use GetLogicalDrives bitmask — avoid touching missing/network letters."""
    roots: list[Path] = []
    try:
        import ctypes

        mask = int(ctypes.windll.kernel32.GetLogicalDrives())
        for i, letter in enumerate(string.ascii_uppercase):
            if mask & (1 << i):
                root = Path(f"{letter}:\\")
                try:
                    # Skip unreachable / empty optical drives quickly.
                    if root.exists():
                        roots.append(root)
                except OSError:
                    continue
    except Exception:
        for letter in string.ascii_uppercase:
            root = Path(f"{letter}:\\")
            try:
                if root.exists():
                    roots.append(root)
            except OSError:
                continue
    return roots or [Path.home()]


def cancel_active_searches() -> None:
    """Kill any in-flight fd processes (overlay closed / new query)."""
    with _active_lock:
        procs = list(_active_procs)
        _active_procs.clear()
    for proc in procs:
        _kill_proc(proc)


def search_filenames(
    query: str,
    *,
    settings: dict | None = None,
    max_results: int | None = None,
    timeout_sec: float | None = None,
    fd_exe: Path | None = None,
    cancel_event: threading.Event | None = None,
) -> list[Path]:
    """Blocking filename search via fd. Returns absolute paths (files + dirs)."""
    text = (query or "").strip()
    if len(text) < MIN_QUERY_CHARS:
        return []
    if cancel_event is not None and cancel_event.is_set():
        return []

    exe = fd_exe or find_fd()
    if exe is None:
        raise FileNotFoundError("fd.exe not found")

    cfg = file_search_settings(settings)
    limit = int(
        max_results if max_results is not None else cfg.get("max_results") or DEFAULT_MAX_RESULTS
    )
    limit = max(1, min(120, limit))
    timeout = float(
        timeout_sec if timeout_sec is not None else cfg.get("timeout_sec") or DEFAULT_TIMEOUT_SEC
    )
    all_drives = bool(cfg.get("search_all_drives", False))
    if all_drives:
        timeout = max(timeout, 4.0)
    timeout = max(0.4, min(8.0, timeout))
    threads = max(1, min(3, int(cfg.get("threads") or DEFAULT_THREADS)))
    roots = resolve_search_roots(settings)
    if not roots:
        return []

    # -F fixed-strings: cheaper than glob escaping, matches basename substring.
    cmd = [
        str(exe),
        "--color",
        "never",
        "--max-results",
        str(limit),
        "--threads",
        str(threads),
        "--fixed-strings",
        text,
    ]
    if bool(cfg.get("include_hidden", False)):
        cmd.append("--hidden")
        cmd.append("--no-ignore-vcs")

    excludes = cfg.get("excludes")
    if isinstance(excludes, (list, tuple)) and excludes:
        chosen = excludes
    elif all_drives or any(_path_key(r) == _path_key(Path.home()) for r in roots):
        chosen = (*_SHALLOW_EXCLUDES, *_HEAVY_EXCLUDES)
    else:
        chosen = _SHALLOW_EXCLUDES
    for name in chosen:
        n = str(name).strip()
        if n:
            cmd.extend(["--exclude", n])

    if all_drives:
        depth = int(cfg.get("max_depth") or DEFAULT_MAX_DEPTH)
        cmd.extend(["--max-depth", str(max(4, min(16, depth)))])

    cmd.extend(str(r) for r in roots)

    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    proc: subprocess.Popen[str] | None = None
    try:
        proc = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
            creationflags=creationflags,
        )
        with _active_lock:
            _active_procs.add(proc)

        if cancel_event is not None and cancel_event.is_set():
            _kill_proc(proc)
            return []

        try:
            stdout, _stderr = proc.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            _kill_proc(proc)
            stdout, _stderr = proc.communicate(timeout=0.4)
            return _parse_paths(stdout or "", limit)

        if cancel_event is not None and cancel_event.is_set():
            return []

        if proc.returncode not in (0, 1):
            raise RuntimeError(f"fd exit {proc.returncode}")
        return _parse_paths(stdout or "", limit)
    except OSError as exc:
        raise RuntimeError(f"fd failed to start: {exc}") from exc
    finally:
        if proc is not None:
            with _active_lock:
                _active_procs.discard(proc)


def _kill_proc(proc: subprocess.Popen) -> None:
    try:
        if proc.poll() is None:
            proc.kill()
    except OSError:
        pass
    try:
        proc.wait(timeout=0.2)
    except Exception:
        pass


def _parse_paths(stdout: str, limit: int) -> list[Path]:
    out: list[Path] = []
    seen: set[str] = set()
    for line in stdout.splitlines():
        line = line.strip().strip('"')
        if not line:
            continue
        key = line.casefold()
        if key in seen:
            continue
        seen.add(key)
        out.append(Path(line))
        if len(out) >= limit:
            break
    return out
