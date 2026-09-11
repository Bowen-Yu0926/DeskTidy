"""Notes library sidebar: file tree + in-library search."""

from __future__ import annotations

from pathlib import Path

from PyQt6.QtCore import (
    QDir,
    QModelIndex,
    QSortFilterProxyModel,
    Qt,
    QTimer,
    pyqtSignal,
)
from PyQt6.QtGui import QAction, QFileSystemModel, QKeySequence, QShortcut
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QStackedWidget,
    QStyledItemDelegate,
    QToolButton,
    QTreeView,
    QVBoxLayout,
    QWidget,
)

from src.notes_library import (
    LibrarySearchHit,
    keep_filename_suffix,
    notes_name_filters,
    search_notes_library,
)


def _path_ctime(path: Path) -> float:
    """Birth/creation time when available (Windows ``st_ctime``)."""
    try:
        st = path.stat()
    except OSError:
        return 0.0
    birth = getattr(st, "st_birthtime", None)
    if birth is not None:
        try:
            return float(birth)
        except (TypeError, ValueError):
            pass
    try:
        return float(st.st_ctime)
    except (TypeError, ValueError, AttributeError):
        return 0.0


class _CreationTimeSortProxy(QSortFilterProxyModel):
    """Sort library tree by file/folder creation time (newest first with Descending)."""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self._ctime_cache: dict[str, float] = {}

    def invalidate_ctime_cache(self) -> None:
        self._ctime_cache.clear()

    def _cached_ctime(self, path: Path) -> float:
        try:
            key = str(path.resolve())
        except OSError:
            key = str(path)
        cached = self._ctime_cache.get(key)
        if cached is not None:
            return cached
        value = _path_ctime(path)
        self._ctime_cache[key] = value
        # Bound cache size for huge libraries.
        if len(self._ctime_cache) > 4000:
            for old in list(self._ctime_cache.keys())[:1000]:
                self._ctime_cache.pop(old, None)
        return value

    def lessThan(self, left: QModelIndex, right: QModelIndex) -> bool:  # noqa: N802
        src = self.sourceModel()
        if not isinstance(src, QFileSystemModel):
            return super().lessThan(left, right)
        try:
            left_path = Path(src.filePath(left))
            right_path = Path(src.filePath(right))
        except (TypeError, ValueError):
            return super().lessThan(left, right)
        # Directories and files share one timeline (creation descending).
        lt = self._cached_ctime(left_path)
        rt = self._cached_ctime(right_path)
        if lt != rt:
            return lt < rt
        # Stable tie-break by name.
        return left_path.name.casefold() < right_path.name.casefold()


def _fs_model_and_index(index: QModelIndex) -> tuple[QFileSystemModel | None, QModelIndex]:
    model = index.model()
    if isinstance(model, QSortFilterProxyModel):
        src = model.sourceModel()
        if isinstance(src, QFileSystemModel):
            return src, model.mapToSource(index)
        return None, QModelIndex()
    if isinstance(model, QFileSystemModel):
        return model, index
    return None, QModelIndex()


class _KeepSuffixRenameDelegate(QStyledItemDelegate):
    """F2 / in-place rename edits the stem only; suffix stays (like Explorer)."""

    def createEditor(self, parent, option, index):  # noqa: N802
        return QLineEdit(parent)

    def setEditorData(self, editor, index) -> None:  # noqa: N802
        fs, src_index = _fs_model_and_index(index)
        if not isinstance(editor, QLineEdit) or fs is None or not src_index.isValid():
            super().setEditorData(editor, index)
            return
        path = Path(fs.filePath(src_index))
        if fs.isDir(src_index):
            editor.setText(path.name)
        else:
            editor.setText(path.stem)
        editor.selectAll()

    def setModelData(self, editor, model, index) -> None:  # noqa: N802
        fs, src_index = _fs_model_and_index(index)
        if not isinstance(editor, QLineEdit) or fs is None or not src_index.isValid():
            super().setModelData(editor, model, index)
            return
        path = Path(fs.filePath(src_index))
        typed = editor.text().strip()
        if fs.isDir(src_index):
            new_name = typed.rstrip(". ").strip() or path.name
        else:
            new_name = keep_filename_suffix(path.name, typed)
        if not new_name or new_name == path.name:
            return
        # Write through the view model so proxy/source stay in sync.
        model.setData(index, new_name)


class NotesLibraryPane(QWidget):
    """Left sidebar: notes folder tree and library search results."""

    file_activated = pyqtSignal(str)  # path
    search_hit_activated = pyqtSignal(str, int)  # path, 0-based line (-1 = none)
    rename_requested = pyqtSignal(str)  # path — host shows name dialog
    file_renamed = pyqtSignal(str, str)  # old_path, new_path — after in-place rename
    delete_requested = pyqtSignal(str)  # path
    close_requested = pyqtSignal(str)  # path — close open tab, keep file
    pin_requested = pyqtSignal(str)  # path — toggle pin (open first if needed)
    reveal_requested = pyqtSignal(str)  # path — show in Explorer
    new_markdown_requested = pyqtSignal()
    open_folder_requested = pyqtSignal()
    refresh_requested = pyqtSignal()

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("notesLibraryPane")
        self.setMinimumWidth(180)
        self._root: Path | None = None
        # Host (NotepadWindow) overrides these for open/pinned state in menus.
        self.path_is_open = lambda _path: False
        self.path_is_pinned = lambda _path: False

        root_lay = QVBoxLayout(self)
        root_lay.setContentsMargins(6, 6, 4, 6)
        root_lay.setSpacing(6)

        head = QHBoxLayout()
        head.setSpacing(4)
        self._title = QLabel("笔记库")
        self._title.setObjectName("sectionHint")
        head.addWidget(self._title, stretch=1)
        self._btn_new = QToolButton()
        self._btn_new.setText("+")
        self._btn_new.setToolTip("新建 Markdown")
        self._btn_new.clicked.connect(self.new_markdown_requested.emit)
        head.addWidget(self._btn_new)
        self._btn_refresh = QToolButton()
        self._btn_refresh.setText("刷新")
        self._btn_refresh.setToolTip("刷新笔记库")
        self._btn_refresh.clicked.connect(self.refresh_requested.emit)
        head.addWidget(self._btn_refresh)
        root_lay.addLayout(head)

        self._search = QLineEdit()
        self._search.setObjectName("notesLibrarySearch")
        self._search.setPlaceholderText("搜索笔记库…")
        self._search.setClearButtonEnabled(True)
        self._search.setToolTip("在笔记文件夹中搜文件名与内容（Ctrl+Shift+F）")
        root_lay.addWidget(self._search)

        self._stack = QStackedWidget()
        self._tree = QTreeView()
        self._tree.setObjectName("notesLibraryTree")
        self._tree.setHeaderHidden(True)
        self._tree.setAnimated(True)
        self._tree.setUniformRowHeights(True)
        self._tree.setSortingEnabled(True)
        self._tree.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self._tree.customContextMenuRequested.connect(self._on_tree_menu)
        self._tree.doubleClicked.connect(self._on_tree_double_clicked)
        self._tree.activated.connect(self._on_tree_double_clicked)
        # F2 rename (context menu also calls edit). Avoid SelectedClicked — fights double-open.
        self._tree.setEditTriggers(QAbstractItemView.EditTrigger.EditKeyPressed)

        self._model = QFileSystemModel(self)
        self._model.setReadOnly(False)
        self._model.setFilter(
            QDir.Filter.AllDirs
            | QDir.Filter.Files
            | QDir.Filter.NoDotAndDotDot
        )
        self._model.setNameFilters(notes_name_filters())
        self._model.setNameFilterDisables(False)
        self._model.fileRenamed.connect(self._on_model_file_renamed)
        self._proxy = _CreationTimeSortProxy(self)
        self._proxy.setSourceModel(self._model)
        # Only sort on set_root/refresh — dynamic re-sort stats every FS churn.
        self._proxy.setDynamicSortFilter(False)
        self._tree.setModel(self._proxy)
        self._tree.setItemDelegate(_KeepSuffixRenameDelegate(self._tree))
        for col in range(1, 4):
            self._tree.hideColumn(col)
        # Newest creation time first (proxy lessThan = ascending ctime).
        self._tree.sortByColumn(0, Qt.SortOrder.DescendingOrder)

        self._results = QListWidget()
        self._results.setObjectName("notesLibraryResults")
        self._results.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self._results.customContextMenuRequested.connect(self._on_results_menu)
        self._results.itemActivated.connect(self._on_result_activated)
        self._results.itemDoubleClicked.connect(self._on_result_activated)

        self._stack.addWidget(self._tree)  # 0
        self._stack.addWidget(self._results)  # 1
        root_lay.addWidget(self._stack, stretch=1)

        self._empty = QLabel("")
        self._empty.setObjectName("sectionHint")
        self._empty.setWordWrap(True)
        self._empty.hide()
        root_lay.addWidget(self._empty)

        self._search_timer = QTimer(self)
        self._search_timer.setSingleShot(True)
        self._search_timer.setInterval(280)
        self._search_timer.timeout.connect(self._run_search)
        self._search.textChanged.connect(self._on_search_text)
        self._search_gen = 0

        del_shortcut = QShortcut(QKeySequence.StandardKey.Delete, self._tree)
        del_shortcut.setContext(Qt.ShortcutContext.WidgetWithChildrenShortcut)
        del_shortcut.activated.connect(self._delete_current_tree_file)

    def focus_search(self) -> None:
        self._search.setFocus(Qt.FocusReason.ShortcutFocusReason)
        self._search.selectAll()

    def set_root(self, folder: Path) -> None:
        folder = Path(folder)
        try:
            folder.mkdir(parents=True, exist_ok=True)
        except OSError:
            pass
        self._root = folder.resolve()
        self._proxy.invalidate_ctime_cache()
        idx = self._model.setRootPath(str(self._root))
        self._tree.setRootIndex(self._proxy.mapFromSource(idx))
        self._tree.sortByColumn(0, Qt.SortOrder.DescendingOrder)
        self._title.setToolTip(str(self._root))
        if self._search.text().strip():
            self._run_search()

    def root_path(self) -> Path | None:
        return self._root

    def highlight_path(self, path: Path | None) -> None:
        if path is None or self._root is None:
            return
        try:
            resolved = path.resolve()
        except OSError:
            return
        index = self._model.index(str(resolved))
        if not index.isValid():
            return
        proxy_index = self._proxy.mapFromSource(index)
        if not proxy_index.isValid():
            return
        self._tree.setCurrentIndex(proxy_index)
        self._tree.scrollTo(proxy_index)

    def refresh(self) -> None:
        if self._root is None:
            return
        self.set_root(self._root)

    def begin_rename(self, path: Path | str) -> None:
        """Start in-place rename on the tree item for *path*."""
        try:
            resolved = Path(path).resolve()
        except OSError:
            return
        index = self._model.index(str(resolved))
        if not index.isValid():
            self.rename_requested.emit(str(path))
            return
        proxy_index = self._proxy.mapFromSource(index)
        if not proxy_index.isValid():
            self.rename_requested.emit(str(path))
            return
        self._stack.setCurrentIndex(0)
        self._tree.setCurrentIndex(proxy_index)
        self._tree.scrollTo(proxy_index)
        self._tree.edit(proxy_index)

    def _on_search_text(self, _text: str) -> None:
        self._search_timer.start()

    def _run_search(self) -> None:
        q = self._search.text().strip()
        self._search_gen += 1
        gen = self._search_gen
        if not q:
            self._stack.setCurrentIndex(0)
            self._empty.hide()
            self._results.clear()
            return
        if self._root is None:
            return
        root = self._root
        self._stack.setCurrentIndex(1)
        self._results.clear()
        self._empty.setText("搜索中…")
        self._empty.show()

        import threading

        from PyQt6.QtWidgets import QApplication

        from src.qt_main_thread import call_on_main_thread, ensure_main_thread_bridge

        ensure_main_thread_bridge(QApplication.instance())

        def _worker() -> None:
            try:
                hits = search_notes_library(root, q)
            except Exception:
                hits = []

            def _apply() -> None:
                if gen != self._search_gen:
                    return
                self._results.clear()
                if not hits:
                    self._empty.setText("无匹配结果")
                    self._empty.show()
                    self._stack.setCurrentIndex(1)
                    return
                self._empty.hide()
                for hit in hits:
                    self._results.addItem(self._hit_item(hit))
                self._stack.setCurrentIndex(1)

            call_on_main_thread(_apply)

        threading.Thread(
            target=_worker, name="desknote-library-search", daemon=True
        ).start()

    def _hit_item(self, hit: LibrarySearchHit) -> QListWidgetItem:
        try:
            rel = hit.path.relative_to(self._root) if self._root else hit.path
            label = str(rel)
        except ValueError:
            label = hit.path.name
        if hit.line >= 0:
            text = f"{label}:{hit.line + 1}\n{hit.preview}"
        else:
            text = f"{label}\n（文件名匹配）"
        item = QListWidgetItem(text)
        item.setData(Qt.ItemDataRole.UserRole, str(hit.path))
        item.setData(Qt.ItemDataRole.UserRole + 1, int(hit.line))
        item.setToolTip(str(hit.path))
        return item

    def _on_result_activated(self, item: QListWidgetItem) -> None:
        path = item.data(Qt.ItemDataRole.UserRole)
        line = item.data(Qt.ItemDataRole.UserRole + 1)
        if path:
            self.search_hit_activated.emit(str(path), int(line if line is not None else -1))

    def _path_from_index(self, index: QModelIndex) -> Path | None:
        if not index.isValid():
            return None
        fs, src = _fs_model_and_index(index)
        if fs is None or not src.isValid():
            return None
        path = Path(fs.filePath(src))
        try:
            if path.is_file():
                return path
        except OSError:
            return None
        return None

    def _on_tree_double_clicked(self, index: QModelIndex) -> None:
        path = self._path_from_index(index)
        if path is not None:
            self.file_activated.emit(str(path))

    def _on_model_file_renamed(self, dir_path: str, old_name: str, new_name: str) -> None:
        folder = Path(dir_path)
        # Belt-and-suspenders: if suffix still drifted, force-restore.
        fixed = keep_filename_suffix(old_name, new_name)
        if fixed != new_name:
            src = folder / new_name
            dest = folder / fixed
            try:
                if src.is_file() and not dest.exists():
                    src.rename(dest)
                    new_name = fixed
            except OSError:
                pass
        try:
            old = str((folder / old_name).resolve())
            new = str((folder / new_name).resolve())
        except OSError:
            return
        self.file_renamed.emit(old, new)

    def _delete_current_tree_file(self) -> None:
        path = self._path_from_index(self._tree.currentIndex())
        if path is not None:
            self.delete_requested.emit(str(path))

    def _file_actions_menu(self, path: Path, menu: QMenu, *, tree_rename: bool) -> None:
        """Same file actions as page tabs: 重命名 / 置顶 / 所在目录 / 删除 (关闭用页签 ×)."""
        act_rename = QAction("重命名", self)
        act_rename.setShortcut(QKeySequence(Qt.Key.Key_F2))
        if tree_rename:
            act_rename.triggered.connect(lambda: self.begin_rename(path))
        else:
            act_rename.triggered.connect(lambda: self.rename_requested.emit(str(path)))
        menu.addAction(act_rename)
        pinned = bool(self.path_is_pinned(path))
        act_pin = QAction("取消置顶" if pinned else "置顶", self)
        act_pin.triggered.connect(lambda: self.pin_requested.emit(str(path)))
        menu.addAction(act_pin)
        act_reveal = QAction("打开文件所在目录", self)
        act_reveal.triggered.connect(lambda: self.reveal_requested.emit(str(path)))
        menu.addAction(act_reveal)
        act_delete = QAction("删除", self)
        act_delete.setShortcut(QKeySequence.StandardKey.Delete)
        act_delete.triggered.connect(lambda: self.delete_requested.emit(str(path)))
        menu.addAction(act_delete)

    def _on_tree_menu(self, pos) -> None:
        index = self._tree.indexAt(pos)
        menu = QMenu(self)
        path = self._path_from_index(index) if index.isValid() else None
        if path is not None:
            self._tree.setCurrentIndex(index)
            self._file_actions_menu(path, menu, tree_rename=True)
        else:
            # Blank area: library actions only (not duplicated on tabs).
            act_new = QAction("新建 Markdown", self)
            act_new.triggered.connect(self.new_markdown_requested.emit)
            menu.addAction(act_new)
            act_refresh = QAction("刷新", self)
            act_refresh.triggered.connect(self.refresh_requested.emit)
            menu.addAction(act_refresh)
            act_folder = QAction("在资源管理器中打开", self)
            act_folder.triggered.connect(self.open_folder_requested.emit)
            menu.addAction(act_folder)
        menu.exec(self._tree.viewport().mapToGlobal(pos))

    def _on_results_menu(self, pos) -> None:
        item = self._results.itemAt(pos)
        if item is None:
            return
        path_s = item.data(Qt.ItemDataRole.UserRole)
        if not path_s:
            return
        path = Path(str(path_s))
        menu = QMenu(self)
        self._file_actions_menu(path, menu, tree_rename=False)
        menu.exec(self._results.viewport().mapToGlobal(pos))
