"""Global filename search overlay powered by bundled fd."""

from __future__ import annotations


def _nudge_font(font, *, delta: int = -1, floor: int = 8):
    """Avoid QFont::setPointSize(-1) when the font is pixel-sized."""
    ps = int(font.pointSize() or 0)
    if ps > 0:
        font.setPointSize(max(floor, ps + delta))
        return font
    px = int(font.pixelSize() or 0)
    if px > 0:
        font.setPixelSize(max(floor, px + delta))
        return font
    font.setPointSize(max(floor, 9 + delta))
    return font

import threading
from pathlib import Path

from PyQt6.QtCore import QEvent, QObject, QPoint, Qt, QThread, QTimer, pyqtSignal
from PyQt6.QtGui import QFont, QFontMetrics, QGuiApplication, QKeyEvent, QMouseEvent
from PyQt6.QtWidgets import (
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from src.fd_locator import find_fd
from src.fd_search import (
    MIN_QUERY_CHARS,
    cancel_active_searches,
    search_config_slice,
    search_filenames,
)
from src.ui.toast import show_toast
from src.win_shell import open_path, reveal_in_explorer


class _DragTitleBar(QWidget):
    """Dedicated title strip — reliable frameless window drag on Windows."""

    def __init__(self, host: "FileSearchOverlay", parent=None) -> None:
        super().__init__(parent)
        self._host = host
        self._drag_pos: QPoint | None = None
        self.setObjectName("fileSearchTitleBar")
        self.setCursor(Qt.CursorShape.SizeAllCursor)

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_pos = (
                event.globalPosition().toPoint() - self._host.frameGeometry().topLeft()
            )
            self.grabMouse()
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if self._drag_pos is not None and event.buttons() & Qt.MouseButton.LeftButton:
            top_left = event.globalPosition().toPoint() - self._drag_pos
            self._host.move(self._host._clamp_to_screen(top_left))
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if self._drag_pos is not None:
            self._host._remember_position()
            self._drag_pos = None
            if self.mouseGrabber() is self:
                self.releaseMouse()
            event.accept()
            return
        super().mouseReleaseEvent(event)


class _SearchWorker(QThread):
    finished_ok = pyqtSignal(object)  # list[Path]
    finished_err = pyqtSignal(str)

    def __init__(
        self,
        query: str,
        settings_slice: dict,
        cancel_event: threading.Event,
        parent=None,
    ) -> None:
        super().__init__(parent)
        self._query = query
        self._settings = settings_slice
        self._cancel = cancel_event

    def run(self) -> None:
        try:
            if self._cancel.is_set():
                self.finished_ok.emit([])
                return
            paths = search_filenames(
                self._query,
                settings=self._settings,
                cancel_event=self._cancel,
            )
            if self._cancel.is_set():
                self.finished_ok.emit([])
                return
            self.finished_ok.emit(paths)
        except FileNotFoundError:
            self.finished_err.emit("missing_fd")
        except Exception as exc:  # noqa: BLE001 — surface to UI
            if self._cancel.is_set():
                self.finished_ok.emit([])
                return
            self.finished_err.emit(str(exc) or "search_failed")


class _SearchCard(QWidget):
    """Themed panel body."""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)


class _SearchInputFilter(QObject):
    """Forward navigation keys from the line edit; ensure digits reach the query."""

    _NAV_KEYS = frozenset(
        {
            Qt.Key.Key_Escape,
            Qt.Key.Key_Return,
            Qt.Key.Key_Enter,
            Qt.Key.Key_Up,
            Qt.Key.Key_Down,
        }
    )
    _EDIT_KEYS = frozenset(
        {
            Qt.Key.Key_Backspace,
            Qt.Key.Key_Delete,
            Qt.Key.Key_Left,
            Qt.Key.Key_Right,
            Qt.Key.Key_Home,
            Qt.Key.Key_End,
        }
    )

    def __init__(self, host: "FileSearchOverlay") -> None:
        super().__init__(host)
        self._host = host

    def eventFilter(self, watched, event) -> bool:  # noqa: N802
        if watched is not self._host._input or event.type() != QEvent.Type.KeyPress:
            return super().eventFilter(watched, event)
        if event.key() in self._EDIT_KEYS:
            return False
        if self._host._try_force_digit_input(event):
            return True
        if event.key() in self._NAV_KEYS:
            self._host._handle_nav_key(event)
            return event.isAccepted()
        return False


class FileSearchOverlay(QWidget):
    """Frameless search box; drag title bar, Esc closes, Enter opens, Ctrl+Enter reveals."""

    def __init__(self, settings: dict, parent=None) -> None:
        super().__init__(parent)
        self.settings = settings
        self._worker: _SearchWorker | None = None
        self._cancel: threading.Event | None = None
        self._req_id = 0
        self._debounce = QTimer(self)
        self._debounce.setSingleShot(True)
        self._debounce.setInterval(420)
        self._debounce.timeout.connect(self._run_search)
        self._missing_tip_shown = False
        self._last_query: str = ""
        self._last_scope_global: bool | None = None
        self._search_all_drives = False
        self._fd_ready: bool | None = None
        self._saved_pos: QPoint | None = None

        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.Tool
            | Qt.WindowType.WindowStaysOnTopHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setObjectName("fileSearchOverlay")

        self.setFixedWidth(560)
        self.setMinimumHeight(120)
        self.setMaximumHeight(520)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        self._card = _SearchCard(self)
        self._card.setObjectName("fileSearchCard")
        self._card.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        root = QVBoxLayout(self._card)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        self._title_bar = _DragTitleBar(self)
        header = QHBoxLayout(self._title_bar)
        header.setContentsMargins(16, 12, 10, 10)
        header.setSpacing(8)

        self._title = QLabel("文件搜索")
        self._title.setObjectName("fileSearchTitle")
        self._mark_label_draggable(self._title)
        header.addWidget(self._title)
        header.addStretch(1)

        self._close_btn = QPushButton("×")
        self._close_btn.setObjectName("fileSearchCloseBtn")
        self._close_btn.setFixedSize(28, 28)
        self._close_btn.setFlat(True)
        self._close_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._close_btn.setToolTip("关闭 (Esc)")
        self._close_btn.clicked.connect(self.close_overlay)
        header.addWidget(self._close_btn)
        root.addWidget(self._title_bar)

        body = QVBoxLayout()
        body.setContentsMargins(16, 0, 16, 14)
        body.setSpacing(10)

        self._input = QLineEdit()
        self._input.setObjectName("fileSearchInput")
        self._input.setPlaceholderText(
            f"至少 {MIN_QUERY_CHARS} 个字符… Enter 打开，Ctrl+Enter 定位"
        )
        self._input.textChanged.connect(self._on_text_changed)
        self._input.installEventFilter(_SearchInputFilter(self))
        body.addWidget(self._input)

        scope_row = QHBoxLayout()
        scope_row.setSpacing(8)
        self._scope_label = QLabel("范围：桌面 · 文档 · 下载")
        self._scope_label.setObjectName("fileSearchScope")
        self._mark_label_draggable(self._scope_label)
        scope_row.addWidget(self._scope_label)
        scope_row.addStretch(1)
        self._global_btn = QPushButton("全局搜索")
        self._global_btn.setObjectName("fileSearchGlobalBtn")
        self._global_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._global_btn.setToolTip("用当前关键词在所有本地磁盘中搜索（较慢）")
        self._global_btn.clicked.connect(self._run_global_search)
        self._global_btn.hide()
        scope_row.addWidget(self._global_btn)
        body.addLayout(scope_row)

        self._status = QLabel("")
        self._status.setObjectName("fileSearchStatus")
        self._mark_label_draggable(self._status)
        body.addWidget(self._status)

        self._list = QListWidget()
        self._list.setObjectName("fileSearchList")
        self._list.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._list.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self._list.customContextMenuRequested.connect(self._show_result_menu)
        self._list.itemActivated.connect(self._open_item)
        self._list.currentItemChanged.connect(self._on_selection_changed)
        body.addWidget(self._list, stretch=1)

        self._location = QLabel("")
        self._location.setObjectName("fileSearchLocation")
        self._location.setWordWrap(False)
        self._mark_label_draggable(self._location)
        body.addWidget(self._location)

        hint_row = QHBoxLayout()
        self._hint = QLabel("Esc 关闭 · ↑↓ 选择 · 右键打开位置 · 拖动标题栏移动")
        self._hint.setObjectName("fileSearchHint")
        self._mark_label_draggable(self._hint)
        hint_row.addWidget(self._hint)
        hint_row.addStretch(1)
        body.addLayout(hint_row)

        root.addLayout(body)
        outer.addWidget(self._card)
        font = QFont(self.font())
        font = _nudge_font(font, delta=0, floor=10)
        self._list.setFont(font)

    @staticmethod
    def _mark_label_draggable(label: QLabel) -> None:
        label.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)

    def refresh_theme(self) -> None:
        """Re-polish after QApplication theme sheet changes."""
        # Local sheets override the app theme with stale colors — always drop them.
        self.setStyleSheet("")
        self._card.setStyleSheet("")
        self._repolish_tree()

    def _repolish_tree(self) -> None:
        seen: set[int] = set()
        for widget in (self, self._card):
            self._repolish_widget(widget, seen)
        for child in self._card.findChildren(QWidget):
            self._repolish_widget(child, seen)
        self.update()

    @staticmethod
    def _repolish_widget(widget: QWidget | None, seen: set[int]) -> None:
        if widget is None:
            return
        key = id(widget)
        if key in seen:
            return
        seen.add(key)
        style = widget.style()
        style.unpolish(widget)
        style.polish(widget)
        widget.update()

    def _run_global_search(self) -> None:
        """One-shot: re-run the current query across all local drives."""
        query = self._input.text().strip()
        if len(query) < MIN_QUERY_CHARS:
            return
        self._search_all_drives = True
        self._update_scope_label()
        self._global_btn.hide()
        self._last_scope_global = None
        self._run_search(force=True)

    @staticmethod
    def _digit_from_key(key: int) -> str | None:
        # PyQt6 maps numpad digits to Key_0..Key_9 (no Keypad0 enum).
        if Qt.Key.Key_0 <= key <= Qt.Key.Key_9:
            return chr(key - Qt.Key.Key_0 + ord("0"))
        return None

    def _try_force_digit_input(self, event: QKeyEvent) -> bool:
        """Insert 0-9 when IME would consume them for candidate selection."""
        if event.modifiers() & (
            Qt.KeyboardModifier.ControlModifier
            | Qt.KeyboardModifier.AltModifier
            | Qt.KeyboardModifier.MetaModifier
        ):
            return False
        ch = self._digit_from_key(event.key())
        if ch is None:
            return False
        if event.text() == ch:
            return False
        im = QGuiApplication.inputMethod()
        if im is not None:
            im.reset()
        self._input.insert(ch)
        event.accept()
        return True

    def _update_scope_label(self) -> None:
        if self._search_all_drives:
            self._scope_label.setText("范围：所有本地磁盘")
            return
        from src.fd_search import open_explorer_folder_paths

        n_open = len(open_explorer_folder_paths())
        if n_open:
            self._scope_label.setText(f"范围：桌面 · 文档 · 下载 · 已打开文件夹({n_open})")
        else:
            self._scope_label.setText("范围：桌面 · 文档 · 下载")

    def _search_settings_slice(self) -> dict:
        return search_config_slice(
            self.settings,
            search_all_drives=self._search_all_drives,
        )

    def _screen_geo(self):
        screen = QGuiApplication.screenAt(self.frameGeometry().center())
        if screen is None:
            screen = QGuiApplication.primaryScreen()
        return screen.availableGeometry() if screen else None

    def _clamp_to_screen(self, top_left: QPoint) -> QPoint:
        geo = self._screen_geo()
        if geo is None:
            return top_left
        x = max(geo.x(), min(top_left.x(), geo.x() + geo.width() - self.width()))
        y = max(geo.y(), min(top_left.y(), geo.y() + geo.height() - self.height()))
        return QPoint(x, y)

    def _remember_position(self) -> None:
        self._saved_pos = self.pos()

    def show_centered(self) -> None:
        self.refresh_theme()
        self.setFixedHeight(380)
        from src.fd_locator import clear_fd_cache

        clear_fd_cache()
        self._fd_ready = None
        self._missing_tip_shown = False
        self._search_all_drives = False
        self._last_scope_global = None
        self._global_btn.hide()
        self._update_scope_label()
        self._list.clear()
        self._status.setText("")
        self._location.setText("")
        self._last_query = ""
        self._input.blockSignals(True)
        self._input.clear()
        self._input.blockSignals(False)
        if self._saved_pos is not None:
            self.move(self._clamp_to_screen(self._saved_pos))
        else:
            geo = self._screen_geo()
            w = self.width()
            if geo is not None:
                x = geo.x() + (geo.width() - w) // 2
                y = geo.y() + max(40, geo.height() // 5)
                self.move(x, y)
        self.show()
        self.raise_()
        self.activateWindow()
        self._input.setFocus(Qt.FocusReason.ActiveWindowFocusReason)

    def close_overlay(self) -> None:
        self._debounce.stop()
        self._cancel_worker()
        if self._title_bar.mouseGrabber() is self._title_bar:
            self._title_bar.releaseMouse()
        self.hide()
        self._list.clear()
        self._status.setText("")
        self._location.setText("")
        self._input.blockSignals(True)
        self._input.clear()
        self._input.blockSignals(False)
        self._last_query = ""
        self._last_scope_global = None
        self._search_all_drives = False
        self._global_btn.hide()
        self._update_scope_label()

    def _cancel_worker(self) -> None:
        if self._cancel is not None:
            self._cancel.set()
        cancel_active_searches()
        worker = self._worker
        self._worker = None
        if worker is not None:
            try:
                worker.finished_ok.disconnect()
            except (TypeError, RuntimeError):
                pass
            try:
                worker.finished_err.disconnect()
            except (TypeError, RuntimeError):
                pass
            if worker.isRunning():
                worker.wait(0)

    def _on_text_changed(self, _text: str) -> None:
        # Typing resets to local scope; global is a one-shot via the button.
        if self._search_all_drives:
            self._search_all_drives = False
            self._update_scope_label()
        self._global_btn.hide()
        self._last_scope_global = None
        self._debounce.start()

    def _run_search(self, *, force: bool = False) -> None:
        query = self._input.text().strip()
        if not query:
            self._last_query = ""
            self._last_scope_global = None
            self._list.clear()
            self._status.setText("")
            self._location.setText("")
            self._global_btn.hide()
            return
        if len(query) < MIN_QUERY_CHARS:
            self._list.clear()
            self._location.setText("")
            self._global_btn.hide()
            self._status.setText(f"再输入 {MIN_QUERY_CHARS - len(query)} 个字符开始搜索")
            return
        scope_key = self._search_all_drives
        if (
            not force
            and query == self._last_query
            and scope_key == self._last_scope_global
            and self._list.count() > 0
        ):
            return
        if self._fd_ready is None:
            self._fd_ready = find_fd() is not None
        if not self._fd_ready:
            self._status.setText("未找到 fd.exe")
            self._location.setText("")
            if not self._missing_tip_shown:
                self._missing_tip_shown = True
                show_toast(
                    "文件搜索不可用",
                    "安装版请确认程序目录下有 assets\\fd\\fd.exe。",
                )
            return
        self._last_query = query
        self._last_scope_global = scope_key
        if self._search_all_drives:
            self._status.setText("全局搜索中…")
        else:
            self._status.setText("搜索中…")
        self._location.setText("")
        self._req_id += 1
        req = self._req_id
        self._cancel_worker()
        cancel = threading.Event()
        self._cancel = cancel
        worker = _SearchWorker(query, self._search_settings_slice(), cancel, self)
        self._worker = worker

        def _ok(paths: object, rid: int = req) -> None:
            if rid != self._req_id:
                return
            self._fill_results(list(paths or []))

        def _err(msg: str, rid: int = req) -> None:
            if rid != self._req_id:
                return
            if msg == "missing_fd":
                self._fd_ready = False
                self._status.setText("未找到 fd.exe")
            else:
                self._status.setText(f"搜索失败：{msg[:80]}")
            self._location.setText("")
            self._global_btn.hide()

        worker.finished_ok.connect(_ok)
        worker.finished_err.connect(_err)
        worker.start()

    def _fill_results(self, paths: list[Path]) -> None:
        self._list.clear()
        if not paths:
            self._status.setText("无匹配结果")
            self._location.setText("")
            # Still offer global when local miss — that is the main use case.
            self._global_btn.setVisible(not self._search_all_drives)
            self._input.setFocus(Qt.FocusReason.OtherFocusReason)
            return
        scope_note = "（全局）" if self._search_all_drives else ""
        self._status.setText(f"{len(paths)} 条结果{scope_note}")
        self._global_btn.setVisible(not self._search_all_drives)
        list_width = max(200, self._list.viewport().width() or (self.width() - 48))
        font = self._list.font()
        self._list.setUpdatesEnabled(False)
        try:
            for path in paths:
                label = self._result_row_text(path, max_width=list_width, font=font)
                item = QListWidgetItem(label)
                item.setToolTip(str(path))
                item.setData(Qt.ItemDataRole.UserRole, str(path))
                self._list.addItem(item)
        finally:
            self._list.setUpdatesEnabled(True)
        self._list.setCurrentRow(0)
        self._update_location_for_row(self._list.currentRow())
        self._input.setFocus(Qt.FocusReason.OtherFocusReason)

    @staticmethod
    def _result_row_text(path: Path, *, max_width: int, font: QFont) -> str:
        """Filename + parent directory, directory elided if the row is too wide."""
        name = path.name or str(path)
        try:
            parent = str(path.parent) if path.parent != path else ""
        except Exception:
            parent = ""
        if not parent:
            return name
        sep = "  ·  "
        fm = QFontMetrics(font)
        name_w = fm.horizontalAdvance(name + sep)
        dir_budget = max(48, int(max_width) - name_w - 20)
        folder = fm.elidedText(parent, Qt.TextElideMode.ElideMiddle, dir_budget)
        return f"{name}{sep}{folder}"

    def _path_from_item(self, item: QListWidgetItem | None) -> Path | None:
        if item is None:
            return None
        raw = item.data(Qt.ItemDataRole.UserRole) or item.text()
        try:
            return Path(str(raw))
        except Exception:
            return None

    def _current_path(self) -> Path | None:
        return self._path_from_item(self._list.currentItem())

    def _on_selection_changed(
        self,
        _current: QListWidgetItem | None,
        _previous: QListWidgetItem | None,
    ) -> None:
        self._update_location_for_row(self._list.currentRow())

    def _update_location_for_row(self, row: int) -> None:
        item = self._list.item(row) if row >= 0 else None
        path = self._path_from_item(item)
        if path is None:
            self._location.setText("")
            return
        parent = str(path.parent) if path.parent != path else str(path)
        text = f"存放位置：{parent}"
        width = max(120, self._location.width() - 24)
        elided = QFontMetrics(self._location.font()).elidedText(
            text,
            Qt.TextElideMode.ElideMiddle,
            width,
        )
        self._location.setText(elided)
        self._location.setToolTip(text if elided != text else "")

    def resizeEvent(self, event) -> None:  # noqa: N802
        super().resizeEvent(event)
        if self._list.currentRow() >= 0:
            self._update_location_for_row(self._list.currentRow())

    def _open_item(self, _item: QListWidgetItem | None = None) -> None:
        path = self._current_path()
        if path is None:
            return
        open_path(path)
        self.close_overlay()

    def _reveal_item(self) -> None:
        path = self._current_path()
        if path is None:
            return
        reveal_in_explorer(path)
        self.close_overlay()

    def _show_result_menu(self, pos) -> None:
        item = self._list.itemAt(pos)
        if item is None:
            return
        self._list.setCurrentItem(item)
        path = self._path_from_item(item)
        if path is None:
            return
        menu = QMenu(self)
        menu.setObjectName("fileSearchResultMenu")
        open_act = menu.addAction("打开")
        reveal_act = menu.addAction("打开所在位置")
        chosen = menu.exec(self._list.mapToGlobal(pos))
        if chosen is open_act:
            self._open_item()
        elif chosen is reveal_act:
            self._reveal_item()

    def _handle_nav_key(self, event: QKeyEvent) -> None:
        key = event.key()
        mods = event.modifiers()
        if key == Qt.Key.Key_Escape:
            event.accept()
            self.close_overlay()
            return
        if key in (Qt.Key.Key_Return, Qt.Key.Key_Enter):
            event.accept()
            if mods & Qt.KeyboardModifier.ControlModifier:
                self._reveal_item()
            else:
                self._open_item()
            return
        if key == Qt.Key.Key_Down and self._list.count():
            event.accept()
            row = min(self._list.count() - 1, self._list.currentRow() + 1)
            self._list.setCurrentRow(row)
            return
        if key == Qt.Key.Key_Up and self._list.count():
            event.accept()
            row = max(0, self._list.currentRow() - 1)
            self._list.setCurrentRow(row)
            return

    def keyPressEvent(self, event: QKeyEvent) -> None:  # noqa: N802
        self._handle_nav_key(event)
        if not event.isAccepted():
            super().keyPressEvent(event)
