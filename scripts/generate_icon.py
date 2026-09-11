"""Generate application icon assets/app_icon.ico (multi-size, sharp).

Always draws a procedural glyph — never paste screenshots.
- Large sizes: stacked blue desk panes (BrandMark silhouette).
- Tray sizes (16/24/32): a near full-bleed rounded square so the
  notification area does not show a tiny “postage stamp”.
"""

from __future__ import annotations

import io
import struct
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    raise SystemExit("Pillow is required: pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "app_icon.ico"
PREVIEW = ROOT / "assets" / "app_icon_source.png"

BLUE_DEEP = (37, 99, 235, 255)
BLUE_MID = (59, 130, 246, 255)
BLUE_LIGHT = (96, 165, 250, 255)
WHITE = (255, 255, 255, 255)

ICON_SIZES = (16, 24, 32, 48, 64, 128, 256)


def _lerp(a: int, b: int, t: float) -> int:
    return int(round(a + (b - a) * t))


def _mix(
    c0: tuple[int, int, int, int], c1: tuple[int, int, int, int], t: float
) -> tuple[int, int, int, int]:
    t = max(0.0, min(1.0, t))
    return (
        _lerp(c0[0], c1[0], t),
        _lerp(c0[1], c1[1], t),
        _lerp(c0[2], c1[2], t),
        _lerp(c0[3], c1[3], t),
    )


def _fill_rounded_rect(
    img: Image.Image,
    box: tuple[float, float, float, float],
    radius: float,
    color_top: tuple[int, int, int, int],
    color_bot: tuple[int, int, int, int],
) -> None:
    x0, y0, x1, y1 = box
    w = max(1, int(round(x1 - x0)))
    h = max(1, int(round(y1 - y0)))
    scale = 2
    sw, sh = w * scale, h * scale
    layer = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    r = max(1, int(round(radius * scale)))
    for row in range(sh):
        t = row / max(1, sh - 1)
        c = _mix(color_top, color_bot, t)
        draw.line([(0, row), (sw - 1, row)], fill=c)
    mask = Image.new("L", (sw, sh), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, sw - 1, sh - 1), radius=r, fill=255
    )
    layer.putalpha(mask)
    layer = layer.resize((w, h), Image.Resampling.LANCZOS)
    img.alpha_composite(layer, (int(round(x0)), int(round(y0))))


def _draw_tray_icon(size: int) -> Image.Image:
    """Same BrandMark as ``src.ui.brand_mark`` (sidebar), for tray ICO entries."""
    return _draw_brand_mark_pil(size)


def _draw_brand_mark_pil(size: int) -> Image.Image:
    """PIL twin of ``paint_brand_mark`` — keep geometry in sync with brand_mark.py."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    scale = size / 40.0
    radius = max(1.0, 6.0 * scale)
    layers = (
        (6, 10, 28, 22, 70),
        (4, 6, 28, 22, 140),
        (2, 2, 28, 22, 255),
    )
    accent = BLUE_MID
    for x, y, w, h, alpha in layers:
        box = (x * scale, y * scale, (x + w) * scale, (y + h) * scale)
        top = (*BLUE_LIGHT[:3], alpha)
        bot = (*accent[:3], alpha)
        _fill_rounded_rect(img, box, radius, top, bot)
        # White rim (matches QPen white outline on BrandMark).
        if size >= 24 or alpha >= 200:
            rim = Image.new("RGBA", img.size, (0, 0, 0, 0))
            rd = ImageDraw.Draw(rim)
            rim_a = min(180, alpha)
            width = max(1, int(round(scale)))
            rd.rounded_rectangle(
                (box[0], box[1], box[2] - 1, box[3] - 1),
                radius=radius,
                outline=(255, 255, 255, rim_a),
                width=width,
            )
            img.alpha_composite(rim)
    # White pip
    px, py, pw, ph = 22, 8, 5, 5
    draw = ImageDraw.Draw(img)
    draw.ellipse(
        (
            px * scale,
            py * scale,
            (px + pw) * scale,
            (py + ph) * scale,
        ),
        fill=(255, 255, 255, 230),
    )
    return img


def _draw_brand_icon(size: int) -> Image.Image:
    """Large / shortcut / Alt-Tab — same BrandMark silhouette."""
    return _draw_brand_mark_pil(size)


def _draw_app_icon(size: int) -> Image.Image:
    return _draw_brand_mark_pil(size)


def _png_bytes(img: Image.Image) -> bytes:
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def _bmp_icon_bytes(img: Image.Image) -> bytes:
    """Classic 32-bpp XOR + 1-bpp AND mask (best tray compatibility on Windows)."""
    img = img.convert("RGBA")
    w, h = img.size
    # XOR bitmap: bottom-up BGRA
    xor = bytearray()
    pixels = img.load()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            xor.extend((b, g, r, a))
        # rows already 4-byte aligned for 32bpp
    # AND mask: 1bpp, padded to 32-bit rows, bottom-up. All zero = fully visible
    # (alpha in XOR handles transparency for modern shells).
    row_bytes = ((w + 31) // 32) * 4
    and_mask = bytearray(row_bytes * h)
    header = struct.pack(
        "<IiiHHIIiiII",
        40,  # biSize
        w,
        h * 2,  # height includes AND mask
        1,  # planes
        32,  # bit count
        0,  # compression
        len(xor),
        0,
        0,
        0,
        0,
    )
    return bytes(header) + bytes(xor) + bytes(and_mask)


def save_multisize_ico(path: Path, images: list[Image.Image]) -> None:
    """Write ICO: BMP for <=32 (tray), PNG for larger (HiDPI / shortcuts)."""
    entries: list[tuple[int, int, bytes]] = []
    for img in images:
        if img.mode != "RGBA":
            img = img.convert("RGBA")
        w, h = img.size
        if max(w, h) <= 32:
            payload = _bmp_icon_bytes(img)
        else:
            payload = _png_bytes(img)
        entries.append((w, h, payload))

    count = len(entries)
    offset = 6 + 16 * count
    chunks: list[bytes] = [struct.pack("<HHH", 0, 1, count)]
    data_blobs: list[bytes] = []
    for w, h, blob in entries:
        wb = 0 if w >= 256 else w
        hb = 0 if h >= 256 else h
        chunks.append(
            struct.pack(
                "<BBBBHHII",
                wb,
                hb,
                0,
                0,
                1,
                32,
                len(blob),
                offset,
            )
        )
        data_blobs.append(blob)
        offset += len(blob)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"".join(chunks) + b"".join(data_blobs))


def main() -> None:
    print("source: procedural tray+BrandMark glyph (no screenshots)")
    images = [_draw_app_icon(size) for size in ICON_SIZES]
    save_multisize_ico(OUT, images)
    # Preview = large brand mark (transparent).
    preview = _draw_brand_icon(256)
    preview.save(PREVIEW, format="PNG", optimize=True)
    print(f"{OUT} ({OUT.stat().st_size} bytes, sizes={list(ICON_SIZES)})")
    print(f"{PREVIEW} ({PREVIEW.stat().st_size} bytes preview)")


if __name__ == "__main__":
    main()
