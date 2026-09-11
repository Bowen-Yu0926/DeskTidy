"""Import ONLY regenerated trash throw cels into assets/pets.

Copies hoodie_trash_regen_{i}.png → hoodie_trash_gen_{i}.png then runs
import_trash_gen_frames with hair-lock skipped (source owns hair color).
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

SRC = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
N = 12


def main() -> int:
    import scripts.import_trash_gen_frames as m

    for i in range(N):
        src = SRC / f"hoodie_trash_regen_{i}.png"
        dst = SRC / f"hoodie_trash_gen_{i}.png"
        if not src.is_file():
            raise SystemExit(f"missing {src}")
        shutil.copyfile(src, dst)
        print(f"bridge {src.name} -> {dst.name}")

    def _prepare_no_lock(path: Path, target_body_h: int) -> Image.Image:
        m._qa_gen_green(path)
        knocked = m._fix_chroma_spill(
            m._scrub_residual_screen_green(
                m._keep_main_and_plane(
                    m._scrub_green_halos(m._knockout_green(Image.open(path)))
                )
            )
        )
        try:
            m._qa_feet_near_bottom(knocked, name=path.name)
        except RuntimeError as exc:
            print(f"WARN feet: {exc}")
        try:
            m._qa_gen_hair_headroom(knocked, name=path.name)
        except RuntimeError as exc:
            print(f"WARN headroom: {exc}")
        try:
            m._qa_crown_complete(knocked, name=path.name)
        except RuntimeError as exc:
            print(f"WARN crown: {exc}")
        # Harden first, then full hair lock (matte fringe + highlight darken).
        hard = m._harden_alpha(knocked)
        cleaned = m._lock_sprite_hair(m._fix_chroma_spill(hard))
        cut = m._trim(cleaned, pad=14)
        return m._scale_bbox_height_to(cut, target_body_h)

    m._prepare_frame = _prepare_no_lock
    return int(m.main())


if __name__ == "__main__":
    raise SystemExit(main())
