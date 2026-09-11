"""Multi-select extension rule picker for fence editor."""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from src.fence_rules import ALL_PRESET_EXTENSIONS, PRESET_EXTENSION_GROUPS, normalize_extension

_TILE_COLUMNS = 5


class ExtensionRulePicker(QWidget):
    def __init__(self, selected: list[str] | None = None, parent=None):
        super().__init__(parent)
        self.setObjectName("extensionRulePicker")
        selected_set = {normalize_extension(e) for e in (selected or [])}
        preset_set = set(ALL_PRESET_EXTENSIONS)
        self._chips: dict[str, QPushButton] = {}
        self._custom_grid: QGridLayout | None = None
        self._custom_count = 0

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        hint = QLabel("点击扩展名即可选中（可多选）；未选中时该分区不参与自动整理。")
        hint.setObjectName("appSubtitle")
        hint.setWordWrap(True)
        layout.addWidget(hint)

        scroll = QScrollArea()
        scroll.setObjectName("rulePickerScroll")
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setMinimumHeight(260)
        scroll.setMaximumHeight(320)

        scroll_host = QWidget()
        scroll_host.setObjectName("rulePickerHost")
        scroll_layout = QVBoxLayout(scroll_host)
        scroll_layout.setContentsMargins(4, 4, 4, 4)
        scroll_layout.setSpacing(10)

        for group_name, extensions in PRESET_EXTENSION_GROUPS.items():
            scroll_layout.addWidget(self._build_group(group_name, extensions, selected_set))

        custom_exts = sorted(ext for ext in selected_set if ext and ext not in preset_set)
        scroll_layout.addWidget(self._build_custom_group(custom_exts))
        scroll_layout.addStretch()
        scroll.setWidget(scroll_host)
        layout.addWidget(scroll)

        add_row = QHBoxLayout()
        self.custom_edit = QLineEdit()
        self.custom_edit.setPlaceholderText("自定义扩展名，如 .psd")
        add_btn = QPushButton("添加")
        add_btn.setObjectName("secondaryBtn")
        add_btn.clicked.connect(self._add_custom)
        self.custom_edit.returnPressed.connect(self._add_custom)
        add_row.addWidget(self.custom_edit, stretch=1)
        add_row.addWidget(add_btn)
        layout.addLayout(add_row)

    def _make_chip(self, ext: str, checked: bool) -> QPushButton:
        chip = QPushButton(ext)
        chip.setObjectName("extRuleChip")
        chip.setCheckable(True)
        chip.setChecked(checked)
        chip.setCursor(Qt.CursorShape.PointingHandCursor)
        chip.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._chips[ext] = chip
        return chip

    def _build_group(self, title: str, extensions: list[str], selected: set[str]) -> QFrame:
        frame = QFrame()
        frame.setObjectName("ruleGroupCard")
        group_layout = QVBoxLayout(frame)
        group_layout.setContentsMargins(12, 10, 12, 10)
        group_layout.setSpacing(8)

        header = QHBoxLayout()
        name_label = QLabel(title)
        name_label.setObjectName("panelTitle")
        select_btn = QPushButton("全选")
        select_btn.setObjectName("ruleGroupActionBtn")
        select_btn.clicked.connect(lambda: self._set_group_checked(extensions, True))
        clear_btn = QPushButton("清空")
        clear_btn.setObjectName("ruleGroupActionBtn")
        clear_btn.clicked.connect(lambda: self._set_group_checked(extensions, False))
        header.addWidget(name_label)
        header.addStretch()
        header.addWidget(select_btn)
        header.addWidget(clear_btn)
        group_layout.addLayout(header)

        grid = QGridLayout()
        grid.setHorizontalSpacing(8)
        grid.setVerticalSpacing(8)
        for index, ext in enumerate(extensions):
            grid.addWidget(
                self._make_chip(ext, ext in selected),
                index // _TILE_COLUMNS,
                index % _TILE_COLUMNS,
            )
        group_layout.addLayout(grid)
        return frame

    def _build_custom_group(self, custom_exts: list[str]) -> QFrame:
        frame = QFrame()
        frame.setObjectName("ruleGroupCard")
        group_layout = QVBoxLayout(frame)
        group_layout.setContentsMargins(12, 10, 12, 10)
        group_layout.setSpacing(8)

        title = QLabel("自定义")
        title.setObjectName("panelTitle")
        group_layout.addWidget(title)

        self._custom_grid = QGridLayout()
        self._custom_grid.setHorizontalSpacing(8)
        self._custom_grid.setVerticalSpacing(8)
        group_layout.addLayout(self._custom_grid)

        for ext in custom_exts:
            self._add_custom_chip(ext, checked=True)
        if not custom_exts:
            empty = QLabel("暂无自定义扩展名")
            empty.setObjectName("appSubtitle")
            group_layout.addWidget(empty)
            self._custom_empty_label = empty
        else:
            self._custom_empty_label = None
        return frame

    def _set_group_checked(self, extensions: list[str], checked: bool) -> None:
        for ext in extensions:
            chip = self._chips.get(ext)
            if chip:
                chip.setChecked(checked)

    def _add_custom_chip(self, ext: str, checked: bool = True) -> None:
        if not self._custom_grid:
            return
        if self._custom_empty_label:
            self._custom_empty_label.hide()
            self._custom_empty_label = None

        row = self._custom_count // _TILE_COLUMNS
        col = self._custom_count % _TILE_COLUMNS
        self._custom_grid.addWidget(self._make_chip(ext, checked), row, col)
        self._custom_count += 1

    def _add_custom(self) -> None:
        ext = normalize_extension(self.custom_edit.text())
        if not ext or ext == ".":
            return
        existing = self._chips.get(ext)
        if existing:
            existing.setChecked(True)
        else:
            self._add_custom_chip(ext, checked=True)
        self.custom_edit.clear()

    def selected_extensions(self) -> list[str]:
        result: list[str] = []
        seen: set[str] = set()
        for ext, chip in self._chips.items():
            if not chip.isChecked():
                continue
            normalized = normalize_extension(ext)
            if normalized and normalized not in seen:
                seen.add(normalized)
                result.append(normalized)
        return result
