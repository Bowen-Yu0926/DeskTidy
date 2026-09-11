"""Rebuild hoodie fall sprites from fall_gen_*.png with correct paper knockout.

Root cause of green/cream flash on mouse-release: fall cels retained opaque
paper (pale yellow / lavender / cream) because _is_bg missed those colors, and
hoodie_fall_clean.gif was baked with cream (248,242,228) that also survived
knockout.
"""

from __future__ import annotations

import math
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PET = ROOT / "assets" / "pets"
OUT_DIR = ROOT.parent
ASSETS = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
MASTER_H = 360


def is_bg(r: int, g: int, b: int) -> bool:
    # Near-white sparkles / page
    if r >= 245 and g >= 245 and b >= 245:
        return True
    # Cream preview plate used by with_bg (~248,242,228)
    if r > 235 and g > 225 and b > 200 and b < 245 and (r - b) < 55 and (g - b) < 50:
        return True
    # Pale yellow / gold paper (fall_gen top ~253,216,126)
    if (
        r > 215
        and g > 175
        and b > 85
        and b < 205
        and (r - b) > 35
        and (g - b) > 15
        and ((r + g) / 2 - b) > 16
    ):
        return True
    # Soft peach mid
    if r > 235 and g > 210 and b > 190 and max(r, g, b) - min(r, g, b) < 50:
        return True
    # Lavender / lilac floor (~206,179,236)
    if (
        b > 175
        and r > 155
        and g > 145
        and b >= g - 5
        and b >= r - 20
        and max(r, g, b) - min(r, g, b) < 95
        and not (r > 210 and g > 210 and b > 210)
    ):
        return True
    # Soft mint crumbs / chroma green (not the chest logo — flood is edge-only)
    if g > 145 and r < 165 and b < 165 and g > r + 22 and g > b + 22:
        return True
    if r < 50 and g > 200 and b < 50:
        return True
    return False


def knockout(cell: Image.Image) -> Image.Image:
    rgb = cell.convert("RGB")
    w, h = rgb.size
    src = rgb.load()
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dst = out.load()
    vis = [[False] * w for _ in range(h)]
    q: deque[tuple[int, int]] = deque()
    for coords in (
        [(i, 0) for i in range(w)],
        [(i, h - 1) for i in range(w)],
        [(0, j) for j in range(h)],
        [(w - 1, j) for j in range(h)],
    ):
        for x, y in coords:
            r, g, b = src[x, y]
            if is_bg(r, g, b) and not vis[y][x]:
                vis[y][x] = True
                q.append((x, y))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not vis[ny][nx]:
                r, g, b = src[nx, ny]
                if is_bg(r, g, b):
                    vis[ny][nx] = True
                    q.append((nx, ny))
    for y in range(h):
        for x in range(w):
            if not vis[y][x]:
                r, g, b = src[x, y]
                dst[x, y] = (r, g, b, 255)
    return out


def wipe_chroma_crumbs(im: Image.Image) -> Image.Image:
    """Remove isolated chroma-green / paper islands not edge-flooded."""
    a = np.array(im.convert("RGBA"))
    r = a[:, :, 0].astype(np.int16)
    g = a[:, :, 1].astype(np.int16)
    b = a[:, :, 2].astype(np.int16)
    al = a[:, :, 3]
    paper = (
        (al > 0)
        & (
            ((r > 235) & (g > 225) & (b > 200) & (b < 245))
            | ((r > 215) & (g > 175) & (b > 85) & (b < 205) & ((r - b) > 35) & ((g - b) > 15))
            | ((b > 175) & (r > 155) & (g > 145) & (b >= g - 5) & (b >= r - 20) & ((np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)) < 95) & ~((r > 210) & (g > 210) & (b > 210)))
            | ((r < 50) & (g > 200) & (b < 50))
        )
    )
    # Only wipe paper-like pixels that are NOT part of a dense character neighborhood
    # (avoid eating white hoodie). Require sparse local opacity.
    if not paper.any():
        return im
    opaque = al > 40
    # Dilate character core: pixels with many opaque neighbors are kept.
    from numpy.lib.stride_tricks import sliding_window_view

    pad = np.pad(opaque.astype(np.uint8), 1, mode="constant")
    windows = sliding_window_view(pad, (3, 3))
    neighbor = windows.sum(axis=(-1, -2))
    # Paper with few opaque neighbors → background crumb
    wipe = paper & (neighbor <= 4)
    a[wipe, 3] = 0
    return Image.fromarray(a, "RGBA")


def keep_largest_blob(im: Image.Image) -> Image.Image:
    alpha = im.split()[-1]
    w, h = im.size
    src_a = alpha.load()
    vis = [[False] * w for _ in range(h)]
    best: list[tuple[int, int]] = []
    for y in range(h):
        for x in range(w):
            if vis[y][x] or src_a[x, y] <= 8:
                continue
            blob: list[tuple[int, int]] = []
            q: deque[tuple[int, int]] = deque([(x, y)])
            vis[y][x] = True
            while q:
                cx, cy = q.popleft()
                blob.append((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < w and 0 <= ny < h and not vis[ny][nx] and src_a[nx, ny] > 8:
                        vis[ny][nx] = True
                        q.append((nx, ny))
            if len(blob) > len(best):
                best = blob
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dst = out.load()
    src = im.load()
    for x, y in best:
        dst[x, y] = src[x, y]
    return out


def trim(im: Image.Image, pad: int = 8) -> Image.Image:
    bb = im.split()[-1].getbbox()
    if not bb:
        return im
    l, t, r, b = bb
    return im.crop(
        (max(0, l - pad), max(0, t - pad), min(im.width, r + pad), min(im.height, b + pad))
    )


def fit_h(im: Image.Image, height: int) -> Image.Image:
    scale = height / float(im.height)
    nw = max(1, int(im.width * scale))
    return im.resize((nw, height), Image.Resampling.LANCZOS)


def on_canvas(im: Image.Image, width: int, height: int) -> Image.Image:
    c = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    bb = im.split()[-1].getbbox()
    if not bb:
        return c
    cropped = im.crop(bb)
    x = (width - cropped.width) // 2
    y = max(0, height - cropped.height - 2)
    c.alpha_composite(cropped, (max(0, x), y))
    return c


def prepare(path: Path, *, keep_largest: bool = True) -> Image.Image:
    cut = knockout(Image.open(path))
    cut = wipe_chroma_crumbs(cut)
    if keep_largest:
        cut = keep_largest_blob(cut)
    fitted = fit_h(trim(cut), int(MASTER_H * 0.95))
    pad_x = max(36, fitted.width // 10)
    return on_canvas(fitted, fitted.width + pad_x * 2, MASTER_H)


def paper_leftover(im: Image.Image) -> int:
    """Count yellow/cream/lavender PAPER still in the outer band (flashing plate)."""
    a = np.array(im.convert("RGBA"))
    h, w = a.shape[:2]
    r = a[:, :, 0].astype(np.int16)
    g = a[:, :, 1].astype(np.int16)
    b = a[:, :, 2].astype(np.int16)
    al = a[:, :, 3]
    # Outer rim only — character feet may touch ~6–8%; paper plates fill this band.
    m = max(4, int(min(h, w) * 0.08))
    edge = np.zeros((h, w), dtype=bool)
    edge[:m, :] = True
    edge[-m:, :] = True
    edge[:, :m] = True
    edge[:, -m:] = True
    # Distinctive gold/yellow paper (fall_gen / dist leak ~246,205,124) — not skin.
    yellow_paper = (
        (al > 40)
        & (r > 230)
        & (g > 175)
        & (g < 235)
        & (b > 90)
        & (b < 185)
        & ((r - b) > 50)
        & ((g - b) > 25)
    )
    cream_plate = (al > 200) & (r > 240) & (g > 235) & (b > 215) & (b < 245) & ((r - b) < 40)
    lavender_paper = (
        (al > 40)
        & (b > 200)
        & (r > 170)
        & (r < 230)
        & (g > 155)
        & (g < 210)
        & (b > r + 5)
        & (b > g + 10)
    )
    chroma = (al > 40) & (r < 50) & (g > 200) & (b < 50)
    return int((edge & (yellow_paper | cream_plate | lavender_paper | chroma)).sum())


def with_checker(frames: list[Image.Image]) -> list[Image.Image]:
    """Preview GIF on checkerboard — never bake opaque cream into the cel."""
    out: list[Image.Image] = []
    for frame in frames:
        w, h = frame.size
        bg = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        tile = 12
        px = bg.load()
        for y in range(h):
            for x in range(w):
                c = 210 if ((x // tile) + (y // tile)) % 2 == 0 else 170
                px[x, y] = (c, c, c, 255)
        bg.alpha_composite(frame)
        out.append(bg.convert("RGB").convert("P", palette=Image.Palette.ADAPTIVE, colors=180))
    return out


def main() -> None:
    # Release sequence: airborne → mid → land. keep_largest drops disconnected hand.
    sources = [
        ASSETS / "fall_gen_1.png",
        ASSETS / "fall_gen_0.png",
        ASSETS / "fall_gen_2.png",
    ]
    frames: list[Image.Image] = []
    for src in sources:
        if not src.is_file():
            print("missing", src)
            continue
        frames.append(prepare(src, keep_largest=True))

    if len(frames) < 2:
        raise SystemExit("need at least 2 fall_gen sources")

    mw = max(f.width for f in frames)
    mh = MASTER_H
    frames = [on_canvas(f, mw, mh) for f in frames]

    leftovers = [paper_leftover(f) for f in frames]
    print("paper leftover", leftovers)
    assert max(leftovers) < 200, leftovers

    for old in PET.glob("hoodie_fall*.png"):
        old.unlink()
    frames[0].save(PET / "hoodie_fall.png")
    for i, fr in enumerate(frames):
        fr.save(PET / f"hoodie_fall_{i}.png")

    gif = with_checker(frames)
    gif[0].save(
        OUT_DIR / "hoodie_fall_clean.gif",
        save_all=True,
        append_images=gif[1:],
        duration=160,
        loop=0,
    )
    print("ok", frames[0].size, "n", len(frames))


if __name__ == "__main__":
    main()
