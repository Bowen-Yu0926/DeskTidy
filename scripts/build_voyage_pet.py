"""Build voyage (出海少女) chibi pet sprites from the user-provided boat avatar PNG."""

from __future__ import annotations

import math
from collections import deque
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pets"
ASSETS = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
IDLE_SRC = ASSETS / (
    "c__Users_baoxin.yu_AppData_Roaming_Cursor_User_workspaceStorage_"
    "d986ad0827732e40ed55fcc1b94b3ab9_images_chibi-girl-boat-avatar-"
    "9f029bc0-003e-48fa-a73d-92374be05926.png"
)

CHAR = "voyage"
MASTER_H = 360


def _is_bg(r: int, g: int, b: int, a: int = 255) -> bool:
    if a < 8:
        return True
    lum = (r + g + b) / 3.0
    mx, mn = max(r, g, b), min(r, g, b)
    # Turquoise water
    if b > 110 and g > 95 and r < 200 and b >= g - 5 and (b - r) > 5:
        return True
    # Sky / pale blue
    if b > 185 and g > 165 and r > 130 and b >= r - 10:
        return True
    # Cliff green
    if g > 85 and r < 160 and g > r + 8 and g > b:
        return True
    # Deck foam / pale highlights outside the sticker
    if lum > 210 and mx - mn < 35 and b > 180:
        return True
    return False


def _knockout(cell: Image.Image) -> Image.Image:
    rgb = cell.convert("RGB")
    w, h = rgb.size
    src = rgb.load()
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
            r, g, b = src[x, y]
            if _is_bg(r, g, b) and not vis[y][x]:
                vis[y][x] = True
                q.append((x, y))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not vis[ny][nx]:
                r, g, b = src[nx, ny]
                if _is_bg(r, g, b):
                    vis[ny][nx] = True
                    q.append((nx, ny))
    for y in range(h):
        for x in range(w):
            if not vis[y][x]:
                r, g, b = src[x, y]
                dst[x, y] = (r, g, b, 255)
    return _keep_largest_blob(out)


def _keep_largest_blob(im: Image.Image) -> Image.Image:
    alpha = im.split()[-1]
    w, h = im.size
    src_a = alpha.load()
    vis = [[False] * w for _ in range(h)]
    best: list[tuple[int, int]] = []
    for y in range(h):
        for x in range(w):
            if vis[y][x] or src_a[x, y] <= 8:
                continue
            blob: list[tuple[int, int]] = []
            q: deque[tuple[int, int]] = deque([(x, y)])
            vis[y][x] = True
            while q:
                cx, cy = q.popleft()
                blob.append((cx, cy))
                for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
                    if 0 <= nx < w and 0 <= ny < h and not vis[ny][nx] and src_a[nx, ny] > 8:
                        vis[ny][nx] = True
                        q.append((nx, ny))
            if len(blob) > len(best):
                best = blob
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dst = out.load()
    src = im.load()
    for x, y in best:
        dst[x, y] = src[x, y]
    return out


def _trim(im: Image.Image, pad: int = 10) -> Image.Image:
    bbox = im.split()[-1].getbbox()
    if not bbox:
        return im
    l, t, r, b = bbox
    return im.crop(
        (max(0, l - pad), max(0, t - pad), min(im.width, r + pad), min(im.height, b + pad))
    )


def _fit_h(im: Image.Image, height: int) -> Image.Image:
    scale = height / float(im.height)
    nw = max(1, int(im.width * scale))
    return im.resize((nw, height), Image.Resampling.LANCZOS)


def _canvas(im: Image.Image, width: int, height: int) -> Image.Image:
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    x = (width - im.width) // 2
    y = height - im.height
    canvas.alpha_composite(im, (max(0, x), max(0, y)))
    return canvas


def _prepare(path: Path, height: int = MASTER_H) -> Image.Image:
    cut = _knockout(Image.open(path))
    fitted = _fit_h(_trim(cut), height)
    pad_x = max(24, fitted.width // 12)
    return _canvas(fitted, fitted.width + pad_x, height + 16)


def _affine(
    im: Image.Image,
    *,
    scale_x: float = 1.0,
    scale_y: float = 1.0,
    dy: float = 0.0,
) -> Image.Image:
    w, h = im.size
    pad = int(max(w, h) * 0.2) + 8
    work = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    work.alpha_composite(im, (pad, pad))
    nw = max(1, int(work.width * scale_x))
    nh = max(1, int(work.height * scale_y))
    scaled = work.resize((nw, nh), Image.Resampling.LANCZOS)
    tmp = Image.new("RGBA", work.size, (0, 0, 0, 0))
    ox = (work.width - nw) // 2
    tmp.alpha_composite(scaled, (ox, work.height - nh))
    cropped = tmp.crop((pad, pad, pad + w, pad + h))
    if abs(dy) < 0.5:
        return cropped
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    shift = int(round(dy))
    if shift >= 0:
        out.alpha_composite(cropped.crop((0, 0, w, h - shift)), (0, shift))
    else:
        out.alpha_composite(cropped.crop((0, -shift, w, h)), (0, 0))
    return out


def _breath_frames(base: Image.Image, n: int = 4) -> list[Image.Image]:
    frames: list[Image.Image] = []
    for i in range(n):
        phase = math.sin(i / float(n) * math.pi * 2)
        frames.append(_affine(base, dy=0.9 * phase))
    return frames


def _walk_frames(base: Image.Image, n: int = 6) -> list[Image.Image]:
    frames: list[Image.Image] = []
    for i in range(n):
        t = i / float(n)
        bob = abs(math.sin(t * math.pi * 2)) * 5.0
        sy = 0.96 + 0.06 * abs(math.sin(t * math.pi * 2))
        sx = 1.04 - 0.04 * abs(math.sin(t * math.pi * 2))
        frames.append(_affine(base, scale_x=sx, scale_y=sy, dy=-bob))
    return frames


def _play_frames(base: Image.Image, n: int = 6) -> list[Image.Image]:
    frames: list[Image.Image] = []
    for i in range(n):
        t = i / float(n)
        hop = abs(math.sin(t * math.pi * 2)) * 10.0
        sy = 0.92 + 0.10 * abs(math.sin(t * math.pi * 2))
        sx = 1.06 - 0.08 * abs(math.sin(t * math.pi * 2))
        frames.append(_affine(base, scale_x=sx, scale_y=sy, dy=-hop))
    return frames


def _sleep_pose(base: Image.Image) -> Image.Image:
    w, h = base.size
    pad = int(max(w, h) * 0.22) + 12
    work = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
    work.alpha_composite(base, (pad, pad))
    rotated = work.rotate(
        -12.0,
        resample=Image.Resampling.BICUBIC,
        center=(work.width * 0.5, work.height * 0.88),
        fillcolor=(0, 0, 0, 0),
    )
    return rotated.crop((pad, pad, pad + w, pad + h))


def _sleep_frames(base: Image.Image, n: int = 4) -> list[Image.Image]:
    sleep = _sleep_pose(base)
    frames: list[Image.Image] = []
    for i in range(n):
        phase = math.sin(i / float(n) * math.pi * 2)
        frames.append(_affine(sleep, dy=0.5 * phase))
    return frames


def _wait_frames(base: Image.Image, n: int = 8) -> list[Image.Image]:
    """Gentle deck sway while looking at the horizon."""
    w, h = base.size
    pivot = (w * 0.50, h * 0.82)
    frames: list[Image.Image] = []
    for i in range(n):
        ang = 2.2 * math.sin(i / float(n) * math.pi * 2)
        pad = int(max(w, h) * 0.16) + 10
        work = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
        work.alpha_composite(base, (pad, pad))
        px = pivot[0] + pad
        py = pivot[1] + pad
        rotated = work.rotate(
            ang,
            resample=Image.Resampling.BICUBIC,
            center=(px, py),
            expand=False,
            fillcolor=(0, 0, 0, 0),
        )
        out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        out.alpha_composite(rotated.crop((pad, pad, pad + w, pad + h)), (0, 0))
        frames.append(out)
    return frames


def _hold_sway_frames(base: Image.Image, n: int = 6) -> list[Image.Image]:
    w, h = base.size
    pivot = (w * 0.50, h * 0.08)
    frames: list[Image.Image] = []
    for i in range(n):
        ang = 3.0 * math.sin(i / float(n) * math.pi * 2)
        pad = int(max(w, h) * 0.18) + 12
        work = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
        work.alpha_composite(base, (pad, pad))
        px = pivot[0] + pad
        py = pivot[1] + pad
        rotated = work.rotate(
            ang,
            resample=Image.Resampling.BICUBIC,
            center=(px, py),
            expand=False,
            fillcolor=(0, 0, 0, 0),
        )
        bob = int(round(1.2 * abs(math.sin(math.radians(ang * 8)))))
        out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        cropped = rotated.crop((pad, pad, pad + w, pad + h))
        if bob:
            out.alpha_composite(cropped.crop((0, 0, w, h - bob)), (0, bob))
        else:
            out.alpha_composite(cropped, (0, 0))
        frames.append(out)
    return frames


def _fall_frames(base: Image.Image, n: int = 3) -> list[Image.Image]:
    w, h = base.size
    pivot = (w * 0.50, h * 0.55)
    angles = (-10.0, 0.0, 10.0)[:n]
    frames: list[Image.Image] = []
    for ang in angles:
        pad = int(max(w, h) * 0.20) + 12
        work = Image.new("RGBA", (w + 2 * pad, h + 2 * pad), (0, 0, 0, 0))
        work.alpha_composite(base, (pad, pad))
        px = pivot[0] + pad
        py = pivot[1] + pad
        rotated = work.rotate(
            ang,
            resample=Image.Resampling.BICUBIC,
            center=(px, py),
            expand=False,
            fillcolor=(0, 0, 0, 0),
        )
        out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        out.alpha_composite(rotated.crop((pad, pad, pad + w, pad + h)), (0, 0))
        frames.append(out)
    return frames


def _trash_frames(base: Image.Image, n: int = 8) -> list[Image.Image]:
    frames: list[Image.Image] = []
    for i in range(n):
        t = i / max(1, n - 1)
        squash = 1.0 - 0.42 * t
        stretch = 1.0 + 0.18 * t
        dy = 8.0 * t
        frames.append(_affine(base, scale_x=stretch, scale_y=squash, dy=dy))
    return frames


def _save_pose(stem: str, base: Image.Image, frames: list[Image.Image]) -> None:
    base.save(OUT / f"{stem}.png")
    for i, fr in enumerate(frames):
        fr.save(OUT / f"{stem}_{i}.png")
    print(stem, base.size, "frames", len(frames))


def _purge(pattern: str) -> None:
    for old in OUT.glob(pattern):
        old.unlink()


def build() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    if not IDLE_SRC.is_file():
        raise FileNotFoundError(IDLE_SRC)

    idle = _prepare(IDLE_SRC, MASTER_H)
    stand = idle.copy()
    walk = idle.copy()

    _purge(f"{CHAR}_idle_*.png")
    idle.save(OUT / f"{CHAR}_idle.png")
    for i, fr in enumerate(_breath_frames(idle, 4)):
        fr.save(OUT / f"{CHAR}_idle_{i}.png")

    _purge(f"{CHAR}_stand_*.png")
    stand.save(OUT / f"{CHAR}_stand.png")
    for i, fr in enumerate(_breath_frames(stand, 4)):
        fr.save(OUT / f"{CHAR}_stand_{i}.png")

    _purge(f"{CHAR}_walk_*.png")
    walk.save(OUT / f"{CHAR}_walk.png")
    for i, fr in enumerate(_walk_frames(walk, 6)):
        fr.save(OUT / f"{CHAR}_walk_{i}.png")

    sleep_base = _sleep_pose(_prepare(IDLE_SRC, int(MASTER_H * 0.88)))
    _purge(f"{CHAR}_sleep_*.png")
    _save_pose(f"{CHAR}_sleep", sleep_base, _sleep_frames(sleep_base, 4))

    play_base = idle.copy()
    _purge(f"{CHAR}_play_*.png")
    _save_pose(f"{CHAR}_play", play_base, _play_frames(play_base, 6))

    wait_base = idle.copy()
    _purge(f"{CHAR}_wait_*.png")
    wait_base.save(OUT / f"{CHAR}_wait.png")
    for i, fr in enumerate(_wait_frames(wait_base, 8)):
        fr.save(OUT / f"{CHAR}_wait_{i}.png")
    print(f"{CHAR}_wait", wait_base.size, "frames", 8)

    hold_base = idle.copy()
    _purge(f"{CHAR}_hold_*.png")
    _save_pose(f"{CHAR}_hold", hold_base, _hold_sway_frames(hold_base, 6))

    fall_base = idle.copy()
    _purge(f"{CHAR}_fall_*.png")
    _save_pose(f"{CHAR}_fall", fall_base, _fall_frames(fall_base, 3))

    trash_base = idle.copy()
    _purge(f"{CHAR}_trash_*.png")
    _save_pose(f"{CHAR}_trash", trash_base, _trash_frames(trash_base, 8))

    # Hoodie-style grab + throw when generated art is present.
    try:
        from scripts.rebuild_voyage_hold_fall import rebuild_fall, rebuild_hold
        from scripts.import_voyage_trash_frames import rebuild_trash
        from scripts.import_voyage_wait_frames import rebuild_wait
        from scripts.import_voyage_play_frames import rebuild_play

        rebuild_hold()
        rebuild_fall()
        rebuild_trash()
        rebuild_wait()
        rebuild_play()
    except Exception as exc:
        print("hold/fall/trash/wait/play gen skipped:", exc)

    test = OUT / "_voyage_knockout_test.png"
    if test.is_file():
        test.unlink()
    print("voyage pet build OK")


if __name__ == "__main__":
    build()
