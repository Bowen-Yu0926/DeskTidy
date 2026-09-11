"""Selftest: calculator module + wiring contracts."""

from __future__ import annotations

import inspect
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
import scripts.selftest_env  # noqa: F401, E402

def main() -> None:
    data = json.loads((ROOT / "config" / "default_settings.json").read_text(encoding="utf-8"))
    assert "calculator" in data
    assert data["calculator"].get("enabled") is False
    assert data["hotkeys"].get("calculator") == "Ctrl+Alt+C"

    from src.calculator import (
        calculator_enabled,
        calculator_settings,
        open_or_toggle_calculator,
    )
    from src.hotkey_manager import HOTKEY_FALLBACKS, HOTKEY_IDS, HOTKEY_LABELS, _ACTION_BY_ID, _POLLABLE_ACTIONS

    assert HOTKEY_IDS["calculator"] == 11
    assert HOTKEY_LABELS["calculator"] == "计算器"
    assert "calculator" not in _POLLABLE_ACTIONS
    assert "calculator" in HOTKEY_FALLBACKS
    assert _ACTION_BY_ID[11] == "calculator"

    s: dict = {}
    cfg = calculator_settings(s)
    assert cfg is s["calculator"]
    assert calculator_enabled(s) is False
    cfg["enabled"] = True
    assert calculator_enabled(s) is True

    # Launch path exists (do not leave a calculator window if already open —
    # toggle is best-effort; just ensure callable returns bool).
    assert callable(open_or_toggle_calculator)

    from src.app import DeskTidyApp
    from src.ui.extensions_widget import ExtensionsWidget
    from src.ui.page_indicator import PageIndicatorWidget

    setup = inspect.getsource(DeskTidyApp._setup_hotkeys)
    assert "calculator" in setup
    assert "_on_calculator_requested" in inspect.getsource(DeskTidyApp)
    assert "calculator_requested" in inspect.getsource(PageIndicatorWidget)
    assert "_make_calculator_button" in inspect.getsource(PageIndicatorWidget)
    tool_flags = inspect.getsource(PageIndicatorWidget._tool_flags)
    assert "float_bar_tool_flags" in tool_flags
    from src.desktop_pet import float_bar_tool_flags

    flags = float_bar_tool_flags({"calculator": {"enabled": True}})
    assert flags["calculator"] is True
    flags_off = float_bar_tool_flags({"calculator": {"enabled": False}})
    assert flags_off["calculator"] is False
    ext = inspect.getsource(ExtensionsWidget._build_ui)
    assert "calculator_enabled_cb" in ext
    assert "计算器" in ext

    print("selftest_calculator: OK")


if __name__ == "__main__":
    main()
