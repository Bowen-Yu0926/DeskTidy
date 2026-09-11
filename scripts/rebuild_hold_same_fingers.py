"""Rebuild hoodie hold sprites from ONE base cel so finger size never jumps."""

from __future__ import annotations

import math
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
ASSETS = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
PET = ROOT / "assets" / "pets"
OUT_DIR = ROOT.parent


def is_bg(r: int, g: int, b: int) -> bool:
    if r >= 242 and g >= 242 and b >= 242:
        return True
    if r > 215 and g > 195 and b > 130 and b < 230 and (r + g) / 2 - b > 18:
        return True
    if r > 185 and g > 170 and b > 190 and max(r, g, b) - min(r, g, b) < 70:
        return True
    if r > 215 and g > 185 and b > 175 and max(r, g, b) - min(r, g, b) < 55:
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


def trim(im: Image.Image, pad: int = 10) -> Image.Image:
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


def strip_pastel_bars(im: Image.Image) -> Image.Image:
    a = np.array(im.convert("RGBA"))
    h, w = a.shape[:2]
    for y in list(range(0, max(1, h // 20))) + list(range(h - max(1, h // 12), h)):
        row = a[y]
        rgb = row[:, :3].astype(int)
        pastel = (
            ((rgb[:, 0] > 180) & (rgb[:, 1] > 150) & (rgb[:, 2] > 140) & (rgb.max(1) - rgb.min(1) < 90))
            | ((rgb[:, 0] > 200) & (rgb[:, 1] > 170) & (rgb[:, 2] < 170))
            | ((rgb[:, 2] > 160) & (rgb[:, 0] > 130) & (rgb[:, 1] > 120) & (rgb[:, 2] >= rgb[:, 1] - 5))
        )
        a[y, pastel, 3] = 0
    # Full-width lilac floor stroke under a dangling grab pose (not shoes).
    band0 = int(h * 0.78)
    r = a[:, :, 0].astype(np.int16)
    g = a[:, :, 1].astype(np.int16)
    b = a[:, :, 2].astype(np.int16)
    al = a[:, :, 3]
    floor = (al > 20) & (b > 180) & ((b - g) > 20) & (r > 140)
    for y in range(band0, h):
        op = al[y] > 20
        n = int(op.sum())
        if n < 3:
            continue
        xs = np.where(op)[0]
        span = int(xs[-1] - xs[0] + 1)
        dens = n / float(span)
        if int(floor[y].sum()) >= 3:
            a[y, floor[y], :] = 0
        if span >= int(w * 0.28) and dens < 0.35 and n < 40:
            a[y, op, :] = 0
    out = Image.fromarray(a, "RGBA")
    bb = out.split()[-1].getbbox()
    return out.crop(bb) if bb else out


def hold_sway_frames(base: Image.Image, n: int = 6) -> list[Image.Image]:
    """Same fingers every frame — rotate the whole sprite around the pinch."""
    w, h = base.size
    pivot = (w * 0.50, h * 0.08)
    frames: list[Image.Image] = []
    for i in range(n):
        ang = 2.5 * math.sin(i / float(n) * math.pi * 2.0)
        pad = int(max(w, h) * 0.18) + 16
        work = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
        work.alpha_composite(base, (pad, pad))
        rotated = work.rotate(
            ang,
            resample=Image.Resampling.BICUBIC,
            center=(pivot[0] + pad, pivot[1] + pad),
            expand=False,
            fillcolor=(0, 0, 0, 0),
        )
        frames.append(rotated.crop((pad, pad, pad + w, pad + h)))
    return frames


def with_bg(frames: list[Image.Image], color=(248, 242, 228, 255)) -> list[Image.Image]:
    out: list[Image.Image] = []
    for frame in frames:
        bg = Image.new("RGBA", frame.size, color)
        bg.alpha_composite(frame)
        out.append(bg.convert("RGB").convert("P", palette=Image.Palette.ADAPTIVE, colors=160))
    return out


def top_finger_width(im: Image.Image) -> int:
    a = np.array(im)
    band = a[: max(1, a.shape[0] // 5)]
    ys, xs = np.where(band[:, :, 3] > 40)
    if len(xs) == 0:
        return 0
    return int(xs.max() - xs.min() + 1)


def main() -> None:
    base_src = ASSETS / "hold_gen_2.png"
    if not base_src.is_file():
        base_src = PET / "hoodie_hold.png"
    if not base_src.is_file():
        raise FileNotFoundError("no hold base image")

    prepared = fit_h(strip_pastel_bars(trim(knockout(Image.open(base_src)))), 340)
    pad_x = max(36, prepared.width // 10)
    base = on_canvas(prepared, prepared.width + pad_x * 2, 360)
    sway = hold_sway_frames(base, 6)

    for old in PET.glob("hoodie_hold*.png"):
        old.unlink()
    sway[0].save(PET / "hoodie_hold.png")
    for i, frame in enumerate(sway):
        frame.save(PET / f"hoodie_hold_{i}.png")

    hold_gif = with_bg(sway)
    hold_gif[0].save(
        OUT_DIR / "hoodie_hold_clean.gif",
        save_all=True,
        append_images=hold_gif[1:],
        duration=120,
        loop=0,
    )

    fall_paths = sorted(PET.glob("hoodie_fall_*.png"))
    falls = [Image.open(p).convert("RGBA") for p in fall_paths]
    if falls:
        mw = max(sway[0].width, max(f.width for f in falls))
        mh = 360

        def place(im: Image.Image) -> Image.Image:
            c = Image.new("RGBA", (mw, mh), (0, 0, 0, 0))
            bb = im.split()[-1].getbbox()
            if not bb:
                return c
            cr = im.crop(bb)
            x = (mw - cr.width) // 2
            y = mh - cr.height - 2
            c.alpha_composite(cr, (max(0, x), max(0, y)))
            return c

        combo_src = [place(f) for f in sway] + [place(f) for f in sway] + [place(f) for f in falls]
        combo = with_bg(combo_src)
        combo[0].save(
            OUT_DIR / "hoodie_collar_lift_clean.gif",
            save_all=True,
            append_images=combo[1:],
            duration=[120] * 12 + [160] * len(falls),
            loop=0,
        )

    widths = [top_finger_width(f) for f in sway]
    print("base", base_src.name, "size", sway[0].size, "n", len(sway))
    print("top_finger_widths", widths)
    assert max(widths) - min(widths) <= 3, widths
    print("ok: finger size stable across hold frames")


if __name__ == "__main__":
    main()
