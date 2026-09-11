"""Desktop sticky todo panel."""

from __future__ import annotations

from PyQt6.QtCore import QEvent, QPoint, QRect, Qt, pyqtSignal
from PyQt6.QtGui import QFont
from PyQt6.QtWidgets import (
    QCheckBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from src.settings import save_settings
from src.todos import (
    add_todo,
    delete_todo,
    load_todos,
    toggle_todo,
    update_todo_text,
)
from src.ui.screen_snap import rect_visible_on_any_screen, snap_geometry, work_screen
from src.ui.styles import get_theme_palette, normalize_theme, _hex_to_rgba
from src.win_shell import configure_desktop_overlay

_DEFAULT_W = 280
_MIN_W = 220
_MAX_W = 420
_DRAG_THRESHOLD = 8
_SNAP_THRESHOLD = 28
# Slight translucency so wallpaper shows through (sticky-note feel).
_CARD_ALPHA = 0.88
_TITLE_ALPHA = 0.82
_HOVER_ALPHA = 0.55
_BORDER_ALPHA = 0.55


class _TodoRow(QFrame):
    toggled = pyqtSignal(str)
    deleted = pyqtSignal(str)
    text_committed = pyqtSignal(str, str)

    def __init__(self, item: dict, palette: dict[str, str], parent: QWidget | None = None):
        super().__init__(parent)
        self.item_id = str(item.get("id") or "")
        self.setObjectName("todoRow")
        self.setCursor(Qt.CursorShape.ArrowCursor)

        row = QHBoxLayout(self)
        row.setContentsMargins(8, 4, 4, 4)
        row.setSpacing(6)

        self.check = QCheckBox()
        self.check.setChecked(bool(item.get("done")))
        self.check.setToolTip("标记完成 / 取消完成")
        self.check.clicked.connect(lambda: self.toggled.emit(self.item_id))
        row.addWidget(self.check, 0, Qt.AlignmentFlag.AlignTop)

        self.edit = QLineEdit(str(item.get("text") or ""))
        self.edit.setObjectName("todoEdit")
        self.edit.setPlaceholderText("输入待办…")
        self.edit.setFrame(False)
        self.edit.editingFinished.connect(self._commit)
        self.edit.returnPressed.connect(self._commit_and_clear_focus)
        row.addWidget(self.edit, stretch=1)

        self.del_btn = QPushButton("×")
        self.del_btn.setObjectName("todoDeleteBtn")
        self.del_btn.setFixedSize(22, 22)
        self.del_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self.del_btn.setToolTip("删除")
        self.del_btn.clicked.connect(lambda: self.deleted.emit(self.item_id))
        row.addWidget(self.del_btn, 0, Qt.AlignmentFlag.AlignTop)

        self.apply_done_style(bool(item.get("done")), palette)

    def apply_done_style(self, done: bool, palette: dict[str, str]) -> None:
        muted = palette.get("text_muted", "#64748B")
        text = palette.get("text", "#1A2332")
        font = self.edit.font()
        font.setStrikeOut(done)
        self.edit.setFont(font)
        color = muted if done else text
        self.edit.setStyleSheet(f"color: {color}; background: transparent;")

    def _commit(self) -> None:
        self.text_committed.emit(self.item_id, self.edit.text())

    def _commit_and_clear_focus(self) -> None:
        self._commit()
        self.edit.clearFocus()


class DesktopTodoWidget(QWidget):
    """Frameless desktop sticky for todos — add / delete / complete by mouse."""

    def __init__(self, settings: dict | None = None, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings if isinstance(settings, dict) else {}
        self._items: list[dict] = []
        self._drag_offset: QPoint | None = None
        self._press_global: QPoint | None = None
        self._dragging = False
        self._rows: list[_TodoRow] = []

        self.setObjectName("desktopTodoPanel")
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint | Qt.WindowType.Window
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
        self.setMinimumWidth(_MIN_W)
        self.setMaximumWidth(_MAX_W)

        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        self._card = QFrame()
        self._card.setObjectName("todoCard")
        card_layout = QVBoxLayout(self._card)
        card_layout.setContentsMargins(0, 0, 0, 0)
        card_layout.setSpacing(0)

        self._title_bar = QFrame()
        self._title_bar.setObjectName("todoTitleBar")
        self._title_bar.setCursor(Qt.CursorShape.SizeAllCursor)
        title_row = QHBoxLayout(self._title_bar)
        title_row.setContentsMargins(12, 8, 8, 8)
        title_row.setSpacing(6)
        self._title = QLabel("待办")
        self._title.setObjectName("todoTitle")
        title_font = QFont()
        title_font.setBold(True)
        title_font.setPointSize(11)
        self._title.setFont(title_font)
        title_row.addWidget(self._title, stretch=1)

        self._count = QLabel("")
        self._count.setObjectName("todoCount")
        title_row.addWidget(self._count)

        self._hide_btn = QPushButton("−")
        self._hide_btn.setObjectName("todoHideBtn")
        self._hide_btn.setFixedSize(24, 24)
        self._hide_btn.setToolTip("暂时隐藏（分页栏「待办」可再次显示）")
        self._hide_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._hide_btn.clicked.connect(self.hide)
        title_row.addWidget(self._hide_btn)
        card_layout.addWidget(self._title_bar)

        self._scroll = QScrollArea()
        self._scroll.setObjectName("todoScroll")
        self._scroll.setWidgetResizable(True)
        self._scroll.setFrameShape(QFrame.Shape.NoFrame)
        self._scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self._scroll.setMaximumHeight(320)
        self._list_host = QWidget()
        self._list_host.setObjectName("todoListHost")
        self._list_layout = QVBoxLayout(self._list_host)
        self._list_layout.setContentsMargins(6, 4, 6, 4)
        self._list_layout.setSpacing(2)
        self._list_layout.addStretch(1)
        self._scroll.setWidget(self._list_host)
        card_layout.addWidget(self._scroll, stretch=1)

        self._add_btn = QPushButton("+ 添加待办事项")
        self._add_btn.setObjectName("todoAddBtn")
        self._add_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._add_btn.setMinimumHeight(34)
        self._add_btn.clicked.connect(self._on_add_clicked)
        card_layout.addWidget(self._add_btn)

        root.addWidget(self._card)

        self._title_bar.installEventFilter(self)
        self._title.installEventFilter(self)
        self._count.installEventFilter(self)

        self.refresh_theme()
        self.reload()
        self._restore_or_default_position()

    def refresh_theme(self) -> None:
        theme = normalize_theme(self.settings.get("theme") if self.settings else None)
        p = get_theme_palette(theme)
        self._palette = p
        card_bg = _hex_to_rgba(p["card"], _CARD_ALPHA)
        title_bg = _hex_to_rgba(p["accent_soft"], _TITLE_ALPHA)
        hover_bg = _hex_to_rgba(p["accent_soft"], _HOVER_ALPHA)
        border = _hex_to_rgba(p["border"], _BORDER_ALPHA)
        self.setStyleSheet(
            f"""
            QFrame#todoCard {{
                background: {card_bg};
                border: 1px solid {border};
                border-radius: 12px;
            }}
            QFrame#todoTitleBar {{
                background: {title_bg};
                border-top-left-radius: 12px;
                border-top-right-radius: 12px;
                border-bottom: 1px solid {border};
            }}
            QLabel#todoTitle {{ color: {p['text']}; background: transparent; }}
            QLabel#todoCount {{ color: {p['text_muted']}; font-size: 11px; background: transparent; }}
            QPushButton#todoHideBtn {{
                background: transparent;
                color: {p['text_muted']};
                border: none;
                border-radius: 6px;
                font-size: 16px;
                font-weight: 700;
            }}
            QPushButton#todoHideBtn:hover {{
                background: {hover_bg};
                color: {p['text']};
            }}
            QScrollArea#todoScroll, QWidget#todoListHost {{
                background: transparent;
                border: none;
            }}
            QFrame#todoRow {{
                background: transparent;
                border-radius: 8px;
            }}
            QFrame#todoRow:hover {{
                background: {hover_bg};
            }}
            QLineEdit#todoEdit {{
                border: none;
                background: transparent;
                padding: 2px 0;
                font-size: 13px;
            }}
            QPushButton#todoDeleteBtn {{
                background: transparent;
                color: {p['text_muted']};
                border: none;
                border-radius: 6px;
                font-size: 14px;
                font-weight: 700;
            }}
            QPushButton#todoDeleteBtn:hover {{
                background: rgba(254, 226, 226, 0.85);
                color: #B91C1C;
            }}
            QPushButton#todoAddBtn {{
                background: transparent;
                color: {p['accent']};
                border: none;
                border-top: 1px solid {border};
                border-bottom-left-radius: 12px;
                border-bottom-right-radius: 12px;
                font-size: 13px;
                font-weight: 600;
                padding: 8px 12px;
                text-align: left;
            }}
            QPushButton#todoAddBtn:hover {{
                background: {hover_bg};
            }}
            """
        )
        for row in self._rows:
            row.apply_done_style(row.check.isChecked(), p)

    def reload(self) -> None:
        self._items = load_todos()
        self._rebuild_rows()

    def _rebuild_rows(self) -> None:
        while self._list_layout.count():
            item = self._list_layout.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()
        self._rows.clear()

        # Unfinished first; completed items sink below.
        pending = [i for i in self._items if not i.get("done")]
        done = [i for i in self._items if i.get("done")]
        ordered = pending + done
        for item in ordered:
            row = _TodoRow(item, getattr(self, "_palette", get_theme_palette(None)), self._list_host)
            row.toggled.connect(self._on_toggle)
            row.deleted.connect(self._on_delete)
            row.text_committed.connect(self._on_text_committed)
            self._list_layout.addWidget(row)
            self._rows.append(row)
        self._list_layout.addStretch(1)

        open_n = len(pending)
        self._count.setText(f"{open_n} 项" if open_n else "")
        self._add_btn.setText("+ 添加待办事项" if ordered else "+ 添加第一条待办")
        self.adjustSize()
        # Keep width preference.
        cfg = self.settings.setdefault("desktop_todos", {})
        if isinstance(cfg, dict):
            w = int(cfg.get("width") or _DEFAULT_W)
            self.resize(max(_MIN_W, min(_MAX_W, w)), self.sizeHint().height())

    def _on_toggle(self, item_id: str) -> None:
        self._items = toggle_todo(item_id, self._items)
        self._rebuild_rows()

    def _on_delete(self, item_id: str) -> None:
        self._items = delete_todo(item_id, self._items)
        self._rebuild_rows()

    def _on_text_committed(self, item_id: str, text: str) -> None:
        self._items = update_todo_text(item_id, text, self._items)
        # Avoid full rebuild while typing focus; only rebuild if empty removed.
        if not str(text or "").strip():
            self._rebuild_rows()
        else:
            for row in self._rows:
                if row.item_id == item_id:
                    row.apply_done_style(row.check.isChecked(), self._palette)
                    break

    def _on_add_clicked(self) -> None:
        self._items = add_todo("", self._items)
        self._rebuild_rows()
        # Focus the new empty row (first unfinished empty, else last pending).
        for row in self._rows:
            if not row.edit.text().strip() and not row.check.isChecked():
                row.edit.setFocus(Qt.FocusReason.MouseFocusReason)
                row.edit.selectAll()
                return
        if self._rows:
            self._rows[0].edit.setFocus(Qt.FocusReason.MouseFocusReason)

    def _default_position(self) -> QPoint:
        screen = work_screen()
        if not screen:
            return QPoint(80, 120)
        geo = screen.availableGeometry()
        self.adjustSize()
        # Default: left side of the work area.
        x = geo.x() + 48
        y = geo.y() + max(80, (geo.height() - self.height()) // 4)
        return QPoint(x, y)

    def _restore_or_default_position(self) -> None:
        self.adjustSize()
        cfg = self.settings.get("desktop_todos")
        saved = cfg.get("pos") if isinstance(cfg, dict) else None
        w = int(cfg.get("width") or _DEFAULT_W) if isinstance(cfg, dict) else _DEFAULT_W
        self.resize(max(_MIN_W, min(_MAX_W, w)), self.sizeHint().height())
        if isinstance(saved, dict) and "x" in saved and "y" in saved:
            geo = QRect(int(saved["x"]), int(saved["y"]), self.width(), self.height())
            if rect_visible_on_any_screen(geo):
                snapped = snap_geometry(geo, _SNAP_THRESHOLD)
                self.move(snapped.topLeft())
                return
        self.move(self._default_position())
        self._persist_geometry()

    def _persist_geometry(self) -> None:
        cfg = self.settings.setdefault("desktop_todos", {})
        if not isinstance(cfg, dict):
            cfg = {}
            self.settings["desktop_todos"] = cfg
        pos = self.pos()
        cfg["pos"] = {"x": int(pos.x()), "y": int(pos.y())}
        cfg["width"] = int(self.width())
        try:
            save_settings(self.settings)
        except OSError:
            pass

    def eventFilter(self, obj, event):  # noqa: ANN001
        if obj in (self._title_bar, self._title, self._count):
            if (
                event.type() == QEvent.Type.MouseButtonPress
                and event.button() == Qt.MouseButton.LeftButton
            ):
                self._begin_drag(event.globalPosition().toPoint())
                return True
            if (
                event.type() == QEvent.Type.MouseMove
                and event.buttons() & Qt.MouseButton.LeftButton
            ):
                if self._update_drag(event.globalPosition().toPoint()):
                    return True
            if (
                event.type() == QEvent.Type.MouseButtonRelease
                and event.button() == Qt.MouseButton.LeftButton
            ):
                self._end_drag()
                return True
        return super().eventFilter(obj, event)

    def _begin_drag(self, global_pos: QPoint) -> None:
        self._press_global = global_pos
        self._drag_offset = global_pos - self.frameGeometry().topLeft()
        self._dragging = False

    def _update_drag(self, global_pos: QPoint) -> bool:
        if self._drag_offset is None or self._press_global is None:
            return False
        if not self._dragging:
            if (global_pos - self._press_global).manhattanLength() < _DRAG_THRESHOLD:
                return False
            self._dragging = True
        top_left = global_pos - self._drag_offset
        snapped = snap_geometry(
            QRect(top_left.x(), top_left.y(), self.width(), self.height()),
            _SNAP_THRESHOLD,
        )
        self.move(snapped.topLeft())
        return True

    def _end_drag(self) -> None:
        if self._dragging:
            snapped = snap_geometry(self.geometry(), _SNAP_THRESHOLD)
            self.setGeometry(snapped)
            self._persist_geometry()
        self._drag_offset = None
        self._press_global = None
        self._dragging = False

    def showEvent(self, event) -> None:  # noqa: N802
        super().showEvent(event)
        configure_desktop_overlay(self, peek=False)

    def sizeHint(self):  # noqa: ANN001
        from PyQt6.QtCore import QSize

        h = 48 + 36 + max(40, min(320, 8 + len(self._rows) * 34))
        return QSize(_DEFAULT_W, h)
