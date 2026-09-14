"""Native Explorer desktop icon cell metrics (width × height).

Market source of truth: DefView ``SysListView32`` ``LVM_GETITEMSPACING`` —
the same pitch Explorer uses without DeskTidy. Falls back to
``SPI_ICONHORIZONTALSPACING`` / ``SPI_ICONVERTICALSPACING``, then safe defaults.

Comfort padding is **relative** (font metrics + fraction of native pitch), not a
fixed pixel add-on, so 100%/125%/150% DPI and different icon sizes keep captions
readable instead of clipping again on another machine.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from functools import lru_cache

# LVM_FIRST + 51
_LVM_GETITEMSPACING = 0x1000 + 51
_SPI_ICONHORIZONTALSPACING = 0x000D
_SPI_ICONVERTICALSPACING = 0x0018
_SPI_GETICONTITLEWRAP = 0x0019

# Classic Control Panel defaults when Explorer is unavailable.
_FALLBACK_CELL_W = 75
_FALLBACK_CELL_H = 75
_FALLBACK_ICON = 48

# Unselected 2-line shelf: aim for this many CJK cells per line before ellipsis.
_CAPTION_CHARS_PER_LINE = 8
# Width stays a bit above native; height follows the tight 2-line shelf (may be
# slightly under Explorer pitch — that empty band under captions was the bug).
_WIDTH_NATIVE_FACTOR = 1.55


@dataclass(frozen=True)
class DesktopIconCell:
    """One desktop icon slot (Explorer pitch + comfort padding)."""

    width: int
    height: int
    icon_size: int
    caption_lines: int  # 1 or 2 (SPI title wrap)


def clear_desktop_icon_metrics_cache() -> None:
    system_desktop_icon_cell.cache_clear()
    _caption_char_width.cache_clear()


@lru_cache(maxsize=1)
def _caption_char_width() -> int:
    """Advance width of one CJK cell in the shell icon-title font (logical px)."""
    try:
        from PyQt6.QtWidgets import QApplication

        # QFontMetrics without a QApplication aborts on Windows (0xC0000409).
        if QApplication.instance() is not None:
            from PyQt6.QtGui import QFontMetrics

            from src.desktop_caption import icon_title_qfont

            fm = QFontMetrics(icon_title_qfont())
            return max(8, int(fm.horizontalAdvance("国")))
    except Exception:
        pass
    # ~12px at 96 DPI; scale with system DPI when Qt font is unavailable.
    return max(8, int(round(12 * _dpi_scale())))


def _caption_line_height() -> int:
    try:
        from PyQt6.QtWidgets import QApplication

        if QApplication.instance() is not None:
            from PyQt6.QtGui import QFontMetrics

            from src.desktop_caption import icon_title_qfont

            fm = QFontMetrics(icon_title_qfont())
            return max(12, int(fm.lineSpacing()))
    except Exception:
        pass
    return max(12, int(round(18 * _dpi_scale())))


def _dpi_scale() -> float:
    """Logical DPI / 96 — keeps comfort padding proportional across monitors."""
    if sys.platform == "win32":
        try:
            import ctypes

            hdc = ctypes.windll.user32.GetDC(0)
            if hdc:
                try:
                    dpi = int(ctypes.windll.gdi32.GetDeviceCaps(hdc, 88))  # LOGPIXELSX
                    if dpi > 0:
                        return max(1.0, float(dpi) / 96.0)
                finally:
                    ctypes.windll.user32.ReleaseDC(0, hdc)
        except Exception:
            pass
    try:
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance()
        if app is not None:
            screen = app.primaryScreen()
            if screen is not None:
                dpi = float(screen.logicalDotsPerInch())
                if dpi > 0:
                    return max(1.0, dpi / 96.0)
    except Exception:
        pass
    return 1.0


def desktop_icon_footprint_height(
    icon_size: int, caption_lines: int = 2, *, scale: float = 1.0
) -> int:
    """Outer icon widget height: margins + glyph + gap + 2-line caption shelf.

    Shared by grid pitch and ``PublicIconWidget`` so CELL_H matches the painted
    shelf (no leftover empty band under the caption).
    """
    scale = max(0.5, float(scale))
    icon = max(16, int(icon_size))
    lines = max(1, int(caption_lines))
    gap = max(2, int(round(3 * scale)))
    # Symmetric margins — leftover cell height is centered around the stack.
    margin = max(2, int(round(3 * scale)))
    pad = max(10, int(round(10 * scale)))
    shelf = _caption_line_height() * lines + pad
    try:
        from PyQt6.QtWidgets import QApplication

        # QFontMetrics without a QApplication aborts on Windows (0xC0000409).
        if QApplication.instance() is not None:
            from src.desktop_caption import caption_box_height, icon_title_qfont

            shelf = caption_box_height(
                icon_title_qfont(), max_lines=lines, extra_pad=pad
            )
    except Exception:
        pass
    if abs(scale - 1.0) > 0.01:
        shelf = int(round(int(shelf) * scale))
    # Slight outer pitch above the tight stack so neighboring icons do not cover
    # captions; the widget centers the stack so the air is not all under text.
    stack = margin + icon + gap + max(16, int(shelf)) + margin
    # Keep a full line of air under the caption so the next icon does not
    # cover line-2 descenders (looked like ``Develop...`` / half 文件夹).
    pitch = max(8, int(round(_caption_line_height() * 0.7 * scale)))
    return stack + pitch


def _comfort_cell_size(
    native_w: int, native_h: int, icon: int, lines: int
) -> tuple[int, int]:
    """Widen native pitch for readable 2-line captions; keep height shelf-safe."""
    cjk = _caption_char_width()
    lines = max(1, int(lines))
    # Label ink budget + side chrome (glow / selection pad), all font-relative.
    side_chrome = max(cjk, int(round(10 * _dpi_scale())))
    target_w = cjk * int(_CAPTION_CHARS_PER_LINE) + side_chrome
    target_w = max(target_w, int(round(native_w * _WIDTH_NATIVE_FACTOR)))
    # Footprint uses a tight *label* shelf; outer cell must still clear Explorer
    # pitch so stacked icons do not cover the previous caption's second line.
    target_h = desktop_icon_footprint_height(icon, lines, scale=1.0)
    target_h = max(int(native_h), target_h)
    return max(native_w, target_w, icon + cjk * 4), max(48, target_h)


def _desktop_bag_icon_size() -> int | None:
    if sys.platform != "win32":
        return None
    try:
        import winreg

        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Software\Microsoft\Windows\Shell\Bags\1\Desktop",
        ) as key:
            raw, _ = winreg.QueryValueEx(key, "IconSize")
            size = int(raw)
            if 16 <= size <= 256:
                return size
    except OSError:
        return None
    except Exception:
        return None
    return None


def _spi_cell() -> tuple[int, int, int] | None:
    if sys.platform != "win32":
        return None
    try:
        import ctypes
        from ctypes import wintypes

        user32 = ctypes.windll.user32
        h = wintypes.UINT()
        v = wintypes.UINT()
        wrap = wintypes.BOOL()
        ok_h = user32.SystemParametersInfoW(
            _SPI_ICONHORIZONTALSPACING, 0, ctypes.byref(h), 0
        )
        ok_v = user32.SystemParametersInfoW(
            _SPI_ICONVERTICALSPACING, 0, ctypes.byref(v), 0
        )
        user32.SystemParametersInfoW(_SPI_GETICONTITLEWRAP, 0, ctypes.byref(wrap), 0)
        if not ok_h or not ok_v:
            return None
        cw, ch = int(h.value), int(v.value)
        if cw < 32 or ch < 32:
            return None
        return cw, ch, 2 if int(wrap.value) else 1
    except Exception:
        return None


def _defview_item_spacing() -> tuple[int, int] | None:
    """Explorer ListView large-icon spacing (actual desktop pitch)."""
    if sys.platform != "win32":
        return None
    try:
        import ctypes

        from src.win_shell import _find_desktop_listview

        lv = int(_find_desktop_listview() or 0)
        if not lv:
            return None
        raw = int(ctypes.windll.user32.SendMessageW(lv, _LVM_GETITEMSPACING, 0, 0))
        if raw <= 0:
            return None
        cw = raw & 0xFFFF
        ch = (raw >> 16) & 0xFFFF
        if cw < 32 or ch < 32:
            return None
        return cw, ch
    except Exception:
        return None


@lru_cache(maxsize=1)
def system_desktop_icon_cell() -> DesktopIconCell:
    """Explorer pitch plus DPI/font-scaled comfort size for DeskTidy overlays."""
    icon = _desktop_bag_icon_size() or _FALLBACK_ICON
    lines = 2
    spi = _spi_cell()
    if spi is not None:
        _cw, _ch, lines = spi

    spacing = _defview_item_spacing()
    if spacing is not None:
        cw, ch = spacing
    elif spi is not None:
        cw, ch = spi[0], spi[1]
    else:
        cw, ch = _FALLBACK_CELL_W, _FALLBACK_CELL_H

    min_caption = max(16, _caption_line_height())
    if ch < icon + min_caption:
        ch = icon + min_caption
    cw, ch = _comfort_cell_size(int(cw), int(ch), int(icon), int(lines))
    return DesktopIconCell(
        width=max(48, int(cw)),
        height=max(icon + min_caption, int(ch)),
        icon_size=int(icon),
        caption_lines=2 if lines >= 2 else 1,
    )
