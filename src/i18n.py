"""Chinese locale and Qt translation setup."""

from __future__ import annotations

from PyQt6.QtCore import QLibraryInfo, QLocale, Qt, QTranslator
from PyQt6.QtWidgets import QApplication, QMessageBox, QWidget

# 视图模式显示名（仅网格 / 列表）
VIEW_MODE_LABELS = {
    "grid": "网格",
    "list": "列表",
}

APP_NAME_ZH = "DeskTidy"

_WEEKDAYS_ZH = (
    "星期一",
    "星期二",
    "星期三",
    "星期四",
    "星期五",
    "星期六",
    "星期日",
)


def format_today_zh(*, include_year: bool = True) -> str:
    """Today's date in Chinese, e.g. ``2026年8月25日 · 星期二``."""
    from datetime import datetime

    now = datetime.now()
    weekday = _WEEKDAYS_ZH[now.weekday()]
    if include_year:
        return f"{now.year}年{now.month}月{now.day}日 · {weekday}"
    return f"{now.month}月{now.day}日 · {weekday}"


def setup_chinese(app: QApplication) -> None:
    """Install Qt Chinese translations and set default locale."""
    QLocale.setDefault(QLocale(QLocale.Language.Chinese, QLocale.Country.China))
    trans_path = QLibraryInfo.path(QLibraryInfo.LibraryPath.TranslationsPath)
    locale = QLocale(QLocale.Language.Chinese, QLocale.Country.China)

    for catalog in ("qtbase", "qt"):
        translator = QTranslator(app)
        if translator.load(locale, catalog, "_", trans_path):
            app.installTranslator(translator)


def normalize_view_mode(mode: object) -> str:
    """Return ``grid`` or ``list``. Legacy ``icon_only`` maps to grid."""
    value = str(mode or "grid").strip().lower()
    if value == "list":
        return "list"
    return "grid"


def view_mode_label(mode: str) -> str:
    return VIEW_MODE_LABELS.get(normalize_view_mode(mode), "网格")


def _prepare_message_box(box: QMessageBox) -> None:
    """Make dialogs visible above desktop overlays without a Z-order war."""
    box.setWindowModality(Qt.WindowModality.ApplicationModal)
    box.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
    # Do NOT call bring_widget_to_foreground here — its TOPMOST pulse fights
    # overlay keepalive and makes the confirm dialog flash endlessly.
    box.raise_()
    box.activateWindow()


def show_info(parent: QWidget | None, title: str, text: str) -> None:
    box = QMessageBox(parent)
    box.setIcon(QMessageBox.Icon.Information)
    box.setWindowTitle(title)
    box.setText(text)
    box.addButton("确定", QMessageBox.ButtonRole.AcceptRole)
    _prepare_message_box(box)
    box.exec()


def show_about(parent: QWidget | None = None) -> None:
    """Compact About dialog — version from a single source of truth."""
    from src._version import __version__

    text = (
        f"{APP_NAME_ZH}\n"
        f"版本 {__version__}\n\n"
        "桌面分区整理工具。整理只钉选显示，文件仍留在桌面。\n"
        "录屏需安装版内置 FFmpeg，或自行安装 FFmpeg。\n"
        "笔记与录屏默认保存在安装目录下的「笔记」「录屏」文件夹。"
    )
    show_info(parent, "关于", text)


def show_scrollable_info(parent: QWidget | None, title: str, text: str) -> None:
    """Info dialog with a scrollable body — for long organize previews."""
    from PyQt6.QtWidgets import QDialog, QDialogButtonBox, QPlainTextEdit, QVBoxLayout

    dlg = QDialog(parent)
    dlg.setWindowTitle(title)
    dlg.setModal(True)
    dlg.resize(520, 480)
    layout = QVBoxLayout(dlg)
    view = QPlainTextEdit()
    view.setReadOnly(True)
    view.setPlainText(text)
    layout.addWidget(view)
    buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok)
    buttons.button(QDialogButtonBox.StandardButton.Ok).setText("确定")
    buttons.accepted.connect(dlg.accept)
    layout.addWidget(buttons)
    dlg.exec()


def show_warning(parent: QWidget | None, title: str, text: str) -> None:
    box = QMessageBox(parent)
    box.setIcon(QMessageBox.Icon.Warning)
    box.setWindowTitle(title)
    box.setText(text)
    box.addButton("确定", QMessageBox.ButtonRole.AcceptRole)
    _prepare_message_box(box)
    box.exec()


def ask_yes_no(parent: QWidget | None, title: str, text: str) -> bool:
    box = QMessageBox(parent)
    box.setIcon(QMessageBox.Icon.Question)
    box.setWindowTitle(title)
    box.setText(text)
    yes_btn = box.addButton("是", QMessageBox.ButtonRole.YesRole)
    box.addButton("否", QMessageBox.ButtonRole.NoRole)
    box.setDefaultButton(yes_btn)
    _prepare_message_box(box)
    box.exec()
    return box.clickedButton() == yes_btn


def ask_keep_or_discard(parent: QWidget | None, title: str, text: str) -> bool:
    """Return True to keep the file, False only when user clicks 取消.

    Esc / title-bar close must **keep** the recording. Treating reject as discard
    deleted finished MP4s when users closed the dialog thinking it was a success
    notice (then reported “录制成功但文件没了”).
    """
    box = QMessageBox(parent)
    box.setIcon(QMessageBox.Icon.Question)
    box.setWindowTitle(title)
    box.setText(text)
    keep_btn = box.addButton("保留", QMessageBox.ButtonRole.AcceptRole)
    discard_btn = box.addButton("取消", QMessageBox.ButtonRole.RejectRole)
    box.setDefaultButton(keep_btn)
    box.setEscapeButton(keep_btn)
    _prepare_message_box(box)
    box.exec()
    clicked = box.clickedButton()
    if clicked is discard_btn:
        return False
    return True


def ask_keep_recording_with_name(
    parent: QWidget | None,
    *,
    path: "Path",
    size_mb: float,
) -> tuple[bool, str]:
    """Prompt to keep/discard a finished recording; return (keep, filename_stem).

    * Esc / title-bar close keeps the file (same as「保留」).
    * Only an explicit「取消」click discards.
    * The returned stem is sanitized; caller renames ``path`` when it differs.
    """
    from pathlib import Path

    from PyQt6.QtWidgets import (
        QDialog,
        QDialogButtonBox,
        QHBoxLayout,
        QLabel,
        QLineEdit,
        QVBoxLayout,
    )

    from src.screen_record_manager import sanitize_recording_stem

    saved = Path(path)
    dlg = QDialog(parent)
    dlg.setWindowTitle("录屏结束")
    dlg.setModal(True)
    dlg.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
    dlg.resize(480, 160)

    layout = QVBoxLayout(dlg)
    layout.addWidget(
        QLabel(
            f"是否保留这段录屏？（约 {size_mb:.1f} MB）\n"
            f"保存位置：{saved.parent}\n"
            "关闭窗口或按 Esc 也会保留文件。"
        )
    )
    name_row = QHBoxLayout()
    name_row.addWidget(QLabel("文件名"))
    name_edit = QLineEdit(saved.stem)
    name_edit.setClearButtonEnabled(True)
    name_edit.selectAll()
    name_row.addWidget(name_edit, stretch=1)
    ext = saved.suffix or ".mp4"
    name_row.addWidget(QLabel(ext))
    layout.addLayout(name_row)

    buttons = QDialogButtonBox()
    keep_btn = buttons.addButton("保留", QDialogButtonBox.ButtonRole.AcceptRole)
    discard_btn = buttons.addButton("取消", QDialogButtonBox.ButtonRole.RejectRole)
    layout.addWidget(buttons)

    explicit_discard = False

    def _on_discard() -> None:
        nonlocal explicit_discard
        explicit_discard = True
        dlg.done(0)

    def _on_keep() -> None:
        dlg.done(1)

    keep_btn.clicked.connect(_on_keep)
    discard_btn.clicked.connect(_on_discard)

    def _reject_to_keep() -> None:
        if explicit_discard:
            dlg.done(0)
        else:
            dlg.done(1)

    dlg.reject = _reject_to_keep  # type: ignore[method-assign]

    dlg.raise_()
    dlg.activateWindow()
    if dlg.exec() != 1:
        return False, ""
    return True, sanitize_recording_stem(name_edit.text(), fallback=saved.stem)
