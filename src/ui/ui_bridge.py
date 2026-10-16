"""UI bridge: marshal background-thread callbacks onto the Qt GUI thread."""

from __future__ import annotations

from PyQt6.QtCore import QObject, pyqtSignal


class _UiBridge(QObject):
    """Marshal background-thread callbacks onto the Qt GUI thread."""

    refresh_fences = pyqtSignal()
    file_created = pyqtSignal(str)
    file_removed = pyqtSignal(str)
    file_moved = pyqtSignal(str, str)
    portal_changed = pyqtSignal(str)
