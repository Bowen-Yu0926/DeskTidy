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
