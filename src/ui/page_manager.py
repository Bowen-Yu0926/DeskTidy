"""Desktop page management UI."""

from __future__ import annotations

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import (
    QCheckBox,
    QHBoxLayout,
    QHeaderView,
    QInputDialog,
    QPushButton,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from src.i18n import ask_yes_no, show_info, show_warning
from src.settings import save_settings


class PageManagerWidget(QWidget):
    pages_changed = pyqtSignal()
    page_selected = pyqtSignal(int)

    def __init__(
        self,
        settings: dict,
        parent: QWidget | None = None,
        *,
        embedded: bool = False,
    ):
        super().__init__(parent)
        self.settings = settings
        self._embedded = embedded
        self._table: QTableWidget | None = None
        self._indicator_cb: QCheckBox | None = None
        self._action_buttons: list[QPushButton] = []
        if not embedded:
            self._build_standalone()
        else:
            self._build_embedded_widgets()
        self.reload_table()

    def _build_embedded_widgets(self) -> None:
        self._indicator_cb = QCheckBox("显示分页切换")
        self._indicator_cb.setChecked(bool(self.settings.get("show_page_indicator", True)))
        self._indicator_cb.setToolTip("在桌面显示分页下拉框，用于快速切换桌面页")
        self._indicator_cb.stateChanged.connect(self._on_indicator_toggle)

        self._table = QTableWidget()
        self._table.setColumnCount(2)
        self._table.setHorizontalHeaderLabels(["页面名称", "分区数量"])
        self._table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self._table.horizontalHeader().setSectionResizeMode(
            1, QHeaderView.ResizeMode.ResizeToContents
        )
        self._table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._table.setAlternatingRowColors(True)
        self._table.verticalHeader().setVisible(False)
        self._table.setShowGrid(False)
        self._table.setMinimumHeight(108)
        self._table.itemSelectionChanged.connect(self._emit_page_selected)

        add_btn = QPushButton("添加页面")
        add_btn.setObjectName("primaryBtn")
        add_btn.clicked.connect(self._add_page)
        rename_btn = QPushButton("重命名")
        rename_btn.setObjectName("secondaryBtn")
        rename_btn.clicked.connect(self._rename_page)
        del_btn = QPushButton("删除")
        del_btn.setObjectName("dangerBtn")
        del_btn.clicked.connect(self._delete_page)
        self._action_buttons = [add_btn, rename_btn, del_btn]

    def _build_standalone(self) -> None:
        self._build_embedded_widgets()
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self._indicator_cb)
        layout.addWidget(self._table, stretch=1)
        row = QHBoxLayout()
        for btn in self._action_buttons:
            row.addWidget(btn)
        row.addStretch()
        layout.addLayout(row)

    def indicator_checkbox(self) -> QCheckBox:
        assert self._indicator_cb is not None
        return self._indicator_cb

    def table_widget(self) -> QTableWidget:
        assert self._table is not None
        return self._table

    def action_buttons(self) -> list[QPushButton]:
        return list(self._action_buttons)

    def _ensure_pages(self) -> list[dict]:
        pages = self.settings.get("desktop_pages")
        if not pages:
            pages = [{"id": 0, "name": "默认"}]
            self.settings["desktop_pages"] = pages
        return pages

    def _count_fences_on_page(self, page_id: int) -> int:
        return sum(
            1 for f in self.settings.get("fences", [])
            if f.get("page", 0) == page_id
        )

    def reload_table(self) -> None:
        pages = self._ensure_pages()
        table = self.table_widget()
        table.setRowCount(len(pages))
        for row, page in enumerate(pages):
            table.setItem(row, 0, QTableWidgetItem(page.get("name", "")))
            count = self._count_fences_on_page(page.get("id", 0))
            table.setItem(row, 1, QTableWidgetItem(str(count)))
        row_height = table.verticalHeader().defaultSectionSize()
        table.setMinimumHeight(max(108, row_height * max(len(pages), 2) + 42))

    def select_page_id(self, page_id: int) -> None:
        pages = self._ensure_pages()
        for row, page in enumerate(pages):
            if page.get("id", 0) == page_id:
                self.table_widget().selectRow(row)
                self._emit_page_selected()
                return
        if pages:
            self.table_widget().selectRow(0)
            self._emit_page_selected()

    def _emit_page_selected(self) -> None:
        idx = self._selected_page_index()
        if idx is None:
            return
        pages = self._ensure_pages()
        page_id = pages[idx].get("id", 0)
        self.page_selected.emit(page_id)

    def _selected_page_index(self) -> int | None:
        rows = self.table_widget().selectionModel().selectedRows()
        if not rows:
            return None
        return rows[0].row()

    def _next_page_id(self) -> int:
        pages = self._ensure_pages()
        if not pages:
            return 0
        return max(p.get("id", 0) for p in pages) + 1

    def _add_page(self) -> None:
        name, ok = QInputDialog.getText(self, "添加页面", "页面名称：")
        if not ok or not name.strip():
            return
        pages = self._ensure_pages()
        new_id = self._next_page_id()
        pages.append({"id": new_id, "name": name.strip()})
        save_settings(self.settings)
        self.reload_table()
        self.pages_changed.emit()

    def _rename_page(self) -> None:
        idx = self._selected_page_index()
        if idx is None:
            show_info(self, "提示", "请先选择一个页面。")
            return
        pages = self._ensure_pages()
        page = pages[idx]
        name, ok = QInputDialog.getText(self, "重命名", "新名称：", text=page.get("name", ""))
        if not ok or not name.strip():
            return
        page["name"] = name.strip()
        save_settings(self.settings)
        self.reload_table()
        self.pages_changed.emit()

    def _delete_page(self) -> None:
        idx = self._selected_page_index()
        if idx is None:
            return
        pages = self._ensure_pages()
        if len(pages) <= 1:
            show_warning(self, "提示", "至少保留一个页面。")
            return
        page = pages[idx]
        page_id = page.get("id", 0)
        from src.system_defaults import is_locked_page, is_locked_page_id

        if is_locked_page(page) or is_locked_page_id(page_id):
            show_warning(self, "提示", "系统默认分页不能删除。")
            return
        fence_count = self._count_fences_on_page(page_id)
        msg = f"确定删除页面「{page.get('name', '')}」？"
        if fence_count:
            msg += f"\n该页面上有 {fence_count} 个分区，将移至默认页面。"
        if not ask_yes_no(self, "确认删除", msg):
            return
        default_id = pages[0].get("id", 0) if idx != 0 else pages[1].get("id", 0)
        for fence in self.settings.get("fences", []):
            if fence.get("page", 0) == page_id:
                fence["page"] = default_id
        pages.pop(idx)
        if self.settings.get("current_page") == page_id:
            self.settings["current_page"] = default_id
        save_settings(self.settings)
        self.reload_table()
        self.pages_changed.emit()

    def _on_indicator_toggle(self, state: int) -> None:
        enabled = state in (
            Qt.CheckState.Checked.value,
            int(Qt.CheckState.Checked),
            Qt.CheckState.Checked,
        )
        self.settings["show_page_indicator"] = bool(enabled)
        save_settings(self.settings)
        self.pages_changed.emit()
