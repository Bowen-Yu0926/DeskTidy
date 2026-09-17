"""Frameless desktop pet — autonomous AI + interactive actions."""

from __future__ import annotations

import math
import random
import time
from pathlib import Path

from PyQt6.QtCore import QPoint, QRect, Qt, QTimer, pyqtSignal
from PyQt6.QtGui import QColor, QCursor, QFont, QFontMetrics, QMouseEvent, QPainter, QPen, QPolygon, QRegion
from PyQt6.QtWidgets import QApplication, QWidget

from src.desktop_pet import (
    desktop_pet_hosts_float_bar,
    desktop_pet_settings,
    float_bar_tool_flags,
    normalize_pet_character,
    pet_action_enabled,
    pet_menu_action_options,
    pet_dizzy_sec,
    pet_happy_sec,
    pet_size_scale,
    pet_sleep_on_rest,
    random_pet_line,
)
from src.settings import save_settings
from src.ui.pet_anim import PetAnimState, load_pet_poses, paint_pet_sprite, trash_anim_sec
from src.ui.screen_snap import rect_visible_on_any_screen, snap_geometry, work_screen
from src.ui.styles import get_theme_palette, normalize_theme
from src.win_shell import (
    collect_drop_paths,
    configure_desktop_overlay,
    delete_to_trash,
    mime_has_droppable_items,
    preferred_drop_action_for_mime,
    raise_pet_above_fences_in_band,
)


def _restack_fences_after_pet_raise() -> None:
    """Keep fences clickable while the pet stays above fence icons."""
    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app is not None else None
    ensure = getattr(desk, "ensure_live_fences_interactive", None) if desk else None
    if callable(ensure):
        try:
            ensure()
            return
        except Exception:
            pass
    raise_pet_above_fences_in_band()

# Layout: bubble above sprite; status strip under sprite; optional action panel.
_SPRITE_H = 220
_SPRITE_W = 180
_STATUS_H = 0
_BUBBLE_PAD = 8
# Speech bubbles retired — ``say()`` is a no-op; keep slot constants at 0 so
# page chips can sit on the hood without a gutter for text.
_MIN_BUBBLE_H = 0
_IDLE_BUBBLE_H = 0
# Page chips hang this many px into the body slot (toward the hair).
_PAGE_CLOUD_OVERLAP = 16
_MENU_BTN = 22
_ACTION_BTN = 30
_ACTION_GAP = 4
_ACTION_PAD = 6
_PAGE_BUBBLE_H = 26
_PAGE_BUBBLE_GAP = 5
_PAGE_BUBBLE_ROW_GAP = 5
_PAGE_BUBBLE_PAD = 4
_PAGE_BUBBLE_MIN_W = 44
_PAGE_BUBBLE_MAX_ROW = 4
# Per-chip vertical scatter so the head cloud is not a flat strip.
_PAGE_BUBBLE_STAGGER = (-5, 3, -2, 4, -4, 2, -3, 5)
# Hold sprites include pinch fingers above the hood — borrow the page-chip band.
_HOLD_DRAG_HEADROOM_FRAC = 0.38
_DRAG_THRESHOLD = 6
_SNAP_THRESHOLD = 24
_TICK_MS = 33
_TICK_IDLE_MS = 100
_AI_MS = 700

# Behavior (market Shimeji-style: idle / wander / follow / sleep / play / climb / fall / perch)
_STATE_IDLE = "idle"
_STATE_WANDER = "wander"
_STATE_FOLLOW = "follow"
_STATE_SLEEP = "sleep"
_STATE_PLAY = "play"
_STATE_HAPPY = "happy"
_STATE_CLIMB = "climb"
_STATE_FALL = "fall"
_STATE_DIZZY = "dizzy"
_STATE_PERCH = "perch"
_STATE_WAIT = "wait"
_STATE_TRASH = "trash"
_IDLE_TICK_STATES = frozenset(
    # Keep wait/trash at full tick — page-flip and crumple cels must not stutter.
    {_STATE_IDLE, _STATE_SLEEP, _STATE_PERCH}
)

_HUNGER_DECAY_PER_MIN = 4.0
_MOOD_DECAY_PER_MIN = 2.0
_CURSOR_NOTICE_PX = 110
_PLAY_SELF_SEC = 7.0
_PLAY_CHASE_SEC = 6.0
_PLAY_MODE_SELF = "self"
_PLAY_MODE_CHASE = "chase"
_GRAVITY = 1600.0
_THROW_MIN = 180.0
_THROW_MAX = 980.0
_CLIMB_SPEED = 95.0
# Sit stunned after landing before returning to idle (brief, then slow stand-up fade).
_DIZZY_SEC = (1.0, 1.5)
# Crumple paper → toss in bin (duration from cel count × per-character pacing).
_TRASH_SEC = 1.8  # fallback when poses not loaded yet
_TRASH_RECYCLE_CEL = 5
# No user petting/feed/menu for this long → prefer sleep / climb / sit / play.
_LONELY_SOFT_SEC = 35.0
_LONELY_HARD_SEC = 90.0


class _PetFileDropZone(QWidget):
    """Sprite-only OLE drop target — parent pet must stay AcceptDrops off.

    Explorer folder→desktop / folder→fence must not register IDropTarget on the
    full pet HWND (page chips in the mask would steal OLE; ``ignore()`` does not
    fall through). This child covers only the character sprite; mouse events
    forward to the pet for grab/drag.
    """

    def __init__(self, pet: "DesktopPetWidget") -> None:
        super().__init__(pet)
        self._pet = pet
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAcceptDrops(True)
        self.setMouseTracking(True)
        self.setCursor(Qt.CursorShape.OpenHandCursor)

    def dragEnterEvent(self, event) -> None:  # noqa: N802
        if self._pet._accept_file_drop(event):
            event.accept()
            return
        event.ignore()

    def dragMoveEvent(self, event) -> None:  # noqa: N802
        if self._pet._accept_file_drop(event):
            event.accept()
            return
        event.ignore()

    def dropEvent(self, event) -> None:  # noqa: N802
        mime = event.mimeData()
        if not self._pet._accept_file_drop(event):
            event.ignore()
            return
        paths = collect_drop_paths(mime)
        if not paths:
            event.ignore()
            return
        recycled = self._pet._start_file_trash(paths)
        if recycled:
            self._pet._cleanup_pins_after_trash(recycled)
        action = preferred_drop_action_for_mime(mime)
        event.setDropAction(action)
        event.accept()

    def _forward_mouse(self, event: QMouseEvent, handler) -> None:
        from PyQt6.QtCore import QPointF

        local = self._pet.mapFromGlobal(event.globalPosition().toPoint())
        remapped = QMouseEvent(
            event.type(),
            QPointF(local),
            event.globalPosition(),
            event.button(),
            event.buttons(),
            event.modifiers(),
        )
        handler(remapped)

    def mousePressEvent(self, event) -> None:  # noqa: N802
        self._forward_mouse(event, self._pet.mousePressEvent)

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        self._forward_mouse(event, self._pet.mouseMoveEvent)

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        self._forward_mouse(event, self._pet.mouseReleaseEvent)

    def mouseDoubleClickEvent(self, event) -> None:  # noqa: N802
        self._forward_mouse(event, self._pet.mouseDoubleClickEvent)


class DesktopPetWidget(QWidget):
    """Interactive desktop companion with autonomous states."""

    page_changed = pyqtSignal(int)
    create_fence_requested = pyqtSignal(int)
    folder_open_requested = pyqtSignal(str)
    note_requested = pyqtSignal()
    note_folder_requested = pyqtSignal()
    record_requested = pyqtSignal()
    record_folder_requested = pyqtSignal()
    minutes_requested = pyqtSignal()
    minutes_folder_requested = pyqtSignal()
    calculator_requested = pyqtSignal()
    todo_requested = pyqtSignal()
    vault_requested = pyqtSignal()

    def __init__(self, settings: dict | None = None, parent: QWidget | None = None):
        super().__init__(parent)
        self.settings = settings if isinstance(settings, dict) else {}
        self._drag_offset: QPoint | None = None
        self._press_global: QPoint | None = None
        self._dragging = False
        self._drag_layout_extra = 0
        self._frame = 0
        self._facing = 1  # 1 right, -1 left
        self._character = normalize_pet_character(None)
        self._poses: dict = {}
        self._anim = PetAnimState()
        self._play_mode = _PLAY_MODE_SELF
        self._play_hop_x: int | None = None

        self._state = _STATE_IDLE
        self._state_until = 0.0
        self._post_trash_wait = False
        self._trash_erase_prev: QRect | None = None
        self._wander_target_x: int | None = None
        self._bubble_text = ""
        self._bubble_until = 0.0
        self._bubble_h = _IDLE_BUBBLE_H
        self._fx_hearts: list[tuple[float, float, float]] = []  # x,y,born
        self._fx_food: list[tuple[float, float, float]] = []
        self._hunger = 70.0
        self._mood = 80.0
        self._last_tick = time.monotonic()
        self._last_interact_at = time.monotonic()
        self._next_chatter = time.monotonic() + random.uniform(18, 40)
        self._cursor_was_near = False
        self._pointer_over = False
        self._menu_open = False
        self._menu_hover: str | None = None
        self._chrome_hover: int | None = None  # index into _chrome_items
        self._press_on_menu = False
        self._vx = 0.0
        self._vy = 0.0
        self._drag_samples: list[tuple[float, QPoint]] = []
        self._climb_dir = -1  # left screen edge
        self._climb_phase = "to_edge"  # to_edge | wriggle
        self._climb_edge_x: int | None = None
        self._climb_patrol_x: int | None = None
        self._perch_y: int | None = None
        self._rest_flip = 0  # auto: newspaper ↔ sleep
        self._chrome_items: list[dict] = []
        self._current_page_id = 0
        self._reload_chrome_from_settings()

        self.setObjectName("desktopPet")
        self.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.Window)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
        # Same as page bar: HWND_BOTTOM after DefView attach buries the pet under
        # the wallpaper WorkerW — patterned wallpaper then covers the sprite.
        self._desktidy_raise_band = True
        self.setCursor(Qt.CursorShape.OpenHandCursor)
        self.setMouseTracking(True)
        # Parent must NOT AcceptDrops: chip cloud mask would steal folder→desktop OLE.
        self.setAcceptDrops(False)
        self._trash_paths: list[Path] = []
        self._trash_recycled = False
        self._drop_zone = _PetFileDropZone(self)
        self._apply_size()

        self._tick = QTimer(self)
        self._tick.setInterval(_TICK_MS)
        self._tick.timeout.connect(self._on_tick)
        self._ai = QTimer(self)
        self._ai.setInterval(_AI_MS)
        self._ai.timeout.connect(self._on_ai)

        self.refresh_theme()
        self._reload_sprite(force=True)
        self._restore_or_default_position()
        self.say(random_pet_line(self.settings))
        self._schedule_idle()
        QTimer.singleShot(1200, self._warm_pose_cache)

    # ------------------------------------------------------------------ size / theme

    def _trash_dest_pad(self) -> tuple[int, int, int]:
        """(left, right, top) — feet at cel center; equal L/R keep body from jumping.

        Asymmetric right pad shifted ``dest.center`` (center-draw) so the boy
        slid right when the throw started.
        """
        w = max(1, self._body_width() - 8)
        h = self._sprite_h()
        # Sheet is already 960×752 with feet at mid-x — equal room both sides.
        side = max(100, w)
        top = max(80, h)
        return side, side, top

    def _trash_paint_slop(self) -> int:
        """Extra erase/repaint margin — throw cels extend past the dest slot.

        Keep tight: oversized Source-clear flashed wallpaper between hard cuts
        (felt like 闪烁 on layered HWNDs). Pad covers plane stretch only.
        """
        return max(16, self._sprite_dest_rect().width() // 10)

    def _trash_erase_rect(self) -> QRect:
        """Current throw dirty plate (sprite dest + modest slop + bubble)."""
        m = self._trash_paint_slop()
        r = self._sprite_dest_rect().adjusted(-m, -m, m, m)
        if self._bubble_text or self._bubble_h > _MIN_BUBBLE_H:
            ox, oy = self._body_origin()
            r = r.united(QRect(ox + 2, oy, self._body_width(), self._bubble_h + 8))
        return r

    def _trash_repaint_rect(self) -> QRect:
        """Dirty region during throw — previous∪current erase (kills plane ghosts)."""
        cur = self._trash_erase_rect()
        prev = self._trash_erase_prev
        return cur.united(prev) if prev is not None and not prev.isNull() else cur

    def _sprite_dest_horizontal_pad(self) -> tuple[int, int]:
        """Extra (left, right) pad for wide pose dest rects — mirrors _sprite_dest_rect."""
        w = max(1, self._body_width() - 8)
        if self._state == _STATE_TRASH:
            pad_l, pad_r, _pad_t = self._trash_dest_pad()
            return pad_l, pad_r
        if self._character == "hoodie" and self._state == _STATE_WAIT:
            p = max(22, w // 3)
            return p, p
        if self._character == "hoodie" and self._state == _STATE_SLEEP:
            p = max(16, w // 5)
            return p, p
        return 0, 0

    def _layout_body_width(self) -> int:
        pad_l, pad_r = self._sprite_dest_horizontal_pad()
        return self._body_width() + pad_l + pad_r

    def _body_pin_local(self) -> QPoint:
        """Body-center in widget coords — must match the layout currently on screen."""
        ox, oy = self._body_origin()
        return QPoint(
            ox + max(1, self._body_width()) // 2,
            oy + self._bubble_h + max(1, self._sprite_h()) // 2,
        )

    def _apply_size(self, *, pin_global: QPoint | None = None) -> None:
        # Keep the standing body fixed on screen when pads appear (trash/wait/etc.).
        # Growing only to the right made the pet look like it slid sideways.
        #
        # Critical: pin_global must be captured under the *pre-change* layout.
        # Capturing with new pads/width through the old widget geometry maps a
        # too-far-right point and jumps the boy (seen on idle/sleep → trash).
        if pin_global is None and self.isVisible() and self.width() > 1 and self.height() > 1:
            pin_global = self.mapToGlobal(self._body_pin_local())

        left_pad, band_h, total_w = self._chrome_layout_metrics()
        panel_w = self._action_panel_width() if self._menu_open else 0
        layout_w = self._layout_body_width()
        body_right = left_pad + layout_w
        widget_w = max(total_w, body_right + panel_w)
        drag_extra = max(0, int(getattr(self, "_drag_layout_extra", 0)))
        body_h = (
            band_h
            + drag_extra
            + self._sprite_dest_extra_top()
            + self._sprite_h()
            + _STATUS_H
            + self._bubble_h
            + 6
        )
        if self._menu_open:
            rows = (len(self._action_defs()) + 1) // 2
            panel_h = (
                band_h
                + drag_extra
                + self._bubble_h
                + _ACTION_PAD * 2
                + rows * (_ACTION_BTN + _ACTION_GAP)
            )
            body_h = max(body_h, panel_h + 8)
        self.setFixedSize(widget_w, body_h)
        if pin_global is not None:
            new_global = self.mapToGlobal(self._body_pin_local())
            dx = pin_global.x() - new_global.x()
            dy = pin_global.y() - new_global.y()
            if dx or dy:
                self.move(self.x() + dx, self.y() + dy)
        self._sync_drop_zone()
        self._sync_input_mask()

    def _schedule_public_host_mask_refresh(self) -> None:
        """Keep public-host pet exclusion hole aligned with this widget.

        PublicIconHost punches the pet's shaped mask out of its full-desktop
        plate. If the pet moves without refreshing that hole, the plate covers
        the new sprite and the second drag never receives mousePress.
        """
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        host = getattr(desk, "_public_icon_host", None) if desk is not None else None
        if host is None:
            return
        schedule = getattr(host, "schedule_mask_refresh", None)
        if callable(schedule):
            try:
                schedule()
            except RuntimeError:
                pass

    def _clear_drag_layout(self) -> None:
        """Drop temporary headroom added while dragging (idempotent)."""
        if int(getattr(self, "_drag_layout_extra", 0)) > 0:
            self._apply_drag_window_extra(0)

    def _sync_drop_zone(self) -> None:
        zone = getattr(self, "_drop_zone", None)
        if zone is None:
            return
        r = self._sprite_dest_rect()
        zone.setGeometry(r)
        zone.raise_()
        zone.setAcceptDrops(True)
        zone.show()

    def _sync_input_mask(self) -> None:
        """Shaped hit region: character + chrome only — not a full body plate.

        A rectangular mask over transparent padding steals Explorer OLE drops so
        「从文件夹拖到桌面」fails under the pet. Outside this region, WindowFromPoint
        reaches DefView (same idea as PublicIconHost click-through).
        """
        region = QRegion(self._sprite_dest_rect().adjusted(-2, -2, 2, 2))
        # Fall / dizzy cels can be wider than the idle dest; keep a small pad so
        # limbs are not clipped by the shaped mask.
        if self._state in (_STATE_FALL, _STATE_DIZZY, _STATE_TRASH, _STATE_WAIT, _STATE_SLEEP):
            pad = max(8, self._sprite_dest_rect().width() // 8)
            if self._state == _STATE_TRASH:
                # Throw cels + plane need generous mask so shaped HWND doesn't clip paint.
                pad = max(pad, self._sprite_dest_rect().width() // 2)
                region = region.united(
                    QRegion(
                        self._sprite_dest_rect().adjusted(
                            -pad, -pad, pad, max(24, pad // 4)
                        )
                    )
                )
                # Skip the generic pad union below — already covered.
                pad = 0
            elif self._character == "hoodie" and self._state in (_STATE_WAIT, _STATE_SLEEP):
                pad = max(pad, self._sprite_dest_rect().width() // 4)
            region = region.united(
                QRegion(self._sprite_dest_rect().adjusted(-pad, -pad // 2, pad, pad // 3))
            )
        if self._bubble_text:
            ox, oy = self._body_origin()
            region = region.united(
                QRegion(QRect(ox + 4, oy + 2, self._body_width() - 8, self._bubble_h - 4))
            )
        region = region.united(QRegion(self._menu_button_rect()))
        if self._show_page_bubbles():
            for i in range(len(self._chrome_items)):
                r = self._page_bubble_rect(i)
                if not r.isNull():
                    region = region.united(QRegion(r))
        if self._menu_open:
            panel = self._action_panel_rect()
            if not panel.isNull():
                region = region.united(QRegion(panel))
            for i in range(len(self._action_defs())):
                region = region.united(QRegion(self._action_button_rect(i)))
        # Never setMask(empty) on a live overlay HWND (can invalidate the window).
        if region.isEmpty():
            region = QRegion(0, 0, 1, 1)
        self.setMask(region)
        self._schedule_public_host_mask_refresh()

    def _action_panel_width(self) -> int:
        return _ACTION_PAD * 2 + _ACTION_BTN * 2 + _ACTION_GAP + 4

    def _size_scale(self) -> float:
        return pet_size_scale(self.settings)

    def _sprite_h(self) -> int:
        return max(96, int(round(_SPRITE_H * self._size_scale())))

    def _sprite_w(self) -> int:
        base = max(80, int(round(_SPRITE_W * self._size_scale())))
        # Fall/dizzy sheets are wider than idle (splayed limbs). Widen the dest
        # so height-matched content is not width-clamped / mask-clipped.
        if self._state in (_STATE_FALL, _STATE_DIZZY):
            # Widen dest so height-matched fall content is not width-clamped.
            return int(round(base * 1.55))
        if self._state == _STATE_TRASH:
            # Keep idle body width — flight room lives in pads, not a wider body
            # (widening body + dest.center foot-anchor made the whole pet jump right).
            return base
        if self._state == _STATE_WAIT and self._character == "hoodie":
            # Prone phone: raised feet extend right of the body.
            return int(round(base * 1.55))
        if self._state == _STATE_SLEEP and self._character == "hoodie":
            return int(round(base * 1.42))
        if self._state == _STATE_WAIT:
            return int(round(base * 1.20))
        return base

    def _body_width(self) -> int:
        return self._sprite_w()

    def _show_page_bubbles(self) -> bool:
        return desktop_pet_hosts_float_bar(self.settings) and bool(self._chrome_items)

    def _chrome_chip_width(self, label: str) -> int:
        font = QFont("Microsoft YaHei UI", 9)
        fm = QFontMetrics(font)
        return max(_PAGE_BUBBLE_MIN_W, fm.horizontalAdvance(str(label or "")) + 18)

    def _layout_page_bubbles(self) -> tuple[list[QRect], int, int, int]:
        """Head-cloud chrome: (rects, left_pad, band_h, total_w).

        Chips sit in staggered arc rows above the pet — not a right-side column.
        ``left_pad`` shifts the body so the cloud can overhang both sides.
        """
        if not self._show_page_bubbles():
            return [], 0, 0, self._layout_body_width()

        chips: list[tuple[int, int]] = []
        for i, item in enumerate(self._chrome_items):
            chips.append((i, self._chrome_chip_width(str(item.get("label") or ""))))

        rows: list[list[tuple[int, int]]] = []
        cur: list[tuple[int, int]] = []
        cur_w = 0
        row_budget = max(self._sprite_w() + 72, 220)
        for chip in chips:
            w = chip[1]
            need = w if not cur else cur_w + _PAGE_BUBBLE_GAP + w
            if cur and (len(cur) >= _PAGE_BUBBLE_MAX_ROW or need > row_budget):
                rows.append(cur)
                cur = []
                cur_w = 0
            cur.append(chip)
            cur_w = sum(c[1] for c in cur) + _PAGE_BUBBLE_GAP * (len(cur) - 1)
        if cur:
            rows.append(cur)

        body_cx = self._body_width() / 2.0
        rel: list[tuple[int, float, float, int]] = []
        y = float(_PAGE_BUBBLE_PAD + 4)
        for row_i, row in enumerate(rows):
            row_w = sum(c[1] for c in row) + _PAGE_BUBBLE_GAP * max(0, len(row) - 1)
            x = body_cx - row_w / 2.0
            mid = (len(row) - 1) / 2.0
            for j, (idx, w) in enumerate(row):
                arc = abs(j - mid) * 3.5
                stagger = _PAGE_BUBBLE_STAGGER[(idx + row_i) % len(_PAGE_BUBBLE_STAGGER)]
                rel.append((idx, x, y + stagger - arc, w))
                x += w + _PAGE_BUBBLE_GAP
            y += _PAGE_BUBBLE_H + _PAGE_BUBBLE_ROW_GAP + 3

        min_x = min(r[1] for r in rel)
        max_x = max(r[1] + r[3] for r in rel)
        left_pad = max(0, int(math.ceil(_PAGE_BUBBLE_PAD - min_x)))
        # Trash/wait sprite pads shift the body column — chips must follow or they
        # jump left on screen when pin keeps the boy fixed (_enter → trash).
        spr_pad_l, _spr_pad_r = self._sprite_dest_horizontal_pad()
        max_y = 0
        ordered: list[QRect | None] = [None] * len(chips)
        for idx, rx, ry, w in rel:
            top = max(2, int(round(ry)))
            rect = QRect(int(round(rx)) + left_pad + spr_pad_l, top, w, _PAGE_BUBBLE_H)
            ordered[idx] = rect
            max_y = max(max_y, rect.bottom())
        rects = [r for r in ordered if r is not None]
        # Hang the cloud toward the hair (no speech gutter — bubbles retired).
        band_h = max(0, max_y + 2 - _PAGE_CLOUD_OVERLAP)
        total_w = max(
            left_pad + self._layout_body_width(),
            int(math.ceil(max_x)) + left_pad + spr_pad_l + _PAGE_BUBBLE_PAD,
        )
        return rects, left_pad, band_h, total_w

    def _chrome_layout_metrics(self) -> tuple[int, int, int]:
        """Return (left_pad, band_h, total_w) for widget sizing."""
        _rects, left_pad, band_h, total_w = self._layout_page_bubbles()
        return left_pad, band_h, total_w

    def _body_origin(self) -> tuple[int, int]:
        """Top-left of the pet body (speech + sprite) inside the widget."""
        left_pad, band_h, _total = self._chrome_layout_metrics()
        spr_pad_l, _spr_pad_r = self._sprite_dest_horizontal_pad()
        return left_pad + spr_pad_l, band_h

    def _page_bubbles_width(self) -> int:
        """Legacy name: horizontal space beyond the body (right overhang only)."""
        left_pad, _band, total_w = self._chrome_layout_metrics()
        return max(0, total_w - left_pad - self._body_width())

    def _reload_chrome_from_settings(self) -> None:
        """Build float-bar chips: pages + folder shortcuts + enabled tools."""
        from src.page_folders import get_page_folder_items

        items: list[dict] = []
        raw = self.settings.get("desktop_pages") if isinstance(self.settings, dict) else None
        if isinstance(raw, list):
            for i, page in enumerate(raw):
                if not isinstance(page, dict):
                    continue
                try:
                    pid = int(page.get("id", i))
                except (TypeError, ValueError):
                    pid = i
                name = str(page.get("name") or f"页{pid + 1}")
                items.append({"kind": "page", "page_id": pid, "label": name})
        for folder in get_page_folder_items(self.settings):
            items.append(
                {
                    "kind": "folder",
                    "path": str(folder.get("path") or ""),
                    "label": str(folder.get("name") or "文件夹"),
                }
            )
        flags = float_bar_tool_flags(self.settings)
        tool_labels = (
            ("record", "录屏"),
            ("note", "记事本"),
            ("minutes", "纪要"),
            ("calculator", "计算器"),
            ("todo", "待办"),
            ("vault", "账号"),
        )
        for key, label in tool_labels:
            if flags.get(key):
                items.append({"kind": "tool", "tool": key, "label": label})
        self._chrome_items = items
        try:
            self._current_page_id = int(self.settings.get("current_page", 0))
        except (TypeError, ValueError):
            self._current_page_id = 0

    def reload_pages(self) -> None:
        """Refresh float-bar bubbles after settings / page / extension changes."""
        old_pos = self.pos()
        self._reload_chrome_from_settings()
        self._apply_size()
        self.move(old_pos)
        self.update()

    def set_current_page(self, page_id: int) -> None:
        try:
            page_id = int(page_id)
        except (TypeError, ValueError):
            return
        if page_id == self._current_page_id:
            self.update()
            return
        self._current_page_id = page_id
        self.update()

    def _page_bubble_rect(self, index: int) -> QRect:
        rects, _pad, _band, _total = self._layout_page_bubbles()
        if index < 0 or index >= len(rects):
            return QRect()
        return rects[index]

    def _hit_chrome_index(self, local: QPoint) -> int | None:
        if not self._show_page_bubbles():
            return None
        for i in range(len(self._chrome_items)):
            if self._page_bubble_rect(i).contains(local):
                return i
        return None

    def _activate_chrome_item(self, index: int, *, right: bool = False) -> None:
        if index < 0 or index >= len(self._chrome_items):
            return
        item = self._chrome_items[index]
        kind = str(item.get("kind") or "")
        if kind == "page":
            try:
                page_id = int(item.get("page_id", 0))
            except (TypeError, ValueError):
                return
            if right:
                self.create_fence_requested.emit(page_id)
                return
            if page_id != self._current_page_id:
                self._current_page_id = page_id
                self.update()
                self.page_changed.emit(page_id)
            return
        if kind == "folder":
            path = str(item.get("path") or "")
            if path:
                self.folder_open_requested.emit(path)
            return
        if kind != "tool":
            return
        tool = str(item.get("tool") or "")
        if right:
            if tool == "note":
                self.note_folder_requested.emit()
            elif tool == "record":
                self.record_folder_requested.emit()
            elif tool == "minutes":
                self.minutes_folder_requested.emit()
            return
        if tool == "record":
            self.record_requested.emit()
        elif tool == "note":
            self.note_requested.emit()
        elif tool == "minutes":
            self.minutes_requested.emit()
        elif tool == "calculator":
            self.calculator_requested.emit()
        elif tool == "todo":
            self.todo_requested.emit()
        elif tool == "vault":
            self.vault_requested.emit()

    def _action_panel_origin_x(self) -> int:
        """Panel hugs the pet body — not the page-chip cloud's right edge."""
        ox, _ = self._body_origin()
        return ox + self._body_width()

    def _action_panel_rect(self) -> QRect:
        if not self._menu_open:
            return QRect()
        defs = self._action_defs()
        rows = max(1, (len(defs) + 1) // 2)
        # Align with 「⋯」 on the sprite, not the old chip↔hair gutter.
        menu = self._menu_button_rect()
        return QRect(
            self._action_panel_origin_x(),
            menu.top(),
            self._action_panel_width(),
            rows * (_ACTION_BTN + _ACTION_GAP) + _ACTION_PAD,
        )

    def _menu_button_rect(self) -> QRect:
        """⋯ hugs the pet sprite (head/shoulder), not the page-cloud gutter.

        Anchoring to ``_body_origin`` + idle bubble left the chip floating in the
        empty band above wait/sleep poses; users asked it closer to the figure.
        """
        sprite = self._sprite_dest_rect()
        x = sprite.right() - _MENU_BTN - 2
        y = sprite.top() + max(8, sprite.height() // 5)
        return QRect(x, y, _MENU_BTN, _MENU_BTN)

    def _wait_action_tip(self, resting: bool = False) -> str:
        del resting  # kept for call-site compatibility
        if self._state == _STATE_WAIT:
            return "站起来"
        char = normalize_pet_character(self._character)
        if char == "voyage":
            return "坐下刷手机"
        if char == "hoodie":
            return "趴着玩手机"
        return "坐下等待"

    def _sleep_action_tip(self, resting: bool = False) -> str:
        del resting
        if self._state == _STATE_SLEEP:
            return "站起来"
        return "趴着睡觉"

    def _action_defs(self) -> list[tuple[str, str, str]]:
        """Return (id, icon_glyph, tooltip) for enabled interactive actions.

        While resting, only the *current* rest action shows 「起」; the other
        keeps 游/睡 so the panel never shows two identical wake buttons.
        """
        defs: list[tuple[str, str, str]] = []
        for aid, label in pet_menu_action_options(self._character):
            if not pet_action_enabled(self.settings, aid, character=self._character):
                continue
            if aid == "wait":
                if self._state == _STATE_WAIT:
                    glyph = "起"
                elif self._state == _STATE_SLEEP:
                    glyph = "游" if self._character == "hoodie" else "等"
                else:
                    glyph = "游" if self._character == "hoodie" else "等"
                defs.append((aid, glyph, self._wait_action_tip(False)))
            elif aid == "sleep":
                if self._state == _STATE_SLEEP:
                    glyph = "起"
                else:
                    glyph = "睡"
                defs.append((aid, glyph, self._sleep_action_tip(False)))
        defs.append(("hide", "隐", "暂时隐藏"))
        return defs

    def _action_button_rect(self, index: int) -> QRect:
        col = index % 2
        row = index // 2
        menu = self._menu_button_rect()
        x = self._action_panel_origin_x() + _ACTION_PAD + col * (_ACTION_BTN + _ACTION_GAP)
        y = menu.top() + _ACTION_PAD + row * (_ACTION_BTN + _ACTION_GAP)
        return QRect(x, y, _ACTION_BTN, _ACTION_BTN)

    def _hit_action_id(self, local: QPoint) -> str | None:
        if not self._menu_open:
            return None
        for i, (aid, _glyph, _tip) in enumerate(self._action_defs()):
            if self._action_button_rect(i).contains(local):
                return aid
        return None

    def _set_menu_open(self, open_: bool) -> None:
        open_ = bool(open_)
        if self._menu_open == open_:
            self.update()
            return
        # Keep pet body position stable when panel width changes
        old_pos = self.pos()
        self._menu_open = open_
        self._menu_hover = None
        self._apply_size()
        self.move(old_pos)
        self.update()

    def _toggle_action_menu(self) -> None:
        self._mark_user_interacted()
        self._set_menu_open(not self._menu_open)

    def _run_action(self, action_id: str) -> None:
        if not pet_action_enabled(self.settings, action_id, character=self._character):
            return
        self._mark_user_interacted()
        self._set_menu_open(False)
        if action_id == "pet":
            self._action_pet()
        elif action_id == "wait":
            self._action_wait_toggle()
        elif action_id == "sleep":
            self._action_sleep_toggle()
        elif action_id == "fall":
            self._action_fall()
        elif action_id == "hide":
            self.hide_pet()

    def refresh_theme(self) -> None:
        theme = normalize_theme(self.settings.get("theme") if self.settings else None)
        self._palette = get_theme_palette(theme)

    def _reload_sprite(self, *, force: bool = False) -> None:
        cfg = self._cfg()
        char_id = normalize_pet_character(str(cfg.get("character", "hoodie")))
        if not force and char_id == self._character and self._poses:
            return
        if char_id != self._character:
            self._anim = PetAnimState()
            self._state = _STATE_IDLE
            self._vx = 0.0
            self._vy = 0.0
        self._character = char_id
        self._poses = load_pet_poses(char_id)

    def _warm_pose_cache(self) -> None:
        """Preload other characters after startup so gallery switches stay instant.

        One character per event-loop turn — loading every PNG for voyage/
        hoodie synchronously used to freeze the UI for seconds after packaging
        (looked like「卡死」while shell IPC / hooks never got to start).
        """
        from src.desktop_pet import pet_character_ids

        self._warm_pose_queue = [
            cid for cid in pet_character_ids() if cid != self._character
        ]
        if self._warm_pose_queue:
            QTimer.singleShot(0, self._warm_pose_cache_tick)

    def _warm_pose_cache_tick(self) -> None:
        from src.ui.pet_anim import load_pet_poses

        queue = list(getattr(self, "_warm_pose_queue", None) or [])
        if not queue:
            return
        cid = queue.pop(0)
        self._warm_pose_queue = queue
        try:
            load_pet_poses(cid)
        except Exception:
            pass
        if queue:
            QTimer.singleShot(0, self._warm_pose_cache_tick)

    def _cfg(self) -> dict:
        return desktop_pet_settings(self.settings)

    # ------------------------------------------------------------------ speech / FX

    def say(self, text: str, *, msec: int = 3200) -> None:
        """Speech bubbles removed — keep call sites, draw nothing."""
        del text, msec
        if self._bubble_text or self._bubble_h:
            self._bubble_text = ""
            self._bubble_until = 0.0
            self._bubble_h = _IDLE_BUBBLE_H
            self._apply_size()
            self.update()

    def _recompute_bubble_height(self) -> None:
        """No-op: speech UI retired; keep height at the idle (zero) slot."""
        if self._state == _STATE_TRASH and self._bubble_h:
            return
        if self._bubble_h != _IDLE_BUBBLE_H or self._bubble_text:
            self._bubble_text = ""
            self._bubble_h = _IDLE_BUBBLE_H
            self._apply_size()

    def _spawn_hearts(self, n: int = 4) -> None:
        now = time.monotonic()
        for _ in range(n):
            self._fx_hearts.append(
                (random.uniform(0.25, 0.75), random.uniform(0.35, 0.55), now)
            )

    def _spawn_food(self) -> None:
        now = time.monotonic()
        self._fx_food.append((0.5, 0.42, now))

    # ------------------------------------------------------------------ lifecycle

    def reload_settings(self, settings: dict | None) -> None:
        if isinstance(settings, dict):
            self.settings = settings
        self.refresh_theme()
        prev_char = self._character
        self._reload_sprite(force=False)
        char_changed = self._character != prev_char
        if char_changed:
            self._schedule_idle()
        self.reload_pages()
        cfg = self._cfg()
        if cfg.get("follow_cursor", False):
            self._enter(_STATE_FOLLOW, duration=9999)
        elif self._state == _STATE_FOLLOW:
            self._schedule_idle()
        old_pos = self.pos()
        self._apply_size()
        self.move(old_pos)
        self.update()

    def _refresh_edge_page_bar(self) -> None:
        """Rebuild or tear down the right-edge float bar when pet chrome ownership changes."""
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        setup = getattr(desk, "_setup_page_indicator", None) if desk is not None else None
        if callable(setup):
            try:
                setup()
            except RuntimeError:
                pass

    def hide_pet(self) -> None:
        cfg = self._cfg()
        cfg["visible"] = False
        try:
            save_settings(self.settings)
        except OSError:
            pass
        self._tick.stop()
        self._ai.stop()
        self.hide()
        # Pages return to the right-edge bar while the pet is hidden.
        self._refresh_edge_page_bar()

    def show_pet(self) -> None:
        cfg = self._cfg()
        cfg["visible"] = True
        try:
            save_settings(self.settings)
        except OSError:
            pass
        self.reload_pages()
        self.show()
        self.raise_()
        configure_desktop_overlay(self, peek=False)
        # Overlay attach can recreate the HWND — never re-register OLE on pet.
        self.setAcceptDrops(False)
        self._sync_drop_zone()
        # Pet must paint above fence icons; shaped mask keeps OLE outside sprite.
        _restack_fences_after_pet_raise()
        if not self._tick.isActive():
            self._tick.start()
        if not self._ai.isActive():
            self._ai.start()
        self.say(random_pet_line(self.settings, action="wake"), msec=2200)
        self._refresh_edge_page_bar()

    def showEvent(self, event) -> None:  # noqa: N802
        super().showEvent(event)
        configure_desktop_overlay(self, peek=False)
        self.setAcceptDrops(False)
        self._sync_drop_zone()
        _restack_fences_after_pet_raise()
        if not self._tick.isActive():
            self._tick.start()
        if not self._ai.isActive():
            self._ai.start()

    def hideEvent(self, event) -> None:  # noqa: N802
        self._tick.stop()
        self._ai.stop()
        super().hideEvent(event)

    # ------------------------------------------------------------------ geometry

    def _work_geo(self) -> QRect:
        screen = work_screen()
        if not screen:
            return QRect(0, 0, 1280, 720)
        return screen.availableGeometry()

    def _floor_y(self) -> int:
        geo = self._work_geo()
        return geo.y() + geo.height() - self.height() - 48

    def _default_position(self) -> QPoint:
        """First-run / invalid saved pos: bottom-right of the work area."""
        geo = self._work_geo()
        margin = 48
        x = geo.x() + max(margin, geo.width() - self.width() - margin)
        return QPoint(x, self._floor_y())

    def _restore_or_default_position(self) -> None:
        cfg = self._cfg()
        saved = cfg.get("pos")
        if isinstance(saved, dict) and "x" in saved and "y" in saved:
            geo = QRect(int(saved["x"]), int(saved["y"]), self.width(), self.height())
            if rect_visible_on_any_screen(geo):
                snapped = snap_geometry(geo, _SNAP_THRESHOLD)
                self.move(snapped.topLeft())
                return
        self.move(self._default_position())
        self._persist_geometry()

    def _persist_geometry(self) -> None:
        cfg = self._cfg()
        pos = self.pos()
        cfg["pos"] = {"x": int(pos.x()), "y": int(pos.y())}
        try:
            save_settings(self.settings)
        except OSError:
            pass

    def _move_clamped(self, x: int, y: int) -> None:
        geo = self._work_geo()
        min_x = geo.x() + 4
        max_x = geo.x() + geo.width() - self.width() - 4
        min_y = geo.y() + 4
        max_y = geo.y() + geo.height() - self.height() - 4
        if max_x < min_x:
            x = geo.x() + max(4, (geo.width() - self.width()) // 2)
        else:
            x = max(min_x, min(x, max_x))
        if max_y < min_y:
            y = geo.y() + max(4, (geo.height() - self.height()) // 2)
        else:
            y = max(min_y, min(y, max_y))
        self.move(x, y)
        # Moving leaves PublicIconHost's pet exclusion at the old screen rect.
        self._schedule_public_host_mask_refresh()

    # ------------------------------------------------------------------ AI states

    def _enter(self, state: str, *, duration: float) -> None:
        # Capture body pin *before* state flips pads/width (see _apply_size).
        pin_global = None
        if self.isVisible() and self.width() > 1 and self.height() > 1:
            pin_global = self.mapToGlobal(self._body_pin_local())
        self._state = state
        self._state_until = time.monotonic() + max(0.3, duration)
        if state != _STATE_WANDER:
            self._wander_target_x = None
        if state != _STATE_PLAY:
            self._play_hop_x = None
        if state != _STATE_PERCH:
            self._perch_y = None
        from src.ui.pet_anim import pose_for_state

        self._anim.set_pose(pose_for_state(state, character=self._character))
        if state == _STATE_TRASH:
            self._anim.frame = 0
            self._trash_erase_prev = None
        elif not self._bubble_text:
            self._bubble_h = _IDLE_BUBBLE_H
        if state != _STATE_TRASH:
            self._trash_erase_prev = None
        # Fall/dizzy/trash widen the sprite dest — resize + remask immediately.
        self._apply_size(pin_global=pin_global)
        if state == _STATE_TRASH:
            # First trash frame must paint the full new plate (no partial dirty
            # rect from the pre-pad idle size — that left a half-erased flash).
            self.update()

    def _mark_user_interacted(self) -> None:
        """User touched the pet (pet/feed/menu/drag) — reset loneliness clock."""
        self._last_interact_at = time.monotonic()

    def _seconds_since_interact(self) -> float:
        return max(0.0, time.monotonic() - float(self._last_interact_at))

    def _schedule_idle(self) -> None:
        self._vx = 0.0
        self._vy = 0.0
        lonely = self._seconds_since_interact()
        # Lonely pets decide the next act sooner (shorter idle dwell).
        if lonely >= _LONELY_HARD_SEC:
            dur = random.uniform(1.8, 3.5)
        elif lonely >= _LONELY_SOFT_SEC:
            dur = random.uniform(2.5, 5.0)
        else:
            dur = random.uniform(3.5, 8.0)
        self._enter(_STATE_IDLE, duration=dur)

    def _rest_actions_enabled(self) -> bool:
        char = normalize_pet_character(self._character)
        if char == "hoodie":
            return pet_action_enabled(self.settings, "wait", character=char) or pet_action_enabled(
                self.settings, "sleep", character=char
            )
        return pet_action_enabled(self.settings, "wait", character=char)

    def _hoodie_rest_alternates(self) -> bool:
        return (
            self._character == "hoodie"
            and pet_action_enabled(self.settings, "wait", character="hoodie")
            and pet_action_enabled(self.settings, "sleep", character="hoodie")
        )

    def _wake_from_rest(self) -> None:
        if self._state == _STATE_SLEEP:
            self.say(random_pet_line(self.settings, action="wake"), msec=2000)
        else:
            self.say("好，不等了。", msec=1800)
        self._schedule_idle()

    def _choose_next_after_idle(self, cfg: dict) -> None:
        """Autonomous rest: hoodie alternates 玩手机 ↔ 睡觉 when both enabled."""
        del cfg
        if not self._rest_actions_enabled():
            self._schedule_idle()
            return
        lonely = self._seconds_since_interact()
        roll = random.random()

        if lonely >= _LONELY_HARD_SEC:
            if roll < 0.55:
                self._start_auto_rest()
            else:
                self._schedule_idle()
            return

        if lonely >= _LONELY_SOFT_SEC:
            if roll < 0.38:
                self._start_auto_rest()
            else:
                self._schedule_idle()
            return

        if roll < 0.18:
            self._start_auto_rest()
        else:
            self._schedule_idle()

    def _start_wander(self) -> None:
        geo = self._work_geo()
        margin = 40
        self._wander_target_x = random.randint(
            geo.x() + margin,
            geo.x() + geo.width() - self.width() - margin,
        )
        self._enter(_STATE_WANDER, duration=random.uniform(5.0, 12.0))
        if random.random() < 0.35:
            self.say(random_pet_line(self.settings, action="wander"), msec=2000)

    def _start_self_play(self) -> None:
        self._play_mode = _PLAY_MODE_SELF
        self._play_hop_x = self.pos().x()
        self._mood = min(100.0, self._mood + 4)
        self._enter(_STATE_PLAY, duration=_PLAY_SELF_SEC)
        if random.random() < 0.55:
            self.say(random_pet_line(self.settings, action="play"), msec=1800)

    def _start_auto_rest(self, *, prefer: str | None = None) -> None:
        """Alternate wait and sleep on rest (hoodie: two menu actions)."""
        char = normalize_pet_character(self._character)
        wait_on = pet_action_enabled(self.settings, "wait", character=char)
        sleep_on = pet_action_enabled(self.settings, "sleep", character=char)
        if char == "hoodie":
            if not wait_on and not sleep_on:
                self._schedule_idle()
                return
            if wait_on and sleep_on:
                if prefer == "sleep":
                    self._start_auto_sleep()
                    return
                if prefer == "wait":
                    self._start_auto_wait()
                    return
                if int(getattr(self, "_rest_flip", 0)) % 2 == 0:
                    self._start_auto_wait()
                else:
                    self._start_auto_sleep()
                self._rest_flip = int(getattr(self, "_rest_flip", 0)) + 1
                return
            if wait_on:
                self._start_auto_wait()
            else:
                self._start_auto_sleep()
            return
        if not pet_sleep_on_rest(char):
            self._start_auto_wait()
            return
        if int(getattr(self, "_rest_flip", 0)) % 2 == 0:
            self._start_auto_wait()
        else:
            self._start_auto_sleep()
        self._rest_flip = int(getattr(self, "_rest_flip", 0)) + 1

    def _start_auto_sleep(self) -> None:
        self.say(random_pet_line(self.settings, action="sleep"), msec=2200)
        self._enter(_STATE_SLEEP, duration=random.uniform(10.0, 18.0))

    def _start_auto_wait(self) -> None:
        self.say(random_pet_line(self.settings, action="wait"), msec=2200)
        self._enter(_STATE_WAIT, duration=random.uniform(12.0, 22.0))

    def _finish_trash_to_wait(self) -> None:
        """Plane gone → hard-cut to wait for a few seconds, then idle."""
        char = normalize_pet_character(self._character)
        if pet_action_enabled(self.settings, "wait", character=char):
            self._post_trash_wait = True
            # Brief linger after the plane — longer than a flash, shorter than auto-rest.
            self._enter(_STATE_WAIT, duration=random.uniform(6.0, 9.0))
            return
        self._post_trash_wait = False
        self._schedule_idle()

    def _clear_follow_cursor(self) -> None:
        cfg = self._cfg()
        if not cfg.get("follow_cursor", False):
            return
        cfg["follow_cursor"] = False
        try:
            save_settings(self.settings)
        except OSError:
            pass

    def _start_climb(self) -> None:
        """Maggot-style floor crawl along the nearer left/right screen edge."""
        self._clear_follow_cursor()
        geo = self._work_geo()
        floor = self._floor_y()
        pos = self.pos()
        left_x = geo.x() + 8
        right_x = geo.x() + geo.width() - self.width() - 8
        # Prefer the nearer side; crawl stays on the floor only (no wall climb).
        if abs(pos.x() - left_x) <= abs(pos.x() - right_x):
            self._climb_dir = -1
            self._climb_edge_x = left_x
        else:
            self._climb_dir = 1
            self._climb_edge_x = right_x
        self._climb_phase = "to_edge"
        self._climb_patrol_x = None
        self._vx = 0.0
        self._vy = 0.0
        self._facing = self._climb_dir
        self._mood = min(100.0, self._mood + 3)
        self._move_clamped(pos.x(), floor)
        self._enter(_STATE_CLIMB, duration=16.0)
        line = random_pet_line(self.settings, action="climb")
        if random.random() < 0.55:
            self.say(line, msec=1800)

    def _start_perch(self) -> None:
        """Sit still at the current position (no teleport to a window ledge)."""
        self._clear_follow_cursor()
        pos = self.pos()
        self._perch_y = pos.y()
        self._vx = 0.0
        self._vy = 0.0
        self._move_clamped(pos.x(), pos.y())
        self._enter(_STATE_PERCH, duration=random.uniform(6.0, 12.0))
        self.say("我坐这儿。", msec=1800)

    def _start_fall(self, vx: float, vy: float) -> None:
        self._vx = max(-_THROW_MAX, min(_THROW_MAX, vx))
        self._vy = max(-_THROW_MAX, min(_THROW_MAX, vy))
        self._enter(_STATE_FALL, duration=8.0)
        if abs(vx) + abs(vy) > 400 and random.random() < 0.55:
            self.say("飞啦！", msec=1400)

    def _enter_dizzy(self) -> None:
        self._vx = 0.0
        self._vy = 0.0
        self._enter(_STATE_DIZZY, duration=random.uniform(*pet_dizzy_sec(self._character)))
        self.say("晕晕的", msec=1100)

    def _on_ai(self) -> None:
        if self._dragging or not self.isVisible():
            return
        # Freeze autonomy while the action panel is open so「取消跟随」can be hit.
        if self._menu_open:
            return
        now = time.monotonic()
        cfg = self._cfg()

        if cfg.get("follow_cursor", False) and self._state not in (
            _STATE_SLEEP,
            _STATE_WAIT,
            _STATE_TRASH,
            _STATE_FALL,
            _STATE_DIZZY,
            _STATE_CLIMB,
        ):
            if self._state != _STATE_FOLLOW:
                self._enter(_STATE_FOLLOW, duration=9999)
            return

        if self._state == _STATE_SLEEP:
            if now >= self._state_until:
                if self._hoodie_rest_alternates():
                    self._start_auto_wait()
                    return
                self._mood = min(100.0, self._mood + 12)
                self.say(random_pet_line(self.settings, action="wake"), msec=2000)
                self._schedule_idle()
            return

        if self._state == _STATE_WAIT:
            if now >= self._state_until:
                # Brief linger after paper-plane trash — back to idle, not sleep.
                if getattr(self, "_post_trash_wait", False):
                    self._post_trash_wait = False
                    self._schedule_idle()
                    return
                if self._hoodie_rest_alternates():
                    self._start_auto_sleep()
                    return
                self._schedule_idle()
            return

        if self._state == _STATE_TRASH:
            self._maybe_recycle_drop()
            if now >= self._state_until:
                self._maybe_recycle_drop()
                self._finish_trash_to_wait()
            return

        if self._state == _STATE_DIZZY:
            if now >= self._state_until:
                self._schedule_idle()
            return

        if self._state in (_STATE_HAPPY, _STATE_PLAY, _STATE_PERCH):
            if now >= self._state_until:
                if self._state == _STATE_PERCH and random.random() < 0.45:
                    self._start_fall(random.uniform(-80, 80), 40)
                else:
                    self._schedule_idle()
            return

        if self._state in (_STATE_FALL, _STATE_CLIMB):
            if now >= self._state_until:
                # Fall timeout (no soft land): still show a brief stun if possible.
                if self._state == _STATE_FALL:
                    self._enter_dizzy()
                else:
                    self._schedule_idle()
            return

        if now >= self._state_until:
            if self._state == _STATE_WANDER:
                if self._seconds_since_interact() >= _LONELY_SOFT_SEC:
                    self._choose_next_after_idle(cfg)
                else:
                    self._schedule_idle()
            elif self._state == _STATE_IDLE:
                self._choose_next_after_idle(cfg)
            else:
                self._schedule_idle()

        if self._state in (_STATE_IDLE, _STATE_WANDER, _STATE_PERCH, _STATE_WAIT) and now >= self._next_chatter:
            self.say(random_pet_line(self.settings), msec=2600)
            self._next_chatter = now + random.uniform(22, 55)

        cursor = QCursor.pos()
        center = self.frameGeometry().center()
        dist = math.hypot(cursor.x() - center.x(), cursor.y() - center.y())
        near = dist < _CURSOR_NOTICE_PX
        if near and not self._cursor_was_near and self._state in (
            _STATE_IDLE,
            _STATE_WANDER,
            _STATE_PERCH,
        ):
            if cursor.x() != center.x():
                self._facing = 1 if cursor.x() > center.x() else -1
            if random.random() < 0.45:
                self.say("嗯？", msec=1200)
            self._mood = min(100.0, self._mood + 1.5)
        self._cursor_was_near = near

    def _on_tick(self) -> None:
        if not self.isVisible():
            return
        now = time.monotonic()
        dt = max(0.0, now - self._last_tick)
        self._last_tick = now
        self._frame = (self._frame + 1) % 10_000
        self._anim.tick(dt)

        # Needs decay (units per minute)
        self._hunger = max(0.0, self._hunger - (_HUNGER_DECAY_PER_MIN / 60.0) * dt)
        self._mood = max(0.0, self._mood - (_MOOD_DECAY_PER_MIN / 60.0) * dt)

        if self._bubble_until and now > self._bubble_until:
            self._bubble_text = ""
            self._bubble_until = 0.0
            self._recompute_bubble_height()

        # Expire FX
        self._fx_hearts = [h for h in self._fx_hearts if now - h[2] < 1.1]
        self._fx_food = [f for f in self._fx_food if now - f[2] < 0.9]

        if not self._dragging:
            self._step_motion(dt)
            if self._state == _STATE_TRASH:
                self._maybe_recycle_drop()

        self.update()
        self._sync_tick_interval()

    def _sync_tick_interval(self) -> None:
        """Slow the render clock when idle and the pointer is away (lighter tray app)."""
        want = _TICK_MS
        if self._state in _IDLE_TICK_STATES and not self._pointer_over:
            want = _TICK_IDLE_MS
        if self._tick.interval() != want:
            self._tick.setInterval(want)

    def _step_motion(self, dt: float) -> None:
        if self._state in (_STATE_SLEEP, _STATE_WAIT, _STATE_TRASH):
            return
        # Keep the pet still while choosing an action (esp. cancel follow).
        if self._menu_open:
            return
        pos = self.pos()
        floor = self._floor_y()
        geo = self._work_geo()

        if self._state == _STATE_FALL:
            self._vy += _GRAVITY * dt
            nx = int(pos.x() + self._vx * dt)
            ny = int(pos.y() + self._vy * dt)
            if self._vx != 0:
                self._facing = 1 if self._vx > 0 else -1
            # Bounce off sides lightly
            if nx <= geo.x() + 4:
                nx = geo.x() + 4
                self._vx = abs(self._vx) * 0.45
            elif nx >= geo.x() + geo.width() - self.width() - 4:
                nx = geo.x() + geo.width() - self.width() - 4
                self._vx = -abs(self._vx) * 0.45
            if ny >= floor:
                ny = floor
                if abs(self._vy) < 220 and abs(self._vx) < 120:
                    self._move_clamped(nx, floor)
                    self._enter_dizzy()
                    return
                self._vy = -abs(self._vy) * 0.35
                self._vx *= 0.72
            self._move_clamped(nx, max(geo.y() + 4, ny))
            return

        if self._state == _STATE_DIZZY:
            # Stay planted on the floor while the stun cel holds.
            if abs(pos.y() - floor) > 1:
                self._move_clamped(pos.x(), floor)
            return

        if self._state == _STATE_CLIMB:
            self._step_climb(dt, geo, floor)
            return

        if self._state == _STATE_PERCH:
            if self._perch_y is not None and abs(pos.y() - self._perch_y) > 2:
                self._move_clamped(pos.x(), self._perch_y)
            return

        if self._state == _STATE_FOLLOW or (
            self._state == _STATE_PLAY and self._play_mode == _PLAY_MODE_CHASE
        ):
            cursor = QCursor.pos()
            cx = self.frameGeometry().center()
            dx = cursor.x() - cx.x()
            dy = cursor.y() - cx.y()
            dist = math.hypot(dx, dy)
            stop = 28 if self._state == _STATE_FOLLOW else 14
            if dist > stop:
                speed = 260.0 if self._state == _STATE_PLAY else 140.0
                step = min(speed * dt, dist - stop * 0.4)
                if dx != 0:
                    self._facing = 1 if dx > 0 else -1
                self._move_clamped(
                    int(pos.x() + dx / dist * step),
                    int(pos.y() + dy / dist * step),
                )
            return

        if self._state == _STATE_PLAY and self._play_mode == _PLAY_MODE_SELF:
            if self._play_hop_x is None:
                self._play_hop_x = pos.x()
            amp = 46.0
            phase = self._frame / 8.0
            target_x = int(self._play_hop_x + math.sin(phase) * amp)
            if abs(target_x - pos.x()) > 1:
                self._facing = 1 if target_x > pos.x() else -1
            ny = int(pos.y() + (floor - pos.y()) * min(1.0, 4.0 * dt))
            self._move_clamped(target_x, ny)
            return

        if self._state == _STATE_WANDER and self._wander_target_x is not None:
            dx = self._wander_target_x - pos.x()
            if abs(dx) < 4:
                self._wander_target_x = None
                self._schedule_idle()
                return
            self._facing = 1 if dx > 0 else -1
            step = min(90.0 * dt, abs(dx))
            nx = int(pos.x() + math.copysign(step, dx))
            ny = int(pos.y() + (floor - pos.y()) * min(1.0, 3.0 * dt))
            self._move_clamped(nx, ny)

    def _step_climb(self, dt: float, geo: QRect, floor: int) -> None:
        """Stay on the floor: scoot to L/R edge, then maggot-wriggle along the bottom."""
        pos = self.pos()
        edge_x = int(self._climb_edge_x if self._climb_edge_x is not None else geo.x() + 8)
        self._facing = -1 if self._climb_dir < 0 else 1
        # Always glued to the floor.
        ny = int(pos.y() + (floor - pos.y()) * min(1.0, 8.0 * dt))

        if self._climb_phase == "to_edge":
            dx = edge_x - pos.x()
            if abs(dx) <= 6:
                self._move_clamped(edge_x, floor)
                # Wriggle a short stretch toward the opposite side, then stop.
                inward = 1 if self._climb_dir < 0 else -1
                span = min(220, max(120, geo.width() // 5))
                self._climb_patrol_x = edge_x + inward * span
                self._climb_phase = "wriggle"
                return
            step = min(95.0 * dt, abs(dx))
            self._move_clamped(int(pos.x() + math.copysign(step, dx)), ny)
            return

        # Maggot wriggle along the floor (slow peristalsis steps).
        target = self._climb_patrol_x
        if target is None:
            self._schedule_idle()
            return
        dx = target - pos.x()
        if abs(dx) <= 8:
            self._move_clamped(target, floor)
            self._schedule_idle()
            return
        self._facing = 1 if dx > 0 else -1
        # Pulse speed: stretch phase scoots, squash phase almost pauses.
        pulse = 0.35 + 0.65 * max(0.0, math.sin(self._frame / 5.0))
        step = min(55.0 * dt * pulse, abs(dx))
        self._move_clamped(int(pos.x() + math.copysign(step, dx)), ny)

    # ------------------------------------------------------------------ actions

    def _action_feed(self) -> None:
        self._mark_user_interacted()
        self._hunger = min(100.0, self._hunger + 35)
        self._mood = min(100.0, self._mood + 10)
        self._spawn_food()
        self.say(random_pet_line(self.settings, action="feed"), msec=2600)
        self._enter(_STATE_HAPPY, duration=pet_happy_sec(self._character))

    def _action_pet(self) -> None:
        self._mark_user_interacted()
        self._mood = min(100.0, self._mood + 18)
        self._spawn_hearts(5)
        self.say(random_pet_line(self.settings, action="pet"), msec=2200)
        self._enter(_STATE_HAPPY, duration=pet_happy_sec(self._character))

    def _action_fall(self) -> None:
        """Menu「跌落」: hop up then tumble (same path as a throw)."""
        self._mark_user_interacted()
        self._clear_follow_cursor()
        self.say(random_pet_line(self.settings, action="fall"), msec=1800)
        self._start_fall(random.uniform(-260, 260), random.uniform(-520, -360))

    def _action_sleep_toggle(self) -> None:
        self._mark_user_interacted()
        if self._state == _STATE_SLEEP:
            self._wake_from_rest()
            return
        if self._state == _STATE_WAIT:
            # Switch rest pose — do not wake (menu shows 睡, not a second 起).
            self._clear_follow_cursor()
            self._start_auto_sleep()
            return
        self._clear_follow_cursor()
        if self._character == "hoodie":
            self._start_auto_rest(prefer="sleep")
            return
        self.say(random_pet_line(self.settings, action="sleep"), msec=2400)
        self._enter(_STATE_SLEEP, duration=random.uniform(12.0, 22.0))

    def _action_wait_toggle(self) -> None:
        """「玩手机」/「等待」: wake if on wait; switch from sleep; else enter rest."""
        self._mark_user_interacted()
        if self._state == _STATE_WAIT:
            self._wake_from_rest()
            return
        if self._state == _STATE_SLEEP:
            self._clear_follow_cursor()
            self._start_auto_wait()
            return
        self._clear_follow_cursor()
        if self._character == "hoodie":
            self._start_auto_rest(prefer="wait")
        elif pet_sleep_on_rest(self._character):
            self._start_auto_rest()
        else:
            self._start_auto_wait()

    def _hold_drag_headroom(self) -> int:
        h = self._sprite_h()
        return max(32, int(round(h * _HOLD_DRAG_HEADROOM_FRAC)))

    def _apply_drag_window_extra(self, extra: int) -> None:
        """Grow or shrink transparent space above the pet without shifting the sprite."""
        extra = max(0, int(extra))
        delta = extra - int(getattr(self, "_drag_layout_extra", 0))
        if delta == 0:
            self._drag_layout_extra = extra
            return
        g = self.frameGeometry()
        self.setFixedSize(g.width(), max(1, g.height() + delta))
        self.move(g.x(), g.y() - delta)
        if self._drag_offset is not None:
            self._drag_offset = QPoint(self._drag_offset.x(), self._drag_offset.y() + delta)
        self._drag_layout_extra = extra
        self._sync_drop_zone()
        self._sync_input_mask()

    def _sprite_dest_rect(self) -> QRect:
        ox, oy = self._body_origin()
        x = ox + 4
        y = oy + self._bubble_h + (0 if self._bubble_h <= _IDLE_BUBBLE_H else 2)
        w = self._body_width() - 8
        h = self._sprite_h()
        if self._dragging:
            extra = self._hold_drag_headroom()
            return QRect(x, y - extra, w, h + extra)
        if self._state == _STATE_TRASH:
            pad_l, pad_r, pad_t = self._trash_dest_pad()
            return QRect(x - pad_l, y - pad_t, w + pad_l + pad_r, h + pad_t)
        if self._character == "hoodie" and self._state == _STATE_WAIT:
            pad_h = max(22, w // 3)
            return QRect(x - pad_h, y, w + 2 * pad_h, h)
        if self._character == "hoodie" and self._state == _STATE_SLEEP:
            pad_h = max(16, w // 5)
            pad_t = self._sprite_dest_extra_top()
            return QRect(x - pad_h, y - pad_t, w + 2 * pad_h, h + pad_t)
        return QRect(x, y, w, h)

    def _sprite_dest_extra_top(self) -> int:
        """Headroom above sprite dest (hoodie sleep zzz / trash throw) — in widget height."""
        if self._character == "hoodie" and self._state == _STATE_SLEEP:
            return max(10, self._sprite_h() // 10)
        if self._character == "hoodie" and self._state == _STATE_TRASH:
            return max(80, self._sprite_h())
        return 0

    def _recycle_paths_now(self, paths: list[Path]) -> list[Path]:
        """Send *paths* to the Recycle Bin immediately. Returns successes."""
        done: list[Path] = []
        try:
            from src.organize_suppress import (
                clear_organize_suppress,
                suppress_desktop_item,
            )
        except Exception:
            clear_organize_suppress = None  # type: ignore[assignment]
            suppress_desktop_item = None  # type: ignore[assignment]
        for raw in paths:
            try:
                path = Path(raw)
            except OSError:
                continue
            try:
                if not path.exists():
                    # Already gone — still count as cleaned for DeskTidy UI.
                    done.append(path)
                    continue
                if suppress_desktop_item is not None:
                    suppress_desktop_item(path)
                delete_to_trash(path)
                done.append(path)
                # Path suppress must not linger: same path recreated / moved back
                # would skip watcher → no public float for ~12s.
                if clear_organize_suppress is not None:
                    clear_organize_suppress(path)
            except Exception:
                if clear_organize_suppress is not None:
                    try:
                        clear_organize_suppress(raw)
                    except Exception:
                        pass
                continue
        return done

    def _start_file_trash(self, paths: list[Path]) -> list[Path]:
        """Play crumple anim; delete files *now* (animation is feedback only).

        Deleting mid-cel left the desktop file on disk after the float was
        removed — dragging the same name back from a folder then hit Explorer's
        「替换或跳过文件」dialog.
        """
        self._mark_user_interacted()
        self._clear_follow_cursor()
        recycled = self._recycle_paths_now(list(paths))
        self._trash_paths = []
        self._trash_recycled = True
        # Trash cels are authored as a rightward throw — lock facing so wander/follow
        # left-facing does not mirror the plane into a second leftward flight.
        self._facing = 1
        duration = trash_anim_sec(
            self._character,
            len(self._poses.get("trash") or []),
        )
        self._enter(_STATE_TRASH, duration=duration)
        bubble_ms = int(duration * 1000) + 300
        if recycled:
            self.say(random_pet_line(self.settings, action="trash"), msec=bubble_ms)
        else:
            self.say("这个扔不掉…", msec=bubble_ms)
        return recycled

    def _accept_file_drop(self, event) -> bool:
        """Explorer OLE onto the sprite — recycle + crumple animation."""
        if self._dragging or self._state == _STATE_TRASH:
            return False
        mime = event.mimeData()
        if mime is None or not mime_has_droppable_items(mime):
            return False
        action = preferred_drop_action_for_mime(mime)
        event.setDropAction(action)
        event.accept()
        return True

    def _cleanup_pins_after_trash(self, recycled: list[Path]) -> None:
        """Drop DeskTidy floats/pins after files are already in the Recycle Bin."""
        if not recycled:
            return
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app else None
        settings = getattr(desk, "settings", None) if desk is not None else None
        if not isinstance(settings, dict):
            settings = self.settings if isinstance(self.settings, dict) else None
        if isinstance(settings, dict):
            try:
                from src.public_desktop import remove_public_paths

                remove_public_paths(settings, recycled)
            except Exception:
                pass
            try:
                from src.fence_rules import unpin_paths_from_virtual_fence

                for fence in list(settings.get("fences") or []):
                    if isinstance(fence, dict):
                        unpin_paths_from_virtual_fence(fence, recycled)
            except Exception:
                pass
            try:
                save_settings(settings)
            except OSError:
                pass
        if desk is None:
            return
        rem = getattr(desk, "_remove_public_icon_widget", None)
        if callable(rem):
            for path in recycled:
                try:
                    rem(path)
                except Exception:
                    pass
        for fw in list(getattr(desk, "fences", []) or []):
            remove = getattr(fw, "_remove_virtual_icon_widget", None)
            if not callable(remove):
                continue
            for path in recycled:
                try:
                    remove(Path(path))
                except Exception:
                    pass
        refresh = getattr(desk, "refresh_public_desktop", None)
        if callable(refresh):
            try:
                refresh(immediate=True)
            except Exception:
                pass

    def accepts_trash_at(self, global_pos: QPoint) -> bool:
        """True when *global_pos* is over the character sprite (not page chips)."""
        if not self.isVisible() or self._dragging or self._state == _STATE_TRASH:
            return False
        try:
            local = self.mapFromGlobal(global_pos)
        except Exception:
            return False
        return self._sprite_dest_rect().adjusted(-4, -4, 4, 4).contains(local)

    def _maybe_recycle_drop(self) -> None:
        """Fallback if paths were queued without immediate recycle (should be rare)."""
        if self._trash_recycled or self._state != _STATE_TRASH:
            return
        if not self._trash_paths:
            self._trash_recycled = True
            return
        recycled = self._recycle_paths_now(list(self._trash_paths))
        self._trash_paths = []
        self._trash_recycled = True
        if recycled:
            self.say("飞走啦～", msec=1600)
        else:
            self.say("这个扔不掉…", msec=1800)

    def _action_toggle_follow(self) -> None:
        self._mark_user_interacted()
        cfg = self._cfg()
        follow = not bool(cfg.get("follow_cursor", False))
        cfg["follow_cursor"] = follow
        try:
            save_settings(self.settings)
        except OSError:
            pass
        if follow:
            self._enter(_STATE_FOLLOW, duration=9999)
            self.say("跟着你走～点「⋯」再点「跟」可取消。", msec=2400)
        else:
            # Leave FOLLOW immediately so motion stops even if menu just closed.
            self._schedule_idle()
            self.say("好，不跟了。", msec=2000)

    def _action_toggle_wander(self) -> None:
        self._mark_user_interacted()
        cfg = self._cfg()
        cfg["auto_wander"] = not bool(cfg.get("auto_wander", True))
        try:
            save_settings(self.settings)
        except OSError:
            pass
        if cfg["auto_wander"]:
            self._start_wander()
            self.say("那我自己逛逛～", msec=2000)
        else:
            self._schedule_idle()
            self.say("好，先站这儿。", msec=2000)

    # ------------------------------------------------------------------ paint

    def paintEvent(self, event) -> None:  # noqa: N802
        del event
        p = getattr(self, "_palette", get_theme_palette("mist"))
        now = time.monotonic()

        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        # Pet sprites opt out of SmoothPixmapTransform inside paint_pet_sprite
        # so HiDPI + squash stay sharp; keep it off here too.
        painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, False)

        bubble_h = self._bubble_h
        body_w = self._body_width()
        ox, oy = self._body_origin()

        sprite_rect = self._sprite_dest_rect()
        anim_state = self._state
        facing = self._facing
        if self._state == _STATE_TRASH:
            # Authored throw is always rightward; never mirror mid-flight.
            facing = 1
        if self._dragging:
            # Dragging = pinched by collar (hold pose).
            anim_state = "hold"
        elif self._state == _STATE_CLIMB:
            # Single floor-wriggle silhouette (no wall cling).
            anim_state = "climb_crawl"
        if self._state == _STATE_TRASH:
            # Full Source clear after the first trash paint. Tight plate erase
            # left arm/plane ghost outlines (轮廓虚) on layered HWNDs; skip the
            # first frame so pad growth does not flash wallpaper (开场闪烁).
            if self._trash_erase_prev is not None:
                painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
                painter.fillRect(self.rect(), QColor(0, 0, 0, 0))
                painter.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)
            self._trash_erase_prev = QRect(self._trash_erase_rect())
        paint_pet_sprite(
            painter,
            self._poses,
            dest=sprite_rect,
            state=anim_state,
            frame=self._frame,
            facing=facing,
            now=now,
            anim=self._anim,
            character=self._character,
        )

        for hx, hy, born in self._fx_hearts:
            age = now - born
            alpha = max(0, int(220 * (1.0 - age / 1.1)))
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(255, 90, 120, alpha))
            px = ox + int(hx * body_w)
            py = oy + int(hy * (bubble_h + self._sprite_h()) - age * 36)
            painter.drawEllipse(px - 4, py - 4, 8, 8)

        for fx, fy, born in self._fx_food:
            age = now - born
            alpha = max(0, int(230 * (1.0 - age / 0.9)))
            painter.setPen(QColor(180, 120, 40, alpha))
            font = QFont("Microsoft YaHei UI", 12)
            painter.setFont(font)
            px = ox + int(fx * body_w) - 8
            py = oy + int(fy * (bubble_h + self._sprite_h()) - age * 28)
            painter.drawText(px, py, "🍱")

        # Page chips + 「⋯」 only — speech bubbles retired.
        self._paint_page_bubbles(painter, p)
        self._paint_menu_chrome(painter, p)
        painter.end()

    def _paint_page_bubbles(self, painter: QPainter, p: dict) -> None:
        if self._dragging:
            return
        if not self._show_page_bubbles():
            return
        border = QColor(p.get("border", "#D5DCE7"))
        accent = QColor(p.get("accent", "#2563EB"))
        soft = QColor(p.get("accent_soft", "#DBEAFE"))
        ink = QColor(p.get("text", "#1F2937"))
        font = QFont("Microsoft YaHei UI", 9)
        painter.setFont(font)
        for i, item in enumerate(self._chrome_items):
            kind = str(item.get("kind") or "")
            name = str(item.get("label") or "")
            r = self._page_bubble_rect(i)
            if r.isNull():
                continue
            current = False
            if kind == "page":
                try:
                    current = int(item.get("page_id", -1)) == self._current_page_id
                except (TypeError, ValueError):
                    current = False
            hovered = self._chrome_hover == i
            painter.setPen(QPen(accent if (current or hovered) else border, 1.5 if current else 1))
            if current:
                painter.setBrush(soft)
            elif hovered:
                painter.setBrush(QColor(255, 255, 255, 250))
            else:
                painter.setBrush(QColor(255, 255, 255, 230))
            painter.drawRoundedRect(r, 14, 14)
            mid_x = r.center().x()
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(soft if current else QColor(255, 255, 255, 230 if not hovered else 250))
            painter.drawPolygon(
                QPolygon(
                    [
                        QPoint(mid_x, r.bottom() + 5),
                        QPoint(mid_x - 5, r.bottom() - 1),
                        QPoint(mid_x + 5, r.bottom() - 1),
                    ]
                )
            )
            painter.setPen(accent if current else ink)
            painter.drawText(r, int(Qt.AlignmentFlag.AlignCenter), name)

    def _paint_menu_chrome(self, painter: QPainter, p: dict) -> None:
        btn = self._menu_button_rect()
        border = QColor(p.get("border", "#D5DCE7"))
        accent = QColor(p.get("accent", "#2563EB"))
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
        painter.setPen(QPen(accent if self._menu_open else border, 1.5))
        painter.setBrush(QColor(255, 255, 255, 235))
        painter.drawEllipse(btn)
        painter.setPen(QColor("#374151"))
        font = QFont("Microsoft YaHei UI", 11, QFont.Weight.Bold)
        painter.setFont(font)
        painter.drawText(btn, int(Qt.AlignmentFlag.AlignCenter), "⋯")

        if not self._menu_open:
            return

        defs = self._action_defs()
        panel = self._action_panel_rect()
        if panel.isNull():
            return
        painter.setPen(QPen(border, 1))
        painter.setBrush(QColor(255, 255, 255, 242))
        painter.drawRoundedRect(panel.adjusted(2, 0, -2, 0), 12, 12)

        icon_font = QFont("Microsoft YaHei UI", 10, QFont.Weight.Bold)
        for i, (aid, glyph, tip) in enumerate(defs):
            r = self._action_button_rect(i)
            hovered = self._menu_hover == aid
            painter.setPen(QPen(accent if hovered else border, 1))
            painter.setBrush(QColor("#DBEAFE") if hovered else QColor(248, 250, 252, 255))
            painter.drawEllipse(r)
            painter.setPen(QColor("#1F2937"))
            painter.setFont(icon_font)
            painter.drawText(r, int(Qt.AlignmentFlag.AlignCenter), glyph)
            if hovered:
                tip_font = QFont("Microsoft YaHei UI", 8)
                painter.setFont(tip_font)
                fm = QFontMetrics(tip_font)
                tw = fm.horizontalAdvance(tip) + 10
                tip_rect = QRect(
                    self._body_origin()[0] + self._body_width() - tw - 2,
                    r.center().y() - 9,
                    tw,
                    18,
                )
                if tip_rect.left() < 4:
                    tip_rect.moveLeft(4)
                painter.setPen(QPen(border, 1))
                painter.setBrush(QColor(255, 255, 255, 245))
                painter.drawRoundedRect(tip_rect, 8, 8)
                painter.setPen(QColor("#374151"))
                painter.drawText(tip_rect, int(Qt.AlignmentFlag.AlignCenter), tip)

    def _draw_bar(
        self,
        painter: QPainter,
        x: int,
        y: int,
        w: int,
        h: int,
        ratio: float,
        color: QColor,
    ) -> None:
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(0, 0, 0, 40))
        painter.drawRoundedRect(x, y, w, h, 3, 3)
        fill = max(0, min(w, int(w * ratio)))
        if fill > 0:
            painter.setBrush(color)
            painter.drawRoundedRect(x, y, fill, h, 3, 3)

    # ------------------------------------------------------------------ mouse

    def mousePressEvent(self, event) -> None:  # noqa: N802
        local = event.position().toPoint()
        if event.button() == Qt.MouseButton.RightButton:
            chrome_i = self._hit_chrome_index(local)
            if chrome_i is not None:
                self._press_on_menu = True
                self._activate_chrome_item(chrome_i, right=True)
                event.accept()
                return
            self._toggle_action_menu()
            event.accept()
            return
        if event.button() == Qt.MouseButton.LeftButton:
            self._press_on_menu = False
            # Toggle 「⋯」 on press (not release): while following, release often
            # misses the button because the widget moves under the cursor.
            if self._menu_button_rect().contains(local):
                self._press_on_menu = True
                self._toggle_action_menu()
                event.accept()
                return
            chrome_i = self._hit_chrome_index(local)
            if chrome_i is not None:
                self._press_on_menu = True
                self._activate_chrome_item(chrome_i, right=False)
                event.accept()
                return
            action_id = self._hit_action_id(local)
            if action_id is not None:
                self._press_on_menu = True
                self._run_action(action_id)
                event.accept()
                return
            if self._menu_open:
                self._set_menu_open(False)
                event.accept()
                return
            self._begin_drag(event.globalPosition().toPoint())
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:  # noqa: N802
        local = event.position().toPoint()
        if self._update_drag(event.globalPosition().toPoint()):
            event.accept()
            return
        if self._menu_button_rect().contains(local):
            self.setCursor(Qt.CursorShape.PointingHandCursor)
            if self._menu_hover is not None or self._chrome_hover is not None:
                self._menu_hover = None
                self._chrome_hover = None
                self.update()
        else:
            chrome_hover = self._hit_chrome_index(local)
            hover = self._hit_action_id(local)
            if chrome_hover is not None or hover is not None:
                self.setCursor(Qt.CursorShape.PointingHandCursor)
            elif not self._dragging:
                self.setCursor(Qt.CursorShape.OpenHandCursor)
            if hover != self._menu_hover or chrome_hover != self._chrome_hover:
                self._menu_hover = hover
                self._chrome_hover = chrome_hover
                self.update()
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            was_drag = self._dragging
            pressed_menu = self._press_on_menu
            self._end_drag()
            self._press_on_menu = False
            self.setCursor(Qt.CursorShape.OpenHandCursor)
            # Menu / actions already handled on press — do not toggle again on release.
            if pressed_menu:
                event.accept()
                return
            if not was_drag:
                if pet_action_enabled(self.settings, "pet", character=self._character):
                    self._action_pet()
            event.accept()
            return
        super().mouseReleaseEvent(event)

    def mouseDoubleClickEvent(self, event) -> None:  # noqa: N802
        if event.button() == Qt.MouseButton.LeftButton:
            if self._menu_button_rect().contains(event.position().toPoint()):
                event.accept()
                return
            if pet_action_enabled(self.settings, "pet", character=self._character):
                self._action_pet()
            event.accept()
            return
        super().mouseDoubleClickEvent(event)

    def leaveEvent(self, event) -> None:  # noqa: N802
        self._pointer_over = False
        self._sync_tick_interval()
        if self._menu_hover is not None or self._chrome_hover is not None:
            self._menu_hover = None
            self._chrome_hover = None
            self.update()
        super().leaveEvent(event)

    def enterEvent(self, event) -> None:  # noqa: N802
        self._pointer_over = True
        self._sync_tick_interval()
        super().enterEvent(event)

    def _begin_drag(self, global_pos: QPoint) -> None:
        self._press_global = global_pos
        self._drag_offset = global_pos - self.frameGeometry().topLeft()
        self._dragging = False
        self._drag_samples = [(time.monotonic(), global_pos)]
        self._vx = 0.0
        self._vy = 0.0

    def _update_drag(self, global_pos: QPoint) -> bool:
        if self._drag_offset is None or self._press_global is None:
            return False
        if not self._dragging:
            if (global_pos - self._press_global).manhattanLength() < _DRAG_THRESHOLD:
                return False
            self._dragging = True
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            self._mark_user_interacted()
            if self._state not in (_STATE_SLEEP, _STATE_WAIT, _STATE_TRASH):
                self._enter(_STATE_IDLE, duration=2.0)
            self._clear_follow_cursor()
            from src.ui.pet_anim import pose_for_state

            self._anim.set_pose(pose_for_state("hold"))
            self._apply_drag_window_extra(self._hold_drag_headroom())
            # Drag can follow a page-switch sink; raise so wallpaper patterns
            # never cover the sprite mid-gesture.
            configure_desktop_overlay(self, peek=False, raise_band=True)
            _restack_fences_after_pet_raise()
            self._sync_input_mask()
            self.update()
        now = time.monotonic()
        self._drag_samples.append((now, global_pos))
        self._drag_samples = [s for s in self._drag_samples if now - s[0] <= 0.12]
        top_left = global_pos - self._drag_offset
        self._move_clamped(top_left.x(), top_left.y())
        return True

    def _end_drag(self) -> None:
        try:
            if self._dragging:
                vx, vy = self._drag_velocity()
                speed = math.hypot(vx, vy)
                self._drag_offset = None
                self._press_global = None
                self._drag_samples = []
                self._dragging = False
                # Clear headroom before say()/snap — otherwise _apply_size shrinks
                # once and finally shrinks again, clipping the sprite on throw.
                self._clear_drag_layout()
                if speed >= _THROW_MIN:
                    self._start_fall(vx, vy)
                    return
                snapped = snap_geometry(self.geometry(), _SNAP_THRESHOLD)
                self.setGeometry(snapped)
                self._persist_geometry()
                # If left above the floor, fall down (Shimeji drop)
                if self.pos().y() < self._floor_y() - 24:
                    self._start_fall(0.0, 80.0)
                    return
                self._schedule_idle()
        finally:
            self._clear_drag_layout()
            self._drag_offset = None
            self._press_global = None
            self._dragging = False
            self._drag_samples = []
            self.setCursor(Qt.CursorShape.OpenHandCursor)
            self._sync_input_mask()

    def _drag_velocity(self) -> tuple[float, float]:
        if len(self._drag_samples) < 2:
            return 0.0, 0.0
        t0, p0 = self._drag_samples[0]
        t1, p1 = self._drag_samples[-1]
        dt = max(0.016, t1 - t0)
        vx = (p1.x() - p0.x()) / dt
        vy = (p1.y() - p0.y()) / dt
        return vx, vy


def find_pet_trash_target(global_pos: QPoint | None = None) -> DesktopPetWidget | None:
    """Visible pet whose sprite contains *global_pos* (custom-drag hit test).

    Geometry alone is not enough: DeskNote / other apps can cover the pet's
    screen rect while staying on top. Without a z-order check, releasing on
    DeskNote still recycled the file (log: ``pet trash recycled``).
    """
    pos = global_pos if global_pos is not None else QCursor.pos()
    try:
        from src.win_shell import (
            is_desknote_window_at,
            is_visible_external_app_drop_point,
        )

        if is_desknote_window_at(pos.x(), pos.y()):
            return None
        if is_visible_external_app_drop_point(pos.x(), pos.y()):
            return None
    except Exception:
        pass
    app = QApplication.instance()
    if app is None:
        return None
    for top in app.topLevelWidgets():
        if not isinstance(top, DesktopPetWidget):
            continue
        try:
            if top.accepts_trash_at(pos):
                return top
        except RuntimeError:
            continue
    return None


def deliver_paths_to_pet_trash(
    paths: list[Path],
    *,
    global_pos: QPoint | None = None,
) -> list[Path]:
    """Trash on the pet under *global_pos*. Returns paths actually recycled."""
    pet = find_pet_trash_target(global_pos)
    if pet is None:
        return []
    clean = [Path(p) for p in paths if p]
    if not clean:
        return []
    return list(pet._start_file_trash(clean))
