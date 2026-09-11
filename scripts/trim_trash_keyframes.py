"""Keep only even-index trash cels (original keyframes) — drop midpoint blends."""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pets"
CHARS = ("hoodie", "voyage")



def trim_keyframes(char: str) -> int:
    frames: list[Image.Image] = []
    for i in range(64):
        path = OUT / f"{char}_trash_{i}.png"
        if not path.is_file():
            break
        frames.append(Image.open(path).convert("RGBA"))
    if len(frames) <= 8:
        print(char, "trash already", len(frames), "cels — skip")
        return len(frames)
    keys = [frames[i] for i in range(0, len(frames), 2)]
    for old in OUT.glob(f"{char}_trash_[0-9]*.png"):
        old.unlink()
    keys[0].save(OUT / f"{char}_trash.png")
    for i, fr in enumerate(keys):
        fr.save(OUT / f"{char}_trash_{i}.png")
    print(char, "trash trimmed", len(frames), "->", len(keys), keys[0].size)
    return len(keys)


def main() -> int:
    for char in CHARS:
        trim_keyframes(char)
    print("OK: trash keyframes restored")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
