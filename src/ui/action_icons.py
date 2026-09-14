"""Small inline action icons for settings tables."""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor, QIcon, QPainter, QPen, QPixmap


def _icon_pixmap(
    kind: str,
    size: int,
    *,
    danger: bool = False,
    color: str | None = None,
) -> QPixmap:
    pm = QPixmap(size, size)
    pm.fill(Qt.GlobalColor.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)

    if color:
        ink = QColor(color)
    elif danger:
        ink = QColor("#EF4444")
    else:
        ink = QColor("#64748B")

    if kind == "eye":
        p.setPen(QPen(ink, 1.6))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(size // 2 - 7, size // 2 - 5, 14, 10)
        p.setBrush(ink)
        p.drawEllipse(size // 2 - 2, size // 2 - 2, 4, 4)
    elif kind == "eye_off":
        p.setPen(QPen(ink, 1.6))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(size // 2 - 7, size // 2 - 5, 14, 10)
        p.setPen(QPen(ink, 1.8))
        p.drawLine(size // 2 - 8, size // 2 + 6, size // 2 + 8, size // 2 - 6)
    elif kind == "trash":
        p.setPen(QPen(ink, 1.5))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawLine(size // 2 - 6, size // 2 - 4, size // 2 + 6, size // 2 - 4)
        p.drawRect(size // 2 - 5, size // 2 - 4, 10, 9)
        p.drawLine(size // 2 - 3, size // 2 - 7, size // 2 + 3, size // 2 - 7)
        p.drawLine(size // 2 - 1, size // 2 - 1, size // 2 - 1, size // 2 + 3)
        p.drawLine(size // 2 + 1, size // 2 - 1, size // 2 + 1, size // 2 + 3)
    elif kind == "edit":
        # Pencil: tip bottom-left, body diagonal, eraser top-right.
        p.setPen(QPen(ink, 1.6, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawLine(size // 2 - 6, size // 2 + 5, size // 2 + 5, size // 2 - 6)
        p.drawLine(size // 2 + 5, size // 2 - 6, size // 2 + 7, size // 2 - 4)
        p.drawLine(size // 2 + 7, size // 2 - 4, size // 2 - 4, size // 2 + 7)
        p.drawLine(size // 2 - 6, size // 2 + 5, size // 2 - 7, size // 2 + 7)
        p.drawLine(size // 2 - 7, size // 2 + 7, size // 2 - 4, size // 2 + 7)
    elif kind in ("plus", "minus"):
        p.setPen(QPen(ink, 2.0, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        mid = size // 2
        half = 5
        p.drawLine(mid - half, mid, mid + half, mid)
        if kind == "plus":
            p.drawLine(mid, mid - half, mid, mid + half)
    elif kind in ("chevron_right", "chevron_down"):
        p.setPen(QPen(ink, 1.8, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        mid = size // 2
        if kind == "chevron_right":
            p.drawLine(mid - 2, mid - 5, mid + 3, mid)
            p.drawLine(mid + 3, mid, mid - 2, mid + 5)
        else:
            p.drawLine(mid - 5, mid - 2, mid, mid + 3)
            p.drawLine(mid, mid + 3, mid + 5, mid - 2)
    elif kind in ("lock", "unlock"):
        # Small padlock — closed shackle when locked, open/shifted when unlocked.
        stroke = max(1.35, size * 0.09)
        pen = QPen(
            ink,
            stroke,
            Qt.PenStyle.SolidLine,
            Qt.PenCapStyle.RoundCap,
            Qt.PenJoinStyle.RoundJoin,
        )
        p.setPen(pen)
        p.setBrush(Qt.BrushStyle.NoBrush)
        mid = size / 2.0
        body_w = size * 0.58
        body_h = size * 0.42
        body_x = mid - body_w / 2.0
        body_y = mid - body_h * 0.05
        p.drawRoundedRect(
            int(round(body_x)),
            int(round(body_y)),
            int(round(body_w)),
            int(round(body_h)),
            1.8,
            1.8,
        )
        # Keyhole
        p.setBrush(ink)
        p.setPen(Qt.PenStyle.NoPen)
        hole = max(1.6, size * 0.12)
        p.drawEllipse(
            int(round(mid - hole / 2.0)),
            int(round(body_y + body_h * 0.28)),
            int(round(hole)),
            int(round(hole)),
        )
        p.setPen(pen)
        p.setBrush(Qt.BrushStyle.NoBrush)
        shackle_w = size * 0.36
        shackle_h = size * 0.42
        shackle_y = body_y - shackle_h * 0.72
        if kind == "lock":
            shackle_x = mid - shackle_w / 2.0
            p.drawArc(
                int(round(shackle_x)),
                int(round(shackle_y)),
                int(round(shackle_w)),
                int(round(shackle_h)),
                0,
                180 * 16,
            )
        else:
            # Open lock: shackle shifted right with a left-side gap.
            shackle_x = mid - shackle_w * 0.15
            p.drawArc(
                int(round(shackle_x)),
                int(round(shackle_y - size * 0.04)),
                int(round(shackle_w)),
                int(round(shackle_h)),
                -20 * 16,
                200 * 16,
            )
    p.end()
    return pm


def make_action_icon(
    kind: str,
    *,
    danger: bool = False,
    size: int = 18,
    color: str | None = None,
) -> QIcon:
    icon = QIcon()
    normal = _icon_pixmap(kind, size, danger=danger, color=color)
    active = _icon_pixmap(
        kind,
        size,
        danger=True if kind == "trash" or danger else False,
        color=color,
    )
    if (
        kind
        in (
            "eye",
            "eye_off",
            "plus",
            "minus",
            "chevron_right",
            "chevron_down",
            "edit",
            "lock",
            "unlock",
        )
        and not danger
        and not color
    ):
        active = _icon_pixmap(kind, size, danger=False)
        p = QPainter(active)
        p.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceAtop)
        p.fillRect(active.rect(), QColor("#2563EB"))
        p.end()
    icon.addPixmap(normal, QIcon.Mode.Normal)
    icon.addPixmap(active, QIcon.Mode.Active)
    icon.addPixmap(active, QIcon.Mode.Selected)
    return icon


def make_fence_lock_icon(*, locked: bool, color: str, size: int = 14) -> QIcon:
    """Padlock for fence chrome — closed when locked, open when unlocked."""
    return make_action_icon(
        "lock" if locked else "unlock",
        size=size,
        color=color,
    )
