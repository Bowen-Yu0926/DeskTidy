"""Snipaste-style screenshot capture, clipboard and pin."""

from __future__ import annotations

from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QGuiApplication, QPixmap
from PyQt6.QtWidgets import QApplication, QWidget

from src.desktop_capture import grab_desktop
from src.ui.pinned_image import PinnedImageWidget
from src.ui.screenshot_overlay import ScreenshotOverlay

# Concurrent desktop pins (F2 / 自动贴图). Hard cap 5; settings choose 1–5.
MAX_PINS_HARD_LIMIT = 5
DEFAULT_MAX_PINS = 3


def clamp_max_pins(raw: object) -> int:
    try:
        n = int(raw)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        n = DEFAULT_MAX_PINS
    return max(1, min(MAX_PINS_HARD_LIMIT, n))


def max_pins_from_settings(settings: dict | None) -> int:
    cfg = (settings or {}).get("screenshot", {})
    if not isinstance(cfg, dict):
        return DEFAULT_MAX_PINS
    return clamp_max_pins(cfg.get("max_pins", DEFAULT_MAX_PINS))


def _allow_self_foreground() -> None:
    """Let this process steal foreground after a global hotkey (Win10+)."""
    try:
        import os

        from src.win_shell import user32

        pid = int(os.getpid())
        # ASFW_ANY = -1 — any window in our process may become foreground.
        user32.AllowSetForegroundWindow(-1)
        user32.AllowSetForegroundWindow(pid)
    except Exception:
        pass


def dismiss_shell_context_menus() -> bool:
    """Send Escape to close Explorer / Win11 desktop context menus if open.

    Not used before region capture: Escape cancels the user's right-click
    (WeChat screenshot does not). Capture grabs first so open menus stay in
    the freeze-frame; the overlay then sits above them.
    """
    try:
        from src.win_shell import is_shell_context_menu_open, user32
    except Exception:
        return False
    try:
        if not is_shell_context_menu_open():
            return False
    except Exception:
        return False
    VK_ESCAPE = 0x1B
    KEYEVENTF_KEYUP = 0x0002
    try:
        user32.keybd_event(VK_ESCAPE, 0, 0, 0)
        user32.keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0)
    except Exception:
        return False
    return True


def force_widget_topmost(widget: QWidget) -> None:
    """Keep a Qt window at HWND_TOPMOST (screenshot overlay must stay above dialogs)."""
    from src.win_shell import bring_widget_to_foreground

    try:
        bring_widget_to_foreground(widget)
    except Exception:
        pass
    try:
        hwnd = int(widget.winId())
    except Exception:
        return
    if not hwnd:
        return
    SWP_NOSIZE = 0x0001
    SWP_NOMOVE = 0x0002
    SWP_SHOWWINDOW = 0x0040
    HWND_TOPMOST = -1
    try:
        from src.win_shell import user32

        user32.SetWindowPos(
            hwnd,
            HWND_TOPMOST,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW,
        )
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
    except Exception:
        pass


class ScreenshotManager:
    def __init__(self, get_settings):
        self._get_settings = get_settings
        self._overlay: ScreenshotOverlay | None = None
        self._capture_armed = False
        self.pinned: list[PinnedImageWidget] = []
        self._recent_pixmaps: list[QPixmap] = []
        self._last_pixmap: QPixmap | None = None
        # How many recent shots the next F2 walk has already revealed (newest first).
        # Reset when a new screenshot is remembered so the next F2 prefers that shot.
        self._f2_reveal_count = 0
        # Modal dialogs hidden/NonModal while snipping so overlay can receive input.
        self._suspended_modals: list[tuple[QWidget, Qt.WindowModality, bool]] = []
        # Non-modal topmost helpers (toast / file search) hidden during capture.
        self._suspended_top_levels: list[tuple[QWidget, bool]] = []

    def _dismiss_desktidy_transient_windows(self) -> None:
        """Close toast tips and hide Tool/Popup helpers that fight the snip overlay.

        Do **not** hide the settings main window or notepad: hide→show flashes the
        UI between ``grab_desktop`` and overlay paint. The fullscreen TOPMOST
        overlay (with mouse/keyboard grab) covers them instead.

        Do **not** hide desktop-band chrome (fences /「账」/ pet / page bar): those
        must stay Progman-owned. Treating them as transient TOPMOST helpers made
        restore() put them above Cursor after F1.
        """
        from PyQt6.QtWidgets import QDialog, QMessageBox

        app = QApplication.instance()
        if app is None:
            return
        desk = self._desk_app()
        main_win = getattr(desk, "window", None) if desk is not None else None
        notepad = getattr(desk, "_notepad_window", None) if desk is not None else None
        overlay = self._overlay
        skip_ids = {id(w) for w in (overlay, main_win, notepad) if w is not None}
        if desk is not None:
            for attr in (
                "vault_launcher",
                "vault_panel",
                "todo_panel",
                "pet_widget",
                "page_indicator",
                "dock",
                "_public_icon_host",
            ):
                w = getattr(desk, attr, None)
                if w is not None:
                    skip_ids.add(id(w))
            for fence in list(getattr(desk, "fences", None) or ()):
                if fence is not None:
                    skip_ids.add(id(fence))
            for icon in list(getattr(desk, "public_icons", None) or ()):
                if icon is not None:
                    skip_ids.add(id(icon))
        suspended = list(self._suspended_top_levels)
        already = {id(w) for w, _ in suspended}

        for w in list(app.topLevelWidgets()):
            if id(w) in skip_ids:
                continue
            if isinstance(w, (ScreenshotOverlay, QMessageBox)):
                continue
            if isinstance(w, QDialog) and w.windowModality() != Qt.WindowModality.NonModal:
                continue
            try:
                if not w.isVisible():
                    continue
            except RuntimeError:
                continue
            if id(w) in already:
                continue
            # Desktop-band chrome never uses StaysOnTop; skip any leftover.
            if bool(getattr(w, "_desktidy_raise_band", False)):
                continue
            try:
                flags = w.windowFlags()
            except RuntimeError:
                continue
            is_topmost = bool(flags & Qt.WindowType.WindowStaysOnTopHint)
            is_popup = bool(flags & Qt.WindowType.Popup)
            is_tool = bool(flags & Qt.WindowType.Tool)
            has_toast = w.findChild(QWidget, "toastCard") is not None
            name = w.objectName()
            transient = has_toast or name in (
                "fileSearchOverlay",
                "recordingIndicator",
            )
            if not transient and not (is_topmost and (is_tool or is_popup)):
                continue
            if has_toast:
                try:
                    w.close()
                except RuntimeError:
                    pass
                continue
            try:
                on_top = bool(flags & Qt.WindowType.WindowStaysOnTopHint)
                w.hide()
                suspended.append((w, on_top))
                already.add(id(w))
            except RuntimeError:
                continue
        self._suspended_top_levels = suspended

    def restore_suspended_top_levels(self) -> None:
        pending = list(self._suspended_top_levels)
        self._suspended_top_levels = []
        for widget, on_top in reversed(pending):
            try:
                if on_top:
                    widget.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
                widget.show()
                widget.raise_()
            except RuntimeError:
                continue

    def _desk_app(self):
        app = QApplication.instance()
        return getattr(app, "_desktidy_app", None) if app is not None else None

    def _toast(self, title: str, body: str, *, msec: int = 2200) -> None:
        try:
            from src.ui.toast import show_toast

            show_toast(title, body, msec=msec)
        except Exception:
            pass

    def _discard_stale_overlay(self) -> None:
        """Drop a dead/hidden overlay that never emitted finished."""
        overlay = self._overlay
        self._overlay = None
        if overlay is None:
            return
        try:
            overlay.finished.disconnect(self._on_finished)
        except (TypeError, RuntimeError):
            pass
        try:
            finish = getattr(overlay, "_finish", None)
            if callable(finish) and not getattr(overlay, "_finish_sent", False):
                finish("cancel")
            else:
                overlay.close()
        except RuntimeError:
            pass
        try:
            overlay.deleteLater()
        except RuntimeError:
            pass
        self.restore_suspended_modals()
        self._end_session_later(200)

    def _begin_session(self) -> None:
        desk = self._desk_app()
        begin = getattr(desk, "_begin_screenshot_session", None) if desk else None
        if callable(begin):
            begin()

    def _end_session_later(self, delay_ms: int = 450) -> None:
        desk = self._desk_app()
        end_later = (
            getattr(desk, "_end_screenshot_session_later", None) if desk else None
        )
        if callable(end_later):
            end_later(delay_ms)
            return
        end = getattr(desk, "_end_screenshot_session", None) if desk else None
        if callable(end):
            QTimer.singleShot(max(0, int(delay_ms)), end)

    def _clear_desktop_popup_freeze(self) -> None:
        """Release menu/modal restack freeze so capture is not stuck under it."""
        desk = self._desk_app()
        if desk is None:
            return
        if getattr(desk, "_explorer_menu_freeze_armed", False):
            desk._explorer_menu_freeze_armed = False
            desk._explorer_menu_freeze_gen = int(
                getattr(desk, "_explorer_menu_freeze_gen", 0)
            ) + 1
        while int(getattr(desk, "_desktop_popup_depth", 0)) > 0:
            end = getattr(desk, "_end_desktop_popup", None)
            if not callable(end):
                break
            end()

    def _close_blocking_qt_popups(self) -> None:
        """Close Qt popup widgets that steal mouse from the snip overlay.

        Call only **after** ``grab_desktop`` so open menus stay in the freeze-frame.
        Never Escape-dismisses Explorer shell menus.
        """
        from PyQt6.QtWidgets import QMenu

        app = QApplication.instance()
        if app is None:
            return
        for _ in range(12):
            popup = app.activePopupWidget()
            if popup is None:
                break
            if isinstance(popup, ScreenshotOverlay):
                break
            # Only tear down Qt menus/popups; leave other widgets alone.
            if not isinstance(popup, QMenu) and not bool(
                popup.windowFlags() & Qt.WindowType.Popup
            ):
                break
            try:
                popup.hide()
                popup.close()
            except RuntimeError:
                break
            QApplication.processEvents()

    def prepare_capture_ui(self) -> None:
        """Post-grab cleanup so the snip overlay can receive input.

        Must run after ``grab_desktop``. Does **not** Escape-dismiss Explorer
        context menus (WeChat-style: RMB stays until the user dismisses it).
        """
        self._close_blocking_qt_popups()
        self._dismiss_desktidy_transient_windows()
        self._suspend_modal_widgets()
        self._clear_desktop_popup_freeze()
        QApplication.processEvents()

    def _suspend_modal_widgets(self) -> None:
        app = QApplication.instance()
        if app is None:
            return
        desk = self._desk_app()
        main_win = getattr(desk, "window", None) if desk is not None else None
        already = {id(modal) for modal, _, _ in self._suspended_modals}
        suspended = list(self._suspended_modals)
        for _ in range(12):
            modal = app.activeModalWidget()
            if modal is None:
                break
            if main_win is not None and modal is main_win:
                break
            if isinstance(modal, ScreenshotOverlay):
                break
            if id(modal) in already:
                # Still reported active while we already hid it — stop looping.
                break
            try:
                modality = modal.windowModality()
                on_top = bool(
                    modal.windowFlags() & Qt.WindowType.WindowStaysOnTopHint
                )
                # setWindowFlag may recreate the HWND — apply NonModal AFTER flags.
                if on_top:
                    modal.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, False)
                modal.setWindowModality(Qt.WindowModality.NonModal)
                modal.hide()
                # Re-apply after hide; some Qt builds keep a stale modal entry.
                modal.setWindowModality(Qt.WindowModality.NonModal)
                suspended.append((modal, modality, on_top))
                already.add(id(modal))
            except RuntimeError:
                break
            QApplication.processEvents()
        self._suspended_modals = suspended

    def restore_suspended_modals(self) -> None:
        pending = list(self._suspended_modals)
        self._suspended_modals = []
        for modal, modality, on_top in reversed(pending):
            try:
                modal.setWindowModality(modality)
                if on_top:
                    modal.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
                modal.show()
                modal.raise_()
            except RuntimeError:
                continue
        self.restore_suspended_top_levels()

    def _blocking_modal_open(self) -> QWidget | None:
        """Only block nested ``QMessageBox.exec`` loops (deadlock if we hide them).

        WindowModal editors (编辑分区等) are suspended after the freeze-frame
        via ``prepare_capture_ui`` — WeChat-style: snip while settings stay open.
        """
        from PyQt6.QtWidgets import QErrorMessage, QMessageBox

        app = QApplication.instance()
        if app is None:
            return None
        modal = app.activeModalWidget()
        if modal is None:
            return None
        if isinstance(modal, ScreenshotOverlay):
            return None
        desk = self._desk_app()
        main_win = getattr(desk, "window", None) if desk is not None else None
        if main_win is not None and modal is main_win:
            return None
        if isinstance(modal, (QMessageBox, QErrorMessage)):
            return modal
        return None

    def _recover_stale_capture_arm(self) -> None:
        """Clear a stuck arm/session when ``_begin_capture`` never finished cleanly."""
        self._capture_armed = False
        if self._overlay is not None:
            try:
                alive = bool(self._overlay.isVisible())
            except RuntimeError:
                alive = False
            if alive:
                return
            self._discard_stale_overlay()
        self.restore_suspended_modals()
        self.restore_suspended_top_levels()
        # End any leftover screenshot session freeze immediately.
        desk = self._desk_app()
        while desk is not None and int(
            getattr(desk, "_screenshot_session_depth", 0)
        ) > 0:
            end = getattr(desk, "_end_screenshot_session", None)
            if not callable(end):
                break
            end()

    def _watchdog_capture_arm(self) -> None:
        """If arm is still set with no overlay, recover (nested-loop / timer loss)."""
        if not self._capture_armed:
            return
        if self._overlay is not None:
            try:
                if self._overlay.isVisible():
                    self._capture_armed = False
                    return
            except RuntimeError:
                self._overlay = None
        try:
            from src.app_logging import get_logger

            get_logger().warning("screenshot: capture arm watchdog — forcing recover")
        except Exception:
            pass
        self._recover_stale_capture_arm()
        self._toast("截图已恢复", "上次截图未正常启动，请再按一次快捷键")

    def start_capture(self) -> None:
        _allow_self_foreground()
        # Never snip under ApplicationModal confirm boxes (首次关闭「后台运行」等).
        blocking = self._blocking_modal_open()
        if blocking is not None:
            self._toast("截图暂不可用", "请先关闭当前提示框后再截图")
            return
        if self._capture_armed:
            # Stale arm used to silent-return forever (F1 dead). Recover then continue.
            self._recover_stale_capture_arm()
        if self._overlay is not None:
            try:
                alive = bool(self._overlay.isVisible())
            except RuntimeError:
                self._overlay = None
                alive = False
            if alive:
                # Second F1 while snipping — bring overlay forward (not silent no-op).
                force_widget_topmost(self._overlay)
                toolbar = getattr(self._overlay, "_toolbar", None)
                if isinstance(toolbar, QWidget):
                    force_widget_topmost(toolbar)
                try:
                    reclaim = getattr(self._overlay, "_reclaim_input_grab", None)
                    if callable(reclaim):
                        reclaim()
                except Exception:
                    pass
                return
            self._discard_stale_overlay()
        # Freeze fence/public restack before the overlay activates the app.
        self._capture_armed = True
        self._begin_session()
        # Grab on next tick so the current frame (incl. open RMB menus) is kept,
        # matching WeChat: do not Escape-dismiss shell menus before capture.
        QTimer.singleShot(0, self._begin_capture)
        # Nested modal loops / lost timers used to leave arm True → F1 forever dead.
        QTimer.singleShot(2000, self._watchdog_capture_arm)

    def _begin_capture(self) -> None:
        try:
            if self._overlay is not None:
                # Session was already begun by start_capture — do not leak the freeze.
                self._end_session_later(0)
                return
            # Modal may have opened between arm and this tick.
            if self._blocking_modal_open() is not None:
                self._toast("截图暂不可用", "请先关闭当前提示框后再截图")
                self.restore_suspended_modals()
                self.restore_suspended_top_levels()
                self._end_session_later(0)
                return
            _allow_self_foreground()
            # Capture first while Explorer / app context menus are still painted.
            desktop, origin = grab_desktop()
            if desktop.isNull() or desktop.width() < 2 or desktop.height() < 2:
                self._toast("截图失败", "无法捕获桌面画面，请重试")
                self.restore_suspended_modals()
                self.restore_suspended_top_levels()
                self._end_session_later(200)
                return

            # Snapshot window bounds before we hide modals / raise the overlay.
            from src.snip_window_regions import snapshot_snip_window_rects

            logical = desktop.deviceIndependentSize().toSize()
            if logical.width() < 2 or logical.height() < 2:
                logical = desktop.size()
            window_rects = snapshot_snip_window_rects(
                desk_origin=origin,
                desk_size=(int(logical.width()), int(logical.height())),
            )

            # Freeze-frame is done — only then clear Qt popups that block overlay input.
            # Shell RMB menus are never Escape-dismissed.
            self.prepare_capture_ui()

            self._overlay = ScreenshotOverlay(
                desktop, origin, window_rects=window_rects
            )
            self._overlay.finished.connect(self._on_finished)
            self._overlay.show_overlay()
            force_widget_topmost(self._overlay)
            toolbar = getattr(self._overlay, "_toolbar", None)
            if isinstance(toolbar, QWidget):
                force_widget_topmost(toolbar)
        except Exception:
            self._overlay = None
            try:
                from src.app_logging import get_logger

                get_logger().exception("screenshot: begin_capture failed")
            except Exception:
                pass
            self._toast("截图失败", "启动截图界面时出错")
            self.restore_suspended_modals()
            self.restore_suspended_top_levels()
            self._end_session_later(200)
        finally:
            self._capture_armed = False

    def _on_finished(self, pixmap, action: str) -> None:
        overlay = self._overlay
        self._overlay = None
        self._capture_armed = False
        if overlay is not None:
            try:
                overlay.close()
            except RuntimeError:
                pass
            try:
                overlay.deleteLater()
            except RuntimeError:
                pass
        try:
            if action == "cancel" or pixmap is None or pixmap.isNull():
                return

            self._remember_pixmap(pixmap)
            cfg = self._get_settings().get("screenshot", {})
            if action == "copy":
                QApplication.clipboard().setPixmap(pixmap)
                if cfg.get("auto_pin_after_copy", False):
                    self._pin_image(pixmap, end_session=False)
            elif action == "pin":
                self._pin_image(pixmap, end_session=False)
            elif action == "save":
                self._save_image(pixmap)
        finally:
            self.restore_suspended_modals()
            self.restore_suspended_top_levels()
            # Let focus/Z-order settle after overlay teardown before resuming restack.
            self._end_session_later(500)

    def _max_pins(self) -> int:
        return max_pins_from_settings(self._get_settings())

    def _remember_pixmap(self, pixmap: QPixmap) -> None:
        copy = pixmap.copy()
        self._recent_pixmaps.append(copy)
        max_n = self._max_pins()
        while len(self._recent_pixmaps) > max_n:
            self._recent_pixmaps.pop(0)
        self._last_pixmap = self._recent_pixmaps[-1]
        # New shot → next F2 starts from this newest (do not keep raising an old pin).
        self._f2_reveal_count = 0

    def _pin_origin(self) -> tuple[int, int]:
        screen = QGuiApplication.primaryScreen()
        if screen is None:
            return (200, 200)
        c = screen.availableGeometry().center()
        return (int(c.x()), int(c.y()))

    def _place_pin(self, widget: PinnedImageWidget, *, index: int = 0) -> None:
        cx, cy = self._pin_origin()
        step = 36
        widget.move(
            cx - widget.width() // 2 + index * step,
            cy - widget.height() // 2 + index * step,
        )

    def _trim_pinned(self) -> None:
        """Keep at most max_pins widgets; close oldest first."""
        max_n = self._max_pins()
        while len(self.pinned) > max_n:
            old = self.pinned.pop(0)
            try:
                old.closed.disconnect()
            except (TypeError, RuntimeError):
                pass
            try:
                old.close()
                old.deleteLater()
            except RuntimeError:
                pass
        # Trim/close frees reveal slots so F2 can pin again.
        self._f2_reveal_count = min(int(self._f2_reveal_count), len(self.pinned))

    def _close_all_pins(self) -> None:
        for widget in list(self.pinned):
            try:
                widget.closed.disconnect()
            except (TypeError, RuntimeError):
                pass
            try:
                widget.close()
                widget.deleteLater()
            except RuntimeError:
                pass
        self.pinned.clear()
        self._f2_reveal_count = 0

    def show_last_screenshot(self) -> bool:
        """F2: pin one recent screenshot per press (newest → older).

        Does not dump the whole stack in one press. Each press adds one pin
        until ``max_pins``. Closing a pin frees a reveal slot so F2 can pin
        again. A new capture resets the walk so the next F2 prefers that shot.
        """
        max_n = self._max_pins()
        if not self._recent_pixmaps:
            clipboard = QApplication.clipboard()
            if clipboard.mimeData().hasImage():
                pm = clipboard.pixmap()
                if pm is not None and not pm.isNull():
                    self._remember_pixmap(pm)
        recent = list(self._recent_pixmaps)[-max_n:]
        if not recent:
            return False

        # Closed pins must not leave reveal_count stuck at the ceiling
        # (double-click close used to make the next F2 a no-op raise-only).
        self._f2_reveal_count = min(int(self._f2_reveal_count), len(self.pinned))

        if self._f2_reveal_count >= len(recent):
            # Whole recent walk already on screen — just raise what is up.
            for widget in self.pinned:
                try:
                    if widget.isVisible():
                        widget.raise_()
                except RuntimeError:
                    pass
            return bool(self.pinned)

        pixmap = recent[-(self._f2_reveal_count + 1)]
        self._begin_session()
        try:
            widget = PinnedImageWidget(pixmap)
            widget.closed.connect(lambda w=widget: self._remove_pin(w))
            self._place_pin(widget, index=len(self.pinned))
            widget.show()
            widget.raise_()
            self.pinned.append(widget)
            self._trim_pinned()
            self._f2_reveal_count = min(self._f2_reveal_count + 1, max_n)
        finally:
            self._end_session_later(400)
        return True

    def _pin_image(self, pixmap: QPixmap, *, end_session: bool = True) -> None:
        if end_session:
            self._begin_session()
        try:
            widget = PinnedImageWidget(pixmap)
            widget.closed.connect(lambda w=widget: self._remove_pin(w))
            self._place_pin(widget, index=len(self.pinned))
            widget.show()
            self.pinned.append(widget)
            self._trim_pinned()
        finally:
            if end_session:
                self._end_session_later(400)

    def _remove_pin(self, widget: PinnedImageWidget) -> None:
        if widget in self.pinned:
            self.pinned.remove(widget)
        # Double-click / × / Esc close must free a reveal slot so F2 can
        # pin the same shot again (otherwise raise-only with empty stack).
        self._f2_reveal_count = min(int(self._f2_reveal_count), len(self.pinned))

    def _save_image(self, pixmap: QPixmap) -> None:
        from src.ui.pinned_image import save_pixmap_dialog

        save_pixmap_dialog(pixmap, None, title="保存截图")

    def close_all(self) -> None:
        self._capture_armed = False
        if self._overlay is not None:
            overlay = self._overlay
            self._overlay = None
            try:
                overlay.finished.disconnect(self._on_finished)
            except TypeError:
                pass
            finish = getattr(overlay, "_finish", None)
            if callable(finish):
                try:
                    finish("cancel")
                except Exception:
                    overlay.close()
            else:
                overlay.close()
            overlay.deleteLater()
        for widget in list(self.pinned):
            widget.close()
        self.pinned.clear()
        self.restore_suspended_modals()
        self.restore_suspended_top_levels()
        # Ensure any open session freeze is released on quit.
        desk = self._desk_app()
        while desk is not None and int(
            getattr(desk, "_screenshot_session_depth", 0)
        ) > 0:
            end = getattr(desk, "_end_screenshot_session", None)
            if not callable(end):
                break
            end()
