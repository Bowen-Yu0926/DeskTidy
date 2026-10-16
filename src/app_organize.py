"""One-click organize + background worker + snapshot merge extracted from ``DeskTidyApp``.

Owns the organize request path, the float-page reveal helper, the worker
completion merge and the background ``_do_organize`` launcher. Method bodies are
copied verbatim from the original class; behaviour is unchanged.

``DeskTidyApp`` inherits this as ``OrganizeMixin``, so every ``self.*`` resolves
through the full MRO at runtime. No ``src.app`` import here (circular).
"""

from __future__ import annotations

from src.app_logging import get_logger
from src.desktop_scanner import invalidate_desktop_scan_cache
from src.i18n import APP_NAME_ZH
from src.organizer import organize_desktop
from src.settings import save_settings


class OrganizeMixin:
    """One-click organize request / worker / snapshot merge."""

    def _on_organize_requested(self, float_page_ids=None) -> None:
        invalidate_desktop_scan_cache()
        self._loose_sync_needed = True
        self._public_force_relayout = True
        # Switch before refresh so floats on the target page are created visible.
        self._reveal_organize_float_pages(float_page_ids)
        self.refresh_fences(refresh_public=True)

    def _reveal_organize_float_pages(self, float_page_ids) -> None:
        """Switch to a page that just received floats so they are not 'missing'."""
        pages = list(float_page_ids or [])
        if not pages:
            return
        try:
            current = int(self._current_page())
        except (TypeError, ValueError):
            current = 0
        dest = None
        for raw in pages:
            try:
                pid = int(raw)
            except (TypeError, ValueError):
                continue
            if pid != current:
                dest = pid
                break
        if dest is None:
            return
        self.switch_page(dest)

    def _merge_organize_snapshot(self, snap: dict) -> None:
        """Apply worker organize results onto live settings + fence widgets."""
        self.settings["organize_mode"] = snap.get("organize_mode", "virtual")
        if isinstance(snap.get("public_desktop_items"), list):
            self.settings["public_desktop_items"] = snap["public_desktop_items"]
        snap_by_id = {
            str(f.get("id")): f
            for f in snap.get("fences") or []
            if isinstance(f, dict) and f.get("id")
        }
        live_ids = {
            str(f.get("id"))
            for f in self.settings.get("fences") or []
            if isinstance(f, dict) and f.get("id")
        }
        for fid, fence_cfg in snap_by_id.items():
            if fid not in live_ids:
                self.settings.setdefault("fences", []).append(fence_cfg)
        for fence_cfg in self.settings.get("fences") or []:
            if not isinstance(fence_cfg, dict):
                continue
            fid = str(fence_cfg.get("id") or "")
            src = snap_by_id.get(fid)
            if src is None:
                continue
            if "virtual_items" in src:
                fence_cfg["virtual_items"] = list(src.get("virtual_items") or [])
        live_by_id = {
            str(f.config.get("id")): f
            for f in self.fences
            if getattr(f, "config", None) and f.config.get("id")
        }
        for fence_cfg in self.settings.get("fences") or []:
            fid = str(fence_cfg.get("id") or "")
            widget = live_by_id.get(fid)
            if widget is not None:
                widget.config = fence_cfg

    def _finish_organize_worker(self, payload, *, silent: bool) -> None:
        self._organize_inflight = False
        if isinstance(payload, Exception):
            get_logger().exception("organize_desktop failed")
            if not silent:
                self.tray.show_message(APP_NAME_ZH, "整理失败，请稍后重试")
            return
        result, snap = payload
        try:
            self._merge_organize_snapshot(snap)
            save_settings(self.settings)
        except OSError:
            pass
        self.window.refresh_data_if_visible()
        self._on_organize_requested(
            float_page_ids=getattr(result, "float_page_ids", None)
        )
        if not silent:
            self.tray.show_message(APP_NAME_ZH, result.summary)

    def _do_organize(self, *, silent: bool = False) -> None:
        if getattr(self, "_organize_inflight", False):
            return
        self._organize_inflight = True
        from copy import deepcopy

        from src.qt_main_thread import call_on_main_thread, ensure_main_thread_bridge

        ensure_main_thread_bridge(self.qt_app)
        snap = deepcopy(self.settings)
        exclude = list(snap.get("exclude_patterns") or [])

        import threading

        def _worker() -> None:
            try:
                result = organize_desktop(
                    settings=snap, exclude=exclude, persist=False
                )
                payload = (result, snap)
            except Exception as exc:
                payload = exc
            # Never QTimer.singleShot from a raw thread — completion never ran
            # → tray「一键整理」looked broken (_organize_inflight stuck / no toast).
            call_on_main_thread(
                lambda: self._finish_organize_worker(payload, silent=silent)
            )

        threading.Thread(
            target=_worker, name="desktidy-organize", daemon=True
        ).start()
