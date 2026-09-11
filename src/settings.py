"""Settings management for DeskTidy."""

from __future__ import annotations

import atexit
import json
import shutil
import sys
import threading
from copy import deepcopy
from datetime import datetime
from pathlib import Path

APP_NAME = "DeskTidy"
# Older product display names — still purged from Startup / Run on migrate/uninstall.
LEGACY_APP_NAMES = ("Desktidy", "桌面整理（私人版）")
APP_DIR = Path.home() / ".desktidy"
SETTINGS_FILE = APP_DIR / "settings.json"
SETTINGS_BACKUP_DIR = APP_DIR / "backups"
_MAX_SETTINGS_BACKUPS = 30
_FULL_SETTINGS_MARKERS = ("hotkeys", "desktop_pages", "organize_rules", "theme")
# DeskNote View prefs — owned by patch_notepad_settings; full DeskTidy saves must
# not overwrite disk with a stale in-memory snapshot from another process.
NOTEPAD_UI_PREF_KEYS = frozenset(
    {
        "md_outline",
        "md_preview",
        "md_sync_scroll",
        "md_edit_mode",
        "typewriter_mode",
        "library_sidebar",
    }
)
_SAVE_DEBOUNCE_SECONDS = 0.2
_SAVE_LOCK = threading.RLock()
_PENDING_SETTINGS: dict | None = None
_PENDING_SAVE_TIMER: threading.Timer | None = None


def _resource_base() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys._MEIPASS)
    return Path(__file__).resolve().parent.parent


DEFAULT_SETTINGS = _resource_base() / "config" / "default_settings.json"


def ensure_app_dir() -> None:
    APP_DIR.mkdir(parents=True, exist_ok=True)


def install_root() -> Path:
    """Directory of the installed/portable exe, or project root in development.

    Used for default Notes / Recordings folders that live beside the app.
    """
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent.parent


def get_fence_storage_root() -> Path:
    """Hidden storage for organized files (keeps desktop free of category folders)."""
    ensure_app_dir()
    root = APP_DIR / "storage"
    root.mkdir(parents=True, exist_ok=True)
    return root


def iter_fence_storage_lnk_files(root: Path | None = None):
    """Yield ``*.lnk`` at storage root and one folder deep (no recursive walk).

    Namespace shortcuts live in ``storage/<fence>/`` or ``storage/.public_system/``.
    A full ``rglob`` plus COM parse of every leftover shortcut stalls the UI.
    """
    base = root if root is not None else get_fence_storage_root()
    try:
        if not base.is_dir():
            return
        for child in base.iterdir():
            try:
                if child.is_file() and child.suffix.casefold() == ".lnk":
                    yield child
                elif child.is_dir():
                    for lnk in child.glob("*.lnk"):
                        yield lnk
            except OSError:
                continue
    except OSError:
        return


def resolve_fence_storage_path(fence_name: str) -> Path:
    name = fence_name.strip() or "未命名"
    return get_fence_storage_root() / name


_ORIGINS_FILE = APP_DIR / "storage_origins.json"


def _storage_rel_key(path: Path) -> str | None:
    """Return storage-relative key for a path under fence storage."""
    try:
        return str(path.resolve().relative_to(get_fence_storage_root().resolve())).replace("\\", "/")
    except (OSError, ValueError):
        return None


def load_storage_origins() -> dict[str, str]:
    ensure_app_dir()
    if not _ORIGINS_FILE.exists():
        return {}
    try:
        with open(_ORIGINS_FILE, encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def save_storage_origins(origins: dict[str, str]) -> None:
    ensure_app_dir()
    with open(_ORIGINS_FILE, "w", encoding="utf-8") as f:
        json.dump(origins, f, ensure_ascii=False, indent=2)


def record_storage_origin(dest: Path, origin_dir: Path) -> None:
    """Remember which desktop folder a storage file came from (user vs Public)."""
    key = _storage_rel_key(dest)
    if not key:
        return
    try:
        origin = str(origin_dir.resolve())
    except OSError:
        origin = str(origin_dir)
    origins = load_storage_origins()
    origins[key] = origin
    save_storage_origins(origins)


def remap_storage_origin(old: Path, new: Path) -> None:
    """Keep origin mapping when a file moves between fence folders."""
    old_key = _storage_rel_key(old)
    new_key = _storage_rel_key(new)
    if not old_key or not new_key or old_key == new_key:
        return
    origins = load_storage_origins()
    origin = origins.pop(old_key, None)
    if origin:
        origins[new_key] = origin
        save_storage_origins(origins)


def get_storage_origin(path: Path) -> Path | None:
    """Return the recorded origin desktop dir without consuming the mapping."""
    key = _storage_rel_key(path)
    if not key:
        return None
    origin = load_storage_origins().get(key)
    if not origin:
        return None
    try:
        p = Path(origin)
        if p.is_dir():
            return p
    except OSError:
        pass
    return None


def pop_storage_origin(path: Path) -> Path | None:
    """Consume and return the recorded origin desktop dir for a storage file."""
    key = _storage_rel_key(path)
    if not key:
        return None
    origins = load_storage_origins()
    origin = origins.pop(key, None)
    if origin is not None:
        save_storage_origins(origins)
        try:
            p = Path(origin)
            if p.is_dir():
                return p
        except OSError:
            pass
    return None


def clear_storage_origins() -> None:
    save_storage_origins({})


def _merge_defaults(settings: dict, defaults: dict) -> dict:
    merged = deepcopy(defaults)
    for key, value in settings.items():
        if key in merged and isinstance(merged[key], dict) and isinstance(value, dict):
            merged[key] = _merge_defaults(value, merged[key])
        else:
            merged[key] = value
    return merged


def apply_first_run_guide_upgrade(user_settings: dict, merged: dict) -> bool:
    """Existing profiles skip the first-install guide. Return True if *merged* changed."""
    if "first_run_guide_done" not in user_settings:
        merged["first_run_guide_done"] = True
        return True
    return False


def apply_first_run_organize_upgrade(user_settings: dict, merged: dict) -> bool:
    """Existing profiles skip the one-time first-install organize. Return True if changed."""
    if "first_run_organize_done" not in user_settings:
        merged["first_run_organize_done"] = True
        return True
    return False


def apply_file_search_hotkey_upgrade(user_settings: dict, merged: dict) -> bool:
    """Move the old default Ctrl+Alt+F → F4; keep any custom binding."""
    hotkeys = merged.get("hotkeys")
    if not isinstance(hotkeys, dict):
        return False
    current = str(hotkeys.get("file_search") or "").strip()
    # Missing key inherits new default from defaults.json on merge.
    if not current:
        return False
    # Only rewrite the previous product default — never clobber a custom chord.
    if current.casefold() in {"ctrl+alt+f", "ctrl + alt + f"}:
        hotkeys["file_search"] = "F4"
        return True
    return False


def apply_public_desktop_default(user_settings: dict, merged: dict) -> bool:
    """First install / upgrade without the key: public area stays off until user opts in."""
    if "enable_public_desktop" in user_settings:
        return False
    if merged.get("enable_public_desktop", False):
        merged["enable_public_desktop"] = False
        return True
    return False


def load_settings() -> dict:
    ensure_app_dir()
    try:
        with open(DEFAULT_SETTINGS, encoding="utf-8") as f:
            defaults = json.load(f)
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"无法加载默认配置: {DEFAULT_SETTINGS}") from exc

    if not SETTINGS_FILE.exists():
        shutil.copy(DEFAULT_SETTINGS, SETTINGS_FILE)
        return deepcopy(defaults)

    try:
        with open(SETTINGS_FILE, encoding="utf-8") as f:
            user_settings = json.load(f)
    except json.JSONDecodeError:
        backup = SETTINGS_FILE.with_suffix(".json.bak")
        try:
            shutil.copy(SETTINGS_FILE, backup)
        except OSError:
            pass
        shutil.copy(DEFAULT_SETTINGS, SETTINGS_FILE)
        return deepcopy(defaults)

    merged = _merge_defaults(user_settings, defaults)
    changed = _strip_temp_desktop_refs(merged)
    if _sanitize_notepad_folder(merged):
        changed = True
    # Product is virtual-only (Fences-like). Coerce legacy physical/mixed.
    if merged.get("organize_mode") != "virtual":
        merged["organize_mode"] = "virtual"
        changed = True
    if _coerce_legacy_fence_view_modes(merged):
        changed = True
    # Existing profiles (upgrade) never had this key — do not pop the first-install
    # guide on people who already use the app.
    if apply_first_run_guide_upgrade(user_settings, merged):
        changed = True
    if apply_first_run_organize_upgrade(user_settings, merged):
        changed = True
    if apply_public_desktop_default(user_settings, merged):
        changed = True
    if apply_file_search_hotkey_upgrade(user_settings, merged):
        changed = True
    from src.fence_style import migrate_legacy_fence_styles

    if migrate_legacy_fence_styles(user_settings, merged):
        changed = True
    if changed:
        save_settings(merged, immediate=True)
    return merged


def _coerce_legacy_fence_view_modes(settings: dict) -> bool:
    """Drop removed ``icon_only`` view (treat as grid)."""
    from src.i18n import normalize_view_mode

    changed = False
    for fence in settings.get("fences") or []:
        if not isinstance(fence, dict):
            continue
        style = fence.get("style")
        if not isinstance(style, dict):
            continue
        raw = style.get("view_mode", "grid")
        normalized = normalize_view_mode(raw)
        if raw != normalized:
            style["view_mode"] = normalized
            changed = True
    return changed


def _strip_temp_desktop_refs(settings: dict) -> bool:
    from src.desktop_scanner import purge_temp_from_settings

    return bool(purge_temp_from_settings(settings))


def _is_ephemeral_notes_folder(folder: str) -> bool:
    """True for %TEMP% / pytest-style paths that must not become the notes root."""
    import os
    import tempfile

    raw = (folder or "").strip()
    if not raw:
        return False
    try:
        path = Path(raw).expanduser().resolve()
    except OSError:
        return True
    temps: list[Path] = []
    for env in ("TEMP", "TMP"):
        value = os.environ.get(env)
        if value:
            try:
                temps.append(Path(value).resolve())
            except OSError:
                pass
    try:
        temps.append(Path(tempfile.gettempdir()).resolve())
    except OSError:
        pass
    for tmp in temps:
        try:
            if path == tmp or path.is_relative_to(tmp):
                return True
        except (OSError, ValueError, AttributeError):
            try:
                if str(path).casefold().startswith(str(tmp).casefold() + os.sep):
                    return True
            except OSError:
                pass
    name = path.name.casefold()
    if name.startswith("tmp") or name.startswith("_selftest") or name.startswith("pytest-"):
        return True
    return False


def _sanitize_notepad_folder(settings: dict) -> bool:
    """Clear notepad.folder when it points at a temp/selftest directory."""
    raw = settings.get("notepad")
    if not isinstance(raw, dict):
        return False
    folder = str(raw.get("folder") or "").strip()
    if not folder or not _is_ephemeral_notes_folder(folder):
        return False
    raw["folder"] = ""
    return True


def _read_disk_settings() -> dict | None:
    if not SETTINGS_FILE.exists():
        return None
    try:
        with open(SETTINGS_FILE, encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else None
    except (OSError, json.JSONDecodeError, TypeError):
        return None


def looks_like_full_settings(settings: dict) -> bool:
    """True when the dict looks like a real app settings object, not a test stub."""
    if not isinstance(settings, dict):
        return False
    return all(key in settings for key in _FULL_SETTINGS_MARKERS)


def _restore_notepad_ui_prefs_from_disk(to_write: dict, disk: dict) -> None:
    """Keep DeskNote UI prefs from disk when a full in-memory save would stomp them.

    DeskNote and DeskTidy share ``settings.json``. DeskTidy's long-lived snapshot
    often still has ``md_outline=True`` after the user collapses panes in DeskNote.
    Intentional updates go through ``patch_notepad_settings``.
    """
    disk_np = disk.get("notepad")
    if not isinstance(disk_np, dict) or not disk_np:
        return
    mem_np = to_write.get("notepad")
    if not isinstance(mem_np, dict):
        to_write["notepad"] = deepcopy(disk_np)
        return
    merged = dict(mem_np)
    changed = False
    for key in NOTEPAD_UI_PREF_KEYS:
        if key in disk_np and merged.get(key) != disk_np[key]:
            merged[key] = disk_np[key]
            changed = True
        elif key in disk_np and key not in merged:
            merged[key] = disk_np[key]
            changed = True
    if changed or any(k in disk_np for k in NOTEPAD_UI_PREF_KEYS):
        # Always reassign so callers see disk-backed UI prefs.
        for key in NOTEPAD_UI_PREF_KEYS:
            if key in disk_np:
                merged[key] = disk_np[key]
        to_write["notepad"] = merged


def patch_notepad_settings(updates: dict, *, immediate: bool = True) -> None:
    """Merge *updates* into on-disk ``notepad`` without replacing the whole file.

    Flushes any pending full save first so DeskTidy work is not dropped, then
    writes the notepad patch (immediate by default).
    """
    if not isinstance(updates, dict) or not updates:
        return
    clean = dict(updates)
    global _PENDING_SAVE_TIMER, _PENDING_SETTINGS
    with _SAVE_LOCK:
        pending = _PENDING_SETTINGS
        timer = _PENDING_SAVE_TIMER
        _PENDING_SETTINGS = None
        _PENDING_SAVE_TIMER = None
        if timer is not None:
            timer.cancel()
        if pending is not None:
            _save_settings_now(pending)
        if immediate:
            _save_settings_now({"notepad": clean})
            return
        # Debounced path: queue a partial notepad patch.
        _PENDING_SETTINGS = {"notepad": clean}
        next_timer = threading.Timer(_SAVE_DEBOUNCE_SECONDS, _flush_pending_settings)
        next_timer.daemon = True
        _PENDING_SAVE_TIMER = next_timer
        next_timer.start()


def _rotate_settings_backup() -> None:
    """Keep rolling copies so accidental wipes can be recovered."""
    if not SETTINGS_FILE.exists():
        return
    try:
        SETTINGS_BACKUP_DIR.mkdir(parents=True, exist_ok=True)
        newest = sorted(
            SETTINGS_BACKUP_DIR.glob("settings_*.json"),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        if newest:
            age_s = datetime.now().timestamp() - newest[0].stat().st_mtime
            if age_s < 300:
                return
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        dest = SETTINGS_BACKUP_DIR / f"settings_{stamp}.json"
        shutil.copy2(SETTINGS_FILE, dest)
        backups = sorted(
            SETTINGS_BACKUP_DIR.glob("settings_*.json"),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        for old in backups[_MAX_SETTINGS_BACKUPS:]:
            try:
                old.unlink()
            except OSError:
                pass
    except OSError:
        pass


def _patch_fences_from_partial(disk_fences: list, partial_fences: list) -> list:
    """Update matching fences by id; never drop disk fences because of a stub save."""
    by_id = {
        f.get("id"): f
        for f in partial_fences
        if isinstance(f, dict) and f.get("id")
    }
    if not by_id:
        return deepcopy(disk_fences)

    patched = deepcopy(disk_fences)
    fields = (
        "virtual_items",
        "sort_by",
        "organize_kinds",
        "extensions",
        "filename_patterns",
        "x",
        "y",
        "width",
        "height",
        "collapsed",
        "visible",
        "pages",
        "page",
        "style",
        "name",
        "folder",
        "locked",
        "position_locked",
        "portal_path",
        "type",
    )
    for fence in patched:
        src = by_id.get(fence.get("id"))
        if not src:
            continue
        for field in fields:
            if field in src:
                fence[field] = deepcopy(src[field])
    return patched


def _merge_partial_settings(disk: dict, partial: dict) -> dict:
    """Merge a stub/partial settings dict into on-disk settings without wiping lists.

    Empty list/dict values in *partial* must not erase non-empty disk values
    (selftests / page-indicator salvage once wrote ``page_folders: []`` and
    cleared the user's folder shortcuts).

    Non-empty structural lists (``desktop_pages``, ``page_folders``, …) from a
    stub must also not replace disk — a pet/selftest once wrote demo page names
    (娱乐/学习) over the user's 工作/文档.
    """
    # Owned by the full app session; never replace from a thin settings stub.
    _STRUCTURAL_LIST_KEYS = frozenset(
        {
            "desktop_pages",
            "page_folders",
            "public_desktop_items",
        }
    )
    _STRUCTURAL_DICT_KEYS = frozenset(
        {
            "hotkeys",
            "organize_rules",
            "filename_rules",
            "category_fence_map",
            "dock",
        }
    )
    _NEST_MERGE_DICT_KEYS = frozenset(
        {
            "desktop_pet",
            "notepad",
            "screen_record",
            "meeting_minutes",
            "calculator",
            "desktop_todos",
            "wallpaper",
        }
    )
    to_write = deepcopy(disk)
    for key, value in partial.items():
        if key == "fences" and isinstance(value, list):
            disk_fences = disk.get("fences") if isinstance(disk.get("fences"), list) else []
            if disk_fences:
                to_write["fences"] = _patch_fences_from_partial(disk_fences, value)
            else:
                to_write["fences"] = value
            continue
        if key in _STRUCTURAL_LIST_KEYS:
            disk_list = disk.get(key)
            if isinstance(disk_list, list) and disk_list and isinstance(value, list):
                continue
        if key in _STRUCTURAL_DICT_KEYS:
            disk_dict = disk.get(key)
            if isinstance(disk_dict, dict) and disk_dict and isinstance(value, dict):
                continue
        if key in _NEST_MERGE_DICT_KEYS and isinstance(value, dict):
            disk_dict = disk.get(key)
            if isinstance(disk_dict, dict) and disk_dict:
                nested = deepcopy(disk_dict)
                incoming = dict(value)
                # Selftests once wrote notepad.folder → %TEMP%\tmp… into user settings.
                if key == "notepad":
                    folder = str(incoming.get("folder") or "").strip()
                    if folder and _is_ephemeral_notes_folder(folder):
                        incoming.pop("folder", None)
                nested.update(incoming)
                to_write[key] = nested
                continue
        if (
            isinstance(value, list)
            and not value
            and isinstance(disk.get(key), list)
            and disk.get(key)
        ):
            # Keep disk list (e.g. page_folders / public_desktop_items).
            continue
        if (
            isinstance(value, dict)
            and not value
            and isinstance(disk.get(key), dict)
            and disk.get(key)
        ):
            continue
        to_write[key] = value
    return to_write


def _save_settings_now(settings: dict) -> None:
    """Persist settings. Preserve live hosted_namespace_icons from disk when the
    in-memory dict is stale (shell helpers update disk independently).

    Also refuses to wipe a full on-disk config with a thin/partial settings dict
    (e.g. a unit/self-test that only built fences + organize_mode).

    Writes atomically (temp + replace) under ``_SAVE_LOCK`` so debounced and
    immediate saves cannot interleave and corrupt ``settings.json``.
    """
    import os
    import tempfile

    ensure_app_dir()
    disk = _read_disk_settings() or {}

    if not looks_like_full_settings(settings) and disk:
        to_write = _merge_partial_settings(disk, settings)
    else:
        to_write = dict(settings)
        # Full snapshots (DeskTidy) must not clobber DeskNote collapse/expand prefs.
        _restore_notepad_ui_prefs_from_disk(to_write, disk)

    # Merge hosted map from disk if memory looks empty but disk has entries,
    # unless the caller intentionally cleared it (session restore sets {}).
    # Prefer explicit in-memory value when it is a dict (including empty after restore).
    if "hosted_namespace_icons" not in to_write:
        hosted = disk.get("hosted_namespace_icons")
        if isinstance(hosted, dict):
            to_write["hosted_namespace_icons"] = hosted

    if to_write == disk:
        return

    _rotate_settings_backup()

    fd, tmp_name = tempfile.mkstemp(
        prefix="settings_", suffix=".tmp", dir=str(SETTINGS_FILE.parent)
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(to_write, f, ensure_ascii=False, indent=2)
        os.replace(tmp_name, SETTINGS_FILE)
    except Exception:
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise


def _flush_pending_settings() -> None:
    global _PENDING_SAVE_TIMER, _PENDING_SETTINGS
    with _SAVE_LOCK:
        settings = _PENDING_SETTINGS
        _PENDING_SETTINGS = None
        timer = _PENDING_SAVE_TIMER
        _PENDING_SAVE_TIMER = None
        if timer is not None:
            timer.cancel()
        if settings is not None:
            _save_settings_now(settings)


def flush_settings() -> None:
    _flush_pending_settings()


def save_settings(settings: dict, *, immediate: bool = False) -> None:
    """Queue or flush a settings write.

    Snapshot and disk write both run under ``_SAVE_LOCK`` so a debounced flush
    cannot interleave with ``immediate=True`` and an older deepcopy cannot
    overwrite a newer one.
    """
    global _PENDING_SAVE_TIMER, _PENDING_SETTINGS
    if immediate:
        with _SAVE_LOCK:
            timer = _PENDING_SAVE_TIMER
            if timer is not None:
                timer.cancel()
            _PENDING_SAVE_TIMER = None
            _PENDING_SETTINGS = None
            # Deepcopy under the lock so concurrent savers serialize on the
            # caller's dict as well as on the write itself.
            snapshot = deepcopy(settings)
            _save_settings_now(snapshot)
        return

    with _SAVE_LOCK:
        _PENDING_SETTINGS = deepcopy(settings)
        timer = _PENDING_SAVE_TIMER
        if timer is not None:
            timer.cancel()
        next_timer = threading.Timer(_SAVE_DEBOUNCE_SECONDS, _flush_pending_settings)
        next_timer.daemon = True
        _PENDING_SAVE_TIMER = next_timer
        next_timer.start()


atexit.register(flush_settings)

_DESKTOP_PATHS_TTL_S = 45.0
_desktop_paths_cache: tuple[float, tuple[Path, ...]] | None = None


def get_desktop_path() -> Path:
    """Return the real Windows desktop folder (supports localized names like 桌面)."""
    paths = get_desktop_paths()
    return paths[0] if paths else Path.home() / "桌面"


def get_desktop_paths() -> list[Path]:
    """User and Public desktop folders (Explorer shows both as one desktop)."""
    import os
    import time
    import winreg

    global _desktop_paths_cache
    now = time.monotonic()
    cached = _desktop_paths_cache
    if cached is not None and now - cached[0] < _DESKTOP_PATHS_TTL_S:
        return list(cached[1])

    paths: list[Path] = []

    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders",
        ) as key:
            desktop, _ = winreg.QueryValueEx(key, "Desktop")
            path = Path(os.path.expandvars(desktop)).resolve()
            if path.is_dir():
                paths.append(path)
    except OSError:
        pass

    if not paths:
        for candidate in (Path.home() / "桌面", Path.home() / "Desktop"):
            if candidate.is_dir():
                paths.append(candidate.resolve())
                break

    public = Path(os.environ.get("PUBLIC", r"C:\Users\Public")) / "Desktop"
    try:
        public = public.resolve()
    except OSError:
        public = public
    if public.is_dir() and public not in paths:
        paths.append(public)

    if not paths:
        paths.append(Path.home() / "桌面")
    _desktop_paths_cache = (now, tuple(paths))
    return list(paths)


def invalidate_desktop_paths_cache() -> None:
    global _desktop_paths_cache
    _desktop_paths_cache = None
    # fd search roots often include "desktop"; keep them in sync with relocate.
    try:
        from src.fd_search import invalidate_search_roots_cache

        invalidate_search_roots_cache()
    except Exception:
        pass
    try:
        from src.fence_rules import invalidate_exe_cover_cache

        invalidate_exe_cover_cache()
    except Exception:
        pass


def expand_path(path: str) -> Path:
    import os

    return Path(os.path.expandvars(os.path.expanduser(path))).resolve()


def _startup_dir() -> Path:
    import os

    return (
        Path(os.environ["APPDATA"])
        / "Microsoft"
        / "Windows"
        / "Start Menu"
        / "Programs"
        / "Startup"
    )


def _startup_link_path() -> Path:
    return _startup_dir() / f"{APP_NAME}.lnk"


def _desktop_guard_link_path() -> Path:
    return _startup_dir() / f"{APP_NAME}-桌面守护.lnk"


def _desktidy_shortcut_icon_location() -> str:
    """Explicit .ico / exe icon so Startup .lnk does not inherit pythonw / deskNote."""
    import sys

    from src.icon_utils import icon_path

    if getattr(sys, "frozen", False):
        return f"{Path(sys.executable)},0"
    ico = icon_path()
    if ico.is_file():
        return f"{ico},0"
    return f"{Path(sys.executable)},0"


def _desktidy_shortcut_target_and_args(*, arguments: str = "") -> tuple[str, str, str]:
    """Prefer pythonw for Startup (no console flash), matching deskNote shortcuts."""
    import sys

    if getattr(sys, "frozen", False):
        exe = Path(sys.executable)
        return str(exe), arguments, str(exe.parent)
    script = Path(__file__).resolve().parent.parent / "main.py"
    exe = Path(sys.executable)
    windowed = exe.with_name("pythonw.exe")
    launcher = windowed if windowed.is_file() else exe
    args = f'"{script}" {arguments}'.strip() if arguments else f'"{script}"'
    return str(launcher), args, str(script.parent)


def _create_startup_shortcut(link_path: Path, *, arguments: str = "") -> None:
    target, args, workdir = _desktidy_shortcut_target_and_args(arguments=arguments)

    link_path.parent.mkdir(parents=True, exist_ok=True)
    from win32com.client import Dispatch

    shell = Dispatch("WScript.Shell")
    shortcut = shell.CreateShortCut(str(link_path))
    shortcut.Targetpath = target
    shortcut.Arguments = args
    shortcut.WorkingDirectory = workdir
    shortcut.WindowStyle = 7  # minimized
    # AIGC START
    shortcut.IconLocation = _desktidy_shortcut_icon_location()
    shortcut.Description = APP_NAME
    # AIGC END
    shortcut.save()
    # AIGC START
    try:
        from src.win_app_id import DESKTIDY_AUMID, apply_shortcut_app_user_model_id

        apply_shortcut_app_user_model_id(link_path, DESKTIDY_AUMID)
    except Exception:
        pass
    # AIGC END


def remove_desktop_guard_autostart() -> None:
    """Delete legacy dual-startup「桌面守护」shortcuts (single DeskTidy.lnk only)."""
    legacy = [
        _startup_dir() / f"{legacy}-桌面守护.lnk" for legacy in LEGACY_APP_NAMES
    ]
    for link in (_desktop_guard_link_path(), *legacy):
        try:
            if link.is_file():
                link.unlink()
        except OSError:
            pass


# Back-compat name used by older call sites / tests during transition.
def ensure_desktop_guard_autostart() -> None:
    remove_desktop_guard_autostart()


def _remove_registry_autostart() -> None:
    import winreg

    names = (APP_NAME, *LEGACY_APP_NAMES)
    try:
        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Run",
            0,
            winreg.KEY_SET_VALUE,
        )
    except OSError:
        return
    try:
        for name in names:
            try:
                winreg.DeleteValue(key, name)
            except FileNotFoundError:
                pass
    finally:
        winreg.CloseKey(key)


def _remove_startup_approved() -> None:
    """Drop Explorer StartupApproved entries for legacy Run-key autostart."""
    import winreg

    names = (APP_NAME, *LEGACY_APP_NAMES)
    try:
        key = winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run",
            0,
            winreg.KEY_SET_VALUE,
        )
    except OSError:
        return
    try:
        for name in names:
            try:
                winreg.DeleteValue(key, name)
            except FileNotFoundError:
                pass
    finally:
        winreg.CloseKey(key)


def remove_all_autostart() -> None:
    """Remove Run-key + StartupApproved + Startup shortcuts (app + legacy guard)."""
    _remove_registry_autostart()
    _remove_startup_approved()
    legacy = []
    for name in LEGACY_APP_NAMES:
        legacy.append(_startup_dir() / f"{name}.lnk")
        legacy.append(_startup_dir() / f"{name}-桌面守护.lnk")
    for link in (_startup_link_path(), _desktop_guard_link_path(), *legacy):
        try:
            if link.is_file():
                link.unlink()
        except OSError:
            pass


def migrate_legacy_autostart() -> None:
    """Drop old Chinese / Desktidy Startup shortcuts; keep one DeskTidy.lnk.

    Older installs used「桌面整理（私人版）」.lnk or Desktidy.lnk plus a separate
    desktop-guard shortcut. Remove those so login does not start two copies, and
    recreate the single APP_NAME shortcut when auto_start is enabled.
    """
    startup = _startup_dir()
    legacy_links = []
    for name in LEGACY_APP_NAMES:
        legacy_links.append(startup / f"{name}.lnk")
        legacy_links.append(startup / f"{name}-桌面守护.lnk")
    had_legacy_app = any(
        (startup / f"{name}.lnk").is_file() for name in LEGACY_APP_NAMES
    )
    for link in (*legacy_links, _desktop_guard_link_path()):
        try:
            if link.is_file():
                link.unlink()
        except OSError:
            pass
    # If the user had legacy auto-start, or current settings want it, ensure new link.
    want_app = had_legacy_app or is_auto_start_enabled()
    # Also respect settings.json if loadable without circular import weight.
    try:
        if SETTINGS_FILE.is_file():
            data = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
            if isinstance(data, dict) and bool(data.get("auto_start", False)):
                want_app = True
    except (OSError, json.JSONDecodeError, TypeError):
        pass
    if want_app:
        set_auto_start(True)
    else:
        # Still purge legacy Run keys if any linger.
        _remove_registry_autostart()
        remove_desktop_guard_autostart()


def is_auto_start_enabled() -> bool:
    return _startup_link_path().exists()


def set_auto_start(enabled: bool) -> None:
    """Enable/disable the single Startup shortcut ``DeskTidy.lnk``."""
    link_path = _startup_link_path()
    _remove_registry_autostart()
    remove_desktop_guard_autostart()

    if not enabled:
        if link_path.exists():
            link_path.unlink()
        return

    _create_startup_shortcut(link_path, arguments="")
