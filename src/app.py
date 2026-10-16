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
from src.app_defaults import apply_missing_defaults
from src.app_launchers import ToolLauncherMixin
from src.app_hotkeys import HotkeyMixin
from src.app_organize import OrganizeMixin
from src.app_page_switch import PageSwitchMixin
from src.app_panels import PanelsMixin
from src.app_settings_glue import SettingsThemeMixin
from src.app_memory_trim import MemoryTrimMixin
from src.app_fence_lifecycle import FenceLifecycleMixin
from src.app_desktop_view import DesktopViewMixin
from src.app_watcher import WatcherMixin
from src.app_fence_layout import FenceLayoutMixin
from src.bootstrap import main, _show_fatal_error
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
from src.ui.ui_bridge import _UiBridge
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


class DeskTidyApp(FenceLayoutMixin, WatcherMixin, DesktopViewMixin, FenceLifecycleMixin, MemoryTrimMixin, PanelsMixin, PageSwitchMixin, HotkeyMixin, OrganizeMixin, SettingsThemeMixin, ToolLauncherMixin):
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

        if ensure_system_defaults(self.settings):
            save_settings(self.settings)
        # Backfill missing keys and heal public-area scope (extracted to
        # src/app_defaults.apply_missing_defaults).
        apply_missing_defaults(self.settings)
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
        mark("startup_complete")

    # ==== [启动调度 / 预热 / offpage] 约 L424-1279（候选 mixin: StartupMixin）====
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
        """Critical overlay restore once the desktop shell is reachable.

        Keep the first paint path short: session view → fences (eager icons) →
        public floats → page chrome → attach/heal. Pet / vault / capture warm
        run on the next event-loop turn so cold boot is not one long hitch.
        """
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
            # One public sync below — skip show_fences' deferred refresh.
            self.show_fences(sync_public=False)
        if callable(mark):
            mark("fences_shown")
        self.refresh_public_desktop(relayout=True, immediate=True)
        # Heal missing key only — never overwrite an explicit user False.
        if "show_page_indicator" not in self.settings:
            self.settings["show_page_indicator"] = True
            try:
                save_settings(self.settings)
            except OSError:
                pass
        self._setup_page_indicator()
        self._setup_dock()
        self._start_overlay_keepalive_timer()
        # Register capture hotkeys *before* off-page HWND warmup. That loop can
        # take a long time after reboot (cold icon extract); F1/F3 must already
        # be live, and the loop must yield so those keys are dispatched.
        try:
            self._setup_hotkeys()
        except Exception:
            get_logger().exception("startup: hotkey registration before overlay warmup failed")
        # Attach before returning to the event loop. A zero-delay timer loses to
        # deferred work that expired while fences were being built, so the boot
        # heal ran ~2s after the fences were already on screen.
        self._startup_force_overlay_attach()
        if callable(mark):
            mark("overlays_ready")
        # Secondary chrome + GDI/FFmpeg warm — next turn, after fences paint.
        QTimer.singleShot(0, self._startup_overlays_secondary)
        if len(self._get_pages()) > 1 and self.settings.get("show_fences", True):
            # Prime off-page HWNDs in the background so the current page paints first.
            self._schedule_startup_offpage_warmup()
        # Arm COM/registry/watcher timers only after overlays, or their deadlines
        # expire during this call and they run as one hitch before the heal.
        self._schedule_startup_deferred()

    def _startup_overlays_secondary(self) -> None:
        """Pet / todo / vault / hooks / capture warm after first overlay paint."""
        if self._exiting or self._icons_hidden:
            return
        try:
            self._setup_todo_panel()
        except Exception:
            get_logger().exception("startup: todo panel failed")
        try:
            self._setup_account_vault_panel()
        except Exception:
            get_logger().exception("startup: vault panel failed")
        try:
            self._setup_desktop_pet()
        except Exception:
            get_logger().exception("startup: desktop pet failed")
        try:
            from src.ui.fence_icon_item import ensure_desktop_icon_key_hook

            ensure_desktop_icon_key_hook()
        except Exception:
            pass
        # Construct capture managers so FFmpeg/GDI warm threads start after
        # fences are interactive — not on the first user click after reboot.
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
        mark = getattr(self, "_startup_mark", None)
        if callable(mark):
            mark("overlays_secondary_ready")

    def _schedule_startup_deferred(self) -> None:
        """Stagger shell/registry work after overlays are attached.

        Must not be armed from ``__init__``: those deadlines expire while
        ``_run_startup_overlays`` is still building fences, and Qt then runs
        the whole batch before boot heal.
        """
        QTimer.singleShot(0, self._startup_deferred_immediate)
        QTimer.singleShot(400, self._startup_deferred_monitors)
        QTimer.singleShot(700, self._startup_deferred_guard)
        QTimer.singleShot(800, self._startup_deferred_shell_menu_warm)
        QTimer.singleShot(1000, self._startup_deferred_window_data)
        QTimer.singleShot(2000, self._startup_deferred_legacy_storage_migrate)
        # Late so the GitHub fetch never competes with boot shell work.
        QTimer.singleShot(15000, self._startup_deferred_update_check)
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

    def _startup_deferred_update_check(self) -> None:
        """Background release check — silent unless a newer tag exists."""
        if self._exiting:
            return
        if not self.settings.get("check_update_on_startup", True):
            return
        from src.update_checker import notify_update_on_startup

        notify_update_on_startup(self.window, settings=self.settings)

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
                # show_fences already applied QSS. setStyleSheet again restyles
                # every icon child and held the UI thread after first paint.
                container = getattr(fence, "container", None)
                already_styled = False
                try:
                    already_styled = bool(
                        container is not None and str(container.styleSheet() or "")
                    )
                except RuntimeError:
                    already_styled = False
                if not already_styled:
                    apply = getattr(fence, "_apply_style", None)
                    if callable(apply):
                        apply()
                        fence.setWindowOpacity(fence._target_opacity())
                cfg = fence.config if isinstance(fence.config, dict) else {}
                needs_icons = bool(cfg.get("virtual_items")) or is_portal_fence(cfg)
                if needs_icons:
                    # show_fences already built the grid. force-refresh here
                    # re-ran SHGetFileInfo for every pin and held the UI thread
                    # ~2s after the fences were already on screen.
                    empty = True
                    try:
                        layout = getattr(fence, "items_layout", None)
                        empty = layout is None or int(layout.count()) == 0
                    except Exception:
                        empty = True
                    if empty:
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
        # After the current page is up. Icon extract waits for the first visit
        # — doing it here blocked the UI thread for every off-page fence.
        QTimer.singleShot(1200, self._startup_offpage_warmup_tick)

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
            # HWND only. Icon extract on this tick stalled login; the page
            # switch refresh fills glyphs on first visit.
            self._warmup_one_offpage_fence(fence_cfg, sync_icons=False)
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
        # Create the startup shortcut when auto_start is enabled.
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

    # ==== [内存裁剪 / 会话] 约 L1356-2011（候选 mixin: MemoryTrimMixin）====
    def _overlay_restack_blocked(self) -> bool:
        """True when Z-order / re-attach would dismiss menus or flash UI."""
        if self._exiting or self._overlay_mgmt_suspended:
            return True
        if getattr(self, "_overlay_drag_active", False):
            return True
        if getattr(self, "_desktop_popup_open", False):
            return True
        if self._overlay_capture_freeze_active():
            return True
        try:
            from src.ui.fence_icon_item import desktop_selection_batch_active

            if desktop_selection_batch_active():
                return True
        except Exception:
            pass
        return False

    def _overlay_capture_freeze_active(self) -> bool:
        """Screenshot / screen-record freeze — must not defer force-attach retries.

        Menu/drag freezes keep retrying force attach so user refresh still lands.
        Capture freezes must **drop** pending force: the snip overlay is HWND_TOPMOST,
        and a deferred ``ensure_live`` after teardown HWND_TOPs every fence
        (「截图后分区到最上层」).
        """
        return bool(getattr(self, "_screenshot_session_active", False)) or bool(
            getattr(self, "_recording_session_active", False)
        )

    def _cancel_pending_force_shell_attach(self) -> None:
        """Invalidate queued force-attach timers (capture freeze / snip end)."""
        self._shell_attach_force_seq = int(
            getattr(self, "_shell_attach_force_seq", 0)
        ) + 1
        self._shell_attach_force_pending = False
        self._shell_attach_force_delay_ms = None

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
        # Drop any force-attach already queued — retries through the snip would
        # fire right after overlay teardown and raise every fence.
        self._cancel_pending_force_shell_attach()
        self._stop_overlay_churn_timers()

    def _end_screenshot_session(self) -> None:
        depth = max(0, int(getattr(self, "_screenshot_session_depth", 0)) - 1)
        self._screenshot_session_depth = depth
        if depth > 0:
            return
        self._screenshot_session_active = False
        if self._exiting or self._overlay_mgmt_suspended:
            return
        # Never let a capture-era force attach land as HWND_TOP on fences.
        self._cancel_pending_force_shell_attach()
        # Soft remap only — do not ensure_live / raise_band after TOPMOST snip.
        try:
            self._remap_hidden_overlay_hwnds()
        except Exception:
            pass
        # Reconcile Z-order now that restack is allowed: foreign FG sinks
        # chrome that snip TOPMOST / restore may have lifted over apps.
        try:
            self._sync_overlays_to_foreground()
        except Exception:
            pass
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
        self._cancel_pending_force_shell_attach()
        self._stop_overlay_churn_timers()

    def _end_recording_session(self) -> None:
        depth = max(0, int(getattr(self, "_recording_session_depth", 0)) - 1)
        self._recording_session_depth = depth
        if depth > 0:
            return
        self._recording_session_active = False
        if self._exiting or self._overlay_mgmt_suspended:
            return
        self._cancel_pending_force_shell_attach()
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
        # Click-to-drag Qt-raises Progman-owned fences over Cursor. Re-bind to
        # DefView + click-through immediately so the editor stays on top.
        try:
            self._arm_drag_overlays_under_apps()
        except Exception:
            pass

    def _arm_drag_overlays_under_apps(self) -> None:
        """Re-attach fences/pet to DefView and pass mouse through for the drag.

        Qt ``raise_()`` on icon press lifts the Progman owner group above
        normal apps (Cursor / Chrome). Detach+reattach restores the desktop
        band; ``WS_EX_TRANSPARENT`` lets hit-tests reach the app underneath.

        PublicIconHost is only HWND_BOTTOM'd (no detach flash, no passthrough) —
        a full-desktop transparent host black-holes clicks after a failed clear.
        """
        from src.desktop_shell_host import (
            attach_overlay_to_desktop,
            detach_overlay_from_desktop,
            is_attached_to_desktop,
            place_overlay_in_desktop_band,
        )
        from src.win_shell import set_overlay_mouse_passthrough

        armed: list = list(getattr(self, "_ole_handoff_passthrough", None) or [])
        for widget, peek in self._iter_overlay_widgets():
            if peek or widget is None:
                continue
            try:
                hwnd = int(widget.winId()) if widget.winId() else 0
            except Exception:
                continue
            if not hwnd:
                continue
            is_host = widget.__class__.__name__ == "PublicIconHost"
            try:
                if is_host:
                    # Sink only — remounting the plate flashes the whole desktop.
                    place_overlay_in_desktop_band(hwnd, force=True)
                elif is_attached_to_desktop(hwnd):
                    # Force re-owner — click-raise lifts the Progman group.
                    detach_overlay_from_desktop(hwnd)
                    attach_overlay_to_desktop(hwnd, show=True)
                    place_overlay_in_desktop_band(hwnd, force=True)
                else:
                    attach_overlay_to_desktop(hwnd, show=True)
                    place_overlay_in_desktop_band(hwnd, force=True)
            except Exception:
                pass
            if is_host:
                continue
            try:
                set_overlay_mouse_passthrough(widget, True)
                if widget not in armed:
                    armed.append(widget)
            except Exception:
                continue
        self._ole_handoff_passthrough = armed

    def _arm_ole_handoff_overlays(self) -> None:
        """Re-bind overlays under apps before ``QDrag.exec``."""
        self._arm_drag_overlays_under_apps()
        get_logger().info(
            "ole handoff: overlay passthrough count=%s",
            len(getattr(self, "_ole_handoff_passthrough", None) or []),
        )

    def _clear_ole_handoff_overlays(self) -> None:
        """Restore hit-testing after external OLE / drag end."""
        from src.win_shell import set_overlay_mouse_passthrough

        armed = list(getattr(self, "_ole_handoff_passthrough", None) or [])
        self._ole_handoff_passthrough = []
        for widget in armed:
            try:
                set_overlay_mouse_passthrough(widget, False)
            except Exception:
                continue
        if armed:
            get_logger().info(
                "ole handoff: overlay passthrough cleared count=%s", len(armed)
            )

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
        # Drop any OLE-era click-through / park left if QDrag aborted oddly.
        try:
            self._clear_ole_handoff_overlays()
        except Exception:
            pass
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
            # quiet_end (pet trash / folder / same-fence): skip public refresh
            # flash, but still restore band Z + clear stuck passthrough left by
            # ``_arm_drag_overlays_under_apps`` (pet buried / Win32-hidden).
            try:
                self._remap_hidden_overlay_hwnds()
            except Exception:
                pass
            try:
                self.ensure_live_fences_interactive()
            except Exception:
                pass
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

    # ==== [前台所有权 / 覆盖层] 约 L2012-3444（候选 mixin: ForegroundOwnershipMixin）====
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
        # Virtual-desktop switch / Win+D can leave a stale grabMouse from an
        # interrupted marquee, and the public host click mask can reference
        # HWNDs that no longer exist. Release grabs + refresh the mask so
        # desktop icons stay clickable after the switch.
        self._release_stuck_grabs()
        # Never leave WS_EX_TRANSPARENT on the public host — that bit is
        # per-HWND and makes every child public float unclickable.
        self._ensure_public_host_mouse_opaque()
        self._refresh_public_host_click_mask()

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
        # Always drop orphaned capture first — foreign modals must stay clickable.
        self._release_stuck_grabs()
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
        # Do NOT set WS_EX_TRANSPARENT on the public host here — one HWND owns
        # all public floats; click-through would make desktop files unselectable.

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

        Unattached overlays always sink. Attached Progman-owned overlays skip
        HWND_BOTTOM while settings/notepad own FG (raise the app instead); on
        foreign FG they must sink — Explorer-desktop HWND_TOP can leave them
        above Mail/Outlook. Stuck-under-wallpaper HWNDs reattach, never sink.
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
                    # Healthy Progman-owned overlays usually sit under APPWINDOWs.
                    # After Explorer-desktop HWND_TOP (ensure_live_fences), they can
                    # float above Mail/Outlook — HWND_BOTTOM+NOOWNERZORDER parks
                    # them just above Progman again (under apps, not wallpaper).
                    if foreign_fg:
                        place_overlay_in_desktop_band(hwnd, force=False)
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
            # Menu/drag freeze: keep force refresh alive. Capture freeze: drop it —
            # retrying through snip fires ensure_live and raises every fence.
            if force and not self._overlay_capture_freeze_active():
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
                # Capture freeze: drop — spinning through snip raises fences after.
                if self._overlay_capture_freeze_active():
                    return
                self._post_refresh_heal_pending = True
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
        # Capture freeze: drop — deferred ensure_live after snip raises fences.
        if self._overlay_restack_blocked():
            if self._overlay_capture_freeze_active():
                return
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
        window — and on foreign app tips such as WeChat「我知道了」. Aborting
        the marquee on the public host plus a global grabber release covers
        fence/screenshot grabs too.
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
        # Win32 SetCapture outlives Qt grabber bookkeeping on some paths.
        try:
            import ctypes

            if ctypes.windll.user32.GetCapture():
                ctypes.windll.user32.ReleaseCapture()
        except Exception:
            pass

    def _ensure_public_host_mouse_opaque(self) -> None:
        """Clear stuck WS_EX_TRANSPARENT on the shared public-icon host.

        Soft-park / a short-lived foreign-FG experiment used host-wide
        click-through; because every public float shares that HWND, the bit
        made desktop files appear frozen until a full restart.
        """
        host = getattr(self, "_public_icon_host", None)
        if host is None:
            return
        try:
            from src.win_shell import set_overlay_mouse_passthrough

            set_overlay_mouse_passthrough(host, False)
        except Exception:
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
            if force and not self._overlay_capture_freeze_active():
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

    # ==== [overlay 停放] 约 L3445-4652（候选 mixin: OverlayParkingMixin）====
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

    # ==== [功能面板 dock/todo/vault/pet/extensions] 约 L4653-5134（候选 mixin: PanelsMixin）====


    def _sink_fences_and_public_under_apps(self) -> None:
        """Sink fence / public overlays into the desktop band (under Mail, etc.).

        Unlike ``_sink_overlays_for_foreign_app``, this does **not** require
        foreign FG — vault/todo TOOLWINDOW clicks often already stole FG via the
        buoy, so ``_foreign_app_owns_foreground`` is False. Chrome stays raised
        separately; only partitions and the public plate are sunk.
        """
        if self._exiting or self._icons_hidden:
            return
        from src.desktop_shell_host import (
            attach_overlay_to_desktop,
            is_attached_to_desktop,
            is_stuck_under_wallpaper,
            place_overlay_in_desktop_band,
        )

        chrome = {
            self.page_indicator,
            self.dock,
            getattr(self, "todo_panel", None),
            getattr(self, "vault_panel", None),
            getattr(self, "vault_launcher", None),
            getattr(self, "pet_widget", None),
        }
        for widget, peek in self._iter_overlay_widgets():
            if peek or widget in chrome:
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
                if not is_attached_to_desktop(hwnd):
                    attach_overlay_to_desktop(hwnd, show=True)
                place_overlay_in_desktop_band(hwnd, force=True)
            except Exception:
                continue










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

    # ==== [watcher 同步] 已抽取至 src/app_watcher.py（WatcherMixin）====

    # ==== [热键] 已抽取至 src/app_hotkeys.py（HotkeyMixin）====

    # ==== [整理] 已抽取至 src/app_organize.py（OrganizeMixin）====

    def _current_page(self) -> int:
        return self.settings.get("current_page", 0)

    def _get_pages(self) -> list[dict]:
        pages = self.settings.get("desktop_pages")
        if not pages:
            pages = [{"id": 0, "name": "默认"}]
            self.settings["desktop_pages"] = pages
        return pages

    # ==== [换页] 约 L5906-6938（候选 mixin: PageSwitchMixin）====




















    # ==== [工具启动] 已抽取至 src/app_launchers.py（ToolLauncherMixin）====

    # ==== [fence 布局] 已抽取至 src/app_fence_layout.py（FenceLayoutMixin）====

    # ==== [设置 / 主题] 已抽取至 src/app_settings_glue.py（SettingsThemeMixin）====

    # ==== [退出] 约 L8648-9010（候选 mixin: ShutdownMixin）====
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

    def _startup_reveal_main_window(self) -> None:
        """Show settings after the event loop starts (never before ``exec``).

        Fresh installs used to ``show()`` synchronously in ``run()`` — that ran
        ``showEvent`` → desktop scan on the UI thread before overlay timers,
        so other PCs saw a blank/black window and a frozen process.
        """
        if self._exiting:
            return
        try:
            self.window.show()
        except RuntimeError:
            return
        QTimer.singleShot(
            0, lambda: bring_widget_to_foreground(self.window, was_mapped=False)
        )

    def run(self) -> int:
        # First install/open: always show the main window (do not start tray-only).
        # Later launches honor close_to_tray after the user has chosen close behavior.
        # Defer show until after ``exec`` so overlay singleShot(0) from __init__
        # runs first (tray + fences) instead of blocking on the settings scan.
        first_open = not bool(self.settings.get("close_behavior_prompted", False))
        if first_open or not self.settings.get("close_to_tray", True):
            QTimer.singleShot(0, self._startup_reveal_main_window)
        return self.qt_app.exec()


if __name__ == "__main__":
    sys.exit(main())
