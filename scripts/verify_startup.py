"""Smoke checks for packaged/runtime startup prerequisites."""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from src.settings import (  # noqa: E402
    DEFAULT_SETTINGS,
    _resource_base,
    expand_path,
    get_desktop_path,
    load_settings,
)


def check(name: str, ok: bool, detail: str = "") -> bool:
    status = "OK" if ok else "FAIL"
    msg = f"[{status}] {name}"
    if detail:
        msg += f" -> {detail}"
    print(msg)
    return ok


def main() -> int:
    ok = True
    desktop = get_desktop_path()
    ok &= check("desktop path exists", desktop.is_dir(), str(desktop))

    ok &= check("resource base", _resource_base().exists(), str(_resource_base()))
    ok &= check("default settings embedded", DEFAULT_SETTINGS.is_file(), str(DEFAULT_SETTINGS))

    try:
        settings = load_settings()
        ok &= check("load_settings", isinstance(settings, dict))
    except Exception as exc:
        ok &= check("load_settings", False, str(exc))

    downloads = expand_path(r"%USERPROFILE%\Downloads")
    ok &= check("path expansion", downloads.is_dir(), str(downloads))

    try:
        from watchdog.observers import Observer  # noqa: F401
        from send2trash import send2trash  # noqa: F401
        from win32com.client import Dispatch  # noqa: F401

        ok &= check("critical imports", True)
    except Exception as exc:
        ok &= check("critical imports", False, str(exc))

    if DEFAULT_SETTINGS.is_file():
        with open(DEFAULT_SETTINGS, encoding="utf-8") as f:
            data = json.load(f)
        ok &= check("default json valid", "fences" in data)

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
