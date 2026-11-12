#!/usr/bin/env python3
"""Deploy DeskTidy intro pages to Cloudflare Pages (static HTML).

Production vault API stays at https://chromhome.kdns.fr/api/ — that host's
web root is Personal StarNav and catch-alls HTML routes, so product pages
must live on a **separate Pages project** (custom domain or *.pages.dev).

Credentials: Desktidy/.env.cloudflare (gitignored) — see .env.cloudflare.example.

Examples:
  python scripts/upload_cloudflare_pages.py
  python scripts/upload_cloudflare_pages.py --dry-run
  python scripts/upload_cloudflare_pages.py --project desktidy-site
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENV_FILE = ROOT / ".env.cloudflare"
DOCS = ROOT / "docs"
PAGES = ("index.html", "desktidy.html", "desknote.html")


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


def stage_pages(dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    missing = [n for n in PAGES if not (DOCS / n).is_file()]
    if missing:
        raise SystemExit(f"missing docs pages: {', '.join(missing)}")
    for name in PAGES:
        shutil.copy2(DOCS / name, dest / name)
        print(f"  stage {name}  ({(DOCS / name).stat().st_size} bytes)")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Upload DeskTidy intro HTML to Cloudflare Pages"
    )
    parser.add_argument(
        "--project",
        default="",
        help="Pages project name (default: CF_PAGES_PROJECT or desktidy-site)",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Stage files and print wrangler command only",
    )
    args = parser.parse_args(argv)

    load_env_file(ENV_FILE)
    project = args.project or cfg("CF_PAGES_PROJECT", "desktidy-site")
    token = cfg("CLOUDFLARE_API_TOKEN")
    account = cfg("CLOUDFLARE_ACCOUNT_ID")
    site = cfg("CF_SITE_URL", f"https://{project}.pages.dev/")

    print("Cloudflare Pages deploy (DeskTidy intro)")
    print(f"  project={project}")
    print(f"  site_url={site}")
    if not token and not args.dry_run:
        print(
            "Missing CLOUDFLARE_API_TOKEN.\n"
            "  1) Copy .env.cloudflare.example → .env.cloudflare\n"
            "  2) Paste an API token with Cloudflare Pages Edit\n"
            "  3) Re-run this script",
            file=sys.stderr,
        )
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="desktidy-pages-"))
    try:
        stage_pages(tmp)
        cmd = [
            "npx",
            "--yes",
            "wrangler",
            "pages",
            "deploy",
            str(tmp),
            "--project-name",
            project,
            "--commit-dirty=true",
        ]
        env = os.environ.copy()
        if token:
            env["CLOUDFLARE_API_TOKEN"] = token
        if account:
            env["CLOUDFLARE_ACCOUNT_ID"] = account

        printable = " ".join(cmd)
        print(f"\n>> {printable}")
        if args.dry_run:
            print("(dry-run) skip wrangler")
            return 0

        # Windows: npx.cmd via shell so PATH resolution matches interactive use.
        completed = subprocess.run(
            printable if os.name == "nt" else cmd,
            cwd=ROOT,
            env=env,
            shell=(os.name == "nt"),
            check=False,
        )
        if completed.returncode != 0:
            return completed.returncode or 1
        print(f"\nDeployed. Open: {site}")
        print("  Tip: bind a custom host (e.g. desktidy.chromhome.kdns.fr) in")
        print("  Pages → Custom domains — keep chromhome.kdns.fr/ for StarNav.")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
