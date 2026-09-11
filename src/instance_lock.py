"""Single-instance mutex for DeskTidy main UI and notepad-only mode."""

from __future__ import annotations

import ctypes
from enum import Enum

_MUTEX_NAME = "Local\\DeskTidyMainInstance"
_NOTEPAD_MUTEX_NAME = "Local\\DeskTidyNotepadInstance"
_ERROR_ALREADY_EXISTS = 183
_SYNCHRONIZE = 0x00100000

_mutex_handle = None
_notepad_mutex_handle = None


class AcquireResult(Enum):
    OWNED = "owned"  # This process owns the main UI mutex.
    BUSY = "busy"  # Another process already owns it.
    FAILED = "failed"  # CreateMutex failed (do not pretend busy).


def _try_acquire_named(mutex_name: str) -> tuple[AcquireResult, object | None]:
    kernel32 = ctypes.windll.kernel32
    kernel32.SetLastError(0)
    handle = kernel32.CreateMutexW(None, True, mutex_name)
    if not handle:
        return AcquireResult.FAILED, None
    if kernel32.GetLastError() == _ERROR_ALREADY_EXISTS:
        kernel32.CloseHandle(handle)
        return AcquireResult.BUSY, None
    return AcquireResult.OWNED, handle


def try_acquire_main_instance() -> AcquireResult:
    """Acquire the main-instance mutex.

    Uses CreateMutex(bInitialOwner=True): the creator owns the mutex
    immediately, and a racing second process sees ERROR_ALREADY_EXISTS.
    """
    global _mutex_handle
    result, handle = _try_acquire_named(_MUTEX_NAME)
    if result is AcquireResult.OWNED:
        _mutex_handle = handle
    return result


def try_acquire_notepad_instance() -> AcquireResult:
    """Acquire the notepad-only instance mutex (separate from main UI)."""
    global _notepad_mutex_handle
    result, handle = _try_acquire_named(_NOTEPAD_MUTEX_NAME)
    if result is AcquireResult.OWNED:
        _notepad_mutex_handle = handle
    return result


def is_main_instance_running() -> bool:
    """True if another (or this) process holds the main mutex."""
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.OpenMutexW(_SYNCHRONIZE, False, _MUTEX_NAME)
    if handle:
        kernel32.CloseHandle(handle)
        return True
    return False


def is_notepad_instance_running() -> bool:
    """True if a notepad-only process holds its mutex."""
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.OpenMutexW(_SYNCHRONIZE, False, _NOTEPAD_MUTEX_NAME)
    if handle:
        kernel32.CloseHandle(handle)
        return True
    return False


def release_main_instance() -> None:
    global _mutex_handle
    if not _mutex_handle:
        return
    kernel32 = ctypes.windll.kernel32
    try:
        kernel32.ReleaseMutex(_mutex_handle)
    except Exception:
        pass
    try:
        kernel32.CloseHandle(_mutex_handle)
    except Exception:
        pass
    _mutex_handle = None


def release_notepad_instance() -> None:
    global _notepad_mutex_handle
    if not _notepad_mutex_handle:
        return
    kernel32 = ctypes.windll.kernel32
    try:
        kernel32.ReleaseMutex(_notepad_mutex_handle)
    except Exception:
        pass
    try:
        kernel32.CloseHandle(_notepad_mutex_handle)
    except Exception:
        pass
    _notepad_mutex_handle = None
