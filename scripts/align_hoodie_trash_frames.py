"""Align hoodie trash cels to frame-0 foot anchor (bin + feet locked)."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pets"
CHAR = "hoodie"
BIN_X_MAX = 175


def _load() -> list[np.ndarray]:
    frames: list[np.ndarray] = []
    for i in range(64):
        path = OUT / f"{CHAR}_trash_{i}.png"
        if not path.is_file():
            break
        frames.append(np.array(Image.open(path).convert("RGBA")))
    if not frames:
        raise FileNotFoundError("no trash frames")
    return frames


def _foot_xy(im: np.ndarray) -> tuple[int, int]:
    sub = im[:, BIN_X_MAX:, :]
    ys, xs = np.where(sub[:, :, 3] > 24)
    if len(xs) == 0:
        return im.shape[1] // 2, im.shape[0] - 1
    # median foot x, bottom y
    return int(BIN_X_MAX + np.median(xs)), int(ys.max())


def _shift(im: np.ndarray, dx: int, dy: int) -> np.ndarray:
    h, w = im.shape[:2]
    out = np.zeros_like(im)
    x0 = max(0, dx)
    y0 = max(0, dy)
    x1 = min(w, w + dx)
    y1 = min(h, h + dy)
    sx0 = x0 - dx
    sy0 = y0 - dy
    out[y0:y1, x0:x1] = im[sy0 : sy0 + (y1 - y0), sx0 : sx0 + (x1 - x0)]
    return out


def align() -> int:
    frames = _load()
    ref_x, ref_y = _foot_xy(frames[0])
    aligned: list[Image.Image] = []
    for i, fr in enumerate(frames):
        fx, fy = _foot_xy(fr)
        shifted = _shift(fr, ref_x - fx, ref_y - fy)
        aligned.append(Image.fromarray(shifted, "RGBA"))
        print(i, "shift", ref_x - fx, ref_y - fy)
    for old in OUT.glob(f"{CHAR}_trash_[0-9]*.png"):
        old.unlink()
    aligned[0].save(OUT / f"{CHAR}_trash.png")
    for i, im in enumerate(aligned):
        im.save(OUT / f"{CHAR}_trash_{i}.png")
    return len(aligned)


def main() -> int:
    n = align()
    print("OK: aligned", n, "hoodie trash cels")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
