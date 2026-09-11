"""Schematic preview images for DeskTidy layout snapshots."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from PyQt6.QtCore import QFileInfo, QRectF, Qt
from PyQt6.QtGui import QColor, QFont, QPainter, QPen, QPixmap
from PyQt6.QtWidgets import QFileIconProvider


def preview_path_for(snapshot_json: Path) -> Path:
    return snapshot_json.with_suffix(".png")


def _parse_color(raw: object, fallback: str) -> QColor:
    text = str(raw or "").strip() or fallback
    color = QColor(text)
    if not color.isValid():
        color = QColor(fallback)
    return color


def _fence_on_page(fence: dict, page_id: int) -> bool:
    pages = fence.get("pages")
    if isinstance(pages, list) and pages:
        try:
            return int(page_id) in {int(p) for p in pages}
        except (TypeError, ValueError):
            return True
    try:
        return int(fence.get("page", 0)) == int(page_id)
    except (TypeError, ValueError):
        return True


def _geom_from_layouts(layout: dict, fence_id: str, page_id: int) -> dict | None:
    """Prefer display-keyed layout geometry when present."""
    by_display = layout.get("fence_layouts_by_display")
    if isinstance(by_display, dict):
        # Use the newest / any profile that has this fence on the page.
        for _fp, pages in by_display.items():
            if not isinstance(pages, dict):
                continue
            page_map = pages.get(str(page_id)) or pages.get(page_id)
            if not isinstance(page_map, dict):
                continue
            geom = page_map.get(fence_id)
            if isinstance(geom, dict) and "x" in geom and "y" in geom:
                return geom
    by_page = layout.get("fence_layouts_by_page")
    if isinstance(by_page, dict):
        page_map = by_page.get(str(page_id)) or by_page.get(page_id)
        if isinstance(page_map, dict):
            geom = page_map.get(fence_id)
            if isinstance(geom, dict) and "x" in geom and "y" in geom:
                return geom
    return None


def _paths_from_virtual_items(raw: object) -> list[str]:
    if not isinstance(raw, list):
        return []
    paths: list[str] = []
    for entry in raw:
        if isinstance(entry, str) and entry.strip():
            paths.append(entry.strip())
        elif isinstance(entry, dict):
            path = str(entry.get("path") or "").strip()
            if path:
                paths.append(path)
    return paths


def layout_pin_count(layout: dict | None) -> int:
    """Count fence pins + public floats in a snapshot layout (not shell icons)."""
    if not isinstance(layout, dict):
        return 0
    n = 0
    fences = layout.get("fences")
    if isinstance(fences, list):
        for fence in fences:
            if isinstance(fence, dict):
                n += len(_paths_from_virtual_items(fence.get("virtual_items")))
    public = layout.get("public_desktop_items")
    if isinstance(public, list):
        for entry in public:
            if isinstance(entry, dict) and str(entry.get("path") or "").strip():
                n += 1
    return n


def preview_cache_needs_refresh(snapshot_json: Path, layout: dict | None) -> bool:
    """True when cached PNG is missing or a tiny pre-icon schematic.

    Early saves wrote empty chrome (~6KB) while the JSON already had pins.
    Real icon previews are typically much larger.
    """
    if not isinstance(layout, dict) or not layout:
        return False
    if layout_pin_count(layout) <= 0:
        return False
    png = preview_path_for(snapshot_json)
    if not png.is_file():
        return True
    try:
        return int(png.stat().st_size) < 14_000
    except OSError:
        return True


def _label_for_path(path: str) -> str:
    name = Path(path).name or path
    # Strip .lnk for cleaner labels in the tiny preview.
    if name.lower().endswith(".lnk"):
        name = name[:-4]
    return name


def _page_ids_and_names(layout: dict) -> list[tuple[int, str]]:
    """Return ordered (page_id, page_name) for every desktop page in the layout."""
    pages = layout.get("desktop_pages")
    out: list[tuple[int, str]] = []
    if isinstance(pages, list):
        for page in pages:
            if not isinstance(page, dict):
                continue
            try:
                pid = int(page.get("id", 0))
            except (TypeError, ValueError):
                continue
            name = str(page.get("name") or f"分页 {pid}")
            out.append((pid, name))
    if out:
        return out
    # Fallback: current page only, or page 0.
    try:
        cur = int(layout.get("current_page", 0))
    except (TypeError, ValueError):
        cur = 0
    return [(cur, "布局预览")]


def collect_preview_fences(layout: dict, page_id: int | None = None) -> list[dict[str, Any]]:
    """Normalize fence rectangles (+ icon paths) for one page.

    When ``page_id`` is omitted, uses ``layout['current_page']`` (legacy single-page).
    """
    if page_id is None:
        try:
            page_id = int(layout.get("current_page", 0))
        except (TypeError, ValueError):
            page_id = 0
    out: list[dict[str, Any]] = []
    fences = layout.get("fences")
    if not isinstance(fences, list):
        return out
    for fence in fences:
        if not isinstance(fence, dict):
            continue
        if not fence.get("visible", True):
            continue
        if not _fence_on_page(fence, page_id):
            continue
        fence_id = str(fence.get("id") or "")
        geom = _geom_from_layouts(layout, fence_id, page_id) if fence_id else None
        if geom is None:
            geom = fence
        try:
            x = int(geom.get("x", fence.get("x", 40)))
            y = int(geom.get("y", fence.get("y", 40)))
            w = max(40, int(geom.get("width", fence.get("width", 220))))
            h = max(40, int(geom.get("height", fence.get("height", 280))))
        except (TypeError, ValueError):
            continue
        style = fence.get("style") if isinstance(fence.get("style"), dict) else {}
        icon_paths = _paths_from_virtual_items(fence.get("virtual_items"))
        out.append(
            {
                "name": str(fence.get("name") or fence_id or "分区"),
                "x": x,
                "y": y,
                "width": w,
                "height": h,
                "collapsed": bool(fence.get("collapsed")),
                "background": _parse_color(style.get("background"), "#FFFFFF"),
                "accent": _parse_color(style.get("accent"), "#3B82F6"),
                "icons": icon_paths,
            }
        )
    # Public / page-local floats as small icon markers.
    public = layout.get("public_desktop_items")
    if isinstance(public, list):
        for entry in public:
            if not isinstance(entry, dict) or not entry.get("path"):
                continue
            try:
                if "page" in entry and int(entry.get("page")) != page_id:
                    continue
            except (TypeError, ValueError):
                pass
            try:
                x = int(entry.get("x", 0))
                y = int(entry.get("y", 0))
            except (TypeError, ValueError):
                continue
            path = str(entry.get("path") or "")
            out.append(
                {
                    "name": _label_for_path(path),
                    "x": x,
                    "y": y,
                    "width": 48,
                    "height": 56,
                    "collapsed": False,
                    "background": QColor("#EFF6FF"),
                    "accent": QColor("#2563EB"),
                    "float": True,
                    "icons": [path] if path else [],
                }
            )
    return out


def collect_preview_pages(layout: dict) -> list[dict[str, Any]]:
    """Collect preview items for every desktop page."""
    pages: list[dict[str, Any]] = []
    for page_id, page_name in _page_ids_and_names(layout):
        pages.append(
            {
                "id": page_id,
                "name": page_name,
                "items": collect_preview_fences(layout, page_id),
            }
        )
    return pages


_icon_provider: QFileIconProvider | None = None
_icon_pix_cache: dict[tuple[str, int], QPixmap] = {}


def _file_icon_pixmap(path: str, size: int) -> QPixmap | None:
    """Best-effort shell icon; returns None when unavailable."""
    global _icon_provider
    key = (path.casefold(), int(size))
    cached = _icon_pix_cache.get(key)
    if cached is not None:
        return None if cached.isNull() else cached
    try:
        if _icon_provider is None:
            _icon_provider = QFileIconProvider()
        info = QFileInfo(path)
        icon = _icon_provider.icon(info)
        if icon.isNull():
            _icon_pix_cache[key] = QPixmap()
            return None
        pix = icon.pixmap(size, size)
        if pix.isNull():
            _icon_pix_cache[key] = QPixmap()
            return None
        _icon_pix_cache[key] = pix
        return pix
    except Exception:
        _icon_pix_cache[key] = QPixmap()
        return None


def _draw_icon_grid(
    painter: QPainter,
    body: QRectF,
    paths: list[str],
    *,
    accent: QColor,
    min_cell: float = 22.0,
) -> None:
    """Draw a compact icon+label grid inside a fence body rectangle."""
    if body.width() < 16 or body.height() < 16 or not paths:
        return

    # Prefer readable icons; only shrink when the body is truly tiny.
    cell = max(min_cell, min(40.0, min(body.width(), body.height()) / 2.4))
    if body.height() < cell + 2:
        cell = max(16.0, body.height() - 2.0)
    gap = max(3.0, cell * 0.14)
    cols = max(1, int((body.width() + gap) // (cell + gap)))
    rows_fit = max(1, int((body.height() + gap) // (cell + gap)))
    capacity = max(1, cols * rows_fit)
    shown = paths[:capacity]
    overflow = len(paths) - len(shown)

    icon_box = max(12.0, cell * 0.62)
    label_h = max(0.0, cell - icon_box - 2.0)
    show_labels = label_h >= 8.0 and cell >= 24.0

    painter.setFont(QFont("Microsoft YaHei UI", 6))
    for idx, path in enumerate(shown):
        row, col = divmod(idx, cols)
        x = body.x() + col * (cell + gap)
        y = body.y() + row * (cell + gap)
        plate = QRectF(x + (cell - icon_box) / 2.0, y + 1.0, icon_box, icon_box)
        pix = _file_icon_pixmap(path, max(16, int(round(icon_box))))
        if pix is not None:
            target = plate.toRect()
            painter.drawPixmap(
                target,
                pix.scaled(
                    target.size(),
                    Qt.AspectRatioMode.KeepAspectRatio,
                    Qt.TransformationMode.SmoothTransformation,
                ),
            )
        else:
            painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 90))
            painter.setPen(QPen(QColor(accent.red(), accent.green(), accent.blue(), 180), 1.0))
            painter.drawRoundedRect(plate, 3.0, 3.0)
            # Fallback glyph so empty plates are still "an icon".
            painter.setPen(QColor("#334155"))
            painter.setFont(QFont("Microsoft YaHei UI", 7, QFont.Weight.Bold))
            painter.drawText(plate, int(Qt.AlignmentFlag.AlignCenter), _label_for_path(path)[:1] or "·")

        if show_labels:
            label = _label_for_path(path)
            text_rect = QRectF(x, y + icon_box + 1.0, cell, label_h)
            painter.setPen(QColor("#1E293B"))
            painter.setFont(QFont("Microsoft YaHei UI", 6))
            painter.drawText(
                text_rect,
                int(
                    Qt.AlignmentFlag.AlignHCenter
                    | Qt.AlignmentFlag.AlignTop
                    | Qt.TextFlag.TextSingleLine
                ),
                label,
            )

    if overflow > 0:
        badge = QRectF(body.right() - 34, body.bottom() - 16, 32, 14)
        painter.setBrush(QColor(15, 23, 42, 200))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.drawRoundedRect(badge, 4.0, 4.0)
        painter.setPen(QColor("#F8FAFC"))
        painter.setFont(QFont("Microsoft YaHei UI", 7, QFont.Weight.DemiBold))
        painter.drawText(badge, int(Qt.AlignmentFlag.AlignCenter), f"+{overflow}")


def _draw_page_items_stacked(painter: QPainter, desk: QRectF, items: list[dict[str, Any]]) -> None:
    """Card stack: each fence gets a readable icon strip (used when geo scale is too small)."""
    fences = [i for i in items if not i.get("float")]
    floats = [i for i in items if i.get("float")]
    if not fences and not floats:
        painter.setPen(QColor("#8B949E"))
        painter.setFont(QFont("Microsoft YaHei UI", 8))
        painter.drawText(desk, int(Qt.AlignmentFlag.AlignCenter), "无分区布局")
        return

    gap = 6.0
    n = max(1, len(fences))
    band_h = max(56.0, (desk.height() - gap * (n - 1)) / n)
    # Prefer slightly taller bands when few fences so icons are clear.
    if n <= 2:
        band_h = max(band_h, min(96.0, desk.height() * 0.42))
    y = desk.y()
    for item in fences:
        rect = QRectF(desk.x(), y, desk.width(), min(band_h, desk.bottom() - y))
        if rect.height() < 36:
            break
        bg = QColor(item["background"])
        bg.setAlpha(235)
        painter.setBrush(bg)
        painter.setPen(QPen(item["accent"], 1.4))
        painter.drawRoundedRect(rect, 6.0, 6.0)

        header_h = 18.0
        header = QRectF(rect.x(), rect.y(), rect.width(), header_h)
        accent = QColor(item["accent"])
        accent.setAlpha(40)
        painter.fillRect(header, accent)
        painter.setPen(QColor("#111827") if bg.lightness() >= 140 else QColor("#F8FAFC"))
        painter.setFont(QFont("Microsoft YaHei UI", 8, QFont.Weight.DemiBold))
        count = len(item.get("icons") or [])
        title = f"{item.get('name') or '分区'}  ·  {count} 项"
        painter.drawText(
            header.adjusted(6, 0, -6, 0),
            int(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft),
            title,
        )

        icons = item.get("icons") or []
        if item.get("collapsed"):
            painter.setPen(QColor("#64748B"))
            painter.setFont(QFont("Microsoft YaHei UI", 7))
            painter.drawText(
                rect.adjusted(6, header_h, -6, -4),
                int(Qt.AlignmentFlag.AlignCenter),
                "已折叠",
            )
        elif icons:
            body = rect.adjusted(6, header_h + 4, -6, -6)
            _draw_icon_grid(painter, body, icons, accent=item["accent"], min_cell=24.0)
        else:
            painter.setPen(QColor("#94A3B8"))
            painter.setFont(QFont("Microsoft YaHei UI", 7))
            painter.drawText(
                rect.adjusted(4, header_h + 2, -4, -4),
                int(Qt.AlignmentFlag.AlignCenter),
                "空分区",
            )
        y = rect.bottom() + gap

    # Page-local floats as a compact icon strip at the bottom if space remains.
    if floats and desk.bottom() - y >= 28:
        strip = QRectF(desk.x(), y, desk.width(), desk.bottom() - y)
        paths = []
        for fl in floats:
            paths.extend(fl.get("icons") or [])
        if paths:
            _draw_icon_grid(
                painter,
                strip.adjusted(2, 2, -2, -2),
                paths,
                accent=QColor("#2563EB"),
                min_cell=20.0,
            )


def _draw_page_items(painter: QPainter, desk: QRectF, items: list[dict[str, Any]]) -> None:
    """Draw fences/floats of one page into ``desk``."""
    if not items:
        painter.setPen(QColor("#8B949E"))
        painter.setFont(QFont("Microsoft YaHei UI", 8))
        painter.drawText(desk, int(Qt.AlignmentFlag.AlignCenter), "无分区布局")
        return

    min_x = min(i["x"] for i in items)
    min_y = min(i["y"] for i in items)
    max_x = max(i["x"] + i["width"] for i in items)
    max_y = max(i["y"] + i["height"] for i in items)
    span_w = max(1, max_x - min_x)
    span_h = max(1, max_y - min_y)
    pad = 0.12
    min_x -= span_w * pad
    min_y -= span_h * pad
    span_w *= 1 + 2 * pad
    span_h *= 1 + 2 * pad
    scale = min(desk.width() / span_w, desk.height() / span_h)

    # Wide short desktop fences crush to ~30px tall — icons vanish. Use stacked cards.
    icon_fences = [
        i for i in items if not i.get("float") and (i.get("icons") or []) and not i.get("collapsed")
    ]
    if icon_fences:
        min_mapped_h = min(max(6.0, i["height"] * scale) for i in icon_fences)
        if min_mapped_h < 64.0:
            _draw_page_items_stacked(painter, desk, items)
            return

    def _map_rect(x: int, y: int, w: int, h: int) -> QRectF:
        mx = desk.x() + (x - min_x) * scale
        my = desk.y() + (y - min_y) * scale
        return QRectF(mx, my, max(6.0, w * scale), max(6.0, h * scale))

    for item in items:
        rect = _map_rect(item["x"], item["y"], item["width"], item["height"])
        # Guarantee a readable body when icons are present.
        if (item.get("icons") or []) and not item.get("float") and not item.get("collapsed"):
            if rect.height() < 72:
                rect.setHeight(min(72.0, desk.height() * 0.55))
            if rect.width() < 110:
                rect.setWidth(min(110.0, desk.width() * 0.55))
            if rect.bottom() > desk.bottom():
                rect.moveTop(max(desk.y(), desk.bottom() - rect.height()))
            if rect.right() > desk.right():
                rect.moveLeft(max(desk.x(), desk.right() - rect.width()))

        bg = QColor(item["background"])
        bg.setAlpha(230 if not item.get("float") else 235)
        painter.setBrush(bg)
        painter.setPen(QPen(item["accent"], 1.5))
        radius = 4.0 if item.get("float") else 6.0
        painter.drawRoundedRect(rect, radius, radius)

        name = item.get("name") or ""
        header_h = 0.0
        if name and not item.get("float") and rect.height() >= 22:
            header_h = min(18.0, max(12.0, rect.height() * 0.14))
            header = QRectF(rect.x(), rect.y(), rect.width(), header_h)
            accent = QColor(item["accent"])
            accent.setAlpha(36)
            painter.fillRect(header, accent)
            painter.setPen(QColor("#111827") if bg.lightness() >= 140 else QColor("#F8FAFC"))
            painter.setFont(QFont("Microsoft YaHei UI", 8, QFont.Weight.Medium))
            painter.drawText(
                header.adjusted(5, 0, -5, 0),
                int(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft),
                name,
            )

        icons = item.get("icons") or []
        if item.get("collapsed"):
            continue
        if item.get("float"):
            if icons:
                _draw_icon_grid(
                    painter,
                    rect.adjusted(3, 3, -3, -3),
                    icons[:1],
                    accent=item["accent"],
                    min_cell=18.0,
                )
            continue
        if icons and rect.height() > header_h + 20:
            body = rect.adjusted(5, header_h + 4, -5, -5)
            _draw_icon_grid(painter, body, icons, accent=item["accent"], min_cell=22.0)
        elif not icons and rect.width() >= 40 and rect.height() >= 28:
            painter.setPen(QColor("#94A3B8"))
            painter.setFont(QFont("Microsoft YaHei UI", 7))
            painter.drawText(
                rect.adjusted(4, header_h + 2, -4, -4),
                int(Qt.AlignmentFlag.AlignCenter),
                "空分区",
            )


def _grid_shape(count: int) -> tuple[int, int]:
    """Choose columns/rows for tiling ``count`` page panels."""
    if count <= 1:
        return 1, 1
    if count == 2:
        return 2, 1
    if count <= 4:
        return 2, 2
    if count <= 6:
        return 3, 2
    cols = min(4, count)
    rows = (count + cols - 1) // cols
    return cols, rows


def preview_canvas_size(page_count: int) -> tuple[int, int]:
    """Hover-friendly canvas size that grows modestly with page count."""
    cols, rows = _grid_shape(max(1, int(page_count)))
    # Base cell ~280x200; keep overall compact for a tooltip.
    width = max(480, min(760, 24 + cols * 280 + (cols - 1) * 8))
    height = max(270, min(520, 36 + rows * 200 + (rows - 1) * 8))
    return width, height


def render_layout_preview(
    layout: dict,
    *,
    width: int | None = None,
    height: int | None = None,
) -> QPixmap:
    """Draw a schematic preview of every desktop page (tiled when multiple)."""
    pages = collect_preview_pages(layout)
    auto_w, auto_h = preview_canvas_size(len(pages))
    width = max(160, int(width if width is not None else auto_w))
    height = max(120, int(height if height is not None else auto_h))
    pix = QPixmap(width, height)
    pix.fill(QColor("#1B1F24"))

    painter = QPainter(pix)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, True)

    title = "布局预览"
    if len(pages) == 1:
        title = pages[0]["name"] or title
    else:
        title = f"布局预览 · {len(pages)} 页"

    painter.fillRect(0, 0, width, 28, QColor("#111418"))
    painter.setPen(QColor("#E6EDF3"))
    painter.setFont(QFont("Microsoft YaHei UI", 9, QFont.Weight.Medium))
    painter.drawText(10, 0, width - 20, 28, int(Qt.AlignmentFlag.AlignVCenter), title)

    if not pages:
        painter.setPen(QColor("#8B949E"))
        painter.drawText(0, 28, width, height - 28, int(Qt.AlignmentFlag.AlignCenter), "无分页")
        painter.end()
        return pix

    cols, rows = _grid_shape(len(pages))
    gap = 8.0
    area = QRectF(8, 36, width - 16, height - 44)
    cell_w = (area.width() - gap * (cols - 1)) / cols
    cell_h = (area.height() - gap * (rows - 1)) / rows

    for idx, page in enumerate(pages):
        row, col = divmod(idx, cols)
        panel = QRectF(
            area.x() + col * (cell_w + gap),
            area.y() + row * (cell_h + gap),
            cell_w,
            cell_h,
        )
        # Page chrome.
        painter.setBrush(QColor("#1A2330"))
        painter.setPen(QPen(QColor("#3D4F66"), 1))
        painter.drawRoundedRect(panel, 6.0, 6.0)

        header_h = 22.0 if panel.height() >= 60 else 16.0
        header = QRectF(panel.x(), panel.y(), panel.width(), header_h)
        painter.fillRect(header.adjusted(1, 1, -1, 0), QColor("#111418"))
        painter.setPen(QColor("#E6EDF3"))
        painter.setFont(QFont("Microsoft YaHei UI", 8, QFont.Weight.Medium))
        painter.drawText(
            header.adjusted(8, 0, -8, 0),
            int(Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft),
            str(page.get("name") or f"分页 {page.get('id')}"),
        )

        desk = panel.adjusted(6, header_h + 4, -6, -6)
        painter.fillRect(desk, QColor("#243041"))
        painter.setPen(QPen(QColor("#3D4F66"), 1))
        painter.drawRect(desk)
        _draw_page_items(painter, desk, page.get("items") or [])

    painter.end()
    return pix


def ensure_snapshot_preview(snapshot_json: Path, layout: dict) -> Path | None:
    """Write/update ``*.png`` beside the snapshot JSON. Returns png path."""
    if not layout:
        return None
    png = preview_path_for(snapshot_json)
    try:
        pix = render_layout_preview(layout)
        if pix.isNull():
            return None
        png.parent.mkdir(parents=True, exist_ok=True)
        pix.save(str(png), "PNG")
        return png
    except Exception:
        return None


def load_snapshot_preview(
    snapshot_json: Path,
    layout: dict | None = None,
    *,
    width: int | None = None,
    height: int | None = None,
    prefer_cache: bool = True,
) -> QPixmap:
    """Load cached PNG when present; otherwise render from ``layout``."""
    png = preview_path_for(snapshot_json)
    if prefer_cache and png.is_file():
        pix = QPixmap(str(png))
        if not pix.isNull():
            if width is not None and height is not None:
                if pix.width() != width or pix.height() != height:
                    return pix.scaled(
                        width,
                        height,
                        Qt.AspectRatioMode.KeepAspectRatio,
                        Qt.TransformationMode.SmoothTransformation,
                    )
            return pix
    if layout:
        pages = collect_preview_pages(layout)
        auto_w, auto_h = preview_canvas_size(len(pages))
        return render_layout_preview(
            layout,
            width=width if width is not None else auto_w,
            height=height if height is not None else auto_h,
        )
    if png.is_file():
        pix = QPixmap(str(png))
        if not pix.isNull():
            if width is not None and height is not None:
                if pix.width() != width or pix.height() != height:
                    return pix.scaled(
                        width,
                        height,
                        Qt.AspectRatioMode.KeepAspectRatio,
                        Qt.TransformationMode.SmoothTransformation,
                    )
            return pix
    return QPixmap()
