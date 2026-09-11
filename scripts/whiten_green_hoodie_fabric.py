"""Whiten mint/green hoodie BODY fabric on regen gens; keep print + chroma screen.

Mechanical color fix only — does not invent pose. Green screen stays for knockout.
Forest-green chest print (darker, more saturated) is preserved.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

SRC = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")


def whiten_green_hoodie_fabric(im: Image.Image) -> tuple[Image.Image, int]:
    arr = np.asarray(im.convert("RGBA")).copy()
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    # Chroma-key screen: very green, not fabric (leave for import knockout).
    screen = (g > 140) & (g > r + 55) & (g > b + 55) & (r < 120)
    # Mint/seafoam hoodie body: green-dominant but lighter / less extreme than screen.
    mint = (
        (~screen)
        & (g > r + 18)
        & (g > b + 12)
        & (g > 110)
        & (r > 70)
        & (b > 70)
        & ((r + g + b) / 3.0 > 130)
    )
    # Forest print: darker green ink — exclude from whitening.
    print_ink = (
        (~screen)
        & (g > r + 25)
        & (g > b + 20)
        & (g > 80)
        & ((r + g + b) / 3.0 < 145)
        & ((g - np.minimum(r, b)) > 40)
    )
    fabric = mint & ~print_ink
    n = int(fabric.sum())
    if n == 0:
        return im.convert("RGBA"), 0
    # Push toward warm white while keeping soft shading from luminance.
    lum = (0.3 * r + 0.5 * g + 0.2 * b).astype(np.float32)
    # Target white ~245 with slight shade from original lum.
    shade = np.clip((lum - 140) / 80.0, -0.25, 0.15)
    target = np.clip(236 + shade * 40, 200, 252)
    # Blend strongly toward white (keep a little original for folds).
    for c in range(3):
        ch = arr[:, :, c].astype(np.float32)
        ch[fabric] = ch[fabric] * 0.18 + target[fabric] * 0.82
        # Kill residual green cast.
        arr[:, :, c] = np.clip(ch, 0, 255).astype(np.uint8)
    # Extra despill on fabric: match G toward R/B average.
    rf = arr[:, :, 0].astype(np.float32)
    gf = arr[:, :, 1].astype(np.float32)
    bf = arr[:, :, 2].astype(np.float32)
    avg = (rf + bf) * 0.5
    gf[fabric] = gf[fabric] * 0.25 + avg[fabric] * 0.75
    arr[:, :, 1] = np.clip(gf, 0, 255).astype(np.uint8)
    return Image.fromarray(arr, "RGBA"), n


def main(argv: list[str]) -> int:
    paths = [SRC / a for a in argv] if argv else [
        SRC / f"hoodie_trash_regen_{i}.png" for i in range(12)
    ] + [SRC / f"hoodie_hold_regen_{i}.png" for i in range(3)]
    for path in paths:
        if not path.is_file():
            print("skip missing", path.name)
            continue
        out, n = whiten_green_hoodie_fabric(Image.open(path))
        if n:
            out.save(path)
            print(f"whitened {path.name}: {n} px")
        else:
            print(f"ok      {path.name}: no mint fabric")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
