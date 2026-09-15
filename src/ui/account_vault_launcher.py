"""Doubao-style floating launcher for the account vault panel."""

from __future__ import annotations

from PyQt6.QtCore import QPoint, QRectF, Qt, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QMouseEvent, QPainter, QPen
from PyQt6.QtWidgets import QWidget

from src.account_vault import (
    account_vault_launcher_pos,
    set_account_vault_launcher_pos,
)
from src.settings import save_settings
from src.ui.screen_snap import work_screen
from src.ui.styles import get_theme_palette, normalize_theme
from src.win_shell import configure_desktop_overlay

_SIZE = 56
_DRAG_THRESHOLD = 6


class AccountVaultLauncher(QWidget):
    """Small circular float; click toggles vault panel, drag repositions."""

    activated = pyqtSignal()

    def __init__(self, settings: dict | None = None, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings if isinstance(settings, dict) else {}
        self._press_global: QPoint | None = None
        self._drag_offset: QPoint | None = None
        self._dragging = False
        self._hover = False

        self.setObjectName("vaultLauncher")
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowStaysOnTopHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self.setFixedSize(_SIZE, _SIZE)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setToolTip("账号管理")
        self._place()

    def refresh_theme(self) -> None:
        self.update()

    def showEvent(self, event) -> None:  # noqa: N802
        super().showEvent(event)
        configure_desktop_overlay(self, peek=False)

    def enterEvent(self, event) -> None:  # noqa: N802
        self._hover = True
        self.update()
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:  # noqa: N802
        self._hover = False
        self.update()
        super().leaveEvent(event)

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self._press_global = event.globalPosition().toPoint()
            self._drag_offset = self._press_global - self.frameGeometry().topLeft()
            self._dragging = False
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if (
            self._press_global is None
            or self._drag_offset is None
            or not (event.buttons() & Qt.MouseButton.LeftButton)
        ):
            super().mouseMoveEvent(event)
            return
        global_pos = event.globalPosition().toPoint()
        if not self._dragging:
            delta = global_pos - self._press_global
            if abs(delta.x()) < _DRAG_THRESHOLD and abs(delta.y()) < _DRAG_THRESHOLD:
                event.accept()
                return
            self._dragging = True
        top_left = global_pos - self._drag_offset
        self.move(self._clamp_pos(top_left))
        event.accept()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() != Qt.MouseButton.LeftButton:
            super().mouseReleaseEvent(event)
            return
        was_drag = self._dragging
        self._press_global = None
        self._drag_offset = None
        self._dragging = False
        if was_drag:
            self._persist_pos()
        else:
            self.activated.emit()
        event.accept()

    def paintEvent(self, event) -> None:  # noqa: N802
        del event
        theme = normalize_theme(
            self.settings.get("theme") if isinstance(self.settings, dict) else None
        )
        p = get_theme_palette(theme)
        accent = QColor(p.get("accent") or "#3B82F6")
        fill = QColor(accent)
        fill.setAlpha(235 if self._hover else 210)
        rim = QColor(255, 255, 255, 210 if self._hover else 160)

        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        margin = 2.5
        oval = QRectF(margin, margin, _SIZE - margin * 2, _SIZE - margin * 2)
        painter.setPen(QPen(rim, 2.0))
        painter.setBrush(fill)
        painter.drawEllipse(oval)

        painter.setPen(QColor("#FFFFFF"))
        font = QFont()
        font.setBold(True)
        font.setPointSize(13)
        painter.setFont(font)
        painter.drawText(self.rect(), int(Qt.AlignmentFlag.AlignCenter), "账")

    def _place(self) -> None:
        saved = account_vault_launcher_pos(self.settings)
        if saved is not None:
            self.move(self._clamp_pos(QPoint(saved[0], saved[1])))
            return
        self.move(self._default_pos())

    def _default_pos(self) -> QPoint:
        screen = work_screen()
        if screen is None:
            return QPoint(80, 80)
        geo = screen.availableGeometry()
        x = geo.x() + geo.width() - _SIZE - 28
        y = geo.y() + geo.height() - _SIZE - 140
        return self._clamp_pos(QPoint(x, y))

    def _clamp_pos(self, top_left: QPoint) -> QPoint:
        screen = work_screen(top_left)
        if screen is None:
            return top_left
        geo = screen.availableGeometry()
        x = max(geo.x(), min(top_left.x(), geo.x() + geo.width() - self.width()))
        y = max(geo.y(), min(top_left.y(), geo.y() + geo.height() - self.height()))
        return QPoint(x, y)

    def _persist_pos(self) -> None:
        pos = self.pos()
        set_account_vault_launcher_pos(self.settings, pos.x(), pos.y())
        try:
            save_settings(self.settings)
        except OSError:
            pass
