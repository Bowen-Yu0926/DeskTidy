"""Feature panels (dock / todo / vault / pet / extensions) extracted from ``DeskTidyApp``.

Owns dock + todo + account-vault + desktop-pet setup and the extension toggle
reconcile. Method bodies are copied verbatim from the original class;
behaviour is unchanged.

``DeskTidyApp`` inherits this as ``PanelsMixin``, so every ``self.*`` resolves
through the full MRO at runtime. No ``src.app`` import here (circular).
"""

from __future__ import annotations

from PyQt6.QtCore import QTimer, Qt

from src.settings import save_settings


class PanelsMixin:
    """Dock / todo / vault / pet / extensions feature panels."""

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

        # DeskTidyApp is not a QObject — parenting the thread to it raises
        # and aborts the probe during startup. QApplication outlives the panel.
        job = VaultApiJob(work, self.qt_app)

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
            # Never raise_band here: buoy/panel are Progman-owned; HWND_TOP without
            # a careful path (or activateWindow) lifts fences over Mail/Outlook.
            self._ensure_page_chrome_visible(raise_band=False)
            self._raise_open_tool_panels_above_pet()
            self._sink_fences_and_public_under_apps()
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
