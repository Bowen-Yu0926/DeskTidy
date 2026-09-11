"""Help landing — open merged intro+handbook HTML in the system browser."""

from __future__ import annotations

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import (
    QDialog,
    QDialogButtonBox,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPushButton,
    QTextBrowser,
    QVBoxLayout,
    QWidget,
)

from src.help_content import about_html
from src.i18n import APP_NAME_ZH
from src.product_pages import open_desktidy_help


class HelpHomePage(QWidget):
    """Sidebar help page: open the combined manual in the browser."""

    def __init__(
        self,
        settings: dict | None = None,
        parent: QWidget | None = None,
        *,
        include_about: bool = True,
        initial_topic: str | None = None,
        open_product: bool = True,
    ) -> None:
        super().__init__(parent)
        del open_product  # always browser; kept for call-site compatibility
        self._settings = settings if isinstance(settings, dict) else {}
        self.setObjectName("helpHomePage")

        root = QVBoxLayout(self)
        root.setContentsMargins(8, 8, 8, 8)
        root.setSpacing(14)

        title = QLabel(f"{APP_NAME_ZH} 使用手册")
        title.setObjectName("pageTitle")
        root.addWidget(title)

        tip = QLabel(
            "本页打开的是 DeskTidy 介绍与使用手册（系统浏览器）。"
            "DeskNote 笔记帮助请在 DeskNote 窗口按 F1 单独打开。"
        )
        tip.setWordWrap(True)
        tip.setObjectName("sectionHint")
        root.addWidget(tip)

        row = QHBoxLayout()
        row.setSpacing(10)
        self._btn_open = QPushButton("在浏览器中打开")
        self._btn_open.setObjectName("primaryBtn")
        self._btn_open.clicked.connect(self._open_browser_manual)
        row.addWidget(self._btn_open)

        self._guide_btn = QPushButton("入门指引")
        self._guide_btn.setObjectName("guideOpenBtn")
        self._guide_btn.setToolTip("在桌面上再次查看分步小提示")
        self._guide_btn.clicked.connect(self._open_first_run_guide)
        row.addWidget(self._guide_btn)
        row.addStretch(1)
        root.addLayout(row)

        # Back-compat attributes used by older tests / callers.
        self._topics = None
        self._btn_product = self._btn_open
        self._btn_handbook = self._btn_open
        self._btn_browser = self._btn_open

        if include_about:
            about = QTextBrowser()
            about.setObjectName("helpBrowser")
            about.setOpenExternalLinks(False)
            about.setHtml(about_html())
            about.setMinimumHeight(180)
            root.addWidget(about, stretch=1)
        else:
            root.addStretch(1)

        if initial_topic:
            # Topic deep-links still open the merged page (browser scrolls via TOC).
            self._open_browser_manual()

    def set_settings(self, settings: dict | None) -> None:
        self._settings = settings if isinstance(settings, dict) else {}

    def select_topic(self, topic_id: str | None) -> None:
        del topic_id
        self._open_browser_manual()

    def show_product(self) -> None:
        self._open_browser_manual()

    def show_handbook(self, topic_id: str | None = None) -> None:
        del topic_id
        self._open_browser_manual()

    def _open_browser_manual(self) -> None:
        if open_desktidy_help(self._settings):
            return
        QMessageBox.warning(
            self,
            "无法打开帮助",
            "未找到帮助页模板（docs/desktidy.html）。请确认安装完整。",
        )

    def _open_first_run_guide(self) -> None:
        from src.ui.first_run_guide import show_first_run_guide

        show_first_run_guide(
            self.window() if self.window() else self,
            self._settings,
            mark_done=False,
        )


# Kept for imports / tests that still reference the old panel class name.
class HelpContentPanel(HelpHomePage):
    """Compatibility alias — same browser-first help landing."""

    def __init__(self, *args, **kwargs) -> None:  # noqa: ANN002, ANN003
        kwargs.setdefault("include_about", True)
        kwargs.pop("open_product", None)
        super().__init__(*args, **kwargs)


class HelpDialog(QDialog):
    def __init__(
        self,
        settings: dict | None = None,
        parent: QWidget | None = None,
        *,
        initial_topic: str | None = None,
    ) -> None:
        super().__init__(parent)
        del initial_topic
        self.setWindowTitle(f"帮助 — {APP_NAME_ZH}")
        self.setModal(True)
        self.resize(520, 420)
        self.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
        self.setObjectName("helpDialog")

        root = QVBoxLayout(self)
        root.setContentsMargins(16, 16, 16, 16)
        self._panel = HelpHomePage(
            settings,
            self,
            include_about=True,
            initial_topic=None,
        )
        root.addWidget(self._panel, stretch=1)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok)
        ok = buttons.button(QDialogButtonBox.StandardButton.Ok)
        if ok is not None:
            ok.setText("关闭")
        buttons.accepted.connect(self.accept)
        root.addWidget(buttons)


def show_help(
    parent: QWidget | None = None,
    settings: dict | None = None,
    *,
    topic_id: str | None = None,
    manual: bool = False,
) -> None:
    """Open the merged DeskTidy help page in the system browser."""
    del parent, topic_id, manual
    if open_desktidy_help(settings):
        return
    QMessageBox.warning(
        None,
        "无法打开帮助",
        "未找到帮助页模板（docs/desktidy.html）。请确认安装完整。",
    )


def show_operation_manual(
    parent: QWidget | None = None, settings: dict | None = None
) -> None:
    """Open the same merged help page (intro + handbook)."""
    show_help(parent, settings)
