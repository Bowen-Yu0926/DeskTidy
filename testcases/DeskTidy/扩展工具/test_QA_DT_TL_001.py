# -*- coding: utf-8 -*-
"""QA-DT-TL-001: calculator enable gate + system calc launch."""

from __future__ import annotations

import ctypes
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"

user32 = ctypes.windll.user32
shell32 = ctypes.windll.shell32


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def test_pet_and_hotkey_gate_before_open():
    app = _read("app.cpp")
    assert "计算器未启用" in app
    assert 'id == L"calculator"' in app or 'id == "calculator"' in app
    # Gate then OpenCalculator — disabled path must return before open.
    assert "OpenCalculator()" in app
    # Pet/hotkey branches mention calculator_enabled.
    assert "calculator_enabled" in app


def test_open_calculator_find_or_shell_launch():
    tools = _read("tools_misc.cpp")
    assert "OpenCalculator" in tools
    assert "计算器" in tools and "Calculator" in tools
    assert "calc.exe" in tools
    assert "calculator:" in tools
    assert "ShowWindow" in tools and "SW_RESTORE" in tools


def test_runtime_system_calculator_appears():
    # Mirror OpenCalculator: prefer existing, else ShellExecute calc.exe.
    EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    found = ctypes.c_void_p()

    def _enum(hwnd, _lp):
        if not user32.IsWindowVisible(hwnd):
            return True
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, buf, 256)
        title = buf.value or ""
        if "计算器" in title or "Calculator" in title:
            found.value = hwnd
            return False
        return True

    user32.EnumWindows(EnumWindowsProc(_enum), 0)
    already = bool(found.value)
    if not already:
        rc = shell32.ShellExecuteW(None, "open", "calc.exe", None, None, 1)
        if rc <= 32:
            shell32.ShellExecuteW(None, "open", "calculator:", None, None, 1)
        deadline = time.time() + 8.0
        while time.time() < deadline and not found.value:
            time.sleep(0.25)
            user32.EnumWindows(EnumWindowsProc(_enum), 0)
    assert found.value, "system calculator window did not appear"
    if not already:
        # Soft-close: leave calc for user if it was already open; otherwise hide.
        user32.ShowWindow(found.value, 6)  # SW_MINIMIZE


def test_disabled_path_returns_before_open_in_hotkey_block():
    app = _read("app.cpp")
    # Snippet around hotkey calculator: toast then return before OpenCalculator.
    idx = app.find('id == "calculator"')
    assert idx > 0
    block = app[idx : idx + 350]
    assert "计算器未启用" in block
    assert "return;" in block
    assert "OpenCalculator()" in block
    # return must appear before OpenCalculator in this gate block.
    assert block.find("return;") < block.find("OpenCalculator()")
