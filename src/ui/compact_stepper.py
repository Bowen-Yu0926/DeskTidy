"""Compact − value + stepper for small integer settings (e.g. pin count)."""

from __future__ import annotations

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtWidgets import QFrame, QHBoxLayout, QLabel, QPushButton, QWidget


class CompactCountStepper(QFrame):
    """Pill stepper: [ − ]  n  [ + ]  unit — ignores hover-wheel (buttons only)."""

    valueChanged = pyqtSignal(int)

    def __init__(
        self,
        minimum: int = 1,
        maximum: int = 5,
        value: int = 1,
        *,
        suffix: str = "",
        parent: QWidget | None = None,
    ) -> None:
        super().__init__(parent)
        self.setObjectName("compactCountStepper")
        self._min = int(minimum)
        self._max = int(maximum)
        self._value = max(self._min, min(self._max, int(value)))

        row = QHBoxLayout(self)
        row.setContentsMargins(4, 4, 8, 4)
        row.setSpacing(0)

        self._minus = QPushButton("−")
        self._minus.setObjectName("stepperBtn")
        self._minus.setCursor(Qt.CursorShape.PointingHandCursor)
        self._minus.setFixedSize(32, 32)
        self._minus.clicked.connect(self._dec)
        row.addWidget(self._minus)

        self._value_label = QLabel(str(self._value))
        self._value_label.setObjectName("stepperValue")
        self._value_label.setAlignment(
            Qt.AlignmentFlag.AlignCenter | Qt.AlignmentFlag.AlignVCenter
        )
        self._value_label.setMinimumWidth(36)
        row.addWidget(self._value_label)

        self._plus = QPushButton("+")
        self._plus.setObjectName("stepperBtn")
        self._plus.setCursor(Qt.CursorShape.PointingHandCursor)
        self._plus.setFixedSize(32, 32)
        self._plus.clicked.connect(self._inc)
        row.addWidget(self._plus)

        if suffix:
            unit = QLabel(suffix)
            unit.setObjectName("stepperUnit")
            row.addSpacing(8)
            row.addWidget(unit)

        self._sync_buttons()

    def setRange(self, minimum: int, maximum: int) -> None:
        self._min = int(minimum)
        self._max = int(maximum)
        self.setValue(self._value)

    def setValue(self, value: int) -> None:
        new = max(self._min, min(self._max, int(value)))
        if new == self._value:
            self._sync_buttons()
            return
        self._value = new
        self._value_label.setText(str(self._value))
        self._sync_buttons()
        self.valueChanged.emit(self._value)

    def value(self) -> int:
        return self._value

    def _inc(self) -> None:
        self.setValue(self._value + 1)

    def _dec(self) -> None:
        self.setValue(self._value - 1)

    def _sync_buttons(self) -> None:
        self._minus.setEnabled(self._value > self._min)
        self._plus.setEnabled(self._value < self._max)

    def wheelEvent(self, event) -> None:  # noqa: ANN001
        # Never change on hover-wheel; outer scroll areas keep the gesture.
        event.ignore()
