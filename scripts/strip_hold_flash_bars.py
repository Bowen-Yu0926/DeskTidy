"""Remove top/bottom flash paint bars from hoodie hold (and fall) sprites."""

from __future__ import annotations

import math
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PET = ROOT / "assets" / "pets"
OUT_DIR = ROOT.parent


def _row_is_flash_stroke(a: np.ndarray, y: int) -> bool:
    """Wide sparse horizontal stroke with gold or lavender mean color."""
    h, w = a.shape[:2]
    alpha = a[y, :, 3]
    op = alpha > 25
    n = int(op.sum())
    if n < 12:
        return False
    xs = np.where(op)[0]
    span = int(xs[-1] - xs[0] + 1)
    dens = n / float(span)
    if span < w * 0.28 or dens > 0.48 or n > w * 0.42:
        return False
    rgb = a[y, op, :3].astype(np.float32).mean(axis=0)
    r, g, b = float(rgb[0]), float(rgb[1]), float(rgb[2])
    is_gold = r > 160 and g > 120 and b < 165 and (r - b) > 40 and (g - b) > 18
    is_lavender = b > 155 and r > 140 and g > 120 and (b - g) > 8 and (b - r) > -15
    return is_gold or is_lavender


def _bright_gold(r: np.ndarray, g: np.ndarray, b: np.ndarray, al: np.ndarray) -> np.ndarray:
    # Distinctive AI gold brush (~254,225,129), not skin/hoodie warm tones.
    return (
        (al > 20)
        & (r > 220)
        & (g > 175)
        & (b < 160)
        & ((r - b) > 70)
        & ((g - b) > 40)
    )


def _lavender_speck(r: np.ndarray, g: np.ndarray, b: np.ndarray, al: np.ndarray) -> np.ndarray:
    return (
        (al > 15)
        & (b > 190)
        & (r > 170)
        & (g > 150)
        & (b >= g)
        & ((b - g) >= 8)
        & ((r + g + b) < 720)
    )


def strip_paint_bars(im: Image.Image) -> Image.Image:
    a = np.array(im.convert("RGBA"))
    h, w = a.shape[:2]
    r = a[:, :, 0].astype(np.int16)
    g = a[:, :, 1].astype(np.int16)
    b = a[:, :, 2].astype(np.int16)
    al = a[:, :, 3]

    wipe = np.zeros((h, w), dtype=bool)

    # 1) Clear whole-row flash strokes (the visible top/bottom flash lines).
    for y in range(h):
        if _row_is_flash_stroke(a, y):
            wipe[y] |= al[y] > 20

    # 2) Clear bright-gold crumbs in the upper band (jagged leftover tips).
    top_n = max(8, h // 5)
    wipe[:top_n] |= _bright_gold(r, g, b, al)[:top_n]

    # 3) Clear lavender speckles in the lower band.
    bot_n = max(10, h // 6)
    wipe[-bot_n:] |= _lavender_speck(r, g, b, al)[-bot_n:]

    # 4) Full-width lilac floor stroke (grab pose). Shoes are narrow + low (b-g).
    floor = (al > 20) & (b > 195) & ((b - g) > 25) & (r > 160)
    band0 = int(h * 0.78)
    for y in range(band0, h):
        op = al[y] > 20
        n = int(op.sum())
        if n < 8:
            continue
        xs = np.where(op)[0]
        span = int(xs[-1] - xs[0] + 1)
        if span >= int(w * 0.42) or int(floor[y].sum()) >= 24:
            wipe[y] |= floor[y]

    a[wipe, :] = 0
    out = Image.fromarray(a, "RGBA")
    bb = out.split()[-1].getbbox()
    return out.crop(bb) if bb else out


def flash_leftover(im: Image.Image) -> int:
    """Count pixels that still look like flash strokes (for assert)."""
    a = np.array(im.convert("RGBA"))
    h, w = a.shape[:2]
    count = 0
    for y in range(h):
        if _row_is_flash_stroke(a, y):
            count += int((a[y, :, 3] > 20).sum())
    r = a[:, :, 0].astype(np.int16)
    g = a[:, :, 1].astype(np.int16)
    b = a[:, :, 2].astype(np.int16)
    al = a[:, :, 3]
    top_n = max(8, h // 5)
    bot_n = max(10, h // 6)
    count += int(_bright_gold(r, g, b, al)[:top_n].sum())
    count += int(_lavender_speck(r, g, b, al)[-bot_n:].sum())
    return count


def on_canvas(im: Image.Image, width: int, height: int) -> Image.Image:
    c = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    bb = im.split()[-1].getbbox()
    if not bb:
        return c
    cropped = im.crop(bb)
    x = (width - cropped.width) // 2
    y = max(0, height - cropped.height - 2)
    c.alpha_composite(cropped, (max(0, x), y))
    return c


def hold_sway_frames(base: Image.Image, n: int = 6) -> list[Image.Image]:
    w, h = base.size
    pivot = (w * 0.50, h * 0.08)
    frames: list[Image.Image] = []
    for i in range(n):
        ang = 2.5 * math.sin(i / float(n) * math.pi * 2.0)
        pad = int(max(w, h) * 0.18) + 16
        work = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
        work.alpha_composite(base, (pad, pad))
        rotated = work.rotate(
            ang,
            resample=Image.Resampling.BICUBIC,
            center=(pivot[0] + pad, pivot[1] + pad),
            expand=False,
            fillcolor=(0, 0, 0, 0),
        )
        frames.append(rotated.crop((pad, pad, pad + w, pad + h)))
    return frames


def with_bg(frames: list[Image.Image], color=(248, 242, 228, 255)) -> list[Image.Image]:
    out: list[Image.Image] = []
    for frame in frames:
        bg = Image.new("RGBA", frame.size, color)
        bg.alpha_composite(frame)
        out.append(bg.convert("RGB").convert("P", palette=Image.Palette.ADAPTIVE, colors=160))
    return out


def _process_hold() -> list[Image.Image]:
    src = PET / "hoodie_hold_0.png"
    if not src.is_file():
        src = PET / "hoodie_hold.png"
    cleaned = strip_paint_bars(Image.open(src).convert("RGBA"))
    pad_x = max(36, cleaned.width // 10)
    base = on_canvas(cleaned, cleaned.width + pad_x * 2, 360)
    base = on_canvas(strip_paint_bars(base), base.width, base.height)
    sway = hold_sway_frames(base, 6)
    sway = [strip_paint_bars(f) for f in sway]
    mw = max(f.width for f in sway) + 8
    mh = 360
    sway = [on_canvas(f, mw, mh) for f in sway]
    sway = [strip_paint_bars(f) for f in sway]
    sway = [on_canvas(f, mw, mh) for f in sway]
    return sway


def _process_fall() -> list[Image.Image]:
    """Do not re-strip fall here — use rebuild_fall_from_gen.py for clean cels."""
    frames: list[Image.Image] = []
    for i in range(16):
        p = PET / f"hoodie_fall_{i}.png"
        if not p.is_file():
            break
        frames.append(Image.open(p).convert("RGBA"))
    return frames


def main() -> None:
    sway = _process_hold()
    for old in PET.glob("hoodie_hold*.png"):
        old.unlink()
    sway[0].save(PET / "hoodie_hold.png")
    for i, frame in enumerate(sway):
        frame.save(PET / f"hoodie_hold_{i}.png")

    leftover = [flash_leftover(f) for f in sway]
    print("hold flash leftover", leftover)
    assert max(leftover) < 40, leftover

    hold_gif = with_bg(sway)
    hold_gif[0].save(
        OUT_DIR / "hoodie_hold_clean.gif",
        save_all=True,
        append_images=hold_gif[1:],
        duration=120,
        loop=0,
    )
    print("hold ok", sway[0].size, "n", len(sway))


if __name__ == "__main__":
    main()
