"""Fence layout extracted from ``DeskTidyApp``.

Owns fence page moves, style preview/preset application, per-fence opacity and
visibility, hide/dissolve handling, floating-icon recovery after dissolve,
and fence creation / layout signature / save flushing. Method bodies are
copied verbatim from the original class; behaviour is unchanged.

``DeskTidyApp`` inherits this as ``FenceLayoutMixin``, so every ``self.*``
resolves through the full MRO at runtime. No ``src.app`` import here
(circular).
"""

from __future__ import annotations

import uuid

from PyQt6.QtCore import QRect, Qt, QTimer
from PyQt6.QtWidgets import QInputDialog

from src.fence_layout import (
    place_new_fence_rect,
    save_fence_geometry,
    save_visible_fences,
)
from src.fence_pages import get_fence_pages, set_fence_pages
from src.i18n import APP_NAME_ZH
from src.public_desktop import visible_floating_items
from src.settings import save_settings
from src.ui.fence_widget import FenceWidget
from src.ui.styles import fence_theme_defaults


class FenceLayoutMixin:
    """Fence page move / style / visibility / create / layout save."""

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
