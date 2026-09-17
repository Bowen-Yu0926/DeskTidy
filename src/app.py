"""Application entry point and fence management."""

from __future__ import annotations

import sys
import time
import uuid
from contextlib import contextmanager
from pathlib import Path

from PyQt6.QtCore import QEventLoop, QObject, QRect, QTimer, Qt, pyqtSignal
from PyQt6.QtGui import QCursor, QGuiApplication, QKeySequence, QShortcut
from PyQt6.QtWidgets import QApplication, QInputDialog, QWidget

from src.i18n import APP_NAME_ZH, ask_yes_no, setup_chinese
from src.icon_utils import get_app_icon
from src.app_logging import get_logger
from src.desktop_context import (
    DesktopContextMonitor,
    get_last_desktop_right_click,
)
from src.shell_background_verbs import sync_desktop_background_verbs
from src.shell_ipc import start_shell_ipc_host, stop_shell_ipc_host
from src.desktop_foreground_monitor import DesktopForegroundMonitor
from src.desktop_monitor import DesktopBlankClickMonitor, DesktopDoubleClickMonitor
from src.fence_region_manager import FenceRegionManager
from src.fence_layout import (
    apply_geometries_to_fences,
    current_display_fingerprint,
    prepare_destination_profile_for_display_change,
    get_fence_geometry,
    migrate_layouts,
    place_new_fence_rect,
    purge_transient_display_profiles,
    save_fence_geometry,
    save_visible_fences,
    set_live_fences_provider,
)
from src.fence_pages import fence_on_page, get_fence_pages, set_fence_pages
from src.fence_rules import (
    all_fence_pinned_keys,
    get_portal_path,
    is_portal_fence,
    migrate_fence_rules,
    migrate_fence_storage,
    migrate_simplify_fences,
)
from src.desktop_scanner import invalidate_desktop_scan_cache
from src.file_watcher import DesktopWatcher
from src.hotkey_manager import HOTKEY_FALLBACKS, HOTKEY_LABELS, HotkeyManager
from src.organizer import organize_desktop, restore_desktop_from_storage
from src.peek_manager import PeekManager
from src.screenshot_manager import ScreenshotManager
from src.screen_record_manager import ScreenRecordManager
from src.settings import (
    get_desktop_path,
    load_settings,
    remove_desktop_guard_autostart,
    save_settings,
)
from src.tray import TrayManager
from src.ui.dock_widget import DockWidget
from src.ui.fence_widget import FenceWidget
from src.ui.main_window import MainWindow
from src.ui.page_indicator import PageIndicatorWidget
from src.ui.public_icon_host import PublicIconHost
from src.ui.public_icon_widget import PublicIconWidget
from src.ui.styles import build_stylesheet, fence_theme_defaults, normalize_theme
from src.public_desktop import (
    add_public_item,
    find_public_entry,
    clear_ephemeral_public_items,
    live_fence_rects,
    prune_missing_public_items,
    relayout_public_items,
    remove_public_paths,
    snap_public_item,
    visible_floating_items,
)
from src.win_shell import (
    are_desktop_icons_visible,
    apply_overlay_stack_mode,
    bring_widget_to_foreground,
    create_namespace_shortcut,
    is_explorer_desktop_foreground,
    overlay_win32_visible,
    restore_all_hosted_namespace_icons,
    reveal_hosted_namespace_icons,
    set_desktop_icons_visible,
    ensure_desktop_icons_visible,
)


class _UiBridge(QObject):
    """Marshal background-thread callbacks onto the Qt GUI thread."""

    refresh_fences = pyqtSignal()
    file_created = pyqtSignal(str)
    file_removed = pyqtSignal(str)
    file_moved = pyqtSignal(str, str)
    portal_changed = pyqtSignal(str)


class DeskTidyApp:
    def __init__(self):
        logger = get_logger()
        started = time.perf_counter()
        self._startup_mark_t0 = started

        def mark(stage: str) -> None:
            logger.info("startup stage=%s elapsed_ms=%d", stage, int((time.perf_counter() - started) * 1000))

        self._startup_mark = mark

        self.settings = load_settings()
        mark("settings_loaded")
        self.settings["theme"] = normalize_theme(self.settings.get("theme"))
        self._migrate_fence_pages()
        migrate_simplify_fences(self.settings)
        migrate_fence_rules(self.settings)
        migrate_fence_storage(self.settings)
        from src.system_defaults import ensure_system_defaults
        from src.fence_rules import migrate_page_local_floats_to_organize_fences

        if ensure_system_defaults(self.settings):
            save_settings(self.settings)
        # Opt-in live pin (Fences Rules-like). Default stays off; do not force-clear
        # a user-enabled preference across restarts.
        if "auto_organize_watch" not in self.settings:
            self.settings["auto_organize_watch"] = False
        # Page-rule organizes used to leave docs as free floats on empty pages —
        # migrate them into a real fence so they are findable as partitions.
        # Skip when live auto-pin is off — otherwise every launch re-vacuums
        # page floats the user left unpinned.
        if bool(self.settings.get("auto_organize_watch", False)):
            if migrate_page_local_floats_to_organize_fences(self.settings):
                save_settings(self.settings)
        # Drop legacy empty-page mirrors (other pages' pins must stay page-local).
        if clear_ephemeral_public_items(self.settings):
            save_settings(self.settings)
        self.settings["organize_mode"] = "virtual"
        if "hosted_namespace_icons" not in self.settings:
            self.settings["hosted_namespace_icons"] = {}
        if "close_behavior_prompted" not in self.settings:
            self.settings["close_behavior_prompted"] = False
        if "session_active" not in self.settings:
            self.settings["session_active"] = False
        if "legacy_storage_migrated" not in self.settings:
            self.settings["legacy_storage_migrated"] = False
        if "public_desktop_items" not in self.settings or not isinstance(
            self.settings.get("public_desktop_items"), list
        ):
            self.settings["public_desktop_items"] = []
        if "enable_public_desktop" not in self.settings:
            self.settings["enable_public_desktop"] = False
        if "organize_whitelist" not in self.settings or not isinstance(
            self.settings.get("organize_whitelist"), list
        ):
            self.settings["organize_whitelist"] = []
        # Heal: floats must match public-area toggle (shared ↔ page-local).
        from src.public_desktop import heal_public_area_scope

        page_id = int(self.settings.get("current_page", 0))
        if heal_public_area_scope(self.settings, page_id):
                try:
                    save_settings(self.settings)
                except OSError:
                    pass
        mark("settings_migrated")
        self.fences: list[FenceWidget] = []
        self.public_icons: list[PublicIconWidget] = []
        # One shell HWND for all public floats (children share the host).
        self._public_icon_host: PublicIconHost | None = None
        # Off-page overlays kept alive so page switches do not rebuild icons.
        self._parked_fences: dict[str, FenceWidget] = {}
        set_live_fences_provider(self._iter_live_fences_for_placement)
        self._parked_public_icons: dict[str, PublicIconWidget] = {}
        self._refreshing_fences = False
        self._refresh_public_from_fences_pending = False
        self._refresh_fences_force = False
        self._refreshing_public = False
        self._public_force_relayout = False
        self._public_relayout_pending = False
        self._last_saved_fence_layout_sig: tuple | None = None
        self._last_public_fence_key: tuple | None = None
        self._last_public_sync_sig: tuple | None = None
        self._loose_sync_needed = True
        self._loose_sync_inflight = False
        self._organize_inflight = False
        self._portal_dirty_dirs: set[str] = set()
        self._pending_watch_paths: list[str] = []
        self._max_parked_fences = 12
        self._max_parked_public_icons = 48
        self._overlay_keepalive_stable = 0
        self._overlay_last_needed_restore = False
        self._last_desktop_fg_recover = 0.0
        self._last_bg_trim_time = 0.0
        # Tracks FG so Win+D / Alt+Tab-back can force re-attach without thrashing
        # while the mouse moves over an already-shown desktop.
        self._last_fg_was_desktop = False
        self._overlays_parked_for_app_fg = False
        self._page_switch_ensure_pending = False
        self._page_switch_batch_fence_ids: set[str] = set()
        self._page_switch_force_refresh_fence_ids: set[str] = set()
        self._page_switch_freeze_depth = 0
        self._page_switch_quiet_until = 0.0
        self._suppress_fence_layout_save = False
        self._overlays_shell_attached = False
        self.dock: DockWidget | None = None
        self.page_indicator: PageIndicatorWidget | None = None
        self.todo_panel = None
        self.vault_panel = None
        self.vault_launcher = None
        self.pet_widget = None
        self._notepad_window = None
        self._icons_hidden = False
        self._fences_hidden = False
        self._displayed_page: int | None = None
        self._desktop_restored = False
        self._exiting = False
        self._overlay_mgmt_suspended = False
        self._overlay_drag_active = False
        self._public_drag_path: str | None = None
        self._public_drag_keys: set[str] = set()
        # After icon drag, skip aggressive shell re-attach (Explorer can deadlock).
        self._shell_attach_quiet_until = 0.0
        # When force attach is throttled, queue one retry instead of dropping it.
        self._shell_attach_force_pending = False
        self._desktop_popup_open = False
        self._desktop_popup_depth = 0
        self._screenshot_session_active = False
        self._screenshot_session_depth = 0
        self._pending_shell_verb: str | None = None
        self._display_fingerprint = ""

        self.qt_app = QApplication(sys.argv)
        self.qt_app.setQuitOnLastWindowClosed(False)
        try:
            from src.qt_main_thread import ensure_main_thread_bridge

            ensure_main_thread_bridge(self.qt_app)
        except Exception:
            pass
        mark("qt_app_created")
        # Live settings bridge so shell helpers can keep hosted icons in sync.
        self.qt_app._desktidy_settings = self.settings
        self.qt_app._desktidy_app = self
        migrate_layouts(self.settings)
        self._display_fingerprint = current_display_fingerprint()
        self._display_layout_timer = QTimer()
        self._display_layout_timer.setSingleShot(True)
        self._display_layout_timer.timeout.connect(self._on_display_layout_changed)
        self._connect_display_signals()
        mark("display_signals_connected")

        # Resume previous session: keep fence storage as-is (do not dump files
        # back to the desktop). Only re-hide hosted system icons still in fences.
        # Defer disk write to the event loop — boot autostart must not block on I/O.
        self.settings["session_active"] = True
        QTimer.singleShot(0, self._persist_session_active_flag)
        mark("desktop_guard_ready")
        self.qt_app.aboutToQuit.connect(self._on_about_to_quit)
        self._ui_bridge = _UiBridge()
        self._ui_bridge.refresh_fences.connect(
            self.refresh_fences, Qt.ConnectionType.QueuedConnection
        )
        self._ui_bridge.file_created.connect(
            self._on_watched_file, Qt.ConnectionType.QueuedConnection
        )
        self._ui_bridge.file_removed.connect(
            self._on_watched_file_removed, Qt.ConnectionType.QueuedConnection
        )
        self._ui_bridge.file_moved.connect(
            self._on_watched_file_moved, Qt.ConnectionType.QueuedConnection
        )
        self._ui_bridge.portal_changed.connect(
            self._on_portal_folder_changed, Qt.ConnectionType.QueuedConnection
        )
        self._watch_debounce = QTimer()
        self._watch_debounce.setSingleShot(True)
        self._watch_debounce.timeout.connect(self._run_watched_organize)
        self._sticky_reprune_timer = QTimer()
        self._sticky_reprune_timer.setSingleShot(True)
        self._sticky_reprune_timer.timeout.connect(self._run_sticky_missing_reprune)
        self._portal_refresh_timer = QTimer()
        self._portal_refresh_timer.setSingleShot(True)
        self._portal_refresh_timer.timeout.connect(self._refresh_portal_fences)
        self._refresh_fences_timer = QTimer()
        self._refresh_fences_timer.setSingleShot(True)
        self._refresh_fences_timer.timeout.connect(self._refresh_fences_impl)
        self._save_fence_layout_timer = QTimer()
        self._save_fence_layout_timer.setSingleShot(True)
        self._save_fence_layout_timer.timeout.connect(self._flush_fence_layout_save)
        self._refresh_public_timer = QTimer()
        self._refresh_public_timer.setSingleShot(True)
        self._refresh_public_timer.timeout.connect(self._run_public_refresh)
        self._desktop_fg_recover_timer = QTimer()
        self._desktop_fg_recover_timer.setSingleShot(True)
        self._desktop_fg_recover_timer.timeout.connect(
            self._recover_from_desktop_foreground
        )
        mark("timers_ready")
        app_icon = get_app_icon()
        if not app_icon.isNull():
            self.qt_app.setWindowIcon(app_icon)
        setup_chinese(self.qt_app)
        self._apply_theme()
        self.qt_app.applicationStateChanged.connect(self._on_application_state_changed)
        mark("theme_ready")

        self._fence_region = FenceRegionManager()
        self._fence_region.region_confirmed.connect(self._create_fence_in_rect)
        mark("fence_region_ready")

        self.window = MainWindow(self.settings)
        mark("main_window_created")
        self.window.fences_toggle_requested.connect(self.toggle_fences)
        self.window.fences_refresh_requested.connect(self.refresh_fences)
        self.window.organize_requested.connect(
            lambda pages=None: self._on_organize_requested(float_page_ids=pages)
        )
        self.window.settings_changed.connect(self._on_settings_changed)
        self.window.theme_changed.connect(self._on_theme_changed)
        self.window.hotkeys_changed.connect(self._schedule_hotkeys_refresh)
        self.window.hotkey_capture_began.connect(self._on_hotkey_capture_began)
        self.window.hotkey_capture_ended.connect(self._on_hotkey_capture_ended)
        self.window.fences_rebuild_requested.connect(self.rebuild_fences)
        self.window.fence_visibility_changed.connect(self._apply_fence_visibility)
        self.window.fence_style_preview_requested.connect(self._preview_fence_style)
        self.window.fence_style_apply_requested.connect(self._apply_fence_style_preset)
        self.window.fence_style_revert_requested.connect(self._revert_fence_style_preview)
        self.window.fence_style_apply_one_requested.connect(
            self._apply_fence_style_preset_one
        )
        self.window.fence_opacity_changed.connect(self._apply_fence_opacity_one)
        self.window.pages_changed.connect(self._on_pages_changed)
        self.window.extensions_changed.connect(self._on_extensions_changed)
        self.window.pet_settings_changed.connect(self._on_pet_settings_changed)
        self.window.set_snapshot_restored_handler(self._on_snapshot_restored)
        self.window.hide_desktop_icons_keep_fences.connect(
            self._hide_desktop_icons_keep_fences
        )
        self.window.minimized_to_tray.connect(self._on_minimized_to_tray)
        self.window.settings_ui_shown.connect(self._on_settings_shown)
        self.window.settings_ui_hidden.connect(self._on_settings_hidden)
        self.window.quit_requested.connect(self.quit)

        self.peek_manager = PeekManager(lambda: self.fences)
        self._screenshot_manager: ScreenshotManager | None = None
        self._screen_record_manager: ScreenRecordManager | None = None

        self.tray = TrayManager(
            self.window,
            self.qt_app,
            self.quit,
            on_organize=self._do_organize,
            on_toggle_fences=self._toggle_fences_visibility,
            on_peek=self._toggle_peek,
            on_screenshot=lambda: self.screenshot_manager.start_capture(),
            on_screen_record=lambda: self.screen_record_manager.toggle_recording(),
        )
        self.tray.apply_feature_visibility(self.settings)
        mark("tray_ready")

        self._file_search_overlay = None
        try:
            from src.fd_locator import warm_fd_cache

            QTimer.singleShot(8000, warm_fd_cache)
        except Exception:
            pass

        # Ensure default Notes / Recordings folders exist only when features are on.
        try:
            from src.notepad import default_notes_folder, notepad_enabled
            from src.screen_record_manager import default_output_dir

            if notepad_enabled(self.settings):
                QTimer.singleShot(3000, lambda: default_notes_folder(ensure=True))
            if bool(self.settings.get("screen_record", {}).get("enabled", False)):
                QTimer.singleShot(3200, lambda: default_output_dir(ensure=True))
        except Exception:
            pass

        self.hotkey_manager = HotkeyManager(self.qt_app)
        self._app_shortcuts: list[QShortcut] = []
        self._hotkey_capture_depth = 0
        self._hotkey_refresh_pending = False
        self._hotkey_refresh_timer: QTimer | None = None
        mark("hotkey_manager_ready")

        # Create monitors early, but start the heavier ones after first paint.
        self.desktop_monitor = DesktopDoubleClickMonitor(self._on_desktop_double_click)
        self._desktop_blank_click_monitor = DesktopBlankClickMonitor(
            self._on_desktop_blank_click
        )
        self._foreground_monitor = DesktopForegroundMonitor()
        self._foreground_monitor.foreground_changed.connect(
            self._on_desktop_foreground_changed,
            Qt.ConnectionType.QueuedConnection,
        )
        self._explorer_menu_freeze_armed = False
        self._explorer_menu_freeze_gen = 0
        self._explorer_menu_defview_host = 0
        self._explorer_menu_recover_gen = 0
        self._explorer_menu_recover_attempts = 0
        self._explorer_shell_wait_gen = 0
        self._explorer_shell_wait_attempts = 0
        self._startup_overlays_done = False
        self._overlay_drag_active = False
        self.desktop_context = DesktopContextMonitor(
            fence_region_manager=self._fence_region,
            is_marquee_enabled=lambda: self.settings.get("desktop_right_click_menu", True),
            on_freeze=self._arm_explorer_menu_freeze,
        )
        self.watcher = DesktopWatcher()
        mark("monitors_created")

        # Tray is up; wait for Explorer DefView on cold boot before shell attach.
        self._schedule_startup_overlays()
        mark("startup_critical_complete")

        # Non-critical work after the event loop starts — keeps first paint responsive.
        self._schedule_startup_deferred()
        mark("startup_complete")

    @property
    def screenshot_manager(self) -> ScreenshotManager:
        if self._screenshot_manager is None:
            self._screenshot_manager = ScreenshotManager(lambda: self.settings)
        return self._screenshot_manager

    @property
    def screen_record_manager(self) -> ScreenRecordManager:
        if self._screen_record_manager is None:
            self._screen_record_manager = ScreenRecordManager(
                lambda: self.settings,
                notify=self._notify_user,
            )
        return self._screen_record_manager

    def _persist_session_active_flag(self) -> None:
        if self._exiting:
            return
        try:
            save_settings(self.settings, immediate=True)
        except OSError:
            pass

    def _schedule_startup_session_retries(self) -> None:
        for delay in (1200, 3500, 8000):
            QTimer.singleShot(
                delay, lambda: self._apply_session_desktop_view(light=True)
            )

    def _schedule_startup_overlays(self) -> None:
        """Show fences/floats once Explorer desktop shell exists (boot autostart).

        Always return to the event loop first — building overlays inside
        ``__init__`` hitchs login (HWND + shell icons) before the tray exists.
        """
        try:
            from src.desktop_shell_host import explorer_shell_ready

            if explorer_shell_ready():
                QTimer.singleShot(0, self._run_startup_overlays)
                return
        except Exception:
            pass
        self._explorer_shell_wait_gen = int(getattr(self, "_explorer_shell_wait_gen", 0)) + 1
        gen = self._explorer_shell_wait_gen
        self._explorer_shell_wait_attempts = 0
        get_logger().info("startup: waiting for explorer shell (boot)")
        QTimer.singleShot(0, lambda g=gen: self._poll_explorer_shell_for_startup(g))

    def _poll_explorer_shell_for_startup(self, gen: int) -> None:
        if gen != int(getattr(self, "_explorer_shell_wait_gen", 0)):
            return
        if self._exiting:
            return
        try:
            from src.desktop_shell_host import explorer_shell_ready

            ready = bool(explorer_shell_ready())
        except Exception:
            ready = False
        if ready:
            attempts = int(getattr(self, "_explorer_shell_wait_attempts", 0))
            if attempts:
                get_logger().info(
                    "startup: explorer shell ready after %d polls", attempts
                )
            self._run_startup_overlays()
            return
        attempts = int(getattr(self, "_explorer_shell_wait_attempts", 0)) + 1
        self._explorer_shell_wait_attempts = attempts
        if attempts >= 30:
            get_logger().warning(
                "startup: explorer shell wait timed out — showing overlays anyway"
            )
            self._run_startup_overlays()
            return
        QTimer.singleShot(100, lambda g=gen: self._poll_explorer_shell_for_startup(g))

    def _run_startup_overlays(self) -> None:
        """Critical overlay restore once the desktop shell is reachable."""
        if getattr(self, "_startup_overlays_done", False):
            return
        self._startup_overlays_done = True
        mark = getattr(self, "_startup_mark", None)
        if callable(mark):
            mark("overlays_begin")
        # Hide Explorer icons first; create overlays once below (not twice).
        self._apply_session_desktop_view(show_overlays=False)
        if callable(mark):
            mark("session_desktop_view_applied")
        self._schedule_startup_session_retries()
        if self.settings.get("show_fences", True):
            self.show_fences()
        if callable(mark):
            mark("fences_shown")
        self.refresh_public_desktop(relayout=True, immediate=True)
        QTimer.singleShot(0, self._startup_force_overlay_attach)
        if self.settings.get("show_page_indicator") is not True:
            self.settings["show_page_indicator"] = True
            try:
                save_settings(self.settings)
            except OSError:
                pass
        self._setup_page_indicator()
        self._setup_dock()
        self._setup_todo_panel()
        self._setup_account_vault_panel()
        self._setup_desktop_pet()
        self._start_overlay_keepalive_timer()
        try:
            from src.ui.fence_icon_item import ensure_desktop_icon_key_hook

            ensure_desktop_icon_key_hook()
        except Exception:
            pass
        if callable(mark):
            mark("overlays_ready")
        # Register capture hotkeys *before* off-page HWND warmup. That loop can
        # take a long time after reboot (cold icon extract); F1/F3 must already
        # be live, and the loop must yield so those keys are dispatched.
        try:
            self._setup_hotkeys()
        except Exception:
            get_logger().exception("startup: hotkey registration before overlay warmup failed")
        # Construct capture managers now so FFmpeg/GDI warm threads start during
        # fence HWND priming instead of on the first user click after reboot.
        try:
            _ = self.screenshot_manager
        except Exception:
            pass
        try:
            if bool(self.settings.get("screen_record", {}).get("enabled", False)):
                mgr = self.screen_record_manager
                warm = getattr(mgr, "_warm_ffmpeg", None)
                if callable(warm):
                    warm()
        except Exception:
            pass
        try:
            from src.desktop_capture import warm_desktop_capture

            warm_desktop_capture()
        except Exception:
            pass
        if len(self._get_pages()) > 1 and self.settings.get("show_fences", True):
            # Prime off-page HWNDs in the background so the current page paints first.
            self._schedule_startup_offpage_warmup()

    def _schedule_startup_deferred(self) -> None:
        """Stagger background startup work so the first paint stays light."""
        QTimer.singleShot(0, self._startup_deferred_immediate)
        QTimer.singleShot(400, self._startup_deferred_monitors)
        QTimer.singleShot(700, self._startup_deferred_guard)
        QTimer.singleShot(800, self._startup_deferred_shell_menu_warm)
        QTimer.singleShot(1000, self._startup_deferred_window_data)
        QTimer.singleShot(2000, self._startup_deferred_legacy_storage_migrate)
        if not self.settings.get("first_run_organize_done", True):
            QTimer.singleShot(2500, self._startup_deferred_first_run)
        elif self.settings.get("auto_organize_on_startup"):
            QTimer.singleShot(2500, self._startup_deferred_organize)
        elif not self.settings.get("first_run_guide_done", True):
            QTimer.singleShot(1600, self._maybe_show_first_run_guide)

    def _startup_deferred_immediate(self) -> None:
        if self._exiting:
            return
        try:
            self._setup_hotkeys()
            mark = getattr(self, "_startup_mark", None)
            if callable(mark):
                mark("hotkeys_ready")
        except Exception:
            get_logger().exception("startup: hotkey registration failed")
        # Foreground recovery should come online quickly after first paint.
        try:
            self._foreground_monitor.start()
        except Exception:
            pass
        QTimer.singleShot(0, self._apply_foreground_overlay_visibility)

    def _startup_deferred_shell_menu_warm(self) -> None:
        """Pre-create shell menu host so the first icon RMB is not paying CreateWindow."""
        if self._exiting:
            return
        try:
            from src.shell_file_menu import warm_shell_context_menu_host

            warm_shell_context_menu_host()
        except Exception:
            pass

    def _pending_offpage_fence_cfgs(self) -> list[dict]:
        """Off-page fences that still need HWND priming."""
        current = self._current_page()
        live = {
            str(f.config.get("id") or "")
            for f in list(getattr(self, "fences", None) or [])
            if getattr(f, "config", None)
        }
        pending: list[dict] = []
        for fence_cfg in self.settings.get("fences") or []:
            if not isinstance(fence_cfg, dict) or not fence_cfg.get("visible", True):
                continue
            fid = str(fence_cfg.get("id") or "")
            if not fid or fid in live or fid in self._parked_fences:
                continue
            if fence_on_page(fence_cfg, current):
                continue
            pending.append(fence_cfg)
        return pending

    def _startup_force_overlay_attach(self) -> None:
        """Boot-only: attach overlays immediately instead of waiting for keepalive.

        Force attach walks ``_iter_overlay_widgets`` with the public host last, so
        a healthy remount can leave the plate above fences. Pet setup also runs
        after the initial ``refresh_public_desktop`` ensure_live. Always re-assert
        fence click/drag ownership here — otherwise the first fence→public drag
        after boot never reaches ``start_virtual_item_drag`` (no log lines).
        """
        if self._exiting or self._icons_hidden:
            return
        try:
            self._ensure_shell_attachments(force=True)
        except Exception:
            get_logger().exception("startup: force shell attach failed")
        try:
            self._ensure_desktop_overlays_visible(force=True)
        except Exception:
            pass
        try:
            self.ensure_live_fences_interactive()
        except Exception:
            pass
        # Restack first (Z-order), then one paint flush — never restack after paint.
        self._heal_boot_fence_surfaces()

    def _heal_boot_fence_surfaces(self) -> None:
        """Paint fence panels/icons after shell restack — do not restack again.

        Caller must already have run live-fence band raise. Raising again after
        paint (especially with discarded client bits) left solid black plates.
        This path only re-applies style, force-refreshes pins, and flushes
        layered paint.
        """
        if self._exiting or self._icons_hidden:
            return
        if not self._fences_should_show():
            return
        from src.fence_rules import is_portal_fence

        rebuilt = 0
        for fence in list(getattr(self, "fences", None) or ()):
            try:
                if bool(getattr(fence, "_desktidy_soft_parked", False)):
                    continue
                apply = getattr(fence, "_apply_style", None)
                if callable(apply):
                    apply()
                    fence.setWindowOpacity(fence._target_opacity())
                cfg = fence.config if isinstance(fence.config, dict) else {}
                needs_icons = bool(cfg.get("virtual_items")) or is_portal_fence(cfg)
                if needs_icons:
                    # Widgets may exist while shell glyphs never painted into the
                    # layered HWND — one boot refresh only (no post-refresh shell
                    # heal: that re-entered force-attach and flickered icons).
                    fence.refresh(force=True, shell_heal=False)
                    rebuilt += 1
                    flush_icons = getattr(fence, "flush_icon_grid_paint", None)
                    if callable(flush_icons):
                        flush_icons()
                flush = getattr(fence, "_flush_style_paint", None)
                if callable(flush):
                    flush()
            except RuntimeError:
                continue
        get_logger().info(
            "boot heal: fence surfaces refreshed count=%s rebuilt=%s",
            len(list(getattr(self, "fences", None) or ())),
            rebuilt,
        )

    def _schedule_startup_offpage_warmup(self) -> None:
        """Stagger off-page fence priming so boot paint is not blocked."""
        if self._exiting or self._icons_hidden:
            return
        if not self._fences_should_show():
            return
        pending = self._pending_offpage_fence_cfgs()
        if not pending:
            return
        self._offpage_warmup_pending = list(pending)
        self._max_parked_fences = max(int(self._max_parked_fences), len(pending))
        get_logger().debug("warmup: scheduling %d off-page fence(s)", len(pending))
        QTimer.singleShot(50, self._startup_offpage_warmup_tick)

    def _startup_offpage_warmup_tick(self) -> None:
        if self._exiting or self._icons_hidden:
            self._offpage_warmup_pending = []
            return
        if getattr(self, "_page_switch_ensure_pending", False):
            QTimer.singleShot(80, self._startup_offpage_warmup_tick)
            return
        if int(getattr(self, "_page_switch_freeze_depth", 0) or 0) > 0:
            QTimer.singleShot(80, self._startup_offpage_warmup_tick)
            return
        pending = list(getattr(self, "_offpage_warmup_pending", None) or [])
        if not pending:
            return
        fence_cfg = pending.pop(0)
        self._offpage_warmup_pending = pending
        try:
            self._warmup_one_offpage_fence(fence_cfg, sync_icons=True)
        except Exception:
            get_logger().exception("warmup: off-page fence failed")
        if pending:
            QTimer.singleShot(40, self._startup_offpage_warmup_tick)
        else:
            get_logger().debug("warmup: parked off-page fences complete")

    def _warmup_all_offpage_fences(self) -> None:
        """Create every off-page fence now so the first page click is only unpark.

        Market pattern (Stardock Fences): keep fence windows alive and hidden.
        Translucent DefView overlays flash wallpaper on DWM's first ``ShowWindow``.
        Do that first-map during boot load — never when the user clicks a page.
        """
        if self._exiting or self._icons_hidden:
            return
        if not self._fences_should_show():
            return
        if getattr(self, "_page_switch_ensure_pending", False):
            return
        if int(getattr(self, "_page_switch_freeze_depth", 0) or 0) > 0:
            return
        pending = self._pending_offpage_fence_cfgs()
        if not pending:
            return
        self._max_parked_fences = max(int(self._max_parked_fences), len(pending))
        app = getattr(self, "qt_app", None)
        if app is None:
            try:
                from PyQt6.QtWidgets import QApplication

                app = QApplication.instance()
            except Exception:
                app = None
        for index, fence_cfg in enumerate(pending):
            self._warmup_one_offpage_fence(fence_cfg, sync_icons=True)
            # Yield so screenshot / record hotkeys and page clicks stay live
            # while remaining pages prime (cold boot icon extract can take minutes).
            if app is not None and index + 1 < len(pending):
                app.processEvents()
        get_logger().debug("warmup: parked %d off-page fence(s)", len(pending))

    def _warmup_fences_for_page(self, page_id: int) -> int:
        """Create+paint+prime any missing fences for *page_id* before the freeze.

        First click must not construct HWNDs inside the paint freeze — that
        path is DWM's first map and flashes the wallpaper.
        """
        if self._exiting or self._icons_hidden:
            return 0
        if not self._fences_should_show():
            return 0
        live = {
            str(f.config.get("id") or "")
            for f in list(getattr(self, "fences", None) or [])
            if getattr(f, "config", None)
        }
        parked = set(self._parked_fences)
        warmed = 0
        for fence_cfg in self.settings.get("fences") or []:
            if not isinstance(fence_cfg, dict) or not fence_cfg.get("visible", True):
                continue
            fid = str(fence_cfg.get("id") or "")
            if not fid or fid in live or fid in parked:
                continue
            if not fence_on_page(fence_cfg, int(page_id)):
                continue
            self._warmup_one_offpage_fence(fence_cfg, sync_icons=True)
            warmed += 1
        return warmed

    def _warmup_offpage_fences(self) -> None:
        """Boot warmup entry — create every remaining off-page fence."""
        self._warmup_all_offpage_fences()

    def _warmup_one_offpage_fence(
        self, fence_cfg: dict, *, sync_icons: bool = False
    ) -> None:
        fid = str(fence_cfg.get("id") or "")
        if not fid:
            return
        pages = get_fence_pages(fence_cfg)
        home = int(pages[0]) if pages else 0
        geom = get_fence_geometry(self.settings, fence_cfg, home)
        fence = FenceWidget(fence_cfg, self.settings)
        # Always paint the grid before DWM's first-map. An empty prime still
        # flashes wallpaper on the first on-screen SHOW after boot.
        fence._desktidy_defer_refresh = True  # type: ignore[attr-defined]
        try:
            try:
                fence.setGeometry(
                    int(geom["x"]),
                    int(geom["y"]),
                    int(geom["width"]),
                    int(geom["height"]),
                )
                fence.set_collapsed(
                    bool(geom.get("collapsed", False)),
                    emit=False,
                    layout=True,
                )
            except (RuntimeError, TypeError, ValueError, KeyError):
                pass
            self._wire_fence_signals(fence)
            from src.desktop_shell_host import (
                attach_overlay_to_desktop,
                prime_hidden_overlay,
                set_overlay_hwnd_visible,
            )

            hwnd = self._fence_hwnd(fence)
            if hwnd:
                try:
                    attach_overlay_to_desktop(hwnd, show=False)
                except Exception:
                    pass
            try:
                if sync_icons:
                    fence._desktidy_page_switch_refresh = True  # type: ignore[attr-defined]
                fence._refresh_impl()
            except RuntimeError:
                pass
            finally:
                fence._desktidy_page_switch_refresh = False  # type: ignore[attr-defined]
                fence._desktidy_defer_refresh = False  # type: ignore[attr-defined]
            if hwnd:
                try:
                    prime_hidden_overlay(
                        hwnd,
                        int(geom["x"]),
                        int(geom["y"]),
                        int(geom["width"]),
                        int(geom["height"]),
                        on_shown=fence.flush_icon_grid_paint,
                    )
                except Exception:
                    pass
                try:
                    set_overlay_hwnd_visible(hwnd, False)
                except Exception:
                    pass
            # Never Qt-show() here — the HWND is primed off-screen and parked.
            self._park_fence(fid, fence, soft=True)
        except Exception:
            get_logger().exception("warmup: off-page fence failed fid=%s", fid)
            if fid not in getattr(self, "_parked_fences", {}):
                try:
                    fence.close()
                    fence.deleteLater()
                except Exception:
                    pass

    def _startup_deferred_monitors(self) -> None:
        if self._exiting:
            return
        try:
            self._desktop_blank_click_monitor.start()
        except Exception:
            pass
        if self.settings.get("double_click_hide", True):
            try:
                self.desktop_monitor.start()
            except Exception:
                pass
        self._sync_desktop_shell_integration()
        try:
            from src.ui.fence_icon_item import destroy_unpin_catchers

            destroy_unpin_catchers()
        except Exception:
            pass
        # Always watch the desktop so Save As / new files get overlay floats.
        # Watcher never auto-pins; only 一键整理 applies organize rules.
        self._start_watcher()

    def _startup_deferred_guard(self) -> None:
        if self._exiting:
            return
        try:
            from src.settings import migrate_legacy_autostart

            migrate_legacy_autostart()
        except Exception:
            pass
        # Market-common: single Startup shortcut only. Drop legacy「桌面守护」.lnk.
        try:
            remove_desktop_guard_autostart()
        except Exception:
            pass
        # New installs default auto_start=true; create the shortcut if missing.
        try:
            from src.settings import is_auto_start_enabled, set_auto_start

            if bool(self.settings.get("auto_start", True)) and not is_auto_start_enabled():
                set_auto_start(True)
                if "settings" in getattr(self.window, "_lazy_built", set()):
                    cb = getattr(self.window, "auto_start_cb", None)
                    if cb is not None:
                        cb.blockSignals(True)
                        cb.setChecked(True)
                        cb.blockSignals(False)
        except Exception:
            pass
        # deskNote.lnk: create/remove Desktop + Start Menu per notepad.enabled.
        try:
            from src.desknote import sync_desknote_shortcuts

            sync_desknote_shortcuts(self.settings)
        except Exception:
            pass
        # Explorer「打开方式」: deskNote in, DeskTidy.exe out.
        try:
            from src.desknote_open_with import sync_desknote_open_with
            from src.notepad import notepad_enabled

            sync_desknote_open_with(enabled=notepad_enabled(self.settings))
        except Exception:
            pass

    def _startup_deferred_window_data(self) -> None:
        if self._exiting:
            return
        # Background tray mode: skip desktop scan until the user opens settings.
        try:
            self.window.refresh_data_if_visible()
        except Exception:
            pass

    def _startup_deferred_organize(self) -> None:
        if self._exiting:
            return
        if not bool(self.settings.get("auto_organize_on_startup", False)):
            return
        # 「新文件自动钉选」是自动按规则钉选的总闸：关闭时启动整理也不要
        # 把桌面未归入项吸进分区（否则每次重启都会抵消关闭钉选）。
        # 手动「一键整理」不受影响。首次安装的默认整理走 _startup_deferred_first_run。
        if not bool(self.settings.get("auto_organize_watch", False)):
            get_logger().info(
                "startup organize skipped: auto_organize_watch is off"
            )
            return
        try:
            self._do_organize()
        except OSError:
            pass

    def _startup_deferred_first_run(self) -> None:
        """First install: organize once by rules, then show the onboarding guide."""
        if self._exiting:
            return
        if self.settings.get("first_run_organize_done", True):
            QTimer.singleShot(0, self._maybe_show_first_run_guide)
            return
        try:
            self._do_organize(silent=True)
            self.settings["first_run_organize_done"] = True
            save_settings(self.settings)
        except OSError:
            get_logger().exception("first-run organize failed")
        QTimer.singleShot(400, self._maybe_show_first_run_guide)

    def _startup_deferred_legacy_storage_migrate(self) -> None:
        """One-time: restore leftover physical warehouse files, then virtual-pin."""
        if self._exiting:
            return
        if self.settings.get("legacy_storage_migrated"):
            return
        from src.organizer import legacy_warehouse_has_files, restore_desktop_from_storage

        try:
            if legacy_warehouse_has_files():
                restore_desktop_from_storage()
                # Pin restored desktop files into fences.
                self._do_organize()
            self.settings["legacy_storage_migrated"] = True
            self.settings["organize_mode"] = "virtual"
            save_settings(self.settings, immediate=True)
        except Exception:
            # Still mark migrated to avoid looping on persistent errors.
            self.settings["legacy_storage_migrated"] = True
            try:
                save_settings(self.settings, immediate=True)
            except OSError:
                pass

    def _maybe_show_first_run_guide(self) -> None:
        """First install only: desktop tip cards near fences / rail / tray."""
        if self._exiting:
            return
        from src.ui.first_run_guide import (
            should_auto_show_first_run_guide,
            show_first_run_guide,
        )

        if not should_auto_show_first_run_guide(self.settings):
            return
        # Non-modal tips on the desk; hide settings so overlays are visible.
        show_first_run_guide(self.window, self.settings, desk_app=self)

    def _log_timed(self, label: str, started: float) -> None:
        elapsed_ms = int((time.perf_counter() - started) * 1000)
        # Hot paths (keepalive / attach) used to flood the log with 20ms INFO writes.
        if elapsed_ms >= 80:
            get_logger().info("%s elapsed_ms=%d", label, elapsed_ms)
        elif elapsed_ms >= 40:
            get_logger().debug("%s elapsed_ms=%d", label, elapsed_ms)

    def _apply_session_desktop_view(
        self, *, light: bool = False, show_overlays: bool = True
    ) -> None:
        """While running: hide shell icons and show fence / public overlays."""
        started = time.perf_counter()
        if self._exiting:
            return
        if light and self._foreign_app_owns_foreground():
            # Startup retries at 1.2s / 3.5s / 8s must not restack over apps.
            self._sink_overlays_for_foreign_app()
            self._log_timed("apply_session_desktop_view", started)
            return
        if light and (self.public_icons or self.fences):
            if not self._overlays_need_shell_repair_light():
                self._log_timed("apply_session_desktop_view", started)
                return
        shell_icons_visible = are_desktop_icons_visible()
        if self.settings.get("show_fences", True):
            if self.settings.get("hide_shell_icons") is not True:
                self.settings["hide_shell_icons"] = True
                try:
                    save_settings(self.settings)
                except OSError:
                    pass
            if shell_icons_visible:
                try:
                    set_desktop_icons_visible(False)
                except Exception:
                    pass
            try:
                self._ensure_native_system_namespace_icons()
            except Exception:
                pass
            self._icons_hidden = False
            self._fences_hidden = False
            if show_overlays:
                if self.settings.get("show_fences", True) and not self.fences:
                    self.show_fences()
                if not light:
                    self.refresh_public_desktop()
                elif self.public_icons:
                    self._schedule_overlay_keepalive()
                self._ensure_public_drop_surface()
        elif not shell_icons_visible:
            self._icons_hidden = True
            self._fences_hidden = True
        else:
            self._icons_hidden = False
        self._log_timed("apply_session_desktop_view", started)

    def _ensure_native_system_namespace_icons(self) -> None:
        """Show This PC / Recycle Bin in「常用」(or floats) while shell icons are hidden."""
        try:
            from src.organizer import (
                ensure_hosted_system_namespace_public_icons,
                restore_native_system_namespace_icons,
            )

            if self.settings.get("hide_shell_icons"):
                changed = ensure_hosted_system_namespace_public_icons(
                    self.settings, apply=True
                )
            else:
                changed = restore_native_system_namespace_icons(
                    self.settings, apply=True
                )
            if changed:
                save_settings(self.settings)
        except Exception:
            pass

    def _connect_display_signals(self) -> None:
        """Watch resolution / monitor plug changes for layout profiles."""
        self.qt_app.screenAdded.connect(self._schedule_display_layout_refresh)
        self.qt_app.screenRemoved.connect(self._schedule_display_layout_refresh_removed)
        for screen in QGuiApplication.screens():
            screen.geometryChanged.connect(self._schedule_display_layout_refresh)
            screen.availableGeometryChanged.connect(self._schedule_display_layout_refresh)

    def _schedule_display_layout_refresh(self, *_args) -> None:
        # Debounce bursty screen change signals (projector / Win+P).
        self._display_layout_timer.start(500)

    def _schedule_display_layout_refresh_removed(self, *_args) -> None:
        # Unplug settles slower (mode switches / transient 1024×768); wait longer.
        self._display_layout_timer.start(1000)

    def _on_display_layout_changed(self) -> None:
        if self._exiting:
            return
        # Re-hook new screens that appear after startup.
        for screen in QGuiApplication.screens():
            try:
                screen.geometryChanged.disconnect(self._schedule_display_layout_refresh)
            except TypeError:
                pass
            try:
                screen.availableGeometryChanged.disconnect(self._schedule_display_layout_refresh)
            except TypeError:
                pass
            screen.geometryChanged.connect(self._schedule_display_layout_refresh)
            screen.availableGeometryChanged.connect(self._schedule_display_layout_refresh)

        new_fp = current_display_fingerprint()
        if new_fp == self._display_fingerprint and self.fences:
            # Same fingerprint but geometry nudge — still re-clamp.
            pass
        elif new_fp != self._display_fingerprint:
            old_fp = self._display_fingerprint
            # Persist the layout we were showing under the previous fingerprint first.
            if self.fences and old_fp:
                page = (
                    self._displayed_page
                    if self._displayed_page is not None
                    else self._current_page()
                )
                by_id = {cfg.get("id"): cfg for cfg in self.settings.get("fences", [])}
                for fence in self.fences:
                    geom = fence.get_config_update()
                    save_fence_geometry(
                        self.settings,
                        fence.config,
                        page,
                        geom,
                        fingerprint=old_fp,
                    )
                    stored = by_id.get(fence.config.get("id"))
                    if stored is not None:
                        fence.config = stored
                try:
                    save_settings(self.settings)
                except OSError:
                    pass
            # Multi→fewer: prefer scaling the just-used multi layout onto the
            # remaining panel (stale single-screen profiles looked "wrong").
            # Plug-in keeps a healthy dual profile as-is.
            prepare_destination_profile_for_display_change(
                self.settings, old_fp, new_fp
            )
            purge_transient_display_profiles(self.settings)
            if old_fp:
                self.settings["last_display_fingerprint"] = old_fp
            self._display_fingerprint = new_fp

        if not self._fences_should_show() or not self.fences:
            self._setup_page_indicator()
            # Taskbar move/auto-hide keeps the same fingerprint — still remask so
            # the public 框选 plate does not keep covering the new work-area strip.
            self._refresh_public_host_click_mask()
            return

        page = (
            self._displayed_page
            if self._displayed_page is not None
            else self._current_page()
        )
        apply_geometries_to_fences(self.settings, self.fences, page)
        try:
            save_settings(self.settings, immediate=True)
        except OSError:
            pass
        self._setup_page_indicator()
        self._ensure_desktop_overlays_visible()
        self._refresh_public_host_click_mask()

    def _migrate_fence_pages(self) -> None:
        for fence in self.settings.get("fences", []):
            set_fence_pages(fence, get_fence_pages(fence))

    def _apply_theme(self) -> None:
        from src.ui.toast import invalidate_toast_theme_cache

        theme = normalize_theme(self.settings.get("theme"))
        self.settings["theme"] = theme
        self.qt_app.setStyleSheet(build_stylesheet(theme))
        invalidate_toast_theme_cache()
        # Sidebar chrome is theme-tinted ink — refresh brand/nav + force QSS polish.
        # Called from __init__ before MainWindow exists; guard window access.
        window = getattr(self, "window", None)
        apply_side = getattr(window, "_apply_sidebar_accent", None) if window is not None else None
        if callable(apply_side):
            apply_side(theme)
        page_indicator = getattr(self, "page_indicator", None)
        if page_indicator is not None:
            page_indicator.refresh_theme()
        todo_panel = getattr(self, "todo_panel", None)
        if todo_panel is not None:
            todo_panel.settings = self.settings
            todo_panel.refresh_theme()
        vault_panel = getattr(self, "vault_panel", None)
        if vault_panel is not None:
            vault_panel.settings = self.settings
            apply = getattr(vault_panel, "apply_settings", None)
            if callable(apply):
                apply(self.settings)
            else:
                vault_panel._apply_theme()
        vault_launcher = getattr(self, "vault_launcher", None)
        if vault_launcher is not None:
            vault_launcher.settings = self.settings
            vault_launcher.refresh_theme()
        pet = getattr(self, "pet_widget", None)
        if pet is not None:
            pet.reload_settings(self.settings)
        overlay = getattr(self, "_file_search_overlay", None)
        if overlay is not None:
            overlay.settings = self.settings
            overlay.refresh_theme()

    def _toggle_peek(self) -> None:
        from src.hotkey_manager import HOTKEY_LABELS

        active = self.peek_manager.toggle()
        if self.dock:
            self.dock.set_peek_mode(active)
        state = "已开启" if active else "已关闭"
        self.tray.show_message(HOTKEY_LABELS["peek_fences"], state)

    def _toggle_file_search(self) -> None:
        """Show / hide the fd-powered filename search overlay."""
        from src.ui.file_search_overlay import FileSearchOverlay

        overlay = getattr(self, "_file_search_overlay", None)
        if overlay is not None and overlay.isVisible():
            overlay.close_overlay()
            return
        if overlay is None:
            overlay = FileSearchOverlay(self.settings)
            self._file_search_overlay = overlay
        else:
            overlay.settings = self.settings
        overlay.show_centered()

    def _on_minimized_to_tray(self) -> None:
        self.tray.show_message(
            APP_NAME_ZH,
            "程序已最小化到系统托盘，继续在后台运行。",
        )
        self._trim_background_memory()
        self._schedule_overlay_keepalive()

    def _trim_background_memory(self) -> None:
        """Release excess parked overlays in tray mode — keep icon caches warm."""
        try:
            from src.ui.public_icon_widget import clear_label_pixmap_cache

            # Label pixmaps are cheap to rebuild; file/shell icon caches are not.
            clear_label_pixmap_cache()
        except Exception:
            pass
        try:
            from src.icon_utils import trim_display_icon_cache

            # Halve zoomed display pixmaps under memory pressure; keep shell raw.
            trim_display_icon_cache()
        except Exception:
            pass
        try:
            from src.ui.pet_anim import trim_fit_cache

            trim_fit_cache()
        except Exception:
            pass
        if not self.window.isVisible():
            try:
                self.window.release_lazy_pages()
            except Exception:
                pass
            # Tray/background: keep fewer parked widgets (fences still own HWNDs;
            # public floats share one host HWND).
            self._trim_parked_overlays(max_public=15, max_fences=3)

    @staticmethod
    def _pop_oldest(mapping: dict) -> tuple[Any, Any]:
        """Pop the first-inserted (oldest) item from a plain dict.

        dict.popitem() takes no arguments; only OrderedDict supports
        ``popitem(last=False)``. DeskTidy uses plain dicts for parked
        overlays, so pop the first key explicitly (insertion-ordered).
        """
        key = next(iter(mapping))
        return key, mapping.pop(key)

    def _trim_parked_overlays(
        self, *, max_public: int | None = None, max_fences: int | None = None
    ) -> None:
        pub_cap = int(max_public if max_public is not None else self._max_parked_public_icons)
        fence_cap = int(max_fences if max_fences is not None else self._max_parked_fences)
        while len(self._parked_public_icons) > pub_cap:
            _, victim = self._pop_oldest(self._parked_public_icons)
            if victim is not None:
                self._retire_overlay_widget(victim)
        while len(self._parked_fences) > fence_cap:
            _, victim = self._pop_oldest(self._parked_fences)
            if victim is not None:
                self._retire_overlay_widget(victim)

    def _overlay_restack_blocked(self) -> bool:
        """True when Z-order / re-attach would dismiss menus or flash UI."""
        if self._exiting or self._overlay_mgmt_suspended:
            return True
        if getattr(self, "_overlay_drag_active", False):
            return True
        if getattr(self, "_desktop_popup_open", False):
            return True
        if getattr(self, "_screenshot_session_active", False):
            return True
        if getattr(self, "_recording_session_active", False):
            return True
        try:
            from src.ui.fence_icon_item import desktop_selection_batch_active

            if desktop_selection_batch_active():
                return True
        except Exception:
            pass
        return False

    def _stop_overlay_churn_timers(self) -> None:
        keepalive = getattr(self, "_overlay_keepalive_timer", None)
        fg_timer = getattr(self, "_desktop_fg_recover_timer", None)
        if keepalive is not None:
            keepalive.stop()
        if fg_timer is not None:
            fg_timer.stop()

    def _begin_screenshot_session(self) -> None:
        """Freeze fence/public restack while capturing or pinning a screenshot.

        activateWindow on the snip overlay / pin otherwise forces
        `_ensure_desktop_overlays_visible(force=True)` and icons flash.
        """
        self._screenshot_session_depth = int(
            getattr(self, "_screenshot_session_depth", 0)
        ) + 1
        self._screenshot_session_active = True
        self._stop_overlay_churn_timers()

    def _end_screenshot_session(self) -> None:
        depth = max(0, int(getattr(self, "_screenshot_session_depth", 0)) - 1)
        self._screenshot_session_depth = depth
        if depth > 0:
            return
        self._screenshot_session_active = False
        if self._exiting or self._overlay_mgmt_suspended:
            return
        # Resume slow keepalive only — do not force a restack (that is the flash).
        if not self._desktop_popup_open and not getattr(
            self, "_overlay_drag_active", False
        ):
            timer = getattr(self, "_overlay_keepalive_timer", None)
            if timer is not None and self._overlay_keepalive_needed() and not timer.isActive():
                timer.start()

    def _end_screenshot_session_later(self, delay_ms: int = 450) -> None:
        QTimer.singleShot(max(0, int(delay_ms)), self._end_screenshot_session)

    def _begin_recording_session(self) -> None:
        """Freeze overlay keepalive while FFmpeg is capturing the desktop."""
        self._recording_session_depth = int(
            getattr(self, "_recording_session_depth", 0)
        ) + 1
        self._recording_session_active = True
        self._stop_overlay_churn_timers()

    def _end_recording_session(self) -> None:
        depth = max(0, int(getattr(self, "_recording_session_depth", 0)) - 1)
        self._recording_session_depth = depth
        if depth > 0:
            return
        self._recording_session_active = False
        if self._exiting or self._overlay_mgmt_suspended:
            return
        if not self._desktop_popup_open and not getattr(
            self, "_overlay_drag_active", False
        ):
            timer = getattr(self, "_overlay_keepalive_timer", None)
            if timer is not None and self._overlay_keepalive_needed() and not timer.isActive():
                timer.start()

    def _begin_desktop_popup(self) -> None:
        """Freeze overlay re-attach while a desktop menu / modal dialog is up."""
        self._desktop_popup_depth = int(getattr(self, "_desktop_popup_depth", 0)) + 1
        self._desktop_popup_open = True
        self._stop_overlay_churn_timers()

    def _begin_overlay_drag(self) -> None:
        """Freeze overlay restack while a fence/public icon drag is in progress."""
        self._overlay_drag_active = True
        # Quiet shell attach during + shortly after drag (Explorer OLE can hang).
        self._shell_attach_quiet_until = time.perf_counter() + 2.0
        get_logger().info("overlay drag begin")
        keepalive = getattr(self, "_overlay_keepalive_timer", None)
        fg_timer = getattr(self, "_desktop_fg_recover_timer", None)
        if keepalive is not None:
            keepalive.stop()
        if fg_timer is not None:
            fg_timer.stop()
        # Hotkey poll wakes the UI thread every ~70–120ms; pause while dragging.
        try:
            poll = getattr(self.hotkey_manager, "_poll_timer", None)
            if poll is not None:
                poll.stop()
        except Exception:
            pass

    def _set_public_drag_path(self, path: Path | str | None) -> None:
        """Remember the float being dragged so refresh will not park/drop it."""
        if path is None:
            self._public_drag_path = None
            self._public_drag_keys = set()
            return
        try:
            key = str(path).casefold()
        except OSError:
            key = str(path).casefold()
        self._public_drag_path = key
        self._public_drag_keys = {key}

    def _set_public_drag_paths(self, paths: list[Path] | None) -> None:
        """Remember multi-select public floats mid-drag (refresh must not park them)."""
        if not paths:
            self._public_drag_path = None
            self._public_drag_keys = set()
            return
        keys: set[str] = set()
        primary: str | None = None
        for path in paths:
            try:
                key = str(path).casefold()
            except OSError:
                key = str(path).casefold()
            keys.add(key)
            if primary is None:
                primary = key
        self._public_drag_path = primary
        self._public_drag_keys = keys

    def _end_overlay_drag(self, *, reconcile_fg: bool = True) -> None:
        """End drag freeze and scrub any leftover fullscreen catcher windows.

        ``reconcile_fg=False`` for local folder drops: the icon is already gone,
        so a public refresh + overlay restack would flash the desktop (Explorer
        does not rebuild the desktop layer after a file lands in a folder).
        """
        from src.ui.fence_icon_item import destroy_unpin_catchers

        destroy_unpin_catchers()
        self._overlay_drag_active = False
        # Keep suppressing full shell re-attach briefly after QDrag.exec returns.
        self._shell_attach_quiet_until = max(
            float(getattr(self, "_shell_attach_quiet_until", 0.0)),
            time.perf_counter() + 1.2,
        )
        get_logger().info("overlay drag end reconcile=%s", bool(reconcile_fg))
        try:
            ensure = getattr(self.hotkey_manager, "_ensure_poll_timer", None)
            if callable(ensure):
                ensure()
        except Exception:
            pass
        # Cancel stuck explorer-menu freeze (stray RMB during drag).
        if self._explorer_menu_freeze_armed:
            self._explorer_menu_freeze_armed = False
            self._explorer_menu_freeze_gen = int(self._explorer_menu_freeze_gen) + 1
            # Release whatever popup depth the menu freeze held.
            # Drag already schedules FG reconcile below — skip duplicate sync here.
            while int(getattr(self, "_desktop_popup_depth", 0)) > 0:
                self._end_desktop_popup(reconcile_fg=False)
        if not self._desktop_popup_open:
            timer = getattr(self, "_overlay_keepalive_timer", None)
            if timer is not None and self._overlay_keepalive_needed() and not timer.isActive():
                timer.start()
        if not reconcile_fg:
            self._public_drag_path = None
            self._public_drag_keys = set()
            return
        # Win32 SW_HIDE can lag Qt after custom drag — remap fences immediately.
        QTimer.singleShot(0, self._heal_overlays_after_shell_menu)
        QTimer.singleShot(120, self._flush_public_refresh_after_drag)
        # Defer FG reconcile until shell quiet ends — mid-quiet force-attach
        # flashed the desktop when dragging one doc out of a fence.
        quiet_until = float(getattr(self, "_shell_attach_quiet_until", 0.0))
        fg_delay = max(200, int((quiet_until - time.perf_counter()) * 1000) + 50)

        def _deferred_fg_sync() -> None:
            if self._exiting:
                return
            quiet = float(getattr(self, "_shell_attach_quiet_until", 0.0))
            if time.perf_counter() < quiet:
                QTimer.singleShot(
                    max(50, int((quiet - time.perf_counter()) * 1000) + 50),
                    _deferred_fg_sync,
                )
                return
            self._heal_overlays_after_shell_menu()
            self._sync_overlays_to_foreground()

        QTimer.singleShot(fg_delay, _deferred_fg_sync)

    def _flush_public_refresh_after_drag(self) -> None:
        if self._exiting or getattr(self, "_overlay_drag_active", False):
            return
        # Keep drag-path protection until pin/restore has settled (sync pin runs
        # right after QDrag.exec; a too-early refresh parked hidden floats).
        if getattr(self, "_public_drag_settling", False):
            QTimer.singleShot(80, self._flush_public_refresh_after_drag)
            return
        self._public_drag_path = None
        self._public_drag_keys = set()
        self.refresh_public_desktop()

    def _snapshot_explorer_defview_host(self) -> None:
        """Remember DefView host before a shell menu — Refresh may rebuild it."""
        try:
            from src.desktop_shell_host import _find_defview_host

            host, _ = _find_defview_host()
            self._explorer_menu_defview_host = int(host or 0)
        except Exception:
            self._explorer_menu_defview_host = 0
        self._explorer_menu_recover_attempts = 0

    def _arm_explorer_menu_freeze(self) -> None:
        """Stop overlay restack while Explorer's desktop context menu is open.

        Shell-verb merge no longer owns the menu HWND, so we freeze on desktop
        right-click and release after the system menu dismisses.
        """
        if not self.settings.get("desktop_right_click_menu", True):
            return
        if self._exiting or self._overlay_mgmt_suspended:
            return
        self._snapshot_explorer_defview_host()
        if not self._explorer_menu_freeze_armed:
            self._explorer_menu_freeze_armed = True
            self._begin_desktop_popup()
        else:
            # Already frozen — keep timers stopped while the menu is up.
            keepalive = getattr(self, "_overlay_keepalive_timer", None)
            fg_timer = getattr(self, "_desktop_fg_recover_timer", None)
            if keepalive is not None:
                keepalive.stop()
            if fg_timer is not None:
                fg_timer.stop()
        self._explorer_menu_freeze_gen = int(self._explorer_menu_freeze_gen) + 1
        gen = self._explorer_menu_freeze_gen
        QTimer.singleShot(80, lambda g=gen: self._poll_explorer_menu_dismiss(g, 0))

    def _poll_explorer_menu_dismiss(
        self, gen: int, ticks: int, closed_streak: int = 0
    ) -> None:
        if gen != self._explorer_menu_freeze_gen:
            return
        if self._exiting or not self._explorer_menu_freeze_armed:
            return
        from src.win_shell import is_shell_context_menu_open

        menu_open = False
        try:
            menu_open = bool(is_shell_context_menu_open(allow_enum=False))
        except Exception:
            menu_open = False
        if menu_open:
            closed_streak = 0
        else:
            closed_streak += 1
        # Keep freeze through Win11 modern →「显示更多选项」classic transition
        # (menus disappear briefly between the two). Poll ~120ms to cut FG churn.
        min_ticks = 10  # ~1.2s after click
        need_closed = 5  # ~600ms stably closed
        if ticks < min_ticks or closed_streak < need_closed:
            if ticks < 170:  # ~20s safety cap
                poll_ms = 120 if ticks < min_ticks else 200
                QTimer.singleShot(
                    poll_ms,
                    lambda g=gen, t=ticks + 1, c=closed_streak: self._poll_explorer_menu_dismiss(
                        g, t, c
                    ),
                )
                return
        self._explorer_menu_freeze_armed = False
        self._end_desktop_popup_later(500)
        # System「刷新」only invalidates Explorer's DefView. Reload our overlays
        # after the menu settles so desktop RMB refresh is useful under DeskTidy.
        QTimer.singleShot(700, self._sync_after_explorer_menu)

    def _sync_after_explorer_menu(self) -> None:
        self._recover_overlays_after_desktop_shell_menu()

    def _any_live_overlay_win32_hidden(self) -> bool:
        """True when Qt shows an overlay but Win32 is still SW_HIDE'd."""
        from src.win_shell import overlay_win32_visible

        for widget, peek in self._iter_overlay_widgets():
            if peek:
                continue
            if bool(getattr(widget, "_desktidy_soft_parked", False)):
                continue
            try:
                if not widget.isVisible():
                    continue
                if not overlay_win32_visible(widget):
                    return True
            except RuntimeError:
                continue
            except Exception:
                continue
        return False

    def _recover_overlays_after_desktop_shell_menu(self) -> None:
        """Re-bind overlays after Explorer desktop/folder RMB (incl. system Refresh).

        Explorer「刷新」rebuilds SHELLDLL_DefView. Stale host cache + light repair
        used to miss detached fences and they stayed invisible until restart.
        Full ``force_refresh_desktop`` stays on the DeskTidy「刷新所有分区」verb.
        """
        if self._exiting or self._explorer_menu_freeze_armed:
            return
        if getattr(self, "_overlay_drag_active", False):
            return
        if getattr(self, "_screenshot_session_active", False):
            return
        if self._icons_hidden:
            return

        self._heal_overlays_after_shell_menu()

        prev_host = int(getattr(self, "_explorer_menu_defview_host", 0) or 0)
        host = 0
        host_changed = False
        try:
            from src.desktop_shell_host import _find_defview_host, invalidate_defview_host_cache

            invalidate_defview_host_cache()
            host, _ = _find_defview_host(force=True)
            host = int(host or 0)
            host_changed = bool(prev_host and host and prev_host != host)
        except Exception:
            host = 0

        attempts = int(getattr(self, "_explorer_menu_recover_attempts", 0) or 0)
        if not host and attempts < 5:
            self._explorer_menu_recover_attempts = attempts + 1
            gen = int(getattr(self, "_explorer_menu_recover_gen", 0) or 0) + 1
            self._explorer_menu_recover_gen = gen
            QTimer.singleShot(
                450,
                lambda g=gen: self._recover_overlays_after_desktop_shell_menu_retry(g),
            )
            return

        self._explorer_menu_recover_attempts = 0
        needs = (
            host_changed
            or self._overlays_need_shell_repair()
            or self._any_live_overlay_win32_hidden()
        )
        if needs:
            if host_changed:
                try:
                    from src.desktop_shell_host import invalidate_defview_host_cache

                    invalidate_defview_host_cache()
                except Exception:
                    pass
            needs_repair = host_changed or self._overlays_need_shell_repair()
            # Remap-only heal above fixes SW_HIDE after menus — reserve force
            # attach for detached/under-wallpaper damage (full attach flashes).
            self._ensure_shell_attachments(force=needs_repair)
            if needs_repair or self._any_live_overlay_win32_hidden():
                self._ensure_desktop_overlays_visible(force=needs_repair)
        else:
            self._schedule_overlay_keepalive()

        if host_changed or self._any_live_overlay_win32_hidden():
            gen = int(getattr(self, "_explorer_menu_recover_gen", 0) or 0) + 1
            self._explorer_menu_recover_gen = gen
            QTimer.singleShot(
                650,
                lambda g=gen: self._recover_overlays_after_desktop_shell_menu_retry(g),
            )

    def _recover_overlays_after_desktop_shell_menu_retry(self, gen: int) -> None:
        if gen != int(getattr(self, "_explorer_menu_recover_gen", 0) or 0):
            return
        if self._exiting or self._explorer_menu_freeze_armed:
            return
        self._heal_overlays_after_shell_menu()
        if not (
            self._any_live_overlay_win32_hidden()
            or self._overlays_need_shell_repair_light()
        ):
            self._explorer_menu_recover_attempts = 0
            return
        needs_repair = self._overlays_need_shell_repair_light()
        if needs_repair:
            needs_repair = self._overlays_need_shell_repair()
        self._ensure_shell_attachments(force=needs_repair)
        if needs_repair or self._any_live_overlay_win32_hidden():
            self._ensure_desktop_overlays_visible(force=needs_repair)
        self._explorer_menu_recover_attempts = 0

    def _reload_overlay_icons(self) -> None:
        """Re-extract shell icons in place (no layout rebuild / restack)."""
        from src.icon_utils import invalidate_file_icon_cache

        for fence in list(self.fences):
            try:
                for path in fence._visible_entry_paths():
                    invalidate_file_icon_cache(path)
                fence.reload_icons()
            except Exception:
                pass
        for icon in list(self.public_icons):
            try:
                invalidate_file_icon_cache(getattr(icon, "file_path", None))
                icon._reload_icon()
                icon._reload_label()
            except Exception:
                pass

    def _invalidate_visible_icon_caches(self) -> None:
        """Drop icon cache entries only for currently shown overlay paths."""
        from src.icon_utils import invalidate_file_icon_cache

        for fence in list(self.fences):
            try:
                for path in fence._visible_entry_paths():
                    invalidate_file_icon_cache(path)
            except Exception:
                pass
        for icon in list(self.public_icons):
            try:
                path = getattr(icon, "file_path", None)
                if path is not None:
                    invalidate_file_icon_cache(path)
            except Exception:
                pass

    def _end_desktop_popup(self, *, reconcile_fg: bool = True) -> None:
        depth = max(0, int(getattr(self, "_desktop_popup_depth", 0)) - 1)
        self._desktop_popup_depth = depth
        if depth > 0:
            return
        self._desktop_popup_open = False
        # Resume the slow timer only — forcing ensure/restack here flashes fences.
        if self._exiting or self._overlay_mgmt_suspended:
            return
        # Menu may have left overlays Win32-hidden while Qt still thinks visible.
        self._heal_overlays_after_shell_menu()
        timer = getattr(self, "_overlay_keepalive_timer", None)
        if timer is not None and self._overlay_keepalive_needed() and not timer.isActive():
            timer.start()
        # Menu freeze skipped FG park/unpark edges — reconcile once the popup is gone.
        if reconcile_fg:
            QTimer.singleShot(0, self._sync_overlays_to_foreground)

    def _end_desktop_popup_later(self, delay_ms: int = 400) -> None:
        """Release popup freeze after focus/Z-order churn from the menu settles."""
        QTimer.singleShot(max(0, int(delay_ms)), self._end_desktop_popup)

    def _on_application_state_changed(self, state) -> None:
        if self._overlay_restack_blocked():
            return
        if state in (
            Qt.ApplicationState.ApplicationInactive,
            Qt.ApplicationState.ApplicationHidden,
        ):
            # Multi-app focus loss: do not re-arm / accelerate keepalive.
            # Overlays stay in the desktop band; slow the timer only — no restack.
            timer = getattr(self, "_overlay_keepalive_timer", None)
            if timer is not None and timer.isActive() and timer.interval() < 20000:
                timer.setInterval(20000)
            # In tray/background mode, do a low-frequency memory trim so long
            # lived sessions don't keep icon pixmaps indefinitely.
            if bool(self.settings.get("close_to_tray", True)):
                now = time.perf_counter()
                if now - float(getattr(self, "_last_bg_trim_time", 0.0)) > 60.0:
                    self._last_bg_trim_time = now
                    self._trim_background_memory()
            return
        if state == Qt.ApplicationState.ApplicationActive:
            if self._desk_app_ui_open() and self._desk_app_owns_foreground():
                # Opening settings/notepad activates the app; do not restack
                # overlays (inserting under the UI covers other apps).
                return
            if self._overlay_keepalive_needed():
                # Never force sync restack on focus — that freezes with Explorer.
                self._schedule_overlay_keepalive()

    def _on_desktop_foreground_changed(self) -> None:
        """Explorer desktop enter/leave — sync overlay visibility (single owner)."""
        if time.perf_counter() < float(getattr(self, "_shell_attach_quiet_until", 0.0)):
            return
        self._sync_overlays_to_foreground()

    def _overlays_need_shell_repair(self) -> bool:
        """True when fences/floats are detached, under wallpaper, or not shown.

        Page indicator / dock only check visibility + wallpaper — requiring
        DefView ownership for chrome caused permanent ``needs_repair`` on some
        builds (refresh looked dead; FG recover thrashed).

        Keepalive may pass ``sample=True`` via ``_overlays_need_shell_repair_light``
        to avoid walking every float every few seconds when the desktop is healthy.
        """
        return self._overlays_need_shell_repair_impl(sample=False)

    def _overlays_need_shell_repair_light(self) -> bool:
        """Cheap keepalive probe: fences + up to a few floats, then chrome."""
        return self._overlays_need_shell_repair_impl(sample=True)

    def _overlays_need_shell_repair_impl(self, *, sample: bool) -> bool:
        from src.desktop_shell_host import (
            _find_defview_host,
            is_attached_to_desktop,
            is_stuck_under_wallpaper,
        )
        # Keep this outside the per-widget probe to reduce repeated imports
        # during periodic keepalive checks (lightweight requirement).
        import ctypes

        if self._fences_should_show() and not self.fences:
            return True

        # Resolve DefView host once — never EnumWindows per overlay.
        host, defview = _find_defview_host()
        # Host unknown (Explorer mid-rebuild): do not claim every overlay is
        # broken — that force-attaches in a loop and flashes the cursor.
        if not host:
            return False

        def _broken(widget, *, require_attached: bool) -> bool:
            try:
                hwnd = int(widget.winId())
            except Exception:
                hwnd = 0
            if not hwnd:
                return True
            try:
                if not ctypes.windll.user32.IsWindow(hwnd):
                    return True
            except Exception:
                return True
            if is_stuck_under_wallpaper(hwnd):
                return True
            if require_attached and not is_attached_to_desktop(
                hwnd, host=host, defview=defview
            ):
                return True
            return not overlay_win32_visible(widget)

        if self._fences_should_show():
            for fence in list(self.fences):
                if fence is not None and _broken(fence, require_attached=True):
                    return True
        if not self._icons_hidden:
            icons = list(self.public_icons)
            if sample and len(icons) > 4:
                # Probe ends + mid — full walk only when something looks wrong.
                probe = [icons[0], icons[len(icons) // 2], icons[-1], icons[1]]
                for icon in probe:
                    if _broken(icon, require_attached=True):
                        # Confirm with a full pass before reporting repair.
                        for icon2 in icons:
                            if _broken(icon2, require_attached=True):
                                return True
                        break
            else:
                for icon in icons:
                    if _broken(icon, require_attached=True):
                        return True
        vault = getattr(self, "vault_panel", None)
        try:
            vault_open = vault is not None and bool(vault.isVisible())
        except RuntimeError:
            vault_open = False
        for chrome in (
            self.page_indicator,
            self.dock,
            getattr(self, "todo_panel", None),
            vault if vault_open else None,
            getattr(self, "vault_launcher", None),
            getattr(self, "pet_widget", None),
        ):
            if chrome is not None and _broken(chrome, require_attached=False):
                return True
        return False

    def _foreign_app_owns_foreground(self) -> bool:
        """True when another process (WeChat / VS Code / …) owns FG."""
        if self._overlay_restack_blocked():
            return False
        if self._desk_app_ui_open() and self._desk_app_owns_foreground():
            return False
        try:
            from src.win_shell import is_desktop_foreground

            return not is_desktop_foreground()
        except Exception:
            return False

    def _should_park_overlays_for_foreign_fg(self) -> bool:
        """Hide-on-foreign-FG gate — always False (Fences-class under-apps).

        Real Fences leave the desktop layer mapped; normal windows cover it.
        Hiding on WeChat FG made partitions vanish instead of sitting underneath.
        Foreign FG only sinks via ``_sink_overlays_for_foreign_app``.
        """
        return False

    def _overlay_widgets_may_show(self) -> bool:
        """Gate for fence / public-float ``show()`` (not page chrome)."""
        if self._exiting or self._icons_hidden:
            return False
        if self._desk_app_ui_open() and self._desk_app_owns_foreground():
            return False
        # Foreign apps: stay mapped under windows — do not gate on FG park latch.
        return True

    def _page_chrome_may_show(self) -> bool:
        """Right-edge page bar / dock — stays up under foreign apps (dock pattern).

        Settings/notepad still suppress chrome (sunk under those app windows).
        """
        if self._exiting or self._icons_hidden:
            return False
        if self._desk_app_ui_open() and self._desk_app_owns_foreground():
            return False
        return True

    def _sync_overlays_to_foreground(self) -> None:
        """Sole owner of FG overlay policy. All FG entry points call this.

        Three states (one owner):
        - foreign app FG → sink under windows (stay visible underneath)
        - DeskTidy chrome FG → stay visible, never shell-repair
        - Explorer desktop FG → recover/repair
        """
        if self._overlay_restack_blocked():
            return
        if self._desk_app_ui_open() and self._desk_app_owns_foreground():
            # Settings / notepad FG: sink overlays under the app window (do not hide).
            self._keep_overlays_under_apps()
            return
        if not self._overlay_keepalive_needed() and not self._fences_should_show():
            return

        if self._foreign_app_owns_foreground():
            self._last_fg_was_desktop = False
            self._sink_overlays_for_foreign_app()
            return

        # Drop sticky hide latch left by older builds — never force-restack.
        if getattr(self, "_overlays_parked_for_app_fg", False):
            self._overlays_parked_for_app_fg = False

        # Own chrome (fence/page bar) is desktop-side for visibility, but must
        # not trigger Explorer shell-repair (would restack over VS Code, etc.).
        try:
            from src.win_shell import is_explorer_desktop_foreground

            explorer_fg = bool(is_explorer_desktop_foreground())
        except Exception:
            explorer_fg = False
        if not explorer_fg:
            if self._fences_should_show():
                self._remap_hidden_overlay_hwnds()
            return

        was_desktop = bool(getattr(self, "_last_fg_was_desktop", False))
        self._last_fg_was_desktop = True
        if was_desktop:
            # Still remap: drag/shell churn can leave fences SW_HIDE while Qt
            # reports visible — was_desktop must not skip the heal path.
            if self._fences_should_show():
                self._remap_hidden_overlay_hwnds()
            return
        if self._desktop_fg_recover_timer.isActive():
            return
        self._desktop_fg_recover_timer.start(450)

    def _recover_from_desktop_foreground(self) -> None:
        """Shell repair after returning to Explorer desktop (host may be new)."""
        if self._overlay_restack_blocked() or (
            self._desk_app_ui_open() and self._desk_app_owns_foreground()
        ):
            return
        if not self._overlay_keepalive_needed() and not self._fences_should_show():
            return

        if self._foreign_app_owns_foreground():
            self._last_fg_was_desktop = False
            self._sink_overlays_for_foreign_app()
            return
        # Repair only when Explorer owns FG — own chrome must not force-reattach.
        try:
            from src.win_shell import is_explorer_desktop_foreground

            if not is_explorer_desktop_foreground():
                return
        except Exception:
            return
        self._last_fg_was_desktop = True
        self._overlays_parked_for_app_fg = False
        self._remap_hidden_overlay_hwnds()

        needs_repair = self._overlays_need_shell_repair_light()
        if needs_repair:
            # Confirm with a full walk only when the light probe suspects damage.
            needs_repair = self._overlays_need_shell_repair()

        now = time.perf_counter()
        min_gap = 0.25 if needs_repair else 0.45
        if now - self._last_desktop_fg_recover < min_gap:
            if not self._desktop_fg_recover_timer.isActive():
                self._desktop_fg_recover_timer.start(450)
            return
        self._last_desktop_fg_recover = now

        if self._fences_should_show() and not self.fences:
            self.show_fences()
            return

        if needs_repair:
            try:
                from src.desktop_shell_host import invalidate_defview_host_cache

                invalidate_defview_host_cache()
            except Exception:
                pass
            self._ensure_shell_attachments(force=True)
        else:
            self._ensure_desktop_overlays_visible(force=False)
            # Only raise when hit-test proves chrome is buried — unconditional
            # raise_band after Alt+Tab flashed the page bar on every return.
            if self._page_chrome_needs_raise():
                self._ensure_page_chrome_visible(raise_band=True)
        self._schedule_overlay_keepalive()

    def _iter_overlay_widgets(self):
        if self._fences_should_show():
            for fence in self.fences:
                if fence is not None:
                    yield fence, bool(getattr(fence, "_peek_mode", False))
        show_chrome = self._fences_should_show() or (
            bool(self.settings.get("show_fences", True))
            and not self._fences_hidden
        )
        # Page bar is independent of fence visibility — hotkey switches must
        # still keep it in the shell-attach set when overlays are active.
        if (
            self.page_indicator
            and self.settings.get("show_page_indicator", True)
            and not self._icons_hidden
        ):
            yield self.page_indicator, False
        if self.dock and self.settings.get("dock", {}).get("enabled") and show_chrome:
            yield self.dock, bool(getattr(self.dock, "_peek_mode", False)) if hasattr(
                self.dock, "_peek_mode"
            ) else False
        todo = getattr(self, "todo_panel", None)
        if todo is not None:
            from src.todos import desktop_todos_enabled

            if desktop_todos_enabled(self.settings) and not self._icons_hidden:
                yield todo, False
        vault = getattr(self, "vault_panel", None)
        if vault is not None:
            from src.account_vault import account_vault_enabled

            # Panel stays user-toggled: do not yield while hidden, or shell
            # repair / apply_overlay_stack_mode would show() it on every
            # desktop FG return (Win+D / Alt+Tab).
            try:
                vault_open = bool(vault.isVisible())
            except RuntimeError:
                vault_open = False
            if (
                account_vault_enabled(self.settings)
                and not self._icons_hidden
                and vault_open
            ):
                yield vault, False
        launcher = getattr(self, "vault_launcher", None)
        if launcher is not None:
            from src.account_vault import account_vault_enabled

            if account_vault_enabled(self.settings) and not self._icons_hidden:
                yield launcher, False
        pet = getattr(self, "pet_widget", None)
        if pet is not None:
            from src.desktop_pet import desktop_pet_visible

            if desktop_pet_visible(self.settings) and not self._icons_hidden:
                yield pet, False
        if not self._icons_hidden:
            host = getattr(self, "_public_icon_host", None)
            if host is not None and (
                self.public_icons or self._parked_public_icons
            ):
                # Single HWND for every public float child.
                yield host, False
            else:
                for icon in self.public_icons:
                    yield icon, False

    def _settings_window_hwnd(self) -> int:
        win = getattr(self, "window", None)
        if win is None:
            return 0
        try:
            if not bool(win.isVisible()) or bool(win.isMinimized()):
                return 0
            return int(win.winId()) or 0
        except Exception:
            return 0

    def _settings_ui_open(self) -> bool:
        return bool(self._settings_window_hwnd())

    def _notepad_window_hwnd(self) -> int:
        from PyQt6 import sip

        win = getattr(self, "_notepad_window", None)
        if win is None:
            return 0
        try:
            if sip.isdeleted(win):
                return 0
            if not bool(win.isVisible()) or bool(win.isMinimized()):
                return 0
            return int(win.winId()) or 0
        except Exception:
            return 0

    def _notepad_ui_open(self) -> bool:
        return bool(self._notepad_window_hwnd())

    def _desk_app_ui_open(self) -> bool:
        """Settings or notepad — normal app windows that must stay above overlays."""
        return self._settings_ui_open() or self._notepad_ui_open()

    def _hwnd_owns_foreground(self, owner_hwnd: int) -> bool:
        """True when *owner_hwnd* or one of its children owns Win32 foreground."""
        if not owner_hwnd:
            return False
        try:
            import win32gui

            from src.win_shell import user32

            fg = int(user32.GetForegroundWindow() or 0)
        except Exception:
            return False
        if not fg:
            return False
        if fg == owner_hwnd:
            return True
        try:
            if win32gui.IsChild(owner_hwnd, fg):
                return True
        except OSError:
            pass
        try:
            root = int(win32gui.GetAncestor(fg, 2) or 0)
            if root == owner_hwnd:
                return True
        except OSError:
            pass
        return False

    def _desk_app_owns_foreground(self) -> bool:
        """True when settings or notepad (or their modal children) owns FG."""
        return self._hwnd_owns_foreground(
            self._settings_window_hwnd()
        ) or self._hwnd_owns_foreground(self._notepad_window_hwnd())

    def _remap_hidden_overlay_hwnds(self) -> None:
        """Fences-class: keep current-page overlays Win32-mapped (never leave SW_HIDE).

        Soft-parked off-page fences stay hidden. First RMB / shell menu can leave
        live fences SW_HIDE'd while Qt still reports isVisible — foreign-FG sink
        used to skip remount and partitions stayed gone.
        """
        from src.desktop_shell_host import set_overlay_hwnd_visible
        from src.win_shell import overlay_win32_visible

        for widget, peek in self._iter_overlay_widgets():
            if peek:
                continue
            if bool(getattr(widget, "_desktidy_soft_parked", False)):
                continue
            try:
                if not widget.isVisible():
                    continue
                hwnd = self._fence_hwnd(widget)
                if not hwnd:
                    continue
                if overlay_win32_visible(widget):
                    continue
                set_overlay_hwnd_visible(hwnd, True)
            except RuntimeError:
                continue
            except Exception:
                continue

    def _heal_overlays_after_shell_menu(self) -> None:
        """After TrackPopupMenu / Explorer RMB: remount any Win32-hidden overlays."""
        if self._exiting or self._icons_hidden:
            return
        self._remap_hidden_overlay_hwnds()

    def _sink_overlays_for_foreign_app(self) -> None:
        """Keep fences/floats mapped under WeChat / VS Code / other apps.

        Fences-class: the desktop layer stays visible; normal windows cover it.
        Never Qt-hide on foreign FG (that made partitions vanish entirely).
        """
        if self._overlay_restack_blocked() or (
            self._desk_app_ui_open() and self._desk_app_owns_foreground()
        ):
            return
        was_hidden = bool(getattr(self, "_overlays_parked_for_app_fg", False))
        self._overlays_parked_for_app_fg = False
        if was_hidden and not self._exiting and not self._icons_hidden:
            for widget, peek in self._iter_overlay_widgets():
                if peek:
                    continue
                if bool(getattr(widget, "_desktidy_soft_parked", False)):
                    continue
                try:
                    if not widget.isVisible():
                        widget.show()
                except RuntimeError:
                    pass
        # Even without the sticky latch: remount SW_HIDE'd live overlays.
        self._remap_hidden_overlay_hwnds()
        self._keep_overlays_under_apps()
        # Never HWND_TOP chrome here: Progman-owned raise used to lift fences
        # over the foreign app a few seconds later (keepalive / FG sink).

    def _park_overlays_for_foreign_app(self) -> None:
        """Compat alias — foreign FG no longer hides; sink under apps instead."""
        self._sink_overlays_for_foreign_app()

    def _clear_foreign_fg_park_latch_if_desktop(self) -> None:
        """Drop sticky hide latch left by older builds when desktop is FG."""
        if not getattr(self, "_overlays_parked_for_app_fg", False):
            return
        if self._foreign_app_owns_foreground():
            return
        self._overlays_parked_for_app_fg = False

    def _apply_foreground_overlay_visibility(self) -> None:
        """Startup / show_fences hook — delegates to the single FG owner."""
        self._sync_overlays_to_foreground()

    def _keep_overlays_under_apps(self) -> None:
        """Sink overlays into the desktop band (under settings / notepad / other apps).

        Fences-class: DefView-attached overlays already sit under APPWINDOWs —
        do **not** HWND_BOTTOM healthy attached HWNDs (tray open / settings show
        used that and buried partitions under the wallpaper).

        Only sink unattached overlays that would float over settings/apps.
        Stuck-under-wallpaper HWNDs are reattached, never sunk again.
        """
        from src.desktop_shell_host import (
            attach_overlay_to_desktop,
            is_attached_to_desktop,
            is_stuck_under_wallpaper,
            place_overlay_in_desktop_band,
        )

        app_ui = self._desk_app_ui_open()
        app_ui_fg = bool(app_ui and self._desk_app_owns_foreground())
        foreign_fg = self._foreign_app_owns_foreground()
        if not app_ui_fg and not foreign_fg:
            self._ensure_page_chrome_visible()
            return

        for widget, peek in self._iter_overlay_widgets():
            if peek:
                continue
            if not app_ui_fg and (
                widget is self.page_indicator
                or widget is self.dock
                or widget is getattr(self, "todo_panel", None)
                or widget is getattr(self, "vault_panel", None)
                or widget is getattr(self, "vault_launcher", None)
                or widget is getattr(self, "pet_widget", None)
            ):
                continue
            is_chrome = (
                widget is self.page_indicator
                or widget is self.dock
                or widget is getattr(self, "todo_panel", None)
                or widget is getattr(self, "vault_panel", None)
                or widget is getattr(self, "vault_launcher", None)
                or widget is getattr(self, "pet_widget", None)
            )
            # Settings/notepad: may-show gate skips reveal, but new fences from
            # hide→show must still map onto DefView (under APPWINDOWs). Leaving
            # them never-shown made 「显示」appear to do nothing.
            if app_ui_fg and not is_chrome:
                try:
                    if not widget.isVisible():
                        self._reveal_desktop_overlay(widget)
                        continue
                except RuntimeError:
                    continue
            try:
                hwnd = int(widget.winId())
            except Exception:
                continue
            if not hwnd:
                continue
            try:
                if is_stuck_under_wallpaper(hwnd):
                    attach_overlay_to_desktop(hwnd, show=True)
                    continue
                if is_attached_to_desktop(hwnd):
                    # Already in the desktop band — HWND_BOTTOM buries under wallpaper.
                    continue
            except Exception:
                pass
            # force only while settings/notepad own FG — foreign-FG sink must
            # respect the 2s HWND_BOTTOM rate limit (cursor flash otherwise).
            place_overlay_in_desktop_band(hwnd, force=app_ui_fg)
        if self._settings_ui_open() and self._hwnd_owns_foreground(
            self._settings_window_hwnd()
        ):
            self._raise_settings_window()
        elif self._notepad_ui_open() and self._hwnd_owns_foreground(
            self._notepad_window_hwnd()
        ):
            self._raise_notepad_window()
        elif not foreign_fg:
            self._ensure_page_chrome_visible()

    def _raise_settings_window(self) -> None:
        """Ensure the main window stays above desktop-band overlays."""
        win = getattr(self, "window", None)
        if win is None:
            return
        try:
            if not bool(win.isVisible()) or bool(win.isMinimized()):
                return
            # raise_ only — activateWindow here steals focus from in-window edits.
            win.raise_()
        except Exception:
            pass

    def _raise_notepad_window(self) -> None:
        """Keep built-in notepad above DefView-attached fences."""
        from PyQt6 import sip

        win = getattr(self, "_notepad_window", None)
        if win is None or sip.isdeleted(win):
            return
        try:
            if not bool(win.isVisible()) or bool(win.isMinimized()):
                return
            win.raise_()
        except Exception:
            pass

    def _ensure_shell_attachments(self, *, force: bool = False) -> None:
        """Re-attach overlays onto the DefView host (Explorer restart safe)."""
        if self._overlay_restack_blocked():
            # Force refresh / page-switch must not die while the menu freeze is up.
            if force:
                self._schedule_force_shell_attach(150)
            return
        # App UI open: skip immediate restack (flashes fences over the UI),
        # but queue force repair so Win+D / refresh still land after close.
        if self._desk_app_ui_open() and self._desk_app_owns_foreground():
            if force:
                self._schedule_force_shell_attach(400)
            return
        if self._foreign_app_owns_foreground():
            # Attach+chrome HWND_TOP here lifts fences over the focused app.
            self._sink_overlays_for_foreign_app()
            return
        now = time.perf_counter()
        quiet_until = float(getattr(self, "_shell_attach_quiet_until", 0.0))
        page_quiet = float(getattr(self, "_page_switch_quiet_until", 0.0))
        if not force and now < page_quiet:
            return
        if now < quiet_until:
            # Post-drag / mid-drag: full attach talks to Explorer and can deadlock.
            # Keep force=True on the retry — demoting to soft used to leave
            # hotkey page-switch floats stuck under the wallpaper.
            if force:
                self._schedule_force_shell_attach(
                    max(50, int((quiet_until - now) * 1000) + 50)
                )
            return
        last = getattr(self, "_last_shell_attach_at", 0.0)
        # Force still throttled: FG-hook storms used to bypass this and flash.
        min_gap = 0.35 if force else 0.8
        if (now - last) < min_gap:
            # Rapid hotkey page flips used to drop the second force attach,
            # leaving unparked icons invisible until a later keepalive.
            if force:
                self._schedule_force_shell_attach(
                    max(40, int((min_gap - (now - last)) * 1000) + 20)
                )
            return
        self._shell_attach_force_pending = False
        self._last_shell_attach_at = now
        started = time.perf_counter()
        self._suppress_fence_layout_save = True
        try:
            for widget, peek in self._iter_overlay_widgets():
                try:
                    apply_overlay_stack_mode(
                        widget,
                        desktop_layer=True,
                        peek=peek,
                    )
                except Exception:
                    # One dead HWND must not abort the whole attach pass
                    # (that used to leave overlays half-fixed and flash the cursor).
                    continue
            self._overlays_shell_attached = True
            # Do NOT HWND_BOTTOM every overlay here — that was the mouse-move flash.
            # Deliberate sink stays on settings show / dissolve / explicit keep.
            # Force repair / FG recover: raise chrome so it is not left under DefView.
            # Soft keepalive attach keeps raise_band=False to avoid Z thrash.
            self._ensure_page_chrome_visible(raise_band=bool(force))
        finally:
            self._suppress_fence_layout_save = False
            self._log_timed("ensure_shell_attachments", started)

    def _schedule_force_shell_attach(self, delay_ms: int) -> None:
        """Queue at most one deferred force attach (prefer the soonest delay)."""
        delay_ms = max(0, int(delay_ms))
        prev_delay = getattr(self, "_shell_attach_force_delay_ms", None)
        if getattr(self, "_shell_attach_force_pending", False):
            if prev_delay is not None and prev_delay <= delay_ms:
                return
        seq = int(getattr(self, "_shell_attach_force_seq", 0)) + 1
        self._shell_attach_force_seq = seq
        self._shell_attach_force_pending = True
        self._shell_attach_force_delay_ms = delay_ms

        def _fire(expected: int = seq) -> None:
            if int(getattr(self, "_shell_attach_force_seq", 0)) != expected:
                return
            self._run_pending_force_shell_attach()

        QTimer.singleShot(delay_ms, _fire)

    def _schedule_post_refresh_shell_heal(self) -> None:
        """Coalesce soft shell heal after fence force-refresh (one timer, not N).

        Per-fence force attach after every menu refresh restacked the whole
        desktop about 300ms later.
        """
        if getattr(self, "_post_refresh_heal_pending", False):
            return
        self._post_refresh_heal_pending = True

        def _run() -> None:
            self._post_refresh_heal_pending = False
            if self._exiting:
                return
            if self._overlay_restack_blocked():
                QTimer.singleShot(150, _run)
                return
            try:
                self._heal_overlays_after_shell_menu()
                force = (
                    self._overlays_need_shell_repair_light()
                    or self._any_live_overlay_win32_hidden()
                )
                self._ensure_shell_attachments(force=force)
                self._ensure_desktop_overlays_visible(force=force)
            except Exception:
                pass

        QTimer.singleShot(300, _run)

    def _run_pending_force_shell_attach(self) -> None:
        self._shell_attach_force_pending = False
        self._shell_attach_force_delay_ms = None
        if self._exiting:
            return
        # Still frozen (menu/drag): keep trying — do not drop user refresh.
        if self._overlay_restack_blocked():
            self._schedule_force_shell_attach(150)
            return
        if self._desk_app_ui_open() and self._desk_app_owns_foreground():
            # Do not spin every 400ms while editing settings/notepad — run once on hide.
            self._shell_attach_after_settings = True
            return
        if self._foreign_app_owns_foreground():
            self._sink_overlays_for_foreign_app()
            return
        # One attach owner this tick. Calling ensure_desktop_overlays_visible(force=True)
        # here re-entered _ensure_shell_attachments via _sync_overlay_layer_mode and
        # hit the 0.35s throttle → scheduled another force attach forever.
        self._clear_foreign_fg_park_latch_if_desktop()
        self._ensure_shell_attachments(force=True)
        # Chrome raise is owned by force attach above; no second force path.
        # Force attach can remount the public host above fences — same invariant
        # as soft-add / refresh_public_desktop / boot force-attach.
        # Do NOT rebuild icon grids here (boot surface heal): force refresh
        # schedules post-refresh shell heal → force attach → flicker loop.
        try:
            self.ensure_live_fences_interactive()
        except Exception:
            pass

    def _release_stuck_grabs(self) -> None:
        """Release any orphaned mouse/keyboard grabs left by interrupted marquee.

        ``_begin_marquee`` calls ``grabMouse``; if the operation is interrupted
        (crash, page switch without Escape, or a modal dialog popping up),
        the grab stays active and silently swallows every click on the settings
        window — the user sees「弹窗点不了」. Aborting the marquee on the public
        host plus a global grabber release covers fence/screenshot grabs too.
        """
        from PyQt6.QtWidgets import QWidget

        host = getattr(self, "_public_icon_host", None)
        if host is not None:
            try:
                abort = getattr(host, "_abort_marquee", None)
                if callable(abort):
                    abort()
            except RuntimeError:
                pass
        # Fence / screenshot overlays also grab — release any survivor.
        try:
            grabber = QWidget.mouseGrabber()
            if grabber is not None:
                grabber.releaseMouse()
        except (RuntimeError, Exception):
            pass
        try:
            kb = QWidget.keyboardGrabber()
            if kb is not None:
                kb.releaseKeyboard()
        except (RuntimeError, Exception):
            pass

    def _on_settings_shown(self) -> None:
        """Main window opened — freeze overlay thrashing; sink once to desktop band."""
        now = time.perf_counter()
        if now - float(getattr(self, "_settings_shown_at", 0.0)) < 0.08:
            return
        self._settings_shown_at = now
        # An interrupted marquee (crash / page switch) leaves grabMouse active
        # and silently swallows all clicks on the settings window.
        self._release_stuck_grabs()
        # HWND_BOTTOM sink — never insert just below the settings HWND (that
        # covers VS Code and every other app under the settings window).
        # Further show()/attach while settings is open also sinks via
        # ``_overlay_widgets_may_show`` / show_fences → ``_keep_overlays_under_apps``.
        self._keep_overlays_under_apps()
        timer = getattr(self, "_overlay_keepalive_timer", None)
        if timer is not None:
            # Slow keepalive only; do not force re-attach while editing settings.
            timer.setInterval(20000)
            if self._overlay_keepalive_needed() and not timer.isActive():
                timer.start()

    def _on_settings_hidden(self) -> None:
        if self._overlay_restack_blocked():
            return
        self._trim_background_memory()
        self._schedule_overlay_keepalive()
        self._remap_hidden_overlay_hwnds()
        need_force = bool(getattr(self, "_shell_attach_after_settings", False))
        self._shell_attach_after_settings = False
        if not need_force:
            try:
                need_force = bool(self._overlays_need_shell_repair_light())
            except Exception:
                need_force = False
        # Closing settings used to always force-restack every fence (stutter).
        # Only force when HWNDs were created under settings or repair is needed.
        if need_force:
            self._schedule_force_shell_attach(120)
        else:
            QTimer.singleShot(0, lambda: self._ensure_shell_attachments(force=False))

    def _sync_overlay_layer_mode(self, *, force: bool = False) -> None:
        """Ensure overlays stay attached to the desktop shell host."""
        if self._overlay_restack_blocked():
            return
        if not force and time.perf_counter() < float(
            getattr(self, "_page_switch_quiet_until", 0.0)
        ):
            return
        if not self._overlay_keepalive_needed():
            return
        if self._desk_app_ui_open() and self._desk_app_owns_foreground():
            # Never force full re-attach while settings/notepad owns FG.
            return
        needs_repair = self._overlays_need_shell_repair()
        if not force and not needs_repair and self._overlays_shell_attached:
            return
        if not self.fences and needs_repair and self._fences_should_show():
            self.show_fences()
            return
        self._ensure_shell_attachments(force=force or needs_repair)

    def _schedule_overlay_keepalive(self) -> None:
        """Arm periodic shell re-attach; do not SetWindowPos immediately.

        Immediate ensure here was the main right-click / focus-churn flicker:
        ApplicationInactive and menu dismiss both called this and restacked every
        fence under the cursor.
        """
        if self._overlay_restack_blocked() or not self._overlay_keepalive_needed():
            timer = getattr(self, "_overlay_keepalive_timer", None)
            if timer is not None:
                timer.stop()
            return
        timer = getattr(self, "_overlay_keepalive_timer", None)
        if timer is not None:
            # Do NOT pull a healthy slow timer back to 2.5s on every Alt-Tab /
            # ApplicationInactive — that was a multi-app stutter source.
            if not timer.isActive():
                if timer.interval() < 15000:
                    timer.setInterval(20000)
                timer.start()

    def _overlay_keepalive_needed(self) -> bool:
        if self._exiting or self._overlay_mgmt_suspended:
            return False
        show_chrome = self._fences_should_show() or (
            bool(self.settings.get("show_fences", True))
            and not self._fences_hidden
        )
        if self.fences:
            return True
        # Page bar alone is enough reason to keep the shell-attach timer alive.
        if (
            self.page_indicator
            and self.settings.get("show_page_indicator", True)
            and not self._icons_hidden
        ):
            return True
        if self.dock and self.settings.get("dock", {}).get("enabled") and show_chrome:
            return True
        todo = getattr(self, "todo_panel", None)
        if todo is not None and not self._icons_hidden:
            from src.todos import desktop_todos_enabled

            if desktop_todos_enabled(self.settings):
                return True
        # Closed vault panel must not keep keepalive / repair alive — buoy alone
        # is enough; otherwise desktop return treats the hidden panel as broken.
        vault = getattr(self, "vault_panel", None)
        if vault is not None and not self._icons_hidden:
            from src.account_vault import account_vault_enabled

            try:
                vault_open = bool(vault.isVisible())
            except RuntimeError:
                vault_open = False
            if account_vault_enabled(self.settings) and vault_open:
                return True
        launcher = getattr(self, "vault_launcher", None)
        if launcher is not None and not self._icons_hidden:
            from src.account_vault import account_vault_enabled

            if account_vault_enabled(self.settings):
                return True
        pet = getattr(self, "pet_widget", None)
        if pet is not None and not self._icons_hidden:
            from src.desktop_pet import desktop_pet_visible

            if desktop_pet_visible(self.settings):
                return True
        if not self._icons_hidden and self.public_icons:
            return True
        return False

    def _start_overlay_keepalive_timer(self) -> None:
        """Periodic shell re-attach so Explorer restarts cannot orphan overlays."""
        if getattr(self, "_overlay_keepalive_timer", None) is not None:
            return
        self._overlay_keepalive_timer = QTimer()
        # Boot: moderate poll until overlays stabilize; handler relaxes to 20s.
        # Was 8s — too chatty right after launch when attachments are already healthy.
        self._overlay_keepalive_timer.setInterval(15000)
        self._overlay_keepalive_timer.timeout.connect(self._ensure_desktop_overlays_visible)
        if self._overlay_keepalive_needed():
            self._overlay_keepalive_timer.start()

    def _ensure_desktop_overlays_visible(self, *, force: bool = False) -> None:
        started = time.perf_counter()
        if not force and time.perf_counter() < float(
            getattr(self, "_page_switch_quiet_until", 0.0)
        ):
            return
        if self._overlay_restack_blocked():
            if force:
                self._schedule_force_shell_attach(150)
            return
        timer = getattr(self, "_overlay_keepalive_timer", None)
        if not self._overlay_keepalive_needed():
            self._overlay_last_needed_restore = False
            self._overlay_keepalive_stable = 0
            if timer is not None:
                timer.stop()
            return
        if self._desk_app_ui_open() and self._desk_app_owns_foreground():
            # Do not restack while settings/notepad owns FG (avoids Z-order fights / flash).
            if force:
                self._shell_attach_after_settings = True
            return

        # Foreign FG: sink under windows (stay visible underneath). Never hide.
        if self._foreign_app_owns_foreground():
            self._sink_overlays_for_foreign_app()
            if timer is not None and timer.interval() < 20000:
                timer.setInterval(20000)
            return
        # Sticky latch from older hide-on-FG builds — clear when desktop-side.
        if getattr(self, "_overlays_parked_for_app_fg", False):
            self._overlays_parked_for_app_fg = False

        # Multi-app: while the user is in other windows, skip repair walks.
        # Returning to desktop (Win+D / Alt+Tab) still repairs via FG recover.
        if not force and not is_explorer_desktop_foreground():
            if timer is not None and timer.interval() < 20000:
                timer.setInterval(20000)
            return

        if force:
            before_hidden = True
        elif int(getattr(self, "_overlay_keepalive_stable", 0)) >= 1:
            before_hidden = self._overlays_need_shell_repair_light()
        else:
            before_hidden = self._overlays_need_shell_repair()
        self._sync_overlay_layer_mode(force=force or before_hidden)
        self._overlay_last_needed_restore = before_hidden
        if timer is None:
            return
        if before_hidden:
            self._overlay_keepalive_stable = 0
            if timer is not None and timer.interval() > 8000:
                timer.setInterval(8000)
        else:
            self._overlay_keepalive_stable += 1
            if timer is not None:
                if self._overlay_keepalive_stable >= 8 and timer.interval() < 30000:
                    timer.setInterval(30000)
                elif self._overlay_keepalive_stable >= 3 and timer.interval() < 20000:
                    timer.setInterval(20000)
            if (
                self._overlay_keepalive_stable >= 6
                and not self.window.isVisible()
                and self._overlay_keepalive_stable % 3 == 0
            ):
                self._trim_background_memory()
            # Rare: Desktop folder relocate while tray-running — rebind watcher.
            if self._overlay_keepalive_stable >= 3 and self._overlay_keepalive_stable % 4 == 0:
                self._maybe_reschedule_desktop_watcher()
        # Force repair raises chrome; keepalive stays soft (no Z thrash).
        self._ensure_page_chrome_visible(raise_band=bool(force))
        self._log_timed("ensure_desktop_overlays_visible", started)

    def _ensure_public_drop_surface(self) -> None:
        """Keep PublicIconHost attached for Explorer folder→desktop OLE when needed."""
        if self._exiting or self._icons_hidden:
            return
        try:
            host = self._ensure_public_icon_host()
            if not host.session_owns_desktop_drops():
                return
            host.present(force_attach=False)
        except Exception:
            pass

    def _try_ingest_desktop_file_as_public(self, raw: str) -> bool:
        """Explorer-owned drop: show a shared/public float for new desktop files."""
        if not raw or not bool(self.settings.get("enable_public_desktop")):
            return False
        if self._icons_hidden:
            return False
        try:
            from src.organize_suppress import is_organize_suppressed

            if is_organize_suppressed(raw):
                return False
        except Exception:
            pass
        try:
            path = Path(raw)
        except OSError:
            return False
        from src.fence_rules import all_fence_pinned_keys, path_in_pinned_keys
        from src.public_desktop import add_public_item, find_public_entry
        from src.desktop_scanner import is_ignored_desktop_entry

        if is_ignored_desktop_entry(
            path.name, self.settings.get("exclude_patterns") or []
        ):
            return False

        try:
            if not path.exists():
                return False
            from src.win_shell import is_desktop_loose_item

            on_desktop = is_desktop_loose_item(path)
        except OSError:
            return False
        if not on_desktop:
            return False
        if path_in_pinned_keys(path, all_fence_pinned_keys(self.settings)):
            return False
        if find_public_entry(self.settings, path) is not None:
            return False
        try:
            page_id = int(self.settings.get("current_page", 0))
        except (TypeError, ValueError):
            page_id = 0
        hint = QCursor.pos()
        add_public_item(
            self.settings,
            path,
            hint.x(),
            hint.y(),
            page_id=page_id,
            # Left-edge grid — Save As / copy onto desktop must not chase the cursor.
            prefer_nearest=False,
            shared=True,
        )
        return True

    def _public_drop_surface_needed(self, host: PublicIconHost | None) -> bool:
        if host is None or self._icons_hidden:
            return False
        try:
            if not host.session_owns_desktop_drops():
                return False
            from src.desktop_shell_host import is_attached_to_desktop

            hwnd = int(host.winId()) if host.winId() else 0
            return bool(hwnd) and not is_attached_to_desktop(hwnd)
        except Exception:
            return False

    def _ensure_public_icon_host(self) -> PublicIconHost:
        """Create/reuse the single shell-attached host for public floats."""
        host = getattr(self, "_public_icon_host", None)
        if host is None:
            host = PublicIconHost()
            self._public_icon_host = host
        try:
            host.sync_desktop_geometry()
        except RuntimeError:
            pass
        return host

    def _configure_public_float_overlay(
        self, icon: PublicIconWidget | None = None, *, force: bool = False
    ) -> None:
        """Attach the shared public host (never per-icon HWND when hosted)."""
        host = getattr(self, "_public_icon_host", None)
        if host is None and icon is not None and icon.is_hosted():
            parent = icon.parentWidget()
            if isinstance(parent, PublicIconHost):
                host = parent
                self._public_icon_host = host
        if host is not None:
            try:
                host.ensure_shell_attached(force=force)
            except Exception:
                from src.win_shell import configure_desktop_overlay

                configure_desktop_overlay(host)
            return
        if icon is None:
            return
        from src.win_shell import configure_desktop_overlay

        try:
            configure_desktop_overlay(icon.window() if icon.parentWidget() else icon)
        except Exception:
            pass

    def _clear_public_icons(self) -> None:
        for icon in self.public_icons:
            self._retire_overlay_widget(icon)
        self.public_icons.clear()
        for icon in list(self._parked_public_icons.values()):
            self._retire_overlay_widget(icon)
        self._parked_public_icons.clear()
        self._last_public_sync_sig = None
        host = getattr(self, "_public_icon_host", None)
        if host is not None:
            self._retire_overlay_widget(host)
            self._public_icon_host = None

    def _fence_hwnd(self, fence: QWidget) -> int:
        try:
            return int(fence.winId()) if fence.winId() else 0
        except Exception:
            return 0

    def _sync_qt_visible_after_win32(self, widget: QWidget, *, visible: bool) -> None:
        """Align ``QWidget.isVisible`` with Win32 without a second ShowWindow.

        Page-switch ``OverlayWindowBatch`` already mapped/unmapped the HWND.
        A plain ``show()``/``hide()`` here re-enters the platform window and
        flashes a solid popup (users: VPN window open/close).
        """
        try:
            if bool(widget.isVisible()) == bool(visible):
                return
        except RuntimeError:
            return
        try:
            widget.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
            if visible:
                widget.show()
            else:
                widget.hide()
        except RuntimeError:
            return
        finally:
            try:
                widget.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, False)
            except RuntimeError:
                pass
        if visible:
            try:
                widget.update()
            except RuntimeError:
                pass

    def _soft_hide_fence_for_page(self, fence: FenceWidget) -> None:
        """Page switch: SW_HIDE in the shared batch — leave geometry alone.

        Opacity-0 soft park left translucent HWNDs in DWM composition; every
        icon selection paint then flashed the whole desktop.
        """
        from src.win_shell import set_overlay_mouse_passthrough

        try:
            # Click-through while still mapped (batch hide commits later).
            set_overlay_mouse_passthrough(fence, True)
            fence._desktidy_soft_parked = True  # type: ignore[attr-defined]
            # Always restore real opacity so a prior opacity-0 bug cannot linger.
            fence.setWindowOpacity(fence._target_opacity())
        except RuntimeError:
            return
        hwnd = self._fence_hwnd(fence)
        batch = getattr(self, "_page_switch_geo_batch", None)
        if batch is not None and hwnd:
            batch.hide(hwnd)
            return
        from src.desktop_shell_host import set_overlay_hwnd_visible

        set_overlay_hwnd_visible(hwnd, False)

    def _soft_show_fence_for_page(self, fence: FenceWidget) -> None:
        from src.desktop_shell_host import set_overlay_hwnd_visible
        from src.win_shell import set_overlay_mouse_passthrough

        try:
            fence._desktidy_soft_parked = False  # type: ignore[attr-defined]
            set_overlay_mouse_passthrough(fence, False)
            try:
                fence.setWindowOpacity(fence._target_opacity())
            except (AttributeError, RuntimeError):
                pass
        except RuntimeError:
            return
        hwnd = self._fence_hwnd(fence)
        if not hwnd:
            return
        batch = getattr(self, "_page_switch_geo_batch", None)
        if batch is not None:
            batch.show(hwnd)
            return
        set_overlay_hwnd_visible(hwnd, True)

    def _prepare_page_switch_new_fence(self, fence: FenceWidget) -> int:
        """Warm a first-visit fence HWND attached but hidden for batch show.

        Creating+``show()`` mid-switch paints the new fence before leavers are
        batch-hidden (first 工作→文档 flash). ``WA_DontShowOnScreen``+``show()``
        is worse: clearing that attribute desyncs Qt/Win32 and leaves 文档页
        with no fence at all.

        Mirror soft-unpark: ``winId()`` births a hidden HWND, attach with
        ``show=False``, then ``OverlayWindowBatch.show`` with the leavers'
        hide. After commit, freeze finally syncs Qt via
        ``_sync_qt_visible_after_win32`` (same as parked restore).
        ``_desktidy_batch_reveal`` blocks ``showEvent`` from ShowWindow'ing
        during that sync.
        """
        from src.desktop_shell_host import (
            attach_overlay_to_desktop,
            set_overlay_hwnd_visible,
        )

        try:
            fence._desktidy_batch_reveal = True  # type: ignore[attr-defined]
            # Do not Qt-show() / DontShowOnScreen here — keep the HWND unmapped
            # until batch SWP_SHOWWINDOW (same invariant as soft-parked restore).
            hwnd = self._fence_hwnd(fence)
        except RuntimeError:
            return 0
        if not hwnd:
            try:
                fence._desktidy_batch_reveal = False  # type: ignore[attr-defined]
            except RuntimeError:
                pass
            return 0
        try:
            import win32gui

            from src.win_shell import (
                GWL_EXSTYLE,
                WS_EX_APPWINDOW,
                WS_EX_NOACTIVATE,
                WS_EX_TOOLWINDOW,
            )

            ex_style = int(win32gui.GetWindowLong(hwnd, GWL_EXSTYLE))
            desired = (ex_style | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) & ~WS_EX_APPWINDOW
            if ex_style != desired:
                win32gui.SetWindowLong(hwnd, GWL_EXSTYLE, desired)
        except Exception:
            pass
        try:
            attach_overlay_to_desktop(hwnd, show=False)
        except Exception:
            pass
        try:
            set_overlay_hwnd_visible(hwnd, False)
        except Exception:
            pass
        # Icon grid fills after batch.commit (freeze finally). Defer ctor
        # singleShot refresh — a mid-freeze refresh poisons ``_last_refresh_sig``
        # (文档分页: empty chrome, no icons).
        fence._desktidy_defer_refresh = True  # type: ignore[attr-defined]
        fid = str(fence.config.get("id") or "")
        if fid:
            batch_ids = getattr(self, "_page_switch_batch_fence_ids", None)
            if batch_ids is not None:
                batch_ids.add(fid)
        return int(hwnd or 0)

    def _park_public_icon(
        self, key: str, icon: PublicIconWidget, *, soft: bool = False
    ) -> None:
        """Park a float for later page reuse.

        *soft* (page switch): ``hide()`` the child under PublicIconHost — no
        top-level ShowWindow and no geometry move (mask updates are batched).
        """
        host = getattr(self, "_public_icon_host", None)
        try:
            if soft:
                try:
                    icon._desktidy_soft_parked = True  # type: ignore[attr-defined]
                except Exception:
                    pass
                try:
                    icon.hide()
                except RuntimeError:
                    return
            else:
                icon.hide()
                try:
                    icon._desktidy_soft_parked = False  # type: ignore[attr-defined]
                except Exception:
                    pass
        except RuntimeError:
            return
        # Keep parented under the shared host so page switches stay HWND-cheap.
        if host is not None:
            try:
                if icon.parentWidget() is not host:
                    icon.setParent(host)
            except RuntimeError:
                pass
        old = self._parked_public_icons.pop(key, None)
        if old is not None and old is not icon:
            self._retire_overlay_widget(old)
        self._parked_public_icons[key] = icon
        while len(self._parked_public_icons) > self._max_parked_public_icons:
            _, victim = self._pop_oldest(self._parked_public_icons)
            if victim is not icon:
                self._retire_overlay_widget(victim)
        if host is not None:
            try:
                host.schedule_mask_refresh()
            except RuntimeError:
                pass

    def _take_parked_public_icon(
        self, key: str, path: Path
    ) -> PublicIconWidget | None:
        icon = self._parked_public_icons.pop(key, None)
        if icon is None:
            # Basename only for system-namespace .lnk drift (.public_system).
            from src.public_desktop import is_system_namespace_path

            if is_system_namespace_path(path):
                name = path.name.casefold()
                for parked_key, candidate in list(self._parked_public_icons.items()):
                    try:
                        same = (
                            is_system_namespace_path(candidate.file_path)
                            and candidate.file_path.name.casefold() == name
                        )
                    except (RuntimeError, OSError):
                        self._parked_public_icons.pop(parked_key, None)
                        continue
                    if same:
                        self._parked_public_icons.pop(parked_key, None)
                        icon = candidate
                        break
        if icon is None:
            return None
        try:
            icon._desktidy_soft_parked = False  # type: ignore[attr-defined]
        except Exception:
            pass
        try:
            icon._desktidy_soft_parked = False  # type: ignore[attr-defined]
        except Exception:
            pass
        try:
            icon.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, False)
        except RuntimeError:
            pass
        # Caller owns show() via ``_overlay_widgets_may_show`` — never bypass park.
        return icon

    def _park_fence(self, fence_id: str, fence: FenceWidget, *, soft: bool = False) -> None:
        """Park a fence for later reuse.

        *soft* (page switch): batch ``SWP_HIDEWINDOW`` at the saved page rect —
        never opacity-0 (stays in DWM) and never per-fence ``SetWindowPos`` move.
        Hard park (foreign FG / teardown) still Qt-hides.
        """
        try:
            if soft:
                self._soft_hide_fence_for_page(fence)
            else:
                from src.desktop_shell_host import set_overlay_hwnd_visible

                hwnd = self._fence_hwnd(fence)
                set_overlay_hwnd_visible(hwnd, False)
                fence.hide()
                try:
                    fence._desktidy_soft_parked = False  # type: ignore[attr-defined]
                    fence.setWindowOpacity(fence._target_opacity())
                except Exception:
                    pass
        except RuntimeError:
            return
        old = self._parked_fences.pop(fence_id, None)
        if old is not None and old is not fence:
            self._retire_overlay_widget(old)
        self._parked_fences[fence_id] = fence
        while len(self._parked_fences) > self._max_parked_fences:
            _, victim = self._pop_oldest(self._parked_fences)
            if victim is not fence:
                self._retire_overlay_widget(victim)

    def _reveal_desktop_overlay(self, widget: QWidget) -> None:
        """Bring a DefView-owned overlay back on-screen without restack storms.

        Soft-parked page fences: batch ``SWP_SHOWWINDOW`` only — geometry already
        correct, never ``configure_desktop_overlay``.
        """
        try:
            widget.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, False)
        except RuntimeError:
            return
        soft = bool(getattr(widget, "_desktidy_soft_parked", False))
        if soft:
            if isinstance(widget, FenceWidget):
                self._soft_show_fence_for_page(widget)
            else:
                from src.win_shell import set_overlay_mouse_passthrough

                try:
                    widget._desktidy_soft_parked = False  # type: ignore[attr-defined]
                    set_overlay_mouse_passthrough(widget, False)
                    widget.setWindowOpacity(1.0)
                except RuntimeError:
                    return
            return
        try:
            # Heal any leftover opacity-0 soft-park from older builds.
            if isinstance(widget, FenceWidget):
                try:
                    if float(widget.windowOpacity()) < 0.99:
                        widget.setWindowOpacity(widget._target_opacity())
                except RuntimeError:
                    pass
            if not widget.isVisible():
                widget.show()
        except RuntimeError:
            return
        try:
            from src.desktop_shell_host import (
                is_attached_to_desktop,
                set_overlay_hwnd_visible,
            )
            from src.win_shell import configure_desktop_overlay, overlay_win32_visible

            hwnd = self._fence_hwnd(widget)
            if not hwnd:
                return
            batch = getattr(self, "_page_switch_geo_batch", None)
            if batch is not None and is_attached_to_desktop(hwnd):
                if not overlay_win32_visible(widget):
                    batch.show(hwnd)
                return
            if is_attached_to_desktop(hwnd):
                if not overlay_win32_visible(widget):
                    set_overlay_hwnd_visible(hwnd, True)
                return
            peek = bool(getattr(widget, "_peek_mode", False))
            configure_desktop_overlay(widget, peek=peek)
        except Exception:
            pass

    def _take_parked_fence(self, fence_id: str) -> FenceWidget | None:
        """Pop a parked fence; leave soft-park flags for ``_soft_show_fence_for_page``.

        Clearing ``_desktidy_soft_parked`` here skipped ``_reveal`` → soft-show, so
        first-boot unpark relied on a bare ``batch.show`` and often left mouse
        passthrough stuck until a later heal.
        """
        fence = self._parked_fences.pop(fence_id, None)
        if fence is None:
            return None
        try:
            if isinstance(fence, FenceWidget):
                fence.setWindowOpacity(fence._target_opacity())
        except RuntimeError:
            pass
        return fence

    def _refresh_public_host_click_mask(self) -> None:
        """Keep the public-float host click-through mask in sync (page switch)."""
        host = getattr(self, "_public_icon_host", None)
        if host is None:
            return
        try:
            # Page switch / heal must not leave an unfinished 框选 grab eating clicks.
            abort = getattr(host, "_abort_marquee", None)
            if callable(abort):
                abort()
            host.sync_desktop_geometry()
            if int(getattr(host, "_mask_batch_depth", 0) or 0) > 0:
                host._mask_refresh_deferred = True
            else:
                host.refresh_click_mask()
        except RuntimeError:
            pass

    def _clear_parked_fences(self) -> None:
        for fence in list(self._parked_fences.values()):
            self._retire_overlay_widget(fence)
        self._parked_fences.clear()

    def refresh_public_desktop(
        self, *, immediate: bool = False, relayout: bool = False
    ) -> None:
        """Sync floating icons (shared public and/or current-page local)."""
        if self._exiting:
            return
        if relayout:
            self._public_relayout_pending = True
        if immediate:
            self._run_public_refresh()
            return
        self._refresh_public_timer.start(200)

    def _run_public_refresh(self) -> None:
        started = time.perf_counter()
        if self._exiting:
            return
        # Mid-drag refresh show()/reparent of the hidden drag source leaves the
        # float HWND detached — icon "vanishes" even though settings still list it.
        if getattr(self, "_overlay_drag_active", False):
            self._refresh_public_timer.start(200)
            return
        if self._refreshing_public:
            delay = 0 if getattr(self, "_page_switch_ensure_pending", False) else 200
            self._refresh_public_timer.start(delay)
            return
        self._refreshing_public = True
        relayout = self._public_relayout_pending
        self._public_relayout_pending = False
        try:
            self._refresh_public_desktop_impl(relayout=relayout)
        finally:
            self._refreshing_public = False
            # Page switch runs immediate public refresh inside the freeze; defer
            # would leave _page_switch_ensure_pending stuck behind this timer.
            if self._public_relayout_pending and not getattr(
                self, "_page_switch_ensure_pending", False
            ):
                self._refresh_public_timer.start(200)
            self._finish_page_switch_overlays_if_pending()
            self._log_timed("refresh_public_desktop", started)

    def _refresh_public_desktop_impl(self, *, relayout: bool) -> None:
        if self._icons_hidden:
            self._clear_public_icons()
            self._last_public_sync_sig = None
            return

        may_show = bool(self._overlay_widgets_may_show())

        if self._purge_temp_desktop_refs():
            try:
                save_settings(self.settings)
            except OSError:
                pass

        # Shell icons are hidden — host unclaimed desktop files/folders as floats.
        # Skip during/just-after drag: scan + shell work on the UI thread freezes.
        quiet_until = float(getattr(self, "_shell_attach_quiet_until", 0.0))
        allow_loose = (
            not getattr(self, "_overlay_drag_active", False)
            and time.perf_counter() >= quiet_until
        )
        if allow_loose and bool(self.settings.get("hide_shell_icons")):
            try:
                from src.public_desktop import sync_loose_desktop_items

                if sync_loose_desktop_items(self.settings):
                    try:
                        save_settings(self.settings)
                    except OSError:
                        pass
            except Exception:
                pass
        if allow_loose and getattr(self, "_loose_sync_needed", False):
            if not getattr(self, "_loose_sync_inflight", False):
                self._start_loose_sync_background()

        page_id = self._current_page()
        try:
            from src.public_desktop import heal_public_area_scope

            if heal_public_area_scope(self.settings, page_id):
                try:
                    save_settings(self.settings)
                except OSError:
                    pass
        except Exception:
            pass
        # Public floats should match the current fence icon zoom so the
        # desktop icon cluster looks visually consistent.
        icon_zoom = 1.0
        fences_should_show_fn = getattr(self, "_fences_should_show", None)
        if callable(fences_should_show_fn) and fences_should_show_fn():
            try:
                icon_zoom = max(
                    float(getattr(f, "_icon_zoom", 1.0))
                    for f in (getattr(self, "fences", None) or [])
                    if f
                )
            except Exception:
                icon_zoom = 1.0
        from src.ui.fence_widget import FenceWidget

        public_icon_size = int(round(FenceWidget.ICON_BASE_SIZE * float(icon_zoom)))
        public_icon_size = max(32, min(128, public_icon_size))
        page_switch = bool(getattr(self, "_page_switch_ensure_pending", False))
        # Page click does not create/delete files — skip Path.exists on every pin.
        changed = False
        if not page_switch:
            changed = prune_missing_public_items(self.settings)
            try:
                from src.fence_rules import prune_missing_virtual_items

                if prune_missing_virtual_items(self.settings):
                    changed = True
            except Exception:
                pass
            if changed:
                self._public_force_relayout = True
                try:
                    save_settings(self.settings)
                except OSError:
                    pass
        if relayout:
            fence_rects = live_fence_rects(self.settings, page_id)
            fence_key = tuple(
                (r.x(), r.y(), r.width(), r.height()) for r in fence_rects
            )
            needs_relayout = (
                self._public_force_relayout
                or fence_key != self._last_public_fence_key
            )
            if visible_floating_items(self.settings, page_id) and needs_relayout:
                if relayout_public_items(
                    self.settings, page_id=page_id, fence_rects=fence_rects
                ):
                    changed = True
                self._last_public_fence_key = fence_key
                self._public_force_relayout = False
            if changed:
                try:
                    save_settings(self.settings)
                except OSError:
                    pass

        entries = visible_floating_items(self.settings, page_id)
        wanted: dict[str, dict] = {}
        sync_items: list[tuple[str, int, int]] = []
        for entry in entries:
            raw_path = str(entry["path"])
            try:
                key = raw_path.casefold()
            except OSError:
                key = raw_path
            x = int(entry.get("x", 80))
            y = int(entry.get("y", 80))
            wanted[key] = entry
            sync_items.append((key, x, y))
        sync_sig = tuple(sync_items)
        host = None if self._icons_hidden else self._ensure_public_icon_host()
        host_ok = True
        if host is not None:
            try:
                from src.win_shell import overlay_win32_visible

                host_ok = overlay_win32_visible(host)
            except Exception:
                host_ok = bool(host.isVisible())
        have_keys = set()
        for icon in list(self.public_icons):
            try:
                have_keys.add(str(icon.file_path).casefold())
            except OSError:
                continue
        wanted_keys = set(wanted.keys())
        if (
            not page_switch
            and not relayout
            and not self._public_drop_surface_needed(host)
            and sync_sig == getattr(self, "_last_public_sync_sig", None)
            and have_keys == wanted_keys
            and all(icon.isVisible() for icon in self.public_icons)
            and all(icon.is_hosted() for icon in self.public_icons)
            and host_ok
        ):
            return

        # Basename match only for system-namespace floats (This PC / Recycle),
        # where the .lnk path under .public_system can drift while the name stays.
        from src.public_desktop import is_system_namespace_path

        wanted_ns_names: dict[str, str] = {}
        for wkey, entry in wanted.items():
            try:
                wp = Path(str(entry["path"]))
            except OSError:
                continue
            if is_system_namespace_path(wp):
                wanted_ns_names[wp.name.casefold()] = wkey
        drag_key = getattr(self, "_public_drag_path", None)
        drag_keys = getattr(self, "_public_drag_keys", None) or set()
        if drag_key and drag_key not in drag_keys:
            drag_keys = set(drag_keys) | {drag_key}
        existing: dict[str, PublicIconWidget] = {}
        protect_drag: list[PublicIconWidget] = []
        for icon in list(self.public_icons):
            try:
                key = str(icon.file_path).casefold()
            except OSError:
                key = str(icon.file_path).casefold()
            matched_key = key if key in wanted else None
            if matched_key is None:
                try:
                    if is_system_namespace_path(icon.file_path):
                        matched_key = wanted_ns_names.get(
                            icon.file_path.name.casefold()
                        )
                except (RuntimeError, OSError):
                    matched_key = None
            if matched_key is None:
                # Never park/drop floats currently mid-drag (often still hidden).
                if key in drag_keys:
                    protect_drag.append(icon)
                    continue
                # In-place rename uses a Tool popup owned by this float — parking
                # the widget mid-edit destroys typing (新建文件夹重命名打不进去).
                if getattr(icon, "_rename_edit", None) is not None:
                    protect_drag.append(icon)
                    continue
                self.public_icons.remove(icon)
                # Park off-page floats — recreating shell icons is the page-switch hitch.
                page_switch = bool(
                    getattr(self, "_page_switch_ensure_pending", False)
                )
                self._park_public_icon(key, icon, soft=page_switch)
                continue
            existing[matched_key] = icon

        next_icons: list[PublicIconWidget] = []
        structure_changed = False
        created_new = 0
        restored_from_park = 0
        new_icon_stagger = 0
        page_switch = bool(getattr(self, "_page_switch_ensure_pending", False))
        used: set[int] = set()
        host = self._ensure_public_icon_host()
        for key, entry in wanted.items():
            path = Path(str(entry["path"]))
            x = int(entry.get("x", 80))
            y = int(entry.get("y", 80))
            icon = existing.get(key)
            if icon is not None and id(icon) in used:
                icon = None
            from_park = False
            if icon is None:
                icon = self._take_parked_public_icon(key, path)
                from_park = icon is not None
            if icon is not None:
                used.add(id(icon))
                if from_park:
                    restored_from_park += 1
                try:
                    if icon.parentWidget() is not host:
                        icon.setParent(host)
                except RuntimeError:
                    pass
                try:
                    sp = icon.screen_pos()
                    need_move = sp.x() != x or sp.y() != y
                except RuntimeError:
                    need_move = True
                if need_move:
                    icon.move_to_screen(x, y)
                    structure_changed = True
                # Keep mid-drag source hidden; otherwise restore visibility.
                if (
                    may_show
                    and not icon.isVisible()
                    and not (
                        key in drag_keys
                        or (
                            drag_key
                            and str(icon.file_path).casefold() == drag_key
                        )
                    )
                ):
                    icon.show()
                    structure_changed = True
                next_icons.append(icon)
                continue
            icon_delay = 0
            if not page_switch and new_icon_stagger >= 5:
                icon_delay = (new_icon_stagger - 4) * 14
            icon = PublicIconWidget(
                path,
                x,
                y,
                parent=host,
                icon_load_delay_ms=icon_delay,
                icon_size=public_icon_size,
            )
            new_icon_stagger += 1
            icon.moved.connect(self._on_public_icon_moved)
            icon.pinned_to_fence.connect(self._on_public_icon_pinned)
            icon.removed.connect(self._on_public_icon_removed)
            if may_show:
                icon.show()
            next_icons.append(icon)
            structure_changed = True
            created_new += 1
            # Shell「新建文件夹」: Explorer starts rename on a hidden ListView while
            # our float shows the default name — keystrokes never arrive. Arm our
            # focusable Tool popup once the float is on screen.
            try:
                from src.ui.fence_icon_item import (
                    looks_like_shell_new_item,
                    maybe_begin_rename_for_shell_new_item,
                )

                if looks_like_shell_new_item(path):
                    QTimer.singleShot(
                        120,
                        lambda ic=icon: maybe_begin_rename_for_shell_new_item(ic),
                    )
            except Exception:
                pass

        page_switch_park = bool(
            getattr(self, "_page_switch_ensure_pending", False)
        )
        for key, icon in existing.items():
            if id(icon) in used:
                continue
            self._park_public_icon(key, icon, soft=page_switch_park)
            structure_changed = True

        for icon in protect_drag:
            if icon not in next_icons:
                next_icons.append(icon)
                structure_changed = True

        self.public_icons = next_icons
        self._last_public_sync_sig = sync_sig
        try:
            if page_switch:
                # One setMask after the batch — never present()/attach per icon move.
                host.present(force_attach=False, mask_only=True)
            else:
                host.present(force_attach=False)
                # present()/mask refresh can restack the host above fences; layered
                # alpha then eats fence-icon clicks. Re-assert fence Z-order + opacity.
                self.ensure_live_fences_interactive()
        except RuntimeError:
            pass
        # New floats and unparked widgets need the shared host re-attached.
        # Soft page switch to a no-fence page used to only arm keepalive — by then the
        # earlier ensure had already exited (no fences/icons yet), so floats
        # stayed under the wallpaper after hotkey page changes.
        if created_new > 0 or restored_from_park > 0:
            # Page A↔B: host.present() already bare-ShowWindow'd when attached.
            # Soft ``ensure_shell_attachments`` restacks every fence and flashes.
            if not page_switch:
                # Soft attach only — force=True restacks every fence/float and flashes
                # the desktop when dragging one item out of a fence.
                QTimer.singleShot(0, lambda: self._ensure_shell_attachments(force=False))
                quiet_until = float(getattr(self, "_shell_attach_quiet_until", 0.0))
                delay_ms = max(80, int((quiet_until - time.perf_counter()) * 1000) + 80)
                QTimer.singleShot(delay_ms, self._ensure_new_public_floats_visible)
        elif structure_changed and not page_switch:
            self._schedule_overlay_keepalive()
            self._configure_public_float_overlay(force=False)

    def _ensure_single_public_float(self, path: Path) -> bool:
        """Create/show one public float after fence unpin — no full public rebuild."""
        if self._exiting or self._icons_hidden:
            return False
        if not self._overlay_widgets_may_show():
            return False
        from src.public_desktop import find_public_entry

        entry = find_public_entry(self.settings, path)
        if entry is None:
            return False
        icon_zoom = 1.0
        fences_should_show_fn = getattr(self, "_fences_should_show", None)
        if callable(fences_should_show_fn) and fences_should_show_fn():
            try:
                icon_zoom = max(
                    float(getattr(f, "_icon_zoom", 1.0))
                    for f in (getattr(self, "fences", None) or [])
                    if f
                )
            except Exception:
                icon_zoom = 1.0
        from src.ui.fence_widget import FenceWidget

        public_icon_size = int(round(FenceWidget.ICON_BASE_SIZE * float(icon_zoom)))
        public_icon_size = max(32, min(128, public_icon_size))
        try:
            key = str(entry.get("path") or path).casefold()
        except OSError:
            key = str(path).casefold()
        x = int(entry.get("x", 80))
        y = int(entry.get("y", 80))
        host = self._ensure_public_icon_host()
        for icon in list(self.public_icons):
            try:
                same = str(icon.file_path).casefold() == key
            except OSError:
                same = False
            if same:
                try:
                    if icon.parentWidget() is not host:
                        icon.setParent(host)
                    sp = icon.screen_pos()
                    if sp.x() != x or sp.y() != y:
                        icon.move_to_screen(x, y)
                    if not icon.isVisible():
                        icon.show()
                    self._configure_public_float_overlay(icon)
                except Exception:
                    pass
                return True

        parked = self._take_parked_public_icon(key, Path(path))
        if parked is not None:
            icon = parked
            try:
                if icon.parentWidget() is not host:
                    icon.setParent(host)
                icon.move_to_screen(x, y)
                icon.show()
            except RuntimeError:
                return False
        else:
            icon = PublicIconWidget(
                Path(str(entry.get("path") or path)),
                x,
                y,
                parent=host,
                icon_size=public_icon_size,
            )
            icon.moved.connect(self._on_public_icon_moved)
            icon.pinned_to_fence.connect(self._on_public_icon_pinned)
            icon.removed.connect(self._on_public_icon_removed)
            icon.show()
        self.public_icons.append(icon)
        # Present remasks the shared host — re-place/show EVERY live float after
        # that, then stamp sync_sig. Stamping before sibling reconcile let the
        # deferred post-drag refresh early-return while float #1 stayed blank
        # until a later drag forced a real rebuild.
        try:
            host.present(force_attach=False)
        except Exception:
            pass
        sync_items = self._reconcile_live_public_floats(host)
        if sync_items is not None:
            self._last_public_sync_sig = tuple(sync_items)
        # Same invariant as full refresh: present()/mask can restack the host
        # above fences so the next fence icon paints but cannot receive drag.
        try:
            self.ensure_live_fences_interactive()
        except Exception:
            pass
        try:
            self._configure_public_float_overlay(icon)
        except Exception:
            pass
        # Soft keepalive only — never force-restack the whole desktop for one float.
        self._schedule_overlay_keepalive()
        return True

    def _reconcile_live_public_floats(self, host) -> list[tuple[str, int, int]] | None:
        """Re-parent/place/show every current-page float under ``host``.

        Returns the sync-signature tuples when every wanted path has a live
        widget; otherwise ``None`` (caller must not pretend sync is complete).
        """
        try:
            page_id = self._current_page()
            wanted = list(visible_floating_items(self.settings, page_id))
        except Exception:
            return None
        by_key: dict[str, object] = {}
        for icon in list(self.public_icons):
            try:
                by_key[str(icon.file_path).casefold()] = icon
            except OSError:
                continue
        sync_items: list[tuple[str, int, int]] = []
        complete = True
        for ent in wanted:
            raw = str(ent.get("path") or "")
            if not raw:
                continue
            try:
                key = raw.casefold()
            except OSError:
                key = raw
            x = int(ent.get("x", 80))
            y = int(ent.get("y", 80))
            sync_items.append((key, x, y))
            icon = by_key.get(key)
            if icon is None:
                complete = False
                continue
            try:
                if icon.parentWidget() is not host:
                    icon.setParent(host)
                if bool(getattr(icon, "_desktidy_soft_parked", False)):
                    icon._desktidy_soft_parked = False  # type: ignore[attr-defined]
                # Always re-apply screen coords — host remask can leave siblings
                # Qt-visible but unpainted / wrong local geometry.
                icon.move_to_screen(x, y)
                if not icon.isVisible():
                    icon.show()
            except Exception:
                complete = False
        return sync_items if complete else None

    def _ensure_new_public_floats_visible(self) -> None:
        """Show + re-attach public floats created during/just after a drag."""
        if self._exiting or getattr(self, "_overlay_drag_active", False):
            return
        if self._icons_hidden:
            return
        if not self._overlay_widgets_may_show():
            return
        for icon in list(self.public_icons):
            try:
                if not icon.isVisible():
                    icon.show()
            except RuntimeError:
                continue
        self._configure_public_float_overlay(force=False)
        # Soft repair only — force restack of every fence flashes the desktop.
        self._ensure_desktop_overlays_visible(force=False)

    def _on_fence_public_items_changed(self, path=None) -> None:
        """Fence unpin added a public float — soft-add near drop, no desktop flash."""
        if self._exiting:
            return
        self._public_drag_settling = True
        try:
            if path is not None:
                try:
                    ok = self._ensure_single_public_float(Path(path))
                except Exception:
                    ok = False
                if ok:
                    self._remap_hidden_overlay_hwnds()
                    return
            # Fallback: soft sync without clearing the whole signature first.
            self.refresh_public_desktop(relayout=False)
        finally:
            self._public_drag_settling = False

    def _restore_public_icon_widget(
        self, path: Path, *, icon: PublicIconWidget | None = None
    ) -> None:
        """Show + re-attach a float after drag hide (settings may still list it)."""
        if not self._overlay_widgets_may_show():
            return
        target_icon = icon
        try:
            target = str(path).casefold()
        except OSError:
            target = str(path).casefold()
        if target_icon is None:
            for candidate in self.public_icons:
                try:
                    same = str(candidate.file_path).casefold() == target
                except OSError:
                    same = candidate.file_path == Path(path)
                if same:
                    target_icon = candidate
                    break
        if target_icon is None:
            # May have been parked by a page sync that raced the drag hide.
            parked = self._take_parked_public_icon(target, Path(path))
            if parked is not None:
                target_icon = parked
                if parked not in self.public_icons:
                    self.public_icons.append(parked)
        if target_icon is None:
            self._last_public_sync_sig = None
            # Never immediate here — may run right after QDrag.exec and deadlock.
            self.refresh_public_desktop()
            return
        try:
            host = self._ensure_public_icon_host()
            if target_icon.parentWidget() is not host:
                target_icon.setParent(host)
            if not target_icon.isVisible():
                target_icon.show()
            host.present(force_attach=False)
            self._configure_public_float_overlay(target_icon)
        except RuntimeError:
            self._last_public_sync_sig = None
            self.refresh_public_desktop()

    def _on_public_icon_moved(self, path: Path, x: int, y: int) -> None:
        page_id = self._current_page()
        snapped = snap_public_item(
            self.settings,
            path,
            x,
            y,
            page_id=page_id,
            fence_rects=live_fence_rects(self.settings, page_id),
        )
        if snapped is None:
            # Still ensure the drag-hidden widget is visible again.
            self._restore_public_icon_widget(path)
            return
        try:
            save_settings(self.settings)
        except OSError:
            pass
        # Apply snapped coords without a full relayout of every icon.
        sx, sy = snapped
        try:
            target = str(path).casefold()
        except OSError:
            target = str(path).casefold()
        for icon in self.public_icons:
            try:
                same = str(icon.file_path).casefold() == target
            except OSError:
                same = icon.file_path == Path(path)
            if same:
                icon.move_to_screen(sx, sy)
                break
        self._restore_public_icon_widget(path)

    def move_desktop_item_to_page(self, path: Path | str, page_id: int) -> bool:
        """RMB「移动到分页」: unpin fences and place as a float on ``page_id``."""
        return self.move_desktop_items_to_page([path], page_id)

    def move_desktop_items_to_page(
        self, paths: list[Path | str], page_id: int
    ) -> bool:
        """RMB「移动到分页」for a multi-selection (Explorer-like).

        With「启用公共区域」, public floats are excluded (stay shared); fence
        icons can still be moved to a page-local float.
        """
        from src.public_desktop import (
            find_public_entry,
            is_public_desktop_enabled,
            move_paths_to_desktop_page,
        )

        if self._exiting:
            return False
        batch: list[Path] = []
        for raw in paths or []:
            try:
                batch.append(Path(raw))
            except OSError:
                continue
        if not batch:
            return False
        if is_public_desktop_enabled(self.settings):
            kept = [p for p in batch if find_public_entry(self.settings, p) is None]
            if len(kept) < len(batch):
                get_logger().info(
                    "move_to_page: skip public floats count=%s (public desktop on)",
                    len(batch) - len(kept),
                )
            batch = kept
            if not batch:
                return False
        try:
            page_id = int(page_id)
        except (TypeError, ValueError):
            return False
        page_ids = {int(p.get("id", 0)) for p in self._get_pages()}
        if page_id not in page_ids:
            return False
        hint = None
        try:
            from PyQt6.QtGui import QCursor

            pos = QCursor.pos()
            hint = (int(pos.x()), int(pos.y()))
        except Exception:
            hint = None
        changed = move_paths_to_desktop_page(
            self.settings,
            batch,
            page_id,
            hint_x=hint[0] if hint else None,
            hint_y=hint[1] if hint else None,
        )
        if not changed:
            get_logger().info(
                "move_to_page: no change count=%s page_id=%s", len(batch), page_id
            )
            return False
        try:
            save_settings(self.settings)
        except OSError:
            pass
        get_logger().info(
            "move_to_page: ok count=%s page_id=%s", len(batch), page_id
        )
        self.window.fence_editor.reload_table()
        self.window.desktop_layout.reload_table()
        self._reveal_organize_float_pages([page_id])
        self.refresh_public_desktop(relayout=True, immediate=True)
        if self._fences_should_show():
            self.show_fences()
        return True

    def move_desktop_item_to_fence(self, path: Path | str, fence_id: str) -> bool:
        """RMB「移动到分区」: pin into another fence on the current page."""
        return self.move_desktop_items_to_fence([path], fence_id)

    def move_desktop_items_to_fence(
        self, paths: list[Path | str], fence_id: str
    ) -> bool:
        """RMB「移动到分区」for a multi-selection — all selected icons move together.

        With「启用公共区域」, public floats are excluded (stay public); drag-into-
        fence still pins via ``pin_public_paths_to_fence``.
        """
        from src.public_desktop import (
            find_public_entry,
            is_public_desktop_enabled,
            move_paths_to_fence,
        )

        if self._exiting:
            return False
        batch: list[Path] = []
        for raw in paths or []:
            try:
                batch.append(Path(raw))
            except OSError:
                continue
        if not batch:
            return False
        if is_public_desktop_enabled(self.settings):
            kept = [p for p in batch if find_public_entry(self.settings, p) is None]
            if len(kept) < len(batch):
                get_logger().info(
                    "move_to_fence: skip public floats count=%s (public desktop on)",
                    len(batch) - len(kept),
                )
            batch = kept
            if not batch:
                return False
        fence_id = str(fence_id or "").strip()
        if not fence_id:
            return False
        changed = move_paths_to_fence(self.settings, batch, fence_id)
        if not changed:
            return False
        try:
            save_settings(self.settings)
        except OSError:
            pass
        get_logger().info(
            "move_to_fence: ok count=%s fence=%s", len(batch), fence_id
        )
        self.window.fence_editor.reload_table()
        if self._fences_should_show():
            self.show_fences()
        else:
            self.refresh_public_desktop(relayout=False)
        return True

    def pin_public_path_to_fence(self, path: Path, fence) -> bool:
        """Atomically pin a public/loose float into a fence and paint it."""
        # Call via class so Fake selftest hosts without the batch method still work.
        return DeskTidyApp.pin_public_paths_to_fence(self, [path], fence)

    def pin_public_paths_to_fence(self, paths: list[Path], fence) -> bool:
        """Atomically pin public/loose floats into a fence and paint them.

        Success = each path is listed in the fence's ``virtual_items``. Public
        floats are removed only after that. On failure, any partial pin is rolled
        back and public entries are restored so icons cannot vanish.

        Multi-drag must assign in one write — looping single pins used to
        refresh/rebuild after the first path and leave peers half-applied.
        """
        from src.fence_rules import (
            assign_paths_to_virtual_fence,
            set_virtual_item_order,
            unpin_paths_from_virtual_fence,
        )
        from src.public_desktop import add_public_item, find_public_entry, remove_public_paths
        from src.ui.fence_icon_item import path_pinned_in_settings

        if fence is None:
            return False
        batch: list[Path] = []
        for raw in paths or []:
            if raw is None:
                continue
            try:
                batch.append(Path(raw))
            except OSError:
                continue
        if not batch:
            return False
        self._public_drag_settling = True
        had_public = {
            str(p).casefold(): find_public_entry(self.settings, p) is not None
            for p in batch
        }
        fid = None
        try:
            cfg = getattr(fence, "config", None)
            if not isinstance(cfg, dict):
                return False
            fid = cfg.get("id")
            # Seed manual order from the FULL pin list in settings — never from
            # currently painted widgets (partial grid would wipe other pins).
            stored = None
            for item in self.settings.get("fences") or []:
                if isinstance(item, dict) and fid and item.get("id") == fid:
                    stored = item
                    break
            seed_src = stored if stored is not None else cfg
            if cfg.get("sort_by") != "manual" or (
                stored is not None
                and len(list(cfg.get("virtual_items") or []))
                < len(list(stored.get("virtual_items") or []))
            ):
                seed = [Path(str(p)) for p in (seed_src.get("virtual_items") or [])]
                set_virtual_item_order(cfg, seed)
                if stored is not None and stored is not cfg:
                    stored["virtual_items"] = list(cfg.get("virtual_items") or [])
                    stored["sort_by"] = "manual"

            added = assign_paths_to_virtual_fence(cfg, self.settings, batch)
            cfg["sort_by"] = "manual"
            try:
                fence.sort_by = "manual"
            except Exception:
                pass
            # Rebind live widget to settings-owned dict.
            if stored is not None:
                fence.config = stored
                cfg = stored
            else:
                for item in self.settings.get("fences") or []:
                    if isinstance(item, dict) and fid and item.get("id") == fid:
                        fence.config = item
                        cfg = item
                        break

            missing = [p for p in batch if not path_pinned_in_settings(self.settings, p)]
            if missing:
                get_logger().warning(
                    "pin_public: settings missing pins count=%s added=%s missing=%s",
                    len(batch),
                    len(added),
                    [p.name for p in missing],
                )
                for path in missing:
                    if had_public.get(str(path).casefold()) and find_public_entry(
                        self.settings, path
                    ) is None:
                        add_public_item(
                            self.settings,
                            path,
                            page_id=self._current_page(),
                            fence_rects=live_fence_rects(
                                self.settings, self._current_page()
                            ),
                            auto_arrange=True,
                        )
                if len(missing) == len(batch):
                    return False

            try:
                save_settings(self.settings)
            except OSError:
                pass
            invalidate_desktop_scan_cache()
            self._last_public_sync_sig = None
            fence._last_refresh_sig = None
            try:
                fence._refresh_debounce.stop()
                fence._refresh_impl()
            except Exception:
                try:
                    fence.refresh()
                except Exception:
                    pass
            # assign_paths already cleared public; scrub live widgets once.
            remove_public_paths(self.settings, batch)
            try:
                save_settings(self.settings)
            except OSError:
                pass
            for path in batch:
                if path_pinned_in_settings(self.settings, path):
                    self._remove_public_icon_widget(path)
            QTimer.singleShot(80, self.refresh_public_desktop)
            get_logger().info(
                "pin_public: ok fence=%s count=%s", fid, len(batch) - len(missing)
            )
            return len(missing) < len(batch)
        except Exception:
            get_logger().exception("pin_public: failed count=%s", len(batch))
            try:
                cfg = getattr(fence, "config", None)
                if isinstance(cfg, dict):
                    unpin_paths_from_virtual_fence(cfg, batch)
                for item in self.settings.get("fences") or []:
                    if isinstance(item, dict) and fid and item.get("id") == fid:
                        unpin_paths_from_virtual_fence(item, batch)
                        break
                for path in batch:
                    if find_public_entry(self.settings, path) is None:
                        add_public_item(
                            self.settings,
                            path,
                            page_id=self._current_page(),
                            fence_rects=live_fence_rects(
                                self.settings, self._current_page()
                            ),
                            auto_arrange=True,
                        )
            except Exception:
                pass
            return False
        finally:
            self._public_drag_settling = False

    def _on_public_icon_pinned(self, path: Path) -> None:
        from src.fence_rules import _norm_virtual_key
        from src.ui.fence_icon_item import destroy_unpin_catchers, path_pinned_in_settings

        destroy_unpin_catchers()

        def _pin_key(raw: Path | str) -> str:
            try:
                return _norm_virtual_key(Path(raw)).casefold().replace("/", "\\")
            except OSError:
                return str(raw).casefold().replace("/", "\\")

        # Also trust live fence widgets — settings deepcopy races can briefly
        # look "unpinned" right after a public→fence drop. Exact keys only
        # (same as path_pinned_in_settings) — never basename.
        pinned = path_pinned_in_settings(self.settings, path)
        if not pinned:
            key = _pin_key(path)
            for fence in list(self.fences):
                for raw in ((fence.config or {}).get("virtual_items") or []):
                    if _pin_key(str(raw)) == key:
                        pinned = True
                        break
                if pinned:
                    break
        # If the drop reported success but settings have no pin, restore the float.
        if not pinned:
            get_logger().warning(
                "public→fence pin missing after drop; restoring float path=%s", path
            )
            page_id = self._current_page()
            add_public_item(
                self.settings,
                path,
                page_id=page_id,
                fence_rects=live_fence_rects(self.settings, page_id),
                auto_arrange=True,
            )
            try:
                save_settings(self.settings)
            except OSError:
                pass
            self.refresh_public_desktop(relayout=True)
            return

        remove_public_paths(self.settings, [path])
        try:
            save_settings(self.settings)
        except OSError:
            pass
        invalidate_desktop_scan_cache()
        self._last_public_sync_sig = None
        self._remove_public_icon_widget(path)
        key = _pin_key(path)
        for fence in list(self.fences):
            fid = fence.config.get("id") if isinstance(fence.config, dict) else None
            stored = None
            for cfg in self.settings.get("fences") or []:
                if isinstance(cfg, dict) and fid and cfg.get("id") == fid:
                    stored = cfg
                    break
            if stored is not None:
                fence.config = stored
            pins = {
                _pin_key(str(x))
                for x in ((fence.config or {}).get("virtual_items") or [])
            }
            if key not in pins:
                continue
            # Soft rebuild only — force=True wipes the whole icon cache and
            # re-extracts every shell icon on the UI thread (multi-second hitch).
            fence._last_refresh_sig = None
            try:
                fence._refresh_debounce.stop()
                fence._refresh_impl()
            except Exception:
                try:
                    fence.refresh()
                except Exception:
                    pass
        # Defer public scrub so shell attach stays outside the pin critical path.
        QTimer.singleShot(80, self.refresh_public_desktop)

    def _remove_public_icon_widget(self, path: Path) -> None:
        """Remove one public icon widget immediately, without full relayout."""
        from src.public_desktop import is_system_namespace_path

        try:
            target = str(path).casefold()
        except OSError:
            target = str(path).casefold()
        keep: list[PublicIconWidget] = []
        for icon in self.public_icons:
            try:
                same = str(icon.file_path).casefold() == target
            except OSError:
                same = icon.file_path == Path(path)
            if same:
                self._retire_overlay_widget(icon)
                continue
            keep.append(icon)
        self.public_icons = keep
        parked = self._parked_public_icons.pop(target, None)
        if parked is not None:
            self._retire_overlay_widget(parked)
        # Basename parked cleanup only for system-namespace .lnk drift.
        if is_system_namespace_path(path):
            name = Path(path).name.casefold()
            for parked_key, icon in list(self._parked_public_icons.items()):
                try:
                    same = (
                        is_system_namespace_path(icon.file_path)
                        and icon.file_path.name.casefold() == name
                    )
                except (RuntimeError, OSError):
                    same = False
                if same or parked_key == target:
                    self._parked_public_icons.pop(parked_key, None)
                    self._retire_overlay_widget(icon)
        self._last_public_sync_sig = None
        host = getattr(self, "_public_icon_host", None)
        if host is not None:
            try:
                host.schedule_mask_refresh()
            except RuntimeError:
                pass

    def _on_public_icon_removed(self, path: Path) -> None:
        remove_public_paths(self.settings, [path])
        try:
            save_settings(self.settings)
        except OSError:
            pass
        # Drop the one float — full public relayout flashes the desktop.
        self._remove_public_icon_widget(path)

    def _repair_overlay_parents(self) -> None:
        """Re-attach overlays to the desktop shell host after show/rebuild."""
        self._ensure_shell_attachments(force=True)

    def _setup_dock(self) -> None:
        if self.dock:
            self.dock.close()
            self.dock.deleteLater()
            self.dock = None
        # Dock UI was removed from 扩展功能; keep any leftover bar hidden.
        dock_cfg = self.settings.setdefault("dock", {})
        if isinstance(dock_cfg, dict) and dock_cfg.get("enabled"):
            dock_cfg["enabled"] = False
            try:
                save_settings(self.settings)
            except OSError:
                pass

    def _setup_todo_panel(self) -> None:
        """Show / hide the desktop sticky todo panel."""
        from src.todos import desktop_todos_enabled

        existing = getattr(self, "todo_panel", None)
        if existing is not None:
            try:
                existing.close()
                existing.deleteLater()
            except RuntimeError:
                pass
            self.todo_panel = None
        if not desktop_todos_enabled(self.settings):
            return
        if self._icons_hidden or self._exiting:
            return
        from src.ui.todo_widget import DesktopTodoWidget

        panel = DesktopTodoWidget(self.settings)
        self.todo_panel = panel
        if self._page_chrome_may_show():
            panel.show()
            self._ensure_page_chrome_visible()

    def _on_todo_requested(self) -> None:
        """Page-bar「待办」: show / raise the sticky panel."""
        from src.todos import desktop_todos_enabled

        if not desktop_todos_enabled(self.settings):
            return
        panel = getattr(self, "todo_panel", None)
        if panel is None:
            self._setup_todo_panel()
            panel = getattr(self, "todo_panel", None)
        if panel is None:
            return
        try:
            if not panel.isVisible():
                panel.show()
            panel.raise_()
            panel.reload()
            self._ensure_page_chrome_visible(raise_band=True)
        except RuntimeError:
            pass

    def _setup_account_vault_panel(self) -> None:
        """Create vault panel + Doubao-style launcher when enabled."""
        from src.account_vault import account_vault_enabled

        enabled = account_vault_enabled(self.settings)
        if not enabled or self._icons_hidden or self._exiting:
            for attr in ("vault_panel", "vault_launcher"):
                existing = getattr(self, attr, None)
                if existing is not None:
                    try:
                        existing.close()
                        existing.deleteLater()
                    except RuntimeError:
                        pass
                    setattr(self, attr, None)
            return

        panel = getattr(self, "vault_panel", None)
        launcher = getattr(self, "vault_launcher", None)
        if panel is not None and launcher is not None:
            try:
                apply = getattr(panel, "apply_settings", None)
                if callable(apply):
                    apply(self.settings)
                else:
                    panel.settings = self.settings
                    panel._apply_theme()
                launcher.settings = self.settings
                launcher.refresh_theme()
                set_anchor = getattr(panel, "set_anchor_widget", None)
                if callable(set_anchor):
                    set_anchor(launcher)
                if not launcher.isVisible():
                    launcher.show()
                    launcher.raise_()
                return
            except RuntimeError:
                pass

        for attr in ("vault_panel", "vault_launcher"):
            existing = getattr(self, attr, None)
            if existing is not None:
                try:
                    existing.close()
                    existing.deleteLater()
                except RuntimeError:
                    pass
                setattr(self, attr, None)

        from src.ui.account_vault_launcher import AccountVaultLauncher
        from src.ui.account_vault_widget import AccountVaultWidget

        self.vault_panel = AccountVaultWidget(self.settings)
        self.vault_launcher = AccountVaultLauncher(self.settings)
        self.vault_panel.set_anchor_widget(self.vault_launcher)
        self.vault_launcher.activated.connect(self._on_vault_launcher_activated)
        self.vault_panel.server_unreachable.connect(self._on_vault_server_unreachable)
        try:
            self.vault_launcher.show()
            self.vault_launcher.raise_()
        except RuntimeError:
            pass
        # Background reachability gate — never block the UI thread.
        QTimer.singleShot(0, self._probe_account_vault_server)

    def _probe_account_vault_server(self) -> None:
        """If vault is enabled but the API host is down, tip + auto-disable."""
        from src.account_vault import account_vault_api_base, account_vault_enabled

        if not account_vault_enabled(self.settings):
            return
        if getattr(self, "_vault_probe_busy", False):
            return
        if self._icons_hidden or self._exiting:
            return
        self._vault_probe_busy = True
        base = account_vault_api_base(self.settings)
        from src.ui.account_vault_async import VaultApiJob

        def work() -> bool:
            from src.account_vault_api import probe_vault_reachable

            ok, msg = probe_vault_reachable(base, timeout=8.0)
            if not ok:
                raise RuntimeError(msg or "无法访问账号服务器")
            return True

        # Parent to self (the app, process lifetime) — NOT to self.vault_panel.
        # _on_vault_server_unreachable destroys the panel, which would kill
        # this QThread mid-run ("QThread: Destroyed while thread is still running").
        job = VaultApiJob(work, self)

        def on_ok(_result: object) -> None:
            self._vault_probe_busy = False

        def on_err(exc: object) -> None:
            self._vault_probe_busy = False
            msg = ""
            if isinstance(exc, BaseException):
                msg = str(exc)
            elif exc is not None:
                msg = str(exc)
            self._on_vault_server_unreachable(msg)

        job.succeeded.connect(on_ok)
        job.failed.connect(on_err)
        job.finished.connect(job.deleteLater)
        self._vault_probe_job = job
        job.start()

    def _on_vault_server_unreachable(self, message: str = "") -> None:
        """Tip the user and turn vault off so a dead host cannot block the app."""
        from src.account_vault import account_vault_enabled, account_vault_settings
        from src.settings import save_settings
        from src.ui.toast import show_toast

        if not account_vault_enabled(self.settings):
            return
        cfg = account_vault_settings(self.settings)
        cfg["enabled"] = False
        try:
            save_settings(self.settings, immediate=True)
        except OSError:
            pass
        tip = (message or "").strip() or "无法访问账号服务器"
        show_toast("账号服务器无法访问，已关闭账号管理", tip, level="warn")
        self._setup_account_vault_panel()
        window = getattr(self, "window", None)
        vault_page = getattr(window, "_vault_settings_widget", None) if window else None
        if vault_page is not None:
            try:
                cb = getattr(vault_page, "enabled_cb", None)
                if cb is not None:
                    cb.blockSignals(True)
                    cb.setChecked(False)
                    cb.blockSignals(False)
                status = getattr(vault_page, "status_lbl", None)
                if status is not None:
                    status.setText(tip)
            except RuntimeError:
                pass

    def _on_vault_launcher_activated(self) -> None:
        """Doubao-style float click: toggle vault panel."""
        self._on_vault_requested(toggle=True)

    def _on_vault_requested(self, *, toggle: bool = False) -> None:
        """Show / raise (or toggle) the vault panel from the desktop launcher."""
        from src.account_vault import account_vault_enabled

        if not account_vault_enabled(self.settings):
            return
        panel = getattr(self, "vault_panel", None)
        if panel is None:
            self._setup_account_vault_panel()
            panel = getattr(self, "vault_panel", None)
        if panel is None:
            return
        try:
            if toggle and panel.isVisible():
                panel.hide()
                return
            panel.raise_panel()
            self._ensure_page_chrome_visible(raise_band=True)
        except RuntimeError:
            pass

    def _setup_desktop_pet(self) -> None:
        """Show / hide the desktop companion pet."""
        from src.desktop_pet import desktop_pet_enabled, desktop_pet_visible

        existing = getattr(self, "pet_widget", None)
        if existing is not None:
            try:
                existing.close()
                existing.deleteLater()
            except RuntimeError:
                pass
            self.pet_widget = None
        if not desktop_pet_enabled(self.settings):
            return
        if self._icons_hidden or self._exiting:
            return
        from src.ui.pet_widget import DesktopPetWidget

        pet = DesktopPetWidget(self.settings)
        self.pet_widget = pet
        pet.page_changed.connect(
            self.switch_page, Qt.ConnectionType.QueuedConnection
        )
        pet.create_fence_requested.connect(
            self._on_page_create_fence_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        pet.folder_open_requested.connect(
            self._on_page_folder_open_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        pet.note_requested.connect(
            self._on_note_requested, Qt.ConnectionType.QueuedConnection
        )
        pet.note_folder_requested.connect(
            self._on_note_folder_requested, Qt.ConnectionType.QueuedConnection
        )
        pet.record_requested.connect(
            self._on_record_requested, Qt.ConnectionType.QueuedConnection
        )
        pet.record_folder_requested.connect(
            self._on_record_folder_requested, Qt.ConnectionType.QueuedConnection
        )
        pet.minutes_requested.connect(
            self._on_meeting_minutes_requested, Qt.ConnectionType.QueuedConnection
        )
        pet.minutes_folder_requested.connect(
            self._on_meeting_minutes_folder_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        pet.calculator_requested.connect(
            self._on_calculator_requested, Qt.ConnectionType.QueuedConnection
        )
        pet.todo_requested.connect(
            self._on_todo_requested, Qt.ConnectionType.QueuedConnection
        )
        pet.vault_requested.connect(
            self._on_vault_requested, Qt.ConnectionType.QueuedConnection
        )
        if desktop_pet_visible(self.settings) and (
            self._page_chrome_may_show() or self._desk_app_ui_open()
        ):
            # Settings open: show for live preview but do not raise_ (owner group
            # would cover the settings window). Sink afterward via caller / FG.
            if self._desk_app_ui_open():
                pet.show()
            else:
                pet.show_pet()
                self._ensure_page_chrome_visible()
        # Pet may own the float bar — rebuild or tear down the edge indicator.
        self._setup_page_indicator()
        if self._desk_app_ui_open() and self._desk_app_owns_foreground():
            self._keep_overlays_under_apps()

    def _on_pet_settings_changed(self) -> None:
        """Apply companion options without destroying the pet (size live-preview).

        While settings/notepad are open, never ``raise_()`` DefView-owned overlays —
        Qt raise lifts the Progman owner group and fences cover the app UI.
        """
        from src.desktop_pet import desktop_pet_enabled, desktop_pet_visible

        try:
            save_settings(self.settings)
        except OSError:
            pass

        app_ui = self._desk_app_ui_open()

        if not desktop_pet_enabled(self.settings) or self._icons_hidden or self._exiting:
            existing = getattr(self, "pet_widget", None)
            if existing is not None:
                try:
                    existing.close()
                    existing.deleteLater()
                except RuntimeError:
                    pass
                self.pet_widget = None
            self._setup_page_indicator()
            if app_ui:
                self._keep_overlays_under_apps()
            return

        pet = getattr(self, "pet_widget", None)
        if pet is None:
            self._setup_desktop_pet()
            if app_ui:
                self._keep_overlays_under_apps()
            return

        try:
            pet.reload_settings(self.settings)
            if desktop_pet_visible(self.settings):
                if not pet.isVisible():
                    if app_ui:
                        # Map without raise_ — showEvent still shell-attaches.
                        pet.show()
                    else:
                        pet.show_pet()
                # Already visible: size/theme are geometry + paint only. Do not
                # raise_/configure — that lifts fences over the settings window.
            else:
                pet.hide()
            self._setup_page_indicator()
            if app_ui:
                self._keep_overlays_under_apps()
        except RuntimeError:
            self._setup_desktop_pet()
            if app_ui:
                self._keep_overlays_under_apps()

    def _on_pet_requested(self) -> None:
        """Page-bar「宠物」: show / raise the desktop pet (also restores after hide)."""
        from src.desktop_pet import desktop_pet_enabled, desktop_pet_settings

        if not desktop_pet_enabled(self.settings):
            return
        # Hide sets visible=False; page-bar click must force it back on.
        cfg = desktop_pet_settings(self.settings)
        cfg["visible"] = True
        try:
            from src.settings import save_settings

            save_settings(self.settings)
        except OSError:
            pass
        pet = getattr(self, "pet_widget", None)
        if pet is None:
            self._setup_desktop_pet()
            pet = getattr(self, "pet_widget", None)
        if pet is None:
            return
        try:
            pet.show_pet()
            pet.raise_()
            self._ensure_page_chrome_visible(raise_band=True)
        except RuntimeError:
            pass

    def _on_extensions_changed(self) -> None:
        save_settings(self.settings, immediate=True)
        self._schedule_hotkeys_refresh()
        self._setup_dock()
        self._setup_todo_panel()
        self._setup_account_vault_panel()
        self._setup_desktop_pet()
        self.tray.apply_feature_visibility(self.settings)
        try:
            from src.desknote import sync_desknote_shortcuts

            sync_desknote_shortcuts(self.settings)
        except Exception:
            pass
        try:
            from src.desknote_open_with import sync_desknote_open_with
            from src.notepad import notepad_enabled

            sync_desknote_open_with(enabled=notepad_enabled(self.settings))
        except Exception:
            pass
        if self.page_indicator:
            self.page_indicator.reload_folder_shortcuts()
        pet = getattr(self, "pet_widget", None)
        if pet is not None:
            try:
                pet.reload_pages()
            except RuntimeError:
                pass
        # deskNote enable/disable must drop/rebuild the public float immediately
        # (sticky-missing grace would leave a dead ghost icon for ~45s).
        try:
            self.refresh_public_desktop(immediate=True, relayout=False)
        except Exception:
            pass
        # While settings owns FG, restacking fences / raising the public host
        # steals clicks on extension checkboxes (「再次勾选要点不了，切桌面才行」).
        if self._settings_ui_open():
            try:
                self._keep_overlays_under_apps()
            except Exception:
                pass
            self._shell_attach_after_settings = True
        elif self._fences_should_show():
            self.show_fences()
        self.window.extensions_widget.reload_tables()

    def _fences_should_show(self) -> bool:
        """Preference on, and not in temporary desktop-clear session.

        Fences stay visible even when shell desktop icons are hidden.
        """
        if not bool(self.settings.get("show_fences", True)):
            return False
        if self._fences_hidden:
            return False
        return True

    def _hide_desktop_icons_keep_fences(self) -> None:
        """Hide Explorer desktop icons but keep fence overlays."""
        self.settings["hide_shell_icons"] = True
        save_settings(self.settings)
        set_desktop_icons_visible(False)
        self._icons_hidden = False
        self._fences_hidden = False
        if self.settings.get("show_fences", True):
            self.show_fences()
        if self.settings.get("show_page_indicator", True):
            if self.page_indicator:
                self.page_indicator.show()
            else:
                self._setup_page_indicator()
        self.tray.show_message(APP_NAME_ZH, "已隐藏桌面图标，分区继续显示")

    def _sync_fences_checkbox(self) -> None:
        if "settings" not in getattr(self.window, "_lazy_built", set()):
            return
        cb = self.window.show_fences_cb
        cb.blockSignals(True)
        cb.setChecked(bool(self.settings.get("show_fences", True)))
        cb.blockSignals(False)

    def _start_watcher(self) -> None:
        self.watcher.start(
            settings=self.settings,
            on_created_file=self._ui_bridge.file_created.emit,
            on_removed_file=self._ui_bridge.file_removed.emit,
            on_moved_file=self._ui_bridge.file_moved.emit,
            on_portal_changed=self._ui_bridge.portal_changed.emit,
        )

    def _maybe_reschedule_desktop_watcher(self) -> None:
        """If Explorer's Desktop path or portal folders changed, restart watcher."""
        try:
            from src.file_watcher import desktop_watch_fingerprint
            from src.settings import invalidate_desktop_paths_cache

            # Cheap compare against cached paths first (45s TTL).
            current = desktop_watch_fingerprint(self.settings)
            known = getattr(self.watcher, "fingerprint", ())
            if current == known and current:
                return
            invalidate_desktop_paths_cache()
            current = desktop_watch_fingerprint(self.settings)
            if current == known:
                return
            self._start_watcher()
        except Exception:
            pass

    def _start_loose_sync_background(self) -> None:
        """Scan desktop off the UI thread; apply + refresh on the GUI thread."""
        if getattr(self, "_loose_sync_inflight", False):
            return
        if not getattr(self, "_loose_sync_needed", False):
            return
        self._loose_sync_inflight = True
        snap = {
            "exclude_patterns": list(self.settings.get("exclude_patterns") or []),
            "fences": list(self.settings.get("fences") or []),
            "current_page": self.settings.get("current_page", 0),
            "enable_public_desktop": self.settings.get("enable_public_desktop", False),
        }

        import threading

        def _worker() -> None:
            try:
                from src.public_desktop import scan_loose_desktop_wanted

                payload = scan_loose_desktop_wanted(snap)
            except OSError as exc:
                payload = exc
            from src.qt_main_thread import call_on_main_thread

            call_on_main_thread(lambda: self._finish_loose_sync_background(payload))

        from src.qt_main_thread import ensure_main_thread_bridge

        ensure_main_thread_bridge(self.qt_app)
        threading.Thread(
            target=_worker, name="desktidy-loose-scan", daemon=True
        ).start()

    def _finish_loose_sync_background(self, payload) -> None:
        self._loose_sync_inflight = False
        if isinstance(payload, OSError):
            return
        try:
            from src.public_desktop import apply_loose_desktop_scan, invalidate_loose_sync_cache

            if apply_loose_desktop_scan(self.settings, payload):
                save_settings(self.settings)
            invalidate_loose_sync_cache()
            self._loose_sync_needed = False
            self.refresh_public_desktop(immediate=True, relayout=False)
        except OSError:
            pass

    def _on_portal_folder_changed(self, raw_path: str = "") -> None:
        """Portal directory changed — refresh mirrored fences only (no auto-pin)."""
        if raw_path:
            try:
                import os

                portal_dir = Path(raw_path)
                # Prefer parent of a file event; directories stay as-is.
                try:
                    if portal_dir.is_file():
                        portal_dir = portal_dir.parent
                except OSError:
                    pass
                key = os.path.normpath(str(portal_dir)).casefold()
                self._portal_dirty_dirs.add(key)
            except OSError:
                self._portal_dirty_dirs.clear()
        timer = getattr(self, "_portal_refresh_timer", None)
        if timer is not None:
            timer.start(350)

    def _refresh_portal_fences(self) -> None:
        if self._exiting:
            return
        dirty = set(getattr(self, "_portal_dirty_dirs", None) or set())
        self._portal_dirty_dirs.clear()
        from src.fence_rules import is_portal_fence
        import os

        refreshed = 0
        for fence in list(self.fences):
            try:
                if not is_portal_fence(getattr(fence, "config", None)):
                    continue
                if dirty:
                    portal = get_portal_path(getattr(fence, "config", None))
                    if portal is None:
                        continue
                    try:
                        key = os.path.normpath(str(portal)).casefold()
                    except OSError:
                        continue
                    # Match exact portal root or a dirty child path under it.
                    matched = key in dirty or any(
                        d == key or d.startswith(key + os.sep)
                        for d in dirty
                    )
                    if not matched:
                        continue
                fence.refresh()
                refreshed += 1
            except RuntimeError:
                continue
            except Exception:
                continue
        # Path form mismatch: fall back to refreshing every portal fence once.
        if dirty and refreshed == 0:
            for fence in list(self.fences):
                try:
                    if is_portal_fence(getattr(fence, "config", None)):
                        fence.refresh()
                except RuntimeError:
                    continue
                except Exception:
                    continue

    def _on_watched_file(self, path: str) -> None:
        if path:
            try:
                from src.organize_suppress import is_organize_suppressed

                if is_organize_suppressed(path):
                    return
            except Exception:
                pass
        invalidate_desktop_scan_cache()
        from src.path_stat_cache import invalidate_path_stat_cache

        if path:
            invalidate_path_stat_cache(path)
        else:
            invalidate_path_stat_cache()
        self._loose_sync_needed = True
        if path:
            pending = getattr(self, "_pending_watch_paths", None)
            if not isinstance(pending, list):
                self._pending_watch_paths = []
                pending = self._pending_watch_paths
            pending.append(str(path))
            # Cap backlog so a desktop flood cannot grow forever.
            if len(pending) > 200:
                del pending[:-200]
        self._watch_debounce.start(300)

    def _on_watched_file_moved(self, src: str, dest: str) -> None:
        """Desktop rename vs leave-desktop move.

        Same-folder rename (Save As / Office / WeChat): relink pins + public floats.
        Move into another folder / off desktop: drop the public float immediately —
        rewriting public→dest would keep a ghost (dest still exists, sticky prune
        never fires). Fence pins still rewrite so they track the file.
        """
        try:
            from src.organize_suppress import is_organize_suppressed

            if is_organize_suppressed(src) or is_organize_suppressed(dest):
                return
        except Exception:
            pass
        invalidate_desktop_scan_cache()
        from src.path_stat_cache import invalidate_path_stat_cache
        from src.fence_rules import rewrite_pinned_path
        from src.public_desktop import remove_public_paths, rewrite_public_path
        from src.settings import get_desktop_paths, save_settings

        invalidate_path_stat_cache(src)
        invalidate_path_stat_cache(dest)
        self._loose_sync_needed = True
        old = Path(src)
        new = Path(dest)
        desktop_roots: set[str] = set()
        for root in get_desktop_paths():
            try:
                desktop_roots.add(str(root).casefold())
            except OSError:
                desktop_roots.add(str(root).casefold())
        try:
            src_parent = str(old.parent).casefold()
        except OSError:
            src_parent = ""
        try:
            dest_parent = str(new.parent).casefold()
        except OSError:
            dest_parent = ""
        leaving_desktop = src_parent in desktop_roots and dest_parent not in desktop_roots

        if leaving_desktop:
            changed = False
            try:
                if rewrite_pinned_path(self.settings, old, new):
                    changed = True
                # Drop public entry for the desktop path (and dest if a prior bug
                # already rewrote it). Never rewrite public → off-desktop.
                if remove_public_paths(self.settings, [old, new]):
                    changed = True
                if changed:
                    save_settings(self.settings)
            except OSError:
                pass
            try:
                self._remove_public_icon_widget(old)
                self._remove_public_icon_widget(new)
            except (RuntimeError, OSError, TypeError, ValueError):
                pass
            self._watch_debounce.start(300)
            return

        changed = False
        try:
            if rewrite_pinned_path(self.settings, old, new):
                changed = True
            if rewrite_public_path(self.settings, old, new):
                changed = True
            if changed:
                save_settings(self.settings)
        except OSError:
            pass
        try:
            target = str(old).casefold()
            for icon in list(self.public_icons):
                try:
                    same = str(icon.file_path).casefold() == target
                except OSError:
                    same = icon.file_path == old
                if not same:
                    continue
                icon.file_path = new
                refresh = getattr(icon, "refresh", None)
                if callable(refresh):
                    refresh()
                break
        except (RuntimeError, OSError, TypeError, ValueError):
            pass
        # Pins/icons already relinked — skip debounced full rebuild unless auto-pin.
        if changed:
            if bool(self.settings.get("auto_organize_watch", False)):
                pending = getattr(self, "_pending_watch_paths", None)
                if not isinstance(pending, list):
                    self._pending_watch_paths = []
                    pending = self._pending_watch_paths
                pending.append(str(new))
                self._watch_debounce.start(300)
            return
        pending = getattr(self, "_pending_watch_paths", None)
        if not isinstance(pending, list):
            self._pending_watch_paths = []
            pending = self._pending_watch_paths
        pending.append(str(new))
        self._watch_debounce.start(300)

    def _on_watched_file_removed(self, path: str) -> None:
        """Desktop delete/move-away — sticky-prune public floats (Office-save safe).

        Fence pins stay sticky (Fences-like): watcher remove must not unpin.
        Public floats share the same grace via sticky public prune — do not
        scrub public entries or retire widgets immediately (delete→recreate
        would lose grid position). Sustained absence is pruned after the
        sticky window; debounce refresh then drops ghosts.
        """
        # Recycle / folder-drop already dropped the one cell. A full overlay
        # rebuild here is the post-delete desktop flash.
        try:
            from src.organize_suppress import is_organize_suppressed

            if path and is_organize_suppressed(path):
                return
        except Exception:
            pass
        invalidate_desktop_scan_cache()
        from src.path_stat_cache import invalidate_path_stat_cache
        from src.public_desktop import prune_missing_public_items

        invalidate_path_stat_cache()
        self._loose_sync_needed = True
        try:
            if prune_missing_public_items(self.settings, force=True):
                save_settings(self.settings)
        except OSError:
            pass
        # First miss only stamps sticky; schedule one pass after the grace
        # window so permanent deletes drop without waiting for another event.
        self._schedule_sticky_missing_reprune()
        self._watch_debounce.start(400)

    def _schedule_sticky_missing_reprune(self) -> None:
        """Coalesce a post-sticky prune (Office-save safe, permanent-delete cleanup)."""
        from src.public_desktop import _STICKY_MISSING_S

        timer = getattr(self, "_sticky_reprune_timer", None)
        if timer is None:
            return
        delay_ms = int((float(_STICKY_MISSING_S) + 1.0) * 1000)
        timer.start(max(1000, delay_ms))

    def _run_sticky_missing_reprune(self) -> None:
        """Force sticky prune after grace — drop sustained-missing floats/pins."""
        from src.path_stat_cache import invalidate_path_stat_cache
        from src.fence_rules import prune_missing_virtual_items
        from src.public_desktop import prune_missing_public_items

        invalidate_path_stat_cache()
        changed = False
        try:
            if prune_missing_public_items(self.settings, force=True):
                changed = True
            if prune_missing_virtual_items(self.settings, force=True):
                changed = True
            if changed:
                save_settings(self.settings)
                self.refresh_fences(refresh_public=True)
        except OSError:
            pass

    def _purge_temp_desktop_refs(self) -> bool:
        """Drop Office/email temp paths from pins and public floats (runtime)."""
        try:
            from src.desktop_scanner import purge_temp_from_settings

            return bool(purge_temp_from_settings(self.settings))
        except Exception:
            return False

    def _run_watched_organize(self) -> None:
        """Debounced desktop watcher: optional rule auto-pin, then float refresh.

        When ``auto_organize_watch`` is on (Fences Rules-like), newly created /
        renamed desktop paths that match software/document rules are pinned.
        Otherwise they stay as current-page floats until 一键整理 / manual drag.
        """
        pending_raw = list(getattr(self, "_pending_watch_paths", None) or [])
        self._pending_watch_paths = []
        purged = self._purge_temp_desktop_refs()
        ingested_public = False
        for raw in pending_raw:
            try:
                if self._try_ingest_desktop_file_as_public(str(raw)):
                    ingested_public = True
            except Exception:
                continue
        if ingested_public:
            try:
                save_settings(self.settings)
            except OSError:
                pass
        elif purged:
            try:
                save_settings(self.settings)
            except OSError:
                pass
        if bool(self.settings.get("hide_shell_icons")):
            try:
                from src.public_desktop import ensure_desktop_loose_floats

                if ensure_desktop_loose_floats(self.settings):
                    try:
                        save_settings(self.settings)
                    except OSError:
                        pass
            except Exception:
                pass
        if pending_raw and bool(self.settings.get("auto_organize_watch", False)):
            try:
                from src.organizer import try_auto_pin_desktop_paths

                pinned, pin_pages = try_auto_pin_desktop_paths(
                    self.settings, pending_raw
                )
                if pinned:
                    try:
                        save_settings(self.settings)
                    except OSError:
                        pass
                    get_logger().info(
                        "auto_organize_watch: pinned %s path(s)", len(pinned)
                    )
                if pin_pages:
                    self._reveal_organize_float_pages(pin_pages)
            except Exception:
                get_logger().exception("auto_organize_watch: pin failed")
        # refresh_public runs background loose sync for unmatched Save As / drops.
        self.refresh_fences(refresh_public=True)
        self.window.refresh_data_if_visible()

    def _schedule_hotkeys_refresh(self, delay_ms: int = 450) -> None:
        """Re-register global hotkeys after settings edits (defer past key release)."""
        if int(getattr(self, "_hotkey_capture_depth", 0)) > 0:
            self._hotkey_refresh_pending = True
            return
        timer = getattr(self, "_hotkey_refresh_timer", None)
        if timer is None:
            self._hotkey_refresh_timer = QTimer()
            self._hotkey_refresh_timer.setSingleShot(True)
            self._hotkey_refresh_timer.timeout.connect(self._setup_hotkeys)
            timer = self._hotkey_refresh_timer
        timer.start(max(0, int(delay_ms)))

    def _on_hotkey_capture_began(self) -> None:
        self._hotkey_capture_depth = int(getattr(self, "_hotkey_capture_depth", 0)) + 1
        if self._hotkey_capture_depth == 1:
            timer = getattr(self, "_hotkey_refresh_timer", None)
            if timer is not None:
                timer.stop()
            self.hotkey_manager.unregister_all()

    def _on_hotkey_capture_ended(self) -> None:
        self._hotkey_capture_depth = max(0, int(getattr(self, "_hotkey_capture_depth", 0)) - 1)
        if self._hotkey_capture_depth == 0:
            delay = 200 if getattr(self, "_hotkey_refresh_pending", False) else 0
            self._hotkey_refresh_pending = False
            self._schedule_hotkeys_refresh(delay)

    def _setup_hotkeys(self) -> None:
        for shortcut in self._app_shortcuts:
            shortcut.deleteLater()
        self._app_shortcuts.clear()
        self.hotkey_manager.unregister_all()

        hotkeys = self.settings.get("hotkeys", {})
        # Extension checkboxes only control page-bar (and tray) chrome — hotkeys
        # stay registered whenever a binding is configured.
        bindings: list[tuple[str, str, object]] = [
            ("organize", hotkeys.get("organize", ""), self._do_organize),
            ("toggle_fences", hotkeys.get("toggle_fences", ""), self._toggle_fences_visibility),
            ("toggle_icons", hotkeys.get("toggle_icons", ""), self._toggle_icons_and_fences),
            ("peek_fences", hotkeys.get("peek_fences", ""), self._toggle_peek),
            ("page_next", hotkeys.get("page_next", ""), self.next_page),
            ("page_prev", hotkeys.get("page_prev", ""), self.prev_page),
            ("screenshot", hotkeys.get("screenshot", ""), self.screenshot_manager.start_capture),
            (
                "show_screenshot",
                hotkeys.get("show_screenshot", ""),
                self._hotkey_show_last_screenshot,
            ),
            (
                "screen_record",
                hotkeys.get("screen_record", "F3"),
                self.screen_record_manager.toggle_recording,
            ),
            (
                "file_search",
                hotkeys.get("file_search", "F4"),
                self._toggle_file_search,
            ),
            (
                "calculator",
                hotkeys.get("calculator", "Ctrl+Alt+C"),
                self._on_calculator_requested,
            ),
        ]
        hotkeys_changed = False
        for action, key, callback in bindings:
            key = str(key or "").strip()
            if not key:
                continue
            # Global hotkey (RegisterHotKey / async poll). Do not also bind QShortcut
            # when global registration succeeded — bare F-keys would fire twice
            # (poll + ApplicationShortcut) while the settings window is open.
            used: str | None = None
            if action in HOTKEY_FALLBACKS:
                used = self.hotkey_manager.register_with_fallbacks(action, key, callback)
                if used and used.casefold() != key.casefold():
                    hotkeys[action] = used
                    self._sync_hotkey_input(action, used)
                    hotkeys_changed = True
                    key = used
            elif self.hotkey_manager.register(action, key, callback):
                used = key
            if used:
                continue
            seq = QKeySequence(key)
            if not seq.isEmpty():
                shortcut = QShortcut(seq, self.window)
                shortcut.setContext(Qt.ShortcutContext.ApplicationShortcut)
                shortcut.activated.connect(callback)
                self._app_shortcuts.append(shortcut)

        if hotkeys_changed:
            try:
                save_settings(self.settings)
            except OSError:
                pass

        failed = self.hotkey_manager.failed_bindings
        if failed:
            parts = [
                f"{HOTKEY_LABELS.get(action, action)}({key})"
                for action, key in failed
            ]
            self.tray.show_message(
                APP_NAME_ZH,
                "以下快捷键未能注册，可能被其他软件占用或格式无效：\n" + "、".join(parts),
            )
    def _sync_hotkey_input(self, action: str, key: str) -> None:
        edits = getattr(self.window, "hotkey_inputs", {})
        edit = edits.get(action)
        if edit is not None:
            edit.setText(key)
        ext = getattr(self.window, "extensions_widget", None)
        if ext is not None:
            ext_edits = getattr(ext, "hotkey_inputs", {})
            ext_edit = ext_edits.get(action)
            if ext_edit is not None:
                ext_edit.setText(key)

    def _notify_user(self, text: str) -> None:
        tray = getattr(self, "tray", None)
        if tray is None:
            return
        # Recording tips get a clearer balloon title.
        title = APP_NAME_ZH
        if text.startswith("录屏已开始"):
            title = "录屏已开始"
        elif text.startswith("录屏已保存"):
            title = "录屏已保存"
        elif text.startswith("已取消保存"):
            title = "录屏"
        elif text.startswith("录屏"):
            title = "录屏"
        tray.show_message(title, text, msec=5000)

    def _hotkey_show_last_screenshot(self) -> None:
        if not self.screenshot_manager.show_last_screenshot():
            hotkey = self.settings.get("hotkeys", {}).get("screenshot", "F1")
            self.tray.show_message(
                APP_NAME_ZH,
                f"暂无截图。请先按 {hotkey} 进行区域截图。",
            )

    def _on_organize_requested(self, float_page_ids=None) -> None:
        invalidate_desktop_scan_cache()
        self._loose_sync_needed = True
        self._public_force_relayout = True
        # Switch before refresh so floats on the target page are created visible.
        self._reveal_organize_float_pages(float_page_ids)
        self.refresh_fences(refresh_public=True)

    def _reveal_organize_float_pages(self, float_page_ids) -> None:
        """Switch to a page that just received floats so they are not 'missing'."""
        pages = list(float_page_ids or [])
        if not pages:
            return
        try:
            current = int(self._current_page())
        except (TypeError, ValueError):
            current = 0
        dest = None
        for raw in pages:
            try:
                pid = int(raw)
            except (TypeError, ValueError):
                continue
            if pid != current:
                dest = pid
                break
        if dest is None:
            return
        self.switch_page(dest)

    def _merge_organize_snapshot(self, snap: dict) -> None:
        """Apply worker organize results onto live settings + fence widgets."""
        self.settings["organize_mode"] = snap.get("organize_mode", "virtual")
        if isinstance(snap.get("public_desktop_items"), list):
            self.settings["public_desktop_items"] = snap["public_desktop_items"]
        snap_by_id = {
            str(f.get("id")): f
            for f in snap.get("fences") or []
            if isinstance(f, dict) and f.get("id")
        }
        live_ids = {
            str(f.get("id"))
            for f in self.settings.get("fences") or []
            if isinstance(f, dict) and f.get("id")
        }
        for fid, fence_cfg in snap_by_id.items():
            if fid not in live_ids:
                self.settings.setdefault("fences", []).append(fence_cfg)
        for fence_cfg in self.settings.get("fences") or []:
            if not isinstance(fence_cfg, dict):
                continue
            fid = str(fence_cfg.get("id") or "")
            src = snap_by_id.get(fid)
            if src is None:
                continue
            if "virtual_items" in src:
                fence_cfg["virtual_items"] = list(src.get("virtual_items") or [])
        live_by_id = {
            str(f.config.get("id")): f
            for f in self.fences
            if getattr(f, "config", None) and f.config.get("id")
        }
        for fence_cfg in self.settings.get("fences") or []:
            fid = str(fence_cfg.get("id") or "")
            widget = live_by_id.get(fid)
            if widget is not None:
                widget.config = fence_cfg

    def _finish_organize_worker(self, payload, *, silent: bool) -> None:
        self._organize_inflight = False
        if isinstance(payload, Exception):
            get_logger().exception("organize_desktop failed")
            if not silent:
                self.tray.show_message(APP_NAME_ZH, "整理失败，请稍后重试")
            return
        result, snap = payload
        try:
            self._merge_organize_snapshot(snap)
            save_settings(self.settings)
        except OSError:
            pass
        self.window.refresh_data_if_visible()
        self._on_organize_requested(
            float_page_ids=getattr(result, "float_page_ids", None)
        )
        if not silent:
            self.tray.show_message(APP_NAME_ZH, result.summary)

    def _do_organize(self, *, silent: bool = False) -> None:
        if getattr(self, "_organize_inflight", False):
            return
        self._organize_inflight = True
        from copy import deepcopy

        from src.qt_main_thread import call_on_main_thread, ensure_main_thread_bridge

        ensure_main_thread_bridge(self.qt_app)
        snap = deepcopy(self.settings)
        exclude = list(snap.get("exclude_patterns") or [])

        import threading

        def _worker() -> None:
            try:
                result = organize_desktop(
                    settings=snap, exclude=exclude, persist=False
                )
                payload = (result, snap)
            except Exception as exc:
                payload = exc
            # Never QTimer.singleShot from a raw thread — completion never ran
            # → tray「一键整理」looked broken (_organize_inflight stuck / no toast).
            call_on_main_thread(
                lambda: self._finish_organize_worker(payload, silent=silent)
            )

        threading.Thread(
            target=_worker, name="desktidy-organize", daemon=True
        ).start()

    def _current_page(self) -> int:
        return self.settings.get("current_page", 0)

    def _get_pages(self) -> list[dict]:
        pages = self.settings.get("desktop_pages")
        if not pages:
            pages = [{"id": 0, "name": "默认"}]
            self.settings["desktop_pages"] = pages
        return pages

    def switch_page(self, page_id: int) -> None:
        started = time.perf_counter()
        page_id = int(page_id)
        # One settings write: leaving-page geometry stays in memory, then
        # current_page + layout persist together (two json.dump's stalled the click).
        if self.fences:
            self._flush_fence_layout_save(persist=False)
        self.settings["current_page"] = page_id
        save_settings(self.settings)
        if self.page_indicator:
            self.page_indicator.set_current_page(page_id)
        elif self.settings.get("show_page_indicator", True) and len(self._get_pages()) > 1:
            # Hotkey switch must not leave chrome missing if the bar was torn down.
            self._setup_page_indicator()
        pet = getattr(self, "pet_widget", None)
        if pet is not None:
            try:
                pet.set_current_page(page_id)
            except RuntimeError:
                pass
        if self.settings.get("show_fences", True):
            # Page switch hotkeys should always keep overlays visible.
            self._fences_hidden = False
        self._page_switch_ensure_pending = True
        self._page_switch_batch_fence_ids: set[str] = set()
        self._page_switch_force_refresh_fence_ids: set[str] = set()
        self._page_switch_quiet_until = time.perf_counter() + 1.0
        # Drop any coalesced public-refresh timer so the swap is one paint cycle,
        # not fences-now / floats-+200ms (that double-flashed on every click).
        timer = getattr(self, "_refresh_public_timer", None)
        if timer is not None and timer.isActive():
            timer.stop()
        # Paint arriving fences off-screen first — never create them inside freeze.
        if self._fences_should_show() and self._warmup_fences_for_page(page_id):
            # Off-screen first-map must be composited before the on-screen SHOW.
            self.qt_app.processEvents(
                QEventLoop.ProcessEventsFlag.ExcludeUserInputEvents
            )
        with self._page_switch_paint_freeze():
            if self._fences_should_show():
                self.show_fences()
            else:
                # Soft page switch — keep saved float positions (no jump).
                self.refresh_public_desktop(immediate=True, relayout=False)
                for fence in list(self.fences):
                    try:
                        fence_id = str(fence.config.get("id") or "")
                        if fence_id:
                            self._park_fence(fence_id, fence, soft=True)
                        else:
                            self._retire_overlay_widget(fence)
                    except RuntimeError:
                        continue
                self.fences.clear()
        # After batch.commit in freeze finally — never ensure mid-freeze (public
        # refresh used to finish early and clear pending before Win32 show).
        self._restore_arriving_fences_interactive()
        self._refresh_public_host_click_mask()
        # Keep page-switch quiet until finish completes icon-grid fill — clearing
        # quiet here let keepalive / shell attach interleave and re-stack overlays
        # before arriving fences were clickable (开机第一次切页).
        self._finish_page_switch_overlays_if_pending()
        # Admin tree after desktop paint — selecting the page used to run before
        # show_fences and made A↔B clicks feel sticky.
        self.window.desktop_layout.select_page_id(page_id)
        # Do not raise/restack page chrome here — it never left. Repeated
        # HWND_TOP / force-attach on every click flashed the desktop 2–3 times.
        self._log_timed("switch_page", started)

    @contextmanager
    def _page_switch_paint_freeze(self):
        """One composition cycle for the whole A↔B overlay swap."""
        from src.desktop_shell_host import OverlayWindowBatch, freeze_desktop_paint

        depth = int(getattr(self, "_page_switch_freeze_depth", 0))
        self._page_switch_freeze_depth = depth + 1
        widgets: list[QWidget] = []
        for widget in list(self.fences):
            widgets.append(widget)
        for widget in list(getattr(self, "_parked_fences", {}).values()):
            widgets.append(widget)
        host = getattr(self, "_public_icon_host", None)
        if host is not None:
            widgets.append(host)
            if depth == 0:
                try:
                    host.begin_mask_batch()
                except RuntimeError:
                    pass
        if depth == 0:
            self._page_switch_geo_batch = OverlayWindowBatch()
            self._page_switch_qt_sync: list[
                tuple[QWidget, int, int, int, int]
            ] = []
        for widget in widgets:
            try:
                widget.setUpdatesEnabled(False)
            except RuntimeError:
                pass
        try:
            if depth == 0:
                with freeze_desktop_paint():
                    yield
            else:
                yield
        finally:
            if depth == 0:
                batch = getattr(self, "_page_switch_geo_batch", None)
                sync = list(getattr(self, "_page_switch_qt_sync", []) or [])
                self._page_switch_geo_batch = None
                self._page_switch_qt_sync = []
                if batch is not None:
                    try:
                        batch.commit()
                    except Exception:
                        pass
                # HWND already at target (DeferWindowPos). Sync Qt cache only —
                # same-rect SetWindowPos is a no-op visually.
                from PyQt6.QtCore import QRect

                for widget, x, y, w, h in sync:
                    try:
                        target = QRect(int(x), int(y), int(w), int(h))
                        if widget.geometry() == target:
                            continue
                        widget.setGeometry(target)
                    except RuntimeError:
                        continue
                # Keep Qt isVisible in sync with Win32 after batch hide/show.
                # Soft-parked fences stay Qt-hidden so paint cannot re-ShowWindow.
                # Soft-shown fences are already Win32-mapped — do NOT bare show()
                # (second ShowWindow flashes like a popup opening/closing).
                from src.win_shell import overlay_win32_visible

                from src.desktop_shell_host import is_attached_to_desktop

                for fence in list(getattr(self, "_parked_fences", {}).values()):
                    try:
                        if not bool(getattr(fence, "_desktidy_soft_parked", False)):
                            continue
                        if fence.isVisible():
                            self._sync_qt_visible_after_win32(fence, visible=False)
                    except RuntimeError:
                        continue
                for fence in list(self.fences):
                    try:
                        batch_reveal = bool(
                            getattr(fence, "_desktidy_batch_reveal", False)
                        )
                        hwnd = self._fence_hwnd(fence)
                        attached = bool(hwnd and is_attached_to_desktop(hwnd))
                        win32_on = bool(
                            hwnd and overlay_win32_visible(fence)
                        )
                        if batch_reveal:
                            # Keep batch_reveal True across Qt sync so showEvent
                            # does not ShowWindow again (VPN-like flash).
                            if float(fence.windowOpacity()) < 0.99:
                                fence.setWindowOpacity(fence._target_opacity())
                            if bool(getattr(fence, "_desktidy_soft_parked", False)):
                                fence._desktidy_batch_reveal = False  # type: ignore[attr-defined]
                                continue
                            if attached and win32_on:
                                if not fence.isVisible():
                                    self._sync_qt_visible_after_win32(
                                        fence, visible=True
                                    )
                                else:
                                    try:
                                        fence.update()
                                    except RuntimeError:
                                        pass
                            else:
                                self._reveal_desktop_overlay(fence)
                            fence._desktidy_batch_reveal = False  # type: ignore[attr-defined]
                            continue
                        if fence.testAttribute(
                            Qt.WidgetAttribute.WA_DontShowOnScreen
                        ):
                            fence.setAttribute(
                                Qt.WidgetAttribute.WA_DontShowOnScreen, False
                            )
                        if float(fence.windowOpacity()) < 0.99:
                            fence.setWindowOpacity(fence._target_opacity())
                        if bool(getattr(fence, "_desktidy_soft_parked", False)):
                            continue
                        if win32_on and attached:
                            if not fence.isVisible():
                                self._sync_qt_visible_after_win32(
                                    fence, visible=True
                                )
                            else:
                                try:
                                    fence.update()
                                except RuntimeError:
                                    pass
                            continue
                        if win32_on:
                            self._sync_qt_visible_after_win32(fence, visible=True)
                        else:
                            self._reveal_desktop_overlay(fence)
                    except RuntimeError:
                        continue
            # Include fences created mid-switch — the start-of-freeze list misses them.
            reenable: list[QWidget] = list(widgets)
            for fence in list(self.fences):
                reenable.append(fence)
            for fence in list(getattr(self, "_parked_fences", {}).values()):
                reenable.append(fence)
            seen: set[int] = set()
            for widget in reenable:
                if id(widget) in seen:
                    continue
                seen.add(id(widget))
                try:
                    widget.setUpdatesEnabled(True)
                except RuntimeError:
                    pass
            if host is not None and depth == 0:
                try:
                    host.end_mask_batch()
                except RuntimeError:
                    pass
            self._page_switch_freeze_depth = max(0, depth)

    def _finish_page_switch_overlays_if_pending(self) -> None:
        if not getattr(self, "_page_switch_ensure_pending", False):
            return
        # Still inside paint freeze — batch not committed yet.
        if int(getattr(self, "_page_switch_freeze_depth", 0) or 0) > 0:
            return
        if getattr(self, "_refreshing_public", False):
            QTimer.singleShot(0, self._finish_page_switch_overlays_if_pending)
            return
        timer = getattr(self, "_refresh_public_timer", None)
        if timer is not None and timer.isActive():
            timer.stop()
        self._page_switch_ensure_pending = False
        self._ensure_page_switch_overlays_visible()
        # Mouse must work before icon-grid fill (文档 first visit can take seconds).
        self._restore_arriving_fences_interactive()
        self._refresh_fences_after_page_switch()
        # Release keepalive only after icon-grid paint — clearing quiet at the
        # start let _ensure_desktop_overlays_visible interleave during hotkey flips.
        self._page_switch_quiet_until = 0.0

    def ensure_live_fences_interactive(self) -> None:
        """Live fences must take clicks: opaque band + pet above fence icons.

        Owner of the DefView-band click invariant. Soft-park, pet ``raise_band``,
        and ``refresh_public_desktop`` after Explorer→fence drops can leave
        WS_EX_TRANSPARENT stuck or mis-order HWNDs so icons paint but cannot be
        selected. Call after page switch, shell attach, and virtual drop finish
        — not via delayed heal timers. Public host stays below fences; pet is
        raised last so the sprite covers fence icons.

        Do **not** HWND_TOP unless Explorer desktop (Progman/DefView) owns FG —
        same gate as shell repair. Tray「一键整理」/ taskbar / settings / foreign
        apps used to count as desktop-side FG and ``refresh_public`` raised every
        fence over WeChat/VS Code until the user switched windows.
        """
        from src.win_shell import set_overlay_mouse_passthrough

        # Opaque + clickable always; band raise only on true Explorer desktop FG.
        skip_raise = True
        try:
            from src.win_shell import is_explorer_desktop_foreground

            if is_explorer_desktop_foreground():
                skip_raise = False
        except Exception:
            skip_raise = True

        if skip_raise:
            first_fence_hwnd = 0
            for fence in list(getattr(self, "fences", None) or ()):
                try:
                    if bool(getattr(fence, "_desktidy_soft_parked", False)):
                        self._soft_show_fence_for_page(fence)
                    set_overlay_mouse_passthrough(fence, False)
                    if not first_fence_hwnd:
                        hwnd = self._fence_hwnd(fence)
                        if hwnd:
                            first_fence_hwnd = int(hwnd)
                except RuntimeError:
                    continue
            pet_hwnd = 0
            pet = getattr(self, "pet_widget", None)
            if pet is not None:
                try:
                    if pet.isVisible():
                        pet_hwnd = int(pet.winId()) if pet.winId() else 0
                except (RuntimeError, Exception):
                    pet_hwnd = 0
            # Still sink the public plate under live fences/pet — skip_raise only
            # means "do not HWND_TOP the band over foreign apps", not "leave the
            # full-desktop host above sibling overlays so it steals their clicks".
            self._sink_public_host_below_overlays(
                first_fence_hwnd, pet_hwnd=pet_hwnd
            )
            return

        from src.desktop_shell_host import (
            raise_overlay_in_desktop_band,
        )

        first_fence_hwnd = 0
        for fence in list(getattr(self, "fences", None) or ()):
            try:
                if bool(getattr(fence, "_desktidy_soft_parked", False)):
                    self._soft_show_fence_for_page(fence)
                set_overlay_mouse_passthrough(fence, False)
                hwnd = self._fence_hwnd(fence)
                if not hwnd:
                    continue
                raise_overlay_in_desktop_band(hwnd)
                if not first_fence_hwnd:
                    first_fence_hwnd = int(hwnd)
            except RuntimeError:
                continue

        pet_hwnd = 0
        pet = getattr(self, "pet_widget", None)
        if pet is not None:
            try:
                if pet.isVisible():
                    pet_hwnd = int(pet.winId()) if pet.winId() else 0
                    if pet_hwnd:
                        raise_overlay_in_desktop_band(int(pet_hwnd))
            except (RuntimeError, Exception):
                pet_hwnd = 0

        self._sink_public_host_below_overlays(first_fence_hwnd, pet_hwnd=pet_hwnd)

    def _sink_public_host_below_overlays(
        self, first_fence_hwnd: int = 0, *, pet_hwnd: int = 0
    ) -> None:
        """Park the full-desktop public plate under fences/pet and refresh holes.

        Owner of host-vs-sibling Z + exclusion mask. ``present`` / setMask can
        leave the host above fences; layered HTCLIENT then steals float clicks.
        """
        from src.desktop_shell_host import (
            SWP_NOACTIVATE,
            SWP_NOMOVE,
            SWP_NOOWNERZORDER,
            SWP_NOREDRAW,
            SWP_NOSIZE,
            user32,
        )

        def _sink_below(widget, above_hwnd: int) -> None:
            if widget is None or not above_hwnd:
                return
            try:
                if not widget.isVisible():
                    return
                other = int(widget.winId()) if widget.winId() else 0
            except RuntimeError:
                return
            if not other:
                return
            try:
                user32.SetWindowPos(
                    int(other),
                    int(above_hwnd),
                    0,
                    0,
                    0,
                    0,
                    SWP_NOMOVE
                    | SWP_NOSIZE
                    | SWP_NOACTIVATE
                    | SWP_NOREDRAW
                    | SWP_NOOWNERZORDER,
                )
            except Exception:
                pass

        host = getattr(self, "_public_icon_host", None)
        if first_fence_hwnd:
            _sink_below(host, first_fence_hwnd)
        elif pet_hwnd:
            _sink_below(host, pet_hwnd)
        else:
            # No fence/pet: still prefer vault buoy / page tip above the plate.
            for attr in ("vault_launcher", "page_indicator", "vault_panel"):
                tip = getattr(self, attr, None)
                if tip is None:
                    continue
                try:
                    if not tip.isVisible():
                        continue
                    hwnd = int(tip.winId()) if tip.winId() else 0
                except RuntimeError:
                    continue
                if hwnd:
                    _sink_below(host, hwnd)
                    break

        if host is not None:
            try:
                refresh = getattr(host, "refresh_click_mask", None)
                if callable(refresh):
                    refresh()
            except RuntimeError:
                pass

    def _restore_arriving_fences_interactive(self) -> None:
        """After A↔B batch: arriving fences must take clicks immediately.

        Soft-park leaves mouse passthrough on leavers; a stuck WS_EX_TRANSPARENT
        / Qt attribute or a pet/public HWND stacked above arriving fences made
        文档 icons unselectable until a later heal / the UI thread finished
        icon fill (especially 开机第一次切页).
        """
        from src.desktop_shell_host import (
            set_overlay_hwnd_visible,
        )
        from src.win_shell import overlay_win32_visible, set_overlay_mouse_passthrough

        # Leavers that stayed mapped with mouse-through steal or black-hole hits.
        for fence in list(getattr(self, "_parked_fences", {}).values()):
            try:
                if not bool(getattr(fence, "_desktidy_soft_parked", False)):
                    continue
                hwnd = self._fence_hwnd(fence)
                if hwnd and overlay_win32_visible(fence):
                    set_overlay_hwnd_visible(hwnd, False)
                set_overlay_mouse_passthrough(fence, True)
            except RuntimeError:
                continue

        self.ensure_live_fences_interactive()

    def _yield_page_switch_ui(self) -> None:
        """Pump the event loop so fence clicks work while icon grids fill."""
        app = getattr(self, "qt_app", None)
        if app is None:
            return
        try:
            app.processEvents(QEventLoop.ProcessEventsFlag.AllEvents)
        except Exception:
            pass

    def _mark_page_switch_fence_force_refresh(self, fence_id: str) -> None:
        ids = getattr(self, "_page_switch_force_refresh_fence_ids", None)
        if ids is not None and fence_id:
            ids.add(str(fence_id))

    def _refresh_fences_after_page_switch(self) -> None:
        """Repaint icon grids after batch show — never during the paint freeze.

        Yield between heavy fills so 工作→文档 does not freeze mouse selection
        for several seconds on the UI thread (portal listing / SHGetFileInfo).
        """
        if getattr(self, "_exiting", False) or getattr(self, "_icons_hidden", False):
            return
        force_ids = set(
            getattr(self, "_page_switch_force_refresh_fence_ids", None) or ()
        )
        try:
            for fence in list(getattr(self, "fences", None) or ()):
                try:
                    if bool(getattr(fence, "_desktidy_soft_parked", False)):
                        continue
                    if not isinstance(fence, FenceWidget):
                        continue
                    hwnd = self._fence_hwnd(fence)
                    try:
                        from src.desktop_shell_host import is_attached_to_desktop
                        from src.win_shell import overlay_win32_visible

                        if (
                            hwnd
                            and is_attached_to_desktop(hwnd)
                            and overlay_win32_visible(fence)
                            and not fence.isVisible()
                        ):
                            self._sync_qt_visible_after_win32(fence, visible=True)
                    except RuntimeError:
                        pass
                    defer = bool(getattr(fence, "_desktidy_defer_refresh", False))
                    fence._desktidy_defer_refresh = False  # type: ignore[attr-defined]
                    fence_id = str(fence.config.get("id") or "")
                    has_icons = fence.has_icon_widgets()
                    # First-visit prepare sets defer — force=True wipes the shell-icon
                    # cache and repaints the whole desktop on the first click.
                    needs_force = fence_id in force_ids and not defer
                    did_heavy = False
                    if needs_force:
                        fence._desktidy_page_switch_refresh = True  # type: ignore[attr-defined]
                        try:
                            fence.refresh(force=True)
                        finally:
                            fence._desktidy_page_switch_refresh = False  # type: ignore[attr-defined]
                        did_heavy = True
                    elif not has_icons or defer:
                        # Unwarmed first visit: fill without wiping cache; keep
                        # stagger so the click is not N× SHGetFileInfo.
                        fence._refresh_impl()
                        fence.flush_icon_grid_paint()
                        did_heavy = True
                    # Warmed parked grids already painted off-screen — ensure/finish
                    # runs update() + processEvents for post-batch client paint.
                    if did_heavy:
                        self._yield_page_switch_ui()
                except RuntimeError:
                    continue
        finally:
            self._page_switch_batch_fence_ids = set()
            self._page_switch_force_refresh_fence_ids = set()

    def _page_chrome_needs_raise(self) -> bool:
        """True when the right-edge page bar is missing, detached, or buried.

        Unconditional HWND_TOP on every page click flashes a solid popup over
        the wallpaper (VPN-like open/close). Only raise when hit-testing shows
        the bar is not receiving the pixels at its center.
        """
        tip = self.page_indicator
        if tip is None or not self.settings.get("show_page_indicator", True):
            return False
        from src.desktop_shell_host import is_attached_to_desktop
        from src.win_shell import overlay_win32_visible

        try:
            if not tip.isVisible():
                return True
            hwnd = int(tip.winId()) if tip.winId() else 0
        except Exception:
            return True
        if not hwnd:
            return True
        if not is_attached_to_desktop(hwnd) or not overlay_win32_visible(tip):
            return True
        try:
            import ctypes
            from ctypes import wintypes

            class POINT(ctypes.Structure):
                _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]

            center = tip.mapToGlobal(tip.rect().center())
            pt = POINT(int(center.x()), int(center.y()))
            hit = int(ctypes.windll.user32.WindowFromPoint(pt) or 0)
        except Exception:
            return False
        if not hit:
            return True
        # Own HWND or a child control (page buttons) → already on top of desktop.
        cur = hit
        for _ in range(8):
            if cur == hwnd:
                return False
            try:
                parent = int(ctypes.windll.user32.GetParent(cur) or 0)
            except Exception:
                break
            if not parent or parent == cur:
                break
            cur = parent
        # Buried under wallpaper/DefView → raise within the desktop band.
        # A foreign app covering this point is correct Z-order; HWND_TOP would
        # lift the whole Progman-owned group (fences) over that app.
        try:
            from src.win_shell import is_desktop_point

            return bool(is_desktop_point(int(center.x()), int(center.y())))
        except Exception:
            return False

    def _ensure_page_chrome_visible(self, *, raise_band: bool = False) -> None:
        """Show + shell-attach the right-edge page bar (and dock if enabled).

        *raise_band*: force HWND_TOP in the desktop band. After hotkey page
        switches, fences can bury the bar under DefView while it still looks
        ``IsWindowVisible`` — without a raise the right-edge buttons vanish.
        Keepalive paths leave *raise_band* False to avoid Z thrash / flash.
        """
        if not self._page_chrome_may_show():
            return
        if self._foreign_app_owns_foreground():
            # HWND_TOP chrome while an app is FG lifts fences over that app.
            raise_band = False
        from src.desktop_shell_host import is_attached_to_desktop
        from src.win_shell import configure_desktop_overlay, overlay_win32_visible

        if (
            self.page_indicator is None
            and self.settings.get("show_page_indicator", True)
            and len(self._get_pages()) > 1
        ):
            self._setup_page_indicator()

        chrome_widgets = []
        if self.page_indicator and self.settings.get("show_page_indicator", True):
            chrome_widgets.append(self.page_indicator)
        if self.dock and self.settings.get("dock", {}).get("enabled"):
            chrome_widgets.append(self.dock)
        todo = getattr(self, "todo_panel", None)
        if todo is not None:
            from src.todos import desktop_todos_enabled

            if desktop_todos_enabled(self.settings):
                chrome_widgets.append(todo)
        vault = getattr(self, "vault_panel", None)
        if vault is not None:
            try:
                if vault.isVisible():
                    chrome_widgets.append(vault)
            except RuntimeError:
                pass
        launcher = getattr(self, "vault_launcher", None)
        if launcher is not None:
            from src.account_vault import account_vault_enabled

            if account_vault_enabled(self.settings):
                chrome_widgets.append(launcher)
        pet = getattr(self, "pet_widget", None)
        if pet is not None:
            from src.desktop_pet import desktop_pet_visible

            if desktop_pet_visible(self.settings):
                chrome_widgets.append(pet)
        for tip in chrome_widgets:
            try:
                if not tip.isVisible():
                    tip.show()
                try:
                    hwnd = int(tip.winId())
                except Exception:
                    hwnd = 0
                # Healthy + no raise requested: skip SetWindowPos (page-switch flash).
                if (
                    not raise_band
                    and hwnd
                    and is_attached_to_desktop(hwnd)
                    and overlay_win32_visible(tip)
                ):
                    continue
                configure_desktop_overlay(tip, raise_band=raise_band)
                if not overlay_win32_visible(tip):
                    tip.show()
                    if raise_band:
                        tip.raise_()
                    configure_desktop_overlay(tip, raise_band=raise_band)
            except RuntimeError:
                continue
            except Exception:
                continue

    def _overlays_healthy_after_page_switch(self) -> bool:
        """True when soft park/unpark left every overlay mapped and visible."""
        from src.desktop_shell_host import is_attached_to_desktop
        from src.win_shell import overlay_win32_visible

        for fence in list(self.fences):
            try:
                if not fence.isVisible():
                    return False
                try:
                    if float(fence.windowOpacity()) < 0.99:
                        return False
                except RuntimeError:
                    return False
                hwnd = int(fence.winId()) if fence.winId() else 0
                if not hwnd or not is_attached_to_desktop(hwnd):
                    return False
                if not overlay_win32_visible(fence):
                    return False
            except RuntimeError:
                return False
            except Exception:
                return False
        for icon in list(self.public_icons):
            try:
                if not icon.isVisible():
                    return False
            except RuntimeError:
                return False
        host = getattr(self, "_public_icon_host", None)
        if host is not None and self.public_icons:
            try:
                hwnd = int(host.winId()) if host.winId() else 0
                if not hwnd or not is_attached_to_desktop(hwnd):
                    return False
                if not overlay_win32_visible(host):
                    return False
            except RuntimeError:
                return False
            except Exception:
                return False
        return True

    def _ensure_page_switch_overlays_visible(self) -> None:
        """After page switch / public refresh, show floats without restack storms.

        Normal A↔B clicks only hide/show parked overlays — never soft-attach every
        fence (that flashed the desktop several times per click).
        """
        if self._exiting or self._icons_hidden:
            return
        if self._foreign_app_owns_foreground():
            self._sink_overlays_for_foreign_app()
        if not self._overlay_widgets_may_show():
            return
        # Always verify arriving fences — batch SWP_SHOW can map Win32 while Qt
        # stays hidden; the old "healthy" shortcut skipped sync until desktop click.
        from src.desktop_shell_host import (
            is_attached_to_desktop,
            is_stuck_under_wallpaper,
            set_overlay_hwnd_visible,
        )
        from src.win_shell import overlay_win32_visible

        # Qt visible + Win32 SW_HIDE after batch commit — desktop click used to
        # be the only path that called remap (via FG monitor).
        self._remap_hidden_overlay_hwnds()

        for fence in list(self.fences):
            try:
                if bool(getattr(fence, "_desktidy_soft_parked", False)):
                    continue
                hwnd = self._fence_hwnd(fence)
                if not hwnd:
                    self._reveal_desktop_overlay(fence)
                    continue
                if is_stuck_under_wallpaper(hwnd):
                    self._reveal_desktop_overlay(fence)
                    continue
                if not is_attached_to_desktop(hwnd) or not overlay_win32_visible(
                    fence
                ):
                    self._reveal_desktop_overlay(fence)
                    continue
                if not fence.isVisible():
                    self._sync_qt_visible_after_win32(fence, visible=True)
                else:
                    try:
                        fence.update()
                    except RuntimeError:
                        pass
            except RuntimeError:
                continue
        for icon in list(self.public_icons):
            try:
                if not icon.isVisible():
                    icon.show()
            except RuntimeError:
                continue
        host = getattr(self, "_public_icon_host", None)
        if host is not None:
            try:
                hwnd = int(host.winId()) if host.winId() else 0
            except Exception:
                hwnd = 0
            try:
                if (
                    hwnd
                    and is_attached_to_desktop(hwnd)
                    and not overlay_win32_visible(host)
                ):
                    set_overlay_hwnd_visible(hwnd, True)
                elif hwnd and not is_attached_to_desktop(hwnd):
                    self._configure_public_float_overlay(force=False)
            except Exception:
                pass
        # Raise page chrome only when buried/detached — unconditional HWND_TOP
        # after every A↔B click flashes a solid popup over the wallpaper.
        if self.page_indicator is not None and self.settings.get(
            "show_page_indicator", True
        ):
            if self._page_chrome_needs_raise():
                self._ensure_page_chrome_visible(raise_band=True)

    def next_page(self) -> None:
        pages = self._get_pages()
        if len(pages) <= 1:
            return
        ids = [p.get("id", i) for i, p in enumerate(pages)]
        current = self._current_page()
        try:
            idx = ids.index(current)
            next_id = ids[(idx + 1) % len(ids)]
        except ValueError:
            next_id = ids[0]
        self.switch_page(next_id)

    def prev_page(self) -> None:
        pages = self._get_pages()
        if len(pages) <= 1:
            return
        ids = [p.get("id", i) for i, p in enumerate(pages)]
        current = self._current_page()
        try:
            idx = ids.index(current)
            prev_id = ids[(idx - 1) % len(ids)]
        except ValueError:
            prev_id = ids[0]
        self.switch_page(prev_id)

    def _on_page_create_fence_requested(self, page_id: int) -> None:
        """Page-tab RMB: switch to that page (if needed) then name a new fence."""
        page_id = int(page_id)
        if page_id != self._current_page():
            self.switch_page(page_id)
        from PyQt6.QtGui import QCursor, QGuiApplication

        from src.fence_layout import primary_desktop_rect

        area = primary_desktop_rect()
        try:
            pos = QCursor.pos()
            screen = QGuiApplication.screenAt(pos)
            if screen is not None:
                area = screen.availableGeometry()
        except Exception:
            pass
        # Place toward the work-area center, not under the page bar.
        cx = int(area.x() + area.width() * 0.4)
        cy = int(area.y() + area.height() * 0.35)
        self._create_fence_at(cx, cy)

    def _setup_page_indicator(self) -> None:
        if self.page_indicator:
            self.page_indicator.close()
            self.page_indicator.deleteLater()
            self.page_indicator = None
        if not self.settings.get("show_page_indicator", True):
            return
        from src.desktop_pet import desktop_pet_hosts_float_bar

        # Float bar lives on the pet — do not open a second edge HWND.
        if desktop_pet_hosts_float_bar(self.settings):
            return
        pages = self._get_pages()
        if len(pages) <= 1:
            return
        self.page_indicator = PageIndicatorWidget(
            pages, self._current_page(), settings=self.settings
        )
        # Queued: finish the indicator click before tearing down/rebuilding fences.
        self.page_indicator.page_changed.connect(
            self.switch_page, Qt.ConnectionType.QueuedConnection
        )
        self.page_indicator.create_fence_requested.connect(
            self._on_page_create_fence_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        self.page_indicator.minutes_requested.connect(
            self._on_meeting_minutes_requested, Qt.ConnectionType.QueuedConnection
        )
        self.page_indicator.minutes_folder_requested.connect(
            self._on_meeting_minutes_folder_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        self.page_indicator.folder_open_requested.connect(
            self._on_page_folder_open_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        self.page_indicator.note_requested.connect(
            self._on_note_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        self.page_indicator.note_folder_requested.connect(
            self._on_note_folder_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        self.page_indicator.record_requested.connect(
            self._on_record_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        self.page_indicator.record_folder_requested.connect(
            self._on_record_folder_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        self.page_indicator.calculator_requested.connect(
            self._on_calculator_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        self.page_indicator.todo_requested.connect(
            self._on_todo_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        self.page_indicator.vault_requested.connect(
            self._on_vault_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        self.page_indicator.pet_requested.connect(
            self._on_pet_requested,
            Qt.ConnectionType.QueuedConnection,
        )
        if not self._icons_hidden:
            self.page_indicator.show()
            # Fresh HWND must raise in the desktop band (not sit under DefView).
            self._ensure_page_chrome_visible()

    def _on_record_requested(self) -> None:
        """Desktop「录屏」button: toggle FFmpeg capture."""
        self.screen_record_manager.toggle_recording()

    def _on_calculator_requested(self) -> None:
        """Desktop「计算器」button / hotkey: open or toggle system Calculator."""
        from src.calculator import open_or_toggle_calculator
        from src.i18n import show_warning

        if not open_or_toggle_calculator():
            show_warning(self.window, "计算器", "无法打开系统计算器。")

    def _on_record_folder_requested(self) -> None:
        """Right-click「录屏」→ open the configured recordings folder."""
        import os

        from src.i18n import show_warning
        from src.screen_record_manager import resolve_output_dir

        raw = str(self.settings.get("screen_record", {}).get("output_dir") or "").strip()
        try:
            folder = resolve_output_dir(raw, ensure=True)
            os.startfile(str(folder))  # noqa: S606
        except OSError as exc:
            show_warning(self.window, "录屏", f"无法打开文件夹：{exc}")

    def _on_note_requested(self) -> None:
        """Open standalone deskNote (separate process); no in-process embed."""
        from src.app_logging import get_logger
        from src.desknote_launch import is_desknote_installed, open_in_desknote

        try:
            if not is_desknote_installed():
                get_logger().info("deskNote not installed; ignore note request")
                return
            if not open_in_desknote():
                get_logger().warning("failed to launch deskNote")
        except Exception:
            get_logger().exception("open deskNote failed")

    def open_paths_in_notepad(self, paths: list) -> None:
        """Open filesystem paths in standalone deskNote (N++-style multi-type)."""
        from pathlib import Path

        from src.app_logging import get_logger
        from src.desknote_launch import is_desknote_installed, open_in_desknote
        from src.notepad import is_notepad_openable_path

        clean = [Path(p) for p in paths if p and is_notepad_openable_path(p)]
        if not clean:
            return
        try:
            if not is_desknote_installed():
                get_logger().info("deskNote not installed; skip open_paths")
                return
            open_in_desknote(clean)
        except Exception:
            get_logger().exception("open paths in deskNote failed")
    def _on_notepad_destroyed(self, *_args) -> None:
        self._notepad_window = None
        if self._exiting or self._overlay_restack_blocked():
            return
        self._sync_overlays_to_foreground()
        self._schedule_overlay_keepalive()
        need_force = bool(getattr(self, "_shell_attach_after_settings", False))
        self._shell_attach_after_settings = False
        if not need_force:
            try:
                need_force = bool(self._overlays_need_shell_repair_light())
            except Exception:
                need_force = False
        if need_force:
            self._schedule_force_shell_attach(120)
        else:
            QTimer.singleShot(0, lambda: self._ensure_shell_attachments(force=False))

    def _on_note_folder_requested(self) -> None:
        """Right-click「note」→ open folder that actually holds open notes."""
        import os

        from src.i18n import show_warning
        from src.notepad import resolve_openable_notes_folder

        hints: list = []
        win = getattr(self, "_notepad_window", None)
        if win is not None:
            try:
                pages = list(getattr(win, "_pages", None) or [])
                cur = win._current_page() if hasattr(win, "_current_page") else None
                if cur is not None and getattr(cur, "path", None) is not None:
                    hints.append(cur.path)
                for page in pages:
                    path = getattr(page, "path", None)
                    if path is None:
                        continue
                    if cur is not None and page is cur:
                        continue
                    hints.append(path)
            except RuntimeError:
                hints = []
        folder = resolve_openable_notes_folder(
            self.settings, hint_paths=hints or None, ensure=True
        )
        try:
            os.startfile(str(folder))  # noqa: S606
        except OSError as exc:
            show_warning(self.window, "笔记", f"无法打开文件夹：{exc}")

    def _on_meeting_minutes_requested(self) -> None:
        """Desktop「纪要」button: create dated Word doc in the configured folder."""
        from src.i18n import show_info, show_warning
        from src.meeting_minutes import create_meeting_minutes

        try:
            path = create_meeting_minutes(self.settings, open_after=True)
        except ValueError as exc:
            show_warning(self.window, "会议纪要", str(exc))
            return
        except OSError as exc:
            show_warning(self.window, "会议纪要", f"创建失败：{exc}")
            return
        try:
            self.tray.show_message("会议纪要", f"已创建：{path.name}")
        except Exception:
            show_info(self.window, "会议纪要", f"已创建：\n{path}")

    def _on_meeting_minutes_folder_requested(self) -> None:
        """Right-click「纪要」→ open the configured save folder."""
        from src.i18n import show_warning
        from src.meeting_minutes import open_minutes_folder

        try:
            open_minutes_folder(self.settings)
        except ValueError as exc:
            show_warning(self.window, "会议纪要", str(exc))
        except OSError as exc:
            show_warning(self.window, "会议纪要", f"无法打开文件夹：{exc}")

    def _on_page_folder_open_requested(self, path: str) -> None:
        """Double-click a page-bar folder shortcut → open that path."""
        from src.i18n import show_warning
        from src.page_folders import open_folder_path

        try:
            open_folder_path(path)
        except ValueError as exc:
            show_warning(self.window, "文件夹快捷方式", str(exc))
        except OSError as exc:
            show_warning(self.window, "文件夹快捷方式", f"无法打开文件夹：{exc}")

    def _on_pages_changed(self) -> None:
        pages = self._get_pages()
        current = self._current_page()
        page_ids = {p.get("id", 0) for p in pages}
        if current not in page_ids:
            current = pages[0].get("id", 0)
            self.settings["current_page"] = current
            save_settings(self.settings)
        if self.settings.get("show_page_indicator", True) and len(pages) > 1:
            if self.page_indicator:
                self.page_indicator.update_pages(pages, current)
                if not self._icons_hidden and not self.page_indicator.isVisible():
                    self.page_indicator.show()
            else:
                self._setup_page_indicator()
        elif self.page_indicator:
            self.page_indicator.close()
            self.page_indicator.deleteLater()
            self.page_indicator = None
        pet = getattr(self, "pet_widget", None)
        if pet is not None:
            try:
                pet.reload_pages()
            except RuntimeError:
                pass
        if self._fences_should_show():
            self.show_fences()

    def rebuild_fences(self) -> None:
        if self._fences_should_show():
            self.show_fences(force_rebuild=True)
        self.window.fence_editor.reload_table()
        self.window.page_manager.reload_table()
        # Portal path changes from the editor need watchers rescheduled.
        try:
            self._start_watcher()
        except Exception:
            pass

    def _on_snapshot_restored(self) -> None:
        """Apply a restored DeskTidy layout snapshot to the live desktop."""
        try:
            save_settings(self.settings)
        except OSError:
            pass
        self._apply_theme()
        # Keep settings editors / page tree in sync with restored data.
        try:
            self.window.desktop_layout.reload_all()
        except Exception:
            try:
                self.window.fence_editor.reload_table()
                self.window.page_manager.reload_table()
            except Exception:
                pass
        self._setup_page_indicator()
        if self._fences_should_show():
            self.show_fences(force_rebuild=True, relayout_public=True)
        else:
            self.hide_fences()
        self.refresh_public_desktop(relayout=True, immediate=True)
        try:
            self.window.refresh_data_if_visible()
        except Exception:
            pass

    @staticmethod
    def _fence_painted_pin_keys(fence: FenceWidget) -> list[str]:
        """Casefold keys of icon cells currently in the fence grid."""
        keys: list[str] = []
        layout = getattr(fence, "items_layout", None)
        if layout is None:
            return keys
        for index in range(layout.count()):
            item = layout.itemAt(index)
            widget = item.widget() if item else None
            if widget is None:
                continue
            fp = getattr(widget, "file_path", None)
            if fp is None:
                continue
            try:
                keys.append(str(Path(fp)).casefold())
            except OSError:
                keys.append(str(fp).casefold())
        return keys

    @staticmethod
    def _fence_needs_icon_rebuild(fence: FenceWidget, fence_cfg: dict) -> bool:
        """Whether soft show_fences must rebuild icons (pins / portal / sort)."""
        old = fence.config if isinstance(fence.config, dict) else {}
        old_pins = list(old.get("virtual_items") or [])
        new_pins = list(fence_cfg.get("virtual_items") or [])
        if old_pins != new_pins:
            return True
        if old.get("sort_by") != fence_cfg.get("sort_by"):
            return True
        if is_portal_fence(old) != is_portal_fence(fence_cfg):
            return True
        if get_portal_path(old) != get_portal_path(fence_cfg):
            return True
        if (bool(new_pins) or is_portal_fence(fence_cfg)) and not fence.has_icon_widgets():
            return True
        # Shared settings/config dict:「移动到分页」unpins in-place so old_pins
        # already equals new_pins, but painted cells can still show ghosts.
        if not is_portal_fence(fence_cfg):
            painted = set(DeskTidyApp._fence_painted_pin_keys(fence))
            wanted: set[str] = set()
            for raw in new_pins:
                try:
                    wanted.add(str(Path(str(raw))).casefold())
                except OSError:
                    wanted.add(str(raw).casefold())
            if painted != wanted:
                return True
        return False

    @staticmethod
    def _retire_overlay_widget(widget: QWidget | None) -> None:
        """Hide and neuter an overlay so deleteLater cannot steal the next click."""
        if widget is None:
            return
        try:
            widget.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        except RuntimeError:
            return
        # Drop opacity first — translucent fences otherwise ghost over shell icons
        # for a frame or two while deleteLater is still pending (first quit).
        try:
            widget.setWindowOpacity(0.0)
        except RuntimeError:
            pass
        try:
            widget.hide()
        except RuntimeError:
            pass
        try:
            # Detach HWND immediately so Explorer restore cannot composite both.
            widget.setParent(None)
        except RuntimeError:
            pass
        try:
            widget.close()
        except RuntimeError:
            pass
        try:
            widget.deleteLater()
        except RuntimeError:
            pass

    def _sync_fence_hover_from_cursor(self) -> None:
        """After page rebuild, mirror hover chrome to the real cursor position."""
        pos = QCursor.pos()
        for fence in list(self.fences):
            try:
                over = fence.frameGeometry().contains(pos)
                if bool(getattr(fence, "_hovered", False)) == over:
                    continue
                fence._hovered = over
                fence._update_chrome_visibility()
            except RuntimeError:
                continue

    def show_fences(
        self, *, force_rebuild: bool = False, relayout_public: bool | None = None
    ) -> None:
        started = time.perf_counter()
        page_switch = bool(getattr(self, "_page_switch_ensure_pending", False))
        # switch_page already flushed the leaving page; skip a second sync write.
        if self.fences and not page_switch:
            self._save_fence_layout(immediate=True)

        current_page = self._current_page()
        wanted_cfgs: list[dict] = []
        for fence_cfg in self.settings.get("fences", []):
            if not fence_cfg.get("visible", True):
                continue
            if not fence_on_page(fence_cfg, current_page):
                continue
            wanted_cfgs.append(fence_cfg)

        if not wanted_cfgs and not self.fences:
            self._log_timed("show_fences", started)
            return

        wanted_ids = {cfg.get("id") for cfg in wanted_cfgs}
        existing = {
            fence.config.get("id"): fence
            for fence in self.fences
            if fence.config.get("id")
        }

        # Defer park/retire until after incoming fences are revealed so the
        # desktop never paints an empty wallpaper hole mid-swap.
        to_park: list[tuple[str, FenceWidget]] = []
        to_retire: list[FenceWidget] = []
        for fence_id, fence in list(existing.items()):
            if force_rebuild or fence_id not in wanted_ids:
                if fence in self.fences:
                    self.fences.remove(fence)
                if force_rebuild or not fence_id:
                    to_retire.append(fence)
                else:
                    to_park.append((str(fence_id), fence))
                existing.pop(fence_id, None)

        def _swap_page_overlays() -> tuple[int, int]:
            if force_rebuild:
                self._clear_parked_fences()

            next_fences: list[FenceWidget] = []
            created_new = 0
            restored_fence = 0
            batch = getattr(self, "_page_switch_geo_batch", None) if page_switch else None
            sync = getattr(self, "_page_switch_qt_sync", None) if page_switch else None

            if page_switch:
                # Hide leavers before any incoming batch show (zero-overlap swap).
                for fence in to_retire:
                    self._retire_overlay_widget(fence)
                for fence_id, fence in to_park:
                    self._park_fence(fence_id, fence, soft=True)
                to_retire.clear()
                to_park.clear()

            def _place_fence(fence: FenceWidget, geom: dict) -> None:
                from PyQt6.QtCore import QRect

                x = int(geom["x"])
                y = int(geom["y"])
                w = int(geom["width"])
                h = int(geom["height"])
                target = QRect(x, y, w, h)
                native = None
                try:
                    native = fence.windowHandle()
                except RuntimeError:
                    native = None
                try:
                    if page_switch and native is not None and fence.geometry() == target:
                        return
                except RuntimeError:
                    pass
                # New fences: set Qt size before winId() so the HWND is not born
                # at MIN_WIDTH (工作→文档 first visit). Existing HWNDs stay on
                # DeferWindowPos until freeze syncs Qt (avoids a mid-switch flash).
                if native is None:
                    try:
                        fence.setGeometry(target)
                    except RuntimeError:
                        pass
                    if page_switch:
                        # First-visit HWND is created in
                        # ``_prepare_page_switch_new_fence`` — do not winId()
                        # here (would birth an unattached mapped-capable HWND
                        # before warm-attach + batch show).
                        return
                if batch is not None:
                    hwnd = self._fence_hwnd(fence)
                    if hwnd:
                        batch.set_rect(hwnd, x, y, w, h)
                        if sync is not None:
                            sync.append((fence, x, y, w, h))
                        return
                try:
                    fence.setGeometry(target)
                except RuntimeError:
                    pass

            for fence_cfg in wanted_cfgs:
                fence_id = fence_cfg.get("id")
                geom = get_fence_geometry(self.settings, fence_cfg, current_page)
                fence = existing.get(fence_id) if fence_id else None
                restored_from_park = False
                if fence is None and fence_id:
                    fence = self._take_parked_fence(str(fence_id))
                    restored_from_park = fence is not None
                if fence is not None:
                    fence.settings = self.settings
                    # Parked fences already hold icons — rebuild only if pin list
                    # changed or the grid never painted.
                    need_icons = self._fence_needs_icon_rebuild(fence, fence_cfg)
                    # Page switch: icon grid fills in _refresh_fences_after_page_switch
                    # after batch.commit — refresh here runs inside the freeze and
                    # poisons signatures / leaves 工作页 empty after 文档→工作.
                    fence.apply_config(
                        fence_cfg,
                        refresh_icons=need_icons and not page_switch,
                        apply_style=(not page_switch) or need_icons,
                        defer_icon_refresh=page_switch,
                    )
                    if (
                        page_switch
                        and need_icons
                        and fence_id
                        and not bool(getattr(fence, "_desktidy_defer_refresh", False))
                    ):
                        self._mark_page_switch_fence_force_refresh(str(fence_id))
                    if restored_from_park:
                        restored_fence += 1
                    _place_fence(fence, geom)
                    fence.set_collapsed(
                        bool(geom.get("collapsed", False)),
                        emit=False,
                        layout=not page_switch,
                    )
                    if self._overlay_widgets_may_show():
                        self._reveal_desktop_overlay(fence)
                    self._wire_fence_signals(fence)
                    next_fences.append(fence)
                    continue

                fence = FenceWidget(fence_cfg, self.settings)
                _place_fence(fence, geom)
                fence.set_collapsed(
                    bool(geom.get("collapsed", False)),
                    emit=False,
                    layout=not page_switch,
                )
                self._wire_fence_signals(fence)
                if self._overlay_widgets_may_show():
                    if page_switch:
                        hwnd = self._prepare_page_switch_new_fence(fence)
                        if batch is not None and hwnd:
                            gx = int(geom["x"])
                            gy = int(geom["y"])
                            gw = int(geom["width"])
                            gh = int(geom["height"])
                            batch.set_rect(hwnd, gx, gy, gw, gh)
                            if sync is not None:
                                sync.append((fence, gx, gy, gw, gh))
                            batch.show(hwnd)
                        else:
                            self._reveal_desktop_overlay(fence)
                    else:
                        self._reveal_desktop_overlay(fence)
                next_fences.append(fence)
                created_new += 1

            self.fences = next_fences
            # Heal leftover opacity-0 soft-park from older builds (icon-click flash).
            for fence in next_fences:
                try:
                    if float(fence.windowOpacity()) < 0.99:
                        fence.setWindowOpacity(fence._target_opacity())
                    if bool(getattr(fence, "_desktidy_soft_parked", False)):
                        # Still soft after reveal — finish unpark via owner path.
                        self._soft_show_fence_for_page(fence)
                except RuntimeError:
                    continue
            # Park/retire leaving page only after arriving overlays are shown.
            for fence in to_retire:
                self._retire_overlay_widget(fence)
            for fence_id, fence in to_park:
                self._park_fence(fence_id, fence, soft=page_switch)

            self._displayed_page = current_page
            if self.peek_manager.active:
                self.peek_manager.reapply()
            self._fences_hidden = False

            if clear_ephemeral_public_items(self.settings):
                try:
                    save_settings(self.settings)
                except OSError:
                    pass

            # Soft page switch: reuse parked floats at saved positions (no jump).
            # Creating fence HWNDs on first visit must NOT relayout floats.
            if relayout_public is None:
                do_relayout = bool(force_rebuild)
            else:
                do_relayout = bool(relayout_public)
            # Page click: sync floats in the same paint freeze (not +200ms later).
            if page_switch:
                self.refresh_public_desktop(immediate=True, relayout=do_relayout)
            else:
                self.refresh_public_desktop(relayout=do_relayout)
            return created_new, restored_fence

        if page_switch and getattr(self, "_page_switch_freeze_depth", 0) == 0:
            with self._page_switch_paint_freeze():
                created_new, restored_fence = _swap_page_overlays()
        elif page_switch:
            created_new, restored_fence = _swap_page_overlays()
        else:
            created_new, restored_fence = _swap_page_overlays()

        # New HWNDs need attach. Parked↔shown fences keep DefView ownership —
        # force-restack on every page click was the multi-flash.
        # Page-switch first-create already warm-attached + batch-shown above —
        # a deferred ensure_shell_attachments here re-ShowWindow'd and flashed.
        # Page switch: batch.commit in freeze finally must run before any
        # HWND_BOTTOM sink — pre-commit sink left first-visit fences unmapped.
        if not page_switch:
            if self._desk_app_ui_open() and self._desk_app_owns_foreground():
                self._keep_overlays_under_apps()
                # Settings-side rebuild/delete can create fresh fence HWNDs while
                # the app window is open. Sink them for now, but force one
                # desktop-shell reattach after the settings UI closes so the
                # remaining fences do not stay buried until a page switch.
                if force_rebuild or created_new > 0 or restored_fence > 0:
                    self._shell_attach_after_settings = True
            elif self._foreign_app_owns_foreground():
                self._sink_overlays_for_foreign_app()
            elif force_rebuild:
                QTimer.singleShot(
                    0,
                    lambda: self._ensure_shell_attachments(force=True),
                )
            elif created_new > 0:
                QTimer.singleShot(
                    0, lambda: self._ensure_shell_attachments(force=False)
                )
            elif restored_fence > 0:
                QTimer.singleShot(
                    0, lambda: self._ensure_desktop_overlays_visible(force=False)
                )
            QTimer.singleShot(0, self._sync_fence_hover_from_cursor)
            self._schedule_overlay_keepalive()
            self._finish_page_switch_overlays_if_pending()
        self._log_timed("show_fences", started)

    def _on_fence_files_changed(self, fence: FenceWidget) -> None:
        """Refresh only the fence that changed — not every overlay on desktop."""
        if self._exiting:
            return
        fence.settings = self.settings
        if self._fences_should_show():
            fence.refresh()
        self.refresh_public_desktop(relayout=False)

    def _wire_fence_signals(self, fence: FenceWidget) -> None:
        if getattr(fence, "_desktidy_signals_wired", False):
            return
        fence._desktidy_signals_wired = True
        fence.geometry_changed.connect(self._save_fence_layout)
        fence.files_changed.connect(
            lambda f=fence: self._on_fence_files_changed(f),
            Qt.ConnectionType.QueuedConnection,
        )
        fence.public_items_changed.connect(
            self._on_fence_public_items_changed,
            Qt.ConnectionType.QueuedConnection,
        )
        fence.hide_requested.connect(
            lambda f=fence: self._on_fence_hide_requested(f)
        )
        fence.page_move_requested.connect(
            lambda page_id, f=fence: self._move_fence_to_page(f, page_id)
        )
        # Queued: run after the fence context menu closes (avoids nested modal
        # + public refresh while _desktop_popup_open blocks shell attach).
        fence.dissolve_requested.connect(
            lambda f=fence: self._on_fence_dissolve_requested(f),
            Qt.ConnectionType.QueuedConnection,
        )

    def _move_fence_to_page(self, fence: FenceWidget, target_page: int) -> None:
        """Move a fence to another desktop page; keep content and size/position."""
        target_page = int(target_page)
        page_ids = {int(p.get("id", 0)) for p in self._get_pages()}
        if target_page not in page_ids:
            return

        from src.system_defaults import is_locked_fence, locked_fence_home_page

        home = locked_fence_home_page(fence.config)
        if home is not None and target_page != home:
            from src.i18n import show_warning

            show_warning(
                None,
                "提示",
                f"「{fence.config.get('name') or '系统分区'}」为系统默认分区，"
                "必须保留在对应分页。",
            )
            return

        if is_locked_fence(fence.config) and home is None:
            # Unknown locked fence: keep previous work-page restriction.
            from src.system_defaults import SYSTEM_WORK_PAGE_ID

            if target_page != SYSTEM_WORK_PAGE_ID:
                from src.i18n import show_warning

                show_warning(
                    None,
                    "提示",
                    "系统默认分区必须保留在对应分页。",
                )
                return

        current_pages = get_fence_pages(fence.config)
        if current_pages == [target_page]:
            return

        # Capture live geometry so size/position stay the same on the new page.
        geo = fence.get_config_update()
        current_page = self._current_page()
        save_fence_geometry(self.settings, fence.config, current_page, geo)
        save_fence_geometry(self.settings, fence.config, target_page, geo)
        for key in ("x", "y", "width", "height", "collapsed"):
            if key in geo:
                fence.config[key] = geo[key]

        # Exclusive membership: content (virtual_items) stays on the fence config.
        set_fence_pages(fence.config, [target_page])
        fid = fence.config.get("id")
        for cfg in self.settings.get("fences", []):
            if cfg.get("id") == fid:
                if cfg is not fence.config:
                    set_fence_pages(cfg, [target_page])
                    for key in ("x", "y", "width", "height", "collapsed"):
                        if key in geo:
                            cfg[key] = geo[key]
                break

        save_settings(self.settings)
        self.window.desktop_layout.reload_table()
        self.window.fence_editor.reload_table()
        # Switch view so the fence stays visible after leaving the old page.
        self.switch_page(target_page)

    def _preview_fence_style(self, visual: dict) -> None:
        """Live-preview paint keys on all mapped fence widgets (unsaved)."""
        from src.fence_style import merge_fence_style_visual

        if not isinstance(visual, dict):
            return
        for fence in list(getattr(self, "fences", None) or ()):
            try:
                # Preview only mutates a detached widget paint cache — never
                # settings / fence.config (shared refs), so re-picks always work.
                style = merge_fence_style_visual(getattr(fence, "_style", None), visual)
                fence._style = dict(style)
                fence._apply_style()
            except RuntimeError:
                continue
        for parked in list(getattr(self, "_parked_fences", {}).values()):
            try:
                style = merge_fence_style_visual(getattr(parked, "_style", None), visual)
                parked._style = dict(style)
                parked._apply_style()
            except RuntimeError:
                continue
        # Style-only: do not restack (SWP_NOREDRAW discards the just-applied QSS).
        for fence in list(getattr(self, "fences", None) or ()):
            flush = getattr(fence, "_flush_style_paint", None)
            if callable(flush):
                try:
                    flush()
                except RuntimeError:
                    continue

    def _apply_fence_style_preset(self, preset_id: str) -> None:
        """Persist preset onto every fence config and refresh live widgets."""
        from src.fence_style import apply_preset_to_settings, normalize_fence_style_preset
        from src.settings import save_settings

        key = normalize_fence_style_preset(preset_id)
        apply_preset_to_settings(self.settings, key)
        save_settings(self.settings)
        by_id = {
            str(cfg.get("id")): cfg
            for cfg in self.settings.get("fences", [])
            if isinstance(cfg, dict) and cfg.get("id")
        }
        for fence in list(getattr(self, "fences", None) or ()):
            try:
                cfg = by_id.get(str(fence.config.get("id")))
                if cfg is None:
                    continue
                # Keep config pointing at the saved dict so later edits persist.
                fence.config = cfg
                fence.apply_config(cfg, refresh_icons=False, apply_style=True)
            except RuntimeError:
                continue
        for parked in list(getattr(self, "_parked_fences", {}).values()):
            try:
                cfg = by_id.get(str(parked.config.get("id")))
                if cfg is None:
                    continue
                parked.config = cfg
                parked.apply_config(
                    cfg, refresh_icons=False, apply_style=True, defer_icon_refresh=True
                )
            except RuntimeError:
                continue
        # Style-only: flush paint without restack so translucent panels keep color.
        for fence in list(getattr(self, "fences", None) or ()):
            flush = getattr(fence, "_flush_style_paint", None)
            if callable(flush):
                try:
                    flush()
                except RuntimeError:
                    continue

    def _apply_fence_style_preset_one(self, fence_id: str, preset_id: str) -> None:
        """Refresh one live fence after its card applied a catalog preset."""
        # AIGC START
        from src.fence_style import apply_preset_to_fence_config, normalize_fence_style_preset
        from src.settings import save_settings

        key = normalize_fence_style_preset(preset_id)
        fid = str(fence_id or "")
        target_cfg = None
        for cfg in self.settings.get("fences", []):
            if isinstance(cfg, dict) and str(cfg.get("id") or "") == fid:
                # Settings may already be updated by the layout card; ensure merge.
                apply_preset_to_fence_config(cfg, key)
                target_cfg = cfg
                break
        if target_cfg is None:
            return
        save_settings(self.settings)
        for fence in list(getattr(self, "fences", None) or ()):
            try:
                if str(fence.config.get("id") or "") != fid:
                    continue
                fence.config = target_cfg
                fence.apply_config(target_cfg, refresh_icons=False, apply_style=True)
                flush = getattr(fence, "_flush_style_paint", None)
                if callable(flush):
                    flush()
            except RuntimeError:
                continue
        for parked in list(getattr(self, "_parked_fences", {}).values()):
            try:
                if str(parked.config.get("id") or "") != fid:
                    continue
                parked.config = target_cfg
                parked.apply_config(
                    target_cfg,
                    refresh_icons=False,
                    apply_style=True,
                    defer_icon_refresh=True,
                )
            except RuntimeError:
                continue
        # AIGC END

    def _apply_fence_opacity_one(self, fence_id: str, opacity: float) -> None:
        """Push per-fence opacity from layout card slider to live overlays."""
        fid = str(fence_id or "")
        try:
            opacity = max(0.35, min(1.0, float(opacity)))
        except (TypeError, ValueError):
            return
        target_cfg = None
        for cfg in self.settings.get("fences", []):
            if not isinstance(cfg, dict) or str(cfg.get("id") or "") != fid:
                continue
            style = cfg.get("style") if isinstance(cfg.get("style"), dict) else {}
            if not isinstance(cfg.get("style"), dict):
                cfg["style"] = style
            style = cfg["style"]
            style["opacity"] = opacity
            target_cfg = cfg
            break
        if target_cfg is None:
            return
        for fence in list(getattr(self, "fences", None) or ()):
            try:
                if str(fence.config.get("id") or "") != fid:
                    continue
                fence.config = target_cfg
                fence.apply_config(target_cfg, refresh_icons=False, apply_style=True)
                flush = getattr(fence, "_flush_style_paint", None)
                if callable(flush):
                    flush()
            except RuntimeError:
                continue
        for parked in list(getattr(self, "_parked_fences", {}).values()):
            try:
                if str(parked.config.get("id") or "") != fid:
                    continue
                parked.config = target_cfg
                parked.apply_config(
                    target_cfg,
                    refresh_icons=False,
                    apply_style=True,
                    defer_icon_refresh=True,
                )
            except RuntimeError:
                continue

    def _revert_fence_style_preview(self) -> None:
        """Drop unsaved preview — restore paint from persisted fence configs."""
        by_id = {
            str(cfg.get("id")): cfg
            for cfg in self.settings.get("fences", [])
            if isinstance(cfg, dict) and cfg.get("id")
        }
        for fence in list(getattr(self, "fences", None) or ()):
            try:
                cfg = by_id.get(str(fence.config.get("id")))
                if cfg is None:
                    continue
                fence.apply_config(cfg, refresh_icons=False, apply_style=True)
            except RuntimeError:
                continue
        for fence in list(getattr(self, "fences", None) or ()):
            flush = getattr(fence, "_flush_style_paint", None)
            if callable(flush):
                try:
                    flush()
                except RuntimeError:
                    continue

    def _apply_fence_visibility(self, fence_id: str, visible: bool) -> None:
        """Show/hide one fence without force-rebuilding every live overlay."""
        from PyQt6.QtCore import QRect

        from src.fence_layout import get_fence_geometry
        from src.fence_pages import fence_on_page

        fence_cfg = next(
            (
                cfg
                for cfg in self.settings.get("fences", [])
                if cfg.get("id") == fence_id
            ),
            None,
        )
        if fence_cfg is None:
            return

        fence_cfg["visible"] = bool(visible)
        current_page = self._current_page()
        on_current = fence_on_page(fence_cfg, current_page)
        live = next(
            (fence for fence in self.fences if fence.config.get("id") == fence_id),
            None,
        )

        if not visible:
            if live is not None:
                live.config["visible"] = False
                self.fences.remove(live)
                self._retire_overlay_widget(live)
            parked = self._parked_fences.pop(str(fence_id), None)
            if parked is not None and parked is not live:
                self._retire_overlay_widget(parked)
        elif on_current and self._fences_should_show():
            if live is not None:
                live.config["visible"] = True
            else:
                geom = get_fence_geometry(self.settings, fence_cfg, current_page)
                fence = self._take_parked_fence(str(fence_id))
                if fence is None:
                    fence = FenceWidget(fence_cfg, self.settings)
                else:
                    fence.settings = self.settings
                    fence.apply_config(
                        fence_cfg,
                        refresh_icons=self._fence_needs_icon_rebuild(fence, fence_cfg),
                    )
                try:
                    fence.setGeometry(
                        QRect(
                            int(geom["x"]),
                            int(geom["y"]),
                            int(geom["width"]),
                            int(geom["height"]),
                        )
                    )
                except RuntimeError:
                    pass
                fence.set_collapsed(bool(geom.get("collapsed", False)), emit=False)
                self._wire_fence_signals(fence)
                self.fences.append(fence)
                # Settings open: may-show is False, but DefView ownership still
                # keeps overlays under the app window — must map, not defer.
                if self._overlay_widgets_may_show() or self._desk_app_ui_open():
                    self._reveal_desktop_overlay(fence)
                if self._desk_app_ui_open():
                    self._shell_attach_after_settings = True

        try:
            save_settings(self.settings)
        except OSError:
            pass
        # Desktop layout refreshes the one row surgically after emit returns.
        if self._desk_app_ui_open() and self._desk_app_owns_foreground() and self._fences_should_show():
            self._keep_overlays_under_apps()
        elif visible and on_current and self._fences_should_show():
            QTimer.singleShot(
                0, lambda: self._ensure_shell_attachments(force=False)
            )

    def _on_fence_hide_requested(self, fence: FenceWidget) -> None:
        """User closed a fence with × — persist hidden and drop from live list."""
        fid = fence.config.get("id")
        fence.config["visible"] = False
        for cfg in self.settings.get("fences", []):
            if cfg.get("id") == fid:
                cfg["visible"] = False
                break
        if fence in self.fences:
            self.fences.remove(fence)
        self._retire_overlay_widget(fence)
        save_settings(self.settings)
        self.window.fence_editor.reload_table()

    def _on_fence_dissolve_requested(self, fence: FenceWidget) -> None:
        """Right-click dissolve: delete fence; pins become current-page floats."""
        from src.fence_rules import dissolve_fence_to_page
        from src.i18n import ask_yes_no

        # Menu freeze would block shell re-attach for new floats → icons only
        # appear after the next page switch. Clear it before creating overlays.
        while int(getattr(self, "_desktop_popup_depth", 0)) > 0:
            self._end_desktop_popup()

        try:
            gone = fence is None or fence.config is None
        except RuntimeError:
            gone = True
        if gone:
            return

        name = str(fence.config.get("name") or "分区")
        from src.system_defaults import is_locked_fence

        if is_locked_fence(fence.config):
            from src.i18n import show_warning

            show_warning(None, "提示", f"「{name}」为系统默认分区，不能解散。")
            return
        if not ask_yes_no(
            None,
            "确认解散",
            f"确定解散分区「{name}」？\n分区将删除，其中的图标会放到当前分页。",
        ):
            return
        page_id = self._current_page()
        # Save other fences' geometry first; dissolving fence is about to vanish.
        self._save_fence_layout(immediate=True)
        # Drop the live fence before slot layout so floats are not placed under it.
        cfg = dict(fence.config) if isinstance(fence.config, dict) else {}
        if fence in self.fences:
            self.fences.remove(fence)
        self._retire_overlay_widget(fence)

        dissolve_fence_to_page(self.settings, cfg, page_id)
        try:
            save_settings(self.settings)
        except OSError:
            pass
        self._last_public_sync_sig = None
        self._public_force_relayout = True
        self.refresh_public_desktop(relayout=True, immediate=True)
        self._ensure_shell_attachments(force=True)
        self.window.desktop_layout.reload_table()
        self.window.fence_editor.reload_table()
        self.window.page_manager.reload_table()
        self._schedule_overlay_keepalive()
        self._keep_overlays_under_apps()
        # Menu dismiss may still be settling — one deferred attach after menu quiet.
        QTimer.singleShot(300, self._ensure_dissolve_floats_visible)

    def _ensure_dissolve_floats_visible(self) -> None:
        """After dissolve, force floating icons to show + attach if still missing."""
        if self._exiting or self._icons_hidden:
            return
        if not self._overlay_widgets_may_show():
            return
        page_id = self._current_page()
        wanted = visible_floating_items(self.settings, page_id)
        if not wanted:
            return
        have = {str(icon.file_path).casefold() for icon in self.public_icons}
        missing = False
        for entry in wanted:
            try:
                key = str(entry.get("path", "")).casefold()
            except OSError:
                key = str(entry.get("path", "")).casefold()
            if key and key not in have:
                missing = True
                break
            # Widget exists but HWND may still be stuck / hidden.
        if missing or len(self.public_icons) < len(wanted):
            self._last_public_sync_sig = None
            self.refresh_public_desktop(relayout=True, immediate=True)
        for icon in list(self.public_icons):
            try:
                if not icon.isVisible():
                    icon.show()
            except RuntimeError:
                continue
        self._configure_public_float_overlay(force=True)
        self._ensure_shell_attachments(force=True)

    def hide_fences(self) -> None:
        # Persist geometry/collapse before tearing down (double-click hide, toggles).
        if self.fences:
            self._save_fence_layout(immediate=True)
        for fence in self.fences:
            self._retire_overlay_widget(fence)
        self.fences.clear()
        self._clear_parked_fences()
        # Public icons stay — they are page-independent — unless icons are fully hidden.

    def toggle_fences(self, visible: bool) -> None:
        self.settings["show_fences"] = visible
        save_settings(self.settings)
        self._sync_fences_checkbox()
        if visible:
            self._fences_hidden = False
            if self._fences_should_show():
                self.show_fences()
        else:
            self.hide_fences()
            self._fences_hidden = False

    def _toggle_fences_visibility(self) -> None:
        new_val = not bool(self.settings.get("show_fences", True))
        self.toggle_fences(new_val)

    def _toggle_icons_and_fences(self) -> None:
        visible = not are_desktop_icons_visible()
        # Shell icons and fence overlays must not both show the same items —
        # translucent panels would look like duplicated / ghost icons.
        if visible:
            self._show_stock_desktop_view()
        else:
            self._show_organized_desktop_view()
        self._schedule_overlay_keepalive()

    def _show_stock_desktop_view(self) -> None:
        """Stock Explorer desktop: shell icons on, DeskTidy overlays off."""
        try:
            self._clear_public_icons()
        except Exception:
            pass
        try:
            self.hide_fences()
        except Exception:
            pass
        self._fences_hidden = True
        if self.page_indicator:
            try:
                self.page_indicator.hide()
            except Exception:
                pass
        if self.dock:
            try:
                self.dock.hide()
            except Exception:
                pass
        todo = getattr(self, "todo_panel", None)
        if todo is not None:
            try:
                todo.hide()
            except Exception:
                pass
        vault = getattr(self, "vault_panel", None)
        if vault is not None:
            try:
                vault.hide()
            except Exception:
                pass
        launcher = getattr(self, "vault_launcher", None)
        if launcher is not None:
            try:
                launcher.hide()
            except Exception:
                pass
        pet = getattr(self, "pet_widget", None)
        if pet is not None:
            try:
                pet.hide()
            except Exception:
                pass
        self._flush_overlay_teardown()
        try:
            reveal_hosted_namespace_icons()
        except Exception:
            pass
        set_desktop_icons_visible(True)
        self.settings["hide_shell_icons"] = False
        try:
            save_settings(self.settings)
        except OSError:
            pass
        self._icons_hidden = False

    def _show_organized_desktop_view(self) -> None:
        """DeskTidy session: shell icons off, fences / floats on."""
        set_desktop_icons_visible(False)
        self.settings["hide_shell_icons"] = True
        try:
            save_settings(self.settings)
        except OSError:
            pass
        self._icons_hidden = False
        self._fences_hidden = False
        if self.settings.get("show_fences", True):
            self.show_fences()
        if self.settings.get("dock", {}).get("enabled"):
            if self.dock:
                self.dock.show()
            else:
                self._setup_dock()
        if self.settings.get("show_page_indicator", True):
            if self.page_indicator:
                self.page_indicator.show()
            else:
                self._setup_page_indicator()
        from src.todos import desktop_todos_enabled

        if desktop_todos_enabled(self.settings):
            todo = getattr(self, "todo_panel", None)
            if todo is not None:
                try:
                    todo.show()
                except RuntimeError:
                    self._setup_todo_panel()
            else:
                self._setup_todo_panel()
        from src.account_vault import account_vault_enabled

        if account_vault_enabled(self.settings):
            # Panel stays hidden until float click; launcher is always shown.
            if getattr(self, "vault_panel", None) is None or getattr(
                self, "vault_launcher", None
            ) is None:
                self._setup_account_vault_panel()
            else:
                launcher = getattr(self, "vault_launcher", None)
                if launcher is not None:
                    try:
                        launcher.show()
                        launcher.raise_()
                    except RuntimeError:
                        self._setup_account_vault_panel()
        else:
            existing = getattr(self, "vault_panel", None)
            if existing is not None:
                try:
                    existing.close()
                    existing.deleteLater()
                except RuntimeError:
                    pass
                self.vault_panel = None
            launcher = getattr(self, "vault_launcher", None)
            if launcher is not None:
                try:
                    launcher.close()
                    launcher.deleteLater()
                except RuntimeError:
                    pass
                self.vault_launcher = None
        from src.desktop_pet import desktop_pet_visible

        if desktop_pet_visible(self.settings):
            pet = getattr(self, "pet_widget", None)
            if pet is not None:
                try:
                    pet.show_pet()
                except RuntimeError:
                    self._setup_desktop_pet()
            else:
                self._setup_desktop_pet()
        self.refresh_public_desktop(relayout=False)

    def _on_desktop_double_click(self) -> None:
        if not self.settings.get("double_click_hide", True):
            return
        icons_visible = are_desktop_icons_visible()
        if icons_visible:
            # Hide shell icons only; fences stay (Fences-like).
            self._show_organized_desktop_view()
        else:
            # Reveal stock desktop — hide overlays first to avoid ghost duplicates.
            self._show_stock_desktop_view()
        self._schedule_overlay_keepalive()

    def _clear_desktop_item_selection(self) -> None:
        for fence in list(self.fences):
            try:
                fence.clear_item_selection()
            except Exception:
                pass
        host = getattr(self, "_public_icon_host", None)
        if host is not None:
            try:
                host.clear_item_selection()
            except Exception:
                pass

    def _on_desktop_blank_click(self) -> None:
        if self._exiting:
            return
        # Do not clear while a public rubber-band is active (release can land on
        # wallpaper after the cursor leaves the host mask mid-gesture).
        host = getattr(self, "_public_icon_host", None)
        if host is not None and getattr(host, "_marquee_origin", None) is not None:
            return
        # Layered alpha holes can make WindowFromPoint report DefView while the
        # cursor is still on our icon — blank-clear then eats the first select.
        try:
            from src.ui.fence_icon_item import _cursor_over_icon_item

            if _cursor_over_icon_item():
                return
        except Exception:
            pass
        self._clear_desktop_item_selection()

    def _start_fence_region_select(
        self,
        x: int | None = None,
        y: int | None = None,
        end_x: int | None = None,
        end_y: int | None = None,
    ) -> None:
        if not self.settings.get("desktop_right_click_menu", True):
            return
        self._fence_region.start(x, y, end_x, end_y)

    def _sync_desktop_shell_integration(self) -> None:
        """Register Explorer menu verbs + click recorder + IPC host when enabled."""
        enabled = bool(self.settings.get("desktop_right_click_menu", True))
        try:
            sync_desktop_background_verbs(enabled=enabled)
        except Exception:
            get_logger().exception("sync desktop background shell verbs failed")
        # Always host IPC so uninstaller can send quit-silent even if menu is off.
        try:
            if start_shell_ipc_host(self._on_shell_verb):
                get_logger().info("shell IPC host ready")
            else:
                get_logger().error("shell IPC host failed to start")
        except Exception:
            get_logger().exception("shell IPC host start failed")
        if enabled:
            try:
                self.desktop_context.start()
            except Exception:
                get_logger().exception("desktop click recorder start failed")
            pending = getattr(self, "_pending_shell_verb", None)
            if pending:
                self._pending_shell_verb = None
                QTimer.singleShot(0, lambda v=pending: self._on_shell_verb(v, 0, 0))
        else:
            try:
                self.desktop_context.stop()
            except Exception:
                pass
            pending = getattr(self, "_pending_shell_verb", None)
            if pending:
                self._pending_shell_verb = None
                QTimer.singleShot(0, lambda v=pending: self._on_shell_verb(v, 0, 0))

    def _on_shell_verb(self, verb: str, x: int = 0, y: int = 0) -> None:
        """Handle Explorer background-menu verbs (via IPC or cold start)."""
        verb = (verb or "").strip().lower()
        if not verb:
            return
        if verb in {"quit-silent", "uninstall-quit"}:
            # Uninstaller / cleanup asks the live UI to exit without a prompt.
            QTimer.singleShot(0, lambda: self.quit(silent=True))
            return
        # deskNote.lnk / --notepad while main UI is running.
        from src.desknote import OPEN_NOTEPAD_VERB

        if verb == OPEN_NOTEPAD_VERB or verb == "open-notepad":
            QTimer.singleShot(0, self._on_note_requested)
            return
        if not self.settings.get("desktop_right_click_menu", True):
            return
        # Prefer last desktop blank click (hook); payload/cursor only as fallback.
        rx, ry = get_last_desktop_right_click()
        if (rx, ry) == (0, 0) and (x or y):
            rx, ry = int(x), int(y)
        get_logger().info("shell verb=%s at %s,%s", verb, rx, ry)
        if verb == "new-fence":
            self._create_fence_at(rx, ry)
        elif verb == "refresh-fences":
            self.force_refresh_desktop()
        elif verb == "show-window":
            self.tray.show_window()

    def _iter_live_fences_for_placement(self) -> list:
        """Current-page widgets plus parked off-page ones (real on-screen rects)."""
        widgets = list(self.fences)
        seen = {id(w) for w in widgets}
        for parked in getattr(self, "_parked_fences", {}).values():
            if id(parked) in seen:
                continue
            widgets.append(parked)
            seen.add(id(parked))
        return widgets

    def _create_fence_in_rect(self, rect: QRect) -> None:
        width = max(rect.width(), 160)
        height = max(rect.height(), 180)
        x, y, width, height = place_new_fence_rect(
            self.settings,
            self._current_page(),
            width=width,
            height=height,
            hint_x=rect.center().x(),
            hint_y=rect.center().y(),
            anchor_x=rect.x(),
            anchor_y=rect.y(),
            live_fences=self._iter_live_fences_for_placement(),
        )
        self._create_fence_with_geometry(x, y, width, height)

    def _create_fence_at(self, x: int, y: int) -> None:
        nx, ny, width, height = place_new_fence_rect(
            self.settings,
            self._current_page(),
            hint_x=x,
            hint_y=y,
            live_fences=self._iter_live_fences_for_placement(),
        )
        self._create_fence_with_geometry(nx, ny, width, height)

    def _create_fence_with_geometry(self, x: int, y: int, width: int, height: int) -> None:
        self._begin_desktop_popup()
        try:
            dialog = QInputDialog(self.window)
            dialog.setWindowTitle("新建分区")
            dialog.setLabelText("分区名称：")
            dialog.setTextValue("新分区")
            dialog.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
            dialog.raise_()
            dialog.activateWindow()
            if not dialog.exec():
                return
            name = dialog.textValue().strip()
            if not name:
                return

            fences = self.settings.setdefault("fences", [])
            existing_names = {f.get("name") for f in fences}
            base_name = name
            counter = 1
            while name in existing_names:
                name = f"{base_name}{counter}"
                counter += 1

            config = {
                "id": uuid.uuid4().hex[:8],
                "name": name,
                "folder": name,
                "sort_by": "name",
                "x": x,
                "y": y,
                "width": width,
                "height": height,
                "visible": True,
                "style": fence_theme_defaults(self.settings.get("theme")),
            }
            set_fence_pages(config, [self._current_page()])
            fences.append(config)
            page_id = self._current_page()
            save_fence_geometry(
                self.settings,
                config,
                page_id,
                {
                    "x": config["x"],
                    "y": config["y"],
                    "width": config["width"],
                    "height": config["height"],
                    "collapsed": False,
                },
            )
            save_settings(self.settings)
            self.window.fence_editor.reload_table()
            if not self._icons_hidden:
                self.settings["show_fences"] = True
                self._fences_hidden = False
                save_settings(self.settings)
                self._sync_fences_checkbox()
                self.show_fences()
            self.tray.show_message(APP_NAME_ZH, f"已创建分区「{name}」")
        finally:
            self._end_desktop_popup()

    def _current_fence_layout_signature(self, page: int) -> tuple:
        items: list[tuple] = []
        for fence in self.fences:
            geo = fence.geometry()
            items.append(
                (
                    fence.config.get("id"),
                    geo.x(),
                    geo.y(),
                    geo.width(),
                    geo.height(),
                    bool(getattr(fence, "_collapsed", False)),
                    round(float(getattr(fence, "_icon_zoom", 1.0)), 3),
                )
            )
        return (page, tuple(items))

    def _save_fence_layout(self, *, immediate: bool = False) -> None:
        if not self.fences:
            return
        if immediate:
            self._flush_fence_layout_save()
            return
        self._save_fence_layout_timer.start(400)

    def _flush_fence_layout_save(self, *, persist: bool = True) -> None:
        if getattr(self, "_suppress_fence_layout_save", False):
            return
        if not self.fences:
            return
        page = (
            self._displayed_page
            if self._displayed_page is not None
            else self._current_page()
        )
        sig = self._current_fence_layout_signature(page)
        if sig == self._last_saved_fence_layout_sig:
            return
        save_visible_fences(self.settings, self.fences, page)
        if persist:
            save_settings(self.settings)
        self._last_saved_fence_layout_sig = sig

    def _on_theme_changed(self) -> None:
        """Apply UI stylesheet only — do not refresh fence contents / public layout."""
        self.settings["theme"] = normalize_theme(self.settings.get("theme"))
        self._apply_theme()
        self._sync_window_icons()
        self.tray.apply_feature_visibility(self.settings)

    def _sync_window_icons(self) -> None:
        from src.ui.styles import get_theme_palette

        accent = get_theme_palette(self.settings.get("theme"))["accent"]
        icon = get_app_icon(accent)
        if icon.isNull():
            return
        self.qt_app.setWindowIcon(icon)
        try:
            self.window.setWindowIcon(icon)
        except RuntimeError:
            pass
        win = getattr(self, "_notepad_window", None)
        if win is not None:
            try:
                if hasattr(win, "apply_window_icon"):
                    win.apply_window_icon(accent)
                else:
                    win.setWindowIcon(icon)
            except RuntimeError:
                pass

    def _on_settings_changed(self) -> None:
        self.settings["theme"] = normalize_theme(self.settings.get("theme"))
        self._apply_theme()
        if hasattr(self.window, "_sync_theme_combo"):
            self.window._sync_theme_combo()
        if hasattr(self.window, "_apply_sidebar_accent"):
            self.window._apply_sidebar_accent()
        self._sync_window_icons()
        self.tray.apply_feature_visibility(self.settings)

        if self.settings.get("double_click_hide", True):
            self.desktop_monitor.start()
        else:
            self.desktop_monitor.stop()

        self._sync_desktop_shell_integration()

        self.watcher.stop()
        self._start_watcher()

        self._setup_page_indicator()
        self._sync_fences_checkbox()

        if not self.settings.get("show_fences", True):
            self.hide_fences()
            self.refresh_public_desktop(relayout=True)
            return

        if self._fences_should_show():
            # Soft update — avoid destroy/recreate flicker on option toggles.
            if self.fences:
                cfg_by_id = {
                    cfg.get("id"): cfg for cfg in self.settings.get("fences", [])
                }
                for fence in self.fences:
                    fence.settings = self.settings
                    cfg = cfg_by_id.get(fence.config.get("id"))
                    if cfg is not None:
                        fence.apply_config(cfg)
                    else:
                        fence.refresh()
            else:
                self.show_fences()
        self._ensure_native_system_namespace_icons()
        self.refresh_public_desktop(relayout=True)

    def refresh_fences(self, *, refresh_public: bool = False, force: bool = False) -> None:
        if self._exiting:
            return
        if refresh_public:
            self._refresh_public_from_fences_pending = True
        if force:
            self._refresh_fences_force = True
        self._refresh_fences_timer.start(200)

    def force_refresh_desktop(self) -> None:
        """User-facing refresh (shell menu / fence RMB): clear caches and rebuild UI."""
        if self._exiting:
            return
        from src.icon_utils import invalidate_file_icon_cache
        from src.settings import get_desktop_paths
        from src.win_shell import refresh_desktop

        get_logger().info("force_refresh_desktop begin")
        invalidate_desktop_scan_cache()
        # Path-scoped invalidation for visible overlays — avoids clearing the
        # whole shell icon cache on every user refresh.
        try:
            self._invalidate_visible_icon_caches()
        except Exception:
            invalidate_file_icon_cache()
        self._last_public_sync_sig = None
        self._loose_sync_needed = True
        self._public_force_relayout = True
        for icon in list(self.public_icons):
            try:
                icon._reload_icon()
                icon._reload_label()
            except Exception:
                pass
        # Run immediately so the menu action feels responsive (no 80ms coalesce).
        self._refresh_fences_force = True
        self._refresh_public_from_fences_pending = True
        self._refresh_fences_timer.stop()
        self._refresh_fences_impl()
        # Bypass attach throttle — user refresh must restore vanished HWNDs.
        # If menu freeze still blocks restack, schedule until it clears.
        self._last_shell_attach_at = 0.0
        self._shell_attach_force_pending = False
        self._ensure_shell_attachments(force=True)
        self._ensure_desktop_overlays_visible(force=True)
        try:
            refresh_desktop(*get_desktop_paths())
        except Exception:
            pass
        get_logger().info("force_refresh_desktop done")

    def _refresh_fences_impl(self) -> None:
        if self._exiting:
            return
        if self._refreshing_fences:
            self._refresh_fences_timer.start(200)
            return
        self._refreshing_fences = True
        refresh_public = self._refresh_public_from_fences_pending
        self._refresh_public_from_fences_pending = False
        force = bool(getattr(self, "_refresh_fences_force", False))
        self._refresh_fences_force = False
        try:
            if refresh_public:
                self.refresh_public_desktop(relayout=True, immediate=force)
            if not self._fences_should_show():
                return
            if self.fences:
                from src.fence_rules import all_fence_pinned_keys

                all_pinned = all_fence_pinned_keys(self.settings)
                for fence in self.fences:
                    fence.settings = self.settings
                    fence._all_pinned_cache = all_pinned
                    fence.refresh(force=force)
                for fence in self.fences:
                    fence._all_pinned_cache = None
                return
            if self.settings.get("show_fences", True):
                self.show_fences()
        finally:
            self._refreshing_fences = False
            if self._refresh_public_from_fences_pending:
                self._refresh_fences_timer.start(200)

    def _restore_desktop_on_exit(self, *, silent: bool = False) -> None:
        """Clear virtual organization and restore a stock Windows desktop.

        Clears fence pins / public overlays, restores hosted system icons, and
        moves any leftover legacy warehouse files back to the Desktop.
        """
        if self._desktop_restored:
            return
        self._desktop_restored = True

        restored = 0
        try:
            restored = restore_desktop_from_storage()
        except Exception:
            restored = 0
        try:
            restore_all_hosted_namespace_icons()
        except Exception:
            pass

        # Clear virtual organization so next launch starts clean.
        for fence in self.settings.get("fences", []):
            if isinstance(fence, dict):
                fence["virtual_items"] = []
        self.settings["public_desktop_items"] = []
        self.settings["hosted_namespace_icons"] = {}
        self.settings["session_active"] = False
        self.settings["hide_shell_icons"] = False

        try:
            ensure_desktop_icons_visible()
            self.qt_app.processEvents()
            ensure_desktop_icons_visible()
        except Exception:
            pass
        self._icons_hidden = False

        try:
            save_settings(self.settings, immediate=True)
        except OSError:
            pass

        if not silent:
            try:
                msg = "已还原系统桌面"
                if restored:
                    msg = f"已还原系统桌面，并回收 {restored} 个仓库残留文件"
                self.tray.show_message(APP_NAME_ZH, msg)
            except Exception:
                pass

    def _ensure_shell_icons_restored(self) -> None:
        """Show Explorer desktop icons after overlays are torn down."""
        try:
            from src.desktop_shell_host import invalidate_defview_host_cache

            invalidate_defview_host_cache()
        except Exception:
            pass
        try:
            reveal_hosted_namespace_icons()
        except Exception:
            pass
        try:
            ensure_desktop_icons_visible()
        except Exception:
            pass
        try:
            self.qt_app.processEvents()
        except Exception:
            pass
        # Second pass — Explorer sometimes misses the first toggle during HWND teardown.
        try:
            ensure_desktop_icons_visible()
        except Exception:
            pass
        self._icons_hidden = False

    def _flush_overlay_teardown(self) -> None:
        """Let deleteLater / HWND detach finish before shell icons come back."""
        try:
            self.qt_app.processEvents()
            self.qt_app.processEvents()
        except Exception:
            pass

    def _park_desktop_for_exit(self) -> None:
        """Leave a normal Windows desktop; keep DeskTidy layout in settings.

        Without the app running, users must see all shell icons (like stock Windows).
        Next launch (or auto-start) re-applies the organized view.
        """
        try:
            self._clear_public_icons()
        except Exception:
            pass
        if self.page_indicator:
            try:
                self._retire_overlay_widget(self.page_indicator)
            except Exception:
                pass
            self.page_indicator = None
        if self.dock:
            try:
                self._retire_overlay_widget(self.dock)
            except Exception:
                pass
            self.dock = None
        todo = getattr(self, "todo_panel", None)
        if todo is not None:
            try:
                self._retire_overlay_widget(todo)
            except Exception:
                pass
            self.todo_panel = None
        vault = getattr(self, "vault_panel", None)
        if vault is not None:
            try:
                self._retire_overlay_widget(vault)
            except Exception:
                pass
            self.vault_panel = None
        launcher = getattr(self, "vault_launcher", None)
        if launcher is not None:
            try:
                self._retire_overlay_widget(launcher)
            except Exception:
                pass
            self.vault_launcher = None
        pet = getattr(self, "pet_widget", None)
        if pet is not None:
            try:
                self._retire_overlay_widget(pet)
            except Exception:
                pass
            self.pet_widget = None
        try:
            self.hide_fences()
        except Exception:
            pass
        # Critical: wait until translucent fence HWNDs are gone, otherwise the
        # first quit shows shell icons *through* the still-compositing overlays
        # (duplicate / ghost icons). Second quit looked fine once HWNDs died.
        self._flush_overlay_teardown()
        self._ensure_shell_icons_restored()
        self.settings["hide_shell_icons"] = False
        self.settings["session_active"] = False
        try:
            save_settings(self.settings)
        except OSError:
            pass

    def _on_about_to_quit(self) -> None:
        """Finalize session: show shell icons, keep fence layout for next launch."""
        if not self._desktop_restored:
            self._park_desktop_for_exit()
            self._desktop_restored = True
        else:
            self.settings["session_active"] = False
            try:
                save_settings(self.settings, immediate=True)
            except OSError:
                pass

    def quit(self, *, silent: bool = False) -> None:
        """Exit and keep fence organization for the next launch."""
        self._quit_impl(silent=silent)

    def _stop_overlay_timers(self) -> None:
        for name in (
            "_overlay_keepalive_timer",
            "_desktop_fg_recover_timer",
            "_refresh_fences_timer",
            "_refresh_public_timer",
            "_watch_debounce",
            "_sticky_reprune_timer",
            "_display_layout_timer",
            "_save_fence_layout_timer",
        ):
            timer = getattr(self, name, None)
            if timer is not None:
                timer.stop()

    def _quit_impl(self, *, silent: bool = False) -> None:
        if self._exiting:
            return

        # Freeze overlay management BEFORE the confirm dialog.
        self._overlay_mgmt_suspended = True
        self._stop_overlay_timers()
        try:
            set_live_fences_provider(None)
        except Exception:
            pass
        try:
            from src.fd_search import cancel_active_searches

            cancel_active_searches()
            overlay = getattr(self, "_file_search_overlay", None)
            if overlay is not None:
                overlay.close_overlay()
        except Exception:
            pass
        try:
            self._foreground_monitor.stop()
        except Exception:
            pass

        if not silent:
            message = (
                "退出后系统桌面会恢复显示全部图标。\n"
                "分区钉选与布局会保留；下次打开或开机自启动后再按你的布局显示。\n\n"
                "是否退出？"
            )
            title = "退出确认"

            parent = self.window if self.window.isVisible() and not self.window.isMinimized() else None
            if parent is None and (not self.window.isVisible() or self.window.isMinimized()):
                bring_widget_to_foreground(self.window, was_mapped=False)
                parent = self.window

            confirmed = ask_yes_no(parent, title, message)
            if not confirmed:
                # User cancelled — resume normal overlay management.
                self._overlay_mgmt_suspended = False
                try:
                    self._foreground_monitor.start()
                except Exception:
                    pass
                # Re-hide shell icons if Explorer flashed them under fences while
                # the confirm dialog was open (looks like duplicated icons).
                try:
                    self._apply_session_desktop_view()
                except Exception:
                    pass
                self._schedule_overlay_keepalive()
                return

        self._exiting = True
        self._stop_overlay_timers()
        self._save_fence_layout(immediate=True)
        self.settings["session_active"] = False
        save_settings(self.settings, immediate=True)
        self.peek_manager.exit()
        self.desktop_monitor.stop()
        self._desktop_blank_click_monitor.stop()
        try:
            self._foreground_monitor.stop()
        except Exception:
            pass
        self.desktop_context.stop()
        try:
            stop_shell_ipc_host()
        except Exception:
            pass
        self.watcher.stop()
        self.screenshot_manager.close_all()
        self.screen_record_manager.shutdown()
        self.hotkey_manager.unregister_all()
        if self.page_indicator:
            self.page_indicator.close()
            self.page_indicator.deleteLater()
            self.page_indicator = None
        if self.dock:
            self.dock.close()
            self.dock.deleteLater()
            self.dock = None
        todo = getattr(self, "todo_panel", None)
        if todo is not None:
            try:
                todo.close()
                todo.deleteLater()
            except RuntimeError:
                pass
            self.todo_panel = None
        vault = getattr(self, "vault_panel", None)
        if vault is not None:
            try:
                vault.close()
                vault.deleteLater()
            except RuntimeError:
                pass
            self.vault_panel = None
        launcher = getattr(self, "vault_launcher", None)
        if launcher is not None:
            try:
                launcher.close()
                launcher.deleteLater()
            except RuntimeError:
                pass
            self.vault_launcher = None
        pet = getattr(self, "pet_widget", None)
        if pet is not None:
            try:
                pet.close()
                pet.deleteLater()
            except RuntimeError:
                pass
            self.pet_widget = None
        self._clear_public_icons()
        self.hide_fences()
        try:
            from src.desktop_ll_mouse import shutdown_ll_mouse

            shutdown_ll_mouse()
        except Exception:
            pass
        try:
            from src.desktop_ll_keyboard import shutdown_ll_keyboard

            shutdown_ll_keyboard()
        except Exception:
            pass
        try:
            self.window.hide()
        except Exception:
            pass
        try:
            self.tray.tray.hide()
        except Exception:
            pass
        self.qt_app.processEvents()
        # Keep fence layout / virtual pins; only release the live shell view.
        self._park_desktop_for_exit()
        self._desktop_restored = True  # aboutToQuit should not double-work
        # Give Win32 a beat to drop hook/DLL refs before the onefile bootloader
        # tries to delete %TEMP%\_MEI* (avoids the cleanup Warning dialog).
        try:
            import gc

            gc.collect()
        except Exception:
            pass
        self.qt_app.quit()
        # Belt-and-suspenders: some Windows builds keep the process alive after quit().
        QTimer.singleShot(0, lambda: self.qt_app.exit(0))

    def run(self) -> int:
        # First install/open: always show the main window (do not start tray-only).
        # Later launches honor close_to_tray after the user has chosen close behavior.
        first_open = not bool(self.settings.get("close_behavior_prompted", False))
        if first_open or not self.settings.get("close_to_tray", True):
            self.window.show()
            QTimer.singleShot(
                0, lambda: bring_widget_to_foreground(self.window, was_mapped=False)
            )
        return self.qt_app.exec()


def main(*, pending_shell_verb: str | None = None) -> int:
    try:
        app = DeskTidyApp()
        if pending_shell_verb:
            app._pending_shell_verb = pending_shell_verb
        return app.run()
    except Exception as exc:
        _show_fatal_error(exc)
        return 1


def _show_fatal_error(exc: Exception) -> None:
    import traceback

    from src.app_logging import log_exception

    detail = traceback.format_exc()
    log_exception("fatal startup/runtime error", exc)
    try:
        qt_app = QApplication.instance() or QApplication(sys.argv)
        from PyQt6.QtWidgets import QMessageBox

        QMessageBox.critical(None, f"{APP_NAME_ZH} - 启动失败", f"{exc}\n\n{detail[-1500:]}")
    except Exception:
        import ctypes

        ctypes.windll.user32.MessageBoxW(0, str(exc), APP_NAME_ZH, 0x10)


if __name__ == "__main__":
    sys.exit(main())
