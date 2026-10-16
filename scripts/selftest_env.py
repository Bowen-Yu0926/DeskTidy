"""Shared selftest environment — import at the top of every selftest runner."""

from __future__ import annotations

import os
import sys
from pathlib import Path

# Child processes inherit this; screen_record_manager skips Explorer reveal, etc.
os.environ.setdefault("DESKTIDY_SELFTEST", "1")


def running() -> bool:
    if os.environ.get("DESKTIDY_SELFTEST") == "1":
        return True
    return any("selftest" in Path(arg).name.casefold() for arg in sys.argv)


def isolate_settings_file() -> None:
    """Redirect ``src.settings.SETTINGS_FILE`` to a temp copy of the real config.

    Selftests drive app methods with Fake objects whose ``save_settings`` calls
    flush via debounce timers *after* the test returns — those would otherwise
    write the developer's real ``settings.json`` (leaking test stub values into
    the user's config). Reads still see the same content because the temp file is
    seeded from the real one. Idempotent; safe to call more than once.
    """
    try:
        import shutil
        import tempfile

        import src.settings as _cfg
    except Exception:
        return
    if getattr(_cfg, "_SELFTEST_ISOLATED", False):
        return
    try:
        real = Path(_cfg.SETTINGS_FILE)
        tmp_dir = Path(tempfile.mkdtemp(prefix="desktidy_selftest_cfg_"))
        target = tmp_dir / "settings.json"
        if real.exists():
            shutil.copy2(real, target)
        _cfg.SETTINGS_FILE = target
        _cfg._SELFTEST_ISOLATED = True
    except Exception:
        pass


# Importing this module opts the test process into config isolation.
isolate_settings_file()
