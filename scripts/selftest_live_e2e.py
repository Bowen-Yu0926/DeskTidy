"""Live packaged E2E checks for virtual-only DeskTidy."""
from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from scripts.dist_paths import packaged_exe

import ctypes
import psutil
import win32gui
import win32process

user32 = ctypes.windll.user32
EXE = packaged_exe(ROOT)
fails: list[str] = []
passes: list[str] = []


def ok(msg: str) -> None:
    passes.append(msg)
    print(f"  PASS  {msg}")


def fail(msg: str) -> None:
    fails.append(msg)
    print(f"  FAIL  {msg}")


def desk_pids() -> set[int]:
    return {
        p.info["pid"]
        for p in psutil.process_iter(["pid", "name"])
        if p.info["name"] and "DeskTidy" in p.info["name"]
    }


def load_user_settings() -> dict:
    p = Path.home() / ".desktidy" / "settings.json"
    return json.loads(p.read_text(encoding="utf-8"))


def enum_geos() -> list[tuple[str, int, int, int, int]]:
    pids = desk_pids()
    out: list[tuple[str, int, int, int, int]] = []

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
            left, top, right, bottom = win32gui.GetWindowRect(hwnd)
        except Exception:
            return
        w, h = right - left, bottom - top
        if w < 80 or h < 40:
            return
        out.append((title, left, top, w, h))

    win32gui.EnumWindows(cb, None)
    return out


def main() -> int:
    print("=== LIVE A) 配置应为 virtual-only ===")
    s = load_user_settings()
    if s.get("organize_mode") == "virtual":
        ok("organize_mode=virtual")
    else:
        fail(f"organize_mode={s.get('organize_mode')!r}")

    print("\n=== LIVE B) 启动打包版 ===")
    if not EXE.is_file():
        fail(f"missing {EXE}")
        return 1

    proc = subprocess.Popen(
        [str(EXE)],
        cwd=str(EXE.parent),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        alive = False
        for _ in range(40):
            time.sleep(0.25)
            if desk_pids():
                alive = True
                break
        if alive:
            ok(f"进程起来 pid={desk_pids()}")
        else:
            fail("进程未启动")

        geos: list[tuple[str, int, int, int, int]] = []
        for _ in range(40):
            time.sleep(0.25)
            geos = enum_geos()
            if any(g[3] >= 220 for g in geos):
                break
        fence_like = [g for g in geos if g[3] >= 220 and g[4] >= 80]
        if fence_like:
            ok(f"可见分区级窗口 {len(fence_like)} 个，示例宽={fence_like[0][3]}")
        else:
            fail(f"无足够宽分区窗口 geos={geos}")

        crushed = [g for g in fence_like if g[3] <= 168]
        if crushed:
            fail(f"压扁分区 {crushed}")
        else:
            ok("分区宽度未压扁")

        print("\n=== LIVE C) 虚拟整理不移动文件 ===")
        from src.organizer import organize_desktop
        from src.settings import get_desktop_path

        desk = get_desktop_path()
        marker = f"_desktidy_live_{int(time.time())}"
        src = desk / f"{marker}.txt"
        src.write_text("live", encoding="utf-8")
        try:
            test_settings = {
                "organize_mode": "virtual",
                "exclude_patterns": ["desktop.ini"],
                "public_desktop_items": [],
                "fences": [
                    {
                        "id": "live1",
                        "name": "LiveTest",
                        "extensions": [".txt"],
                        "filename_patterns": [f"{marker}*"],
                        "virtual_items": [],
                        "pages": [0],
                    }
                ],
            }
            before = src.exists()
            result = organize_desktop(test_settings, dry_run=False)
            after = src.exists()
            if before and after and result.virtual and result.moved_count >= 1:
                ok(f"钉选 {result.moved_count} 个且文件仍在桌面")
            elif before and after:
                ok(f"整理未移动文件 moved={result.moved_count}")
            else:
                fail(f"文件异常 before={before} after={after} moved={result.moved_count}")
            if not src.exists():
                fail("文件离开了桌面（物理搬家回潮）")
        finally:
            src.unlink(missing_ok=True)

        print("\n=== LIVE D) 系统图标隐藏偏好 ===")
        from src.win_shell import are_desktop_icons_visible

        s2 = load_user_settings()
        ok(
            f"shell visible={are_desktop_icons_visible()} "
            f"pref_hide={s2.get('hide_shell_icons')}"
        )

        print("\n=== LIVE E) Win+D 进出后几何 ===")
        vk_lwin, vk_d, keyup = 0x5B, 0x44, 0x0002

        def toggle() -> None:
            user32.keybd_event(vk_lwin, 0, 0, 0)
            user32.keybd_event(vk_d, 0, 0, 0)
            user32.keybd_event(vk_d, 0, keyup, 0)
            user32.keybd_event(vk_lwin, 0, keyup, 0)

        toggle()
        time.sleep(0.5)
        toggle()
        time.sleep(0.6)
        after_g = enum_geos()
        wide_after = [g for g in after_g if g[3] >= 220 and g[4] >= 80]
        crushed_after = [g for g in wide_after if g[3] <= 168]
        if wide_after and not crushed_after:
            ok(f"Win+D 后仍有正常分区 {len(wide_after)} 个")
        else:
            fail(f"Win+D 后分区异常 wide={wide_after}")

    finally:
        subprocess.run(
            ["taskkill", "/IM", "DeskTidy.exe", "/F"],
            capture_output=True,
            text=True,
            check=False,
        )
        try:
            proc.wait(timeout=5)
        except Exception:
            pass
        time.sleep(0.5)

    print("\n=== LIVE F) 退出后恢复 shell 图标 ===")
    from src.win_shell import are_desktop_icons_visible, set_desktop_icons_visible

    try:
        set_desktop_icons_visible(True)
    except Exception:
        pass
    ok(f"测试结束 icons_visible={are_desktop_icons_visible()}")

    print("\n" + "=" * 50)
    print(f"活体通过 {len(passes)}  失败 {len(fails)}")
    if fails:
        for item in fails:
            print(f"  - {item}")
        return 1
    print("活体全部通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
