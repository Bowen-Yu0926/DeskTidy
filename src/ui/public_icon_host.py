"""Single shell-attached host for all public (non-fence) desktop floats.

Market pattern (Fences / Explorer DefView): one HWND owns many icons. DeskTidy
previously opened one top-level window per float; page switches and Z-order
repair then scaled with icon count. Children paint into this host; click-through
uses a QRegion mask so empty desktop still reaches Explorer.

Hit testing (mouse):
- Prefer the union of icon footprints (safe click-through).
- When icons form a compact cluster (<50% of the host), expand the mask to the
  cluster bounding box so Explorer-like rubber-band selection works in the gaps.
- Sparse layouts keep padded footprints so far wallpaper stays click-through —
  Explorer keeps native empty-desktop features (右键 / 粘贴 / 刷新).
- When DeskTidy owns the desktop (``hide_shell_icons`` + icons present), expand
  the mask to the full host minus fence/pet HWNDs so 框选 can start on empty
  wallpaper (DefView cannot select our floats) without swallowing the desktop
  pet. Empty-plate **LMB** starts 框选; empty-plate **RMB** must open Explorer's
  real desktop menu (查看 / 排序 / 粘贴 / 个性化 / …) — never a thin filesystem
  ``CreateViewObject`` substitute. When this session owns the plate
  (``hide_shell_icons``), ListView is hidden so ``HTTRANSPARENT`` pass-through
  cannot show a menu — keep HTCLIENT and forward via
  ``request_desktop_background_menu`` + explorer-menu freeze. When shell icons
  are still visible, empty RMB may ``HTTRANSPARENT`` through to DefView.
- Each ``PublicIconWidget`` masks hits to glyph+caption only; transparent shelf
  margins fall through to this host so 框选 can start between overlapping shelves.
- ``paintEvent`` fills *shelf-gap* pixels only (cluster mask minus glyph/caption
  ink) with alpha=1. Win32 layered + ``WA_TranslucentBackground`` hit-tests by
  alpha: a solid full-cluster plate stole clicks from fences underneath whenever
  the public host stacked above them. Glyph pixels stay alpha=0 so icon widgets
  still own those hits; fence/pet exclusion stays in ``hit_test_region`` / setMask.
- Alpha stays 1 — higher values showed a dark vertical veil behind floats.

OLE (folder→desktop when shell icons are hidden):
- ``AcceptDrops`` stays on while the session owns drops.
- Mouse mask expands to the full host (minus fences/pet) when shell icons are
  hidden so 框选 works on empty wallpaper; empty RMB goes to DefView (hit-test
  pass-through or ``WM_CONTEXTMENU`` forward). When shell icons are visible,
  mask stays icon/cluster-only so Explorer keeps empty-desktop 右键 / 粘贴 / 刷新.
"""

from __future__ import annotations

import ctypes
from ctypes import wintypes

from PyQt6.QtCore import QEvent, QObject, QPoint, QRect, QTimer, Qt
from PyQt6.QtGui import QColor, QPainter, QRegion
from PyQt6.QtWidgets import QApplication, QRubberBand, QWidget

_WM_NCHITTEST = 0x0084
_HTCLIENT = 1
_HTTRANSPARENT = -1
_VK_RBUTTON = 0x02

from src.win_shell import (
    DESKTIDY_VIRTUAL_MIME,
    configure_desktop_overlay,
    mime_has_droppable_items,
    preferred_drop_action_for_mime,
)

# Cluster hit ink for Win32 layered windows (fully transparent pixels skip hits).
# Alpha must stay 1 — higher values showed a grey veil behind the float column.
_HIT_PLATE = QColor(0, 0, 0, 1)


class PublicIconHost(QWidget):
    """Full-desktop translucent overlay that parents PublicIconWidget children."""

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setObjectName("desktidyPublicIconHost")
        self.setWindowFlags(
            Qt.WindowType.FramelessWindowHint | Qt.WindowType.Window
        )
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_QuitOnClose, False)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setMouseTracking(True)
        self.setStyleSheet(
            "#desktidyPublicIconHost { background: transparent; border: none; }"
        )
        self._mask_timer = QTimer(self)
        self._mask_timer.setSingleShot(True)
        self._mask_timer.setInterval(0)
        self._mask_timer.timeout.connect(self.refresh_click_mask)
        self._mask_batch_depth = 0
        self._mask_refresh_deferred = False
        self._selection_anchor: QWidget | None = None
        self._marquee_origin: QPoint | None = None
        self._marquee_additive = False
        self._marquee_band = QRubberBand(QRubberBand.Shape.Rectangle, self)
        self._marquee_band.hide()
        self._icon_cache: list[QWidget] | None = None
        self._marquee_icons: list[QWidget] | None = None
        self._hit_plate_cache: QRegion | None = None
        self._last_mask_region: QRegion | None = None
        self._last_plate_sig: tuple | None = None
        self._painting_plate = False
        self.installEventFilter(self)
        self.sync_desktop_geometry()

    def invalidate_icon_cache(self) -> None:
        self._icon_cache = None
        self._marquee_icons = None
        self._hit_plate_cache = None

    def _plate_content_signature(self) -> tuple:
        """Icon footprints + caption chrome — changes even when the outer mask does not.

        Owned full-desktop masks stay equal across select/height churn; layered
        alpha carve-outs must still repaint or stale ink steals clicks.
        """
        parts: list[tuple[int, ...]] = []
        for child in self.public_icon_widgets():
            try:
                geo = child.geometry()
                rect_fn = getattr(child, "_content_hit_rect", None)
                if callable(rect_fn):
                    r = rect_fn()
                    parts.append(
                        (
                            int(geo.x()),
                            int(geo.y()),
                            int(geo.width()),
                            int(geo.height()),
                            int(r.x()),
                            int(r.y()),
                            int(r.width()),
                            int(r.height()),
                        )
                    )
                else:
                    parts.append(
                        (
                            int(geo.x()),
                            int(geo.y()),
                            int(geo.width()),
                            int(geo.height()),
                        )
                    )
            except RuntimeError:
                continue
        return tuple(parts)
    def sync_desktop_geometry(self) -> None:
        """Cover the virtual desktop union (multi-monitor screen coords)."""
        from src.fence_layout import virtual_desktop_rect

        area = virtual_desktop_rect()
        try:
            geo = QRect(area)
            if self.geometry() != geo:
                self.setGeometry(geo)
                self._last_mask_region = None
                self._last_plate_sig = None
                self._hit_plate_cache = None
        except RuntimeError:
            return

    def _work_area_region_local(self) -> QRegion:
        """Host-local work areas only — never the multi-mon taskbar gap."""
        from src.fence_layout import virtual_desktop_work_region

        try:
            host_tl = self.geometry().topLeft()
            return virtual_desktop_work_region().translated(-host_tl.x(), -host_tl.y())
        except RuntimeError:
            return QRegion()

    def public_icon_widgets(self) -> list[QWidget]:
        cached = self._icon_cache
        if cached is not None:
            return list(cached)
        items: list[QWidget] = []
        # Direct children only — PublicIconWidget is always parented to the host.
        try:
            children = self.children()
        except RuntimeError:
            return []
        for child in children:
            if not isinstance(child, QWidget):
                continue
            if child is self or child.parentWidget() is not self:
                continue
            if child.__class__.__name__ != "PublicIconWidget":
                continue
            try:
                if child.isHidden():
                    continue
            except RuntimeError:
                continue
            # Soft page-park: still Qt-visible off-screen — exclude from hit mask.
            if bool(getattr(child, "_desktidy_soft_parked", False)):
                continue
            items.append(child)
        items.sort(key=lambda w: (int(w.y()), int(w.x())))
        self._icon_cache = items
        return list(items)

    def visible_icon_region(self) -> QRegion:
        """Union of icon geometries that are not explicitly hidden.

        Uses ``isHidden()`` rather than ``isVisible()`` so we can build a mask
        while the host itself is still hidden (Qt hides all descendants).
        """
        region = QRegion()
        for child in self.public_icon_widgets():
            try:
                region = region.united(QRegion(child.geometry()))
            except RuntimeError:
                continue
        return region

    def session_owns_desktop_drops(self) -> bool:
        """True when DeskTidy must receive Explorer folder→desktop OLE (shell icons off)."""
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app else None
        if desk is None:
            return False
        if getattr(desk, "_exiting", False) or getattr(desk, "_icons_hidden", False):
            return False
        settings = getattr(desk, "settings", None)
        if not isinstance(settings, dict):
            return False
        if not bool(settings.get("hide_shell_icons")):
            return False
        # Native icons still visible (user toggled stock view / registry desync):
        # let Explorer DefView own the drop; watcher + loose sync show floats.
        try:
            from src.win_shell import are_desktop_icons_visible

            if are_desktop_icons_visible():
                return False
        except Exception:
            pass
        return True

    def _fence_exclusion_region(self) -> QRegion:
        """Areas owned by live fence overlays; leave them clickable/selectable.

        Skip soft-parked fences (WS_EX_TRANSPARENT / off-page): punching holes for
        them made full-desktop 框选 plates pass clicks to DefView instead of the
        live fence underneath — felt like「选不到分区」near tray / side columns.
        """
        region = QRegion()
        host_geo = self.geometry()
        host_tl = host_geo.topLeft()
        for top in QApplication.topLevelWidgets():
            if top.__class__.__name__ != "FenceWidget":
                continue
            try:
                if not top.isVisible():
                    continue
                if bool(getattr(top, "_desktidy_soft_parked", False)):
                    continue
                # Prefer frameGeometry — includes window frame vs client drift.
                fence_geo = top.frameGeometry()
                tl = QPoint(
                    int(fence_geo.x() - host_tl.x()),
                    int(fence_geo.y() - host_tl.y()),
                )
                # Small pad so border/shadow pixels are not stolen by the plate.
                rect = QRect(tl, fence_geo.size()).adjusted(-4, -4, 4, 4)
                region = region.united(QRegion(rect))
            except RuntimeError:
                continue
        return region

    def _pet_exclusion_region(self) -> QRegion:
        """Desktop pet sprite + chrome must stay mouse-reachable.

        Use the pet's shaped hit mask (or sprite dest), NOT ``frameGeometry``.
        The pet window includes large transparent padding (wait/trash/sleep pads,
        page bubbles). Punching the full frame out of the public host mask made
        floats under that padding undraggable — clicks fell through to DefView
        while shell icons are hidden, so the file looked frozen.
        """
        region = QRegion()
        host_tl = self.geometry().topLeft()
        for top in QApplication.topLevelWidgets():
            if top.__class__.__name__ != "DesktopPetWidget":
                continue
            try:
                if not top.isVisible():
                    continue
                pet_geo = top.frameGeometry()
                tl = QPoint(
                    int(pet_geo.x() - host_tl.x()),
                    int(pet_geo.y() - host_tl.y()),
                )
                shaped = top.mask()
                if not shaped.isNull() and not shaped.isEmpty():
                    region = region.united(shaped.translated(tl))
                    continue
                sprite = getattr(top, "_sprite_dest_rect", None)
                if callable(sprite):
                    rect = sprite().translated(tl).adjusted(-4, -4, 4, 4)
                    region = region.united(QRegion(rect))
                    continue
                rect = QRect(tl, pet_geo.size()).adjusted(-4, -4, 4, 4)
                region = region.united(QRegion(rect))
            except RuntimeError:
                continue
        return region

    def _overlay_exclusion_region(self) -> QRegion:
        """Union of other DefView-band overlays that must keep their own hits."""
        region = self._fence_exclusion_region()
        pet = self._pet_exclusion_region()
        if not pet.isEmpty():
            region = region.united(pet)
        return region

    def _icon_hit_region(self) -> QRegion:
        """Icon footprints / compact cluster — never the full virtual desktop."""
        icons = self.public_icon_widgets()
        union = QRegion()
        padded = QRegion()
        # ~one public float — nearby icons' pads meet for Explorer-like gaps.
        # Prefer a slightly wider corridor so rubber-band can start beside icons
        # (widget rects often overlap each other with little raw host gap).
        pad = 100
        for child in icons:
            try:
                geo = child.geometry()
            except RuntimeError:
                continue
            union = union.united(QRegion(geo))
            padded = padded.united(
                QRegion(geo.adjusted(-pad, -pad, pad, pad))
            )
        if union.isEmpty():
            return union
        bounds = union.boundingRect().adjusted(-16, -16, 16, 16)
        host_area = max(1, self.width() * self.height())
        box_area = max(1, bounds.width() * bounds.height())
        # Compact / medium cluster: one bounding box (full gaps inside the cloud).
        if box_area / host_area <= 0.50:
            region = QRegion(bounds)
        else:
            # Sparse: padded footprints — empty far wallpaper still click-through.
            region = padded if not padded.isEmpty() else union
        excl = self._overlay_exclusion_region()
        if not excl.isEmpty():
            region = region.subtracted(excl)
        work = self._work_area_region_local()
        if not work.isEmpty():
            region = region.intersected(work)
        return region

    def hit_test_region(self) -> QRegion:
        """Mask region for mouse hits.

        Default: icon cluster only — full-screen masking stole Explorer empty
        desktop 右键 / 粘贴 when shell icons were still visible.

        When this session owns the desktop (shell icons hidden) and public
        floats exist, expand to the full host minus fence/pet overlays so
        rubber-band 框选 can start on empty wallpaper (DefView cannot select
        floats) without swallowing the desktop pet.
        Always clip to the multi-mon *work* region so taskbar strips stay
        clickable (bounding availableGeometry union used to cover them).
        """
        if self.session_owns_desktop_drops() and self.has_visible_icons():
            region = QRegion(self.rect())
            work = self._work_area_region_local()
            if not work.isEmpty():
                region = region.intersected(work)
            excl = self._overlay_exclusion_region()
            if not excl.isEmpty():
                region = region.subtracted(excl)
            return region
        return self._icon_hit_region()

    def _hit_plate_region(self) -> QRegion:
        """Alpha≥1 ink for 框选 gaps — never a solid plate under glyphs/fences.

        Layered translucent HWNDs hit-test by pixel alpha more than ``setMask``.
        Painting the whole cluster at alpha=1 made the public host swallow mouse
        over fences stacked underneath. Keep ink only in shelf margins (cluster
        minus each float's glyph+caption), which is where ``childAt`` is empty
        and rubber-band must start.
        """
        cached = self._hit_plate_cache
        if cached is not None:
            return QRegion(cached)
        region = self.hit_test_region()
        if region.isEmpty():
            self._hit_plate_cache = QRegion()
            return region
        for child in self.public_icon_widgets():
            content = getattr(child, "_content_hit_region", None)
            if not callable(content):
                continue
            try:
                local = content()
                if local.isEmpty():
                    continue
                # Child geometry is already in host coords when parented here.
                region = region.subtracted(local.translated(child.pos()))
            except RuntimeError:
                continue
        self._hit_plate_cache = QRegion(region)
        return region

    def paintEvent(self, event) -> None:  # noqa: N802
        """Alpha=1 ink in marquee gaps only — invisible, host-hittable, fence-safe.

        PyQt6/Qt6 ``QRegion`` is not iterable (no ``rects()``). Use ``QPainterPath``.
        Skip redundant full-desktop floods when the mask is unchanged — keepalive
        used to ``update()`` every tick and freeze MainThread in paintEvent.
        """
        if self._painting_plate:
            return
        self._painting_plate = True
        painter = QPainter(self)
        try:
            region = self._hit_plate_region()
            if region.isEmpty():
                return
            clip = region.intersected(QRegion(event.region()))
            if clip.isEmpty():
                return
            from PyQt6.QtGui import QPainterPath

            path = QPainterPath()
            path.addRegion(clip)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(_HIT_PLATE)
            painter.drawPath(path)
        finally:
            if painter.isActive():
                painter.end()
            self._painting_plate = False

    def has_visible_icons(self) -> bool:
        return bool(self.public_icon_widgets())

    def clear_item_selection(self) -> None:
        items = self.public_icon_widgets()
        self._selection_anchor = None
        if items:
            from src.ui.fence_icon_item import public_selection_batch

            with public_selection_batch(items[0]):
                for item in items:
                    setter = getattr(item, "set_selected", None)
                    if callable(setter):
                        setter(False)
        try:
            from src.ui.fence_icon_item import mark_desktop_selection_live

            mark_desktop_selection_live(False)
        except Exception:
            pass

    def _clear_desktop_selections_for_empty_click(self) -> None:
        """Clear public *and* fence icon selection on empty wallpaper click.

        When shell icons are hidden this host owns the full-desktop plate, so
        ``DesktopBlankClickMonitor`` never sees DefView and cannot clear fence
        selection — users saw partition icons stay selected after clicking desk.
        """
        self.clear_item_selection()
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        if desk is None:
            return
        for fence in list(getattr(desk, "fences", []) or []):
            clearer = getattr(fence, "clear_item_selection", None)
            if not callable(clearer):
                continue
            try:
                clearer()
            except RuntimeError:
                continue
            except Exception:
                continue

    def keyPressEvent(self, event) -> None:  # noqa: N802
        from src.ui.fence_icon_item import (
            dispatch_desktop_icon_chord,
            ensure_desktop_icon_key_hook,
        )

        ensure_desktop_icon_key_hook()
        key = event.key()
        mods = event.modifiers()
        ctrl = bool(mods & Qt.KeyboardModifier.ControlModifier)
        if ctrl and key == Qt.Key.Key_A:
            if dispatch_desktop_icon_chord("select_all", self):
                event.accept()
                return
        if ctrl and key == Qt.Key.Key_V:
            if dispatch_desktop_icon_chord("paste", self):
                event.accept()
                return
        if key == Qt.Key.Key_Escape:
            if self._marquee_origin is not None or QWidget.mouseGrabber() is self:
                self._abort_marquee()
                event.accept()
                return
        if key == Qt.Key.Key_Delete and not ctrl:
            anchor = self._selection_anchor
            if anchor is None:
                selected = [
                    w
                    for w in self.public_icon_widgets()
                    if getattr(w, "is_selected", lambda: False)()
                ]
                anchor = selected[0] if selected else None
            if anchor is not None and dispatch_desktop_icon_chord("delete", anchor):
                event.accept()
                return
        super().keyPressEvent(event)

    def begin_mask_batch(self) -> None:
        """Coalesce setMask during page switch (each move used to flash the desktop)."""
        self._mask_batch_depth += 1

    def end_mask_batch(self) -> None:
        self._mask_batch_depth = max(0, self._mask_batch_depth - 1)
        if self._mask_batch_depth == 0 and self._mask_refresh_deferred:
            self._mask_refresh_deferred = False
            self._mask_timer.stop()
            self.refresh_click_mask()

    def schedule_mask_refresh(self) -> None:
        self.invalidate_icon_cache()
        if self._mask_batch_depth > 0:
            self._mask_refresh_deferred = True
            return
        if not self._mask_timer.isActive():
            self._mask_timer.start()

    def _park_mask_region(self) -> QRegion:
        """Tiny off-screen hit box — keeps HWND mapped without stealing clicks."""
        from src.desktop_shell_host import OFFSCREEN_PARK_POS

        return QRegion(
            QRect(int(OFFSCREEN_PARK_POS[0]), int(OFFSCREEN_PARK_POS[1]), 1, 1)
        )

    def refresh_click_mask(self) -> None:
        """Limit hit-testing so Explorer still receives empty-desktop clicks."""
        try:
            owns = self.session_owns_desktop_drops()
            self.setAcceptDrops(owns)
            region = self.hit_test_region()
            if region.isEmpty():
                self.invalidate_icon_cache()
                # Empty setMask() can invalidate the HWND; keep a 1×1 park mask.
                park = self._park_mask_region()
                self.setMask(park)
                self._last_mask_region = QRegion(park)
                self._last_plate_sig = ()
                self.update(park.boundingRect())
                return
            plate_sig = self._plate_content_signature()
            # Keepalive / ensure_live used to setMask+update every tick; repainting
            # a full-desktop alpha plate froze the UI (MainThread stuck in paintEvent).
            last = self._last_mask_region
            mask_same = last is not None and last == region
            plate_same = (
                self._last_plate_sig is not None and self._last_plate_sig == plate_sig
            )
            if mask_same and plate_same:
                return
            if not mask_same:
                self.invalidate_icon_cache()
                region = self.hit_test_region()
                if region.isEmpty():
                    park = self._park_mask_region()
                    self.setMask(park)
                    self._last_mask_region = QRegion(park)
                    self._last_plate_sig = ()
                    return
                self.setMask(region)
                self._last_mask_region = QRegion(region)
            # Mask may be unchanged while caption chrome / height moved — still
            # rebuild layered alpha carve-outs (Win32 hits by alpha more than mask).
            self._hit_plate_cache = None
            self._last_plate_sig = plate_sig
            plate = self._hit_plate_region()
            if plate.isEmpty():
                return
            self.update(plate.boundingRect())
        except RuntimeError:
            return

    def _accept_external_file_drop(self, event) -> bool:
        """Explorer folder→desktop when shell icons are hidden."""
        if not self.session_owns_desktop_drops():
            return False
        mime = event.mimeData()
        if mime is None or mime.hasFormat(DESKTIDY_VIRTUAL_MIME):
            return False
        if not mime_has_droppable_items(mime):
            return False
        from src.settings import get_desktop_path

        action = preferred_drop_action_for_mime(mime, target_dir=get_desktop_path())
        event.setDropAction(action)
        event.accept()
        return True

    def dragEnterEvent(self, event) -> None:  # noqa: N802
        if not self._accept_external_file_drop(event):
            event.ignore()

    def dragMoveEvent(self, event) -> None:  # noqa: N802
        if not self._accept_external_file_drop(event):
            event.ignore()

    def dropEvent(self, event) -> None:  # noqa: N802
        mime = event.mimeData()
        if mime is None or mime.hasFormat(DESKTIDY_VIRTUAL_MIME):
            event.ignore()
            return
        if not self.session_owns_desktop_drops() or not mime_has_droppable_items(mime):
            event.ignore()
            return
        from src.ui.fence_icon_item import drop_files_to_public_from_mime

        if drop_files_to_public_from_mime(mime, drop_action=event.dropAction()):
            from src.settings import get_desktop_path

            event.setDropAction(
                preferred_drop_action_for_mime(mime, target_dir=get_desktop_path())
            )
            event.accept()
            return
        event.ignore()

    def present(
        self, *, force_attach: bool = False, mask_only: bool = False
    ) -> None:
        """Show host with a non-empty mask, then attach to DefView."""
        if mask_only:
            self.refresh_click_mask()
            return
        try:
            self.sync_desktop_geometry()
            self.refresh_click_mask()
            if not self.isVisible():
                self.show()
            self.ensure_shell_attached(force=force_attach)
        except RuntimeError:
            return

    def ensure_shell_attached(self, *, force: bool = False) -> None:
        """Attach this single HWND to the DefView host (never attach children)."""
        try:
            owns_drops = self.session_owns_desktop_drops()
            if not self.has_visible_icons() and not owns_drops:
                self.refresh_click_mask()
                return
            if not self.isVisible():
                region = self.hit_test_region()
                if region.isEmpty():
                    region = self._park_mask_region()
                self.setMask(region)
                self.show()
            from src.desktop_shell_host import (
                is_attached_to_desktop,
                set_overlay_hwnd_visible,
            )
            from src.win_shell import overlay_win32_visible

            hwnd = int(self.winId()) if self.winId() else 0
            if (
                not force
                and hwnd
                and is_attached_to_desktop(hwnd)
            ):
                if self.isVisible() and not overlay_win32_visible(self):
                    set_overlay_hwnd_visible(hwnd, True)
                self.schedule_mask_refresh()
                return
            configure_desktop_overlay(self)
            self.schedule_mask_refresh()
        except RuntimeError:
            return
        except Exception:
            try:
                configure_desktop_overlay(self)
            except Exception:
                pass

    def showEvent(self, event) -> None:  # noqa: N802
        super().showEvent(event)
        self.sync_desktop_geometry()
        try:
            from src.desktop_shell_host import (
                is_attached_to_desktop,
                set_overlay_hwnd_visible,
            )
            from src.win_shell import overlay_win32_visible

            hwnd = int(self.winId()) if self.winId() else 0
            if hwnd and is_attached_to_desktop(hwnd):
                if not overlay_win32_visible(self):
                    set_overlay_hwnd_visible(hwnd, True)
                self.schedule_mask_refresh()
                return
        except Exception:
            pass
        try:
            configure_desktop_overlay(self)
        except Exception:
            pass
        self.schedule_mask_refresh()

    def map_screen_to_host(self, x: int, y: int) -> QPoint:
        return self.mapFromGlobal(QPoint(int(x), int(y)))

    def map_host_to_screen(self, local: QPoint) -> QPoint:
        return self.mapToGlobal(local)

    def _hit_public_icon_at(self, local: QPoint) -> bool:
        """True when *local* lands on a float glyph/caption (respects child masks)."""
        for item in self.public_icon_widgets():
            try:
                if not item.isVisible():
                    continue
                geom = item.geometry()
                if not geom.contains(local):
                    continue
                rel = QPoint(local.x() - geom.x(), local.y() - geom.y())
                region = item.mask()
                if region.isNull() or region.isEmpty() or region.contains(rel):
                    return True
            except RuntimeError:
                continue
        return False

    def nativeEvent(self, eventType, message):  # noqa: N802
        """Empty-plate RMB hit-test: never punch through an owned (icons-hidden) plate.

        With ``hide_shell_icons``, SysListView32 is invisible — ``HTTRANSPARENT``
        delivers RMB to a dead layer and no menu appears. Keep HTCLIENT so Qt
        gets the click and ``eventFilter`` forwards ``WM_CONTEXTMENU`` to DefView.

        When shell icons are visible, empty gaps may still ``HTTRANSPARENT`` to
        live DefView while VK_RBUTTON is down.

        Do not forward to QWidget.nativeEvent — on this PyQt6/sip build that
        path crashes when ``message`` is a ``voidptr``. Return ``(False, 0)``
        so Qt keeps the default hit-test handling.
        """
        try:
            try:
                et = (
                    bytes(eventType)
                    if not isinstance(eventType, (bytes, bytearray))
                    else eventType
                )
            except Exception:
                return False, 0
            if et != b"windows_generic_MSG":
                return False, 0
            try:
                addr = int(message)
            except Exception:
                return False, 0
            if not addr:
                return False, 0
            try:
                msg = wintypes.MSG.from_address(addr)
            except Exception:
                return False, 0
            if int(msg.message) != _WM_NCHITTEST:
                return False, 0
            # Owned plate: ListView hidden — pass-through cannot open a menu.
            if self.session_owns_desktop_drops():
                return False, 0
            # Only pass through while the right button is down so LMB 框选 still hits us.
            if not (ctypes.windll.user32.GetAsyncKeyState(_VK_RBUTTON) & 0x8000):
                return False, 0
            try:
                gx = ctypes.c_short(msg.lParam & 0xFFFF).value
                gy = ctypes.c_short((msg.lParam >> 16) & 0xFFFF).value
                local = self.mapFromGlobal(QPoint(int(gx), int(gy)))
            except Exception:
                return False, 0
            if self._hit_public_icon_at(local):
                return True, _HTCLIENT
            return True, _HTTRANSPARENT
        except Exception:
            # Never raise out of a Win32 callback (STATUS_FATAL_USER_CALLBACK_EXCEPTION).
            return False, 0

    def _forward_empty_plate_rmb(self, global_pos: QPoint) -> None:
        """Owned-plate empty RMB → DefView menu; freeze until the menu dismisses."""
        from src.desktop_context import set_last_desktop_right_click
        from src.win_shell import request_desktop_background_menu

        x, y = int(global_pos.x()), int(global_pos.y())
        set_last_desktop_right_click(x, y)
        app = QApplication.instance()
        desk = getattr(app, "_desktidy_app", None) if app is not None else None
        # Same freeze as LL-hook desktop RMB — polls until menu closes.
        # Do NOT release popup freeze on a short timer: PostMessage is async; a
        # quick heal/restack dismisses the menu before it appears.
        arm = getattr(desk, "_arm_explorer_menu_freeze", None) if desk is not None else None
        if callable(arm):
            arm()
        else:
            begin = getattr(desk, "_begin_desktop_popup", None) if desk is not None else None
            if callable(begin):
                begin()
        snap = getattr(desk, "_snapshot_explorer_defview_host", None) if desk else None
        if callable(snap):
            snap()
        request_desktop_background_menu(x, y)

    def _abort_marquee(self) -> None:
        """Drop rubber-band + mouse grab so leftovers cannot steal all clicks.

        Without this, an unfinished 框选 (page switch / hide / OLE) leaves
        ``grabMouse`` active — desktop files and app windows ignore input until
        something remounts the host (e.g. virtual-desktop switch).
        """
        self._marquee_origin = None
        self._marquee_icons = None
        band = getattr(self, "_marquee_band", None)
        if band is not None:
            try:
                band.hide()
            except RuntimeError:
                pass
        try:
            if QWidget.mouseGrabber() is self:
                self.releaseMouse()
        except RuntimeError:
            pass

    def _begin_marquee(self, pos: QPoint, *, additive: bool) -> None:
        self._marquee_origin = QPoint(pos)
        self._marquee_additive = bool(additive)
        if not additive:
            # Owned full-desktop plate: this is the Explorer empty-click path.
            self._clear_desktop_selections_for_empty_click()
        self._marquee_icons = self.public_icon_widgets()
        self._marquee_band.setGeometry(QRect(pos, pos))
        self._marquee_band.show()
        self._marquee_band.raise_()
        # Keep MouseMove/Release after the cursor leaves the click mask.
        try:
            self.grabMouse()
        except RuntimeError:
            pass

    def _update_marquee(self, pos: QPoint) -> None:
        origin = self._marquee_origin
        if origin is None:
            return
        rect = QRect(origin, pos).normalized()
        self._marquee_band.setGeometry(rect)
        additive = bool(self._marquee_additive)
        icons = self._marquee_icons or self.public_icon_widgets()
        from src.ui.fence_icon_item import public_selection_batch

        with public_selection_batch(icons[0] if icons else None):
            for item in icons:
                setter = getattr(item, "set_selected", None)
                if not callable(setter):
                    continue
                try:
                    hit = rect.intersects(item.geometry())
                except RuntimeError:
                    continue
                if additive:
                    if hit:
                        setter(True)
                else:
                    setter(hit)

    def _finish_marquee(self, pos: QPoint) -> None:
        origin = self._marquee_origin
        additive = bool(self._marquee_additive)
        if origin is None:
            self._abort_marquee()
            return
        # Apply final selection while grab is still held, then always abort.
        tiny = (pos - origin).manhattanLength() < 4 and not additive
        if not tiny:
            self._update_marquee(pos)
        self._abort_marquee()
        if tiny:
            # Tiny drag = clear (non-additive) like Explorer empty click.
            self._clear_desktop_selections_for_empty_click()
            return
        try:
            from src.ui.fence_icon_item import mark_desktop_selection_live

            mark_desktop_selection_live()
        except Exception:
            pass

    def hideEvent(self, event) -> None:  # noqa: N802
        self._abort_marquee()
        super().hideEvent(event)

    def eventFilter(self, obj: QObject, event: QEvent) -> bool:  # noqa: N802
        if obj is not self:
            return super().eventFilter(obj, event)
        et = event.type()
        if et == QEvent.Type.MouseButtonPress:
            pos = event.position().toPoint()  # type: ignore[attr-defined]
            child = self.childAt(pos)
            if event.button() == Qt.MouseButton.RightButton:  # type: ignore[attr-defined]
                # Owned plate: keep the click (nativeEvent skips HTTRANSPARENT)
                # and forward WM_CONTEXTMENU to DefView — never thin CreateViewObject.
                if child is None or child is self._marquee_band:
                    self._forward_empty_plate_rmb(self.mapToGlobal(pos))
                    return True
            if event.button() == Qt.MouseButton.LeftButton:  # type: ignore[attr-defined]
                # Empty host area only — icons handle their own presses.
                # PublicIconWidget uses a content setMask so shelf margins are
                # not childAt hits and rubber-band can start between floats.
                if child is None or child is self._marquee_band:
                    additive = bool(
                        event.modifiers() & Qt.KeyboardModifier.ControlModifier  # type: ignore[attr-defined]
                    )
                    self._begin_marquee(pos, additive=additive)
                    return True
        elif et == QEvent.Type.MouseMove:
            if self._marquee_origin is not None and (
                event.buttons() & Qt.MouseButton.LeftButton  # type: ignore[attr-defined]
            ):
                self._update_marquee(event.position().toPoint())  # type: ignore[attr-defined]
                return True
        elif et == QEvent.Type.MouseButtonRelease:
            if (
                self._marquee_origin is not None
                and event.button() == Qt.MouseButton.LeftButton  # type: ignore[attr-defined]
            ):
                self._finish_marquee(event.position().toPoint())  # type: ignore[attr-defined]
                return True
        return super().eventFilter(obj, event)
