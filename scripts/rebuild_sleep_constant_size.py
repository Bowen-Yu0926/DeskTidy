"""Rebuild hoodie sleep cels at constant size (no baked zzz / breath scale)."""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

OUT = ROOT / "assets" / "pets"
GEN = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
CHAR = "hoodie"
TARGET_W = 422
TARGET_H = 322


def _import_sleep_base() -> Image.Image:
    from scripts.import_wait_gen_frames import _knockout_green, _scrub_green_halos, _trim

    gen = GEN / f"{CHAR}_sleep_gen_0.png"
    if not gen.is_file():
        legacy = OUT / f"{CHAR}_sleep.png"
        if legacy.is_file():
            return Image.open(legacy).convert("RGBA")
        raise FileNotFoundError(gen)
    cut = _scrub_green_halos(_knockout_green(Image.open(gen)))
    cut = _trim(cut)
    scale = (TARGET_H * 0.92) / float(max(1, cut.height))
    nw = max(1, int(round(cut.width * scale)))
    nh = max(1, int(round(cut.height * scale)))
    if nw > TARGET_W - 8:
        scale = (TARGET_W - 8) / float(cut.width)
        nw = max(1, int(round(cut.width * scale)))
        nh = max(1, int(round(cut.height * scale)))
    scaled = cut.resize((nw, nh), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (TARGET_W, TARGET_H), (0, 0, 0, 0))
    x = (TARGET_W - nw) // 2
    y = TARGET_H - nh - 2
    canvas.alpha_composite(scaled, (x, y))
    return canvas


def main() -> None:
    base = _import_sleep_base()
    for old in OUT.glob(f"{CHAR}_sleep_*.png"):
        old.unlink()
    base.save(OUT / f"{CHAR}_sleep.png")
    for i in range(4):
        base.copy().save(OUT / f"{CHAR}_sleep_{i}.png")
        print("wrote", f"{CHAR}_sleep_{i}.png", base.size)
    print("sleep constant-size loop rebuilt (no baked zzz)")


if __name__ == "__main__":
    main()
