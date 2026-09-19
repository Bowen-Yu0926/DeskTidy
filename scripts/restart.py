"""Kill running DeskTidy / deskNote and relaunch main.py from the venv.

Usage:  python scripts/restart.py
"""
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY = ROOT / ".venv" / "Scripts" / "pythonw.exe"
MAIN = ROOT / "main.py"


def kill_existing() -> None:
    creationflags = 0
    try:
        creationflags = subprocess.CREATE_NO_WINDOW  # type: ignore[attr-defined]
    except Exception:
        creationflags = 0
    for image in ("DeskTidy.exe", "deskNote.exe", "pythonw.exe"):
        subprocess.run(
            ["taskkill", "/F", "/IM", image, "/T"],
            capture_output=True,
            text=True,
            timeout=10,
            creationflags=creationflags,
        )
    time.sleep(1.0)


def launch() -> None:
    if not PY.exists():
        print(f"venv python not found: {PY}", file=sys.stderr)
        sys.exit(1)
    subprocess.Popen(
        [str(PY), str(MAIN)],
        cwd=str(ROOT),
        creationflags=subprocess.CREATE_NO_WINDOW,  # type: ignore[attr-defined]
    )
    print(f"DeskTidy launched (pid check via tasklist)")
    time.sleep(3)
    r = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq pythonw.exe", "/NH"],
        capture_output=True, text=True, creationflags=subprocess.CREATE_NO_WINDOW,  # type: ignore[attr-defined]
    )
    print(r.stdout.strip() or "pythonw.exe not found in tasklist")


if __name__ == "__main__":
    kill_existing()
    launch()
