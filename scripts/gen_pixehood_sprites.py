#!/usr/bin/env python3
"""Generate pixehood_* clean pixel-art sprites from hoodie_* frames.

Design goals (vs first muddy pass):
  - Chunkier grid + integer nearest upscale (no fractional soft edges)
  - Fewer flat colors, hard 1-bit alpha
  - Mild contrast so fabric / hair / skin read as separate blocks
  - Optional 1px dark outline for silhouette clarity

Usage:
  python scripts/gen_pixehood_sprites.py
"""

from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path

from PIL import Image, ImageEnhance, ImageFilter

CHAR_ID = "pixehood"
# Logical pixel columns — lower = chunkier / cleaner.
GRID_W = 32
# Exact integer scale → crisp blocks (32×5 = 160).
PIXEL_SCALE = 5
OUT_W = GRID_W * PIXEL_SCALE
PALETTE_COLORS = 10
ALPHA_CUT = 160
# Outline adds a heavy dark fringe; off by default for a cleaner read.
ADD_OUTLINE = False


def _content_bbox(im: Image.Image) -> tuple[int, int, int, int]:
    if im.mode != "RGBA":
        im = im.convert("RGBA")
    bbox = im.split()[-1].getbbox()
    if not bbox:
        return (0, 0, im.width, im.height)
    pad = max(1, min(im.width, im.height) // 64)
    l, t, r, b = bbox
    return (
        max(0, l - pad),
        max(0, t - pad),
        min(im.width, r + pad),
        min(im.height, b + pad),
    )


def _hard_alpha(im: Image.Image, cut: int = ALPHA_CUT) -> Image.Image:
    im = im.convert("RGBA")
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < cut:
                px[x, y] = (0, 0, 0, 0)
            else:
                px[x, y] = (r, g, b, 255)
    return im


def _contrast_prep(im: Image.Image) -> Image.Image:
    """Sharpen + contrast on RGB only; keep original alpha for cut later."""
    im = im.convert("RGBA")
    alpha = im.split()[-1]
    rgb = im.convert("RGB")
    rgb = ImageEnhance.Contrast(rgb).enhance(1.55)
    rgb = ImageEnhance.Color(rgb).enhance(1.25)
    rgb = ImageEnhance.Sharpness(rgb).enhance(1.4)
    rgb = rgb.filter(ImageFilter.UnsharpMask(radius=1.5, percent=160, threshold=1))
    out = rgb.convert("RGBA")
    out.putalpha(alpha)
    return out


def _quantize_opaque(im: Image.Image, colors: int) -> Image.Image:
    """Adaptive palette on opaque pixels only (transparent keyed out)."""
    im = im.convert("RGBA")
    w, h = im.size
    px = im.load()
    key = (255, 0, 255)  # magenta key never in character after hard alpha
    rgb = Image.new("RGB", (w, h), key)
    rp = rgb.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a >= 255:
                rp[x, y] = (r, g, b)
    # MEDIANCUT on keyed image; remap key back to transparent.
    q = rgb.quantize(colors=max(2, colors + 1), method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    q_rgb = q.convert("RGB")
    qp = q_rgb.load()
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    op = out.load()
    for y in range(h):
        for x in range(w):
            if px[x, y][3] < 255:
                continue
            r, g, b = qp[x, y]
            # If quantizer collapsed into key, keep pre-quantize color.
            if (r, g, b) == key:
                r, g, b = px[x, y][:3]
            op[x, y] = (r, g, b, 255)
    return out


def _merge_near_palette(palette: list[tuple[int, int, int]], min_dist2: int = 900) -> list[tuple[int, int, int]]:
    """Drop near-duplicate palette entries so blocks don't muddy together."""
    kept: list[tuple[int, int, int]] = []
    for c in palette:
        if any((c[0] - k[0]) ** 2 + (c[1] - k[1]) ** 2 + (c[2] - k[2]) ** 2 < min_dist2 for k in kept):
            continue
        kept.append(c)
    return kept or palette[:1]


def _snap_common_colors(im: Image.Image, max_colors: int) -> Image.Image:
    """Second pass: keep only the most common opaque colors (flatter blocks)."""
    im = im.convert("RGBA")
    px = im.load()
    w, h = im.size
    counts: Counter[tuple[int, int, int]] = Counter()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a:
                counts[(r, g, b)] += 1
    if not counts:
        return im
    palette = _merge_near_palette([c for c, _ in counts.most_common(max_colors * 3)], min_dist2=1100)[
        :max_colors
    ]

    def nearest(c: tuple[int, int, int]) -> tuple[int, int, int]:
        br, bg, bb = c
        best = palette[0]
        best_d = 10**9
        for pr, pg, pb in palette:
            d = (pr - br) ** 2 + (pg - bg) ** 2 + (pb - bb) ** 2
            if d < best_d:
                best_d = d
                best = (pr, pg, pb)
        return best

    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a:
                nr, ng, nb = nearest((r, g, b))
                # Crush near-white / near-black for cleaner fabric & hair.
                if nr + ng + nb >= 720:
                    nr, ng, nb = 255, 255, 255
                elif nr + ng + nb <= 70:
                    nr, ng, nb = 18, 16, 16
                px[x, y] = (nr, ng, nb, 255)
    return im


def _clean_orphan_pixels(im: Image.Image) -> Image.Image:
    """Drop opaque pixels with <2 opaque 4-neighbors (fringe dust)."""
    im = im.convert("RGBA")
    w, h = im.size
    px = im.load()
    keep = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if px[x, y][3] == 0:
                continue
            n = 0
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and 0 <= yy < h and px[xx, yy][3]:
                    n += 1
            keep[y][x] = n >= 2 or (
                # keep 1-neighbor tips on silhouette (hair spikes)
                n >= 1
                and any(
                    0 <= x + dx < w
                    and 0 <= y + dy < h
                    and px[x + dx, y + dy][3] == 0
                    for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (1, -1), (-1, 1), (1, 1))
                )
            )
            # Simpler: keep if n>=1 for edge, drop only true isolates (n==0)
            keep[y][x] = n >= 1
    for y in range(h):
        for x in range(w):
            if px[x, y][3] and not keep[y][x]:
                px[x, y] = (0, 0, 0, 0)
    return im


def _outline(im: Image.Image, color: tuple[int, int, int] = (28, 24, 22)) -> Image.Image:
    """1px dark outline outside opaque silhouette."""
    im = im.convert("RGBA")
    w, h = im.size
    alpha = im.split()[-1]
    # Dilate alpha by 1
    dil = alpha.filter(ImageFilter.MaxFilter(3))
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    op = out.load()
    ap = alpha.load()
    dp = dil.load()
    ip = im.load()
    cr, cg, cb = color
    for y in range(h):
        for x in range(w):
            if ap[x, y]:
                op[x, y] = ip[x, y]
            elif dp[x, y]:
                op[x, y] = (cr, cg, cb, 255)
    return out


def pixelate_frame(src: Image.Image, grid_w: int = GRID_W, scale: int = PIXEL_SCALE) -> Image.Image:
    im = _contrast_prep(src.convert("RGBA"))
    l, t, r, b = _content_bbox(im)
    crop = im.crop((l, t, r, b))
    cw, ch = crop.size
    if cw < 1 or ch < 1:
        return Image.new("RGBA", (grid_w * scale, grid_w * scale), (0, 0, 0, 0))

    grid_h = max(8, int(round(ch * (grid_w / float(cw)))))
    # BOX → solid cells; hard alpha before palette so fringe never muddies colors.
    low = crop.resize((grid_w, grid_h), Image.Resampling.BOX)
    low = _hard_alpha(low)
    low = _quantize_opaque(low, PALETTE_COLORS)
    low = _snap_common_colors(low, PALETTE_COLORS)
    low = _clean_orphan_pixels(low)
    if ADD_OUTLINE:
        low = _outline(low)

    out_w = grid_w * scale
    out_h = grid_h * scale
    hi = low.resize((out_w, out_h), Image.Resampling.NEAREST)

    # Canvas matches hoodie aspect (~314×376) at out_w.
    canvas_h = int(round(out_w * (376 / 314.0)))
    canvas = Image.new("RGBA", (out_w, canvas_h), (0, 0, 0, 0))
    ox = (out_w - hi.width) // 2
    oy = max(0, canvas_h - hi.height)
    canvas.paste(hi, (ox, oy), hi)
    return canvas


def collect_hoodie_frames(src_dir: Path) -> list[Path]:
    return sorted(src_dir.glob("hoodie_*.png"))


def out_name(src_name: str) -> str:
    if not src_name.startswith("hoodie_"):
        raise ValueError(src_name)
    return CHAR_ID + src_name[len("hoodie") :]


def generate(src_dir: Path, dst_dirs: list[Path], grid_w: int, scale: int) -> int:
    frames = collect_hoodie_frames(src_dir)
    if not frames:
        raise SystemExit(f"No hoodie_*.png under {src_dir}")
    for d in dst_dirs:
        d.mkdir(parents=True, exist_ok=True)
    n = 0
    for path in frames:
        out = pixelate_frame(Image.open(path), grid_w=grid_w, scale=scale)
        name = out_name(path.name)
        for d in dst_dirs:
            out.save(d / name, "PNG", optimize=True)
        n += 1
        print(f"  {path.name} -> {name} ({out.size[0]}x{out.size[1]})")
    return n


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", type=Path, default=root / "native" / "assets" / "pets")
    ap.add_argument("--dst", type=Path, nargs="*", default=None)
    ap.add_argument("--grid", type=int, default=GRID_W)
    ap.add_argument("--scale", type=int, default=PIXEL_SCALE)
    args = ap.parse_args()

    dsts = args.dst
    if not dsts:
        dsts = [
            root / "native" / "assets" / "pets",
            root / "assets" / "pets",
            root / "native" / "build" / "Release" / "assets" / "pets",
        ]
        py_pets = Path("E:/Desktidy/assets/pets")
        if py_pets.is_dir():
            dsts.append(py_pets)

    print(f"Source: {args.src}  grid={args.grid} scale={args.scale} → {args.grid * args.scale}px")
    n = generate(args.src, dsts, grid_w=args.grid, scale=args.scale)
    print(f"Done: {n} frames → {CHAR_ID}_*")


if __name__ == "__main__":
    main()
