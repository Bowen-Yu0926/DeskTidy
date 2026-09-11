"""Fence management and style editor UI."""

from __future__ import annotations

import uuid

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QColor
from PyQt6.QtWidgets import (
    QCheckBox,
    QColorDialog,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QPushButton,
    QScrollArea,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from src.ui.no_wheel_combo import (
    NoWheelComboBox,
    NoWheelDoubleSpinBox,
    NoWheelSlider,
    NoWheelSpinBox,
)

from src.fence_pages import fence_on_page, get_fence_pages, set_fence_pages
from src.fence_rules import (
    fence_rules_summary,
    get_portal_path,
    is_portal_fence,
    set_portal_path,
)
from src.i18n import ask_yes_no, normalize_view_mode, show_info, view_mode_label
from src.fence_layout import (
    DEFAULT_NEW_FENCE_HEIGHT,
    DEFAULT_NEW_FENCE_WIDTH,
    place_new_fence_config,
    save_fence_geometry,
)
from src.settings import save_settings
from src.ui.organize_kind_picker import OrganizeKindPicker
from src.ui.styles import fence_theme_defaults
from src.fence_style import (
    FENCE_STYLE_PRESETS,
    fence_style_preset_ids,
    fence_style_visual_from_preset,
)


class FenceEditDialog(QDialog):
    def __init__(
        self,
        fence: dict | None,
        pages: list[dict] | None = None,
        parent=None,
    ):
        super().__init__(parent)
        self.setWindowTitle("编辑分区" if fence else "新建分区")
        self.setMinimumWidth(560)
        self.setMinimumHeight(620)
        from src.system_defaults import (
            SYSTEM_COMMON_FENCE_ID,
            SYSTEM_DOCS_FENCE_ID,
            is_locked_fence,
        )

        self._locked = is_locked_fence(fence) if fence else False
        fence = fence or {}
        self._existing_id = fence.get("id")
        if str(self._existing_id or "") in {SYSTEM_COMMON_FENCE_ID, SYSTEM_DOCS_FENCE_ID}:
            self._locked = True
        defaults = fence_theme_defaults()
        style = {**defaults, **fence.get("style", {})}
        pages = pages or [{"id": 0, "name": "默认"}]
        selected_pages = set(get_fence_pages(fence))

        outer = QVBoxLayout(self)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        form_host = QWidget()
        layout = QFormLayout(form_host)

        self.name_edit = QLineEdit(fence.get("name", ""))
        self.name_edit.setPlaceholderText("分区名称")
        layout.addRow("名称", self.name_edit)

        # Folder Portal (Fences-like): mirror a real directory instead of pins.
        portal_row = QHBoxLayout()
        self.portal_cb = QCheckBox("文件夹门户（镜像真实文件夹）")
        self.portal_cb.setChecked(is_portal_fence(fence) and not self._locked)
        self.portal_cb.setEnabled(not self._locked)
        self.portal_cb.setToolTip(
            "开启后分区显示所选文件夹的顶层内容；拖入文件会真实移入该文件夹。"
            "系统默认分区不支持门户。"
        )
        self.portal_path_edit = QLineEdit(str(get_portal_path(fence) or ""))
        self.portal_path_edit.setPlaceholderText("选择要镜像的文件夹…")
        self.portal_path_edit.setEnabled(self.portal_cb.isChecked())
        browse_btn = QPushButton("浏览…")
        browse_btn.setEnabled(self.portal_cb.isChecked() and not self._locked)
        browse_btn.clicked.connect(self._browse_portal_folder)
        self.portal_cb.toggled.connect(self.portal_path_edit.setEnabled)
        self.portal_cb.toggled.connect(browse_btn.setEnabled)
        portal_row.addWidget(self.portal_path_edit, 1)
        portal_row.addWidget(browse_btn)
        layout.addRow(self.portal_cb, portal_row)

        from src.fence_rules import get_fence_organize_kinds

        self.kind_picker = OrganizeKindPicker(get_fence_organize_kinds(fence))
        self.kind_picker.setEnabled(not self.portal_cb.isChecked())
        self.portal_cb.toggled.connect(lambda on: self.kind_picker.setEnabled(not on))
        layout.addRow("整理规则", self.kind_picker)

        self.preset_combo = NoWheelComboBox()
        self.preset_combo.addItem("（自定义 / 保持当前）", "")
        for preset_id in fence_style_preset_ids():
            meta = FENCE_STYLE_PRESETS[preset_id]
            self.preset_combo.addItem(str(meta.get("name") or preset_id), preset_id)
        self.preset_combo.currentIndexChanged.connect(self._on_preset_chosen)
        layout.addRow("外观预设", self.preset_combo)

        try:
            opacity_val = float(style.get("opacity", 0.88))
        except (TypeError, ValueError):
            opacity_val = 0.88
        opacity_val = max(0.35, min(1.0, opacity_val))
        opacity_row = QHBoxLayout()
        self.opacity_slider = NoWheelSlider(Qt.Orientation.Horizontal)
        self.opacity_slider.setRange(35, 100)
        self.opacity_slider.setSingleStep(1)
        self.opacity_slider.setPageStep(5)
        self.opacity_slider.setValue(int(round(opacity_val * 100)))
        self.opacity_pct = QLabel(f"{self.opacity_slider.value()}%")
        self.opacity_pct.setMinimumWidth(42)
        self.opacity_spin = NoWheelDoubleSpinBox()
        self.opacity_spin.setRange(0.35, 1.0)
        self.opacity_spin.setSingleStep(0.05)
        self.opacity_spin.setDecimals(2)
        self.opacity_spin.setValue(opacity_val)
        self.opacity_spin.setVisible(False)
        self.opacity_slider.valueChanged.connect(self._on_opacity_slider)
        self.opacity_spin.valueChanged.connect(self._on_opacity_spin)
        opacity_row.addWidget(self.opacity_slider, 1)
        opacity_row.addWidget(self.opacity_pct)
        layout.addRow("透明度", opacity_row)

        self.bg_edit = QLineEdit(str(style.get("background", "#20242C")))
        bg_row = QHBoxLayout()
        bg_row.addWidget(self.bg_edit, 1)
        bg_btn = QPushButton("取色")
        bg_btn.setObjectName("secondaryBtn")
        bg_btn.clicked.connect(lambda: self._pick_color(self.bg_edit))
        bg_row.addWidget(bg_btn)
        layout.addRow("背景色", bg_row)

        self.accent_edit = QLineEdit(str(style.get("accent", "#07C160")))
        accent_row = QHBoxLayout()
        accent_row.addWidget(self.accent_edit, 1)
        accent_btn = QPushButton("取色")
        accent_btn.setObjectName("secondaryBtn")
        accent_btn.clicked.connect(lambda: self._pick_color(self.accent_edit))
        accent_row.addWidget(accent_btn)
        layout.addRow("强调色", accent_row)

        self.radius_spin = NoWheelSpinBox()
        self.radius_spin.setRange(0, 28)
        self.radius_spin.setValue(int(style.get("border_radius", 10)))
        layout.addRow("圆角", self.radius_spin)

        self.view_combo = NoWheelComboBox()
        self.view_combo.addItem("网格", "grid")
        self.view_combo.addItem("列表", "list")
        view_mode = normalize_view_mode(style.get("view_mode", "grid"))
        for i in range(self.view_combo.count()):
            if self.view_combo.itemData(i) == view_mode:
                self.view_combo.setCurrentIndex(i)
        layout.addRow("视图模式", self.view_combo)

        self.show_title_cb = QCheckBox("显示标题栏")
        self.show_title_cb.setChecked(style.get("show_title", True))
        layout.addRow("", self.show_title_cb)

        self.collapsible_cb = QCheckBox("允许折叠")
        self.collapsible_cb.setChecked(style.get("collapsible", True))
        layout.addRow("", self.collapsible_cb)

        self.sort_combo = NoWheelComboBox()
        self.sort_combo.addItem("名称", "name")
        self.sort_combo.addItem("修改时间", "date")
        self.sort_combo.addItem("大小", "size")
        self.sort_combo.addItem("类型", "type")
        sort_by = fence.get("sort_by", "name")
        for i in range(self.sort_combo.count()):
            if self.sort_combo.itemData(i) == sort_by:
                self.sort_combo.setCurrentIndex(i)
        layout.addRow("排序方式", self.sort_combo)

        self.page_checks: list[QCheckBox] = []
        pages_box = QGroupBox("所属页面")
        pages_box.setObjectName("fencePagesGroup")
        pages_layout = QVBoxLayout(pages_box)
        pages_layout.setContentsMargins(12, 12, 12, 12)
        pages_layout.setSpacing(10)

        pages_hint = QLabel("可多选；同一分区可在多个分页显示，并分别保存拖拽位置。")
        pages_hint.setObjectName("sectionHint")
        pages_hint.setWordWrap(True)
        pages_layout.addWidget(pages_hint)

        pages_grid = QGridLayout()
        pages_grid.setHorizontalSpacing(18)
        pages_grid.setVerticalSpacing(8)
        for index, page in enumerate(pages):
            page_id = page.get("id", 0)
            cb = QCheckBox(page.get("name", f"页{page_id}"))
            cb.setProperty("page_id", page_id)
            cb.setChecked(page_id in selected_pages)
            self.page_checks.append(cb)
            pages_grid.addWidget(cb, index // 2, index % 2)
        pages_layout.addLayout(pages_grid)
        layout.addRow("", pages_box)

        scroll.setWidget(form_host)
        outer.addWidget(scroll)

        buttons = QDialogButtonBox(
            QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel
        )
        buttons.button(QDialogButtonBox.StandardButton.Ok).setText("确定")
        buttons.button(QDialogButtonBox.StandardButton.Cancel).setText("取消")
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        outer.addWidget(buttons)

    def _on_opacity_slider(self, value: int) -> None:
        pct = max(35, min(100, int(value)))
        self.opacity_pct.setText(f"{pct}%")
        spin = getattr(self, "opacity_spin", None)
        if spin is not None:
            spin.blockSignals(True)
            spin.setValue(pct / 100.0)
            spin.blockSignals(False)

    def _on_opacity_spin(self, value: float) -> None:
        pct = int(round(max(0.35, min(1.0, float(value))) * 100))
        slider = getattr(self, "opacity_slider", None)
        if slider is not None:
            slider.blockSignals(True)
            slider.setValue(pct)
            slider.blockSignals(False)
        label = getattr(self, "opacity_pct", None)
        if label is not None:
            label.setText(f"{pct}%")

    def _pick_color(self, edit: QLineEdit) -> None:
        current = QColor(edit.text().strip() or "#20242C")
        if not current.isValid():
            current = QColor("#20242C")
        chosen = QColorDialog.getColor(current, self, "选择颜色")
        if chosen.isValid():
            edit.setText(chosen.name().upper())

    def _on_preset_chosen(self, _index: int = 0) -> None:
        preset_id = self.preset_combo.currentData()
        if not preset_id:
            return
        visual = fence_style_visual_from_preset(str(preset_id))
        opacity = float(visual.get("opacity", 0.88))
        self.opacity_spin.setValue(opacity)
        if hasattr(self, "opacity_slider"):
            self.opacity_slider.setValue(int(round(max(0.35, min(1.0, opacity)) * 100)))
        self.bg_edit.setText(str(visual.get("background", "#20242C")))
        self.accent_edit.setText(str(visual.get("accent", "#07C160")))
        self.radius_spin.setValue(int(visual.get("border_radius", 10)))

    def _browse_portal_folder(self) -> None:
        from pathlib import Path as _P

        start = self.portal_path_edit.text().strip() or str(_P.home())
        chosen = QFileDialog.getExistingDirectory(self, "选择要镜像的文件夹", start)
        if chosen:
            self.portal_path_edit.setText(chosen)
            self.portal_cb.setChecked(True)

    def get_fence_config(self) -> dict:
        name = self.name_edit.text().strip() or "新分区"
        page_ids = [
            int(cb.property("page_id"))
            for cb in self.page_checks
            if cb.isChecked()
        ] or [0]
        style = {
            "opacity": float(self.opacity_slider.value()) / 100.0
            if hasattr(self, "opacity_slider")
            else self.opacity_spin.value(),
            "background": self.bg_edit.text().strip() or "#20242C",
            "accent": self.accent_edit.text().strip() or "#07C160",
            "border_radius": self.radius_spin.value(),
            "show_title": self.show_title_cb.isChecked(),
            "view_mode": normalize_view_mode(self.view_combo.currentData()),
            "collapsible": self.collapsible_cb.isChecked(),
        }
        config = {
            "id": self._existing_id or uuid.uuid4().hex[:8],
            "name": name,
            "folder": name,
            "organize_kinds": self.kind_picker.selected_kinds(),
            "extensions": [],
            "filename_patterns": [],
            "visible": True,
            "sort_by": self.sort_combo.currentData(),
            "style": style,
            "x": 50,
            "y": 50,
            "width": DEFAULT_NEW_FENCE_WIDTH,
            "height": DEFAULT_NEW_FENCE_HEIGHT,
            "collapsed": False,
        }
        preset_id = self.preset_combo.currentData()
        if preset_id:
            config["style_preset"] = str(preset_id)
        set_fence_pages(config, page_ids)
        if self.portal_cb.isChecked() and not self._locked:
            portal = self.portal_path_edit.text().strip()
            if portal:
                set_portal_path(config, portal)
            else:
                set_portal_path(config, None)
        else:
            set_portal_path(config, None)
        if self._locked:
            config["locked"] = True
            from src.system_defaults import SYSTEM_COMMON_FENCE_ID, SYSTEM_WORK_PAGE_ID

            config["id"] = self._existing_id or SYSTEM_COMMON_FENCE_ID
            if SYSTEM_WORK_PAGE_ID not in get_fence_pages(config):
                set_fence_pages(config, [SYSTEM_WORK_PAGE_ID, *get_fence_pages(config)])
            set_portal_path(config, None)
        return config


class FenceEditorWidget(QWidget):
    fences_changed = pyqtSignal()

    def __init__(
        self,
        settings: dict,
        parent: QWidget | None = None,
        *,
        show_page_column: bool = True,
        embedded: bool = False,
    ):
        super().__init__(parent)
        self.settings = settings
        self._show_page_column = show_page_column
        self._embedded = embedded
        self._page_filter: int | None = None
        self._row_fence_indices: list[int] = []
        self._table: QTableWidget | None = None
        self._action_buttons: list[QPushButton] = []
        self._build_ui()
        self.reload_table()

    def set_page_filter(self, page_id: int | None) -> None:
        self._page_filter = page_id
        self.reload_table()

    def table_widget(self) -> QTableWidget:
        assert self._table is not None
        return self._table

    def action_buttons(self) -> list[QPushButton]:
        return list(self._action_buttons)

    def _build_ui(self) -> None:
        headers = ["名称", "整理规则", "视图", "可见"]
        if self._show_page_column:
            headers.insert(2, "页面")

        self._table = QTableWidget()
        self._table.setColumnCount(len(headers))
        self._table.setHorizontalHeaderLabels(headers)
        self._table.horizontalHeader().setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        self._table.horizontalHeader().setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        if self._show_page_column:
            self._table.horizontalHeader().setSectionResizeMode(
                2, QHeaderView.ResizeMode.ResizeToContents
            )
        self._table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self._table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self._table.setAlternatingRowColors(True)
        self._table.verticalHeader().setVisible(False)
        self._table.setShowGrid(False)
        self._table.cellDoubleClicked.connect(self._on_table_double_clicked)

        add_btn = QPushButton("新建分区")
        add_btn.setObjectName("primaryBtn")
        add_btn.clicked.connect(self._add_fence)
        edit_btn = QPushButton("编辑")
        edit_btn.setObjectName("secondaryBtn")
        edit_btn.clicked.connect(self._edit_fence)
        del_btn = QPushButton("删除")
        del_btn.setObjectName("dangerBtn")
        del_btn.clicked.connect(self._delete_fence)
        visible_btn = QPushButton("切换可见")
        visible_btn.setObjectName("secondaryBtn")
        visible_btn.clicked.connect(self._toggle_visible)
        self._action_buttons = [add_btn, edit_btn, del_btn, visible_btn]

        if self._embedded:
            return

        layout = QVBoxLayout(self)
        hint = QLabel(
            "管理桌面分区：整理规则仅支持软件 / 文档两类（文档含文件夹与其它文件）；一键整理按类型钉选到分区（不移动原文件）。"
        )
        hint.setObjectName("appSubtitle")
        hint.setWordWrap(True)
        layout.addWidget(hint)
        layout.addWidget(self._table, stretch=1)
        btn_row = QHBoxLayout()
        for btn in self._action_buttons:
            btn_row.addWidget(btn)
        btn_row.addStretch()
        layout.addLayout(btn_row)

    @property
    def table(self) -> QTableWidget:
        return self.table_widget()

    def _page_name(self, page_id: int) -> str:
        for page in self.settings.get("desktop_pages", [{"id": 0, "name": "默认"}]):
            if page.get("id", 0) == page_id:
                return page.get("name", "默认")
        return "默认"

    def reload_table(self) -> None:
        fences = self.settings.get("fences", [])
        if self._page_filter is not None:
            fences = [
                fence
                for fence in fences
                if fence_on_page(fence, self._page_filter)
            ]
        table = self.table_widget()
        table.setRowCount(len(fences))
        self._row_fence_indices = []
        all_fences = self.settings.get("fences", [])
        for row, fence in enumerate(fences):
            self._row_fence_indices.append(all_fences.index(fence))
            page_name = self._page_name(get_fence_pages(fence)[0])
            view = view_mode_label(fence.get("style", {}).get("view_mode", "grid"))
            col = 0
            table.setItem(row, col, QTableWidgetItem(fence.get("name", "")))
            col += 1
            table.setItem(row, col, QTableWidgetItem(fence_rules_summary(fence)))
            col += 1
            if self._show_page_column:
                table.setItem(row, col, QTableWidgetItem(page_name))
                col += 1
            table.setItem(row, col, QTableWidgetItem(view))
            col += 1
            visible = fence.get("visible", True)
            visible_btn = QPushButton("显示" if visible else "隐藏")
            visible_btn.setObjectName("secondaryBtn")
            visible_btn.setMinimumWidth(72)
            visible_btn.setCursor(Qt.CursorShape.PointingHandCursor)
            fence_index = self._row_fence_indices[row]
            visible_btn.clicked.connect(
                lambda _checked=False, fi=fence_index: self._toggle_visible_at(fi)
            )
            table.setCellWidget(row, col, visible_btn)

    def _selected_index(self) -> int | None:
        rows = self.table_widget().selectionModel().selectedRows()
        if not rows:
            return None
        row = rows[0].row()
        if row < 0 or row >= len(getattr(self, "_row_fence_indices", [])):
            return None
        return self._row_fence_indices[row]

    def _on_table_double_clicked(self, row: int, _column: int) -> None:
        if row < 0:
            return
        self.table_widget().selectRow(row)
        self._edit_fence()

    def _add_fence(self) -> None:
        pages = self.settings.get("desktop_pages", [{"id": 0, "name": "默认"}])
        dialog = FenceEditDialog(None, pages, self)
        if self._page_filter is not None:
            for cb in dialog.page_checks:
                cb.setChecked(int(cb.property("page_id")) == self._page_filter)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        config = dialog.get_fence_config()
        fences = self.settings.setdefault("fences", [])
        page_ids = get_fence_pages(config)
        page_id = int(page_ids[0]) if page_ids else int(self._page_filter or 0)
        place_new_fence_config(self.settings, config, page_id)
        fences.append(config)
        save_fence_geometry(
            self.settings,
            config,
            get_fence_pages(config)[0],
            {"x": config["x"], "y": config["y"], "width": config["width"], "height": config["height"]},
        )
        save_settings(self.settings)
        self.reload_table()
        self.fences_changed.emit()

    def _edit_fence(self) -> None:
        idx = self._selected_index()
        if idx is None:
            show_info(self, "提示", "请先选择一个分区。")
            return
        fences = self.settings.get("fences", [])
        fence = fences[idx]
        pages = self.settings.get("desktop_pages", [{"id": 0, "name": "默认"}])
        dialog = FenceEditDialog(fence, pages, self)
        if dialog.exec() != QDialog.DialogCode.Accepted:
            return
        updated = dialog.get_fence_config()
        updated["x"] = fence.get("x", 50)
        updated["y"] = fence.get("y", 50)
        updated["width"] = fence.get("width", DEFAULT_NEW_FENCE_WIDTH)
        updated["height"] = fence.get("height", DEFAULT_NEW_FENCE_HEIGHT)
        updated["visible"] = fence.get("visible", True)
        updated["collapsed"] = fence.get("collapsed", False)
        updated["id"] = fence.get("id", updated.get("id"))
        updated["virtual_items"] = list(fence.get("virtual_items") or [])
        # Preserve / apply portal fields from the dialog result.
        if updated.get("portal_path"):
            pass
        else:
            updated.pop("portal_path", None)
            updated.pop("type", None)
        from src.system_defaults import is_locked_fence, locked_fence_home_page

        if is_locked_fence(fence):
            updated["locked"] = True
            updated["id"] = fence.get("id")
            home = locked_fence_home_page(fence)
            if home is not None:
                set_fence_pages(updated, [home])
            updated.pop("portal_path", None)
            updated.pop("type", None)
        fences[idx] = updated
        save_settings(self.settings)
        self.reload_table()
        self.fences_changed.emit()

    def _delete_fence(self) -> None:
        idx = self._selected_index()
        if idx is None:
            return
        fence = self.settings["fences"][idx]
        from src.system_defaults import is_locked_fence

        if is_locked_fence(fence):
            show_info(self, "提示", "系统默认分区不能删除。")
            return
        if not ask_yes_no(self, "确认删除", f"确定删除分区「{fence.get('name', '')}」？"):
            return
        self.settings["fences"].pop(idx)
        save_settings(self.settings)
        self.reload_table()
        self.fences_changed.emit()

    def _toggle_visible_at(self, fence_index: int) -> None:
        fences = self.settings.get("fences", [])
        if fence_index < 0 or fence_index >= len(fences):
            return
        fence = fences[fence_index]
        fence["visible"] = not fence.get("visible", True)
        save_settings(self.settings)
        self.reload_table()
        self.fences_changed.emit()

    def _toggle_visible(self) -> None:
        idx = self._selected_index()
        if idx is None:
            show_info(self, "提示", "请先选择一个分区。")
            return
        self._toggle_visible_at(idx)

    def _on_visible_changed(self, fence_index: int, combo: QComboBox) -> None:
        # Kept for compatibility; prefer button toggles.
        fences = self.settings.get("fences", [])
        if fence_index < 0 or fence_index >= len(fences):
            return
        visible = bool(combo.currentData())
        if fences[fence_index].get("visible", True) == visible:
            return
        fences[fence_index]["visible"] = visible
        save_settings(self.settings)
        self.fences_changed.emit()
