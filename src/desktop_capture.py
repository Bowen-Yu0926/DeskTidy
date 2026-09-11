"""Capture desktop pixels for region screenshot."""

from __future__ import annotations

import sys
import threading

from PyQt6.QtCore import QPoint, QRect, Qt
from PyQt6.QtGui import QGuiApplication, QImage, QPainter, QPixmap

# Cold boot: GDI BitBlt of the desktop can stall for a minute+ while DWM/GPU
# come up. Never block the UI thread that long — fall back or fail fast.
_GDI_GRAB_TIMEOUT_S = 1.8


def _virtual_desktop_rect() -> QRect:
    app = QGuiApplication.instance()
    if app is None:
        return QRect(0, 0, 0, 0)
    screens = app.screens()
    if not screens:
        return QRect(0, 0, 0, 0)
    rect = screens[0].geometry()
    for screen in screens[1:]:
        rect = rect.united(screen.geometry())
    return rect


def _grab_desktop_qt() -> tuple[QPixmap, QPoint]:
    app = QGuiApplication.instance()
    if app is None:
        return QPixmap(), QPoint(0, 0)

    screens = app.screens()
    if not screens:
        return QPixmap(), QPoint(0, 0)

    geo = _virtual_desktop_rect()
    result = QPixmap(geo.size())
    result.fill(Qt.GlobalColor.black)

    painter = QPainter(result)
    for screen in screens:
        sg = screen.geometry()
        shot = screen.grabWindow(0)
        if shot.isNull():
            continue
        target_w, target_h = sg.width(), sg.height()
        if shot.width() != target_w or shot.height() != target_h:
            shot = shot.scaled(
                target_w,
                target_h,
                Qt.AspectRatioMode.IgnoreAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            )
        painter.drawPixmap(sg.x() - geo.x(), sg.y() - geo.y(), shot)
    painter.end()
    return result, geo.topLeft()


def _grab_desktop_win32() -> tuple[QPixmap, QPoint]:
    import win32api
    import win32con
    import win32gui
    import win32ui

    left = win32api.GetSystemMetrics(win32con.SM_XVIRTUALSCREEN)
    top = win32api.GetSystemMetrics(win32con.SM_YVIRTUALSCREEN)
    width = win32api.GetSystemMetrics(win32con.SM_CXVIRTUALSCREEN)
    height = win32api.GetSystemMetrics(win32con.SM_CYVIRTUALSCREEN)
    if width <= 0 or height <= 0:
        return QPixmap(), QPoint(0, 0)

    desktop_hwnd = win32gui.GetDesktopWindow()
    desktop_dc = win32gui.GetWindowDC(desktop_hwnd)
    try:
        img_dc = win32ui.CreateDCFromHandle(desktop_dc)
        mem_dc = img_dc.CreateCompatibleDC()
        bitmap = win32ui.CreateBitmap()
        bitmap.CreateCompatibleBitmap(img_dc, width, height)
        mem_dc.SelectObject(bitmap)
        mem_dc.BitBlt((0, 0), (width, height), img_dc, (left, top), win32con.SRCCOPY)

        bits = bitmap.GetBitmapBits(True)
        # GDI 32-bpp is BGRA in memory — same layout as Qt Format_RGB32 on
        # little-endian Windows. Do NOT rgbSwapped(); that swaps R/B and makes
        # desktop icons look yellow/orange in the capture.
        image = QImage(bits, width, height, width * 4, QImage.Format.Format_RGB32)
        image = image.copy()

        mem_dc.DeleteDC()
        img_dc.DeleteDC()
        win32gui.DeleteObject(bitmap.GetHandle())
    finally:
        win32gui.ReleaseDC(desktop_hwnd, desktop_dc)

    return QPixmap.fromImage(image), QPoint(left, top)


def _is_capture_invalid(pixmap: QPixmap) -> bool:
    if pixmap.isNull() or pixmap.width() < 2 or pixmap.height() < 2:
        return True
    sample = pixmap.scaled(32, 32, Qt.AspectRatioMode.IgnoreAspectRatio)
    image = sample.toImage().convertToFormat(QImage.Format.Format_RGB32)
    total = 0
    dark = 0
    for y in range(image.height()):
        for x in range(image.width()):
            color = image.pixelColor(x, y)
            total += 1
            if color.red() < 8 and color.green() < 8 and color.blue() < 8:
                dark += 1
    return total > 0 and dark / total > 0.98


def virtual_desktop_rect() -> QRect:
    return _virtual_desktop_rect()


def _physical_virtual_desktop_size() -> tuple[int, int, int, int] | None:
    """Return (left, top, width, height) in physical pixels, or None."""
    if sys.platform != "win32":
        return None
    try:
        import win32api
        import win32con
    except ImportError:
        return None
    left = int(win32api.GetSystemMetrics(win32con.SM_XVIRTUALSCREEN))
    top = int(win32api.GetSystemMetrics(win32con.SM_YVIRTUALSCREEN))
    width = int(win32api.GetSystemMetrics(win32con.SM_CXVIRTUALSCREEN))
    height = int(win32api.GetSystemMetrics(win32con.SM_CYVIRTUALSCREEN))
    if width <= 0 or height <= 0:
        return None
    return left, top, width, height


def align_capture_to_logical_desktop(
    pixmap: QPixmap, origin: QPoint
) -> tuple[QPixmap, QPoint]:
    """Make a desktop capture match Qt logical geometry (fixes HiDPI zoom).

    Win32 ``BitBlt`` returns *physical* pixels. Qt overlays use *logical*
    coordinates. On 125%/150% displays that mismatch makes F1 look zoomed-in.
    Prefer tagging ``devicePixelRatio`` (keeps full resolution) over scaling.
    """
    logical = _virtual_desktop_rect()
    if pixmap.isNull() or logical.isEmpty():
        return pixmap, origin

    # Already a logical-sized buffer (Qt grab path, or 100% DPI Win32).
    if pixmap.width() == logical.width() and pixmap.height() == logical.height():
        if abs(float(pixmap.devicePixelRatio() or 1.0) - 1.0) < 0.01:
            return pixmap, logical.topLeft()
        # Keep existing DPR if Qt already stamped one and DI size matches.
        return pixmap, logical.topLeft()

    phys = _physical_virtual_desktop_size()
    if (
        phys is not None
        and pixmap.width() == phys[2]
        and pixmap.height() == phys[3]
        and logical.width() > 0
        and logical.height() > 0
    ):
        dpr_x = phys[2] / float(logical.width())
        dpr_y = phys[3] / float(logical.height())
        dpr = (dpr_x + dpr_y) / 2.0
        if dpr >= 1.01:
            aligned = QPixmap(pixmap)
            aligned.setDevicePixelRatio(dpr)
            return aligned, logical.topLeft()

    # Mixed DPI / unexpected size: fall back to a logical-sized image.
    if pixmap.width() != logical.width() or pixmap.height() != logical.height():
        scaled = pixmap.scaled(
            logical.size(),
            Qt.AspectRatioMode.IgnoreAspectRatio,
            Qt.TransformationMode.SmoothTransformation,
        )
        return scaled, logical.topLeft()
    return pixmap, logical.topLeft()


def _grab_desktop_win32_timed() -> tuple[QPixmap, QPoint]:
    """BitBlt off the GUI thread so a hung DWM cannot freeze F1."""
    box: list[tuple[QPixmap, QPoint]] = []

    def _run() -> None:
        try:
            box.append(_grab_desktop_win32())
        except OSError:
            box.append((QPixmap(), QPoint(0, 0)))

    worker = threading.Thread(target=_run, name="DeskTidyDesktopGrab", daemon=True)
    worker.start()
    worker.join(_GDI_GRAB_TIMEOUT_S)
    if worker.is_alive() or not box:
        return QPixmap(), QPoint(0, 0)
    return box[0]


def warm_desktop_capture() -> None:
    """Touch GDI capture in the background after login so the first F1 is warm."""
    if sys.platform != "win32":
        return

    def _run() -> None:
        try:
            _grab_desktop_win32()
        except Exception:
            pass

    threading.Thread(target=_run, name="DeskTidyCaptureWarm", daemon=True).start()


def grab_desktop() -> tuple[QPixmap, QPoint]:
    """Return a pixmap of the virtual desktop and its top-left screen coordinate."""
    pixmap, origin = QPixmap(), QPoint(0, 0)
    if sys.platform == "win32":
        pixmap, origin = _grab_desktop_win32_timed()
        if not _is_capture_invalid(pixmap):
            return align_capture_to_logical_desktop(pixmap, origin)

    pixmap, origin = _grab_desktop_qt()
    if sys.platform == "win32" and _is_capture_invalid(pixmap):
        pixmap, origin = _grab_desktop_win32_timed()
    return align_capture_to_logical_desktop(pixmap, origin)
