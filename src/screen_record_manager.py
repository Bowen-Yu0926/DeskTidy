"""Desktop screen recording via FFmpeg (ddagrab / gdigrab).

Uses the open-source FFmpeg encoder. Hotkey (default F3) toggles start/stop;
MP4 is saved under <install>/录屏 by default.

Single-monitor picks use Desktop Duplication (``ddagrab``) so the hardware
cursor does not flicker. Spanning / virtual-desktop capture falls back to
``gdigrab``.

Recording identity (pid + exe + create time) is persisted under ~/.desktidy so
a restart can reclaim an orphaned ffmpeg without killing an unrelated PID.
Stop/finalize runs off the UI thread to avoid freezing overlays/hotkeys.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

from PyQt6.QtCore import QObject, QTimer, pyqtSignal

from src.app_logging import get_logger
from src.ffmpeg_locator import find_ffmpeg
from src.settings import APP_DIR, install_root

_STATE_PATH = APP_DIR / "screen_record_state.json"
RECORDINGS_DIR_NAME = "录屏"

# capture_screen values: "primary" | "extended" | legacy "all" | "current" | "0" | "1" | …
_CAPTURE_ALL = "all"
_CAPTURE_PRIMARY = "primary"
_CAPTURE_CURRENT = "current"
_CAPTURE_EXTENDED = "extended"


def list_capture_screens() -> list[dict]:
    """Return monitor entries for UI / gdigrab: index, label, rect, primary.

    Rect is Windows virtual-desktop pixels (same space FFmpeg gdigrab uses).
    """
    screens: list[dict] = []
    try:
        import win32api
        import win32con

        for index, (hmon, _hdc, _rect) in enumerate(win32api.EnumDisplayMonitors(None, None)):
            info = win32api.GetMonitorInfo(hmon)
            left, top, right, bottom = info["Monitor"]
            width = max(0, int(right) - int(left))
            height = max(0, int(bottom) - int(top))
            if width <= 0 or height <= 0:
                continue
            primary = bool(info.get("Flags", 0) & win32con.MONITORINFOF_PRIMARY)
            device = str(info.get("Device") or "").strip()
            name = device.rsplit("\\", 1)[-1] if device else f"显示器 {index + 1}"
            label = f"{'主屏幕' if primary else f'屏幕 {index + 1}'}（{width}×{height}）"
            if name and name.lower() not in ("\\\\.\\display1", "display1"):
                # Keep short device tag when useful.
                pass
            screens.append(
                {
                    "index": index,
                    "id": str(index),
                    "label": label,
                    "primary": primary,
                    "left": int(left),
                    "top": int(top),
                    "width": width,
                    "height": height,
                }
            )
    except Exception:
        screens = []

    if screens:
        return screens

    # Fallback: Qt screens (dev / non-Windows).
    try:
        from PyQt6.QtGui import QGuiApplication

        primary = QGuiApplication.primaryScreen()
        for index, screen in enumerate(QGuiApplication.screens() or []):
            g = screen.geometry()
            is_primary = screen is primary
            screens.append(
                {
                    "index": index,
                    "id": str(index),
                    "label": (
                        f"{'主屏幕' if is_primary else f'屏幕 {index + 1}'}"
                        f"（{g.width()}×{g.height()}）"
                    ),
                    "primary": is_primary,
                    "left": int(g.x()),
                    "top": int(g.y()),
                    "width": int(g.width()),
                    "height": int(g.height()),
                }
            )
    except Exception:
        pass
    return screens


def capture_screen_choices() -> list[tuple[str, str]]:
    """``(value, label)`` for the start-time picker — only 主屏幕 / 扩展屏."""
    choices: list[tuple[str, str]] = [(_CAPTURE_PRIMARY, "主屏幕")]
    screens = list_capture_screens()
    primary = next((s for s in screens if s.get("primary")), screens[0] if screens else None)
    has_extended = any(s is not primary for s in screens) if primary is not None else False
    if not has_extended:
        # Fallback: any non-primary flag.
        has_extended = any(not s.get("primary") for s in screens)
    if has_extended:
        choices.append((_CAPTURE_EXTENDED, "扩展屏"))
    return choices


def normalize_capture_screen(raw: object) -> str:
    text = str(raw or "").strip().lower()
    if not text:
        return _CAPTURE_PRIMARY
    if text in (
        _CAPTURE_ALL,
        _CAPTURE_PRIMARY,
        _CAPTURE_CURRENT,
        _CAPTURE_EXTENDED,
    ):
        return text
    if text.isdigit():
        return text
    return _CAPTURE_PRIMARY


def _even(n: int) -> int:
    n = int(n)
    return n if n % 2 == 0 else max(0, n - 1)


def _region_from_screen(chosen: dict, *, label: str | None = None) -> dict | None:
    width = _even(int(chosen["width"]))
    height = _even(int(chosen["height"]))
    if width < 2 or height < 2:
        return None
    return {
        "left": int(chosen["left"]),
        "top": int(chosen["top"]),
        "width": width,
        "height": height,
        "label": str(label or chosen.get("label") or "屏幕"),
    }


def qt_geometry_for_capture_region(region: dict | None):
    """Map a gdigrab/Win32 *physical* capture region to Qt *logical* geometry.

    FFmpeg needs physical pixels; REC chrome / toast use ``QWidget.move`` which
    is logical. On 125%/150% gaming-laptop DPI those spaces diverge — placing
    chrome with the raw region puts「结束录制」off-screen.
    """
    from PyQt6.QtCore import QRect
    from PyQt6.QtGui import QGuiApplication

    if not isinstance(region, dict):
        return None
    try:
        left = int(region["left"])
        top = int(region["top"])
        width = int(region["width"])
        height = int(region["height"])
    except (KeyError, TypeError, ValueError):
        return None
    if width < 2 or height < 2:
        return None

    screens = list(QGuiApplication.screens() or [])
    if not screens:
        return None

    # Already logical (100% DPI or Qt fallback list_capture_screens).
    for screen in screens:
        g = screen.geometry()
        if (
            abs(int(g.x()) - left) <= 2
            and abs(int(g.y()) - top) <= 2
            and abs(int(g.width()) - width) <= 2
            and abs(int(g.height()) - height) <= 2
        ):
            return QRect(g)

    matched = _qt_screen_for_physical_region(left, top, width, height)
    if matched is not None:
        return QRect(matched.geometry())
    return None


def _qt_screen_for_physical_region(left: int, top: int, width: int, height: int):
    """Resolve the Qt screen that owns a Win32 physical desktop rect."""
    from PyQt6.QtGui import QGuiApplication

    screens = list(QGuiApplication.screens() or [])
    if not screens:
        return None

    # Prefer HMONITOR under the physical capture center (handles mixed DPI).
    try:
        import win32api
        import win32con

        cx = int(left) + max(0, int(width)) // 2
        cy = int(top) + max(0, int(height)) // 2
        hmon = int(
            win32api.MonitorFromPoint(
                (cx, cy), win32con.MONITOR_DEFAULTTONEAREST
            )
        )
        via_mon = _qt_screen_for_hmonitor(hmon)
        if via_mon is not None:
            return via_mon
    except Exception:
        pass

    # Fallback: match physical size ≈ logical × DPR; prefer primary near origin.
    primary = QGuiApplication.primaryScreen()
    scored: list[tuple[tuple, object]] = []
    for screen in screens:
        g = screen.geometry()
        dpr = max(1.0, float(screen.devicePixelRatio() or 1.0))
        phys_w = _even(int(round(g.width() * dpr)))
        phys_h = _even(int(round(g.height() * dpr)))
        size_err = abs(phys_w - width) + abs(phys_h - height)
        primary_bonus = 0
        if screen is primary and abs(left) <= 8 and abs(top) <= 8:
            primary_bonus = -10
        scored.append(((size_err, primary_bonus, abs(int(g.x()))), screen))
    scored.sort(key=lambda item: item[0])
    if scored and scored[0][0][0] <= 8:
        return scored[0][1]
    return primary or screens[0]


def _qt_screen_for_hmonitor(hmon: int):
    """Map a Win32 HMONITOR to the Qt QScreen that lives on it."""
    from PyQt6.QtCore import Qt
    from PyQt6.QtGui import QGuiApplication
    from PyQt6.QtWidgets import QApplication, QWidget

    app = QApplication.instance()
    if app is None or not hmon:
        return None
    try:
        import win32api
    except ImportError:
        return None

    for screen in QGuiApplication.screens() or []:
        g = screen.geometry()
        probe = QWidget()
        probe.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        probe.setWindowFlags(
            Qt.WindowType.Tool
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        # 1×1 is enough for MonitorFromWindow; keep it on-screen for that monitor.
        probe.setGeometry(int(g.x() + g.width() // 2), int(g.y() + g.height() // 2), 1, 1)
        probe.show()
        app.processEvents()
        try:
            hwnd = int(probe.winId())
            if hwnd and int(win32api.MonitorFromWindow(hwnd)) == int(hmon):
                return screen
        except Exception:
            pass
        finally:
            try:
                probe.hide()
                probe.close()
                probe.deleteLater()
            except Exception:
                pass
            app.processEvents()
    return None


def _screen_under_cursor(screens: list[dict]) -> dict | None:
    try:
        from PyQt6.QtGui import QCursor, QGuiApplication

        pos = QCursor.pos()
        screen = QGuiApplication.screenAt(pos)
        if screen is not None:
            g = screen.geometry()
            best = None
            best_area = -1
            for entry in screens:
                left, top = int(entry["left"]), int(entry["top"])
                right = left + int(entry["width"])
                bottom = top + int(entry["height"])
                inter_l = max(left, int(g.x()))
                inter_t = max(top, int(g.y()))
                inter_r = min(right, int(g.x() + g.width()))
                inter_b = min(bottom, int(g.y() + g.height()))
                area = max(0, inter_r - inter_l) * max(0, inter_b - inter_t)
                if area > best_area:
                    best_area = area
                    best = entry
            if best is not None and best_area > 0:
                return best
        x, y = int(pos.x()), int(pos.y())
        for entry in screens:
            left, top = int(entry["left"]), int(entry["top"])
            if (
                left <= x < left + int(entry["width"])
                and top <= y < top + int(entry["height"])
            ):
                return entry
    except Exception:
        return None
    return None


def resolve_capture_region(capture_screen: object) -> dict | None:
    """Return ``{left, top, width, height, label}`` for one screen, or None = all."""
    target = normalize_capture_screen(capture_screen)
    screens = list_capture_screens()
    if not screens:
        return None
    if target == _CAPTURE_ALL:
        return None

    primary = next((s for s in screens if s.get("primary")), screens[0])
    chosen = None
    label_override: str | None = None

    if target == _CAPTURE_PRIMARY:
        chosen = primary
        label_override = "主屏幕"
    elif target == _CAPTURE_EXTENDED:
        secondaries = [s for s in screens if s is not primary]
        if not secondaries:
            secondaries = [s for s in screens if not s.get("primary")]
        if not secondaries:
            chosen = primary
            label_override = "主屏幕"
        elif len(secondaries) == 1:
            chosen = secondaries[0]
            label_override = "扩展屏"
        else:
            under = _screen_under_cursor(secondaries)
            chosen = under or secondaries[0]
            label_override = "扩展屏"
    elif target == _CAPTURE_CURRENT:
        chosen = _screen_under_cursor(screens) or primary
    else:
        for entry in screens:
            if str(entry.get("id")) == target:
                chosen = entry
                break
        if chosen is None:
            chosen = primary

    return _region_from_screen(chosen, label=label_override)


def build_gdigrab_input_args(region: dict | None) -> list[str]:
    """FFmpeg args between ``-f gdigrab`` options and ``-i desktop``."""
    args: list[str] = ["-draw_mouse", "1"]
    if region is not None:
        args.extend(
            [
                "-offset_x",
                str(int(region["left"])),
                "-offset_y",
                str(int(region["top"])),
                "-video_size",
                f"{int(region['width'])}x{int(region['height'])}",
            ]
        )
    return args


# Cached after first probe (warm or first record). Prefer HW MF over libx264.
_preferred_encoder: str | None = None
_ddagrab_supported_cache: dict[str, bool] = {}


def ddagrab_supported(ffmpeg: str | Path) -> bool:
    """True when this ffmpeg build exposes the ddagrab demuxer (optional fast path)."""
    path = Path(ffmpeg)
    key = str(path.resolve()) if path.is_file() else str(ffmpeg)
    cached = _ddagrab_supported_cache.get(key)
    if cached is not None:
        return cached
    if not path.is_file():
        _ddagrab_supported_cache[key] = False
        return False
    from src.ffmpeg_bundle import has_ddagrab

    ok = has_ddagrab(path)
    _ddagrab_supported_cache[key] = ok
    return ok


def _rects_match(
    left: int, top: int, width: int, height: int, other: dict, *, tol: int = 2
) -> bool:
    try:
        return (
            abs(int(other["left"]) - left) <= tol
            and abs(int(other["top"]) - top) <= tol
            and abs(int(other["width"]) - width) <= tol
            and abs(int(other["height"]) - height) <= tol
        )
    except (KeyError, TypeError, ValueError):
        return False


def list_dxgi_outputs() -> list[dict]:
    """DXGI outputs in ``ddagrab`` ``output_idx`` order: left/top/width/height.

    EnumDisplayMonitors order is *not* reliable vs DXGI; DesktopCoordinates are.
    """
    import ctypes
    from ctypes import wintypes

    class GUID(ctypes.Structure):
        _fields_ = [
            ("Data1", ctypes.c_uint32),
            ("Data2", ctypes.c_uint16),
            ("Data3", ctypes.c_uint16),
            ("Data4", ctypes.c_ubyte * 8),
        ]

    class DXGI_OUTPUT_DESC(ctypes.Structure):
        _fields_ = [
            ("DeviceName", wintypes.WCHAR * 32),
            ("DesktopCoordinates", wintypes.RECT),
            ("AttachedToDesktop", wintypes.BOOL),
            ("Rotation", ctypes.c_int),
            ("Monitor", wintypes.HMONITOR),
        ]

    outputs: list[dict] = []
    try:
        dxgi = ctypes.WinDLL("dxgi")
    except OSError:
        return outputs

    # IID_IDXGIFactory {7b7166ec-21c7-44ae-b21a-c9ae321ae369}
    iid_factory = GUID(
        0x7B7166EC,
        0x21C7,
        0x44AE,
        (ctypes.c_ubyte * 8)(0xB2, 0x1A, 0xC9, 0xAE, 0x32, 0x1A, 0xE3, 0x69),
    )
    factory = ctypes.c_void_p()
    create = dxgi.CreateDXGIFactory
    create.argtypes = [ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p)]
    create.restype = ctypes.c_long
    if int(create(ctypes.byref(iid_factory), ctypes.byref(factory))) < 0 or not factory:
        return outputs

    def _vtbl_fn(obj: ctypes.c_void_p, index: int, restype, *argtypes):
        vtbl = ctypes.cast(
            ctypes.cast(obj, ctypes.POINTER(ctypes.c_void_p))[0],
            ctypes.POINTER(ctypes.c_void_p),
        )
        proto = ctypes.WINFUNCTYPE(restype, ctypes.c_void_p, *argtypes)
        return proto(vtbl[index])

    try:
        # IDXGIFactory::EnumAdapters (slot 7)
        enum_adapters = _vtbl_fn(
            factory, 7, ctypes.c_long, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)
        )
        idx = 0
        adapter_i = 0
        while True:
            adapter = ctypes.c_void_p()
            if int(enum_adapters(factory, adapter_i, ctypes.byref(adapter))) < 0:
                break
            adapter_i += 1
            enum_outputs = _vtbl_fn(
                adapter, 7, ctypes.c_long, ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)
            )
            out_i = 0
            while True:
                output = ctypes.c_void_p()
                if int(enum_outputs(adapter, out_i, ctypes.byref(output))) < 0:
                    break
                out_i += 1
                get_desc = _vtbl_fn(
                    output, 7, ctypes.c_long, ctypes.POINTER(DXGI_OUTPUT_DESC)
                )
                desc = DXGI_OUTPUT_DESC()
                if int(get_desc(output, ctypes.byref(desc))) >= 0:
                    rc = desc.DesktopCoordinates
                    left, top = int(rc.left), int(rc.top)
                    width = max(0, int(rc.right) - left)
                    height = max(0, int(rc.bottom) - top)
                    if width > 0 and height > 0 and bool(desc.AttachedToDesktop):
                        outputs.append(
                            {
                                "idx": idx,
                                "left": left,
                                "top": top,
                                "width": width,
                                "height": height,
                            }
                        )
                        idx += 1
                release = _vtbl_fn(output, 2, ctypes.c_ulong)
                release(output)
            release_a = _vtbl_fn(adapter, 2, ctypes.c_ulong)
            release_a(adapter)
        release_f = _vtbl_fn(factory, 2, ctypes.c_ulong)
        release_f(factory)
    except Exception:
        return []
    return outputs


def _match_ddagrab_output_idx(region: dict | None) -> int | None:
    """Return DXGI ``output_idx`` when *region* is exactly one attached monitor.

    Multi-monitor PCs used to always fall back to ``gdigrab`` (cursor flicker).
    Match by DXGI DesktopCoordinates so primary/extended picks stay on ddagrab.
    """
    if region is None:
        return None
    try:
        left = int(region["left"])
        top = int(region["top"])
        width = int(region["width"])
        height = int(region["height"])
    except (KeyError, TypeError, ValueError):
        return None

    dxgi = list_dxgi_outputs()
    for entry in dxgi:
        if _rects_match(left, top, width, height, entry):
            return int(entry["idx"])

    # Single-monitor machine: DXGI enum empty/unavailable → output 0.
    screens = list_capture_screens()
    if len(screens) == 1 and _rects_match(left, top, width, height, screens[0]):
        return 0

    # Last resort: EnumDisplayMonitors sorted by origin ≈ DXGI on many PCs.
    ordered = sorted(screens, key=lambda s: (int(s["left"]), int(s["top"])))
    for index, screen in enumerate(ordered):
        if _rects_match(left, top, width, height, screen):
            return index
    return None


def build_encoder_args(codec: str) -> list[str]:
    """Video encoder args. ``h264_mf`` is the Windows market default when available."""
    name = (codec or "libx264").strip().lower()
    if name == "h264_mf":
        return [
            "-c:v",
            "h264_mf",
            "-rate_control",
            "quality",
            "-quality",
            "70",
            "-pix_fmt",
            "nv12",
        ]
    # Software fallback — ultrafast + zerolatency keeps the desktop responsive.
    return [
        "-c:v",
        "libx264",
        "-preset",
        "ultrafast",
        "-tune",
        "zerolatency",
        "-crf",
        "28",
        "-pix_fmt",
        "yuv420p",
    ]


def probe_preferred_encoder(ffmpeg: str | Path) -> str:
    """Pick a low-CPU encoder once (background warm / first start)."""
    global _preferred_encoder
    if _preferred_encoder:
        return _preferred_encoder
    exe = str(ffmpeg)
    # Tiny synthetic frame — avoids grabbing the desktop during probe.
    for codec in ("h264_mf", "libx264"):
        cmd = [
            exe,
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "color=c=black:s=160x90:d=0.12",
            *build_encoder_args(codec),
            "-f",
            "null",
            "-",
        ]
        try:
            completed = subprocess.run(
                cmd,
                capture_output=True,
                creationflags=_creation_flags(),
                timeout=8,
                check=False,
            )
            if completed.returncode == 0:
                _preferred_encoder = codec
                get_logger().info("screen_record encoder=%s", codec)
                return codec
        except (OSError, subprocess.TimeoutExpired):
            continue
    _preferred_encoder = "libx264"
    return _preferred_encoder


def preferred_encoder() -> str:
    return _preferred_encoder or "libx264"


def build_record_command(
    ffmpeg: str | Path,
    output: Path,
    *,
    fps: int,
    region: dict | None,
    codec: str | None = None,
) -> list[str]:
    """Build the FFmpeg argv for one recording session.

    Market path on modern Windows:
    - Single monitor → Desktop Duplication (``ddagrab``) + Media Foundation H.264
    - All / partial region → ``gdigrab`` fallback
    Software ``libx264`` is only the fallback when ``h264_mf`` probe fails.
    """
    fps = max(15, min(60, int(fps)))
    if codec is None:
        exe_path = Path(ffmpeg)
        if exe_path.is_file():
            codec = probe_preferred_encoder(exe_path)
        else:
            codec = _preferred_encoder or "h264_mf"
    enc = build_encoder_args(codec)
    exe = str(ffmpeg)
    out = str(output)
    dda_idx = _match_ddagrab_output_idx(region)
    if dda_idx is not None and ddagrab_supported(ffmpeg):
        return [
            exe,
            "-y",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            f"ddagrab=output_idx={dda_idx}:framerate={fps}:draw_mouse=1",
            "-vf",
            "hwdownload,format=bgra",
            *enc,
            out,
        ]
    # Virtual desktop / non-monitor crop: classic gdigrab.
    return [
        exe,
        "-y",
        "-loglevel",
        "error",
        "-probesize",
        "32",
        "-analyzeduration",
        "0",
        "-f",
        "gdigrab",
        *build_gdigrab_input_args(region),
        "-framerate",
        str(fps),
        "-rtbufsize",
        "256M",
        "-i",
        "desktop",
        *enc,
        out,
    ]


class _StopBridge(QObject):
    """Marshal stop-finalize results onto the Qt UI thread."""

    done = pyqtSignal(object)


class _StartBridge(QObject):
    """Marshal async start results onto the Qt UI thread."""

    done = pyqtSignal(object)


def default_output_dir(*, ensure: bool = True) -> Path:
    """Default: <install_root>/录屏 beside the exe (or project root in dev)."""
    folder = install_root() / RECORDINGS_DIR_NAME
    if ensure:
        try:
            folder.mkdir(parents=True, exist_ok=True)
        except OSError:
            pass
    return folder


def resolve_output_dir(raw: str | None, *, ensure: bool = True) -> Path:
    """Resolve configured output_dir; empty → install_root/录屏."""
    text = str(raw or "").strip()
    if not text:
        return default_output_dir(ensure=ensure)
    path = Path(os.path.expandvars(os.path.expanduser(text)))
    if ensure:
        path.mkdir(parents=True, exist_ok=True)
    return path


def _default_output_dir() -> Path:
    """Back-compat alias used by older call sites / tests."""
    return default_output_dir(ensure=True)


def _creation_flags() -> int:
    if sys.platform == "win32":
        return subprocess.CREATE_NO_WINDOW  # type: ignore[attr-defined]
    return 0


def _pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if sys.platform == "win32":
        try:
            import ctypes
            from ctypes import wintypes

            PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
            STILL_ACTIVE = 259
            handle = ctypes.windll.kernel32.OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid)
            )
            if not handle:
                return False
            code = wintypes.DWORD()
            ok = ctypes.windll.kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
            ctypes.windll.kernel32.CloseHandle(handle)
            return bool(ok) and int(code.value) == STILL_ACTIVE
        except Exception:
            return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    except Exception:
        return False
    return True


def _process_image_path(pid: int) -> str:
    """Full image path for pid, or '' if unknown."""
    if pid <= 0 or sys.platform != "win32":
        return ""
    try:
        import ctypes
        from ctypes import wintypes

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        handle = ctypes.windll.kernel32.OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid)
        )
        if not handle:
            return ""
        try:
            buf = ctypes.create_unicode_buffer(1024)
            size = wintypes.DWORD(len(buf))
            # QueryFullProcessImageNameW
            ok = ctypes.windll.kernel32.QueryFullProcessImageNameW(
                handle, 0, buf, ctypes.byref(size)
            )
            return buf.value if ok else ""
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    except Exception:
        return ""


def _process_create_time_ms(pid: int) -> int:
    """Process creation time as ms since Windows epoch, or 0."""
    if pid <= 0 or sys.platform != "win32":
        return 0
    try:
        import ctypes
        from ctypes import wintypes

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        handle = ctypes.windll.kernel32.OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid)
        )
        if not handle:
            return 0
        try:
            creation = wintypes.FILETIME()
            exit_t = wintypes.FILETIME()
            kernel_t = wintypes.FILETIME()
            user_t = wintypes.FILETIME()
            ok = ctypes.windll.kernel32.GetProcessTimes(
                handle,
                ctypes.byref(creation),
                ctypes.byref(exit_t),
                ctypes.byref(kernel_t),
                ctypes.byref(user_t),
            )
            if not ok:
                return 0
            hi = int(creation.dwHighDateTime)
            lo = int(creation.dwLowDateTime)
            return ((hi << 32) | lo) // 10_000
        finally:
            ctypes.windll.kernel32.CloseHandle(handle)
    except Exception:
        return 0


def _is_ffmpeg_process(pid: int, *, expected_exe: str = "") -> bool:
    image = _process_image_path(pid)
    if not image:
        return False
    name = Path(image).name.lower()
    if name != "ffmpeg.exe":
        return False
    if expected_exe:
        try:
            return Path(image).resolve() == Path(expected_exe).resolve()
        except OSError:
            return Path(image).name.lower() == Path(expected_exe).name.lower()
    return True


def _read_state() -> dict:
    try:
        if not _STATE_PATH.is_file():
            return {}
        data = json.loads(_STATE_PATH.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError, TypeError):
        return {}


def _write_state(pid: int, output: Path, *, exe: str, create_time_ms: int) -> None:
    try:
        APP_DIR.mkdir(parents=True, exist_ok=True)
        _STATE_PATH.write_text(
            json.dumps(
                {
                    "pid": int(pid),
                    "output": str(output),
                    "exe": str(exe),
                    "create_time_ms": int(create_time_ms),
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
    except OSError as exc:
        get_logger().warning("screen_record state write failed: %s", exc)


def _clear_state() -> None:
    try:
        _STATE_PATH.unlink(missing_ok=True)
    except OSError:
        pass


def _terminate_pid(pid: int) -> None:
    if pid <= 0 or not _pid_alive(pid):
        return
    # Never kill a PID that is not clearly ffmpeg.
    if not _is_ffmpeg_process(pid):
        get_logger().warning("screen_record refuse kill non-ffmpeg pid=%s", pid)
        return
    if sys.platform == "win32":
        try:
            subprocess.run(
                ["taskkill", "/PID", str(pid), "/T", "/F"],
                capture_output=True,
                creationflags=_creation_flags(),
                check=False,
            )
            return
        except OSError:
            pass
    try:
        os.kill(pid, 9)
    except OSError:
        pass


def _wait_for_saved_file(path: Path | None, *, timeout_s: float = 2.0) -> Path | None:
    """Return *path* once it exists with a stable non-empty size.

    Media Foundation / libx264 may keep growing the file briefly after ffmpeg
    exits while the moov atom is flushed — require two identical size samples.
    """
    if path is None:
        return None
    deadline = time.monotonic() + timeout_s
    last_size = -1
    stable = 0
    while True:
        try:
            if path.is_file():
                size = int(path.stat().st_size)
                if size > 0:
                    if size == last_size:
                        stable += 1
                        if stable >= 2:
                            return path
                    else:
                        last_size = size
                        stable = 1
        except OSError:
            pass
        if time.monotonic() >= deadline:
            break
        time.sleep(0.1)
    try:
        if path.is_file() and path.stat().st_size > 0:
            return path
    except OSError:
        pass
    return None


_WIN_INVALID_FILENAME = '<>:"/\\|?*'


def sanitize_recording_stem(name: str, *, fallback: str = "DeskTidy_recording") -> str:
    """Strip characters illegal in Windows file names."""
    cleaned = "".join(
        ch for ch in str(name or "").strip() if ch not in _WIN_INVALID_FILENAME and ch not in "\0\r\n\t"
    )
    cleaned = cleaned.strip().strip(".")
    return cleaned or fallback


def rename_recording_file(path: Path, new_stem: str) -> Path:
    """Rename MP4 (and sidecar ffmpeg log) within the same folder."""
    src = Path(path)
    if not src.is_file():
        return src
    stem = sanitize_recording_stem(new_stem, fallback=src.stem)
    if stem == src.stem:
        return src
    ext = src.suffix or ".mp4"
    dest = src.with_name(f"{stem}{ext}")
    if dest.exists() and dest.resolve() != src.resolve():
        base = stem
        n = 1
        while dest.exists():
            dest = src.with_name(f"{base}_{n}{ext}")
            n += 1
    src.rename(dest)
    log_src = src.with_suffix(".ffmpeg.log")
    if log_src.is_file():
        try:
            log_src.rename(dest.with_suffix(".ffmpeg.log"))
        except OSError:
            pass
    return dest


def _selftest_active() -> bool:
    if os.environ.get("DESKTIDY_SELFTEST") == "1":
        return True
    try:
        from pathlib import Path as _Path

        return any("selftest" in _Path(arg).name.casefold() for arg in sys.argv)
    except Exception:
        return False


def _reveal_recording(path: Path) -> None:
    """Select the saved MP4 in Explorer so the user can see where it landed."""
    if _selftest_active():
        return
    try:
        if sys.platform == "win32" and path.is_file():
            subprocess.Popen(
                ["explorer", "/select,", str(path.resolve())],
                creationflags=_creation_flags(),
            )
            return
    except OSError:
        pass
    try:
        folder = path.parent if path.is_file() else path
        if folder.is_dir():
            os.startfile(str(folder))  # type: ignore[attr-defined]
    except OSError:
        pass


def _finalize_stop(proc, path: Path | None) -> tuple[Path | None, str]:
    """Blocking finalize — must not run on the Qt UI thread."""
    pid = int(getattr(proc, "pid", 0) or 0)
    has_stdin = getattr(proc, "stdin", None) is not None
    try:
        if proc.poll() is None and has_stdin:
            proc.stdin.write(b"q")
            proc.stdin.flush()
            proc.stdin.close()
    except OSError:
        has_stdin = False

    try:
        if proc.poll() is None and not has_stdin:
            _terminate_pid(pid)
        # h264_mf needs more time than libx264 to flush the container.
        proc.wait(timeout=10 if has_stdin else 4)
    except subprocess.TimeoutExpired:
        _terminate_pid(pid)
        try:
            proc.wait(timeout=3)
        except (subprocess.TimeoutExpired, OSError):
            pass
    except OSError:
        _terminate_pid(pid)

    if pid > 0:
        deadline = time.monotonic() + 1.5
        while _pid_alive(pid) and time.monotonic() < deadline:
            _terminate_pid(pid)
            time.sleep(0.1)

    _clear_state()
    saved = _wait_for_saved_file(path, timeout_s=3.0)
    return saved, ""


class _OrphanProc:
    """Minimal stand-in for subprocess.Popen when we only know a PID."""

    def __init__(self, pid: int) -> None:
        self.pid = int(pid)
        self.stdin = None
        self.stderr = None

    def poll(self) -> int | None:
        return None if _pid_alive(self.pid) else 0

    def wait(self, timeout: float | None = None) -> int:
        deadline = None if timeout is None else (time.monotonic() + float(timeout))
        while _pid_alive(self.pid):
            if deadline is not None and time.monotonic() >= deadline:
                raise subprocess.TimeoutExpired(cmd=f"pid={self.pid}", timeout=timeout or 0)
            time.sleep(0.15)
        return 0


class ScreenRecordManager:
    def __init__(self, get_settings, *, notify=None) -> None:
        self._get_settings = get_settings
        self._notify = notify
        self._proc: subprocess.Popen | _OrphanProc | None = None
        self._output_path: Path | None = None
        self._ffmpeg_exe: str = ""
        self._ffmpeg_tip_shown = False
        self._stopping = False
        self._starting = False
        self._cancel_start = threading.Event()
        self._capture_region: dict | None = None
        self._err_log_path: Path | None = None
        self._err_log_fh = None
        self._stop_bridge = _StopBridge()
        self._stop_bridge.done.connect(self._on_stop_finished)
        self._start_bridge = _StartBridge()
        self._start_bridge.done.connect(self._on_start_finished)
        self._last_capture: str = "primary"
        self._ffmpeg_warm_started = False
        self._adopt_orphan_if_any()
        # Page-in ffmpeg.exe immediately (cold boot + Defender first-scan of the
        # bundled binary otherwise blocks the first record click for ~1–2 min).
        self._warm_ffmpeg()

    def _warm_ffmpeg(self) -> None:
        if self._ffmpeg_warm_started:
            return
        self._ffmpeg_warm_started = True

        def _worker() -> None:
            try:
                from src.ffmpeg_locator import warm_ffmpeg_cache

                warm_ffmpeg_cache()
                ffmpeg = find_ffmpeg()
                if ffmpeg is None:
                    return
                # Map the binary into the OS cache before the user clicks 录屏.
                try:
                    subprocess.run(
                        [str(ffmpeg), "-hide_banner", "-version"],
                        capture_output=True,
                        creationflags=_creation_flags(),
                        timeout=90,
                        check=False,
                    )
                except (OSError, subprocess.TimeoutExpired):
                    pass
                probe_preferred_encoder(ffmpeg)
                ddagrab_supported(ffmpeg)
            except Exception:
                pass

        threading.Thread(target=_worker, name="DeskTidyFFmpegWarm", daemon=True).start()

    @property
    def is_recording(self) -> bool:
        if self._proc is not None and self._proc.poll() is None:
            return True
        self._adopt_orphan_if_any()
        return self._proc is not None and self._proc.poll() is None

    def _adopt_orphan_if_any(self) -> None:
        if self._proc is not None and self._proc.poll() is None:
            return
        state = _read_state()
        pid = int(state.get("pid") or 0)
        raw = str(state.get("output") or "").strip()
        exe = str(state.get("exe") or "").strip()
        create_ms = int(state.get("create_time_ms") or 0)
        if pid <= 0 or not raw:
            _clear_state()
            return
        if not _pid_alive(pid):
            _clear_state()
            return
        if not _is_ffmpeg_process(pid, expected_exe=exe):
            get_logger().warning(
                "screen_record orphan rejected pid=%s (not matching ffmpeg)", pid
            )
            _clear_state()
            return
        if create_ms > 0:
            live_ms = _process_create_time_ms(pid)
            # Allow small skew; reject obvious PID reuse.
            if live_ms > 0 and abs(live_ms - create_ms) > 2000:
                get_logger().warning(
                    "screen_record orphan rejected pid=%s (create_time mismatch)", pid
                )
                _clear_state()
                return
        self._proc = _OrphanProc(pid)
        self._output_path = Path(raw)
        self._ffmpeg_exe = exe
        get_logger().info("screen_record adopted orphan pid=%s path=%s", pid, raw)

    def toggle_recording(self) -> None:
        if self._starting:
            self._cancel_start.set()
            return
        if self.is_recording or self._stopping:
            self.stop_recording()
        else:
            self.start_recording()

    def start_recording(self, capture_screen: str | None = None) -> None:
        """Start FFmpeg capture.

        When *capture_screen* is omitted, show the on-screen picker first
        (settings no longer own this choice). Pass an explicit value to skip
        the picker (tests / automation).
        """
        if self.is_recording or self._stopping or self._starting:
            return
        if capture_screen is None:
            try:
                from src.ui.record_screen_picker import pick_capture_screen

                chosen = pick_capture_screen(initial=self._last_capture)
            except Exception as exc:
                get_logger().warning("screen_record picker failed: %s", exc)
                chosen = None
            if chosen is None:
                return
            capture_screen = chosen
        capture = normalize_capture_screen(capture_screen)
        self._last_capture = capture

        self._starting = True
        self._cancel_start.clear()
        cfg = self._get_settings().get("screen_record", {})
        raw_dir = str(cfg.get("output_dir") or "").strip()
        fps = max(15, min(60, int(cfg.get("fps", 30) or 30)))
        # Resolve "current" on the UI thread so cursor position is accurate.
        region = resolve_capture_region(capture)

        def _worker() -> None:
            try:
                if self._cancel_start.is_set():
                    self._start_bridge.done.emit({"ok": False, "reason": "cancelled"})
                    return
                ffmpeg = find_ffmpeg()
                if ffmpeg is None:
                    self._start_bridge.done.emit({"ok": False, "reason": "no_ffmpeg"})
                    return
                try:
                    out_dir = resolve_output_dir(raw_dir, ensure=True)
                except OSError as exc:
                    self._start_bridge.done.emit(
                        {
                            "ok": False,
                            "reason": "bad_dir",
                            "detail": f"{raw_dir or default_output_dir(ensure=False)}\n{exc}",
                        }
                    )
                    return
                if self._cancel_start.is_set():
                    self._start_bridge.done.emit({"ok": False, "reason": "cancelled"})
                    return
                stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                output = out_dir / f"DeskTidy_{stamp}.mp4"
                codec = preferred_encoder()
                if codec == "libx264":
                    codec = probe_preferred_encoder(ffmpeg)
                cmd = build_record_command(
                    ffmpeg, output, fps=fps, region=region, codec=codec
                )
                # Keep stderr on disk for early-exit diagnosis (DEVNULL hid causes).
                err_log = output.with_suffix(".ffmpeg.log")
                try:
                    err_fh: object = open(err_log, "wb")
                except OSError:
                    err_fh = subprocess.DEVNULL
                proc = subprocess.Popen(
                    cmd,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL,
                    stderr=err_fh,
                    creationflags=_creation_flags(),
                )
                # Parent must not close err_fh until ffmpeg exits (inherited handle).
                if self._cancel_start.is_set():
                    try:
                        proc.stdin.write(b"q")
                        proc.stdin.flush()
                    except Exception:
                        pass
                    try:
                        proc.wait(timeout=2.0)
                    except Exception:
                        _terminate_pid(int(proc.pid))
                    if err_fh is not subprocess.DEVNULL:
                        try:
                            err_fh.close()  # type: ignore[union-attr]
                        except OSError:
                            pass
                    try:
                        output.unlink(missing_ok=True)
                    except OSError:
                        pass
                    try:
                        err_log.unlink(missing_ok=True)
                    except OSError:
                        pass
                    self._start_bridge.done.emit({"ok": False, "reason": "cancelled"})
                    return
                create_ms = _process_create_time_ms(int(proc.pid))
                _write_state(
                    int(proc.pid),
                    output,
                    exe=str(ffmpeg),
                    create_time_ms=create_ms,
                )
                self._start_bridge.done.emit(
                    {
                        "ok": True,
                        "proc": proc,
                        "path": output,
                        "exe": str(ffmpeg),
                        "region": region,
                        "err_log": str(err_log),
                        "err_fh": err_fh if err_fh is not subprocess.DEVNULL else None,
                    }
                )
            except Exception as exc:
                get_logger().error("screen_record start failed: %s", exc)
                self._start_bridge.done.emit(
                    {"ok": False, "reason": "start_failed", "detail": str(exc)}
                )

        threading.Thread(target=_worker, name="DeskTidyRecordStart", daemon=True).start()

    def _on_start_finished(self, result: object) -> None:
        self._starting = False
        data = result if isinstance(result, dict) else {}
        if not data.get("ok"):
            reason = str(data.get("reason") or "")
            if reason == "cancelled":
                return
            if reason == "no_ffmpeg":
                msg = (
                    "未找到 FFmpeg。\n"
                    "安装版请确认程序目录下有 assets\\ffmpeg\\ffmpeg.exe。\n"
                    "或执行：winget install Gyan.FFmpeg"
                )
                get_logger().warning("screen_record: ffmpeg not found")
                self._message(msg)
                if not self._ffmpeg_tip_shown:
                    self._ffmpeg_tip_shown = True
                    try:
                        from src.i18n import show_warning

                        QTimer.singleShot(0, lambda: show_warning(None, "录屏不可用", msg))
                    except Exception:
                        pass
            elif reason == "bad_dir":
                self._message(f"无法创建录屏目录：{data.get('detail') or ''}")
            else:
                self._message(f"启动录屏失败：{data.get('detail') or '未知错误'}")
            return

        self._proc = data.get("proc")
        self._output_path = data.get("path")
        self._ffmpeg_exe = str(data.get("exe") or "")
        self._capture_region = data.get("region")
        self._err_log_path = Path(str(data["err_log"])) if data.get("err_log") else None
        self._err_log_fh = data.get("err_fh")
        get_logger().info(
            "screen_record started path=%s ffmpeg=%s encoder=%s region=%s",
            self._output_path,
            self._ffmpeg_exe,
            preferred_encoder(),
            self._capture_region,
        )
        # Show chrome immediately — waiting for a settle timer made start feel slow.
        # Verify FFmpeg stayed up shortly after (early-exit → hide + error).
        self._announce_recording_active()
        QTimer.singleShot(450, self._check_started)

    def _close_err_log(self, *, keep_file: bool = False) -> str:
        """Close ffmpeg stderr sink; return tail text for diagnostics."""
        fh = getattr(self, "_err_log_fh", None)
        self._err_log_fh = None
        if fh is not None:
            try:
                fh.close()
            except OSError:
                pass
        path = getattr(self, "_err_log_path", None)
        self._err_log_path = None
        text = ""
        if isinstance(path, Path) and path.is_file():
            try:
                raw = path.read_bytes()[-2000:]
                text = raw.decode("utf-8", errors="replace").strip()
            except OSError:
                text = ""
            if not keep_file:
                try:
                    path.unlink(missing_ok=True)
                except OSError:
                    pass
        return text

    def _check_started(self) -> None:
        proc = self._proc
        if proc is None:
            return
        code = proc.poll()
        if code is None:
            return
        self._hide_recording_chrome()
        self._proc = None
        path = self._output_path
        self._output_path = None
        self._capture_region = None
        _clear_state()
        detail = self._close_err_log(keep_file=True)
        get_logger().error(
            "screen_record exited early code=%s stderr=%s", code, detail[:500]
        )
        self._message("录屏启动失败（FFmpeg 立即退出）。")
        if path is not None:
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass

    def _announce_recording_active(self) -> None:
        """On-screen chrome first; toast deferred so DWM work does not pile on encode start."""
        try:
            from PyQt6.QtWidgets import QApplication

            app = QApplication.instance()
            desk = getattr(app, "_desktidy_app", None) if app else None
            begin = getattr(desk, "_begin_recording_session", None) if desk else None
            if callable(begin):
                begin()
        except Exception:
            pass
        hotkey = self._get_settings().get("hotkeys", {}).get("screen_record", "F3")
        self._show_recording_chrome(hotkey)

    def _show_recording_chrome(self, hotkey: str) -> None:
        try:
            from PyQt6.QtCore import QRect

            from src.ui.recording_indicator import show_recording_indicator
            from src.ui.toast import show_toast

            region = self._capture_region
            show_recording_indicator(
                self.stop_recording, capture_region=region
            )
            path = self._output_path
            if region is None:
                where = "全部屏幕"
                toast_anchor = None
            else:
                where = str(region.get("label") or "选定屏幕")
                toast_anchor = qt_geometry_for_capture_region(region)
                if toast_anchor is None:
                    try:
                        toast_anchor = QRect(
                            int(region["left"]),
                            int(region["top"]),
                            int(region["width"]),
                            int(region["height"]),
                        )
                    except (KeyError, TypeError, ValueError):
                        toast_anchor = None
            body = f"{where}\n右上角结束，或按 {hotkey}"
            if path is not None:
                body += f"\n{path.name}"

            def _toast() -> None:
                if self._proc is None or self._proc.poll() is not None:
                    return
                try:
                    show_toast(
                        "录屏已开始", body, msec=1800, anchor=toast_anchor
                    )
                except Exception:
                    pass

            QTimer.singleShot(320, _toast)
        except Exception as exc:
            get_logger().warning("screen_record indicator failed: %s", exc)
            # Fallback if chrome/toast fails.
            self._message(f"录屏已开始，再按 {hotkey} 结束。")

    def _hide_recording_chrome(self) -> None:
        try:
            from src.ui.recording_indicator import hide_recording_indicator

            hide_recording_indicator()
        except Exception:
            pass
        try:
            from PyQt6.QtWidgets import QApplication

            app = QApplication.instance()
            desk = getattr(app, "_desktidy_app", None) if app else None
            end = getattr(desk, "_end_recording_session", None) if desk else None
            if callable(end):
                end()
        except Exception:
            pass

    def stop_recording(self) -> None:
        if self._stopping:
            return
        if self._starting:
            self._cancel_start.set()
            return
        self._stopping = True
        self._hide_recording_chrome()
        proc = self._proc
        path = self._output_path
        self._proc = None
        self._output_path = None
        self._capture_region = None
        # Close stderr sink before waiting so ffmpeg can flush the log.
        self._close_err_log(keep_file=True)
        if proc is None:
            _clear_state()
            self._stopping = False
            return

        def _worker() -> None:
            try:
                saved, _detail = _finalize_stop(proc, path)
            except Exception as exc:
                get_logger().error("screen_record stop worker failed: %s", exc)
                saved = None
            # Queued connection — safe from a background thread.
            self._stop_bridge.done.emit(saved)

        threading.Thread(target=_worker, name="DeskTidyRecordStop", daemon=True).start()

    def _on_stop_finished(self, saved: Path | None) -> None:
        try:
            if saved is None or not saved.is_file():
                get_logger().warning("screen_record save failed path=%s", saved)
                self._message("录屏未能保存。（文件为空或未生成，可再试一次）")
                return
            try:
                size_mb = saved.stat().st_size / (1024 * 1024)
            except OSError:
                get_logger().warning("screen_record save vanished path=%s", saved)
                self._message("录屏未能保存。（文件为空或未生成，可再试一次）")
                return
            if size_mb <= 0:
                get_logger().warning("screen_record empty file path=%s", saved)
                try:
                    saved.unlink(missing_ok=True)
                except OSError:
                    pass
                self._message("录屏未能保存。（文件为空或未生成，可再试一次）")
                return
            keep = True
            final_stem = saved.stem
            try:
                from src.i18n import ask_keep_recording_with_name

                keep, final_stem = ask_keep_recording_with_name(
                    None, path=saved, size_mb=size_mb
                )
            except Exception as exc:
                get_logger().warning("screen_record keep prompt failed: %s", exc)
            if keep:
                if not saved.is_file():
                    get_logger().warning(
                        "screen_record kept but file missing path=%s", saved
                    )
                    self._message("录屏未能保存。（文件为空或未生成，可再试一次）")
                    return
                try:
                    saved = rename_recording_file(saved, final_stem)
                except OSError as exc:
                    get_logger().warning("screen_record rename failed: %s", exc)
                    self._message(f"保留成功，但重命名失败：\n{saved}\n{exc}")
                try:
                    size_mb = saved.stat().st_size / (1024 * 1024)
                except OSError:
                    pass
                get_logger().info(
                    "screen_record saved path=%s size_mb=%.1f", saved, size_mb
                )
                self._message(f"录屏已保存：\n{saved}\n({size_mb:.1f} MB)")
                _reveal_recording(saved)
                # Drop sidecar ffmpeg log after a good save.
                try:
                    saved.with_suffix(".ffmpeg.log").unlink(missing_ok=True)
                except OSError:
                    pass
            else:
                try:
                    saved.unlink(missing_ok=True)
                except OSError as exc:
                    get_logger().warning("screen_record discard failed: %s", exc)
                    self._message(f"取消失败，文件仍在：\n{saved}")
                    return
                get_logger().info("screen_record discarded path=%s", saved)
                self._message("已取消保存，录屏文件已删除。")
        finally:
            self._stopping = False

    def shutdown(self) -> None:
        self._cancel_start.set()
        if self._starting:
            self._hide_recording_chrome()
            self._starting = False
            state = _read_state()
            pid = int(state.get("pid") or 0)
            if pid > 0:
                try:
                    _terminate_pid(pid)
                except Exception:
                    pass
            _clear_state()
        if self.is_recording or self._stopping:
            # Best-effort sync stop on app exit (UI is going away anyway).
            self._hide_recording_chrome()
            proc = self._proc
            path = self._output_path
            self._proc = None
            self._output_path = None
            self._capture_region = None
            if proc is not None:
                try:
                    _finalize_stop(proc, path)
                except Exception:
                    pass
            self._stopping = False
        _clear_state()

    def _message(self, text: str) -> None:
        if callable(self._notify):
            # Call notify directly when already on the UI thread (stop bridge /
            # QTimer callbacks). Avoid an extra singleShot that tests can miss.
            try:
                self._notify(text)
            except Exception:
                QTimer.singleShot(0, lambda t=text: self._notify(t))
            return
        print(text, file=sys.stderr)
