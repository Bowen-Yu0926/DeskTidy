"""Shared green-screen import for generated pet pose cels."""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pets"
ASSETS = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")


def knockout_green(im: Image.Image) -> Image.Image:
    arr = np.array(im.convert("RGBA"))
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    green = (g > 140) & (g > r + 40) & (g > b + 40)
    green |= (g > 200) & (r < 120) & (b < 120)
    green |= (g > 180) & (r < 160) & (b < 160) & ((g - r) > 30) & ((g - b) > 30)
    lum = (r + g + b) / 3.0
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    plate = (lum > 200) & (mx - mn < 28)
    arr[green | plate, 3] = 0
    return Image.fromarray(arr, "RGBA")


def trim(im: Image.Image, pad: int = 8) -> Image.Image:
    bbox = im.split()[-1].getbbox()
    if not bbox:
        return im
    l, t, r, b = bbox
    return im.crop(
        (max(0, l - pad), max(0, t - pad), min(im.width, r + pad), min(im.height, b + pad))
    )


def fit_canvas(im: Image.Image, width: int, height: int) -> Image.Image:
    if im.height < 1:
        return Image.new("RGBA", (width, height), (0, 0, 0, 0))
    scale = height / float(im.height)
    nw = max(1, int(round(im.width * scale)))
    nh = height
    if nw > width - 8:
        scale = (width - 8) / float(im.width)
        nw = max(1, int(round(im.width * scale)))
        nh = max(1, int(round(im.height * scale)))
    scaled = im.resize((nw, nh), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    x = (width - nw) // 2
    y = height - nh
    canvas.alpha_composite(scaled, (max(0, x), max(0, y)))
    return canvas


def silver_bin_bbox(im: Image.Image) -> tuple[int, int, int, int] | None:
    """Left-side galvanized bin bbox (soft silver ribs + bag cuff excluded)."""
    arr = np.array(im.convert("RGBA"))
    h, w = arr.shape[:2]
    if h < 8 or w < 8:
        return None
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    a = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    mx = np.maximum(np.maximum(r, g), b)
    mn = np.minimum(np.minimum(r, g), b)
    sat = mx - mn
    mask = (
        (a > 40)
        & (lum > 90)
        & (lum < 210)
        & (sat < 45)
        & (np.abs(r - g) < 30)
        & (np.abs(g - b) < 30)
    )
    # Bin is authored on the left; ignore cat fur highlights on the right.
    mask[:, int(w * 0.48) :] = False
    ys, xs = np.where(mask)
    if len(ys) < 400:
        return None
    return int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1


def fit_canvas_bin_planted(
    cuts: list[Image.Image],
    width: int,
    height: int,
    *,
    target_bin_h: int | None = None,
    foot_pad: int = 6,
    bin_left: int = 12,
) -> list[Image.Image]:
    """Uniform bin height + pinned bin foot — stops trash-can bob between cels.

    Mechanical canvas align only: scale/place so the silver bin stays planted.
    Does not invent motion or redesign the prop.
    """
    if not cuts:
        return []
    bboxes = [silver_bin_bbox(cut) for cut in cuts]
    if target_bin_h is None:
        target_bin_h = int(round(height * 0.68))
    target_bin_h = max(48, min(height - foot_pad - 8, int(target_bin_h)))

    frames: list[Image.Image] = []
    for cut, bb in zip(cuts, bboxes):
        if bb is None:
            frames.append(fit_canvas(cut, width, height))
            continue
        l, t, r, btm = bb
        bin_h = max(1, btm - t)
        scale = target_bin_h / float(bin_h)
        nw = max(1, int(round(cut.width * scale)))
        nh = max(1, int(round(cut.height * scale)))
        scaled = cut.resize((nw, nh), Image.Resampling.LANCZOS)
        sx = nw / float(cut.width)
        sy = nh / float(cut.height)
        bl = int(round(l * sx))
        bb_bot = int(round(btm * sy))
        x = int(bin_left - bl)
        y = int(height - foot_pad - bb_bot)
        # Clip overflow — never reshrink (width shrink used to bob the bin height).
        if y < 0:
            scaled = scaled.crop((0, -y, scaled.width, scaled.height))
            y = 0
        if x < 0:
            scaled = scaled.crop((-x, 0, scaled.width, scaled.height))
            x = 0
        if x + scaled.width > width:
            scaled = scaled.crop((0, 0, max(0, width - x), scaled.height))
        if y + scaled.height > height:
            scaled = scaled.crop((0, 0, scaled.width, max(0, height - y)))
        canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
        if scaled.width > 0 and scaled.height > 0:
            canvas.alpha_composite(scaled, (x, y))
        frames.append(canvas)
    return frames


def import_numbered(
    char: str,
    pose: str,
    n: int,
    *,
    master_h: int = 347,
    target_w: int = 430,
    plant_bin: bool = False,
) -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    cuts: list[Image.Image] = []
    for i in range(n):
        path = ASSETS / f"{char}_{pose}_gen_{i}.png"
        if not path.is_file():
            raise FileNotFoundError(path)
        cuts.append(trim(knockout_green(Image.open(path)), pad=10))
    if plant_bin:
        frames = fit_canvas_bin_planted(cuts, target_w, master_h)
    else:
        frames = [fit_canvas(cut, target_w, master_h) for cut in cuts]
    for old in OUT.glob(f"{char}_{pose}_[0-9]*.png"):
        old.unlink()
    frames[0].save(OUT / f"{char}_{pose}.png")
    for i, fr in enumerate(frames):
        fr.save(OUT / f"{char}_{pose}_{i}.png")
    print(char, pose, "frames", len(frames), frames[0].size, "plant_bin", plant_bin)
    return len(frames)
