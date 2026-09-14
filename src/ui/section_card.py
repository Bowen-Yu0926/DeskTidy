"""Reusable section card for content panels."""

from __future__ import annotations

from PyQt6.QtWidgets import QFrame, QHBoxLayout, QLabel, QVBoxLayout, QWidget


class SectionCard(QFrame):
    """Card container with title, optional hint, body area and footer actions."""

    def __init__(
        self,
        title: str,
        subtitle: str = "",
        parent: QWidget | None = None,
    ):
        super().__init__(parent)
        self.setObjectName("sectionCard")

        shell = QHBoxLayout(self)
        shell.setContentsMargins(0, 0, 0, 0)
        shell.setSpacing(0)
        inlay = QFrame()
        inlay.setObjectName("sectionInlay")
        inlay.setFixedWidth(3)
        shell.addWidget(inlay)

        inner = QWidget()
        inner.setObjectName("sectionCardInner")
        root = QVBoxLayout(inner)
        root.setContentsMargins(14, 12, 14, 12)
        root.setSpacing(10)
        shell.addWidget(inner, stretch=1)

        header = QHBoxLayout()
        header.setSpacing(10)
        title_col = QVBoxLayout()
        title_col.setSpacing(3)
        title_label = QLabel(title)
        title_label.setObjectName("panelTitle")
        title_col.addWidget(title_label)
        self._subtitle_label: QLabel | None = None
        if subtitle:
            self._subtitle_label = QLabel(subtitle)
            self._subtitle_label.setObjectName("sectionHint")
            self._subtitle_label.setWordWrap(True)
            title_col.addWidget(self._subtitle_label)
        header.addLayout(title_col, stretch=1)

        self._header_actions = QHBoxLayout()
        self._header_actions.setSpacing(8)
        header.addLayout(self._header_actions, stretch=0)
        root.addLayout(header)

        self.body = QVBoxLayout()
        self.body.setSpacing(10)
        root.addLayout(self.body, stretch=1)

        self._footer = QHBoxLayout()
        self._footer.setSpacing(8)
        self._footer_host = QWidget()
        self._footer_host.setObjectName("sectionFooter")
        footer_layout = QHBoxLayout(self._footer_host)
        footer_layout.setContentsMargins(0, 8, 0, 0)
        footer_layout.setSpacing(8)
        footer_layout.addLayout(self._footer)
        footer_layout.addStretch()
        root.addWidget(self._footer_host)
        self._footer_host.hide()

    def set_subtitle(self, text: str) -> None:
        if self._subtitle_label is None:
            return
        self._subtitle_label.setText(text)
        self._subtitle_label.setVisible(bool(text))

    def add_header_widget(self, widget: QWidget) -> None:
        self._header_actions.addWidget(widget)

    def add_body_widget(self, widget: QWidget, *, stretch: int = 0) -> None:
        self.body.addWidget(widget, stretch)

    def add_body_layout(self, layout) -> None:
        self.body.addLayout(layout)

    def add_footer_widget(self, widget: QWidget) -> None:
        self._footer_host.show()
        self._footer.addWidget(widget)

    def add_footer_stretch(self) -> None:
        self._footer_host.show()
        self._footer.addStretch()
