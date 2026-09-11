"""Fence style preset picker — click applies immediately (360桌面助手-like)."""

from __future__ import annotations

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor, QPainter, QPen
from PyQt6.QtWidgets import (
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from src.fence_style import (
    FENCE_STYLE_PRESETS,
    fence_style_preset_ids,
    fence_style_visual_from_preset,
    normalize_fence_style_preset,
    settings_fence_style_preset,
)
from src.ui.section_card import SectionCard


class FenceStylePresetCard(QFrame):
    """Clickable preset swatch (name + color chip)."""

    clicked = pyqtSignal(str)

    def __init__(self, preset_id: str, parent: QWidget | None = None):
        super().__init__(parent)
        self.preset_id = preset_id
        meta = FENCE_STYLE_PRESETS[preset_id]
        self.setObjectName("fenceStylePresetCard")
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFixedHeight(72)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self._selected = False
        self._bg = QColor(str(meta.get("background") or "#2A2E35"))
        self._accent = QColor(str(meta.get("accent") or "#4C8DFF"))
        try:
            self._opacity = float(meta.get("opacity") or 0.7)
        except (TypeError, ValueError):
            self._opacity = 0.7

        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 8, 10, 8)
        layout.setSpacing(2)
        self._name = QLabel(str(meta.get("name") or preset_id))
        self._name.setObjectName("fenceStylePresetName")
        self._name.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        light = self._is_light_bg()
        ink = "#0F172A" if light else "#F8FAFC"
        muted = "#475569" if light else "#CBD5E1"
        self._name.setStyleSheet(f"color: {ink}; font-weight: 700; background: transparent;")
        self._hint = QLabel(str(meta.get("hint") or ""))
        self._hint.setObjectName("fenceStylePresetHint")
        self._hint.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self._hint.setWordWrap(True)
        self._hint.setStyleSheet(f"color: {muted}; font-size: 11px; background: transparent;")
        layout.addStretch(1)
        layout.addWidget(self._name)
        layout.addWidget(self._hint)

    def _is_light_bg(self) -> bool:
        r, g, b, _a = self._bg.getRgb()
        return (0.2126 * r + 0.7152 * g + 0.0722 * b) > 160

    def set_selected(self, selected: bool) -> None:
        self._selected = bool(selected)
        self.update()

    def mousePressEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit(self.preset_id)
            event.accept()
            return
        super().mousePressEvent(event)

    def paintEvent(self, event) -> None:  # noqa: N802
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = self.rect().adjusted(1, 1, -1, -1)
        fill = QColor(self._bg)
        fill.setAlpha(int(max(0.40, min(1.0, self._opacity)) * 255))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(fill)
        painter.drawRoundedRect(rect, 8, 8)
        # Accent bar on the left edge.
        bar = rect.adjusted(0, 6, 0, -6)
        bar.setWidth(4)
        painter.setBrush(self._accent)
        painter.drawRoundedRect(bar, 2, 2)
        if self._selected:
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.setPen(QPen(self._accent, 2))
            painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 8, 8)
        painter.end()
        # Do not call QFrame.paintEvent — it would paint over the swatch.


class FenceStylePickerWidget(QWidget):
    """Choose a pack → apply to all fences (live + persist), 360-like one-click."""

    style_preview_requested = pyqtSignal(dict)
    style_apply_requested = pyqtSignal(str)
    style_revert_requested = pyqtSignal()

    def __init__(self, settings: dict, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings
        self._cards: dict[str, FenceStylePresetCard] = {}
        self._selected = settings_fence_style_preset(settings)
        self._applied = self._selected
        self._build_ui()
        self._sync_selection()
        self._sync_status()

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        card = SectionCard(
            "分区外观",
            "点击色块立即应用到全部分区（只改分区底色/透明度，不改图标标题颜色）。"
            "单个分区仍可在编辑里微调。",
        )
        grid = QGridLayout()
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(10)
        for index, preset_id in enumerate(fence_style_preset_ids()):
            chip = FenceStylePresetCard(preset_id)
            chip.clicked.connect(self._on_preset_clicked)
            self._cards[preset_id] = chip
            grid.addWidget(chip, index // 3, index % 3)
        card.add_body_layout(grid)

        actions = QHBoxLayout()
        actions.setSpacing(8)
        self._status = QLabel("")
        self._status.setObjectName("sectionHint")
        actions.addWidget(self._status, stretch=1)
        self._apply_btn = QPushButton("重新应用")
        self._apply_btn.setObjectName("secondaryBtn")
        self._apply_btn.setToolTip("将当前选中样式再次写入全部分区（含后来新建的分区）")
        self._apply_btn.clicked.connect(self._on_reapply)
        actions.addWidget(self._apply_btn)
        card.add_body_layout(actions)
        root.addWidget(card)

    def reload_from_settings(self) -> None:
        self._applied = settings_fence_style_preset(self.settings)
        self._selected = self._applied
        self._sync_selection()
        self._sync_status()

    def _sync_selection(self) -> None:
        for preset_id, chip in self._cards.items():
            chip.set_selected(preset_id == self._selected)

    def _sync_status(self) -> None:
        name = FENCE_STYLE_PRESETS.get(self._applied, {}).get("name", self._applied)
        self._status.setText(f"当前：{name}")
        self._apply_btn.setEnabled(True)

    def _on_preset_clicked(self, preset_id: str) -> None:
        key = normalize_fence_style_preset(preset_id)
        self._selected = key
        self._sync_selection()
        # Live paint first so the desktop updates even if persist is slow.
        self.style_preview_requested.emit(fence_style_visual_from_preset(key))
        self.style_apply_requested.emit(key)
        self._applied = key
        self.settings["fence_style_preset"] = key
        self._sync_status()

    def _on_reapply(self) -> None:
        key = normalize_fence_style_preset(self._selected)
        self.style_preview_requested.emit(fence_style_visual_from_preset(key))
        self.style_apply_requested.emit(key)
        self._applied = key
        self.settings["fence_style_preset"] = key
        self._sync_status()
