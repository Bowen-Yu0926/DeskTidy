"""Rebuild hoodie wait (看报纸): left↔right page turn without a flashing slab.

Keep the open newspaper pixels intact. Animate by:
  - a soft shadow that sweeps across one half (reads as the page lifting)
  - a thin crease (page edge) that travels from the free edge to the spine
  - a lightly rotated paper-only overlay (clipped to the newspaper mask)

Never fill a half with flat paper colour — that was the middle grey flash.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pets"
CHAR = "hoodie"
N_FRAMES = 8


def _strip_foot_junk(im: Image.Image) -> Image.Image:
    """Clear pink/lilac GIF crumbs near the chair feet."""
    arr = np.array(im.convert("RGBA"))
    h, w = arr.shape[:2]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    a = arr[:, :, 3]
    zone = np.zeros((h, w), dtype=bool)
    zone[int(h * 0.58) :, : int(w * 0.26)] = True
    zone[int(h * 0.74) :, int(w * 0.74) :] = True
    wood = (
        (a > 40)
        & (r > 55)
        & (r < 205)
        & (g > 35)
        & (g < 165)
        & (b < 125)
        & ((r - b) > 12)
        & ((r - g) < 60)
    )
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    junk = zone & (a > 10) & (~wood) & (
        ((r > 165) & (b > 135) & (chroma > 10))
        | ((r > 185) & (g > 155) & (b > 155) & ((r - g) > 2))
        | ((a > 10) & (a < 100) & (chroma > 6))
    )
    arr[junk, 3] = 0
    return Image.fromarray(arr, "RGBA")


def _paper_mask(arr: np.ndarray) -> np.ndarray:
    h, w = arr.shape[:2]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    a = arr[:, :, 3]
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    paper = (
        (a > 50)
        & (r > 135)
        & (g > 135)
        & (b > 120)
        & (chroma < 60)
        & ((r.astype(np.int32) + g + b) > 410)
    )
    y0, y1 = int(h * 0.38), int(h * 0.72)
    paper[:y0] = False
    paper[y1:] = False
    paper[:, : int(w * 0.08)] = False
    paper[:, int(w * 0.92) :] = False
    return paper


def _news_bbox(mask: np.ndarray) -> tuple[int, int, int, int]:
    ys, xs = np.where(mask)
    if len(xs) == 0:
        raise RuntimeError("newspaper mask empty")
    return int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())


def _ease(t: float) -> float:
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def _turn_half(
    base: Image.Image,
    *,
    side: str,
    amount: float,
    bbox: tuple[int, int, int, int],
    full_mask: np.ndarray,
) -> Image.Image:
    """Turn one half toward the spine. ``amount`` 0=open … 1=nearly folded."""
    amount = _ease(max(0.0, min(1.0, amount)))
    if amount < 0.04:
        return base.copy()

    base_arr = np.array(base.convert("RGBA"))
    l, t, r, b = bbox
    fold = (l + r) // 2
    if side == "right":
        x0, x1 = fold, r + 1
        angle = -9.0 * amount
    else:
        x0, x1 = l, fold + 1
        angle = 9.0 * amount

    # 1) Soft shadow on the turning half — darken existing paper only.
    shade = base_arr.copy()
    local = full_mask[t : b + 1, x0:x1]
    half = shade[t : b + 1, x0:x1].copy()
    pw = max(1, x1 - x0)
    xs = np.linspace(0.0, 1.0, pw, dtype=np.float32)
    grad = xs if side == "right" else (1.0 - xs)
    # Stronger near free edge as the page lifts.
    strength = (0.08 + 0.42 * amount) * (grad ** 1.15)[None, :]
    for c in range(3):
        ch = half[:, :, c].astype(np.float32)
        ch = ch * (1.0 - strength) + ch * 0.45 * strength
        half[:, :, c] = np.clip(ch, 0, 255).astype(np.uint8)
    region = shade[t : b + 1, x0:x1]
    region[local] = half[local]
    shade[t : b + 1, x0:x1] = region
    out = Image.fromarray(shade, "RGBA")

    # 2) Moving crease (page edge) from free edge → spine.
    crease = Image.new("RGBA", base.size, (0, 0, 0, 0))
    cp = crease.load()
    if side == "right":
        cx = int(round(r - amount * 0.92 * (r - fold)))
    else:
        cx = int(round(l + amount * 0.92 * (fold - l)))
    alpha = int(70 + 110 * amount)
    span = max(1, int(1 + 2.5 * amount))
    hh, ww = full_mask.shape
    for yy in range(t, min(b + 1, hh)):
        for dx in range(-span, span + 1):
            xx = cx + dx
            if 0 <= xx < ww and full_mask[yy, xx]:
                a = max(0, alpha - abs(dx) * 40)
                # Slight highlight on the spine side of the crease.
                if (side == "right" and dx < 0) or (side == "left" and dx > 0):
                    cp[xx, yy] = (235, 235, 230, min(180, a + 40))
                else:
                    cp[xx, yy] = (40, 40, 45, a)
    crease = crease.filter(ImageFilter.GaussianBlur(radius=0.55))
    out = Image.alpha_composite(out, crease)

    # 3) Mild rotate overlay of paper-only pixels (lifted sheet feel).
    page = base.crop((x0, t, x1, b + 1)).convert("RGBA")
    mask_im = Image.fromarray(
        (full_mask[t : b + 1, x0:x1].astype(np.uint8) * 255), "L"
    )
    page_arr = np.array(page)
    page_arr[np.array(mask_im) < 128, 3] = 0
    page = Image.fromarray(page_arr, "RGBA")
    rotated = page.rotate(
        angle,
        resample=Image.Resampling.BICUBIC,
        expand=False,
        fillcolor=(0, 0, 0, 0),
    )
    rot_arr = np.array(rotated)
    # Peak opacity mid-lift; stay translucent so under-print never becomes a slab.
    fade = 0.22 + 0.38 * amount
    rot_arr[:, :, 3] = (rot_arr[:, :, 3].astype(np.float32) * fade).astype(np.uint8)
    rotated = Image.fromarray(rot_arr, "RGBA")
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    layer.paste(rotated, (x0, t), rotated)
    layer_arr = np.array(layer)
    layer_arr[~full_mask, 3] = 0
    out = Image.alpha_composite(out, Image.fromarray(layer_arr, "RGBA"))

    out_arr = np.array(out)
    out_arr[~full_mask] = base_arr[~full_mask]
    return Image.fromarray(out_arr, "RGBA")


def build_frames(base: Image.Image) -> list[Image.Image]:
    base = _strip_foot_junk(base.convert("RGBA"))
    arr = np.array(base)
    mask = _paper_mask(arr)
    bbox = _news_bbox(mask)
    l, t, r, b = bbox
    inset_x = max(5, (r - l) // 16)
    inset_y = max(3, (b - t) // 14)
    bbox = (l + inset_x, t + inset_y, r - inset_x, b - inset_y)

    # Rest → turn right page in → settle → rest → turn left page in → settle.
    plan: list[tuple[str | None, float]] = [
        (None, 0.0),
        ("right", 0.32),
        ("right", 0.78),
        ("right", 0.28),
        (None, 0.0),
        ("left", 0.32),
        ("left", 0.78),
        ("left", 0.28),
    ]
    frames: list[Image.Image] = []
    for side, amt in plan:
        if side is None:
            frames.append(base.copy())
        else:
            frames.append(
                _turn_half(
                    base,
                    side=side,
                    amount=amt,
                    bbox=bbox,
                    full_mask=mask,
                )
            )
    return frames


def seed_base_from_source() -> Image.Image:
    """Re-knockout wait cel from the newspaper GIF (clean rest pose)."""
    import sys

    if str(ROOT) not in sys.path:
        sys.path.insert(0, str(ROOT))
    from scripts.build_hoodie_pet import (
        MASTER_H,
        WAIT_SRC,
        _prepare,
        _remove_newspaper_seam,
    )

    if not WAIT_SRC.is_file():
        raise SystemExit(f"missing WAIT_SRC: {WAIT_SRC}")
    wait = _prepare(WAIT_SRC, int(MASTER_H * 0.92), keep_largest=False)
    wait = _remove_newspaper_seam(wait)
    wait = _strip_foot_junk(wait)
    clean = OUT / f"{CHAR}_wait_base.png"
    wait.save(clean)
    return wait


def main() -> None:
    clean_path = OUT / f"{CHAR}_wait_base.png"
    # Always re-seed from source so we never start from a mid-flip / dirty cel.
    base = seed_base_from_source()
    frames = build_frames(base)
    assert len(frames) == N_FRAMES
    for old in OUT.glob(f"{CHAR}_wait_[0-9]*.png"):
        old.unlink()
    frames[0].save(OUT / f"{CHAR}_wait.png")
    for i, fr in enumerate(frames):
        fr.save(OUT / f"{CHAR}_wait_{i}.png")
        print("wrote", f"{CHAR}_wait_{i}.png", fr.size)
    print("wait newspaper flip rebuilt:", N_FRAMES, "frames", "base", clean_path.name)


if __name__ == "__main__":
    main()
