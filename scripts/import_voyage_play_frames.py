"""Import AI-generated voyage play/happy (跳起来欢呼) cels into assets/pets."""

from __future__ import annotations

import sys
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.build_voyage_pet import _keep_largest_blob  # noqa: E402

OUT = ROOT / "assets" / "pets"
SRC_DIR = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
CHAR = "voyage"
N = 6
SKIP_LEAD = 2  # drop squat wind-up — click cheer starts on the jump
MASTER_H = 360
TARGET_W = 430


def _is_chroma_bg(r: int, g: int, b: int, a: int = 255) -> bool:
    if a < 8:
        return True
    lum = (r + g + b) / 3.0
    mx, mn = max(r, g, b), min(r, g, b)
    if lum > 200 and mx - mn < 28:
        return True
    if g > 90 and g > r + 10 and g > b + 10 and lum > 85:
        return True
    if g > 140 and g > r + 40 and g > b + 40:
        return True
    if g > 200 and r < 120 and b < 120:
        return True
    return False


def _knockout_gen(cell: Image.Image) -> Image.Image:
    rgba = cell.convert("RGBA")
    w, h = rgba.size
    src = rgba.load()
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
            r, g, b, a = src[x, y]
            if _is_chroma_bg(r, g, b, a) and not vis[y][x]:
                vis[y][x] = True
                q.append((x, y))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not vis[ny][nx]:
                r, g, b, a = src[nx, ny]
                if _is_chroma_bg(r, g, b, a):
                    vis[ny][nx] = True
                    q.append((nx, ny))
    for y in range(h):
        for x in range(w):
            if not vis[y][x]:
                dst[x, y] = src[x, y]
    return _keep_largest_blob(out)


def _scrub_green_crumbs(im: Image.Image) -> Image.Image:
    arr = np.array(im.convert("RGBA"))
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    crumbs = (al > 20) & (g > 90) & (g > r + 10) & (g > b + 10) & (lum > 85)
    crumbs |= (al > 20) & (g > 140) & (g > r + 30) & (g > b + 30)
    arr[crumbs, 3] = 0
    return Image.fromarray(arr, "RGBA")


def _trim(im: Image.Image, pad: int = 8) -> Image.Image:
    bbox = im.split()[-1].getbbox()
    if not bbox:
        return im
    l, t, r, b = bbox
    return im.crop(
        (
            max(0, l - pad),
            max(0, t - pad),
            min(im.width, r + pad),
            min(im.height, b + pad),
        )
    )


def _fit_canvas(im: Image.Image, width: int, height: int) -> Image.Image:
    if im.height < 1:
        return Image.new("RGBA", (width, height), (0, 0, 0, 0))
    scale = height / float(im.height)
    nw = max(1, int(round(im.width * scale)))
    nh = height
    if nw > width - 8:
        scale = (width - 8) / float(im.width)
        nw = max(1, int(round(im.width * scale)))
        nh = max(1, int(round(im.height * scale)))
    scaled = im.resize((nw, nh), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    x = (width - nw) // 2
    y = height - nh
    canvas.alpha_composite(scaled, (max(0, x), max(0, y)))
    return canvas


def _source_path(i: int) -> Path | None:
    gen = SRC_DIR / f"{CHAR}_play_gen_{i}.png"
    if gen.is_file():
        return gen
    baked = OUT / f"{CHAR}_play_{i}.png"
    if baked.is_file():
        return baked
    return None


def rebuild_play() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    raw_frames: list[Image.Image] = []
    for i in range(N):
        path = _source_path(i)
        if path is None:
            break
        cut = _trim(_scrub_green_crumbs(_knockout_gen(Image.open(path))), pad=10)
        raw_frames.append(_scrub_green_crumbs(_fit_canvas(cut, TARGET_W, MASTER_H)))

    if len(raw_frames) <= SKIP_LEAD:
        raise FileNotFoundError("voyage play import incomplete — need gen or baked cels")

    frames = raw_frames[SKIP_LEAD:]
    for old in OUT.glob(f"{CHAR}_play_[0-9]*.png"):
        old.unlink()

    frames[0].save(OUT / f"{CHAR}_play.png")
    for i, fr in enumerate(frames):
        fr.save(OUT / f"{CHAR}_play_{i}.png")
        print("wrote", f"{CHAR}_play_{i}.png", fr.size)
    print("voyage play", len(frames), "cels (skipped squat", SKIP_LEAD, ")")
    return len(frames)


def main() -> int:
    n = rebuild_play()
    if n < 2:
        raise SystemExit("voyage play import incomplete")
    print("OK: voyage cheer-jump cels installed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
