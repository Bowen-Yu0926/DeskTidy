# -*- coding: utf-8 -*-
"""QA-DT-CORE-001: process live + search/vault/todo/pet/page-bar contracts."""

from __future__ import annotations

import ctypes
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"

kernel32 = ctypes.windll.kernel32
CreateToolhelp32Snapshot = kernel32.CreateToolhelp32Snapshot
Process32FirstW = kernel32.Process32FirstW
Process32NextW = kernel32.Process32NextW
CloseHandle = kernel32.CloseHandle
TH32CS_SNAPPROCESS = 0x00000002


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", ctypes.c_ulong),
        ("cntUsage", ctypes.c_ulong),
        ("th32ProcessID", ctypes.c_ulong),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", ctypes.c_ulong),
        ("cntThreads", ctypes.c_ulong),
        ("th32ParentProcessID", ctypes.c_ulong),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", ctypes.c_ulong),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def _process_running(name: str) -> bool:
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == ctypes.c_void_p(-1).value or snap == -1:
        return False
    try:
        pe = PROCESSENTRY32W()
        pe.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        if not Process32FirstW(snap, ctypes.byref(pe)):
            return False
        while True:
            if pe.szExeFile.lower() == name.lower():
                return True
            if not Process32NextW(snap, ctypes.byref(pe)):
                break
        return False
    finally:
        CloseHandle(snap)


def test_desktidy_native_process_running():
    assert _process_running("desktidy_native.exe"), "desktidy_native.exe not running"


def test_core_entry_points_present():
    app_h = _read("app.h")
    app = _read("app.cpp")
    pet = _read("pet_window.cpp")
    assert "OpenSearch" in app_h and "OpenSearch" in app
    assert "OpenVault" in app_h and "OpenVault" in app
    assert "OpenTodos" in app_h and "OpenTodos" in app
    assert "void PetWindow::Tick" in pet
    assert "WantedTickMs" in pet


def test_page_bar_text_center_alignment():
    pb = _read("page_bar.cpp")
    assert "StringAlignmentCenter" in pb
