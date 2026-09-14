"""Vertical sidebar navigation."""

from __future__ import annotations

from PyQt6.QtCore import QSize, Qt, pyqtSignal
from PyQt6.QtWidgets import QListWidget, QListWidgetItem

from src.ui.sidebar_nav_icons import nav_icon_for, refresh_nav_item_icon


class SidebarNavWidget(QListWidget):
    page_changed = pyqtSignal(str, str, int)

    def __init__(
        self,
        items: list[tuple[str, str]],
        parent=None,
        *,
        accent: str = "#3B82F6",
        muted: str = "#8B97A8",
        bright: str = "#E8EDF4",
    ):
        super().__init__(parent)
        self.setObjectName("sidebarNav")
        self._items = items
        self._accent = accent
        self._muted = muted
        self._bright = bright
        self.setFrameShape(QListWidget.Shape.NoFrame)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        self.setSpacing(2)
        self.setIconSize(QSize(18, 18))
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)

        for page_id, title in items:
            item = QListWidgetItem(
                nav_icon_for(page_id, accent=accent, muted=muted, bright=bright),
                title,
            )
            item.setData(Qt.ItemDataRole.UserRole, page_id)
            item.setSizeHint(QSize(0, 36))
            self.addItem(item)

        self.currentRowChanged.connect(self._emit_change)
        self.setCurrentRow(0)

    def set_accent(
        self,
        accent: str,
        *,
        muted: str | None = None,
        bright: str | None = None,
    ) -> None:
        self._accent = accent
        if muted is not None:
            self._muted = muted
        if bright is not None:
            self._bright = bright
        for row in range(self.count()):
            item = self.item(row)
            if not item:
                continue
            page_id = item.data(Qt.ItemDataRole.UserRole)
            if page_id:
                refresh_nav_item_icon(
                    item,
                    page_id,
                    accent=self._accent,
                    muted=self._muted,
                    bright=self._bright,
                )

    def set_current(self, page_id: str) -> None:
        for row in range(self.count()):
            item = self.item(row)
            if item and item.data(Qt.ItemDataRole.UserRole) == page_id:
                self.setCurrentRow(row)
                return

    def _emit_change(self, row: int) -> None:
        if row < 0:
            return
        item = self.item(row)
        if not item:
            return
        page_id = item.data(Qt.ItemDataRole.UserRole)
        self.page_changed.emit(page_id, item.text(), row)
