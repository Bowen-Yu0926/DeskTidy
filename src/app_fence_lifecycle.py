"""Fence lifecycle extracted from ``DeskTidyApp``.

Owns fence (re)build, snapshot restore, icon-rebuild heuristics, overlay
retirement, cursor-hover sync, the ``show_fences`` composition loop and fence
signal wiring. Method bodies are copied verbatim from the original class;
behaviour is unchanged.

``DeskTidyApp`` inherits this as ``FenceLifecycleMixin``, so every ``self.*``
resolves through the full MRO at runtime. No ``src.app`` import here
(circular).
"""

from __future__ import annotations

import time
from pathlib import Path

from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QCursor
from PyQt6.QtWidgets import QWidget

from src.app_logging import get_logger
from src.fence_layout import get_fence_geometry
from src.fence_pages import fence_on_page
from src.fence_rules import get_portal_path, is_portal_fence
from src.public_desktop import clear_ephemeral_public_items
from src.settings import save_settings
from src.ui.fence_widget import FenceWidget


class FenceLifecycleMixin:
    """Fence (re)build / restore / signal wiring / retirement."""

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
            painted = set(FenceLifecycleMixin._fence_painted_pin_keys(fence))
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
        self,
        *,
        force_rebuild: bool = False,
        relayout_public: bool | None = None,
        sync_public: bool = True,
    ) -> None:
        started = time.perf_counter()
        # Soft-hide only while the switch freeze owns a batch. Pending is set
        # before warmup processEvents, and the batch appears only inside the
        # freeze — clearing pending when the batch is still None dropped the
        # real swap onto the normal show path. Stale pending without a batch
        # must not soft-hide either; finish still owns clearing the flag.
        page_switch = bool(
            getattr(self, "_page_switch_ensure_pending", False)
        ) and getattr(self, "_page_switch_geo_batch", None) is not None
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

        # Collect leavers first; _swap_page_overlays parks/retires them before
        # any incoming show (page switch and settings/rebuild alike) so pages
        # never stack during the reveal.
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

            # Hide leavers before any incoming show (page switch and
            # settings/rebuild alike) — park-after-reveal briefly stacked pages.
            if to_retire or to_park:
                for fence in to_retire:
                    self._retire_overlay_widget(fence)
                for fence_id, fence in to_park:
                    self._park_fence(fence_id, fence, soft=page_switch)
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
                    # Skip only when Qt *and* HWND already match. HiDPI
                    # SetWindowPos used to shrink the HWND while Qt still
                    # reported the full logical size — early-return left a
                    # ~80%-width 文档 strip until restart.
                    if page_switch and native is not None and fence.geometry() == target:
                        hwnd = self._fence_hwnd(fence)
                        if hwnd:
                            try:
                                import win32gui
                                from src.desktop_shell_host import (
                                    qt_logical_to_physical,
                                )

                                left, top, right, bottom = win32gui.GetWindowRect(
                                    int(hwnd)
                                )
                                px, py, pw, ph = qt_logical_to_physical(
                                    int(hwnd), x, y, w, h
                                )
                                if (
                                    abs(int(left) - px) <= 2
                                    and abs(int(top) - py) <= 2
                                    and abs(int(right - left) - pw) <= 2
                                    and abs(int(bottom - top) - ph) <= 2
                                ):
                                    return
                            except Exception:
                                pass
                        else:
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
                # Boot/settings create: fill icons now. Ctor only schedules a
                # deferred refresh — boot heal used to see an empty grid and
                # re-run SHGetFileInfo after public/pet setup (cold-boot hitch).
                if not page_switch:
                    setattr(fence, "_desktidy_defer_refresh", True)
                    try:
                        fence._refresh_impl()
                    except Exception:
                        get_logger().exception(
                            "show_fences: eager icon refresh failed fid=%s",
                            fence_id,
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
            # Leavers already parked/retired above (zero-overlap). Keep loops for
            # any late appends; normally both lists are empty here.
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
            # Boot path passes sync_public=False and refreshes once outside.
            if page_switch:
                self.refresh_public_desktop(immediate=True, relayout=do_relayout)
            elif sync_public:
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
        # Back-reference so FenceWidget.mouseMoveEvent can enumerate siblings
        # on this app's fences (FenceWidget is a top-level widget with no
        # parent chain to walk).
        try:
            fence.desk_app = self
        except Exception:
            pass
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
