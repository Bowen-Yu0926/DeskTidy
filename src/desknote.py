"""deskNote desktop / Start Menu shortcuts and install detection."""

from __future__ import annotations

import os
import sys
from pathlib import Path

from src.notepad import notepad_enabled

DESKNOTE_NAME = "DeskNote"
# Legacy CLI still accepted by main.py for old shortcuts.
DESKNOTE_ARGS = "--notepad"
OPEN_NOTEPAD_VERB = "open-notepad"
# Older builds used deskNote.lnk (lowercase d); Windows treats it as the same path.
_LEGACY_LINK_NAMES = ("deskNote.lnk",)


def is_desknote_shortcut(path: Path | str | None) -> bool:
    """True when ``path`` is the DeskNote.lnk launcher (any Desktop location)."""
    if path is None:
        return False
    try:
        name = Path(path).name
    except (TypeError, ValueError, OSError):
        return False
    key = name.casefold()
    if key == f"{DESKNOTE_NAME}.lnk".casefold():
        return True
    return key in {n.casefold() for n in _LEGACY_LINK_NAMES}


def is_desknote_installed() -> bool:
    from src.desknote_launch import is_desknote_installed as _installed

    return _installed()


def try_open_desknote_in_running_app(path: Path | str | None = None) -> bool:
    """Open / focus standalone deskNote when ``path`` is deskNote.lnk.

    Prefer launching the deskNote process (or IPC) instead of embedding notepad
    inside DeskTidy.
    """
    if path is not None and not is_desknote_shortcut(path):
        return False
    try:
        from src.desknote_launch import open_in_desknote

        return bool(open_in_desknote())
    except Exception:
        return False


def wants_notepad_cli(argv: list[str] | None = None) -> bool:
    """True when argv requests notepad-only / open-notepad launch."""
    args = argv if argv is not None else sys.argv[1:]
    for arg in args:
        if arg in {"--notepad", "--desk-note", "--desknote"}:
            return True
        if arg.startswith("--shell-verb="):
            verb = arg.split("=", 1)[1].strip().lower()
            if verb == OPEN_NOTEPAD_VERB:
                return True
        if arg == "--shell-verb":
            continue
    for i, arg in enumerate(args):
        if arg == "--shell-verb" and i + 1 < len(args):
            if str(args[i + 1]).strip().lower() == OPEN_NOTEPAD_VERB:
                return True
    return False


def desktop_link_path() -> Path:
    """Real user Desktop (honors relocated folders like D:\\desktop)."""
    from src.settings import get_desktop_path

    return get_desktop_path() / f"{DESKNOTE_NAME}.lnk"


def _legacy_desktop_link_paths() -> list[Path]:
    """Pre-fix locations that ignored Desktop folder redirection."""
    home = Path(os.environ.get("USERPROFILE") or Path.home())
    return [
        home / "Desktop" / f"{DESKNOTE_NAME}.lnk",
        home / "桌面" / f"{DESKNOTE_NAME}.lnk",
    ]


def start_menu_link_path() -> Path:
    appdata = Path(os.environ["APPDATA"])
    from src.settings import APP_NAME

    return (
        appdata
        / "Microsoft"
        / "Windows"
        / "Start Menu"
        / "Programs"
        / APP_NAME
        / f"{DESKNOTE_NAME}.lnk"
    )


def desknote_link_paths() -> tuple[Path, Path]:
    return desktop_link_path(), start_menu_link_path()


def _stale_desktop_link_paths() -> list[Path]:
    """Legacy Desktop\\deskNote.lnk when that folder is not the real desktop."""
    try:
        real = desktop_link_path().resolve()
    except OSError:
        real = desktop_link_path()
    stale: list[Path] = []
    for path in _legacy_desktop_link_paths():
        try:
            if path.resolve() == real:
                continue
        except OSError:
            pass
        stale.append(path)
    return stale


def _shortcut_target_and_args() -> tuple[str, str, str]:
    """Return (target, arguments, workdir) for deskNote.lnk → deskNote process."""
    from src.desknote_launch import desknote_exe_path

    target_path = desknote_exe_path()
    if getattr(sys, "frozen", False):
        # Prefer sibling deskNote.exe; fall back to DeskTidy.exe --notepad.
        if target_path.is_file():
            return str(target_path), "", str(target_path.parent)
        exe = Path(sys.executable)
        return str(exe), DESKNOTE_ARGS, str(exe.parent)
    windowed = Path(sys.executable).with_name("pythonw.exe")
    launcher = windowed if windowed.is_file() else Path(sys.executable)
    return str(launcher), f'-u "{target_path}"', str(target_path.parent)


def _icon_location() -> str:
    from src.icon_utils import desknote_icon_path, icon_path

    ico = desknote_icon_path()
    if ico.is_file():
        return f"{ico},0"
    # Frozen: prefer embedding on deskNote.exe itself.
    from src.desknote_launch import desknote_exe_path

    target = desknote_exe_path()
    if getattr(sys, "frozen", False) and target.is_file():
        return f"{target},0"
    fallback = icon_path()
    if fallback.is_file():
        return f"{fallback},0"
    exe = Path(sys.executable)
    return f"{exe},0"


def create_desknote_shortcut(link_path: Path) -> None:
    """Create or refresh one deskNote shortcut (normal window, with app icon)."""
    if not is_desknote_installed():
        remove_desknote_shortcut(link_path)
        return
    target, args, workdir = _shortcut_target_and_args()
    link_path.parent.mkdir(parents=True, exist_ok=True)
    from win32com.client import Dispatch

    shell = Dispatch("WScript.Shell")
    shortcut = shell.CreateShortCut(str(link_path))
    shortcut.Targetpath = target
    shortcut.Arguments = args
    shortcut.WorkingDirectory = workdir
    shortcut.WindowStyle = 1  # normal
    shortcut.IconLocation = _icon_location()
    shortcut.Description = "DeskNote 记事本"
    shortcut.save()
    # Windows: force Explorer-visible casing when an older deskNote.lnk existed.
    try:
        want = link_path.with_name(f"{DESKNOTE_NAME}.lnk")
        if link_path.resolve() == want.resolve() and link_path.name != want.name:
            tmp = link_path.with_name(f"{DESKNOTE_NAME}.__rename__.lnk")
            link_path.rename(tmp)
            tmp.rename(want)
            link_path = want
    except OSError:
        pass
    try:
        from src.icon_utils import invalidate_file_icon_cache

        invalidate_file_icon_cache(link_path)
    except Exception:
        pass
    try:
        from src.win_app_id import DESKNOTE_AUMID, apply_shortcut_app_user_model_id

        apply_shortcut_app_user_model_id(link_path, DESKNOTE_AUMID)
    except Exception:
        pass


def remove_desknote_shortcut(link_path: Path) -> None:
    try:
        if link_path.is_file():
            link_path.unlink()
    except OSError:
        pass


def remove_all_desknote_shortcuts() -> None:
    for path in (*desknote_link_paths(), *_stale_desktop_link_paths()):
        remove_desknote_shortcut(path)


def sync_desknote_shortcuts(settings: dict | None = None) -> None:
    """Create deskNote.lnk on Desktop + Start Menu when enabled; else delete.

    Also drops the public-desktop float for those shortcuts when disabling —
    otherwise a 45s sticky-missing ghost stays clickable-looking but dead
    (「快捷方式还在 / 没法再点选」).

    Re-enable must invalidate the loose-desktop sync TTL and force a rescan —
    otherwise the float stays missing until a VD switch (~12s cache).
    """
    from src.public_desktop import invalidate_loose_sync_cache

    if notepad_enabled(settings):
        created: list[Path] = []
        for path in desknote_link_paths():
            try:
                create_desknote_shortcut(path)
                created.append(path)
            except Exception:
                pass
        for path in _stale_desktop_link_paths():
            remove_desknote_shortcut(path)
        invalidate_loose_sync_cache()
        try:
            from src.settings import get_desktop_path
            from src.win_shell import refresh_desktop

            # Notify the desktop folder (not the .lnk path) so Explorer / our
            # loose sync see the new shortcut without waiting for a VD switch.
            refresh_desktop(get_desktop_path())
        except Exception:
            pass
        if settings is not None and bool(settings.get("hide_shell_icons")):
            try:
                from src.public_desktop import (
                    find_public_entry,
                    sync_loose_desktop_items,
                )
                from src.fence_rules import all_fence_pinned_keys, path_in_pinned_keys

                sync_loose_desktop_items(settings, force=True)
                desk = desktop_link_path()
                # Already in a fence → do not also spawn a public-desktop float
                # (looks like a second DeskNote icon on the plate).
                if (
                    desk.is_file()
                    and find_public_entry(settings, desk) is None
                    and not path_in_pinned_keys(desk, all_fence_pinned_keys(settings))
                ):
                    from src.public_desktop import add_public_item

                    try:
                        page_id = int(settings.get("current_page", 0))
                    except (TypeError, ValueError):
                        page_id = 0
                    entry = add_public_item(
                        settings,
                        desk,
                        80,
                        80,
                        page_id=page_id,
                        prefer_nearest=False,
                        shared=False,
                    )
                    entry["loose"] = True
                    entry["page"] = page_id
            except Exception:
                pass
        # Force float glyphs to drop any cached blank-document shell icon.
        try:
            from src.icon_utils import invalidate_file_icon_cache

            for path in created:
                invalidate_file_icon_cache(path)
        except Exception:
            pass
        return

    remove_all_desknote_shortcuts()
    invalidate_loose_sync_cache()
    try:
        from src.settings import get_desktop_path
        from src.win_shell import refresh_desktop

        refresh_desktop(get_desktop_path())
    except Exception:
        pass
    if not settings:
        return
    try:
        from src.public_desktop import remove_public_paths

        paths = [*desknote_link_paths(), *_stale_desktop_link_paths()]
        remove_public_paths(settings, paths)
    except Exception:
        pass
