"""Organize rule picker: icon / document."""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QCheckBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QVBoxLayout,
    QWidget,
)

from src.fence_rules import (
    ORGANIZE_KIND_LABELS,
    ORGANIZE_KINDS,
    normalize_organize_kinds,
)


class OrganizeKindPicker(QWidget):
    """Checkbox group for the only supported organize rule kinds."""

    def __init__(self, selected: list[str] | None = None, parent=None):
        super().__init__(parent)
        self.setObjectName("organizeKindPicker")
        selected_set = set(normalize_organize_kinds(selected or []))
        self._checks: dict[str, QCheckBox] = {}

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        hint = QLabel(
            "只按类型整理：软件（快捷方式）、文档（含文件夹与其它文件）。"
            "可多选；未勾选则该分区不参与一键整理。"
        )
        hint.setObjectName("appSubtitle")
        hint.setWordWrap(True)
        layout.addWidget(hint)

        card = QFrame()
        card.setObjectName("ruleGroupCard")
        row = QHBoxLayout(card)
        row.setContentsMargins(14, 12, 14, 12)
        row.setSpacing(18)
        for kind in ORGANIZE_KINDS:
            cb = QCheckBox(ORGANIZE_KIND_LABELS[kind])
            cb.setChecked(kind in selected_set)
            cb.setCursor(Qt.CursorShape.PointingHandCursor)
            self._checks[kind] = cb
            row.addWidget(cb)
        row.addStretch()
        layout.addWidget(card)

    def selected_kinds(self) -> list[str]:
        return [kind for kind in ORGANIZE_KINDS if self._checks[kind].isChecked()]
