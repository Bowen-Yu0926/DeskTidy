"""Offscreen render benchmark for the desktop pet (per-frame paint cost).

Drives DesktopPetWidget's paintEvent through the real pose/anim path and
measures per-state render() cost, plus _FIT_CACHE hit/miss counts.

Usage:
    python scripts/bench_pet_perf.py [--frames N] [--char hoodie|voyage|langfrog]

Not a test — a measurement harness. Prints structured lines for diffing.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QImage, QColor
from PyQt6.QtWidgets import QApplication


# ---------------------------------------------------------------- scenario setup

_STATE_IDLE = "idle"
_STATE_PLAY = "play"
_STATE_HAPPY = "happy"
_STATE_WANDER = "wander"
_STATE_FALL = "fall"
_STATE_TRASH = "trash"


def _setup_state(widget, state: str) -> None:
    """Directly pin the paint-determining fields (no _enter/_apply_size churn)."""
    w = widget
    w._dragging = state == "hold"
    if state == "hold":
        w._state = _STATE_IDLE
    else:
        w._state = state
    w._trash_erase_prev = None
    w._fx_hearts = []
    w._fx_food = []
    w._menu_open = False
    w._menu_hover = None
    w._chrome_hover = None
    # Reset anim so each scenario starts from a clean pose/blend.
    from src.ui.pet_anim import PetAnimState, pose_for_state

    w._anim = PetAnimState()
    w._anim.frame = 0
    w._anim.set_pose(pose_for_state("hold" if state == "hold" else state, character=w._character))


def _render_once(widget, img: QImage) -> None:
    img.fill(QColor(0, 0, 0, 0))
    widget.render(img)


def _measure(widget, img, frames: int, fit_stats: dict) -> dict:
    times = []
    for _ in range(frames):
        widget._anim.tick(0.033)
        widget._frame += 1
        t0 = time.perf_counter()
        _render_once(widget, img)
        times.append((time.perf_counter() - t0) * 1000.0)
    times.sort()
    avg = sum(times) / len(times)
    p50 = times[len(times) // 2]
    p99 = times[int(len(times) * 0.99)]
    return {
        "avg_ms": avg,
        "p50_ms": p50,
        "p99_ms": p99,
        "max_ms": times[-1],
        "fit_calls": fit_stats["calls"],
        "fit_miss": fit_stats["miss"],
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=150)
    ap.add_argument("--char", type=str, default="hoodie")
    args = ap.parse_args()

    app = QApplication.instance() or QApplication(sys.argv)

    from src.ui import pet_anim as pa
    from src.ui.pet_widget import DesktopPetWidget

    # Instrument _fit_sprite: a cache miss = the _FIT_CACHE key-set changed.
    fit_stats = {"calls": 0, "miss": 0}
    orig_fit = pa._fit_sprite

    def _wrapped_fit(*a, **k):
        before = set(pa._FIT_CACHE.keys())
        r = orig_fit(*a, **k)
        after = set(pa._FIT_CACHE.keys())
        fit_stats["calls"] += 1
        if after != before:
            fit_stats["miss"] += 1
        return r

    pa._fit_sprite = _wrapped_fit

    widget = DesktopPetWidget(settings={"pet": {"character": args.char}})
    # Force the requested character's sheets (settings may already have loaded it).
    widget._character = args.char
    widget._reload_sprite(force=True)
    widget.resize(widget.sizeHint()) if widget.width() <= 1 else None

    img = QImage(max(1, widget.width()), max(1, widget.height()), QImage.Format.Format_ARGB32_Premultiplied)
    print(f"char={args.char} widget={widget.width()}x{widget.height()} frames={args.frames}")

    scenarios = [
        ("idle", _STATE_IDLE),
        ("play", _STATE_PLAY),
        ("happy", _STATE_HAPPY),
        ("walk", _STATE_WANDER),
        ("hold(drag)", "hold"),
        ("fall", _STATE_FALL),
        ("trash", _STATE_TRASH),
    ]

    results = {}
    for name, state in scenarios:
        _setup_state(widget, state)
        # Cold-ish: drop fitted pixmaps so we also see per-state fit miss cost.
        pa._FIT_CACHE.clear()
        fit_stats["calls"] = 0
        fit_stats["miss"] = 0
        # Warmup: settle pose blend + fill the fit cache (steady-state proxy).
        for _ in range(30):
            widget._anim.tick(0.033)
            widget._frame += 1
            _render_once(widget, img)
        fit_stats["calls"] = 0
        fit_stats["miss"] = 0
        r = _measure(widget, img, args.frames, fit_stats)
        results[name] = r
        print(
            f"{name:12s} avg={r['avg_ms']:6.2f}ms p50={r['p50_ms']:6.2f}ms "
            f"p99={r['p99_ms']:6.2f}ms max={r['max_ms']:6.2f}ms "
            f"fit_calls={r['fit_calls']} fit_miss={r['fit_miss']}"
        )

    # --- cold state-switch fit cost (hypothesis #2: interactive re-scale) ---
    _setup_state(widget, _STATE_IDLE)
    for _ in range(20):
        widget._anim.tick(0.033)
        widget._frame += 1
        _render_once(widget, img)
    pa._FIT_CACHE.clear()
    fit_stats["calls"] = 0
    fit_stats["miss"] = 0
    _setup_state(widget, "hold")
    t0 = time.perf_counter()
    for _ in range(3):
        widget._anim.tick(0.033)
        widget._frame += 1
        _render_once(widget, img)
    cold_hold_ms = (time.perf_counter() - t0) * 1000.0 / 3.0
    print(
        f"cold_switch_hold first3_avg={cold_hold_ms:6.2f}ms "
        f"fit_calls={fit_stats['calls']} fit_miss={fit_stats['miss']}"
    )

    pa._fit_sprite = orig_fit
    print("BENCH DONE")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
