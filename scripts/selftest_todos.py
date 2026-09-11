"""Selftest: desktop todos module + wiring."""

from __future__ import annotations

import inspect
import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
import scripts.selftest_env  # noqa: F401, E402


def main() -> None:
    data = json.loads((ROOT / "config" / "default_settings.json").read_text(encoding="utf-8"))
    assert "desktop_todos" in data
    assert data["desktop_todos"].get("enabled") is False

    from src.todos import (
        add_todo,
        delete_todo,
        desktop_todos_enabled,
        desktop_todos_settings,
        load_todos,
        temporary_todos_file,
        toggle_todo,
        update_todo_text,
    )

    s: dict = {}
    cfg = desktop_todos_settings(s)
    assert cfg is s["desktop_todos"]
    assert desktop_todos_enabled(s) is False
    cfg["enabled"] = True
    assert desktop_todos_enabled(s) is True

    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "todos.json"
        with temporary_todos_file(path):
            assert load_todos() == []
            items = add_todo("买牛奶")
            assert len(items) == 1
            assert items[0]["text"] == "买牛奶"
            assert items[0]["done"] is False
            item_id = items[0]["id"]
            items = toggle_todo(item_id, items)
            assert items[0]["done"] is True
            items = update_todo_text(item_id, "买牛奶和面包", items)
            assert items[0]["text"] == "买牛奶和面包"
            items = add_todo("开会", items)
            assert len(items) == 2
            items = delete_todo(item_id, items)
            assert len(items) == 1
            assert items[0]["text"] == "开会"
            assert path.is_file()

    from src.app import DeskTidyApp
    from src.ui.extensions_widget import ExtensionsWidget
    from src.ui.page_indicator import PageIndicatorWidget
    from src.ui.todo_widget import DesktopTodoWidget

    assert "_setup_todo_panel" in inspect.getsource(DeskTidyApp)
    assert "_on_todo_requested" in inspect.getsource(DeskTidyApp)
    assert "todo_panel" in inspect.getsource(DeskTidyApp._iter_overlay_widgets)
    assert "todo_requested" in inspect.getsource(PageIndicatorWidget)
    assert "_make_todo_button" in inspect.getsource(PageIndicatorWidget)
    tool_flags = inspect.getsource(PageIndicatorWidget._tool_flags)
    assert "float_bar_tool_flags" in tool_flags
    from src.desktop_pet import float_bar_tool_flags

    flags = float_bar_tool_flags({"desktop_todos": {"enabled": True}})
    assert flags["todo"] is True
    flags_off = float_bar_tool_flags({"desktop_todos": {"enabled": False}})
    assert flags_off["todo"] is False
    ext = inspect.getsource(ExtensionsWidget._build_ui)
    assert "desktop_todos_enabled_cb" in ext
    assert "桌面待办" in ext
    assert "添加待办事项" in inspect.getsource(DesktopTodoWidget)
    assert "todoDeleteBtn" in inspect.getsource(DesktopTodoWidget)

    print("selftest_todos: OK")


if __name__ == "__main__":
    main()
