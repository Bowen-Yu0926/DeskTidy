"""Rebuild cute curled-up sleep sprites for langfrog + pig (breath frames)."""

from __future__ import annotations

import math
from collections import deque
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pets"
GEN = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")

_SCALE = 2
SLEEP_H = 140 * _SCALE

JOBS = (
    ("langfrog", "gen_langfrog_sleep_cute.png"),
    ("pig", "gen_pig_sleep_cute.png"),
)


def _lum(rgb: tuple[int, int, int]) -> float:
    return (rgb[0] + rgb[1] + rgb[2]) / 3.0


def _knockout_paper(cell: Image.Image) -> Image.Image:
    rgb = cell.convert("RGB")
    w, h = rgb.size
    src = rgb.load()
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dst = out.load()
    vis = [[False] * w for _ in range(h)]
    q: deque[tuple[int, int]] = deque()

    def is_bg(x: int, y: int) -> bool:
        r, g, b = src[x, y]
        if r >= 242 and g >= 242 and b >= 242:
            return True
        mx = max(r, g, b)
        mn = min(r, g, b)
        if mx - mn < 12 and _lum((r, g, b)) > 235:
            return True
        return False

    for coords in (
        [(i, 0) for i in range(w)],
        [(i, h - 1) for i in range(w)],
        [(0, j) for j in range(h)],
        [(w - 1, j) for j in range(h)],
    ):
        for x, y in coords:
            if is_bg(x, y) and not vis[y][x]:
                vis[y][x] = True
                q.append((x, y))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not vis[ny][nx] and is_bg(nx, ny):
                vis[ny][nx] = True
                q.append((nx, ny))
    for y in range(h):
        for x in range(w):
            if vis[y][x]:
                continue
            r, g, b = src[x, y]
            dst[x, y] = (r, g, b, 255)
    return out


def _trim(im: Image.Image, pad: int = 8) -> Image.Image:
    bbox = im.split()[-1].getbbox()
    if not bbox:
        return im
    l, t, r, b = bbox
    return im.crop(
        (max(0, l - pad), max(0, t - pad), min(im.width, r + pad), min(im.height, b + pad))
    )


def _fit_h(im: Image.Image, height: int) -> Image.Image:
    scale = height / float(im.height)
    nw = max(1, int(im.width * scale))
    return im.resize((nw, height), Image.Resampling.LANCZOS)


def _canvas(im: Image.Image, width: int, height: int) -> Image.Image:
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    x = (width - im.width) // 2
    y = height - im.height
    canvas.alpha_composite(im, (max(0, x), max(0, y)))
    return canvas


def _affine_frame(
    im: Image.Image,
    *,
    scale_x: float = 1.0,
    scale_y: float = 1.0,
    dy: float = 0.0,
    dx: float = 0.0,
) -> Image.Image:
    w, h = im.size
    pad = int(max(w, h) * 0.28) + 10
    work = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    work.alpha_composite(im, (pad, pad))
    nw = max(1, int(work.width * scale_x))
    nh = max(1, int(work.height * scale_y))
    scaled = work.resize((nw, nh), Image.Resampling.LANCZOS)
    tmp = Image.new("RGBA", work.size, (0, 0, 0, 0))
    ox = (work.width - nw) // 2 + int(round(dx))
    tmp.alpha_composite(scaled, (ox, work.height - nh))
    cropped = tmp.crop((pad, pad, pad + w, pad + h))
    if abs(dy) < 0.5:
        return cropped
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    shift = int(round(dy))
    if shift >= 0:
        out.alpha_composite(cropped.crop((0, 0, w, h - shift)), (0, shift))
    else:
        out.alpha_composite(cropped.crop((0, -shift, w, h)), (0, 0))
    return out


def _prepare(path: Path) -> Image.Image:
    cut = _knockout_paper(Image.open(path))
    fitted = _fit_h(_trim(cut, pad=max(12, SLEEP_H // 20)), SLEEP_H)
    pad_x = max(24, fitted.width // 10)
    return _canvas(fitted, fitted.width + pad_x, SLEEP_H + max(12, SLEEP_H // 24))


def _sleep_frames(base: Image.Image, n: int = 5) -> list[Image.Image]:
    frames: list[Image.Image] = []
    s = float(_SCALE)
    for i in range(n):
        phase = math.sin(i / float(n) * math.pi * 2)
        frames.append(
            _affine_frame(
                base,
                scale_x=1.0 - 0.03 * phase,
                scale_y=1.0 + 0.055 * phase,
                dy=1.5 * phase * s,
            )
        )
    return frames


def build() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    for char, fname in JOBS:
        src = GEN / fname
        if not src.is_file():
            raise FileNotFoundError(src)
        base = _prepare(src)
        base.save(OUT / f"{char}_sleep.png")
        for old in OUT.glob(f"{char}_sleep_*.png"):
            old.unlink()
        for i, fr in enumerate(_sleep_frames(base, 5)):
            fr.save(OUT / f"{char}_sleep_{i}.png")
        print(char, "sleep", base.size, "frames", 5)


if __name__ == "__main__":
    build()
