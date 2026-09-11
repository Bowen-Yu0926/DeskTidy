"""Two-column card UI for desktop pages and fences (snapshot-like)."""

from __future__ import annotations

from PyQt6.QtCore import QPoint, QRect, QSize, Qt, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QIcon, QPainter, QPen, QPixmap
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QCheckBox,
    QFrame,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMenu,
    QPushButton,
    QSizePolicy,
    QStyle,
    QStyledItemDelegate,
    QStyleOptionViewItem,
    QVBoxLayout,
    QWidget,
)


def _nudge_font(font: QFont, *, delta: int = -1, floor: int = 8) -> QFont:
    """Shrink/grow font without calling setPointSize(-1) on pixel-size fonts."""
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

from src.fence_layout import (
    DEFAULT_NEW_FENCE_HEIGHT,
    DEFAULT_NEW_FENCE_WIDTH,
    place_new_fence_config,
    save_fence_geometry,
)
from src.fence_pages import fence_on_page, get_fence_pages, set_fence_pages
from src.fence_rules import fence_rules_summary
from src.i18n import ask_yes_no, normalize_view_mode, show_warning, view_mode_label
from src.settings import save_settings
from src.system_defaults import (
    is_locked_fence,
    is_locked_page,
    is_locked_page_id,
)
from src.ui.fence_editor import FenceEditDialog
from src.ui.page_edit_dialog import PageEditDialog
from src.ui.section_card import SectionCard
from src.ui.styles import get_theme_palette, normalize_theme
from src.fence_style import (
    apply_preset_to_fence_config,
    match_fence_style_preset,
)
_ROLE_TYPE = int(Qt.ItemDataRole.UserRole)
_ROLE_ID = _ROLE_TYPE + 1
_ROLE_PAGE_ID = _ROLE_TYPE + 2
_ROLE_META = _ROLE_TYPE + 10
_ROLE_CREATED = _ROLE_TYPE + 11
_ROLE_OPACITY_PCT = _ROLE_TYPE + 20
_ROLE_SWATCH = _ROLE_TYPE + 21
_ROLE_ACCENT = _ROLE_TYPE + 22
_ROLE_PRESET = _ROLE_TYPE + 23
_ROLE_OPACITY = _ROLE_TYPE + 24
_ROLE_VISIBLE = _ROLE_TYPE + 25

# Left: Explorer-like page list rows. Right: zone icon cards with style chips.
_PAGE_ROW_H = 52
_PAGE_ADD_H = 40
_PAGE_LIST_ICON = 28
_FENCE_W, _FENCE_H = 196, 200
_THUMB_W, _THUMB_H = 168, 120
_CARD_INSET = 2
_ICON_BTN = 26
_BTN_W, _BTN_H, _BTN_GAP = _ICON_BTN, _ICON_BTN, 4
_CHIP = 16
_CHIP_GAP = 5
_CHIP_PAD = 4
_OPACITY_TRACK_H = 10
_OPACITY_BAND = 22
_FENCE_GRID_GAP = 10
_OPACITY_MIN = 0.35
_OPACITY_MAX = 1.0

_PAGE_ACTIONS: tuple[tuple[str, str], ...] = (
    ("edit", "编辑"),
    ("delete", "删除"),
)
_FENCE_ACTIONS: tuple[tuple[str, str], ...] = (
    ("edit", "编辑"),
    ("toggle", "显隐"),
    ("delete", "删除"),
)

_ACTION_ICON_KIND = {
    "edit": "edit",
    "delete": "trash",
    "toggle": "eye",
}


def _card_content_rect(item_rect: QRect) -> QRect:
    return item_rect.adjusted(_CARD_INSET, _CARD_INSET, -_CARD_INSET, -_CARD_INSET)


def _thumb_rect(card_rect: QRect, *, thumb_w: int, thumb_h: int) -> QRect:
    return QRect(
        card_rect.left() + (card_rect.width() - thumb_w) // 2,
        card_rect.top() + 22,
        thumb_w,
        thumb_h,
    )


def _fence_preview_rect(thumb: QRect) -> QRect:
    """Main style panel above preset chips + opacity slider."""
    chip_band = _CHIP + _CHIP_PAD * 2
    return thumb.adjusted(0, 0, 0, -(chip_band + _OPACITY_BAND))


def _fence_chip_rects(thumb: QRect) -> dict[str, QRect]:
    from src.fence_style import fence_style_preset_ids

    ids = fence_style_preset_ids()
    if not ids:
        return {}
    total = len(ids) * _CHIP + (len(ids) - 1) * _CHIP_GAP
    x = thumb.left() + max(4, (thumb.width() - total) // 2)
    y = thumb.bottom() - _OPACITY_BAND - _CHIP - _CHIP_PAD
    out: dict[str, QRect] = {}
    for i, preset_id in enumerate(ids):
        out[preset_id] = QRect(x + i * (_CHIP + _CHIP_GAP), y, _CHIP, _CHIP)
    return out


def _fence_opacity_track_rect(thumb: QRect) -> QRect:
    """Horizontal opacity track under style chips (leaves room for % label)."""
    label_w = 34
    y = thumb.bottom() - _OPACITY_BAND + (_OPACITY_BAND - _OPACITY_TRACK_H) // 2
    return QRect(
        thumb.left() + 8,
        y,
        max(40, thumb.width() - 16 - label_w),
        _OPACITY_TRACK_H,
    )


def _fence_opacity_label_rect(thumb: QRect) -> QRect:
    track = _fence_opacity_track_rect(thumb)
    return QRect(track.right() + 4, track.top() - 2, 32, track.height() + 4)


def _opacity_from_track_x(track: QRect, x: int) -> float:
    if track.width() <= 0:
        return _OPACITY_MIN
    t = (float(x) - float(track.left())) / float(track.width())
    t = max(0.0, min(1.0, t))
    return _OPACITY_MIN + t * (_OPACITY_MAX - _OPACITY_MIN)


def _paint_opacity_slider(
    painter: QPainter,
    thumb: QRect,
    opacity: float,
    *,
    accent: QColor,
    palette: dict,
) -> None:
    """Draw track + fill + knob + % under style chips."""
    opacity = max(_OPACITY_MIN, min(_OPACITY_MAX, float(opacity)))
    track = _fence_opacity_track_rect(thumb)
    label = _fence_opacity_label_rect(thumb)
    frac = (opacity - _OPACITY_MIN) / (_OPACITY_MAX - _OPACITY_MIN)
    fill_w = int(round(track.width() * frac))

    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(QColor(palette.get("border", "#C5D0DE")))
    painter.drawRoundedRect(track, 5, 5)
    if fill_w > 0:
        fill = QRect(track.left(), track.top(), fill_w, track.height())
        painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 200))
        painter.drawRoundedRect(fill, 5, 5)

    knob_x = track.left() + fill_w
    knob_r = 6
    knob = QRect(knob_x - knob_r, track.center().y() - knob_r, knob_r * 2, knob_r * 2)
    painter.setPen(QPen(QColor("#FFFFFF"), 1.5))
    painter.setBrush(accent)
    painter.drawEllipse(knob)

    pct = int(round(opacity * 100))
    painter.setPen(QColor(palette.get("text_muted", "#64748B")))
    font = painter.font()
    _nudge_font(font, delta=-1, floor=8)
    font.setBold(True)
    painter.setFont(font)
    painter.drawText(label, int(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft), f"{pct}%")


def _card_action_rects(
    rect: QRect, actions: tuple[tuple[str, str], ...], *, thumb_w: int, thumb_h: int
) -> dict[str, QRect]:
    thumb = _thumb_rect(rect, thumb_w=thumb_w, thumb_h=thumb_h)
    total = len(actions) * _BTN_W + (len(actions) - 1) * _BTN_GAP
    x = rect.right() - total - 10
    footer_top = thumb.bottom() + 4
    footer_h = max(_BTN_H, rect.bottom() - footer_top - 4)
    y = footer_top + max(0, (footer_h - _BTN_H) // 2)
    out: dict[str, QRect] = {}
    for i, (key, _) in enumerate(actions):
        out[key] = QRect(x + i * (_BTN_W + _BTN_GAP), y, _BTN_W, _BTN_H)
    return out


def _page_list_action_rects(rect: QRect) -> dict[str, QRect]:
    """Trailing edit/delete icon buttons on a page list row."""
    y = rect.top() + max(0, (rect.height() - _BTN_H) // 2)
    delete = QRect(rect.right() - _BTN_W - 8, y, _BTN_W, _BTN_H)
    edit = QRect(delete.left() - _BTN_W - _BTN_GAP, y, _BTN_W, _BTN_H)
    return {"edit": edit, "delete": delete}


def _paint_action_icon_btn(
    painter: QPainter,
    btn_rect: QRect,
    action_key: str,
    *,
    accent: QColor,
    palette: dict,
    visible: bool = True,
) -> None:
    """Draw a compact round icon chip (edit / eye / trash)."""
    from src.ui.action_icons import make_action_icon

    kind = _ACTION_ICON_KIND.get(action_key, "edit")
    if action_key == "toggle":
        kind = "eye" if visible else "eye_off"
    danger = action_key == "delete"
    if danger:
        btn_fill = QColor(254, 226, 226, 220)
        btn_edge = QColor(palette.get("danger", "#DC2626"))
    else:
        btn_fill = QColor(accent.red(), accent.green(), accent.blue(), 36)
        btn_edge = QColor(accent.red(), accent.green(), accent.blue(), 120)
    painter.setPen(QPen(btn_edge, 1.0))
    painter.setBrush(btn_fill)
    painter.drawRoundedRect(btn_rect, 7, 7)
    icon = make_action_icon(kind, danger=danger, size=16)
    pix = icon.pixmap(QSize(16, 16))
    if not pix.isNull():
        x = btn_rect.left() + (btn_rect.width() - pix.width()) // 2
        y = btn_rect.top() + (btn_rect.height() - pix.height()) // 2
        painter.drawPixmap(x, y, pix)


class _PageListDelegate(QStyledItemDelegate):
    """Left page rows painted as full-width buttons (no list chrome)."""

    def paint(self, painter: QPainter, option: QStyleOptionViewItem, index) -> None:  # noqa: ANN001
        # AIGC START
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        hovered = bool(option.state & QStyle.StateFlag.State_MouseOver)
        theme = "sky"
        widget = option.widget
        if widget is not None:
            host = widget.window()
            settings = getattr(host, "settings", None)
            if isinstance(settings, dict):
                theme = normalize_theme(settings.get("theme"))
        palette = get_theme_palette(theme)
        accent = QColor(palette["accent"])
        soft = QColor(palette["accent_soft"])
        text_c = QColor(palette["text"])
        muted = QColor(palette.get("text_muted", "#64748B"))
        border = QColor(palette.get("border", "#C5D0DE"))
        card = QColor(palette.get("card", "#FFFFFF"))

        if str(index.data(_ROLE_TYPE) or "") == "add_page":
            # Real QPushButton is mounted via setItemWidget — do not paint a
            # second 「+」 (that caused visual vs hit-target mismatch).
            painter.restore()
            return

        # Full-width button chrome (always visible).
        rect = option.rect.adjusted(0, 2, -6, -2)
        if selected:
            fill = soft
            edge = accent
            edge_w = 1.6
        elif hovered:
            fill = QColor(palette.get("surface", "#F1F5F9"))
            edge = accent
            edge_w = 1.2
        else:
            fill = card
            edge = border
            edge_w = 1.0
        painter.setPen(QPen(edge, edge_w))
        painter.setBrush(fill)
        painter.drawRoundedRect(rect, 10, 10)
        if selected:
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(accent)
            painter.drawRoundedRect(rect.left() + 3, rect.top() + 10, 3, rect.height() - 20, 1.5, 1.5)

        icon_side = _PAGE_LIST_ICON
        icon_rect = QRect(
            rect.left() + 14,
            rect.top() + (rect.height() - icon_side) // 2,
            icon_side,
            icon_side,
        )
        icon = index.data(Qt.ItemDataRole.DecorationRole)
        if isinstance(icon, QIcon) and not icon.isNull():
            pix = icon.pixmap(QSize(icon_side, icon_side))
            if not pix.isNull():
                painter.drawPixmap(icon_rect.topLeft(), pix)
        else:
            painter.setPen(QPen(border, 1))
            painter.setBrush(QColor(palette.get("surface", "#E8EEF6")))
            painter.drawRoundedRect(icon_rect, 6, 6)

        actions = _page_list_action_rects(rect)
        text_right = actions["edit"].left() - 10
        title = str(index.data(Qt.ItemDataRole.DisplayRole) or "")
        meta = str(index.data(_ROLE_META) or "").strip()
        title_rect = QRect(
            icon_rect.right() + 10,
            rect.top() + 6,
            max(40, text_right - (icon_rect.right() + 10)),
            20 if meta else rect.height() - 12,
        )
        font = painter.font()
        font.setBold(True if selected else False)
        _nudge_font(font, delta=0, floor=9)
        painter.setFont(font)
        painter.setPen(text_c)
        painter.drawText(
            title_rect,
            int(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter),
            title,
        )
        if meta:
            meta_rect = QRect(
                title_rect.left(),
                title_rect.bottom(),
                title_rect.width(),
                max(14, rect.bottom() - title_rect.bottom() - 4),
            )
            mf = painter.font()
            mf.setBold(False)
            _nudge_font(mf, delta=-1, floor=8)
            painter.setFont(mf)
            painter.setPen(muted)
            painter.drawText(
                meta_rect,
                int(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter),
                meta,
            )

        # Action icon chips stay visible so hit-testing matches paint.
        for key, _label in _PAGE_ACTIONS:
            _paint_action_icon_btn(
                painter,
                actions[key],
                key,
                accent=accent,
                palette=palette,
            )
        painter.restore()
        # AIGC END

    def sizeHint(self, option: QStyleOptionViewItem, index) -> QSize:  # noqa: ANN001
        w = option.rect.width() if option.rect.width() > 0 else 220
        if str(index.data(_ROLE_TYPE) or "") == "add_page":
            return QSize(w, _PAGE_ADD_H)
        return QSize(w, _PAGE_ROW_H)


class _LayoutCardDelegate(QStyledItemDelegate):
    """Paint snapshot-like cards for fences (right icon pane)."""

    def __init__(
        self,
        *,
        actions: tuple[tuple[str, str], ...],
        item_w: int,
        item_h: int,
        thumb_w: int,
        thumb_h: int,
        parent: QWidget | None = None,
    ):
        super().__init__(parent)
        self._actions = actions
        self._item_w = item_w
        self._item_h = item_h
        self._thumb_w = thumb_w
        self._thumb_h = thumb_h

    def paint(self, painter: QPainter, option: QStyleOptionViewItem, index) -> None:  # noqa: ANN001
        # AIGC START
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        hovered = bool(option.state & QStyle.StateFlag.State_MouseOver)
        theme = "sky"
        widget = option.widget
        if widget is not None:
            host = widget.window()
            settings = getattr(host, "settings", None)
            if isinstance(settings, dict):
                theme = normalize_theme(settings.get("theme"))
        palette = get_theme_palette(theme)
        accent_ui = QColor(palette["accent"])
        soft = QColor(palette["accent_soft"])
        card = QColor(palette["card"])
        border = QColor(palette["border_strong"] if (selected or hovered) else palette["border"])
        text_c = QColor(palette["text"])

        rect = _card_content_rect(option.rect)
        if str(index.data(_ROLE_TYPE) or "") == "add_fence":
            fill = soft if hovered else card
            pen = QPen(accent_ui if hovered else QColor(palette["border"]), 1.5)
            pen.setStyle(Qt.PenStyle.DashLine)
            painter.setPen(pen)
            painter.setBrush(fill)
            painter.drawRoundedRect(QRect(rect), 12, 12)
            painter.setPen(QPen(accent_ui, 2.4))
            cx, cy = rect.center().x(), rect.center().y() - 8
            arm = 14
            painter.drawLine(cx - arm, cy, cx + arm, cy)
            painter.drawLine(cx, cy - arm, cx, cy + arm)
            painter.setPen(QColor(palette.get("text_muted") or text_c))
            font = painter.font()
            _nudge_font(font, delta=0, floor=9)
            painter.setFont(font)
            label = QRect(rect.left() + 8, cy + arm + 10, rect.width() - 16, 22)
            painter.drawText(
                label,
                int(Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignVCenter),
                "新建分区",
            )
            painter.restore()
            return

        fill = soft if (selected or hovered) else card
        edge = accent_ui if selected else border
        painter.setPen(QPen(edge, 1.5 if selected else 1.0))
        painter.setBrush(fill)
        painter.drawRoundedRect(QRect(rect), 12, 12)
        if selected or hovered:
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(accent_ui)
            painter.drawRoundedRect(rect.left() + 1, rect.top() + 8, 3, rect.height() - 16, 1.5, 1.5)

        thumb = _thumb_rect(rect, thumb_w=self._thumb_w, thumb_h=self._thumb_h)
        preview = _fence_preview_rect(thumb)
        swatch = index.data(_ROLE_SWATCH)
        accent_hex = index.data(_ROLE_ACCENT)
        try:
            opacity = float(index.data(_ROLE_OPACITY) or 0.7)
        except (TypeError, ValueError):
            opacity = 0.7
        opacity = max(0.35, min(1.0, opacity))
        bg = QColor(swatch) if isinstance(swatch, str) and QColor(swatch).isValid() else QColor("#2A2E35")
        accent = (
            QColor(accent_hex)
            if isinstance(accent_hex, str) and QColor(accent_hex).isValid()
            else QColor("#4C8DFF")
        )
        # Mini fence panel (background + title bar + accent).
        panel = QColor(bg)
        panel.setAlpha(int(opacity * 255))
        painter.setPen(QPen(QColor(palette["border"]), 1))
        painter.setBrush(panel)
        painter.drawRoundedRect(preview, 8, 8)
        title_bar = QRect(preview.left(), preview.top(), preview.width(), 18)
        bar_fill = QColor(bg)
        bar_fill.setAlpha(min(255, int(opacity * 255) + 40))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(bar_fill)
        painter.drawRoundedRect(title_bar.adjusted(1, 1, -1, 0), 7, 7)
        painter.setBrush(accent)
        painter.drawRoundedRect(preview.left() + 6, preview.top() + 5, 3, 10, 1.5, 1.5)
        # Fake icon dots so the panel never reads as empty.
        painter.setBrush(QColor(255, 255, 255, 90 if panel.lightness() < 140 else 55))
        for i in range(3):
            painter.drawRoundedRect(
                preview.left() + 14 + i * 28,
                preview.top() + 28,
                22,
                22,
                5,
                5,
            )

        opacity_txt = str(index.data(_ROLE_OPACITY_PCT) or "").strip()
        if opacity_txt:
            badge = QRect(preview.right() - 52, preview.top() + 6, 46, 18)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(15, 23, 42, 200))
            painter.drawRoundedRect(badge, 8, 8)
            painter.setPen(QColor("#F8FAFC"))
            bf = painter.font()
            _nudge_font(bf, delta=-1, floor=8)
            bf.setBold(True)
            painter.setFont(bf)
            painter.drawText(badge, int(Qt.AlignmentFlag.AlignCenter), opacity_txt)

        meta_text = str(index.data(_ROLE_META) or "").strip()
        if meta_text:
            badge_y = rect.top() + 3
            badge_h = min(18, max(14, thumb.top() - rect.top() - 2))
            badge_rect = QRect(thumb.left() + 6, badge_y, max(40, thumb.width() - 12), badge_h)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(15, 23, 42, 200))
            painter.drawRoundedRect(badge_rect, 8, 8)
            painter.setPen(QColor("#F8FAFC"))
            bf = painter.font()
            _nudge_font(bf, delta=-1, floor=8)
            bf.setBold(True)
            painter.setFont(bf)
            painter.drawText(
                badge_rect.adjusted(8, 0, -8, 0),
                int(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft),
                meta_text,
            )

        # Per-fence style preset chips.
        from src.fence_style import FENCE_STYLE_PRESETS

        current_preset = str(index.data(_ROLE_PRESET) or "")
        for preset_id, chip in _fence_chip_rects(thumb).items():
            meta = FENCE_STYLE_PRESETS.get(preset_id) or {}
            chip_bg = QColor(str(meta.get("background") or "#2A2E35"))
            try:
                chip_op = float(meta.get("opacity") or 0.7)
            except (TypeError, ValueError):
                chip_op = 0.7
            chip_fill = QColor(chip_bg)
            chip_fill.setAlpha(int(max(0.45, min(1.0, chip_op)) * 255))
            painter.setPen(QPen(QColor(palette["border"]), 1))
            painter.setBrush(chip_fill)
            painter.drawRoundedRect(chip, 4, 4)
            if preset_id == current_preset:
                painter.setBrush(Qt.BrushStyle.NoBrush)
                painter.setPen(QPen(accent_ui, 2))
                painter.drawRoundedRect(chip.adjusted(-1, -1, 1, 1), 5, 5)

        # Opacity slider under style chips — live preview on the panel above.
        _paint_opacity_slider(
            painter,
            thumb,
            opacity,
            accent=accent_ui,
            palette=palette,
        )

        actions = _card_action_rects(
            rect, self._actions, thumb_w=self._thumb_w, thumb_h=self._thumb_h
        )
        visible = bool(index.data(_ROLE_VISIBLE) if index.data(_ROLE_VISIBLE) is not None else True)
        for key, _label in self._actions:
            _paint_action_icon_btn(
                painter,
                actions[key],
                key,
                accent=accent_ui,
                palette=palette,
                visible=visible,
            )

        text = str(index.data(Qt.ItemDataRole.DisplayRole) or "")
        footer_top = thumb.bottom() + 4
        text_right = next(iter(actions.values())).left() - 8 if actions else rect.right() - 10
        text_rect = QRect(
            rect.left() + 10,
            footer_top,
            max(40, text_right - (rect.left() + 10)),
            22,
        )
        painter.setPen(text_c)
        font = painter.font()
        font.setBold(selected)
        _nudge_font(font, delta=0, floor=9)
        painter.setFont(font)
        painter.drawText(
            text_rect,
            int(Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter),
            text,
        )
        painter.restore()
        # AIGC END

    def sizeHint(self, option: QStyleOptionViewItem, index) -> QSize:  # noqa: ANN001
        return QSize(self._item_w, self._item_h)


def _page_swatch_icon(page: dict) -> QIcon:
    side = _PAGE_LIST_ICON
    pix = QPixmap(side, side)
    pix.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pix)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setBrush(QColor("#E8EEF6"))
    painter.setPen(QPen(QColor("#C5D0DE"), 1))
    painter.drawRoundedRect(1, 1, side - 2, side - 2, 6, 6)
    painter.setPen(QColor("#334155"))
    name = str(page.get("name") or "分页")
    font = painter.font()
    _nudge_font(font, delta=-2, floor=7)
    font.setBold(True)
    painter.setFont(font)
    painter.drawText(pix.rect(), int(Qt.AlignmentFlag.AlignCenter), name[:1] or "页")
    painter.end()
    return QIcon(pix)


class DesktopLayoutWidget(QWidget):
    pages_changed = pyqtSignal()
    fences_changed = pyqtSignal()
    fence_visibility_changed = pyqtSignal(str, bool)
    fence_style_preview_requested = pyqtSignal(dict)
    fence_style_apply_requested = pyqtSignal(str)
    fence_style_revert_requested = pyqtSignal()
    fence_style_apply_one_requested = pyqtSignal(str, str)  # fence_id, preset_id
    fence_opacity_changed = pyqtSignal(str, float)  # fence_id, opacity 0.35–1.0

    def __init__(self, settings: dict, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings
        self.page_manager = self
        self.fence_editor = self
        self._selected_page: int | None = None
        self._opacity_drag_fence_id: str | None = None
        self._opacity_drag_item: QListWidgetItem | None = None
        self._build_ui()
        self._sync_page_selection()

    def _build_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(14)

        # AIGC START — style presets live on each fence card (no global picker)
        self._tree_card = SectionCard(
            "桌面分页与分区",
            "分页切换本软件分区（≠ Windows 虚拟桌面）。左列表选分页，右卡片点色块改该分区外观。",
        )
        # AIGC END
        self._indicator_cb = QCheckBox("显示页码指示器")
        self._indicator_cb.setChecked(self.settings.get("show_page_indicator", True))
        self._indicator_cb.stateChanged.connect(self._on_indicator_toggle)
        self._date_label = QLabel()
        self._date_label.setObjectName("desktopLayoutDate")
        self._date_label.setToolTip("今天")
        self._refresh_date_label()
        self._tree_card.add_header_widget(self._date_label)
        self._tree_card.add_header_widget(self._indicator_cb)

        columns = QHBoxLayout()
        columns.setContentsMargins(0, 0, 0, 0)
        columns.setSpacing(12)

        left = QVBoxLayout()
        left.setSpacing(6)
        left_header = QHBoxLayout()
        left_header.setContentsMargins(0, 0, 0, 0)
        left_header.setSpacing(6)
        left_title = QLabel("分页")
        left_title.setObjectName("appSubtitle")
        left_header.addWidget(left_title)
        left_header.addStretch(1)
        left.addLayout(left_header)
        self.page_view = QListWidget()
        self.page_view.setObjectName("desktopPageListView")
        self._configure_page_list_view(self.page_view)
        self.page_view.setItemDelegate(_PageListDelegate(parent=self.page_view))
        self.page_view.itemSelectionChanged.connect(self._on_page_view_selection)
        self.page_view.itemDoubleClicked.connect(self._on_page_double_clicked)
        self.page_view.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.page_view.customContextMenuRequested.connect(self._on_page_context_menu)
        self.page_view.viewport().installEventFilter(self)
        left.addWidget(self.page_view, stretch=1)
        # 「新建分页」 is a compact list row under the last page (not a footer that
        # sinks to the bottom of the stretch column).
        self._add_page_btn = None
        columns.addLayout(left, stretch=2)

        right = QVBoxLayout()
        right.setSpacing(6)
        right_header = QHBoxLayout()
        right_header.setContentsMargins(0, 0, 0, 0)
        right_header.setSpacing(6)
        self._fence_column_title = QLabel("分区")
        self._fence_column_title.setObjectName("appSubtitle")
        right_header.addWidget(self._fence_column_title)
        right_header.addStretch(1)
        # New fence: 「+」 card inside the icon grid (not header).
        self._add_fence_btn = None
        right.addLayout(right_header)
        self.fence_view = QListWidget()
        self.fence_view.setObjectName("desktopFenceIconView")
        self._configure_fence_icon_view(self.fence_view)
        self.fence_view.setItemDelegate(
            _LayoutCardDelegate(
                actions=_FENCE_ACTIONS,
                item_w=_FENCE_W,
                item_h=_FENCE_H,
                thumb_w=_THUMB_W,
                thumb_h=_THUMB_H,
                parent=self.fence_view,
            )
        )
        self.fence_view.itemDoubleClicked.connect(self._on_fence_double_clicked)
        self.fence_view.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.fence_view.customContextMenuRequested.connect(self._on_fence_context_menu)
        self.fence_view.viewport().installEventFilter(self)
        right.addWidget(self.fence_view, stretch=1)
        columns.addLayout(right, stretch=3)

        self._tree_card.add_body_layout(columns)
        layout.addWidget(self._tree_card, stretch=1)
        self.reload_table()

    def _make_plus_button(self, tip: str, handler) -> QPushButton:  # noqa: ANN001
        from src.ui.action_icons import make_action_icon

        btn = QPushButton()
        btn.setObjectName("desktopLayoutAddBtn")
        btn.setIcon(make_action_icon("plus", size=16))
        btn.setToolTip(tip)
        btn.setFixedSize(28, 28)
        btn.setCursor(Qt.CursorShape.PointingHandCursor)
        btn.clicked.connect(lambda _checked=False, fn=handler: fn())
        return btn

    def _configure_page_list_view(self, view: QListWidget) -> None:
        """Left pane: vertical page button list (no outer frame)."""
        # AIGC START
        view.setViewMode(QListWidget.ViewMode.ListMode)
        view.setFlow(QListWidget.Flow.TopToBottom)
        view.setWrapping(False)
        view.setMovement(QListWidget.Movement.Static)
        view.setResizeMode(QListWidget.ResizeMode.Adjust)
        # add_page row is shorter than page rows
        view.setUniformItemSizes(False)
        view.setSpacing(6)
        view.setFrameShape(QFrame.Shape.NoFrame)
        view.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        view.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        view.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        view.setMouseTracking(True)
        view.viewport().setMouseTracking(True)
        view.setMinimumHeight(200)
        view.setMinimumWidth(200)
        view.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        # AIGC END

    def _configure_fence_icon_view(self, view: QListWidget) -> None:
        """Right pane: zone cards in a left-to-right wrapping grid."""
        # AIGC START
        view.setViewMode(QListWidget.ViewMode.IconMode)
        view.setFlow(QListWidget.Flow.LeftToRight)
        view.setWrapping(True)
        view.setResizeMode(QListWidget.ResizeMode.Adjust)
        view.setMovement(QListWidget.Movement.Static)
        view.setSpacing(_FENCE_GRID_GAP)
        view.setUniformItemSizes(True)
        # Fixed grid cell — without this, IconMode often stretches to one column.
        view.setGridSize(
            QSize(_FENCE_W + _FENCE_GRID_GAP, _FENCE_H + _FENCE_GRID_GAP)
        )
        view.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        view.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        view.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        view.setMinimumHeight(200)
        view.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Expanding)
        view.setMouseTracking(True)
        view.viewport().setMouseTracking(True)
        # AIGC END

    def eventFilter(self, obj, event):  # noqa: ANN001
        from PyQt6.QtCore import QEvent

        if obj is self.fence_view.viewport():
            et = event.type()
            if et == QEvent.Type.MouseButtonRelease and event.button() == Qt.MouseButton.LeftButton:
                if self._opacity_drag_fence_id is not None:
                    self._finish_opacity_drag(persist=True)
                    return True
            if et == QEvent.Type.MouseMove and self._opacity_drag_fence_id is not None:
                self._update_opacity_drag(event.position().toPoint())
                return True
            if et == QEvent.Type.MouseButtonPress and event.button() == Qt.MouseButton.LeftButton:
                item = self.fence_view.itemAt(event.position().toPoint())
                if item is not None:
                    if str(item.data(_ROLE_TYPE) or "") == "add_fence":
                        self._add_fence()
                        return True
                    if self._begin_opacity_drag(item, event.position().toPoint()):
                        return True
                    if self._handle_card_click(
                        self.fence_view, item, event.position().toPoint(), kind="fence"
                    ):
                        return True
            return super().eventFilter(obj, event)

        if event.type() == QEvent.Type.MouseButtonPress and event.button() == Qt.MouseButton.LeftButton:
            if obj is self.page_view.viewport():
                item = self.page_view.itemAt(event.position().toPoint())
                if item is not None:
                    if str(item.data(_ROLE_TYPE) or "") == "add_page":
                        # Hit target is the mounted QPushButton only — ignore
                        # empty row chrome so paint/click stay aligned.
                        return True
                    if self._handle_card_click(
                        self.page_view, item, event.position().toPoint(), kind="page"
                    ):
                        return True
        return super().eventFilter(obj, event)

    def _begin_opacity_drag(self, item: QListWidgetItem, pos: QPoint) -> bool:
        if str(item.data(_ROLE_TYPE) or "") != "fence":
            return False
        card = _card_content_rect(self.fence_view.visualItemRect(item))
        thumb = _thumb_rect(card, thumb_w=_THUMB_W, thumb_h=_THUMB_H)
        track = _fence_opacity_track_rect(thumb)
        hit = track.adjusted(-4, -6, 4, 6)
        if not hit.contains(pos):
            return False
        fence_id = str(item.data(_ROLE_ID) or "")
        if not fence_id:
            return False
        self._opacity_drag_fence_id = fence_id
        self._opacity_drag_item = item
        self._apply_opacity_at(item, fence_id, pos, persist=False)
        return True

    def _update_opacity_drag(self, pos: QPoint) -> None:
        item = self._opacity_drag_item
        fence_id = self._opacity_drag_fence_id
        if item is None or not fence_id:
            return
        self._apply_opacity_at(item, fence_id, pos, persist=False)

    def _finish_opacity_drag(self, *, persist: bool) -> None:
        item = self._opacity_drag_item
        fence_id = self._opacity_drag_fence_id
        self._opacity_drag_item = None
        self._opacity_drag_fence_id = None
        if item is None or not fence_id:
            return
        if persist:
            save_settings(self.settings)
            # Re-emit final value so live fence stays in sync after save.
            try:
                opacity = float(item.data(_ROLE_OPACITY) or 0.88)
            except (TypeError, ValueError):
                opacity = 0.88
            self.fence_opacity_changed.emit(fence_id, opacity)

    def _apply_opacity_at(
        self,
        item: QListWidgetItem,
        fence_id: str,
        pos: QPoint,
        *,
        persist: bool,
    ) -> None:
        card = _card_content_rect(self.fence_view.visualItemRect(item))
        thumb = _thumb_rect(card, thumb_w=_THUMB_W, thumb_h=_THUMB_H)
        track = _fence_opacity_track_rect(thumb)
        opacity = _opacity_from_track_x(track, pos.x())
        fence = self._find_fence(fence_id)
        if fence is None:
            return
        style = fence.get("style") if isinstance(fence.get("style"), dict) else {}
        if not isinstance(fence.get("style"), dict):
            fence["style"] = style
        style = fence["style"]
        style["opacity"] = round(opacity, 3)
        item.setData(_ROLE_OPACITY, opacity)
        item.setData(_ROLE_OPACITY_PCT, f"{int(round(opacity * 100))}%")
        self.fence_view.viewport().update(self.fence_view.visualItemRect(item))
        self.fence_opacity_changed.emit(fence_id, opacity)
        if persist:
            save_settings(self.settings)

    def _handle_card_click(
        self, view: QListWidget, item: QListWidgetItem, pos: QPoint, *, kind: str
    ) -> bool:
        rect = view.visualItemRect(item)
        if kind == "page":
            # Match button chrome inset used by _PageListDelegate.paint.
            hits = _page_list_action_rects(rect.adjusted(0, 2, -6, -2))
            for key, btn in hits.items():
                if btn.contains(pos):
                    self._run_card_action(kind, key, item)
                    return True
            return False

        card = _card_content_rect(rect)
        thumb = _thumb_rect(card, thumb_w=_THUMB_W, thumb_h=_THUMB_H)
        for preset_id, chip in _fence_chip_rects(thumb).items():
            if chip.contains(pos):
                fence_id = str(item.data(_ROLE_ID) or "")
                self._apply_fence_style_preset(fence_id, preset_id)
                return True
        hits = _card_action_rects(
            card, _FENCE_ACTIONS, thumb_w=_THUMB_W, thumb_h=_THUMB_H
        )
        for key, btn in hits.items():
            if btn.contains(pos):
                self._run_card_action(kind, key, item)
                return True
        return False

    def _apply_fence_style_preset(self, fence_id: str, preset_id: str) -> None:
        """Apply a catalog preset to one fence only; refresh card + live desktop."""
        # AIGC START
        fence = self._find_fence(fence_id)
        if fence is None:
            return
        if apply_preset_to_fence_config(fence, preset_id):
            save_settings(self.settings)
        self.fence_style_apply_one_requested.emit(fence_id, preset_id)
        page_id = self._selected_page_id()
        if page_id is not None:
            keep = fence_id
            self._reload_fence_cards(page_id)
            self._select_fence_item(keep, page_id)
        # AIGC END

    def _run_card_action(self, kind: str, action: str, item: QListWidgetItem) -> None:
        if kind == "page":
            page_id = int(item.data(_ROLE_ID))
            if action == "edit":
                self._edit_page(page_id)
            elif action == "delete":
                self._delete_page(page_id)
            return
        fence_id = str(item.data(_ROLE_ID))
        fence = self._find_fence(fence_id)
        if fence is None:
            return
        if action == "edit":
            self._edit_fence(fence)
        elif action == "toggle":
            self._toggle_visible_by_id(fence_id)
        elif action == "delete":
            self._delete_fence(fence)

    def _refresh_date_label(self) -> None:
        from src.i18n import format_today_zh

        if hasattr(self, "_date_label"):
            self._date_label.setText(format_today_zh())

    def showEvent(self, event) -> None:  # noqa: N802
        self._refresh_date_label()
        super().showEvent(event)

    def _ensure_pages(self) -> list[dict]:
        pages = self.settings.get("desktop_pages")
        if not pages:
            pages = [{"id": 0, "name": "默认"}]
            self.settings["desktop_pages"] = pages
        return pages

    def _page_name(self, page_id: int) -> str:
        for page in self._ensure_pages():
            if page.get("id", 0) == page_id:
                return page.get("name", "默认")
        return "默认"

    def _next_page_id(self) -> int:
        pages = self._ensure_pages()
        return max((p.get("id", 0) for p in pages), default=-1) + 1

    def _find_fence(self, fence_id: str) -> dict | None:
        for fence in self.settings.get("fences", []):
            if fence.get("id") == fence_id:
                return fence
        return None

    def _selected_page_id(self) -> int | None:
        # Prefer the list's live selection — cached `_selected_page` alone made
        # itemSelectionChanged reload the *previous* page's fence cards.
        items = self.page_view.selectedItems()
        if items and str(items[0].data(_ROLE_TYPE) or "") == "page":
            try:
                return int(items[0].data(_ROLE_ID))
            except (TypeError, ValueError):
                pass
        if self._selected_page is not None:
            return int(self._selected_page)
        return None

    def _selected_fence(self) -> dict | None:
        items = self.fence_view.selectedItems()
        if not items:
            return None
        if str(items[0].data(_ROLE_TYPE) or "") == "add_fence":
            return None
        return self._find_fence(str(items[0].data(_ROLE_ID)))

    def _count_fences_on_page(self, page_id: int) -> int:
        return sum(1 for fence in self.settings.get("fences", []) if fence_on_page(fence, page_id))

    def _opacity_pct_text(self, fence: dict) -> str:
        style = fence.get("style") if isinstance(fence.get("style"), dict) else {}
        try:
            opacity = float(style.get("opacity", 0.88))
        except (TypeError, ValueError):
            opacity = 0.88
        pct = int(round(max(0.35, min(1.0, opacity)) * 100))
        return f"{pct}%"

    def _fence_opacity_value(self, fence: dict) -> float:
        style = fence.get("style") if isinstance(fence.get("style"), dict) else {}
        try:
            opacity = float(style.get("opacity", 0.88))
        except (TypeError, ValueError):
            opacity = 0.88
        return max(0.35, min(1.0, opacity))

    def reload_table(self) -> None:
        self._refresh_date_label()
        keep_page = self._selected_page_id()
        keep_fence = None
        sel_fence = self._selected_fence()
        if sel_fence is not None:
            keep_fence = str(sel_fence.get("id") or "")

        self.page_view.blockSignals(True)
        try:
            self.page_view.clear()
            for page in self._ensure_pages():
                page_id = int(page.get("id", 0))
                page_name = str(page.get("name") or "默认")
                fence_count = self._count_fences_on_page(page_id)
                meta = f"{fence_count} 个分区"
                item = QListWidgetItem(page_name)
                item.setData(_ROLE_TYPE, "page")
                item.setData(_ROLE_ID, page_id)
                item.setData(_ROLE_META, meta)
                item.setData(Qt.ItemDataRole.DecorationRole, _page_swatch_icon(page))
                item.setSizeHint(QSize(220, _PAGE_ROW_H))
                self.page_view.addItem(item)
            add_page = QListWidgetItem("")
            add_page.setData(_ROLE_TYPE, "add_page")
            add_page.setData(_ROLE_ID, "")
            add_page.setToolTip("新建分页")
            add_page.setFlags(Qt.ItemFlag.ItemIsEnabled)
            add_page.setSizeHint(QSize(40, _PAGE_ADD_H))
            self.page_view.addItem(add_page)
            self._mount_add_page_button(add_page)
        finally:
            self.page_view.blockSignals(False)

        if keep_page is not None and self.select_page_id(int(keep_page)):
            if keep_fence:
                self._select_fence_item(keep_fence, int(keep_page))
            return
        self._sync_page_selection()

    def _reload_fence_cards(self, page_id: int) -> None:
        self.fence_view.blockSignals(True)
        try:
            self.fence_view.clear()
            for fence in self.settings.get("fences", []):
                if not fence_on_page(fence, page_id):
                    continue
                name = str(fence.get("name") or "")
                style = fence.get("style") if isinstance(fence.get("style"), dict) else {}
                bg = str(style.get("background") or "#2A2E35")
                accent = str(style.get("accent") or "#4C8DFF")
                rules = (fence_rules_summary(fence) or "").strip()
                mode = view_mode_label(style.get("view_mode", "grid"))
                visible = bool(fence.get("visible", True))
                meta_parts = [p for p in (rules, mode, "隐藏" if not visible else "") if p]
                item = QListWidgetItem(name)
                item.setData(_ROLE_TYPE, "fence")
                item.setData(_ROLE_ID, fence.get("id", ""))
                item.setData(_ROLE_PAGE_ID, page_id)
                item.setData(_ROLE_META, " · ".join(meta_parts) if meta_parts else "—")
                item.setData(_ROLE_OPACITY_PCT, self._opacity_pct_text(fence))
                item.setData(_ROLE_OPACITY, self._fence_opacity_value(fence))
                item.setData(_ROLE_SWATCH, bg)
                item.setData(_ROLE_ACCENT, accent)
                item.setData(_ROLE_PRESET, match_fence_style_preset(fence))
                item.setData(_ROLE_VISIBLE, visible)
                item.setSizeHint(QSize(_FENCE_W, _FENCE_H))
                self.fence_view.addItem(item)
            add_item = QListWidgetItem("新建分区")
            add_item.setData(_ROLE_TYPE, "add_fence")
            add_item.setData(_ROLE_ID, "")
            add_item.setData(_ROLE_PAGE_ID, page_id)
            add_item.setToolTip("新建分区")
            add_item.setSizeHint(QSize(_FENCE_W, _FENCE_H))
            self.fence_view.addItem(add_item)
        finally:
            self.fence_view.blockSignals(False)
        # Re-apply icon grid after clear/add (keeps cards left-to-right).
        self.fence_view.setGridSize(
            QSize(_FENCE_W + _FENCE_GRID_GAP, _FENCE_H + _FENCE_GRID_GAP)
        )
        self.fence_view.doItemsLayout()
        page_name = self._page_name(page_id)
        self._fence_column_title.setText(f"分区 · {page_name}")
        self._tree_card.set_subtitle(
            f"分页「{page_name}」— 左选分页；右卡片点色块改该分区外观；右键可移动到其他分页"
        )

    def _sync_page_selection(self) -> None:
        self.select_page_id(int(self.settings.get("current_page", 0) or 0))

    def select_page_id(self, page_id: int) -> bool:
        for index in range(self.page_view.count()):
            item = self.page_view.item(index)
            if str(item.data(_ROLE_TYPE) or "") != "page":
                continue
            if int(item.data(_ROLE_ID)) == page_id:
                self.page_view.setCurrentItem(item)
                self._selected_page = page_id
                self._on_page_selected(page_id)
                return True
        for index in range(self.page_view.count()):
            item = self.page_view.item(index)
            if str(item.data(_ROLE_TYPE) or "") != "page":
                continue
            self.page_view.setCurrentItem(item)
            pid = int(item.data(_ROLE_ID))
            self._selected_page = pid
            self._on_page_selected(pid)
            return True
        return False

    def _select_fence_item(self, fence_id: str, page_id: int) -> bool:
        if not self.select_page_id(page_id):
            return False
        for index in range(self.fence_view.count()):
            item = self.fence_view.item(index)
            if str(item.data(_ROLE_TYPE) or "") == "add_fence":
                continue
            if str(item.data(_ROLE_ID)) == fence_id:
                self.fence_view.setCurrentItem(item)
                return True
        return False

    def _on_page_view_selection(self) -> None:
        items = self.page_view.selectedItems()
        if not items:
            return
        if str(items[0].data(_ROLE_TYPE) or "") != "page":
            return
        try:
            page_id = int(items[0].data(_ROLE_ID))
        except (TypeError, ValueError):
            return
        if self._selected_page is not None and int(self._selected_page) == page_id:
            # Same page re-selected (e.g. reload_table) — still refresh cards if empty.
            if self.fence_view.count() > 0:
                return
        self._selected_page = page_id
        self._on_page_selected(page_id)

    def _on_page_selected(self, page_id: int) -> None:
        """Update admin cards only — never switch the live desktop page."""
        self._reload_fence_cards(page_id)
        fence = self._selected_fence()
        if fence is not None:
            fence_name = str(fence.get("name") or "分区")
            page_name = self._page_name(page_id)
            self._tree_card.set_subtitle(
                f"「{fence_name}」· {page_name} — 双击编辑，右键切换网格/列表或移动分页"
            )

    def _on_page_double_clicked(self, item: QListWidgetItem) -> None:
        if str(item.data(_ROLE_TYPE) or "") == "add_page":
            # Single-click on the mounted 「+」 button adds; ignore row chrome.
            return
        self._edit_page(int(item.data(_ROLE_ID)))

    def _on_fence_double_clicked(self, item: QListWidgetItem) -> None:
        if str(item.data(_ROLE_TYPE) or "") == "add_fence":
            self._add_fence()
            return
        fence = self._find_fence(str(item.data(_ROLE_ID)))
        if fence is not None:
            self._edit_fence(fence)

    def _mount_add_page_button(self, item: QListWidgetItem) -> None:
        """Mount a real 「+」 QPushButton so hit-testing matches the control."""
        host = QWidget()
        host.setObjectName("desktopLayoutAddPageHost")
        host.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        lay = QHBoxLayout(host)
        # Match prior painted inset (left +2, vertically centered in add row).
        vpad = max(2, (_PAGE_ADD_H - 28) // 2)
        lay.setContentsMargins(2, vpad, 0, vpad)
        lay.setSpacing(0)
        btn = self._make_plus_button("新建分页", self._add_page)
        lay.addWidget(
            btn,
            0,
            Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
        )
        lay.addStretch(1)
        host.setFixedHeight(_PAGE_ADD_H)
        item.setSizeHint(QSize(max(40, host.sizeHint().width()), _PAGE_ADD_H))
        self.page_view.setItemWidget(item, host)

    def _on_page_context_menu(self, pos: QPoint) -> None:
        item = self.page_view.itemAt(pos)
        if item is None:
            return
        if str(item.data(_ROLE_TYPE) or "") == "add_page":
            return
        page_id = int(item.data(_ROLE_ID))
        self.page_view.setCurrentItem(item)
        menu = QMenu(self)
        add_act = menu.addAction("新增分区")
        edit_act = menu.addAction("编辑分页")
        delete_act = None
        if not is_locked_page_id(page_id):
            menu.addSeparator()
            delete_act = menu.addAction("删除分页")
        chosen = menu.exec(self.page_view.viewport().mapToGlobal(pos))
        if chosen == add_act:
            self._add_fence(page_id=page_id)
        elif chosen == edit_act:
            self._edit_page(page_id)
        elif delete_act is not None and chosen == delete_act:
            self._delete_page(page_id)

    def _on_fence_context_menu(self, pos: QPoint) -> None:
        item = self.fence_view.itemAt(pos)
        if item is None:
            return
        if str(item.data(_ROLE_TYPE) or "") == "add_fence":
            self._add_fence()
            return
        fence = self._find_fence(str(item.data(_ROLE_ID)))
        if fence is None:
            return
        self.fence_view.setCurrentItem(item)
        page_id = int(item.data(_ROLE_PAGE_ID))
        menu = QMenu(self)
        edit_act = menu.addAction("编辑分区")
        mode = normalize_view_mode((fence.get("style") or {}).get("view_mode"))
        toggle_label = "切换为列表" if mode != "list" else "切换为网格"
        view_act = menu.addAction(toggle_label)
        move_menu = menu.addMenu("移动到分页")
        move_actions: dict[object, int] = {}
        for page in self._ensure_pages():
            pid = int(page.get("id", 0))
            if pid == page_id:
                continue
            act = move_menu.addAction(str(page.get("name") or f"页{pid}"))
            move_actions[act] = pid
        if not move_actions:
            move_menu.setEnabled(False)
        delete_act = None
        if not is_locked_fence(fence):
            menu.addSeparator()
            delete_act = menu.addAction("删除分区")
        chosen = menu.exec(self.fence_view.viewport().mapToGlobal(pos))
        if chosen == edit_act:
            self._edit_fence(fence)
        elif chosen == view_act:
            self._toggle_fence_view_mode(str(fence.get("id") or ""))
        elif chosen in move_actions:
            self._on_fence_moved(str(fence.get("id") or ""), page_id, move_actions[chosen])
        elif delete_act is not None and chosen == delete_act:
            self._delete_fence(fence)

    # Compat alias used by older selftests / docs.
    def _on_tree_context_menu(self, pos: QPoint) -> None:
        self._on_fence_context_menu(pos)

    def _add_page(self) -> None:
        name, ok = QInputDialog.getText(self, "添加页面", "页面名称：")
        if not ok or not name.strip():
            return
        self._ensure_pages().append(
            {
                "id": self._next_page_id(),
                "name": name.strip(),
            }
        )
        save_settings(self.settings)
        self.reload_table()
        self.pages_changed.emit()

    def _edit_page(self, page_id: int) -> None:
        pages = self._ensure_pages()
        page = next((p for p in pages if p.get("id", 0) == page_id), None)
        if page is None:
            return
        dialog = PageEditDialog(page, self)
        if dialog.exec() != dialog.DialogCode.Accepted:
            return
        dialog.apply_to(page)
        save_settings(self.settings)
        self.reload_table()
        self.pages_changed.emit()

    def _rename_page(self, page_id: int) -> None:
        self._edit_page(page_id)

    def _delete_page(self, page_id: int) -> None:
        pages = self._ensure_pages()
        if len(pages) <= 1:
            show_warning(self, "提示", "至少保留一个页面。")
            return
        index = next((i for i, page in enumerate(pages) if page.get("id", 0) == page_id), None)
        if index is None:
            return
        page = pages[index]
        if is_locked_page(page) or is_locked_page_id(page_id):
            show_warning(self, "提示", "系统默认分页不能删除。")
            return
        fence_count = self._count_fences_on_page(page_id)
        msg = f"确定删除页面「{page.get('name', '')}」？"
        if fence_count:
            msg += f"\n该页面下有 {fence_count} 个分区，会移动到其他页面。"
        if not ask_yes_no(self, "确认删除", msg):
            return
        default_id = pages[0].get("id", 0) if index != 0 else pages[1].get("id", 0)
        for fence in self.settings.get("fences", []):
            page_ids = [value for value in get_fence_pages(fence) if value != page_id]
            if len(page_ids) != len(get_fence_pages(fence)):
                set_fence_pages(fence, page_ids or [default_id])
        pages.pop(index)
        if self.settings.get("current_page") == page_id:
            self.settings["current_page"] = default_id
        save_settings(self.settings)
        self.reload_table()
        self.pages_changed.emit()

    def _add_fence(self, page_id: int | None = None) -> None:
        pages = self._ensure_pages()
        dialog = FenceEditDialog(None, pages, self)
        target_page_id = page_id if page_id is not None else self._selected_page_id()
        if target_page_id is not None:
            for cb in dialog.page_checks:
                cb.setChecked(int(cb.property("page_id")) == target_page_id)
        if dialog.exec() != dialog.DialogCode.Accepted:
            return
        config = dialog.get_fence_config()
        fences = self.settings.setdefault("fences", [])
        page_ids = get_fence_pages(config)
        page_id = int(page_ids[0]) if page_ids else int(self._selected_page_id() or 0)
        place_new_fence_config(self.settings, config, page_id)
        fences.append(config)
        save_fence_geometry(
            self.settings,
            config,
            get_fence_pages(config)[0],
            {
                "x": config["x"],
                "y": config["y"],
                "width": config["width"],
                "height": config["height"],
            },
        )
        save_settings(self.settings)
        self.reload_table()
        self.fences_changed.emit()

    def _edit_fence(self, fence: dict) -> None:
        dialog = FenceEditDialog(fence, self._ensure_pages(), self)
        if dialog.exec() != dialog.DialogCode.Accepted:
            return
        updated = dialog.get_fence_config()
        updated["x"] = fence.get("x", 50)
        updated["y"] = fence.get("y", 50)
        updated["width"] = fence.get("width", DEFAULT_NEW_FENCE_WIDTH)
        updated["height"] = fence.get("height", DEFAULT_NEW_FENCE_HEIGHT)
        updated["visible"] = fence.get("visible", True)
        updated["collapsed"] = fence.get("collapsed", False)
        updated["id"] = fence.get("id", updated.get("id"))
        updated["virtual_items"] = list(fence.get("virtual_items") or [])
        if is_locked_fence(fence):
            from src.system_defaults import locked_fence_home_page

            updated["locked"] = True
            updated["id"] = fence.get("id")
            home = locked_fence_home_page(fence)
            if home is not None:
                set_fence_pages(updated, [home])
        fences = self.settings.get("fences", [])
        for index, current in enumerate(fences):
            if current.get("id") == fence.get("id"):
                fences[index] = updated
                break
        save_settings(self.settings)
        self.reload_table()
        self.fences_changed.emit()

    def _delete_fence(self, fence: dict) -> None:
        if is_locked_fence(fence):
            show_warning(self, "提示", "系统默认分区不能删除。")
            return
        if not ask_yes_no(self, "确认删除", f"确定删除分区「{fence.get('name', '')}」？"):
            return
        fences = self.settings.get("fences", [])
        for index, current in enumerate(fences):
            if current.get("id") == fence.get("id"):
                fences.pop(index)
                break
        save_settings(self.settings)
        self.reload_table()
        self.fences_changed.emit()

    def _delete_fence_by_id(self, fence_id: str) -> None:
        fence = self._find_fence(fence_id)
        if fence is not None:
            self._delete_fence(fence)

    def _toggle_visible_by_id(self, fence_id: str) -> None:
        fence = self._find_fence(fence_id)
        if fence is None:
            return
        new_visible = not bool(fence.get("visible", True))
        self.fence_visibility_changed.emit(fence_id, new_visible)
        self._refresh_fence_row(fence_id)

    def _refresh_fence_row(self, fence_id: str) -> None:
        page_id = self._selected_page_id()
        if page_id is None:
            return
        self._reload_fence_cards(page_id)
        self._select_fence_item(fence_id, page_id)

    def _toggle_fence_view_mode(self, fence_id: str) -> None:
        fence = self._find_fence(fence_id)
        if fence is None:
            return
        style = fence.get("style")
        if not isinstance(style, dict):
            style = {}
            fence["style"] = style
        current = normalize_view_mode(style.get("view_mode"))
        style["view_mode"] = "list" if current != "list" else "grid"
        save_settings(self.settings)
        self._refresh_fence_row(fence_id)
        self.fences_changed.emit()

    def _on_fence_moved(self, fence_id: str, source_page_id: int, target_page_id: int) -> None:
        fence = self._find_fence(fence_id)
        if fence is None:
            return
        from src.system_defaults import locked_fence_home_page

        home = locked_fence_home_page(fence)
        if home is not None and target_page_id != home:
            show_warning(
                self,
                "提示",
                f"「{fence.get('name') or '系统分区'}」为系统默认分区，不能移出对应分页。",
            )
            return
        page_ids = [value for value in get_fence_pages(fence) if value != source_page_id]
        if target_page_id not in page_ids:
            page_ids.append(target_page_id)
        if home is not None:
            page_ids = [home]
        set_fence_pages(fence, page_ids)
        save_settings(self.settings)
        self.reload_table()
        self._select_fence_item(fence_id, target_page_id)
        self.fences_changed.emit()

    def _on_indicator_toggle(self, state: int) -> None:
        self.settings["show_page_indicator"] = state == Qt.CheckState.Checked.value
        save_settings(self.settings)
        self.pages_changed.emit()

    def reload_all(self) -> None:
        self.reload_table()

    # Legacy helpers kept so older selftests that inspect names still resolve.
    def _make_page_card_widget(self, *args, **kwargs):  # noqa: ANN002, ANN003
        return QWidget()

    def _make_fence_card_widget(self, *args, **kwargs):  # noqa: ANN002, ANN003
        return QWidget()

    def _make_row_actions(self, *args, **kwargs):  # noqa: ANN002, ANN003
        return QWidget()
