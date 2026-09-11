"""Built-in notepad helpers."""

from __future__ import annotations

import json
from contextlib import contextmanager
from datetime import datetime
from pathlib import Path
from typing import Iterator

from src.settings import APP_DIR, ensure_app_dir, install_root

SESSION_FILE = APP_DIR / "notepad_session.json"
BACKUP_DIR_NAME = "notepad_backup"
NOTES_DIR_NAME = "笔记"
# Notepad++-style text / config / code suffixes the built-in editor can open.
NOTE_OPEN_SUFFIXES = frozenset(
    {
        # plain / notes
        ".txt",
        ".text",
        ".md",
        ".markdown",
        ".log",
        ".csv",
        ".tsv",
        # config / data
        ".json",
        ".xml",
        ".yml",
        ".yaml",
        ".toml",
        ".ini",
        ".cfg",
        ".conf",
        ".config",
        ".env",
        ".properties",
        # scripts / code
        ".py",
        ".pyw",
        ".pyi",
        ".js",
        ".mjs",
        ".cjs",
        ".ts",
        ".tsx",
        ".jsx",
        ".css",
        ".scss",
        ".less",
        ".html",
        ".htm",
        ".xhtml",
        ".vue",
        ".java",
        ".c",
        ".h",
        ".cpp",
        ".hpp",
        ".cc",
        ".cs",
        ".go",
        ".rs",
        ".rb",
        ".php",
        ".sql",
        ".sh",
        ".bash",
        ".zsh",
        ".ps1",
        ".psm1",
        ".bat",
        ".cmd",
        ".vbs",
        # project / misc text
        ".gitignore",
        ".gitattributes",
        ".editorconfig",
        ".dockerfile",
        ".makefile",
        ".cmake",
        ".gradle",
        ".r",
        ".lua",
        ".pl",
        ".swift",
        ".kt",
        ".kts",
        ".dart",
        ".jsonc",
        ".json5",
        ".svg",
        ".tex",
        ".rst",
        ".adoc",
        ".nfo",
        ".diz",
        ".reg",
        ".inf",
        ".url",
        ".eml",
        ".ics",
    }
)
# Backward-compatible alias used by older call sites / tests.
_NOTE_SUFFIXES = NOTE_OPEN_SUFFIXES
# Tests may redirect the session file so they never clobber the user's reopen list.
_session_file_override: Path | None = None
_backup_dir_override: Path | None = None


def session_file_path() -> Path:
    return _session_file_override or SESSION_FILE


@contextmanager
def temporary_session_file(path: Path) -> Iterator[Path]:
    """Redirect notepad session I/O to *path* (selftests must not touch user session)."""
    global _session_file_override
    prev = _session_file_override
    _session_file_override = Path(path)
    try:
        yield _session_file_override
    finally:
        _session_file_override = prev


@contextmanager
def temporary_backup_dir(path: Path) -> Iterator[Path]:
    """Redirect untitled backups so tests never touch the user backup folder."""
    global _backup_dir_override
    prev = _backup_dir_override
    folder = Path(path)
    folder.mkdir(parents=True, exist_ok=True)
    _backup_dir_override = folder
    try:
        yield folder
    finally:
        _backup_dir_override = prev


def notepad_settings(settings: dict) -> dict:
    raw = settings.get("notepad")
    if not isinstance(raw, dict):
        raw = {}
        settings["notepad"] = raw
    raw.setdefault("enabled", True)
    raw.setdefault("md_preview", True)
    raw.setdefault("md_outline", True)
    return raw


def notepad_enabled(settings: dict | None) -> bool:
    if not settings:
        return False
    return bool(notepad_settings(settings).get("enabled", True))


def default_notes_folder(*, ensure: bool = True) -> Path:
    """Default: <install_root>/笔记 beside the exe (or project root in dev)."""
    folder = install_root() / NOTES_DIR_NAME
    if ensure:
        folder.mkdir(parents=True, exist_ok=True)
    return folder


def resolve_notes_folder(settings: dict | None = None, *, ensure: bool = True) -> Path:
    """Return configured notes folder, or ``<install_root>/笔记``.

    Custom path comes from DeskTidy → 扩展 → deskNote「默认文件夹」.
    Empty / unset always means the install directory's ``笔记`` folder
    (beside the exe when frozen; project root when running from source).
    """
    folder: Path | None = None
    custom = ""
    if settings:
        cfg = notepad_settings(settings)
        custom = str(cfg.get("folder") or "").strip()
        if custom:
            folder = Path(custom).expanduser()
    if folder is None:
        folder = default_notes_folder(ensure=False)
    if ensure:
        folder.mkdir(parents=True, exist_ok=True)
    return folder


def resolve_openable_notes_folder(
    settings: dict | None = None,
    *,
    hint_paths: list[Path | str] | None = None,
    ensure: bool = True,
) -> Path:
    """Folder for「打开笔记文件夹」— follow open tabs, not an empty 笔记 dir.

    Untitled tabs autosave under ``notepad_backup`` (N++-style). Right-click
    used to always open the configured ``笔记`` folder, which is empty until
    the user explicitly Saves — while the window still shows those tabs.
    Prefer the parent of an open/session file when present; otherwise the
    configured notes folder.

    ``hint_paths=None`` (default) falls back to the persisted session list.
    Pass an explicit list (possibly empty) to skip the session fallback.
    """
    if hint_paths is None:
        hints: list[Path | str] = list(load_notepad_session().get("files") or [])
    else:
        hints = list(hint_paths)
    for raw in hints:
        try:
            path = Path(raw).expanduser()
            if path.is_file():
                return path.parent.resolve()
        except OSError:
            continue
    return resolve_notes_folder(settings, ensure=ensure)


def notepad_backup_dir(*, ensure: bool = True) -> Path:
    """Crash/session snapshots for untitled tabs (Notepad++ ``backup\\``)."""
    folder = _backup_dir_override or (APP_DIR / BACKUP_DIR_NAME)
    if _backup_dir_override is None:
        ensure_app_dir()
    if ensure:
        folder.mkdir(parents=True, exist_ok=True)
    return folder


def is_notepad_backup_path(path: Path | str | None) -> bool:
    """True when *path* lives in the untitled backup folder (safe to delete)."""
    if path is None:
        return False
    try:
        resolved = Path(path).expanduser().resolve()
        backup = notepad_backup_dir(ensure=False).resolve()
        return resolved.parent == backup and resolved.is_file()
    except OSError:
        return False


def next_untitled_backup_path(when: datetime | None = None) -> Path:
    """Notepad++ style: ``new 1@YYYY-MM-DD_HHMMSS`` under the backup folder."""
    when = when or datetime.now()
    stamp = when.strftime("%Y-%m-%d_%H%M%S")
    folder = notepad_backup_dir(ensure=True)
    path = folder / f"new 1@{stamp}.txt"
    n = 2
    while path.exists():
        path = folder / f"new {n}@{stamp}.txt"
        n += 1
    return path


def is_notepad_owned_note(path: Path | str | None, *, notes_dir: Path) -> bool:
    """True when this tab's file is a notepad backup or a file in the notes folder.

    Used when deciding ownership (e.g. explicit「删除」). Ordinary tab close keeps
    library notes on disk; only untitled backups are discarded on close.
    """
    if path is None:
        return False
    if is_notepad_backup_path(path):
        return True
    try:
        resolved = Path(path).expanduser().resolve()
        notes = Path(notes_dir).expanduser().resolve()
        return resolved.is_file() and resolved.parent == notes
    except OSError:
        return False


def delete_notepad_backup(path: Path | str | None) -> None:
    """``FileManager::deleteBufferBackup`` — only unlink files in the backup dir."""
    if not is_notepad_backup_path(path):
        return
    try:
        Path(path).unlink(missing_ok=True)
    except OSError:
        pass


def next_auto_save_path(folder: Path, when: datetime | None = None) -> Path:
    """Unique default filename under folder, e.g. 笔记_20260806_141500.txt."""
    when = when or datetime.now()
    base = when.strftime("笔记_%Y%m%d_%H%M%S")
    path = folder / f"{base}.txt"
    n = 1
    while path.exists():
        path = folder / f"{base}_{n:02d}.txt"
        n += 1
    return path


def is_notepad_openable_path(path: Path | str | None) -> bool:
    """True when the built-in notepad can open *path* as text (N++-style)."""
    if path is None:
        return False
    try:
        p = Path(path)
    except OSError:
        return False
    try:
        if not p.is_file():
            return False
    except OSError:
        return False
    suffix = p.suffix.casefold()
    if suffix in NOTE_OPEN_SUFFIXES:
        return True
    # Extensionless text-ish names (Dockerfile, Makefile, LICENSE, …).
    name = p.name.casefold()
    if name in {
        "dockerfile",
        "makefile",
        "gnumakefile",
        "license",
        "licence",
        "readme",
        "changelog",
        "authors",
        "copying",
        "cmakelists.txt",
    }:
        return True
    if name.startswith("dockerfile") or name.startswith("makefile"):
        return True
    return False


def notepad_open_dialog_filter() -> str:
    """QFileDialog filter: common text/code groups + all files."""
    return (
        "文本与代码 ("
        "*.txt *.md *.markdown *.log *.csv *.json *.xml *.yml *.yaml "
        "*.toml *.ini *.cfg *.py *.js *.ts *.html *.css *.bat *.ps1 *.sql "
        "*.c *.cpp *.h *.java *.go *.rs *.sh"
        ");;"
        "纯文本 (*.txt *.md *.log *.csv);;"
        "配置 (*.json *.xml *.yml *.yaml *.toml *.ini *.cfg *.env);;"
        "代码 (*.py *.js *.ts *.tsx *.jsx *.html *.css *.java *.c *.cpp "
        "*.h *.go *.rs *.cs *.php *.sql *.sh *.ps1 *.bat *.cmd);;"
        "所有文件 (*.*)"
    )


def notepad_save_dialog_filter() -> str:
    return (
        "文本文件 (*.txt);;"
        "Markdown (*.md);;"
        "日志 (*.log);;"
        "CSV (*.csv);;"
        "JSON (*.json);;"
        "所有文件 (*.*)"
    )


def document_open_shell_commands(
    path: Path | str,
    settings: dict | None,
    desk,
) -> list:
    """DeskTidy extras for file RMB:「用记事本打开」+「打开方式…」(before shell verbs)."""
    from src.shell_file_menu import ShellMenuCommand
    from src.win_shell import open_path_with_picker

    try:
        target = Path(path)
    except OSError:
        return []
    try:
        if not target.is_file():
            return []
    except OSError:
        return []

    extras: list = [
        ShellMenuCommand(
            "打开方式…",
            lambda p=target: open_path_with_picker(p),
        )
    ]
    from src.desknote_launch import is_desknote_installed

    if (
        notepad_enabled(settings)
        and is_desknote_installed()
        and is_notepad_openable_path(target)
    ):
        open_np = getattr(desk, "open_paths_in_notepad", None) if desk else None
        if callable(open_np):
            extras.insert(
                0,
                ShellMenuCommand(
                    "用记事本打开",
                    lambda p=target: open_np([p]),
                ),
            )
    return extras


def recent_note_files(folder: Path, *, limit: int = 20) -> list[Path]:
    """Newest openable notes under *folder* (reopen fallback when session paths are gone)."""
    if limit <= 0 or not folder.is_dir():
        return []
    found: list[tuple[float, Path]] = []
    try:
        entries = list(folder.iterdir())
    except OSError:
        return []
    for path in entries:
        try:
            if not is_notepad_openable_path(path):
                continue
            # Skip selftest litter and hidden files.
            name = path.name
            if name.startswith("_sess_") or name.startswith("."):
                continue
            found.append((path.stat().st_mtime, path.resolve()))
        except OSError:
            continue
    found.sort(key=lambda item: item[0], reverse=True)
    return [path for _, path in found[:limit]]


def load_notepad_session() -> dict:
    """Return {files: [str], current: int, pinned: [str]} from last close."""
    ensure_app_dir()
    path = session_file_path()
    if not path.is_file():
        return {"files": [], "current": 0, "pinned": []}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {"files": [], "current": 0, "pinned": []}
    if not isinstance(data, dict):
        return {"files": [], "current": 0, "pinned": []}
    files = data.get("files")
    if not isinstance(files, list):
        files = []
    paths = [str(p) for p in files if isinstance(p, str) and p.strip()]
    try:
        current = int(data.get("current") or 0)
    except (TypeError, ValueError):
        current = 0
    pinned_raw = data.get("pinned")
    if not isinstance(pinned_raw, list):
        pinned_raw = []
    pinned = [str(p) for p in pinned_raw if isinstance(p, str) and p.strip()]
    return {"files": paths, "current": max(0, current), "pinned": pinned}


def save_notepad_session(
    files: list[str],
    current: int,
    pinned: list[str] | None = None,
) -> None:
    """Persist open note paths, active tab index, and pinned paths."""
    ensure_app_dir()
    clean: list[str] = []
    seen: set[str] = set()
    for item in files:
        text = str(item or "").strip()
        if not text:
            continue
        key = text.casefold()
        if key in seen:
            continue
        seen.add(key)
        clean.append(text)
    pinned_clean: list[str] = []
    pinned_seen: set[str] = set()
    file_keys = {p.casefold() for p in clean}
    for item in pinned or []:
        text = str(item or "").strip()
        if not text:
            continue
        key = text.casefold()
        if key in pinned_seen or key not in file_keys:
            continue
        pinned_seen.add(key)
        pinned_clean.append(text)
    payload = {
        "files": clean,
        "current": max(0, min(int(current), max(0, len(clean) - 1))) if clean else 0,
        "pinned": pinned_clean,
    }
    session_file_path().write_text(
        json.dumps(payload, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
