"""Fail packaging when bundled FFmpeg lacks recording performance features."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from src.ffmpeg_bundle import format_validation_report, validate_recording_ffmpeg


def main() -> int:
    exe = ROOT / "assets" / "ffmpeg" / "ffmpeg.exe"
    report = validate_recording_ffmpeg(exe, bundle_dir=exe.parent)
    print(format_validation_report(report, exe=exe))
    if not report.ok:
        return 1
    if report.preferred_encoder != "h264_mf":
        print(
            "ERROR: h264_mf probe failed — install gpl-shared via "
            "scripts/prepare_ffmpeg_recording.py (libx264-only hurts CPU).",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
