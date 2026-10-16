"""Tool-launching entry points extracted from ``DeskTidyApp`` (app.py).

Owns the launch/toggle entry points for the desktop tools — calculator, screen
recorder, standalone deskNote, meeting minutes, page-bar folder shortcuts, plus
the peek and file-search toggles and the todo-panel raise. Method bodies are
copied verbatim from the original class; behaviour is unchanged.

``DeskTidyApp`` inherits this as ``ToolLauncherMixin``, so every ``self.*``
attribute/method these entry points touch resolves through the full MRO at
runtime. No ``src.app`` import here (that would be circular).
"""

from __future__ import annotations

from PyQt6.QtCore import QTimer


class ToolLauncherMixin:
    """Launch/toggle entry points for the desktop tool buttons and hotkeys."""

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
            # raise_band=True after chrome FG would lift Progman over Mail.
            self._ensure_page_chrome_visible(raise_band=False)
            self._raise_open_tool_panels_above_pet()
            self._sink_fences_and_public_under_apps()
        except RuntimeError:
            pass

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
        # A public refresh that ran while notepad owned FG skipped icon.show()
        # (may_show gate), and _remap_hidden_overlay_hwnds only repairs
        # Win32-hidden HWNDs — floats left Qt-hidden stayed gone after close.
        QTimer.singleShot(0, self.refresh_public_desktop)
        # If all floats were already visible the refresh early-returns and
        # skips ensure_live_fences_interactive, leaving host.present()'s
        # restack of the public plate above fences to eat their clicks.
        QTimer.singleShot(250, self.ensure_live_fences_interactive)
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
