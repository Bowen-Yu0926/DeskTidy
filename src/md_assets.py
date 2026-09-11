"""Save clipboard / dropped images into a note-local assets/ folder."""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

IMAGE_SUFFIXES = frozenset(
    {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".tif", ".tiff"}
)


def is_image_path(path: Path | str | None) -> bool:
    if path is None:
        return False
    try:
        p = Path(path)
    except OSError:
        return False
    return p.suffix.casefold() in IMAGE_SUFFIXES


def assets_dir_for(base_dir: Path, *, ensure: bool = True) -> Path:
    folder = Path(base_dir) / "assets"
    if ensure:
        folder.mkdir(parents=True, exist_ok=True)
    return folder


def _unique_asset_path(folder: Path, suffix: str) -> Path:
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    suffix = suffix if suffix.startswith(".") else f".{suffix}"
    suffix = suffix.casefold()
    path = folder / f"img_{stamp}{suffix}"
    n = 1
    while path.exists():
        path = folder / f"img_{stamp}_{n:02d}{suffix}"
        n += 1
    return path


def save_image_bytes(
    data: bytes,
    base_dir: Path,
    *,
    suffix: str = ".png",
) -> tuple[Path, str]:
    """Write *data* under ``base_dir/assets/``.

    Returns ``(absolute_path, markdown_snippet)`` with a relative ``assets/…`` link.
    """
    if not data:
        raise ValueError("empty image data")
    folder = assets_dir_for(base_dir, ensure=True)
    dest = _unique_asset_path(folder, suffix)
    dest.write_bytes(data)
    rel = Path("assets") / dest.name
    # Markdown uses forward slashes.
    link = rel.as_posix()
    return dest, f"![]({link})"


def save_qimage(image, base_dir: Path, *, suffix: str = ".png") -> tuple[Path, str]:
    """Save a QImage / QPixmap-compatible image to assets/."""
    from PyQt6.QtCore import QByteArray, QBuffer, QIODevice
    from PyQt6.QtGui import QImage

    if image is None or image.isNull():
        raise ValueError("null image")
    qimg = image if isinstance(image, QImage) else image.toImage()
    fmt = "PNG" if suffix.casefold() in {".png", "png"} else "JPEG"
    if suffix.casefold() in {".jpg", ".jpeg", "jpg", "jpeg"}:
        fmt = "JPEG"
        suffix = ".jpg"
    else:
        suffix = ".png"
        fmt = "PNG"
    buf = QByteArray()
    device = QBuffer(buf)
    device.open(QIODevice.OpenModeFlag.WriteOnly)
    if not qimg.save(device, fmt):
        raise OSError("failed to encode image")
    device.close()
    return save_image_bytes(bytes(buf), base_dir, suffix=suffix)


def copy_image_file(src: Path, base_dir: Path) -> tuple[Path, str]:
    """Copy an existing image into assets/ (keeps original suffix when known)."""
    src = Path(src)
    data = src.read_bytes()
    suffix = src.suffix.casefold() if src.suffix else ".png"
    if suffix not in IMAGE_SUFFIXES:
        suffix = ".png"
    return save_image_bytes(data, base_dir, suffix=suffix)


def count_text_stats(text: str) -> tuple[int, int]:
    """Return ``(chars_no_whitespace, words)`` for status bar."""
    if not text:
        return 0, 0
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    chars = len("".join(normalized.split()))
    words = len(normalized.split())
    return chars, words
