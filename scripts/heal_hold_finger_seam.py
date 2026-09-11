"""Rebuild hold sprites from hold_gen and clear the white finger-gap seam."""

from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.rebuild_hold_same_fingers import (  # noqa: E402
    ASSETS,
    OUT_DIR,
    PET,
    fit_h,
    hold_sway_frames,
    knockout,
    on_canvas,
    strip_pastel_bars,
    trim,
    with_bg,
)


def _is_skin(r: int, g: int, b: int, a: int) -> bool:
    if a < 40:
        return False
    if r < 165 or g < 100 or b < 70:
        return False
    if g > 200 and b > 185:
        return False
    # Peach: strong red dominance over blue.
    return r >= g + 8 and r - b >= 30 and g < 200


def _is_seam_pale(r: int, g: int, b: int, a: int) -> bool:
    if a < 25:
        return False
    # Yellow-gold paper between fingers (AI fringe). Peach skin has g typically < 210.
    if r > 240 and g > 215 and 120 < b < 200 and (g - b) > 40 and abs(r - g) < 45:
        return True
    # Over-bright peach/white gap highlight (normal finger skin g is usually ≤205).
    if r > 245 and g > 208 and 155 < b < 205 and (r + g + b) / 3.0 > 215:
        return True
    # Yellow-white AI fringe / flash remnant
    if r > 245 and g > 235 and 150 < b < 220 and abs(r - g) < 25:
        return True
    # Pure / near white
    if r > 235 and g > 228 and b > 218:
        return True
    luma = (r + g + b) / 3.0
    sat = max(r, g, b) - min(r, g, b)
    # Desaturated pale mauve/gray between fingers (not peach skin).
    if luma > 205 and sat < 40 and g > 185 and b > 180 and (r - b) < 35:
        return True
    # Soft paper white
    if luma > 230 and sat < 25:
        return True
    return False


def heal_finger_white_seam(im: Image.Image) -> Image.Image:
    a = np.array(im.convert("RGBA"))
    h, w = a.shape[:2]
    y_max = max(48, int(h * 0.45))
    out = a.copy()

    for y in range(y_max):
        pales = [
            x
            for x in range(w)
            if _is_seam_pale(int(out[y, x, 0]), int(out[y, x, 1]), int(out[y, x, 2]), int(out[y, x, 3]))
        ]
        if not pales:
            continue
        runs: list[tuple[int, int]] = []
        start = prev = pales[0]
        for x in pales[1:]:
            if x == prev + 1:
                prev = x
                continue
            runs.append((start, prev))
            start = prev = x
        runs.append((start, prev))

        for x0, x1 in runs:
            width = x1 - x0 + 1
            if width > 14:
                continue
            left = right = None
            for d in range(1, 14):
                if left is None and x0 - d >= 0:
                    px = out[y, x0 - d]
                    if _is_skin(int(px[0]), int(px[1]), int(px[2]), int(px[3])):
                        left = px
                if right is None and x1 + d < w:
                    px = out[y, x1 + d]
                    if _is_skin(int(px[0]), int(px[1]), int(px[2]), int(px[3])):
                        right = px
                if left is not None and right is not None:
                    break
            # True contact micro-gap (≤3px) between two fingers → blend skin.
            # Wider pale wedge / paper slit → transparent (natural pinch gap).
            if left is not None and right is not None and width <= 3:
                fill = ((left.astype(np.int16) + right.astype(np.int16)) // 2).astype(np.uint8)
                for x in range(x0, x1 + 1):
                    out[y, x] = fill
            else:
                out[y, x0 : x1 + 1] = 0

    # Orphan pale crumbs next to transparency.
    for y in range(y_max):
        for x in range(1, w - 1):
            r, g, b, al = map(int, out[y, x])
            if not _is_seam_pale(r, g, b, al):
                continue
            if int(out[y, x - 1, 3]) < 20 or int(out[y, x + 1, 3]) < 20:
                out[y, x] = 0
            elif y > 0 and int(out[y - 1, x, 3]) < 20:
                out[y, x] = 0

    return Image.fromarray(out, "RGBA")


def finger_seam_count(im: Image.Image) -> int:
    a = np.array(im.convert("RGBA"))
    h, w = a.shape[:2]
    y_max = max(48, int(h * 0.45))
    count = 0
    for y in range(y_max):
        pales = [
            x
            for x in range(w)
            if _is_seam_pale(int(a[y, x, 0]), int(a[y, x, 1]), int(a[y, x, 2]), int(a[y, x, 3]))
        ]
        if not pales:
            continue
        runs: list[tuple[int, int]] = []
        start = prev = pales[0]
        for x in pales[1:]:
            if x == prev + 1:
                prev = x
                continue
            runs.append((start, prev))
            start = prev = x
        runs.append((start, prev))
        for x0, x1 in runs:
            if x1 - x0 + 1 <= 14:
                count += x1 - x0 + 1
    return count


def main() -> None:
    src = ASSETS / "hold_gen_2.png"
    if not src.is_file():
        src = PET / "hoodie_hold_0.png"
    prepared = fit_h(strip_pastel_bars(trim(knockout(Image.open(src)))), 340)
    pad_x = max(36, prepared.width // 10)
    base = on_canvas(prepared, prepared.width + pad_x * 2, 360)
    before = finger_seam_count(base)
    base = heal_finger_white_seam(heal_finger_white_seam(base))
    after = finger_seam_count(base)
    print("base seam before/after", before, after)
    assert after < max(25, before // 5), (before, after)

    sway = hold_sway_frames(base, 6)
    sway = [heal_finger_white_seam(heal_finger_white_seam(f)) for f in sway]
    mw = max(f.width for f in sway)
    mh = max(f.height for f in sway)
    sway = [on_canvas(f, mw, mh) for f in sway]
    sway = [heal_finger_white_seam(f) for f in sway]

    leftovers = [finger_seam_count(f) for f in sway]
    print("per-frame seam", leftovers)
    assert max(leftovers) < 35, leftovers

    for old in PET.glob("hoodie_hold*.png"):
        old.unlink()
    sway[0].save(PET / "hoodie_hold.png")
    for i, frame in enumerate(sway):
        frame.save(PET / f"hoodie_hold_{i}.png")

    gif = with_bg(sway)
    gif[0].save(
        OUT_DIR / "hoodie_hold_clean.gif",
        save_all=True,
        append_images=gif[1:],
        duration=120,
        loop=0,
    )

    (ROOT / "dist").mkdir(exist_ok=True)
    # Center on fingers for QA crop
    a = np.array(sway[0])
    h, w = a.shape[:2]
    cx = w // 2
    crop = sway[0].crop((cx - 50, 40, cx + 50, 140)).resize((400, 400), Image.Resampling.NEAREST)
    crop.save(ROOT / "dist" / "_hold_gap_after.png")
    print("ok", sway[0].size, "n", len(sway))


if __name__ == "__main__":
    main()
