"""Outline list + Markdown preview (WebEngine with QTextBrowser fallback)."""

from __future__ import annotations

import json
from pathlib import Path

from PyQt6.QtCore import QObject, QUrl, Qt, pyqtSignal, pyqtSlot
from PyQt6.QtGui import QDesktopServices, QFont
from PyQt6.QtWidgets import (
    QHBoxLayout,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QSizePolicy,
    QStackedWidget,
    QTextBrowser,
    QVBoxLayout,
    QWidget,
)

from src.markdown_preview import (
    OutlineItem,
    extract_outline,
    js_string_literal,
    md_preview_assets_dir,
    render_html,
    webengine_available,
)

_COLLAPSED_STRIP_W = 28


class CollapsibleMdPane(QWidget):
    """Side pane that collapses **horizontally** (left/right), never up/down.

    A vertical handle strip always sits toward the editor. Expanded = body +
    strip; collapsed = strip only (fixed width). ``expanded_changed`` syncs
    View-menu outline/preview toggles.
    """

    expanded_changed = pyqtSignal(bool)

    def __init__(
        self,
        title: str,
        body: QWidget,
        parent: QWidget | None = None,
        *,
        expanded: bool = True,
        handle_on_right: bool = True,
    ) -> None:
        super().__init__(parent)
        self._title = title
        self._body = body
        self._expanded = bool(expanded)
        self._handle_on_right = bool(handle_on_right)
        self._saved_width = 160
        self.setObjectName("notepadCollapsiblePane")

        self._body_host = QWidget()
        self._body_host.setObjectName("notepadPaneBody")
        body_lay = QVBoxLayout(self._body_host)
        body_lay.setContentsMargins(0, 0, 0, 0)
        body_lay.setSpacing(0)
        title_lbl = QPushButton(title)
        title_lbl.setObjectName("notepadPaneTitle")
        title_lbl.setFlat(True)
        title_lbl.setEnabled(False)
        title_lbl.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        body_lay.addWidget(title_lbl)
        body_lay.addWidget(body, stretch=1)

        self._strip = QPushButton()
        self._strip.setObjectName("notepadPaneStrip")
        self._strip.setCursor(Qt.CursorShape.PointingHandCursor)
        self._strip.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._strip.clicked.connect(self._toggle)
        self._strip.setFixedWidth(_COLLAPSED_STRIP_W)
        self._strip.setSizePolicy(
            QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Expanding
        )

        root = QHBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)
        if self._handle_on_right:
            # Outline (left): body | ›  — collapses toward the left.
            root.addWidget(self._body_host, stretch=1)
            root.addWidget(self._strip)
        else:
            # Preview (right): ‹ | body — collapses toward the right.
            root.addWidget(self._strip)
            root.addWidget(self._body_host, stretch=1)

        self.setStyleSheet(
            "QPushButton#notepadPaneTitle {"
            "  text-align: left; padding: 5px 10px; border: none;"
            "  border-bottom: 1px solid #E5E7EB; background: #F8FAFC;"
            "  font-weight: 600; color: #374151;"
            "}"
            "QPushButton#notepadPaneStrip {"
            "  text-align: center; padding: 8px 2px; border: none;"
            "  background: #F1F5F9; font-weight: 600; color: #374151;"
            "  border-left: 1px solid #E5E7EB; border-right: 1px solid #E5E7EB;"
            "}"
            "QPushButton#notepadPaneStrip:hover { background: #EEF2FF; color: #1D4ED8; }"
        )
        self._apply_expanded_ui(emit=False)

    @property
    def body(self) -> QWidget:
        return self._body

    def is_expanded(self) -> bool:
        return bool(self._expanded)

    def set_expanded(self, expanded: bool, *, emit: bool = True) -> None:
        expanded = bool(expanded)
        if self._expanded and not expanded:
            # Remember width before sideways collapse so re-expand restores it.
            w = max(self.width(), self.minimumWidth(), 120)
            if w > _COLLAPSED_STRIP_W + 8:
                self._saved_width = w
        if self._expanded == expanded:
            self._apply_expanded_ui(emit=False)
            return
        self._expanded = expanded
        self._apply_expanded_ui(emit=emit)

    def preferred_expanded_width(self) -> int:
        return max(120, int(self._saved_width))

    def _toggle(self) -> None:
        self.set_expanded(not self._expanded, emit=True)

    def _apply_expanded_ui(self, *, emit: bool) -> None:
        title = self._title
        if self._expanded:
            # Chevrons point toward collapse direction (horizontal, not ▾/▴).
            chevron = "‹" if self._handle_on_right else "›"
            self._strip.setText(chevron)
            self._strip.setToolTip(f"向{'左' if self._handle_on_right else '右'}收起「{title}」")
            self._body_host.setVisible(True)
            self.setMinimumWidth(120)
            self.setMaximumWidth(16777215)
            self.setSizePolicy(
                QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Expanding
            )
        else:
            chevron = "›" if self._handle_on_right else "‹"
            self._strip.setText(chevron + "\n" + "\n".join(title))
            self._strip.setToolTip(f"展开「{title}」")
            self._body_host.setVisible(False)
            self.setFixedWidth(_COLLAPSED_STRIP_W)
            self.setSizePolicy(
                QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Expanding
            )
        if emit:
            self.expanded_changed.emit(self._expanded)


class MarkdownOutlineList(QListWidget):
    """Clickable heading outline; emits 0-based source line."""

    line_activated = pyqtSignal(int)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("notepadOutline")
        self.setMinimumWidth(120)
        self.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.itemActivated.connect(self._on_item)
        self.itemClicked.connect(self._on_item)

    def set_outline(self, items: list[OutlineItem]) -> None:
        self.clear()
        for item in items:
            label = ("  " * max(0, item.level - 1)) + item.title
            row = QListWidgetItem(label)
            row.setData(Qt.ItemDataRole.UserRole, int(item.line))
            self.addItem(row)

    def _on_item(self, item: QListWidgetItem) -> None:
        line = item.data(Qt.ItemDataRole.UserRole)
        if isinstance(line, int):
            self.line_activated.emit(line)


class _FallbackPreview(QTextBrowser):
    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("notepadPreview")
        self.setOpenExternalLinks(False)
        self.setOpenLinks(False)
        self.setReadOnly(True)
        self.anchorClicked.connect(lambda url: QDesktopServices.openUrl(url))
        font = QFont("Segoe UI")
        font.setPointSize(10)
        self.setFont(font)

    def set_markdown(self, md: str, *, base_dir: Path | None = None, theme: str = "") -> None:
        del theme
        self.setHtml(render_html(md, base_dir=base_dir))


class _PreviewBridge(QObject):
    scrollRatioChanged = pyqtSignal(float)
    ready = pyqtSignal()

    @pyqtSlot(float)
    def onPreviewScroll(self, ratio: float) -> None:
        self.scrollRatioChanged.emit(float(ratio))

    @pyqtSlot(str)
    def openExternal(self, href: str) -> None:
        QDesktopServices.openUrl(QUrl(href))

    @pyqtSlot()
    def previewReady(self) -> None:
        self.ready.emit()


class MarkdownPreviewPane(QWidget):
    """WebEngine viewer when available; otherwise QTextBrowser fallback.

    Public API mirrors the old ``MarkdownPreviewBrowser.set_markdown``.
    """

    scroll_ratio_changed = pyqtSignal(float)

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("notepadPreview")
        self._stack = QStackedWidget(self)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.addWidget(self._stack)

        self._fallback = _FallbackPreview()
        self._stack.addWidget(self._fallback)

        self._web = None
        self._channel = None
        self._bridge = None
        self._page_ready = False
        self._pending: tuple[str, Path | None, str] | None = None
        self._engine = "fallback"
        self._suppress_scroll = False

        assets = md_preview_assets_dir()
        if assets is not None and webengine_available():
            try:
                self._init_web(assets)
            except Exception:
                self._web = None
                self._engine = "fallback"

        self._stack.setCurrentWidget(
            self._web if self._web is not None else self._fallback
        )

    def _init_web(self, assets: Path) -> None:
        from PyQt6.QtWebChannel import QWebChannel
        from PyQt6.QtWebEngineCore import QWebEngineSettings
        from PyQt6.QtWebEngineWidgets import QWebEngineView

        view = QWebEngineView(self)
        view.setObjectName("notepadPreviewWeb")
        settings = view.settings()
        settings.setAttribute(
            QWebEngineSettings.WebAttribute.LocalContentCanAccessFileUrls, True
        )
        settings.setAttribute(
            QWebEngineSettings.WebAttribute.JavascriptEnabled, True
        )
        bridge = _PreviewBridge(self)
        bridge.scrollRatioChanged.connect(self._on_bridge_scroll)
        bridge.ready.connect(self._on_page_ready)
        channel = QWebChannel(view.page())
        channel.registerObject("bridge", bridge)
        view.page().setWebChannel(channel)
        viewer = (assets / "viewer.html").resolve()
        view.load(QUrl.fromLocalFile(str(viewer)))
        self._web = view
        self._bridge = bridge
        self._channel = channel
        self._engine = "web"
        self._stack.addWidget(view)
        self._stack.setCurrentWidget(view)

    @property
    def engine(self) -> str:
        return self._engine

    def set_markdown(
        self,
        md: str,
        *,
        base_dir: Path | None = None,
        theme: str = "",
    ) -> None:
        if self._web is None:
            self._fallback.set_markdown(md, base_dir=base_dir, theme=theme)
            return
        if not self._page_ready:
            self._pending = (md, base_dir, theme)
            return
        self._push_markdown(md, base_dir=base_dir, theme=theme)

    def set_scroll_ratio(self, ratio: float) -> None:
        if self._web is None:
            return
        self._suppress_scroll = True
        r = max(0.0, min(1.0, float(ratio)))
        self._web.page().runJavaScript(
            f"window.__desknoteSetScrollRatio && window.__desknoteSetScrollRatio({r});"
        )

        def _clear() -> None:
            self._suppress_scroll = False

        from PyQt6.QtCore import QTimer

        QTimer.singleShot(100, _clear)

    def print_to_pdf(self, path: Path, *, on_done=None) -> bool:
        """Print current preview to PDF. Returns False if WebEngine unavailable."""
        if self._web is None:
            return False
        dest = str(Path(path))

        def _finished(ok: bool) -> None:
            if callable(on_done):
                try:
                    on_done(bool(ok))
                except Exception:
                    pass

        page = self._web.page()
        try:
            page.pdfPrintingFinished.connect(  # type: ignore[attr-defined]
                lambda _p, ok: _finished(bool(ok))
            )
        except Exception:
            from PyQt6.QtCore import QTimer

            QTimer.singleShot(2000, lambda: _finished(Path(dest).is_file()))
        page.printToPdf(dest)
        return True

    def _on_page_ready(self) -> None:
        self._page_ready = True
        if self._pending is not None:
            md, base_dir, theme = self._pending
            self._pending = None
            self._push_markdown(md, base_dir=base_dir, theme=theme)

    def _on_bridge_scroll(self, ratio: float) -> None:
        if self._suppress_scroll:
            return
        self.scroll_ratio_changed.emit(float(ratio))

    def _push_markdown(
        self, md: str, *, base_dir: Path | None, theme: str
    ) -> None:
        assert self._web is not None
        base_url = ""
        if base_dir is not None:
            try:
                base_url = Path(base_dir).resolve().as_uri()
                if not base_url.endswith("/"):
                    base_url += "/"
            except OSError:
                base_url = ""
        opts = {"baseUrl": base_url, "theme": theme or "light"}
        script = (
            "window.__desknoteSetMarkdown && window.__desknoteSetMarkdown("
            f"{js_string_literal(md)}, {json.dumps(opts, ensure_ascii=False)});"
        )
        self._web.page().runJavaScript(script)


# Back-compat alias used by older imports / tests.
MarkdownPreviewBrowser = MarkdownPreviewPane


def refresh_outline_and_preview(
    outline: MarkdownOutlineList | None,
    preview: MarkdownPreviewPane | MarkdownPreviewBrowser | None,
    text: str,
    *,
    base_dir: Path | None,
    show_outline: bool,
    show_preview: bool,
    theme: str = "",
) -> None:
    if outline is not None and show_outline:
        outline.set_outline(extract_outline(text))
    if preview is not None and show_preview:
        preview.set_markdown(text, base_dir=base_dir, theme=theme)
