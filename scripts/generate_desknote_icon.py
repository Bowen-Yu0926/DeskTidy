"""Generate deskNote icon assets/desknote_icon.ico (multi-size).

Distinct from DeskTidy BrandMark: teal notepad + markdown ``#`` mark.
"""

from __future__ import annotations

import io
import struct
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    raise SystemExit("Pillow is required: pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "assets" / "desknote_icon.ico"
PREVIEW = ROOT / "assets" / "desknote_icon_source.png"

TEAL_DEEP = (15, 118, 110, 255)  # #0F766E
TEAL_MID = (20, 184, 166, 255)  # #14B8A6
TEAL_LIGHT = (45, 212, 191, 255)  # #2DD4BF
PAPER = (255, 255, 255, 255)
PAPER_EDGE = (226, 232, 240, 255)
INK = (51, 65, 85, 255)  # slate-700
LINE = (148, 163, 184, 255)  # slate-400

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


def _font(size: int) -> ImageFont.ImageFont | ImageFont.FreeTypeFont:
    for name in (
        "segoeuib.ttf",
        "seguisb.ttf",
        "arialbd.ttf",
        "arial.ttf",
        "msyhbd.ttc",
        "msyh.ttc",
    ):
        try:
            return ImageFont.truetype(name, size=size)
        except OSError:
            continue
    return ImageFont.load_default()


def _draw_desknote_icon(size: int) -> Image.Image:
    """Teal rounded tile + white note + markdown hash."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    s = float(size)
    pad = max(1.0, s * 0.06)
    radius = max(2.0, s * 0.22)
    _fill_rounded_rect(
        img,
        (pad, pad, s - pad, s - pad),
        radius,
        TEAL_LIGHT,
        TEAL_DEEP,
    )

    # Paper card
    px0 = s * 0.22
    py0 = s * 0.18
    px1 = s * 0.82
    py1 = s * 0.86
    paper_r = max(1.5, s * 0.08)
    _fill_rounded_rect(img, (px0, py0, px1, py1), paper_r, PAPER, PAPER_EDGE)

    draw = ImageDraw.Draw(img)

    # Folded corner (top-right of paper)
    if size >= 24:
        fold = max(3.0, s * 0.14)
        fx1, fy0 = px1 - 1, py0 + 1
        tri = [
            (fx1 - fold, fy0),
            (fx1, fy0),
            (fx1, fy0 + fold),
        ]
        draw.polygon(tri, fill=TEAL_MID)
        # Fold crease hint
        draw.line(
            [(fx1 - fold, fy0), (fx1 - fold * 0.15, fy0 + fold * 0.85)],
            fill=(255, 255, 255, 160),
            width=max(1, int(s * 0.02)),
        )

    # Text lines on paper
    if size >= 24:
        lx0 = px0 + s * 0.08
        lx1 = px1 - s * 0.18
        line_w = max(1, int(round(s * 0.035)))
        for i, t in enumerate((0.42, 0.55, 0.68)):
            y = py0 + (py1 - py0) * t
            end = lx1 if i < 2 else lx0 + (lx1 - lx0) * 0.55
            draw.line([(lx0, y), (end, y)], fill=LINE, width=line_w)

    # Markdown ``#``
    hash_size = max(8, int(round(s * (0.42 if size <= 32 else 0.38))))
    font = _font(hash_size)
    label = "#"
    # Prefer measuring with textbbox
    try:
        bbox = draw.textbbox((0, 0), label, font=font)
        tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
        tx = (s - tw) / 2 - bbox[0]
        ty = s * (0.28 if size >= 48 else 0.22) - bbox[1]
    except Exception:
        tx, ty = s * 0.32, s * 0.22
    # Shadow then ink
    if size >= 32:
        draw.text((tx + 1, ty + 1), label, font=font, fill=(15, 118, 110, 90))
    draw.text((tx, ty), label, font=font, fill=TEAL_DEEP if size >= 24 else PAPER)

    # On tiny tray icons, put white ``#`` centered on teal (paper is crowded).
    if size <= 20:
        img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        _fill_rounded_rect(
            img,
            (0.5, 0.5, size - 0.5, size - 0.5),
            max(2.0, size * 0.22),
            TEAL_LIGHT,
            TEAL_DEEP,
        )
        draw = ImageDraw.Draw(img)
        font = _font(max(9, int(size * 0.7)))
        try:
            bbox = draw.textbbox((0, 0), "#", font=font)
            tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
            tx = (size - tw) / 2 - bbox[0]
            ty = (size - th) / 2 - bbox[1] - size * 0.04
        except Exception:
            tx, ty = size * 0.25, size * 0.05
        draw.text((tx, ty), "#", font=font, fill=PAPER)

    return img


def _png_bytes(img: Image.Image) -> bytes:
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def _bmp_icon_bytes(img: Image.Image) -> bytes:
    img = img.convert("RGBA")
    w, h = img.size
    xor = bytearray()
    pixels = img.load()
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            xor.extend((b, g, r, a))
    row_bytes = ((w + 31) // 32) * 4
    and_mask = bytearray(row_bytes * h)
    header = struct.pack(
        "<IiiHHIIiiII",
        40,
        w,
        h * 2,
        1,
        32,
        0,
        len(xor),
        0,
        0,
        0,
        0,
    )
    return bytes(header) + bytes(xor) + bytes(and_mask)


def save_multisize_ico(path: Path, images: list[Image.Image]) -> None:
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
    print("source: procedural deskNote notepad + # glyph")
    images = [_draw_desknote_icon(size) for size in ICON_SIZES]
    save_multisize_ico(OUT, images)
    preview = _draw_desknote_icon(256)
    preview.save(PREVIEW, format="PNG", optimize=True)
    print(f"{OUT} ({OUT.stat().st_size} bytes, sizes={list(ICON_SIZES)})")
    print(f"{PREVIEW} ({PREVIEW.stat().st_size} bytes preview)")


if __name__ == "__main__":
    main()
