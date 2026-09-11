"""On-screen recording indicator: red border strips, REC chrome, stop button."""

from __future__ import annotations

from typing import Callable

from PyQt6.QtCore import QRect, Qt, QTimer
from PyQt6.QtGui import QColor, QGuiApplication, QPainter
from PyQt6.QtWidgets import QHBoxLayout, QLabel, QPushButton, QWidget

from src.ui.screen_snap import work_screen

_BORDER_PX = 4
_CHROME_MARGIN = 16
_CHROME_W = 248
_CHROME_H = 52
_BORDER_COLOR = QColor(239, 68, 68, 230)


def _tool_flags() -> Qt.WindowType:
    return (
        Qt.WindowType.FramelessWindowHint
        | Qt.WindowType.WindowStaysOnTopHint
        | Qt.WindowType.Tool
        | Qt.WindowType.WindowDoesNotAcceptFocus
    )


class _BorderStrip(QWidget):
    """Thin edge strip — avoids one huge translucent fullscreen surface."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowFlags(_tool_flags())
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)

    def paintEvent(self, event) -> None:  # noqa: N802
        del event
        p = QPainter(self)
        p.fillRect(self.rect(), _BORDER_COLOR)


class _RecChrome(QWidget):
    def __init__(self, on_stop: Callable[[], None], parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._on_stop = on_stop
        self._pulse_on = True
        self._elapsed_s = 0
        self._stopping = False

        self.setWindowFlags(_tool_flags())
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setFixedSize(_CHROME_W, _CHROME_H)
        self.setObjectName("recChrome")
        self.setStyleSheet(
            """
            QWidget#recChrome {
                background: rgba(24, 10, 14, 240);
                border: 1px solid rgba(248, 113, 113, 230);
                border-radius: 12px;
            }
            QLabel#recBadge {
                color: #FFFFFF;
                background: #DC2626;
                border-radius: 5px;
                padding: 2px 7px;
                font-size: 11px;
                font-weight: 800;
                letter-spacing: 1px;
            }
            QLabel#recDot {
                color: #EF4444;
                font-size: 15px;
                font-weight: 700;
                min-width: 12px;
            }
            QLabel#recTimer {
                color: #FECACA;
                font-size: 13px;
                font-family: Consolas, "Cascadia Mono", monospace;
                font-weight: 700;
                min-width: 48px;
            }
            QPushButton#recStopBtn {
                background: #DC2626;
                color: #FFFFFF;
                border: 1px solid #FCA5A5;
                border-radius: 8px;
                padding: 6px 12px;
                font-size: 12px;
                font-weight: 700;
            }
            QPushButton#recStopBtn:hover { background: #EF4444; }
            QPushButton#recStopBtn:pressed { background: #B91C1C; }
            QPushButton#recStopBtn:disabled {
                background: #7F1D1D;
                color: #FECACA;
                border-color: #7F1D1D;
            }
            """
        )

        row = QHBoxLayout(self)
        row.setContentsMargins(12, 8, 10, 8)
        row.setSpacing(8)

        self._badge = QLabel("REC")
        self._badge.setObjectName("recBadge")
        self._badge.setAlignment(Qt.AlignmentFlag.AlignCenter)
        row.addWidget(self._badge, 0, Qt.AlignmentFlag.AlignVCenter)

        self._dot = QLabel("●")
        self._dot.setObjectName("recDot")
        self._dot.setAlignment(Qt.AlignmentFlag.AlignCenter)
        row.addWidget(self._dot, 0, Qt.AlignmentFlag.AlignVCenter)

        self.timer_lbl = QLabel("00:00")
        self.timer_lbl.setObjectName("recTimer")
        self.timer_lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)
        row.addWidget(self.timer_lbl, 0, Qt.AlignmentFlag.AlignVCenter)
        row.addStretch(1)

        self.stop_btn = QPushButton("结束录制")
        self.stop_btn.setObjectName("recStopBtn")
        self.stop_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.stop_btn.clicked.connect(self._handle_stop)
        row.addWidget(self.stop_btn, 0, Qt.AlignmentFlag.AlignVCenter)

        self._pulse_timer = QTimer(self)
        self._pulse_timer.setInterval(480)
        self._pulse_timer.timeout.connect(self._tick_pulse)

        self._elapsed_timer = QTimer(self)
        self._elapsed_timer.setInterval(1000)
        self._elapsed_timer.timeout.connect(self._tick_elapsed)

        # Short preset styles — swap without unpolish/polish each tick.
        self._badge_on_css = (
            "color:#FFFFFF;background:#DC2626;border-radius:5px;"
            "padding:2px 7px;font-size:11px;font-weight:800;letter-spacing:1px;"
        )
        self._badge_off_css = (
            "color:#FECACA;background:#7F1D1D;border-radius:5px;"
            "padding:2px 7px;font-size:11px;font-weight:800;letter-spacing:1px;"
        )
        self._dot_on_css = "color:#EF4444;font-size:15px;font-weight:700;min-width:12px;"
        self._dot_off_css = "color:#7F1D1D;font-size:15px;font-weight:700;min-width:12px;"

    def showEvent(self, event) -> None:  # noqa: N802
        super().showEvent(event)
        self._stopping = False
        self._elapsed_s = 0
        self._pulse_on = True
        self.timer_lbl.setText("00:00")
        self._apply_pulse_style(True)
        self.stop_btn.setEnabled(True)
        self.stop_btn.setText("结束录制")
        self._pulse_timer.start()
        self._elapsed_timer.start()
        self.raise_()

    def hideEvent(self, event) -> None:  # noqa: N802
        self._pulse_timer.stop()
        self._elapsed_timer.stop()
        super().hideEvent(event)

    def _apply_pulse_style(self, on: bool) -> None:
        """Toggle pulse via short preset stylesheets (no style polish churn)."""
        self._badge.setStyleSheet(self._badge_on_css if on else self._badge_off_css)
        self._dot.setStyleSheet(self._dot_on_css if on else self._dot_off_css)

    def _tick_pulse(self) -> None:
        self._pulse_on = not self._pulse_on
        self._apply_pulse_style(self._pulse_on)

    def _tick_elapsed(self) -> None:
        self._elapsed_s += 1
        mins, secs = divmod(self._elapsed_s, 60)
        hours, mins = divmod(mins, 60)
        if hours:
            self.timer_lbl.setText(f"{hours:d}:{mins:02d}:{secs:02d}")
        else:
            self.timer_lbl.setText(f"{mins:02d}:{secs:02d}")

    def _handle_stop(self) -> None:
        if self._stopping:
            return
        self._stopping = True
        self.stop_btn.setEnabled(False)
        self.stop_btn.setText("结束中…")
        cb = self._on_stop
        if callable(cb):
            QTimer.singleShot(0, cb)


class RecordingIndicator:
    """Owns border strips on the capture target + chrome panel."""

    def __init__(
        self,
        on_stop: Callable[[], None],
        *,
        capture_region: dict | None = None,
    ) -> None:
        self._on_stop = on_stop
        self._capture_region = capture_region if isinstance(capture_region, dict) else None
        self._strips: list[_BorderStrip] = []
        self._chrome = _RecChrome(on_stop)
        self._build_strips()
        self._place_chrome()

    def _place_chrome(self) -> None:
        """Pin REC panel to the top-right of the capture target (not cursor screen)."""
        g = self._capture_target_rect()
        if g is not None:
            self._chrome.move(
                int(g.x() + g.width() - _CHROME_MARGIN - _CHROME_W),
                int(g.y() + _CHROME_MARGIN),
            )
            return
        screen = work_screen()
        if screen is None:
            return
        area = screen.availableGeometry()
        self._chrome.move(
            int(area.x() + area.width() - _CHROME_MARGIN - _CHROME_W),
            int(area.y() + _CHROME_MARGIN),
        )

    def _capture_target_rect(self) -> QRect | None:
        """Qt *logical* geometry of the capture target (for chrome / border strips).

        ``capture_region`` from the recorder is Win32/gdigrab *physical* pixels —
        never feed that straight into ``QWidget.move`` on HiDPI laptops.
        """
        from src.screen_record_manager import qt_geometry_for_capture_region

        return qt_geometry_for_capture_region(self._capture_region)

    def _target_geometries(self) -> list[QRect]:
        target = self._capture_target_rect()
        if target is not None and target.width() > 0 and target.height() > 0:
            return [target]
        return [screen.geometry() for screen in QGuiApplication.screens()]

    def _build_strips(self) -> None:
        self._clear_strips()
        b = _BORDER_PX
        for g in self._target_geometries():
            if g.width() <= 0 or g.height() <= 0:
                continue
            specs = (
                QRect(g.x(), g.y(), g.width(), b),
                QRect(g.x(), g.y() + g.height() - b, g.width(), b),
                QRect(g.x(), g.y(), b, g.height()),
                QRect(g.x() + g.width() - b, g.y(), b, g.height()),
            )
            for rect in specs:
                strip = _BorderStrip()
                strip.setGeometry(rect)
                self._strips.append(strip)

    def _clear_strips(self) -> None:
        for strip in self._strips:
            try:
                strip.hide()
                strip.close()
                strip.deleteLater()
            except Exception:
                pass
        self._strips.clear()

    def show(self) -> None:
        # Chrome first — border strips deferred so encode start + DWM work do not pile up.
        self._place_chrome()
        self._chrome.show()
        self._chrome.raise_()
        self._ensure_chrome_topmost()
        QTimer.singleShot(200, self._show_border_strips)

    def _ensure_chrome_topmost(self) -> None:
        """Keep REC above game overlays / other TOPMOST tools on gaming laptops."""
        try:
            from src.screenshot_manager import force_widget_topmost

            force_widget_topmost(self._chrome)
        except Exception:
            try:
                self._chrome.raise_()
            except Exception:
                pass

    def _show_border_strips(self) -> None:
        if not self._chrome.isVisible():
            return
        if not self._strips:
            self._build_strips()
        for strip in self._strips:
            strip.show()
            strip.raise_()
        self._chrome.raise_()
        self._ensure_chrome_topmost()

    def hide(self) -> None:
        self._chrome.hide()
        for strip in self._strips:
            strip.hide()

    def close(self) -> None:
        self.hide()
        try:
            self._chrome.close()
            self._chrome.deleteLater()
        except Exception:
            pass
        self._clear_strips()

    def isVisible(self) -> bool:  # noqa: N802
        return self._chrome.isVisible()

    @property
    def _timer_lbl(self) -> QLabel:
        return self._chrome.timer_lbl

    @property
    def _stop_btn(self) -> QPushButton:
        return self._chrome.stop_btn

    @property
    def _stopping(self) -> bool:
        return self._chrome._stopping

    def _handle_stop(self) -> None:
        self._chrome._handle_stop()


_active_indicator: RecordingIndicator | None = None


def show_recording_indicator(
    on_stop: Callable[[], None],
    *,
    capture_region: dict | None = None,
) -> RecordingIndicator:
    global _active_indicator
    hide_recording_indicator()
    tip = RecordingIndicator(on_stop, capture_region=capture_region)
    _active_indicator = tip
    tip.show()
    return tip


def hide_recording_indicator() -> None:
    global _active_indicator
    tip = _active_indicator
    _active_indicator = None
    if tip is None:
        return
    try:
        tip.close()
    except Exception:
        pass


def is_recording_indicator_visible() -> bool:
    tip = _active_indicator
    return tip is not None and tip.isVisible()
