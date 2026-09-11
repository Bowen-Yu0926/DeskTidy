"""Desktop page indicator: buttons peek from the right; hover slides one out."""

from __future__ import annotations

from PyQt6.QtCore import (
    QAbstractAnimation,
    QEasingCurve,
    QEvent,
    QObject,
    QPoint,
    QPointF,
    QPropertyAnimation,
    QRect,
    QRectF,
    Qt,
    QTimer,
    QVariantAnimation,
    pyqtProperty,
    pyqtSignal,
)
from PyQt6.QtGui import (
    QColor,
    QCursor,
    QFont,
    QLinearGradient,
    QPainter,
    QPainterPath,
    QPen,
)
from PyQt6.QtWidgets import (
    QApplication,
    QMenu,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from src.page_folders import get_page_folder_items
from src.settings import save_settings
from src.ui.screen_snap import rect_visible_on_any_screen, snap_geometry, work_screen
from src.win_shell import configure_desktop_overlay


class _RippleOverlay(QWidget):
    """Soft expanding rings on the hovered button."""

    def __init__(self, parent: QWidget):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self._progress = 0.0
        self._origin = QPoint(0, 0)
        self._anim: QVariantAnimation | None = None
        self.hide()

    def play(self, origin: QPoint, *, accent_hex: str = "#1D4ED8") -> None:
        # Lightweight: ripple disabled (kept as no-op for call-site compat).
        _ = origin, accent_hex
        return

    def _on_progress(self, value) -> None:
        self._progress = float(value)
        self.update()

    def paintEvent(self, _event) -> None:
        if self._progress <= 0.0:
            return
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        cx, cy = self._origin.x(), self._origin.y()
        max_r = max(self.width(), self.height()) * 1.1
        base = getattr(self, "_accent", QColor(59, 130, 246))
        # Single ring — enough motion cue, less hover redraw.
        tint = QColor(base.red(), base.green(), base.blue(), 90)
        t = max(0.0, min(1.0, self._progress))
        radius = max_r * (0.16 + 0.84 * t)
        color = QColor(tint)
        color.setAlpha(int(tint.alpha() * (1.0 - t * 0.92)))
        pen = QPen(color)
        pen.setWidthF(2.0)
        painter.setPen(pen)
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawEllipse(QPoint(cx, cy), int(radius), int(radius))
        painter.end()


class _PeekChip(QPushButton):
    """Right-edge frosted bookmark: glass body, soft active wash + thin rail."""

    def __init__(
        self,
        text: str,
        *,
        role: str,
        object_name: str,
        settings: dict | None = None,
        parent=None,
    ):
        super().__init__(text, parent)
        self._role = role  # page | folder | note | minutes | record
        self._settings = settings if settings is not None else {}
        self._theme_cache_key: object | None = None
        self._theme_cache: dict[str, str] | None = None
        self._path_cache_key: tuple[int, int] | None = None
        self._path_cache: QPainterPath | None = None
        self._path_rect_cache: QRectF | None = None
        self.setObjectName(object_name)
        self.setFlat(True)
        self.setAutoFillBackground(False)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_Hover, True)
        self.setStyleSheet("background:transparent;border:none;padding:0;margin:0;")
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        # Utility chips read quieter; pages carry the dock identity.
        size = 8 if role in ("folder", "note", "minutes", "record") else 9
        font = QFont("Microsoft YaHei UI", size)
        font.setWeight(QFont.Weight.Medium)
        font.setHintingPreference(QFont.HintingPreference.PreferFullHinting)
        font.setStyleStrategy(
            QFont.StyleStrategy.PreferAntialias | QFont.StyleStrategy.PreferQuality
        )
        self.setFont(font)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        # Explicit hover latch — underMouse() stays stale on translucent desktop
        # overlays when Leave is dropped or the chip slides under the cursor.
        self._hovered = False

    def set_hovered(self, hovered: bool) -> None:
        hovered = bool(hovered)
        if self._hovered == hovered:
            return
        self._hovered = hovered
        self.update()

    def enterEvent(self, event) -> None:
        self.set_hovered(True)
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:
        self.set_hovered(False)
        super().leaveEvent(event)

    def resizeEvent(self, event) -> None:
        self._path_cache_key = None
        self._path_cache = None
        self._path_rect_cache = None
        super().resizeEvent(event)

    def invalidate_theme_cache(self) -> None:
        self._theme_cache_key = None
        self._theme_cache = None

    def _tab_rect(self) -> QRectF:
        # Leave a hair of AA room on the left; right stays flush to the screen.
        return QRectF(self.rect()).adjusted(1.0, 1.0, 0.0, -1.0)

    def _tab_path(self, rect: QRectF | None = None) -> QPainterPath:
        """Bookmark tab: generous left radius, flush right edge."""
        r = rect if rect is not None else self._tab_rect()
        key = (int(self.width()), int(self.height()))
        if (
            rect is None
            and self._path_cache is not None
            and self._path_cache_key == key
        ):
            return self._path_cache
        radius = min(10.0, r.height() * 0.42)
        path = QPainterPath()
        path.moveTo(r.right(), r.top())
        path.lineTo(r.left() + radius, r.top())
        path.quadTo(r.left(), r.top(), r.left(), r.top() + radius)
        path.lineTo(r.left(), r.bottom() - radius)
        path.quadTo(r.left(), r.bottom(), r.left() + radius, r.bottom())
        path.lineTo(r.right(), r.bottom())
        path.closeSubpath()
        if rect is None:
            self._path_cache = path
            self._path_cache_key = key
            self._path_rect_cache = QRectF(r)
        return path

    def _is_active(self) -> bool:
        val = self.property("active")
        return val is True or val == "true"

    @staticmethod
    def _qcolor(hex_color: str, alpha: int = 255) -> QColor:
        c = QColor(hex_color)
        c.setAlpha(max(0, min(255, int(alpha))))
        return c

    @staticmethod
    def _mix(a: QColor, b: QColor, t: float) -> QColor:
        t = max(0.0, min(1.0, t))
        return QColor(
            int(a.red() + (b.red() - a.red()) * t),
            int(a.green() + (b.green() - a.green()) * t),
            int(a.blue() + (b.blue() - a.blue()) * t),
            int(a.alpha() + (b.alpha() - a.alpha()) * t),
        )

    def _theme(self) -> dict[str, str]:
        from src.ui.styles import get_theme_palette, normalize_theme

        key = self._settings.get("theme")
        if self._theme_cache is not None and key == self._theme_cache_key:
            return self._theme_cache
        self._theme_cache = get_theme_palette(normalize_theme(key))
        self._theme_cache_key = key
        return self._theme_cache

    def _palette(self) -> tuple[QColor, QColor, QColor, QColor, QColor]:
        """Return (fill_top, fill_bottom, text, border, rail)."""
        p = self._theme()
        hovered = bool(self._hovered)
        active = self._is_active()
        utility = self._role in ("folder", "note", "minutes", "record")

        accent = self._qcolor(p["accent"], 255)
        accent_hover = self._qcolor(p["accent_hover"], 255)
        soft = self._qcolor(p["accent_soft"], 255)
        card = self._qcolor(p["card"], 255)
        ink = self._qcolor(p.get("nav_active", p["text"]), 255)
        muted = self._qcolor(p["text_muted"], 255)

        if active:
            # Soft indigo wash — selected paper, not a solid brick.
            top = self._mix(soft, QColor(255, 255, 255), 0.35 if hovered else 0.22)
            bottom = self._mix(soft, accent, 0.08)
            bottom = self._mix(bottom, card, 0.42)
            top.setAlpha(236)
            bottom.setAlpha(244)
            text = accent_hover
            border = QColor(accent)
            border.setAlpha(55 if hovered else 40)
            rail = QColor(accent)
            rail.setAlpha(255)
            return top, bottom, text, border, rail

        if hovered:
            top = self._mix(card, soft, 0.22)
            bottom = self._mix(card, soft, 0.34)
            top.setAlpha(228 if utility else 236)
            bottom.setAlpha(236 if utility else 244)
            text = accent_hover
            border = QColor(accent)
            border.setAlpha(55)
        else:
            # Light frost — stay bright over green/photo wallpapers.
            top = QColor(255, 255, 255)
            bottom = self._mix(card, soft, 0.10)
            top.setAlpha(214 if utility else 228)
            bottom.setAlpha(222 if utility else 236)
            text = muted if utility else ink
            if utility:
                text = self._mix(muted, ink, 0.35)
            border = QColor(255, 255, 255)
            border.setAlpha(130 if utility else 150)
        rail = QColor(0, 0, 0, 0)
        return top, bottom, text, border, rail

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setRenderHint(QPainter.RenderHint.TextAntialiasing, True)
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
        painter.fillRect(self.rect(), QColor(0, 0, 0, 0))
        painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)

        rect = self._tab_rect() if self._path_rect_cache is None else self._path_rect_cache
        path = self._tab_path()
        if self._path_rect_cache is not None:
            rect = self._path_rect_cache
        fill_top, fill_bottom, text, border, rail = self._palette()
        active = self._is_active()
        hovered = bool(self._hovered)
        utility = self._role in ("folder", "note", "minutes", "record")

        painter.save()
        painter.setClipPath(path)

        body = QLinearGradient(rect.topLeft(), rect.bottomLeft())
        body.setColorAt(0.0, fill_top)
        body.setColorAt(1.0, fill_bottom)
        painter.fillRect(rect, body)

        # Whisper left shade — dock cue without a heavy 3D bevel.
        depth = QLinearGradient(rect.topLeft(), QPointF(rect.left() + 14.0, rect.top()))
        depth.setColorAt(0.0, QColor(15, 23, 42, 10 if active else 6))
        depth.setColorAt(1.0, QColor(15, 23, 42, 0))
        painter.fillRect(QRectF(rect.left(), rect.top(), 14.0, rect.height()), depth)

        lip = QLinearGradient(rect.topLeft(), QPointF(rect.left(), rect.top() + 4.0))
        lip.setColorAt(0.0, QColor(255, 255, 255, 70 if not utility else 50))
        lip.setColorAt(1.0, QColor(255, 255, 255, 0))
        painter.fillRect(QRectF(rect.left(), rect.top(), rect.width(), 4.0), lip)

        if active:
            bloom = QLinearGradient(rect.topLeft(), QPointF(rect.left() + 22.0, rect.top()))
            bloom_c = QColor(rail)
            bloom_c.setAlpha(28 if hovered else 18)
            bloom.setColorAt(0.0, bloom_c)
            bloom.setColorAt(1.0, QColor(rail.red(), rail.green(), rail.blue(), 0))
            painter.fillRect(QRectF(rect.left(), rect.top(), 22.0, rect.height()), bloom)

        painter.restore()

        # Outline only when hovered/active — idle frost stays borderless (no black box).
        if active or hovered:
            pen = QPen(border, 0.85)
            pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
            pen.setCapStyle(Qt.PenCapStyle.RoundCap)
            painter.setPen(pen)
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawPath(path)

        if active and rail.alpha() > 0:
            rail_h = max(11.0, rect.height() - 16.0)
            rail_rect = QRectF(
                rect.left() + 5.0, rect.center().y() - rail_h / 2.0, 2.2, rail_h
            )
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(rail)
            painter.drawRoundedRect(rail_rect, 1.1, 1.1)

        painter.setPen(text)
        font = QFont(self.font())
        if active:
            font.setWeight(QFont.Weight.DemiBold)
        elif hovered:
            font.setWeight(QFont.Weight.DemiBold if not utility else QFont.Weight.Medium)
        painter.setFont(font)
        left_pad = 13 if active else (9 if utility else 8)
        text_rect = QRectF(self.rect()).adjusted(left_pad, 0, -6, 0)
        fm = painter.fontMetrics()
        label = fm.elidedText(
            self.text(), Qt.TextElideMode.ElideRight, int(text_rect.width())
        )
        painter.drawText(text_rect, int(Qt.AlignmentFlag.AlignCenter), label)
        painter.end()


class _PeekRow(QWidget):
    """Clips a button that slides between tucked (right) and full (left=0)."""

    def __init__(
        self,
        button: QPushButton,
        *,
        full_w: int,
        peek_w: int,
        btn_h: int,
        parent: QWidget | None = None,
    ):
        super().__init__(parent)
        self.button = button
        self._full_w = full_w
        self._peek_w = peek_w
        self._btn_h = btn_h
        self._slide = float(full_w - peek_w)  # 0 = open, positive = tucked right
        self._anim: QPropertyAnimation | None = None
        self._hover_leave_timer = QTimer(self)
        self._hover_leave_timer.setSingleShot(True)
        self._hover_leave_timer.setInterval(210)
        self._hover_leave_timer.timeout.connect(self._on_leave_timeout)

        self.setFixedSize(full_w, btn_h)
        # Do NOT set WA_TranslucentBackground on child rows — on Windows that
        # fills the row with an opaque black rect behind rounded chips.
        self.setAutoFillBackground(False)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
        self.setStyleSheet("background:transparent;border:none;")
        button.setParent(self)
        button.setFixedSize(full_w, btn_h)
        button.move(int(self._slide), 0)
        self._ripple = _RippleOverlay(self)

    def paintEvent(self, _event) -> None:
        # Fully transparent clip host — only the chip paints.
        return

    def get_slide(self) -> float:
        return self._slide

    def set_slide(self, value: float) -> None:
        self._slide = float(value)
        self.button.move(int(round(self._slide)), 0)

    slide = pyqtProperty(float, fget=get_slide, fset=set_slide)

    @property
    def is_open(self) -> bool:
        return self._slide <= 1.0

    def _stop_anim(self) -> None:
        if self._anim is not None:
            self._anim.stop()
            self._anim.deleteLater()
            self._anim = None

    def open_out(self, *, animate: bool = True, ripple: bool = True) -> None:
        self._hover_leave_timer.stop()
        self._animate_to(0.0, animate=animate)
        if ripple and animate:
            # Ripple near the visible left edge of the resting tab.
            accent = "#1D4ED8"
            btn = self.button
            if isinstance(btn, _PeekChip):
                accent = btn._theme().get("accent", accent)
            self._ripple.play(
                QPoint(max(14, self._full_w - self._peek_w + 10), self._btn_h // 2),
                accent_hex=accent,
            )

    def tuck(self, *, animate: bool = True) -> None:
        self._hover_leave_timer.stop()
        self._clear_button_hover()
        self._animate_to(float(self._full_w - self._peek_w), animate=animate)

    def schedule_tuck(self) -> None:
        self._hover_leave_timer.start()

    def cancel_tuck(self) -> None:
        self._hover_leave_timer.stop()

    def _clear_button_hover(self) -> None:
        btn = self.button
        if isinstance(btn, _PeekChip):
            btn.set_hovered(False)

    def _cursor_on_button(self) -> bool:
        try:
            return self.button.rect().contains(self.button.mapFromGlobal(QCursor.pos()))
        except RuntimeError:
            return False

    def _on_leave_timeout(self) -> None:
        # Re-check after the grace window: slide animations / missed Leave on
        # translucent overlays can otherwise leave the row permanently open.
        if self._cursor_on_button():
            return
        self.tuck(animate=True)

    def _animate_to(self, target: float, *, animate: bool) -> None:
        target = float(target)
        if (
            self._anim is not None
            and self._anim.state() == QAbstractAnimation.State.Running
            and abs(float(self._anim.endValue()) - target) < 0.5
        ):
            return
        self._stop_anim()
        if not animate:
            self.set_slide(target)
            if target > 1.0:
                self._clear_button_hover()
            return
        start = float(self._slide)
        distance = abs(target - start)
        max_travel = float(max(1, self._full_w - self._peek_w))
        opening = target < start
        if opening:
            duration = 160
            curve = QEasingCurve.Type.OutCubic
        else:
            duration = 140
            curve = QEasingCurve.Type.InCubic
        # Short hops finish sooner so tiny adjustments stay snappy.
        if distance < max_travel * 0.55:
            duration = max(80, int(duration * 0.72))
        anim = QPropertyAnimation(self, b"slide", self)
        anim.setDuration(duration)
        anim.setStartValue(start)
        anim.setEndValue(target)
        anim.setEasingCurve(curve)
        if not opening:
            anim.finished.connect(self._clear_button_hover)
        self._anim = anim
        anim.start()


class PageIndicatorWidget(QWidget):
    """Page switcher docked on the right: peeks in; hover slides one button out."""

    page_changed = pyqtSignal(int)
    create_fence_requested = pyqtSignal(int)
    minutes_requested = pyqtSignal()
    minutes_folder_requested = pyqtSignal()
    folder_open_requested = pyqtSignal(str)
    note_requested = pyqtSignal()
    note_folder_requested = pyqtSignal()
    record_requested = pyqtSignal()
    record_folder_requested = pyqtSignal()
    calculator_requested = pyqtSignal()
    todo_requested = pyqtSignal()
    pet_requested = pyqtSignal()

    _EDGE_MARGIN = 8
    _DRAG_THRESHOLD = 16
    _SNAP_THRESHOLD = 28
    _BTN_H = 34
    _FULL_W = 104
    # Resting state keeps labels readable; hover only slides out the last bit.
    _PEEK_W = 86

    def __init__(
        self,
        pages: list[dict],
        current_page: int = 0,
        settings: dict | None = None,
        parent=None,
    ):
        super().__init__(parent)
        self.pages = pages
        self.current_page = current_page
        self.settings = settings if settings is not None else {}
        self._buttons: list[QPushButton] = []
        self._rows: list[_PeekRow] = []
        self._folder_buttons: list[QPushButton] = []
        self._folder_rows: list[_PeekRow] = []
        self._note_btn: QPushButton | None = None
        self._note_row: _PeekRow | None = None
        self._minutes_btn: QPushButton | None = None
        self._minutes_row: _PeekRow | None = None
        self._record_btn: QPushButton | None = None
        self._record_row: _PeekRow | None = None
        self._calc_btn: QPushButton | None = None
        self._calc_row: _PeekRow | None = None
        self._todo_btn: QPushButton | None = None
        self._todo_row: _PeekRow | None = None
        self._pet_btn: QPushButton | None = None
        self._pet_row: _PeekRow | None = None
        self._drag_offset: QPoint | None = None
        self._press_global: QPoint | None = None
        self._dragging = False
        self._menu_open = False
        self._context_action_row: _PeekRow | None = None
        self._ctx_menu_style_key: object | None = None
        self._ctx_menu_style: str | None = None
        # Failsafe: frameless translucent desktop bands often drop Leave when
        # the cursor exits the HWND. Poll while any peek is open.
        self._hover_watch = QTimer(self)
        self._hover_watch.setInterval(120)
        self._hover_watch.timeout.connect(self._on_hover_watch)

        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.Window
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_NoSystemBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
        self.setAutoFillBackground(False)
        self.setStyleSheet("background:transparent;")
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        # Shell attach must raise this HWND (not HWND_BOTTOM) or DefView buries it.
        self._desktidy_raise_band = True

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 10, 0, 10)
        layout.setSpacing(4)

        self._rebuild_buttons(layout)
        self._update_styles()
        self._restore_or_default_position()
        self._tuck_all(animate=False)

    # --- compatibility helpers used by tests / older call sites ---
    @property
    def _expanded(self) -> bool:
        """True when any button is slid fully open."""
        return any(row.is_open for row in self._iter_rows())

    def _set_expanded(self, expanded: bool, *, animate: bool = True) -> None:
        """Open all (True) or tuck all (False). Prefer per-button hover in UI."""
        if expanded:
            for row in self._iter_rows():
                row.open_out(animate=animate, ripple=False)
        else:
            self._tuck_all(animate=animate)

    def _iter_rows(self) -> list[_PeekRow]:
        rows = list(self._rows)
        rows.extend(self._folder_rows)
        for extra in (self._record_row, self._note_row, self._minutes_row, self._calc_row, self._todo_row):
            if extra is not None:
                rows.append(extra)
        return rows

    def _tuck_all(self, *, animate: bool = True) -> None:
        for row in self._iter_rows():
            row.tuck(animate=animate)
        self._stop_hover_watch()

    def _arm_hover_watch(self) -> None:
        if not self._hover_watch.isActive():
            self._hover_watch.start()

    def _stop_hover_watch(self) -> None:
        self._hover_watch.stop()

    def _on_hover_watch(self) -> None:
        if self._menu_open or self._dragging:
            return
        if not self._cursor_on_widget():
            self._clear_all_chip_hover()
            self._tuck_all(animate=True)
            return
        any_open = False
        for row in self._iter_rows():
            if row.is_open:
                any_open = True
                if not row._cursor_on_button():
                    row.schedule_tuck()
            elif isinstance(row.button, _PeekChip) and row.button._hovered:
                if not row._cursor_on_button():
                    row._clear_button_hover()
        if not any_open and not any(
            isinstance(r.button, _PeekChip) and r.button._hovered
            for r in self._iter_rows()
        ):
            self._stop_hover_watch()

    def _clear_all_chip_hover(self) -> None:
        for row in self._iter_rows():
            row._clear_button_hover()

    def _make_page_button(self, page: dict) -> QPushButton:
        btn = _PeekChip(
            page.get("name", f"页{page.get('id', 0) + 1}"),
            role="page",
            object_name="pageBtn",
            settings=self.settings,
        )
        btn.setToolTip("单击切换分页；右键在该页新建分区")
        btn.installEventFilter(self)
        return btn

    def _tool_flags(self) -> dict[str, bool]:
        from src.desktop_pet import float_bar_tool_flags

        return float_bar_tool_flags(self.settings)

    def _make_note_button(self) -> QPushButton:
        btn = _PeekChip(
            "记事本", role="note", object_name="pageNoteBtn", settings=self.settings
        )
        btn.setToolTip("单击打开记事本；右键打开当前笔记所在文件夹")
        btn.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        btn.customContextMenuRequested.connect(self._show_note_menu)
        btn.installEventFilter(self)
        return btn

    def _make_minutes_button(self) -> QPushButton:
        btn = _PeekChip(
            "纪要", role="minutes", object_name="pageMinutesBtn", settings=self.settings
        )
        btn.setToolTip("双击新建会议纪要；右键打开纪要文件夹")
        btn.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        btn.customContextMenuRequested.connect(self._show_minutes_menu)
        btn.installEventFilter(self)
        return btn

    def _make_record_button(self) -> QPushButton:
        hotkey = str(
            (self.settings.get("hotkeys") or {}).get("screen_record") or "F3"
        )
        btn = _PeekChip(
            "录屏", role="record", object_name="pageRecordBtn", settings=self.settings
        )
        btn.setToolTip(f"单击开始/结束录屏（{hotkey}）；开始时选择屏幕；右键打开录屏文件夹")
        btn.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        btn.customContextMenuRequested.connect(self._show_record_menu)
        btn.installEventFilter(self)
        return btn

    def _make_calculator_button(self) -> QPushButton:
        hotkey = str(
            (self.settings.get("hotkeys") or {}).get("calculator") or "Ctrl+Alt+C"
        )
        btn = _PeekChip(
            "计算器",
            role="calculator",
            object_name="pageCalcBtn",
            settings=self.settings,
        )
        btn.setToolTip(f"双击打开/收起系统计算器（{hotkey}）")
        btn.installEventFilter(self)
        return btn

    def _make_todo_button(self) -> QPushButton:
        btn = _PeekChip(
            "待办",
            role="todo",
            object_name="pageTodoBtn",
            settings=self.settings,
        )
        btn.setToolTip("单击显示/置顶桌面待办面板")
        btn.installEventFilter(self)
        return btn

    def _make_pet_button(self) -> QPushButton:
        btn = _PeekChip(
            "宠物",
            role="pet",
            object_name="pagePetBtn",
            settings=self.settings,
        )
        btn.setToolTip("单击显示/置顶桌面宠物（隐藏后也可点此找回）")
        btn.installEventFilter(self)
        return btn

    def _make_folder_button(self, item: dict) -> QPushButton:
        name = str(item.get("name") or "文件夹")
        path = str(item.get("path") or "")
        # Keep full name; paintEvent elides with font metrics.
        btn = _PeekChip(
            name, role="folder", object_name="pageFolderBtn", settings=self.settings
        )
        btn.setProperty("folder_path", path)
        btn.setToolTip("双击打开文件夹；右键打开所在位置")
        btn.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        btn.customContextMenuRequested.connect(
            lambda pos, b=btn: self._show_folder_menu(b, pos)
        )
        btn.installEventFilter(self)
        return btn

    def refresh_theme(self) -> None:
        """Repaint chips after settings theme changes."""
        self._ctx_menu_style_key = None
        self._ctx_menu_style = None
        chips = list(self._buttons) + list(self._folder_buttons)
        for extra in (
            self._record_btn,
            self._note_btn,
            self._minutes_btn,
            self._calc_btn,
            self._todo_btn,
            self._pet_btn,
        ):
            if extra is not None:
                chips.append(extra)
        for btn in chips:
            if isinstance(btn, _PeekChip):
                btn.invalidate_theme_cache()
            btn.update()

    def _wrap_row(self, btn: QPushButton) -> _PeekRow:
        row = _PeekRow(
            btn,
            full_w=self._FULL_W,
            peek_w=self._PEEK_W,
            btn_h=self._BTN_H,
            parent=self,
        )
        return row

    def _rebuild_buttons(self, layout: QVBoxLayout | None = None) -> None:
        layout = layout or self.layout()  # type: ignore[assignment]
        assert layout is not None
        while layout.count():
            item = layout.takeAt(0)
            widget = item.widget()
            if widget is not None:
                widget.deleteLater()
        self._buttons.clear()
        self._rows.clear()
        self._folder_buttons.clear()
        self._folder_rows.clear()
        self._note_btn = None
        self._note_row = None
        self._minutes_btn = None
        self._minutes_row = None
        self._record_btn = None
        self._record_row = None
        self._calc_btn = None
        self._calc_row = None
        self._todo_btn = None
        self._todo_row = None
        self._pet_btn = None
        self._pet_row = None
        from src.desktop_pet import desktop_pet_hosts_float_bar

        # Full float bar moved onto the pet — keep this edge HWND empty/absent.
        show_pages_here = not desktop_pet_hosts_float_bar(self.settings)
        if show_pages_here:
            for page in self.pages:
                btn = self._make_page_button(page)
                row = self._wrap_row(btn)
                self._buttons.append(btn)
                self._rows.append(row)
                layout.addWidget(row, 0, Qt.AlignmentFlag.AlignRight)
        if desktop_pet_hosts_float_bar(self.settings):
            # Folders + tools also live on the pet.
            return
        folder_items = get_page_folder_items(self.settings)
        flags = self._tool_flags()
        if folder_items:
            layout.addSpacing(6)
        for item in folder_items:
            btn = self._make_folder_button(item)
            row = self._wrap_row(btn)
            self._folder_buttons.append(btn)
            self._folder_rows.append(row)
            layout.addWidget(row, 0, Qt.AlignmentFlag.AlignRight)
        if any(flags.values()):
            layout.addSpacing(8)
        if flags["record"]:
            self._record_btn = self._make_record_button()
            self._record_row = self._wrap_row(self._record_btn)
            layout.addWidget(self._record_row, 0, Qt.AlignmentFlag.AlignRight)
        if flags["note"]:
            self._note_btn = self._make_note_button()
            self._note_row = self._wrap_row(self._note_btn)
            layout.addWidget(self._note_row, 0, Qt.AlignmentFlag.AlignRight)
        if flags["minutes"]:
            self._minutes_btn = self._make_minutes_button()
            self._minutes_row = self._wrap_row(self._minutes_btn)
            layout.addWidget(self._minutes_row, 0, Qt.AlignmentFlag.AlignRight)
        if flags["calculator"]:
            self._calc_btn = self._make_calculator_button()
            self._calc_row = self._wrap_row(self._calc_btn)
            layout.addWidget(self._calc_row, 0, Qt.AlignmentFlag.AlignRight)
        if flags.get("todo"):
            self._todo_btn = self._make_todo_button()
            self._todo_row = self._wrap_row(self._todo_btn)
            layout.addWidget(self._todo_row, 0, Qt.AlignmentFlag.AlignRight)
        if flags.get("pet"):
            self._pet_btn = self._make_pet_button()
            self._pet_row = self._wrap_row(self._pet_btn)
            layout.addWidget(self._pet_row, 0, Qt.AlignmentFlag.AlignRight)

    def reload_folder_shortcuts(self) -> None:
        """Refresh folder peeks after settings change without resetting page list."""
        self._rebuild_buttons()
        self._update_styles()
        self._tuck_all(animate=False)
        self.adjustSize()
        snapped = snap_geometry(self.geometry(), self._SNAP_THRESHOLD)
        self.setGeometry(snapped)

    def _page_id_for_button(self, btn: QObject) -> int | None:
        try:
            idx = self._buttons.index(btn)  # type: ignore[arg-type]
        except ValueError:
            return None
        if idx < 0 or idx >= len(self.pages):
            return None
        return int(self.pages[idx].get("id", idx))

    def _row_for_button(self, btn: QObject) -> _PeekRow | None:
        for row in self._iter_rows():
            if row.button is btn:
                return row
        return None

    def _default_position(self) -> QPoint:
        screen = work_screen()
        if not screen:
            return QPoint(100, 100)
        geo = screen.availableGeometry()
        self.adjustSize()
        x = geo.x() + geo.width() - self.width() - self._EDGE_MARGIN
        y = geo.y() + (geo.height() - self.height()) // 2
        return QPoint(x, y)

    def _restore_or_default_position(self) -> None:
        self.adjustSize()
        saved = self.settings.get("page_indicator_pos")
        if isinstance(saved, dict) and "x" in saved and "y" in saved:
            geo = QRect(int(saved["x"]), int(saved["y"]), self.width(), self.height())
            if rect_visible_on_any_screen(geo):
                snapped = snap_geometry(geo, self._SNAP_THRESHOLD)
                self.move(snapped.topLeft())
                return
            # Saved coords were on an unplugged / dead screen — fresh default.
        self.move(self._default_position())
        # Keep settings dict in sync; persist on next drag / normal save path
        # (avoid save_settings here — callers may pass a partial test dict).
        pos = self.pos()
        self.settings["page_indicator_pos"] = {"x": int(pos.x()), "y": int(pos.y())}

    def _save_position(self) -> None:
        pos = self.pos()
        self.settings["page_indicator_pos"] = {"x": pos.x(), "y": pos.y()}
        save_settings(self.settings)

    def _begin_drag(self, global_pos: QPoint) -> None:
        self._press_global = global_pos
        self._drag_offset = global_pos - self.frameGeometry().topLeft()
        self._dragging = False

    def _update_drag(self, global_pos: QPoint) -> bool:
        if self._drag_offset is None or self._press_global is None:
            return False
        if not self._dragging:
            if (global_pos - self._press_global).manhattanLength() < self._DRAG_THRESHOLD:
                return False
            self._dragging = True
        top_left = global_pos - self._drag_offset
        snapped = snap_geometry(
            QRect(top_left.x(), top_left.y(), self.width(), self.height()),
            self._SNAP_THRESHOLD,
        )
        self.move(snapped.topLeft())
        return True

    def _end_drag(self) -> None:
        if self._dragging:
            snapped = snap_geometry(self.geometry(), self._SNAP_THRESHOLD)
            self.setGeometry(snapped)
            self._save_position()
        self._drag_offset = None
        self._press_global = None
        self._dragging = False

    def _is_interactive_button(self, obj: QObject) -> bool:
        return (
            obj in self._buttons
            or obj in self._folder_buttons
            or obj is self._note_btn
            or obj is self._minutes_btn
            or obj is self._record_btn
            or obj is self._calc_btn
            or obj is self._todo_btn
        )

    def _folder_path_for_button(self, obj: QObject) -> str | None:
        if obj not in self._folder_buttons:
            return None
        path = obj.property("folder_path")  # type: ignore[union-attr]
        text = str(path or "").strip()
        return text or None

    def _context_menu_stylesheet(self) -> str:
        theme = None
        if isinstance(self.settings, dict):
            theme = self.settings.get("theme")
        if self._ctx_menu_style is not None and theme == self._ctx_menu_style_key:
            return self._ctx_menu_style
        from src.ui.styles import get_theme_palette

        p = get_theme_palette(theme)
        style = (
            f"QMenu {{"
            f" background-color: {p['card']};"
            f" color: {p['text']};"
            f" border: 1px solid {p['border']};"
            f" border-radius: 8px;"
            f" padding: 4px;"
            f"}}"
            f"QMenu::item {{"
            f" background-color: transparent;"
            f" color: {p['text']};"
            f" padding: 6px 18px 6px 12px;"
            f" border-radius: 4px;"
            f"}}"
            f"QMenu::item:selected {{"
            f" background-color: {p['accent_soft']};"
            f" color: {p['text']};"
            f"}}"
            f"QMenu::item:disabled {{"
            f" color: {p['text_muted']};"
            f"}}"
        )
        self._ctx_menu_style_key = theme
        self._ctx_menu_style = style
        return style

    def _make_context_menu(self) -> QMenu:
        """Build a peek-bar context menu that is readable on desktop overlays.

        PageIndicator uses ``background:transparent`` for the shell band. A QMenu
        parented to that widget inherits the rule and paints as a solid black
        slab with invisible text until hover. Parent the menu to None and set an
        opaque theme stylesheet so labels show immediately.
        """
        menu = QMenu(None)
        menu.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)
        menu.setStyleSheet(self._context_menu_stylesheet())
        return menu

    def _cursor_on_widget(self) -> bool:
        try:
            return self.rect().contains(self.mapFromGlobal(QCursor.pos()))
        except RuntimeError:
            return False

    def _cursor_on_row(self, row: _PeekRow) -> bool:
        try:
            return row.rect().contains(row.mapFromGlobal(QCursor.pos()))
        except RuntimeError:
            return False

    def _finish_context_menu(self, row: _PeekRow | None, *, action_chosen: bool) -> None:
        """After menu.exec: keep the row open when an item was chosen."""
        self._menu_open = False
        if row is None:
            return
        row.cancel_tuck()
        if action_chosen:
            self._context_action_row = row
            row.open_out(animate=True)
            self._arm_hover_watch()
            return
        self._context_action_row = None
        if not self._cursor_on_row(row):
            row.schedule_tuck()
        else:
            self._arm_hover_watch()

    def _run_context_menu(self, row: _PeekRow, btn: QPushButton, pos: QPoint, action_text: str):
        """Show opaque peek menu; keep the row expanded until the menu hides.

        Freeze overlay park/restack for the menu lifetime — QMenu FG is not
        Explorer desktop, and without a popup freeze the FG monitor parks every
        fence (flash → only the menu left on screen).
        """
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        begin = getattr(desk, "_begin_desktop_popup", None) if desk is not None else None
        end_later = (
            getattr(desk, "_end_desktop_popup_later", None) if desk is not None else None
        )
        if callable(begin):
            begin()

        self._menu_open = True
        row.cancel_tuck()
        row.open_out(animate=True)

        menu = self._make_context_menu()
        action = menu.addAction(action_text)

        chosen = None
        try:
            chosen = menu.exec(btn.mapToGlobal(pos))
        finally:
            self._finish_context_menu(row, action_chosen=(chosen is action))
            menu.deleteLater()
            if callable(end_later):
                end_later(400)
            elif desk is not None:
                end = getattr(desk, "_end_desktop_popup", None)
                if callable(end):
                    end()
        return chosen is action

    def _show_note_menu(self, pos: QPoint) -> None:
        if self._note_btn is None or self._note_row is None:
            return
        if self._run_context_menu(
            self._note_row, self._note_btn, pos, "打开笔记文件夹"
        ):
            self.note_folder_requested.emit()

    def _show_minutes_menu(self, pos: QPoint) -> None:
        if self._minutes_btn is None or self._minutes_row is None:
            return
        if self._run_context_menu(
            self._minutes_row, self._minutes_btn, pos, "打开纪要文件夹"
        ):
            self.minutes_folder_requested.emit()

    def _show_record_menu(self, pos: QPoint) -> None:
        if self._record_btn is None or self._record_row is None:
            return
        if self._run_context_menu(
            self._record_row, self._record_btn, pos, "打开录屏文件夹"
        ):
            self.record_folder_requested.emit()

    def _show_folder_menu(self, btn: QPushButton, pos: QPoint) -> None:
        row = self._row_for_button(btn)
        if row is None:
            return
        path = self._folder_path_for_button(btn)
        if not path:
            return
        if self._run_context_menu(row, btn, pos, "打开文件夹位置"):
            self.folder_open_requested.emit(path)

    def eventFilter(self, obj: QObject, event: QEvent) -> bool:
        row = self._row_for_button(obj) if self._is_interactive_button(obj) else None

        if row is not None:
            if event.type() == QEvent.Type.Enter:
                self._context_action_row = None
                self._arm_hover_watch()
                for other in self._iter_rows():
                    if other is row:
                        other.cancel_tuck()
                        if isinstance(other.button, _PeekChip):
                            other.button.set_hovered(True)
                        other.open_out(animate=True, ripple=False)
                    else:
                        if isinstance(other.button, _PeekChip):
                            other.button.set_hovered(False)
                        if not self._menu_open:
                            other.tuck(animate=True)
                return False
            if event.type() == QEvent.Type.Leave:
                if isinstance(obj, _PeekChip):
                    obj.set_hovered(False)
                if not self._menu_open:
                    if self._context_action_row is row and self._cursor_on_widget():
                        return False
                    if self._context_action_row is row:
                        self._context_action_row = None
                    # Always schedule: cursor may still sit in the row's empty
                    # clip while already off the chip; timeout re-checks.
                    row.schedule_tuck()
                return False

        if self._is_interactive_button(obj):
            if event.type() == QEvent.Type.MouseButtonPress and event.button() == Qt.MouseButton.RightButton:
                page_id = self._page_id_for_button(obj)
                if page_id is not None:
                    # LMB switches pages; RMB creates a fence on that page.
                    self.create_fence_requested.emit(int(page_id))
                    return True
            if event.type() == QEvent.Type.MouseButtonPress and event.button() == Qt.MouseButton.LeftButton:
                self._begin_drag(event.globalPosition().toPoint())
            elif event.type() == QEvent.Type.MouseMove and event.buttons() & Qt.MouseButton.LeftButton:
                if self._update_drag(event.globalPosition().toPoint()):
                    return True
            elif event.type() == QEvent.Type.MouseButtonRelease and event.button() == Qt.MouseButton.LeftButton:
                was_dragging = self._dragging
                self._end_drag()
                if was_dragging:
                    return True
                if obj is self._note_btn:
                    self.note_requested.emit()
                    return True
                if obj is self._record_btn:
                    self.record_requested.emit()
                    return True
                if obj is self._calc_btn:
                    # Open only on double-click (same as 纪要 / folder shortcuts).
                    return True
                if obj is self._todo_btn:
                    self.todo_requested.emit()
                    return True
                if obj is self._pet_btn:
                    self.pet_requested.emit()
                    return True
                if obj is self._minutes_btn:
                    # Create only on double-click.
                    return True
                if self._folder_path_for_button(obj) is not None:
                    return True
                page_id = self._page_id_for_button(obj)
                if page_id is not None:
                    self._select_page(page_id)
                return True
            elif (
                event.type() == QEvent.Type.MouseButtonDblClick
                and event.button() == Qt.MouseButton.LeftButton
            ):
                if obj is self._minutes_btn:
                    self.minutes_requested.emit()
                    return True
                if obj is self._calc_btn:
                    self.calculator_requested.emit()
                    return True
                path = self._folder_path_for_button(obj)
                if path is not None:
                    self.folder_open_requested.emit(path)
                    return True
        return super().eventFilter(obj, event)

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._begin_drag(event.globalPosition().toPoint())
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if event.buttons() & Qt.MouseButton.LeftButton:
            if self._update_drag(event.globalPosition().toPoint()):
                event.accept()
                return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._end_drag()
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def enterEvent(self, event) -> None:
        self.setCursor(QCursor(Qt.CursorShape.SizeAllCursor))
        super().enterEvent(event)

    def leaveEvent(self, event) -> None:
        if not self._menu_open and not self._dragging and not self._cursor_on_widget():
            self._context_action_row = None
            self._clear_all_chip_hover()
            self._tuck_all(animate=True)
        super().leaveEvent(event)

    def _select_page(self, page_id: int) -> None:
        page_id = int(page_id)
        if page_id == int(self.current_page):
            return
        self.current_page = page_id
        self._update_styles()
        self.page_changed.emit(page_id)

    def set_current_page(self, page_id: int) -> None:
        page_id = int(page_id)
        if page_id == int(self.current_page):
            return
        self.current_page = page_id
        self._update_styles()

    def update_pages(self, pages: list[dict], current_page: int) -> None:
        self.pages = pages
        self.current_page = current_page
        self._rebuild_buttons()
        self._update_styles()
        self._tuck_all(animate=False)
        self.adjustSize()
        snapped = snap_geometry(self.geometry(), self._SNAP_THRESHOLD)
        self.setGeometry(snapped)

    def _update_styles(self) -> None:
        for i, btn in enumerate(self._buttons):
            page_id = self.pages[i].get("id", i) if i < len(self.pages) else i
            btn.setProperty("active", page_id == self.current_page)
            btn.update()

    def paintEvent(self, _event) -> None:
        # Top-level host stays fully transparent (no black chrome frame).
        return

    def showEvent(self, event) -> None:
        if not self.settings.get("page_indicator_pos"):
            self.move(self._default_position())
        else:
            snapped = snap_geometry(self.geometry(), self._SNAP_THRESHOLD)
            self.setGeometry(snapped)
        super().showEvent(event)
        configure_desktop_overlay(self)
