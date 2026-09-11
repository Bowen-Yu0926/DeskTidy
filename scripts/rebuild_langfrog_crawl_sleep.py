"""Process unified-outfit langfrog sleep + crawl frames into assets/pets."""

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
CRAWL_H = 110 * _SCALE  # low silhouette


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


def _unify_robe_red(im: Image.Image) -> Image.Image:
    """Nudge yellow kasaya fills toward the canonical bright red grid robe."""
    out = im.convert("RGBA")
    px = out.load()
    w, h = out.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 40:
                continue
            # Yellow/orange robe fill (not pale tan sleeves, not gold trim sparkle)
            if r > 180 and g > 140 and b < 120 and (r - b) > 60 and (g - b) > 40:
                # Keep brighter gold as trim; shift mid yellow toward red.
                if g > r * 0.92 and r < 250:
                    nr = min(255, int(r * 0.55 + 200))
                    ng = min(255, int(g * 0.28 + 40))
                    nb = min(255, int(b * 0.35 + 30))
                    px[x, y] = (nr, ng, nb, a)
    return out


def _prepare(path: Path, height: int) -> Image.Image:
    cut = _unify_robe_red(_knockout_paper(Image.open(path)))
    fitted = _fit_h(_trim(cut, pad=max(12, height // 20)), height)
    pad_x = max(28, fitted.width // 8)
    return _canvas(fitted, fitted.width + pad_x, height + max(12, height // 24))


def _sleep_breath(base: Image.Image, n: int = 5) -> list[Image.Image]:
    frames: list[Image.Image] = []
    s = float(_SCALE)
    for i in range(n):
        phase = math.sin(i / float(n) * math.pi * 2)
        frames.append(
            _affine_frame(
                base,
                scale_x=1.0 - 0.02 * phase,
                scale_y=1.0 + 0.04 * phase,
                dy=1.2 * phase * s,
            )
        )
    return frames


def build() -> None:
    OUT.mkdir(parents=True, exist_ok=True)

    sleep_src = GEN / "gen_langfrog_sleep_robe.png"
    if not sleep_src.is_file():
        raise FileNotFoundError(sleep_src)
    sleep = _prepare(sleep_src, SLEEP_H)
    sleep.save(OUT / "langfrog_sleep.png")
    for old in OUT.glob("langfrog_sleep_*.png"):
        old.unlink()
    for i, fr in enumerate(_sleep_breath(sleep, 5)):
        fr.save(OUT / f"langfrog_sleep_{i}.png")
    print("sleep", sleep.size, "frames", 5)

    crawl_names = [
        "gen_langfrog_crawl_1.png",
        "gen_langfrog_crawl_2.png",
        "gen_langfrog_crawl_3.png",
        "gen_langfrog_crawl_4.png",
    ]
    crawl_frames: list[Image.Image] = []
    for name in crawl_names:
        src = GEN / name
        if not src.is_file():
            raise FileNotFoundError(src)
        crawl_frames.append(_prepare(src, CRAWL_H))

    # Unify canvas width so frame switches don't jump.
    max_w = max(fr.width for fr in crawl_frames)
    max_h = max(fr.height for fr in crawl_frames)
    unified: list[Image.Image] = []
    for fr in crawl_frames:
        unified.append(_canvas(fr, max_w, max_h))

    # Canonical + numbered: loop 1-2-3-4-3-2 for smoother wriggle
    cycle = [0, 1, 2, 3, 2, 1]
    unified[0].save(OUT / "langfrog_crawl.png")
    for old in OUT.glob("langfrog_crawl_*.png"):
        old.unlink()
    for i, idx in enumerate(cycle):
        unified[idx].save(OUT / f"langfrog_crawl_{i}.png")
    print("crawl", (max_w, max_h), "frames", len(cycle))


if __name__ == "__main__":
    build()
