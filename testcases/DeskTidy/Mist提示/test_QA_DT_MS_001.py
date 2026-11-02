# -*- coding: utf-8 -*-
"""QA-DT-MS-001: Mist alerts on key failure paths."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def test_wallpaper_help_capture_use_mist():
    tools = _read("tools_misc.cpp")
    help_c = _read("help_center.cpp")
    cap = _read("capture_overlay.cpp")
    assert "MistAlert" in tools
    assert "MistAlert" in help_c
    assert "MistAlert" in cap
    assert "设置壁纸失败" in tools or "wallpaper" in tools.lower()
    assert "找不到 help.html" in help_c or "无法打开帮助" in help_c
    assert "截图保存失败" in cap


def test_key_files_prefer_mist_over_messagebox_for_alerts():
    for name in ("tools_misc.cpp", "help_center.cpp", "capture_overlay.cpp"):
        text = _read(name)
        mist_n = text.count("MistAlert") + text.count("MistMessage")
        # Native MessageBox may still exist for rare paths; Mist should dominate.
        mb_n = text.count("MessageBoxW") + text.count("MessageBoxA")
        assert mist_n > 0, name
        assert mist_n >= mb_n, f"{name}: Mist={mist_n} MessageBox={mb_n}"
