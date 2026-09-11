"""Stroke icons for the admin sidebar navigation."""

from __future__ import annotations

import math

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor, QIcon, QPainter, QPen, QPixmap

_NAV_ICON_KIND: dict[str, str] = {
    "files": "sort",
    "fences": "zones",
    "snapshot": "snapshot",
    "extensions": "extensions",
    "pet": "pet",
    "settings": "settings",
    "help": "help",
}

_MUTED = "#8B97A8"
_BRIGHT = "#E8EDF4"


def _pen(color: QColor, width: float = 1.55) -> QPen:
    return QPen(color, width, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)


def _icon_pixmap(kind: str, size: int, color: QColor) -> QPixmap:
    pm = QPixmap(size, size)
    pm.fill(Qt.GlobalColor.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    pen = _pen(color)
    p.setPen(pen)
    p.setBrush(Qt.BrushStyle.NoBrush)
    cx = size // 2
    cy = size // 2

    if kind == "sort":
        widths = (12, 9, 6)
        y0 = cy - 5
        for i, w in enumerate(widths):
            x = cx - w // 2
            p.drawLine(x, y0 + i * 5, x + w, y0 + i * 5)
        p.drawLine(cx + 4, y0 + 10, cx + 4, y0 + 14)
        p.drawLine(cx + 2, y0 + 12, cx + 4, y0 + 14)
        p.drawLine(cx + 6, y0 + 12, cx + 4, y0 + 14)
    elif kind == "zones":
        layers = ((4, 9, 12, 8), (3, 6, 12, 8), (2, 3, 12, 8))
        for x, y, w, h in layers:
            p.drawRoundedRect(x, y, w, h, 2.5, 2.5)
        p.setBrush(color)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(cx + 3, cy - 5, 3, 3)
    elif kind == "snapshot":
        p.drawRoundedRect(3, 5, size - 6, size - 8, 2.5, 2.5)
        p.drawLine(3, 8, size - 3, 8)
        p.setBrush(color)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(cx - 2, cy + 1, 4, 4)
        p.setPen(pen)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawLine(size - 7, 6, size - 5, 6)
    elif kind == "extensions":
        p.drawRoundedRect(3, 4, 8, 8, 2, 2)
        p.drawRoundedRect(size - 11, 4, 8, 8, 2, 2)
        p.drawRoundedRect(3, size - 12, 8, 8, 2, 2)
        p.setPen(_pen(color, 1.8))
        p.drawLine(cx - 3, cy, cx + 3, cy)
        p.drawLine(cx, cy - 3, cx, cy + 3)
    elif kind == "pet":
        p.drawEllipse(cx - 6, cy - 1, 12, 9)
        p.drawEllipse(cx - 6, cy - 8, 4, 6)
        p.drawEllipse(cx + 2, cy - 8, 4, 6)
        p.setBrush(color)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(cx - 3, cy + 2, 2, 2)
        p.drawEllipse(cx + 1, cy + 2, 2, 2)
        p.setPen(_pen(color, 1.3))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawArc(cx - 3, cy + 2, 6, 4, 0, -180 * 16)
    elif kind == "settings":
        p.drawEllipse(cx - 5, cy - 5, 10, 10)
        for angle in (0, 60, 120, 180, 240, 300):
            rad = math.radians(angle)
            x1 = cx + int(round(math.cos(rad) * 5))
            y1 = cy + int(round(math.sin(rad) * 5))
            x2 = cx + int(round(math.cos(rad) * 8))
            y2 = cy + int(round(math.sin(rad) * 8))
            p.drawLine(x1, y1, x2, y2)
        p.setBrush(color)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(cx - 2, cy - 2, 4, 4)
    elif kind == "help":
        p.drawEllipse(3, 3, size - 6, size - 6)
        p.setPen(_pen(color, 1.7))
        p.drawArc(cx - 3, cy - 7, 6, 6, 0, -180 * 16)
        p.drawLine(cx, cy - 1, cx, cy + 1)
        p.setBrush(color)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(cx - 1, cy + 3, 2, 2)

    p.end()
    return pm


def nav_icon_for(
    page_id: str,
    *,
    accent: str | None = None,
    muted: str | None = None,
    bright: str | None = None,
    size: int = 20,
) -> QIcon:
    kind = _NAV_ICON_KIND.get(page_id, "help")
    accent_c = QColor(accent or "#3B82F6")
    if not accent_c.isValid():
        accent_c = QColor("#3B82F6")
    muted_c = QColor(muted or _MUTED)
    if not muted_c.isValid():
        muted_c = QColor(_MUTED)
    bright_c = QColor(bright or _BRIGHT)
    if not bright_c.isValid():
        bright_c = QColor(_BRIGHT)
    icon = QIcon()
    icon.addPixmap(_icon_pixmap(kind, size, muted_c), QIcon.Mode.Normal, QIcon.State.Off)
    icon.addPixmap(_icon_pixmap(kind, size, accent_c), QIcon.Mode.Selected, QIcon.State.Off)
    icon.addPixmap(_icon_pixmap(kind, size, bright_c), QIcon.Mode.Active, QIcon.State.Off)
    return icon


def refresh_nav_item_icon(
    item,
    page_id: str,
    *,
    accent: str | None = None,
    muted: str | None = None,
    bright: str | None = None,
    size: int = 20,
) -> None:
    from PyQt6.QtWidgets import QListWidgetItem

    if not isinstance(item, QListWidgetItem):
        return
    item.setIcon(
        nav_icon_for(page_id, accent=accent, muted=muted, bright=bright, size=size)
    )
