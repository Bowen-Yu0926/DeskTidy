#!/usr/bin/env python3
"""Upload DeskTidy site pages to ByetHost via FTP.

Credentials: Desktidy/.env.byethost (gitignored) or env vars BYETHOST_*.

Examples:
  python scripts/upload_byethost.py              # index(=desktidy) + desktidy + desknote
  python scripts/upload_byethost.py --purge-snapkeep  # also remove SnapKeep files on host
  python scripts/upload_byethost.py --list       # list htdocs after connect
"""
from __future__ import annotations

import argparse
import os
import sys
import time
from ftplib import FTP, error_perm
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENV_FILE = ROOT / ".env.byethost"
DOCS = ROOT / "docs"

# SnapKeep leftovers to remove from htdocs when --purge-snapkeep
SNAPKEEP_ROOT_FILES = (
    "index.php",
    "config.php",
    "config.example.php",
    "favicon.svg",
    "icons.svg",
    "README.md",
    ".gitignore",
    ".htaccess",
)
SNAPKEEP_ROOT_DIRS = (
    "snapkeep",
    "assets",  # old SnapKeep SPA assets at site root
    "lib",
    "src",
    "db",
    "data",
    "public",
)


def load_env_file(path: Path) -> None:
    if not path.is_file():
        return
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        val = val.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = val


def cfg(name: str, default: str = "") -> str:
    return (os.environ.get(name) or default).strip()


def ensure_cwd(ftp: FTP, path: str) -> None:
    parts = [p for p in path.replace("\\", "/").split("/") if p]
    ftp.cwd("/")
    for p in parts:
        try:
            ftp.cwd(p)
        except error_perm:
            try:
                ftp.mkd(p)
            except error_perm:
                pass
            ftp.cwd(p)


def upload_file(ftp: FTP, local: Path, remote_name: str) -> None:
    size = local.stat().st_size
    print(f"  STOR {remote_name}  ({size / (1024 * 1024):.1f} MB) ...", flush=True)
    t0 = time.time()
    with local.open("rb") as f:
        ftp.storbinary(f"STOR {remote_name}", f, blocksize=1024 * 256)
    print(f"    ok in {time.time() - t0:.1f}s", flush=True)


def connect() -> tuple[FTP, str]:
    load_env_file(ENV_FILE)
    host = cfg("BYETHOST_FTP_HOST", "ftpupload.net")
    port = int(cfg("BYETHOST_FTP_PORT", "21") or "21")
    user = cfg("BYETHOST_FTP_USER")
    password = cfg("BYETHOST_FTP_PASS")
    remote_root = cfg("BYETHOST_FTP_REMOTE_ROOT", "htdocs")
    if not user or not password:
        sys.exit(
            f"Missing FTP credentials. Create {ENV_FILE} from .env.byethost.example "
            "or set BYETHOST_FTP_USER / BYETHOST_FTP_PASS."
        )
    ftp = FTP()
    ftp.connect(host, port, timeout=120)
    ftp.login(user, password)
    ftp.set_pasv(True)
    ensure_cwd(ftp, remote_root)
    return ftp, remote_root


def list_remote(ftp: FTP) -> None:
    print(f"PWD {ftp.pwd()}")
    ftp.retrlines("LIST")


def _ftp_rm_tree(ftp: FTP, remote_root: str, rel: str) -> None:
    """Delete remote file or directory tree under remote_root/rel."""
    path = f"{remote_root}/{rel}".replace("//", "/")
    # Try as file first
    try:
        ensure_cwd(ftp, remote_root if "/" not in rel.rstrip("/") else f"{remote_root}/{'/'.join(rel.split('/')[:-1])}")
        name = rel.rstrip("/").split("/")[-1]
        ensure_cwd(ftp, str(Path(remote_root, *rel.split("/")[:-1]).as_posix()) if "/" in rel else remote_root)
        ftp.delete(name)
        print(f"  DEL {rel}")
        return
    except error_perm:
        pass

    # Directory: recurse
    try:
        ensure_cwd(ftp, f"{remote_root}/{rel}")
    except error_perm:
        print(f"  skip missing {rel}")
        ensure_cwd(ftp, remote_root)
        return

    entries: list[tuple[str, bool]] = []

    def _collect(line: str) -> None:
        parts = line.split(maxsplit=8)
        if len(parts) < 9:
            return
        name = parts[8]
        if name in (".", ".."):
            return
        is_dir = line.startswith("d")
        entries.append((name, is_dir))

    ftp.retrlines("LIST", _collect)
    for name, is_dir in entries:
        child = f"{rel}/{name}"
        if is_dir:
            _ftp_rm_tree(ftp, remote_root, child)
        else:
            ensure_cwd(ftp, f"{remote_root}/{rel}")
            try:
                ftp.delete(name)
                print(f"  DEL {child}")
            except error_perm as e:
                print(f"  DEL fail {child}: {e}")
    ensure_cwd(ftp, remote_root if "/" not in rel else f"{remote_root}/{'/'.join(rel.split('/')[:-1])}")
    try:
        # parent then rmdir leaf
        parent = "/".join(rel.split("/")[:-1])
        leaf = rel.split("/")[-1]
        ensure_cwd(ftp, f"{remote_root}/{parent}" if parent else remote_root)
        ftp.rmd(leaf)
        print(f"  RMD {rel}")
    except error_perm as e:
        print(f"  RMD fail {rel}: {e}")
    ensure_cwd(ftp, remote_root)


def purge_snapkeep(ftp: FTP, remote_root: str) -> None:
    print("Purging SnapKeep files from host:")
    ensure_cwd(ftp, remote_root)
    for name in SNAPKEEP_ROOT_FILES:
        try:
            ftp.delete(name)
            print(f"  DEL {name}")
        except error_perm:
            print(f"  skip {name}")
    for name in SNAPKEEP_ROOT_DIRS:
        _ftp_rm_tree(ftp, remote_root, name)
    ensure_cwd(ftp, remote_root)


def main() -> int:
    ap = argparse.ArgumentParser(description="Upload DeskTidy site to ByetHost")
    ap.add_argument("--purge-snapkeep", action="store_true", help="Remove SnapKeep SPA/PHP from host")
    ap.add_argument("--list", action="store_true", help="Only list remote htdocs")
    args = ap.parse_args()

    desktidy_html = DOCS / "desktidy.html"
    desknote_html = DOCS / "desknote.html"
    portal_html = DOCS / "index.html"
    if not portal_html.is_file():
        sys.exit(f"Missing HTML: {portal_html}")

    ftp, remote_root = connect()
    site = cfg("BYETHOST_SITE_URL", "https://bowen-yu0926.byethost10.com/")
    print(f"Connected → {remote_root}  site={site}")

    if args.list:
        list_remote(ftp)
        ftp.quit()
        return 0

    if args.purge_snapkeep:
        purge_snapkeep(ftp, remote_root)

    pages = [
        (portal_html, "index.html"),
        (desktidy_html, "desktidy.html"),
        (desknote_html, "desknote.html"),
    ]
    for local, _remote in pages:
        if not local.is_file():
            sys.exit(f"Missing HTML: {local}")

    print("Uploading DeskTidy pages:")
    for local, remote in pages:
        upload_file(ftp, local, remote)

    images_dir = DOCS / "images"
    if images_dir.is_dir():
        print("Uploading docs/images:")
        ensure_cwd(ftp, f"{remote_root}/images")
        for path in sorted(images_dir.glob("*")):
            if not path.is_file():
                continue
            # Replace broken/no-perm uploads: delete first when present.
            try:
                ftp.delete(path.name)
            except error_perm:
                pass
            upload_file(ftp, path, path.name)
        ensure_cwd(ftp, remote_root)

    print("Remote listing:")
    list_remote(ftp)
    ftp.quit()
    print(f"Done. Open: {site}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
