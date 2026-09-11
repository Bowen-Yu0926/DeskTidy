"""Snap windows to screen edges."""

from __future__ import annotations

from PyQt6.QtCore import QPoint, QRect
from PyQt6.QtGui import QCursor, QGuiApplication


def _screen_at(point: QPoint):
    screen = QGuiApplication.screenAt(point)
    if screen:
        return screen
    return QGuiApplication.primaryScreen()


def work_screen(point: QPoint | None = None):
    """Screen the user is working on (cursor / given point), else primary.

    Market-aligned chrome default: one global bar/toast on the active work
    monitor — not blindly ``primaryScreen()`` on multi-monitor setups.
    """
    if point is None:
        try:
            point = QCursor.pos()
        except Exception:
            point = None
    if point is not None:
        screen = QGuiApplication.screenAt(point)
        if screen is not None:
            return screen
    return QGuiApplication.primaryScreen()


def rect_visible_on_any_screen(geo: QRect, *, min_area: int = 24) -> bool:
    """True if *geo* meaningfully overlaps any monitor work area."""
    if geo.isNull() or geo.width() <= 0 or geo.height() <= 0:
        return False
    for screen in QGuiApplication.screens() or []:
        inter = geo.intersected(screen.availableGeometry())
        if inter.width() * inter.height() >= int(min_area):
            return True
    return False


def snap_geometry(geo: QRect, threshold: int = 24) -> QRect:
    """Snap a window rect to the nearest edges of one screen's work area.

    Always clamps to a single monitor. If the rect overlaps multiple screens
    (legacy dual-monitor full-bleed fences), pin to the primary screen.
    Orphan rects (unplugged monitor) clamp onto the work screen under the
    cursor when possible, else primary.
    """
    screens = list(QGuiApplication.screens() or [])
    primary = QGuiApplication.primaryScreen()
    overlaps: list[tuple[int, object]] = []
    for screen in screens:
        area = screen.availableGeometry()
        inter = geo.intersected(area)
        if inter.width() > 0 and inter.height() > 0:
            overlaps.append((inter.width() * inter.height(), screen))

    if len(overlaps) >= 2 and primary:
        screen = primary
    elif overlaps:
        overlaps.sort(key=lambda item: item[0], reverse=True)
        screen = overlaps[0][1]
    else:
        # Off-screen / unplugged: prefer cursor work screen over a stale center.
        screen = work_screen() or _screen_at(geo.center())

    if not screen:
        return geo

    area = screen.availableGeometry()
    x, y, w, h = geo.x(), geo.y(), geo.width(), geo.height()

    if abs(x - area.left()) <= threshold:
        x = area.left()
    elif abs((x + w) - area.right()) <= threshold:
        x = area.right() - w

    # Top snap is tighter — loose top snap + SetParent(0) churn left fences
    # stuck as a full-width icon strip at y=0.
    top_threshold = min(8, threshold)
    if abs(y - area.top()) <= top_threshold:
        y = area.top()
    elif abs((y + h) - area.bottom()) <= threshold:
        y = area.bottom() - h

    max_w = area.width()
    max_h = area.height()
    w = min(w, max_w)
    h = min(h, max_h)
    x = max(area.left(), min(x, area.right() - w))
    y = max(area.top(), min(y, area.bottom() - h))

    return QRect(x, y, w, h)
