#!/usr/bin/env python3
"""Extend xianjian sleep frames with a continuous trunk + full canopy.

Source sleep cels only include the lower trunk (cut at the shoulder). This pads
upward, continues the real bark-colored trunk (not the sword), and paints a
canopy that overlaps the tip so the tree reads as one piece.
"""

from __future__ import annotations

import io
import subprocess
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
DSTS = [
    ROOT / "native" / "assets" / "pets",
    ROOT / "assets" / "pets",
    ROOT / "native" / "build" / "Release" / "assets" / "pets",
]

PAD_FRAC = 0.95


def is_bark(r: int, g: int, b: int, a: int) -> bool:
    """Match olive/brown trunk bark (exclude hair gray, robe white, foliage)."""
    if a < 110:
        return False
    if r > 190 and g > 180 and b > 170:
        return False
    if g > r + 18 and g > b + 18:
        return False  # foliage
    # Dark bark / warm wood — not near-neutral hair/shadow gray.
    if r < 48 or g < 40 or b > 130:
        return False
    chroma = max(r, g, b) - min(r, g, b)
    if chroma < 14:
        return False
    if abs(r - g) > 52:
        return False
    if r < g - 6:
        return False  # cool gray / blueish
    return r + g > b + 24


def longest_run(xs: list[int]) -> tuple[int, int]:
    if not xs:
        return 0, -1
    best = (xs[0], xs[0])
    cur = (xs[0], xs[0])
    for x in xs[1:]:
        if x == cur[1] + 1:
            cur = (cur[0], x)
        else:
            if cur[1] - cur[0] > best[1] - best[0]:
                best = cur
            cur = (x, x)
    if cur[1] - cur[0] > best[1] - best[0]:
        best = cur
    return best


def trunk_info(im: Image.Image) -> tuple[int, float, float, tuple[int, int, int]]:
    w, h = im.size
    px = im.load()
    # Measure trunk where it is thickest (lower half), then walk up that column.
    # Score prefers wide bark that is not the far-right sword strip.
    best = None  # (score, width, y, x0, x1)
    for y in range(h * 40 // 100, h * 94 // 100):
        xs = [x for x in range(w) if is_bark(*px[x, y])]
        if len(xs) < 10:
            continue
        x0, x1 = longest_run(xs)
        width = x1 - x0 + 1
        if width < 24:
            continue
        mid = (x0 + x1) * 0.5
        # Penalize far-right sword; reward mid/behind-character trunks.
        side_pen = 0.0
        if mid > w * 0.72:
            side_pen = (mid / w - 0.72) * 80.0
        score = width - side_pen
        if best is None or score > best[0]:
            best = (score, width, y, x0, x1)
    if best is None:
        raise RuntimeError("no trunk bark detected")
    _, _, _, x0, x1 = best
    cx = (x0 + x1) * 0.5
    tw = float(x1 - x0 + 1)

    # Highest bark pixel inside this trunk column band.
    measure_y = best[2]
    top_y = measure_y
    colors: list[tuple[int, int, int]] = []
    for y in range(0, measure_y + 1):
        row_cols: list[tuple[int, int, int]] = []
        for x in range(x0, x1 + 1):
            r, g, b, a = px[x, y]
            if is_bark(r, g, b, a):
                row_cols.append((r, g, b))
        if row_cols:
            top_y = y
            colors = row_cols
            break
    if not colors:
        colors = [(90, 78, 58)]
    # Prefer a lower, thicker band for bark color.
    for y in range(measure_y, max(0, measure_y - 40), -1):
        row_cols = []
        for x in range(x0, x1 + 1):
            r, g, b, a = px[x, y]
            if is_bark(r, g, b, a):
                row_cols.append((r, g, b))
        if len(row_cols) >= 8:
            colors = row_cols
            break
    n = len(colors)
    avg = (
        sum(c[0] for c in colors) // n,
        sum(c[1] for c in colors) // n,
        sum(c[2] for c in colors) // n,
    )
    return top_y, cx, tw, avg


def extend_frame(im: Image.Image) -> Image.Image:
    w, h = im.size
    pad = max(80, int(h * PAD_FRAC))
    out_h = h + pad
    out = Image.new("RGBA", (w, out_h), (0, 0, 0, 0))
    out.paste(im, (0, pad), im)

    top_y0, cx, tw, avg = trunk_info(im)
    top_y = top_y0 + pad
    trunk_top = int(pad * 0.28)

    under = Image.new("RGBA", (w, out_h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(under)

    half0 = tw * 0.58
    half1 = tw * 0.42
    bark = (avg[0], avg[1], avg[2], 255)
    bark_dk = (
        max(0, avg[0] - 18),
        max(0, avg[1] - 14),
        max(0, avg[2] - 10),
        255,
    )
    join_y = min(out_h - 1, top_y + 28)
    draw.polygon(
        [
            (cx - half0, join_y),
            (cx + half0, join_y),
            (cx + half1, trunk_top),
            (cx - half1, trunk_top),
        ],
        fill=bark,
    )
    draw.line(
        [(cx - half0 * 0.85, join_y), (cx - half1 * 0.9, trunk_top)],
        fill=bark_dk,
        width=max(3, int(tw * 0.07)),
    )

    canopy_cx = cx
    canopy_cy = pad * 0.44
    rw = max(tw * 3.8, w * 0.72)
    rh = pad * 0.72
    greens = [
        (38, 96, 50, 245),
        (50, 118, 62, 235),
        (32, 84, 44, 230),
        (62, 136, 72, 220),
        (44, 108, 56, 240),
        (56, 128, 68, 210),
    ]
    blobs = [
        (1.18, 0.00, 0.04),
        (0.92, -0.34, 0.00),
        (0.90, 0.36, 0.02),
        (0.78, -0.06, -0.22),
        (0.74, 0.20, -0.24),
        (0.66, -0.38, 0.16),
        (0.64, 0.40, 0.14),
        (0.60, 0.00, 0.28),
        (0.54, -0.24, -0.34),
        (0.52, 0.26, -0.32),
        (0.50, -0.14, 0.34),
        (0.48, 0.16, 0.32),
        (0.72, 0.00, 0.18),
    ]
    for i, (s, ox, oy) in enumerate(blobs):
        col = greens[i % len(greens)]
        draw.ellipse(
            [
                canopy_cx + ox * rw - rw * s * 0.5,
                canopy_cy + oy * rh - rh * s * 0.5,
                canopy_cx + ox * rw + rw * s * 0.5,
                canopy_cy + oy * rh + rh * s * 0.5,
            ],
            fill=col,
        )

    # Lower foliage mass — must cover trunk tip and sit behind the head sides.
    draw.ellipse(
        [
            canopy_cx - rw * 0.55,
            trunk_top - rh * 0.06,
            canopy_cx + rw * 0.55,
            trunk_top + rh * 0.55,
        ],
        fill=(40, 100, 52, 245),
    )
    draw.ellipse(
        [
            canopy_cx - rw * 0.40,
            trunk_top + rh * 0.02,
            canopy_cx + rw * 0.40,
            trunk_top + rh * 0.40,
        ],
        fill=(48, 114, 60, 240),
    )

    under = under.filter(ImageFilter.GaussianBlur(radius=0.4))
    return Image.alpha_composite(under, out)


def main() -> None:
    frames: list[Image.Image] = []
    for i in range(8):
        rel = f"native/assets/pets/xianjian_sleep_{i}.png"
        raw = subprocess.check_output(["git", "-C", str(ROOT), "show", f"HEAD:{rel}"])
        im = Image.open(io.BytesIO(raw)).convert("RGBA")
        info = trunk_info(im)
        fr = extend_frame(im)
        frames.append(fr)
        print(
            f"sleep_{i}: trunk top={info[0]} cx={info[1]:.1f} tw={info[2]:.1f} "
            f"-> {fr.size[0]}x{fr.size[1]}"
        )

    import shutil
    import tempfile
    import time

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        for i, fr in enumerate(frames):
            fr.save(tmp / f"xianjian_sleep_{i}.png", "PNG", optimize=True)
        for d in DSTS:
            d.mkdir(parents=True, exist_ok=True)
            for i in range(8):
                src = tmp / f"xianjian_sleep_{i}.png"
                dst = d / f"xianjian_sleep_{i}.png"
                for attempt in range(8):
                    try:
                        shutil.copy2(src, dst)
                        break
                    except OSError:
                        time.sleep(0.25)
                else:
                    raise SystemExit(f"failed to write {dst}")
    print("done")


if __name__ == "__main__":
    main()
