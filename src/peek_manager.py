"""Peek mode: temporarily raise fences above all windows."""

from __future__ import annotations

from typing import Callable


class PeekManager:
    """Toggle or hold peek mode for fence widgets."""

    def __init__(self, get_fences: Callable[[], list], on_peek_changed: Callable[[bool], None] | None = None):
        self._get_fences = get_fences
        self._on_peek_changed = on_peek_changed
        self._active = False

    @property
    def active(self) -> bool:
        return self._active

    def toggle(self) -> bool:
        self._active = not self._active
        self._apply(self._active)
        if self._on_peek_changed:
            self._on_peek_changed(self._active)
        return self._active

    def enter(self) -> None:
        was_active = self._active
        self._active = True
        self._apply(True)
        if not was_active and self._on_peek_changed:
            self._on_peek_changed(True)

    def reapply(self) -> None:
        """Re-apply current peek state to newly created fence widgets."""
        if self._active:
            self._apply(True)

    def exit(self) -> None:
        if self._active:
            self._active = False
            self._apply(False)
            if self._on_peek_changed:
                self._on_peek_changed(False)

    def _apply(self, peek: bool) -> None:
        for fence in self._get_fences():
            fence.set_peek_mode(peek)
