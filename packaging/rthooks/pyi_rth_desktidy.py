# PyInstaller runtime hook — reduce _MEI cleanup failures on Windows.
#
# One-file builds extract to %TEMP%\_MEI* and the bootloader deletes that folder
# after the app exits. Antivirus-injected DLLs (and UPX-packed binaries) often
# keep a lock on the bundled VCRUNTIME140.dll, which surfaces as:
#   "Failed to remove temporary directory: ...\_MEIxxxxxx"
#
# Prefer the system VC runtime early, and clear SetDllDirectory so children do
# not inherit _MEIPASS as a DLL search path.

from __future__ import annotations


def _preload_system_vcrt() -> None:
    import ctypes
    import os
    import sys

    if sys.platform != "win32":
        return
    try:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        # Stop inheriting _MEIPASS as an extra DLL directory for child processes.
        kernel32.SetDllDirectoryW(None)
    except Exception:
        pass

    root = os.environ.get("SystemRoot") or os.environ.get("WINDIR") or r"C:\Windows"
    system32 = os.path.join(root, "System32")
    for name in ("VCRUNTIME140.dll", "VCRUNTIME140_1.dll", "MSVCP140.dll"):
        path = os.path.join(system32, name)
        try:
            if os.path.isfile(path):
                ctypes.WinDLL(path)
            else:
                ctypes.WinDLL(name)
        except OSError:
            continue


_preload_system_vcrt()


def _sanitize_qt_platform() -> None:
    """Drop headless Qt platforms leaked from selftest / CI parent shells."""
    import os

    if os.environ.get("DESKTIDY_SELFTEST") == "1":
        return
    plat = (os.environ.get("QT_QPA_PLATFORM") or "").strip().lower()
    if plat in {"offscreen", "minimal", "null"}:
        os.environ.pop("QT_QPA_PLATFORM", None)
        os.environ.pop("QT_QPA_PLATFORM_PLUGIN_PATH", None)


_sanitize_qt_platform()
