"""Import AI-generated hoodie wait (趴着打游戏) cel into assets/pets.

Single static pose — prone on floor, no cushion/pillow (no multi-cel flip).

Identity lock (match hoodie_idle face/hair/glasses): plain WHITE hoodie with NO chest
logo / NO globe / NO PEOPLE print (rest poses keep fabric blank). Black pants, white
sneakers, round black/gold glasses, dark gray smartphone.
Body scale matches hoodie_idle opaque height. Bright green chroma key only on background.
"""

from __future__ import annotations

import sys
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

OUT = ROOT / "assets" / "pets"
SRC_DIR = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
CHAR = "hoodie"
N = 8
MASTER_H = 376  # match hoodie_idle canvas — prone feet-up needs full standing height
TARGET_W = 540  # prone pose: phone + raised feet + mirror headroom
WAIT_SIDE_PAD = 28
_MIN_OPAQUE = 8_000


def _is_screen_green(r: int, g: int, b: int, a: int = 255) -> bool:
    if a < 8:
        return True
    lum = (r + g + b) / 3.0
    mx, mn = max(r, g, b), min(r, g, b)
    if lum > 200 and mx - mn < 28:
        return True
    # Bright lime chroma key — not the dark green PEOPLE logo on white hoodie.
    if g > 150 and g > r + 45 and g > b + 45:
        return True
    if g > 200 and r < 120 and b < 120:
        return True
    return False


def _knockout_green(im: Image.Image) -> Image.Image:
    rgba = im.convert("RGBA")
    w, h = rgba.size
    src = rgba.load()
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dst = out.load()
    vis = [[False] * w for _ in range(h)]
    q: deque[tuple[int, int]] = deque()
    for coords in (
        [(i, 0) for i in range(w)],
        [(i, h - 1) for i in range(w)],
        [(0, j) for j in range(h)],
        [(w - 1, j) for j in range(h)],
    ):
        for x, y in coords:
            r, g, b, a = src[x, y]
            if _is_screen_green(r, g, b, a) and not vis[y][x]:
                vis[y][x] = True
                q.append((x, y))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not vis[ny][nx]:
                r, g, b, a = src[nx, ny]
                if _is_screen_green(r, g, b, a):
                    vis[ny][nx] = True
                    q.append((nx, ny))
    for y in range(h):
        for x in range(w):
            if not vis[y][x]:
                dst[x, y] = src[x, y]
    return out


def _scrub_green_halos(im: Image.Image) -> Image.Image:
    """Remove screen-green halos; keep PEOPLE logo green on white fabric."""
    arr = np.array(im.convert("RGBA"))
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    screen = (
        (al > 16)
        & (g > 130)
        & (g > r + 35)
        & (g > b + 35)
        & (lum > 100)
        & (lum < 245)
    )
    # Logo / print green sits on bright white — lower saturation than neon key.
    logo = (al > 40) & (r > 170) & (g > 60) & (g < 160) & (b < 140) & (g > r - 20)
    arr[screen & ~logo, 3] = 0
    return Image.fromarray(arr, "RGBA")


def _trim(im: Image.Image, pad: int = 10) -> Image.Image:
    bb = im.split()[-1].getbbox()
    if not bb:
        return im
    l, t, r, b = bb
    return im.crop(
        (max(0, l - pad), max(0, t - pad), min(im.width, r + pad), min(im.height, b + pad))
    )


def _fit_h(im: Image.Image, height: int) -> Image.Image:
    if im.height < 2:
        return Image.new("RGBA", (1, max(1, height)), (0, 0, 0, 0))
    scale = height / float(im.height)
    nw = max(1, int(round(im.width * scale)))
    return im.resize((nw, height), Image.Resampling.LANCZOS)


def _idle_body_height() -> int:
    idle_path = OUT / f"{CHAR}_idle.png"
    if not idle_path.is_file():
        return int(round(MASTER_H * 1.02))
    l, t, r, b = _opaque_bbox_pil(Image.open(idle_path))
    return max(1, b - t)


def _scale_bbox_height_to(im: Image.Image, target_h: int) -> Image.Image:
    """Uniform scale so opaque bbox height matches idle standing body."""
    l, t, r, b = _opaque_bbox_pil(im)
    h = b - t
    if h < 2:
        return im
    scale = target_h / float(h)
    nw = max(1, int(round(im.width * scale)))
    nh = max(1, int(round(im.height * scale)))
    return im.resize((nw, nh), Image.Resampling.LANCZOS)


def _opaque_bbox_pil(im: Image.Image) -> tuple[int, int, int, int]:
    bb = im.split()[-1].getbbox()
    return bb if bb else (0, 0, im.width, im.height)


def _content_span(im: Image.Image) -> int:
    l, t, r, b = _opaque_bbox_pil(im)
    return max(1, r - l, b - t)


def _scale_for_span(im: Image.Image, canvas_h: int, *, fill: float = 0.90) -> float:
    return (canvas_h * fill) / float(_content_span(im))


def _fit_canvas_probe(im: Image.Image, width: int, height: int) -> Image.Image:
    if im.height < 2:
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
    canvas.alpha_composite(scaled, (max(0, (width - nw) // 2), height - nh))
    return canvas


def _fit_canvas_foot(
    im: Image.Image,
    width: int,
    height: int,
    *,
    foot_x: int,
    foot_y: int,
    torso_x: int | None = None,
) -> Image.Image:
    if im.height < 2:
        return Image.new("RGBA", (width, height), (0, 0, 0, 0))
    scale = _scale_for_span(im, height)
    nw = max(1, int(round(im.width * scale)))
    nh = max(1, int(round(im.height * scale)))
    if nw > width - 8:
        scale = (width - 8) / float(im.width)
        nw = max(1, int(round(im.width * scale)))
        nh = max(1, int(round(im.height * scale)))
    scaled = im.resize((nw, nh), Image.Resampling.LANCZOS)
    l, t, r, b = _opaque_bbox_pil(scaled)
    foot_local_x = (l + r) / 2.0
    foot_local_y = float(b)
    torso_local_x = (l + r) / 2.0
    if torso_x is not None:
        x = int(round(torso_x - torso_local_x))
        y = int(round(foot_y - foot_local_y))
        x_foot = int(round(foot_x - foot_local_x))
        y_foot = int(round(foot_y - foot_local_y))
        if abs(y - y_foot) > 2:
            x, y = x_foot, y_foot
    else:
        x = int(round(foot_x - foot_local_x))
        y = int(round(foot_y - foot_local_y))
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    canvas.alpha_composite(scaled, (x, y))
    return canvas


def _bottom_align_canvas(
    im: Image.Image,
    width: int,
    height: int,
    *,
    bottom_pad: int = 2,
    side_pad: int = 8,
) -> Image.Image:
    """Seat opaque content on the canvas foot line (match sleep / idle)."""
    l, t, r, b = _opaque_bbox_pil(im)
    if b <= t:
        return im
    crop_l = max(0, l - side_pad)
    crop_t = max(0, t - side_pad)
    crop_r = min(im.width, r + side_pad)
    crop_b = min(im.height, b + side_pad)
    cropped = im.crop((crop_l, crop_t, crop_r, crop_b))
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    foot_x = (l + r) / 2.0
    x = int(round(foot_x - crop_l - cropped.width / 2.0))
    x = max(0, min(width - cropped.width, x))
    y = height - cropped.height - bottom_pad
    canvas.alpha_composite(cropped, (x, y))
    return canvas


def _shared_anchors(frames: list[Image.Image], width: int, height: int) -> tuple[int, int, int]:
    feet_x: list[float] = []
    feet_y: list[float] = []
    torso_xs: list[float] = []
    for im in frames:
        if im.height < 2:
            continue
        scale = _scale_for_span(im, height)
        nw = max(1, int(round(im.width * scale)))
        nh = max(1, int(round(im.height * scale)))
        if nw > width - 8:
            scale = (width - 8) / float(im.width)
            nw = max(1, int(round(im.width * scale)))
            nh = max(1, int(round(im.height * scale)))
        scaled = im.resize((nw, nh), Image.Resampling.LANCZOS)
        l, t, r, b = _opaque_bbox_pil(scaled)
        feet_x.append((l + r) / 2.0)
        feet_y.append(float(b))
        torso_xs.append((l + r) / 2.0)
    if not feet_x:
        return width // 2, height - 4, width // 2
    mid = len(feet_x) // 2
    feet_x.sort()
    feet_y.sort()
    torso_xs.sort()
    ax = max(24, min(width - 24, int(round(feet_x[mid]))))
    tx = max(24, min(width - 24, int(round(torso_xs[mid]))))
    ay = max(height // 2, min(height - 2, int(round(feet_y[mid]))))
    return ax, ay, tx


def _normalize_body_span(frames: list[Image.Image]) -> list[Image.Image]:
    spans = [_content_span(im) for im in frames]
    target = sorted(spans)[len(spans) // 2]
    out: list[Image.Image] = []
    for im, span in zip(frames, spans):
        if span < 8 or abs(span - target) < 4:
            out.append(im)
            continue
        scale = target / float(span)
        nw = max(1, int(round(im.width * scale)))
        nh = max(1, int(round(im.height * scale)))
        out.append(im.resize((nw, nh), Image.Resampling.LANCZOS))
    return out


def _opaque_count(im: Image.Image) -> int:
    return int((np.array(im.convert("RGBA"))[:, :, 3] > 24).sum())


def _green_fringe_count(im: Image.Image) -> int:
    arr = np.array(im.convert("RGBA"))
    a = arr[:, :, 3]
    return int(
        (
            (a > 20)
            & (arr[:, :, 1].astype(int) > arr[:, :, 0].astype(int) + 18)
            & (arr[:, :, 1].astype(int) > arr[:, :, 2].astype(int) + 18)
            & (arr[:, :, 1] > 130)
        ).sum()
    )


def _prepare_wait(path: Path) -> Image.Image:
    cut = _scrub_green_halos(_knockout_green(Image.open(path)))
    trimmed = _trim(cut, pad=14)
    # Torso height only — prone bbox includes raised feet and reads taller than idle.
    target = max(1, int(round(_idle_body_height() * 0.90)))
    return _scale_bbox_height_to(trimmed, target)


def rebuild_wait() -> int:
    """Pack one gaming-wait cel — held continuously (no 8-frame scene drift)."""
    OUT.mkdir(parents=True, exist_ok=True)
    path = SRC_DIR / "hoodie_wait_gen_0.png"
    if not path.is_file():
        raise FileNotFoundError(f"missing generated frame: {path}")
    prepared = _scrub_green_halos(_prepare_wait(path))
    frame = _scrub_green_halos(
        _bottom_align_canvas(prepared, TARGET_W, MASTER_H, side_pad=WAIT_SIDE_PAD)
    )
    if _opaque_count(frame) < _MIN_OPAQUE:
        raise RuntimeError(f"wait cel too empty after knockout: {path}")
    print(
        "imported",
        0,
        prepared.size,
        "opaque",
        _opaque_count(frame),
        "green",
        _green_fringe_count(frame),
    )

    gc = _green_fringe_count(frame)
    if gc > 120:
        print("warn: residual green on wait", gc)
    l, t, r, b = _opaque_bbox_pil(frame)
    print("body on canvas", r - l, "x", b - t, "foot_y", b)

    for old in OUT.glob(f"{CHAR}_wait_[0-9]*.png"):
        old.unlink()

    frame.save(OUT / f"{CHAR}_wait.png")
    frame.save(OUT / f"{CHAR}_wait_base.png")
    frame.save(OUT / f"{CHAR}_wait_0.png")
    print("wrote", f"{CHAR}_wait_0.png", frame.size, "(single held pose)")
    return 1


def main() -> int:
    n = rebuild_wait()
    if n < 1:
        raise SystemExit("hoodie wait import incomplete")
    print("OK: hoodie gaming-wait single cel installed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
