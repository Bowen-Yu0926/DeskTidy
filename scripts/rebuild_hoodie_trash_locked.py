"""Hoodie trash: paper-plane fold & throw — feet at canvas center + crisp alpha."""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
OUT = ROOT / "assets" / "pets"
CHAR = "hoodie"
# Keep wait-like soft AA — binary cutoff 210 made hoodie edges look jagged/虚.
_ALPHA_CUTOFF = 48


def _load_raw() -> list[np.ndarray]:
    frames: list[np.ndarray] = []
    for i in range(64):
        path = OUT / f"{CHAR}_trash_{i}.png"
        if not path.is_file():
            break
        frames.append(np.array(Image.open(path).convert("RGBA")))
    if len(frames) < 2:
        raise FileNotFoundError("hoodie trash frames missing")
    return frames


def _opaque_bbox(im: Image.Image) -> tuple[int, int, int, int]:
    bb = im.split()[-1].getbbox()
    return bb if bb else (0, 0, im.width, im.height)


def _foot_xy(im: Image.Image) -> tuple[float, float]:
    """Body foot anchor — ignore detached plane / soft purple ground shadow."""
    arr = np.array(im.convert("RGBA"))
    al = arr[:, :, 3]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    # Soft purple oval under feet must not pull the sole baseline.
    shadow = (
        (al >= 20)
        & (al < 200)
        & (r > 70)
        & (r < 200)
        & (b > g)
        & (b >= r - 20)
        & (g < 165)
    )
    if shadow.any():
        masked = arr.copy()
        masked[shadow, 3] = 0
        im_body = Image.fromarray(masked, "RGBA")
    else:
        im_body = im
    try:
        from scripts.import_trash_gen_frames import _largest_blob_bbox

        l, t, rgt, btm = _largest_blob_bbox(im_body)
    except Exception:
        l, t, rgt, btm = _opaque_bbox(im_body)
    if btm - t < 2:
        l, t, rgt, btm = _opaque_bbox(im)
    return (l + rgt) / 2.0, float(btm)


def _harden_alpha(im: Image.Image, *, cutoff: int = _ALPHA_CUTOFF) -> Image.Image:
    """Keep wait-like soft AA — only punch near-clear stubs."""
    arr = np.array(im.convert("RGBA"), copy=True)
    al = arr[:, :, 3]
    arr[:, :, 3] = np.where(al >= cutoff, al, 0).astype(np.uint8)
    mid = (arr[:, :, 3] >= cutoff) & (arr[:, :, 3] < 220)
    arr[mid, 3] = np.maximum(arr[mid, 3], 200)
    return Image.fromarray(arr, "RGBA")


def _realign_to_foot(im: Image.Image, foot_x: float, foot_y: float) -> Image.Image:
    """Shift cel so opaque foot lands on the shared canvas anchor."""
    fx, fy = _foot_xy(im)
    dx = int(round(foot_x - fx))
    dy = int(round(foot_y - fy))
    if dx == 0 and dy == 0:
        return im
    canvas = Image.new("RGBA", im.size, (0, 0, 0, 0))
    canvas.alpha_composite(im, (dx, dy))
    return canvas


def _postprocess_frame(im: Image.Image, foot_x: float, foot_y: float) -> Image.Image:
    """Harden alpha, realign feet, scrub chroma fringe, then re-seal silhouette ink.

    LANCZOS place / older rebuilds leave pale/green RGB on binary-alpha edges
    (reads as 绿屏虚). Scrub + seal must run *after* the last resample/realign.
    """
    from scripts.import_trash_gen_frames import (
        _fix_chroma_spill,
        _ensure_foot_shadow,
        _lock_sprite_hair,
        _scrub_green_halos,
    )

    im = _harden_alpha(im)
    for _ in range(2):
        im = _realign_to_foot(im, foot_x, foot_y)
    from scripts.import_trash_gen_frames import _scrub_pale_edge_fringe

    im = _fix_chroma_spill(_scrub_pale_edge_fringe(_scrub_green_halos(im)))
    im = _ensure_foot_shadow(_lock_sprite_hair(im))
    im = _harden_alpha(im, cutoff=_ALPHA_CUTOFF)
    # Sole unify / shadow run inside lock — re-pin feet to the shared anchor.
    for _ in range(2):
        im = _realign_to_foot(im, foot_x, foot_y)
    return im


def rebuild_from_imported() -> int:
    raw = _load_raw()
    # Feet at horizontal canvas center so runtime can center-draw (no foot math).
    w = int(raw[0].shape[1])
    foot_x = w / 2.0
    ref = Image.fromarray(raw[0].copy(), "RGBA")
    _fx, foot_y = _foot_xy(ref)
    composed = [
        _postprocess_frame(Image.fromarray(fr.copy(), "RGBA"), foot_x, foot_y) for fr in raw
    ]
    for old in OUT.glob(f"{CHAR}_trash_[0-9]*.png"):
        old.unlink()
    composed[0].save(OUT / f"{CHAR}_trash.png")
    for i, im in enumerate(composed):
        im.save(OUT / f"{CHAR}_trash_{i}.png")
        fx, fy = _foot_xy(im)
        print("wrote", f"{CHAR}_trash_{i}.png", "foot", (round(fx, 1), round(fy, 1)))
    try:
        from src.ui.pet_anim import invalidate_pet_pose_cache

        invalidate_pet_pose_cache(CHAR)
    except Exception:
        pass
    return len(composed)


def main() -> int:
    from scripts.import_trash_gen_frames import main as import_frames

    if import_frames() != 0:
        return 1
    n = rebuild_from_imported()
    return 0 if n >= 2 else 1


if __name__ == "__main__":
    raise SystemExit(main())
