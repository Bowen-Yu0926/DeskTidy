"""Pinned screenshot image on desktop (Snipaste-style)."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

from PyQt6.QtCore import Qt, QPoint, pyqtSignal
from PyQt6.QtGui import QPixmap
from PyQt6.QtWidgets import (
    QFileDialog,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMenu,
    QPushButton,
    QVBoxLayout,
    QWidget,
)


def save_pixmap_dialog(
    pixmap: QPixmap,
    parent: QWidget | None = None,
    *,
    title: str = "另存为",
    default_name: str = "",
) -> bool:
    """Native Save As for an in-memory screenshot (no Explorer IContextMenu)."""
    if pixmap is None or pixmap.isNull():
        return False
    pictures = Path.home() / "Pictures"
    try:
        pictures.mkdir(parents=True, exist_ok=True)
    except OSError:
        pictures = Path.home()
    name = default_name.strip() or f"截图_{datetime.now().strftime('%Y%m%d_%H%M%S')}.png"
    if Path(name).suffix.lower() not in {".png", ".jpg", ".jpeg"}:
        name = f"{Path(name).stem}.png"
    # Topmost pins cover the modal dialog unless TOPMOST is dropped first.
    topmost = False
    if parent is not None:
        try:
            topmost = bool(parent.windowFlags() & Qt.WindowType.WindowStaysOnTopHint)
            if topmost:
                parent.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, False)
                parent.show()
        except RuntimeError:
            topmost = False
    try:
        path, selected = QFileDialog.getSaveFileName(
            parent,
            title,
            str(pictures / name),
            "PNG 图片 (*.png);;JPEG 图片 (*.jpg *.jpeg)",
        )
    finally:
        if parent is not None and topmost:
            try:
                parent.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
                parent.show()
                parent.raise_()
            except RuntimeError:
                pass
    if not path:
        return False
    out = Path(path)
    suffix = out.suffix.lower()
    if suffix not in {".png", ".jpg", ".jpeg"}:
        if "jpg" in (selected or "").lower() or "jpeg" in (selected or "").lower():
            out = out.with_suffix(".jpg")
        else:
            out = out.with_suffix(".png")
    return bool(pixmap.save(str(out)))


class PinnedImageWidget(QWidget):
    closed = pyqtSignal()

    def __init__(self, pixmap: QPixmap, parent: QWidget | None = None):
        super().__init__(parent)
        self._pixmap = pixmap
        self._drag_pos: QPoint | None = None
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.Window
            | Qt.WindowType.WindowStaysOnTopHint
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
        self.setContextMenuPolicy(Qt.ContextMenuPolicy.DefaultContextMenu)
        self._build_ui()
        self.resize(pixmap.size().boundedTo(pixmap.size()))
        self.setFixedSize(pixmap.size())

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)

        self.container = QFrame()
        self.container.setObjectName("pinnedImage")
        inner = QVBoxLayout(self.container)
        inner.setContentsMargins(2, 2, 2, 2)
        inner.setSpacing(0)

        header = QHBoxLayout()
        header.addStretch()
        close_btn = QPushButton("×")
        close_btn.setObjectName("fenceBtn")
        close_btn.setFixedSize(22, 22)
        close_btn.setToolTip("关闭贴图（双击图片也可关闭）")
        close_btn.clicked.connect(self._close)
        header.addWidget(close_btn)
        inner.addLayout(header)

        self.image_label = QLabel()
        self.image_label.setPixmap(self._pixmap)
        self.image_label.setScaledContents(False)
        inner.addWidget(self.image_label)

        # Children would swallow RMB otherwise — bubble to this widget.
        for child in (self.container, self.image_label, close_btn):
            child.setContextMenuPolicy(Qt.ContextMenuPolicy.NoContextMenu)

        layout.addWidget(self.container)

    def _close(self) -> None:
        self.closed.emit()
        self.close()

    def save_as(self) -> bool:
        return save_pixmap_dialog(self._pixmap, self, title="另存为")

    def contextMenuEvent(self, event) -> None:
        menu = QMenu(self)
        save_act = menu.addAction("另存为(&S)…")
        copy_act = menu.addAction("复制")
        menu.addSeparator()
        close_act = menu.addAction("关闭")
        chosen = menu.exec(event.globalPos())
        if chosen is save_act:
            self.save_as()
        elif chosen is copy_act:
            from PyQt6.QtWidgets import QApplication

            clip = QApplication.clipboard()
            if clip is not None:
                clip.setPixmap(self._pixmap)
        elif chosen is close_act:
            self._close()
        event.accept()

    def mouseDoubleClickEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._close()
            event.accept()
            return
        super().mouseDoubleClickEvent(event)

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_pos = event.globalPosition().toPoint() - self.frameGeometry().topLeft()
            event.accept()

    def mouseMoveEvent(self, event) -> None:
        if self._drag_pos is not None and event.buttons() & Qt.MouseButton.LeftButton:
            self.move(event.globalPosition().toPoint() - self._drag_pos)
            event.accept()

    def mouseReleaseEvent(self, event) -> None:
        self._drag_pos = None
        super().mouseReleaseEvent(event)

    def keyPressEvent(self, event) -> None:
        if event.key() == Qt.Key.Key_Escape:
            self._close()
        else:
            super().keyPressEvent(event)
