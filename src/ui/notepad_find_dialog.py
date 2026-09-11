"""Find / replace dialog for the built-in notepad."""

from __future__ import annotations

from typing import TYPE_CHECKING

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QKeySequence, QShortcut
from PyQt6.QtWidgets import (
    QCheckBox,
    QDialog,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMessageBox,
    QPushButton,
    QVBoxLayout,
)

from src.notepad_search import find_next, find_previous, replace_all_in_document, replace_once

if TYPE_CHECKING:
    from src.ui.notepad_window import NotepadWindow


class NotepadFindDialog(QDialog):
    """Modeless find/replace panel (current tab + optional all tabs)."""

    def __init__(self, window: NotepadWindow, *, replace_mode: bool = False) -> None:
        super().__init__(window)
        self._window = window
        self.setObjectName("notepadFindDialog")
        self.setWindowTitle("替换" if replace_mode else "查找")
        self.setModal(False)
        self.setMinimumWidth(420)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(14, 12, 14, 12)
        layout.setSpacing(10)

        grid = QGridLayout()
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(8)
        grid.addWidget(QLabel("查找内容"), 0, 0)
        self.find_edit = QLineEdit()
        self.find_edit.setClearButtonEnabled(True)
        self.find_edit.returnPressed.connect(self._find_next)
        grid.addWidget(self.find_edit, 0, 1)

        self.replace_label = QLabel("替换为")
        grid.addWidget(self.replace_label, 1, 0)
        self.replace_edit = QLineEdit()
        self.replace_edit.setClearButtonEnabled(True)
        self.replace_edit.returnPressed.connect(self._replace_one)
        grid.addWidget(self.replace_edit, 1, 1)
        layout.addLayout(grid)

        opts = QHBoxLayout()
        self.case_cb = QCheckBox("区分大小写")
        self.word_cb = QCheckBox("全字匹配")
        self.wrap_cb = QCheckBox("循环查找")
        self.wrap_cb.setChecked(True)
        self.all_tabs_cb = QCheckBox("所有页签（全部替换）")
        opts.addWidget(self.case_cb)
        opts.addWidget(self.word_cb)
        opts.addWidget(self.wrap_cb)
        opts.addStretch()
        layout.addLayout(opts)
        layout.addWidget(self.all_tabs_cb)

        btn_row = QHBoxLayout()
        btn_row.addStretch()
        self.btn_find_prev = QPushButton("查找上一个")
        self.btn_find_prev.setObjectName("secondaryBtn")
        self.btn_find_prev.clicked.connect(self._find_previous)
        btn_row.addWidget(self.btn_find_prev)

        self.btn_find_next = QPushButton("查找下一个")
        self.btn_find_next.setObjectName("primaryBtn")
        self.btn_find_next.clicked.connect(self._find_next)
        btn_row.addWidget(self.btn_find_next)

        self.btn_replace = QPushButton("替换")
        self.btn_replace.setObjectName("secondaryBtn")
        self.btn_replace.clicked.connect(self._replace_one)
        btn_row.addWidget(self.btn_replace)

        self.btn_replace_all = QPushButton("全部替换")
        self.btn_replace_all.setObjectName("secondaryBtn")
        self.btn_replace_all.clicked.connect(self._replace_all)
        btn_row.addWidget(self.btn_replace_all)

        close_btn = QPushButton("关闭")
        close_btn.setObjectName("secondaryBtn")
        close_btn.clicked.connect(self.close)
        btn_row.addWidget(close_btn)
        layout.addLayout(btn_row)

        self._set_replace_mode(replace_mode)

        QShortcut(QKeySequence("Esc"), self, activated=self.close)
        QShortcut(QKeySequence("F3"), self, activated=self._find_next)
        QShortcut(QKeySequence("Shift+F3"), self, activated=self._find_previous)

    def _set_replace_mode(self, enabled: bool) -> None:
        self.replace_label.setVisible(enabled)
        self.replace_edit.setVisible(enabled)
        self.btn_replace.setVisible(enabled)
        self.btn_replace_all.setVisible(enabled)
        self.all_tabs_cb.setVisible(enabled)
        self.setWindowTitle("查找和替换" if enabled else "查找")

    def open_find(self, *, replace: bool = False) -> None:
        page = self._window._current_page()
        if page is not None:
            sel = page.editor.textCursor().selectedText().replace("\u2029", "\n")
            if sel.strip():
                self.find_edit.setText(sel)
        self._set_replace_mode(replace)
        self.show()
        self.raise_()
        self.activateWindow()
        self.find_edit.setFocus(Qt.FocusReason.ActiveWindowFocusReason)
        if replace:
            self.replace_edit.setFocus(Qt.FocusReason.OtherFocusReason)
            self.find_edit.setFocus(Qt.FocusReason.ActiveWindowFocusReason)

    def _options(self) -> dict:
        return {
            "case_sensitive": self.case_cb.isChecked(),
            "whole_word": self.word_cb.isChecked(),
            "wrap": self.wrap_cb.isChecked(),
        }

    def _needle(self) -> str:
        return self.find_edit.text()

    def _replacement(self) -> str:
        return self.replace_edit.text()

    def _current_editor(self):
        page = self._window._current_page()
        return page.editor if page is not None else None

    def _find_next(self) -> None:
        editor = self._current_editor()
        needle = self._needle()
        if editor is None or not needle:
            return
        opts = self._options()
        found = find_next(
            editor.document(),
            editor.textCursor(),
            needle,
            case_sensitive=opts["case_sensitive"],
            whole_word=opts["whole_word"],
            wrap=opts["wrap"],
        )
        if found is None:
            QMessageBox.information(self, "查找", "未找到匹配内容。")
            return
        editor.setTextCursor(found)
        editor.setFocus()

    def _find_previous(self) -> None:
        editor = self._current_editor()
        needle = self._needle()
        if editor is None or not needle:
            return
        opts = self._options()
        found = find_previous(
            editor.document(),
            editor.textCursor(),
            needle,
            case_sensitive=opts["case_sensitive"],
            whole_word=opts["whole_word"],
            wrap=opts["wrap"],
        )
        if found is None:
            QMessageBox.information(self, "查找", "未找到匹配内容。")
            return
        editor.setTextCursor(found)
        editor.setFocus()

    def _replace_one(self) -> None:
        editor = self._current_editor()
        needle = self._needle()
        if editor is None or not needle:
            return
        opts = self._options()
        cursor = editor.textCursor()
        if replace_once(
            cursor,
            needle,
            self._replacement(),
            case_sensitive=opts["case_sensitive"],
            whole_word=opts["whole_word"],
        ):
            editor.setTextCursor(cursor)
            self._find_next()
            return
        self._find_next()

    def _replace_all(self) -> None:
        needle = self._needle()
        if not needle:
            return
        opts = self._options()
        replacement = self._replacement()
        if self.all_tabs_cb.isChecked():
            total = 0
            tabs = 0
            for page in self._window._pages:
                n = replace_all_in_document(
                    page.editor.document(),
                    needle,
                    replacement,
                    case_sensitive=opts["case_sensitive"],
                    whole_word=opts["whole_word"],
                )
                if n:
                    tabs += 1
                    total += n
            self._window._update_window_title()
            self._window._update_status()
            QMessageBox.information(
                self,
                "全部替换",
                f"已在 {tabs} 个页签中替换 {total} 处。",
            )
            return
        editor = self._current_editor()
        if editor is None:
            return
        count = replace_all_in_document(
            editor.document(),
            needle,
            replacement,
            case_sensitive=opts["case_sensitive"],
            whole_word=opts["whole_word"],
        )
        self._window._update_window_title()
        self._window._update_status()
        QMessageBox.information(self, "全部替换", f"已替换 {count} 处。")
