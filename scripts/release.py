#!/usr/bin/env python3
"""One-shot DeskTidy release: version → installer → GitHub Releases → site.

Keeps existing helpers (bump_version / build_native_installer / upload_byethost).
Default does NOT bump the version; pass --bump to raise the patch.

Examples:
  python scripts/release.py
  python scripts/release.py --bump --notes "fix fence icon sharpness"
  python scripts/release.py --no-github --no-site   # build installer only
  python scripts/release.py --dry-run --bump
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION_FILE = ROOT / "VERSION"
DESKTIDY_HTML = ROOT / "docs" / "desktidy.html"
INSTALLER_BAT = ROOT / "scripts" / "build_native_installer.bat"
BUMP_PY = ROOT / "scripts" / "bump_version.py"
UPLOAD_PY = ROOT / "scripts" / "upload_byethost.py"
RELEASES_REPO = "Bowen-Yu0926/DeskTidy-Releases"
DOWNLOAD_HOST = "https://github.com/Bowen-Yu0926/DeskTidy-Releases/releases/download"


def run(cmd: list[str] | str, *, dry_run: bool, shell: bool = False) -> None:
    printable = cmd if isinstance(cmd, str) else " ".join(cmd)
    print(f"\n>> {printable}")
    if dry_run:
        return
    subprocess.run(cmd, cwd=ROOT, check=True, shell=shell)


def read_version() -> str:
    return VERSION_FILE.read_text(encoding="utf-8").strip()


def sync_version(*, bump: bool, dry_run: bool) -> str:
    args = [sys.executable, str(BUMP_PY)]
    if bump:
        args.append("--bump")
    print(f"\n>> {' '.join(args)}")
    if dry_run:
        current = read_version()
        if bump:
            parts = [int(x) for x in current.split(".")[:3]]
            while len(parts) < 3:
                parts.append(0)
            parts[2] += 1
            return f"{parts[0]}.{parts[1]}.{parts[2]}"
        return current
    out = subprocess.check_output(args, cwd=ROOT, text=True).strip()
    # bump_version prints the version on the last line
    version = out.splitlines()[-1].strip()
    print(f"version={version}")
    return version


def build_installer(*, dry_run: bool) -> Path:
    if dry_run:
        ver = read_version()
        print(f"\n>> {INSTALLER_BAT}")
        return ROOT / "dist" / f"DeskTidy_Setup_{ver}.exe"
    if not INSTALLER_BAT.is_file():
        raise SystemExit(f"missing {INSTALLER_BAT}")
    run(str(INSTALLER_BAT), dry_run=False, shell=True)
    ver = read_version()
    setup = ROOT / "dist" / f"DeskTidy_Setup_{ver}.exe"
    if not setup.is_file():
        raise SystemExit(f"installer not found: {setup}")
    print(f"installer={setup} ({setup.stat().st_size} bytes)")
    return setup


def update_download_links(version: str, *, dry_run: bool) -> None:
    if not DESKTIDY_HTML.is_file():
        raise SystemExit(f"missing {DESKTIDY_HTML}")
    text = DESKTIDY_HTML.read_text(encoding="utf-8")
    url = f"{DOWNLOAD_HOST}/v{version}/DeskTidy_Setup_{version}.exe"
    updated = re.sub(
        r"https://github\.com/Bowen-Yu0926/DeskTidy-Releases/releases/download/v[\d.]+/DeskTidy_Setup_[\d.]+\.exe",
        url,
        text,
    )
    updated = re.sub(
        r"DeskTidy_Setup_[\d.]+\.exe",
        f"DeskTidy_Setup_{version}.exe",
        updated,
    )
    if updated == text:
        print(f"docs/desktidy.html already points at {version}")
        return
    print(f"\n>> update docs/desktidy.html → v{version}")
    if dry_run:
        return
    DESKTIDY_HTML.write_text(updated, encoding="utf-8")


def github_release(version: str, setup: Path, notes: str, *, dry_run: bool) -> None:
    if shutil.which("gh") is None:
        raise SystemExit("gh CLI not found; install GitHub CLI or pass --no-github")
    tag = f"v{version}"
    title = f"DeskTidy {version}"
    body = notes.strip() or f"DeskTidy {version}"
    # Replace existing tag/release assets when re-running the same version.
    view = subprocess.run(
        ["gh", "release", "view", tag, "--repo", RELEASES_REPO],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if view.returncode == 0:
        run(
            [
                "gh",
                "release",
                "upload",
                tag,
                str(setup),
                "--repo",
                RELEASES_REPO,
                "--clobber",
            ],
            dry_run=dry_run,
        )
        run(
            [
                "gh",
                "release",
                "edit",
                tag,
                "--repo",
                RELEASES_REPO,
                "--title",
                title,
                "--notes",
                body,
            ],
            dry_run=dry_run,
        )
    else:
        run(
            [
                "gh",
                "release",
                "create",
                tag,
                str(setup),
                "--repo",
                RELEASES_REPO,
                "--title",
                title,
                "--notes",
                body,
            ],
            dry_run=dry_run,
        )


def upload_site(*, dry_run: bool) -> None:
    args = [sys.executable, str(UPLOAD_PY)]
    run(args, dry_run=dry_run)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="DeskTidy one-shot release")
    parser.add_argument(
        "--bump",
        action="store_true",
        help="Increment patch before packaging (default: keep VERSION)",
    )
    parser.add_argument("--notes", default="", help="GitHub release notes")
    parser.add_argument(
        "--no-github",
        action="store_true",
        help="Skip gh release create/upload",
    )
    parser.add_argument(
        "--no-site",
        action="store_true",
        help="Skip ByetHost page upload (still updates docs/desktidy.html)",
    )
    parser.add_argument(
        "--skip-docs",
        action="store_true",
        help="Do not rewrite docs/desktidy.html download links",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print steps only",
    )
    args = parser.parse_args(argv)

    print("DeskTidy release")
    print(f"  root={ROOT}")
    print(f"  bump={args.bump} github={not args.no_github} site={not args.no_site}")

    version = sync_version(bump=args.bump, dry_run=args.dry_run)
    setup = build_installer(dry_run=args.dry_run)
    if not args.skip_docs:
        update_download_links(version, dry_run=args.dry_run)
    if not args.no_github:
        github_release(version, setup, args.notes, dry_run=args.dry_run)
    if not args.no_site:
        upload_site(dry_run=args.dry_run)

    print("\nDone.")
    print(f"  version={version}")
    print(f"  setup={setup}")
    if not args.no_github:
        print(f"  release={DOWNLOAD_HOST}/v{version}/DeskTidy_Setup_{version}.exe")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as exc:
        print(f"FAILED: {exc}", file=sys.stderr)
        raise SystemExit(exc.returncode or 1)
