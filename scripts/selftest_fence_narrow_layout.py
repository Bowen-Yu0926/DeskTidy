"""F-t12-01 regression gate (t16+t19, offscreen) -- wide-fence icon cells.

Root cause fixed in src/ui/fence_widget.py: the icon-scale cache compared only
icon_size/max_cols, so when a wide multi-column fence was dragged narrower
(label_width shrank before max_cols changed) the new label_width was never
dispatched to FenceIconItem.set_icon_size and cell min-width stayed stuck at
210px -- wider than the viewport once the fence reaches MIN_WIDTH=160, with
the horizontal scrollbar off.

Gate scope (F-t16-R1 repair, t19): the defect is only reachable on a WIDE
multi-column fence with 18 pinned items following the t12 narrowing sequence,
so this gate replays exactly that scenario (ported from the t12 Q3 probe):

  WIDTHS = [640, 512, 416, 320, 256, 200] + [MIN_WIDTH]

and asserts, at every step (persisted pass -- cfg["width"] follows the resize):
  1. cell_fits_viewport: max cell width <= scroll viewport width (no clip);
  2. rows_honoured: grid rows == ceil(cells / max_cols) and rows are
     monotonically non-decreasing as the width shrinks;
  3. no icon cell overlaps another or spills visibly past the fence frame;
  4. no icon widget sticks out of its parent (items host) rectangle.

Known-good numbers for the fixed code (recorded in docs/fix-log-4.0.28.md):
  cell_w = [118, 113, 120, 135, 210, 154, 114], clipped all 0.
With the pre-t16 cache (early return compares only icon_size/max_cols) the
sequence stalls at cell_w=210 for the last three steps and clipped_px grows
to 56/96 -- assertion 1 fails and this gate returns rc=1.

Negative testing (how to prove the gate is not a tautology): copy
src/ui/fence_widget.py to a temp file, delete the single line
"and label_width == self._last_label_width" from the early-return condition,
and load that copy under the module name "src.ui.fence_widget" BEFORE this
script imports it (see docs/fix-log-4.0.28.md t19 section for the runner).
The gate must fail (rc=1) under the negated module.

Usage: python selftest_fence_narrow_layout.py  (exit 0 = pass)
ASCII-only stdout on purpose (console codepage).
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

ROOT = Path(__file__).resolve().parents[1]
assert (ROOT / "src").is_dir(), ROOT
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from PyQt6.QtCore import QPoint, QRect  # noqa: E402
from PyQt6.QtTest import QTest  # noqa: E402
from PyQt6.QtWidgets import QApplication, QWidget  # noqa: E402

WIDTHS = [640, 512, 416, 320, 256, 200]
N_ITEMS = 18
STALE_TOLERANCE_PX = 2  # frame-decoration allowance for host-vs-viewport width


def _viewport_rect(w):
    scroll = getattr(w, "scroll", None)
    if scroll is None:
        return None
    vp = scroll.viewport()
    if vp is None:
        return None
    return QRect(vp.mapTo(w, QPoint(0, 0)), vp.size())


def _grid_state(w, icon_item_cls):
    rects = []
    grid_pos = {}
    for i in range(w.items_layout.count()):
        it = w.items_layout.itemAt(i)
        child = it.widget() if it is not None else None
        if isinstance(child, icon_item_cls):
            grid_pos[i] = w.items_layout.getItemPosition(i)[:2]
            rects.append(QRect(child.mapTo(w, QPoint(0, 0)), child.size()))
    cells = len(grid_pos)
    rows = max((g[0] for g in grid_pos.values()), default=-1) + 1
    cols_used = max((g[1] for g in grid_pos.values()), default=-1) + 1
    return rects, cells, rows, cols_used


def _wait_reflow(w, icon_item_cls, target_cols, timeout_ms=2500):
    started = __import__("time").perf_counter()
    deadline = started + timeout_ms / 1000.0
    while True:
        _rects, _cells, rows, cols_used = _grid_state(w, icon_item_cls)
        if cols_used == target_cols or __import__("time").perf_counter() >= deadline:
            return (started and (__import__("time").perf_counter() - started) * 1000.0), rows, cols_used


def _measure(w, app, icon_item_cls, width):
    icon_size, max_cols, _label_w = w._icon_layout_metrics()
    _lat_ms, rows, cols_used = _wait_reflow(w, icon_item_cls, max_cols)
    QTest.qWait(60)
    app.processEvents()
    w.items_layout.activate()
    app.processEvents()
    rects, cells, rows, cols_used = _grid_state(w, icon_item_cls)

    host = getattr(w, "items_widget", None)
    if not isinstance(host, QWidget):
        hrect, hname = w.rect(), "fence"
    else:
        hrect, hname = QRect(host.mapTo(w, QPoint(0, 0)), host.size()), "items_widget"
    vrect = _viewport_rect(w)

    overlap = 0
    for i in range(len(rects)):
        for j in range(i + 1, len(rects)):
            inter = rects[i].intersected(rects[j])
            if inter.width() > 0 and inter.height() > 0:
                overlap += 1

    fence_rect = QRect(0, 0, w.width(), w.height())
    footer = getattr(w, "footer_label", None)
    frect = None
    if isinstance(footer, QWidget) and footer.isVisible():
        frect = QRect(footer.mapTo(w, QPoint(0, 0)), footer.size())
    footer_overlap_visible = 0
    visible = [r.intersected(vrect) for r in rects] if vrect is not None else rects
    visible = [r for r in visible if r.width() > 0 and r.height() > 0]
    spill_fence_visible = sum(
        1 for r in visible if r.bottom() > fence_rect.bottom() or r.right() > fence_rect.right()
    )
    if frect is not None:
        footer_overlap_visible = sum(
            1 for r in visible
            if r.intersected(frect).width() > 0 and r.intersected(frect).height() > 0
        )

    cell_w = max((r.width() for r in rects), default=0)
    clipped_px = 0
    if vrect is not None:
        clipped_px = max(0, (hrect.x() + hrect.width()) - (vrect.x() + vrect.width()))

    return {
        "width": int(width),
        "cells": cells,
        "max_cols": int(max_cols),
        "cols_used": cols_used,
        "cols_honoured": cols_used == int(max_cols),
        "icon_size": int(icon_size),
        "label_width": int(_label_w),
        "cell_w": int(cell_w),
        "rows_from_grid": rows,
        "rows_expected_ceil": -(-cells // int(max_cols)) if max_cols else None,
        "rows_honoured": rows == (-(-cells // int(max_cols)) if max_cols else None),
        "overlap_pairs": overlap,
        "spill_fence_visible": spill_fence_visible,
        "footer_overlap_visible": footer_overlap_visible,
        "clipped_px": clipped_px,
        "cell_fits_viewport": (None if vrect is None else cell_w <= vrect.width()),
        "host": hname,
        "host_fits_viewport_w": (
            None if vrect is None else hrect.width() <= vrect.width() + STALE_TOLERANCE_PX
        ),
        "outside_parent": sum(
            1 for r in rects
            if r.right() > hrect.right() or r.left() < hrect.left()
        ),
    }


def _build_fence(width: int, real_dir):
    from src.ui.fence_widget import FenceWidget

    items = []
    for i in range(N_ITEMS):
        p = real_dir / f"q3_{i:02d}.txt"
        p.write_text("x", encoding="utf-8")
        items.append(str(p))

    cfg = {
        "id": "f-narrow",
        "name": "narrow",
        "virtual_items": items,
        "sort_by": "manual",
        "width": width,
        "height": 420,
        "style": {"view_mode": "grid"},
        "pages": [0],
        "visible": True,
    }
    settings = {"theme": "mist", "fences": [cfg], "page_folders": [], "exclude_patterns": []}
    fence = FenceWidget(cfg, settings)
    fence._sync_fence_virtual_items = lambda: None
    fence._sync_all_fence_configs_from_settings = lambda: None
    return fence, cfg


def main() -> int:
    app = QApplication.instance() or QApplication([])
    from src.ui.fence_widget import FenceIconItem, FenceWidget

    min_w = int(FenceWidget.MIN_WIDTH)
    widths = WIDTHS + [min_w]

    import tempfile

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="t19_narrow_") as td:
        real_dir = Path(td) / "pins"
        real_dir.mkdir()
        fence, cfg = _build_fence(widths[0], real_dir)
        fence.show()
        fence.resize(widths[0], 420)
        fence.refresh(force=True)  # build the real icon grid from virtual_items
        QTest.qWait(400)
        app.processEvents()

        print(f"{'width':>6} {'vp_w':>6} {'cell_w':>18} {'rows':>5} {'clipped':>8}  verdict")
        prev_rows = -1
        for width in widths:
            cfg["width"] = int(width)
            fence.resize(int(width), 420)
            QTest.qWait(150)
            row = _measure(fence, app, FenceIconItem, width)
            ok = (
                row["cell_fits_viewport"] is not False
                and row["rows_honoured"]
                and row["rows_from_grid"] >= prev_rows
                and row["overlap_pairs"] == 0
                and row["spill_fence_visible"] == 0
                and row["footer_overlap_visible"] == 0
                and row["outside_parent"] == 0
            )
            prev_rows = row["rows_from_grid"] or prev_rows
            if not ok:
                why = []
                if row["cell_fits_viewport"] is False:
                    why.append(f"cell_w {row['cell_w']} > viewport, clipped={row['clipped_px']}")
                if not row["rows_honoured"]:
                    why.append(
                        f"rows {row['rows_from_grid']} != ceil({row['cells']}/{row['max_cols']})"
                    )
                if row["rows_from_grid"] < prev_rows:
                    why.append(f"rows {row['rows_from_grid']} < prev {prev_rows}")
                if row["overlap_pairs"]:
                    why.append(f"overlap pairs={row['overlap_pairs']}")
                if row["spill_fence_visible"]:
                    why.append(f"visible spill past fence={row['spill_fence_visible']}")
                if row["footer_overlap_visible"]:
                    why.append(f"visible footer overlap={row['footer_overlap_visible']}")
                if row["outside_parent"]:
                    why.append(f"icons outside parent={row['outside_parent']}")
                failures.append(f"width={width}: " + "; ".join(why))
            print(
                f"{width:>6} "
                f"{row['clipped_px']:>8} "
                f"{str([row['cell_w']]):>18} "
                f"{row['rows_from_grid']:>5} "
                f"{row['clipped_px']:>8}  "
                f"{'OK' if ok else 'FAIL'}"
            )

    if failures:
        print("\nselftest_fence_narrow_layout: FAIL")
        for f in failures:
            print(" -", f)
        return 1
    print("\nselftest_fence_narrow_layout: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
