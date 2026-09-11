"""Quick live check: organize metrics after tray reopen (source build)."""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import psutil
import pyautogui
import win32gui
import win32process
import ctypes

user32 = ctypes.windll.user32
SHOT = ROOT / "dist" / "_live_gui_shots" / "fix_metrics_reshow.png"


def desk_pids() -> set[int]:
    return {
        p.info["pid"]
        for p in psutil.process_iter(["pid", "name", "cmdline"])
        if (p.info["name"] and "DeskTidy" in p.info["name"])
        or (
            p.info.get("cmdline")
            and any("Desktidy" in str(x) and "main.py" in str(x) for x in p.info["cmdline"])
        )
    }


def kill_all() -> None:
    for p in psutil.process_iter(["pid", "name", "cmdline"]):
        try:
            name = p.info["name"] or ""
            cmd = " ".join(p.info.get("cmdline") or [])
            if "DeskTidy" in name or ("main.py" in cmd and "Desktidy" in cmd.replace("\\", "/")):
                p.kill()
        except Exception:
            pass
    time.sleep(1.0)


def find_main() -> tuple[int, tuple[int, int, int, int]] | None:
    pids = desk_pids()
    found = None

    def cb(hwnd, _):
        nonlocal found
        if not user32.IsWindowVisible(hwnd):
            return
        try:
            _, pid = win32process.GetWindowThreadProcessId(hwnd)
        except Exception:
            return
        if pid not in pids:
            return
        title = win32gui.GetWindowText(hwnd) or ""
        if not title.startswith("DeskTidy v"):
            return
        rect = win32gui.GetWindowRect(hwnd)
        w, h = rect[2] - rect[0], rect[3] - rect[1]
        if w >= 700 and h >= 480:
            found = (hwnd, rect)

    win32gui.EnumWindows(cb, None)
    return found


def main() -> int:
    kill_all()
    py = sys.executable
    main_py = ROOT / "main.py"
    proc = subprocess.Popen(
        [py, str(main_py)],
        cwd=str(ROOT),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    for _ in range(40):
        time.sleep(0.25)
        if desk_pids():
            break
    time.sleep(2.0)
    from src.shell_ipc import send_shell_verb_to_running_instance

    assert send_shell_verb_to_running_instance("show-window"), "ipc failed"
    hwnd_rect = None
    for _ in range(30):
        time.sleep(0.3)
        hwnd_rect = find_main()
        if hwnd_rect:
            break
    if not hwnd_rect:
        print("FAIL no main window")
        return 1
    hwnd, rect = hwnd_rect
    left, top, right, bottom = rect
    win32gui.MoveWindow(hwnd, 80, 60, 1100, 760, True)
    time.sleep(0.6)
    rect = win32gui.GetWindowRect(hwnd)
    SHOT.parent.mkdir(parents=True, exist_ok=True)
    pyautogui.screenshot(
        str(SHOT),
        region=(rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]),
    )
    # Close to tray then reopen
    win32gui.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    pyautogui.hotkey("alt", "f4")
    time.sleep(0.8)
    assert send_shell_verb_to_running_instance("show-window")
    time.sleep(1.0)
    hwnd_rect = find_main()
    if not hwnd_rect:
        print("FAIL reopen")
        return 1
    hwnd, rect = hwnd_rect
    win32gui.MoveWindow(hwnd, 80, 60, 1100, 760, True)
    time.sleep(0.7)
    rect = win32gui.GetWindowRect(hwnd)
    shot2 = ROOT / "dist" / "_live_gui_shots" / "fix_metrics_reshow2.png"
    pyautogui.screenshot(
        str(shot2),
        region=(rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]),
    )
    print("SHOT", SHOT)
    print("SHOT2", shot2)
    print("pid", proc.pid)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
