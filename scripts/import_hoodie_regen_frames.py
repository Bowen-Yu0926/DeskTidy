"""Import regenerated hoodie cels (green screen) into assets/pets.

Sources (Cursor assets/):
  hoodie_idle_regen_{0..3}.png
  hoodie_sleep_regen_{0..1}.png
  hoodie_wait_regen_0.png
  hoodie_hold_regen_{0..2}.png
  hoodie_trash_regen_{0..11}.png

Mechanical only: chroma knockout + green despill + canvas place.
Does NOT invent pose. Does NOT run aggressive hair regrade (source must be clean).
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.import_trash_gen_frames import (  # noqa: E402
    TARGET_W,
    MASTER_H,
    BOTTOM_PAD,
    TOP_MIN,
    _BODY_HEIGHT_MULT,
    _count_pale_hair_px,
    _fix_chroma_spill,
    _foot_xy,
    _harden_alpha,
    _idle_body_height,
    _keep_main_and_plane,
    _knockout_green,
    _place_on_canvas,
    _qa_crown_complete,
    _qa_feet_near_bottom,
    _qa_gen_green,
    _qa_gen_hair_headroom,
    _qa_hair_not_chalky,
    _scale_bbox_height_to,
    _scrub_green_halos,
    _scrub_hair_edge_halo,
    _scrub_residual_screen_green,
    _trim,
)

SRC = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
OUT = ROOT / "assets" / "pets"

# Canvas sizes matching current pets layout.
_IDLE_WH = (314, 376)
_SLEEP_WH = (422, 322)
_WAIT_WH = (540, 376)
_HOLD_WH = (444, 360)


def _prepare(path: Path, *, trash_plane: bool = False) -> Image.Image:
    _qa_gen_green(path)
    knocked = _scrub_hair_edge_halo(
        _fix_chroma_spill(
            _scrub_residual_screen_green(
                (
                    _keep_main_and_plane(_scrub_green_halos(_knockout_green(Image.open(path))))
                    if trash_plane
                    else _scrub_green_halos(_knockout_green(Image.open(path)))
                )
            )
        )
    )
    # Soft edge harden only — no full hair tone lock (regen source owns hair color).
    return _harden_alpha(knocked, cutoff=140)


def _place_standing(
    im: Image.Image,
    *,
    width: int,
    height: int,
    target_body_h: int,
    name: str,
    require_feet: bool = True,
) -> Image.Image:
    cut = _trim(im, pad=14)
    if require_feet:
        try:
            _qa_feet_near_bottom(cut, name=name)
        except RuntimeError as exc:
            print(f"WARN feet: {exc}")
    try:
        _qa_gen_hair_headroom(cut, name=name)
    except RuntimeError as exc:
        print(f"WARN headroom: {exc}")
    try:
        _qa_crown_complete(cut, name=name)
    except RuntimeError as exc:
        print(f"WARN crown: {exc}")
    # Ensure a few px transparent pad above hair before scale.
    pad_top = Image.new("RGBA", (cut.width, cut.height + 24), (0, 0, 0, 0))
    pad_top.alpha_composite(cut, (0, 24))
    cut = pad_top
    scaled = _scale_bbox_height_to(cut, target_body_h)
    fx, fy = _foot_xy(scaled)
    foot_x = width / 2.0
    foot_y = float(height - BOTTOM_PAD - 1)
    placed = _place_on_canvas(
        scaled, foot_x=foot_x, foot_y=foot_y, width=width, height=height
    )
    # Ensure headroom
    arr_top = 0
    from PIL import Image as _I
    import numpy as np

    a = np.array(placed)
    ys = (a[:, :, 3] >= 80).any(axis=1)
    if ys.any():
        arr_top = int(np.argmax(ys))
    if arr_top < TOP_MIN:
        # Nudge down slightly if clipped
        shift = TOP_MIN - arr_top
        canvas = _I.new("RGBA", (width, height), (0, 0, 0, 0))
        canvas.alpha_composite(placed, (0, min(shift, height // 8)))
        placed = canvas
    pale, white = _count_pale_hair_px(placed)
    print(f"  {name}: pale={pale} white={white} size={placed.size}")
    _qa_hair_not_chalky(placed, name=name)
    return placed


def _place_bbox_fit(
    im: Image.Image, *, width: int, height: int, name: str, pad: int = 10
) -> Image.Image:
    """Center opaque bbox in canvas (sleep / wait seated)."""
    cut = _trim(im, pad=12)
    # Scale to fit inside canvas with pad
    max_w, max_h = width - 2 * pad, height - 2 * pad
    scale = min(max_w / cut.width, max_h / cut.height)
    nw = max(1, int(round(cut.width * scale)))
    nh = max(1, int(round(cut.height * scale)))
    scaled = cut.resize((nw, nh), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    x = (width - nw) // 2
    y = (height - nh) // 2
    canvas.alpha_composite(scaled, (x, y))
    pale, white = _count_pale_hair_px(canvas)
    print(f"  {name}: pale={pale} white={white} size={canvas.size}")
    _qa_hair_not_chalky(canvas, name=name)
    return canvas


def import_idle() -> None:
    body_h = max(1, int(round(_idle_body_height() * 0.98)))
    # If replacing idle, measure from regen after knockout scale target from old idle.
    old = OUT / "hoodie_idle_0.png"
    if old.is_file():
        body_h = max(1, int(round(_idle_body_height() * 0.98)))
    w, h = _IDLE_WH
    for i in range(4):
        path = SRC / f"hoodie_idle_regen_{i}.png"
        if not path.is_file():
            raise SystemExit(f"missing {path}")
        prepared = _prepare(path)
        placed = _place_standing(
            prepared, width=w, height=h, target_body_h=body_h, name=f"idle_{i}"
        )
        placed.save(OUT / f"hoodie_idle_{i}.png")
    # aliases / play
    for alias, src in (
        ("hoodie_idle.png", "hoodie_idle_0.png"),
        ("hoodie_stand.png", "hoodie_idle_0.png"),
        ("hoodie_walk.png", "hoodie_idle_0.png"),
        ("hoodie_play.png", "hoodie_idle_0.png"),
        ("hoodie_play_0.png", "hoodie_idle_0.png"),
        ("hoodie_play_1.png", "hoodie_idle_1.png"),
        ("hoodie_play_2.png", "hoodie_idle_2.png"),
        ("hoodie_play_3.png", "hoodie_idle_3.png"),
    ):
        shutil.copyfile(OUT / src, OUT / alias)
        print(f"  mirror {alias}")


def import_sleep() -> None:
    w, h = _SLEEP_WH
    frames = []
    for i in range(2):
        path = SRC / f"hoodie_sleep_regen_{i}.png"
        if not path.is_file():
            raise SystemExit(f"missing {path}")
        frames.append(_place_bbox_fit(_prepare(path), width=w, height=h, name=f"sleep_{i}"))
    # 4 cels: alternate 0,1,0,1 for mild phone tilt loop
    seq = [frames[0], frames[1], frames[0], frames[1]]
    for i, im in enumerate(seq):
        im.save(OUT / f"hoodie_sleep_{i}.png")
    shutil.copyfile(OUT / "hoodie_sleep_0.png", OUT / "hoodie_sleep.png")
    print("  mirror hoodie_sleep.png")


def import_wait() -> None:
    w, h = _WAIT_WH
    path = SRC / "hoodie_wait_regen_0.png"
    if not path.is_file():
        raise SystemExit(f"missing {path}")
    placed = _place_bbox_fit(_prepare(path), width=w, height=h, name="wait_0")
    placed.save(OUT / "hoodie_wait_0.png")
    shutil.copyfile(OUT / "hoodie_wait_0.png", OUT / "hoodie_wait.png")
    shutil.copyfile(OUT / "hoodie_wait_0.png", OUT / "hoodie_wait_base.png")
    print("  mirror wait aliases")


def import_hold() -> None:
    w, h = _HOLD_WH
    body_h = max(1, int(round(h * 0.82)))
    frames = []
    for i in range(3):
        path = SRC / f"hoodie_hold_regen_{i}.png"
        if not path.is_file():
            raise SystemExit(f"missing {path}")
        prepared = _prepare(path)
        # Hold may not have feet at bottom — bbox fit safer
        frames.append(_place_bbox_fit(prepared, width=w, height=h, name=f"hold_{i}"))
    # Expand to 6 cels: 0,1,2,0,1,2
    seq = frames + frames
    for i, im in enumerate(seq):
        im.save(OUT / f"hoodie_hold_{i}.png")
    shutil.copyfile(OUT / "hoodie_hold_0.png", OUT / "hoodie_hold.png")
    print("  mirror hoodie_hold.png")


def import_trash() -> None:
    """Reuse trash canvas placer via gen filename bridge."""
    for i in range(12):
        src = SRC / f"hoodie_trash_regen_{i}.png"
        dst = SRC / f"hoodie_trash_gen_{i}.png"
        if not src.is_file():
            raise SystemExit(f"missing {src}")
        shutil.copyfile(src, dst)
        print(f"  bridge {src.name} -> {dst.name}")
    # Patch prepare temporarily: skip hair lock by monkeypatching
    import scripts.import_trash_gen_frames as m

    def _prepare_no_lock(path: Path, target_body_h: int) -> Image.Image:
        m._qa_gen_green(path)
        knocked = m._fix_chroma_spill(
            m._scrub_residual_screen_green(
                m._keep_main_and_plane(
                    m._scrub_green_halos(m._knockout_green(Image.open(path)))
                )
            )
        )
        m._qa_feet_near_bottom(knocked, name=path.name)
        m._qa_gen_hair_headroom(knocked, name=path.name)
        try:
            m._qa_crown_complete(knocked, name=path.name)
        except RuntimeError as exc:
            print(f"WARN {exc}")
        hard = m._harden_alpha(knocked)
        cleaned = m._scrub_hair_edge_halo(m._fix_chroma_spill(hard))
        cut = m._trim(cleaned, pad=14)
        return m._scale_bbox_height_to(cut, target_body_h)

    m._prepare_frame = _prepare_no_lock
    rc = m.main()
    if rc != 0:
        raise SystemExit(f"trash import failed rc={rc}")


def main() -> int:
    print("=== idle ===")
    import_idle()
    print("=== sleep ===")
    import_sleep()
    print("=== wait ===")
    import_wait()
    print("=== hold ===")
    import_hold()
    print("=== trash ===")
    import_trash()
    print("OK: regen import complete")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
