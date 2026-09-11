"""Import AI-generated trash (折纸飞机飞走) cels into assets/pets.

Expects green-screen (#00FF00-ish) frames:
  C:/Users/.../assets/hoodie_trash_gen_{0..11}.png
Writes:
  assets/pets/hoodie_trash.png, hoodie_trash_0..11.png

Route A: 12 key cels held longer (~10 ticks) ≈ 4–5s throw — fewer cels = less identity flicker.
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
N = 12  # keyframes; held longer at runtime
# Reject gens that are not chroma green or lack feet near the bottom (waist crop).
_MIN_GEN_GREEN_FRAC = 0.35
_MIN_FOOT_Y_FRAC = 0.65  # foot must sit in lower ~35% of the gen canvas


def _qa_gen_green(path: Path) -> None:
    arr = np.array(Image.open(path).convert("RGBA"))
    r, g, b = arr[:, :, 0].astype(np.int16), arr[:, :, 1].astype(np.int16), arr[:, :, 2].astype(np.int16)
    green = (g > 150) & (g > r + 40) & (g > b + 40)
    frac = float(green.mean())
    if frac < _MIN_GEN_GREEN_FRAC:
        raise RuntimeError(
            f"{path.name}: chroma green only {frac:.0%} of pixels "
            f"(need ≥{_MIN_GEN_GREEN_FRAC:.0%}) — regenerate with #00FF00 background"
        )


def _qa_feet_near_bottom(im: Image.Image, *, name: str) -> None:
    """Catch waist-cropped gens before scale-to-height inflates the torso."""
    _fx, fy = _foot_xy(im)
    frac = fy / float(max(1, im.height))
    if frac < _MIN_FOOT_Y_FRAC:
        raise RuntimeError(
            f"{name}: feet at y={fy:.0f}/{im.height} ({frac:.0%}) — "
            f"need ≥{_MIN_FOOT_Y_FRAC:.0%} (FULL BODY, feet at bottom-center)"
        )


def _qa_gen_hair_headroom(im: Image.Image, *, name: str) -> None:
    """Reject gens whose hair is flush with the canvas top (flat-cropped spikes)."""
    l, t, r, b = _largest_blob_bbox(im, alpha_cut=80)
    h = max(1, im.height)
    # ~4% of canvas — flush-to-edge crops only (0–2% was the recurring defect).
    if t < int(h * 0.04):
        raise RuntimeError(
            f"{name}: hair too close to canvas top (body_top={t}/{h}) — "
            "regenerate with clear green headroom above spikes"
        )
    # Flat helmet: continuous opaque run on the topmost body row across the head.
    arr = np.array(im.convert("RGBA"))
    a = arr[:, :, 3] >= 80
    bw = max(1, r - l)
    cx0 = l + int(bw * 0.22)
    cx1 = l + int(bw * 0.78)
    top_row = a[t, cx0:cx1]
    # Longest consecutive True run.
    best = cur = 0
    for v in top_row.tolist():
        if v:
            cur += 1
            best = max(best, cur)
        else:
            cur = 0
    if best >= int((cx1 - cx0) * 0.42):
        raise RuntimeError(
            f"{name}: flat-cropped crown (top-run={best}px) — regenerate hair tips"
        )
# Match hoodie_idle canvas — standing + paper plane headroom upper-right.
# Match idle canvas pitch at ~2× on-screen detail (gens are 1024² — keep more ink).
MASTER_H = 752
TARGET_W = 960
TOP_MIN = 36
BOTTOM_PAD = 10
_BODY_HEIGHT_MULT = 1.92  # ~2× idle opaque height → sharper HiDPI paint


def _is_screen_green(r: int, g: int, b: int, a: int = 255) -> bool:
    if a < 8:
        return True
    # Do NOT treat pure white / light grey as chroma — that eats the paper plane.
    # Black fill only when clearly void (AI sometimes draws black instead of green).
    mx, mn = max(r, g, b), min(r, g, b)
    if mx <= 14:
        return True
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
    """Remove chroma-key green only where it touches transparency (true halo).

    Interior green (hoodie PEOPLE map print) must survive — older logo guards
    required r>170 and ate the real chest graphic.

    Uses a 3px near-clear band: soft gen edges leave muted green 2–3px inside
    the matte that reads as 绿屏虚 on wallpaper.
    """
    arr = np.array(im.convert("RGBA"), copy=True)
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    # Strong screen green + muted spill (lower g) on the fringe.
    screen = (
        (al > 8)
        & (g > 95)
        & (g > r + 22)
        & (g > b + 18)
        & (lum > 70)
        & (lum < 250)
    )
    clear = al < 40
    pad = np.pad(clear, 3, mode="constant", constant_values=True)
    near_clear = np.zeros_like(clear)
    for dy in range(7):
        for dx in range(7):
            if abs(dy - 3) + abs(dx - 3) > 4:
                continue
            near_clear |= pad[dy : dy + clear.shape[0], dx : dx + clear.shape[1]]
    # Punch fringe green to transparent (do not leave grey stubs).
    arr[screen & near_clear, 3] = 0
    return Image.fromarray(arr, "RGBA")


def _scrub_pale_edge_fringe(im: Image.Image) -> Image.Image:
    """Punch only soft low-alpha pale matte stubs (true 白边), not hoodie fabric.

    Mechanical knockout only — does not invent pose. Protects paper plane,
    solid white fabric, skin, and eye glints.
    """
    src = im.convert("RGBA")
    arr = np.array(src, copy=True)
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    clear = al < 40
    pad = np.pad(clear, 2, mode="constant", constant_values=True)
    near_clear = np.zeros_like(clear)
    for dy in range(5):
        for dx in range(5):
            if abs(dy - 2) + abs(dx - 2) > 2:
                continue
            near_clear |= pad[dy : dy + clear.shape[0], dx : dx + clear.shape[1]]
    _logo, skin, eyeish, paper = _hair_exclusions(arr)
    # Soft AA only (partial alpha). Never punch solid white hoodie / plane.
    pale = (
        (al > 8)
        & (al < 160)
        & near_clear
        & (lum > 175)
        & (chroma < 40)
        & ~skin
        & ~eyeish
        & ~paper
        & ~_logo
    )
    arr[pale, 3] = 0
    return Image.fromarray(arr, "RGBA")


def _hair_exclusions(arr: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """logo / skin / eye glints / flying-plane (UR only — never crown chalk)."""
    h, w = arr.shape[:2]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    logo = (al > 40) & (r > 170) & (g > 60) & (g < 160) & (b < 140) & (g > r - 20)
    # Warm skin anywhere (forehead/cheeks/hands). Low-chroma grey chalk must NOT match.
    skin = (
        (al > 80)
        & (r > 145)
        & (g > 100)
        & (g < 220)
        & (b > 85)
        & (b < 200)
        & (r > g + 10)
        & (r > b + 12)
        & (chroma > 18)
        & (lum > 110)
        & (lum < 245)
    )
    # Eye glints: bright low-chroma inside a loose face band under hair peak.
    _crown, _dark, peak = _hair_crown_mask(arr)
    face = np.zeros((h, w), dtype=bool)
    face[max(0, peak) : min(h, peak + max(40, int(h * 0.18))), int(w * 0.22) : int(w * 0.78)] = True
    eyeish = face & (al >= 140) & (lum > 180) & (chroma < 45) & ~skin
    # Paper plane lives upper-RIGHT of the throw sheet — not the crown.
    plane_zone = np.zeros((h, w), dtype=bool)
    plane_zone[: max(1, int(h * 0.48)), int(w * 0.58) :] = True
    paper = (
        plane_zone
        & (al > 80)
        & (lum > 200)
        & (chroma < 35)
        & (r > 200)
        & (g > 200)
        & (b > 200)
    )
    return logo, skin, eyeish, paper


def _darken_hair_highlights(im: Image.Image) -> Image.Image:
    """Pull AI chalk / pale hair tips toward locked dark-brown (match idle/trash_0).

    Mechanical only — does not invent pose. Soft mid highlights stay; white/grey
    specular that caused 头发闪烁 between cels is flattened.
    """
    arr = np.array(im.convert("RGBA"), copy=True)
    h, w = arr.shape[:2]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    y1 = max(1, int(h * 0.28))
    x0, x1 = int(w * 0.16), int(w * 0.76)
    zone = np.zeros((h, w), dtype=bool)
    zone[:y1, x0:x1] = True
    logo, skin, eyeish, paper = _hair_exclusions(arr)
    fabric = (al >= 120) & (lum > 195) & (chroma < 40) & (r > 185) & (g > 185)
    dark_hair = zone & (al >= 140) & (lum < 95) & (r < 110) & (g < 110) & (b < 110)
    dpad = np.pad(dark_hair, 5, mode="constant")
    near = np.zeros_like(dark_hair)
    for dy in range(11):
        for dx in range(11):
            if dy == 5 and dx == 5:
                continue
            near |= dpad[dy : dy + h, dx : dx + w]
    # Crown band: chalk often floats away from the dark mass — do not require *near*.
    crown = np.zeros((h, w), dtype=bool)
    crown[: max(1, int(h * 0.22)), x0:x1] = True
    # Chalk-white / ash / cream tips → locked dark brown (aggressive).
    # Do NOT use a full-canvas "paper" exclusion — that skipped crown cream chalk.
    pale = (
        zone
        & (al >= 100)
        & (lum >= 100)
        & (lum < 230)
        & (chroma < 60)
        & ~logo
        & ~skin
        & ~paper
        & ~eyeish
        & ~fabric
        & (near | crown)
    )
    if pale.any():
        t = np.clip((lum[pale] - 90.0) / 110.0, 0.60, 0.98).astype(np.float32)
        target = np.array([34, 30, 29], dtype=np.float32)
        for c in range(3):
            src = arr[pale, c].astype(np.float32)
            arr[pale, c] = np.clip(src * (1.0 - t) + target[c] * t, 0, 255).astype(
                np.uint8
            )
        arr[pale, 3] = np.maximum(arr[pale, 3], 230)
    # Soft grey/light-brown AI streaks near dark hair → deepen.
    streak = (
        zone
        & near
        & (al >= 120)
        & (lum > 65)
        & (lum < 120)
        & (chroma < 50)
        & ~logo
        & ~skin
        & ~paper
        & ~eyeish
        & ~fabric
    )
    if streak.any():
        arr[streak, 0] = np.minimum(arr[streak, 0], 46)
        arr[streak, 1] = np.minimum(arr[streak, 1], 40)
        arr[streak, 2] = np.minimum(arr[streak, 2], 38)
    return Image.fromarray(arr, "RGBA")


def _hair_crown_mask(arr: np.ndarray) -> tuple[np.ndarray, np.ndarray, int]:
    """Crown band from dark-hair peak (ignores paper-plane bbox inflation)."""
    h, w = arr.shape[:2]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    ys, xs = np.where(al >= 80)
    if len(ys) == 0:
        return np.zeros((h, w), dtype=bool), np.zeros((h, w), dtype=bool), 0
    top, bot = int(ys.min()), int(ys.max())
    left, right = int(xs.min()), int(xs.max())
    bh = max(1, bot - top)
    dark = (al >= 140) & (lum < 95) & (r < 110) & (g < 110) & (b < 110)
    if not dark.any():
        crown = np.zeros((h, w), dtype=bool)
        crown[top : top + max(2, int(bh * 0.30)), left:right] = True
        return crown, dark, top + int(bh * 0.15)
    dy, _dx = np.where(dark)
    hist = np.bincount(dy, minlength=h)
    y_hi = top + int(bh * 0.55)
    band = hist[top:y_hi]
    peak = top + int(np.argmax(band)) if band.size else top + int(bh * 0.15)
    y1 = min(h, peak + max(18, int(bh * 0.22)))
    crown = np.zeros((h, w), dtype=bool)
    crown[top:y1, left:right] = True
    dyc, dxc = np.where(dark & crown)
    if len(dxc) > 20:
        crown[:, :] = False
        # Wider side pad so temple / ear hair fringe is inside the scrub zone.
        crown[top:y1, max(0, int(dxc.min()) - 18) : min(w, int(dxc.max()) + 18)] = True
    return crown, dark & crown, peak


def _scrub_hair_edge_halo(im: Image.Image) -> Image.Image:
    """Remove light-BG matte fringe on hair silhouette (reads as 白边 on wallpaper).

    Mechanical only: un-matte white bleed, then force remaining pale silhouette
    pixels near dark hair to locked dark brown. Does not invent pose.
    """
    src = im.convert("RGBA")
    crown, dark, peak = _hair_crown_mask(np.array(src))
    if not crown.any() or not dark.any():
        return im
    arr = np.array(src, copy=True).astype(np.float32)
    h, w = arr.shape[:2]
    r, g, b = arr[:, :, 0], arr[:, :, 1], arr[:, :, 2]
    al = arr[:, :, 3]
    a = np.clip(al / 255.0, 0.0, 1.0)
    lum = (r + g + b) / 3.0
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    pad2 = np.pad(al, 2, mode="constant")
    near_clear = np.zeros((h, w), dtype=bool)
    for dy in range(5):
        for dx in range(5):
            if abs(dy - 2) + abs(dx - 2) > 2:
                continue
            if dy == 2 and dx == 2:
                continue
            near_clear |= pad2[dy : dy + h, dx : dx + w] < 40
    pad_d = np.pad(dark, 4, mode="constant")
    near_dark = np.zeros_like(dark)
    for dy in range(9):
        for dx in range(9):
            if dy == 4 and dx == 4:
                continue
            near_dark |= pad_d[dy : dy + h, dx : dx + w]
    # Hoodie fabric is interior white — never treat silhouette next to dark
    # hair as fabric (that was skipping the exact 白边 the user sees).
    fabric = (
        (al >= 120)
        & (lum > 195)
        & (chroma < 40)
        & (r > 185)
        & (g > 185)
        & ~(near_clear & near_dark)
    )
    _logo, skin, eyeish, paper = _hair_exclusions(np.array(src))
    # Soft AA fringe (partial alpha) + opaque chalk tips on silhouette.
    soft = (
        crown
        & near_clear
        & ~fabric
        & ~skin
        & ~eyeish
        & ~paper
        & (a > 0.02)
        & (a < 0.999)
        & (lum > 55)
        & (chroma < 70)  # skip warm skin-like AA
        & (near_dark | (lum > 140))  # floating chalk tips may sit off the dark mass
    )
    opaque_chalk = (
        crown
        & near_clear
        & ~fabric
        & ~skin
        & ~eyeish
        & ~paper
        & ~dark
        & (a >= 0.999)
        & (lum > 70)
        # Include pure white matte fringe (was capped at 210 and skipped).
        & (chroma < 55)  # grey/cream/white chalk, not peach skin
        & (near_dark | (lum > 150))
    )
    # Keep face/ear skin below hair peak.
    face_guard = np.zeros((h, w), dtype=bool)
    face_guard[peak + 8 :, :] = True
    soft &= ~face_guard
    opaque_chalk &= ~face_guard
    mask = soft | opaque_chalk
    if mask.any():
        aa = np.clip(a[mask], 1e-3, 1.0)
        bg = 255.0
        for c in range(3):
            stored = arr[:, :, c][mask]
            unpre = (stored - bg * (1.0 - aa)) / aa
            arr[:, :, c][mask] = np.clip(unpre, 0, 255)
        lum2 = (arr[:, :, 0] + arr[:, :, 1] + arr[:, :, 2]) / 3.0
        still = mask & (lum2 > 45)
        if still.any():
            # Hard lock: opaque dark tips beat semi-transparent chalk on wallpaper.
            arr[:, :, 0][still] = 32.0
            arr[:, :, 1][still] = 28.0
            arr[:, :, 2][still] = 27.0
            arr[:, :, 3][still] = 255.0
        # Ultra-white floating matte only: cut if barely attached to dark hair.
        kill = mask & (lum2 > 210) & ~near_dark
        if kill.any():
            arr[:, :, 3][kill] = 0.0
    # Interior cream chalk in crown (not only silhouette).
    lum3 = (arr[:, :, 0] + arr[:, :, 1] + arr[:, :, 2]) / 3.0
    chroma3 = (
        np.maximum(np.maximum(arr[:, :, 0], arr[:, :, 1]), arr[:, :, 2])
        - np.minimum(np.minimum(arr[:, :, 0], arr[:, :, 1]), arr[:, :, 2])
    )
    interior = (
        crown
        & near_dark
        & ~fabric
        & ~skin
        & ~eyeish
        & ~paper
        & ~face_guard
        & (al >= 120)
        & (lum3 >= 100)
        & (lum3 < 210)
        & (chroma3 < 50)
    )
    if interior.any():
        t = np.clip((lum3[interior] - 85.0) / 100.0, 0.55, 0.92).astype(np.float32)
        target = np.array([34.0, 30.0, 29.0], dtype=np.float32)
        for c in range(3):
            src_c = arr[:, :, c][interior]
            arr[:, :, c][interior] = src_c * (1.0 - t) + target[c] * t
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGBA")


def _fix_chroma_spill(im: Image.Image) -> Image.Image:
    """Kill green leftover + pale hair-edge spill from chroma key (reads as 发白)."""
    arr = np.array(im.convert("RGBA"), copy=True)
    h, w = arr.shape[:2]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    # Residual green blobs between legs only — never torso PEOPLE print.
    lower = np.zeros(al.shape, dtype=bool)
    lower[int(h * 0.55) :, :] = True
    green = (al > 16) & (g > 120) & (g > r + 28) & (g > b + 28) & lower
    arr[green, 3] = 0
    al = arr[:, :, 3]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    opaque = al >= 80
    pad = np.pad(al, 1, mode="constant")
    edge = opaque & (
        (pad[1:-1, 2:] < 40)
        | (pad[1:-1, :-2] < 40)
        | (pad[2:, 1:-1] < 40)
        | (pad[:-2, 1:-1] < 40)
    )
    lum = (r + g + b) / 3.0
    head_zone = np.zeros_like(opaque)
    head_zone[: max(1, int(h * 0.48)), :] = True
    dark = (al >= 120) & (lum < 95) & (r < 110) & (g < 110) & (b < 110)
    # Larger neighborhood so chalk tips one spike away from dark mass still match.
    dpad = np.pad(dark, 3, mode="constant")
    near_dark = np.zeros_like(dark)
    for dy in range(7):
        for dx in range(7):
            if abs(dy - 3) + abs(dx - 3) > 4:
                continue
            if dy == 3 and dx == 3:
                continue
            near_dark |= dpad[dy : dy + h, dx : dx + w]
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    # Pale/white matte fringe on hair: recolor to dark charcoal (do NOT punch
    # alpha holes — that reads as wallpaper notches inside the spikes).
    pale_edge = (
        edge
        & head_zone
        & near_dark
        & (lum > 130)
        & (chroma < 60)
        & (al > 40)
    )
    if pale_edge.any():
        arr[pale_edge, 0] = 32
        arr[pale_edge, 1] = 28
        arr[pale_edge, 2] = 27
        arr[pale_edge, 3] = 255
    # Floating chalk scrap (edge but not near dark mass): cut away.
    float_chalk = (
        edge
        & head_zone
        & ~near_dark
        & (lum > 170)
        & (chroma < 50)
        & (al > 40)
    )
    arr[float_chalk, 3] = 0
    # Despill only near-silhouette pixels — interior green print must stay green.
    al = arr[:, :, 3]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    clear = al < 40
    cpad = np.pad(clear, 3, mode="constant", constant_values=True)
    near_sil = np.zeros_like(clear)
    for dy in range(7):
        for dx in range(7):
            if abs(dy - 3) + abs(dx - 3) > 4:
                continue
            near_sil |= cpad[dy : dy + clear.shape[0], dx : dx + clear.shape[1]]
    spill = (al > 40) & near_sil & (g > r + 10) & (g > b + 8) & (g > 50)
    # Exclude chest logo band so PEOPLE globe stays green.
    h, w = al.shape
    logo = np.zeros_like(spill)
    logo[int(h * 0.34) : int(h * 0.64), int(w * 0.30) : int(w * 0.70)] = True
    spill = spill & ~logo
    if spill.any():
        # Punch soft green fringe (stronger than desaturate — leftover still reads 绿屏).
        strong = spill & (g > r + 28) & (g > 100)
        arr[strong, 3] = 0
        mild = spill & ~strong
        if mild.any():
            arr[mild, 1] = np.minimum(
                arr[mild, 1], np.maximum(arr[mild, 0], arr[mild, 2])
            )
    return Image.fromarray(arr, "RGBA")


def _keep_main_and_plane(im: Image.Image, *, alpha_cut: int = 200) -> Image.Image:
    """Drop faint double-exposure ghosts; keep largest body blob + small upper plane."""
    arr = np.array(im.convert("RGBA"), copy=True)
    h, w = arr.shape[:2]
    mask = arr[:, :, 3] >= alpha_cut
    if not mask.any():
        return im
    vis = np.zeros((h, w), dtype=bool)
    components: list[tuple[int, list[tuple[int, int]]]] = []
    for y0 in range(h):
        for x0 in range(w):
            if not mask[y0, x0] or vis[y0, x0]:
                continue
            q: deque[tuple[int, int]] = deque([(x0, y0)])
            vis[y0, x0] = True
            cells: list[tuple[int, int]] = []
            while q:
                x, y = q.popleft()
                cells.append((x, y))
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < w and 0 <= ny < h and mask[ny, nx] and not vis[ny, nx]:
                        vis[ny, nx] = True
                        q.append((nx, ny))
            components.append((len(cells), cells))
    if len(components) <= 1:
        return Image.fromarray(arr, "RGBA")
    components.sort(key=lambda t: t[0], reverse=True)
    main_sz, main_cells = components[0]
    keep = np.zeros((h, w), dtype=bool)
    for x, y in main_cells:
        keep[y, x] = True
    # Candidate detached planes: small blobs in upper half / right half.
    plane_cands: list[tuple[float, int, list[tuple[int, int]]]] = []
    main_xs = [x for x, _y in main_cells]
    main_ys = [y for _x, y in main_cells]
    main_top = min(main_ys)
    main_cx = sum(main_xs) / float(main_sz)
    for sz, cells in components[1:]:
        if sz < 8 or sz > main_sz * 0.45:
            continue
        mean_y = sum(y for _x, y in cells) / float(sz)
        mean_x = sum(x for x, _y in cells) / float(sz)
        # Keep near-crown tip fragments the green knockout often severs —
        # but only dark hair tips, never chalk/white scrap (reads as 白边).
        mean_lum = float(
            sum(int(arr[y, x, 0]) + int(arr[y, x, 1]) + int(arr[y, x, 2]) for x, y in cells)
        ) / float(max(1, sz * 3))
        near_crown = (
            mean_y <= main_top + h * 0.12
            and abs(mean_x - main_cx) <= w * 0.28
            and sz <= main_sz * 0.08
            and mean_lum < 110.0
        )
        if near_crown:
            for x, y in cells:
                keep[y, x] = True
            continue
        if mean_y < h * 0.55 or mean_x > w * 0.55:
            # Prefer upper-right (higher score = farther right + higher).
            score = mean_x / float(w) + (1.0 - mean_y / float(h)) * 0.35
            plane_cands.append((score, sz, cells))
    if plane_cands:
        plane_cands.sort(key=lambda t: t[0], reverse=True)
        # One plane only — extras read as double-throw flicker.
        for x, y in plane_cands[0][2]:
            keep[y, x] = True
    out = np.zeros_like(arr)
    out[keep] = arr[keep]
    return Image.fromarray(out, "RGBA")


def _trim(im: Image.Image, pad: int = 12) -> Image.Image:
    bb = im.split()[-1].getbbox()
    if not bb:
        return im
    l, t, r, b = bb
    return im.crop(
        (max(0, l - pad), max(0, t - pad), min(im.width, r + pad), min(im.height, b + pad))
    )


def _opaque_bbox_pil(im: Image.Image) -> tuple[int, int, int, int]:
    bb = im.split()[-1].getbbox()
    return bb if bb else (0, 0, im.width, im.height)


def _idle_body_height() -> int:
    idle_path = OUT / f"{CHAR}_idle.png"
    if not idle_path.is_file():
        return int(round(MASTER_H * 0.92))
    l, t, r, b = _opaque_bbox_pil(Image.open(idle_path))
    return max(1, b - t)


def _largest_blob_bbox(im: Image.Image, *, alpha_cut: int = 200) -> tuple[int, int, int, int]:
    """BBox of the largest opaque connected component (body, not detached plane)."""
    arr = np.array(im.convert("RGBA"))
    h, w = arr.shape[:2]
    mask = arr[:, :, 3] >= alpha_cut
    if not mask.any():
        return _opaque_bbox_pil(im)
    vis = np.zeros((h, w), dtype=bool)
    best: list[tuple[int, int]] = []
    for y0 in range(h):
        for x0 in range(w):
            if not mask[y0, x0] or vis[y0, x0]:
                continue
            q: deque[tuple[int, int]] = deque([(x0, y0)])
            vis[y0, x0] = True
            cells: list[tuple[int, int]] = []
            while q:
                x, y = q.popleft()
                cells.append((x, y))
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if 0 <= nx < w and 0 <= ny < h and mask[ny, nx] and not vis[ny, nx]:
                        vis[ny, nx] = True
                        q.append((nx, ny))
            if len(cells) > len(best):
                best = cells
    if not best:
        return _opaque_bbox_pil(im)
    xs = [x for x, _y in best]
    ys = [y for _x, y in best]
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def _scale_bbox_height_to(im: Image.Image, target_h: int) -> Image.Image:
    """Scale so the *body* height matches target — ignore detached plane headroom.

    Full opaque bbox includes far-flying planes and shrinks the boy (felt like
    size flicker + missing hair on mid-flight cels).
    """
    l, t, r, b = _largest_blob_bbox(im)
    h = b - t
    if h < 2:
        l, t, r, b = _opaque_bbox_pil(im)
        h = b - t
    if h < 2:
        return im
    scale = target_h / float(h)
    nw = max(1, int(round(im.width * scale)))
    nh = max(1, int(round(im.height * scale)))
    return im.resize((nw, nh), Image.Resampling.LANCZOS)


def _scrub_residual_screen_green(im: Image.Image) -> Image.Image:
    """Clear enclosed chroma-key green pockets (bright #00FF00), keep forest logo ink."""
    arr = np.array(im.convert("RGBA"), copy=True)
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    # Pure chroma key is neon-bright; chest PEOPLE print is darker forest green.
    chroma_key = (
        (al > 16)
        & (g > 180)
        & (g > r + 45)
        & (g > b + 45)
        & (r < 120)
        & (b < 120)
    )
    arr[chroma_key, 3] = 0
    return Image.fromarray(arr, "RGBA")


def _crown_half_masses(im: Image.Image, *, alpha_cut: int = 80) -> tuple[int, int, int]:
    """Opaque pixel counts in crown band (top 10% of *body* blob): total, left, right.

    Uses the largest connected component so a detached paper plane on the right
    cannot shift the midline and fake a missing-right-crown failure.
    """
    arr = np.array(im.convert("RGBA"))
    a = arr[:, :, 3] >= alpha_cut
    # Prefer body blob bbox when available.
    try:
        l, t, r, b = _largest_blob_bbox(im, alpha_cut=alpha_cut)
    except Exception:
        ys, xs = np.where(a)
        if len(ys) == 0:
            return 0, 0, 0
        t, b, l, r = int(ys.min()), int(ys.max()), int(xs.min()), int(xs.max())
    bh = max(1, b - t)
    bw = max(1, r - l)
    band_b = t + max(2, int(bh * 0.10))
    # Head sits in the upper-central portion of the body — not full body width.
    cx0 = l + int(bw * 0.22)
    cx1 = l + int(bw * 0.78)
    mid = (cx0 + cx1) // 2
    left = int(a[t:band_b, cx0:mid].sum())
    right = int(a[t:band_b, mid:cx1].sum())
    return left + right, left, right


def _qa_crown_complete(im: Image.Image, *, name: str) -> None:
    """Reject gens with flat/helmet crowns or missing L/R spikes (recurring trash bug)."""
    idle_path = OUT / f"{CHAR}_idle.png"
    if not idle_path.is_file():
        return
    idle = Image.open(idle_path).convert("RGBA")
    tot, left, right = _crown_half_masses(im)
    i_tot, i_left, i_right = _crown_half_masses(idle)
    if tot < 400:
        raise RuntimeError(f"{name}: crown almost empty ({tot}px) — regenerate hair")
    if left < 400 or right < 400:
        raise RuntimeError(
            f"{name}: crown missing side spikes L={left} R={right} — regenerate hair"
        )
    # Relative to idle crown density (scale-invariant via body area ratio).
    body = max(1, int((np.array(im)[:, :, 3] >= 80).sum()))
    idle_body = max(1, int((np.array(idle)[:, :, 3] >= 80).sum()))
    expect = i_tot * (body / float(idle_body))
    if tot < expect * 0.45:
        raise RuntimeError(
            f"{name}: crown too thin ({tot} vs expect≥{expect * 0.45:.0f}) — regenerate hair"
        )
    # 3/4 throw poses shift mass to one side — only reject extreme one-sided crowns.
    ratio = (left + 1) / float(right + 1)
    if ratio < 0.12 or ratio > 8.5:
        raise RuntimeError(
            f"{name}: crown L/R imbalance ({left}:{right}) — regenerate hair"
        )


def _harden_alpha(im: Image.Image, *, cutoff: int = 48) -> Image.Image:
    """Drop near-clear matte stubs; keep wait-like soft AA (was 160 → jagged 虚边)."""
    arr = np.array(im.convert("RGBA"), copy=True)
    al = arr[:, :, 3]
    # Soft keep: zero only near-clear; leave partial alpha for clean edges.
    arr[:, :, 3] = np.where(al >= cutoff, al, 0).astype(np.uint8)
    # Boost mid-AA toward solid so hair tips stay readable without binary crunch.
    mid = (arr[:, :, 3] >= cutoff) & (arr[:, :, 3] < 220)
    arr[mid, 3] = np.maximum(arr[mid, 3], 200)
    return Image.fromarray(arr, "RGBA")


def _foot_xy(im: Image.Image) -> tuple[float, float]:
    """Body foot anchor — ignore detached plane and soft purple ground shadow."""
    arr = np.array(im.convert("RGBA"))
    al = arr[:, :, 3]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
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
        body = Image.fromarray(masked, "RGBA")
    else:
        body = im
    l, t, rgt, btm = _largest_blob_bbox(body)
    if btm - t < 2:
        l, t, rgt, btm = _opaque_bbox_pil(body)
    return (l + rgt) / 2.0, float(btm)


def _place_on_canvas(
    im: Image.Image,
    *,
    width: int,
    height: int,
    foot_x: float,
    foot_y: float,
) -> Image.Image:
    l, t, r, b = _opaque_bbox_pil(im)
    fx, fy = _foot_xy(im)
    x = int(round(foot_x - fx))
    y = int(round(foot_y - fy))
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    canvas.alpha_composite(im, (x, y))
    return canvas


def _unify_trash_hair_tone(im: Image.Image) -> Image.Image:
    """Pull mid/light + warm-brown hair in the head zone toward trash_0 dark brown.

    Regen cels often ship caramel crown caps / grey streaks that flash between
    frames. Mechanical regrade only — does not invent pose. Skips skin, logo, paper.
    """
    arr = np.array(im.convert("RGBA"), copy=True)
    h, w = arr.shape[:2]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    zone = np.zeros((h, w), dtype=bool)
    zone[: max(1, int(h * 0.28)), int(w * 0.16) : int(w * 0.76)] = True
    logo, skin, eyeish, paper = _hair_exclusions(arr)
    fabric = (al >= 120) & (lum > 195) & (chroma < 40) & (r > 185) & (g > 185)
    # Neutral mid/light streaks + near-white chalk (was capped at lum<210).
    neutral = (
        zone
        & (al >= 120)
        & (chroma < 55)
        & (lum > 50)
        & (lum < 220)
        & ~logo
        & ~skin
        & ~paper
        & ~eyeish
        & ~fabric
    )
    # Warm caramel/tan crown caps (higher chroma) that chroma-low mask missed.
    warm = (
        zone
        & (al >= 120)
        & (lum > 50)
        & (lum < 175)
        & (r > g + 6)
        & (r > b + 10)
        & (r < 175)
        & (g < 150)
        & (b < 130)
        & ~logo
        & ~skin
        & ~paper
        & ~eyeish
        & ~fabric
    )
    hairish = neutral | warm
    if not hairish.any():
        return im
    # Locked dark brown (idle / trash mean ~30–36). Stronger pull on brighter pixels.
    target = np.array([34, 30, 29], dtype=np.float32)
    t = np.clip((lum[hairish] - 45.0) / 90.0, 0.55, 0.97)
    for c in range(3):
        src = arr[hairish, c].astype(np.float32)
        arr[hairish, c] = np.clip(src * (1.0 - t) + target[c] * t, 0, 255).astype(
            np.uint8
        )
    return Image.fromarray(arr, "RGBA")


def _seal_hair_silhouette(im: Image.Image) -> Image.Image:
    """After LANCZOS place: seal hair edge to solid dark (no wallpaper-glow fringe).

    Mechanical only — recolors silhouette fringe / fills 1px crown holes.
    Does not invent pose.
    """
    src = im.convert("RGBA")
    arr = np.array(src, copy=True)
    h, w = arr.shape[:2]
    crown, dark, peak = _hair_crown_mask(arr)
    if not crown.any() or not dark.any():
        return im
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    _logo, skin, eyeish, paper = _hair_exclusions(arr)
    pad = np.pad(al, 1, mode="constant")
    near_clear = (
        (pad[:-2, 1:-1] < 40)
        | (pad[2:, 1:-1] < 40)
        | (pad[1:-1, :-2] < 40)
        | (pad[1:-1, 2:] < 40)
    )
    edge = (al >= 40) & near_clear
    # Any hair silhouette fringe (even mid-grey AA) → solid charcoal.
    # Do not require lum>55 — light wallpaper makes dark-grey AA read as 白边.
    fringe = (
        crown
        & edge
        & ~skin
        & ~eyeish
        & ~paper
        & ~dark  # already-solid dark tips stay; only non-dark edge needs seal
    )
    # Also catch grey/dark-brown tips that still glow on wallpaper (lum 40–95).
    soft_dark = (
        crown
        & edge
        & ~skin
        & ~eyeish
        & ~paper
        & dark
        & (lum > 42)
    )
    seal = fringe | soft_dark
    if seal.any():
        arr[seal, 0] = 22
        arr[seal, 1] = 18
        arr[seal, 2] = 17
        arr[seal, 3] = 255
    # Fill tiny crown holes (transparent with dark on ≥2 of 4 sides).
    hole = crown & (al < 40) & ~paper
    dpad = np.pad(dark, 1, mode="constant")
    n4 = (
        dpad[:-2, 1:-1].astype(np.int16)
        + dpad[2:, 1:-1].astype(np.int16)
        + dpad[1:-1, :-2].astype(np.int16)
        + dpad[1:-1, 2:].astype(np.int16)
    )
    fill = hole & (n4 >= 2)
    if fill.any():
        arr[fill, 0] = 26
        arr[fill, 1] = 22
        arr[fill, 2] = 21
        arr[fill, 3] = 255
    # Silhouette chalk flecks only — sealing interior bangs near forehead skin
    # created salt-and-pepper black dots (读作头发模糊/脏边).
    pad_d = np.pad(dark, 2, mode="constant")
    near_d = np.zeros_like(dark)
    for dy in range(5):
        for dx in range(5):
            if dy == 2 and dx == 2:
                continue
            near_d |= pad_d[dy : dy + h, dx : dx + w]
    fleck = (
        crown
        & near_d
        & near_clear
        & ~skin
        & ~eyeish
        & ~paper
        & (al >= 80)
        & (lum > 110)
        & (chroma < 40)
    )
    if fleck.any():
        arr[fleck, 0] = 28
        arr[fleck, 1] = 24
        arr[fleck, 2] = 23
    # Keep face below peak from being sealed.
    face = np.zeros((h, w), dtype=bool)
    face[peak + 10 :, :] = True
    # Revert accidental seal on skin/face if fringe matched loosely.
    bad = seal & face & skin
    if bad.any():
        arr[bad] = np.array(src)[bad]
    return Image.fromarray(arr, "RGBA")


def _seal_body_ink_outline(im: Image.Image) -> Image.Image:
    """Seal soft chroma-AA / grey hair fringe — never charcoal the white hoodie.

    Wait/idle sprites keep clean white fabric AA; inking every lum>45 edge made
    trash cels read as 虚边 (dirty outline) against the wallpaper.
    """
    arr = np.array(im.convert("RGBA"), copy=True)
    al = arr[:, :, 3]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    lum = (r + g + b) / 3.0
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    clear = al < 40
    pad = np.pad(clear, 2, mode="constant", constant_values=True)
    near_clear = np.zeros_like(clear)
    for dy in range(5):
        for dx in range(5):
            if abs(dy - 2) + abs(dx - 2) > 2:
                continue
            near_clear |= pad[dy : dy + clear.shape[0], dx : dx + clear.shape[1]]
    edge = (al >= 40) & near_clear
    _logo, skin, eyeish, paper = _hair_exclusions(arr)
    # Solid white hoodie / sneakers — match wait (no forced charcoal rim).
    fabric = (al >= 100) & (lum > 185) & (chroma < 50) & (r > 170) & (g > 170)
    # Grey hair AA or green spill only.
    soft = (
        edge
        & ~fabric
        & ~skin
        & ~eyeish
        & ~paper
        & ~_logo
        & (
            ((lum > 45) & (lum < 165) & (chroma < 55))
            | ((g > r + 12) & (g > b + 8) & (g > 55))
        )
    )
    if soft.any():
        arr[soft, 0] = 22
        arr[soft, 1] = 18
        arr[soft, 2] = 17
        arr[soft, 3] = 255
    return Image.fromarray(arr, "RGBA")


def _ink_light_silhouette_edge(im: Image.Image) -> Image.Image:
    """Ink pale non-fabric fringe only — white hoodie edges stay wait-clean."""
    arr = np.array(im.convert("RGBA"), copy=True)
    al = arr[:, :, 3]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    lum = (r + g + b) / 3.0
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    clear = al < 40
    pad = np.pad(clear, 1, mode="constant", constant_values=True)
    touch = (
        pad[:-2, 1:-1]
        | pad[2:, 1:-1]
        | pad[1:-1, :-2]
        | pad[1:-1, 2:]
    )
    _logo, skin, eyeish, paper = _hair_exclusions(arr)
    fabric = (al >= 100) & (lum > 185) & (chroma < 50) & (r > 170) & (g > 170)
    fringe = (
        (al >= 80)
        & touch
        & (lum > 90)
        & (lum < 200)  # leave bright white fabric alone
        & ~fabric
        & ~skin
        & ~eyeish
        & ~paper
        & ~_logo
    )
    if fringe.any():
        arr[fringe, 0] = 22
        arr[fringe, 1] = 18
        arr[fringe, 2] = 17
        arr[fringe, 3] = 255
    return Image.fromarray(arr, "RGBA")


def _lock_sprite_hair(im: Image.Image) -> Image.Image:
    """Defringe chalk hair matte; never darken white hoodie fabric or face skin.

    Mechanical regrade only — does not invent pose.
    Foot shadow is applied after canvas place so soft ovals do not inflate prep
    bboxes past MASTER_H (plane-fit shrink loop).
    """
    sealed = _seal_hair_silhouette(
        _scrub_hair_edge_halo(_darken_hair_highlights(_scrub_pale_edge_fringe(im)))
    )
    cleaned = _scrub_orphan_dark_specks(
        _ink_light_silhouette_edge(_seal_body_ink_outline(sealed))
    )
    return _unify_shoe_sole_baseline(_scrub_plane_surface_specks(cleaned))


def _scrub_plane_surface_specks(im: Image.Image, *, max_blob: int = 90) -> Image.Image:
    """Remove dark flecks sitting on the white paper-plane face (飞行时「身上的点」).

    Keeps long crease strokes and the outer ink outline; punches dirty islands
    whose neighbors are mostly bright paper.
    """
    arr = np.array(im.convert("RGBA"), copy=True)
    h, w = arr.shape[:2]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    paper = (al >= 100) & (lum > 210) & (r > 195) & (g > 195) & (b > 195)
    paper[int(h * 0.55) :, :] = False
    if int(paper.sum()) < 60:
        return im
    # Largest upper paper blob = plane / sheet.
    visited = np.zeros((h, w), dtype=bool)
    comps: list[list[tuple[int, int]]] = []
    ys_i, xs_i = np.where(paper)
    for y0, x0 in zip(ys_i.tolist(), xs_i.tolist()):
        if visited[y0, x0]:
            continue
        q: deque[tuple[int, int]] = deque([(x0, y0)])
        visited[y0, x0] = True
        cells: list[tuple[int, int]] = []
        while q:
            x, y = q.popleft()
            cells.append((x, y))
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                if 0 <= nx < w and 0 <= ny < h and paper[ny, nx] and not visited[ny, nx]:
                    visited[ny, nx] = True
                    q.append((nx, ny))
        comps.append(cells)
    if not comps:
        return im
    plane_cells = max(comps, key=len)
    if len(plane_cells) < 80:
        return im
    mask = np.zeros((h, w), dtype=bool)
    for x, y in plane_cells:
        mask[y, x] = True
    dil = mask.copy()
    for _ in range(2):
        pad = np.pad(dil, 1, mode="constant")
        dil = dil | pad[:-2, 1:-1] | pad[2:, 1:-1] | pad[1:-1, :-2] | pad[1:-1, 2:]
    pad_m = np.pad(mask, 1, mode="constant")
    n_paper = (
        pad_m[:-2, 1:-1].astype(np.int16)
        + pad_m[2:, 1:-1]
        + pad_m[1:-1, :-2]
        + pad_m[1:-1, 2:]
    )
    # Interior dirt / flecks (not isolated outline ink with 0–1 paper neighbors).
    dirt = dil & (al >= 80) & (lum < 195) & (r < 200) & (n_paper >= 2)
    if not dirt.any():
        return im
    # Also drop tiny connected dirty blobs (legacy path).
    visited2 = np.zeros((h, w), dtype=bool)
    ys_d, xs_d = np.where(dirt)
    for y0, x0 in zip(ys_d.tolist(), xs_d.tolist()):
        if visited2[y0, x0]:
            continue
        q2: deque[tuple[int, int]] = deque([(x0, y0)])
        visited2[y0, x0] = True
        cells2: list[tuple[int, int]] = []
        while q2:
            x, y = q2.popleft()
            cells2.append((x, y))
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                if 0 <= nx < w and 0 <= ny < h and dirt[ny, nx] and not visited2[ny, nx]:
                    visited2[ny, nx] = True
                    q2.append((nx, ny))
        xs = [c[0] for c in cells2]
        ys = [c[1] for c in cells2]
        bw = max(xs) - min(xs) + 1
        bh = max(ys) - min(ys) + 1
        # Skip long crease runs.
        if len(cells2) > max_blob and max(bw, bh) / max(1, min(bw, bh)) > 5.0:
            continue
        if max(bw, bh) >= 36 and min(bw, bh) <= 3:
            continue
        for x, y in cells2:
            y0s, y1s = max(0, y - 6), min(h, y + 7)
            x0s, x1s = max(0, x - 6), min(w, x + 7)
            patch = mask[y0s:y1s, x0s:x1s]
            if patch.any():
                arr[y, x] = np.median(arr[y0s:y1s, x0s:x1s][patch], axis=0).astype(np.uint8)
            else:
                arr[y, x] = [246, 246, 248, 255]
    return Image.fromarray(arr, "RGBA")


def _unify_shoe_sole_baseline(im: Image.Image, *, target_gap: int = 3) -> Image.Image:
    """Shift each shoe so soles share one ground line (脚底不齐 → 统一).

    Mechanical vertical nudge of left/right *shoe* clusters only — never grow into
    pants/hoodie (that wiped throw cels when black pants were included).
    """
    arr = np.array(im.convert("RGBA"), copy=True)
    h, w = arr.shape[:2]
    al = arr[:, :, 3]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    lum = (r + g + b) / 3.0
    try:
        l, t, rr, bb = _largest_blob_bbox(im, alpha_cut=80)
    except Exception:
        return im
    body_h = max(1, bb - t)
    if body_h < 120:
        # Already truncated — do not touch.
        return im
    y0 = bb - int(body_h * 0.14)
    # White / light sneaker only (not hoodie fabric above mid-shin).
    shoe = (al >= 100) & (lum > 175) & (r > 170) & (g > 170) & (b > 170)
    shoe[: max(y0, int(h * 0.62)), :] = False
    shoe[:, :l] = False
    shoe[:, rr + 1 :] = False
    if int(shoe.sum()) < 40:
        return im
    mid = (l + rr) / 2.0
    target_y = int(bb - target_gap)
    # Dark sole ink / lace — only if touching shoe (tight dilate on shoe colors).
    near_shoe = shoe.copy()
    for _ in range(2):
        pad = np.pad(near_shoe, 1, mode="constant")
        near_shoe = near_shoe | pad[:-2, 1:-1] | pad[2:, 1:-1] | pad[1:-1, :-2] | pad[1:-1, 2:]
    sole_ink = near_shoe & (al >= 100) & (lum < 90) & (np.arange(h)[:, None] >= y0)

    def _shift_side(*, left: bool) -> None:
        nonlocal arr
        side = (np.arange(w)[None, :] < mid) if left else (np.arange(w)[None, :] >= mid)
        mask = (shoe | sole_ink) & side
        ys, xs = np.where(mask)
        if len(ys) == 0:
            return
        sole_y = int(ys.max())
        dy = target_y - sole_y
        if dy == 0 or abs(dy) > 14:
            return
        y_min, y_max = int(ys.min()), int(ys.max())
        x_min, x_max = int(xs.min()), int(xs.max())
        patch = arr[y_min : y_max + 1, x_min : x_max + 1].copy()
        pmask = mask[y_min : y_max + 1, x_min : x_max + 1]
        arr[mask, 3] = 0
        ny0 = y_min + dy
        ny1 = ny0 + patch.shape[0]
        if ny0 < 0 or ny1 > h:
            arr[y_min : y_max + 1, x_min : x_max + 1][pmask] = patch[pmask]
            return
        dest = arr[ny0:ny1, x_min : x_max + 1]
        dest[pmask] = patch[pmask]
        arr[ny0:ny1, x_min : x_max + 1] = dest

    _shift_side(left=True)
    _shift_side(left=False)
    return Image.fromarray(arr, "RGBA")


def _ensure_foot_shadow(im: Image.Image) -> Image.Image:
    """Soft purple oval under both soles when missing (脚底无影会显得漂)."""
    arr = np.array(im.convert("RGBA"), copy=True)
    h, w = arr.shape[:2]
    al = arr[:, :, 3]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    try:
        l, t, rr, bb = _largest_blob_bbox(im, alpha_cut=80)
    except Exception:
        return im
    shadow = (
        (al >= 40)
        & (r > 70)
        & (r < 200)
        & (b > g)
        & (b >= r - 15)
        & (g < 160)
    )
    shadow[: int(h * 0.82), :] = False
    if int(shadow.sum()) >= 80:
        return im
    cx = int(round((l + rr) / 2.0))
    # Keep shadow just under soles — never past canvas or inflate foot_y.
    cy = min(h - 6, bb + 1)
    rw = max(28, int((rr - l) * 0.36))
    rh = max(6, int(rw * 0.18))
    yy, xx = np.ogrid[:h, :w]
    oval = ((xx - cx) / float(rw)) ** 2 + ((yy - cy) / float(rh)) ** 2 <= 1.0
    oval &= al < 36
    oval &= yy <= min(h - 2, bb + rh + 1)
    for y, x in zip(*np.where(oval)):
        dist = ((x - cx) / float(rw)) ** 2 + ((y - cy) / float(rh)) ** 2
        alpha = int(90 * max(0.0, 1.0 - dist))
        if alpha < 18:
            continue
        arr[y, x] = [148, 108, 188, alpha]
    return Image.fromarray(arr, "RGBA")


def _scrub_orphan_dark_specks(im: Image.Image, *, max_area: int = 900) -> Image.Image:
    """Remove floating flecks above the crown (seal leftover / gen watermark 黑块).

    Keeps the main body and paper plane. Drops small islands that sit entirely
    above the body crown — including grey/green watermark scraps (e.g. ``/12``).
    """
    arr = np.array(im.convert("RGBA"), copy=True)
    h, w = arr.shape[:2]
    al = arr[:, :, 3]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    lum = (r + g + b) / 3.0
    opaque = al >= 80
    if not opaque.any():
        return im
    try:
        _l, body_t, _r, _b = _largest_blob_bbox(im, alpha_cut=80)
    except Exception:
        return im
    labels = np.zeros((h, w), dtype=np.int32)
    lab = 0
    sizes: dict[int, int] = {}
    ys_idx, xs_idx = np.where(opaque)
    visited = np.zeros((h, w), dtype=bool)
    for y0, x0 in zip(ys_idx.tolist(), xs_idx.tolist()):
        if visited[y0, x0]:
            continue
        lab += 1
        q: deque[tuple[int, int]] = deque([(x0, y0)])
        visited[y0, x0] = True
        labels[y0, x0] = lab
        count = 0
        while q:
            x, y = q.popleft()
            count += 1
            for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                if 0 <= nx < w and 0 <= ny < h and opaque[ny, nx] and not visited[ny, nx]:
                    visited[ny, nx] = True
                    labels[ny, nx] = lab
                    q.append((nx, ny))
        sizes[lab] = count
    if not sizes:
        return im
    main = max(sizes, key=sizes.get)
    paperish = opaque & (lum > 200) & (r > 190) & (g > 190) & (b > 190)
    # Dilate paper so dark plane outlines stay attached to the plane component.
    pad_p = np.pad(paperish, 3, mode="constant")
    near_paper = np.zeros_like(paperish)
    for dy in range(7):
        for dx in range(7):
            near_paper |= pad_p[dy : dy + h, dx : dx + w]

    for lid, sz in sizes.items():
        if lid == main:
            continue
        comp = labels == lid
        if paperish[comp].any() or near_paper[comp].any():
            continue
        ys, xs = np.where(comp)
        if ys.size == 0:
            continue
        y_max = int(ys.max())
        y_min = int(ys.min())
        x_mean = float(xs.mean())
        # Entirely above crown: watermark / seal flecks (incl. grey ``/12`` scraps).
        if y_max <= body_t + 28 and sz <= max_area:
            arr[comp, 3] = 0
            continue
        # Small dark crumbs in the upper half, not near the plane (right flight path).
        if (
            sz <= 80
            and y_max < int(h * 0.45)
            and float(lum[comp].mean()) < 90
            and x_mean < w * 0.62
        ):
            arr[comp, 3] = 0
            continue
        # Mid-size dark fleck straddling crown but mostly above hairline.
        if (
            sz <= max_area
            and y_min < body_t
            and y_max <= body_t + 40
            and float(lum[comp].mean()) < 100
            and not near_paper[comp].any()
        ):
            arr[comp, 3] = 0
    return Image.fromarray(arr, "RGBA")


def _count_pale_hair_px(im: Image.Image) -> tuple[int, int]:
    """Return (pale, chalk-white) counts on hair silhouette / near dark crown.

    Excludes white hoodie fabric and paper plane so idle QA is not inflated.
    """
    arr = np.array(im.convert("RGBA"))
    h, w = arr.shape[:2]
    r = arr[:, :, 0].astype(np.int16)
    g = arr[:, :, 1].astype(np.int16)
    b = arr[:, :, 2].astype(np.int16)
    al = arr[:, :, 3]
    lum = (r + g + b) / 3.0
    chroma = np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)
    crown, dark, _peak = _hair_crown_mask(arr)
    if not dark.any():
        return 0, 0
    pad_d = np.pad(dark, 4, mode="constant")
    near_dark = np.zeros_like(dark)
    for dy in range(9):
        for dx in range(9):
            if dy == 4 and dx == 4:
                continue
            near_dark |= pad_d[dy : dy + h, dx : dx + w]
    pad = np.pad(al, 1, mode="constant")
    near_clear = (
        (pad[:-2, 1:-1] < 40)
        | (pad[2:, 1:-1] < 40)
        | (pad[1:-1, :-2] < 40)
        | (pad[1:-1, 2:] < 40)
    )
    fabric = (
        (al >= 120)
        & (lum > 195)
        & (chroma < 40)
        & (r > 185)
        & (g > 185)
        & ~(near_clear & near_dark)
    )
    _logo, skin, eyeish, paper = _hair_exclusions(arr)
    zone = (
        crown
        & near_dark
        & near_clear
        & ~fabric
        & ~skin
        & ~eyeish
        & ~paper
    )
    pale = zone & (al >= 80) & (lum >= 95) & (chroma < 55)
    white = zone & (al >= 60) & (lum >= 140) & (chroma < 50)
    return int(pale.sum()), int(white.sum())


def _qa_hair_not_chalky(im: Image.Image, *, name: str) -> None:
    """Reject cels that still flash chalk-white hair after tone lock."""
    pale, white = _count_pale_hair_px(im)
    # Absolute caps on the placed canvas (960×752 ≈ 4× prior area).
    if white > 880:
        raise RuntimeError(
            f"{name}: chalk-white hair tips ({white}px) — regenerate / retone"
        )
    if pale > 3600:
        raise RuntimeError(
            f"{name}: pale hair mass too high ({pale}px) — regenerate / retone"
        )


def _ensure_top_headroom(im: Image.Image, *, min_frac: float = 0.055) -> Image.Image:
    """Pad transparent top so spikes clear the headroom QA floor (canvas align only)."""
    _l, t, _r, _b = _largest_blob_bbox(im, alpha_cut=80)
    need = int(im.height * min_frac)
    if t >= need:
        return im
    pad = need - t + 4
    w, h = im.size
    canvas = Image.new("RGBA", (w, h + pad), (0, 0, 0, 0))
    canvas.paste(im, (0, pad), im)
    return canvas


def _prepare_frame(path: Path, target_body_h: int) -> Image.Image:
    _qa_gen_green(path)
    knocked = _lock_sprite_hair(
        _fix_chroma_spill(
            _scrub_residual_screen_green(
                _keep_main_and_plane(
                    _scrub_pale_edge_fringe(
                        _scrub_green_halos(_knockout_green(Image.open(path)))
                    )
                )
            )
        )
    )
    knocked = _ensure_top_headroom(knocked)
    _qa_feet_near_bottom(knocked, name=path.name)
    _qa_gen_hair_headroom(knocked, name=path.name)
    _qa_crown_complete(knocked, name=path.name)
    cut = _trim(_harden_alpha(knocked), pad=14)
    return _scale_bbox_height_to(cut, target_body_h)


def _shared_foot_anchor(frames: list[Image.Image]) -> tuple[float, float]:
    xs: list[float] = []
    ys: list[float] = []
    for im in frames:
        fx, fy = _foot_xy(im)
        xs.append(fx)
        ys.append(fy)
    xs.sort()
    ys.sort()
    mid = len(xs) // 2
    return xs[mid], ys[mid]


def _fit_frames_to_canvas() -> list[Image.Image]:
    target_body_h = max(1, int(round(_idle_body_height() * _BODY_HEIGHT_MULT)))
    prepared = [
        _prepare_frame(SRC_DIR / f"{CHAR}_trash_gen_{i}.png", target_body_h) for i in range(N)
    ]
    foot_y_canvas = float(MASTER_H - BOTTOM_PAD - 1)
    # Feet live at horizontal canvas center — runtime draws the cel centered
    # (no foot-offset math). Plane flight extends into the right half of the sheet.
    foot_x_canvas = TARGET_W / 2.0

    def _place_metrics(im: Image.Image, scale: float) -> tuple[int, int, int, int, int]:
        nw = max(1, int(round(im.width * scale)))
        nh = max(1, int(round(im.height * scale)))
        scaled = im.resize((nw, nh), Image.Resampling.LANCZOS)
        fx, fy = _foot_xy(scaled)
        x = int(round(foot_x_canvas - fx))
        y = int(round(foot_y_canvas - fy))
        l, t, r, _b = _opaque_bbox_pil(scaled)
        _bl, body_t, _br, body_b = _largest_blob_bbox(scaled)
        # Use body sole bottom (not soft shadow) so prep shadows cannot fail fit.
        return t + y, r + x, body_b + y, body_t + y, l + x

    def _body_top_after_place(scale: float) -> int:
        """Headroom above the *boy* only — flying planes may enter the top pad."""
        return min(_place_metrics(im, scale)[3] for im in prepared)

    def _plane_fits(im: Image.Image, scale: float) -> bool:
        top, right, bottom, _body_t, left = _place_metrics(im, scale)
        return top >= 0 and left >= 0 and right <= TARGET_W and bottom <= MASTER_H - 1

    scale = 1.0
    for _ in range(8):
        if _body_top_after_place(scale) >= TOP_MIN:
            break
        scale *= 0.94
    # Per-cel shrink so far-flying planes are not cropped off the sheet.
    fitted: list[Image.Image] = []
    for im in prepared:
        s = scale
        for _ in range(20):
            if _plane_fits(im, s):
                break
            s *= 0.95
        else:
            raise RuntimeError("trash cel plane does not fit canvas after shrink")
        if s < 0.999:
            im = im.resize(
                (max(1, int(round(im.width * s))), max(1, int(round(im.height * s)))),
                Image.Resampling.LANCZOS,
            )
        fitted.append(im)
    prepared = fitted

    out: list[Image.Image] = []
    for i, im in enumerate(prepared):
        placed = _place_on_canvas(
            im,
            width=TARGET_W,
            height=MASTER_H,
            foot_x=foot_x_canvas,
            foot_y=foot_y_canvas,
        )
        # LANCZOS place can reintroduce pale fringe — lock hair again on canvas.
        placed = _ensure_foot_shadow(_lock_sprite_hair(placed))
        _qa_hair_not_chalky(placed, name=f"{CHAR}_trash_{i}")
        _l, body_t, _r, body_b = _largest_blob_bbox(placed)
        if body_t < TOP_MIN:
            raise RuntimeError(f"head clipped: body_top={body_t} < {TOP_MIN}")
        # Soft shadow may sit 1–2px under soles; sole line must stay on-canvas.
        fx, fy = _foot_xy(placed)
        if fy > MASTER_H - 1:
            raise RuntimeError(f"feet clipped: foot_y={fy}")
        ol, ot, orr, ob = _opaque_bbox_pil(placed)
        # Planes may use the top pad; only fail if content is fully off-canvas.
        if orr > TARGET_W + 1 or ol < -1:
            raise RuntimeError(f"plane/content clipped horizontally: opaque=({ol},{ot},{orr},{ob})")
        out.append(placed)
    return out


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    for i in range(N):
        path = SRC_DIR / f"{CHAR}_trash_gen_{i}.png"
        if not path.is_file():
            raise SystemExit(f"missing generated frame: {path}")

    frames = _fit_frames_to_canvas()
    for i, fr in enumerate(frames):
        l, t, r, b = _opaque_bbox_pil(fr)
        print(
            "imported",
            i,
            fr.size,
            "bbox_top",
            t,
            "foot_y",
            b,
            "opaque",
            int((np.array(fr)[:, :, 3] > 20).sum()),
        )

    for old in OUT.glob(f"{CHAR}_trash*.png"):
        old.unlink()

    frames[0].save(OUT / f"{CHAR}_trash.png")
    for i, fr in enumerate(frames):
        fr.save(OUT / f"{CHAR}_trash_{i}.png")
        print("wrote", f"{CHAR}_trash_{i}.png", fr.size)

    from scripts.rebuild_hoodie_trash_locked import rebuild_from_imported

    rebuild_from_imported()
    print("OK: hoodie paper-plane trash cels installed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
