# -*- coding: utf-8 -*-
"""QA-DT-TL-002: desktop todos enable, persist, CLI show."""

from __future__ import annotations

import ctypes
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"
EXE = ROOT / "native" / "build" / "Release" / "desktidy_native.exe"

user32 = ctypes.windll.user32


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def test_enable_gate_and_show_desktop_band():
    app = _read("app.cpp")
    todo = _read("todo_window.cpp")
    assert "桌面待办未启用" in app
    assert "OpenTodos" in app
    assert "todos_.Show" in app or "void App::OpenTodos" in app
    assert "ShowWindow" in todo
    assert "AttachOverlayToDesktop" in todo
    assert "PlaceOverlayInDesktopBand" in todo


def test_add_toggle_clear_del_save_and_partition():
    todo = _read("todo_window.cpp")
    for action in ('id == L"add"', 'id == L"toggle"', 'id == L"clear"', 'id == L"del"'):
        assert action in todo
    # Save after mutations.
    assert todo.count("Save();") >= 4
    assert "stable_partition" in todo


def test_runtime_cli_todos_shows_window():
    assert EXE.is_file(), f"missing build: {EXE}"
    # Ensure a host instance is running.
    running = False
    try:
        import psutil  # type: ignore

        running = any(
            (p.name() or "").lower() == "desktidy_native.exe" for p in psutil.process_iter()
        )
    except Exception:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq desktidy_native.exe"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
        )
        running = "desktidy_native.exe" in (out.stdout or "")

    if not running:
        subprocess.Popen(
            [str(EXE)],
            cwd=str(EXE.parent),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(3.0)

    subprocess.Popen(
        [str(EXE), "--todos"],
        cwd=str(EXE.parent),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    cls = "DeskTidyNativeTodo"
    hwnd = None
    deadline = time.time() + 10.0
    while time.time() < deadline:
        hwnd = user32.FindWindowW(cls, None)
        if hwnd:
            break
        time.sleep(0.3)
    assert hwnd, "DeskTidyNativeTodo window not found after --todos"
    # Hide so we don't leave chrome up unexpectedly.
    user32.ShowWindow(hwnd, 0)  # SW_HIDE


def test_disabled_todos_hotkey_returns_before_open():
    app = _read("app.cpp")
    idx = app.find('id == "todos"')
    assert idx > 0
    block = app[idx : idx + 350]
    assert "桌面待办未启用" in block
    assert block.find("return;") < block.find("OpenTodos()")
