"""Form controls that ignore hover-wheel value changes.

Closed combos / unfocused spins inside a scroll area must not change on
wheel — the page should scroll instead (common desktop settings UX).
"""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QComboBox, QDoubleSpinBox, QSlider, QSpinBox


class NoWheelComboBox(QComboBox):
    """Combo that ignores mouse wheel unless the popup list is open."""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        # Avoid WheelFocus granting focus on the first hover-wheel tick.
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def wheelEvent(self, event) -> None:
        if self.view().isVisible():
            super().wheelEvent(event)
        else:
            event.ignore()


class NoWheelSpinBox(QSpinBox):
    """Spin that ignores mouse wheel unless it has keyboard focus."""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def wheelEvent(self, event) -> None:
        if self.hasFocus():
            super().wheelEvent(event)
        else:
            event.ignore()


class NoWheelDoubleSpinBox(QDoubleSpinBox):
    """Double spin that ignores mouse wheel unless it has keyboard focus."""

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def wheelEvent(self, event) -> None:
        if self.hasFocus():
            super().wheelEvent(event)
        else:
            event.ignore()


class NoWheelSlider(QSlider):
    """Slider that ignores hover-wheel so the parent scroll area can move."""

    def __init__(self, orientation=Qt.Orientation.Horizontal, parent=None) -> None:
        super().__init__(orientation, parent)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)

    def wheelEvent(self, event) -> None:
        if self.hasFocus():
            super().wheelEvent(event)
        else:
            event.ignore()
