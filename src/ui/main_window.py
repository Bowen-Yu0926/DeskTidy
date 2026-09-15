"""Main application window."""

from __future__ import annotations

from pathlib import Path

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import (
    QCheckBox,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QHeaderView,
    QLabel,
    QLineEdit,
    QListWidget,
    QMainWindow,
    QMenu,
    QPushButton,
    QScrollArea,
    QSplitter,
    QStackedWidget,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from src.account_vault import (
    add_session_listener,
    load_session,
    mask_login,
)
from src.desktop_scanner import format_size, scan_desktop
from src.fence_rules import all_fence_pinned_keys
from src.icon_utils import get_app_icon
from src.i18n import APP_NAME_ZH, ask_yes_no, show_about, show_info
from src.organizer import build_organize_preview_model, organize_desktop
from src.organize_whitelist import (
    add_organize_whitelist_entry,
    get_organize_whitelist,
    is_organize_whitelisted,
    remove_organize_whitelist_entry,
)
from src.public_desktop import public_claimed_keys
from src.ui.organize_preview_panel import OrganizePreviewPanel
from src.settings import is_auto_start_enabled, save_settings, set_auto_start
from src._version import __version__
from src.ui.brand_mark import BrandMark
from src.ui.desktop_layout_widget import DesktopLayoutWidget
from src.ui.extensions_widget import ExtensionsWidget
from src.ui.account_vault_settings_widget import AccountVaultSettingsWidget
from src.ui.pet_settings_widget import PetSettingsWidget
from src.ui.hotkey_edit import HotkeyEdit
from src.ui.snapshot_widget import SnapshotWidget
from src.ui.section_card import SectionCard
from src.ui.no_wheel_combo import NoWheelComboBox
from src.ui.sidebar_nav import SidebarNavWidget
from src.ui.styles import THEME_OPTIONS, get_theme_palette, normalize_theme


_NAV_ITEMS: list[tuple[str, str]] = [
    ("files", "整理"),
    ("fences", "桌面分区"),
    ("snapshot", "布局快照"),
    ("extensions", "扩展功能"),
    ("vault", "账号管理"),
    ("pet", "桌面宠物"),
    ("settings", "设置"),
    ("help", "帮助"),
]

_PAGE_SUBTITLES: dict[str, str] = {
    "files": "未归入分区的桌面项会出现在这里；一键整理只钉选显示，文件仍留在桌面。",
        "fences": "DeskTidy 分页（≠ Windows 虚拟桌面）与分区；上方可选外观样式预览并应用。",
    "snapshot": "保存分区布局与样式；卡片可预览、应用或删除。",
    "extensions": "会议纪要、壁纸、录屏保存目录与分页栏文件夹快捷方式。",
    "vault": "在此直接维护账号、密码、网址；启用后桌面浮标点击即可打开面板。",
    "pet": "独立宠物菜单：启用、交互行为与角色形象选择。",
    "settings": "主题、整理模式、快捷键与应用行为。",
    "help": "介绍与使用手册已合并；点击打开系统浏览器查看。",
}

_PAGE_KICKERS: dict[str, str] = {
    "files": "DESK · SORT",
    "fences": "DESK · ZONES",
    "snapshot": "DESK · SNAP",
    "extensions": "DESK · EXT",
    "vault": "DESK · VAULT",
    "pet": "DESK · PET",
    "settings": "DESK · PREFS",
    "help": "DESK · HELP",
}

_LAZY_PAGE_IDS: tuple[str, ...] = (
    "files",
    "fences",
    "snapshot",
    "extensions",
    "vault",
    "pet",
    "settings",
    "help",
)


class MainWindow(QMainWindow):
    # Optional list[int] of page ids that received new page-local floats.
    organize_requested = pyqtSignal(object)
    fences_toggle_requested = pyqtSignal(bool)
    fences_refresh_requested = pyqtSignal()
    settings_changed = pyqtSignal()
    theme_changed = pyqtSignal()
    hotkeys_changed = pyqtSignal()
    hotkey_capture_began = pyqtSignal()
    hotkey_capture_ended = pyqtSignal()
    fences_rebuild_requested = pyqtSignal()
    fence_visibility_changed = pyqtSignal(str, bool)
    fence_style_preview_requested = pyqtSignal(dict)
    fence_style_apply_requested = pyqtSignal(str)
    fence_style_revert_requested = pyqtSignal()
    fence_style_apply_one_requested = pyqtSignal(str, str)
    fence_opacity_changed = pyqtSignal(str, float)
    pages_changed = pyqtSignal()
    extensions_changed = pyqtSignal()
    pet_settings_changed = pyqtSignal()
    hide_desktop_icons_keep_fences = pyqtSignal()
    minimized_to_tray = pyqtSignal()
    settings_ui_shown = pyqtSignal()
    settings_ui_hidden = pyqtSignal()
    quit_requested = pyqtSignal()

    def __init__(self, settings: dict):
        super().__init__()
        self.settings = settings
        self._desktop_layout = None
        self._fence_editor = None
        self._page_manager = None
        self._snapshot_widget = None
        self._extensions_widget = None
        self._vault_settings_widget = None
        self._pet_settings_widget = None
        self._show_fences_cb = None
        self.hotkey_inputs: dict[str, HotkeyEdit] = {}
        self.setWindowTitle(f"{APP_NAME_ZH} v{__version__}")
        icon = get_app_icon()
        if not icon.isNull():
            self.setWindowIcon(icon)
        self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
        self.setMinimumSize(880, 600)
        self.resize(1120, 740)
        self.statusBar().show()

        central = QWidget()
        central.setObjectName("centralWidget")
        self.setCentralWidget(central)
        root = QHBoxLayout(central)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        root.addWidget(self._build_sidebar())
        root.addWidget(self._build_content_area(), stretch=1)
        # Help is a normal sidebar page (content on the right), not a top menu bar.
        self.menuBar().hide()
        self._current_page_id = "files"
        self._help_panel = None

    def _build_help_page(self) -> QWidget:
        from src.ui.help_dialog import HelpHomePage

        self._help_panel = HelpHomePage(
            self.settings,
            include_about=True,
            open_product=True,
        )
        return self._help_panel

    def _build_sidebar(self) -> QFrame:
        sidebar = QFrame()
        sidebar.setObjectName("sidebar")
        sidebar.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        sidebar.setFixedWidth(200)
        layout = QVBoxLayout(sidebar)
        layout.setContentsMargins(12, 16, 12, 12)
        layout.setSpacing(16)

        brand = QFrame()
        brand.setObjectName("sidebarBrand")
        brand_row = QHBoxLayout(brand)
        brand_row.setContentsMargins(2, 2, 2, 10)
        brand_row.setSpacing(10)

        palette = get_theme_palette(normalize_theme(self.settings.get("theme")))
        accent = palette["accent"]
        self._brand_mark = BrandMark(accent)
        brand_row.addWidget(self._brand_mark, 0, Qt.AlignmentFlag.AlignTop)

        text_col = QVBoxLayout()
        text_col.setSpacing(2)
        self._brand_title = QLabel(APP_NAME_ZH)
        self._brand_title.setObjectName("appTitle")
        self._brand_title.setWordWrap(True)
        text_col.addWidget(self._brand_title)
        self._brand_tag = QLabel("DESK · TIDY")
        self._brand_tag.setObjectName("brandTag")
        text_col.addWidget(self._brand_tag)
        self._brand_version = QLabel(f"v{__version__}")
        self._brand_version.setObjectName("appSubtitle")
        self._brand_version.setToolTip("点击查看关于")
        self._brand_version.setCursor(Qt.CursorShape.PointingHandCursor)
        self._brand_version.mousePressEvent = lambda _e: show_about(self)  # type: ignore[method-assign]
        text_col.addWidget(self._brand_version)
        self._account_name = QLabel("")
        self._account_name.setObjectName("appTitle")
        self._account_name.setWordWrap(True)
        self._account_name.setVisible(False)
        text_col.addWidget(self._account_name)
        self._account_login = QLabel("")
        self._account_login.setObjectName("appSubtitle")
        self._account_login.setWordWrap(True)
        self._account_login.setVisible(False)
        text_col.addWidget(self._account_login)
        brand_row.addLayout(text_col, stretch=1)
        layout.addWidget(brand)
        self._refresh_sidebar_brand()
        add_session_listener(self._refresh_sidebar_brand)

        self._nav = SidebarNavWidget(
            _NAV_ITEMS,
            accent=accent,
            muted=palette["sidebar_muted"],
            bright=palette["sidebar_text"],
        )
        self._nav.page_changed.connect(self._on_nav_changed)
        layout.addWidget(self._nav, stretch=1)

        tip = QLabel("整理只钉选显示，文件仍留在桌面。")
        tip.setObjectName("sidebarTip")
        tip.setWordWrap(True)
        layout.addWidget(tip)

        return sidebar

    def _refresh_sidebar_brand(self) -> None:
        """Show vault account in sidebar when logged in; otherwise DeskTidy branding."""
        title = getattr(self, "_brand_title", None)
        if title is None:
            return
        session = load_session()
        logged_in = session is not None
        self._brand_title.setVisible(not logged_in)
        self._brand_tag.setVisible(not logged_in)
        self._brand_version.setVisible(not logged_in)
        self._account_name.setVisible(logged_in)
        self._account_login.setVisible(logged_in)
        if logged_in:
            nick = str(session.get("display_name") or "").strip()
            masked = mask_login(str(session.get("login") or ""))
            self._account_name.setText(nick or masked or "已登录")
            if nick:
                self._account_login.setText(masked)
                self._account_login.setVisible(True)
            else:
                self._account_login.setText("")
                self._account_login.setVisible(False)
            self._account_name.setToolTip("账号管理已登录")
            self._account_login.setToolTip(str(session.get("login") or ""))

    def _build_content_area(self) -> QFrame:
        panel = QFrame()
        panel.setObjectName("contentPanel")
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(24, 18, 24, 18)
        layout.setSpacing(16)

        header = QVBoxLayout()
        header.setSpacing(8)
        self._page_kicker = QLabel(_PAGE_KICKERS["files"])
        self._page_kicker.setObjectName("pageKicker")
        header.addWidget(self._page_kicker)
        title_row = QHBoxLayout()
        title_row.setSpacing(10)
        self._page_title = QLabel("整理")
        self._page_title.setObjectName("pageTitle")
        title_row.addWidget(self._page_title, stretch=0)
        self._page_badge = QLabel("")
        self._page_badge.setObjectName("pageBadge")
        self._page_badge.setVisible(False)
        title_row.addWidget(self._page_badge, stretch=0)
        title_row.addStretch()
        header.addLayout(title_row)
        self._page_subtitle = QLabel(_PAGE_SUBTITLES["files"])
        self._page_subtitle.setObjectName("pageSubtitle")
        self._page_subtitle.setWordWrap(True)
        header.addWidget(self._page_subtitle)
        layout.addLayout(header)

        self._stack = QStackedWidget()
        self._stack.setObjectName("contentStack")
        self._lazy_built: set[str] = set()
        self._page_index = {pid: i for i, pid in enumerate(_LAZY_PAGE_IDS)}
        self._snapshot_restored_handler = None
        self._init_lazy_stack()
        layout.addWidget(self._stack, stretch=1)

        self._nav.set_current("files")
        return panel

    def _init_lazy_stack(self) -> None:
        """Reserve stack slots; build page widgets on first show/nav."""
        for page_id in _LAZY_PAGE_IDS:
            placeholder = QWidget()
            placeholder.setObjectName(f"lazyPlaceholder_{page_id}")
            self._stack.addWidget(placeholder)

    def set_snapshot_restored_handler(self, handler) -> None:
        self._snapshot_restored_handler = handler
        if "snapshot" in self._lazy_built and self._snapshot_widget is not None:
            self._snapshot_widget.snapshot_restored.connect(handler)

    def _ensure_page(self, page_id: str) -> None:
        if page_id in self._lazy_built:
            return
        widget = self._build_page_widget(page_id)
        idx = self._page_index[page_id]
        old = self._stack.widget(idx)
        self._stack.removeWidget(old)
        old.deleteLater()
        self._stack.insertWidget(idx, widget)
        self._lazy_built.add(page_id)
        if page_id == "snapshot" and self._snapshot_restored_handler is not None:
            self._snapshot_widget.snapshot_restored.connect(self._snapshot_restored_handler)

    def release_lazy_pages(self) -> None:
        """Drop built settings pages while the window stays in tray."""
        for page_id in list(self._lazy_built):
            idx = self._page_index[page_id]
            widget = self._stack.widget(idx)
            self._stack.removeWidget(widget)
            widget.deleteLater()
            placeholder = QWidget()
            placeholder.setObjectName(f"lazyPlaceholder_{page_id}")
            self._stack.insertWidget(idx, placeholder)
            self._lazy_built.discard(page_id)
        self._clear_lazy_page_refs()

    def _clear_lazy_page_refs(self) -> None:
        self.files_table = None  # type: ignore[assignment]
        self._files_empty = None
        self._files_empty_state = None
        self._files_status_pending = None
        self._files_status_organized = None
        self._files_status_total = None
        self._preview_panel = None
        self._desktop_layout = None
        self._fence_editor = None
        self._page_manager = None
        self._snapshot_widget = None
        self._extensions_widget = None
        self._vault_settings_widget = None
        self._pet_settings_widget = None
        self._help_panel = None
        self._show_fences_cb = None
        self.auto_start_cb = None
        self.auto_organize_cb = None
        self.double_click_hide_cb = None
        self.close_to_tray_cb = None
        self.desktop_menu_cb = None
        self.public_desktop_cb = None
        self.theme_combo = None
        self.hotkey_inputs = {}
        self.whitelist_list = None
        self.whitelist_edit = None

    @property
    def desktop_layout(self) -> DesktopLayoutWidget:
        self._ensure_page("fences")
        assert self._desktop_layout is not None
        return self._desktop_layout

    @property
    def fence_editor(self):
        self._ensure_page("fences")
        assert self._fence_editor is not None
        return self._fence_editor

    @property
    def page_manager(self):
        self._ensure_page("fences")
        assert self._page_manager is not None
        return self._page_manager

    @property
    def snapshot_widget(self) -> SnapshotWidget:
        self._ensure_page("snapshot")
        assert self._snapshot_widget is not None
        return self._snapshot_widget

    @property
    def extensions_widget(self) -> ExtensionsWidget:
        self._ensure_page("extensions")
        assert self._extensions_widget is not None
        return self._extensions_widget

    @property
    def vault_settings_widget(self) -> AccountVaultSettingsWidget:
        self._ensure_page("vault")
        assert self._vault_settings_widget is not None
        return self._vault_settings_widget

    @property
    def show_fences_cb(self) -> QCheckBox:
        self._ensure_page("settings")
        assert self._show_fences_cb is not None
        return self._show_fences_cb

    @property
    def pet_settings_widget(self) -> PetSettingsWidget:
        self._ensure_page("pet")
        assert self._pet_settings_widget is not None
        return self._pet_settings_widget

    def _build_page_widget(self, page_id: str) -> QWidget:
        if page_id == "files":
            return self._build_files_page()
        if page_id == "fences":
            return self._build_fences_page()
        if page_id == "snapshot":
            return self._build_snapshot_page()
        if page_id == "extensions":
            return self._build_extensions_page()
        if page_id == "vault":
            return self._build_vault_page()
        if page_id == "pet":
            return self._build_pet_page()
        if page_id == "settings":
            return self._build_settings_page()
        if page_id == "help":
            return self._build_help_page()
        raise ValueError(f"unknown page: {page_id}")

    def _build_files_page(self) -> QWidget:
        from src.ui.organize_preview_panel import OrganizeStatTile

        files_page = QWidget()
        files_layout = QVBoxLayout(files_page)
        files_layout.setContentsMargins(0, 0, 0, 0)
        files_layout.setSpacing(0)

        files_card = SectionCard(
            "待整理",
            "已在分区或公共区的项目不会出现在此列表。",
        )
        organize_btn = QPushButton("一键整理")
        organize_btn.setObjectName("primaryBtn")
        organize_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        organize_btn.clicked.connect(self._on_organize)
        preview_btn = QPushButton("预览")
        preview_btn.setObjectName("secondaryBtn")
        preview_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        preview_btn.clicked.connect(self._on_preview)
        refresh_btn = QPushButton("刷新")
        refresh_btn.setObjectName("secondaryBtn")
        refresh_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        refresh_btn.clicked.connect(self.refresh_data)
        files_card.add_header_widget(organize_btn)
        files_card.add_header_widget(preview_btn)
        files_card.add_header_widget(refresh_btn)

        status_row = QHBoxLayout()
        status_row.setContentsMargins(0, 0, 0, 0)
        status_row.setSpacing(10)
        self._files_status_pending = OrganizeStatTile("未归入")
        self._files_status_organized = OrganizeStatTile("已在分区")
        self._files_status_total = OrganizeStatTile("桌面合计")
        for tile in (
            self._files_status_pending,
            self._files_status_organized,
            self._files_status_total,
        ):
            status_row.addWidget(tile, stretch=1)
        files_card.add_body_layout(status_row)

        self._files_empty = QLabel("")
        self._files_empty.setObjectName("filesEmptyHint")
        self._files_empty.setWordWrap(True)
        self._files_empty.setVisible(False)
        files_card.add_body_widget(self._files_empty)

        split = QSplitter(Qt.Orientation.Horizontal)
        split.setObjectName("filesOrganizeSplitter")
        split.setChildrenCollapsible(False)
        self._files_split = split

        left = QWidget()
        left.setObjectName("filesListPane")
        left_lay = QVBoxLayout(left)
        left_lay.setContentsMargins(0, 0, 0, 0)
        left_lay.setSpacing(0)

        self._files_empty_state = QFrame()
        self._files_empty_state.setObjectName("filesEmptyState")
        empty_lay = QVBoxLayout(self._files_empty_state)
        empty_lay.setContentsMargins(28, 36, 28, 36)
        empty_lay.setSpacing(10)
        empty_kicker = QLabel("DESK · CLEAR")
        empty_kicker.setObjectName("filesEmptyKicker")
        empty_kicker.setAlignment(Qt.AlignmentFlag.AlignCenter)
        empty_title = QLabel("桌面已整理完毕")
        empty_title.setObjectName("filesEmptyTitle")
        empty_title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        empty_body = QLabel("新出现在桌面且匹配规则的项目会显示在左侧列表。")
        empty_body.setObjectName("filesEmptyBody")
        empty_body.setWordWrap(True)
        empty_body.setAlignment(Qt.AlignmentFlag.AlignCenter)
        empty_lay.addStretch(1)
        empty_lay.addWidget(empty_kicker)
        empty_lay.addWidget(empty_title)
        empty_lay.addWidget(empty_body)
        empty_lay.addStretch(2)
        self._files_empty_state.setVisible(False)
        left_lay.addWidget(self._files_empty_state, stretch=1)

        self.files_table = QTableWidget()
        self.files_table.setObjectName("filesTable")
        self.files_table.setColumnCount(3)
        self.files_table.setHorizontalHeaderLabels(["名称", "类型", "大小"])
        self.files_table.horizontalHeader().setSectionResizeMode(
            0, QHeaderView.ResizeMode.Stretch
        )
        self.files_table.horizontalHeader().setSectionResizeMode(
            1, QHeaderView.ResizeMode.ResizeToContents
        )
        self.files_table.horizontalHeader().setSectionResizeMode(
            2, QHeaderView.ResizeMode.ResizeToContents
        )
        self.files_table.horizontalHeader().setDefaultAlignment(
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter
        )
        self.files_table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers)
        self.files_table.setSelectionBehavior(QTableWidget.SelectionBehavior.SelectRows)
        self.files_table.setSelectionMode(QTableWidget.SelectionMode.SingleSelection)
        self.files_table.setAlternatingRowColors(True)
        self.files_table.verticalHeader().setVisible(False)
        self.files_table.verticalHeader().setDefaultSectionSize(40)
        self.files_table.setShowGrid(False)
        self.files_table.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.files_table.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.files_table.customContextMenuRequested.connect(self._on_files_context_menu)
        left_lay.addWidget(self.files_table, stretch=1)

        self._preview_panel = OrganizePreviewPanel()
        split.addWidget(left)
        split.addWidget(self._preview_panel)
        split.setStretchFactor(0, 5)
        split.setStretchFactor(1, 2)
        split.setSizes([640, 280])
        files_card.add_body_widget(split, stretch=1)
        files_layout.addWidget(files_card)
        return files_page

    def _build_fences_page(self) -> QWidget:
        # AIGC START
        fences_scroll = QScrollArea()
        fences_scroll.setWidgetResizable(True)
        fences_scroll.setFrameShape(QFrame.Shape.NoFrame)
        fences_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        fences_scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)

        self._desktop_layout = DesktopLayoutWidget(self.settings)
        self._desktop_layout.pages_changed.connect(self._on_pages_changed)
        self._desktop_layout.fences_changed.connect(self._on_fences_changed)
        self._desktop_layout.fence_visibility_changed.connect(
            self._on_fence_visibility_changed
        )
        self._desktop_layout.fence_style_preview_requested.connect(
            self.fence_style_preview_requested.emit
        )
        self._desktop_layout.fence_style_apply_requested.connect(
            self.fence_style_apply_requested.emit
        )
        self._desktop_layout.fence_style_revert_requested.connect(
            self.fence_style_revert_requested.emit
        )
        self._desktop_layout.fence_style_apply_one_requested.connect(
            self.fence_style_apply_one_requested.emit
        )
        self._desktop_layout.fence_opacity_changed.connect(
            self.fence_opacity_changed.emit
        )
        self._fence_editor = self._desktop_layout.fence_editor
        self._page_manager = self._desktop_layout.page_manager
        fences_scroll.setWidget(self._desktop_layout)
        return fences_scroll
        # AIGC END

    def _build_snapshot_page(self) -> QWidget:
        self._snapshot_widget = SnapshotWidget(self.settings)
        return self._snapshot_widget

    def _build_extensions_page(self) -> QWidget:
        self._extensions_widget = ExtensionsWidget(self.settings)
        self._extensions_widget.extensions_changed.connect(self._on_extensions_changed)
        self._extensions_widget.hotkey_capture_began.connect(self.hotkey_capture_began.emit)
        self._extensions_widget.hotkey_capture_ended.connect(self.hotkey_capture_ended.emit)
        return self._extensions_widget

    def _build_vault_page(self) -> QWidget:
        self._vault_settings_widget = AccountVaultSettingsWidget(self.settings)
        self._vault_settings_widget.vault_settings_changed.connect(self._on_extensions_changed)
        return self._vault_settings_widget

    def _build_pet_page(self) -> QWidget:
        self._pet_settings_widget = PetSettingsWidget(self.settings)
        self._pet_settings_widget.pet_settings_changed.connect(self._on_pet_settings_changed)
        return self._pet_settings_widget

    def _build_settings_page(self) -> QWidget:
        settings_scroll = QScrollArea()
        settings_scroll.setWidgetResizable(True)
        settings_scroll.setFrameShape(QFrame.Shape.NoFrame)
        settings_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)

        settings_widget = QWidget()
        settings_widget.setObjectName("settingsPanel")
        settings_layout = QVBoxLayout(settings_widget)
        settings_layout.setContentsMargins(0, 0, 0, 0)
        settings_layout.setSpacing(18)

        general_card = SectionCard("常规选项", "控制启动、整理与桌面交互行为。")
        general_body = QVBoxLayout()
        general_body.setSpacing(10)

        self.auto_start_cb = QCheckBox("开机自动启动")
        self.auto_start_cb.setChecked(is_auto_start_enabled())
        self.auto_start_cb.stateChanged.connect(self._on_auto_start_changed)

        self._show_fences_cb = QCheckBox("显示桌面分区")
        self._show_fences_cb.setChecked(self.settings.get("show_fences", True))
        self._show_fences_cb.stateChanged.connect(self._on_fences_toggle)

        self.auto_organize_cb = QCheckBox("启动时自动整理桌面（按规则钉选到分区）")
        self.auto_organize_cb.setChecked(self.settings.get("auto_organize_on_startup", False))
        self.auto_organize_cb.setToolTip(
            "启动后跑一次整理。需同时开启下方「新文件自动钉选」才会执行；"
            "关闭钉选时启动整理会跳过，避免重启把浮标又吸进分区。"
            "随时可用「一键整理」手动归类。"
        )
        self.auto_organize_cb.stateChanged.connect(self._on_auto_organize_changed)

        self.auto_watch_cb = QCheckBox("新桌面文件按规则自动钉选到分区")
        self.auto_watch_cb.setChecked(bool(self.settings.get("auto_organize_watch", False)))
        self.auto_watch_cb.setToolTip(
            "类似 Fences Rules：监听到桌面新增/另存为后，按分区「软件 / 文档」规则自动钉选。"
            "临时文件与整理白名单会跳过；从分区拖出后短时不会被立刻吸回。"
            "关闭后：新文件留在当前分页浮标，启动时也不会再自动按规则吸回；"
            "需「一键整理」或手动拖入分区。"
        )
        self.auto_watch_cb.stateChanged.connect(self._on_auto_watch_organize_changed)

        self.double_click_hide_cb = QCheckBox("双击桌面空白处隐藏/显示系统桌面图标")
        self.double_click_hide_cb.setChecked(self.settings.get("double_click_hide", True))
        self.double_click_hide_cb.stateChanged.connect(self._on_double_click_hide_changed)

        self.close_to_tray_cb = QCheckBox("关闭窗口后在后台运行（系统托盘）")
        self.close_to_tray_cb.setChecked(self.settings.get("close_to_tray", True))
        self.close_to_tray_cb.setToolTip("关闭主窗口时最小化到托盘；可随时在此修改。首次关闭也会询问并记住选择。")
        self.close_to_tray_cb.stateChanged.connect(self._on_close_to_tray_changed)

        self.desktop_menu_cb = QCheckBox("桌面右键菜单（新建分区）")
        self.desktop_menu_cb.setChecked(self.settings.get("desktop_right_click_menu", True))
        self.desktop_menu_cb.setToolTip(
            "开启后在系统桌面右键菜单顶部显示「DeskTidy」子菜单（新建分区等），与系统项一起出现"
        )
        self.desktop_menu_cb.stateChanged.connect(self._on_desktop_menu_changed)

        self.public_desktop_cb = QCheckBox("启用公共区域")
        self.public_desktop_cb.setChecked(
            self.settings.get("enable_public_desktop", False)
        )
        self.public_desktop_cb.setToolTip(
            "勾选：拖出分区的图标进入公共区域，所有分页都能看到；"
            "不勾选：拖出后仍显示在桌面，但只在当前分页可见。"
        )
        self.public_desktop_cb.stateChanged.connect(self._on_public_desktop_changed)

        for cb in (
            self.auto_start_cb,
            self._show_fences_cb,
            self.auto_organize_cb,
            self.auto_watch_cb,
            self.double_click_hide_cb,
            self.close_to_tray_cb,
            self.desktop_menu_cb,
            self.public_desktop_cb,
        ):
            general_body.addWidget(cb)

        mode_hint = QLabel(
            "分区为虚拟钉选，文件始终留在桌面。"
            "「新文件自动钉选」关闭时，新文件留在当前分页浮标，启动整理也不会自动吸回；"
            "需要归类时用「一键整理」或拖入分区。"
            "建议隐藏系统桌面图标，避免与分区重叠。"
        )
        mode_hint.setObjectName("sectionHint")
        mode_hint.setWordWrap(True)
        general_body.addWidget(mode_hint)
        general_card.add_body_layout(general_body)
        settings_layout.addWidget(general_card)

        appearance_card = SectionCard(
            "外观主题",
            "推荐「薄雾 / 石墨」；分区强调色会跟随主题。淡紫、浅杏偏柔和装饰向。",
        )
        appearance_body = QHBoxLayout()
        appearance_body.setSpacing(12)
        theme_label = QLabel("界面主题")
        theme_label.setMinimumWidth(72)
        appearance_body.addWidget(theme_label)
        self.theme_combo = NoWheelComboBox()
        current_theme = normalize_theme(self.settings.get("theme", "mist"))
        for theme_id, theme_name in THEME_OPTIONS:
            self.theme_combo.addItem(theme_name, theme_id)
        for i in range(self.theme_combo.count()):
            if self.theme_combo.itemData(i) == current_theme:
                self.theme_combo.setCurrentIndex(i)
                break
        self.theme_combo.currentIndexChanged.connect(self._on_theme_changed)
        appearance_body.addWidget(self.theme_combo, stretch=1)
        appearance_card.add_body_layout(appearance_body)
        settings_layout.addWidget(appearance_card)

        hotkey_card = SectionCard(
            "全局快捷键",
            "点击输入框后按下组合键即可设置；Delete/Backspace 清空表示不绑定。",
        )
        self.hotkey_inputs: dict[str, HotkeyEdit] = {}
        hotkeys = self.settings.get("hotkeys", {})
        hotkey_grid = QGridLayout()
        hotkey_grid.setHorizontalSpacing(16)
        hotkey_grid.setVerticalSpacing(10)
        from src.hotkey_manager import HOTKEY_LABELS

        for row, action in enumerate(
            (
                "organize",
                "toggle_fences",
                "toggle_icons",
                "peek_fences",
                "page_next",
                "page_prev",
            )
        ):
            name_label = QLabel(HOTKEY_LABELS[action])
            name_label.setMinimumWidth(140)
            edit = HotkeyEdit(hotkeys.get(action, ""))
            edit.hotkey_changed.connect(self._on_hotkeys_changed)
            self._wire_hotkey_edit(edit)
            self.hotkey_inputs[action] = edit
            hotkey_grid.addWidget(name_label, row, 0)
            hotkey_grid.addWidget(edit, row, 1)
        hotkey_grid.setColumnStretch(1, 1)
        hotkey_card.add_body_layout(hotkey_grid)
        settings_layout.addWidget(hotkey_card)

        whitelist_card = SectionCard(
            "整理白名单",
            "点「添加」选择文件加入白名单；也可在输入框填通配符（如 *.lnk）后回车。"
            "名单中的文件在一键整理 / 自动整理时都会跳过。",
        )
        whitelist_body = QVBoxLayout()
        whitelist_body.setSpacing(10)
        self.whitelist_list = QListWidget()
        self.whitelist_list.setMinimumHeight(120)
        whitelist_body.addWidget(self.whitelist_list)
        whitelist_row = QHBoxLayout()
        whitelist_row.setSpacing(8)
        self.whitelist_edit = QLineEdit()
        self.whitelist_edit.setPlaceholderText("也可输入通配符，如 *.tmp / 截图*，回车添加")
        self.whitelist_edit.returnPressed.connect(self._on_whitelist_add_typed)
        add_wl_btn = QPushButton("添加")
        add_wl_btn.setObjectName("secondaryBtn")
        add_wl_btn.setToolTip("打开文件选择框，把选中的文件加入白名单")
        add_wl_btn.clicked.connect(self._on_whitelist_add_pick)
        del_wl_btn = QPushButton("删除选中")
        del_wl_btn.setObjectName("dangerBtn")
        del_wl_btn.clicked.connect(self._on_whitelist_remove)
        whitelist_row.addWidget(self.whitelist_edit, stretch=1)
        whitelist_row.addWidget(add_wl_btn)
        whitelist_row.addWidget(del_wl_btn)
        whitelist_body.addLayout(whitelist_row)
        whitelist_card.add_body_layout(whitelist_body)
        settings_layout.addWidget(whitelist_card)
        self._reload_whitelist_list()

        backup_card = SectionCard("配置备份", "导出或导入完整应用配置。")
        export_row = QHBoxLayout()
        export_row.setSpacing(10)
        export_btn = QPushButton("导出配置")
        export_btn.setObjectName("secondaryBtn")
        export_btn.clicked.connect(self._export_settings)
        import_btn = QPushButton("导入配置")
        import_btn.setObjectName("secondaryBtn")
        import_btn.clicked.connect(self._import_settings)
        export_row.addWidget(export_btn)
        export_row.addWidget(import_btn)
        export_row.addStretch()
        backup_card.add_body_layout(export_row)
        settings_layout.addWidget(backup_card)
        settings_layout.addStretch()

        settings_scroll.setWidget(settings_widget)
        return settings_scroll

    def _on_nav_changed(self, page_id: str, title: str, index: int) -> None:
        self._current_page_id = page_id
        self._page_title.setText(title)
        self._page_kicker.setText(_PAGE_KICKERS.get(page_id, "DESK · CONSOLE"))
        self._page_subtitle.setText(_PAGE_SUBTITLES.get(page_id, ""))
        if self.isVisible():
            self._ensure_page(page_id)
        if page_id == "help" and self._help_panel is not None:
            self._help_panel.set_settings(self.settings)
            open_fn = getattr(self._help_panel, "_open_browser_manual", None)
            if callable(open_fn):
                open_fn()
        self._stack.setCurrentIndex(self._page_index[page_id])
        self._refresh_page_badge(page_id)
        if page_id == "files":
            self.refresh_data()
        elif page_id == "vault" and self._vault_settings_widget is not None:
            # Same pattern as files: lazy rebuild / first open must pull cloud list.
            self._vault_settings_widget.reload()

    def _refresh_page_badge(self, page_id: str | None = None) -> None:
        """Files: pending count. Fences: today's date. Others: hidden."""
        if not hasattr(self, "_page_badge"):
            return
        pid = page_id or getattr(self, "_current_page_id", "")
        if pid == "fences":
            from src.i18n import format_today_zh

            self._page_badge.setText(format_today_zh())
            self._page_badge.setToolTip("今天")
            self._page_badge.setVisible(True)
            return
        if pid == "files":
            # Text filled by refresh_data when list is scanned.
            return
        self._page_badge.setVisible(False)

    def is_files_page_visible(self) -> bool:
        if not hasattr(self, "_nav"):
            return False
        item = self._nav.currentItem()
        if item is None:
            return False
        return item.data(Qt.ItemDataRole.UserRole) == "files"

    def refresh_data_if_visible(self) -> bool:
        """Refresh the desktop file list only when the settings UI is open."""
        if not self.isVisible():
            return False
        if not self.is_files_page_visible():
            return False
        self.refresh_data()
        return True

    def refresh_data(self) -> None:
        self._ensure_page("files")
        exclude = self.settings.get("exclude_patterns", [])
        scan = scan_desktop(exclude=exclude)
        items = list(scan.items)
        from src.fence_rules import all_fence_pinned_keys, path_in_pinned_keys

        claimed = all_fence_pinned_keys(self.settings) | public_claimed_keys(self.settings)
        items = [
            item for item in items if not path_in_pinned_keys(item.path, claimed)
        ]
        folders = sum(1 for item in items if item.is_dir)
        files_n = len(items) - folders
        organized = max(0, len(scan.items) - len(items))

        self.files_table.setRowCount(len(items))
        for row, item in enumerate(items):
            name_item = QTableWidgetItem(item.name)
            name_item.setData(Qt.ItemDataRole.UserRole, str(item.path))
            if is_organize_whitelisted(item.path, self.settings):
                name_item.setToolTip("已在整理白名单中，一键整理会跳过")
            self.files_table.setItem(row, 0, name_item)
            type_text = "文件夹" if item.is_dir else (item.extension or "—")
            size_text = "—" if item.is_dir else format_size(item.size)
            type_item = QTableWidgetItem(type_text)
            size_item = QTableWidgetItem(size_text)
            type_item.setTextAlignment(
                int(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
            )
            size_item.setTextAlignment(
                int(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
            )
            self.files_table.setItem(row, 1, type_item)
            self.files_table.setItem(row, 2, size_item)

        if hasattr(self, "_page_badge"):
            self._page_badge.setText(f"未归入 {len(items)}")
            self._page_badge.setVisible(self.is_files_page_visible())
        if self._files_status_pending is not None:
            self._files_status_pending.set_value(len(items))
        if self._files_status_organized is not None:
            self._files_status_organized.set_value(organized)
        if self._files_status_total is not None:
            self._files_status_total.set_value(len(scan.items))
        if hasattr(self, "_files_empty") and self._files_empty is not None:
            if not items:
                self._files_empty.setVisible(False)
                self.files_table.setVisible(False)
                if self._files_empty_state is not None:
                    self._files_empty_state.setVisible(True)
                split = getattr(self, "_files_split", None)
                if split is not None:
                    split.setSizes([720, 240])
            else:
                bits = [f"{len(items)} 项待整理"]
                if files_n:
                    bits.append(f"文件 {files_n}")
                if folders:
                    from src.fence_rules import organize_accepts_folders

                    if organize_accepts_folders(self.settings):
                        bits.append(f"文件夹 {folders}")
                    else:
                        bits.append(f"文件夹 {folders}（未勾选文档规则）")
                self._files_empty.setText(" · ".join(bits))
                self._files_empty.setVisible(True)
                self.files_table.setVisible(True)
                if self._files_empty_state is not None:
                    self._files_empty_state.setVisible(False)
                split = getattr(self, "_files_split", None)
                if split is not None:
                    split.setSizes([640, 280])

        self._refresh_organize_preview()

        if not items:
            self.statusBar().showMessage(f"桌面已整理 · 共 {len(scan.items)} 项", 0)
        else:
            self.statusBar().showMessage(f"待整理 {len(items)} 项", 0)
        self.fences_refresh_requested.emit()
        try:
            from PyQt6.QtWidgets import QApplication

            from src.public_desktop import ensure_desktop_loose_floats
            from src.settings import save_settings

            app = QApplication.instance()
            desk = getattr(app, "_desktidy_app", None) if app else None
            if desk is not None and bool(self.settings.get("hide_shell_icons")):
                if ensure_desktop_loose_floats(self.settings):
                    save_settings(self.settings)
                refresh = getattr(desk, "refresh_public_desktop", None)
                if callable(refresh):
                    refresh(immediate=True, relayout=True)
        except Exception:
            pass

    def _on_files_context_menu(self, pos) -> None:
        index = self.files_table.indexAt(pos)
        if not index.isValid():
            return
        name_item = self.files_table.item(index.row(), 0)
        if name_item is None:
            return
        raw = name_item.data(Qt.ItemDataRole.UserRole) or name_item.text()
        path = Path(str(raw))
        from src.win_shell import open_containing_folder, open_path, reveal_in_explorer

        menu = QMenu(self)
        open_action = menu.addAction("打开")
        folder_action = menu.addAction("打开所在文件夹")
        reveal_action = menu.addAction("在资源管理器中显示")
        menu.addSeparator()
        if is_organize_whitelisted(path, self.settings):
            act = menu.addAction("移出整理白名单")
            chosen = menu.exec(self.files_table.viewport().mapToGlobal(pos))
            if chosen == open_action:
                open_path(path)
                return
            if chosen == folder_action:
                open_containing_folder(path)
                return
            if chosen == reveal_action:
                reveal_in_explorer(path)
                return
            if chosen == act and remove_organize_whitelist_entry(self.settings, path.name):
                save_settings(self.settings)
                self._reload_whitelist_list()
                self.refresh_data()
            return
        act = menu.addAction("加入整理白名单")
        chosen = menu.exec(self.files_table.viewport().mapToGlobal(pos))
        if chosen == open_action:
            open_path(path)
            return
        if chosen == folder_action:
            open_containing_folder(path)
            return
        if chosen == reveal_action:
            reveal_in_explorer(path)
            return
        if chosen == act and add_organize_whitelist_entry(self.settings, path.name):
            save_settings(self.settings)
            self._reload_whitelist_list()
            self.refresh_data()

    def _reload_whitelist_list(self) -> None:
        if not hasattr(self, "whitelist_list"):
            return
        self.whitelist_list.clear()
        for pattern in get_organize_whitelist(self.settings):
            self.whitelist_list.addItem(pattern)

    def _on_whitelist_add_pick(self) -> None:
        """Add button: pick desktop (or other) files via system file dialog."""
        from PyQt6.QtWidgets import QFileDialog

        from src.settings import get_desktop_path

        start_dir = str(get_desktop_path())
        paths, _ = QFileDialog.getOpenFileNames(
            self,
            "选择要加入整理白名单的文件",
            start_dir,
            "所有文件 (*.*)",
        )
        if not paths:
            return
        added = 0
        skipped = 0
        for raw in paths:
            if add_organize_whitelist_entry(self.settings, Path(raw)):
                added += 1
            else:
                skipped += 1
        if added:
            save_settings(self.settings)
            self._reload_whitelist_list()
            self.refresh_data()
        if added and skipped:
            show_info(
                self,
                "整理白名单",
                f"已添加 {added} 项，另有 {skipped} 项已在白名单中。",
            )
        elif not added and skipped:
            show_info(self, "整理白名单", "所选文件均已在白名单中。")

    def _on_whitelist_add_typed(self) -> None:
        """Line-edit Enter: add a typed name or glob pattern."""
        text = self.whitelist_edit.text().strip()
        if not text:
            # Empty input → same as clicking Add (pick files).
            self._on_whitelist_add_pick()
            return
        if add_organize_whitelist_entry(self.settings, text):
            save_settings(self.settings)
            self.whitelist_edit.clear()
            self._reload_whitelist_list()
            self.refresh_data()
        else:
            show_info(self, "整理白名单", "该项已在白名单中。")

    def _on_whitelist_remove(self) -> None:
        row = self.whitelist_list.currentRow()
        item = self.whitelist_list.currentItem()
        if item is None:
            return
        if remove_organize_whitelist_entry(self.settings, item.text()):
            save_settings(self.settings)
            self._reload_whitelist_list()
            if row >= 0:
                self.whitelist_list.setCurrentRow(min(row, self.whitelist_list.count() - 1))
            self.refresh_data()

    def _on_organize(self) -> None:
        msg = (
            "将把桌面上未归入分区的项目，按各分区的图标/文档规则钉选显示"
            "（已在分区内的不会变动，原文件仍留在桌面，不会移动）。是否继续？"
        )
        if not ask_yes_no(self, "确认整理", msg):
            return

        result = organize_desktop(settings=self.settings)
        note = result.summary
        pages = list(getattr(result, "float_page_ids", None) or [])
        if pages:
            from src.fence_rules import get_page_by_id

            names = []
            for pid in pages:
                page = get_page_by_id(self.settings, int(pid))
                names.append(str((page or {}).get("name") or f"分页 {pid}"))
            note = f"{result.summary}\n浮动图标在：{'、'.join(names)}（将切换到该分页显示）"
        show_info(self, "整理完成", note)
        self.refresh_data()
        self.organize_requested.emit(pages or None)

    def _refresh_organize_preview(self) -> None:
        """Dry-run organize and fill the side preview panel."""
        if self._preview_panel is None:
            return
        result = organize_desktop(settings=self.settings, dry_run=True)
        exclude = self.settings.get("exclude_patterns", [])
        scan = scan_desktop(exclude=exclude)
        claimed = all_fence_pinned_keys(self.settings) | public_claimed_keys(
            self.settings
        )

        from src.fence_rules import organize_accepts_folders, path_in_pinned_keys

        if organize_accepts_folders(self.settings):
            unclaimed_folders: list[str] = []
        else:
            unclaimed_folders = [
                item.name
                for item in scan.items
                if item.is_dir and not path_in_pinned_keys(item.path, claimed)
            ]
        model = build_organize_preview_model(
            result,
            self.settings,
            unclaimed_folders=unclaimed_folders,
        )
        self._preview_panel.apply_model(model)

    def _on_preview(self) -> None:
        # Show on the organize page instead of a modal dialog.
        if hasattr(self, "_nav"):
            self._nav.set_current("files")
        self._ensure_page("files")
        self._refresh_organize_preview()

    def _on_auto_start_changed(self, state: int) -> None:
        enabled = state == Qt.CheckState.Checked.value
        set_auto_start(enabled)
        self.settings["auto_start"] = enabled
        save_settings(self.settings)

    def _on_fences_toggle(self, state: int) -> None:
        visible = state == Qt.CheckState.Checked.value
        self.settings["show_fences"] = visible
        save_settings(self.settings)
        self.fences_toggle_requested.emit(visible)

    def _on_auto_organize_changed(self, state: int) -> None:
        enabled = state == Qt.CheckState.Checked.value
        self.settings["auto_organize_on_startup"] = enabled
        save_settings(self.settings)

    def _on_auto_watch_organize_changed(self, state: int) -> None:
        enabled = state == Qt.CheckState.Checked.value
        self.settings["auto_organize_watch"] = enabled
        save_settings(self.settings)

    def _on_double_click_hide_changed(self, state: int) -> None:
        enabled = state == Qt.CheckState.Checked.value
        self.settings["double_click_hide"] = enabled
        save_settings(self.settings)
        self.settings_changed.emit()

    def _on_close_to_tray_changed(self, state: int) -> None:
        enabled = state == Qt.CheckState.Checked.value
        self.settings["close_to_tray"] = enabled
        self.settings["close_behavior_prompted"] = True
        save_settings(self.settings)

    def _sync_close_to_tray_checkbox(self) -> None:
        if "settings" not in self._lazy_built:
            return
        if self.close_to_tray_cb is None:
            return
        self.close_to_tray_cb.blockSignals(True)
        self.close_to_tray_cb.setChecked(bool(self.settings.get("close_to_tray", True)))
        self.close_to_tray_cb.blockSignals(False)

    def _on_desktop_menu_changed(self, state: int) -> None:
        enabled = state == Qt.CheckState.Checked.value
        self.settings["desktop_right_click_menu"] = enabled
        save_settings(self.settings)
        self.settings_changed.emit()

    def _on_public_desktop_changed(self, state: int) -> None:
        from PyQt6.QtWidgets import QApplication

        from src.public_desktop import heal_public_area_scope

        enabled = state == Qt.CheckState.Checked.value
        self.settings["enable_public_desktop"] = enabled
        desk = getattr(QApplication.instance(), "_desktidy_app", None)
        page_id = (
            int(desk._current_page())
            if desk is not None and hasattr(desk, "_current_page")
            else int(self.settings.get("current_page", 0))
        )
        # Keep icons visible as page-local floats when turning public area off.
        heal_public_area_scope(self.settings, page_id)
        save_settings(self.settings)
        self.settings_changed.emit()

    def _apply_sidebar_accent(self, theme: str | None = None) -> None:
        palette = get_theme_palette(normalize_theme(theme or self.settings.get("theme")))
        accent = palette["accent"]
        mark = getattr(self, "_brand_mark", None)
        if mark is not None:
            mark.set_accent(accent)
        nav = getattr(self, "_nav", None)
        if nav is not None:
            nav.set_accent(
                accent,
                muted=palette["sidebar_muted"],
                bright=palette["sidebar_text"],
            )
        # Force QSS chrome (background / tip / nav) to repaint after palette swap.
        sidebar = self.findChild(QFrame, "sidebar")
        if sidebar is not None:
            style = sidebar.style()
            if style is not None:
                style.unpolish(sidebar)
                style.polish(sidebar)
            sidebar.update()
            for child in sidebar.findChildren(QWidget):
                child_style = child.style()
                if child_style is not None:
                    child_style.unpolish(child)
                    child_style.polish(child)
                child.update()

    def _sync_theme_combo(self) -> None:
        if self.theme_combo is None:
            return
        theme = normalize_theme(self.settings.get("theme"))
        self.theme_combo.blockSignals(True)
        for i in range(self.theme_combo.count()):
            if self.theme_combo.itemData(i) == theme:
                self.theme_combo.setCurrentIndex(i)
                break
        self.theme_combo.blockSignals(False)

    def _on_theme_changed(self, index: int) -> None:
        theme = self.theme_combo.itemData(index)
        self.settings["theme"] = normalize_theme(theme)
        save_settings(self.settings)
        self._apply_sidebar_accent(theme)
        # Theme only — do not emit settings_changed (that soft-refreshes every fence).
        self.theme_changed.emit()

    def _wire_hotkey_edit(self, edit: HotkeyEdit) -> None:
        edit.capture_began.connect(self.hotkey_capture_began.emit)
        edit.capture_ended.connect(self.hotkey_capture_ended.emit)

    def _on_hotkeys_changed(self) -> None:
        hotkeys = self.settings.setdefault("hotkeys", {})
        for action, edit in self.hotkey_inputs.items():
            hotkeys[action] = edit.text().strip()
        save_settings(self.settings)
        self.hotkeys_changed.emit()

    def _on_fences_changed(self) -> None:
        self.desktop_layout.reload_all()
        self.fences_rebuild_requested.emit()

    def _on_fence_visibility_changed(self, fence_id: str, visible: bool) -> None:
        self.fence_visibility_changed.emit(fence_id, visible)

    def _on_pages_changed(self) -> None:
        self.desktop_layout.reload_all()
        save_settings(self.settings)
        self.pages_changed.emit()

    def _on_extensions_changed(self) -> None:
        if self._extensions_widget is not None:
            self._extensions_widget.reload_tables()
        if self._vault_settings_widget is not None:
            self._vault_settings_widget.reload()
        self.extensions_changed.emit()

    def _on_pet_settings_changed(self) -> None:
        # Soft path: size/character/actions must not tear down via extensions
        # (settings UI open → chrome gate hides the recreated pet).
        self.pet_settings_changed.emit()

    def _export_settings(self) -> None:
        from PyQt6.QtWidgets import QFileDialog
        import json

        path, _ = QFileDialog.getSaveFileName(
            self, "导出配置", "desktidy_settings.json", "JSON (*.json)"
        )
        if not path:
            return
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.settings, f, ensure_ascii=False, indent=2)
        self.statusBar().showMessage(f"配置已导出到 {path}", 5000)

    def _import_settings(self) -> None:
        from PyQt6.QtWidgets import QFileDialog
        import json

        path, _ = QFileDialog.getOpenFileName(
            self, "导入配置", "", "JSON (*.json)"
        )
        if not path:
            return
        if not ask_yes_no(self, "确认导入", "导入配置将覆盖当前设置，是否继续？"):
            return
        with open(path, encoding="utf-8") as f:
            imported = json.load(f)
        self.settings.clear()
        self.settings.update(imported)
        save_settings(self.settings)
        self._sync_theme_combo()
        self._apply_sidebar_accent()
        self.settings_changed.emit()
        self.fences_rebuild_requested.emit()
        self.pages_changed.emit()
        self._reload_whitelist_list()
        self.refresh_data()
        show_info(self, "导入完成", "配置已导入，部分选项可能需要重启生效。")

    def _screenshot_snip_active(self) -> bool:
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        return bool(desk and getattr(desk, "_screenshot_session_active", False))

    def showEvent(self, event) -> None:
        super().showEvent(event)
        if self._screenshot_snip_active():
            return
        item = self._nav.currentItem()
        if item is not None:
            page_id = str(item.data(Qt.ItemDataRole.UserRole) or "")
            if page_id in self._page_index:
                self._ensure_page(page_id)
                self._stack.setCurrentIndex(self._page_index[page_id])
                self._current_page_id = page_id
                # hideEvent releases lazy pages; rebuild leaves OrganizeStatTiles at 0.
                # Initial nav currentRowChanged also fires before the slot is connected,
                # so opening on「整理」must refresh here (not only on re-click).
                if page_id == "files":
                    self.refresh_data()
                elif page_id == "vault":
                    self._ensure_page("vault")
                    if self._vault_settings_widget is not None:
                        self._vault_settings_widget.reload()
        self.settings_ui_shown.emit()

    def hideEvent(self, event) -> None:
        if self._screenshot_snip_active():
            super().hideEvent(event)
            return
        self.release_lazy_pages()
        super().hideEvent(event)
        self.settings_ui_hidden.emit()

    def closeEvent(self, event) -> None:
        event.ignore()
        # First close: ask whether to keep running in tray, then remember.
        if not self.settings.get("close_behavior_prompted", False):
            keep_background = ask_yes_no(
                self,
                "后台运行",
                "关闭 DeskTidy 窗口后是否在系统托盘后台继续运行？\n\n"
                "选择「是」：隐藏到托盘，桌面分区继续工作，此选择将被记住。\n"
                "选择「否」：按退出流程处理（分区整理会保留）。",
            )
            self.settings["close_behavior_prompted"] = True
            self.settings["close_to_tray"] = keep_background
            save_settings(self.settings, immediate=True)
            self._sync_close_to_tray_checkbox()
            if keep_background:
                self.hide()
                self.minimized_to_tray.emit()
            else:
                self.quit_requested.emit()
            return

        if self.settings.get("close_to_tray", True):
            self.hide()
            self.minimized_to_tray.emit()
            return

        # Do not accept yet — quit() shows confirm; cancel must keep the window.
        self.quit_requested.emit()
