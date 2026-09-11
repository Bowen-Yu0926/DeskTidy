"""Lightweight notepad-only UI (deskNote process; no fences / tray / pet)."""

from __future__ import annotations

import sys
from pathlib import Path


def attach_desknote_ipc(win) -> None:
    """Host IPC so DeskTidy / second launches can focus or open files."""
    from src.desknote_ipc import start_desknote_ipc_host
    from src.desknote_launch import OPEN_FILES_VERB, OPEN_VERB

    def _on_verb(verb: str, paths: list[str]) -> None:
        try:
            win.present()
        except Exception:
            pass
        if verb == OPEN_FILES_VERB and paths:
            try:
                win.open_paths([Path(p) for p in paths])
            except Exception:
                pass
        elif verb in {OPEN_VERB, OPEN_FILES_VERB, "open-notepad"}:
            pass

    start_desknote_ipc_host(_on_verb)


def run_notepad_only() -> int:
    """Open built-in notepad and quit when the window closes.

    Does not acquire the main DeskTidy instance mutex and does not start
    desktop overlays. Used by legacy ``DeskTidy.exe --notepad`` and by
    ``desknote_main`` via ``run_desknote_process``.
    """
    from src.desknote_launch import run_desknote_process

    return int(run_desknote_process(sys.argv[1:]) or 0)
