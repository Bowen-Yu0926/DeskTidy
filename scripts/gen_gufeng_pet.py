#!/usr/bin/env python3
"""Generate a 古风 (classical Chinese) anime girl pixel pet.

Draws a hanfu-clad girl (double buns, hanfu robe, sash, wide sleeves, long
skirt) on a clean pixel grid, then NEAREST-upscales to the same footprint the
pet engine expects (pixehood-style chunky pixels). Outputs gufeng_* frames.

Usage:
  python scripts/gen_gufeng_pet.py
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

CHAR_ID = "gufeng"
GRID_W = 36
GRID_H = 52
PIXEL_SCALE = 5

# Palette (ancient / refined)
C_HAIR = (40, 32, 36)       # ink black hair
C_SKIN = (244, 220, 198)    # skin
C_BLUSH = (240, 196, 186)   # blush
C_EYE = (46, 38, 34)        # eye
C_ROBE = (92, 148, 140)     # jade robe (upper)
C_SKIRT = (148, 182, 176)   # pale jade skirt
C_SASH = (176, 64, 54)      # vermilion sash
C_COLLAR = (228, 234, 240)  # moon-white collar / cuffs
C_SHOE = (84, 58, 46)       # shoe
C_OUTLINE = (30, 26, 24)    # outline


def _blank() -> Image.Image:
    return Image.new("RGBA", (GRID_W, GRID_H), (0, 0, 0, 0))


def _rect(d: ImageDraw.ImageDraw, x0, y0, x1, y1, color):
    d.rectangle([x0, y0, x1, y1], fill=color)


def _px(d: ImageDraw.ImageDraw, grid, color):
    for (x, y) in grid:
        d.point((x, y), fill=color)


def draw_idle() -> Image.Image:
    im = _blank()
    d = ImageDraw.Draw(im)

    # --- hair: double buns + fringe + side locks ---
    _rect(d, 8, 2, 14, 8, C_HAIR)    # left bun
    _rect(d, 22, 2, 28, 8, C_HAIR)   # right bun
    _rect(d, 10, 4, 26, 9, C_HAIR)   # crown
    _rect(d, 7, 7, 29, 14, C_HAIR)   # top hair
    _rect(d, 8, 9, 12, 24, C_HAIR)   # left side lock
    _rect(d, 24, 9, 28, 24, C_HAIR)  # right side lock
    # fringe
    for i, x in enumerate(range(10, 27)):
        _rect(d, x, 13, x, 15 + (i % 3), C_HAIR)

    # --- face ---
    _rect(d, 12, 12, 24, 23, C_SKIN)
    _rect(d, 13, 13, 23, 22, C_SKIN)
    # blush
    _rect(d, 13, 18, 15, 20, C_BLUSH)
    _rect(d, 21, 18, 23, 20, C_BLUSH)
    # eyes
    _rect(d, 14, 16, 16, 17, C_EYE)
    _rect(d, 20, 16, 22, 17, C_EYE)
    # mouth
    _rect(d, 17, 20, 19, 20, C_EYE)

    # --- neck ---
    _rect(d, 16, 23, 20, 26, C_SKIN)

    # --- robe body (cross-collar hanfu) ---
    _rect(d, 10, 24, 26, 32, C_ROBE)
    # cross collar (V)
    _rect(d, 16, 24, 20, 25, C_COLLAR)
    _rect(d, 16, 25, 20, 26, C_COLLAR)
    _rect(d, 16, 26, 20, 27, C_COLLAR)
    _rect(d, 18, 24, 20, 25, C_COLLAR)
    # wide sleeves
    _rect(d, 6, 24, 10, 32, C_ROBE)
    _rect(d, 26, 24, 30, 32, C_ROBE)
    _rect(d, 6, 30, 10, 33, C_COLLAR)   # left cuff
    _rect(d, 26, 30, 30, 33, C_COLLAR)  # right cuff

    # --- sash ---
    _rect(d, 10, 31, 26, 33, C_SASH)
    _rect(d, 16, 32, 20, 33, C_COLLAR)

    # --- skirt ---
    _rect(d, 10, 33, 26, 44, C_SKIRT)
    _rect(d, 11, 33, 25, 43, C_SKIRT)

    # --- shoes ---
    _rect(d, 14, 44, 17, 46, C_SHOE)
    _rect(d, 19, 44, 22, 46, C_SHOE)

    # --- outline (dilate silhouette by 1) ---
    im = _outline(im)
    return im


def draw_walk(step: int) -> Image.Image:
    im = draw_idle()
    d = ImageDraw.Draw(im)
    # leg/skirt sway + one shoe forward per frame
    if step % 2 == 0:
        _rect(d, 12, 44, 15, 46, C_SHOE)
        _rect(d, 20, 44, 23, 46, C_SHOE)
    else:
        _rect(d, 14, 44, 17, 46, C_SHOE)
        _rect(d, 18, 44, 21, 46, C_SHOE)
    return im


def draw_sleep() -> Image.Image:
    im = _blank()
    d = ImageDraw.Draw(im)
    # lying figure (simplified): hair + robe + skirt horizontal
    _rect(d, 6, 24, 30, 28, C_HAIR)   # hair bun side
    _rect(d, 8, 22, 24, 26, C_SKIN)   # face
    _rect(d, 9, 23, 10, 24, C_EYE)
    _rect(d, 12, 23, 13, 24, C_EYE)
    _rect(d, 8, 26, 28, 32, C_ROBE)   # body
    _rect(d, 8, 32, 28, 36, C_SKIRT)  # skirt
    _rect(d, 10, 36, 26, 38, C_SASH)
    im = _outline(im)
    return im


def draw_fall() -> Image.Image:
    # mid-fall tilt: reuse idle but offset legs / skirt up
    im = draw_idle()
    d = ImageDraw.Draw(im)
    _rect(d, 10, 44, 26, 46, C_SKIRT)
    _rect(d, 13, 46, 16, 48, C_SHOE)
    _rect(d, 19, 46, 22, 48, C_SHOE)
    return im


def _outline(im: Image.Image) -> Image.Image:
    from PIL import ImageFilter

    im = im.convert("RGBA")
    alpha = im.split()[-1]
    dil = alpha.filter(ImageFilter.MaxFilter(3))
    out = Image.new("RGBA", im.size, (0, 0, 0, 0))
    op = out.load()
    ap = alpha.load()
    dp = dil.load()
    ip = im.load()
    for y in range(im.height):
        for x in range(im.width):
            if ap[x, y]:
                op[x, y] = ip[x, y]
            elif dp[x, y]:
                op[x, y] = (*C_OUTLINE, 255)
    return out


def upscale(im: Image.Image) -> Image.Image:
    return im.resize((im.width * PIXEL_SCALE, im.height * PIXEL_SCALE), Image.Resampling.NEAREST)


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    dsts = [
        root / "native" / "assets" / "pets",
        root / "assets" / "pets",
        root / "native" / "build" / "Release" / "assets" / "pets",
    ]

    def save(name: str, im: Image.Image):
        hi = upscale(im)
        for d in dsts:
            d.mkdir(parents=True, exist_ok=True)
            hi.save(d / f"{CHAR_ID}_{name}.png", "PNG", optimize=True)
        print(f"  {CHAR_ID}_{name}.png ({hi.size[0]}x{hi.size[1]})")

    save("idle", draw_idle())
    save("idle_0", draw_idle())
    save("idle_1", draw_idle())
    save("idle_2", draw_idle())
    save("idle_3", draw_idle())
    save("stand", draw_idle())
    for i in range(4):
        save(f"walk_{i}", draw_walk(i))
    save("walk", draw_walk(0))
    save("wait_0", draw_idle())
    save("wait_1", draw_idle())
    save("sleep_0", draw_sleep())
    save("sleep_1", draw_sleep())
    save("sleep_2", draw_sleep())
    save("sleep_3", draw_sleep())
    save("fall_0", draw_fall())
    save("fall_1", draw_fall())
    save("fall_2", draw_fall())
    save("hold_0", draw_idle())
    save("hold_1", draw_idle())
    save("hold_2", draw_idle())
    save("play_0", draw_walk(0))
    save("play_1", draw_walk(1))
    save("play_2", draw_walk(2))
    save("play_3", draw_walk(3))
    save("trash_0", draw_idle())
    for i in range(12):
        save(f"trash_{i}", draw_idle())
    print(f"Done: {CHAR_ID}_*")


if __name__ == "__main__":
    main()
