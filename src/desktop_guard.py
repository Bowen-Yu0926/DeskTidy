"""Legacy login-time desktop guard (kept for old Startup shortcuts).

Older builds installed a second ``DeskTidy-桌面守护.lnk`` / ``Desktidy-桌面守护.lnk``
that ran ``--desktop-guard``. Current builds use a single ``DeskTidy.lnk``; the main
app restores or re-hides shell icons itself. If an old guard shortcut still
fires once, restore a normal desktop when the main UI is absent, then delete
the guard shortcut so it does not keep appearing in startup managers.
"""

from __future__ import annotations

import time


def run_desktop_guard(*, wait_seconds: float = 8.0) -> int:
    from src.instance_lock import is_main_instance_running
    from src.settings import load_settings, remove_desktop_guard_autostart, save_settings
    from src.win_shell import (
        ensure_desktop_icons_visible,
        reveal_hosted_namespace_icons,
    )

    try:
        # Always retire the dual-startup shortcut, even if main UI is present.
        remove_desktop_guard_autostart()
    except Exception:
        pass

    deadline = time.time() + max(1.0, wait_seconds)
    while time.time() < deadline:
        if is_main_instance_running():
            return 0
        time.sleep(0.5)

    if is_main_instance_running():
        return 0

    # Main UI did not start — leave a normal Windows desktop.
    try:
        ensure_desktop_icons_visible()
    except Exception:
        pass
    try:
        reveal_hosted_namespace_icons()
    except Exception:
        pass

    try:
        settings = load_settings()
        settings["session_active"] = False
        settings["hide_shell_icons"] = False
        save_settings(settings, immediate=True)
    except Exception:
        pass
    return 0
