"""Sync release version across VERSION / Inno / runtime (no auto-bump).

Packaging keeps the current version. Pass --bump only when the user
explicitly asks to raise the version number.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_FILE = ROOT / "VERSION"
ISS_FILE = ROOT / "installer" / "DeskTidy.iss"
VERSION_PY = ROOT / "src" / "_version.py"


def parse_version(raw: str) -> tuple[int, int, int]:
    parts = raw.strip().split(".")
    nums = [int(part) for part in parts[:3]]
    while len(nums) < 3:
        nums.append(0)
    return nums[0], nums[1], nums[2]


def format_version(major: int, minor: int, patch: int) -> str:
    return f"{major}.{minor}.{patch}"


def bump_version(raw: str) -> str:
    major, minor, patch = parse_version(raw)
    if major < 4:
        return "4.0.0"
    return format_version(major, minor, patch + 1)


def read_current() -> str:
    if VERSION_FILE.exists():
        return VERSION_FILE.read_text(encoding="utf-8").strip() or "1.0.0"
    if VERSION_PY.exists():
        m = re.search(r'__version__\s*=\s*"([^"]+)"', VERSION_PY.read_text(encoding="utf-8"))
        if m:
            return m.group(1)
    return "1.0.0"


def sync_iss(version: str) -> None:
    text = ISS_FILE.read_text(encoding="utf-8")
    updated = re.sub(
        r'#define MyAppVersion "[^"]+"',
        f'#define MyAppVersion "{version}"',
        text,
        count=1,
    )
    ISS_FILE.write_text(updated, encoding="utf-8")


def write_runtime_version(version: str) -> None:
    VERSION_PY.write_text(f'__version__ = "{version}"\n', encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Sync or bump DeskTidy version")
    parser.add_argument(
        "--bump",
        action="store_true",
        help="Increment patch (or jump 3.x→4.0.0). Default is keep current.",
    )
    args = parser.parse_args(argv)

    current = read_current()
    version = bump_version(current) if args.bump else current
    # Normalize & keep files in sync even when not bumping.
    major, minor, patch = parse_version(version)
    version = format_version(major, minor, patch)
    VERSION_FILE.write_text(f"{version}\n", encoding="utf-8")
    sync_iss(version)
    write_runtime_version(version)
    print(version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
