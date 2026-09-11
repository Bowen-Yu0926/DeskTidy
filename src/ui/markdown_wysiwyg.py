"""Toast UI Editor host for deskNote WYSIWYG mode."""

from __future__ import annotations

import json
from pathlib import Path

from PyQt6.QtCore import QObject, QUrl, pyqtSignal, pyqtSlot
from PyQt6.QtWidgets import QVBoxLayout, QWidget

from src.markdown_preview import js_string_literal, md_preview_assets_dir, webengine_available


class _WysiwygBridge(QObject):
    markdownChanged = pyqtSignal(str)
    ready = pyqtSignal()

    @pyqtSlot(str)
    def onMarkdownChanged(self, md: str) -> None:
        self.markdownChanged.emit(str(md))

    @pyqtSlot()
    def previewReady(self) -> None:
        self.ready.emit()


class MarkdownWysiwygPane(QWidget):
    """Offline Toast UI Editor (WYSIWYG). Emits markdown on change."""

    markdown_changed = pyqtSignal(str)
    engine_ready = pyqtSignal()

    def __init__(self, parent=None) -> None:
        super().__init__(parent)
        self.setObjectName("notepadWysiwyg")
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        self._web = None
        self._bridge = None
        self._page_ready = False
        self._pending: tuple[str, str] | None = None
        self._available = False

        assets = md_preview_assets_dir()
        if (
            assets is not None
            and (assets / "wysiwyg.html").is_file()
            and (assets / "vendor" / "toastui-editor-all.min.js").is_file()
            and webengine_available()
        ):
            try:
                self._init_web(assets)
                self._available = True
            except Exception:
                self._available = False

    @property
    def available(self) -> bool:
        return bool(self._available and self._web is not None)

    def _init_web(self, assets: Path) -> None:
        from PyQt6.QtWebChannel import QWebChannel
        from PyQt6.QtWebEngineCore import QWebEngineSettings
        from PyQt6.QtWebEngineWidgets import QWebEngineView

        view = QWebEngineView(self)
        settings = view.settings()
        settings.setAttribute(
            QWebEngineSettings.WebAttribute.LocalContentCanAccessFileUrls, True
        )
        settings.setAttribute(QWebEngineSettings.WebAttribute.JavascriptEnabled, True)
        bridge = _WysiwygBridge(self)
        bridge.markdownChanged.connect(self.markdown_changed.emit)
        bridge.ready.connect(self._on_ready)
        channel = QWebChannel(view.page())
        channel.registerObject("bridge", bridge)
        view.page().setWebChannel(channel)
        view.load(QUrl.fromLocalFile(str((assets / "wysiwyg.html").resolve())))
        self.layout().addWidget(view)
        self._web = view
        self._bridge = bridge

    def _on_ready(self) -> None:
        self._page_ready = True
        self.engine_ready.emit()
        if self._pending is not None:
            md, theme = self._pending
            self._pending = None
            self.set_markdown(md, theme=theme)

    def set_markdown(self, md: str, *, theme: str = "") -> None:
        if self._web is None:
            return
        if not self._page_ready:
            self._pending = (md, theme)
            return
        script = (
            "window.__desknoteWysiwygSet && window.__desknoteWysiwygSet("
            f"{js_string_literal(md)}, {json.dumps(theme or 'light')});"
        )
        self._web.page().runJavaScript(script)

    def get_markdown(self, callback) -> None:
        """Async read of current markdown via *callback(str)*."""
        if self._web is None:
            callback("")
            return
        self._web.page().runJavaScript(
            "window.__desknoteWysiwygGet ? window.__desknoteWysiwygGet() : ''",
            callback,
        )

    def insert_markdown(self, md: str) -> None:
        """Insert Markdown at the current cursor (Toast UI)."""
        if self._web is None or not md:
            return
        script = (
            "window.__desknoteWysiwygInsert && window.__desknoteWysiwygInsert("
            f"{js_string_literal(md)});"
        )
        self._web.page().runJavaScript(script)
