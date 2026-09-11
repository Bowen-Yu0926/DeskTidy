"""Remove numbered duplicate files (e.g. foo_1.lnk when foo.lnk exists)."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from src.settings import get_desktop_path  # noqa: E402

_SUFFIX_RE = re.compile(r"^(.*)_(\d+)(\.[^.]+)$")


def find_duplicates_in_folder(folder: Path) -> list[Path]:
    groups: dict[str, list[tuple[int, Path]]] = {}

    for path in folder.iterdir():
        if not path.is_file() or path.name.startswith("."):
            continue
        match = _SUFFIX_RE.match(path.name)
        if match:
            base_name = f"{match.group(1)}{match.group(3)}"
            groups.setdefault(base_name, []).append((int(match.group(2)), path))
        else:
            groups.setdefault(path.name, []).append((-1, path))

    to_delete: list[Path] = []
    for entries in groups.values():
        if len(entries) <= 1:
            continue
        entries.sort(key=lambda item: item[0])
        for _, path in entries[1:]:
            to_delete.append(path)
    return to_delete


def cleanup_desktop(max_depth: int = 1) -> list[Path]:
    desktop = get_desktop_path()
    folders = [desktop]
    if max_depth > 0:
        folders.extend(p for p in desktop.iterdir() if p.is_dir())

    deleted: list[Path] = []
    for folder in folders:
        for path in find_duplicates_in_folder(folder):
            path.unlink(missing_ok=True)
            deleted.append(path)
    return deleted


def main() -> int:
    deleted = cleanup_desktop(max_depth=1)
    if not deleted:
        print("No numbered duplicates found.")
        return 0
    print(f"Deleted {len(deleted)} duplicate file(s):")
    for path in deleted:
        print(f"  {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
