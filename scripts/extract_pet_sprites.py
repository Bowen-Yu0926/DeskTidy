"""Cut Journey-to-the-West pet sprites from the 5x4 concept sheet."""

from __future__ import annotations

from collections import deque
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SRC = Path(
    r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets"
    r"\c__Users_baoxin.yu_AppData_Roaming_Cursor_User_workspaceStorage"
    r"_d986ad0827732e40ed55fcc1b94b3ab9_images"
    r"_composer-annotation-edff44af-736f-454d-928b-8416cdd4886d.png"
)
OUT = ROOT / "assets" / "pets"
NAMES = ("pig",)
IDLE_H = 168
WALK_H = 156


def _lum(rgb: tuple[int, int, int]) -> float:
    return (rgb[0] + rgb[1] + rgb[2]) / 3.0


def _cell(img: Image.Image, col: int, row: int) -> Image.Image:
    w, h = img.size
    x0 = int(round(col * w / 5))
    x1 = int(round((col + 1) * w / 5))
    y0 = int(round(row * h / 4))
    y1 = int(round((row + 1) * h / 4))
    return img.crop((x0, y0, x1, y1))


def _inset_dark_frame(cell: Image.Image) -> Image.Image:
    """Drop the sheet's dark cell frame (thick on the last column)."""
    rgb = cell.convert("RGB")
    w, h = rgb.size
    px = rgb.load()

    def col_dark(x: int) -> bool:
        vals = [_lum(px[x, y]) for y in range(0, h, 2)]
        return sum(1 for v in vals if v < 90) > len(vals) * 0.45

    def row_dark(y: int) -> bool:
        vals = [_lum(px[x, y]) for x in range(0, w, 2)]
        return sum(1 for v in vals if v < 90) > len(vals) * 0.45

    left = 0
    while left < w // 4 and col_dark(left):
        left += 1
    right = w - 1
    while right > w * 3 // 4 and col_dark(right):
        right -= 1
    top = 0
    while top < h // 5 and row_dark(top):
        top += 1
    bottom = h - 1
    while bottom > h * 4 // 5 and row_dark(bottom):
        bottom -= 1
    pad = 3
    box = (
        max(0, left + pad),
        max(0, top + pad),
        min(w, right + 1 - pad),
        min(h, bottom + 1 - pad),
    )
    if box[2] - box[0] < 40 or box[3] - box[1] < 40:
        return cell
    return cell.crop(box)


def _knockout(cell: Image.Image) -> Image.Image:
    rgb = cell.convert("RGB")
    w, h = rgb.size
    src = rgb.load()
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dst = out.load()
    vis = [[False] * w for _ in range(h)]
    q: deque[tuple[int, int]] = deque()

    def is_bg(x: int, y: int) -> bool:
        r, g, b = src[x, y]
        # Paper only — never treat dark fur as background.
        if r >= 246 and g >= 246 and b >= 246:
            return True
        mx = max(r, g, b)
        mn = min(r, g, b)
        if mx - mn < 10 and _lum((r, g, b)) > 250:
            return True
        return False

    for x, y in (
        *[(i, 0) for i in range(w)],
        *[(i, h - 1) for i in range(w)],
        *[(0, j) for j in range(h)],
        *[(w - 1, j) for j in range(h)],
    ):
        if is_bg(x, y) and not vis[y][x]:
            vis[y][x] = True
            q.append((x, y))
    while q:
        x, y = q.popleft()
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and not vis[ny][nx] and is_bg(nx, ny):
                vis[ny][nx] = True
                q.append((nx, ny))

    for y in range(h):
        for x in range(w):
            if vis[y][x]:
                continue
            r, g, b = src[x, y]
            dst[x, y] = (r, g, b, 255)
    # Drop leftover dark frame / white fringe glued to empty space.
    changed = True
    while changed:
        changed = False
        for y in range(h):
            for x in range(w):
                r, g, b, a = dst[x, y]
                if a == 0:
                    continue
                edge = False
                for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
                    if not (0 <= nx < w and 0 <= ny < h) or dst[nx, ny][3] == 0:
                        edge = True
                        break
                if not edge:
                    continue
                mx = max(r, g, b)
                mn = min(r, g, b)
                gray = mx - mn < 22
                if gray and _lum((r, g, b)) < 100:
                    dst[x, y] = (0, 0, 0, 0)
                    changed = True
                elif r > 232 and g > 232 and b > 232:
                    dst[x, y] = (0, 0, 0, 0)
                    changed = True
    return out


def _trim(im: Image.Image, pad: int = 10) -> Image.Image:
    bbox = im.split()[-1].getbbox()
    if not bbox:
        return im
    l, t, r, b = bbox
    l = max(0, l - pad)
    t = max(0, t - pad)
    r = min(im.width, r + pad)
    b = min(im.height, b + pad)
    return im.crop((l, t, r, b))


def _fit_h(im: Image.Image, height: int) -> Image.Image:
    if im.height == height:
        return im
    scale = height / float(im.height)
    nw = max(1, int(im.width * scale))
    return im.resize((nw, height), Image.Resampling.LANCZOS)


def _canvas(im: Image.Image, width: int, height: int) -> Image.Image:
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    x = (width - im.width) // 2
    y = height - im.height
    canvas.alpha_composite(im, (max(0, x), max(0, y)))
    return canvas


def extract(src: Path = SRC, out_dir: Path = OUT) -> None:
    if not src.is_file():
        raise FileNotFoundError(src)
    out_dir.mkdir(parents=True, exist_ok=True)
    sheet = Image.open(src).convert("RGB")
    for row, name in enumerate(NAMES):
        idle_cell = _inset_dark_frame(_cell(sheet, 4, row))
        walk_cell = _inset_dark_frame(_cell(sheet, 2, row))
        stand_cell = _inset_dark_frame(_cell(sheet, 1, row))
        idle = _fit_h(_trim(_knockout(idle_cell)), IDLE_H)
        walk = _fit_h(_trim(_knockout(walk_cell)), WALK_H)
        stand = _fit_h(_trim(_knockout(stand_cell)), WALK_H)
        idle_out = _canvas(idle, idle.width + 12, IDLE_H + 4)
        walk_out = _canvas(walk, walk.width + 12, WALK_H + 4)
        stand_out = _canvas(stand, stand.width + 12, WALK_H + 4)
        idle_out.save(out_dir / f"{name}.png")
        walk_out.save(out_dir / f"{name}_walk.png")
        stand_out.save(out_dir / f"{name}_stand.png")
        print(name, "idle", idle_out.size, "walk", walk_out.size)


if __name__ == "__main__":
    extract()
