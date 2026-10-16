"""Page switching extracted from ``DeskTidyApp``.

Owns page navigation, the paint-freeze composition cycle, and the overlay
visibility / Z-order reconciliation that follows each A-B page switch. Method
bodies are copied verbatim from the original class; behaviour is unchanged.

``DeskTidyApp`` inherits this as ``PageSwitchMixin``, so every ``self.*``
resolves through the full MRO at runtime. No ``src.app`` import here
(circular).
"""

from __future__ import annotations

import time
from contextlib import contextmanager

from PyQt6.QtCore import QEventLoop, Qt, QTimer
from PyQt6.QtWidgets import QWidget

from src.settings import save_settings
from src.ui.fence_widget import FenceWidget
from src.ui.page_indicator import PageIndicatorWidget


class PageSwitchMixin:
    """Page navigation + paint-freeze overlay swap."""

    def switch_page(self, page_id: int) -> None:
        started = time.perf_counter()
        page_id = int(page_id)
        self._release_stuck_grabs()
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
                        # Always sync Qt cache after physical DeferWindowPos —
                        # skipping when geometry() already matched left a
                        # short HWND on HiDPI (logical SetWindowPos bug).
                        widget.setGeometry(QRect(int(x), int(y), int(w), int(h)))
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

        # Host-wide WS_EX_TRANSPARENT would click-through every public float.
        self._ensure_public_host_mouse_opaque()

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
            # Foreign app FG: keep fences/pet under SoftMgr — never HWND_TOP.
            if self._foreign_app_owns_foreground():
                self._seat_live_overlays_under_apps()
                return
            # Own chrome FG (pet chips): seat under other open apps, then raise
            # pet above sibling fences only (hair clearance).
            self._seat_live_overlays_under_apps()
            if pet_hwnd:
                try:
                    from src.desktop_shell_host import raise_overlay_in_desktop_band

                    raise_overlay_in_desktop_band(int(pet_hwnd))
                except Exception:
                    pass
            nudge = getattr(pet, "_nudge_clear_of_fence_bottoms", None) if pet else None
            if callable(nudge):
                try:
                    nudge()
                except Exception:
                    pass
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
        # Pet is raised above fences; open vault/todo must stay above the pet.
        self._raise_open_tool_panels_above_pet()
        pet = getattr(self, "pet_widget", None)
        nudge = getattr(pet, "_nudge_clear_of_fence_bottoms", None) if pet else None
        if callable(nudge):
            try:
                nudge()
            except Exception:
                pass

    def _raise_open_tool_panels_above_pet(self) -> None:
        """Keep visible vault/todo panels above the desktop pet sprite."""
        from src.desktop_shell_host import raise_overlay_in_desktop_band

        for attr in ("todo_panel", "vault_panel"):
            panel = getattr(self, attr, None)
            if panel is None:
                continue
            try:
                if not panel.isVisible():
                    continue
                hwnd = int(panel.winId()) if panel.winId() else 0
            except RuntimeError:
                continue
            if not hwnd:
                continue
            try:
                raise_overlay_in_desktop_band(int(hwnd))
                panel.raise_()
            except (RuntimeError, Exception):
                continue

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

    def _seat_live_overlays_under_apps(self) -> None:
        """Force live fences/pet into the desktop band (under SoftMgr / WeChat).

        Page-switch ShowWindow / batch reveal can leave the 文档 strip covering
        normal APPWINDOWs until the next foreign-FG sink. ``force=True`` bypasses
        the HWND_BOTTOM rate limit so the seat always lands after a page flip.
        """
        from src.desktop_shell_host import place_overlay_in_desktop_band

        for fence in list(getattr(self, "fences", None) or ()):
            try:
                if bool(getattr(fence, "_desktidy_soft_parked", False)):
                    continue
                hwnd = self._fence_hwnd(fence)
                if hwnd:
                    place_overlay_in_desktop_band(int(hwnd), force=True)
            except RuntimeError:
                continue
        pet = getattr(self, "pet_widget", None)
        if pet is None:
            return
        try:
            if not pet.isVisible():
                return
            hwnd = int(pet.winId()) if pet.winId() else 0
        except (RuntimeError, Exception):
            return
        if hwnd:
            try:
                place_overlay_in_desktop_band(int(hwnd), force=True)
            except Exception:
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

        # Seat under open apps *before* any HWND_TOP raise — otherwise 切页
        # leaves 文档 covering SoftMgr / VS Code until the user Alt+Tabs.
        self._seat_live_overlays_under_apps()
        if self._foreign_app_owns_foreground():
            # Opaque so icons still work once the app yields; stay under FG.
            for fence in list(getattr(self, "fences", None) or ()):
                try:
                    if bool(getattr(fence, "_desktidy_soft_parked", False)):
                        continue
                    set_overlay_mouse_passthrough(fence, False)
                except RuntimeError:
                    continue
            self._sink_overlays_for_foreign_app()
            return

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
        # Open tool panels last — pet used to be appended after vault/todo, so
        # raise_band=True buried the panel under the sprite (「浮窗在宠物下面」).
        todo = getattr(self, "todo_panel", None)
        if todo is not None:
            from src.todos import desktop_todos_enabled

            try:
                if desktop_todos_enabled(self.settings) and todo.isVisible():
                    chrome_widgets.append(todo)
            except RuntimeError:
                pass
        vault = getattr(self, "vault_panel", None)
        if vault is not None:
            try:
                if vault.isVisible():
                    chrome_widgets.append(vault)
            except RuntimeError:
                pass
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
                # Live list can still carry soft-park if reveal was skipped
                # (may_show false mid-switch) — never leave them unmapped.
                if bool(getattr(fence, "_desktidy_soft_parked", False)):
                    self._soft_show_fence_for_page(fence)
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
                if bool(getattr(icon, "_desktidy_soft_parked", False)):
                    try:
                        icon._desktidy_soft_parked = False  # type: ignore[attr-defined]
                    except Exception:
                        pass
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
                if host is not None and not host.isVisible():
                    host.show()
            except Exception:
                pass
        # Raise page chrome only when buried/detached — unconditional HWND_TOP
        # after every A↔B click flashes a solid popup over the wallpaper.
        if self.page_indicator is not None and self.settings.get(
            "show_page_indicator", True
        ):
            if self._page_chrome_needs_raise():
                self._ensure_page_chrome_visible(raise_band=True)

    def _page_nav_hotkey_gate(self) -> bool:
        """True if this next/prev should run; False if a duplicate hotkey fire.

        ``RegisterHotKey`` and the LL-chord fallback both deliver Ctrl+Shift+←/→
        once per keypress. Two pages then become next→next (land on start) and
        soft-park can leave partitions/icons unmapped. Gate only hotkey nav —
        page-bar clicks call ``switch_page`` directly and stay unrestricted.
        """
        now = time.perf_counter()
        gate_until = float(getattr(self, "_page_nav_hotkey_gate_until", 0.0) or 0.0)
        if now < gate_until:
            return False
        self._page_nav_hotkey_gate_until = now + 0.35
        return True

    def next_page(self) -> None:
        if not self._page_nav_hotkey_gate():
            return
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
        if not self._page_nav_hotkey_gate():
            return
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
        from src.desktop_pet import edge_page_indicator_allowed

        # Pet visible → no edge page bar (pet + tabs must not show together).
        # Pet hidden/disabled → edge tabs on the right. Bubbles opt-in → pages on pet.
        if not edge_page_indicator_allowed(self.settings):
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
