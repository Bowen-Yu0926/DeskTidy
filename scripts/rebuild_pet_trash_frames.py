"""Import regenerated trash cels for hoodie and voyage."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.double_trash_frames import double_trash
from scripts.import_trash_gen_frames import main as rebuild_hoodie_trash
from scripts.import_voyage_trash_frames import rebuild_trash as rebuild_voyage_trash


def main() -> int:
    rebuild_hoodie_trash()
    rebuild_voyage_trash()
    for char in ("hoodie", "voyage"):
        double_trash(char)
    print("OK: all pet trash cels installed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
