"""Global-hotkey orchestration extracted from ``DeskTidyApp`` (app.py).

Owns the hotkey setup / defer-refresh / capture / LL-chord fallback layer and the
screenshot/snapshot hotkey helper. Method bodies are copied verbatim from the
original class; behaviour is unchanged.

The low-level input-hook lifecycle itself (``desktop_ll_keyboard`` /
``desktop_ll_mouse`` register/unregister/shutdown and the ``_on_ll_*`` handlers)
is intentionally left where it was — these methods only *call*
``register_chord_handler`` / ``clear_chord_handlers``, they never install the
hooks.

``DeskTidyApp`` inherits this as ``HotkeyMixin``, so every ``self.*`` attribute /
method these entry points touch resolves through the full MRO at runtime. No
``src.app`` import here (that would be circular).
"""

from __future__ import annotations

from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QKeySequence, QShortcut

from src.hotkey_manager import HOTKEY_FALLBACKS, HOTKEY_LABELS
from src.i18n import APP_NAME_ZH
from src.settings import save_settings


class HotkeyMixin:
    """Hotkey setup / refresh / capture / LL-chord orchestration."""

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
            from src.desktop_ll_keyboard import clear_chord_handlers

            clear_chord_handlers()

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
        from src.desktop_ll_keyboard import clear_chord_handlers

        clear_chord_handlers()

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
                hotkeys.get("screen_record", "Ctrl+Alt+3"),
                self.screen_record_manager.toggle_recording,
            ),
            (
                "file_search",
                hotkeys.get("file_search", "Ctrl+Alt+4"),
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

        self._register_hotkey_chords(hotkeys)

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

    def _register_hotkey_chords(self, hotkeys: dict) -> None:
        """LL-hook fallback so modifier-combo global hotkeys still fire while
        another app runs in exclusive fullscreen (RegisterHotKey WM_HOTKEY is
        not delivered in that state). The shared WH_KEYBOARD_LL hook sees all
        input regardless of focus.  Bare F-keys are already polled via
        GetAsyncKeyState (works system-wide), so only modifier combos need this.

        When RegisterHotKey also succeeds, both paths can fire once —
        ``next_page`` / ``prev_page`` gate (~350ms) so Ctrl+Shift+←/→ does not
        flip twice and leave partitions/icons soft-parked.
        """
        from src.desktop_ll_keyboard import register_chord_handler
        from src.hotkey_manager import _parse_hotkey as parse_hotkey

        actions = (
            ("organize", self._do_organize),
            ("toggle_fences", self._toggle_fences_visibility),
            ("toggle_icons", self._toggle_icons_and_fences),
            ("peek_fences", self._toggle_peek),
            ("page_next", self.next_page),
            ("page_prev", self.prev_page),
            ("screenshot", self.screenshot_manager.start_capture),
            ("show_screenshot", self._hotkey_show_last_screenshot),
            ("screen_record", self.screen_record_manager.toggle_recording),
            ("file_search", self._toggle_file_search),
            ("calculator", self._on_calculator_requested),
        )
        for action, cb in actions:
            key = str(hotkeys.get(action, "") or "").strip()
            if not key:
                continue
            parsed = parse_hotkey(key)
            if parsed is None:
                continue
            modifiers, vk = parsed
            if modifiers == 0:
                continue

            def _deferred(cb=cb) -> None:
                QTimer.singleShot(0, cb)

            register_chord_handler(modifiers, vk, _deferred)

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
            hotkey = self.settings.get("hotkeys", {}).get("screenshot", "Ctrl+Alt+1")
            self.tray.show_message(
                APP_NAME_ZH,
                f"暂无截图。请先按 {hotkey} 进行区域截图。",
            )
