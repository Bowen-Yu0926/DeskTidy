"""Strip non-runtime WebEngine/debug assets from the onedir payload.

PyQt6 WebEngine ships Chromium debug packs and DevTools front-end that DeskNote
never loads. Leaving them in dist bloats DeskTidy_Setup_*.exe (~80MB+ uncompressed)
and slows the first double-click while Defender scans the large unsigned setup.

Keep zh-CN / zh-TW / en-US locale packs only.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KEEP_LOCALES = frozenset({"en-US.pak", "zh-CN.pak", "zh-TW.pak"})


def _is_debug_asset(path: Path) -> bool:
    name = path.name.lower()
    if name.endswith(".pdb"):
        return True
    if ".debug." in name:
        return True
    if "devtools" in name and name.endswith((".pak", ".bin")):
        return True
    return False


def _is_unused_webengine_locale(path: Path) -> bool:
    if path.parent.name.lower() != "qtwebengine_locales":
        return False
    return path.suffix.lower() == ".pak" and path.name not in KEEP_LOCALES


def prune_tree(root: Path, *, dry_run: bool = False) -> tuple[int, int]:
    """Return (removed_count, removed_bytes)."""
    if not root.is_dir():
        raise FileNotFoundError(root)
    removed = 0
    nbytes = 0
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if not (_is_debug_asset(path) or _is_unused_webengine_locale(path)):
            continue
        size = path.stat().st_size
        removed += 1
        nbytes += size
        if dry_run:
            continue
        path.unlink(missing_ok=True)
    return removed, nbytes


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "root",
        nargs="?",
        default=str(ROOT / "dist" / "DeskTidy"),
        help="onedir payload root (default: dist/DeskTidy)",
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    root = Path(args.root)
    count, nbytes = prune_tree(root, dry_run=args.dry_run)
    mb = nbytes / (1024 * 1024)
    mode = "would remove" if args.dry_run else "removed"
    print(f"prune_release_payload: {mode} {count} files ({mb:.1f} MB) under {root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
