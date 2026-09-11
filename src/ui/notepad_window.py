"""Built-in notepad — tabbed editor (maximize, N++ backup-on-close, dbl-click new tab)."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from PyQt6.QtCore import QEvent, QRect, Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QAction, QActionGroup, QCloseEvent, QFont, QKeySequence, QMouseEvent, QTextCursor
from PyQt6.QtWidgets import (
    QApplication,
    QFileDialog,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QMainWindow,
    QMenu,
    QMessageBox,
    QPushButton,
    QPlainTextEdit,
    QSplitter,
    QStackedWidget,
    QTabBar,
    QTabWidget,
    QToolButton,
    QVBoxLayout,
    QWidget,
)

from src.html_to_markdown import clipboard_html_to_markdown
from src.icon_utils import get_desknote_icon
from src.markdown_preview import is_markdown_path, webengine_available
from src.md_assets import (
    copy_image_file,
    count_text_stats,
    is_image_path,
    save_qimage,
)
from src.notepad import (
    delete_notepad_backup,
    is_notepad_backup_path,
    is_notepad_openable_path,
    load_notepad_session,
    next_auto_save_path,
    next_untitled_backup_path,
    notepad_open_dialog_filter,
    notepad_save_dialog_filter,
    notepad_settings,
    resolve_notes_folder,
    save_notepad_session,
)
from src.ui.markdown_pane import (
    CollapsibleMdPane,
    MarkdownOutlineList,
    MarkdownPreviewPane,
    refresh_outline_and_preview,
)
from src.ui.markdown_wysiwyg import MarkdownWysiwygPane
from src.ui.notes_library_pane import NotesLibraryPane
from src.ui.notepad_find_dialog import NotepadFindDialog

# Back-compat name used in type hints / older code.
MarkdownPreviewBrowser = MarkdownPreviewPane


class _TabPinButton(QToolButton):
    """Pin control on a tab — swallow presses so movable QTabBar cannot steal them."""

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self.setDown(True)
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            inside = self.rect().contains(event.position().toPoint())
            was_down = self.isDown()
            self.setDown(False)
            if was_down and inside:
                self.click()
            event.accept()
            return
        super().mouseReleaseEvent(event)


class _TabCloseButton(QToolButton):
    """Tab × — swallow presses so movable QTabBar cannot steal the click."""

    def mousePressEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            self.setDown(True)
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            inside = self.rect().contains(event.position().toPoint())
            was_down = self.isDown()
            self.setDown(False)
            if was_down and inside:
                self.click()
            event.accept()
            return
        super().mouseReleaseEvent(event)


class _NoteTabBar(QTabBar):
    """Tab bar: dbl-click blank → new; middle-click close; RMB → shared file menu."""

    empty_double_clicked = pyqtSignal()
    tab_close_requested = pyqtSignal(int)
    tab_delete_requested = pyqtSignal(int)
    tab_pin_requested = pyqtSignal(int)
    tab_menu_requested = pyqtSignal(int, object)  # idx, global QPoint

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        # Keep tabs left-aligned so blank area remains for double-click (Notepad++).
        self.setExpanding(False)
        self.setToolTip("双击空白处：新建；点 × 或中键关闭；右键：重命名 / 置顶 / 所在目录 / 删除")

    def _is_blank_strip(self, pos) -> bool:
        if self.tabAt(pos) >= 0:
            return False
        for i in range(self.count()):
            if self.tabRect(i).contains(pos):
                return False
        return True

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        if (
            event.button() == Qt.MouseButton.LeftButton
            and self._is_blank_strip(event.pos())
        ):
            self.empty_double_clicked.emit()
            event.accept()
            return
        super().mouseDoubleClickEvent(event)

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.MiddleButton:
            idx = self.tabAt(event.pos())
            if idx >= 0:
                self.tab_close_requested.emit(idx)
                event.accept()
                return
        super().mousePressEvent(event)

    def contextMenuEvent(self, event) -> None:
        idx = self.tabAt(event.pos())
        if idx < 0:
            return
        self.tab_menu_requested.emit(idx, event.globalPos())
        event.accept()


class _NoteTabWidget(QTabWidget):
    """Tab widget: catch double-clicks on the full tab strip blank area."""

    empty_strip_double_clicked = pyqtSignal()

    def _is_blank_tab_strip(self, pos) -> bool:
        bar = self.tabBar()
        if bar is None or not bar.isVisible():
            return False
        bar_geom = bar.geometry()
        strip_rect = QRect(0, bar_geom.y(), self.width(), bar_geom.height())
        if not strip_rect.contains(pos):
            return False
        bar_pos = pos - bar_geom.topLeft()
        return bar.tabAt(bar_pos) < 0

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        pos = event.position().toPoint()
        if (
            event.button() == Qt.MouseButton.LeftButton
            and self._is_blank_tab_strip(pos)
        ):
            self.empty_strip_double_clicked.emit()
            event.accept()
            return
        super().mouseDoubleClickEvent(event)


@dataclass
class _NotePage:
    editor: QPlainTextEdit
    host: QWidget
    outline: MarkdownOutlineList
    preview: MarkdownPreviewPane
    outline_pane: CollapsibleMdPane
    preview_pane: CollapsibleMdPane
    splitter: QSplitter
    stack: QStackedWidget
    wysiwyg: MarkdownWysiwygPane
    path: Path | None = None
    dirty: bool = False
    wrap: bool = True
    ephemeral: bool = False  # untitled snapshot; kept on window close, deleted on tab close
    pinned: bool = False  # stay at the left of the tab bar
    _suppress: bool = field(default=False, repr=False)
    _scroll_guard: bool = field(default=False, repr=False)


class NotepadWindow(QMainWindow):
    """Lightweight tabbed notepad shipped with DeskTidy."""

    def __init__(self, settings: dict, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.settings = settings
        self._pages: list[_NotePage] = []
        self._find_dialog: NotepadFindDialog | None = None
        self.setObjectName("notepadWindow")
        self.setWindowTitle("DeskNote")
        self.resize(960, 620)
        self.setMinimumSize(420, 280)
        # Taskbar / title-bar glyph — QApplication icon alone is not enough on Windows.
        self.apply_window_icon()
        # N++-style: drop text/code files from Explorer onto the window to open tabs.
        self.setAcceptDrops(True)
        self._app_drop_filter = False

        npp = notepad_settings(self.settings)
        self._md_preview_enabled = bool(npp.get("md_preview", True))
        self._md_outline_enabled = bool(npp.get("md_outline", True))
        self._md_sync_scroll = bool(npp.get("md_sync_scroll", True))
        self._md_edit_mode = str(npp.get("md_edit_mode") or "source")  # source|wysiwyg
        if self._md_edit_mode not in ("source", "wysiwyg"):
            self._md_edit_mode = "source"
        self._focus_mode = False
        self._typewriter_mode = bool(npp.get("typewriter_mode", False))
        self._library_enabled = bool(npp.get("library_sidebar", True))
        self._md_refresh = QTimer(self)
        self._md_refresh.setSingleShot(True)
        self._md_refresh.setInterval(250)
        self._md_refresh.timeout.connect(self._refresh_markdown_panes)

        central = QWidget()
        outer = QVBoxLayout(central)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        self._main_splitter = QSplitter(Qt.Orientation.Horizontal)
        self._main_splitter.setObjectName("notepadMainSplitter")
        self.library = NotesLibraryPane()
        self.library.set_root(self._notes_dir())
        self.library.file_activated.connect(self._on_library_file)
        self.library.search_hit_activated.connect(self._on_library_search_hit)
        self.library.rename_requested.connect(self._on_library_rename)
        self.library.file_renamed.connect(self._on_library_file_renamed)
        self.library.delete_requested.connect(self._on_library_delete)
        self.library.close_requested.connect(self._on_library_close)
        self.library.pin_requested.connect(self._on_library_pin)
        self.library.reveal_requested.connect(self._reveal_path_in_explorer)
        self.library.path_is_open = self._library_path_is_open
        self.library.path_is_pinned = self._library_path_is_pinned
        self.library.new_markdown_requested.connect(self.new_markdown_in_library)
        self.library.open_folder_requested.connect(self.open_notes_folder)
        self.library.refresh_requested.connect(self._refresh_library)
        self._main_splitter.addWidget(self.library)

        right = QWidget()
        layout = QVBoxLayout(right)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        self.tabs = _NoteTabWidget()
        self.tabs.setObjectName("notepadTabs")
        self.tabs.setDocumentMode(True)
        # Closable chrome installed per-tab via _TabCloseButton (movable-safe).
        self.tabs.setTabsClosable(False)
        self.tabs.setMovable(True)
        self.tabs.setUsesScrollButtons(True)
        tab_bar = _NoteTabBar()
        self.tabs.setTabBar(tab_bar)
        # QTabWidget.setTabBar() resets expanding — re-apply so blank strip stays clickable.
        tab_bar.setExpanding(False)
        tab_bar.setTabsClosable(False)
        tab_bar.empty_double_clicked.connect(self.new_document)
        self.tabs.empty_strip_double_clicked.connect(self.new_document)
        # Middle-click close (custom signal on the bar).
        tab_bar.tab_close_requested.connect(self._close_tab)
        tab_bar.tab_delete_requested.connect(self._delete_tab)
        tab_bar.tab_pin_requested.connect(self._toggle_pin_at)
        tab_bar.tab_menu_requested.connect(self._on_tab_context_menu)
        tab_bar.tabMoved.connect(self._on_tab_moved)
        self.tabs.currentChanged.connect(self._on_tab_changed)
        self._close_btn = QPushButton("×")
        self._close_btn.setObjectName("notepadCloseBtn")
        self._close_btn.setFixedSize(28, 28)
        self._close_btn.setFlat(True)
        self._close_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._close_btn.setToolTip("关闭窗口")
        self._close_btn.clicked.connect(self.close)
        corner = QWidget()
        corner_layout = QHBoxLayout(corner)
        corner_layout.setContentsMargins(0, 0, 6, 0)
        corner_layout.addWidget(self._close_btn)
        self.tabs.setCornerWidget(corner, Qt.Corner.TopRightCorner)
        layout.addWidget(self.tabs, stretch=1)

        status_row = QHBoxLayout()
        status_row.setContentsMargins(10, 4, 10, 6)
        self._status = QLabel("就绪")
        self._status.setObjectName("sectionHint")
        status_row.addWidget(self._status, stretch=1)
        self._pos_label = QLabel("行 1, 列 1")
        self._pos_label.setObjectName("sectionHint")
        self._pos_label.setAlignment(
            Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
        )
        status_row.addWidget(self._pos_label)
        layout.addLayout(status_row)

        self._main_splitter.addWidget(right)
        self._main_splitter.setStretchFactor(0, 0)
        self._main_splitter.setStretchFactor(1, 1)
        self._main_splitter.setSizes([220, 740])
        outer.addWidget(self._main_splitter, stretch=1)
        self.setCentralWidget(central)
        self._apply_library_visibility()

        self._build_menus()
        if not self._restore_session():
            self.new_document()
        # After children exist: catch file drops on library / tabs / preview / chrome.
        app = QApplication.instance()
        if app is not None:
            app.installEventFilter(self)
            self._app_drop_filter = True

    @property
    def editor(self) -> QPlainTextEdit:
        page = self._current_page()
        assert page is not None
        return page.editor

    @property
    def _dirty(self) -> bool:
        page = self._current_page()
        return bool(page and page.dirty)

    @_dirty.setter
    def _dirty(self, value: bool) -> None:
        page = self._current_page()
        if page is not None:
            page.dirty = bool(value)

    @property
    def _path(self) -> Path | None:
        page = self._current_page()
        return page.path if page else None

    @_path.setter
    def _path(self, value: Path | None) -> None:
        page = self._current_page()
        if page is not None:
            page.path = value

    def _notes_dir(self) -> Path:
        return resolve_notes_folder(self.settings, ensure=True)

    def _current_page(self) -> _NotePage | None:
        idx = self.tabs.currentIndex()
        if idx < 0 or idx >= len(self._pages):
            return None
        return self._pages[idx]

    def _make_editor(self) -> QPlainTextEdit:
        editor = QPlainTextEdit()
        editor.setObjectName("notepadEditor")
        editor.setLineWrapMode(QPlainTextEdit.LineWrapMode.WidgetWidth)
        font = QFont("Consolas")
        font.setPointSize(11)
        if not font.exactMatch():
            font = QFont("Cascadia Mono")
            font.setPointSize(11)
        editor.setFont(font)
        editor.setTabStopDistance(32)
        editor.setPlaceholderText("在此输入文字…")
        editor.setAcceptDrops(True)
        return editor

    def _make_page(self, *, path: Path | None = None, ephemeral: bool = False) -> _NotePage:
        editor = self._make_editor()
        outline = MarkdownOutlineList()
        preview = MarkdownPreviewPane()
        outline_pane = CollapsibleMdPane(
            "大纲",
            outline,
            expanded=self._md_outline_enabled,
            handle_on_right=True,
        )
        preview_pane = CollapsibleMdPane(
            "预览",
            preview,
            expanded=self._md_preview_enabled,
            handle_on_right=False,
        )
        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.setObjectName("notepadMdSplitter")
        splitter.addWidget(outline_pane)
        splitter.addWidget(editor)
        splitter.addWidget(preview_pane)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        splitter.setStretchFactor(2, 1)
        splitter.setSizes([160, 420, 380])
        wysiwyg = MarkdownWysiwygPane()
        stack = QStackedWidget()
        stack.addWidget(splitter)  # 0 = source
        stack.addWidget(wysiwyg)  # 1 = wysiwyg
        host = QWidget()
        host_layout = QVBoxLayout(host)
        host_layout.setContentsMargins(0, 0, 0, 0)
        host_layout.setSpacing(0)
        host_layout.addWidget(stack)
        page = _NotePage(
            editor=editor,
            host=host,
            outline=outline,
            preview=preview,
            outline_pane=outline_pane,
            preview_pane=preview_pane,
            splitter=splitter,
            stack=stack,
            wysiwyg=wysiwyg,
            path=path,
            ephemeral=ephemeral,
        )
        outline.line_activated.connect(lambda line, p=page: self._goto_outline_line(p, line))
        editor.verticalScrollBar().valueChanged.connect(
            lambda _v, p=page: self._on_editor_scroll(p)
        )
        preview.scroll_ratio_changed.connect(
            lambda ratio, p=page: self._on_preview_scroll(p, ratio)
        )
        editor.cursorPositionChanged.connect(lambda p=page: self._on_cursor_moved(p))
        # QPlainTextEdit delivers DragEnter/Drop to the *viewport*, not the
        # editor widget — filter both or the default handler pastes path text.
        editor.installEventFilter(self)
        editor.viewport().installEventFilter(self)
        outline_pane.expanded_changed.connect(
            lambda on, p=page: self._on_outline_pane_toggled(on, p)
        )
        preview_pane.expanded_changed.connect(
            lambda on, p=page: self._on_preview_pane_toggled(on, p)
        )
        wysiwyg.markdown_changed.connect(
            lambda md, p=page: self._on_wysiwyg_changed(p, md)
        )
        return page

    def _page_is_markdown(self, page: _NotePage | None) -> bool:
        if page is None:
            return False
        return is_markdown_path(page.path)

    def _md_base_dir(self, page: _NotePage) -> Path:
        if page.path is not None:
            return page.path.parent
        return self._notes_dir()

    def _apply_markdown_layout(self, page: _NotePage | None = None) -> None:
        page = page or self._current_page()
        if page is None:
            return
        if self._md_edit_mode == "wysiwyg" and self._page_is_markdown(page):
            return
        is_md = self._page_is_markdown(page)
        # MD docs: keep collapsible strips visible so user can re-expand by click.
        # Focus mode / non-md: hide wrappers entirely.
        show_chrome = is_md and not self._focus_mode
        page.outline_pane.setVisible(show_chrome)
        page.preview_pane.setVisible(show_chrome)
        if show_chrome:
            page.outline_pane.set_expanded(self._md_outline_enabled, emit=False)
            page.preview_pane.set_expanded(self._md_preview_enabled, emit=False)
            self._redistribute_md_splitter(page)
            self._refresh_markdown_panes(page)

    def _redistribute_md_splitter(self, page: _NotePage) -> None:
        """Give freed width to the editor when outline/preview collapse sideways."""
        from src.ui.markdown_pane import _COLLAPSED_STRIP_W

        sp = page.splitter
        sizes = sp.sizes()
        total = max(sp.width(), sum(sizes) if sizes else 0, 1)
        # Refresh saved widths while still expanded (user may have dragged splitter).
        if page.outline_pane.is_expanded() and len(sizes) > 0 and sizes[0] > _COLLAPSED_STRIP_W + 8:
            page.outline_pane._saved_width = sizes[0]
        if page.preview_pane.is_expanded() and len(sizes) > 2 and sizes[2] > _COLLAPSED_STRIP_W + 8:
            page.preview_pane._saved_width = sizes[2]

        outline_w = (
            page.outline_pane.preferred_expanded_width()
            if page.outline_pane.is_expanded()
            else _COLLAPSED_STRIP_W
        )
        preview_w = (
            page.preview_pane.preferred_expanded_width()
            if page.preview_pane.is_expanded()
            else _COLLAPSED_STRIP_W
        )
        mid = max(240, total - outline_w - preview_w)
        sp.setStretchFactor(0, 0)
        sp.setStretchFactor(1, 1)
        sp.setStretchFactor(2, 1 if page.preview_pane.is_expanded() else 0)
        sp.setSizes([outline_w, mid, preview_w])

    def _on_outline_pane_toggled(self, expanded: bool, page: _NotePage) -> None:
        if page is not self._current_page():
            return
        self._md_outline_enabled = bool(expanded)
        if hasattr(self, "_md_outline_action"):
            self._md_outline_action.blockSignals(True)
            self._md_outline_action.setChecked(self._md_outline_enabled)
            self._md_outline_action.blockSignals(False)
        self._persist_md_settings()
        self._redistribute_md_splitter(page)
        if expanded:
            self._refresh_markdown_panes(page)

    def _on_preview_pane_toggled(self, expanded: bool, page: _NotePage) -> None:
        if page is not self._current_page():
            return
        self._md_preview_enabled = bool(expanded)
        if hasattr(self, "_md_preview_action"):
            self._md_preview_action.blockSignals(True)
            self._md_preview_action.setChecked(self._md_preview_enabled)
            self._md_preview_action.blockSignals(False)
        self._persist_md_settings()
        self._redistribute_md_splitter(page)
        if expanded:
            self._refresh_markdown_panes(page)

    def _refresh_markdown_panes(self, page: _NotePage | None = None) -> None:
        page = page or self._current_page()
        if page is None or not self._page_is_markdown(page):
            return
        refresh_outline_and_preview(
            page.outline,
            page.preview,
            page.editor.toPlainText(),
            base_dir=self._md_base_dir(page),
            show_outline=self._md_outline_enabled,
            show_preview=self._md_preview_enabled,
            theme=str(self.settings.get("theme") or "sky"),
        )

    def _editor_scroll_ratio(self, page: _NotePage) -> float:
        bar = page.editor.verticalScrollBar()
        maximum = max(1, bar.maximum())
        return float(bar.value()) / float(maximum)

    def _on_editor_scroll(self, page: _NotePage) -> None:
        if not self._md_sync_scroll or page._scroll_guard:
            return
        if page is not self._current_page() or not self._page_is_markdown(page):
            return
        if not page.preview.isVisible():
            return
        page._scroll_guard = True
        try:
            page.preview.set_scroll_ratio(self._editor_scroll_ratio(page))
        finally:
            page._scroll_guard = False

    def _on_preview_scroll(self, page: _NotePage, ratio: float) -> None:
        if not self._md_sync_scroll or page._scroll_guard:
            return
        if page is not self._current_page() or not self._page_is_markdown(page):
            return
        page._scroll_guard = True
        try:
            bar = page.editor.verticalScrollBar()
            bar.setValue(int(max(0.0, min(1.0, float(ratio))) * bar.maximum()))
        finally:
            page._scroll_guard = False

    def _schedule_markdown_refresh(self, page: _NotePage) -> None:
        if page is not self._current_page():
            return
        if not self._page_is_markdown(page):
            return
        self._md_refresh.start()

    def _goto_outline_line(self, page: _NotePage, line: int) -> None:
        block = page.editor.document().findBlockByLineNumber(max(0, int(line)))
        if not block.isValid():
            return
        cursor = QTextCursor(block)
        page.editor.setTextCursor(cursor)
        page.editor.setFocus(Qt.FocusReason.MouseFocusReason)
        page.editor.centerCursor()

    def _persist_md_settings(self) -> None:
        cfg = notepad_settings(self.settings)
        cfg["md_preview"] = bool(self._md_preview_enabled)
        cfg["md_outline"] = bool(self._md_outline_enabled)
        cfg["md_sync_scroll"] = bool(self._md_sync_scroll)
        cfg["md_edit_mode"] = str(self._md_edit_mode)
        cfg["typewriter_mode"] = bool(self._typewriter_mode)
        cfg["library_sidebar"] = bool(self._library_enabled)
        try:
            from src.settings import patch_notepad_settings

            # Patch-only + immediate: DeskTidy's full snapshot must not stomp these.
            patch_notepad_settings(
                {
                    "md_preview": cfg["md_preview"],
                    "md_outline": cfg["md_outline"],
                    "md_sync_scroll": cfg["md_sync_scroll"],
                    "md_edit_mode": cfg["md_edit_mode"],
                    "typewriter_mode": cfg["typewriter_mode"],
                    "library_sidebar": cfg["library_sidebar"],
                },
                immediate=True,
            )
        except Exception:
            pass

    def _apply_library_visibility(self) -> None:
        show = bool(self._library_enabled) and not self._focus_mode
        self.library.setVisible(show)
        if hasattr(self, "_library_action"):
            self._library_action.blockSignals(True)
            self._library_action.setChecked(self._library_enabled)
            self._library_action.blockSignals(False)

    def _set_library_sidebar(self, enabled: bool) -> None:
        self._library_enabled = bool(enabled)
        self._persist_md_settings()
        self._apply_library_visibility()
        if self._library_enabled:
            self._refresh_library()

    def _refresh_library(self) -> None:
        self.library.set_root(self._notes_dir())
        page = self._current_page()
        if page is not None and page.path is not None:
            self.library.highlight_path(page.path)

    def focus_library_search(self) -> None:
        if not self._library_enabled:
            self._set_library_sidebar(True)
        if self._focus_mode:
            self._focus_action.setChecked(False)
        self.library.focus_search()

    def _on_library_file(self, path: str) -> None:
        self._activate_or_open_path(Path(path))

    def _on_library_search_hit(self, path: str, line: int) -> None:
        self._activate_or_open_path(Path(path), line=int(line))

    def _index_for_path(self, path: Path | str | None) -> int:
        if path is None:
            return -1
        try:
            key = Path(path).resolve()
        except OSError:
            key = Path(path)
        for i, page in enumerate(self._pages):
            if page.path is None:
                continue
            try:
                if page.path.resolve() == key:
                    return i
            except OSError:
                continue
        return -1

    def _library_path_is_open(self, path: Path | str) -> bool:
        return self._index_for_path(path) >= 0

    def _library_path_is_pinned(self, path: Path | str) -> bool:
        idx = self._index_for_path(path)
        if idx < 0:
            return False
        return bool(self._pages[idx].pinned)

    def _on_library_close(self, path: str) -> None:
        idx = self._index_for_path(path)
        if idx >= 0:
            self._close_tab(idx)

    def _on_library_pin(self, path: str) -> None:
        idx = self._index_for_path(path)
        if idx < 0:
            self._activate_or_open_path(Path(path))
            idx = self._index_for_path(path)
        if idx >= 0:
            self._toggle_pin_at(idx)

    def _on_tab_context_menu(self, idx: int, global_pos) -> None:
        """Same actions as notes-library file menu (重命名 / 置顶 / 所在目录 / 删除)."""
        if idx < 0 or idx >= len(self._pages):
            return
        page = self._pages[idx]
        menu = QMenu(self)
        path = page.path

        act_rename = menu.addAction("重命名")
        act_rename.setEnabled(path is not None)
        act_pin = menu.addAction("取消置顶" if page.pinned else "置顶")
        act_reveal = menu.addAction("打开文件所在目录")
        act_reveal.setEnabled(path is not None)
        act_delete = menu.addAction("删除")

        chosen = menu.exec(global_pos)
        if chosen is act_rename and path is not None:
            self.library.begin_rename(path)
        elif chosen is act_pin:
            self._toggle_pin_at(idx)
        elif chosen is act_reveal and path is not None:
            self._reveal_path_in_explorer(str(path))
        elif chosen is act_delete:
            self._delete_tab(idx)

    def _reveal_path_in_explorer(self, path: str) -> None:
        """Select *path* in Explorer (file's containing folder)."""
        try:
            target = Path(path)
            if not target.exists():
                QMessageBox.warning(self, "笔记", "文件不存在。")
                return
            from src.win_shell import reveal_in_explorer

            reveal_in_explorer(target)
        except OSError as exc:
            QMessageBox.warning(self, "笔记", f"无法打开所在目录：{exc}")

    def _on_library_rename(self, path: str) -> None:
        """Dialog rename (e.g. from search results) — stem only; keep suffix."""
        from src.notes_library import keep_filename_suffix

        src = Path(path)
        try:
            if not src.is_file():
                return
        except OSError:
            return
        hint = f"新文件名（{src.suffix or '无后缀'} 不变）：" if src.suffix else "新文件名："
        name, ok = QInputDialog.getText(
            self,
            "重命名",
            hint,
            text=src.stem,
        )
        if not ok:
            return
        new_name = keep_filename_suffix(src.name, str(name or ""))
        if not new_name or new_name == src.name:
            return
        dest = src.with_name(new_name)
        try:
            if dest.exists():
                QMessageBox.warning(self, "重命名", "该名称已存在。")
                return
            src.rename(dest)
        except OSError as exc:
            QMessageBox.warning(self, "重命名", f"无法重命名：{exc}")
            return
        self._retarget_pages(src, dest)
        self._refresh_library()
        self.library.highlight_path(dest)
        self._persist_session()

    def _on_library_file_renamed(self, old_path: str, new_path: str) -> None:
        """In-place tree rename finished — sync open tabs."""
        try:
            old = Path(old_path)
            new = Path(new_path)
        except OSError:
            return
        self._retarget_pages(old, new)
        self._persist_session()
        page = self._current_page()
        if page is not None and page.path is not None:
            self.library.highlight_path(page.path)
        self._update_window_title()
        self._update_status()

    def _retarget_pages(self, old: Path, new: Path) -> None:
        try:
            old_key = old.resolve()
        except OSError:
            old_key = old
        try:
            new_resolved = new.resolve()
        except OSError:
            new_resolved = new
        for i, page in enumerate(self._pages):
            if page.path is None:
                continue
            try:
                if page.path.resolve() != old_key:
                    continue
            except OSError:
                continue
            page.path = new_resolved
            self._refresh_tab_title(i)
        self._update_window_title()

    def _on_library_delete(self, path: str) -> None:
        target = Path(path)
        try:
            if not target.is_file():
                return
        except OSError:
            return
        reply = QMessageBox.question(
            self,
            "删除",
            f"确定删除「{target.name}」？\n此操作不可撤销。",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            QMessageBox.StandardButton.No,
        )
        if reply != QMessageBox.StandardButton.Yes:
            return
        # Close matching tabs first without deleting again via tab-close.
        try:
            key = target.resolve()
        except OSError:
            key = target
        for i in range(len(self._pages) - 1, -1, -1):
            page = self._pages[i]
            if page.path is None:
                continue
            try:
                if page.path.resolve() != key:
                    continue
            except OSError:
                continue
            page.path = None
            page.ephemeral = False
            page.dirty = False
            self.tabs.removeTab(i)
            self._pages.pop(i)
        try:
            target.unlink(missing_ok=True)
        except OSError as exc:
            QMessageBox.warning(self, "删除", f"无法删除：{exc}")
            return
        if not self._pages:
            # Keep the window open with zero tabs — no auto 未命名_*.md, no quit.
            self._update_window_title()
            self._update_status()
            self._refresh_library()
            self._persist_session()
            self._status.setText(f"已删除：{target.name}")
            return
        self._update_window_title()
        self._update_status()
        self._refresh_library()
        self._persist_session()
        self._status.setText(f"已删除：{target.name}")

    def new_markdown_in_library(self) -> None:
        """Create ``未命名.md`` (unique) under the notes library folder and open it."""
        folder = self._notes_dir()
        base = "未命名"
        path = folder / f"{base}.md"
        n = 1
        while path.exists():
            path = folder / f"{base}_{n:02d}.md"
            n += 1
        try:
            path.write_text("# \n\n", encoding="utf-8")
        except OSError as exc:
            QMessageBox.warning(self, "新建", f"无法创建文件：{exc}")
            return
        self._refresh_library()
        self._activate_or_open_path(path)

    def new_document(self) -> None:
        """新建页签：默认在笔记库文件夹创建 Markdown 文件并打开。"""
        self.new_markdown_in_library()

    def _activate_or_open_path(self, path: Path, *, line: int | None = None) -> None:
        try:
            resolved = path.resolve()
        except OSError:
            resolved = path
        for i, page in enumerate(self._pages):
            if page.path is None:
                continue
            try:
                if page.path.resolve() == resolved:
                    self.tabs.setCurrentIndex(i)
                    if line is not None and line >= 0:
                        self._goto_outline_line(page, line)
                    return
            except OSError:
                continue
        self._open_path_in_new_tab(path)
        page = self._current_page()
        if page is not None and line is not None and line >= 0:
            self._goto_outline_line(page, line)

    def _set_md_sync_scroll(self, enabled: bool) -> None:
        self._md_sync_scroll = bool(enabled)
        self._persist_md_settings()

    def _set_edit_mode(self, mode: str) -> None:
        mode = "wysiwyg" if mode == "wysiwyg" else "source"
        if mode == "wysiwyg" and not webengine_available():
            QMessageBox.information(
                self, "所见即所得", "当前环境无 WebEngine，无法使用所见即所得模式。"
            )
            mode = "source"
        page = self._current_page()
        if mode == "wysiwyg" and page is not None and not page.wysiwyg.available:
            QMessageBox.information(
                self, "所见即所得", "所见即所得资源未就绪，请确认已安装增强预览组件。"
            )
            mode = "source"
        # Flush WYSIWYG → source editor before leaving wysiwyg.
        if self._md_edit_mode == "wysiwyg" and mode == "source" and page is not None:
            self._pull_wysiwyg_into_editor(page)
        self._md_edit_mode = mode
        if hasattr(self, "_mode_source_action"):
            self._mode_source_action.blockSignals(True)
            self._mode_wysiwyg_action.blockSignals(True)
            self._mode_source_action.setChecked(mode == "source")
            self._mode_wysiwyg_action.setChecked(mode == "wysiwyg")
            self._mode_source_action.blockSignals(False)
            self._mode_wysiwyg_action.blockSignals(False)
        self._persist_md_settings()
        self._apply_edit_mode()

    def _apply_edit_mode(self, page: _NotePage | None = None) -> None:
        page = page or self._current_page()
        if page is None:
            return
        use_wysiwyg = (
            self._md_edit_mode == "wysiwyg"
            and self._page_is_markdown(page)
            and page.wysiwyg.available
        )
        page.stack.setCurrentIndex(1 if use_wysiwyg else 0)
        if use_wysiwyg:
            page.wysiwyg.set_markdown(
                page.editor.toPlainText(),
                theme=str(self.settings.get("theme") or "sky"),
            )
        else:
            self._apply_markdown_layout(page)

    def _pull_wysiwyg_into_editor(self, page: _NotePage) -> None:
        done = {"ok": False}

        def _apply(md: str) -> None:
            done["ok"] = True
            if page._suppress:
                return
            page._suppress = True
            try:
                cur = page.editor.textCursor()
                page.editor.setPlainText(str(md or ""))
                page.editor.setTextCursor(cur)
            finally:
                page._suppress = False

        page.wysiwyg.get_markdown(_apply)
        # Spin briefly so save paths get content (local JS is sync enough usually).
        app = QApplication.instance()
        for _ in range(20):
            if done["ok"]:
                break
            if app is not None:
                app.processEvents()

    def _on_wysiwyg_changed(self, page: _NotePage, md: str) -> None:
        if page._suppress:
            return
        page._suppress = True
        try:
            page.editor.setPlainText(md)
        finally:
            page._suppress = False
        if not page.dirty:
            page.dirty = True
            idx = self._index_of(page)
            if idx >= 0:
                self._refresh_tab_title(idx)
            self._update_window_title()

    def _set_focus_mode(self, enabled: bool) -> None:
        self._focus_mode = bool(enabled)
        self._apply_library_visibility()
        page = self._current_page()
        if page is None:
            return
        if self._focus_mode:
            # Hide outline/preview chrome; keep editor wide.
            page.outline_pane.setVisible(False)
            page.preview_pane.setVisible(False)
            if self._md_edit_mode != "source":
                self._set_edit_mode("source")
            self.menuBar().setVisible(True)  # need exit toggle
            self._status.setVisible(False)
            self._pos_label.setVisible(False)
        else:
            self._status.setVisible(True)
            self._pos_label.setVisible(True)
            self._apply_edit_mode(page)
            self._apply_markdown_layout(page)

    def _set_typewriter_mode(self, enabled: bool) -> None:
        self._typewriter_mode = bool(enabled)
        self._persist_md_settings()
        page = self._current_page()
        if page is not None and self._typewriter_mode:
            self._on_cursor_moved(page)

    def _on_cursor_moved(self, page: _NotePage) -> None:
        if not self._typewriter_mode:
            return
        if page is not self._current_page():
            return
        if self._md_edit_mode == "wysiwyg":
            return
        page.editor.centerCursor()

    def smart_paste(self) -> None:
        """Paste image → assets/, or HTML → Markdown, else normal paste."""
        page = self._current_page()
        if page is None:
            return
        clip = QApplication.clipboard()
        mime = clip.mimeData() if clip is not None else None
        if (
            mime is not None
            and self._page_is_markdown(page)
            and mime.hasImage()
            and not mime.hasHtml()
            and self._try_paste_clipboard_image(page, mime)
        ):
            return
        html_md = ""
        if mime is not None and mime.hasHtml() and self._page_is_markdown(page):
            html_md = clipboard_html_to_markdown(
                mime.html(), mime.text() if mime.hasText() else ""
            )
        if (
            self._md_edit_mode == "wysiwyg"
            and self._page_is_markdown(page)
            and page.wysiwyg.available
        ):
            if html_md:
                page.wysiwyg.insert_markdown(html_md)
            elif mime is not None and mime.hasText():
                page.wysiwyg.insert_markdown(mime.text())
            return
        if html_md:
            page.editor.insertPlainText(html_md)
            return
        page.editor.paste()

    def _try_paste_clipboard_image(self, page: _NotePage, mime) -> bool:  # noqa: ANN001
        from PyQt6.QtGui import QImage

        image = None
        if mime.hasImage():
            raw = mime.imageData()
            if isinstance(raw, QImage):
                image = raw
            elif raw is not None and hasattr(raw, "toImage"):
                image = raw.toImage()
        if image is None or image.isNull():
            return False
        try:
            _path, snippet = save_qimage(image, self._md_base_dir(page))
        except (OSError, ValueError) as exc:
            QMessageBox.warning(self, "插入图片", f"无法保存图片：{exc}")
            return True
        self._insert_markdown_snippet(page, snippet + "\n")
        self._status.setText(f"已保存图片到 assets/")
        return True

    def _insert_markdown_snippet(self, page: _NotePage, snippet: str) -> None:
        if (
            self._md_edit_mode == "wysiwyg"
            and self._page_is_markdown(page)
            and page.wysiwyg.available
        ):
            page.wysiwyg.insert_markdown(snippet)
            return
        page.editor.insertPlainText(snippet)

    def _insert_image_paths(self, page: _NotePage, paths: list[Path]) -> int:
        n = 0
        base = self._md_base_dir(page)
        for path in paths:
            if not is_image_path(path):
                continue
            try:
                _dest, snippet = copy_image_file(path, base)
            except OSError as exc:
                QMessageBox.warning(self, "插入图片", f"无法导入 {path.name}：{exc}")
                continue
            self._insert_markdown_snippet(page, snippet + "\n")
            n += 1
        return n

    def _widget_in_this_window(self, obj) -> bool:  # noqa: ANN001
        """True when *obj* is this window or a descendant (any drop surface)."""
        if not isinstance(obj, QWidget):
            return False
        try:
            return obj is self or self.isAncestorOf(obj)
        except RuntimeError:
            return False

    def _accept_file_drop(self, event, *, mode: str = "open") -> None:  # noqa: ANN001
        """Finish a file drag: ``open`` keeps source; ``import`` uses Copy.

        - **open** (text/code into tabs): accept enter so Drop arrives; on Drop
          report ``IgnoreAction`` so Explorer/DeskTidy keep the desktop icon.
          Never accept Copy/Move (that made icons vanish).
        - **import** (images into Markdown assets): Copy so the source file stays
          and we may copy bytes into ``assets/``.
        """
        et = event.type()
        if mode == "import":
            possible = event.possibleActions()
            if possible & Qt.DropAction.CopyAction:
                event.setDropAction(Qt.DropAction.CopyAction)
                event.accept()
                return
            if possible & Qt.DropAction.LinkAction:
                event.setDropAction(Qt.DropAction.LinkAction)
                event.accept()
                return
            if et == QEvent.Type.Drop:
                event.setDropAction(Qt.DropAction.IgnoreAction)
                event.ignore()
                return
            event.acceptProposedAction()
            return

        # mode == "open": open path in place — never Copy/Move the filesystem
        # object. Accept enter so Drop arrives; on Drop report IgnoreAction so
        # Explorer / DeskTidy keep the desktop icon (accepting Copy/Move made
        # icons vanish while the tab never opened).
        if et in (QEvent.Type.DragEnter, QEvent.Type.DragMove):
            possible = event.possibleActions()
            if possible & Qt.DropAction.CopyAction:
                event.setDropAction(Qt.DropAction.CopyAction)
                event.accept()
            elif possible & Qt.DropAction.LinkAction:
                event.setDropAction(Qt.DropAction.LinkAction)
                event.accept()
            else:
                # Move-only offer: still accept enter (so Drop is delivered) but
                # do not acceptProposedAction(Move) — that primed Explorer to
                # delete the source after Drop.
                event.setDropAction(Qt.DropAction.CopyAction)
                event.accept()
            return
        if et == QEvent.Type.Drop:
            event.setDropAction(Qt.DropAction.IgnoreAction)
            event.ignore()
            return
        event.acceptProposedAction()

    def _handle_window_file_drag(self, event) -> bool:  # noqa: ANN001
        """Open / swallow file drops anywhere on the DeskNote window.

        Returns True when the event is fully handled (do not deliver to children).
        """
        mime = event.mimeData()
        if mime is None:
            return False
        et = event.type()
        if et in (QEvent.Type.DragEnter, QEvent.Type.DragMove):
            if self._mime_has_openable_files(mime):
                self._accept_file_drop(event, mode="open")
                return True
            page = self._current_page()
            if (
                page is not None
                and self._page_is_markdown(page)
                and self._mime_has_images(mime)
            ):
                self._accept_file_drop(event, mode="import")
                return True
            if self._mime_has_filesystem_paths(mime):
                self._accept_file_drop(event, mode="open")
                return True
            return False
        if et != QEvent.Type.Drop:
            return False
        if self._mime_has_openable_files(mime):
            self._open_paths_from_mime(mime)
            self._accept_file_drop(event, mode="open")
            return True
        page = self._current_page()
        if page is not None and self._page_is_markdown(page) and self._mime_has_images(mime):
            paths = self._image_paths_from_mime(mime)
            if paths and self._insert_image_paths(page, paths):
                self._accept_file_drop(event, mode="import")
                return True
        if self._mime_has_filesystem_paths(mime):
            try:
                from src.ui.toast import show_toast

                names = [p.name for p in self._paths_from_drop_mime(mime)[:3]]
                tip = "、".join(names) if names else "该文件"
                show_toast(
                    "无法打开",
                    f"{tip} 不是 DeskNote 可编辑的文本类型",
                    msec=2800,
                )
            except Exception:
                pass
            self._accept_file_drop(event, mode="open")
            return True
        return False

    def _editor_filter_target(self, obj, page: _NotePage | None) -> bool:  # noqa: ANN001
        """True when *obj* is the source editor or its viewport (paste shortcut)."""
        if page is None:
            return False
        editor = page.editor
        if obj is editor:
            return True
        try:
            return obj is editor.viewport()
        except RuntimeError:
            return False

    def eventFilter(self, obj, event):  # noqa: ANN001
        # Guard: app filter may see events before tabs exist (or after teardown).
        if not hasattr(self, "tabs"):
            return False
        # Whole-window file drops (library / tabs / preview / editor / chrome).
        if event.type() in (
            QEvent.Type.DragEnter,
            QEvent.Type.DragMove,
            QEvent.Type.Drop,
        ) and self._widget_in_this_window(obj):
            if self._handle_window_file_drag(event):
                return True

        page = self._current_page()
        if self._editor_filter_target(obj, page):
            et = event.type()
            # Paste shortcut is delivered to the editor widget (not viewport).
            if (
                et == QEvent.Type.KeyPress
                and obj is page.editor
                and event.matches(QKeySequence.StandardKey.Paste)
            ):
                self.smart_paste()
                return True
        return super().eventFilter(obj, event)

    @staticmethod
    def _paths_from_drop_mime(mime) -> list[Path]:  # noqa: ANN001
        """Local paths from Explorer / DeskTidy drops (URLs + CF_HDROP + virtual)."""
        if mime is None:
            return []
        out: list[Path] = []
        seen: set[str] = set()

        def _add(path: Path) -> None:
            try:
                key = str(path).casefold()
            except OSError:
                key = str(path).casefold()
            if key in seen:
                return
            seen.add(key)
            out.append(path)

        try:
            from src.win_shell import collect_drop_paths

            for path in collect_drop_paths(mime):
                _add(Path(path))
        except Exception:
            pass
        if mime.hasUrls():
            for url in mime.urls():
                try:
                    local = url.toLocalFile()
                except Exception:
                    continue
                if local:
                    _add(Path(local))
        # Some apps only put a quoted path in text/plain.
        if not out and mime.hasText():
            text = (mime.text() or "").strip().strip('"')
            if text and ("\\" in text or "/" in text or text[1:3] == ":\\"):
                try:
                    cand = Path(text)
                    if cand.exists():
                        _add(cand)
                except OSError:
                    pass
        return out

    @classmethod
    def _mime_has_filesystem_paths(cls, mime) -> bool:  # noqa: ANN001
        return bool(cls._paths_from_drop_mime(mime))

    @classmethod
    def _mime_has_openable_files(cls, mime) -> bool:  # noqa: ANN001
        return any(is_notepad_openable_path(p) for p in cls._paths_from_drop_mime(mime))

    def _open_paths_from_mime(self, mime) -> int:  # noqa: ANN001
        n = 0
        for path in self._paths_from_drop_mime(mime):
            if is_notepad_openable_path(path):
                self._open_path_in_new_tab(path)
                n += 1
        if n:
            self.present()
        return n

    @staticmethod
    def _mime_has_images(mime) -> bool:  # noqa: ANN001
        if mime.hasImage() and not mime.hasUrls():
            return True
        for url in mime.urls() if mime.hasUrls() else []:
            try:
                local = url.toLocalFile()
            except Exception:
                continue
            if local and is_image_path(Path(local)):
                return True
        return False

    @staticmethod
    def _image_paths_from_mime(mime) -> list[Path]:  # noqa: ANN001
        out: list[Path] = []
        for url in mime.urls() if mime.hasUrls() else []:
            try:
                local = url.toLocalFile()
            except Exception:
                continue
            if local and is_image_path(Path(local)):
                out.append(Path(local))
        return out

    def _build_menus(self) -> None:
        file_menu = self.menuBar().addMenu("文件(&F)")
        act_new = QAction("新建(&N)", self)
        act_new.setShortcut(QKeySequence.StandardKey.New)
        act_new.triggered.connect(self.new_document)
        file_menu.addAction(act_new)

        act_open = QAction("打开(&O)…", self)
        act_open.setShortcut(QKeySequence.StandardKey.Open)
        act_open.triggered.connect(self.open_document)
        file_menu.addAction(act_open)

        act_save = QAction("保存(&S)", self)
        act_save.setShortcut(QKeySequence.StandardKey.Save)
        act_save.triggered.connect(self.save_document)
        file_menu.addAction(act_save)

        act_save_as = QAction("另存为(&A)…", self)
        act_save_as.setShortcut(QKeySequence.StandardKey.SaveAs)
        act_save_as.triggered.connect(self.save_document_as)
        file_menu.addAction(act_save_as)

        file_menu.addSeparator()
        self._export_html_action = QAction("导出 HTML…", self)
        self._export_html_action.triggered.connect(self.export_html)
        file_menu.addAction(self._export_html_action)
        self._export_pdf_action = QAction("导出 PDF…", self)
        self._export_pdf_action.triggered.connect(self.export_pdf)
        file_menu.addAction(self._export_pdf_action)
        self._export_docx_action = QAction("导出 Word…", self)
        self._export_docx_action.triggered.connect(self.export_docx)
        file_menu.addAction(self._export_docx_action)

        file_menu.addSeparator()
        act_folder = QAction("打开笔记文件夹", self)
        act_folder.triggered.connect(self.open_notes_folder)
        file_menu.addAction(act_folder)

        file_menu.addSeparator()
        act_close_tab = QAction("关闭页签", self)
        act_close_tab.setShortcut(QKeySequence("Ctrl+W"))
        act_close_tab.triggered.connect(lambda: self._close_tab(self.tabs.currentIndex()))
        file_menu.addAction(act_close_tab)

        act_close = QAction("关闭窗口(&C)", self)
        act_close.triggered.connect(self.close)
        file_menu.addAction(act_close)

        edit_menu = self.menuBar().addMenu("编辑(&E)")
        for text, shortcut, slot in (
            ("撤销(&U)", QKeySequence.StandardKey.Undo, lambda: self.editor.undo()),
            ("剪切(&T)", QKeySequence.StandardKey.Cut, lambda: self.editor.cut()),
            ("复制(&C)", QKeySequence.StandardKey.Copy, lambda: self.editor.copy()),
            ("粘贴(&P)", QKeySequence.StandardKey.Paste, self.smart_paste),
            ("全选(&A)", QKeySequence.StandardKey.SelectAll, lambda: self.editor.selectAll()),
        ):
            act = QAction(text, self)
            act.setShortcut(shortcut)
            act.triggered.connect(slot)
            edit_menu.addAction(act)
        edit_menu.addSeparator()
        act_find = QAction("查找(&F)…", self)
        act_find.setShortcut(QKeySequence.StandardKey.Find)
        act_find.triggered.connect(self.show_find_dialog)
        edit_menu.addAction(act_find)
        act_replace = QAction("替换(&H)…", self)
        act_replace.setShortcut(QKeySequence("Ctrl+H"))
        act_replace.triggered.connect(self.show_replace_dialog)
        edit_menu.addAction(act_replace)
        act_find_next = QAction("查找下一个", self)
        act_find_next.setShortcut(QKeySequence("F3"))
        act_find_next.triggered.connect(self.find_next)
        edit_menu.addAction(act_find_next)
        act_find_prev = QAction("查找上一个", self)
        act_find_prev.setShortcut(QKeySequence("Shift+F3"))
        act_find_prev.triggered.connect(self.find_previous)
        edit_menu.addAction(act_find_prev)

        search_menu = self.menuBar().addMenu("搜索(&S)")
        search_menu.addAction(act_find)
        search_menu.addAction(act_replace)
        search_menu.addSeparator()
        search_menu.addAction(act_find_next)
        search_menu.addAction(act_find_prev)

        fmt_menu = self.menuBar().addMenu("格式(&O)")
        self._wrap_action = QAction("自动换行(&W)", self)
        self._wrap_action.setCheckable(True)
        self._wrap_action.setChecked(True)
        self._wrap_action.toggled.connect(self._set_word_wrap)
        fmt_menu.addAction(self._wrap_action)

        view_menu = self.menuBar().addMenu("查看(&V)")
        self._md_preview_action = QAction("Markdown 预览", self)
        self._md_preview_action.setCheckable(True)
        self._md_preview_action.setChecked(self._md_preview_enabled)
        self._md_preview_action.setShortcut(QKeySequence("Ctrl+Shift+V"))
        self._md_preview_action.toggled.connect(self._set_md_preview)
        view_menu.addAction(self._md_preview_action)
        self._md_outline_action = QAction("大纲", self)
        self._md_outline_action.setCheckable(True)
        self._md_outline_action.setChecked(self._md_outline_enabled)
        self._md_outline_action.setShortcut(QKeySequence("Ctrl+Shift+O"))
        self._md_outline_action.toggled.connect(self._set_md_outline)
        view_menu.addAction(self._md_outline_action)
        self._md_sync_action = QAction("同步滚动", self)
        self._md_sync_action.setCheckable(True)
        self._md_sync_action.setChecked(self._md_sync_scroll)
        self._md_sync_action.toggled.connect(self._set_md_sync_scroll)
        view_menu.addAction(self._md_sync_action)
        view_menu.addSeparator()
        self._mode_source_action = QAction("源码三栏", self)
        self._mode_source_action.setCheckable(True)
        self._mode_source_action.setChecked(self._md_edit_mode == "source")
        self._mode_source_action.triggered.connect(lambda: self._set_edit_mode("source"))
        view_menu.addAction(self._mode_source_action)
        self._mode_wysiwyg_action = QAction("所见即所得", self)
        self._mode_wysiwyg_action.setCheckable(True)
        self._mode_wysiwyg_action.setChecked(self._md_edit_mode == "wysiwyg")
        self._mode_wysiwyg_action.setEnabled(webengine_available())
        self._mode_wysiwyg_action.triggered.connect(lambda: self._set_edit_mode("wysiwyg"))
        view_menu.addAction(self._mode_wysiwyg_action)
        mode_group = QActionGroup(self)
        mode_group.setExclusive(True)
        mode_group.addAction(self._mode_source_action)
        mode_group.addAction(self._mode_wysiwyg_action)
        view_menu.addSeparator()
        self._focus_action = QAction("专注模式", self)
        self._focus_action.setCheckable(True)
        self._focus_action.setShortcut(QKeySequence("F11"))
        self._focus_action.toggled.connect(self._set_focus_mode)
        view_menu.addAction(self._focus_action)
        self._typewriter_action = QAction("打字机模式", self)
        self._typewriter_action.setCheckable(True)
        self._typewriter_action.setChecked(self._typewriter_mode)
        self._typewriter_action.toggled.connect(self._set_typewriter_mode)
        view_menu.addAction(self._typewriter_action)
        view_menu.addSeparator()
        self._library_action = QAction("文件树", self)
        self._library_action.setCheckable(True)
        self._library_action.setChecked(self._library_enabled)
        self._library_action.setShortcut(QKeySequence("Ctrl+Shift+E"))
        self._library_action.toggled.connect(self._set_library_sidebar)
        view_menu.addAction(self._library_action)
        act_lib_search = QAction("搜索笔记库", self)
        act_lib_search.setShortcut(QKeySequence("Ctrl+Shift+F"))
        act_lib_search.triggered.connect(self.focus_library_search)
        view_menu.addAction(act_lib_search)

        help_menu = self.menuBar().addMenu("帮助(&H)")
        act_help = QAction("DeskNote 使用说明", self)
        act_help.setShortcut(QKeySequence.StandardKey.HelpContents)  # F1
        act_help.triggered.connect(self.show_help)
        help_menu.addAction(act_help)

    def show_help(self) -> None:
        from src.desknote_help import show_desknote_help

        show_desknote_help(self)

    def _set_md_preview(self, enabled: bool) -> None:
        self._md_preview_enabled = bool(enabled)
        self._persist_md_settings()
        self._apply_markdown_layout()

    def _set_md_outline(self, enabled: bool) -> None:
        self._md_outline_enabled = bool(enabled)
        self._persist_md_settings()
        self._apply_markdown_layout()

    def export_html(self) -> None:
        page = self._current_page()
        if page is None or not self._page_is_markdown(page):
            QMessageBox.information(self, "导出", "请先打开或保存一个 Markdown（.md）文档。")
            return
        from src.markdown_preview import export_standalone_html

        suggest = "export.html"
        if page.path is not None:
            suggest = page.path.with_suffix(".html").name
        path, _ = QFileDialog.getSaveFileName(
            self, "导出 HTML", str(self._notes_dir() / suggest), "HTML (*.html)"
        )
        if not path:
            return
        html_doc = export_standalone_html(
            page.editor.toPlainText(),
            base_dir=self._md_base_dir(page),
            title=Path(path).stem,
        )
        try:
            Path(path).write_text(html_doc, encoding="utf-8")
            self._status.setText(f"已导出 HTML：{path}")
        except OSError as exc:
            QMessageBox.warning(self, "导出", f"无法写入文件：{exc}")

    def export_pdf(self) -> None:
        page = self._current_page()
        if page is None or not self._page_is_markdown(page):
            QMessageBox.information(self, "导出", "请先打开或保存一个 Markdown（.md）文档。")
            return
        suggest = "export.pdf"
        if page.path is not None:
            suggest = page.path.with_suffix(".pdf").name
        path, _ = QFileDialog.getSaveFileName(
            self, "导出 PDF", str(self._notes_dir() / suggest), "PDF (*.pdf)"
        )
        if not path:
            return
        # Ensure preview is up to date before printing.
        self._refresh_markdown_panes(page)
        ok = page.preview.print_to_pdf(
            Path(path),
            on_done=lambda success: self._status.setText(
                f"已导出 PDF：{path}" if success else "PDF 导出失败（需要增强预览引擎）"
            ),
        )
        if not ok:
            QMessageBox.warning(
                self,
                "导出",
                "当前环境无 WebEngine 增强预览，无法导出 PDF。\n可改用「导出 HTML」。",
            )

    def export_docx(self) -> None:
        page = self._current_page()
        if page is None or not self._page_is_markdown(page):
            QMessageBox.information(self, "导出", "请先打开或保存一个 Markdown（.md）文档。")
            return
        from src.pandoc_export import (
            export_markdown_to_docx,
            find_pandoc,
            pandoc_install_hint,
        )

        if not find_pandoc():
            QMessageBox.information(self, "导出 Word", pandoc_install_hint())
            return
        suggest = "export.docx"
        if page.path is not None:
            suggest = page.path.with_suffix(".docx").name
        path, _ = QFileDialog.getSaveFileName(
            self, "导出 Word", str(self._notes_dir() / suggest), "Word (*.docx)"
        )
        if not path:
            return
        try:
            export_markdown_to_docx(
                page.editor.toPlainText(),
                Path(path),
                resource_dir=self._md_base_dir(page),
            )
            self._status.setText(f"已导出 Word：{path}")
        except FileNotFoundError:
            QMessageBox.information(self, "导出 Word", pandoc_install_hint())
        except (OSError, RuntimeError) as exc:
            QMessageBox.warning(self, "导出 Word", f"导出失败：{exc}")

    def apply_window_icon(self, accent: str | None = None) -> None:
        """Set taskbar / title-bar icon to the deskNote product glyph."""
        del accent  # deskNote uses a fixed product icon, not theme BrandMark.
        icon = get_desknote_icon()
        if not icon.isNull():
            self.setWindowIcon(icon)

    def present(self) -> None:
        """Show maximized, raise, and focus the last active tab."""
        if not self._pages:
            if not self._restore_session():
                self.new_document()
        self.showMaximized()
        from src.win_shell import bring_widget_to_foreground

        bring_widget_to_foreground(self)
        self._focus_session_tab()
        page = self._current_page()
        if page is not None:
            self._apply_edit_mode(page)
            self._focus_page_editor(page)

    def _focus_session_tab(self) -> None:
        """Select the tab recorded as current in the last session."""
        session = load_notepad_session()
        files = session.get("files") or []
        want = int(session.get("current") or 0)
        if not files or want < 0 or want >= len(files):
            return
        try:
            target = str(Path(files[want]).resolve()).casefold()
        except OSError:
            return
        for i, page in enumerate(self._pages):
            if page.path is None:
                continue
            try:
                if str(page.path.resolve()).casefold() == target:
                    if self.tabs.currentIndex() != i:
                        self.tabs.setCurrentIndex(i)
                    return
            except OSError:
                continue

    def _persist_session(self) -> None:
        """Remember open files, pinned tabs, and the active tab for the next open."""
        files: list[str] = []
        pinned: list[str] = []
        active_path: str | None = None
        cur = self._current_page()
        if cur is not None and cur.path is not None:
            active_path = str(cur.path.resolve())
        for page in self._pages:
            if page.path is None:
                continue
            try:
                if page.path.is_file():
                    resolved = str(page.path.resolve())
                    files.append(resolved)
                    if page.pinned:
                        pinned.append(resolved)
            except OSError:
                continue
        current = 0
        if active_path:
            for i, path in enumerate(files):
                if path.casefold() == active_path.casefold():
                    current = i
                    break
        try:
            save_notepad_session(files, current, pinned)
        except OSError:
            pass

    def _restore_session(self) -> bool:
        """Reload last session tabs, then every file still in the notes folder.

        Session alone used to hide notes that were saved but not in the last
        tab list (folder has 3 files, window showed 1).
        """
        from src.notepad import recent_note_files

        session = load_notepad_session()
        raw_files = session.get("files") or []
        paths: list[Path] = []
        seen: set[str] = set()

        def _add(path: Path) -> None:
            try:
                resolved = path.resolve()
                if not resolved.is_file():
                    return
                key = str(resolved).casefold()
                if key in seen:
                    return
                seen.add(key)
                paths.append(resolved)
            except OSError:
                return

        for item in raw_files:
            _add(Path(str(item)))
        session_count = len(paths)
        for path in recent_note_files(self._notes_dir(), limit=100):
            _add(path)
        if not paths:
            return False
        pinned_keys: set[str] = set()
        for item in session.get("pinned") or []:
            try:
                pinned_keys.add(str(Path(str(item)).resolve()).casefold())
            except OSError:
                continue
        self.tabs.blockSignals(True)
        try:
            for path in paths:
                page = self._add_file_tab(path, select=False)
                try:
                    key = str(path.resolve()).casefold()
                except OSError:
                    key = str(path).casefold()
                if page is not None and key in pinned_keys:
                    page.pinned = True
                    self._update_pin_button(page)
            self._normalize_pin_order(persist=False)
            current = int(session.get("current") or 0)
            if session_count == 0 or current < 0 or current >= session_count:
                current = 0
            # After pin reorder, resolve current by session file path when possible.
            if 0 <= current < session_count:
                want = str(paths[current]).casefold()
                for i, page in enumerate(self._pages):
                    if page.path is None:
                        continue
                    try:
                        if str(page.path.resolve()).casefold() == want:
                            current = i
                            break
                    except OSError:
                        continue
            if current >= len(self._pages):
                current = 0
            self.tabs.setCurrentIndex(current)
        finally:
            self.tabs.blockSignals(False)
        self._on_tab_changed(self.tabs.currentIndex())
        if len(paths) != session_count:
            self._persist_session()
        return True

    def _add_file_tab(self, path: Path, *, select: bool = True) -> _NotePage | None:
        try:
            data = path.read_bytes()
        except OSError as exc:
            QMessageBox.warning(self, "笔记", f"无法打开：{exc}")
            return None
        text = self._decode_bytes(data)
        page = self._make_page(
            path=path.resolve(),
            ephemeral=is_notepad_backup_path(path),
        )
        editor = page.editor
        editor.textChanged.connect(lambda p=page: self._on_text_changed(p))
        editor.cursorPositionChanged.connect(self._update_status)
        page._suppress = True
        editor.setPlainText(text)
        page._suppress = False
        self._pages.append(page)
        idx = self.tabs.addTab(page.host, path.name)
        self._install_tab_side_buttons(page)
        self._refresh_tab_title(idx)
        self._apply_edit_mode(page)
        if select:
            self.tabs.setCurrentIndex(idx)
            editor.moveCursor(QTextCursor.MoveOperation.Start)
            self._focus_page_editor(page)
        return page

    def _install_tab_side_buttons(self, page: _NotePage) -> None:
        """Pin (left) + close × (right). Custom buttons so movable tabs don't eat clicks."""
        self._install_pin_button(page)
        self._install_close_button(page)

    def _install_close_button(self, page: _NotePage) -> None:
        idx = self._index_of(page)
        if idx < 0:
            return
        btn = _TabCloseButton(self.tabs.tabBar())
        btn.setObjectName("notepadTabCloseBtn")
        btn.setAutoRaise(True)
        btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setFixedSize(18, 18)
        btn.setText("×")
        btn.setToolTip("关闭")
        btn.clicked.connect(lambda _checked=False, p=page: self._close_tab(self._index_of(p)))
        self.tabs.tabBar().setTabButton(idx, QTabBar.ButtonPosition.RightSide, btn)

    def _install_pin_button(self, page: _NotePage) -> None:
        idx = self._index_of(page)
        if idx < 0:
            return
        btn = _TabPinButton(self.tabs.tabBar())
        btn.setObjectName("notepadPinBtn")
        btn.setAutoRaise(True)
        btn.setCheckable(True)
        btn.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.setFixedSize(20, 20)
        btn.clicked.connect(lambda _checked=False, p=page: self._toggle_pin_page(p))
        self.tabs.tabBar().setTabButton(idx, QTabBar.ButtonPosition.LeftSide, btn)
        self._update_pin_button(page)

    def _update_pin_button(self, page: _NotePage) -> None:
        idx = self._index_of(page)
        if idx < 0:
            return
        btn = self.tabs.tabBar().tabButton(idx, QTabBar.ButtonPosition.LeftSide)
        if not isinstance(btn, QToolButton):
            return
        btn.blockSignals(True)
        try:
            btn.setChecked(bool(page.pinned))
            if page.pinned:
                btn.setText("📌")
                btn.setToolTip("取消置顶")
                btn.setProperty("pinned", "true")
            else:
                # Distinct glyph so pinned vs unpinned is obvious; click toggles off.
                btn.setText("📍")
                btn.setToolTip("置顶")
                btn.setProperty("pinned", "false")
        finally:
            btn.blockSignals(False)
        btn.style().unpolish(btn)
        btn.style().polish(btn)

    def _toggle_pin_at(self, idx: int) -> None:
        if idx < 0 or idx >= len(self._pages):
            return
        self._toggle_pin_page(self._pages[idx])

    def _toggle_pin_page(self, page: _NotePage) -> None:
        """Toggle pin; second click on the nail always unpins."""
        if self._index_of(page) < 0:
            return
        page.pinned = not page.pinned
        # Reorder + refresh all pin buttons (handles pin and unpin).
        self._normalize_pin_order(persist=True)

    def _move_page_to(self, from_idx: int, to_idx: int) -> None:
        if from_idx == to_idx:
            return
        if from_idx < 0 or from_idx >= len(self._pages):
            return
        to_idx = max(0, min(to_idx, len(self._pages) - 1))
        if from_idx == to_idx:
            return
        bar = self.tabs.tabBar()
        bar.blockSignals(True)
        try:
            bar.moveTab(from_idx, to_idx)
            page = self._pages.pop(from_idx)
            self._pages.insert(to_idx, page)
        finally:
            bar.blockSignals(False)

    def _normalize_pin_order(self, *, persist: bool = True) -> None:
        """Keep all pinned tabs on the left, preserving relative order in each group."""
        pinned = [p for p in self._pages if p.pinned]
        unpinned = [p for p in self._pages if not p.pinned]
        desired = pinned + unpinned
        if desired == list(self._pages):
            for page in self._pages:
                self._update_pin_button(page)
            return
        current = self._current_page()
        bar = self.tabs.tabBar()
        bar.blockSignals(True)
        self.tabs.blockSignals(True)
        try:
            for target, page in enumerate(desired):
                src = self._pages.index(page)
                if src == target:
                    continue
                bar.moveTab(src, target)
                self._pages.insert(target, self._pages.pop(src))
            if current is not None:
                cur_idx = self._index_of(current)
                if cur_idx >= 0:
                    self.tabs.setCurrentIndex(cur_idx)
        finally:
            self.tabs.blockSignals(False)
            bar.blockSignals(False)
        for page in self._pages:
            self._update_pin_button(page)
        if persist:
            self._persist_session()

    def _on_tab_moved(self, from_idx: int, to_idx: int) -> None:
        if from_idx == to_idx:
            return
        if from_idx < 0 or from_idx >= len(self._pages):
            return
        page = self._pages.pop(from_idx)
        to_idx = max(0, min(to_idx, len(self._pages)))
        self._pages.insert(to_idx, page)
        # Keep pinned group contiguous on the left after manual drag.
        self._normalize_pin_order(persist=True)

    def _focus_page_editor(self, page: _NotePage) -> None:
        use_wysiwyg = (
            self._md_edit_mode == "wysiwyg"
            and self._page_is_markdown(page)
            and page.wysiwyg.available
        )
        target = page.wysiwyg if use_wysiwyg else page.editor
        target.setFocus(Qt.FocusReason.ActiveWindowFocusReason)

    def _on_text_changed(self, page: _NotePage) -> None:
        if page._suppress:
            return
        if not page.dirty:
            page.dirty = True
            idx = self._index_of(page)
            if idx >= 0:
                self._refresh_tab_title(idx)
            self._update_window_title()
        self._update_status()
        self._schedule_markdown_refresh(page)

    def _index_of(self, page: _NotePage) -> int:
        try:
            return self._pages.index(page)
        except ValueError:
            return -1

    def _tab_label(self, page: _NotePage) -> str:
        name = page.path.name if page.path else "未命名"
        return f"* {name}" if page.dirty else name

    def _refresh_tab_title(self, idx: int) -> None:
        if 0 <= idx < len(self._pages):
            self.tabs.setTabText(idx, self._tab_label(self._pages[idx]))

    def _update_window_title(self) -> None:
        page = self._current_page()
        if page is None:
            self.setWindowTitle("DeskNote")
            return
        name = page.path.name if page.path else "未命名"
        dirty = "*" if page.dirty else ""
        self.setWindowTitle(f"{dirty}{name} — DeskNote")

    def _on_tab_changed(self, _idx: int) -> None:
        page = self._current_page()
        if page is not None:
            self._wrap_action.blockSignals(True)
            self._wrap_action.setChecked(page.wrap)
            self._wrap_action.blockSignals(False)
            self._apply_edit_mode(page)
            self._apply_markdown_layout(page)
            if self._focus_mode:
                self._set_focus_mode(True)
        is_md = self._page_is_markdown(page)
        for act in (
            getattr(self, "_export_html_action", None),
            getattr(self, "_export_pdf_action", None),
            getattr(self, "_export_docx_action", None),
        ):
            if act is not None:
                act.setEnabled(is_md)
        if hasattr(self, "_mode_wysiwyg_action"):
            self._mode_wysiwyg_action.setEnabled(is_md and webengine_available())
        if page is not None and page.path is not None:
            self.library.highlight_path(page.path)
        self._update_window_title()
        self._update_status()
        # Keep last active tab index warm for reopen.
        self._persist_session()

    def _mark_clean(self, page: _NotePage | None = None) -> None:
        page = page or self._current_page()
        if page is None:
            return
        page.dirty = False
        idx = self._index_of(page)
        if idx >= 0:
            self._refresh_tab_title(idx)
        self._update_window_title()
        self._update_status()

    def _update_status(self) -> None:
        page = self._current_page()
        if page is None:
            self._status.setText("就绪")
            self._pos_label.setText("")
            return
        cursor = page.editor.textCursor()
        line = cursor.blockNumber() + 1
        col = cursor.positionInBlock() + 1
        selected = cursor.selectedText().replace("\u2029", "\n")
        stats_src = selected if selected else page.editor.toPlainText()
        chars, words = count_text_stats(stats_src)
        scope = "选区" if selected else "全文"
        self._pos_label.setText(f"行 {line}, 列 {col}  ·  {scope} {chars} 字 / {words} 词")
        if page.path:
            state = "已修改" if page.dirty else "已保存"
            self._status.setText(f"{page.path}  ·  {state}")
        else:
            self._status.setText("未命名  ·  已修改" if page.dirty else "未命名")

    def _set_word_wrap(self, enabled: bool) -> None:
        page = self._current_page()
        if page is None:
            return
        page.wrap = enabled
        mode = (
            QPlainTextEdit.LineWrapMode.WidgetWidth
            if enabled
            else QPlainTextEdit.LineWrapMode.NoWrap
        )
        page.editor.setLineWrapMode(mode)

    def open_document(self) -> None:
        page = self._current_page()
        start = str(page.path.parent if page is not None and page.path else self._notes_dir())
        path, _ = QFileDialog.getOpenFileName(
            self,
            "打开文档",
            start,
            notepad_open_dialog_filter(),
        )
        if not path:
            return
        self._open_path_in_new_tab(Path(path))

    def open_paths(self, paths: list[Path | str]) -> None:
        """Open one or more files in tabs (menu / drag-drop /「用记事本打开」)."""
        for raw in paths:
            try:
                path = Path(raw)
            except OSError:
                continue
            if not is_notepad_openable_path(path):
                continue
            self._open_path_in_new_tab(path)
        self.present()

    def dragEnterEvent(self, event) -> None:  # noqa: N802
        mime = event.mimeData()
        if mime is None:
            event.ignore()
            return
        page = self._current_page()
        if self._mime_has_openable_files(mime):
            self._accept_file_drop(event, mode="open")
            return
        if page is not None and self._page_is_markdown(page) and self._mime_has_images(mime):
            self._accept_file_drop(event, mode="import")
            return
        event.ignore()

    def dropEvent(self, event) -> None:  # noqa: N802
        mime = event.mimeData()
        if mime is None:
            event.ignore()
            return
        page = self._current_page()
        # Openable text/code files always open as tabs (even over a Markdown page).
        if self._mime_has_openable_files(mime):
            self._open_paths_from_mime(mime)
            self._accept_file_drop(event, mode="open")
            return
        if page is not None and self._page_is_markdown(page) and self._mime_has_images(mime):
            paths = self._image_paths_from_mime(mime)
            if paths and self._insert_image_paths(page, paths):
                self._accept_file_drop(event, mode="import")
                return
        event.ignore()

    def _open_path_in_new_tab(self, path: Path) -> None:
        try:
            resolved = path.resolve()
        except OSError:
            resolved = path
        for i, page in enumerate(self._pages):
            if page.path is None:
                continue
            try:
                if page.path.resolve() == resolved:
                    self.tabs.setCurrentIndex(i)
                    return
            except OSError:
                continue
        page = self._current_page()
        reuse = (
            page is not None
            and page.path is None
            and not page.dirty
            and not page.editor.toPlainText()
        )
        if reuse:
            try:
                data = path.read_bytes()
            except OSError as exc:
                QMessageBox.warning(self, "笔记", f"无法打开：{exc}")
                return
            text = self._decode_bytes(data)
            assert page is not None
            page._suppress = True
            page.editor.setPlainText(text)
            page._suppress = False
            page.path = path.resolve()
            self._mark_clean(page)
            page.editor.moveCursor(QTextCursor.MoveOperation.Start)
            self._apply_edit_mode(page)
            self.library.highlight_path(page.path)
            self._persist_session()
            return
        self._add_file_tab(path, select=True)
        self._persist_session()

    @staticmethod
    def _decode_bytes(data: bytes) -> str:
        for enc in ("utf-8-sig", "utf-8", "gbk", "utf-16"):
            try:
                return data.decode(enc)
            except UnicodeDecodeError:
                continue
        return data.decode("utf-8", errors="replace")

    def save_document(self) -> bool:
        page = self._current_page()
        if page is None:
            return False
        return self._save_page(page, prompt_as=False)

    def save_document_as(self) -> bool:
        page = self._current_page()
        if page is None:
            return False
        return self._save_page(page, prompt_as=True)

    def _save_page(self, page: _NotePage, *, prompt_as: bool) -> bool:
        if (
            self._md_edit_mode == "wysiwyg"
            and self._page_is_markdown(page)
            and page.wysiwyg.available
        ):
            self._pull_wysiwyg_into_editor(page)
        if prompt_as:
            start_dir = page.path.parent if page.path else self._notes_dir()
            start_name = page.path.name if page.path else "未命名.txt"
            path, _ = QFileDialog.getSaveFileName(
                self,
                "另存为",
                str(start_dir / start_name),
                notepad_save_dialog_filter(),
            )
            if not path:
                return False
            target = Path(path)
            if target.suffix == "":
                target = target.with_suffix(".txt")
            return self._write_page(page, target, ephemeral=False)
        if page.path is None or page.ephemeral:
            return self._promote_untitled(page)
        return self._write_page(page, page.path, ephemeral=False)

    def _promote_untitled(self, page: _NotePage) -> bool:
        """User Save on untitled: write a real note, then drop the N++ backup."""
        backup = page.path if page.ephemeral else None
        ok = self._write_page(page, next_auto_save_path(self._notes_dir()), ephemeral=False)
        if ok:
            delete_notepad_backup(backup)
        return ok

    def _autosave_page(self, page: _NotePage) -> bool:
        """Named files write through; untitled snapshots go to the backup folder."""
        text = page.editor.toPlainText()
        if page.path is None and not page.dirty and not text:
            return True
        if page.path is not None and not page.ephemeral:
            return self._write_page(page, page.path, ephemeral=False)
        if not page.dirty and not text.strip():
            return True
        target = page.path if page.ephemeral and page.path is not None else next_untitled_backup_path()
        return self._write_page(page, target, ephemeral=True)

    def _write_path(self, path: Path) -> bool:
        """Write current tab to path (test / API helper)."""
        page = self._current_page()
        if page is None:
            return False
        return self._write_page(page, path, ephemeral=False)

    def _write_page(
        self, page: _NotePage, path: Path, *, ephemeral: bool | None = None
    ) -> bool:
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(page.editor.toPlainText(), encoding="utf-8")
        except OSError as exc:
            QMessageBox.warning(self, "笔记", f"保存失败：{exc}")
            return False
        page.path = path.resolve()
        if ephemeral is None:
            page.ephemeral = is_notepad_backup_path(page.path)
        else:
            page.ephemeral = bool(ephemeral)
        self._mark_clean(page)
        self._apply_edit_mode(page)
        if not page.ephemeral:
            self._refresh_library()
            self.library.highlight_path(page.path)
        self._persist_session()
        return True

    def _delete_tab_file(self, page: _NotePage) -> None:
        """Unlink this tab's file from disk (library note or untitled backup)."""
        path = page.path
        page.path = None
        page.ephemeral = False
        if path is None:
            return
        try:
            path.unlink(missing_ok=True)
        except OSError:
            pass

    def _discard_ephemeral_backup(self, page: _NotePage) -> None:
        """On close: only remove Notepad++-style backup snapshots, keep library notes."""
        path = page.path
        if path is None:
            return
        if page.ephemeral or is_notepad_backup_path(path):
            delete_notepad_backup(path)
        page.path = None
        page.ephemeral = False

    def _prompt_save_if_dirty(self, page: _NotePage) -> bool:
        """Ask Save / Discard / Cancel when dirty. Returns False if user cancels."""
        if not page.dirty:
            return True
        name = page.path.name if page.path else "未命名"
        box = QMessageBox(self)
        box.setWindowTitle("关闭")
        box.setIcon(QMessageBox.Icon.Question)
        box.setText(f"「{name}」已修改，是否保存？")
        save_btn = box.addButton("保存", QMessageBox.ButtonRole.AcceptRole)
        discard_btn = box.addButton("不保存", QMessageBox.ButtonRole.DestructiveRole)
        box.addButton("取消", QMessageBox.ButtonRole.RejectRole)
        box.setDefaultButton(save_btn)
        box.exec()
        clicked = box.clickedButton()
        if clicked is save_btn:
            return self._save_page(page, prompt_as=False)
        if clicked is discard_btn:
            return True
        return False

    def _finish_remove_tab(self, idx: int) -> None:
        if idx < 0 or idx >= len(self._pages):
            return
        self.tabs.removeTab(idx)
        self._pages.pop(idx)
        # Last tab: leave an empty tab strip; window stays open (no auto-create,
        # no quit).
        self._update_window_title()
        self._update_status()
        self._refresh_library()
        self._persist_session()

    def _close_tab(self, idx: int) -> None:
        """Close tab: prompt to save if dirty; keep library files on disk."""
        if idx < 0 or idx >= len(self._pages):
            return
        page = self._pages[idx]
        if not self._prompt_save_if_dirty(page):
            return
        self._discard_ephemeral_backup(page)
        self._finish_remove_tab(idx)

    def _delete_tab(self, idx: int) -> None:
        """Close tab and delete its file (after confirm)."""
        if idx < 0 or idx >= len(self._pages):
            return
        page = self._pages[idx]
        name = page.path.name if page.path else "未命名"
        path = page.path
        if path is not None:
            reply = QMessageBox.question(
                self,
                "删除",
                f"确定删除「{name}」？\n文件将从磁盘移除，此操作不可撤销。",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
                QMessageBox.StandardButton.No,
            )
            if reply != QMessageBox.StandardButton.Yes:
                return
            self._delete_tab_file(page)
        else:
            # Untitled with no path yet — just close.
            page.path = None
            page.ephemeral = False
        self._finish_remove_tab(idx)

    def open_notes_folder(self) -> None:
        import os

        from src.notepad import resolve_openable_notes_folder

        hints: list[Path] = []
        cur = self._current_page()
        if cur is not None and cur.path is not None:
            hints.append(cur.path)
        for page in self._pages:
            if page.path is None:
                continue
            if cur is not None and page is cur:
                continue
            hints.append(page.path)
        folder = resolve_openable_notes_folder(
            self.settings, hint_paths=hints or None, ensure=True
        )
        try:
            os.startfile(str(folder))  # noqa: S606
        except OSError as exc:
            QMessageBox.warning(self, "笔记", f"无法打开文件夹：{exc}")

    def _ensure_find_dialog(self) -> NotepadFindDialog:
        if self._find_dialog is None:
            self._find_dialog = NotepadFindDialog(self)
        return self._find_dialog

    def show_find_dialog(self) -> None:
        self._ensure_find_dialog().open_find(replace=False)

    def show_replace_dialog(self) -> None:
        dlg = self._ensure_find_dialog()
        dlg.open_find(replace=True)
        dlg.replace_edit.setFocus(Qt.FocusReason.ActiveWindowFocusReason)

    def find_text(self) -> None:
        """Open find dialog (API / tests)."""
        self.show_find_dialog()

    def find_next(self) -> None:
        dlg = self._ensure_find_dialog()
        if not dlg.isVisible():
            dlg.open_find(replace=False)
        dlg._find_next()

    def find_previous(self) -> None:
        dlg = self._ensure_find_dialog()
        if not dlg.isVisible():
            dlg.open_find(replace=False)
        dlg._find_previous()

    def closeEvent(self, event: QCloseEvent) -> None:
        # Window close: keep every tab on disk (incl. untitled backups) for next open.
        for page in list(self._pages):
            if not self._autosave_page(page):
                event.ignore()
                return
        # Remember outline/preview collapse before process exit.
        self._persist_md_settings()
        try:
            from src.settings import flush_settings

            flush_settings()
        except Exception:
            pass
        self._persist_session()
        if getattr(self, "_app_drop_filter", False):
            app = QApplication.instance()
            if app is not None:
                try:
                    app.removeEventFilter(self)
                except RuntimeError:
                    pass
            self._app_drop_filter = False
        event.accept()


def show_notepad(
    settings: dict,
    *,
    existing: NotepadWindow | None = None,
    parent: QWidget | None = None,
    new_tab: bool = False,
) -> NotepadWindow:
    """Open or reuse the built-in notepad window (maximized)."""
    win = existing if existing is not None else NotepadWindow(settings, parent=parent)
    win.settings = settings
    app = QApplication.instance()
    if app is not None:
        sheet = app.styleSheet()
        if sheet:
            win.setStyleSheet(sheet)
    if new_tab and existing is not None:
        win.new_document()
    win.present()
    return win
