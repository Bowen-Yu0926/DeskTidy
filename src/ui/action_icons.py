"""Small inline action icons for settings tables."""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor, QIcon, QPainter, QPen, QPixmap


def _icon_pixmap(kind: str, size: int, *, danger: bool = False) -> QPixmap:
    pm = QPixmap(size, size)
    pm.fill(Qt.GlobalColor.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)

    if danger:
        color = QColor("#EF4444")
    else:
        color = QColor("#64748B")

    if kind == "eye":
        p.setPen(QPen(color, 1.6))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(size // 2 - 7, size // 2 - 5, 14, 10)
        p.setBrush(color)
        p.drawEllipse(size // 2 - 2, size // 2 - 2, 4, 4)
    elif kind == "eye_off":
        p.setPen(QPen(color, 1.6))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(size // 2 - 7, size // 2 - 5, 14, 10)
        p.setPen(QPen(color, 1.8))
        p.drawLine(size // 2 - 8, size // 2 + 6, size // 2 + 8, size // 2 - 6)
    elif kind == "trash":
        p.setPen(QPen(color, 1.5))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawLine(size // 2 - 6, size // 2 - 4, size // 2 + 6, size // 2 - 4)
        p.drawRect(size // 2 - 5, size // 2 - 4, 10, 9)
        p.drawLine(size // 2 - 3, size // 2 - 7, size // 2 + 3, size // 2 - 7)
        p.drawLine(size // 2 - 1, size // 2 - 1, size // 2 - 1, size // 2 + 3)
        p.drawLine(size // 2 + 1, size // 2 - 1, size // 2 + 1, size // 2 + 3)
    elif kind == "edit":
        # Pencil: tip bottom-left, body diagonal, eraser top-right.
        p.setPen(QPen(color, 1.6, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawLine(size // 2 - 6, size // 2 + 5, size // 2 + 5, size // 2 - 6)
        p.drawLine(size // 2 + 5, size // 2 - 6, size // 2 + 7, size // 2 - 4)
        p.drawLine(size // 2 + 7, size // 2 - 4, size // 2 - 4, size // 2 + 7)
        p.drawLine(size // 2 - 6, size // 2 + 5, size // 2 - 7, size // 2 + 7)
        p.drawLine(size // 2 - 7, size // 2 + 7, size // 2 - 4, size // 2 + 7)
    elif kind in ("plus", "minus"):
        p.setPen(QPen(color, 2.0, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        mid = size // 2
        half = 5
        p.drawLine(mid - half, mid, mid + half, mid)
        if kind == "plus":
            p.drawLine(mid, mid - half, mid, mid + half)
    elif kind in ("chevron_right", "chevron_down"):
        p.setPen(QPen(color, 1.8, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        mid = size // 2
        if kind == "chevron_right":
            p.drawLine(mid - 2, mid - 5, mid + 3, mid)
            p.drawLine(mid + 3, mid, mid - 2, mid + 5)
        else:
            p.drawLine(mid - 5, mid - 2, mid, mid + 3)
            p.drawLine(mid, mid + 3, mid + 5, mid - 2)
    elif kind == "lock":
        p.setPen(QPen(color, 1.5))
        p.setBrush(Qt.BrushStyle.NoBrush)
        body_top = size // 2 - 1
        p.drawRoundedRect(size // 2 - 5, body_top, 10, 8, 1.5, 1.5)
        p.drawArc(size // 2 - 3, size // 2 - 7, 6, 8, 0, 180 * 16)
    p.end()
    return pm


def make_action_icon(kind: str, *, danger: bool = False, size: int = 18) -> QIcon:
    icon = QIcon()
    normal = _icon_pixmap(kind, size, danger=danger)
    active = _icon_pixmap(kind, size, danger=True if kind == "trash" or danger else False)
    if kind in ("eye", "eye_off", "plus", "minus", "chevron_right", "chevron_down", "edit") and not danger:
        active = _icon_pixmap(kind, size, danger=False)
        p = QPainter(active)
        p.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceAtop)
        p.fillRect(active.rect(), QColor("#2563EB"))
        p.end()
    icon.addPixmap(normal, QIcon.Mode.Normal)
    icon.addPixmap(active, QIcon.Mode.Active)
    icon.addPixmap(active, QIcon.Mode.Selected)
    return icon
