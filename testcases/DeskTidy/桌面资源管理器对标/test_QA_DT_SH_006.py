# -*- coding: utf-8 -*-
"""QA-DT-SH-006: page switch hard-hide leavers + Show/Present arrivals."""

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


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def _fn(src: str, sig: str, span: int = 2800) -> str:
    i = src.find(sig)
    assert i >= 0, sig
    return src[i : i + span]


def test_setpage_hides_then_show_refresh():
    app = _read("app.cpp")
    body = _fn(app, "void App::SetPage(int page)", 3200)
    assert "HideOffPageFences()" in body
    assert "fence->Show()" in body
    assert "RefreshLayer()" in body
    for line in body.splitlines():
        s = line.strip()
        if not s or s.startswith("//"):
            continue
        assert "WarmLayerOffscreen" not in s, line


def test_hide_off_page_uses_sw_hide():
    app = _read("app.cpp")
    body = _fn(app, "void App::HideOffPageFences()", 1200)
    assert "SW_HIDE" in body
    assert "ShowWindow" in body
    assert "peek_active_" in body


def test_fence_show_presents_when_buried():
    fw = _read("fence_window.cpp")
    body = _fn(fw, "void FenceWindow::Show()", 1600)
    assert "IsOverlayAboveDefView" in body
    assert "buried" in body
    assert "PresentLayered()" in body
    present_guard = body[body.find("if (!was_visible") :]
    assert "buried" in present_guard


def test_seat_all_calls_hide_off_page_first():
    app = _read("app.cpp")
    body = _fn(app, "void App::SeatAllFencesUnderApps()", 1600)
    hide_i = body.find("HideOffPageFences()")
    assert hide_i >= 0
    loop_i = body.find("for (auto& fence : fences_)")
    assert loop_i > hide_i


def _class_name(hwnd: int) -> str:
    buf = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, buf, 256)
    return buf.value


def _visible_fences() -> set[int]:
    out: set[int] = set()

    @EnumWindowsProc
    def enum_proc(hwnd, _lp):
        if _class_name(hwnd) != "DeskTidyNativeFence":
            return True
        if user32.IsWindowVisible(hwnd) and not user32.IsIconic(hwnd):
            out.add(int(hwnd))
        return True

    user32.EnumWindows(enum_proc, 0)
    return out


def test_live_page_switch_roundtrip():
    """Runtime: Ctrl+Shift+Right/Left；切回后须仍有可见分区且 owner=Progman。"""
    if not user32.FindWindowW("DeskTidyNativePageBar", None):
        pytest.skip("DeskTidy 未运行 / 无页指示")

    before = _visible_fences()
    if not before:
        pytest.skip("当前无可见分区")

    VK_CONTROL, VK_SHIFT, VK_LEFT, VK_RIGHT = 0x11, 0x10, 0x25, 0x27
    KEYEVENTF_KEYUP = 0x0002

    def chord(vk_arrow: int) -> None:
        user32.keybd_event(VK_CONTROL, 0, 0, 0)
        user32.keybd_event(VK_SHIFT, 0, 0, 0)
        user32.keybd_event(vk_arrow, 0, 0, 0)
        time.sleep(0.05)
        user32.keybd_event(vk_arrow, 0, KEYEVENTF_KEYUP, 0)
        user32.keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0)
        user32.keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0)

    chord(VK_RIGHT)
    time.sleep(0.6)
    mid = _visible_fences()
    chord(VK_LEFT)
    time.sleep(0.6)
    after = _visible_fences()

    assert after, "切回后无可见分区（疑似空白页）"

    GW_OWNER = 4
    GWL_STYLE = -16
    WS_CHILD = 0x40000000
    progman = int(user32.FindWindowW("Progman", None) or 0)
    bad = []
    for h in after:
        owner = int(user32.GetWindow(h, GW_OWNER) or 0)
        style = int(user32.GetWindowLongPtrW(h, GWL_STYLE))
        if style & WS_CHILD:
            bad.append(f"{h} WS_CHILD")
        if owner == 0:
            bad.append(f"{h} owner=0")
        elif progman and owner != progman:
            bad.append(f"{h} owner={owner} want Progman={progman}")
    assert not bad, bad
    # Multi-page layouts should usually change membership; single-page OK if equal.
    _ = before, mid
