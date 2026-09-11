"""Windows AppUserModelID — keep DeskTidy and deskNote as separate taskbar apps.

When both launch via ``pythonw.exe`` (source runs), Windows groups windows by the
host EXE unless each process sets a distinct AppUserModelID. Frozen builds use
separate EXEs but still benefit from stable IDs on shortcuts / pins.
"""

from __future__ import annotations

from pathlib import Path

# Stable IDs (Company.Product). Must differ so the taskbar does not merge them.
DESKTIDY_AUMID = "DeskTidy.Desktop"
DESKNOTE_AUMID = "DeskTidy.DeskNote"


def set_current_process_app_user_model_id(aumid: str) -> bool:
    """Call before creating top-level windows so the taskbar uses ``aumid``."""
    text = str(aumid or "").strip()
    if not text:
        return False
    try:
        import ctypes

        hr = ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID(text)
        return int(hr) == 0
    except Exception:
        return False


def get_current_process_app_user_model_id() -> str | None:
    """Return the process AUMID when one was set; otherwise None."""
    try:
        import ctypes
        from ctypes import wintypes

        shell32 = ctypes.windll.shell32
        ptr = wintypes.LPWSTR()
        hr = shell32.GetCurrentProcessExplicitAppUserModelID(ctypes.byref(ptr))
        if int(hr) != 0 or not ptr:
            return None
        try:
            return str(ptr.value or "") or None
        finally:
            ctypes.windll.ole32.CoTaskMemFree(ptr)
    except Exception:
        return None


def apply_shortcut_app_user_model_id(link_path: Path | str, aumid: str) -> bool:
    """Write ``System.AppUserModel.ID`` on a ``.lnk`` (pins keep product identity)."""
    text = str(aumid or "").strip()
    path = Path(link_path)
    if not text or not path.is_file():
        return False
    try:
        from win32com.propsys import propsys
        from win32com.shell import shellcon

        store = propsys.SHGetPropertyStoreFromParsingName(
            str(path),
            None,
            shellcon.GPS_READWRITE,
            propsys.IID_IPropertyStore,
        )
        key = propsys.PSGetPropertyKeyFromName("System.AppUserModel.ID")
        store.SetValue(key, propsys.PROPVARIANTType(text))
        store.Commit()
        return True
    except Exception:
        return False
