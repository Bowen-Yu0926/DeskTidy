"""Global hotkey registration via Windows RegisterHotKey."""

from __future__ import annotations

import ctypes
import time
from ctypes import wintypes
from typing import Callable

from PyQt6.QtCore import QObject, QTimer
from PyQt6.QtWidgets import QApplication

user32 = ctypes.windll.user32
MOD_ALT = 0x0001
MOD_CONTROL = 0x0002
MOD_SHIFT = 0x0004
MOD_WIN = 0x0008
MOD_NOREPEAT = 0x4000
WM_HOTKEY = 0x0312
PM_REMOVE = 0x0001

HOTKEY_IDS = {
    "organize": 1,
    "toggle_fences": 2,
    "toggle_icons": 3,
    "peek_fences": 4,
    "page_next": 5,
    "page_prev": 6,
    "screenshot": 7,
    "show_screenshot": 8,
    "screen_record": 9,
    "file_search": 10,
    "calculator": 11,
}
_ACTION_BY_ID = {hid: name for name, hid in HOTKEY_IDS.items()}

# Canonical Chinese labels — tray, settings, and conflict tips must share these.
HOTKEY_LABELS = {
    "organize": "一键整理",
    "toggle_fences": "显示/隐藏分区",
    "toggle_icons": "显示/隐藏桌面图标",
    "peek_fences": "临时查看分区",
    "page_next": "下一页",
    "page_prev": "上一页",
    "screenshot": "区域截图",
    "show_screenshot": "显示截图贴图",
    "screen_record": "录屏（开始/结束）",
    "file_search": "文件搜索",
    "calculator": "计算器",
}

# Used only when the configured binding cannot be parsed / registered at all.
HOTKEY_FALLBACKS: dict[str, list[str]] = {
    "screenshot": ["Ctrl+Shift+S", "Ctrl+Alt+S"],
    "show_screenshot": ["Ctrl+Shift+P", "Ctrl+Alt+P"],
    "screen_record": ["Ctrl+Shift+R"],
    "file_search": ["Ctrl+Alt+F", "Ctrl+Alt+Space"],
    "calculator": ["Ctrl+Alt+C"],
}
# Poll intervals (ms): keep idle cost low for 24/7 tray operation.
_POLL_IDLE_MS = 1200
_POLL_HELD_MS = 200
_POLL_WM_ONLY_MS = 1500
# Bare F-keys need GetAsyncKeyState; Ctrl/Alt combos use RegisterHotKey only.
_POLLABLE_ACTIONS = {"screenshot", "show_screenshot", "screen_record", "file_search"}
_VK_CONTROL = 0x11
_VK_MENU = 0x12
_VK_SHIFT = 0x10
_VK_LWIN = 0x5B
_VK_RWIN = 0x5C


_VK_F1 = 0x70
_VK_F12 = 0x7B


def _bare_function_key(modifiers: int, vk: int) -> bool:
    """True for F1–F12 without Ctrl/Alt/Shift/Win.

    Bare F-keys need RegisterHotKey (consume system Help etc.) plus
    GetAsyncKeyState poll — WM_HOTKEY is often not delivered under Qt.
    """
    return modifiers == 0 and _VK_F1 <= vk <= _VK_F12


def _parse_hotkey(hotkey_str: str) -> tuple[int, int] | None:
    """Parse 'Ctrl+Shift+O' into (modifiers, vk_code)."""
    if not hotkey_str:
        return None

    parts = [p.strip().lower() for p in hotkey_str.split("+") if p.strip()]
    if not parts:
        return None

    modifiers = 0
    key_part = parts[-1]
    for part in parts[:-1]:
        if part in ("ctrl", "control"):
            modifiers |= MOD_CONTROL
        elif part == "alt":
            modifiers |= MOD_ALT
        elif part == "shift":
            modifiers |= MOD_SHIFT
        elif part in ("win", "windows", "meta", "super"):
            modifiers |= MOD_WIN

    vk_map = {
        "a": 0x41, "b": 0x42, "c": 0x43, "d": 0x44, "e": 0x45, "f": 0x46,
        "g": 0x47, "h": 0x48, "i": 0x49, "j": 0x4A, "k": 0x4B, "l": 0x4C,
        "m": 0x4D, "n": 0x4E, "o": 0x4F, "p": 0x50, "q": 0x51, "r": 0x52,
        "s": 0x53, "t": 0x54, "u": 0x55, "v": 0x56, "w": 0x57, "x": 0x58,
        "y": 0x59, "z": 0x5A,
        "0": 0x30, "1": 0x31, "2": 0x32, "3": 0x33, "4": 0x34,
        "5": 0x35, "6": 0x36, "7": 0x37, "8": 0x38, "9": 0x39,
        "f1": 0x70, "f2": 0x71, "f3": 0x72, "f4": 0x73, "f5": 0x74,
        "f6": 0x75, "f7": 0x76, "f8": 0x77, "f9": 0x78, "f10": 0x79,
        "f11": 0x7A, "f12": 0x7B,
        "space": 0x20,
        "left": 0x25, "right": 0x27, "up": 0x26, "down": 0x28,
    }

    vk = vk_map.get(key_part)
    if vk is None and len(key_part) == 1:
        vk = ord(key_part.upper())
    if vk is None:
        return None
    return modifiers, vk


def _dispatch_on_main_thread(callback: Callable[[], None]) -> None:
    app = QApplication.instance()
    if app is None:
        callback()
        return
    # Poll/timer already runs on the GUI thread — invoke immediately so
    # RegisterHotKey foreground allowance survives (deferring breaks F1 under modals).
    try:
        from PyQt6.QtCore import QThread

        if QThread.currentThread() == app.thread():
            callback()
            return
    except Exception:
        pass
    QTimer.singleShot(0, callback)


class HotkeyManager(QObject):
    """Register global hotkeys and dispatch callbacks.

    PyQt6 on Windows often does not deliver WM_HOTKEY through
    QAbstractNativeEventFilter, so we poll the thread message queue instead.
    """

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self._callbacks: dict[int, Callable[[], None]] = {}
        self._registered: list[int] = []
        self._failed: list[tuple[str, str]] = []
        self._polled: dict[str, tuple[int, int, Callable[[], None]]] = {}
        self._polled_down: dict[str, bool] = {}
        self._last_triggered: dict[str, float] = {}
        self._suppress_until: dict[str, float] = {}
        self._poll_timer = QTimer(self)
        # PeekMessage for WM_HOTKEY; GetAsyncKeyState for bare F-keys (quick taps).
        self._poll_timer.setInterval(_POLL_IDLE_MS)
        self._poll_timer.timeout.connect(self._poll_hotkeys)

    @property
    def failed_bindings(self) -> list[tuple[str, str]]:
        return list(self._failed)

    def register(self, action: str, hotkey_str: str, callback: Callable[[], None]) -> bool:
        hotkey_id = HOTKEY_IDS.get(action)
        if hotkey_id is None:
            return False

        self.unregister(action)
        parsed = _parse_hotkey(hotkey_str)
        if parsed is None:
            self._failed.append((action, hotkey_str))
            self._ensure_poll_timer()
            return False

        modifiers, vk = parsed
        bare_fkey = _bare_function_key(modifiers, vk)
        # Bare F1–F12: RegisterHotKey(NOREPEAT) consumes system Help; also poll
        # because Qt often never delivers WM_HOTKEY. Never fall back to a
        # repeating RegisterHotKey — that thrash-toggles while the key is held.
        # Poll applies to *all* actions with a bare F binding (organize/pages/…),
        # not only screenshot/record.

        if bare_fkey:
            modifier_attempts = [modifiers | MOD_NOREPEAT]
        else:
            modifier_attempts = [modifiers | MOD_NOREPEAT, modifiers]
        seen: set[int] = set()
        registered = False
        for attempt in modifier_attempts:
            if attempt in seen:
                continue
            seen.add(attempt)
            if user32.RegisterHotKey(None, hotkey_id, attempt, vk):
                self._callbacks[hotkey_id] = callback
                self._registered.append(hotkey_id)
                registered = True
                break

        # Bare F: always async-poll. Screenshot/record also poll when RegisterHotKey
        # failed (modifier combos) so the binding still works without QShortcut focus.
        if bare_fkey or (action in _POLLABLE_ACTIONS and not registered):
            self._polled[action] = (modifiers, vk, callback)
            self._polled_down[action] = False

        if registered or action in self._polled:
            self._failed = [(a, k) for a, k in self._failed if a != action]
            self._arm_after_register(action, modifiers, vk)
            self._ensure_poll_timer()
            return True

        self._failed.append((action, hotkey_str))
        self._ensure_poll_timer()
        return False

    def register_with_fallbacks(
        self,
        action: str,
        hotkey_str: str,
        callback: Callable[[], None],
    ) -> str | None:
        """Try primary binding then HOTKEY_FALLBACKS for this action.

        Prefer keeping the user's string whenever it can be parsed; only fall
        back when the primary binding is empty or unusable.
        """
        if hotkey_str and self.register(action, hotkey_str, callback):
            return hotkey_str
        for alt in HOTKEY_FALLBACKS.get(action, []):
            if alt.casefold() == (hotkey_str or "").casefold():
                continue
            if self.register(action, alt, callback):
                return alt
        return None

    def unregister(self, action: str) -> None:
        hotkey_id = HOTKEY_IDS.get(action)
        if hotkey_id is None:
            return
        if hotkey_id in self._registered:
            user32.UnregisterHotKey(None, hotkey_id)
            self._registered.remove(hotkey_id)
        self._callbacks.pop(hotkey_id, None)
        self._polled.pop(action, None)
        self._polled_down.pop(action, None)
        self._last_triggered.pop(action, None)
        self._suppress_until.pop(action, None)
        self._ensure_poll_timer()

    def unregister_all(self) -> None:
        for hotkey_id in list(self._registered):
            user32.UnregisterHotKey(None, hotkey_id)
        self._registered.clear()
        self._callbacks.clear()
        self._failed.clear()
        self._polled.clear()
        self._polled_down.clear()
        self._last_triggered.clear()
        self._suppress_until.clear()
        self._poll_timer.stop()

    @staticmethod
    def _flush_wm_hotkey_queue() -> None:
        """Drop queued WM_HOTKEY messages (e.g. key still held during RegisterHotKey)."""
        msg = wintypes.MSG()
        while user32.PeekMessageW(
            ctypes.byref(msg),
            None,
            WM_HOTKEY,
            WM_HOTKEY,
            PM_REMOVE,
        ):
            pass

    def _arm_after_register(self, action: str, modifiers: int, vk: int) -> None:
        """Ignore the key chord that was just used to bind this action in settings."""
        now = time.monotonic()
        self._last_triggered[action] = now
        self._suppress_until[action] = now + 0.55
        if action in self._polled:
            held = self._modifiers_match(modifiers) and self._vk_down(vk)
            # Treat an still-held binding key as already down — wait for release.
            self._polled_down[action] = held
        self._flush_wm_hotkey_queue()

    def _ensure_poll_timer(self) -> None:
        if not self._callbacks and not self._polled:
            self._poll_timer.stop()
            return
        # Bare F-keys: slower idle poll; tighten while held. WM_HOTKEY-only stays slower.
        if self._polled:
            any_down = any(self._polled_down.get(a, False) for a in self._polled)
            wanted = _POLL_HELD_MS if any_down else _POLL_IDLE_MS
        else:
            wanted = _POLL_WM_ONLY_MS
        if self._poll_timer.interval() != wanted:
            self._poll_timer.setInterval(wanted)
        if not self._poll_timer.isActive():
            self._poll_timer.start()

    def _trigger_action(self, action: str, callback: Callable[[], None]) -> None:
        now = time.monotonic()
        if now < float(self._suppress_until.get(action, 0.0)):
            return
        last = self._last_triggered.get(action, 0.0)
        # Pollable toggles (screenshot/record) need a slightly longer gate so
        # WM_HOTKEY + async poll cannot double-fire across adjacent ticks.
        gate = 0.45 if action in _POLLABLE_ACTIONS else 0.35
        if now - last < gate:
            return
        self._last_triggered[action] = now
        _dispatch_on_main_thread(callback)

    @staticmethod
    def _vk_down(vk: int) -> bool:
        return bool(user32.GetAsyncKeyState(vk) & 0x8000)

    @staticmethod
    def _vk_state(vk: int) -> tuple[bool, bool]:
        """Return (currently_down, pressed_since_last_query).

        Bit 0 catches quick taps between poll ticks so F1 does not need to be held.
        """
        state = user32.GetAsyncKeyState(vk)
        return bool(state & 0x8000), bool(state & 0x0001)

    def _modifiers_match(self, modifiers: int) -> bool:
        want_ctrl = bool(modifiers & MOD_CONTROL)
        want_alt = bool(modifiers & MOD_ALT)
        want_shift = bool(modifiers & MOD_SHIFT)
        want_win = bool(modifiers & MOD_WIN)
        have_ctrl = self._vk_down(_VK_CONTROL)
        have_alt = self._vk_down(_VK_MENU)
        have_shift = self._vk_down(_VK_SHIFT)
        have_win = self._vk_down(_VK_LWIN) or self._vk_down(_VK_RWIN)
        return (
            want_ctrl == have_ctrl
            and want_alt == have_alt
            and want_shift == have_shift
            and want_win == have_win
        )

    def _poll_hotkeys(self) -> None:
        msg = wintypes.MSG()
        while user32.PeekMessageW(
            ctypes.byref(msg),
            None,
            WM_HOTKEY,
            WM_HOTKEY,
            PM_REMOVE,
        ):
            hotkey_id = int(msg.wParam)
            callback = self._callbacks.get(hotkey_id)
            if callback is not None:
                action = _ACTION_BY_ID.get(hotkey_id, str(hotkey_id))
                self._trigger_action(action, callback)
        any_down = False
        for action, (modifiers, vk, callback) in list(self._polled.items()):
            currently_down, pressed_since = self._vk_state(vk)
            if not self._modifiers_match(modifiers):
                currently_down = False
                pressed_since = False
            was_down = self._polled_down.get(action, False)
            # Rising edge or "pressed since last poll" — short F1 taps must not be missed.
            if (currently_down and not was_down) or pressed_since:
                self._trigger_action(action, callback)
            self._polled_down[action] = currently_down
            if currently_down:
                any_down = True
        # Adapt poll rate after each tick (idle vs key-held).
        if self._polled:
            wanted = _POLL_HELD_MS if any_down else _POLL_IDLE_MS
            if self._poll_timer.interval() != wanted:
                self._poll_timer.setInterval(wanted)
