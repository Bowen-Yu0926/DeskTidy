"""Import AI-generated voyage trash (揉纸丢进回收站) cels into assets/pets."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pets"
SRC_DIR = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
CHAR = "voyage"
N = 8
MASTER_H = 347
TARGET_W = 430


def _knockout_green(im: Image.Image) -> Image.Image:
    arr = np.array(im.convert("RGBA"))
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    green = (g > 140) & (g > r + 40) & (g > b + 40)
    green |= (g > 200) & (r < 120) & (b < 120)
    green |= (g > 180) & (r < 160) & (b < 160) & ((g - r) > 30) & ((g - b) > 30)
    # Checkerboard / near-white preview plate
    lum = (r + g + b) / 3.0
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    plate = (lum > 200) & (mx - mn < 28)
    arr[green | plate, 3] = 0
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


def rebuild_trash() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    frames: list[Image.Image] = []
    for i in range(N):
        path = SRC_DIR / f"voyage_trash_gen_{i}.png"
        if not path.is_file():
            raise FileNotFoundError(f"missing generated frame: {path}")
        cut = _trim(_knockout_green(Image.open(path)), pad=10)
        fitted = _fit_canvas(cut, TARGET_W, MASTER_H)
        frames.append(fitted)
        print("imported", i, "opaque", int((np.array(fitted)[:, :, 3] > 20).sum()))

    for old in OUT.glob(f"{CHAR}_trash*.png"):
        old.unlink()

    frames[0].save(OUT / f"{CHAR}_trash.png")
    for i, fr in enumerate(frames):
        fr.save(OUT / f"{CHAR}_trash_{i}.png")
        print("wrote", f"{CHAR}_trash_{i}.png", fr.size)
    return len(frames)


def main() -> None:
    n = rebuild_trash()
    if n < N:
        raise SystemExit("voyage trash import incomplete")
    print("OK: voyage trash cels installed")


if __name__ == "__main__":
    raise SystemExit(main())
