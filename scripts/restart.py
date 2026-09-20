"""Stop DeskTidy / deskNote and relaunch main.py.

Usage:  python scripts/restart.py

Only processes whose command line is this repo's main.py / desknote_main.py
are stopped (via stop_desktidy.bat). Other pythonw processes are left alone.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "main.py"
STOP = ROOT / "scripts" / "stop_desktidy.bat"


def _pythonw() -> Path:
    exe = Path(sys.executable)
    if exe.name.lower() == "pythonw.exe" and exe.exists():
        return exe
    sibling = exe.with_name("pythonw.exe")
    if sibling.exists():
        return sibling
    venv = ROOT / ".venv" / "Scripts" / "pythonw.exe"
    if venv.exists():
        return venv
    return sibling


def kill_existing() -> None:
    creationflags = 0
    try:
        creationflags = subprocess.CREATE_NO_WINDOW  # type: ignore[attr-defined]
    except Exception:
        creationflags = 0
    subprocess.run(
        ["cmd", "/c", str(STOP)],
        cwd=str(ROOT),
        timeout=40,
        creationflags=creationflags,
    )


def launch() -> None:
    py = _pythonw()
    if not py.exists():
        print(f"pythonw not found: {py}", file=sys.stderr)
        sys.exit(1)
    creationflags = 0
    try:
        creationflags = subprocess.CREATE_NO_WINDOW  # type: ignore[attr-defined]
    except Exception:
        creationflags = 0
    subprocess.Popen(
        [str(py), "-u", str(MAIN)],
        cwd=str(ROOT),
        creationflags=creationflags,
    )
    print(f"DeskTidy launched via {py}")


if __name__ == "__main__":
    kill_existing()
    launch()
