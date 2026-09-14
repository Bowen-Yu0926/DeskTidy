"""Explorer-style desktop icon captions for public floats.

Uses the shell icon-title font (``SPI_GETICONTITLELOGFONT``) and white
glyphs with a soft dark glow — the DefView look on wallpaper.

No drop-shadow / offset shade: native desktop labels use a glow, not a
cast shadow under the text.

``DrawThemeTextEx`` + ``DTT_GLOWSIZE`` is attempted first; when it emits
white-only glyphs we fall back to a soft glow painter (no hard outline).
"""

from __future__ import annotations

import ctypes
import sys
from ctypes import wintypes
from functools import lru_cache

from PyQt6.QtCore import QPointF, Qt
from PyQt6.QtGui import (
    QColor,
    QFont,
    QFontMetrics,
    QImage,
    QPainter,
    QPixmap,
    QTextLayout,
    QTextOption,
)

SPI_GETICONTITLELOGFONT = 0x001F
DTT_TEXTCOLOR = 0x00000001
DTT_GLOWSIZE = 0x00000800
DTT_COMPOSITED = 0x00002000
DT_CENTER = 0x00000001
DT_WORDBREAK = 0x00000010
DT_NOPREFIX = 0x00000800
DT_END_ELLIPSIS = 0x00008000
DT_EDITCONTROL = 0x00002000
BI_RGB = 0
DIB_RGB_COLORS = 0


class LOGFONTW(ctypes.Structure):
    _fields_ = [
        ("lfHeight", wintypes.LONG),
        ("lfWidth", wintypes.LONG),
        ("lfEscapement", wintypes.LONG),
        ("lfOrientation", wintypes.LONG),
        ("lfWeight", wintypes.LONG),
        ("lfItalic", wintypes.BYTE),
        ("lfUnderline", wintypes.BYTE),
        ("lfStrikeOut", wintypes.BYTE),
        ("lfCharSet", wintypes.BYTE),
        ("lfOutPrecision", wintypes.BYTE),
        ("lfClipPrecision", wintypes.BYTE),
        ("lfQuality", wintypes.BYTE),
        ("lfPitchAndFamily", wintypes.BYTE),
        ("lfFaceName", wintypes.WCHAR * 32),
    ]


class POINT(ctypes.Structure):
    _fields_ = [("x", wintypes.LONG), ("y", wintypes.LONG)]


class DTTOPTS(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("crText", wintypes.COLORREF),
        ("crBorder", wintypes.COLORREF),
        ("crShadow", wintypes.COLORREF),
        ("iTextShadowType", ctypes.c_int),
        ("ptShadowOffset", POINT),
        ("iBorderSize", ctypes.c_int),
        ("iFontPropId", ctypes.c_int),
        ("iColorPropId", ctypes.c_int),
        ("iStateId", ctypes.c_int),
        ("fApplyOverlay", wintypes.BOOL),
        ("iGlowSize", ctypes.c_int),
        ("pfnDrawTextCallback", ctypes.c_void_p),
        ("lParam", wintypes.LPARAM),
    ]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ("biSize", wintypes.DWORD),
        ("biWidth", wintypes.LONG),
        ("biHeight", wintypes.LONG),
        ("biPlanes", wintypes.WORD),
        ("biBitCount", wintypes.WORD),
        ("biCompression", wintypes.DWORD),
        ("biSizeImage", wintypes.DWORD),
        ("biXPelsPerMeter", wintypes.LONG),
        ("biYPelsPerMeter", wintypes.LONG),
        ("biClrUsed", wintypes.DWORD),
        ("biClrImportant", wintypes.DWORD),
    ]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [
        ("bmiHeader", BITMAPINFOHEADER),
        ("bmiColors", wintypes.DWORD * 3),
    ]


class RECT(ctypes.Structure):
    _fields_ = [
        ("left", wintypes.LONG),
        ("top", wintypes.LONG),
        ("right", wintypes.LONG),
        ("bottom", wintypes.LONG),
    ]


def _bind_apis() -> tuple:
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
    uxtheme = ctypes.WinDLL("uxtheme", use_last_error=True)

    user32.SystemParametersInfoW.argtypes = [
        wintypes.UINT,
        wintypes.UINT,
        wintypes.LPVOID,
        wintypes.UINT,
    ]
    user32.SystemParametersInfoW.restype = wintypes.BOOL
    user32.GetDC.argtypes = [wintypes.HWND]
    user32.GetDC.restype = wintypes.HDC
    user32.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
    user32.ReleaseDC.restype = ctypes.c_int

    gdi32.CreateCompatibleDC.argtypes = [wintypes.HDC]
    gdi32.CreateCompatibleDC.restype = wintypes.HDC
    gdi32.DeleteDC.argtypes = [wintypes.HDC]
    gdi32.DeleteDC.restype = wintypes.BOOL
    gdi32.CreateDIBSection.argtypes = [
        wintypes.HDC,
        ctypes.POINTER(BITMAPINFO),
        wintypes.UINT,
        ctypes.POINTER(ctypes.c_void_p),
        wintypes.HANDLE,
        wintypes.DWORD,
    ]
    gdi32.CreateDIBSection.restype = wintypes.HBITMAP
    gdi32.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
    gdi32.SelectObject.restype = wintypes.HGDIOBJ
    gdi32.DeleteObject.argtypes = [wintypes.HGDIOBJ]
    gdi32.DeleteObject.restype = wintypes.BOOL
    gdi32.CreateFontIndirectW.argtypes = [ctypes.POINTER(LOGFONTW)]
    gdi32.CreateFontIndirectW.restype = wintypes.HFONT

    uxtheme.OpenThemeData.argtypes = [wintypes.HWND, wintypes.LPCWSTR]
    uxtheme.OpenThemeData.restype = wintypes.HANDLE
    uxtheme.CloseThemeData.argtypes = [wintypes.HANDLE]
    uxtheme.CloseThemeData.restype = ctypes.c_long
    uxtheme.DrawThemeTextEx.argtypes = [
        wintypes.HANDLE,
        wintypes.HDC,
        ctypes.c_int,
        ctypes.c_int,
        wintypes.LPCWSTR,
        ctypes.c_int,
        wintypes.DWORD,
        ctypes.POINTER(RECT),
        ctypes.POINTER(DTTOPTS),
    ]
    uxtheme.DrawThemeTextEx.restype = ctypes.c_long
    return user32, gdi32, uxtheme


@lru_cache(maxsize=1)
def icon_title_qfont() -> QFont:
    """Return the shell icon-title font (``SPI_GETICONTITLELOGFONT``)."""
    font = QFont()
    if sys.platform != "win32":
        font.setPointSize(9)
        return font
    try:
        user32, _, _ = _bind_apis()
        lf = LOGFONTW()
        ok = user32.SystemParametersInfoW(
            SPI_GETICONTITLELOGFONT,
            ctypes.sizeof(LOGFONTW),
            ctypes.byref(lf),
            0,
        )
        if not ok:
            raise OSError("SystemParametersInfoW failed")
        face = str(lf.lfFaceName).strip("\x00") or "Segoe UI"
        font.setFamily(face)
        height = abs(int(lf.lfHeight)) or 12
        font.setPixelSize(height)
        weight = int(lf.lfWeight) or 400
        try:
            font.setWeight(QFont.Weight(max(1, min(1000, weight))))
        except Exception:
            font.setBold(weight >= 600)
        font.setItalic(bool(lf.lfItalic))
        font.setStyleStrategy(
            QFont.StyleStrategy.PreferDefault | QFont.StyleStrategy.PreferQuality
        )
        return font
    except Exception:
        font.setPointSize(9)
        return font


def clear_desktop_caption_caches() -> None:
    """Drop cached fonts after DPI / theme changes."""
    icon_title_qfont.cache_clear()


def _rgb(r: int, g: int, b: int) -> int:
    return int(r) | (int(g) << 8) | (int(b) << 16)


def _font_pixel_size(font: QFont, dpr: float) -> int:
    px = font.pixelSize()
    if px > 0:
        return max(1, int(round(px * dpr)))
    return max(1, int(round((font.pointSizeF() or 9.0) * dpr * 96.0 / 72.0)))


def _image_has_dark_halo(image: QImage) -> bool:
    """True if theme output includes a readable dark glow (not white-only)."""
    if image is None or image.isNull():
        return False
    dark = 0
    step = max(1, image.width() // 40)
    for y in range(0, image.height(), step):
        for x in range(0, image.width(), step):
            c = image.pixelColor(x, y)
            if c.alpha() < 24:
                continue
            if c.red() < 70 and c.green() < 70 and c.blue() < 70:
                dark += 1
                if dark >= 3:
                    return True
    return False


def _caption_ink_inset_px(dpr: float) -> int:
    """Physical px to keep glyphs + glow inside the cropped pixmap (DPI-scaled)."""
    dpr = max(1.0, float(dpr or 1.0))
    # Soft glow radius ≈2 logical px; keep ink that far from the crop edge.
    return max(3, int(round(3 * dpr)))


def _caption_ink_inset_logical(dpr: float) -> int:
    """Logical px matching ``_caption_ink_inset_px`` for elide/wrap width."""
    dpr = max(1.0, float(dpr or 1.0))
    return max(2, int(round(_caption_ink_inset_px(dpr) / dpr)))


def _draw_theme_caption(
    text: str,
    width: int,
    height: int,
    *,
    font: QFont,
    glow_size: int,
    dpr: float,
) -> QImage | None:
    """Try shell ``DrawThemeTextEx`` glow; may return white-only on Win10/11."""
    if sys.platform != "win32" or width < 8 or height < 8:
        return None
    # Pre-broken elided text must use the Qt line painter — DT_WORDBREAK would
    # re-wrap and recreate 孤字 + a clipped third line in the 2-line shelf.
    if "\n" in (text or ""):
        return None
    try:
        user32, gdi32, uxtheme = _bind_apis()
    except Exception:
        return None

    phys_w = max(8, int(round(width * dpr)))
    phys_h = max(8, int(round(height * dpr)))
    ink = _caption_ink_inset_px(dpr)
    pad = max(ink + 2, int(round(glow_size * dpr)) + 2)
    dib_w = phys_w + pad * 2
    dib_h = phys_h + pad * 2

    hdc_screen = user32.GetDC(0)
    if not hdc_screen:
        return None
    hdc = gdi32.CreateCompatibleDC(hdc_screen)
    user32.ReleaseDC(0, hdc_screen)
    if not hdc:
        return None

    bmi = BITMAPINFO()
    bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmi.bmiHeader.biWidth = dib_w
    bmi.bmiHeader.biHeight = -dib_h
    bmi.bmiHeader.biPlanes = 1
    bmi.bmiHeader.biBitCount = 32
    bmi.bmiHeader.biCompression = BI_RGB

    bits = ctypes.c_void_p()
    hbmp = gdi32.CreateDIBSection(
        hdc,
        ctypes.byref(bmi),
        DIB_RGB_COLORS,
        ctypes.byref(bits),
        None,
        0,
    )
    if not hbmp or not bits.value:
        gdi32.DeleteDC(hdc)
        return None

    old_bmp = gdi32.SelectObject(hdc, hbmp)
    ctypes.memset(bits, 0, dib_w * dib_h * 4)

    px = _font_pixel_size(font, dpr)
    face = font.family() or "Segoe UI"
    lf = LOGFONTW()
    lf.lfHeight = -px
    lf.lfWeight = 400
    lf.lfCharSet = 1
    lf.lfQuality = 5
    lf.lfFaceName = face[:31]
    hfont = gdi32.CreateFontIndirectW(ctypes.byref(lf))
    old_font = gdi32.SelectObject(hdc, hfont) if hfont else None

    htheme = uxtheme.OpenThemeData(None, "TextStyle")
    if not htheme:
        htheme = uxtheme.OpenThemeData(None, "CompositedText")

    image: QImage | None = None
    try:
        if htheme:
            opts = DTTOPTS()
            opts.dwSize = ctypes.sizeof(DTTOPTS)
            opts.dwFlags = DTT_COMPOSITED | DTT_GLOWSIZE | DTT_TEXTCOLOR
            opts.crText = _rgb(255, 255, 255)
            opts.iGlowSize = max(1, int(round(glow_size * dpr)))
            # Horizontal inset only — vertical glow lives in DIB ``pad``; a
            # vertical ink shrink clipped the second caption line in half.
            rc = RECT(
                pad + ink,
                pad,
                pad + phys_w - ink,
                pad + phys_h,
            )
            flags = (
                DT_CENTER
                | DT_WORDBREAK
                | DT_NOPREFIX
                | DT_EDITCONTROL
                | DT_END_ELLIPSIS
            )
            hr = uxtheme.DrawThemeTextEx(
                htheme,
                hdc,
                0,
                0,
                text,
                -1,
                flags,
                ctypes.byref(rc),
                ctypes.byref(opts),
            )
            if int(hr) >= 0:
                buf = ctypes.string_at(bits, dib_w * dib_h * 4)
                full = QImage(
                    buf,
                    dib_w,
                    dib_h,
                    dib_w * 4,
                    QImage.Format.Format_ARGB32_Premultiplied,
                ).copy()
                image = full.copy(pad, pad, phys_w, phys_h)
                image.setDevicePixelRatio(dpr)
    finally:
        if htheme:
            uxtheme.CloseThemeData(htheme)
        if old_font is not None:
            gdi32.SelectObject(hdc, old_font)
        if hfont:
            gdi32.DeleteObject(hfont)
        gdi32.SelectObject(hdc, old_bmp)
        gdi32.DeleteObject(hbmp)
        gdi32.DeleteDC(hdc)

    return image


def _qt_explorer_caption(
    text: str,
    width: int,
    height: int,
    *,
    font: QFont,
    dpr: float,
) -> QImage:
    """White caption + soft glow only (no offset drop-shadow).

    Matches Explorer/DefView: readable on light wallpaper via a symmetric
    glow, without the hard outline + downward shade that looked like a
    title shadow after fence→public unpin.

    Pre-elided captions already contain ``\\n`` breaks — draw each line
    single-line. Re-wrapping with ``TextWrapAnywhere`` recreates 孤字 and a
    clipped third row inside the 2-line shelf.
    """
    from PyQt6.QtCore import QRect

    phys_w = max(8, int(round(width * dpr)))
    phys_h = max(8, int(round(height * dpr)))
    ink = _caption_ink_inset_px(dpr)
    # Outer pad holds glow bleed; ink keeps glyph edges off the final crop.
    pad = ink + max(2, int(round(2 * dpr)))
    canvas_w = phys_w + pad * 2
    canvas_h = phys_h + pad * 2
    img = QImage(canvas_w, canvas_h, QImage.Format.Format_ARGB32_Premultiplied)
    img.fill(Qt.GlobalColor.transparent)

    painter = QPainter(img)
    painter.setRenderHint(QPainter.RenderHint.TextAntialiasing, True)
    scaled = QFont(font)
    scaled.setPixelSize(_font_pixel_size(font, dpr))
    painter.setFont(scaled)
    # Horizontal inset only. Vertical ink used to shrink the paint box so line 2
    # was drawn then cropped — the second row showed as half-glyphs.
    # Vertical glow bleeds into the outer canvas ``pad``, then we crop to phys.
    rect = QRect(pad + ink, pad, max(1, phys_w - 2 * ink), phys_h)
    lines = [ln for ln in (text or "").replace("\r\n", "\n").replace("\r", "\n").split("\n") if ln]
    if not lines:
        lines = [" "]
    fm = QFontMetrics(scaled)
    # Same pitch as ``caption_box_height``.
    line_h = max(
        1,
        int(fm.lineSpacing()),
        int(fm.height() + max(0, fm.descent())),
    )
    single_flags = int(
        Qt.AlignmentFlag.AlignHCenter
        | Qt.AlignmentFlag.AlignTop
        | Qt.TextFlag.TextSingleLine
    )

    def _paint_lines(pen: QColor, ox: int = 0, oy: int = 0) -> None:
        painter.setPen(pen)
        y = rect.top() + oy
        for ln in lines:
            row = QRect(rect.left() + ox, y, rect.width(), line_h)
            painter.drawText(row, single_flags, ln)
            y += line_h

    # Soft symmetric glow only (outer ring then inner). No hard 1px outline,
    # no downward offset — those read as an artificial title shadow.
    r_outer = max(1, int(round(2 * dpr)))
    for dx in range(-r_outer, r_outer + 1):
        for dy in range(-r_outer, r_outer + 1):
            if dx == 0 and dy == 0:
                continue
            if dx * dx + dy * dy > r_outer * r_outer:
                continue
            _paint_lines(QColor(0, 0, 0, 70), dx, dy)

    r_inner = max(1, int(round(dpr)))
    for dx in range(-r_inner, r_inner + 1):
        for dy in range(-r_inner, r_inner + 1):
            if dx == 0 and dy == 0:
                continue
            if dx * dx + dy * dy > r_inner * r_inner:
                continue
            _paint_lines(QColor(0, 0, 0, 110), dx, dy)

    _paint_lines(QColor(255, 255, 255, 255))
    painter.end()

    cropped = img.copy(pad, pad, phys_w, phys_h)
    cropped.setDevicePixelRatio(dpr)
    return cropped


def _is_cjk_char(ch: str) -> bool:
    if not ch:
        return False
    o = ord(ch[0])
    return (
        0x4E00 <= o <= 0x9FFF
        or 0x3400 <= o <= 0x4DBF
        or 0xF900 <= o <= 0xFAFF
        or 0x3000 <= o <= 0x303F
    )


# Frequent filename digrams that must not split across the caption break.
_CJK_KEEP_DIGRAMS = frozenset(
    {
        "合同",
        "公司",
        "集团",
        "有限",
        "外购",
        "签订",
        "模板",
        "测试",
        "方案",
        "报告",
        "文件",
        "资料",
        "项目",
        "技术",
        "协议",
        "软件",
        "系统",
        "管理",
        "会议",
        "纪要",
        "通知",
        "申请",
        "审批",
        "预算",
        "结算",
        "发票",
        "清单",
        "明细",
        "汇总",
        "分析",
        "统计",
        "报表",
        "副本",
        "原稿",
        "最终",
        "版本",
        "更新",
        "发布",
        "说明",
        "手册",
        "指南",
        "规范",
        "标准",
        "流程",
        "制度",
        "办法",
        "计划",
        "总结",
        "周报",
        "月报",
        "年报",
        "附件",
        "附录",
        "目录",
        "封面",
        "正文",
        "草稿",
        "修订",
        "确认",
        "验收",
        "交付",
        "采购",
        "销售",
        "客户",
        "供应商",
        "订单",
    }
)


def _should_keep_cjk_pair(left: str, right: str) -> bool:
    if not left or not right:
        return False
    if not (_is_cjk_char(left[-1]) and _is_cjk_char(right[0])):
        return False
    return (left[-1] + right[0]) in _CJK_KEEP_DIGRAMS


def _balance_caption_orphan_lines(parts: list[str], *, min_last: int = 2) -> list[str]:
    """Avoid lonely CJK glyphs (孤字) like 「合」/「同」 alone on a line.

    1. No line shorter than ``min_last`` when a previous line can donate.
    2. At a CJK|CJK break, pull one char down once so ``…合`` / ``同…``
       becomes ``…`` / ``合同…`` (common 2-char word split on desktop labels).
    """
    if len(parts) < 2:
        return list(parts)
    out = [p for p in parts if p]
    if len(out) < 2:
        return out
    min_last = max(1, int(min_last))

    # Fix mid-list single-char lines first (rewrap artifacts / tight widths).
    i = 0
    while i < len(out):
        if 0 < len(out[i]) < min_last:
            if i > 0 and len(out[i - 1]) > min_last:
                need = min_last - len(out[i])
                donor = out[i - 1]
                if len(donor) - need >= 1:
                    out[i - 1] = donor[:-need]
                    out[i] = donor[-need:] + out[i]
                    continue
            if i > 0:
                out[i - 1] = out[i - 1] + out[i]
                out.pop(i)
                i = max(0, i - 1)
                continue
        i += 1

    while (
        len(out) >= 2
        and 0 < len(out[-1]) < min_last
        and len(out[-2]) > min_last
    ):
        need = min_last - len(out[-1])
        donor = out[-2]
        if len(donor) - need < 1:
            break
        out[-2] = donor[:-need]
        out[-1] = donor[-need:] + out[-1]
    if len(out) >= 2 and 0 < len(out[-1]) < min_last:
        out[-2] = out[-2] + out[-1]
        out.pop()
    return out


def elide_desktop_caption_text(
    text: str,
    width: int,
    font: QFont | None = None,
    *,
    max_lines: int = 2,
) -> str:
    """Explorer-like wrap: at most ``max_lines``, ellipsis on the last line.

    Unselected icons use ``max_lines=2``. Selected icons pass a larger
    ``max_lines`` (or skip elide) so the full name can show.

    Also avoids a one-character last line (CJK 孤字) by borrowing from the
    previous line — e.g. ``…集团合`` / ``同`` becomes ``…集团`` / ``合同``.
    At a CJK|CJK break, prefers pulling one character down when that keeps
    ``合同``-style pairs together without recreating an orphan line.
    """
    text = (text or "").replace("\r\n", "\n").replace("\r", "\n").strip()
    if not text:
        return ""
    max_lines = max(1, int(max_lines))
    width = max(8, int(width))
    use_font = QFont(font) if font is not None else icon_title_qfont()
    option = QTextOption()
    # Prefer word breaks for Latin (``Android`` / ``Developer``); still wrap
    # mid-token for CJK and long extensions like ``.docx`` when needed.
    option.setWrapMode(QTextOption.WrapMode.WrapAtWordBoundaryOrAnywhere)
    option.setAlignment(Qt.AlignmentFlag.AlignHCenter | Qt.AlignmentFlag.AlignTop)

    layout = QTextLayout(text, use_font)
    layout.setTextOption(option)
    layout.beginLayout()
    lines: list = []
    y = 0.0
    while len(lines) < max_lines:
        line = layout.createLine()
        if not line.isValid():
            break
        line.setLineWidth(width)
        line.setPosition(QPointF(0, y))
        y += float(line.height())
        lines.append(line)
    layout.endLayout()
    if not lines:
        return text

    raw: list[str] = []
    for line in lines:
        start = int(line.textStart())
        raw.append(text[start : start + int(line.textLength())])

    last = lines[-1]
    consumed = int(last.textStart() + last.textLength())
    fm = QFontMetrics(use_font)

    def _finalize(chunks: list[str], *, overflow: bool) -> str:
        balanced = _balance_caption_orphan_lines(chunks, min_last=2)
        # Keep common digrams like 「合同」 on one line when WrapAnywhere split them.
        if (
            len(balanced) == 2
            and len(balanced[0]) > 2
            and _should_keep_cjk_pair(balanced[0], balanced[1])
        ):
            pulled = [balanced[0][:-1], balanced[0][-1] + balanced[1]]
            pulled = _balance_caption_orphan_lines(pulled, min_last=2)
            if all(len(p) >= 2 for p in pulled) and fm.horizontalAdvance(pulled[0]) >= 8:
                balanced = pulled
        if not overflow:
            return "\n".join(balanced)
        if len(balanced) == 1:
            return fm.elidedText(balanced[0], Qt.TextElideMode.ElideRight, width)
        head = list(balanced[:-1])
        head.append(
            fm.elidedText(balanced[-1], Qt.TextElideMode.ElideRight, width)
        )
        # Elide may leave a 1-char last line at tiny widths — fold again.
        head = _balance_caption_orphan_lines(head, min_last=2)
        if len(head) >= 2:
            head[-1] = fm.elidedText(head[-1], Qt.TextElideMode.ElideRight, width)
        elif head:
            head[0] = fm.elidedText(head[0], Qt.TextElideMode.ElideRight, width)
        return "\n".join(head)

    if consumed >= len(text):
        return _finalize(raw, overflow=False)

    head = raw[:-1]
    rest = text[int(last.textStart()) :]
    return _finalize(head + [rest], overflow=True)


def render_desktop_caption(
    text: str,
    width: int,
    height: int,
    *,
    dpr: float = 1.0,
    font: QFont | None = None,
    max_lines: int | None = 2,
    elide: bool = True,
) -> QPixmap:
    """Render a wrapped desktop icon caption pixmap (logical ``width``×``height``).

    Default matches Explorer: wrap within ``max_lines`` and ellipsize overflow.
    Pass ``elide=False`` (selected icon) to paint the full name in ``height``.
    """
    text = (text or "").strip() or " "
    width = max(16, int(width))
    height = max(16, int(height))
    dpr = max(1.0, float(dpr or 1.0))
    use_font = QFont(font) if font is not None else icon_title_qfont()
    # Wrap/elide in the same inset the painter uses — otherwise glyphs that
    # fit the elider still paint flush to the pixmap edge and look clipped.
    ink = _caption_ink_inset_logical(dpr)
    layout_w = max(8, width - 2 * ink)
    if elide and max_lines is not None and int(max_lines) > 0:
        text = elide_desktop_caption_text(
            text, layout_w, use_font, max_lines=int(max_lines)
        ) or " "

    theme = _draw_theme_caption(
        text,
        width,
        height,
        font=use_font,
        glow_size=12,
        dpr=dpr,
    )
    if theme is not None and not theme.isNull() and _image_has_dark_halo(theme):
        image = theme
    else:
        image = _qt_explorer_caption(
            text, width, height, font=use_font, dpr=dpr
        )
    pix = QPixmap.fromImage(image)
    pix.setDevicePixelRatio(dpr)
    return pix


def caption_box_height(
    font: QFont | None = None, *, max_lines: int = 2, extra_pad: int = 6
) -> int:
    """Logical height for an Explorer-like multi-line caption box.

    Must clear ``max_lines`` of glyphs *including* descent and the soft glow;
    a too-tight shelf was clipping the bottom half of line 2.
    """
    use_font = QFont(font) if font is not None else icon_title_qfont()
    fm = QFontMetrics(use_font)
    # Match ``_qt_explorer_caption`` row pitch (lineSpacing), not a shorter
    # height() that leaves descent / glow hanging outside the shelf.
    line_h = max(1, int(fm.lineSpacing()), int(fm.height() + max(0, fm.descent())))
    try:
        ink = _caption_ink_inset_logical(screen_device_pixel_ratio())
    except Exception:
        ink = 3
    # Last-line descent + outer glow must sit *inside* the shelf. A 6px pad
    # still shaved 「件」/ ``p``/ ``g`` and made ``Developer`` look like ``Develop...``.
    glow = max(4, int(ink) + 1)
    pad = max(int(extra_pad), int(fm.descent()) + glow * 2 + 4)
    return int(line_h * max(1, max_lines) + pad)


def caption_needed_height(
    text: str,
    width: int,
    font: QFont | None = None,
    *,
    max_lines: int = 4,
    extra_pad: int = 6,
) -> int:
    """Height that fits *text* wrapped in *width*, capped at *max_lines*.

    Explorer/Fences wrap desktop filenames mid-token (CJK + ``.xlsx``). A
    fixed 2-line shelf plus ellipsis is what made public-area names look cut off.
    """
    from PyQt6.QtCore import QRect

    use_font = QFont(font) if font is not None else icon_title_qfont()
    fm = QFontMetrics(use_font)
    line_h = max(fm.lineSpacing(), fm.height() + max(0, fm.descent()))
    max_lines = max(1, int(max_lines))
    width = max(16, int(width))
    text = (text or "").strip()
    wrap_flags = int(
        Qt.AlignmentFlag.AlignHCenter
        | Qt.AlignmentFlag.AlignTop
        | Qt.TextFlag.TextWordWrap
        | Qt.TextFlag.TextWrapAnywhere
    )
    if text:
        br = fm.boundingRect(
            QRect(0, 0, width, line_h * max_lines * 4),
            wrap_flags,
            text,
        )
        lines = max(1, (int(br.height()) + line_h - 1) // max(1, line_h))
        lines = min(max_lines, lines)
    else:
        lines = 1
    return int(line_h * lines + max(0, extra_pad))


def screen_device_pixel_ratio() -> float:
    """Best-effort DPR for caption rasterization."""
    try:
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance()
        if app is not None:
            screen = app.primaryScreen()
            if screen is not None:
                return max(1.0, float(screen.devicePixelRatio()))
    except Exception:
        pass
    return 1.0
