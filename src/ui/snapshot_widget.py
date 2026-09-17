"""Snapshot management UI — icon grid with per-card actions."""

from __future__ import annotations


def _nudge_font(font, *, delta: int = -1, floor: int = 8):
    """Avoid QFont::setPointSize(-1) when the font is pixel-sized."""
    ps = int(font.pointSize() or 0)
    if ps > 0:
        font.setPointSize(max(floor, ps + delta))
        return font
    px = int(font.pixelSize() or 0)
    if px > 0:
        font.setPixelSize(max(floor, px + delta))
        return font
    font.setPointSize(max(floor, 9 + delta))
    return font

from pathlib import Path

from PyQt6.QtCore import QPoint, QRect, QSize, Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QColor, QFontMetrics, QIcon, QPainter, QPen, QPixmap
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QStyle,
    QStyledItemDelegate,
    QStyleOptionViewItem,
    QVBoxLayout,
    QWidget,
)

from src.i18n import ask_yes_no, show_info, show_warning
from src.layout_snapshot import (
    apply_layout_snapshot,
    capture_layout,
    default_snapshot_name,
    delete_snapshot,
    list_snapshots,
    save_snapshot,
)
from src.settings import save_settings
from src.snapshot_preview import (
    ensure_snapshot_preview,
    layout_pin_count,
    load_snapshot_preview,
    preview_cache_needs_refresh,
    preview_canvas_size,
    preview_path_for,
)
from src.ui.styles import get_theme_palette, normalize_theme

_THUMB_W = 220
_THUMB_H = 132
_ITEM_W = 248
# Thumb + full-width title row + action chips (chips no longer share the title lane).
_ITEM_H = 226
_ROLE_META = Qt.ItemDataRole.UserRole + 10
_ROLE_CREATED = Qt.ItemDataRole.UserRole + 11

_CARD_ACTIONS: tuple[tuple[str, str], ...] = (
    ("preview", "预览"),
    ("apply", "应用"),
    ("delete", "删除"),
)
_BTN_W = 48
_BTN_H = 24
_BTN_GAP = 5
_CARD_INSET = 2
_FOOTER_TITLE_H = 20
_FOOTER_TITLE_GAP = 4


class _SnapshotIconDelegate(QStyledItemDelegate):
    """Paint icon cards with a clear selected/hover chrome (IconMode QSS is weak)."""

    def paint(self, painter: QPainter, option: QStyleOptionViewItem, index) -> None:  # noqa: ANN001
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        hovered = bool(option.state & QStyle.StateFlag.State_MouseOver)
        widget = option.widget
        theme = "sky"
        if widget is not None:
            host = widget.window()
            settings = getattr(host, "settings", None)
            if isinstance(settings, dict):
                theme = normalize_theme(settings.get("theme"))
        palette = get_theme_palette(theme)
        accent = QColor(palette["accent"])
        soft = QColor(palette["accent_soft"])
        card = QColor(palette["card"])
        border = QColor(palette["border_strong"] if (selected or hovered) else palette["border"])
        text_c = QColor(palette["text"])

        rect = _card_content_rect(option.rect)
        if selected:
            fill = soft
            edge = accent
            left = accent
        elif hovered:
            fill = soft
            edge = border
            left = accent
        else:
            fill = card
            edge = border
            left = QColor(0, 0, 0, 0)

        painter.setPen(QPen(edge, 1.5 if selected else 1.0))
        painter.setBrush(fill)
        painter.drawRoundedRect(QRect(rect), 12, 12)
        if selected or hovered:
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(left)
            painter.drawRoundedRect(rect.left() + 1, rect.top() + 8, 3, rect.height() - 16, 1.5, 1.5)

        icon = index.data(Qt.ItemDataRole.DecorationRole)
        text = str(index.data(Qt.ItemDataRole.DisplayRole) or "")
        icon_rect = _thumb_rect(rect)
        if isinstance(icon, QIcon) and not icon.isNull():
            pix = icon.pixmap(QSize(_THUMB_W, _THUMB_H))
            if not pix.isNull():
                x = icon_rect.left() + (icon_rect.width() - pix.width()) // 2
                y = icon_rect.top() + (icon_rect.height() - pix.height()) // 2
                painter.drawPixmap(x, y, pix)
        meta_text = str(index.data(_ROLE_META) or "").strip()
        if meta_text:
            # Full-width strip in the card top margin — use the blank beside counts.
            badge_y = rect.top() + 3
            badge_h = min(18, max(14, icon_rect.top() - rect.top() - 2))
            badge_rect = QRect(
                icon_rect.left() + 6,
                badge_y,
                max(40, icon_rect.width() - 12),
                badge_h,
            )
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(15, 23, 42, 200))
            painter.drawRoundedRect(badge_rect, 8, 8)
            painter.setPen(QColor("#F8FAFC"))
            badge_font = painter.font()
            badge_font = _nudge_font(badge_font, delta=-1, floor=8)
            badge_font.setBold(True)
            painter.setFont(badge_font)
            pad = badge_rect.adjusted(10, 0, -10, 0)
            if " / " in meta_text:
                left_txt, right_txt = meta_text.split(" / ", 1)
                painter.drawText(
                    pad,
                    int(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft),
                    left_txt.strip(),
                )
                painter.drawText(
                    pad,
                    int(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight),
                    right_txt.strip(),
                )
            else:
                painter.drawText(
                    pad,
                    int(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignHCenter),
                    meta_text,
                )

        # Action chips on their own row under the title (full-width name above).
        actions = _card_action_rects(rect)
        btn_font = painter.font()
        btn_font = _nudge_font(btn_font, delta=-1, floor=8)
        btn_font.setBold(True)
        painter.setFont(btn_font)
        for key, label in _CARD_ACTIONS:
            btn_rect = actions[key]
            if key == "delete":
                btn_fill = QColor(254, 226, 226, 235)
                btn_edge = QColor(palette.get("danger", "#DC2626"))
                btn_text = QColor(palette.get("danger", "#B91C1C"))
            elif key == "apply":
                btn_fill = QColor(accent.red(), accent.green(), accent.blue(), 210)
                btn_edge = accent
                btn_text = QColor("#FFFFFF")
            else:
                btn_fill = QColor(248, 250, 252, 235)
                btn_edge = QColor(palette.get("border_strong", "#94A3B8"))
                btn_text = QColor(palette.get("accent_hover", accent))
            painter.setPen(QPen(btn_edge, 1.2))
            painter.setBrush(btn_fill)
            painter.drawRoundedRect(btn_rect, 7, 7)
            painter.setPen(btn_text)
            painter.drawText(btn_rect, int(Qt.AlignmentFlag.AlignCenter), label)

        footer_top = icon_rect.bottom() + 4
        # Full card width — buttons used to squeeze this to ~70px and elide
        # default names like「2026-09-17 14:57」into「2026-09-1...」.
        text_rect = QRect(
            rect.left() + 10,
            footer_top,
            max(40, rect.width() - 20),
            _FOOTER_TITLE_H,
        )
        painter.setPen(text_c)
        font = painter.font()
        font.setBold(selected)
        font = _nudge_font(font, delta=0, floor=9)
        painter.setFont(font)
        title = QFontMetrics(font).elidedText(
            text, Qt.TextElideMode.ElideRight, text_rect.width()
        )
        painter.drawText(
            text_rect,
            int(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter),
            title,
        )
        created_text = str(index.data(_ROLE_CREATED) or "").strip()
        if created_text and not _created_redundant_with_name(text, created_text):
            painter.setPen(QColor(palette.get("text_muted", "#64748B")))
            sub_font = painter.font()
            sub_font.setBold(False)
            sub_font = _nudge_font(sub_font, delta=-1, floor=8)
            painter.setFont(sub_font)
            # Sit on the button row left; chips stay right-aligned.
            sub_rect = QRect(
                text_rect.left(),
                actions["preview"].top(),
                max(40, actions["preview"].left() - text_rect.left() - 8),
                _BTN_H,
            )
            sub = QFontMetrics(sub_font).elidedText(
                created_text, Qt.TextElideMode.ElideRight, sub_rect.width()
            )
            painter.drawText(
                sub_rect,
                int(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter),
                sub,
            )
        painter.restore()

    def sizeHint(self, option: QStyleOptionViewItem, index) -> QSize:  # noqa: ANN001
        return QSize(_ITEM_W, _ITEM_H)


def _created_redundant_with_name(name: str, created: str) -> bool:
    """True when the footer title already carries the same day/minute stamp."""
    n = str(name or "").strip().replace("T", " ")
    c = str(created or "").strip().replace("T", " ")
    if not n or not c:
        return False
    if n == c:
        return True
    # 「2026-09-17 14:57」vs ISO「2026-09-17 14:57:03」/ truncated created.
    n16, c16 = n[:16], c[:16]
    return n.startswith(c16) or c.startswith(n16)


def _card_content_rect(item_rect: QRect) -> QRect:
    """Inset matching delegate paint — keep hit-testing aligned with chips."""
    return item_rect.adjusted(_CARD_INSET, _CARD_INSET, -_CARD_INSET, -_CARD_INSET)


def _thumb_rect(card_rect: QRect) -> QRect:
    return QRect(
        card_rect.left() + (card_rect.width() - _THUMB_W) // 2,
        card_rect.top() + 22,
        _THUMB_W,
        _THUMB_H,
    )


def _card_action_rects(rect: QRect) -> dict[str, QRect]:
    """Action chips on a row under the full-width title (not beside it)."""
    thumb = _thumb_rect(rect)
    total = len(_CARD_ACTIONS) * _BTN_W + (len(_CARD_ACTIONS) - 1) * _BTN_GAP
    x = rect.right() - total - 10
    y = thumb.bottom() + 4 + _FOOTER_TITLE_H + _FOOTER_TITLE_GAP
    # Keep chips inside the card if the item is unusually short.
    y = min(y, rect.bottom() - _BTN_H - 6)
    out: dict[str, QRect] = {}
    for i, (key, _) in enumerate(_CARD_ACTIONS):
        out[key] = QRect(x + i * (_BTN_W + _BTN_GAP), y, _BTN_W, _BTN_H)
    return out


def _preview_button_rect(rect: QRect) -> QRect:
    """Compat alias — preview chip only."""
    return _card_action_rects(rect)["preview"]


class _SnapshotPreviewPopup(QWidget):
    """Frameless click-open card showing a larger layout preview."""

    def __init__(self, parent: QWidget | None = None):
        super().__init__(
            parent,
            Qt.WindowType.Popup
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowStaysOnTopHint,
        )
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 8, 8, 8)
        layout.setSpacing(4)
        self.title = QLabel()
        self.title.setObjectName("appSubtitle")
        self.image = QLabel()
        self.image.setMinimumSize(480, 270)
        self.image.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.image.setStyleSheet(
            "QLabel { background: #1B1F24; border: 1px solid #3D4F66; border-radius: 8px; }"
        )
        self.hint = QLabel("单击空白处或按 Esc 关闭预览")
        self.hint.setObjectName("appHint")
        layout.addWidget(self.title)
        layout.addWidget(self.image)
        layout.addWidget(self.hint)
        self.setStyleSheet(
            "QWidget { background: #0D1117; border: 1px solid #30363D; border-radius: 10px; }"
        )

    def show_preview(self, title: str, pixmap) -> None:
        self.title.setText(title or "布局预览")
        if pixmap is None or pixmap.isNull():
            self.image.clear()
            self.image.setText("暂无预览")
            self.image.setFixedSize(480, 270)
        else:
            self.image.setText("")
            self.image.setFixedSize(pixmap.width(), pixmap.height())
            self.image.setPixmap(pixmap)
        self.adjustSize()
        from PyQt6.QtWidgets import QApplication

        screen = None
        app = QApplication.instance()
        if app is not None:
            win = app.activeWindow()
            if win is not None:
                try:
                    screen = win.screen()
                except RuntimeError:
                    screen = None
            if screen is None:
                screen = app.primaryScreen()
        if screen is not None:
            geo = screen.availableGeometry()
            x = geo.x() + max(0, (geo.width() - self.width()) // 2)
            y = geo.y() + max(0, (geo.height() - self.height()) // 2)
            self.move(x, y)
        self.show()
        self.raise_()


def _placeholder_thumb() -> QPixmap:
    pix = QPixmap(_THUMB_W, _THUMB_H)
    pix.fill(Qt.GlobalColor.transparent)
    from PyQt6.QtGui import QColor, QPainter, QPen

    painter = QPainter(pix)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setBrush(QColor("#E8EEF6"))
    painter.setPen(QPen(QColor("#C5D0DE"), 1))
    painter.drawRoundedRect(1, 1, _THUMB_W - 2, _THUMB_H - 2, 8, 8)
    painter.setPen(QColor("#8B97A8"))
    painter.drawText(pix.rect(), int(Qt.AlignmentFlag.AlignCenter), "暂无预览")
    painter.end()
    return pix


def _thumb_for(path: Path, snap: object) -> QPixmap:
    """Use cached PNG; refresh tiny pre-icon schematics from layout pins."""
    layout = getattr(snap, "layout", None)
    if isinstance(layout, dict) and preview_cache_needs_refresh(path, layout):
        try:
            ensure_snapshot_preview(path, layout)
        except Exception:
            pass
    png = preview_path_for(path)
    if png.is_file():
        pix = load_snapshot_preview(
            path,
            layout if isinstance(layout, dict) else None,
            width=_THUMB_W * 2,
            height=_THUMB_H * 2,
            prefer_cache=True,
        )
        if not pix.isNull():
            return pix.scaled(
                _THUMB_W,
                _THUMB_H,
                Qt.AspectRatioMode.KeepAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            )
    if isinstance(layout, dict) and layout:
        try:
            pix = load_snapshot_preview(
                path,
                layout,
                width=_THUMB_W * 2,
                height=_THUMB_H * 2,
                prefer_cache=False,
            )
            if not pix.isNull():
                return pix.scaled(
                    _THUMB_W,
                    _THUMB_H,
                    Qt.AspectRatioMode.KeepAspectRatio,
                    Qt.TransformationMode.SmoothTransformation,
                )
        except Exception:
            pass
    return _placeholder_thumb()


class SnapshotWidget(QWidget):
    snapshot_restored = pyqtSignal()

    def __init__(self, settings: dict | None = None, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings if isinstance(settings, dict) else {}
        self._snapshots: list[tuple[Path, object]] = []
        self._preview = _SnapshotPreviewPopup()
        self._build_ui()
        self.reload_table()

    def set_settings(self, settings: dict) -> None:
        self.settings = settings

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(14)

        hint = QLabel(
            "保存当前分区布局与样式（位置、大小、折叠、外观、分页、浮标等）。"
            "卡片右下角可预览、应用或删除；双击卡片也可应用该快照。"
        )
        hint.setObjectName("appSubtitle")
        hint.setWordWrap(True)
        layout.addWidget(hint)

        self.view = QListWidget()
        self.view.setObjectName("snapshotIconView")
        self.view.setViewMode(QListWidget.ViewMode.IconMode)
        self.view.setResizeMode(QListWidget.ResizeMode.Adjust)
        self.view.setMovement(QListWidget.Movement.Static)
        self.view.setWrapping(True)
        self.view.setUniformItemSizes(True)
        self.view.setSpacing(14)
        self.view.setIconSize(QSize(_THUMB_W, _THUMB_H))
        self.view.setGridSize(QSize(_ITEM_W, _ITEM_H))
        self.view.setWordWrap(True)
        self.view.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.view.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectItems)
        self.view.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.view.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.view.setMouseTracking(True)
        self.view.setItemDelegate(_SnapshotIconDelegate(self.view))
        self.view.itemDoubleClicked.connect(self._on_item_double_clicked)
        self.view.itemSelectionChanged.connect(self._on_selection_changed)
        self.view.viewport().installEventFilter(self)
        layout.addWidget(self.view, stretch=1)

        btn_row = QHBoxLayout()
        capture_btn = QPushButton("保存当前布局")
        capture_btn.setObjectName("primaryBtn")
        capture_btn.clicked.connect(self._capture)
        refresh_btn = QPushButton("刷新")
        refresh_btn.setObjectName("secondaryBtn")
        refresh_btn.clicked.connect(self.reload_table)
        btn_row.addWidget(capture_btn)
        btn_row.addWidget(refresh_btn)
        btn_row.addStretch()
        layout.addLayout(btn_row)

    def eventFilter(self, obj, event):  # noqa: ANN001
        from PyQt6.QtCore import QEvent

        if obj is self.view.viewport():
            # Press (not Release): IconMode often eats Release after selection.
            if (
                event.type() == QEvent.Type.MouseButtonPress
                and event.button() == Qt.MouseButton.LeftButton  # type: ignore[attr-defined]
            ):
                pos = event.position().toPoint()  # type: ignore[attr-defined]
                item = self.view.itemAt(pos)
                if item is not None:
                    # Same content rect the delegate paints into.
                    item_rect = _card_content_rect(self.view.visualItemRect(item))
                    action = _hit_card_action(item_rect, pos)
                    if action is not None:
                        row = _item_row(item)
                        if row is not None:
                            self.view.setCurrentItem(item)
                            self._run_card_action(action, row)
                            event.accept()
                            return True
        return super().eventFilter(obj, event)

    def hideEvent(self, event) -> None:  # noqa: ANN001
        self._hide_preview()
        super().hideEvent(event)

    def _hide_preview(self) -> None:
        self._preview.hide()

    def _on_selection_changed(self) -> None:
        # Force style refresh so the left inlay / border reads clearly.
        self.view.viewport().update()

    def _run_card_action(self, action: str, row: int) -> None:
        if action == "preview":
            self._show_click_preview(row)
            return
        if action == "apply":
            self._restore(row)
            return
        if action == "delete":
            self._delete(row)

    def _show_click_preview(self, row: int) -> None:
        if row is None or row < 0 or row >= len(self._snapshots):
            self._preview.hide()
            return
        path, snap = self._snapshots[row]
        layout = getattr(snap, "layout", None) or {}
        base_w, base_h = preview_canvas_size(max(1, len((layout.get("desktop_pages") or [])) if isinstance(layout, dict) else 1))
        pix = load_snapshot_preview(
            path,
            layout if isinstance(layout, dict) else None,
            width=max(760, base_w),
            height=max(420, base_h),
            # Enlarge wants a fresh hi-res render; thumbs use the PNG cache.
            prefer_cache=False,
        )
        title = f"{getattr(snap, 'name', path.stem)} · 放大预览"
        self._preview.show_preview(title, pix)

    def reload_table(self) -> None:
        """Reload the icon grid (name kept for callers / tests)."""
        self._hide_preview()
        snapshots = list_snapshots()
        self._snapshots = snapshots
        self.view.clear()
        for row, (path, snap) in enumerate(snapshots):
            thumb = _thumb_for(path, snap)
            name = str(getattr(snap, "name", path.stem) or path.stem)
            item = QListWidgetItem(QIcon(thumb), name)
            item.setData(Qt.ItemDataRole.UserRole, row)
            item.setSizeHint(QSize(_ITEM_W, _ITEM_H))
            item.setTextAlignment(Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop)
            fences = getattr(snap, "fence_count", 0)
            layout = getattr(snap, "layout", None)
            pins = layout_pin_count(layout if isinstance(layout, dict) else None)
            # Prefer pin count for the badge — shell icon_count is 0 when icons are hidden.
            icons = pins if pins > 0 else int(getattr(snap, "icon_count", 0) or 0)
            created = getattr(snap, "created_at", "") or ""
            meta = f"{fences} 分区 / {icons} 图标"
            item.setToolTip(
                f"{name}\n分区 {fences} · 钉选图标 {icons}\n{created}\n{path.name}"
            )
            item.setData(_ROLE_META, meta)
            item.setData(_ROLE_CREATED, str(created)[:16].replace("T", " "))
            self.view.addItem(item)

    def _selected_index(self) -> int | None:
        items = self.view.selectedItems()
        if not items:
            return None
        return _item_row(items[0])

    def _on_item_double_clicked(self, item: QListWidgetItem) -> None:
        row = _item_row(item)
        if row is not None:
            self.view.setCurrentItem(item)
            self._restore(row)

    def _capture(self) -> None:
        existing = [snap.name for _path, snap in list_snapshots()]
        suggested = default_snapshot_name(existing_names=existing)
        name, ok = QInputDialog.getText(
            self,
            "保存快照",
            "快照名称：",
            text=suggested,
        )
        if not ok:
            return
        cleaned = name.strip() or suggested
        try:
            snapshot = capture_layout(cleaned, settings=self.settings)
            if not snapshot.has_desktidy_layout and snapshot.icon_count == 0:
                show_warning(self, "保存失败", "当前没有可保存的分区布局或系统图标。")
                return
            # JSON first — preview PNG is deferred so the dialog returns quickly.
            path = save_snapshot(snapshot, write_preview=False)
            layout = snapshot.layout if snapshot.has_desktidy_layout else {}
            self.reload_table()
            show_info(
                self,
                "保存成功",
                f"已保存 {snapshot.fence_count} 个分区布局"
                f"（系统图标 {snapshot.icon_count} 个）。\n{path.name}",
            )
            if layout:
                QTimer.singleShot(
                    0, lambda p=path, lay=dict(layout): self._finish_snapshot_preview(p, lay)
                )
        except (RuntimeError, OSError, TypeError) as exc:
            show_warning(self, "保存失败", str(exc))

    def _finish_snapshot_preview(self, path: Path, layout: dict) -> None:
        try:
            ensure_snapshot_preview(path, layout)
        except Exception:
            return
        # Refresh thumbs once the PNG is ready (placeholder → real preview).
        if self.isVisible():
            self.reload_table()

    def _restore(self, *_args) -> None:
        self._hide_preview()
        idx = self._resolve_row(*_args)
        if idx is None:
            show_info(self, "提示", "请先选择一个快照。")
            return
        if idx >= len(self._snapshots):
            return
        _, snap = self._snapshots[idx]
        if not snap.has_desktidy_layout and not snap.icons:
            show_warning(self, "无法应用", "该快照没有分区布局或系统图标数据。")
            return
        detail = (
            f"分区布局 {snap.fence_count} 个"
            if snap.has_desktidy_layout
            else "仅系统图标位置"
        )
        if not ask_yes_no(
            self,
            "确认应用",
            f"将应用快照「{snap.name}」（{detail}），是否继续？",
        ):
            return
        try:
            summary = apply_layout_snapshot(self.settings, snap)
            try:
                save_settings(self.settings)
            except OSError:
                pass
            self.snapshot_restored.emit()
            show_info(
                self,
                "应用完成",
                f"已应用 {summary.get('fence_count', 0)} 个分区布局；"
                f"系统图标 {summary.get('shell_restored', 0)} 个"
                f"（未找到 {summary.get('shell_missing', 0)} 个）。",
            )
        except (RuntimeError, OSError, TypeError) as exc:
            show_warning(self, "应用失败", str(exc))

    def _delete(self, *_args) -> None:
        self._hide_preview()
        idx = self._resolve_row(*_args)
        if idx is None or idx >= len(self._snapshots):
            return
        path = self._snapshots[idx][0]
        if not ask_yes_no(self, "确认删除", f"确定删除快照 {path.name}？"):
            return
        delete_snapshot(path)
        self.reload_table()

    def _resolve_row(self, *_args) -> int | None:
        if _args and isinstance(_args[0], int):
            idx = int(_args[0])
            if 0 <= idx < self.view.count():
                self.view.setCurrentRow(idx)
                return idx
        return self._selected_index()


def _item_row(item: QListWidgetItem | None) -> int | None:
    """Read snapshot index from UserRole — must treat 0 as valid (not ``or -1``)."""
    if item is None:
        return None
    raw = item.data(Qt.ItemDataRole.UserRole)
    if raw is None:
        return None
    try:
        row = int(raw)
    except (TypeError, ValueError):
        return None
    return row if row >= 0 else None


def _hit_card_action(item_rect: QRect, pos: QPoint) -> str | None:
    for key, rect in _card_action_rects(item_rect).items():
        if rect.contains(pos):
            return key
    return None
