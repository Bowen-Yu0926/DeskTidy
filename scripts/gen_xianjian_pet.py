#!/usr/bin/env python3
"""Generate a 仙剑奇侠传1-style swordsman pixel pet — 白衣飘飘剑客.

白衣飘飘、高马尾、交领白袍、广袖、深青腰封、长袍下摆飘逸、斜背长剑。
Pure grid + NEAREST upscale. Outputs xianjian_* frames.

Usage:
  python scripts/gen_xianjian_pet.py            # all frames
  python scripts/gen_xianjian_pet.py --preview  # single preview PNG
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

CHAR_ID = "xianjian"
GRID_W = 44
GRID_H = 62
PIXEL_SCALE = 5

# Palette — 白衣飘飘
C_HAIR = (36, 29, 31)         # ink-black hair
C_HAIR_HI = (62, 51, 52)      # hair highlight
C_SKIN = (241, 219, 199)      # skin
C_EYE = (34, 30, 28)          # eye
C_BROW = (48, 40, 38)         # sword brow
C_ROBE = (240, 242, 246)      # moon-white robe
C_ROBE_SH = (213, 219, 228)   # robe shadow (folds)
C_COLLAR = (230, 233, 240)    # collar
C_SASH = (64, 78, 102)        # deep-blue sash
C_SASH_HI = (116, 134, 164)   # sash highlight
C_PANTS = (96, 102, 114)      # pants
C_BOOT = (72, 62, 56)         # boots
C_BLADE = (207, 215, 225)     # blade
C_GUARD = (156, 132, 92)      # guard
C_GRIP = (82, 57, 45)         # grip
C_OUTLINE = (27, 23, 21)


def _blank() -> Image.Image:
    return Image.new("RGBA", (GRID_W, GRID_H), (0, 0, 0, 0))


def _rect(d, x0, y0, x1, y1, color):
    d.rectangle([x0, y0, x1, y1], fill=color)


def draw_idle() -> Image.Image:
    im = _blank()
    d = ImageDraw.Draw(im)

    # --- 高马尾 (high ponytail flowing right) ---
    _rect(d, 20, 0, 26, 4, C_HAIR)         # topknot
    _rect(d, 19, 1, 27, 3, C_HAIR)
    for i in range(5):                       # flowing hair strands
        _rect(d, 26 + i, 2 + (i % 2), 27 + i, 4 + (i % 2), C_HAIR)
    _rect(d, 17, 4, 29, 11, C_HAIR)         # crown
    _rect(d, 15, 10, 19, 26, C_HAIR)        # left lock
    _rect(d, 27, 10, 31, 26, C_HAIR)        # right lock
    for i, x in enumerate(range(16, 30)):   # fringe
        _rect(d, x, 11, x, 13 + (i % 3), C_HAIR)

    # --- face (slimmer, more handsome) ---
    _rect(d, 17, 12, 29, 24, C_SKIN)
    _rect(d, 18, 13, 28, 23, C_SKIN)
    # sword brows
    _rect(d, 19, 15, 21, 15, C_BROW)
    _rect(d, 25, 15, 27, 15, C_BROW)
    # eyes
    _rect(d, 19, 16, 21, 18, C_EYE)
    _rect(d, 25, 16, 27, 18, C_EYE)
    # nose hint
    _rect(d, 22, 18, 23, 19, C_BROW)
    # mouth
    _rect(d, 21, 21, 24, 21, C_EYE)

    # --- neck ---
    _rect(d, 21, 24, 25, 27, C_SKIN)

    # --- 白衣 robe (cross-collar, tall & trim) ---
    _rect(d, 13, 26, 33, 40, C_ROBE)
    _rect(d, 14, 27, 32, 39, C_ROBE)
    # collar V
    _rect(d, 21, 26, 25, 27, C_COLLAR)
    _rect(d, 20, 27, 25, 28, C_COLLAR)
    _rect(d, 20, 28, 26, 29, C_COLLAR)
    _rect(d, 21, 29, 27, 30, C_COLLAR)
    # robe fold shading
    _rect(d, 14, 29, 16, 38, C_ROBE_SH)
    _rect(d, 30, 29, 32, 38, C_ROBE_SH)
    _rect(d, 21, 30, 25, 30, C_ROBE_SH)
    # 广袖 (wide sleeves)
    _rect(d, 7, 27, 13, 40, C_ROBE)
    _rect(d, 33, 27, 39, 40, C_ROBE)
    _rect(d, 7, 38, 13, 41, C_ROBE_SH)   # left cuff fold
    _rect(d, 33, 38, 39, 41, C_ROBE_SH)  # right cuff fold
    _rect(d, 8, 40, 13, 42, C_COLLAR)    # left cuff
    _rect(d, 33, 40, 39, 42, C_COLLAR)   # right cuff

    # --- 腰封 sash ---
    _rect(d, 13, 39, 33, 41, C_SASH)
    _rect(d, 21, 39, 25, 40, C_SASH_HI)

    # --- 长袍下摆 (flowing skirt) ---
    _rect(d, 13, 41, 33, 55, C_ROBE)
    _rect(d, 14, 42, 32, 54, C_ROBE)
    _rect(d, 12, 43, 14, 54, C_ROBE_SH)   # left fold
    _rect(d, 32, 43, 34, 54, C_ROBE_SH)   # right fold
    _rect(d, 15, 45, 17, 54, C_ROBE_SH)
    _rect(d, 29, 45, 31, 54, C_ROBE_SH)
    # fluttering hem (飘带)
    for i in range(4):
        _rect(d, 11 - i, 54 + (i % 2), 12 - i, 56 + (i % 2), C_ROBE)
    for i in range(4):
        _rect(d, 34 + i, 54 + (i % 2), 35 + i, 56 + (i % 2), C_ROBE)

    # --- pants + boots ---
    _rect(d, 19, 55, 23, 57, C_PANTS)
    _rect(d, 25, 55, 28, 57, C_PANTS)
    _rect(d, 18, 57, 22, 59, C_BOOT)
    _rect(d, 24, 57, 28, 59, C_BOOT)

    # --- 斜背长剑 (diagonal back sword) ---
    _rect(d, 35, 2, 37, 4, C_GRIP)
    _rect(d, 35, 4, 37, 6, C_GUARD)
    _rect(d, 36, 6, 38, 48, C_BLADE)
    _rect(d, 35, 6, 36, 48, C_BLADE)

    return _outline(im)


def _outline(im: Image.Image) -> Image.Image:
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
    return im.resize((im.width * PIXEL_SCALE, im.height * PIXEL_SCALE),
                     Image.Resampling.NEAREST)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--preview", action="store_true")
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[1]
    img = upscale(draw_idle())

    if args.preview:
        out = root / "scripts" / "xianjian_preview.png"
        img.save(out, "PNG")
        print(f"Preview: {out} ({img.size[0]}x{img.size[1]})")
        return

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
