"""Desktop view (stock vs organized) extracted from ``DeskTidyApp``.

Owns hide/toggle of fences, the stock-desktop / organized-desktop view swap,
desktop double/blank click handling, fence region select and the shell
integration (Explorer verbs / IPC host / click recorder) wiring. Method bodies
are copied verbatim from the original class; behaviour is unchanged.

``DeskTidyApp`` inherits this as ``DesktopViewMixin``, so every ``self.*``
resolves through the full MRO at runtime. No ``src.app`` import here
(circular).
"""

from __future__ import annotations

from PyQt6.QtCore import QTimer

from src.app_logging import get_logger
from src.desktop_context import get_last_desktop_right_click
from src.settings import save_settings
from src.shell_background_verbs import sync_desktop_background_verbs
from src.shell_ipc import start_shell_ipc_host
from src.win_shell import (
    are_desktop_icons_visible,
    reveal_hosted_namespace_icons,
    set_desktop_icons_visible,
)


class DesktopViewMixin:
    """Stock/organized desktop view + shell integration wiring."""

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
