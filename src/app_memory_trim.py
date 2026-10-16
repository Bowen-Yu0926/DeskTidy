"""Memory trim / tray-session methods extracted from ``DeskTidyApp``.

Owns the minimized-to-tray handshake, background memory trimming and the
parked-overlay cap eviction (``_pop_oldest``). Method bodies are copied
verbatim from the original class; behaviour is unchanged.

``DeskTidyApp`` inherits this as ``MemoryTrimMixin``, so every ``self.*``
resolves through the full MRO at runtime. No ``src.app`` import here
(circular).
"""

from __future__ import annotations

from typing import Any

from src.i18n import APP_NAME_ZH


class MemoryTrimMixin:
    """Minimized-to-tray handshake + background memory trimming."""

    def _on_minimized_to_tray(self) -> None:
        self.tray.show_message(
            APP_NAME_ZH,
            "程序已最小化到系统托盘，继续在后台运行。",
        )
        self._trim_background_memory()
        self._schedule_overlay_keepalive()

    def _trim_background_memory(self) -> None:
        """Release excess parked overlays in tray mode — keep icon caches warm."""
        try:
            from src.ui.public_icon_widget import clear_label_pixmap_cache

            # Label pixmaps are cheap to rebuild; file/shell icon caches are not.
            clear_label_pixmap_cache()
        except Exception:
            pass
        try:
            from src.icon_utils import trim_display_icon_cache

            # Halve zoomed display pixmaps under memory pressure; keep shell raw.
            trim_display_icon_cache()
        except Exception:
            pass
        try:
            from src.ui.pet_anim import trim_fit_cache

            trim_fit_cache()
        except Exception:
            pass
        if not self.window.isVisible():
            try:
                self.window.release_lazy_pages()
            except Exception:
                pass
            # Tray/background: keep fewer parked widgets (fences still own HWNDs;
            # public floats share one host HWND).
            self._trim_parked_overlays(max_public=15, max_fences=3)

    @staticmethod
    def _pop_oldest(mapping: dict) -> tuple[Any, Any]:
        """Pop the first-inserted (oldest) item from a plain dict.

        dict.popitem() takes no arguments; only OrderedDict supports
        ``popitem(last=False)``. DeskTidy uses plain dicts for parked
        overlays, so pop the first key explicitly (insertion-ordered).
        """
        key = next(iter(mapping))
        return key, mapping.pop(key)

    def _trim_parked_overlays(
        self, *, max_public: int | None = None, max_fences: int | None = None
    ) -> None:
        pub_cap = int(max_public if max_public is not None else self._max_parked_public_icons)
        fence_cap = int(max_fences if max_fences is not None else self._max_parked_fences)
        while len(self._parked_public_icons) > pub_cap:
            _, victim = self._pop_oldest(self._parked_public_icons)
            if victim is not None:
                self._retire_overlay_widget(victim)
        while len(self._parked_fences) > fence_cap:
            _, victim = self._pop_oldest(self._parked_fences)
            if victim is not None:
                self._retire_overlay_widget(victim)
