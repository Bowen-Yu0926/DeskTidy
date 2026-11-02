# -*- coding: utf-8 -*-
"""Runtime probe: DeskTidy chrome must be DefView children (Fences seat)."""
from __future__ import annotations

import ctypes
from ctypes import wintypes
import sys

user32 = ctypes.windll.user32

EnumProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
GetClassNameW = user32.GetClassNameW
IsWindowVisible = user32.IsWindowVisible
IsIconic = user32.IsIconic
GetParent = user32.GetParent
GetWindow = user32.GetWindow
GetWindowRect = user32.GetWindowRect
GetWindowLongPtrW = user32.GetWindowLongPtrW
FindWindowW = user32.FindWindowW
FindWindowExW = user32.FindWindowExW
GW_OWNER = 4
GWL_STYLE = -16
GWL_EXSTYLE = -20
WS_CHILD = 0x40000000
WS_POPUP = 0x80000000
WS_EX_LAYERED = 0x00080000


class RECT(ctypes.Structure):
    _fields_ = [
        ("l", ctypes.c_long),
        ("t", ctypes.c_long),
        ("r", ctypes.c_long),
        ("b", ctypes.c_long),
    ]


def cls(hwnd: int) -> str:
    buf = ctypes.create_unicode_buffer(256)
    GetClassNameW(hwnd, buf, 256)
    return buf.value


def find_defview() -> tuple[int, int]:
    progman = FindWindowW("Progman", None)
    if progman:
        dv = FindWindowExW(progman, None, "SHELLDLL_DefView", None)
        if dv:
            return int(progman), int(dv)
    worker = None
    for _ in range(64):
        worker = FindWindowExW(None, worker, "WorkerW", None)
        if not worker:
            break
        dv = FindWindowExW(worker, None, "SHELLDLL_DefView", None)
        if dv:
            return int(worker), int(dv)
    return 0, 0


def main() -> int:
    host, defview = find_defview()
    print(f"host={host} defview={defview}")
    if not defview:
        print("FAIL: no DefView")
        return 2

    classes = {
        "DeskTidyNativeFence",
        "DeskTidyNativeDock",
        "DeskTidyNativePageBar",
        "DeskTidyNativePublic",
        "DeskTidyNativePet",
    }
    found: dict[str, list[int]] = {c: [] for c in classes}

    @EnumProc
    def enum_child(hwnd, _lp):
        c = cls(hwnd)
        if c in classes:
            found[c].append(int(hwnd))
        return True

    user32.EnumChildWindows(defview, enum_child, 0)

    orphans: list[tuple] = []

    @EnumProc
    def enum_top(hwnd, _lp):
        c = cls(hwnd)
        if c in classes and int(GetParent(hwnd) or 0) != defview:
            if IsWindowVisible(hwnd) and not IsIconic(hwnd):
                orphans.append(
                    (
                        c,
                        int(hwnd),
                        int(GetParent(hwnd) or 0),
                        int(GetWindow(hwnd, GW_OWNER) or 0),
                    )
                )
        return True

    user32.EnumWindows(enum_top, 0)

    bugs: list[str] = []
    print("--- DefView children ---")
    for c, hs in found.items():
        print(f"{c}: {len(hs)}")
        for h in hs:
            style = int(GetWindowLongPtrW(h, GWL_STYLE))
            ex = int(GetWindowLongPtrW(h, GWL_EXSTYLE))
            parent = int(GetParent(h) or 0)
            owner = int(GetWindow(h, GW_OWNER) or 0)
            rc = RECT()
            GetWindowRect(h, ctypes.byref(rc))
            w, hh = rc.r - rc.l, rc.b - rc.t
            vis = bool(IsWindowVisible(h))
            iconic = bool(IsIconic(h))
            child = bool(style & WS_CHILD)
            popup = bool(style & WS_POPUP)
            layered = bool(ex & WS_EX_LAYERED)
            print(
                f"  hwnd={h} parent={parent} owner={owner} {w}x{hh} "
                f"vis={vis} iconic={iconic} CHILD={child} POPUP={popup} "
                f"LAYERED={layered}"
            )
            if parent != defview:
                bugs.append(f"{c} {h} parent!=defview")
            if not child:
                bugs.append(f"{c} {h} missing WS_CHILD")
            if popup:
                bugs.append(f"{c} {h} still WS_POPUP")
            if vis and not iconic and (w <= 1 or hh <= 1):
                bugs.append(f"{c} {h} degenerate size {w}x{hh}")
            if vis and not iconic and (rc.r < -10000 or rc.b < -10000):
                bugs.append(f"{c} {h} iconic coords {rc.l},{rc.t}")

    print("--- visible top-level orphans ---")
    for row in orphans:
        print(row)
        # Page-hidden fences may SoftHide but stay DefView children; visible
        # top-level means Attach failed / Peek left them detached.
        bugs.append(
            f"orphan visible {row[0]} hwnd={row[1]} parent={row[2]} owner={row[3]}"
        )

    if not found["DeskTidyNativeFence"]:
        bugs.append("no DeskTidyNativeFence under DefView")

    print("--- RESULT ---")
    if bugs:
        print("BUGS:")
        for b in bugs:
            print(" -", b)
        return 1
    print("OK: DefView-child seat looks healthy")
    return 0


if __name__ == "__main__":
    sys.exit(main())
