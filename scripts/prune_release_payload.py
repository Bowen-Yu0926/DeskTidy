"""Strip non-runtime bloat from the onedir payload before Inno packs it.

Keeps DeskNote WebEngine + recording FFmpeg intact. Removes:
  - Chromium debug / DevTools packs
  - Unused WebEngine locales (keep zh-CN / zh-TW / en-US)
  - Unused Qt translations (keep zh_CN / zh_TW / en)
  - Qt Quick3D / Sensors / TTS / Test / RemoteObjects (not used by DeskTidy UI)
  - Alternate Qt Quick Controls themes (Imagine / Universal / Fusion)
  - Accidental Cython compiler tree
  - Pet QA / base scratch PNGs that are never loaded at runtime

Run from build.bat on dist_staging\\DeskTidy (also safe on dist\\DeskTidy).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KEEP_LOCALES = frozenset({"en-US.pak", "zh-CN.pak", "zh-TW.pak"})

# Qt translation keep: Chinese + English only.
_TR_KEEP = re.compile(
    r"(?:^|[_.-])(?:zh_CN|zh_TW|en)(?:[_.-]|$)|(?:zh_CN|zh_TW|en)\.qm$",
    re.IGNORECASE,
)

# Qt6 bin DLLs never imported by DeskTidy / DeskNote Widgets + WebEngine path.
_UNUSED_BIN_PREFIXES = (
    "Qt6Quick3D",
    "Qt6Test",
    "Qt6QuickTest",
    "Qt6Sensors",
    "Qt6TextToSpeech",
    "Qt6RemoteObjects",
    "Qt6StateMachine",
    "Qt6SpatialAudio",
)
_UNUSED_BIN_EXACT = frozenset(
    {
        "Qt6QuickControls2Imagine.dll",
        "Qt6QuickControls2ImagineStyleImpl.dll",
        "Qt6QuickControls2Universal.dll",
        "Qt6QuickControls2UniversalStyleImpl.dll",
        "Qt6QuickControls2Fusion.dll",
        "Qt6QuickControls2FusionStyleImpl.dll",
    }
)

_UNUSED_QML_TOP = frozenset(
    {
        "QtQuick3D",
        "QtTest",
        "QtSensors",
        "QtTextToSpeech",
        "QtRemoteObjects",
    }
)

_PET_SCRATCH = re.compile(r"(?i)^_qa_|_wait_base\.png$")


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


def _is_unused_qt_translation(path: Path) -> bool:
    parts = {p.lower() for p in path.parts}
    if "translations" not in parts:
        return False
    if path.suffix.lower() != ".qm":
        return False
    # Never strip WebEngine locale packs here (handled separately).
    if path.parent.name.lower() == "qtwebengine_locales":
        return False
    return _TR_KEEP.search(path.name) is None


def _is_cython_tree(path: Path) -> bool:
    parts = [p.lower() for p in path.parts]
    try:
        i = parts.index("_internal")
    except ValueError:
        return False
    return i + 1 < len(parts) and parts[i + 1] == "cython"


def _is_unused_qt_bin(path: Path) -> bool:
    if path.suffix.lower() != ".dll":
        return False
    parts = [p.lower() for p in path.parts]
    if "qt6" not in parts or "bin" not in parts:
        return False
    name = path.name
    if name in _UNUSED_BIN_EXACT:
        return True
    return any(name.startswith(prefix) for prefix in _UNUSED_BIN_PREFIXES)


def _is_unused_qml_tree(path: Path) -> bool:
    parts = list(path.parts)
    lower = [p.lower() for p in parts]
    try:
        qi = lower.index("qml")
    except ValueError:
        return False
    if qi + 1 >= len(parts):
        return False
    # .../Qt6/qml/<TopLevel>/...
    return parts[qi + 1] in _UNUSED_QML_TOP


def _is_pet_scratch(path: Path) -> bool:
    parts = [p.lower() for p in path.parts]
    if "pets" not in parts:
        return False
    return _PET_SCRATCH.search(path.name) is not None


def should_prune(path: Path) -> bool:
    if not path.is_file():
        return False
    return (
        _is_debug_asset(path)
        or _is_unused_webengine_locale(path)
        or _is_unused_qt_translation(path)
        or _is_cython_tree(path)
        or _is_unused_qt_bin(path)
        or _is_unused_qml_tree(path)
        or _is_pet_scratch(path)
    )


def prune_tree(root: Path, *, dry_run: bool = False) -> tuple[int, int]:
    """Return (removed_count, removed_bytes)."""
    if not root.is_dir():
        raise FileNotFoundError(root)
    removed = 0
    nbytes = 0
    # Collect first — deleting while walking can skip siblings on some FS.
    targets = [p for p in root.rglob("*") if should_prune(p)]
    for path in targets:
        size = path.stat().st_size
        removed += 1
        nbytes += size
        if dry_run:
            continue
        path.unlink(missing_ok=True)
    if not dry_run:
        _remove_empty_dirs(root)
    return removed, nbytes


def _remove_empty_dirs(root: Path) -> None:
    """Drop empty folders left after file pruning (Cython / QML trees)."""
    # Deepest paths first.
    dirs = sorted(
        (p for p in root.rglob("*") if p.is_dir()),
        key=lambda p: len(p.parts),
        reverse=True,
    )
    for path in dirs:
        try:
            next(path.iterdir())
        except StopIteration:
            path.rmdir()
        except OSError:
            pass


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
