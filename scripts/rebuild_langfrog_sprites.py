"""Rebuild langfrog sprites from Buddha-frog–matched generated art + multi-frames."""

from __future__ import annotations

import math
from collections import deque
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pets"
GEN = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")

# 2× master height vs on-screen ~172px — keeps 150–200% DPI sharp.
_SCALE = 2
IDLE_H = 168 * _SCALE
WALK_H = 156 * _SCALE
STAND_H = 156 * _SCALE
SLEEP_H = 140 * _SCALE
PLAY_H = 168 * _SCALE

POSES = {
    "idle": ("gen_langfrog_idle.png", IDLE_H),
    "stand": ("gen_langfrog_stand.png", STAND_H),
    "walk": ("gen_langfrog_walk.png", WALK_H),
    "sleep": ("gen_langfrog_sleep.png", SLEEP_H),
    "play": ("gen_langfrog_play.png", PLAY_H),
}


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
    return im.crop(
        (max(0, l - pad), max(0, t - pad), min(im.width, r + pad), min(im.height, b + pad))
    )


def _fit_h(im: Image.Image, height: int) -> Image.Image:
    if im.height == height:
        return im
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
    rotate: float = 0.0,
    dy: float = 0.0,
    dx: float = 0.0,
) -> Image.Image:
    from PIL import ImageFilter

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
    if abs(rotate) > 0.05:
        tmp = tmp.rotate(
            rotate,
            resample=Image.Resampling.BICUBIC,
            center=(tmp.width / 2, tmp.height * 0.85),
            fillcolor=(0, 0, 0, 0),
        )
        # Soften hard alpha stairsteps from rotation (avoids 锯齿 on sleep/play).
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
    """Idle: soft belly + tiny side rock (佛系鼓肚). No bake-rotate (avoids jaggies)."""
    frames: list[Image.Image] = []
    s = float(_SCALE)
    for i in range(n):
        t = i / float(n)
        phase = math.sin(t * math.pi * 2)
        frames.append(
            _affine_frame(
                base,
                scale_x=1.0 - 0.028 * phase,
                scale_y=1.0 + 0.05 * phase,
                dy=1.8 * phase * s,
                dx=2.0 * phase * s,
            )
        )
    return frames


def _stand_frames(base: Image.Image, n: int = 4) -> list[Image.Image]:
    """Perch/stand: almost still, micro breath only."""
    frames: list[Image.Image] = []
    s = float(_SCALE)
    for i in range(n):
        t = i / float(n)
        phase = math.sin(t * math.pi * 2)
        frames.append(
            _affine_frame(
                base,
                scale_x=1.0 - 0.01 * phase,
                scale_y=1.0 + 0.018 * phase,
                dy=0.6 * phase * s,
            )
        )
    return frames


def _walk_frames(base: Image.Image, n: int = 8) -> list[Image.Image]:
    """Hop cycle: crouch → launch → air stretch → land squash (no bake-rotate)."""
    frames: list[Image.Image] = []
    s = float(_SCALE)
    for i in range(n):
        t = i / float(n)
        hop = abs(math.sin(t * math.pi * 2)) ** 2.4
        land = 1.0 - hop
        frames.append(
            _affine_frame(
                base,
                scale_x=1.08 - 0.12 * hop + 0.04 * land,
                scale_y=0.88 + 0.18 * hop,
                dy=-hop * 14.0 * s,
            )
        )
    return frames


def _sleep_frames(base: Image.Image, n: int = 5) -> list[Image.Image]:
    """Side-snooze: art is already reclining — only soft belly breath (no tip rotate)."""
    frames: list[Image.Image] = []
    s = float(_SCALE)
    for i in range(n):
        t = i / float(n)
        phase = math.sin(t * math.pi * 2)
        frames.append(
            _affine_frame(
                base,
                scale_x=1.0 - 0.025 * phase,
                scale_y=1.0 + 0.045 * phase,
                dy=(1.2 * phase) * s,
            )
        )
    return frames


def _play_frames(base: Image.Image, n: int = 8) -> list[Image.Image]:
    """Tumble hop: squash + hop only; lean is runtime (avoids double-rotate jaggies)."""
    frames: list[Image.Image] = []
    s = float(_SCALE)
    for i in range(n):
        t = i / float(n)
        hop = abs(math.sin(t * math.pi * 2)) ** 1.6
        frames.append(
            _affine_frame(
                base,
                scale_x=1.1 - 0.16 * hop,
                scale_y=0.84 + 0.22 * hop,
                dy=-hop * 16.0 * s,
            )
        )
    return frames


def _prepare(path: Path, height: int) -> Image.Image:
    # Knock out + trim at full gen resolution, then LANCZOS down to master height.
    cut = _knockout_paper(Image.open(path))
    fitted = _fit_h(_trim(cut, pad=max(12, height // 20)), height)
    pad_x = max(24, fitted.width // 10)
    return _canvas(fitted, fitted.width + pad_x, height + max(12, height // 24))



def _clear_old_frames() -> None:
    for p in OUT.glob("langfrog*"):
        p.unlink()


def build() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    _clear_old_frames()
    prepared: dict[str, Image.Image] = {}
    for pose, (fname, height) in POSES.items():
        src = GEN / fname
        if not src.is_file():
            raise FileNotFoundError(src)
        prepared[pose] = _prepare(src, height)

    # Canonical filenames used by desktop_pet catalog
    prepared["idle"].save(OUT / "langfrog_idle.png")
    prepared["stand"].save(OUT / "langfrog_stand.png")
    prepared["walk"].save(OUT / "langfrog_walk.png")
    prepared["sleep"].save(OUT / "langfrog_sleep.png")
    prepared["play"].save(OUT / "langfrog_play.png")

    for i, fr in enumerate(_breath_frames(prepared["idle"], 6)):
        fr.save(OUT / f"langfrog_idle_{i}.png")
    for i, fr in enumerate(_stand_frames(prepared["stand"], 4)):
        fr.save(OUT / f"langfrog_stand_{i}.png")
    for i, fr in enumerate(_walk_frames(prepared["walk"], 8)):
        fr.save(OUT / f"langfrog_walk_{i}.png")
    for i, fr in enumerate(_sleep_frames(prepared["sleep"], 5)):
        fr.save(OUT / f"langfrog_sleep_{i}.png")
    for i, fr in enumerate(_play_frames(prepared["play"], 8)):
        fr.save(OUT / f"langfrog_play_{i}.png")

    for pose in POSES:
        n = len(list(OUT.glob(f"langfrog_{pose}_*.png")))
        print(pose, prepared[pose].size, "frames", n)


if __name__ == "__main__":
    build()
