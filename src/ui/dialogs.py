"""Reusable confirmation dialogs (G8, UX-F-18 — v4.0.28).

Product decision (docs/ux-decisions-4.0.28.md, Q4/G8): explicit deletes
(context menu / Del key) require a confirmation; drag-to-recycle never does.
In-app restore / undo stack is explicitly out of scope for this round.

F-101: the app ships on PyQt6 (PyQt6 6.11.0 / Qt 6.11.0 in .venv); the wrong
Qt binding import breaks the Del-key path while smoke stays green, so the
import must come from PyQt6. Any dialog failure
falls back to False (refuse the delete) and logs — F-102: a broken confirm
dialog must never silently delete files or kill the Del-key path.
"""

from __future__ import annotations

from src.app_logging import get_logger

from PyQt6.QtWidgets import QMessageBox, QWidget

__all__ = ["ask_confirm_delete", "qt_binding_self_check"]


def qt_binding_self_check() -> list[str]:
    """F-101 regression gate (t14): detect a wrong/missing Qt binding early.

    Returns a list of problems (empty = healthy):
    - every src.ui.* and src.desktop_pet module must import (a module that
      defers an import of a missing package used to slip past smoke while
      breaking the Del-key path at runtime);
    - no source file in src/ or scripts/ may import PySide6 (the app ships
      on PyQt6; this repo must not regress to the wrong binding).

    Cheap enough to call from a selftest or a debug hook; not run on import.
    """
    problems: list[str] = []
    import re
    from pathlib import Path

    pat = re.compile(r"^\s*(from|import)\s+PySide6\b")
    root = Path(__file__).resolve().parents[2]
    for base in (root / "src", root / "scripts"):
        for py in base.rglob("*.py"):
            try:
                for lineno, line in enumerate(
                    py.read_text(encoding="utf-8", errors="replace").splitlines(), 1
                ):
                    if pat.match(line):
                        problems.append(f"{py.relative_to(root)}:{lineno}: PySide6")
            except OSError:
                continue
    for module in (
        "src.ui.dialogs",
        "src.ui.toast",
        "src.ui.page_indicator",
        "src.ui.pet_widget",
        "src.desktop_pet",
    ):
        try:
            __import__(module)
        except Exception as exc:  # noqa: BLE001 - report, don't raise
            problems.append(f"{module}: import failed: {exc}")
    return problems


def ask_confirm_delete(parent: QWidget | None, count: int) -> bool:
    """Return True when the user confirms moving *count* item(s) to the Bin.

    Any exception (headless / broken Qt / dialog failure) logs and returns
    False — refuse the delete instead of deleting without confirmation.
    """
    if count <= 0:
        return False
    if count == 1:
        text = "确定要将所选 1 项移入回收站吗？"
    else:
        text = f"确定要将所选 {count} 项移入回收站吗？"
    try:
        box = QMessageBox(parent)
        box.setIcon(QMessageBox.Icon.Question)
        box.setWindowTitle("删除确认")
        box.setText(text)
        box.setInformativeText("项目将从 DeskTidy 移除并放入系统回收站。")
        box.setStandardButtons(
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.Cancel
        )
        box.setDefaultButton(QMessageBox.StandardButton.Cancel)
        box.button(QMessageBox.StandardButton.Yes).setText("移入回收站")
        box.button(QMessageBox.StandardButton.Cancel).setText("取消")
        # PyQt6 exec() returns int; compare against the enum's actual value.
        return box.exec() == int(QMessageBox.StandardButton.Yes.value)
    except Exception:
        try:
            get_logger().exception(
                "ask_confirm_delete failed; refusing delete (count=%s)", count
            )
        except Exception:
            pass
        return False
