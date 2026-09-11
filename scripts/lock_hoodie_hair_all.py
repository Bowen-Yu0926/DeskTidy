"""Lock chalk-white hair highlights across all hoodie pet sprites.

Uses the same mechanical hair regrade as trash import (no pose inventing).
Run after regenerating trash, or when idle/wait/sleep still flash white tips
on light wallpaper.
"""

from __future__ import annotations

import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.import_trash_gen_frames import (  # noqa: E402
    _count_pale_hair_px,
    _lock_sprite_hair,
)

OUT = ROOT / "assets" / "pets"
# Alias masters that should mirror frame 0 after lock.
_MIRROR = {
    "hoodie_idle.png": "hoodie_idle_0.png",
    "hoodie_play.png": "hoodie_play_0.png",
    "hoodie_stand.png": "hoodie_idle_0.png",
    "hoodie_walk.png": "hoodie_idle_0.png",
    "hoodie_sleep.png": "hoodie_sleep_0.png",
    "hoodie_wait.png": "hoodie_wait_0.png",
    "hoodie_wait_base.png": "hoodie_wait_0.png",
    "hoodie_trash.png": "hoodie_trash_0.png",
    "hoodie_hold.png": "hoodie_hold_0.png",
    "hoodie_fall.png": "hoodie_fall_0.png",
}


def main() -> int:
    paths = sorted(
        p
        for p in OUT.glob("hoodie_*.png")
        if p.is_file() and "gen" not in p.name and "sheet" not in p.name
    )
    if not paths:
        raise SystemExit(f"no hoodie sprites under {OUT}")
    changed = 0
    for path in paths:
        if path.name in _MIRROR:
            continue  # rewrite aliases after frame_0
        before = Image.open(path).convert("RGBA")
        bp, bw = _count_pale_hair_px(before)
        # One pass only — double-lock was darkening white hoodie fabric.
        after = _lock_sprite_hair(before)
        ap, aw = _count_pale_hair_px(after)
        after.save(path)
        changed += 1
        print(f"{path.name}: pale {bp}->{ap}  white {bw}->{aw}")
    for alias, src_name in _MIRROR.items():
        src = OUT / src_name
        dst = OUT / alias
        if src.is_file():
            Image.open(src).convert("RGBA").save(dst)
            print(f"mirror {alias} <- {src_name}")
    print(f"OK: locked {changed} hoodie sprites")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
