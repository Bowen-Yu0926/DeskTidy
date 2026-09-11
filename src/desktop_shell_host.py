"""Bind overlays to the Explorer desktop shell (Fences / Rainmeter style).

On modern Windows 11, SetParent onto Progman often fails for Qt top-level windows
and corrupts geometry. The reliable market approach is GWLP_HWNDPARENT ownership
tied to the SHELLDLL_DefView host (Progman or the WorkerW that contains DefView).

Never bind to the wallpaper-only WorkerW (no mouse input; under the wallpaper).
"""

from __future__ import annotations

import ctypes
import time
from contextlib import contextmanager

import win32gui

user32 = ctypes.windll.user32

SWP_NOSIZE = 0x0001
SWP_NOMOVE = 0x0002
SWP_NOZORDER = 0x0004
SWP_NOREDRAW = 0x0008
SWP_NOACTIVATE = 0x0010
SWP_HIDEWINDOW = 0x0080
SWP_SHOWWINDOW = 0x0040
SWP_NOCOPYBITS = 0x0100
SWP_NOOWNERZORDER = 0x0200
SW_HIDE = 0
SW_SHOWNOACTIVATE = 4

RDW_INVALIDATE = 0x0001
RDW_ERASE = 0x0004
RDW_ALLCHILDREN = 0x0080
RDW_UPDATENOW = 0x0100
GWLP_HWNDPARENT = -8
GW_OWNER = 4


class OverlayWindowBatch:
    """One DWM composition for page-switch show/hide/move of translucent HWNDs.

    Market pattern: ``BeginDeferWindowPos`` / ``EndDeferWindowPos`` — never N
    individual ``SetWindowPos`` / ``ShowWindow`` calls (each flashes the desktop).

    Soft park must ``SWP_HIDEWINDOW`` (remove from composition). Leaving a window
    mapped at opacity 0 still composites and flashes on every child paint (icon
    click selection).
    """

    _HIDE = 1
    _SHOW = 2
    _MOVE = 3

    def __init__(self) -> None:
        # (op, hwnd, x, y, w, h)
        self._items: list[tuple[int, int, int, int, int, int]] = []

    def _alive(self, hwnd: int) -> bool:
        if not hwnd:
            return False
        try:
            return bool(user32.IsWindow(int(hwnd)))
        except Exception:
            return False

    def hide(self, hwnd: int) -> None:
        if not self._alive(hwnd):
            return
        self._items.append((self._HIDE, int(hwnd), 0, 0, 0, 0))

    def show(self, hwnd: int) -> None:
        if not self._alive(hwnd):
            return
        self._items.append((self._SHOW, int(hwnd), 0, 0, 0, 0))

    def set_rect(self, hwnd: int, x: int, y: int, w: int, h: int) -> None:
        if not self._alive(hwnd) or w <= 0 or h <= 0:
            return
        self._items.append((self._MOVE, int(hwnd), int(x), int(y), int(w), int(h)))

    def commit(self) -> None:
        items = self._items
        self._items = []
        if not items:
            return
        # Always hide leavers before showing arrivers — one-frame overlap flashes.
        _order = {self._HIDE: 0, self._MOVE: 1, self._SHOW: 2}
        items.sort(key=lambda t: (_order.get(t[0], 9), t[1]))
        hide_base = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS
        move_base = hide_base
        # SHOW must not use SWP_NOREDRAW (stayed blank) or SWP_NOCOPYBITS
        # (discards the client buffer — first map flashes wallpaper for a frame).
        show_base = SWP_NOZORDER | SWP_NOACTIVATE

        def _flags(op: int) -> int:
            if op == self._HIDE:
                return hide_base | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW
            if op == self._SHOW:
                return show_base | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW
            return move_base

        try:
            hdwp = user32.BeginDeferWindowPos(len(items))
        except Exception:
            hdwp = 0

        def _apply_one(op: int, hwnd: int, x: int, y: int, w: int, h: int) -> None:
            try:
                user32.SetWindowPos(hwnd, 0, x, y, w, h, _flags(op))
            except Exception:
                pass

        if not hdwp:
            for op, hwnd, x, y, w, h in items:
                _apply_one(op, hwnd, x, y, w, h)
            return
        try:
            for op, hwnd, x, y, w, h in items:
                hdwp = user32.DeferWindowPos(
                    hdwp, hwnd, 0, x, y, w, h, _flags(op)
                )
                if not hdwp:
                    # Incomplete hdwp is discarded — apply the whole swap or
                    # first-visit 文档 fences stay Win32-hidden.
                    for op2, hwnd2, x2, y2, w2, h2 in items:
                        _apply_one(op2, hwnd2, x2, y2, w2, h2)
                    return
            user32.EndDeferWindowPos(hdwp)
        except Exception:
            for op, hwnd, x, y, w, h in items:
                _apply_one(op, hwnd, x, y, w, h)


def prime_hidden_overlay(
    hwnd: int, x: int, y: int, w: int, h: int, *, on_shown=None
) -> None:
    """Map once off-screen so the first on-screen SHOW is not DWM's first surface.

    Translucent DefView-owned windows flash the wallpaper on the first
    ``ShowWindow``. Idle warmup paints off-screen, then hides at the real
    rect; page clicks only hide/show an already-composited HWND.
    """
    if not hwnd or w <= 0 or h <= 0:
        return
    try:
        if not user32.IsWindow(int(hwnd)):
            return
        if user32.IsWindowVisible(int(hwnd)):
            return
    except Exception:
        return
    ox, oy = OFFSCREEN_PARK_POS
    show_flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW
    hide_flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_HIDEWINDOW
    try:
        user32.SetWindowPos(int(hwnd), 0, int(ox), int(oy), int(w), int(h), show_flags)
        if callable(on_shown):
            try:
                on_shown()
            except Exception:
                pass
        redraw_overlay_hwnd(int(hwnd))
        user32.SetWindowPos(
            int(hwnd), 0, int(x), int(y), int(w), int(h), hide_flags
        )
    except Exception:
        try:
            user32.SetWindowPos(
                int(hwnd), 0, int(x), int(y), int(w), int(h), hide_flags
            )
        except Exception:
            pass


def redraw_overlay_hwnd(hwnd: int) -> None:
    """Push Qt icon-grid content to an attached overlay HWND after page switch.

    Never pass ``RDW_ERASE`` — that cleared translucent fences to blank until
    the next desktop activation.
    """
    if not hwnd:
        return
    try:
        if not user32.IsWindow(int(hwnd)):
            return
    except Exception:
        return
    flags = RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW
    try:
        user32.RedrawWindow(int(hwnd), None, 0, flags)
    except Exception:
        pass


# Back-compat alias used by older call sites / tests.
OverlayGeometryBatch = OverlayWindowBatch


def set_overlay_hwnd_visible(hwnd: int, visible: bool) -> None:
    """Show/hide an already-attached overlay HWND without restacking.

    Page-switch park/unpark must use this — ``configure_desktop_overlay`` /
    ``SetWindowPos`` storms flash translucent fences several times per click.
    """
    if not hwnd:
        return
    try:
        if not user32.IsWindow(int(hwnd)):
            return
        shown = bool(user32.IsWindowVisible(int(hwnd)))
        if visible and not shown:
            user32.ShowWindow(int(hwnd), SW_SHOWNOACTIVATE)
        elif (not visible) and shown:
            user32.ShowWindow(int(hwnd), SW_HIDE)
    except Exception:
        pass


WM_SETREDRAW = 0x000B


@contextmanager
def freeze_desktop_paint():
    """No-op: DefView WM_SETREDRAW thaw repaints the whole wallpaper.

    Page switch uses ``OverlayWindowBatch`` (one DeferWindowPos) instead.
    """
    yield


# Legacy constant — soft park no longer moves off-screen (opacity/move both flash).
OFFSCREEN_PARK_POS = (-32000, -32000)
_DEFVIEW_CACHE: dict = {"t": 0.0, "host": 0, "defview": 0}
_DEFVIEW_CACHE_TTL_S = 2.5
# Rate-limit HWND_BOTTOM sinks to avoid cursor flash over translucent overlays.
_BAND_SINK_AT: dict[int, float] = {}


def invalidate_defview_host_cache() -> None:
    _DEFVIEW_CACHE["t"] = 0.0
    _DEFVIEW_CACHE["host"] = 0
    _DEFVIEW_CACHE["defview"] = 0


def explorer_shell_ready() -> bool:
    """True when Explorer's desktop DefView host is present (boot-safe gate)."""
    host, defview = _find_defview_host()
    return bool(host and defview)


def _find_defview_host(*, force: bool = False) -> tuple[int, int]:
    """Return (host_hwnd, defview_hwnd). host is Progman or WorkerW containing DefView.

    Prefer Progman / WorkerW FindWindowEx chains — avoid scanning every top-level
    window. With dozens of open apps, a full top-level enum freezes the UI thread.
    """
    now = time.monotonic()
    if (
        not force
        and _DEFVIEW_CACHE["host"]
        and now - float(_DEFVIEW_CACHE["t"]) < _DEFVIEW_CACHE_TTL_S
    ):
        host = int(_DEFVIEW_CACHE["host"])
        defview = int(_DEFVIEW_CACHE["defview"])
        # Cheap liveness check — avoid using a stale HWND after Explorer restart.
        try:
            if host and defview and user32.IsWindow(host) and user32.IsWindow(defview):
                return host, defview
        except Exception:
            pass

    shell_w = int(win32gui.FindWindow("Progman", None) or 0)
    if shell_w:
        defview = int(win32gui.FindWindowEx(shell_w, 0, "SHELLDLL_DefView", None) or 0)
        if defview:
            _DEFVIEW_CACHE["t"] = now
            _DEFVIEW_CACHE["host"] = shell_w
            _DEFVIEW_CACHE["defview"] = defview
            return shell_w, defview

    # Win11: DefView often lives under a WorkerW sibling — walk only WorkerW,
    # not every top-level window (Chrome/VS Code/etc.).
    found_host = 0
    found_def = 0
    worker = 0
    for _ in range(64):
        try:
            worker = int(win32gui.FindWindowEx(0, worker, "WorkerW", None) or 0)
        except OSError:
            worker = 0
        if not worker:
            break
        try:
            defview = int(
                win32gui.FindWindowEx(worker, 0, "SHELLDLL_DefView", None) or 0
            )
        except OSError:
            defview = 0
        if defview:
            found_host = worker
            found_def = defview
            break

    _DEFVIEW_CACHE["t"] = now
    _DEFVIEW_CACHE["host"] = found_host
    _DEFVIEW_CACHE["defview"] = found_def
    if found_host and found_def:
        return found_host, found_def
    return 0, 0


def find_desktop_shell_host() -> int:
    host, _defview = _find_defview_host()
    return host


def is_wallpaper_workerw(hwnd: int) -> bool:
    if not hwnd:
        return False
    try:
        if not user32.IsWindow(int(hwnd)):
            return False
        if win32gui.GetClassName(int(hwnd)) != "WorkerW":
            return False
        defview = win32gui.FindWindowEx(int(hwnd), 0, "SHELLDLL_DefView", None)
        return not bool(defview)
    except Exception:
        # pywintypes.error is not always an OSError — must not abort attach loops.
        return False


def is_stuck_under_wallpaper(hwnd: int) -> bool:
    """True when overlay is owned/parented by a wallpaper-only WorkerW (invisible)."""
    if not hwnd:
        return False
    try:
        if not user32.IsWindow(int(hwnd)):
            return False
        return is_wallpaper_workerw(_owner_of(hwnd)) or is_wallpaper_workerw(
            _parent_of(hwnd)
        )
    except Exception:
        return False


def _owner_of(hwnd: int) -> int:
    try:
        if not user32.IsWindow(int(hwnd)):
            return 0
        return int(win32gui.GetWindow(int(hwnd), GW_OWNER) or 0)
    except Exception:
        return 0


def _parent_of(hwnd: int) -> int:
    try:
        if not user32.IsWindow(int(hwnd)):
            return 0
        return int(win32gui.GetParent(int(hwnd)) or 0)
    except Exception:
        return 0


def is_attached_to_desktop(
    hwnd: int,
    *,
    host: int | None = None,
    defview: int | None = None,
) -> bool:
    if not hwnd:
        return False
    try:
        if not user32.IsWindow(int(hwnd)):
            return False
    except Exception:
        return False
    if host is None or defview is None:
        host, defview = _find_defview_host()
    if not host:
        return False
    owner = _owner_of(hwnd)
    parent = _parent_of(hwnd)
    # Owner or parent may report host or DefView depending on Win build.
    return owner in {host, defview} or parent in {host, defview}


def _screen_rect(hwnd: int) -> tuple[int, int, int, int]:
    left, top, right, bottom = win32gui.GetWindowRect(int(hwnd))
    return int(left), int(top), int(right), int(bottom)


def _restore_screen_rect(hwnd: int, rect: tuple[int, int, int, int]) -> None:
    left, top, right, bottom = rect
    w = max(1, right - left)
    h = max(1, bottom - top)
    user32.SetWindowPos(
        int(hwnd),
        0,
        left,
        top,
        w,
        h,
        SWP_NOZORDER | SWP_NOACTIVATE,
    )


def _set_owner(hwnd: int, owner: int) -> bool:
    try:
        user32.SetWindowLongPtrW(int(hwnd), GWLP_HWNDPARENT, int(owner))
        return True
    except Exception:
        try:
            win32gui.SetWindowLong(int(hwnd), GWLP_HWNDPARENT, int(owner))
            return True
        except OSError:
            return False


def place_overlay_in_desktop_band(hwnd: int, *, force: bool = False) -> None:
    if not hwnd:
        return
    try:
        if not user32.IsWindow(int(hwnd)):
            return
    except Exception:
        return
    # Rate-limit HWND_BOTTOM: repeating it while the cursor is over translucent
    # overlays makes the mouse flash white. Chrome (page bar) may pass force=True
    # after hotkey page switches so it is not left buried for 2s.
    now = time.monotonic()
    last = float(_BAND_SINK_AT.get(int(hwnd), 0.0))
    if not force and now - last < 2.0:
        return
    HWND_BOTTOM = 1
    flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
    try:
        user32.SetWindowPos(int(hwnd), HWND_BOTTOM, 0, 0, 0, 0, flags)
        _BAND_SINK_AT[int(hwnd)] = now
        # Bound cache size.
        if len(_BAND_SINK_AT) > 256:
            oldest = sorted(_BAND_SINK_AT.items(), key=lambda kv: kv[1])[:64]
            for key, _ in oldest:
                _BAND_SINK_AT.pop(key, None)
    except Exception:
        return


def raise_overlay_in_desktop_band(hwnd: int) -> None:
    """Raise an overlay to the top of the desktop-owned z-order (still under apps).

    Page chrome uses this after hotkey page flips: HWND_BOTTOM (band sink) can
    bury the right-edge bar under DefView/wallpaper so buttons vanish until a
    mouse-driven reattach.

    ``SWP_NOREDRAW`` avoids the brief opaque popup flash (users describe it as a
    VPN-like window opening and closing) when only Z-order needs fixing.

    Never discard client bits on Z-order-only raise — that wiped mint fence
    panels to solid black after boot live-fence restack (same rule as
    OverlayWindowBatch SHOW).

    ``SWP_NOOWNERZORDER`` is required: these overlays are Progman-owned top-level
    TOOLWINDOWs. HWND_TOP without it lifts the whole owner group above normal
    apps — fences then cover VS Code / WeChat a few seconds later (keepalive /
    chrome raise).

    Refuse when the HWND is not DefView/Progman-owned: bare HWND_TOP then paints
    above normal apps (organize / Explorer→desktop refresh).
    """
    if not hwnd:
        return
    try:
        if not user32.IsWindow(int(hwnd)):
            return
    except Exception:
        return
    try:
        if not is_attached_to_desktop(int(hwnd)):
            return
    except Exception:
        return
    HWND_TOP = 0
    flags = (
        SWP_NOMOVE
        | SWP_NOSIZE
        | SWP_NOACTIVATE
        | SWP_NOREDRAW
        | SWP_NOOWNERZORDER
    )
    try:
        user32.SetWindowPos(int(hwnd), HWND_TOP, 0, 0, 0, 0, flags)
        try:
            if not user32.IsWindowVisible(int(hwnd)):
                user32.ShowWindow(int(hwnd), SW_SHOWNOACTIVATE)
        except Exception:
            pass
    except Exception:
        return


def attach_overlay_to_desktop(hwnd: int, *, show: bool = True) -> bool:
    """Bind overlay to the DefView host.

    *show*: when False, attach only (page-switch first-create warms the HWND
    hidden so the shared ``OverlayWindowBatch`` can SWP_SHOWWINDOW with leavers).
    """
    if not hwnd:
        return False
    if is_stuck_under_wallpaper(hwnd):
        detach_overlay_from_desktop(hwnd)

    host, defview = _find_defview_host()
    if not host:
        return False
    if is_wallpaper_workerw(host):
        return False

    if is_attached_to_desktop(hwnd, host=host, defview=defview):
        if show:
            try:
                if not user32.IsWindowVisible(int(hwnd)):
                    user32.ShowWindow(int(hwnd), SW_SHOWNOACTIVATE)
            except Exception:
                pass
        # Do NOT HWND_BOTTOM here — repeating SetWindowPos on healthy overlays
        # makes the mouse cursor flicker when crossing fences/floats.
        return True

    try:
        rect = _screen_rect(int(hwnd))
    except OSError:
        return False

    target = host or defview
    if not target or is_wallpaper_workerw(target):
        return False
    if not _set_owner(int(hwnd), int(target)):
        return False

    try:
        _restore_screen_rect(int(hwnd), rect)
    except OSError:
        pass

    place_overlay_in_desktop_band(hwnd)

    if show:
        try:
            if not user32.IsWindowVisible(int(hwnd)):
                user32.ShowWindow(int(hwnd), SW_SHOWNOACTIVATE)
        except OSError:
            pass

    return is_attached_to_desktop(hwnd, host=host, defview=defview)


def detach_overlay_from_desktop(hwnd: int) -> bool:
    if not hwnd:
        return False
    if (
        not is_attached_to_desktop(hwnd)
        and not _owner_of(hwnd)
        and not _parent_of(hwnd)
    ):
        return True
    try:
        rect = _screen_rect(int(hwnd))
    except OSError:
        rect = (0, 0, 0, 0)

    _set_owner(int(hwnd), 0)

    try:
        if _parent_of(hwnd):
            win32gui.SetParent(int(hwnd), 0)
    except OSError:
        pass

    if rect[2] > rect[0] and rect[3] > rect[1]:
        try:
            _restore_screen_rect(int(hwnd), rect)
        except OSError:
            pass
    return True


def ensure_overlay_on_desktop(hwnd: int) -> bool:
    if not hwnd:
        return False
    if is_stuck_under_wallpaper(hwnd):
        return attach_overlay_to_desktop(hwnd)
    if is_attached_to_desktop(hwnd):
        try:
            if not user32.IsWindowVisible(int(hwnd)):
                user32.ShowWindow(int(hwnd), SW_SHOWNOACTIVATE)
        except OSError:
            pass
        return True
    return attach_overlay_to_desktop(hwnd)
