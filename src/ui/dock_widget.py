"""Quick launch dock widget."""

from __future__ import annotations

from pathlib import Path

from PyQt6.QtCore import Qt, QSize, QFileInfo
from PyQt6.QtWidgets import (
    QFileIconProvider,
    QHBoxLayout,
    QPushButton,
    QWidget,
)

from src.settings import expand_path
from src.ui.screen_snap import work_screen
from src.win_shell import configure_desktop_overlay, open_path


class DockWidget(QWidget):
    """A desktop dock bar for quick-launching pinned apps."""

    def __init__(self, config: dict, parent: QWidget | None = None):
        super().__init__(parent)
        self.config = config
        self._icon_provider = QFileIconProvider()

        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.Window
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)

        self._layout = QHBoxLayout(self)
        self._layout.setContentsMargins(10, 6, 10, 6)
        self._layout.setSpacing(8)
        self._rebuild()

    def _rebuild(self) -> None:
        while self._layout.count():
            item = self._layout.takeAt(0)
            if item.widget():
                item.widget().deleteLater()

        opacity = self.config.get("opacity", 0.85)
        bg = self.config.get("background", "#161b22")
        self.setStyleSheet(
            f"QWidget {{ background-color: rgba(22,27,34,{int(opacity*255)}); "
            f"border: 1px solid rgba(88,166,255,100); border-radius: 16px; }}"
            f"QPushButton#dockBtn {{ background: transparent; border: none; border-radius: 8px; padding: 4px; }}"
            f"QPushButton#dockBtn:hover {{ background-color: rgba(88,166,255,60); }}"
        )

        for item in self.config.get("items", []):
            path = expand_path(item.get("path", ""))
            if not path.exists():
                continue
            btn = QPushButton()
            btn.setObjectName("dockBtn")
            btn.setFixedSize(44, 44)
            btn.setIcon(self._icon_provider.icon(QFileInfo(str(path))))
            btn.setIconSize(QSize(32, 32))
            btn.setToolTip(item.get("name") or path.name)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.clicked.connect(lambda checked, p=path: open_path(p))
            self._layout.addWidget(btn)

    def update_config(self, config: dict) -> None:
        self.config = config
        self._rebuild()
        self._position()

    def _position(self) -> None:
        screen = work_screen()
        if not screen:
            return
        geo = screen.availableGeometry()
        self.adjustSize()
        position = self.config.get("position", "bottom")
        if position == "left":
            x = geo.x() + 12
            y = geo.y() + (geo.height() - self.height()) // 2
        elif position == "right":
            x = geo.x() + geo.width() - self.width() - 12
            y = geo.y() + (geo.height() - self.height()) // 2
        else:
            x = geo.x() + (geo.width() - self.width()) // 2
            y = geo.y() + geo.height() - self.height() - 48
        self.move(x, y)

    def showEvent(self, event) -> None:
        self._position()
        super().showEvent(event)
        configure_desktop_overlay(self, peek=False)

    def set_peek_mode(self, peek: bool) -> None:
        from src.win_shell import apply_overlay_stack_mode

        apply_overlay_stack_mode(self, desktop_layer=not peek, peek=peek)
