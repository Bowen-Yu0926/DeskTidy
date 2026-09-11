"""Download and install a recording-sized FFmpeg bundle (BtbN gpl-shared).

Keeps performance-critical pieces DeskTidy uses:
  ddagrab, gdigrab, lavfi, h264_mf (HW), libx264 (fallback), MP4 mux.

Default artifact: win64 gpl-shared (~70–90 MB installed vs ~210 MB static Gyan).

Usage:
  python scripts/prepare_ffmpeg_recording.py
  python scripts/prepare_ffmpeg_recording.py --url <zip-url>
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

DEFAULT_URL = (
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/"
    "ffmpeg-master-latest-win64-gpl-shared.zip"
)
TARGET_DIR = ROOT / "assets" / "ffmpeg"
NOTICE = """DeskTidy screen-recording runtime (FFmpeg gpl-shared).

Source: BtbN/FFmpeg-Builds (win64 gpl-shared)
License: GPL v3 — https://www.gnu.org/licenses/gpl-3.0.html

DeskTidy invokes ffmpeg.exe only for desktop capture (gdigrab/ddagrab)
and H.264 encoding (h264_mf / libx264). Replaced via:
  python scripts/prepare_ffmpeg_recording.py
"""


def _download(url: str, dest: Path) -> None:
    print(f"Downloading {url}")
    with urllib.request.urlopen(url, timeout=600) as resp, dest.open("wb") as out:
        shutil.copyfileobj(resp, out)
    print(f"  saved {dest.stat().st_size / (1024 * 1024):.1f} MB")


def _extract_bin(zip_path: Path, dest: Path) -> None:
    """Install ffmpeg.exe + runtime DLLs only (skip ffplay/ffprobe)."""
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(zip_path) as zf:
        bin_members = [n for n in zf.namelist() if "/bin/" in n.replace("\\", "/") and not n.endswith("/")]
        if not bin_members:
            raise RuntimeError("no bin/ entries in archive")
        for name in bin_members:
            base = Path(name.replace("\\", "/")).name
            if not base or base in ("ffplay.exe", "ffprobe.exe"):
                continue
            if base != "ffmpeg.exe" and not base.lower().endswith(".dll"):
                continue
            target = dest / base
            with zf.open(name) as src, target.open("wb") as out:
                shutil.copyfileobj(src, out)


def main() -> int:
    parser = argparse.ArgumentParser(description="Install recording FFmpeg into assets/ffmpeg")
    parser.add_argument("--url", default=DEFAULT_URL, help="gpl-shared zip URL")
    parser.add_argument("--keep-existing", action="store_true", help="skip if validate passes")
    args = parser.parse_args()

    from src.ffmpeg_bundle import format_validation_report, validate_recording_ffmpeg

    exe = TARGET_DIR / "ffmpeg.exe"
    if args.keep_existing and exe.is_file():
        report = validate_recording_ffmpeg(exe, bundle_dir=TARGET_DIR)
        if report.ok and report.preferred_encoder == "h264_mf":
            print(format_validation_report(report, exe=exe))
            print("Existing bundle OK — nothing to do.")
            return 0

    staging = TARGET_DIR.with_name("ffmpeg.staging")
    if staging.exists():
        shutil.rmtree(staging, ignore_errors=True)

    with tempfile.TemporaryDirectory(prefix="desktidy-ffmpeg-") as tmp:
        zip_path = Path(tmp) / "ffmpeg.zip"
        _download(args.url, zip_path)
        _extract_bin(zip_path, staging)

    staged_exe = staging / "ffmpeg.exe"
    if not staged_exe.is_file():
        print("ERROR: staged ffmpeg.exe missing", file=sys.stderr)
        return 1

    report = validate_recording_ffmpeg(staged_exe, bundle_dir=staging)
    print(format_validation_report(report, exe=staged_exe))
    if not report.ok:
        print("ERROR: staged FFmpeg failed validation — not installing.", file=sys.stderr)
        shutil.rmtree(staging, ignore_errors=True)
        return 1

    if TARGET_DIR.exists():
        shutil.rmtree(TARGET_DIR, ignore_errors=True)
    try:
        staging.rename(TARGET_DIR)
    except OSError:
        shutil.copytree(staging, TARGET_DIR)
        shutil.rmtree(staging, ignore_errors=True)
    (TARGET_DIR / "NOTICE.txt").write_text(NOTICE, encoding="utf-8")

    final = validate_recording_ffmpeg(TARGET_DIR / "ffmpeg.exe", bundle_dir=TARGET_DIR)
    print("\nInstalled:")
    print(format_validation_report(final, exe=TARGET_DIR / "ffmpeg.exe"))
    return 0 if final.ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
