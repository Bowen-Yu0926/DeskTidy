# -*- coding: utf-8 -*-
"""QA-DT-SH-005: Progman-owned desktop seat (ULW-visible on Win11 raised desktop)."""

from __future__ import annotations

import ctypes
import time
from ctypes import wintypes
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"

user32 = ctypes.windll.user32

EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)


def _read(*parts: str) -> str:
    return (NATIVE.joinpath(*parts)).read_text(encoding="utf-8", errors="ignore")


def test_place_delegates_to_raise():
    shell = _read("desktop_shell.cpp")
    place_idx = shell.find("void PlaceOverlayInDesktopBand")
    place_body = shell[place_idx : place_idx + 500]
    assert "RaiseOverlayInDesktopBand" in place_body
    raise_idx = shell.find("void RaiseOverlayInDesktopBand")
    raise_body = shell[raise_idx : raise_idx + 2800]
    assert "HWND_BOTTOM" in raise_body
    assert "SoftSeatToOwner" in raise_body


def test_attach_uses_progman_owner_not_defview_child():
    shell = _read("desktop_shell.cpp")
    assert "SoftSeatToOwner" in shell
    assert "SetParent(hwnd, defview)" not in shell
    attach = shell[shell.find("bool AttachOverlayToDesktop") :]
    attach = attach[: attach.find("\n}\n") + 2]
    assert "SoftSeatToOwner" in attach
    assert "host.host" in attach


def test_raise_heals_without_sibling_defview_insert():
    shell = _read("desktop_shell.cpp")
    raise_idx = shell.find("void RaiseOverlayInDesktopBand")
    body = shell[raise_idx : raise_idx + 2800]
    assert "SoftSeatToOwner" in body
    assert "SetWindowPos(hwnd, host.defview" not in body
    assert "GW_HWNDPREV" not in body


def test_above_defview_is_ownership_not_sibling_walk():
    shell = _read("desktop_shell.cpp")
    fn = shell[shell.find("bool IsOverlayAboveDefView") :]
    fn = fn[: fn.find("\n}\n") + 2]
    code = "\n".join(
        ln for ln in fn.splitlines() if ln.strip() and not ln.strip().startswith("//")
    )
    assert "GW_HWNDNEXT" not in code
    assert "IsAttachedToDesktop" in code


def test_layered_child_ulw_guard_exists():
    shell = _read("desktop_shell.cpp")
    assert "UpdateOverlayLayered" in shell
    fn = shell[shell.find("bool UpdateOverlayLayered") :]
    fn = fn[: fn.find("\n}\n") + 2]
    assert "ParentOf(hwnd)" in fn


def test_attach_always_raises_when_already_seated():
    for name in (
        "fence_window.cpp",
        "public_host.cpp",
        "dock_window.cpp",
        "page_bar.cpp",
        "pet_window.cpp",
    ):
        src = _read(name)
        assert "ForceAttachOverlayToDesktop" in src, name
        attach = src[src.find("AttachToDesktopLayer()") :]
        attach = attach[:1200]
        assert "if (IsAttachedToDesktop(hwnd_)) {\n    return;" not in attach.replace(
            "\r\n", "\n"
        ), name


def test_setpage_and_watch_keepalive_wiring():
    app = _read("app.cpp")
    assert "IsOverlayAboveDefView(hwnd)" in app
    assert "PollExplorerDesktopRebuild()" in app
    assert "ScheduleDesktopBandRestore()" in app
    assert "DesktopChromeHealthy()" in app


def test_live_visible_fences_owned_by_defview_host():
    """Runtime: visible fences are Progman-owned top-level (not DefView children)."""
    GetClassNameW = user32.GetClassNameW
    IsWindowVisible = user32.IsWindowVisible
    IsIconic = user32.IsIconic
    FindWindowW = user32.FindWindowW
    FindWindowExW = user32.FindWindowExW
    GetWindow = user32.GetWindow
    GetWindowLongPtrW = user32.GetWindowLongPtrW
    GetWindowRect = user32.GetWindowRect
    GW_OWNER = 4
    GWL_STYLE = -16
    WS_CHILD = 0x40000000

    class RECT(ctypes.Structure):
        _fields_ = [
            ("l", ctypes.c_long),
            ("t", ctypes.c_long),
            ("r", ctypes.c_long),
            ("b", ctypes.c_long),
        ]

    def class_name(hwnd: int) -> str:
        buf = ctypes.create_unicode_buffer(256)
        GetClassNameW(hwnd, buf, 256)
        return buf.value

    def find_host() -> tuple[int, int]:
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

    host, defview = find_host()
    if not host or not defview:
        pytest.skip("未找到 DefView host")

    fences: list[int] = []

    @EnumWindowsProc
    def enum_proc(hwnd, _lp):
        if class_name(hwnd) != "DeskTidyNativeFence":
            return True
        if IsWindowVisible(hwnd) and not IsIconic(hwnd):
            fences.append(int(hwnd))
        return True

    for _ in range(10):
        fences.clear()
        user32.EnumWindows(enum_proc, 0)
        if fences:
            break
        time.sleep(0.4)
    if not fences:
        pytest.skip("无可见 DeskTidyNativeFence")

    bad: list[str] = []
    for h in fences:
        owner = int(GetWindow(h, GW_OWNER) or 0)
        style = int(GetWindowLongPtrW(h, GWL_STYLE))
        rc = RECT()
        GetWindowRect(h, ctypes.byref(rc))
        # GetParent on WS_POPUP returns owner — check WS_CHILD instead.
        if style & WS_CHILD:
            bad.append(f"{h} still WS_CHILD")
        if owner != host and owner != defview:
            bad.append(f"{h} owner={owner} want host={host}")
        if rc.r < -3000 or rc.l > 4000:
            bad.append(f"{h} offscreen {rc.l},{rc.t}")
    assert not bad, bad
