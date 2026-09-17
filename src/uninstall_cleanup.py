"""Cleanup used by the installer when DeskTidy is uninstalled."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path


def _taskkill_desktidy() -> None:
    creationflags = 0
    try:
        creationflags = subprocess.CREATE_NO_WINDOW  # type: ignore[attr-defined]
    except Exception:
        creationflags = 0
    for image in ("DeskTidy.exe", "deskNote.exe"):
        try:
            subprocess.run(
                ["taskkill", "/F", "/IM", image, "/T"],
                capture_output=True,
                text=True,
                timeout=15,
                creationflags=creationflags,
            )
        except Exception:
            pass


def _wait_main_instance_exit(*, timeout_s: float = 20.0) -> bool:
    from src.instance_lock import is_main_instance_running

    deadline = time.monotonic() + max(1.0, timeout_s)
    while time.monotonic() < deadline:
        if not is_main_instance_running():
            return True
        time.sleep(0.4)
    return not is_main_instance_running()


def _request_silent_quit() -> bool:
    """Ask the running UI instance to quit without confirmation."""
    try:
        from src.instance_lock import is_main_instance_running
        from src.shell_ipc import send_shell_verb_to_running_instance

        if not is_main_instance_running():
            return False
        return bool(send_shell_verb_to_running_instance("quit-silent"))
    except Exception:
        return False


def _purge_shell_and_autostart() -> None:
    try:
        from src.shell_background_verbs import unregister_desktop_background_verbs

        unregister_desktop_background_verbs()
    except Exception:
        pass
    try:
        from src.desknote_open_with import (
            unregister_desknote_open_with,
            unregister_desktidy_open_with,
        )

        unregister_desknote_open_with()
        unregister_desktidy_open_with()
    except Exception:
        pass
    try:
        from src.settings import remove_all_autostart

        remove_all_autostart()
    except Exception:
        pass


def _restore_shell_desktop() -> None:
    try:
        from src.win_shell import (
            ensure_desktop_icons_visible,
            reveal_hosted_namespace_icons,
        )

        try:
            reveal_hosted_namespace_icons()
        except Exception:
            pass
        ensure_desktop_icons_visible()
    except Exception:
        pass


def wants_purge_userdata(argv: list[str] | None = None) -> bool:
    """True when uninstall should wipe local DeskTidy data folders."""
    args = argv if argv is not None else sys.argv[1:]
    return any(a in {"--purge-userdata", "--purge-user-data"} for a in args)


def _load_settings_snapshot() -> dict | None:
    from src.settings import SETTINGS_FILE

    try:
        if not SETTINGS_FILE.is_file():
            return None
        raw = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
        return raw if isinstance(raw, dict) else None
    except Exception:
        return None


def _known_user_roots() -> set[Path]:
    """Paths that must never be deleted wholesale (Desktop / home / etc.)."""
    roots: set[Path] = set()
    home = Path.home()
    roots.add(home.resolve())
    for name in (
        "Desktop",
        "Documents",
        "Downloads",
        "Pictures",
        "Videos",
        "Music",
        "OneDrive",
    ):
        try:
            roots.add((home / name).resolve())
        except OSError:
            pass
    for env_key in (
        "USERPROFILE",
        "HOMEDRIVE",
        "PUBLIC",
        "SystemRoot",
        "windir",
        "ProgramFiles",
        "ProgramFiles(x86)",
        "LOCALAPPDATA",
        "APPDATA",
    ):
        val = os.environ.get(env_key) or ""
        if not val:
            continue
        try:
            roots.add(Path(val).expanduser().resolve())
        except OSError:
            pass
    # Common Chinese Desktop / Documents localized names.
    for name in ("桌面", "文档", "下载", "图片", "视频", "音乐"):
        try:
            roots.add((home / name).resolve())
        except OSError:
            pass
    return roots


def is_safe_data_purge_target(path: Path | str | None) -> bool:
    """Refuse to wipe home/Desktop/Documents or drive roots."""
    if path is None:
        return False
    try:
        target = Path(path).expanduser().resolve()
    except OSError:
        return False
    if not target.exists():
        # Still "safe" to attempt delete of a missing path; callers may skip.
        pass
    if len(target.parts) <= 1:
        return False
    # Drive root like C:\
    if target.parent == target:
        return False
    protected = _known_user_roots()
    if target in protected:
        return False
    # Never delete the Programs folder itself — only app subfolders.
    try:
        programs = (Path(os.environ.get("LOCALAPPDATA", "")) / "Programs").resolve()
        if programs.exists() and target == programs:
            return False
    except OSError:
        pass
    return True


def _rmtree_retry(path: Path, *, attempts: int = 6) -> None:
    if not path.exists():
        return
    last_exc: Exception | None = None
    for i in range(max(1, attempts)):
        try:
            if path.is_file() or path.is_symlink():
                path.unlink(missing_ok=True)
            else:
                shutil.rmtree(path, ignore_errors=False)
            if not path.exists():
                return
        except Exception as exc:
            last_exc = exc
        time.sleep(0.25 + 0.15 * i)
        # Clear read-only bits that block rmtree on Windows.
        try:
            if path.is_dir():
                for root, _dirs, files in os.walk(path):
                    for name in files:
                        fp = Path(root) / name
                        try:
                            os.chmod(fp, 0o666)
                        except OSError:
                            pass
        except Exception:
            pass
    if path.exists():
        try:
            shutil.rmtree(path, ignore_errors=True)
        except Exception:
            if last_exc:
                pass


def _default_install_data_dirs(install: Path) -> list[Path]:
    """Install-side folders DeskTidy creates beside the exe."""
    return [
        install / "笔记",
        install / "录屏",
        install / "纪要",
        install / "logs",
        install / "dist" / "笔记",
        install / "dist" / "录屏",
        install / "dist" / "纪要",
    ]


def collect_custom_data_dirs(settings: dict | None) -> list[tuple[str, Path]]:
    """User-configured notes / recordings / meeting-minutes folders (never auto-deleted)."""
    if not isinstance(settings, dict):
        return []
    out: list[tuple[str, Path]] = []

    notepad = settings.get("notepad")
    if isinstance(notepad, dict):
        custom = str(notepad.get("folder") or "").strip()
        if custom:
            out.append(("笔记", Path(custom).expanduser()))

    rec = settings.get("screen_record")
    if isinstance(rec, dict):
        custom = str(rec.get("output_dir") or "").strip()
        if custom:
            out.append(("录屏", Path(custom).expanduser()))

    minutes = settings.get("meeting_minutes")
    if isinstance(minutes, dict):
        custom = str(minutes.get("folder") or "").strip()
        if custom:
            out.append(("会议纪要", Path(custom).expanduser()))

    return out


def _install_data_dirs_to_purge(install: Path, settings: dict | None) -> list[Path]:
    """Default install folders, skipping any path the user configured explicitly."""
    skip: set[Path] = set()
    for _label, path in collect_custom_data_dirs(settings):
        try:
            skip.add(path.resolve())
        except OSError:
            pass
    dirs: list[Path] = []
    for folder in _default_install_data_dirs(install):
        try:
            if folder.resolve() in skip:
                continue
        except OSError:
            pass
        dirs.append(folder)
    return dirs


def _notify_custom_data_dirs(custom: list[tuple[str, Path]]) -> None:
    """Tell the user to manually remove configured folders after uninstall."""
    if not custom:
        return
    lines = [
        "以下为您自定义的保存文件夹，未自动删除，如需清理请手动删除：",
        "",
    ]
    for label, path in custom:
        try:
            lines.append(f"• {label}：{path.resolve()}")
        except OSError:
            lines.append(f"• {label}：{path}")
    text = "\r\n".join(lines)
    try:
        import ctypes

        ctypes.windll.user32.MessageBoxW(0, text, "DeskTidy 卸载", 0x40)
    except Exception:
        pass


def _delete_leftover_shortcuts() -> None:
    """Remove desktop / start-menu style leftovers Inno may miss."""
    names = (
        "DeskTidy.lnk",
        "Desktidy.lnk",
        "桌面整理（私人版）.lnk",
        "DeskNote.lnk",
        "deskNote.lnk",
    )
    candidates: list[Path] = []
    home = Path.home()
    for base_name in ("Desktop", "桌面"):
        candidates.append(home / base_name)
    for env_key in ("USERPROFILE", "PUBLIC"):
        base = os.environ.get(env_key)
        if base:
            candidates.append(Path(base) / "Desktop")
            candidates.append(Path(base) / "桌面")
    # Relocated user Desktop (e.g. D:\desktop) — same source as runtime sync.
    try:
        from src.settings import get_desktop_paths

        candidates.extend(get_desktop_paths())
    except Exception:
        pass
    # Start Menu Programs\DeskTidy\deskNote.lnk
    appdata = os.environ.get("APPDATA")
    if appdata:
        candidates.append(
            Path(appdata) / "Microsoft" / "Windows" / "Start Menu" / "Programs" / "DeskTidy"
        )
    seen: set[Path] = set()
    for folder in candidates:
        try:
            resolved = folder.resolve()
        except OSError:
            continue
        if resolved in seen or not resolved.is_dir():
            continue
        seen.add(resolved)
        for name in names:
            try:
                (resolved / name).unlink(missing_ok=True)
            except OSError:
                pass


def purge_user_data(*, install_dir: Path | None = None) -> None:
    """Delete DeskTidy local data so uninstall leaves no app residue.

    Removes ``~/.desktidy`` and default install-side ``笔记`` / ``录屏`` / ``logs``.
    User-configured save folders are kept; the user is prompted to delete them manually.
    """
    from src.settings import APP_DIR, install_root

    install = Path(install_dir) if install_dir is not None else install_root()
    settings = _load_settings_snapshot()
    custom_dirs = collect_custom_data_dirs(settings)

    for folder in _install_data_dirs_to_purge(install, settings):
        try:
            if is_safe_data_purge_target(folder):
                _rmtree_retry(folder)
        except Exception:
            pass

    try:
        if is_safe_data_purge_target(APP_DIR):
            _rmtree_retry(APP_DIR)
    except Exception:
        pass

    _notify_custom_data_dirs(custom_dirs)


def run_uninstall_cleanup() -> int:
    """Stop DeskTidy, restore the desktop, and clear shell/autostart registry.

    With ``--purge-userdata``, also wipe local data folders. Safe to run from
    Inno Setup before files are deleted. Never starts the UI.
    """
    purge = wants_purge_userdata()
    try:
        delivered = _request_silent_quit()
        if delivered:
            _wait_main_instance_exit(timeout_s=20.0)
        _taskkill_desktidy()
        # Brief pause so file locks / mutex release before cleanup.
        time.sleep(0.4)
        _purge_shell_and_autostart()
        _restore_shell_desktop()
        # Shortcuts are app artifacts, not user data — always remove them
        # so DeskNote.lnk / DeskTidy.lnk do not survive a normal uninstall.
        _delete_leftover_shortcuts()
        if purge:
            purge_user_data()
    except Exception:
        # Never block uninstall on cleanup errors; still try a hard kill.
        try:
            _taskkill_desktidy()
        except Exception:
            pass
        try:
            _purge_shell_and_autostart()
        except Exception:
            pass
        try:
            _delete_leftover_shortcuts()
        except Exception:
            pass
        if purge:
            try:
                purge_user_data()
            except Exception:
                pass
    return 0
