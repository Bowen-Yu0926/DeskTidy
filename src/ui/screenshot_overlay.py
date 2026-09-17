"""Fullscreen screenshot overlay with WeChat-style annotation toolbar."""

from __future__ import annotations

import math

from PyQt6.QtCore import QPoint, QRect, QSize, Qt, pyqtSignal
from PyQt6.QtGui import (
    QColor,
    QFont,
    QIcon,
    QKeySequence,
    QPainter,
    QPen,
    QPixmap,
    QPolygon,
)
from PyQt6.QtWidgets import (
    QFrame,
    QHBoxLayout,
    QLineEdit,
    QPushButton,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

_MASK = QColor(22, 26, 32, 150)
_BORDER = QColor(7, 193, 96)
_BORDER_OUTER = QColor(255, 255, 255, 200)
_HANDLE = QColor(7, 193, 96)
_HANDLE_BORDER = QColor(255, 255, 255)
_BADGE_BG = QColor(18, 20, 24, 188)
_BADGE_TEXT = QColor(255, 255, 255)
_ANNOTATION_TEXT = QColor(255, 255, 255)

# WeChat screenshot palette
_COLORS = {
    "red": QColor(250, 81, 81),
    "orange": QColor(250, 157, 59),
    "blue": QColor(20, 133, 238),
    "green": QColor(7, 193, 96),
    "black": QColor(25, 25, 25),
    "white": QColor(255, 255, 255),
}
_WIDTHS = (2, 4, 7)
_DRAW_TOOLS = {"rect", "ellipse", "arrow", "brush", "mosaic", "text", "emoji"}
_EMOJI_CHOICES = ("😀", "👍", "❤️", "🔥", "⭐", "👀", "🎉", "❗")


def _icon_pixmap(kind: str, size: int = 22, active: bool = False) -> QPixmap:
    """Paint compact WeChat-like toolbar icons."""
    pm = QPixmap(size, size)
    pm.fill(Qt.GlobalColor.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    color = QColor(7, 193, 96) if active else QColor(53, 53, 53)
    pen = QPen(color, 1.8, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin)
    p.setPen(pen)
    p.setBrush(Qt.BrushStyle.NoBrush)
    m = 3
    s = size - m * 2

    if kind == "rect":
        p.drawRoundedRect(m + 1, m + 2, s - 2, s - 4, 2, 2)
    elif kind == "ellipse":
        p.drawEllipse(m + 1, m + 3, s - 2, s - 6)
    elif kind == "emoji":
        p.setFont(QFont("Segoe UI Emoji", 12))
        p.drawText(QRect(0, 0, size, size), Qt.AlignmentFlag.AlignCenter, "☺")
    elif kind == "arrow":
        p.drawLine(m + 2, size - m - 2, size - m - 2, m + 2)
        tip = QPoint(size - m - 2, m + 2)
        p.drawLine(tip, QPoint(tip.x() - 7, tip.y() + 2))
        p.drawLine(tip, QPoint(tip.x() - 2, tip.y() + 7))
    elif kind == "brush":
        p.drawLine(m + 3, size - m - 3, size // 2, size // 2)
        p.setBrush(color)
        p.drawEllipse(size // 2 - 1, m + 2, 8, 8)
    elif kind == "mosaic":
        cell = 4
        for y in range(m + 1, size - m - 1, cell):
            for x in range(m + 1, size - m - 1, cell):
                if ((x + y) // cell) % 2 == 0:
                    p.fillRect(x, y, cell - 1, cell - 1, color)
    elif kind == "text":
        p.setFont(QFont("Microsoft YaHei UI", 13, QFont.Weight.Bold))
        p.drawText(QRect(0, 0, size, size), Qt.AlignmentFlag.AlignCenter, "A")
    elif kind == "undo":
        p.drawArc(m + 2, m + 3, s - 4, s - 4, 40 * 16, 250 * 16)
        tip = QPoint(m + 3, size // 2 - 1)
        p.drawLine(tip, QPoint(tip.x() + 5, tip.y() - 4))
        p.drawLine(tip, QPoint(tip.x() + 5, tip.y() + 4))
    elif kind == "save":
        p.drawRoundedRect(m + 2, m + 2, s - 4, s - 4, 2, 2)
        p.drawRect(m + 5, m + 2, s - 10, 5)
        p.drawRect(m + 6, size // 2, s - 12, s // 2 - 2)
    elif kind == "cancel":
        p.drawLine(m + 4, m + 4, size - m - 4, size - m - 4)
        p.drawLine(size - m - 4, m + 4, m + 4, size - m - 4)
    elif kind == "confirm":
        p.setPen(QPen(QColor(255, 255, 255), 2.4, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.drawLine(m + 3, size // 2 + 1, size // 2 - 1, size - m - 4)
        p.drawLine(size // 2 - 1, size - m - 4, size - m - 3, m + 4)
    p.end()
    return pm


def _make_icon(kind: str, size: int = 22) -> QIcon:
    icon = QIcon()
    icon.addPixmap(_icon_pixmap(kind, size, False), QIcon.Mode.Normal)
    icon.addPixmap(_icon_pixmap(kind, size, True), QIcon.Mode.Active)
    icon.addPixmap(_icon_pixmap(kind, size, True), QIcon.Mode.Selected)
    return icon


class InlineTextInput(QLineEdit):
    accepted_text = pyqtSignal(str)
    cancelled = pyqtSignal()

    def keyPressEvent(self, event) -> None:
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            self.accepted_text.emit(self.text())
            return
        if event.key() == Qt.Key.Key_Escape:
            self.cancelled.emit()
            return
        super().keyPressEvent(event)

    def focusOutEvent(self, event) -> None:
        self.accepted_text.emit(self.text())
        super().focusOutEvent(event)


class ScreenshotToolbar(QWidget):
    """WeChat-style screenshot toolbar: tools | save/cancel/confirm + style strip."""

    action_triggered = pyqtSignal(str)
    tool_selected = pyqtSignal(str)
    color_selected = pyqtSignal(str)
    width_selected = pyqtSignal(int)
    undo_requested = pyqtSignal()

    def __init__(self, parent: QWidget | None = None):
        super().__init__(
            parent,
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.Window
            | Qt.WindowType.WindowStaysOnTopHint,
        )
        self.setObjectName("screenshotToolbar")
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # Secondary strip: stroke width + colors (WeChat shows when a draw tool is active)
        self._style_bar = QWidget()
        self._style_bar.setObjectName("screenshotStyleBar")
        self._style_bar.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        style_layout = QHBoxLayout(self._style_bar)
        style_layout.setContentsMargins(10, 6, 10, 6)
        style_layout.setSpacing(8)

        self._width_buttons: dict[int, QPushButton] = {}
        for width in _WIDTHS:
            btn = QPushButton()
            btn.setObjectName("screenshotWidthBtn")
            btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            btn.setCheckable(True)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.setFixedSize(22, 22)
            btn.setProperty("strokeWidth", width)
            btn.setToolTip(f"粗细 {width}")
            btn.clicked.connect(lambda _=False, w=width: self._on_width(w))
            style_layout.addWidget(btn)
            self._width_buttons[width] = btn

        sep = QFrame()
        sep.setObjectName("screenshotSep")
        sep.setFrameShape(QFrame.Shape.VLine)
        sep.setFixedSize(1, 18)
        style_layout.addWidget(sep)

        self._color_buttons: dict[str, QPushButton] = {}
        for color_name in ("red", "orange", "blue", "green", "black", "white"):
            btn = QPushButton()
            btn.setObjectName("screenshotColorBtn")
            btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            btn.setProperty("colorName", color_name)
            btn.setCheckable(True)
            btn.setCursor(Qt.CursorShape.PointingHandCursor)
            btn.setFixedSize(18, 18)
            btn.setToolTip({"red": "红", "orange": "橙", "blue": "蓝", "green": "绿", "black": "黑", "white": "白"}[color_name])
            btn.clicked.connect(lambda _=False, c=color_name: self._on_color(c))
            style_layout.addWidget(btn)
            self._color_buttons[color_name] = btn
        style_layout.addStretch(1)
        self._style_bar.hide()
        root.addWidget(self._style_bar)

        # Main WeChat bar
        main = QWidget()
        main.setObjectName("screenshotMainBar")
        main.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        layout = QHBoxLayout(main)
        layout.setContentsMargins(8, 6, 8, 6)
        layout.setSpacing(2)

        self._tool_buttons: dict[str, QPushButton] = {}
        for tool, tip in (
            ("rect", "矩形"),
            ("ellipse", "椭圆"),
            ("emoji", "表情"),
            ("arrow", "箭头"),
            ("brush", "画笔"),
            ("mosaic", "马赛克"),
            ("text", "文字"),
        ):
            btn = self._icon_button(tool, tip, checkable=True)
            btn.clicked.connect(lambda _=False, t=tool: self._on_tool(t))
            layout.addWidget(btn)
            self._tool_buttons[tool] = btn

        undo_btn = self._icon_button("undo", "撤销")
        undo_btn.clicked.connect(self.undo_requested.emit)
        layout.addWidget(undo_btn)

        layout.addSpacing(6)
        sep2 = QFrame()
        sep2.setObjectName("screenshotSep")
        sep2.setFrameShape(QFrame.Shape.VLine)
        sep2.setFixedSize(1, 22)
        layout.addWidget(sep2)
        layout.addSpacing(6)

        save_btn = self._icon_button("save", "保存")
        save_btn.clicked.connect(lambda: self.action_triggered.emit("save"))
        layout.addWidget(save_btn)

        cancel_btn = self._icon_button("cancel", "退出")
        cancel_btn.setObjectName("screenshotCancelBtn")
        cancel_btn.clicked.connect(lambda: self.action_triggered.emit("cancel"))
        layout.addWidget(cancel_btn)

        confirm_btn = QPushButton()
        confirm_btn.setObjectName("screenshotConfirmBtn")
        confirm_btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        confirm_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        confirm_btn.setToolTip("完成")
        confirm_btn.setFixedSize(32, 32)
        confirm_btn.setIcon(_make_icon("confirm", 18))
        confirm_btn.setIconSize(QSize(18, 18))
        confirm_btn.clicked.connect(lambda: self.action_triggered.emit("copy"))
        layout.addWidget(confirm_btn)

        root.addWidget(main)
        self.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Fixed)

    def _icon_button(self, kind: str, tip: str, *, checkable: bool = False) -> QPushButton:
        btn = QPushButton()
        btn.setObjectName("screenshotIconBtn")
        btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setToolTip(tip)
        btn.setFixedSize(32, 32)
        btn.setCheckable(checkable)
        btn.setIcon(_make_icon(kind, 20))
        btn.setIconSize(QSize(20, 20))
        return btn

    def _on_tool(self, tool: str) -> None:
        self.set_active_tool(tool)
        self.tool_selected.emit(tool)

    def _on_color(self, color_name: str) -> None:
        self.set_active_color(color_name)
        self.color_selected.emit(color_name)

    def _on_width(self, width: int) -> None:
        self.set_active_width(width)
        self.width_selected.emit(width)

    def set_active_tool(self, tool: str) -> None:
        for name, btn in self._tool_buttons.items():
            btn.setChecked(name == tool)
            btn.setIcon(_make_icon(name, 20))
        show_style = tool in {"rect", "ellipse", "arrow", "brush", "text"}
        # mosaic only needs width in WeChat; still show widths
        if tool == "mosaic":
            show_style = True
            for btn in self._color_buttons.values():
                btn.hide()
        else:
            for btn in self._color_buttons.values():
                btn.setVisible(True)
        self._style_bar.setVisible(show_style and bool(tool))

    def set_active_color(self, color_name: str) -> None:
        for name, btn in self._color_buttons.items():
            btn.setChecked(name == color_name)

    def set_active_width(self, width: int) -> None:
        for value, btn in self._width_buttons.items():
            btn.setChecked(value == width)


class ScreenshotOverlay(QWidget):
    finished = pyqtSignal(object, str)  # QPixmap | None, action

    HANDLE = 5
    _CLICK_DRAG_SLOP = 6

    def __init__(
        self,
        desktop: QPixmap,
        origin: QPoint,
        parent: QWidget | None = None,
        *,
        window_rects: list[QRect] | None = None,
    ):
        super().__init__(
            parent,
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.Window
            | Qt.WindowType.WindowStaysOnTopHint,
        )
        self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
        self._desktop = desktop
        self._origin = origin
        self._window_rects: list[QRect] = [
            QRect(r) for r in (window_rects or []) if isinstance(r, QRect) and not r.isNull()
        ]
        self._hover_rect: QRect | None = None
        self._press_hover: QRect | None = None
        # WeChat: no hover chrome until the cursor actually moves (grabMouse
        # often synthesizes a move at the current position → would look like
        # a default selection box).
        self._hover_ready = False
        self._hover_arm_pos: QPoint | None = None
        self._start: QPoint | None = None
        self._current: QPoint | None = None
        self._selection: QRect | None = None
        self._tool = ""
        self._color_name = "red"
        self._stroke_width = 4
        self._annotations: list[dict] = []
        self._draft_annotation: dict | None = None
        self._emoji_index = 0
        self._text_input: InlineTextInput | None = None
        self._text_anchor: QPoint | None = None
        self._toolbar = ScreenshotToolbar()
        self._toolbar.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._toolbar.action_triggered.connect(self._on_action)
        self._toolbar.tool_selected.connect(self._set_tool)
        self._toolbar.color_selected.connect(self._set_color)
        self._toolbar.width_selected.connect(self._set_width)
        self._toolbar.undo_requested.connect(self._undo)
        self._toolbar.set_active_color(self._color_name)
        self._toolbar.set_active_width(self._stroke_width)
        self._toolbar.hide()
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.CrossCursor)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent, True)

    def _logical_desktop_size(self) -> QSize:
        """Qt logical size of the freeze-frame (not the physical buffer)."""
        size = self._desktop.deviceIndependentSize().toSize()
        if size.width() >= 2 and size.height() >= 2:
            return size
        return self._desktop.size()

    def _logical_desktop_rect(self) -> QRect:
        return QRect(QPoint(0, 0), self._logical_desktop_size())

    def show_overlay(self) -> None:
        # Geometry must stay in Qt *logical* pixels. A Win32 capture may be
        # physical-sized but stamped with devicePixelRatio by grab_desktop().
        self.setGeometry(QRect(self._origin, self._logical_desktop_size()))
        # Idle like WeChat: dim only, no green box until hover recognizes a window.
        self._hover_ready = False
        self._hover_arm_pos = None
        self._hover_rect = None
        self.show()
        self.raise_()
        self.activateWindow()
        self.setFocus(Qt.FocusReason.ActiveWindowFocusReason)
        self.grabKeyboard()
        # Grab mouse too — settings / dialogs can briefly steal Z-order and
        # leave a visible freeze-frame that ignores clicks (feels "stuck").
        self.grabMouse()
        # Stay above ApplicationModal / topmost dialogs that were just suspended.
        try:
            from src.screenshot_manager import force_widget_topmost

            force_widget_topmost(self)
            if self._toolbar is not None and self._toolbar.isVisible():
                force_widget_topmost(self._toolbar)
        except Exception:
            pass
        self.repaint()

    def show_fullscreen(self) -> None:
        self.show_overlay()

    def _release_input_grab(self) -> None:
        try:
            self.releaseMouse()
        except RuntimeError:
            pass
        try:
            self.releaseKeyboard()
        except RuntimeError:
            pass

    def _reclaim_input_grab(self) -> None:
        if self._text_input is not None:
            return
        self.activateWindow()
        self.setFocus(Qt.FocusReason.ActiveWindowFocusReason)
        self.grabKeyboard()
        # Toolbar is a separate top-level — release mouse so its buttons work.
        toolbar_up = self._toolbar is not None and self._toolbar.isVisible()
        if toolbar_up:
            try:
                self.releaseMouse()
            except RuntimeError:
                pass
        else:
            self.grabMouse()
        try:
            from src.screenshot_manager import force_widget_topmost

            force_widget_topmost(self)
            if self._toolbar is not None and self._toolbar.isVisible():
                force_widget_topmost(self._toolbar)
        except Exception:
            pass

    def _set_tool(self, tool: str) -> None:
        self._tool = tool
        if self._toolbar is not None:
            self._toolbar.set_active_tool(tool)
            self._reposition_toolbar()
        self._reclaim_input_grab()

    def _set_color(self, color_name: str) -> None:
        if color_name not in _COLORS:
            return
        self._color_name = color_name
        if self._toolbar is not None:
            self._toolbar.set_active_color(color_name)
        self._reclaim_input_grab()

    def _set_width(self, width: int) -> None:
        self._stroke_width = int(width)
        if self._toolbar is not None:
            self._toolbar.set_active_width(self._stroke_width)
        self._reclaim_input_grab()

    def _undo(self) -> None:
        if self._annotations:
            self._annotations.pop()
            self.update()
        self._reclaim_input_grab()

    def _active_rect(self) -> QRect | None:
        if self._selection and not self._selection.isNull():
            return self._selection
        return self._selection_rect()

    def _selection_rect(self) -> QRect | None:
        if self._start is None or self._current is None:
            return None
        return QRect(self._start, self._current).normalized()

    def _crop_selection(self) -> QPixmap | None:
        rect = self._active_rect()
        if rect is None or rect.width() < 2 or rect.height() < 2:
            return None
        dpr = self._desktop.devicePixelRatio()
        phys = QRect(
            int(rect.x() * dpr),
            int(rect.y() * dpr),
            int(rect.width() * dpr),
            int(rect.height() * dpr),
        )
        result = self._desktop.copy(phys)
        result.setDevicePixelRatio(dpr)
        painter = QPainter(result)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        for annotation in self._annotations:
            self._draw_annotation(painter, annotation, rect.topLeft())
        painter.end()
        return result

    def _draw_mask_outside(self, painter: QPainter, rect: QRect) -> None:
        full = self.rect()
        painter.fillRect(0, 0, full.width(), rect.top(), _MASK)
        painter.fillRect(0, rect.bottom() + 1, full.width(), full.height() - rect.bottom() - 1, _MASK)
        painter.fillRect(0, rect.top(), rect.left(), rect.height(), _MASK)
        painter.fillRect(rect.right() + 1, rect.top(), full.width() - rect.right() - 1, rect.height(), _MASK)

    def _draw_handle(self, painter: QPainter, center: QPoint) -> None:
        half = self.HANDLE // 2
        r = QRect(center.x() - half, center.y() - half, self.HANDLE, self.HANDLE)
        painter.fillRect(r, _HANDLE)
        painter.setPen(QPen(_HANDLE_BORDER, 1))
        painter.drawRect(r)

    def _draw_hover_chrome(self, painter: QPainter, rect: QRect) -> None:
        """WeChat hover: green border only — no handles / size badge yet."""
        painter.setPen(QPen(_BORDER, 2))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRect(rect)

    def _draw_selection_chrome(self, painter: QPainter, rect: QRect) -> None:
        inner = rect.adjusted(1, 1, -1, -1)
        painter.setPen(QPen(_BORDER_OUTER, 1))
        painter.setBrush(Qt.BrushStyle.NoBrush)
        painter.drawRect(rect)
        painter.setPen(QPen(_BORDER, 2))
        painter.drawRect(inner)
        for corner in (rect.topLeft(), rect.topRight(), rect.bottomLeft(), rect.bottomRight()):
            self._draw_handle(painter, corner)

        text = f"{rect.width()} × {rect.height()}"
        font = QFont("Microsoft YaHei UI", 9)
        painter.setFont(font)
        metrics = painter.fontMetrics()
        pad_x, pad_y = 8, 4
        badge_w = metrics.horizontalAdvance(text) + pad_x * 2
        badge_h = metrics.height() + pad_y * 2
        badge_x = rect.left()
        badge_y = rect.top() - badge_h - 4
        if badge_y < 0:
            badge_y = rect.top() + 4
        badge = QRect(badge_x, badge_y, badge_w, badge_h)
        painter.fillRect(badge, _BADGE_BG)
        painter.setPen(_BADGE_TEXT)
        painter.drawText(badge, Qt.AlignmentFlag.AlignCenter, text)

    def _mosaic_blocks_for_points(
        self, points: list, block: int, offset: QPoint
    ) -> list[tuple[int, int, QColor]]:
        """Sample desktop tiles once per mosaic stroke (not every paintEvent)."""
        visited: set[tuple[int, int]] = set()
        blocks: list[tuple[int, int, QColor]] = []
        for pt in points:
            local = pt - offset
            gx = (local.x() // block) * block
            gy = (local.y() // block) * block
            key = (gx, gy)
            if key in visited:
                continue
            visited.add(key)
            src = QRect(gx + offset.x(), gy + offset.y(), block, block)
            clipped = src.intersected(self._logical_desktop_rect())
            dpr = self._desktop.devicePixelRatio()
            phys = QRect(
                int(clipped.x() * dpr),
                int(clipped.y() * dpr),
                int(clipped.width() * dpr),
                int(clipped.height() * dpr),
            )
            sample = self._desktop.copy(phys)
            if sample.isNull():
                continue
            avg = sample.scaled(
                1,
                1,
                Qt.AspectRatioMode.IgnoreAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            )
            blocks.append((gx, gy, QColor(avg.toImage().pixelColor(0, 0))))
        return blocks

    def _draw_annotation(self, painter: QPainter, annotation: dict, offset: QPoint = QPoint(0, 0)) -> None:
        kind = annotation.get("kind")
        color = annotation.get("color", _COLORS["red"])
        width = int(annotation.get("width", 4))
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)

        if kind in ("rect", "ellipse", "arrow"):
            start = annotation.get("start", QPoint()) - offset
            end = annotation.get("end", start) - offset
            if kind == "rect":
                painter.setPen(QPen(color, width))
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.drawRect(QRect(start, end).normalized())
                return
            if kind == "ellipse":
                painter.setPen(QPen(color, width))
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.drawEllipse(QRect(start, end).normalized())
                return
            # arrow
            painter.setPen(QPen(color, width, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
            painter.drawLine(start, end)
            angle = math.atan2(end.y() - start.y(), end.x() - start.x())
            arrow_len = 10 + width * 2
            left = QPoint(
                int(end.x() - arrow_len * math.cos(angle - math.pi / 6)),
                int(end.y() - arrow_len * math.sin(angle - math.pi / 6)),
            )
            right = QPoint(
                int(end.x() - arrow_len * math.cos(angle + math.pi / 6)),
                int(end.y() - arrow_len * math.sin(angle + math.pi / 6)),
            )
            painter.setBrush(color)
            painter.drawPolygon(QPolygon([end, left, right]))
            return

        if kind == "brush":
            points = [pt - offset for pt in annotation.get("points", [])]
            if len(points) < 2:
                return
            painter.setPen(QPen(color, width, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
            for i in range(1, len(points)):
                painter.drawLine(points[i - 1], points[i])
            return

        if kind == "mosaic":
            block = max(6, width * 3)
            blocks = annotation.get("blocks")
            if not isinstance(blocks, list):
                blocks = self._mosaic_blocks_for_points(
                    annotation.get("points", []), block, offset
                )
                if annotation is not self._draft_annotation:
                    annotation["blocks"] = blocks
            for gx, gy, color in blocks:
                painter.fillRect(QRect(gx, gy, block, block), color)
            return

        if kind == "text":
            start = annotation.get("start", QPoint()) - offset
            painter.setFont(QFont("Microsoft YaHei UI", 12 + width, QFont.Weight.Bold))
            painter.setPen(color)
            painter.drawText(start, annotation.get("text", ""))
            return

        if kind == "emoji":
            start = annotation.get("start", QPoint()) - offset
            size = 18 + width * 4
            painter.setFont(QFont("Segoe UI Emoji", size))
            painter.drawText(start, annotation.get("text", "😀"))

    def _reposition_toolbar(self) -> None:
        rect = self._active_rect()
        if rect is None or self._toolbar is None:
            return
        self._show_toolbar(rect)

    def _show_toolbar(self, rect: QRect) -> None:
        if self._toolbar is None:
            return
        self._toolbar.adjustSize()
        global_bottom = self.mapToGlobal(rect.bottomLeft())
        pos = QPoint(
            global_bottom.x() + rect.width() // 2 - self._toolbar.width() // 2,
            global_bottom.y() + 8,
        )
        desk = self._logical_desktop_size()
        max_x = self._origin.x() + desk.width() - self._toolbar.width()
        max_y = self._origin.y() + desk.height() - self._toolbar.height()
        pos.setX(max(self._origin.x(), min(pos.x(), max_x)))
        if pos.y() > max_y:
            pos.setY(self.mapToGlobal(rect.topLeft()).y() - self._toolbar.height() - 8)
        pos.setY(max(self._origin.y(), min(pos.y(), max_y)))
        self._toolbar.move(pos)
        self._toolbar.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self._toolbar.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._toolbar.show()
        self._toolbar.raise_()
        # Mouse grab must be off so toolbar buttons receive clicks.
        try:
            self.releaseMouse()
        except RuntimeError:
            pass
        self._reclaim_input_grab()

    def _hide_toolbar(self) -> None:
        if self._toolbar is not None:
            self._toolbar.hide()
        # Back to region-select — reclaim mouse so clicks are not stolen.
        if self._text_input is None:
            try:
                self.grabMouse()
            except RuntimeError:
                pass

    def _close_text_input(self, *, reclaim: bool = True) -> None:
        if self._text_input is None:
            return
        self._text_input.blockSignals(True)
        self._text_input.close()
        self._text_input.deleteLater()
        self._text_input = None
        self._text_anchor = None
        if reclaim:
            self._reclaim_input_grab()

    def _begin_inline_text(self, pos: QPoint) -> None:
        self._close_text_input(reclaim=False)
        rect = self._active_rect()
        if rect is None:
            return
        self._release_input_grab()
        self._text_anchor = pos
        edit = InlineTextInput(self)
        edit.setPlaceholderText("输入文字，回车确认")
        edit.resize(180, 32)
        x = min(max(rect.left(), pos.x()), max(rect.left(), rect.right() - edit.width()))
        y = min(max(rect.top(), pos.y() - edit.height() // 2), max(rect.top(), rect.bottom() - edit.height()))
        edit.move(x, y)
        edit.accepted_text.connect(self._commit_inline_text)
        edit.cancelled.connect(self._cancel_inline_text)
        edit.show()
        edit.setFocus(Qt.FocusReason.ActiveWindowFocusReason)
        self._text_input = edit

    def _commit_inline_text(self, text: str) -> None:
        if self._text_input is None or self._text_anchor is None:
            return
        if text.strip():
            self._annotations.append(
                {
                    "kind": "text",
                    "start": QPoint(self._text_anchor),
                    "end": QPoint(self._text_anchor),
                    "text": text.strip(),
                    "color": QColor(_COLORS[self._color_name]),
                    "width": self._stroke_width,
                }
            )
        self._close_text_input()
        self.update()

    def _cancel_inline_text(self) -> None:
        self._close_text_input()
        self.update()

    def _finish(self, action: str) -> None:
        if getattr(self, "_finish_sent", False):
            return
        self._finish_sent = True
        pixmap = self._crop_selection() if action != "cancel" else None
        self._release_input_grab()
        self._hide_toolbar()
        if self._toolbar is not None:
            self._toolbar.close()
            self._toolbar.deleteLater()
            self._toolbar = None
        self._close_text_input(reclaim=False)
        self._desktop = QPixmap()
        self.hide()
        self.finished.emit(pixmap, action)

    def closeEvent(self, event) -> None:
        # Alt+F4 / external close must still emit finished — otherwise
        # ScreenshotManager keeps a stale _overlay and F1 silently no-ops.
        if not getattr(self, "_finish_sent", False):
            try:
                self._finish("cancel")
            except Exception:
                self._release_input_grab()
                self._hide_toolbar()
                if self._toolbar is not None:
                    try:
                        self._toolbar.close()
                        self._toolbar.deleteLater()
                    except RuntimeError:
                        pass
                    self._toolbar = None
                self._close_text_input(reclaim=False)
        event.accept()

    def _on_action(self, action: str) -> None:
        self._finish(action)

    def _reset_annotation_state(self) -> None:
        self._close_text_input()
        self._annotations.clear()
        self._draft_annotation = None
        self._tool = ""
        if self._toolbar is not None:
            self._toolbar.set_active_tool("")

    def _inside_active_rect(self, pos: QPoint) -> bool:
        rect = self._active_rect()
        return rect is not None and rect.contains(pos)

    def _annotation_base(self, kind: str, pos: QPoint) -> dict:
        return {
            "kind": kind,
            "start": pos,
            "end": pos,
            "points": [QPoint(pos)],
            "color": QColor(_COLORS[self._color_name]),
            "width": self._stroke_width,
        }

    def _update_hover_window(self, pos: QPoint) -> None:
        """WeChat-style: highlight the smallest pre-snapshotted window under cursor."""
        if self._selection is not None or self._start is not None:
            if self._hover_rect is not None:
                self._hover_rect = None
                self.update()
            return
        # First move samples arm position only — no chrome yet (matches WeChat).
        if not self._hover_ready:
            if self._hover_arm_pos is None:
                self._hover_arm_pos = QPoint(pos)
                return
            if (pos - self._hover_arm_pos).manhattanLength() < 3:
                return
            self._hover_ready = True
        from src.snip_window_regions import hit_test_snip_window

        hit = hit_test_snip_window(self._window_rects, pos)
        if hit == self._hover_rect:
            return
        self._hover_rect = hit
        self.update()

    def _commit_selection(self, rect: QRect | None, *, snap: bool = False) -> None:
        if rect is None or rect.width() < 2 or rect.height() < 2:
            self._start = None
            self._current = None
            self._selection = None
            self._hover_rect = None
            self.update()
            return
        final = QRect(rect).normalized()
        if snap and self._window_rects:
            from src.snip_window_regions import expand_rect_to_nearby_window

            final = expand_rect_to_nearby_window(final, self._window_rects)
            final = final.intersected(self._logical_desktop_rect())
        self._selection = final
        self._start = final.topLeft()
        self._current = final.bottomRight()
        self._hover_rect = None
        self.update()
        self._show_toolbar(final)

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, False)
        painter.drawPixmap(self._logical_desktop_rect(), self._desktop)

        pending_click = (
            self._start is not None
            and self._selection is None
            and self._current is not None
            and (self._current - self._start).manhattanLength() < self._CLICK_DRAG_SLOP
        )

        rect: QRect | None = None
        show_annotations = False
        committed_or_drag = False
        if self._selection is not None:
            rect = self._selection
            show_annotations = True
            committed_or_drag = True
        elif self._start is not None and not pending_click:
            rect = self._selection_rect()
            committed_or_drag = True
        elif pending_click:
            rect = self._press_hover or self._hover_rect
        elif self._hover_rect is not None:
            rect = self._hover_rect

        if rect is not None and rect.width() > 0 and rect.height() > 0:
            self._draw_mask_outside(painter, rect)
            if committed_or_drag:
                self._draw_selection_chrome(painter, rect)
            else:
                # Recognized window hover / click-pending: border only.
                self._draw_hover_chrome(painter, rect)
            if show_annotations:
                for annotation in self._annotations:
                    self._draw_annotation(painter, annotation)
                if self._draft_annotation is not None:
                    self._draw_annotation(painter, self._draft_annotation)
            return

        # Default: dim only — no selection frame until a window is recognized.
        painter.fillRect(self.rect(), QColor(22, 26, 32, 72))

    def mousePressEvent(self, event) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        pos = event.position().toPoint()
        if self._selection is not None and self._inside_active_rect(pos):
            if self._tool in ("rect", "ellipse", "arrow"):
                self._draft_annotation = self._annotation_base(self._tool, pos)
                return
            if self._tool in ("brush", "mosaic"):
                self._draft_annotation = self._annotation_base(self._tool, pos)
                return
            if self._tool == "emoji":
                self._annotations.append(
                    {
                        "kind": "emoji",
                        "start": pos,
                        "end": pos,
                        "text": _EMOJI_CHOICES[self._emoji_index % len(_EMOJI_CHOICES)],
                        "color": QColor(_COLORS[self._color_name]),
                        "width": self._stroke_width,
                    }
                )
                self._emoji_index += 1
                self.update()
                return
            if self._tool == "text":
                self._begin_inline_text(pos)
                return

        self._hide_toolbar()
        self._reset_annotation_state()
        from src.snip_window_regions import hit_test_snip_window

        self._press_hover = self._hover_rect or hit_test_snip_window(
            self._window_rects, pos
        )
        self._start = pos
        self._current = pos
        self._selection = None
        self.update()

    def mouseMoveEvent(self, event) -> None:
        pos = event.position().toPoint()
        if self._draft_annotation is not None:
            kind = self._draft_annotation.get("kind")
            if kind in ("brush", "mosaic"):
                points = self._draft_annotation.get("points", [])
                if not points or (pos - points[-1]).manhattanLength() >= (
                    8 if kind == "mosaic" else 2
                ):
                    points.append(QPoint(pos))
                self._draft_annotation["end"] = pos
            else:
                self._draft_annotation["end"] = pos
            self.update()
            return
        if self._start is not None and self._selection is None:
            self._current = pos
            self.update()
            return
        self._update_hover_window(pos)

    def mouseReleaseEvent(self, event) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        if self._draft_annotation is not None:
            self._draft_annotation["end"] = event.position().toPoint()
            kind = self._draft_annotation.get("kind")
            if kind in ("brush", "mosaic"):
                points = self._draft_annotation.get("points", [])
                if len(points) >= 2:
                    self._annotations.append(self._draft_annotation)
            else:
                start = self._draft_annotation["start"]
                end = self._draft_annotation["end"]
                if abs(start.x() - end.x()) + abs(start.y() - end.y()) >= 6:
                    self._annotations.append(self._draft_annotation)
            self._draft_annotation = None
            self.update()
            return
        if self._start is None or self._selection is not None:
            return
        self._current = event.position().toPoint()
        drag = (self._current - self._start).manhattanLength()
        # Click (no drag): select hovered window if any (WeChat 单击窗).
        if drag < self._CLICK_DRAG_SLOP:
            hover = self._press_hover or self._hover_rect
            self._press_hover = None
            if hover is not None:
                self._commit_selection(hover, snap=False)
                return
            self._start = None
            self._current = None
            self._selection = None
            self.update()
            return
        self._press_hover = None
        # Free drag keeps the user's rectangle (WeChat). Do NOT expand to the
        # host window — that made partial selections jump to full app bounds.
        rect = self._selection_rect()
        self._commit_selection(rect, snap=False)

    def mouseDoubleClickEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton and self._active_rect():
            self._finish("copy")
            return
        super().mouseDoubleClickEvent(event)

    def keyPressEvent(self, event) -> None:
        if self._text_input is not None and self._text_input.isVisible():
            super().keyPressEvent(event)
            return

        if event.key() == Qt.Key.Key_Escape:
            self._finish("cancel")
            return
        if event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            if self._active_rect() and self._active_rect().width() >= 2 and self._active_rect().height() >= 2:
                self._finish("copy")
            return
        if event.matches(QKeySequence.StandardKey.Undo):
            self._undo()
            return
        if event.matches(QKeySequence.StandardKey.Copy):
            if self._active_rect():
                self._finish("copy")
            return
        if event.matches(QKeySequence.StandardKey.Save):
            if self._active_rect():
                self._finish("save")
            return
        super().keyPressEvent(event)
