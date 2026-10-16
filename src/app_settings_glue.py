"""Theme / settings-change glue and fence-refresh entry points extracted from ``DeskTidyApp``.

Owns theme application + window-icon sync, the settings-changed handler, and the
fence refresh / force-refresh / refresh-impl entry points. Method bodies are
copied verbatim from the original class; behaviour is unchanged.

``DeskTidyApp`` inherits this as ``SettingsThemeMixin``, so every ``self.*``
resolves through the full MRO at runtime. No ``src.app`` import here (circular).
"""

from __future__ import annotations

from src.app_logging import get_logger
from src.desktop_scanner import invalidate_desktop_scan_cache
from src.icon_utils import get_app_icon
from src.ui.styles import normalize_theme


class SettingsThemeMixin:
    """Theme / settings-change handling and fence refresh entry points."""

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
