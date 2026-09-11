"""Edit desktop page name (organize rules belong to fences only)."""

from __future__ import annotations

from PyQt6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QLineEdit,
    QVBoxLayout,
)


class PageEditDialog(QDialog):
    def __init__(self, page: dict, parent=None):
        super().__init__(parent)
        self._page = page
        self.setWindowTitle("编辑分页")
        self.setMinimumWidth(360)

        outer = QVBoxLayout(self)
        form = QFormLayout()

        self.name_edit = QLineEdit(str(page.get("name") or ""))
        self.name_edit.setPlaceholderText("分页名称")
        form.addRow("名称", self.name_edit)
        outer.addLayout(form)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("确定")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("取消")
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        outer.addWidget(buttons)

    def apply_to(self, page: dict) -> None:
        # AIGC START
        page["name"] = self.name_edit.text().strip() or str(page.get("name") or "分页")
        page.pop("organize_kinds", None)
        # AIGC END
