"""Build hoodie chibi pet sprites from user-provided idle PNG + sleep GIF."""

from __future__ import annotations

import math
from collections import deque
from pathlib import Path

from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "assets" / "pets"
ASSETS = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")

IDLE_SRC = ASSETS / (
    "c__Users_baoxin.yu_AppData_Roaming_Cursor_User_workspaceStorage_"
    "d986ad0827732e40ed55fcc1b94b3ab9_images_image-1b9286ce-9002-4158-9ec0-f2aba71fe77b.png"
)
SLEEP_SRC = ASSETS / "e__soft_AIproject_chibi-sleeping.gif"
WAIT_SRC = ASSETS / "e__soft_AIproject_chibi-reading-newspaper.gif"
# Prefer clean generated GIFs (no baked motion blur). Fallback to original collar GIF.
HOLD_CLEAN = ROOT.parent / "hoodie_hold_clean.gif"
FALL_CLEAN = ROOT.parent / "hoodie_fall_clean.gif"
COLLAR_SRC = ROOT.parent / "chibi-collar-lift-bounce.gif"
if not COLLAR_SRC.is_file():
    COLLAR_SRC = ASSETS / "e__soft_AIproject_chibi-collar-lift-bounce.gif"

CHAR = "hoodie"
MASTER_H = 360  # ~2× on-screen sprite
# Legacy collar GIF: only frame 7 is clean enough; sway is synthesized.
_HOLD_CLEAN_IDX = 7
_FALL_FRAME_IDX = (30, 25)


def _is_bg(r: int, g: int, b: int) -> bool:
    # Soft yellow→lavender gradient + near-white sparkles + cream preview plate.
    if r >= 245 and g >= 245 and b >= 245:
        return True
    # Cream plate baked into *_clean.gif previews (~248,242,228)
    if r > 235 and g > 225 and b > 200 and b < 245 and (r - b) < 55 and (g - b) < 50:
        return True
    # Pale yellow / gold paper (fall_gen top ~253,216,126)
    if (
        r > 215
        and g > 175
        and b > 85
        and b < 205
        and (r - b) > 35
        and (g - b) > 15
        and ((r + g) / 2 - b) > 16
    ):
        return True
    # Pale lavender / lilac floor (~206,179,236) and sleep GIF bars
    if (
        b > 175
        and r > 155
        and g > 145
        and b >= g - 5
        and b >= r - 20
        and max(r, g, b) - min(r, g, b) < 95
        and not (r > 210 and g > 210 and b > 210)
    ):
        return True
    # Soft peach mid
    if r > 235 and g > 210 and b > 190 and max(r, g, b) - min(r, g, b) < 50:
        return True
    # Edge mint / chroma-green crumbs (chest logo is interior — flood won't reach)
    if g > 145 and r < 165 and b < 165 and g > r + 22 and g > b + 22:
        return True
    if r < 50 and g > 200 and b < 50:
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
            if vis[y][x]:
                continue
            r, g, b = src[x, y]
            # Drop leftover sparkle dots (tiny bright non-character pixels near bg)
            if r > 240 and g > 240 and b > 200 and max(r, g, b) - min(r, g, b) < 40:
                # keep if surrounded by character; cheap: skip isolated? keep for safety on white shoes
                pass
            dst[x, y] = (r, g, b, 255)
    return out


def _keep_largest_blob(im: Image.Image) -> Image.Image:
    """Drop sparkles / haze that are not attached to the character."""
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


def _prepare(path: Path, height: int = MASTER_H, *, keep_largest: bool = False) -> Image.Image:
    cut = _knockout(Image.open(path))
    if keep_largest:
        cut = _keep_largest_blob(cut)
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
    """Idle/stand micro-motion. Prefer tiny bob — avoid scale (fit-sprite thrash)."""
    frames: list[Image.Image] = []
    for i in range(n):
        phase = math.sin(i / float(n) * math.pi * 2)
        frames.append(_affine(base, dy=0.9 * phase))
    return frames


def _sleep_loop_frames(base: Image.Image, n: int = 4) -> list[Image.Image]:
    """Sleep loop at constant size — no baked scale (user: one size while sleeping)."""
    rgba = base.convert("RGBA")
    return [rgba.copy() for _ in range(n)]


def _news_mask(im: Image.Image) -> Image.Image:
    """Mask: roughly newspaper pixels (exclude hoodie + background)."""
    rgba = im.convert("RGBA")
    w, h = rgba.size
    r, g, b, a = rgba.split()
    pix_r, pix_g, pix_b, pix_a = r.load(), g.load(), b.load(), a.load()
    mask = Image.new("L", (w, h), 0)
    mp = mask.load()
    for y in range(h):
        # Newspaper lives lower than hoodie; limit by y-band.
        if y < int(h * 0.42) or y > int(h * 0.78):
            continue
        for x in range(w):
            if x < int(w * 0.20) or x > int(w * 0.90):
                continue
            if pix_a[x, y] <= 20:
                continue
            rr, gg, bb = pix_r[x, y], pix_g[x, y], pix_b[x, y]
            # Paper-like pixels (including darker crease).
            if rr > 80 and gg > 80 and bb > 80 and max(rr, gg, bb) - min(rr, gg, bb) < 80:
                mp[x, y] = 255
    return mask


def _remove_newspaper_seam(im: Image.Image) -> Image.Image:
    """Remove the visible center crease by smoothing across a narrow band."""
    rgba = im.convert("RGBA")
    w, h = rgba.size
    midx = w // 2
    seam_left = max(0, midx - 3)
    seam_right = min(w - 1, midx + 3)

    out = rgba.copy()
    px = out.load()
    src = rgba.load()

    y0 = int(h * 0.42)
    y1 = int(h * 0.78)
    # Only smooth if pixels look like low-chroma paper/crease (avoid outlines + hair).
    for y in range(h):
        if y < y0 or y > y1:
            continue
        for x in range(seam_left, seam_right + 1):
            r, g, b, a = src[x, y]
            if a <= 20:
                continue
            l = (r + g + b) / 3.0
            if l < 45:  # keep ultra-dark outlines
                continue
            if max(r, g, b) - min(r, g, b) >= 95:
                continue
            # Sample both sides and average.
            xl = max(0, x - 6)
            xr = min(w - 1, x + 6)
            rl, gl, bl, al = src[xl, y]
            rr, gr, br, ar = src[xr, y]
            if al <= 20 or ar <= 20:
                continue
            if max(rl, gl, bl) - min(rl, gl, bl) >= 110 or max(rr, gr, br) - min(rr, gr, br) >= 110:
                continue
            px[x, y] = ((rl + rr) // 2, (gl + gr) // 2, (bl + br) // 2, px[x, y][3])
    return out


def _wait_page_flip_frames(base: Image.Image, n: int = 8) -> list[Image.Image]:
    """Prefer AI-generated flip cels; fall back to procedural rebuild."""
    del base, n
    gen_dir = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
    single = gen_dir / "hoodie_wait_gen_0.png"
    if single.is_file():
        from scripts.import_wait_gen_frames import rebuild_wait

        rebuild_wait()
        return [Image.open(OUT / f"{CHAR}_wait_0.png").convert("RGBA")]
    if all((gen_dir / f"hoodie_wait_gen_{i}.png").is_file() for i in range(8)):
        from scripts.import_wait_gen_frames import _fit_canvas, _knockout_green, _trim

        frames: list[Image.Image] = []
        for i in range(8):
            cut = _knockout_green(Image.open(gen_dir / f"hoodie_wait_gen_{i}.png"))
            frames.append(_fit_canvas(_trim(cut, pad=10), 430, 347))
        return frames
    from scripts.rebuild_wait_newspaper_flip import build_frames

    return build_frames(Image.open(OUT / f"{CHAR}_wait_base.png").convert("RGBA"))


def _gif_frame(path: Path, index: int) -> Image.Image | None:
    im = Image.open(path)
    n = int(getattr(im, "n_frames", 1) or 1)
    if index < 0 or index >= n:
        return None
    im.seek(index)
    # Do not keep_largest — hand may be a separate blob from the body.
    cut = _knockout(im.convert("RGBA"))
    fitted = _fit_h(_trim(cut, pad=8), int(MASTER_H * 0.95))
    pad_x = max(24, fitted.width // 12)
    return _canvas(fitted, fitted.width + pad_x, int(MASTER_H * 0.95) + 24)


def _gif_frames(path: Path, indices: tuple[int, ...]) -> list[Image.Image]:
    frames: list[Image.Image] = []
    for idx in indices:
        fr = _gif_frame(path, idx)
        if fr is not None:
            frames.append(fr)
    return frames


def _hold_sway_frames(base: Image.Image, n: int = 6) -> list[Image.Image]:
    """Pendulum sway from one clean hold cel — no baked motion-blur trails.

    Market desktop pets (Shimeji / Goose): 4–8 crisp cels at ~8–12 FPS while dragged.
    """
    w, h = base.size
    # Pivot near the pinched hood (top-center of the sprite).
    pivot = (w * 0.50, h * 0.10)
    # Full swing cycle: center → left → center → right → …
    angles = [
        3.0 * math.sin(i / float(n) * math.pi * 2.0) for i in range(n)
    ]
    frames: list[Image.Image] = []
    for ang in angles:
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
        # Tiny vertical bob so the dangle reads as weight, not a hard cut.
        bob = int(round(1.2 * abs(math.sin(math.radians(ang * 8)))))
        out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        cropped = rotated.crop((pad, pad, pad + w, pad + h))
        if bob:
            out.alpha_composite(cropped.crop((0, 0, w, h - bob)), (0, bob))
        else:
            out.alpha_composite(cropped, (0, 0))
        frames.append(out)
    return frames


def build() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    if not IDLE_SRC.is_file():
        raise FileNotFoundError(IDLE_SRC)

    idle = _prepare(IDLE_SRC, MASTER_H)
    idle.save(OUT / f"{CHAR}_idle.png")
    # Until interaction GIFs arrive: reuse idle for walk/stand/play.
    for pose in ("walk", "stand", "play"):
        idle.save(OUT / f"{CHAR}_{pose}.png")
    for old in OUT.glob(f"{CHAR}_idle_*.png"):
        old.unlink()
    for i, fr in enumerate(_breath_frames(idle, 4)):
        fr.save(OUT / f"{CHAR}_idle_{i}.png")
    for old in OUT.glob(f"{CHAR}_play_*.png"):
        old.unlink()
    # Soft hop frames for 摸摸 until a real play GIF arrives.
    for i, fr in enumerate(_breath_frames(idle, 4)):
        fr.save(OUT / f"{CHAR}_play_{i}.png")
    print("idle", idle.size)

    if SLEEP_SRC.is_file():
        sleep = _prepare(SLEEP_SRC, int(MASTER_H * 0.85))
        sleep.save(OUT / f"{CHAR}_sleep.png")
        for old in OUT.glob(f"{CHAR}_sleep_*.png"):
            old.unlink()
        for i, fr in enumerate(_sleep_loop_frames(sleep, 4)):
            fr.save(OUT / f"{CHAR}_sleep_{i}.png")
        print("sleep", sleep.size)
    else:
        idle.save(OUT / f"{CHAR}_sleep.png")
        print("sleep: fallback to idle")

    if WAIT_SRC.is_file():
        wait = _prepare(WAIT_SRC, int(MASTER_H * 0.92), keep_largest=False)
        wait = _remove_newspaper_seam(wait)
        wait.save(OUT / f"{CHAR}_wait.png")
        wait.save(OUT / f"{CHAR}_wait_base.png")
        for old in OUT.glob(f"{CHAR}_wait_[0-9]*.png"):
            old.unlink()
        for i, fr in enumerate(_wait_page_flip_frames(wait, 8)):
            fr.save(OUT / f"{CHAR}_wait_{i}.png")
        print("wait", wait.size)
    else:
        idle.save(OUT / f"{CHAR}_wait.png")
        print("wait: fallback to idle")

    # Prefer clean GIFs (no baked motion blur). Else synthesize from legacy collar GIF.
    hold_frames: list[Image.Image] = []
    fall_frames: list[Image.Image] = []
    if HOLD_CLEAN.is_file() and getattr(Image.open(HOLD_CLEAN), "n_frames", 1) >= 1:
        # Always synthesize sway from ONE clean cel so finger size never jumps
        # between independently generated AI frames.
        hold_base = _gif_frame(HOLD_CLEAN, 0)
        hold_frames = _hold_sway_frames(hold_base, 6) if hold_base is not None else []
        print("hold from", HOLD_CLEAN.name, "n", len(hold_frames), "(same-finger sway)")
    # Prefer fall_gen PNGs (correct knockout). Clean GIF may bake cream paper.
    FALL_GEN = (
        ASSETS / "fall_gen_1.png",
        ASSETS / "fall_gen_0.png",
        ASSETS / "fall_gen_2.png",
    )
    if all(p.is_file() for p in FALL_GEN):
        fall_frames = [_prepare(p, int(MASTER_H * 0.95), keep_largest=True) for p in FALL_GEN]
        print("fall from fall_gen_*.png", "n", len(fall_frames))
    elif FALL_CLEAN.is_file() and getattr(Image.open(FALL_CLEAN), "n_frames", 1) > 1:
        fall_frames = _gif_frames(
            FALL_CLEAN, tuple(range(int(Image.open(FALL_CLEAN).n_frames)))
        )
        print("fall from", FALL_CLEAN.name, "n", len(fall_frames))

    if (not hold_frames or not fall_frames) and COLLAR_SRC.is_file() and getattr(
        Image.open(COLLAR_SRC), "n_frames", 1
    ) > 1:
        if not hold_frames:
            hold_base = _gif_frame(COLLAR_SRC, _HOLD_CLEAN_IDX)
            hold_frames = (
                _hold_sway_frames(hold_base, 6) if hold_base is not None else []
            )
        if not fall_frames:
            fall_frames = _gif_frames(COLLAR_SRC, _FALL_FRAME_IDX)

    for old in OUT.glob(f"{CHAR}_hold*.png"):
        old.unlink()
    for old in OUT.glob(f"{CHAR}_fall*.png"):
        old.unlink()
    if hold_frames:
        hold_frames[0].save(OUT / f"{CHAR}_hold.png")
        for i, fr in enumerate(hold_frames):
            fr.save(OUT / f"{CHAR}_hold_{i}.png")
        print("hold", hold_frames[0].size, "n", len(hold_frames))
    else:
        idle.save(OUT / f"{CHAR}_hold.png")
        print("hold: fallback to idle")
    if fall_frames:
        fall_frames[0].save(OUT / f"{CHAR}_fall.png")
        for i, fr in enumerate(fall_frames):
            fr.save(OUT / f"{CHAR}_fall_{i}.png")
        print("fall", fall_frames[0].size, "n", len(fall_frames))
    else:
        idle.save(OUT / f"{CHAR}_fall.png")
        print("fall: fallback to idle")


if __name__ == "__main__":
    build()
