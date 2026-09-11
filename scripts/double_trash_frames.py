"""Double trash pose cels: insert 50% blends between consecutive keyframes.

Reads assets/pets/<char>_trash_0..N-1.png (uniform canvas) and writes 2N cels
(key, midpoint, key, midpoint, …). Mechanical in-betweening — not new poses.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pets"
CHARS = ("hoodie", "voyage")



def _load_keyframes(char: str) -> list[Image.Image]:
    frames: list[Image.Image] = []
    for i in range(64):
        path = OUT / f"{char}_trash_{i}.png"
        if not path.is_file():
            break
        frames.append(Image.open(path).convert("RGBA"))
    if len(frames) < 2:
        raise FileNotFoundError(f"{char}: need at least 2 trash keyframes, got {len(frames)}")
    sizes = {(f.width, f.height) for f in frames}
    if len(sizes) != 1:
        raise ValueError(f"{char}: trash keyframes must share one canvas size, got {sizes}")
    return frames


def _blend(a: Image.Image, b: Image.Image, t: float = 0.5) -> Image.Image:
    aa = np.array(a, dtype=np.float32)
    bb = np.array(b, dtype=np.float32)
    out = np.clip(aa * (1.0 - t) + bb * t, 0, 255).astype(np.uint8)
    return Image.fromarray(out, "RGBA")


def double_trash(char: str) -> int:
    keys = _load_keyframes(char)
    # If already doubled (even count > 8 with alternating similarity), still rebuild
    # from the first N/2 keyframes when N is a power-of-two expansion — keep simple:
    # only expand when we have the canonical 8-key import.
    if len(keys) == 16:
        # Re-expand from even indices (idempotent source = 8 keys).
        keys = [keys[i] for i in range(0, 16, 2)]
    elif len(keys) != 8:
        raise ValueError(f"{char}: expected 8 trash keyframes before doubling, got {len(keys)}")

    doubled: list[Image.Image] = []
    for i, key in enumerate(keys):
        doubled.append(key.copy())
        nxt = keys[min(i + 1, len(keys) - 1)]
        doubled.append(_blend(key, nxt, 0.5))

    for old in OUT.glob(f"{char}_trash_[0-9]*.png"):
        old.unlink()
    doubled[0].save(OUT / f"{char}_trash.png")
    for i, fr in enumerate(doubled):
        fr.save(OUT / f"{char}_trash_{i}.png")
    print(char, "trash doubled", len(keys), "->", len(doubled), doubled[0].size)
    return len(doubled)


def main() -> int:
    n = 0
    for char in CHARS:
        n = double_trash(char)
    print("OK: trash cels doubled to", n)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
