"""Pick which screen to record — shown at record start (not in settings)."""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QGuiApplication, QKeySequence, QShortcut
from PyQt6.QtWidgets import (
    QDialog,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from src.screen_record_manager import capture_screen_choices, normalize_capture_screen
from src.ui.styles import get_theme_palette, normalize_theme


def pick_capture_screen(
    *,
    initial: str | None = None,
    parent: QWidget | None = None,
) -> str | None:
    """Modal picker. Returns a capture_screen value, or None if cancelled."""
    dlg = RecordScreenPickerDialog(initial=initial, parent=parent)
    if dlg.exec() != QDialog.DialogCode.Accepted:
        return None
    return dlg.selected_value()


class RecordScreenPickerDialog(QDialog):
    """Topmost dialog: click a screen to start; Esc / 取消 aborts."""

    def __init__(
        self,
        *,
        initial: str | None = None,
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setWindowTitle("选择录制屏幕")
        self.setModal(True)
        self.setWindowFlags(
            Qt.WindowType.Dialog
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.CustomizeWindowHint
            | Qt.WindowType.WindowTitleHint
            | Qt.WindowType.WindowCloseButtonHint
        )
        self.setMinimumWidth(360)
        self._selected: str | None = None
        preferred = normalize_capture_screen(initial or "primary")
        if preferred not in {v for v, _ in capture_screen_choices()}:
            preferred = "primary"

        theme = "mist"
        try:
            from src.settings import load_settings

            theme = normalize_theme(load_settings().get("theme"))
        except Exception:
            pass
        p = get_theme_palette(theme)
        self.setStyleSheet(
            f"""
            QDialog {{
                background: {p['card']};
                color: {p['text']};
            }}
            QLabel#pickerTitle {{
                color: {p['text']};
                font-size: 16px;
                font-weight: 700;
            }}
            QLabel#pickerHint {{
                color: {p['text_muted']};
                font-size: 12px;
            }}
            QPushButton#screenChoice {{
                background: {p['card_alt']};
                color: {p['text']};
                border: 1px solid {p['border']};
                border-radius: 10px;
                padding: 12px 14px;
                text-align: left;
                font-size: 13px;
                font-weight: 600;
            }}
            QPushButton#screenChoice:hover {{
                background: {p['accent_soft']};
                border-color: {p['accent']};
            }}
            QPushButton#screenChoice[preferred="true"] {{
                border: 2px solid {p['accent']};
                background: {p['accent_soft']};
            }}
            QPushButton#pickerCancel {{
                background: transparent;
                color: {p['text_muted']};
                border: 1px solid {p['border']};
                border-radius: 8px;
                padding: 8px 16px;
            }}
            """
        )

        root = QVBoxLayout(self)
        root.setContentsMargins(18, 16, 18, 14)
        root.setSpacing(10)

        title = QLabel("选择要录制的屏幕")
        title.setObjectName("pickerTitle")
        root.addWidget(title)
        hint = QLabel("点击一项即开始录制；Esc 取消。")
        hint.setObjectName("pickerHint")
        hint.setWordWrap(True)
        root.addWidget(hint)

        for value, label in capture_screen_choices():
            btn = QPushButton(label)
            btn.setObjectName("screenChoice")
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            if value == preferred:
                btn.setProperty("preferred", "true")
            btn.clicked.connect(lambda _checked=False, v=value: self._accept_value(v))
            root.addWidget(btn)

        actions = QHBoxLayout()
        actions.addStretch(1)
        cancel = QPushButton("取消")
        cancel.setObjectName("pickerCancel")
        cancel.clicked.connect(self.reject)
        actions.addWidget(cancel)
        root.addLayout(actions)

        QShortcut(QKeySequence(Qt.Key.Key_Escape), self, activated=self.reject)
        self._center_on_cursor_screen()

    def _center_on_cursor_screen(self) -> None:
        screen = QGuiApplication.screenAt(self.cursor().pos())
        if screen is None:
            screen = QGuiApplication.primaryScreen()
        if screen is None:
            return
        geo = screen.availableGeometry()
        self.adjustSize()
        size = self.sizeHint()
        self.move(
            geo.center().x() - size.width() // 2,
            geo.center().y() - size.height() // 2,
        )

    def _accept_value(self, value: str) -> None:
        self._selected = normalize_capture_screen(value)
        self.accept()

    def selected_value(self) -> str | None:
        return self._selected
