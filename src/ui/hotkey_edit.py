"""Line edit that records a global hotkey from a key press."""

from __future__ import annotations

from PyQt6.QtCore import Qt, pyqtSignal
from PyQt6.QtGui import QKeyEvent, QKeySequence
from PyQt6.QtWidgets import QLineEdit


def key_event_to_hotkey(event: QKeyEvent) -> str | None:
    """Convert a key event into a 'Ctrl+Shift+S' style string, or None if incomplete."""
    key = event.key()
    if key in (
        Qt.Key.Key_Control,
        Qt.Key.Key_Shift,
        Qt.Key.Key_Alt,
        Qt.Key.Key_Meta,
        Qt.Key.Key_AltGr,
        Qt.Key.Key_unknown,
    ):
        return None

    parts: list[str] = []
    mods = event.modifiers()
    if mods & Qt.KeyboardModifier.ControlModifier:
        parts.append("Ctrl")
    if mods & Qt.KeyboardModifier.AltModifier:
        parts.append("Alt")
    if mods & Qt.KeyboardModifier.ShiftModifier:
        parts.append("Shift")
    if mods & Qt.KeyboardModifier.MetaModifier:
        parts.append("Win")

    if Qt.Key.Key_F1 <= key <= Qt.Key.Key_F24:
        parts.append(f"F{key - Qt.Key.Key_F1 + 1}")
        return "+".join(parts)

    if key == Qt.Key.Key_Space:
        parts.append("Space")
        return "+".join(parts)

    if key in (
        Qt.Key.Key_Left,
        Qt.Key.Key_Right,
        Qt.Key.Key_Up,
        Qt.Key.Key_Down,
    ):
        parts.append(
            {
                Qt.Key.Key_Left: "Left",
                Qt.Key.Key_Right: "Right",
                Qt.Key.Key_Up: "Up",
                Qt.Key.Key_Down: "Down",
            }[key]
        )
        return "+".join(parts)

    text = QKeySequence(key).toString(QKeySequence.SequenceFormat.NativeText).strip()
    if not text:
        return None
    # NativeText may already include modifiers on some platforms; keep letter only.
    if "+" in text:
        text = text.split("+")[-1].strip()
    if len(text) == 1:
        text = text.upper()
    parts.append(text)
    return "+".join(parts)


class HotkeyEdit(QLineEdit):
    """Click and press a shortcut; Backspace/Delete clears. Does not type characters."""

    hotkey_changed = pyqtSignal(str)
    capture_began = pyqtSignal()
    capture_ended = pyqtSignal()

    def __init__(self, text: str = "", parent=None) -> None:
        super().__init__(text, parent)
        self.setReadOnly(True)
        self.setPlaceholderText("点击后按下快捷键，Delete 清空")
        self.setClearButtonEnabled(False)

    def focusInEvent(self, event) -> None:
        super().focusInEvent(event)
        self.capture_began.emit()

    def focusOutEvent(self, event) -> None:
        super().focusOutEvent(event)
        self.capture_ended.emit()

    def keyPressEvent(self, event: QKeyEvent) -> None:
        key = event.key()
        if key in (Qt.Key.Key_Backspace, Qt.Key.Key_Delete, Qt.Key.Key_Escape):
            if self.text():
                self.clear()
                self.hotkey_changed.emit("")
            event.accept()
            return

        hotkey = key_event_to_hotkey(event)
        if hotkey is None:
            event.accept()
            return

        self.setText(hotkey)
        self.hotkey_changed.emit(hotkey)
        event.accept()

    def mousePressEvent(self, event) -> None:
        super().mousePressEvent(event)
        self.selectAll()
