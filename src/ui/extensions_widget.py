"""Extensions panel: meeting minutes and page-folder shortcuts."""

from __future__ import annotations

from pathlib import Path

from PyQt6.QtCore import Qt, pyqtSignal
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
from src.ui.action_icons import make_action_icon
from src.ui.compact_stepper import CompactCountStepper
from src.ui.section_card import SectionCard


class ExtensionsWidget(QWidget):
    extensions_changed = pyqtSignal()
    hotkey_capture_began = pyqtSignal()
    hotkey_capture_ended = pyqtSignal()

    def __init__(self, settings: dict, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings
        self._build_ui()
        self.reload_tables()

    def _panel_layout(self, panel: QFrame) -> QVBoxLayout:
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(16, 12, 16, 12)
        layout.setSpacing(10)
        return layout

    def _build_ui(self) -> None:
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)

        content = QWidget()
        content.setObjectName("extensionsPanel")
        layout = QVBoxLayout(content)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(14)

        minutes_panel = QFrame()
        minutes_panel.setObjectName("panelCard")
        minutes_layout = self._panel_layout(minutes_panel)
        minutes_title = QLabel("会议纪要")
        minutes_title.setObjectName("panelTitle")
        minutes_layout.addWidget(minutes_title)
        from src.meeting_minutes import resolve_minutes_folder

        minutes_hint = QLabel(
            "桌面分页栏底部「纪要」按钮双击后会在此文件夹生成 Word 文档，"
            "文件名格式为 年月日_流水号（如 20260803_001.docx）。"
            "留空则使用安装目录下的「纪要」文件夹。"
            "关闭后分页栏不显示该按钮。"
        )
        minutes_hint.setObjectName("appSubtitle")
        minutes_hint.setWordWrap(True)
        minutes_layout.addWidget(minutes_hint)
        minutes_cfg = self.settings.setdefault("meeting_minutes", {})
        if not isinstance(minutes_cfg, dict):
            minutes_cfg = {}
            self.settings["meeting_minutes"] = minutes_cfg
        self.minutes_enabled_cb = QCheckBox("在分页栏显示「纪要」")
        self.minutes_enabled_cb.setChecked(bool(minutes_cfg.get("enabled", True)))
        self.minutes_enabled_cb.setToolTip("仅控制分页栏按钮显示。")
        self.minutes_enabled_cb.stateChanged.connect(self._on_minutes_enabled_changed)
        minutes_layout.addWidget(self.minutes_enabled_cb)
        folder_row = QHBoxLayout()
        folder_row.addWidget(QLabel("保存文件夹"))
        self.minutes_folder_edit = QLineEdit(str(minutes_cfg.get("folder") or ""))
        self.minutes_folder_edit.setPlaceholderText(
            str(resolve_minutes_folder(self.settings, ensure=False))
        )
        self.minutes_folder_edit.setToolTip(
            "留空则使用安装目录下的「纪要」文件夹。可点浏览选择文件夹。"
        )
        self.minutes_folder_edit.editingFinished.connect(self._on_minutes_folder_edited)
        folder_row.addWidget(self.minutes_folder_edit, stretch=1)
        browse_btn = QPushButton("浏览…")
        browse_btn.setObjectName("secondaryBtn")
        browse_btn.clicked.connect(self._browse_minutes_folder)
        folder_row.addWidget(browse_btn)
        reset_min_btn = QPushButton("默认")
        reset_min_btn.setObjectName("secondaryBtn")
        reset_min_btn.setToolTip("清空自定义路径，改回安装目录\\纪要")
        reset_min_btn.clicked.connect(self._reset_minutes_folder)
        folder_row.addWidget(reset_min_btn)
        minutes_layout.addLayout(folder_row)
        layout.addWidget(minutes_panel)

        npp_panel = QFrame()
        npp_panel.setObjectName("panelCard")
        npp_layout = self._panel_layout(npp_panel)
        npp_title = QLabel("DeskNote")
        npp_title.setObjectName("panelTitle")
        npp_layout.addWidget(npp_title)
        from src.notepad import notepad_settings, resolve_notes_folder

        npp_hint = QLabel(
            "DeskNote 随 DeskTidy 一并安装，是独立记事本进程。"
            "支持 Markdown 预览与大纲；Ctrl+F 查找、Ctrl+H 替换、F3 查找下一个。"
            "默认保存在安装目录下的「笔记」文件夹。"
            "下方勾选仅控制是否启用（创建桌面 / 开始菜单快捷方式）。"
        )
        npp_hint.setObjectName("appSubtitle")
        npp_hint.setWordWrap(True)
        npp_layout.addWidget(npp_hint)

        npp_cfg = notepad_settings(self.settings)
        self.notepad_enabled_cb = QCheckBox("启用 DeskNote（桌面 + 开始菜单快捷方式）")
        self.notepad_enabled_cb.setChecked(bool(npp_cfg.get("enabled", True)))
        self.notepad_enabled_cb.setToolTip(
            "勾选时创建/恢复 DeskNote.lnk；取消勾选则删除该快捷方式（不卸载程序）。"
        )
        self.notepad_enabled_cb.stateChanged.connect(self._on_notepad_enabled_changed)
        npp_layout.addWidget(self.notepad_enabled_cb)
        npp_row = QHBoxLayout()
        npp_row.addWidget(QLabel("默认文件夹"))
        self.notepad_folder_edit = QLineEdit(str(npp_cfg.get("folder") or ""))
        self.notepad_folder_edit.setPlaceholderText(
            str(resolve_notes_folder(self.settings, ensure=False))
        )
        self.notepad_folder_edit.editingFinished.connect(self._on_notepad_folder_edited)
        npp_row.addWidget(self.notepad_folder_edit, stretch=1)
        npp_browse = QPushButton("浏览…")
        npp_browse.setObjectName("secondaryBtn")
        npp_browse.clicked.connect(self._browse_notepad_folder)
        npp_row.addWidget(npp_browse)
        npp_layout.addLayout(npp_row)
        layout.addWidget(npp_panel)

        wallpaper_panel = QFrame()
        wallpaper_panel.setObjectName("panelCard")
        wp_layout = self._panel_layout(wallpaper_panel)
        wp_title = QLabel("桌面壁纸")
        wp_title.setObjectName("panelTitle")
        wp_layout.addWidget(wp_title)
        wp_hint = QLabel(
            "未配置时不会改动系统壁纸。配置壁纸库后，可在此更换或从库中随机更换。"
            "支持 JPG / PNG / BMP。"
        )
        wp_hint.setObjectName("appSubtitle")
        wp_hint.setWordWrap(True)
        wp_layout.addWidget(wp_hint)
        wp_cfg = self.settings.setdefault("wallpaper", {})
        if not isinstance(wp_cfg, dict):
            wp_cfg = {}
            self.settings["wallpaper"] = wp_cfg
        wp_folder_row = QHBoxLayout()
        wp_folder_row.addWidget(QLabel("壁纸库文件夹"))
        self.wallpaper_folder_edit = QLineEdit(str(wp_cfg.get("library_folder") or ""))
        self.wallpaper_folder_edit.setPlaceholderText("可选：存放壁纸图片的文件夹…")
        self.wallpaper_folder_edit.editingFinished.connect(self._on_wallpaper_folder_edited)
        wp_folder_row.addWidget(self.wallpaper_folder_edit, stretch=1)
        wp_browse = QPushButton("浏览…")
        wp_browse.setObjectName("secondaryBtn")
        wp_browse.clicked.connect(self._browse_wallpaper_folder)
        wp_folder_row.addWidget(wp_browse)
        wp_layout.addLayout(wp_folder_row)
        wp_btn_row = QHBoxLayout()
        pick_btn = QPushButton("更换壁纸…")
        pick_btn.setObjectName("primaryBtn")
        pick_btn.clicked.connect(self._pick_wallpaper)
        random_btn = QPushButton("从库随机更换")
        random_btn.setObjectName("secondaryBtn")
        random_btn.clicked.connect(self._random_wallpaper)
        wp_btn_row.addWidget(pick_btn)
        wp_btn_row.addWidget(random_btn)
        wp_btn_row.addStretch()
        wp_layout.addLayout(wp_btn_row)
        layout.addWidget(wallpaper_panel)

        shot_cfg = self.settings.setdefault("screenshot", {})
        if not isinstance(shot_cfg, dict):
            shot_cfg = {}
            self.settings["screenshot"] = shot_cfg
        hotkeys = self.settings.setdefault("hotkeys", {})
        if not isinstance(hotkeys, dict):
            hotkeys = {}
            self.settings["hotkeys"] = hotkeys
        self.hotkey_inputs: dict[str, object] = {}

        screenshot_panel = QFrame()
        screenshot_panel.setObjectName("panelCard")
        shot_layout = self._panel_layout(screenshot_panel)
        shot_title = QLabel("区域截图")
        shot_title.setObjectName("panelTitle")
        shot_layout.addWidget(shot_title)
        shot_hint = QLabel(
            "框选屏幕区域进行截图。可在下方配置全局快捷键；"
            "「显示贴图」(F2) 每按一次多贴一张（从最近往旧），"
            "不超过「同时贴图最多」。"
        )
        shot_hint.setObjectName("appSubtitle")
        shot_hint.setWordWrap(True)
        shot_layout.addWidget(shot_hint)
        self.screenshot_pin_cb = QCheckBox("截图复制后自动贴图到桌面")
        self.screenshot_pin_cb.setChecked(bool(shot_cfg.get("auto_pin_after_copy", False)))
        self.screenshot_pin_cb.stateChanged.connect(self._on_screenshot_options_changed)
        shot_layout.addWidget(self.screenshot_pin_cb)

        from src.screenshot_manager import (
            DEFAULT_MAX_PINS,
            MAX_PINS_HARD_LIMIT,
            clamp_max_pins,
        )
        from src.ui.hotkey_edit import HotkeyEdit

        pin_count_row = QHBoxLayout()
        pin_count_row.setSpacing(12)
        pin_label = QLabel("同时贴图最多")
        pin_label.setObjectName("fieldLabel")
        pin_label.setMinimumWidth(120)
        pin_count_row.addWidget(pin_label)
        self.screenshot_max_pins_spin = CompactCountStepper(
            1,
            MAX_PINS_HARD_LIMIT,
            clamp_max_pins(shot_cfg.get("max_pins", DEFAULT_MAX_PINS)),
            suffix="张",
        )
        self.screenshot_max_pins_spin.setToolTip(
            f"桌面同时保留的贴图数量（1–{MAX_PINS_HARD_LIMIT}），"
            "用于 F2「显示贴图」、截图「贴图」与自动贴图。"
        )
        self.screenshot_max_pins_spin.valueChanged.connect(self._on_screenshot_max_pins_changed)
        pin_count_row.addWidget(self.screenshot_max_pins_spin)
        pin_count_row.addStretch(1)
        shot_layout.addLayout(pin_count_row)

        shot_key_row = QHBoxLayout()
        shot_key_row.addWidget(QLabel("截图快捷键"))
        self.hotkey_inputs["screenshot"] = HotkeyEdit(str(hotkeys.get("screenshot") or ""))
        self.hotkey_inputs["screenshot"].hotkey_changed.connect(
            lambda key: self._on_hotkey_changed("screenshot", key)
        )
        self._wire_hotkey_edit(self.hotkey_inputs["screenshot"])
        shot_key_row.addWidget(self.hotkey_inputs["screenshot"], stretch=1)
        shot_layout.addLayout(shot_key_row)
        pin_key_row = QHBoxLayout()
        pin_key_row.addWidget(QLabel("显示贴图快捷键"))
        self.hotkey_inputs["show_screenshot"] = HotkeyEdit(
            str(hotkeys.get("show_screenshot") or "")
        )
        self.hotkey_inputs["show_screenshot"].hotkey_changed.connect(
            lambda key: self._on_hotkey_changed("show_screenshot", key)
        )
        self._wire_hotkey_edit(self.hotkey_inputs["show_screenshot"])
        pin_key_row.addWidget(self.hotkey_inputs["show_screenshot"], stretch=1)
        shot_layout.addLayout(pin_key_row)
        layout.addWidget(screenshot_panel)

        from src.screen_record_manager import default_output_dir

        rec_cfg = self.settings.setdefault("screen_record", {})
        if not isinstance(rec_cfg, dict):
            rec_cfg = {}
            self.settings["screen_record"] = rec_cfg

        record_panel = QFrame()
        record_panel.setObjectName("panelCard")
        rec_layout = self._panel_layout(record_panel)
        rec_title = QLabel("桌面录屏")
        rec_title.setObjectName("panelTitle")
        rec_layout.addWidget(rec_title)
        rec_hint = QLabel(
            "使用下方快捷键开始/结束录屏。每次开始时会先选择要录制的屏幕。"
            "安装版已内置 FFmpeg；留空保存目录则使用安装目录下的「录屏」文件夹。"
        )
        rec_hint.setObjectName("appSubtitle")
        rec_hint.setWordWrap(True)
        rec_layout.addWidget(rec_hint)
        self.screen_record_enabled_cb = QCheckBox("在分页栏显示「录屏」")
        self.screen_record_enabled_cb.setChecked(bool(rec_cfg.get("enabled", False)))
        self.screen_record_enabled_cb.setToolTip(
            "仅控制分页栏与托盘入口；全局快捷键始终可用。安装版已内置 FFmpeg。"
        )
        self.screen_record_enabled_cb.stateChanged.connect(self._on_screen_record_enabled_changed)
        rec_layout.addWidget(self.screen_record_enabled_cb)

        rec_key_row = QHBoxLayout()
        rec_key_row.addWidget(QLabel("录屏快捷键"))
        self.hotkey_inputs["screen_record"] = HotkeyEdit(
            str(hotkeys.get("screen_record") or "F3")
        )
        self.hotkey_inputs["screen_record"].hotkey_changed.connect(
            lambda key: self._on_hotkey_changed("screen_record", key)
        )
        self._wire_hotkey_edit(self.hotkey_inputs["screen_record"])
        rec_key_row.addWidget(self.hotkey_inputs["screen_record"], stretch=1)
        rec_layout.addLayout(rec_key_row)
        rec_dir_row = QHBoxLayout()
        rec_dir_row.addWidget(QLabel("保存目录"))
        self.screen_record_dir_edit = QLineEdit(str(rec_cfg.get("output_dir") or ""))
        self.screen_record_dir_edit.setPlaceholderText(str(default_output_dir(ensure=False)))
        self.screen_record_dir_edit.setToolTip(
            "留空则使用安装目录下的「录屏」文件夹。可点浏览选择文件夹。"
        )
        self.screen_record_dir_edit.editingFinished.connect(self._on_screen_record_dir_edited)
        rec_dir_row.addWidget(self.screen_record_dir_edit, stretch=1)
        browse_rec_btn = QPushButton("浏览…")
        browse_rec_btn.setObjectName("secondaryBtn")
        browse_rec_btn.clicked.connect(self._browse_screen_record_dir)
        rec_dir_row.addWidget(browse_rec_btn)
        reset_rec_btn = QPushButton("默认")
        reset_rec_btn.setObjectName("secondaryBtn")
        reset_rec_btn.setToolTip("清空自定义路径，改回安装目录\\录屏")
        reset_rec_btn.clicked.connect(self._reset_screen_record_dir)
        rec_dir_row.addWidget(reset_rec_btn)
        open_rec_btn = QPushButton("打开")
        open_rec_btn.setObjectName("secondaryBtn")
        open_rec_btn.setToolTip("在资源管理器中打开当前保存目录")
        open_rec_btn.clicked.connect(self._open_screen_record_dir)
        rec_dir_row.addWidget(open_rec_btn)
        rec_layout.addLayout(rec_dir_row)
        layout.addWidget(record_panel)

        from src.fd_search import file_search_settings

        fs_cfg = file_search_settings(self.settings)
        self.settings["file_search"] = fs_cfg
        search_panel = QFrame()
        search_panel.setObjectName("panelCard")
        fs_layout = self._panel_layout(search_panel)
        fs_title = QLabel("文件搜索")
        fs_title.setObjectName("panelTitle")
        fs_layout.addWidget(fs_title)
        fs_hint = QLabel(
            "内置开源工具 fd，按文件名快速查找。默认搜桌面 / 文档 / 下载，"
            "并包含当前已打开的资源管理器文件夹；本地搜完后可点弹窗上的「全局搜索」扫全盘。"
        )
        fs_hint.setObjectName("appSubtitle")
        fs_hint.setWordWrap(True)
        fs_layout.addWidget(fs_hint)
        fs_key_row = QHBoxLayout()
        fs_key_row.addWidget(QLabel("搜索快捷键"))
        self.hotkey_inputs["file_search"] = HotkeyEdit(
            str(hotkeys.get("file_search") or "F4")
        )
        self.hotkey_inputs["file_search"].hotkey_changed.connect(
            lambda key: self._on_hotkey_changed("file_search", key)
        )
        self._wire_hotkey_edit(self.hotkey_inputs["file_search"])
        fs_key_row.addWidget(self.hotkey_inputs["file_search"], stretch=1)
        fs_layout.addLayout(fs_key_row)
        layout.addWidget(search_panel)

        calc_panel = QFrame()
        calc_panel.setObjectName("panelCard")
        calc_layout = self._panel_layout(calc_panel)
        calc_title = QLabel("计算器")
        calc_title.setObjectName("panelTitle")
        calc_layout.addWidget(calc_title)
        calc_hint = QLabel(
            "分页栏可显示「计算器」按钮（双击打开系统计算器）；"
            "再次操作可收起（已在前台时最小化）。下方全局快捷键始终可用。"
        )
        calc_hint.setObjectName("appSubtitle")
        calc_hint.setWordWrap(True)
        calc_layout.addWidget(calc_hint)
        from src.calculator import calculator_settings

        calc_cfg = calculator_settings(self.settings)
        self.calculator_enabled_cb = QCheckBox("在分页栏显示「计算器」")
        self.calculator_enabled_cb.setChecked(bool(calc_cfg.get("enabled", False)))
        self.calculator_enabled_cb.setToolTip(
            "仅控制分页栏按钮；全局快捷键始终可用。"
        )
        self.calculator_enabled_cb.stateChanged.connect(self._on_calculator_enabled_changed)
        calc_layout.addWidget(self.calculator_enabled_cb)
        calc_key_row = QHBoxLayout()
        calc_key_row.addWidget(QLabel("快捷键"))
        self.hotkey_inputs["calculator"] = HotkeyEdit(
            str(hotkeys.get("calculator") or "Ctrl+Alt+C")
        )
        self.hotkey_inputs["calculator"].hotkey_changed.connect(
            lambda key: self._on_hotkey_changed("calculator", key)
        )
        self._wire_hotkey_edit(self.hotkey_inputs["calculator"])
        calc_key_row.addWidget(self.hotkey_inputs["calculator"], stretch=1)
        calc_layout.addLayout(calc_key_row)
        layout.addWidget(calc_panel)

        todo_panel = QFrame()
        todo_panel.setObjectName("panelCard")
        todo_layout = self._panel_layout(todo_panel)
        todo_title = QLabel("桌面待办")
        todo_title.setObjectName("panelTitle")
        todo_layout.addWidget(todo_title)
        todo_hint = QLabel(
            "在桌面显示待办便签。"
            "可鼠标添加、删除、勾选完成（完成项带删除线）。"
            "拖动标题栏可移动位置；分页栏「待办」可置顶面板。"
        )
        todo_hint.setObjectName("appSubtitle")
        todo_hint.setWordWrap(True)
        todo_layout.addWidget(todo_hint)
        from src.todos import desktop_todos_settings

        todo_cfg = desktop_todos_settings(self.settings)
        self.desktop_todos_enabled_cb = QCheckBox("在分页栏显示「待办」")
        self.desktop_todos_enabled_cb.setChecked(bool(todo_cfg.get("enabled", False)))
        self.desktop_todos_enabled_cb.setToolTip(
            "仅控制分页栏按钮与桌面待办面板的显示。"
        )
        self.desktop_todos_enabled_cb.stateChanged.connect(self._on_desktop_todos_enabled_changed)
        todo_layout.addWidget(self.desktop_todos_enabled_cb)
        layout.addWidget(todo_panel)

        page_folders_card = SectionCard(
            "分页栏文件夹",
            "显示在右侧分页栏下方。双击打开文件夹；右键打开所在位置。",
        )
        pf_toolbar = QHBoxLayout()
        pf_toolbar.setContentsMargins(0, 0, 0, 0)
        pf_toolbar.setSpacing(8)
        add_pf_btn = QPushButton("添加文件夹")
        add_pf_btn.setObjectName("primaryBtn")
        add_pf_btn.clicked.connect(self._add_page_folder)
        pf_toolbar.addWidget(add_pf_btn)
        pf_toolbar.addStretch()
        page_folders_card.add_body_layout(pf_toolbar)

        self._page_folders_host = QWidget()
        self._page_folders_host.setObjectName("pageFoldersList")
        self._page_folders_layout = QVBoxLayout(self._page_folders_host)
        self._page_folders_layout.setContentsMargins(0, 0, 0, 0)
        self._page_folders_layout.setSpacing(6)
        page_folders_card.add_body_widget(self._page_folders_host)
        # Compat alias for older selftests / callers.
        self.page_folders_table = self._page_folders_host
        layout.addWidget(page_folders_card)

        layout.addStretch()
        scroll.setWidget(content)
        outer.addWidget(scroll)

    def reload_tables(self) -> None:
        from src.page_folders import get_page_folder_items

        folders = get_page_folder_items(self.settings)
        while self._page_folders_layout.count():
            item = self._page_folders_layout.takeAt(0)
            widget = item.widget() if item is not None else None
            if widget is not None:
                widget.deleteLater()

        if not folders:
            empty = QLabel("还没有快捷文件夹 — 添加后会出现在右侧分页栏")
            empty.setObjectName("pageFoldersEmpty")
            empty.setWordWrap(True)
            empty.setAlignment(Qt.AlignmentFlag.AlignCenter)
            self._page_folders_layout.addWidget(empty)
            return

        for index, entry in enumerate(folders):
            self._page_folders_layout.addWidget(
                self._make_page_folder_row(index, entry)
            )

    def _make_page_folder_row(self, index: int, entry: dict) -> QWidget:
        row = QFrame()
        row.setObjectName("pageFolderRow")
        row.setFrameShape(QFrame.Shape.NoFrame)
        lay = QHBoxLayout(row)
        lay.setContentsMargins(12, 10, 10, 10)
        lay.setSpacing(10)

        text_col = QVBoxLayout()
        text_col.setContentsMargins(0, 0, 0, 0)
        text_col.setSpacing(2)
        name = QLabel(str(entry.get("name") or "未命名"))
        name.setObjectName("pageFolderName")
        path = QLabel(str(entry.get("path") or ""))
        path.setObjectName("pageFolderPath")
        path.setWordWrap(True)
        text_col.addWidget(name)
        text_col.addWidget(path)
        lay.addLayout(text_col, stretch=1)

        trash = QPushButton()
        trash.setObjectName("iconDangerBtn")
        trash.setIcon(make_action_icon("trash", danger=True))
        trash.setToolTip("删除此快捷方式")
        trash.setFixedSize(28, 28)
        trash.setCursor(Qt.CursorShape.PointingHandCursor)
        trash.clicked.connect(lambda _=False, i=index: self._delete_page_folder_at(i))
        lay.addWidget(trash)
        return row

    def _wire_hotkey_edit(self, edit) -> None:
        edit.capture_began.connect(self.hotkey_capture_began.emit)
        edit.capture_ended.connect(self.hotkey_capture_ended.emit)

    def _on_hotkey_changed(self, action: str, key: str) -> None:
        hotkeys = self.settings.setdefault("hotkeys", {})
        if not isinstance(hotkeys, dict):
            hotkeys = {}
            self.settings["hotkeys"] = hotkeys
        hotkeys[action] = key.strip()
        save_settings(self.settings)
        self.extensions_changed.emit()

    def _on_screenshot_options_changed(self, _state: int) -> None:
        shot = self.settings.setdefault("screenshot", {})
        if not isinstance(shot, dict):
            shot = {}
            self.settings["screenshot"] = shot
        shot["auto_pin_after_copy"] = self.screenshot_pin_cb.isChecked()
        save_settings(self.settings)
        self.extensions_changed.emit()

    def _on_screenshot_max_pins_changed(self, value: int) -> None:
        from src.screenshot_manager import clamp_max_pins

        shot = self.settings.setdefault("screenshot", {})
        if not isinstance(shot, dict):
            shot = {}
            self.settings["screenshot"] = shot
        shot["max_pins"] = clamp_max_pins(value)
        save_settings(self.settings)
        self.extensions_changed.emit()

    def _on_screen_record_enabled_changed(self, _state: int) -> None:
        rec = self.settings.setdefault("screen_record", {})
        if not isinstance(rec, dict):
            rec = {}
            self.settings["screen_record"] = rec
        rec["enabled"] = self.screen_record_enabled_cb.isChecked()
        save_settings(self.settings)
        self.extensions_changed.emit()

    def _on_minutes_enabled_changed(self, _state: int) -> None:
        cfg = self.settings.setdefault("meeting_minutes", {})
        if not isinstance(cfg, dict):
            cfg = {}
            self.settings["meeting_minutes"] = cfg
        cfg["enabled"] = self.minutes_enabled_cb.isChecked()
        save_settings(self.settings)
        self.extensions_changed.emit()

    def _on_notepad_enabled_changed(self, _state: int) -> None:
        from src.desknote import sync_desknote_shortcuts
        from src.notepad import notepad_settings

        cfg = notepad_settings(self.settings)
        cfg["enabled"] = self.notepad_enabled_cb.isChecked()
        # Persist immediately so a concurrent debounced save cannot revive
        # enabled=True and recreate deskNote.lnk after we delete it.
        save_settings(self.settings, immediate=True)
        sync_desknote_shortcuts(self.settings)
        self.extensions_changed.emit()

    def _on_calculator_enabled_changed(self, _state: int) -> None:
        from src.calculator import calculator_settings

        cfg = calculator_settings(self.settings)
        cfg["enabled"] = self.calculator_enabled_cb.isChecked()
        save_settings(self.settings)
        self.extensions_changed.emit()

    def _on_desktop_todos_enabled_changed(self, _state: int) -> None:
        from src.todos import desktop_todos_settings

        cfg = desktop_todos_settings(self.settings)
        cfg["enabled"] = self.desktop_todos_enabled_cb.isChecked()
        save_settings(self.settings)
        self.extensions_changed.emit()

    def _save_minutes_folder(self, folder: str) -> None:
        cfg = self.settings.setdefault("meeting_minutes", {})
        if not isinstance(cfg, dict):
            cfg = {}
            self.settings["meeting_minutes"] = cfg
        cfg["folder"] = folder.strip()
        save_settings(self.settings)
        self.extensions_changed.emit()

    def _on_minutes_folder_edited(self) -> None:
        self._save_minutes_folder(self.minutes_folder_edit.text())

    def _browse_minutes_folder(self) -> None:
        from PyQt6.QtWidgets import QFileDialog

        from src.meeting_minutes import resolve_minutes_folder

        start = self.minutes_folder_edit.text().strip() or str(
            resolve_minutes_folder(self.settings, ensure=False)
        )
        path = QFileDialog.getExistingDirectory(self, "选择会议纪要保存文件夹", start)
        if not path:
            return
        self.minutes_folder_edit.setText(path)
        self._save_minutes_folder(path)

    def _reset_minutes_folder(self) -> None:
        from src.meeting_minutes import default_minutes_folder

        self.minutes_folder_edit.clear()
        self.minutes_folder_edit.setPlaceholderText(str(default_minutes_folder(ensure=False)))
        self._save_minutes_folder("")

    def _save_notepad_folder(self, folder: str) -> None:
        from src.notepad import notepad_settings

        cfg = notepad_settings(self.settings)
        cfg["folder"] = folder.strip()
        save_settings(self.settings)
        self.extensions_changed.emit()

    def _on_notepad_folder_edited(self) -> None:
        self._save_notepad_folder(self.notepad_folder_edit.text())

    def _browse_notepad_folder(self) -> None:
        from PyQt6.QtWidgets import QFileDialog

        from src.notepad import resolve_notes_folder

        start = self.notepad_folder_edit.text().strip() or str(
            resolve_notes_folder(self.settings, ensure=False)
        )
        path = QFileDialog.getExistingDirectory(self, "选择笔记默认文件夹", start)
        if not path:
            return
        self.notepad_folder_edit.setText(path)
        self._save_notepad_folder(path)

    def _save_wallpaper_folder(self, folder: str) -> None:
        from src.wallpaper import wallpaper_settings

        cfg = wallpaper_settings(self.settings)
        cfg["library_folder"] = folder.strip()
        save_settings(self.settings)
        self.extensions_changed.emit()

    def _on_wallpaper_folder_edited(self) -> None:
        self._save_wallpaper_folder(self.wallpaper_folder_edit.text())

    def _browse_wallpaper_folder(self) -> None:
        from PyQt6.QtWidgets import QFileDialog

        start = self.wallpaper_folder_edit.text().strip() or str(Path.home())
        path = QFileDialog.getExistingDirectory(self, "选择壁纸库文件夹", start)
        if not path:
            return
        self.wallpaper_folder_edit.setText(path)
        self._save_wallpaper_folder(path)

    def _pick_wallpaper(self) -> None:
        from src.i18n import show_info, show_warning
        from src.wallpaper import pick_and_set_wallpaper

        try:
            applied = pick_and_set_wallpaper(self, self.settings)
        except ValueError as exc:
            show_warning(self, "桌面壁纸", str(exc))
            return
        except OSError as exc:
            show_warning(self, "桌面壁纸", f"设置失败：{exc}")
            return
        if applied is None:
            return
        save_settings(self.settings)
        self.extensions_changed.emit()
        show_info(self, "桌面壁纸", "壁纸已更换。")

    def _random_wallpaper(self) -> None:
        from src.i18n import show_info, show_warning
        from src.wallpaper import set_random_wallpaper_from_library

        try:
            applied = set_random_wallpaper_from_library(self.settings)
        except ValueError as exc:
            show_warning(self, "桌面壁纸", str(exc))
            return
        except OSError as exc:
            show_warning(self, "桌面壁纸", f"设置失败：{exc}")
            return
        save_settings(self.settings)
        self.extensions_changed.emit()
        show_info(self, "桌面壁纸", f"已更换为：{applied.name}")

    def _add_page_folder(self) -> None:
        from PyQt6.QtWidgets import QFileDialog, QInputDialog

        from src.page_folders import get_page_folder_items, set_page_folder_items

        start = str(Path.home())
        path = QFileDialog.getExistingDirectory(self, "选择要快捷打开的文件夹", start)
        if not path:
            return
        name, ok = QInputDialog.getText(
            self, "名称", "分页栏显示名称：", text=Path(path).name
        )
        if not ok:
            return
        items = get_page_folder_items(self.settings)
        items.append({"name": name.strip() or Path(path).name, "path": path})
        set_page_folder_items(self.settings, items)
        save_settings(self.settings)
        self.reload_tables()
        self.extensions_changed.emit()

    def _delete_page_folder_at(self, index: int) -> None:
        from src.page_folders import get_page_folder_items, set_page_folder_items

        items = get_page_folder_items(self.settings)
        if index < 0 or index >= len(items):
            return
        items.pop(index)
        set_page_folder_items(self.settings, items)
        save_settings(self.settings)
        self.reload_tables()
        self.extensions_changed.emit()

    def _save_screen_record_dir(self, folder: str) -> None:
        rec = self.settings.setdefault("screen_record", {})
        if not isinstance(rec, dict):
            rec = {}
            self.settings["screen_record"] = rec
        rec["output_dir"] = folder.strip()
        save_settings(self.settings)
        self.extensions_changed.emit()

    def _on_screen_record_dir_edited(self) -> None:
        self._save_screen_record_dir(self.screen_record_dir_edit.text())

    def _browse_screen_record_dir(self) -> None:
        from PyQt6.QtWidgets import QFileDialog

        from src.screen_record_manager import default_output_dir, resolve_output_dir

        current = self.screen_record_dir_edit.text().strip()
        start = str(
            resolve_output_dir(current, ensure=False)
            if current
            else default_output_dir(ensure=False)
        )
        path = QFileDialog.getExistingDirectory(self, "选择录屏保存文件夹", start)
        if not path:
            return
        self.screen_record_dir_edit.setText(path)
        self._save_screen_record_dir(path)

    def _reset_screen_record_dir(self) -> None:
        self.screen_record_dir_edit.clear()
        self._save_screen_record_dir("")

    def _open_screen_record_dir(self) -> None:
        import os

        from src.i18n import show_info
        from src.screen_record_manager import resolve_output_dir

        try:
            folder = resolve_output_dir(
                self.screen_record_dir_edit.text().strip(), ensure=True
            )
        except OSError as exc:
            show_info(self, "无法打开目录", str(exc))
            return
        os.startfile(str(folder))  # noqa: S606
