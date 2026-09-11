"""Structured organize dry-run panel for the organize page."""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QFrame,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QVBoxLayout,
    QWidget,
)

from src.organizer import OrganizePreviewModel


class OrganizeStatTile(QFrame):
    """Compact metric used on the organize page strip (not the preview panel)."""

    def __init__(self, caption: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("organizeStatTile")
        lay = QVBoxLayout(self)
        lay.setContentsMargins(14, 10, 14, 10)
        lay.setSpacing(2)
        self._value = QLabel("0")
        self._value.setObjectName("organizeStatValue")
        self._value.setAlignment(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter)
        self._caption = QLabel(caption)
        self._caption.setObjectName("organizeStatCaption")
        self._caption.setWordWrap(True)
        lay.addWidget(self._value)
        lay.addWidget(self._caption)

    def set_value(self, value: int | str) -> None:
        self._value.setText(str(value))


class OrganizePreviewPanel(QFrame):
    """Pin list + skip summary for organize dry-run (no duplicate metric tiles)."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("organizePreviewPanel")

        root = QVBoxLayout(self)
        root.setContentsMargins(14, 14, 14, 14)
        root.setSpacing(10)

        head = QVBoxLayout()
        head.setSpacing(2)
        title = QLabel("整理预览")
        title.setObjectName("panelTitle")
        head.addWidget(title)
        self._hint = QLabel("干跑预览 · 不会改动桌面")
        self._hint.setObjectName("sectionHint")
        self._hint.setWordWrap(True)
        head.addWidget(self._hint)
        root.addLayout(head)

        self._summary = QLabel("")
        self._summary.setObjectName("organizePreviewSummary")
        self._summary.setWordWrap(True)
        root.addWidget(self._summary)

        pins_label = QLabel("将钉入")
        pins_label.setObjectName("organizePreviewSection")
        root.addWidget(pins_label)
        self._pins_label = pins_label

        self._pins = QListWidget()
        self._pins.setObjectName("organizePreviewList")
        self._pins.setAlternatingRowColors(True)
        self._pins.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._pins.setMinimumHeight(100)
        root.addWidget(self._pins, stretch=1)

        self._pins_empty = QLabel("点「预览」查看将钉入哪些项")
        self._pins_empty.setObjectName("organizePreviewEmpty")
        self._pins_empty.setWordWrap(True)
        root.addWidget(self._pins_empty)

        self._buckets = QLabel("")
        self._buckets.setObjectName("organizePreviewBuckets")
        self._buckets.setWordWrap(True)
        self._buckets.setTextInteractionFlags(
            Qt.TextInteractionFlag.TextSelectableByMouse
        )
        root.addWidget(self._buckets)

        self.clear()

    def clear(self) -> None:
        self._summary.setText("尚未预览")
        self._pins.clear()
        self._pins.setVisible(False)
        self._pins_label.setVisible(False)
        self._pins_empty.setVisible(True)
        self._pins_empty.setText("点「预览」或「刷新」查看干跑结果")
        self._buckets.clear()
        self._buckets.setVisible(False)
        self._hint.setText("干跑预览 · 不会改动桌面")

    def apply_model(self, model: OrganizePreviewModel) -> None:
        self._hint.setText("干跑结果 · 不会改动桌面")
        if model.will_count:
            self._summary.setText(
                f"将钉入 {model.will_count} 项 · 跳过 {model.skip_count} · 扫描 {model.scan_total}"
            )
        else:
            self._summary.setText(
                f"没有新项可钉入 · 已扫描 {model.scan_total} · 跳过 {model.skip_count}"
            )

        self._pins.clear()
        if model.pins:
            self._pins_label.setVisible(True)
            self._pins.setVisible(True)
            self._pins_empty.setVisible(False)
            for pin in model.pins:
                item = QListWidgetItem(f"{pin.name}  →  {pin.target}")
                item.setToolTip(f"规则：{pin.category}\n目标：{pin.target}")
                self._pins.addItem(item)
            if model.pin_omitted:
                more = QListWidgetItem(f"…另有 {model.pin_omitted} 个")
                more.setFlags(Qt.ItemFlag.NoItemFlags)
                self._pins.addItem(more)
        else:
            self._pins_label.setVisible(False)
            self._pins.setVisible(False)
            self._pins_empty.setVisible(True)
            self._pins_empty.setText("当前规则下没有需要新钉入的文件")

        bucket_lines: list[str] = []
        for bucket in model.buckets:
            if bucket.count <= 0:
                continue
            if bucket.list_names and bucket.names:
                sample = "、".join(bucket.names[:4])
                extra = bucket.count - min(4, len(bucket.names))
                tail = f" 等 {bucket.count}" if extra > 0 else f" · {bucket.count}"
                line = f"{bucket.title}：{sample}{tail}"
            else:
                line = f"{bucket.title}：{bucket.count}"
            bucket_lines.append(line)

        if bucket_lines:
            self._buckets.setText("\n".join(bucket_lines[:6]))
            self._buckets.setVisible(True)
        else:
            self._buckets.clear()
            self._buckets.setVisible(False)
