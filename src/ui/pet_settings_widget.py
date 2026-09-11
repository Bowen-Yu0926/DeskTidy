"""Dedicated settings page for desktop pet."""

from __future__ import annotations

from PyQt6.QtCore import QRect, Qt, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QPainter, QPen, QPixmap
from PyQt6.QtWidgets import (
    QCheckBox,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QScrollArea,
    QSlider,
    QVBoxLayout,
    QWidget,
)

from src.desktop_pet import (
    MAX_SIZE_SCALE,
    MIN_SIZE_SCALE,
    desktop_pet_settings,
    normalize_pet_character,
    normalize_pet_size_scale,
    pet_catalog,
    pet_character_ids,
    pet_enabled_actions,
    pet_settings_action_options,
    pet_size_scale,
    pet_sprite_path,
    set_pet_enabled_actions_for_character,
)
from src.settings import save_settings
from src.ui.styles import get_theme_palette, normalize_theme

_PET_BLURBS = {
    "hoodie": "卫衣少年：摸摸、玩手机 / 睡觉（等待时轮流）、跌落；拖拽揪领，松开跌落",
    "voyage": "出海少女：救生衣草帽造型；摸摸、等待（刷手机）、跌落；拖拽拎起",
}


class PetCharCard(QFrame):
    """Gallery card: preview left, per-character action toggles on the right when selected."""

    clicked = pyqtSignal(str)
    actions_changed = pyqtSignal(str)

    def __init__(
        self,
        char_id: str,
        title: str,
        blurb: str,
        parent: QWidget | None = None,
    ):
        super().__init__(parent)
        self.char_id = char_id
        self._selected = False
        self._hover = False
        self._pix = QPixmap(str(pet_sprite_path(char_id, "idle")))
        self.setObjectName("petCharCard")
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setMinimumSize(268, 236)
        self.setMouseTracking(True)

        root = QVBoxLayout(self)
        root.setContentsMargins(12, 10, 12, 10)
        root.setSpacing(6)

        top = QHBoxLayout()
        top.setSpacing(8)

        self._preview = QLabel()
        self._preview.setFixedSize(108, 148)
        self._preview.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._preview.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self._refresh_preview()
        top.addWidget(self._preview, 0)

        self._actions_col = QWidget()
        self._actions_col.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, False)
        actions_layout = QVBoxLayout(self._actions_col)
        actions_layout.setContentsMargins(0, 4, 0, 0)
        actions_layout.setSpacing(4)

        hint = QLabel("可选动作\n（取消勾选即从「⋯」隐藏）")
        hint.setObjectName("petCardActionHint")
        hint.setWordWrap(True)
        actions_layout.addWidget(hint)

        self._action_cbs: dict[str, QCheckBox] = {}
        for aid, label in pet_settings_action_options(char_id):
            cb = QCheckBox(label)
            cb.setObjectName("petCardActionCb")
            cb.stateChanged.connect(self._on_action_toggled)
            self._action_cbs[aid] = cb
            actions_layout.addWidget(cb)
        actions_layout.addStretch()
        top.addWidget(self._actions_col, 1)

        root.addLayout(top)

        self._title_label = QLabel(title)
        self._title_label.setObjectName("petCardTitle")
        self._title_label.setAlignment(Qt.AlignmentFlag.AlignHCenter)
        self._title_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        root.addWidget(self._title_label)

        self._blurb_label = QLabel(blurb)
        self._blurb_label.setObjectName("petCardBlurb")
        self._blurb_label.setWordWrap(True)
        self._blurb_label.setAlignment(Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop)
        self._blurb_label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        root.addWidget(self._blurb_label)

        self._actions_col.setVisible(False)

    def _refresh_preview(self) -> None:
        if self._pix.isNull():
            self._preview.clear()
            return
        scaled = self._pix.scaled(
            self._preview.size(),
            Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        self._preview.setPixmap(scaled)

    def action_checkboxes(self) -> dict[str, QCheckBox]:
        return self._action_cbs

    def set_action_options(
        self,
        *,
        enabled_ids: set[str],
        visible_ids: set[str],
        labels: dict[str, str],
        show_panel: bool,
    ) -> None:
        self._actions_col.setVisible(show_panel)
        for aid, cb in self._action_cbs.items():
            cb.blockSignals(True)
            visible = aid in visible_ids
            cb.setVisible(visible)
            if visible:
                cb.setText(labels.get(aid, cb.text()))
                cb.setChecked(aid in enabled_ids)
            cb.blockSignals(False)

    def enabled_action_ids(self) -> list[str]:
        return [aid for aid, cb in self._action_cbs.items() if cb.isChecked()]

    def set_selected(self, selected: bool) -> None:
        selected = bool(selected)
        if self._selected == selected:
            return
        self._selected = selected
        self._actions_col.setVisible(selected)
        self.update()

    def _on_action_toggled(self, _state: int) -> None:
        if self._selected:
            self.actions_changed.emit(self.char_id)

    def enterEvent(self, event) -> None:  # noqa: N802
        self._hover = True
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:  # noqa: N802
        self._hover = False
        self.update()
        super().leaveEvent(event)

    def mousePressEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            widget = self.childAt(event.position().toPoint())
            while widget is not None and widget is not self:
                if isinstance(widget, QCheckBox) or widget is self._actions_col:
                    return super().mousePressEvent(event)
                widget = widget.parent()
            self.clicked.emit(self.char_id)
            event.accept()
            return
        super().mousePressEvent(event)

    def paintEvent(self, event) -> None:  # noqa: N802
        del event
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        rect = self.rect().adjusted(1, 1, -1, -1)
        border = QColor("#2563EB") if self._selected else QColor("#D5DCE7")
        fill = QColor("#DBEAFE") if (self._selected or self._hover) else QColor(255, 255, 255, 245)
        width = 2 if self._selected else 1
        painter.setPen(QPen(border, width))
        painter.setBrush(fill)
        painter.drawRoundedRect(rect, 18, 18)
        if self._selected:
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor("#2563EB"))
            painter.drawEllipse(self.width() - 28, 12, 16, 16)
            painter.setPen(QColor("white"))
            check = QFont("Segoe UI Symbol", 9)
            check.setBold(True)
            painter.setFont(check)
            painter.drawText(QRect(self.width() - 28, 12, 16, 16), int(Qt.AlignmentFlag.AlignCenter), "✓")
        painter.end()


class PetSettingsWidget(QWidget):
    pet_settings_changed = pyqtSignal()

    def __init__(self, settings: dict, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings
        self._cards: dict[str, PetCharCard] = {}
        self._palette = get_theme_palette(normalize_theme(self.settings.get("theme")))
        cfg = desktop_pet_settings(self.settings)
        self._actions_char_id = normalize_pet_character(str(cfg.get("character", "hoodie")))
        self._build_ui()
        self._apply_styles()

    def _build_ui(self) -> None:
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)

        content = QWidget()
        content.setObjectName("petSettingsPanel")
        layout = QVBoxLayout(content)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(14)

        panel = QFrame()
        panel.setObjectName("panelCard")
        panel_layout = QVBoxLayout(panel)
        panel_layout.setContentsMargins(16, 12, 16, 12)
        panel_layout.setSpacing(10)

        title = QLabel("桌面宠物")
        title.setObjectName("panelTitle")
        panel_layout.addWidget(title)
        hint = QLabel(
            "启用后宠物会出现在桌面；隐藏后可点下方「显示到桌面」找回。"
        )
        hint.setObjectName("appSubtitle")
        hint.setWordWrap(True)
        panel_layout.addWidget(hint)

        stat = QLabel("选中形象后，在卡片右侧勾选互动动作；桌面点「⋯」或点宠物使用")
        stat.setObjectName("petHeroMeta")
        panel_layout.addWidget(stat)

        cfg = desktop_pet_settings(self.settings)
        self.enabled_cb = QCheckBox("启用桌面宠物")
        self.enabled_cb.setChecked(bool(cfg.get("enabled", True)))
        self.enabled_cb.stateChanged.connect(self._on_enabled_changed)
        panel_layout.addWidget(self.enabled_cb)

        self.page_bubbles_cb = QCheckBox(
            "在宠物上显示浮标栏（分页 / 文件夹 / 录屏等；开启后隐藏右侧浮标栏）"
        )
        self.page_bubbles_cb.setChecked(bool(cfg.get("show_page_bubbles", True)))
        self.page_bubbles_cb.setToolTip(
            "是：浮标栏跟在宠物头上；否：恢复屏幕右侧浮标栏"
        )
        self.page_bubbles_cb.stateChanged.connect(self._on_page_bubbles_changed)
        panel_layout.addWidget(self.page_bubbles_cb)

        size_label = QLabel("宠物大小")
        size_label.setObjectName("fieldLabel")
        panel_layout.addWidget(size_label)
        size_row = QHBoxLayout()
        self.size_slider = QSlider(Qt.Orientation.Horizontal)
        self.size_slider.setMinimum(int(round(MIN_SIZE_SCALE * 100)))
        self.size_slider.setMaximum(int(round(MAX_SIZE_SCALE * 100)))
        self.size_slider.setSingleStep(5)
        self.size_slider.setPageStep(10)
        self.size_slider.setTickInterval(10)
        self.size_slider.setTickPosition(QSlider.TickPosition.TicksBelow)
        cur_scale = pet_size_scale(self.settings)
        self.size_slider.setValue(int(round(cur_scale * 100)))
        self.size_slider.setToolTip("调节桌面宠物显示大小（50%～160%，默认 100%）")
        self.size_value_label = QLabel(f"{int(round(cur_scale * 100))}%")
        self.size_value_label.setMinimumWidth(44)
        self.size_slider.valueChanged.connect(self._on_size_slider_changed)
        self.size_slider.sliderReleased.connect(self._on_size_slider_committed)
        size_row.addWidget(self.size_slider, 1)
        size_row.addWidget(self.size_value_label)
        panel_layout.addLayout(size_row)

        choose = QLabel("选择宠物形象")
        choose.setObjectName("fieldLabel")
        panel_layout.addWidget(choose)

        grid = QGridLayout()
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setHorizontalSpacing(12)
        grid.setVerticalSpacing(12)

        catalog = pet_catalog()
        current = normalize_pet_character(str(cfg.get("character", "hoodie")))
        for idx, char_id in enumerate(pet_character_ids()):
            meta = catalog[char_id]
            card = PetCharCard(
                char_id,
                str(meta.get("name", char_id)),
                _PET_BLURBS.get(char_id, "桌面小伙伴"),
            )
            card.set_selected(char_id == current)
            card.clicked.connect(self._on_card_clicked)
            card.actions_changed.connect(self._on_card_actions_changed)
            self._cards[char_id] = card
            grid.addWidget(card, idx // 2, idx % 2)

        panel_layout.addLayout(grid)
        self._refresh_action_checkboxes()

        show_row = QHBoxLayout()
        self.show_btn = QPushButton("显示到桌面")
        self.show_btn.setObjectName("primaryButton")
        self.show_btn.setToolTip("隐藏后点这里即可重新显示到桌面")
        self.show_btn.clicked.connect(self._on_show_clicked)
        show_row.addWidget(self.show_btn)
        show_row.addStretch()
        panel_layout.addLayout(show_row)

        layout.addWidget(panel)
        layout.addStretch()
        scroll.setWidget(content)
        outer.addWidget(scroll)

    def _cfg(self) -> dict:
        return desktop_pet_settings(self.settings)

    def _apply_styles(self) -> None:
        p = self._palette
        accent = p.get("accent", "#2563EB")
        border = p.get("border", "#D5DCE7")
        soft = p.get("accent_soft", "#DBEAFE")
        self.setStyleSheet(
            f"""
QLabel#petHeroMeta {{
    color: {accent};
    background: {soft};
    border: 1px solid {border};
    border-radius: 999px;
    padding: 6px 10px;
    font-weight: 600;
}}
QLabel#petCardTitle {{
    color: #1F2937;
    font-weight: 700;
    font-size: 10pt;
}}
QLabel#petCardBlurb {{
    color: #6B7280;
    font-size: 8pt;
}}
QLabel#petCardActionHint {{
    color: #6B7280;
    font-size: 8pt;
    font-weight: 600;
}}
QCheckBox#petCardActionCb {{
    font-size: 9pt;
}}
"""
        )

    def _emit_and_save(self) -> None:
        save_settings(self.settings)
        self.pet_settings_changed.emit()

    def _on_enabled_changed(self, _state: int) -> None:
        cfg = self._cfg()
        cfg["enabled"] = self.enabled_cb.isChecked()
        if cfg["enabled"]:
            # Re-enable must restore the pet; setdefault would leave a prior hide stuck.
            cfg["visible"] = True
        self._emit_and_save()

    def _on_show_clicked(self) -> None:
        cfg = self._cfg()
        cfg["enabled"] = True
        cfg["visible"] = True
        self.enabled_cb.blockSignals(True)
        self.enabled_cb.setChecked(True)
        self.enabled_cb.blockSignals(False)
        self._emit_and_save()

    def _on_page_bubbles_changed(self, _state: int) -> None:
        cfg = self._cfg()
        cfg["show_page_bubbles"] = self.page_bubbles_cb.isChecked()
        self._emit_and_save()

    def _on_size_slider_changed(self, value: int) -> None:
        self.size_value_label.setText(f"{int(value)}%")

    def _on_size_slider_committed(self) -> None:
        cfg = self._cfg()
        cfg["size_scale"] = normalize_pet_size_scale(self.size_slider.value() / 100.0)
        self.size_value_label.setText(f"{int(round(cfg['size_scale'] * 100))}%")
        self._emit_and_save()

    def _refresh_action_checkboxes(self) -> None:
        selected = normalize_pet_character(self._actions_char_id)
        for char_id, card in self._cards.items():
            char_id = normalize_pet_character(char_id)
            enabled_set = set(pet_enabled_actions(self.settings, character=char_id))
            visible_ids = {aid for aid, _ in pet_settings_action_options(char_id)}
            labels = dict(pet_settings_action_options(char_id))
            card.set_action_options(
                enabled_ids=enabled_set,
                visible_ids=visible_ids,
                labels=labels,
                show_panel=(char_id == selected),
            )

    def _persist_action_checkboxes(self, char_id: str) -> None:
        char_id = normalize_pet_character(char_id)
        card = self._cards.get(char_id)
        if card is None:
            return
        chosen = card.enabled_action_ids()
        set_pet_enabled_actions_for_character(self._cfg(), char_id, chosen)

    def _on_card_actions_changed(self, char_id: str) -> None:
        char_id = normalize_pet_character(char_id)
        if char_id != self._actions_char_id:
            return
        self._persist_action_checkboxes(char_id)
        self._emit_and_save()

    def _on_actions_changed(self, _state: int) -> None:
        """Legacy hook — actions now live on PetCharCard."""
        self._persist_action_checkboxes(self._actions_char_id)
        self._emit_and_save()

    def _on_card_clicked(self, char_id: str) -> None:
        char_id = normalize_pet_character(char_id)
        if char_id != self._actions_char_id:
            self._persist_action_checkboxes(self._actions_char_id)
            self._actions_char_id = char_id
            self._refresh_action_checkboxes()
        cfg = self._cfg()
        cfg["character"] = char_id
        cfg["enabled_actions"] = list(
            cfg.get("enabled_actions_by_character", {}).get(char_id, ["pet", "wait", "fall"])
        )
        for cid, card in self._cards.items():
            card.set_selected(cid == char_id)
        self._emit_and_save()
