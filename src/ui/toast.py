"""Brief always-on-top toast for user-visible tips (e.g. recording started)."""

from __future__ import annotations

from PyQt6.QtCore import (
    QEasingCurve,
    QPoint,
    QPropertyAnimation,
    QParallelAnimationGroup,
    QRect,
    Qt,
    QTimer,
)
from PyQt6.QtWidgets import QLabel, QVBoxLayout, QWidget

from src.ui.screen_snap import work_screen

_theme_colors_cache_key: object | None = None
_theme_colors_cache: dict[str, str] | None = None

_ToastLevel = str  # "info" | "success" | "warn"


def invalidate_toast_theme_cache() -> None:
    global _theme_colors_cache_key, _theme_colors_cache
    _theme_colors_cache_key = None
    _theme_colors_cache = None


def _theme_colors() -> dict[str, str]:
    global _theme_colors_cache_key, _theme_colors_cache
    try:
        from src.settings import load_settings
        from src.ui.styles import get_theme_palette, normalize_theme

        key = normalize_theme(load_settings().get("theme"))
        if _theme_colors_cache is not None and key == _theme_colors_cache_key:
            return _theme_colors_cache
        _theme_colors_cache = get_theme_palette(key)
        _theme_colors_cache_key = key
        return _theme_colors_cache
    except Exception:
        return {
            "card": "#FFFFFF",
            "border": "#D5DCE7",
            "text": "#1F2937",
            "text_muted": "#6B7280",
            "accent": "#2563EB",
            "accent_soft": "#DBEAFE",
            "primary": "#16A34A",
            "danger": "#DC2626",
        }


def _accent_for_level(level: str, palette: dict[str, str]) -> str:
    if level == "success":
        return palette.get("primary") or "#16A34A"
    if level == "warn":
        return palette.get("danger") or "#DC2626"
    return palette.get("accent") or "#2563EB"


_active_toast: _ToastTip | None = None


def show_toast(
    title: str,
    body: str = "",
    *,
    msec: int = 2800,
    anchor: QRect | QPoint | None = None,
    level: _ToastLevel = "info",
) -> None:
    """Show a non-modal topmost tip that auto-closes.

    Only one toast is visible: a new tip dismisses any previous tip so
    「正在移动」cannot linger under「已移入」after a short background move.

    *anchor*: place on that screen/rect (e.g. recording target). Default = cursor work screen.
    *level*: ``info`` / ``success`` / ``warn`` — left accent bar color.
    """
    global _active_toast
    prev = _active_toast
    _active_toast = None
    if prev is not None:
        try:
            prev.close()
        except RuntimeError:
            pass
    tip = _ToastTip(title, body, msec=msec, anchor=anchor, level=level)
    _active_toast = tip
    tip.destroyed.connect(lambda *_: _clear_active_toast(tip))
    tip.show()
    tip.raise_()


def _clear_active_toast(tip: _ToastTip) -> None:
    global _active_toast
    if _active_toast is tip:
        _active_toast = None


def dismiss_toast() -> None:
    """Close the current toast immediately (e.g. background op finished)."""
    global _active_toast
    tip = _active_toast
    _active_toast = None
    if tip is None:
        return
    try:
        tip.close()
    except RuntimeError:
        pass


class _ToastTip(QWidget):
    def __init__(
        self,
        title: str,
        body: str,
        *,
        msec: int,
        anchor: QRect | QPoint | None = None,
        level: str = "info",
    ) -> None:
        super().__init__(None)
        self._anchor = anchor
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        # Opaque child card — avoid translucent tool-window painting crashes on Win.
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)

        p = _theme_colors()
        bar = _accent_for_level(level, p)
        # Prefer a crisp theme border over Win Tool-window drop shadows (paint cost).
        self.setStyleSheet(
            f"QWidget#toastCard {{"
            f" background: {p['card']};"
            f" border: 1px solid {p['border']};"
            f" border-left: 3px solid {bar};"
            f" border-radius: 12px;"
            f"}}"
            f"QLabel#toastTitle {{ color: {p['text']}; font-size: 14px; font-weight: 700; }}"
            f"QLabel#toastBody {{ color: {p['text_muted']}; font-size: 12px; }}"
        )

        card = QWidget(self)
        card.setObjectName("toastCard")
        layout = QVBoxLayout(card)
        layout.setContentsMargins(16, 12, 16, 12)
        layout.setSpacing(4)

        title_lbl = QLabel(title)
        title_lbl.setObjectName("toastTitle")
        title_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        title_lbl.setMaximumWidth(400)
        layout.addWidget(title_lbl)

        if body.strip():
            body_lbl = QLabel(body.strip())
            body_lbl.setObjectName("toastBody")
            body_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
            body_lbl.setWordWrap(True)
            body_lbl.setMaximumWidth(400)
            layout.addWidget(body_lbl)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(card)

        self.adjustSize()
        self.setWindowOpacity(0.0)
        rest_y = self._place_top_center()

        fade = QPropertyAnimation(self, b"windowOpacity", self)
        fade.setDuration(180)
        fade.setStartValue(0.0)
        fade.setEndValue(1.0)
        fade.setEasingCurve(QEasingCurve.Type.OutCubic)

        slide = QPropertyAnimation(self, b"pos", self)
        slide.setDuration(180)
        slide.setStartValue(QPoint(self.x(), rest_y - 10))
        slide.setEndValue(QPoint(self.x(), rest_y))
        slide.setEasingCurve(QEasingCurve.Type.OutCubic)

        group = QParallelAnimationGroup(self)
        group.addAnimation(fade)
        group.addAnimation(slide)
        group.start()
        self._enter = group

        hold = max(900, int(msec))
        QTimer.singleShot(hold, self._fade_out_and_close)

    def _fade_out_and_close(self) -> None:
        fade = QPropertyAnimation(self, b"windowOpacity", self)
        fade.setDuration(160)
        fade.setStartValue(self.windowOpacity())
        fade.setEndValue(0.0)
        fade.setEasingCurve(QEasingCurve.Type.InCubic)
        fade.finished.connect(self.close)
        fade.start()
        self._fade_out = fade

    def _place_top_center(self) -> int:
        """Top-center of *anchor* screen/rect, else cursor work screen."""
        from PyQt6.QtGui import QGuiApplication

        geo: QRect | None = None
        anchor = self._anchor
        if isinstance(anchor, QRect) and anchor.width() > 0 and anchor.height() > 0:
            geo = QRect(anchor)
        elif isinstance(anchor, QPoint):
            screen = work_screen(anchor)
            if screen is not None:
                geo = screen.availableGeometry()
        if geo is None:
            screen = work_screen()
            if screen is not None:
                geo = screen.availableGeometry()
        if geo is None:
            primary = QGuiApplication.primaryScreen()
            if primary is not None:
                geo = primary.availableGeometry()
        if geo is None:
            return 0
        self.adjustSize()
        x = geo.x() + (geo.width() - self.width()) // 2
        y = geo.y() + max(40, geo.height() // 14)
        self.move(x, y)
        return y
