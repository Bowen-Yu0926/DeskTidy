"""QA chroma green + white-hoodie torso for hoodie_*_regen_*.png gens."""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image

SRC = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")


def main() -> int:
    for kind, n in (("hold", 3), ("trash", 12)):
        for i in range(n):
            p = SRC / f"hoodie_{kind}_regen_{i}.png"
            if not p.is_file():
                print("MISSING", p.name)
                continue
            im = np.asarray(Image.open(p).convert("RGB"))
            h, w, _ = im.shape
            # Full-frame green screen share
            gdom = (
                (im[:, :, 1].astype(int) > im[:, :, 0].astype(int) + 40)
                & (im[:, :, 1].astype(int) > im[:, :, 2].astype(int) + 40)
                & (im[:, :, 1] > 120)
            )
            # Torso patch (avoid hair / plane)
            patch = im[int(h * 0.42) : int(h * 0.68), int(w * 0.32) : int(w * 0.68)]
            g_r = (
                patch[:, :, 1].astype(int) - patch[:, :, 0].astype(int)
            ).mean()
            lum = float(patch.mean())
            flag = "GREENISH" if g_r > 22 else "ok"
            print(
                f"{p.name}: screen_green%={100*gdom.mean():.0f} "
                f"torso_g-r={g_r:.1f} lum={lum:.0f} {flag}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
