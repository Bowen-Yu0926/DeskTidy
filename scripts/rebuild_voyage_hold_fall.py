"""Rebuild voyage hold/fall sprites from generated art (hoodie-style grab + throw)."""

from __future__ import annotations

import math
import sys
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.build_voyage_pet import _keep_largest_blob, _knockout  # noqa: E402

PET = ROOT / "assets" / "pets"
ASSETS = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
CHAR = "voyage"
MASTER_H = 360


def _is_checker_bg(r: int, g: int, b: int, a: int = 255) -> bool:
    if a < 8:
        return True
    lum = (r + g + b) / 3.0
    mx, mn = max(r, g, b), min(r, g, b)
    if lum > 200 and mx - mn < 28:
        return True
    # AI green-screen plate
    if g > 140 and g > r + 40 and g > b + 40:
        return True
    if g > 200 and r < 120 and b < 120:
        return True
    return False


def _knockout_green(im: Image.Image) -> Image.Image:
    """Remove chroma-green / near-white preview plates from generated art."""
    arr = np.array(im.convert("RGBA"))
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    green = (g > 140) & (g > r + 40) & (g > b + 40)
    green |= (g > 200) & (r < 120) & (b < 120)
    green |= (g > 180) & (r < 160) & (b < 160) & ((g - r) > 30) & ((g - b) > 30)
    lum = (r + g + b) / 3.0
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    plate = (lum > 200) & (mx - mn < 28)
    arr[green | plate, 3] = 0
    return _keep_largest_blob(Image.fromarray(arr, "RGBA"))


def _knockout_gen(cell: Image.Image) -> Image.Image:
    """Knock out AI preview checkerboard / near-white plate."""
    rgba = cell.convert("RGBA")
    if rgba.getextrema()[3][0] < 250:
        cut = _knockout(rgba)
        return _keep_largest_blob(cut)
    rgb = rgba.convert("RGB")
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
            if _is_checker_bg(r, g, b) and not vis[y][x]:
                vis[y][x] = True
                q.append((x, y))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not vis[ny][nx]:
                r, g, b = src[nx, ny]
                if _is_checker_bg(r, g, b):
                    vis[ny][nx] = True
                    q.append((nx, ny))
    for y in range(h):
        for x in range(w):
            if not vis[y][x]:
                r, g, b = src[x, y]
                dst[x, y] = (r, g, b, 255)
    return _keep_largest_blob(out)


def _prepare_rgba(path: Path) -> Image.Image:
    raw = Image.open(path)
    cut = _knockout_green(_knockout_gen(raw))
    return _fit_h(_strip_pastel_bars(_trim(cut)), int(MASTER_H * 0.95))


def _trim(im: Image.Image, pad: int = 10) -> Image.Image:
    bb = im.split()[-1].getbbox()
    if not bb:
        return im
    l, t, r, b = bb
    return im.crop(
        (max(0, l - pad), max(0, t - pad), min(im.width, r + pad), min(im.height, b + pad))
    )


def _fit_h(im: Image.Image, height: int) -> Image.Image:
    scale = height / float(im.height)
    nw = max(1, int(im.width * scale))
    return im.resize((nw, height), Image.Resampling.LANCZOS)


def _on_canvas(im: Image.Image, width: int, height: int) -> Image.Image:
    c = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    bb = im.split()[-1].getbbox()
    if not bb:
        return c
    cropped = im.crop(bb)
    x = (width - cropped.width) // 2
    y = max(0, height - cropped.height - 2)
    c.alpha_composite(cropped, (max(0, x), y))
    return c


def _strip_pastel_bars(im: Image.Image) -> Image.Image:
    a = np.array(im.convert("RGBA"))
    h, w = a.shape[:2]
    for y in list(range(0, max(1, h // 20))) + list(range(h - max(1, h // 12), h)):
        row = a[y]
        rgb = row[:, :3].astype(int)
        pastel = (
            (rgb[:, 0] > 180) & (rgb[:, 1] > 150) & (rgb[:, 2] > 140) & (rgb.max(1) - rgb.min(1) < 90)
        ) | ((rgb[:, 0] > 200) & (rgb[:, 1] > 170) & (rgb[:, 2] < 170))
        a[y, pastel, 3] = 0
    out = Image.fromarray(a, "RGBA")
    bb = out.split()[-1].getbbox()
    return out.crop(bb) if bb else out


def _hold_sway_frames(base: Image.Image, n: int = 6) -> list[Image.Image]:
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
        bob = int(round(1.0 * abs(math.sin(math.radians(ang * 8)))))
        cropped = rotated.crop((pad, pad, pad + w, pad + h))
        out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        if bob:
            out.alpha_composite(cropped.crop((0, 0, w, h - bob)), (0, bob))
        else:
            out.alpha_composite(cropped, (0, 0))
        frames.append(out)
    return frames


def _prepare_hold(path: Path) -> Image.Image:
    prepared = _prepare_rgba(path)
    pad_x = max(36, prepared.width // 10)
    return _on_canvas(prepared, prepared.width + pad_x * 2, MASTER_H)


def _prepare_fall(path: Path) -> Image.Image:
    prepared = _prepare_rgba(path)
    pad_x = max(28, prepared.width // 12)
    return _on_canvas(prepared, prepared.width + pad_x * 2, MASTER_H)


def _purge(pattern: str) -> None:
    for old in PET.glob(pattern):
        old.unlink()


def rebuild_hold() -> int:
    src = ASSETS / "voyage_hold_gen.png"
    if not src.is_file():
        print("skip hold: missing", src)
        return 0
    base = _prepare_hold(src)
    sway = _hold_sway_frames(base, 6)
    _purge(f"{CHAR}_hold*.png")
    sway[0].save(PET / f"{CHAR}_hold.png")
    for i, frame in enumerate(sway):
        frame.save(PET / f"{CHAR}_hold_{i}.png")
    sizes = {(f.width, f.height) for f in sway}
    print("hold", sway[0].size, "frames", len(sway), "uniform", len(sizes) == 1)
    return len(sway)


def rebuild_fall() -> int:
    paths = [ASSETS / f"voyage_fall_gen_{i}.png" for i in range(3)]
    frames: list[Image.Image] = []
    for path in paths:
        if path.is_file():
            frames.append(_prepare_fall(path))
    if len(frames) < 2:
        print("skip fall: need voyage_fall_gen_0/1")
        return 0
    # Same canvas for every cel — dizzy (last frame) must not shrink vs airborne.
    mw = max(f.width for f in frames)
    frames = [_on_canvas(f, mw, MASTER_H) for f in frames]
    _purge(f"{CHAR}_fall*.png")
    frames[0].save(PET / f"{CHAR}_fall.png")
    for i, frame in enumerate(frames):
        frame.save(PET / f"{CHAR}_fall_{i}.png")
    sizes = {(f.width, f.height) for f in frames}
    print("fall", frames[0].size, "frames", len(frames), "uniform", len(sizes) == 1)
    return len(frames)


def main() -> None:
    PET.mkdir(parents=True, exist_ok=True)
    nh = rebuild_hold()
    nf = rebuild_fall()
    if nh < 1 or nf < 2:
        raise SystemExit("voyage hold/fall rebuild incomplete")
    print("voyage hold/fall OK")


if __name__ == "__main__":
    main()
