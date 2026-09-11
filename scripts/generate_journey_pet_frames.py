"""Build multi-frame sleep/play/idle/walk sprites for Journey pets (pig/frog/weasel/gorilla)."""

from __future__ import annotations

import math
from collections import deque
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pets"
GEN = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")

CHARS = ("pig",)
_SCALE = 2
IDLE_H = 168 * _SCALE
WALK_H = 156 * _SCALE
SLEEP_H = 140 * _SCALE
PLAY_H = 168 * _SCALE


def _lum(rgb: tuple[int, int, int]) -> float:
    return (rgb[0] + rgb[1] + rgb[2]) / 3.0


def _knockout_paper(cell: Image.Image) -> Image.Image:
    """Flood-fill near-white paper from edges → transparent RGBA."""
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

    for x, y in (
        *[(i, 0) for i in range(w)],
        *[(i, h - 1) for i in range(w)],
        *[(0, j) for j in range(h)],
        *[(w - 1, j) for j in range(h)],
    ):
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
    l = max(0, l - pad)
    t = max(0, t - pad)
    r = min(im.width, r + pad)
    b = min(im.height, b + pad)
    return im.crop((l, t, r, b))


def _fit_h(im: Image.Image, height: int) -> Image.Image:
    if im.height == height:
        return im
    scale = height / float(im.height)
    nw = max(1, int(im.width * scale))
    return im.resize((nw, height), Image.Resampling.LANCZOS)


def _canvas(im: Image.Image, width: int, height: int, *, bottom: bool = True) -> Image.Image:
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    x = (width - im.width) // 2
    y = (height - im.height) if bottom else (height - im.height) // 2
    canvas.alpha_composite(im, (max(0, x), max(0, y)))
    return canvas


def _affine_frame(
    im: Image.Image,
    *,
    scale_x: float = 1.0,
    scale_y: float = 1.0,
    rotate: float = 0.0,
    dy: float = 0.0,
) -> Image.Image:
    """Subtle squash/bob for animation frames (keeps canvas size)."""
    from PIL import ImageFilter

    w, h = im.size
    pad = int(max(w, h) * 0.25) + 8
    work = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    work.alpha_composite(im, (pad, pad))
    nw = max(1, int(work.width * scale_x))
    nh = max(1, int(work.height * scale_y))
    scaled = work.resize((nw, nh), Image.Resampling.LANCZOS)
    tmp = Image.new("RGBA", work.size, (0, 0, 0, 0))
    ox = (work.width - nw) // 2
    oy = work.height - nh
    tmp.alpha_composite(scaled, (ox, oy))
    if abs(rotate) > 0.05:
        tmp = tmp.rotate(
            rotate,
            resample=Image.Resampling.BICUBIC,
            center=(tmp.width / 2, tmp.height * 0.85),
            fillcolor=(0, 0, 0, 0),
        )
        r, g, b, a = tmp.split()
        a = a.filter(ImageFilter.GaussianBlur(radius=0.9))
        tmp = Image.merge("RGBA", (r, g, b, a))
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


def _breath_frames(base: Image.Image, n: int = 6) -> list[Image.Image]:
    frames: list[Image.Image] = []
    s = float(_SCALE)
    for i in range(n):
        t = i / float(n)
        phase = math.sin(t * math.pi * 2)
        sy = 1.0 + 0.035 * phase
        sx = 1.0 - 0.02 * phase
        dy = 1.5 * phase * s
        frames.append(_affine_frame(base, scale_x=sx, scale_y=sy, dy=dy))
    return frames


def _walk_frames(base: Image.Image, n: int = 6) -> list[Image.Image]:
    frames: list[Image.Image] = []
    s = float(_SCALE)
    for i in range(n):
        t = i / float(n)
        bob = abs(math.sin(t * math.pi * 2)) * 5.0 * s
        sy = 0.96 + 0.06 * abs(math.sin(t * math.pi * 2))
        sx = 1.04 - 0.04 * abs(math.sin(t * math.pi * 2))
        frames.append(_affine_frame(base, scale_x=sx, scale_y=sy, dy=-bob))
    return frames


def _sleep_frames(base: Image.Image, n: int = 4) -> list[Image.Image]:
    frames: list[Image.Image] = []
    s = float(_SCALE)
    for i in range(n):
        t = i / float(n)
        phase = math.sin(t * math.pi * 2)
        sy = 1.0 + 0.04 * phase
        sx = 1.0 - 0.025 * phase
        frames.append(_affine_frame(base, scale_x=sx, scale_y=sy, dy=0.8 * phase * s))
    return frames


def _play_frames(base: Image.Image, n: int = 6) -> list[Image.Image]:
    frames: list[Image.Image] = []
    s = float(_SCALE)
    for i in range(n):
        t = i / float(n)
        hop = abs(math.sin(t * math.pi * 2)) * 10.0 * s
        sy = 0.92 + 0.10 * abs(math.sin(t * math.pi * 2))
        sx = 1.06 - 0.08 * abs(math.sin(t * math.pi * 2))
        frames.append(_affine_frame(base, scale_x=sx, scale_y=sy, dy=-hop))
    return frames


def _save_pose(stem: str, base: Image.Image, frames: list[Image.Image]) -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    base.save(OUT / f"{stem}.png")
    for i, fr in enumerate(frames):
        fr.save(OUT / f"{stem}_{i}.png")
    print(stem, "base", base.size, "frames", len(frames))


def _prepare_gen(path: Path, height: int) -> Image.Image:
    raw = Image.open(path)
    cut = _knockout_paper(raw)
    trimmed = _trim(cut, pad=10)
    fitted = _fit_h(trimmed, height)
    return _canvas(fitted, fitted.width + 16, height + 8, bottom=True)


def _prepare_existing(path: Path, height: int) -> Image.Image:
    im = Image.open(path).convert("RGBA")
    trimmed = _trim(im, pad=4)
    fitted = _fit_h(trimmed, height)
    return _canvas(fitted, fitted.width + 12, height + 4, bottom=True)


def build() -> None:
    for char in CHARS:
        idle_src = OUT / f"{char}.png"
        walk_src = OUT / f"{char}_walk.png"
        stand_src = OUT / f"{char}_stand.png"
        sleep_gen = GEN / f"gen_{char}_sleep.png"
        play_gen = GEN / f"gen_{char}_play.png"
        if not sleep_gen.is_file() or not play_gen.is_file():
            raise FileNotFoundError(f"missing generated art for {char}")

        idle = _prepare_existing(idle_src, IDLE_H)
        walk = _prepare_existing(walk_src, WALK_H)
        stand = _prepare_existing(stand_src, WALK_H) if stand_src.is_file() else walk
        sleep = _prepare_gen(sleep_gen, SLEEP_H)
        play = _prepare_gen(play_gen, PLAY_H)

        # Keep legacy filenames for idle/walk/stand, add sleep/play + numbered frames
        idle.save(OUT / f"{char}.png")
        walk.save(OUT / f"{char}_walk.png")
        stand.save(OUT / f"{char}_stand.png")

        for i, fr in enumerate(_breath_frames(idle, 6)):
            fr.save(OUT / f"{char}_{i}.png")
        for i, fr in enumerate(_walk_frames(walk, 6)):
            fr.save(OUT / f"{char}_walk_{i}.png")
        for i, fr in enumerate(_breath_frames(stand, 4)):
            fr.save(OUT / f"{char}_stand_{i}.png")

        _save_pose(f"{char}_sleep", sleep, _sleep_frames(sleep, 4))
        _save_pose(f"{char}_play", play, _play_frames(play, 6))


if __name__ == "__main__":
    build()
