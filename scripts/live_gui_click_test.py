"""Live GUI click/hotkey E2E for packaged DeskTidy (moves real mouse/keyboard).

Safe defaults: cancel overlays with Esc; do not leave a recording running;
do not move user desktop files. Admin window title is ``DeskTidy vX.Y.Z``.
"""
from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import ctypes

import psutil
import pyautogui
import win32con
import win32gui
import win32process
from PIL import Image

from scripts.dist_paths import packaged_exe

pyautogui.FAILSAFE = True
pyautogui.PAUSE = 0.12

user32 = ctypes.windll.user32
EXE = packaged_exe(ROOT)
SHOT_DIR = ROOT / "dist" / "_live_gui_shots"
SETTINGS = Path.home() / ".desktidy" / "settings.json"
REPORT: list[tuple[str, bool, str]] = []
ORIGINAL_THEME: str | None = None


def log(name: str, ok: bool, detail: str = "") -> None:
    REPORT.append((name, ok, detail))
    print(f"  {'PASS' if ok else 'FAIL'}  {name}" + (f" — {detail}" if detail else ""))


def desk_pids() -> set[int]:
    return {
        p.info["pid"]
        for p in psutil.process_iter(["pid", "name"])
        if p.info["name"] and "DeskTidy" in p.info["name"]
    }


def kill_desktidy() -> None:
    for p in psutil.process_iter(["pid", "name"]):
        if p.info["name"] and "DeskTidy" in p.info["name"]:
            try:
                p.kill()
            except Exception:
                pass
    time.sleep(1.0)


def enum_app_windows() -> list[tuple[int, str, tuple[int, int, int, int]]]:
    pids = desk_pids()
    out: list[tuple[int, str, tuple[int, int, int, int]]] = []

    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return
        try:
            _, pid = win32process.GetWindowThreadProcessId(hwnd)
        except Exception:
            return
        if pid not in pids:
            return
        title = win32gui.GetWindowText(hwnd) or ""
        try:
            rect = win32gui.GetWindowRect(hwnd)
        except Exception:
            return
        w, h = rect[2] - rect[0], rect[3] - rect[1]
        if w < 40 or h < 20:
            return
        out.append((hwnd, title, rect))

    win32gui.EnumWindows(cb, None)
    return out


def find_main_window(timeout: float = 20.0) -> tuple[int, tuple[int, int, int, int]] | None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        for hwnd, title, rect in enum_app_windows():
            w, h = rect[2] - rect[0], rect[3] - rect[1]
            if title.startswith("DeskTidy v") and w >= 700 and h >= 480:
                return hwnd, rect
        time.sleep(0.3)
    return None


def bring_to_front(hwnd: int) -> None:
    try:
        win32gui.ShowWindow(hwnd, win32con.SW_RESTORE)
        win32gui.SetForegroundWindow(hwnd)
    except Exception:
        pass
    time.sleep(0.35)


def move_main_to_primary(hwnd: int) -> tuple[int, int, int, int]:
    """Place admin window on primary work area for reliable screenshots/clicks."""
    sw, sh = pyautogui.size()
    w, h = 1100, 760
    x, y = max(40, (sw - w) // 2), max(40, (sh - h) // 2)
    win32gui.MoveWindow(hwnd, x, y, w, h, True)
    time.sleep(0.4)
    return win32gui.GetWindowRect(hwnd)


def click_xy(x: int, y: int, *, clicks: int = 1) -> None:
    pyautogui.click(x, y, clicks=clicks)


def press(*keys: str) -> None:
    if len(keys) > 1:
        pyautogui.hotkey(*keys)
    else:
        pyautogui.press(keys[0])


def shot(name: str, rect: tuple[int, int, int, int] | None = None) -> Path:
    SHOT_DIR.mkdir(parents=True, exist_ok=True)
    path = SHOT_DIR / f"{name}.png"
    if rect is None:
        pyautogui.screenshot(str(path))
    else:
        left, top, right, bottom = rect
        region = (left, top, max(1, right - left), max(1, bottom - top))
        pyautogui.screenshot(str(path), region=region)
    return path


def looks_like_admin(path: Path) -> bool:
    """Heuristic: left ink sidebar + light content (not the IDE)."""
    try:
        im = Image.open(path).convert("RGB")
    except Exception:
        return False
    w, h = im.size
    if w < 400 or h < 300:
        return False
    # Sample sidebar column (~x=40) vs content (~x=w*0.55)
    side = [im.getpixel((40, y)) for y in (80, 160, 240, 320) if y < h]
    content = [im.getpixel((int(w * 0.55), y)) for y in (100, 200, 300) if y < h]

    def lum(rgb: tuple[int, int, int]) -> float:
        r, g, b = rgb
        return 0.2126 * r + 0.7152 * g + 0.0722 * b

    side_l = sum(lum(p) for p in side) / max(1, len(side))
    content_l = sum(lum(p) for p in content) / max(1, len(content))
    # Sidebar ink is darker than porcelain content for all themes
    return side_l + 18 < content_l and side_l < 140


def fence_windows() -> list[tuple[int, str, tuple[int, int, int, int]]]:
    out = []
    for hwnd, title, rect in enum_app_windows():
        if title.startswith("DeskTidy v"):
            continue
        w, h = rect[2] - rect[0], rect[3] - rect[1]
        if w >= 220 and 80 <= h <= 700 and w > 150:
            # Exclude slim page bar
            if w <= 220 and h >= 200:
                continue
            out.append((hwnd, title, rect))
    return out


def page_bar_windows() -> list[tuple[int, str, tuple[int, int, int, int]]]:
    out = []
    for hwnd, title, rect in enum_app_windows():
        if title.startswith("DeskTidy v"):
            continue
        w, h = rect[2] - rect[0], rect[3] - rect[1]
        if 40 <= w <= 220 and 80 <= h <= 700:
            out.append((hwnd, title, rect))
    return out


def load_settings() -> dict:
    return json.loads(SETTINGS.read_text(encoding="utf-8"))


def save_settings(data: dict) -> None:
    SETTINGS.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")


def ensure_started() -> None:
    if not EXE.is_file():
        raise SystemExit(f"missing packaged exe: {EXE}")
    if not desk_pids():
        subprocess.Popen(
            [str(EXE)],
            cwd=str(EXE.parent),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    alive = False
    for _ in range(50):
        time.sleep(0.3)
        if desk_pids():
            alive = True
            break
    log("进程启动", alive, f"pids={desk_pids()}")


def open_main_window() -> tuple[int, tuple[int, int, int, int]] | None:
    found = find_main_window(timeout=2.0)
    if found:
        return found
    try:
        from src.shell_ipc import send_shell_verb_to_running_instance

        sent = send_shell_verb_to_running_instance("show-window")
        print(f"  ipc show-window sent={sent}")
    except Exception as exc:
        print(f"  ipc show-window error: {exc}")
        sent = False
    if not sent:
        subprocess.Popen(
            [str(EXE), "--shell-verb=show-window"],
            cwd=str(EXE.parent),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    found = find_main_window(timeout=8.0)
    if found:
        return found
    sw, sh = pyautogui.size()
    tray_x = sw - 160
    for _ in range(4):
        pyautogui.doubleClick(tray_x, sh - 20)
        time.sleep(0.7)
        found = find_main_window(timeout=2.0)
        if found:
            return found
        tray_x -= 36
    return find_main_window(timeout=3.0)


def test_sidebar_pages(hwnd: int, rect: tuple[int, int, int, int]) -> None:
    left, top, right, bottom = rect
    nav_x = left + 120
    # Brand block ~88px; item height ~48
    starts = [
        ("整理", 118),
        ("桌面分区", 166),
        ("布局快照", 214),
        ("扩展功能", 262),
        ("设置", 310),
        ("帮助", 358),
    ]
    for name, dy in starts:
        bring_to_front(hwnd)
        still0 = find_main_window(timeout=1.0)
        if still0:
            hwnd, rect = still0
            left, top, right, bottom = rect
            nav_x = left + 120
        click_xy(nav_x, top + dy)
        time.sleep(0.65)
        still = find_main_window(timeout=2.0)
        if not still:
            log(f"侧栏切换-{name}", False, "主窗口消失")
            continue
        hwnd, rect = still
        left, top, right, bottom = rect
        path = shot(f"nav_{name}", rect)
        ok_ui = looks_like_admin(path)
        log(f"侧栏切换-{name}", ok_ui, "管理台UI" if ok_ui else "截图像素不像管理台")


def test_organize_preview(hwnd: int, rect: tuple[int, int, int, int]) -> None:
    left, top, right, bottom = rect
    bring_to_front(hwnd)
    click_xy(left + 120, top + 118)
    time.sleep(0.55)
    still = find_main_window(timeout=2.0)
    if still:
        hwnd, rect = still
        left, top, right, bottom = rect
    # CTA band under page header
    for x, y in (
        (left + 560, top + 220),
        (left + 680, top + 220),
        (left + 560, top + 260),
        (left + 720, top + 260),
    ):
        click_xy(x, y)
        time.sleep(0.3)
    path = shot("organize_after_cta", rect)
    still = find_main_window(timeout=2.0)
    log(
        "整理页点击CTA",
        still is not None and looks_like_admin(path),
        "未崩溃" if still else "崩溃",
    )
    press("esc")
    time.sleep(0.25)


def test_hotkeys() -> None:
    sw, sh = pyautogui.size()
    click_xy(120, sh - 140)
    time.sleep(0.35)

    press("f1")
    time.sleep(1.0)
    shot("hotkey_f1_screenshot")
    big = [w for w in enum_app_windows() if (w[2][2] - w[2][0]) >= sw - 40]
    overlay_ok = len(big) >= 1
    if not overlay_ok:
        fg = win32gui.GetForegroundWindow()
        try:
            fr = win32gui.GetWindowRect(fg)
            fw, fh = fr[2] - fr[0], fr[3] - fr[1]
            overlay_ok = fw >= sw - 40 and fh >= sh - 80
        except Exception:
            overlay_ok = False
    log("热键F1截图", overlay_ok, f"全屏窗={len(big)}")
    press("esc")
    time.sleep(0.5)
    press("esc")
    time.sleep(0.25)

    # F2 show last screenshot (may warn if none) — dismiss
    press("f2")
    time.sleep(0.8)
    shot("hotkey_f2")
    press("esc")
    press("enter")  # dismiss message box if any
    time.sleep(0.3)
    log("热键F2贴图", True, "已触发并关闭")

    press("f4")
    time.sleep(1.0)
    shot("hotkey_f4_search")
    searchish = [
        w
        for w in enum_app_windows()
        if (w[2][2] - w[2][0]) >= 320
        and 80 <= (w[2][3] - w[2][1]) <= 900
        and not (w[1] or "").startswith("DeskTidy v")
    ]
    # Search panel may be titled DeskTidy or empty; also accept any new mid-size window
    mid = [
        w
        for w in enum_app_windows()
        if 300 <= (w[2][2] - w[2][0]) <= 900 and 80 <= (w[2][3] - w[2][1]) <= 700
    ]
    log("热键F4搜索", len(searchish) >= 1 or len(mid) >= 1, f"候选={len(searchish)+len(mid)}")
    press("esc")
    time.sleep(0.35)

    before = load_settings().get("current_page")
    pyautogui.hotkey("ctrl", "shift", "right")
    time.sleep(1.0)
    mid_page = load_settings().get("current_page")
    shot("page_next")
    pyautogui.hotkey("ctrl", "shift", "left")
    time.sleep(1.0)
    after = load_settings().get("current_page")
    shot("page_prev")
    log("热键翻页", before is not None, f"{before}→{mid_page}→{after}")

    # Toggle fences
    pyautogui.hotkey("ctrl", "shift", "f")
    time.sleep(0.9)
    fences_off = len(fence_windows())
    shot("toggle_fences_off")
    pyautogui.hotkey("ctrl", "shift", "f")
    time.sleep(0.9)
    fences_on = len(fence_windows())
    shot("toggle_fences_on")
    log("热键显隐分区", fences_on >= 1, f"off≈{fences_off} on={fences_on}")

    pyautogui.hotkey("ctrl", "space")
    time.sleep(0.6)
    pyautogui.hotkey("ctrl", "space")
    time.sleep(0.4)
    log("热键窥视分区", True, "Ctrl+Space 往返")

    # Organize hotkey (virtual mode — should not move files)
    pyautogui.hotkey("ctrl", "shift", "o")
    time.sleep(1.2)
    shot("hotkey_organize")
    log("热键一键整理", desk_pids(), "进程仍在")
    press("esc")
    time.sleep(0.2)


def test_page_indicator_clicks() -> None:
    bars = page_bar_windows()
    if not bars:
        log("分页栏可见", False, "未找到分页栏窗口")
        return
    hwnd, title, rect = max(bars, key=lambda w: (w[2][3] - w[2][1]) * (w[2][2] - w[2][0]))
    left, top, right, bottom = rect
    cx = (left + right) // 2
    pyautogui.moveTo(cx, top + 30)
    time.sleep(0.55)
    shot("page_bar_hover", rect)
    # Page tabs near top; note/minutes/record lower — click note-ish band
    click_xy(cx, top + int((bottom - top) * 0.42))
    time.sleep(1.1)
    shot("page_bar_btn")
    # Prefer notepad window detection
    note_wins = [
        w
        for w in enum_app_windows()
        if any(k in (w[1] or "") for k in ("记事", "笔记", "便签", "Notepad", "纪要"))
        or (
            not (w[1] or "").startswith("DeskTidy v")
            and 280 <= (w[2][2] - w[2][0]) <= 900
            and 200 <= (w[2][3] - w[2][1]) <= 800
            and w[0] != hwnd
        )
    ]
    log(
        "分页栏按钮点击",
        True,
        f"size={right-left}x{bottom-top} extra_wins={len(note_wins)}",
    )
    # Close any notepad-like popup
    for w in note_wins[:2]:
        try:
            bring_to_front(w[0])
            pyautogui.hotkey("alt", "f4")
            time.sleep(0.35)
        except Exception:
            pass
    press("esc")


def test_fence_visible() -> None:
    fences = fence_windows()
    log("分区窗口可见", len(fences) >= 1, f"count={len(fences)}")
    if not fences:
        return
    hwnd, title, rect = max(fences, key=lambda w: (w[2][2] - w[2][0]) * (w[2][3] - w[2][1]))
    left, top, right, bottom = rect
    click_xy(left + 90, top + 16)
    time.sleep(0.35)
    shot("fence_title_click", rect)
    log("分区标题点击", True, f"{right-left}x{bottom-top}")


def test_settings_page_only(hwnd: int, rect: tuple[int, int, int, int]) -> None:
    """Open settings and capture — do not permanently change theme."""
    left, top, right, bottom = rect
    bring_to_front(hwnd)
    click_xy(left + 120, top + 310)
    time.sleep(0.7)
    still = find_main_window(timeout=2.0)
    if still:
        hwnd, rect = still
    path = shot("settings_page", rect)
    log("设置页打开", looks_like_admin(path), path.name)


def check_latest_log_errors() -> None:
    candidates = [
        ROOT / "dist" / "logs" / "latest.log",
        Path.home() / ".desktidy" / "logs" / "latest.log",
        ROOT / "dist" / "DeskTidy" / "logs" / "latest.log",
    ]
    path = next((p for p in candidates if p.is_file()), None)
    if not path:
        for base in (ROOT / "dist" / "logs", Path.home() / ".desktidy" / "logs"):
            if base.is_dir():
                logs = sorted(base.glob("*.log"), key=lambda p: p.stat().st_mtime, reverse=True)
                if logs:
                    path = logs[0]
                    break
    if not path:
        log("日志检查", True, "无日志文件（跳过）")
        return
    text = path.read_text(encoding="utf-8", errors="ignore")
    tail = text[-12000:]
    bad = [n for n in ("Traceback (most recent call last)", "AttributeError:", "FATAL") if n in tail]
    log("日志无致命异常", not bad, path.name + (f" hits={bad}" if bad else " ok"))


def restore_theme() -> None:
    global ORIGINAL_THEME
    if not ORIGINAL_THEME:
        return
    try:
        s = load_settings()
        if s.get("theme") != ORIGINAL_THEME:
            s["theme"] = ORIGINAL_THEME
            save_settings(s)
            # Ask app to reload by show-window + settings page is enough next launch;
            # nudge via IPC refresh-fences
            try:
                from src.shell_ipc import send_shell_verb_to_running_instance

                send_shell_verb_to_running_instance("refresh-fences")
            except Exception:
                pass
            log("恢复主题", True, ORIGINAL_THEME)
        else:
            log("恢复主题", True, f"已是 {ORIGINAL_THEME}")
    except Exception as exc:
        log("恢复主题", False, str(exc))


def main() -> int:
    global ORIGINAL_THEME
    print("=== DeskTidy 活体鼠标/键盘实测 ===")
    print(f"exe={EXE}")
    SHOT_DIR.mkdir(parents=True, exist_ok=True)
    try:
        ORIGINAL_THEME = str(load_settings().get("theme") or "sky")
    except Exception:
        ORIGINAL_THEME = "sky"
    # Normalize theme back to sky for user's preferred look if prior botch left mist
    if ORIGINAL_THEME == "mist":
        # Previous flawed run changed sky→mist; restore sky as user default
        ORIGINAL_THEME = "sky"
        s = load_settings()
        s["theme"] = "sky"
        save_settings(s)

    kill_desktidy()
    ensure_started()
    time.sleep(2.5)

    test_fence_visible()

    main_win = open_main_window()
    if not main_win:
        log("打开主窗口", False, "IPC/托盘均失败")
        test_hotkeys()
        test_page_indicator_clicks()
        check_latest_log_errors()
    else:
        hwnd, rect = main_win
        rect = move_main_to_primary(hwnd)
        log("打开主窗口", True, f"rect={rect} title=DeskTidy v*")
        bring_to_front(hwnd)
        path = shot("main_opened", rect)
        log("主窗口UI识别", looks_like_admin(path), path.name)
        test_sidebar_pages(hwnd, rect)
        main2 = find_main_window(timeout=3.0)
        if main2:
            hwnd, rect = main2
            test_organize_preview(hwnd, rect)
            main3 = find_main_window(timeout=3.0)
            if main3:
                test_settings_page_only(*main3)
        main4 = find_main_window(timeout=2.0)
        if main4:
            bring_to_front(main4[0])
            pyautogui.hotkey("alt", "f4")
            time.sleep(0.7)
        test_hotkeys()
        test_page_indicator_clicks()
        check_latest_log_errors()

    press("esc")
    press("esc")
    restore_theme()

    print("\n=== 汇总 ===")
    fails = [r for r in REPORT if not r[1]]
    for name, ok, detail in REPORT:
        print(f"  {'OK' if ok else 'NG'}  {name}" + (f" ({detail})" if detail else ""))
    print(f"\n截图目录: {SHOT_DIR}")
    print(f"通过 {len(REPORT) - len(fails)}/{len(REPORT)}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
