#!/usr/bin/env python3
"""Turn an AI-generated 白衣剑仙 image into pet sprites.

1) rembg background removal (transparent subject)
2) pixelate via the same pipeline as gen_pixehood_sprites (crop → grid →
   palette → outline → NEAREST upscale)
3) write xianjian_* frames

Usage:
  python scripts/gen_from_reference.py <input.png> [--grid 36] [--scale 5]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image

# reuse the pixelate pipeline
sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_pixehood_sprites import pixelate_frame  # noqa: E402

CHAR_ID = "xianjian"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", type=Path)
    ap.add_argument("--grid", type=int, default=36)
    ap.add_argument("--scale", type=int, default=5)
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[1]
    src = Image.open(args.input).convert("RGB")

    print("removing background…")
    from rembg import remove  # lazy import (onnx runtime)

    cut = remove(src)  # RGBA transparent subject

    if args.preview:
        out = root / "scripts" / "xianjian_cut.png"
        cut.save(out, "PNG")
        print(f"Cut preview: {out} ({cut.size[0]}x{cut.size[1]})")
        # also pixelate preview
        pv = pixelate_frame(cut, grid_w=args.grid, scale=args.scale)
        pvout = root / "scripts" / "xianjian_preview.png"
        pv.save(pvout, "PNG")
        print(f"Pixel preview: {pvout} ({pv.size[0]}x{pv.size[1]})")
        return

    img = pixelate_frame(cut, grid_w=args.grid, scale=args.scale)
    dsts = [
        root / "native" / "assets" / "pets",
        root / "assets" / "pets",
        root / "native" / "build" / "Release" / "assets" / "pets",
    ]
    names = ["idle", "idle_0", "idle_1", "idle_2", "idle_3", "stand",
             "walk", "walk_0", "walk_1", "walk_2", "walk_3",
             "wait_0", "wait_1", "sleep_0", "sleep_1", "sleep_2", "sleep_3",
             "fall_0", "fall_1", "fall_2",
             "hold_0", "hold_1", "hold_2",
             "play_0", "play_1", "play_2", "play_3"] + \
            [f"trash_{i}" for i in range(12)]
    for d in dsts:
        d.mkdir(parents=True, exist_ok=True)
    for name in names:
        for d in dsts:
            img.save(d / f"{CHAR_ID}_{name}.png", "PNG", optimize=True)
    print(f"Done: {CHAR_ID}_* ({img.size[0]}x{img.size[1]})")


if __name__ == "__main__":
    main()
