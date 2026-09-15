#!/usr/bin/env python3
"""Upload server/desktidy-vault to ByetHost htdocs/desktidy-vault via FTP.

Uses Desktidy/.env.byethost (same as upload_byethost.py).
Never uploads config.local.php (create/upload secrets yourself).

  python scripts/upload_vault_api.py
"""

from __future__ import annotations

import os
import sys
from ftplib import FTP, error_perm
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENV_FILE = ROOT / ".env.byethost"
LOCAL_DIR = ROOT / "server" / "desktidy-vault"
REMOTE_DIR = "desktidy-vault"


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


def should_skip(path: Path) -> bool:
    if path.name == "config.local.php":
        return True
    if path.name == "__pycache__" or path.suffix == ".pyc":
        return True
    return False


def upload_tree(ftp: FTP, local: Path, remote_rel: str) -> int:
    count = 0
    for path in sorted(local.rglob("*")):
        if should_skip(path):
            continue
        if any(should_skip(Path(p)) for p in path.parts):
            continue
        rel = path.relative_to(local).as_posix()
        if path.is_dir():
            ensure_cwd(ftp, f"htdocs/{remote_rel}/{rel}")
            continue
        parent = path.parent.relative_to(local).as_posix()
        remote_parent = f"htdocs/{remote_rel}" + (f"/{parent}" if parent != "." else "")
        ensure_cwd(ftp, remote_parent)
        print(f"  STOR {rel}")
        with path.open("rb") as fh:
            ftp.storbinary(f"STOR {path.name}", fh)
        count += 1
    return count


def main() -> int:
    load_env_file(ENV_FILE)
    host = cfg("BYETHOST_FTP_HOST", "ftpupload.net")
    user = cfg("BYETHOST_FTP_USER")
    password = cfg("BYETHOST_FTP_PASS")
    port = int(cfg("BYETHOST_FTP_PORT", "21") or "21")
    if not user or not password:
        print("Missing BYETHOST_FTP_USER / BYETHOST_FTP_PASS in .env.byethost", file=sys.stderr)
        return 1
    if not LOCAL_DIR.is_dir():
        print(f"Missing {LOCAL_DIR}", file=sys.stderr)
        return 1
    print(f"FTP {host}:{port} as {user}")
    ftp = FTP()
    ftp.connect(host, port, timeout=60)
    ftp.login(user, password)
    try:
        n = upload_tree(ftp, LOCAL_DIR, REMOTE_DIR)
    finally:
        try:
            ftp.quit()
        except Exception:
            ftp.close()
    print(f"Uploaded {n} files to htdocs/{REMOTE_DIR}/")
    print("Next: create MySQL DB, import schema.sql, place config.local.php on server.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
