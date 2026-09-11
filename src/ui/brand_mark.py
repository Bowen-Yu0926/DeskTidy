"""Sidebar brand mark — stacked desk panes (product signature)."""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor, QIcon, QLinearGradient, QPainter, QPainterPath, QPen, QPixmap
from PyQt6.QtWidgets import QWidget

# Design canvas for the mark (sidebar widget is 40×40).
_MARK_BASE = 40.0
# (x, y, w, h, alpha) on the 40×40 canvas — back → front.
_LAYERS = (
    (6, 10, 28, 22, 70),
    (4, 6, 28, 22, 140),
    (2, 2, 28, 22, 255),
)
_PIP = (22, 8, 5, 5)  # x, y, w, h
_DEFAULT_ACCENT = "#3B82F6"


def paint_brand_mark(
    painter: QPainter,
    *,
    size: int = 40,
    accent: QColor | str | None = None,
) -> None:
    """Paint the product BrandMark into *painter* at ``size``×``size``.

    Shared by the settings sidebar and the system-tray / window icons so every
    surface shows the same stacked-pane glyph.
    """
    size = max(8, int(size))
    if isinstance(accent, str):
        accent_c = QColor(accent)
    elif isinstance(accent, QColor):
        accent_c = QColor(accent)
    else:
        accent_c = QColor(_DEFAULT_ACCENT)
    if not accent_c.isValid():
        accent_c = QColor(_DEFAULT_ACCENT)

    scale = float(size) / _MARK_BASE
    radius = max(1.0, 6.0 * scale)
    # Soft AA only when we have enough pixels for the panes to read.
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, size >= 20)

    for x, y, w, h, alpha in _LAYERS:
        sx = x * scale
        sy = y * scale
        sw = w * scale
        sh = h * scale
        path = QPainterPath()
        path.addRoundedRect(sx, sy, sw, sh, radius, radius)
        c = QColor(accent_c)
        c.setAlpha(int(alpha))
        grad = QLinearGradient(sx, sy, sx + sw, sy + sh)
        top = QColor(c)
        top = top.lighter(118)
        top.setAlpha(int(alpha))
        grad.setColorAt(0.0, top)
        grad.setColorAt(1.0, c)
        painter.fillPath(path, grad)
        pen = QPen(QColor(255, 255, 255, min(180, int(alpha))))
        pen.setWidthF(max(0.8, 1.0 * scale))
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawPath(path)

    px, py, pw, ph = _PIP
    pip = QColor(255, 255, 255, 230)
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(pip)
    painter.drawEllipse(
        int(round(px * scale)),
        int(round(py * scale)),
        max(1, int(round(pw * scale))),
        max(1, int(round(ph * scale))),
    )


def brand_mark_pixmap(size: int, accent: str | QColor | None = None) -> QPixmap:
    """Rasterize the BrandMark onto a transparent ``size``×``size`` pixmap."""
    size = max(8, int(size))
    pm = QPixmap(size, size)
    pm.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pm)
    paint_brand_mark(painter, size=size, accent=accent)
    painter.end()
    pm.setDevicePixelRatio(1.0)
    return pm


def brand_mark_icon(accent: str | QColor | None = None) -> QIcon:
    """Multi-size QIcon matching the sidebar BrandMark (for tray / window)."""
    icon = QIcon()
    for edge in (16, 20, 24, 32, 40, 48, 64):
        pm = brand_mark_pixmap(edge, accent)
        if not pm.isNull():
            icon.addPixmap(pm)
    return icon


class BrandMark(QWidget):
    """Three overlapping rounded panes — reads as tidy desktop layers."""

    def __init__(self, accent: str = _DEFAULT_ACCENT, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._accent = QColor(accent)
        self.setFixedSize(44, 44)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)

    def set_accent(self, hex_color: str) -> None:
        self._accent = QColor(hex_color)
        self.update()

    def paintEvent(self, _event) -> None:  # noqa: N802
        painter = QPainter(self)
        paint_brand_mark(painter, size=max(self.width(), self.height()), accent=self._accent)
        painter.end()
