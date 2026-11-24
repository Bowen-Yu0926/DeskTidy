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
ISS_NATIVE_FILE = ROOT / "installer" / "DeskTidyNative.iss"
VERSION_RC = ROOT / "native" / "src" / "version.rc"


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
    return "1.0.0"


def sync_iss(path: Path, version: str) -> None:
    if not path.exists():
        return
    text = path.read_text(encoding="utf-8")
    updated = re.sub(
        r'#define MyAppVersion "[^"]+"',
        f'#define MyAppVersion "{version}"',
        text,
        count=1,
    )
    path.write_text(updated, encoding="utf-8")


def sync_version_rc(version: str) -> None:
    if not VERSION_RC.exists():
        return
    major, minor, patch = parse_version(version)
    text = VERSION_RC.read_text(encoding="utf-8")
    text = re.sub(
        r"FILEVERSION\s+\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+",
        f"FILEVERSION {major},{minor},{patch},0",
        text,
        count=1,
    )
    text = re.sub(
        r"PRODUCTVERSION\s+\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+",
        f"PRODUCTVERSION {major},{minor},{patch},0",
        text,
        count=1,
    )
    text = re.sub(
        r'VALUE "FileVersion", "[^"]*"',
        f'VALUE "FileVersion", "{version}"',
        text,
        count=1,
    )
    text = re.sub(
        r'VALUE "ProductVersion", "[^"]*"',
        f'VALUE "ProductVersion", "{version}"',
        text,
        count=1,
    )
    # Fallback strings in update_check.cpp stay as last-resort only; rc is truth.
    VERSION_RC.write_text(text, encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Sync or bump DeskTidy version")
    parser.add_argument(
        "--bump",
        action="store_true",
        help="Increment patch (or jump 3.x→4.0.0). Default is keep current.",
    )
    parser.add_argument(
        "--version",
        dest="explicit_version",
        default="",
        help="Explicitly set version (e.g. 4.0.52).",
    )
    args = parser.parse_args(argv)

    current = read_current()
    if args.explicit_version:
        version = args.explicit_version.strip().lstrip("vV")
    elif args.bump:
        version = bump_version(current)
    else:
        version = current
    # Normalize & keep files in sync even when not bumping.
    major, minor, patch = parse_version(version)
    version = format_version(major, minor, patch)
    VERSION_FILE.write_text(f"{version}\n", encoding="utf-8")
    sync_iss(ISS_FILE, version)
    sync_iss(ISS_NATIVE_FILE, version)
    sync_version_rc(version)
    # Keep update_check.cpp compile-time fallbacks aligned (not 0.0.0).
    uc = ROOT / "native" / "src" / "update_check.cpp"
    if uc.exists():
        ut = uc.read_text(encoding="utf-8")

        def _repl(m: re.Match[str]) -> str:
            if m.group(1) == "0.0.0":
                return m.group(0)
            return f'return L"{version}";'

        ut2 = re.sub(r'return L"(\d+\.\d+\.\d+)";', _repl, ut)
        if ut2 != ut:
            uc.write_text(ut2, encoding="utf-8")
    print(version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
