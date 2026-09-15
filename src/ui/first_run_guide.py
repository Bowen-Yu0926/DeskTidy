"""First-install walkthrough — desktop tip cards near real chrome."""

from __future__ import annotations

from typing import Callable

from PyQt6.QtCore import QPoint, QRect, Qt, QTimer
from PyQt6.QtGui import QGuiApplication
from PyQt6.QtWidgets import (
    QApplication,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from src.i18n import APP_NAME_ZH
from src.settings import save_settings
from src.ui.screen_snap import work_screen
from src.ui.styles import get_theme_palette, normalize_theme

# (title, body, anchor) — anchor: fence | rail | todo | tray | center
GUIDE_STEPS: tuple[tuple[str, str, str], ...] = (
    (
        "桌面上的分区",
        "文件仍在系统桌面里，分区只是把它们分类钉选出来。删分区不会删文件。",
        "fence",
    ),
    (
        "一键整理",
        "按规则把图标、文档自动钉进对应分区。主窗口「整理」页可以改规则和白名单。",
        "fence",
    ),
    (
        "右侧浮标",
        "单击切换「工作 / 文档」等分页；右键分页可新建分区。录屏、笔记、纪要、计算器、待办等扩展按钮也在这条栏上。",
        "rail",
    ),
    (
        "区域截图",
        "按 F1（可在设置里改）冻结画面后框选区域，可复制、保存到文件，或贴到桌面。托盘菜单也有入口。",
        "center",
    ),
    (
        "截图贴图",
        "按 F2 把最近一张截图贴到桌面，可叠多张；贴图右键可另存或复制。扩展功能里可调同时贴图上限。",
        "center",
    ),
    (
        "桌面录屏",
        "按 F3 或点浮标「录屏」开始/结束；开始时会选屏幕。文件默认保存在安装目录「录屏」文件夹（可改路径）。",
        "rail",
    ),
    (
        "内置记事本",
        "用桌面或开始菜单的 DeskNote 快捷方式打开多标签记事本；关窗口会保留页签，下次还在。关掉某个页签会删除对应笔记文件。",
        "rail",
    ),
    (
        "会议纪要",
        "双击浮标「纪要」新建或打开 Word 格式纪要；右键打开纪要所在文件夹。路径可在「扩展功能」里配置。",
        "rail",
    ),
    (
        "桌面待办",
        "浮标「待办」可显示桌面便签：添加、勾选完成、删除。可在扩展功能里开关分页栏入口。账号密码请用侧栏「账号管理」启用桌面「账」浮标。",
        "todo",
    ),
    (
        "关窗口不停",
        "关闭主窗口后可继续在托盘运行，桌面分区与扩展功能保持可用。帮助页随时能再打开本指引。",
        "tray",
    ),
)


def _resolve_desk_app():
    app = QApplication.instance()
    return getattr(app, "_desktidy_app", None) if app is not None else None


_active_tip = None


def _work_geo(anchor: QRect | QPoint | None = None) -> QRect:
    if isinstance(anchor, QRect) and anchor.width() > 0 and anchor.height() > 0:
        screen = QGuiApplication.screenAt(anchor.center())
        if screen is not None:
            return screen.availableGeometry()
    if isinstance(anchor, QPoint):
        screen = work_screen(anchor)
        if screen is not None:
            return screen.availableGeometry()
    screen = work_screen()
    if screen is not None:
        return screen.availableGeometry()
    primary = QGuiApplication.primaryScreen()
    if primary is not None:
        return primary.availableGeometry()
    return QRect(0, 0, 1280, 720)


def _anchor_rect(desk, kind: str) -> tuple[QRect | None, str]:
    """Return (target rect, place side). side: left_of | right_of | above | below | center."""
    if desk is None:
        return None, "center"

    if kind == "fence":
        for fence in list(getattr(desk, "fences", None) or []):
            try:
                if fence.isVisible():
                    return QRect(fence.frameGeometry()), "right_of"
            except RuntimeError:
                continue
        return None, "center"

    if kind == "rail":
        tip = getattr(desk, "page_indicator", None)
        if tip is not None:
            try:
                if tip.isVisible():
                    return QRect(tip.frameGeometry()), "left_of"
            except RuntimeError:
                pass
        return None, "center"

    if kind == "todo":
        panel = getattr(desk, "todo_panel", None)
        if panel is not None:
            try:
                if panel.isVisible():
                    return QRect(panel.frameGeometry()), "right_of"
            except RuntimeError:
                pass
        # Fall back near the page rail if todos are off.
        tip = getattr(desk, "page_indicator", None)
        if tip is not None:
            try:
                if tip.isVisible():
                    return QRect(tip.frameGeometry()), "left_of"
            except RuntimeError:
                pass
        return None, "center"

    if kind == "tray":
        geo = _work_geo()
        # Near the notification area (bottom-right on LTR Windows).
        tray = QRect(geo.right() - 120, geo.bottom() - 48, 100, 36)
        return tray, "above"

    return None, "center"


class DesktopGuideTip(QWidget):
    """Toast-like tip card on the desktop; Next / Skip through GUIDE_STEPS."""

    def __init__(
        self,
        settings: dict | None = None,
        *,
        desk_app=None,
        on_finished: Callable[[], None] | None = None,
        restore_window: QWidget | None = None,
    ) -> None:
        super().__init__(None)
        self._settings = settings if isinstance(settings, dict) else {}
        self._desk = desk_app if desk_app is not None else _resolve_desk_app()
        self._on_finished = on_finished
        self._restore_window = restore_window
        self._index = 0
        theme = normalize_theme(self._settings.get("theme"))
        self._palette = get_theme_palette(theme)

        self.setObjectName("desktopGuideTip")
        self.setWindowTitle(f"入门指引 — {APP_NAME_ZH}")
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint
            | Qt.WindowType.Tool
        )
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose, True)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, False)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)

        p = self._palette
        self.setStyleSheet(
            f"QWidget#desktopGuideCard {{"
            f" background: {p.get('card', '#FFFFFF')};"
            f" border: 1px solid {p.get('border', '#D5DCE7')};"
            f" border-left: 4px solid {p.get('accent', '#2563EB')};"
            f" border-radius: 16px;"
            f"}}"
            f"QLabel#guideKicker {{ color: {p.get('accent', '#2563EB')};"
            f" font-size: 11px; font-weight: 800; letter-spacing: 1px; }}"
            f"QLabel#guideTitle {{ color: {p.get('text', '#1F2937')};"
            f" font-size: 16px; font-weight: 800; }}"
            f"QLabel#guideBody {{ color: {p.get('text_muted', '#6B7280')};"
            f" font-size: 13px; }}"
            f"QLabel#guideProgress {{ color: {p.get('text_muted', '#6B7280')};"
            f" font-size: 12px; }}"
            f"QPushButton#guideSkipBtn, QPushButton#guideBackBtn {{"
            f" background: transparent; color: {p.get('text_muted', '#6B7280')};"
            f" border: none; padding: 6px 10px; font-size: 13px; }}"
            f"QPushButton#guideSkipBtn:hover, QPushButton#guideBackBtn:hover {{"
            f" color: {p.get('text', '#1F2937')}; }}"
            f"QPushButton#guideNextBtn {{"
            f" background: {p.get('accent', '#2563EB')}; color: #FFFFFF;"
            f" border: none; border-radius: 10px; padding: 8px 16px;"
            f" font-weight: 700; font-size: 13px; }}"
            f"QPushButton#guideNextBtn:hover {{"
            f" background: {p.get('nav_active', p.get('accent', '#1D4ED8'))}; }}"
        )

        card = QWidget(self)
        card.setObjectName("desktopGuideCard")
        card.setFixedWidth(360)
        layout = QVBoxLayout(card)
        layout.setContentsMargins(20, 16, 20, 14)
        layout.setSpacing(8)

        self._kicker = QLabel("第一次使用")
        self._kicker.setObjectName("guideKicker")
        layout.addWidget(self._kicker)

        self._title = QLabel()
        self._title.setObjectName("guideTitle")
        self._title.setWordWrap(True)
        layout.addWidget(self._title)

        self._body = QLabel()
        self._body.setObjectName("guideBody")
        self._body.setWordWrap(True)
        layout.addWidget(self._body)

        self._progress = QLabel()
        self._progress.setObjectName("guideProgress")
        layout.addWidget(self._progress)

        buttons = QHBoxLayout()
        buttons.setSpacing(6)
        self._skip = QPushButton("跳过")
        self._skip.setObjectName("guideSkipBtn")
        self._skip.setCursor(Qt.CursorShape.PointingHandCursor)
        self._skip.clicked.connect(self._finish)
        buttons.addWidget(self._skip)
        buttons.addStretch(1)
        self._back = QPushButton("上一步")
        self._back.setObjectName("guideBackBtn")
        self._back.setCursor(Qt.CursorShape.PointingHandCursor)
        self._back.clicked.connect(self._prev)
        buttons.addWidget(self._back)
        self._next = QPushButton("下一步")
        self._next.setObjectName("guideNextBtn")
        self._next.setCursor(Qt.CursorShape.PointingHandCursor)
        self._next.clicked.connect(self._advance)
        buttons.addWidget(self._next)
        layout.addLayout(buttons)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(card)

        self._render()

    def _render(self) -> None:
        title, body, _anchor = GUIDE_STEPS[self._index]
        self._title.setText(title)
        self._body.setText(body)
        self._progress.setText(f"{self._index + 1} / {len(GUIDE_STEPS)}")
        last = self._index >= len(GUIDE_STEPS) - 1
        self._next.setText("开始使用" if last else "下一步")
        self._back.setEnabled(self._index > 0)
        self.adjustSize()
        self._reposition()

    def _reposition(self) -> None:
        _title, _body, kind = GUIDE_STEPS[self._index]
        target, side = _anchor_rect(self._desk, kind)
        geo = _work_geo(target)
        self.adjustSize()
        w, h = self.width(), self.height()
        margin = 16

        if target is None or side == "center":
            x = geo.x() + (geo.width() - w) // 2
            y = geo.y() + max(64, geo.height() // 8)
        elif side == "left_of":
            x = target.left() - w - margin
            y = target.center().y() - h // 2
        elif side == "right_of":
            x = target.right() + margin
            y = target.center().y() - h // 2
        elif side == "above":
            x = target.center().x() - w // 2
            y = target.top() - h - margin
        else:  # below
            x = target.center().x() - w // 2
            y = target.bottom() + margin

        x = max(geo.left() + 8, min(x, geo.right() - w - 8))
        y = max(geo.top() + 8, min(y, geo.bottom() - h - 8))
        self.move(x, y)

    def _advance(self) -> None:
        if self._index >= len(GUIDE_STEPS) - 1:
            self._finish()
            return
        self._index += 1
        self._render()

    def _prev(self) -> None:
        if self._index <= 0:
            return
        self._index -= 1
        self._render()

    def _finish(self) -> None:
        global _active_tip
        cb = self._on_finished
        restore = self._restore_window
        _active_tip = None
        self.close()
        if restore is not None:
            try:
                restore.show()
                restore.raise_()
                restore.activateWindow()
            except RuntimeError:
                pass
        if callable(cb):
            cb()

    def closeEvent(self, event) -> None:  # noqa: N802
        global _active_tip
        if _active_tip is self:
            _active_tip = None
        super().closeEvent(event)

    def keyPressEvent(self, event) -> None:  # noqa: N802
        if event.key() == Qt.Key.Key_Escape:
            self._finish()
            return
        super().keyPressEvent(event)


# Back-compat name used by older selftests / imports.
FirstRunGuideDialog = DesktopGuideTip


def mark_first_run_guide_done(settings: dict | None) -> None:
    if not isinstance(settings, dict):
        return
    settings["first_run_guide_done"] = True
    try:
        save_settings(settings, immediate=True)
    except OSError:
        pass


def should_auto_show_first_run_guide(settings: dict | None) -> bool:
    if not isinstance(settings, dict):
        return False
    return not bool(settings.get("first_run_guide_done", True))


def show_first_run_guide(
    parent: QWidget | None = None,
    settings: dict | None = None,
    *,
    mark_done: bool = True,
    desk_app=None,
) -> None:
    """Show desktop tip tour (non-modal). Hides main settings briefly so the desk is visible."""
    global _active_tip
    del parent  # tips are top-level Tool windows

    desk = desk_app if desk_app is not None else _resolve_desk_app()
    cfg = settings if isinstance(settings, dict) else getattr(desk, "settings", None)
    if not isinstance(cfg, dict):
        cfg = {}

    if _active_tip is not None:
        try:
            _active_tip.close()
        except RuntimeError:
            pass
        _active_tip = None

    restore: QWidget | None = None
    main = getattr(desk, "window", None) if desk is not None else None
    if isinstance(main, QWidget):
        try:
            if main.isVisible() and not main.isMinimized():
                restore = main
                main.hide()
        except RuntimeError:
            restore = None

    begin = getattr(desk, "_begin_desktop_popup", None) if desk is not None else None
    end_later = getattr(desk, "_end_desktop_popup_later", None) if desk is not None else None
    if callable(begin):
        begin()

    finished_flag = {"done": False}

    def _on_finished() -> None:
        if finished_flag["done"]:
            return
        finished_flag["done"] = True
        if mark_done:
            mark_first_run_guide_done(cfg)
        if callable(end_later):
            end_later(400)

    tip = DesktopGuideTip(
        settings=cfg,
        desk_app=desk,
        on_finished=_on_finished,
        restore_window=restore,
    )
    _active_tip = tip
    tip.show()
    tip.raise_()
    tip.activateWindow()
    # Reposition after a tick — fences / rail may finish mapping.
    QTimer.singleShot(80, tip._reposition)
