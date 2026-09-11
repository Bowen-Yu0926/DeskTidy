"""Import AI-generated voyage wait (坐下玩手机) cels into assets/pets.

Identity lock (match voyage_idle): white sneakers + white socks, orange life vest,
navy striped tank, purple sunglasses on vest, gold crescent necklace, dark gray iPhone.
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

from scripts.build_voyage_pet import _keep_largest_blob, _knockout  # noqa: E402

OUT = ROOT / "assets" / "pets"
SRC_DIR = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
CHAR = "voyage"
N = 8
MASTER_H = 360
TARGET_W = 430
_MIN_OPAQUE = 8_000


def _is_chroma_bg(r: int, g: int, b: int, a: int = 255) -> bool:
    if a < 8:
        return True
    lum = (r + g + b) / 3.0
    mx, mn = max(r, g, b), min(r, g, b)
    if lum > 200 and mx - mn < 28:
        return True
    # Bright lime chroma key
    if g > 140 and g > r + 40 and g > b + 40:
        return True
    if g > 200 and r < 120 and b < 120:
        return True
    # Sage / olive AI plate (common in voyage wait gens)
    if g >= 70 and g > r + 6 and g > b + 6 and 70 < lum < 215:
        return True
    return False


def _knockout_gen(cell: Image.Image) -> Image.Image:
    rgba = cell.convert("RGBA")
    if rgba.getextrema()[3][0] < 250:
        return _keep_largest_blob(_knockout(rgba))
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
            if _is_chroma_bg(r, g, b, a) and not vis[y][x]:
                vis[y][x] = True
                q.append((x, y))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not vis[ny][nx]:
                r, g, b, a = src[nx, ny]
                if _is_chroma_bg(r, g, b, a):
                    vis[ny][nx] = True
                    q.append((nx, ny))
    for y in range(h):
        for x in range(w):
            if not vis[y][x]:
                dst[x, y] = src[x, y]
    return _keep_largest_blob(out)


def _scrub_green_crumbs(im: Image.Image) -> Image.Image:
    """Remove chroma-key halos / green specks (major flicker source on desktop)."""
    arr = np.array(im.convert("RGBA"))
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    crumbs = (al > 20) & (g > r + 15) & (g > b + 15) & (g > 100) & (lum > 100) & (lum < 200)
    crumbs &= r <= g  # keep orange vest / warm skin
    halos = (al > 20) & (g > 120) & (g > r + 8) & (g > b + 8) & (lum > 90) & (lum < 230)
    halos &= ~( (r > 140) & (r > g) )  # keep orange vest
    arr[crumbs | halos, 3] = 0
    # Final pass: any leftover screen-green pixels (flicker on desktop).
    screen = (
        (al > 16)
        & (g > 90)
        & (g > r + 4)
        & (g > b + 4)
        & (lum > 85)
        & (lum < 245)
    )
    screen &= ~((r > 145) & (r > g))  # orange vest / warm skin
    arr[screen, 3] = 0
    # Defringe: green pixels touching transparency (edge halos flash on desktop).
    al2 = arr[:, :, 3]
    opaque = al2 > 24
    trans = ~opaque
    near_trans = np.zeros_like(opaque)
    h, w = opaque.shape
    for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
        shifted = np.roll(np.roll(trans, dy, axis=0), dx, axis=1)
        if dy == -1:
            shifted[-1, :] = False
        if dy == 1:
            shifted[0, :] = False
        if dx == -1:
            shifted[:, -1] = False
        if dx == 1:
            shifted[:, 0] = False
        near_trans |= shifted
    fringe = near_trans & opaque & screen
    arr[fringe, 3] = 0
    return Image.fromarray(arr, "RGBA")


def _strip_pastel_bars(im: Image.Image) -> Image.Image:
    a = np.array(im.convert("RGBA"))
    h, w = a.shape[:2]
    for y in list(range(0, max(1, h // 20))) + list(range(h - max(1, h // 12), h)):
        row = a[y]
        rgb = row[:, :3].astype(int)
        pastel = (
            (rgb[:, 0] > 180) & (rgb[:, 1] > 150) & (rgb[:, 2] > 140) & (rgb.max(1) - rgb.min(1) < 90)
        ) | ((rgb[:, 0] > 200) & (rgb[:, 1] > 170) & (rgb[:, 2] < 170))
        a[y, pastel, 3] = 0
    out = Image.fromarray(a, "RGBA")
    bb = out.split()[-1].getbbox()
    return out.crop(bb) if bb else out


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
    nw = max(1, int(im.width * scale))
    return im.resize((nw, height), Image.Resampling.LANCZOS)


def _opaque_bbox_pil(im: Image.Image) -> tuple[int, int, int, int]:
    a = im.split()[-1]
    bb = a.getbbox()
    return bb if bb else (0, 0, im.width, im.height)


def _fit_canvas(im: Image.Image, width: int, height: int) -> Image.Image:
    """Legacy single-frame fit — prefer _fit_canvas_foot for wait loops."""
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
    x = (width - nw) // 2
    y = height - nh
    canvas.alpha_composite(scaled, (max(0, x), max(0, y)))
    return canvas


def _scale_for_span(im: Image.Image, canvas_h: int, *, fill: float = 0.90) -> float:
    return (canvas_h * fill) / float(_content_span(im))


def _fit_canvas_foot(
    im: Image.Image,
    width: int,
    height: int,
    *,
    foot_x: int,
    foot_y: int,
    torso_x: int | None = None,
) -> Image.Image:
    """Place scaled cel: foot bottom + torso center locked (stops sideways flash)."""
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
        # Keep feet near anchor if torso lock would lift the sit off the floor.
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


def _shared_foot_anchor(frames: list[Image.Image], width: int, height: int) -> tuple[int, int, int]:
    """Median foot + torso anchors across cels."""
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
    feet_x.sort()
    feet_y.sort()
    torso_xs.sort()
    mid = len(feet_x) // 2
    ax = int(round(feet_x[mid]))
    ay = int(round(feet_y[mid]))
    tx = int(round(torso_xs[mid]))
    ax = max(24, min(width - 24, ax))
    tx = max(24, min(width - 24, tx))
    ay = max(height // 2, min(height - 2, ay))
    return ax, ay, tx


def _opaque_count(im: Image.Image) -> int:
    return int((np.array(im.convert("RGBA"))[:, :, 3] > 24).sum())


def _green_count(im: Image.Image) -> int:
    arr = np.array(im.convert("RGBA"))
    a = arr[:, :, 3]
    return int(
        (
            (a > 20)
            & (arr[:, :, 1].astype(int) > arr[:, :, 0].astype(int) + 18)
            & (arr[:, :, 1].astype(int) > arr[:, :, 2].astype(int) + 18)
        ).sum()
    )


def _prepare_wait(path: Path) -> Image.Image:
    cut = _scrub_green_crumbs(_knockout_gen(Image.open(path)))
    cut = _strip_pastel_bars(_trim(cut))
    return _fit_h(cut, int(round(MASTER_H * 1.04)))


def _repair_cel(im: Image.Image, fallback: Image.Image) -> Image.Image:
    """Never ship an empty cel or a blown-up green plate."""
    if _opaque_count(im) >= _MIN_OPAQUE and _green_count(im) < _opaque_count(im) // 2:
        return im
    print("repair wait cel from neighbor — opaque", _opaque_count(im), "green", _green_count(im))
    return fallback.copy()


def _content_span(im: Image.Image) -> int:
    l, t, r, b = _opaque_bbox_pil(im)
    return max(1, r - l, b - t)


def _normalize_body_height(frames: list[Image.Image]) -> list[Image.Image]:
    """Scale each cut so opaque content span matches the loop median (no size pops)."""
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


def rebuild_wait() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    frames: list[Image.Image] = []
    last_good = Image.new("RGBA", (TARGET_W, MASTER_H), (0, 0, 0, 0))
    for i in range(N):
        path = SRC_DIR / f"voyage_wait_gen_{i}.png"
        if not path.is_file():
            raise FileNotFoundError(f"missing generated frame: {path}")
        prepared = _prepare_wait(path)
        probe = _fit_canvas(prepared, TARGET_W, MASTER_H)
        if _opaque_count(probe) < _MIN_OPAQUE and _opaque_count(last_good) >= _MIN_OPAQUE:
            prepared = last_good.copy()
        elif _opaque_count(probe) < _MIN_OPAQUE or _green_count(probe) >= _opaque_count(probe) // 2:
            prepared = _repair_cel(probe, last_good)
        if _opaque_count(probe) >= _MIN_OPAQUE and _green_count(probe) < _opaque_count(probe) // 2:
            last_good = prepared
        frames.append(prepared)
        print("imported", i, prepared.size, "opaque", _opaque_count(probe), "green", _green_count(probe))

    frames = _normalize_body_height(frames)
    foot_x, foot_y, torso_x = _shared_foot_anchor(frames, TARGET_W, MASTER_H)
    frames = [
        _fit_canvas_foot(fr, TARGET_W, MASTER_H, foot_x=foot_x, foot_y=foot_y, torso_x=torso_x)
        for fr in frames
    ]
    frames = [_scrub_green_crumbs(_scrub_green_crumbs(fr)) for fr in frames]
    for i, fr in enumerate(frames):
        gc = _green_count(fr)
        if gc > 120:
            print("warn: residual green on wait", i, gc)
    print("anchors foot", foot_x, foot_y, "torso", torso_x)

    for old in OUT.glob(f"{CHAR}_wait_[0-9]*.png"):
        old.unlink()

    frames[0].save(OUT / f"{CHAR}_wait.png")
    for i, fr in enumerate(frames):
        fr.save(OUT / f"{CHAR}_wait_{i}.png")
        print("wrote", f"{CHAR}_wait_{i}.png", fr.size)
    return len(frames)


def main() -> int:
    n = rebuild_wait()
    if n < N:
        raise SystemExit("voyage wait import incomplete")
    print("OK: voyage phone-wait cels installed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
