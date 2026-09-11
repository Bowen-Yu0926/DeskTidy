"""Register DeskNote in Windows「打开方式」(ProgId + Applications).

Market pattern (MSDN):
- ProgId with FriendlyTypeName + DefaultIcon + shell\\open\\command
- Per-extension OpenWithProgids (Classes + FileExts)
- Applications\\deskNote.exe with FriendlyAppName + DefaultIcon + SupportedTypes

Source runs via pythonw + desknote_main.py (no frozen deskNote.exe). Explorer
ignores Applications keys whose EXE cannot be resolved — so we keep a small
launcher copy under ~/.desktidy/launchers/deskNote.exe (pythonw twin) and
point App Paths + open command at it, with DefaultIcon = desknote_icon.ico
so the picker shows DeskNote instead of the Python glyph.
"""

from __future__ import annotations

import shutil
import sys
import winreg
from pathlib import Path

# Keep this list short — only common note/code types in the Open With picker.
# Full openable set remains NOTE_OPEN_SUFFIXES inside deskNote itself.
_OPEN_WITH_SUFFIXES: tuple[str, ...] = (
    ".md",
    ".markdown",
    ".txt",
    ".text",
    ".log",
    ".csv",
    ".tsv",
    ".json",
    ".xml",
    ".yml",
    ".yaml",
    ".toml",
    ".ini",
    ".cfg",
    ".conf",
    ".py",
    ".pyw",
    ".js",
    ".ts",
    ".tsx",
    ".jsx",
    ".html",
    ".htm",
    ".css",
    ".vue",
    ".java",
    ".c",
    ".h",
    ".cpp",
    ".cs",
    ".go",
    ".rs",
    ".sql",
    ".bat",
    ".cmd",
    ".ps1",
    ".sh",
)

# ProgId / OpenWithProgids only for types DeskNote should own lightly.
# Registering DeskNote.Document under .txt OpenWithProgids makes Windows use
# DeskNote as the effective handler when the system ProgId (e.g. txtfilelegacy)
# has no shell\open — 「新建文本文档」then shows the DeskNote icon and fails
# with filesystem error 65535. Other types stay on Applications\SupportedTypes
# (「选择其他应用」) without becoming the association fallback.
_OPEN_WITH_PROGID_SUFFIXES: tuple[str, ...] = (
    ".md",
    ".markdown",
)

_PROGID = "DeskNote.Document"
_APP_KEY_NAME = "deskNote.exe"
_LEGACY_DESKTIDY_APP = "DeskTidy.exe"
_APPLICATIONS = r"Software\Classes\Applications"
_APP_PATHS = r"Software\Microsoft\Windows\CurrentVersion\App Paths"
_FRIENDLY_NAME = "DeskNote"

# Process-local debounce: DeskTidy startup + DeskNote launch both sync.
_LAST_SYNC_AT = 0.0
_LAST_SYNC_ENABLED: bool | None = None
_SYNC_DEBOUNCE_S = 45.0


def _pythonw_path() -> Path:
    windowed = Path(sys.executable).with_name("pythonw.exe")
    return windowed if windowed.is_file() else Path(sys.executable)


def source_open_with_launcher_path() -> Path:
    """Stable deskNote.exe shim for source Open With / App Paths."""
    from src.settings import APP_DIR

    return APP_DIR / "launchers" / _APP_KEY_NAME


def ensure_source_open_with_launcher() -> Path | None:
    """Ensure ~/.desktidy/launchers/deskNote.exe exists (copy of pythonw).

    Returns the launcher path, or None when frozen / copy failed.
    Icon embed runs only when the launcher was (re)copied — UpdateResource on
    every sync made Open With / startup hitch.
    """
    if getattr(sys, "frozen", False):
        return None
    src = _pythonw_path()
    if not src.is_file():
        return None
    dest = source_open_with_launcher_path()
    try:
        dest.parent.mkdir(parents=True, exist_ok=True)
        need_copy = True
        if dest.is_file():
            try:
                need_copy = dest.stat().st_size != src.stat().st_size
            except OSError:
                need_copy = True
        if need_copy:
            shutil.copy2(src, dest)
            _try_apply_exe_icon(dest)
        return dest
    except OSError:
        return dest if dest.is_file() else None


def _try_apply_exe_icon(exe: Path) -> None:
    """Best-effort: embed desknote_icon.ico into the launcher PE (browse dialog)."""
    try:
        from src.icon_utils import desknote_icon_path

        ico = desknote_icon_path()
        if not ico.is_file() or not exe.is_file():
            return
        _embed_ico_into_exe(exe, ico)
    except Exception:
        return


def _embed_ico_into_exe(exe: Path, ico: Path) -> None:
    """Replace RT_GROUP_ICON / RT_ICON in *exe* with *ico* via Win32 UpdateResource."""
    import win32api
    import win32con

    data = ico.read_bytes()
    if len(data) < 6:
        return
    reserved = int.from_bytes(data[0:2], "little")
    itype = int.from_bytes(data[2:4], "little")
    count = int.from_bytes(data[4:6], "little")
    if reserved != 0 or itype != 1 or count < 1:
        return

    images: list[bytes] = []
    grp = bytearray()
    grp += (0).to_bytes(2, "little")
    grp += (1).to_bytes(2, "little")
    grp += count.to_bytes(2, "little")
    offset = 6
    for idx in range(1, count + 1):
        if offset + 16 > len(data):
            return
        entry = data[offset : offset + 16]
        size = int.from_bytes(entry[8:12], "little")
        img_off = int.from_bytes(entry[12:16], "little")
        if img_off + size > len(data):
            return
        images.append(data[img_off : img_off + size])
        # GRPICONDIRENTRY = ICONDIRENTRY[0:12] + id(2)
        grp += entry[:12]
        grp += idx.to_bytes(2, "little")
        offset += 16

    handle = win32api.BeginUpdateResource(str(exe), False)
    try:
        for idx, image in enumerate(images, start=1):
            win32api.UpdateResource(handle, win32con.RT_ICON, idx, image)
        win32api.UpdateResource(handle, win32con.RT_GROUP_ICON, 1, bytes(grp))
        win32api.EndUpdateResource(handle, False)
    except Exception:
        try:
            win32api.EndUpdateResource(handle, True)
        except Exception:
            pass


def _default_icon_value() -> str | None:
    """Icon for ProgId / Applications — prefer deskNote.exe (embedded), else .ico."""
    exe = _resolved_app_exe()
    if exe.is_file() and exe.name.casefold() == _APP_KEY_NAME.casefold():
        # Explorer uses this for desktop icons when we are the default handler.
        return f"{exe},0"
    from src.icon_utils import desknote_icon_path

    ico = desknote_icon_path()
    if ico.is_file():
        return f"{ico},0"
    return None


def _release_script_launcher_userchoice() -> None:
    """Source builds: drop UserChoice that points at DeskNote.Document.

    Choosing「始终使用」with a pythonw-based launcher remaps .txt/.md desktop
    icons to the Python glyph (or a sticky icon-cache entry). Open With only
    needs OpenWithProgids — not owning the default handler.
    Frozen deskNote.exe may remain the default (proper product icon).

    Note: UserChoice is often not writable (Access Denied on OpenKey), but
    ``DeleteKey`` from the same user still works — do not go through
    ``_delete_tree`` (it opens the key for write first and bails).
    """
    if getattr(sys, "frozen", False):
        return
    for suf in _OPEN_WITH_SUFFIXES:
        path = (
            r"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts"
            rf"\{suf}\UserChoice"
        )
        progid = None
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ) as key:
                progid, _ = winreg.QueryValueEx(key, "ProgId")
        except OSError:
            continue
        if str(progid) != _PROGID:
            continue
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)
        except OSError:
            # Some builds nest a Hash-only value; try delete-tree via enum with READ.
            try:
                with winreg.OpenKey(
                    winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ
                ) as key:
                    while True:
                        try:
                            child = winreg.EnumKey(key, 0)
                        except OSError:
                            break
                        try:
                            winreg.DeleteKey(
                                winreg.HKEY_CURRENT_USER, f"{path}\\{child}"
                            )
                        except OSError:
                            break
                winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)
            except OSError:
                continue


def _scrub_pythonw_from_open_with_list() -> None:
    """Remove pythonw.exe MRU leftovers that reinforce the Python icon."""
    for suf in _OPEN_WITH_SUFFIXES:
        path = (
            r"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts"
            rf"\{suf}\OpenWithList"
        )
        try:
            with winreg.OpenKey(
                winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ | winreg.KEY_SET_VALUE
            ) as key:
                i = 0
                to_delete: list[str] = []
                while True:
                    try:
                        name, val, _typ = winreg.EnumValue(key, i)
                    except OSError:
                        break
                    i += 1
                    if name == "MRUList":
                        continue
                    if str(val).casefold() in {"pythonw.exe", "python.exe"}:
                        to_delete.append(name)
                for name in to_delete:
                    try:
                        winreg.DeleteValue(key, name)
                    except OSError:
                        pass
        except OSError:
            continue


def _resolved_app_exe() -> Path:
    """Executable Windows should resolve for Applications\\deskNote.exe."""
    from src.desknote_launch import desknote_exe_path

    if getattr(sys, "frozen", False):
        exe = desknote_exe_path()
        if exe.is_file():
            return exe
    launcher = ensure_source_open_with_launcher()
    if launcher is not None and launcher.is_file():
        return launcher
    return _pythonw_path()


def _open_command() -> str:
    """Command line for Explorer Open With → DeskNote process."""
    from src.desknote_launch import desknote_exe_path

    target = desknote_exe_path()
    if getattr(sys, "frozen", False) and target.is_file():
        return f'"{target}" "%1"'
    launcher = _resolved_app_exe()
    return f'"{launcher}" -u "{target}" "%1"'


def _delete_tree(root: int, subkey: str) -> None:
    try:
        with winreg.OpenKey(root, subkey, 0, winreg.KEY_READ | winreg.KEY_WRITE) as key:
            while True:
                try:
                    child = winreg.EnumKey(key, 0)
                except OSError:
                    break
                _delete_tree(root, f"{subkey}\\{child}")
        winreg.DeleteKey(root, subkey)
    except FileNotFoundError:
        return
    except OSError:
        return


def _set_reg_none(key, name: str) -> None:
    # OpenWithProgids entries are typically REG_NONE.
    winreg.SetValueEx(key, name, 0, winreg.REG_NONE, b"")


def _notify_assoc_changed() -> None:
    try:
        import ctypes

        SHCNE_ASSOCCHANGED = 0x08000000
        SHCNF_IDLIST = 0x0000
        ctypes.windll.shell32.SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, None, None)
    except Exception:
        pass


def unregister_desktidy_open_with() -> None:
    """Drop Applications\\DeskTidy.exe only (no FileExts full-tree scan)."""
    _delete_tree(winreg.HKEY_CURRENT_USER, f"{_APPLICATIONS}\\{_LEGACY_DESKTIDY_APP}")


def _register_progid(command: str) -> None:
    root = rf"Software\Classes\{_PROGID}"
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, root) as key:
        winreg.SetValueEx(key, None, 0, winreg.REG_SZ, _FRIENDLY_NAME)
        winreg.SetValueEx(key, "FriendlyTypeName", 0, winreg.REG_SZ, _FRIENDLY_NAME)
    icon = _default_icon_value()
    if icon:
        with winreg.CreateKeyEx(
            winreg.HKEY_CURRENT_USER, rf"{root}\DefaultIcon"
        ) as key:
            winreg.SetValueEx(key, None, 0, winreg.REG_SZ, icon)
    with winreg.CreateKeyEx(
        winreg.HKEY_CURRENT_USER, rf"{root}\shell\open\command"
    ) as key:
        winreg.SetValueEx(key, None, 0, winreg.REG_SZ, command)


def _register_suffix(suf: str) -> None:
    # Classic: Classes\.ext\OpenWithProgids
    with winreg.CreateKeyEx(
        winreg.HKEY_CURRENT_USER, rf"Software\Classes\{suf}\OpenWithProgids"
    ) as key:
        _set_reg_none(key, _PROGID)
    # Creating Classes\{suf} under HKCU can leave an empty (default) that
    # shadows HKLM — restore the system ProgId when we wiped it.
    _ensure_hkcu_extension_default(suf)
    # Modern Open With / “选择其他应用” uses FileExts
    with winreg.CreateKeyEx(
        winreg.HKEY_CURRENT_USER,
        rf"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\{suf}\OpenWithProgids",
    ) as key:
        _set_reg_none(key, _PROGID)
    # Legacy cascade menu (pre-XP style list still consulted by some shells).
    with winreg.CreateKeyEx(
        winreg.HKEY_CURRENT_USER, rf"Software\Classes\{suf}\OpenWithList"
    ) as key:
        winreg.SetValueEx(key, _APP_KEY_NAME, 0, winreg.REG_SZ, "")


def _hkcu_extension_default(suf: str) -> str | None:
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, rf"Software\Classes\{suf}"
        ) as key:
            val = winreg.QueryValue(key, None)
            return str(val) if val is not None else ""
    except OSError:
        return None


def _hklm_extension_default(suf: str) -> str | None:
    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, rf"Software\Classes\{suf}"
        ) as key:
            val = winreg.QueryValue(key, None)
            text = str(val or "").strip()
            return text or None
    except OSError:
        return None


def _ensure_hkcu_extension_default(suf: str) -> None:
    """If HKCU Classes\\{suf} (default) is empty, copy HKLM so New/open keep working."""
    if suf.casefold() == ".txt":
        # .txt on Win11 often points at broken txtfilelegacy — dedicated repair.
        return
    current = _hkcu_extension_default(suf)
    if current is None:
        return
    text = str(current).strip()
    if text and text != _PROGID:
        return
    system = _hklm_extension_default(suf)
    if not system:
        return
    try:
        with winreg.CreateKeyEx(
            winreg.HKEY_CURRENT_USER, rf"Software\Classes\{suf}"
        ) as key:
            winreg.SetValueEx(key, None, 0, winreg.REG_SZ, system)
    except OSError:
        return


def _remove_progid_from_suffix(suf: str) -> None:
    """Drop DeskNote.Document / deskNote.exe from an extension's Open With lists."""
    for path in (
        rf"Software\Classes\{suf}\OpenWithProgids",
        rf"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\{suf}\OpenWithProgids",
    ):
        try:
            with winreg.OpenKey(
                winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_SET_VALUE
            ) as key:
                try:
                    winreg.DeleteValue(key, _PROGID)
                except FileNotFoundError:
                    pass
                except OSError:
                    pass
        except OSError:
            continue
    for path in (
        rf"Software\Classes\{suf}\OpenWithList",
        rf"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\{suf}\OpenWithList",
    ):
        try:
            with winreg.OpenKey(
                winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_SET_VALUE
            ) as key:
                try:
                    winreg.DeleteValue(key, _APP_KEY_NAME)
                except FileNotFoundError:
                    pass
                except OSError:
                    pass
                # FileExts OpenWithList uses MRU letters (a/b/c) → value=exe name.
                try:
                    i = 0
                    to_delete: list[str] = []
                    while True:
                        try:
                            name, val, _typ = winreg.EnumValue(key, i)
                        except OSError:
                            break
                        i += 1
                        if name == "MRUList":
                            continue
                        if str(val).casefold() == _APP_KEY_NAME.casefold():
                            to_delete.append(name)
                    for name in to_delete:
                        try:
                            winreg.DeleteValue(key, name)
                        except OSError:
                            pass
                except OSError:
                    pass
        except OSError:
            continue
    _ensure_hkcu_extension_default(suf)


def _scrub_progid_from_non_owned_suffixes() -> None:
    """Undo past OpenWithProgids on .txt/… that stole New/open from Notepad."""
    owned = {s.casefold() for s in _OPEN_WITH_PROGID_SUFFIXES}
    for suf in _OPEN_WITH_SUFFIXES:
        if suf.casefold() in owned:
            continue
        _remove_progid_from_suffix(suf)


def _progid_has_open_command(progid: str) -> bool:
    if not progid:
        return False
    for root, path in (
        (winreg.HKEY_CURRENT_USER, rf"Software\Classes\{progid}\shell\open\command"),
        (winreg.HKEY_LOCAL_MACHINE, rf"Software\Classes\{progid}\shell\open\command"),
        (winreg.HKEY_CLASSES_ROOT, rf"{progid}\shell\open\command"),
    ):
        try:
            with winreg.OpenKey(root, path) as key:
                val = winreg.QueryValue(key, None)
                if str(val or "").strip():
                    return True
        except OSError:
            continue
    return False


def _windows_notepad_txt_progid() -> str | None:
    """Prefer Store Notepad AppX ProgId listed under .txt OpenWithProgids."""
    for root, path in (
        (winreg.HKEY_CURRENT_USER, r"Software\Classes\.txt\OpenWithProgids"),
        (winreg.HKEY_LOCAL_MACHINE, r"Software\Classes\.txt\OpenWithProgids"),
        (winreg.HKEY_CLASSES_ROOT, r".txt\OpenWithProgids"),
    ):
        try:
            with winreg.OpenKey(root, path) as key:
                i = 0
                while True:
                    try:
                        name, _val, _typ = winreg.EnumValue(key, i)
                    except OSError:
                        break
                    i += 1
                    if str(name).startswith("AppX"):
                        return str(name)
        except OSError:
            continue
    return None


def _ensure_txt_in_postsetup_shellnew() -> None:
    """Win11 New menu also reads Explorer\\…\\PostSetup\\ShellNew Classes."""
    path = (
        r"Software\Microsoft\Windows\CurrentVersion\Explorer"
        r"\Discardable\PostSetup\ShellNew"
    )
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ | winreg.KEY_SET_VALUE
        ) as key:
            try:
                classes, typ = winreg.QueryValueEx(key, "Classes")
            except OSError:
                return
            if typ != winreg.REG_MULTI_SZ or not isinstance(classes, list):
                return
            if ".txt" in classes:
                return
            updated = [c for c in classes if str(c).strip()]
            updated.insert(0, ".txt")
            winreg.SetValueEx(key, "Classes", 0, winreg.REG_MULTI_SZ, updated)
    except OSError:
        return


def _ensure_txt_shellnew() -> None:
    """Repair 「新建文本文档」 after DeskNote Open With damaged .txt (error 65535).

    Win11 often leaves HKLM ``.txt`` → ``txtfilelegacy`` with no ``shell\\open``.
    Creating HKCU OpenWith keys + that broken ProgId makes Shell New fail even
    when NullFile exists. Retarget to Store Notepad AppX when needed, keep a
    clean ``ShellNew\\NullFile``, and list ``.txt`` in PostSetup Classes.
    """
    _ensure_txt_in_postsetup_shellnew()
    try:
        with winreg.CreateKeyEx(
            winreg.HKEY_CURRENT_USER, r"Software\Classes\.txt\ShellNew"
        ) as key:
            # Drop non-NullFile values that some machines leave broken.
            try:
                i = 0
                names: list[str] = []
                while True:
                    try:
                        name, _val, _typ = winreg.EnumValue(key, i)
                    except OSError:
                        break
                    i += 1
                    if name != "NullFile":
                        names.append(name)
                for name in names:
                    try:
                        winreg.DeleteValue(key, name)
                    except OSError:
                        pass
            except OSError:
                pass
            winreg.SetValueEx(key, "NullFile", 0, winreg.REG_SZ, "")
    except OSError:
        pass

    current = (_hkcu_extension_default(".txt") or "").strip()
    if not current:
        current = (_hklm_extension_default(".txt") or "").strip()
    needs_retarget = (
        not current
        or current == _PROGID
        or not _progid_has_open_command(current)
    )
    if needs_retarget:
        appx = _windows_notepad_txt_progid()
        target = appx
        if not target:
            # Classic fallback when Store Notepad ProgId is absent.
            target = "txtfile"
            try:
                with winreg.CreateKeyEx(
                    winreg.HKEY_CURRENT_USER,
                    r"Software\Classes\txtfile\shell\open\command",
                ) as key:
                    winreg.SetValueEx(
                        key,
                        None,
                        0,
                        winreg.REG_EXPAND_SZ,
                        r"%SystemRoot%\system32\NOTEPAD.EXE %1",
                    )
                with winreg.CreateKeyEx(
                    winreg.HKEY_CURRENT_USER, r"Software\Classes\txtfile\DefaultIcon"
                ) as key:
                    winreg.SetValueEx(
                        key,
                        None,
                        0,
                        winreg.REG_EXPAND_SZ,
                        r"%SystemRoot%\system32\imageres.dll,-102",
                    )
            except OSError:
                target = None
        if target:
            try:
                with winreg.CreateKeyEx(
                    winreg.HKEY_CURRENT_USER, r"Software\Classes\.txt"
                ) as key:
                    winreg.SetValueEx(key, None, 0, winreg.REG_SZ, target)
            except OSError:
                pass


def _register_app_paths(exe: Path) -> None:
    key_path = rf"{_APP_PATHS}\{_APP_KEY_NAME}"
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, key_path) as key:
        winreg.SetValueEx(key, None, 0, winreg.REG_SZ, str(exe))
        winreg.SetValueEx(key, "Path", 0, winreg.REG_SZ, str(exe.parent))


def _register_applications(command: str, exe: Path) -> None:
    """Always register Applications\\deskNote.exe when a resolvable EXE exists."""
    if not exe.is_file():
        _delete_tree(winreg.HKEY_CURRENT_USER, f"{_APPLICATIONS}\\{_APP_KEY_NAME}")
        return
    app_root = f"{_APPLICATIONS}\\{_APP_KEY_NAME}"
    with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, app_root) as key:
        winreg.SetValueEx(key, "FriendlyAppName", 0, winreg.REG_SZ, _FRIENDLY_NAME)
    icon = _default_icon_value()
    if icon:
        with winreg.CreateKeyEx(
            winreg.HKEY_CURRENT_USER, f"{app_root}\\DefaultIcon"
        ) as key:
            winreg.SetValueEx(key, None, 0, winreg.REG_SZ, icon)
    with winreg.CreateKeyEx(
        winreg.HKEY_CURRENT_USER, f"{app_root}\\shell\\open\\command"
    ) as key:
        winreg.SetValueEx(key, None, 0, winreg.REG_SZ, command)
    with winreg.CreateKeyEx(
        winreg.HKEY_CURRENT_USER, f"{app_root}\\SupportedTypes"
    ) as key:
        for suf in _OPEN_WITH_SUFFIXES:
            winreg.SetValueEx(key, suf, 0, winreg.REG_SZ, "")
    _register_app_paths(exe)


def _progid_open_command() -> str | None:
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            rf"Software\Classes\{_PROGID}\shell\open\command",
        ) as key:
            val, _typ = winreg.QueryValueEx(key, None)
            return str(val)
    except OSError:
        return None


def _desknote_open_with_up_to_date(command: str) -> bool:
    """True when ProgId open command already matches (skip heavy re-register)."""
    current = _progid_open_command()
    if not current or current.strip().casefold() != command.strip().casefold():
        return False
    # Spot-check owned markdown suffix still lists our ProgId.
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            rf"Software\Classes\.md\OpenWithProgids",
        ) as key:
            winreg.QueryValueEx(key, _PROGID)
    except OSError:
        return False
    # Past builds put DeskNote under .txt OpenWithProgids — force a scrub pass.
    try:
        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            rf"Software\Classes\.txt\OpenWithProgids",
        ) as key:
            winreg.QueryValueEx(key, _PROGID)
            return False
    except OSError:
        pass
    # Broken txtfilelegacy / missing PostSetup .txt → still run txt repair.
    try:
        default = (_hkcu_extension_default(".txt") or _hklm_extension_default(".txt") or "")
        if default and not _progid_has_open_command(str(default)):
            return False
    except OSError:
        return False
    return True


def register_desknote_open_with(*, force: bool = False) -> None:
    """Ensure DeskNote appears under Explorer「打开方式」for common suffixes."""
    unregister_desktidy_open_with()
    exe = _resolved_app_exe()
    command = _open_command()
    if not force and _desknote_open_with_up_to_date(command):
        _ensure_txt_shellnew()
        return
    _register_progid(command)
    for suf in _OPEN_WITH_PROGID_SUFFIXES:
        try:
            _register_suffix(suf)
        except OSError:
            continue
    _scrub_progid_from_non_owned_suffixes()
    _ensure_txt_shellnew()
    _register_applications(command, exe)
    _release_script_launcher_userchoice()
    _scrub_pythonw_from_open_with_list()
    _notify_assoc_changed()


def unregister_desknote_open_with() -> None:
    """Remove DeskNote Open With registration (uninstall / disable)."""
    _delete_tree(winreg.HKEY_CURRENT_USER, rf"Software\Classes\{_PROGID}")
    _delete_tree(winreg.HKEY_CURRENT_USER, f"{_APPLICATIONS}\\{_APP_KEY_NAME}")
    _delete_tree(winreg.HKEY_CURRENT_USER, rf"{_APP_PATHS}\{_APP_KEY_NAME}")
    for suf in _OPEN_WITH_SUFFIXES:
        for path in (
            rf"Software\Classes\{suf}\OpenWithProgids",
            rf"Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\{suf}\OpenWithProgids",
        ):
            try:
                with winreg.OpenKey(
                    winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_SET_VALUE
                ) as key:
                    try:
                        winreg.DeleteValue(key, _PROGID)
                    except FileNotFoundError:
                        pass
                    except OSError:
                        pass
            except OSError:
                continue
        try:
            with winreg.OpenKey(
                winreg.HKEY_CURRENT_USER,
                rf"Software\Classes\{suf}\OpenWithList",
                0,
                winreg.KEY_SET_VALUE,
            ) as key:
                try:
                    winreg.DeleteValue(key, _APP_KEY_NAME)
                except FileNotFoundError:
                    pass
                except OSError:
                    pass
        except OSError:
            pass
    _notify_assoc_changed()


def sync_desknote_open_with(*, enabled: bool = True, force: bool = False) -> None:
    """Register or tear down Open With entries; always strip DeskTidy.exe.

    *force*: skip the short process-local debounce (tests / uninstall repair).
    """
    global _LAST_SYNC_AT, _LAST_SYNC_ENABLED
    import time

    now = time.perf_counter()
    if (
        not force
        and _LAST_SYNC_ENABLED is enabled
        and (now - _LAST_SYNC_AT) < _SYNC_DEBOUNCE_S
    ):
        return
    unregister_desktidy_open_with()
    if enabled:
        register_desknote_open_with(force=False)
    else:
        if _progid_open_command() is not None:
            unregister_desknote_open_with()
    _LAST_SYNC_AT = time.perf_counter()
    _LAST_SYNC_ENABLED = bool(enabled)
