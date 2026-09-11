"""System tray integration."""

from __future__ import annotations

from typing import Callable

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QAction
from PyQt6.QtWidgets import QApplication, QMenu, QSystemTrayIcon

from src.hotkey_manager import HOTKEY_LABELS
from src.i18n import APP_NAME_ZH, show_about
from src.icon_utils import get_tray_icon
from src.ui.main_window import MainWindow
from src.ui.styles import get_theme_palette, normalize_theme
from src.win_shell import bring_widget_to_foreground


def _tray_menu_stylesheet(theme: str | None = None) -> str:
    p = get_theme_palette(normalize_theme(theme))
    return f"""
    QMenu {{
        background-color: {p['card']};
        color: {p['text']};
        border: 1px solid {p['border']};
        border-radius: 10px;
        padding: 6px;
    }}
    QMenu::item {{
        background: transparent;
        color: {p['text']};
        padding: 7px 22px 7px 14px;
        border-radius: 6px;
    }}
    QMenu::item:selected {{
        background-color: {p['accent_soft']};
        color: {p['text']};
    }}
    QMenu::item:disabled {{
        color: {p['text_muted']};
    }}
    QMenu::separator {{
        height: 1px;
        background: {p['border']};
        margin: 5px 8px;
    }}
    QMenu::right-arrow {{
        width: 10px;
        height: 10px;
    }}
    """


class TrayManager:
    def __init__(
        self,
        window: MainWindow,
        app: QApplication,
        on_quit: Callable[[], None],
        on_organize: Callable[[], None] | None = None,
        on_toggle_fences: Callable[[], None] | None = None,
        on_peek: Callable[[], None] | None = None,
        on_screenshot: Callable[[], None] | None = None,
        on_screen_record: Callable[[], None] | None = None,
    ):
        self.window = window
        self.app = app
        self.on_quit = on_quit

        theme = None
        settings = getattr(window, "settings", None)
        if isinstance(settings, dict):
            theme = settings.get("theme")
        accent = get_theme_palette(normalize_theme(theme))["accent"]

        self.tray = QSystemTrayIcon(app)
        self.tray.setToolTip(APP_NAME_ZH)
        icon = get_tray_icon(accent)
        if not icon.isNull():
            self.tray.setIcon(icon)

        menu = QMenu()
        menu.setStyleSheet(_tray_menu_stylesheet(theme))
        self._menu = menu

        show_action = QAction("显示主窗口", menu)
        show_action.triggered.connect(self.show_window)
        menu.addAction(show_action)

        # QueuedConnection: run after the tray menu fully closes. Direct slots
        # during menu teardown made「一键整理」feel dead (no toast / stuck flag).
        organize_action = QAction(HOTKEY_LABELS["organize"], menu)
        organize_cb = on_organize or window._on_organize
        organize_action.triggered.connect(
            lambda *_a: organize_cb(),
            Qt.ConnectionType.QueuedConnection,
        )
        menu.addAction(organize_action)

        toggle_fences_action = QAction(HOTKEY_LABELS["toggle_fences"], menu)
        if on_toggle_fences:
            toggle_fences_action.triggered.connect(
                lambda *_a: on_toggle_fences(),
                Qt.ConnectionType.QueuedConnection,
            )
        menu.addAction(toggle_fences_action)

        peek_action = QAction(HOTKEY_LABELS["peek_fences"], menu)
        if on_peek:
            peek_action.triggered.connect(
                lambda *_a: on_peek(),
                Qt.ConnectionType.QueuedConnection,
            )
        menu.addAction(peek_action)

        self._screenshot_action = QAction(HOTKEY_LABELS["screenshot"], menu)
        if on_screenshot:
            self._screenshot_action.triggered.connect(
                lambda *_a: on_screenshot(),
                Qt.ConnectionType.QueuedConnection,
            )
        menu.addAction(self._screenshot_action)

        self._record_action = QAction(HOTKEY_LABELS["screen_record"], menu)
        if on_screen_record:
            self._record_action.triggered.connect(
                lambda *_a: on_screen_record(),
                Qt.ConnectionType.QueuedConnection,
            )
        menu.addAction(self._record_action)

        menu.addSeparator()

        # Help submenu sits with the “background utility” actions (below tools).
        help_menu = menu.addMenu("帮助")
        help_menu.setStyleSheet(_tray_menu_stylesheet(theme))
        help_action = QAction("使用说明", help_menu)
        help_action.triggered.connect(self._show_help)
        help_menu.addAction(help_action)
        help_menu.addSeparator()
        about_action = QAction("关于", help_menu)
        about_action.triggered.connect(lambda: show_about(self.window))
        help_menu.addAction(about_action)

        quit_action = QAction("退出", menu)
        quit_action.triggered.connect(on_quit)
        menu.addAction(quit_action)

        self.tray.setContextMenu(menu)
        self.tray.activated.connect(self._on_activated)
        if QSystemTrayIcon.isSystemTrayAvailable():
            self.tray.show()

    def apply_feature_visibility(self, settings: dict | None) -> None:
        """Show/hide tray actions for optional chrome (hotkeys stay registered)."""
        settings = settings if isinstance(settings, dict) else {}
        rec = settings.get("screen_record", {})
        rec_on = bool(rec.get("enabled", False)) if isinstance(rec, dict) else False
        self._screenshot_action.setVisible(True)
        self._record_action.setVisible(rec_on)
        theme = settings.get("theme")
        accent = get_theme_palette(normalize_theme(theme))["accent"]
        icon = get_tray_icon(accent)
        if not icon.isNull():
            try:
                self.tray.setIcon(icon)
            except RuntimeError:
                pass
        if self._menu is not None:
            self._menu.setStyleSheet(_tray_menu_stylesheet(theme))

    def _on_activated(self, reason: QSystemTrayIcon.ActivationReason) -> None:
        if reason in (
            QSystemTrayIcon.ActivationReason.Trigger,
            QSystemTrayIcon.ActivationReason.DoubleClick,
        ):
            self.show_window()

    def show_window(self) -> None:
        try:
            was_mapped = bool(self.window.isVisible()) and not bool(
                self.window.isMinimized()
            )
        except RuntimeError:
            was_mapped = False
        bring_widget_to_foreground(self.window, was_mapped=was_mapped)
        # Hidden→shown: MainWindow.showEvent emits settings_ui_shown once.
        # Already mapped: showEvent does not re-fire — sink overlays here.
        if was_mapped:
            qt_app = QApplication.instance()
            desk = getattr(qt_app, "_desktidy_app", None) if qt_app else None
            if desk is not None and hasattr(desk, "_on_settings_shown"):
                desk._on_settings_shown()

    def _show_help(self) -> None:
        from src.product_pages import open_desktidy_help

        settings = getattr(self.window, "settings", None)
        if open_desktidy_help(settings if isinstance(settings, dict) else None):
            return
        from src.ui.help_dialog import show_help

        show_help(self.window, settings)

    def show_message(self, title: str, message: str, *, msec: int = 4500) -> None:
        self.tray.showMessage(
            title,
            message,
            QSystemTrayIcon.MessageIcon.Information,
            max(1500, int(msec)),
        )
