"""Self-test for runtime FFmpeg fetch (REQ-P2-1) — extraction only, no network.

Run:  workdir=E:\\Desktidy
      python scripts/selftest_ffmpeg_fetch.py

Covers:
  1. install_ffmpeg_from_zip extracts bin/ffmpeg.exe + DLLs, skips ffplay/ffprobe
     and non-bin entries (locally constructed mini zip — never touches the net).
  2. A zip with no bin/ffmpeg.exe raises FfmpegFetchError.
  3. fetch_in_flight() starts False; FFMPEG_CACHE_DIR points at ~/.desktidy/ffmpeg.
"""

from __future__ import annotations

import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def _make_zip(path: Path, entries: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, data in entries.items():
            zf.writestr(name, data)


def test_install_ffmpeg_from_zip() -> None:
    from src.ffmpeg_fetch import install_ffmpeg_from_zip

    with tempfile.TemporaryDirectory(prefix="desktidy_fftest_") as tmp:
        base = Path(tmp)
        zip_path = base / "ffmpeg.zip"
        dest = base / "out" / "ffmpeg"
        _make_zip(
            zip_path,
            {
                "ffmpeg-master-latest-win64-gpl-shared/bin/ffmpeg.exe": b"FAKE-EXE",
                "ffmpeg-master-latest-win64-gpl-shared/bin/avcodec-60.dll": b"FAKE-DLL",
                "ffmpeg-master-latest-win64-gpl-shared/bin/ffplay.exe": b"SKIP-ME",
                "ffmpeg-master-latest-win64-gpl-shared/bin/ffprobe.exe": b"SKIP-ME",
                "ffmpeg-master-latest-win64-gpl-shared/README.txt": b"SKIP-ME",
            },
        )

        exe = install_ffmpeg_from_zip(zip_path, dest)

        assert exe == dest / "ffmpeg.exe", exe
        assert (dest / "ffmpeg.exe").read_bytes() == b"FAKE-EXE"
        assert (dest / "avcodec-60.dll").read_bytes() == b"FAKE-DLL"
        assert not (dest / "ffplay.exe").exists()
        assert not (dest / "ffprobe.exe").exists()
        assert not (dest / "README.txt").exists()

    print("test_install_ffmpeg_from_zip OK")


def test_install_missing_bin_raises() -> None:
    from src.ffmpeg_fetch import FfmpegFetchError, install_ffmpeg_from_zip

    with tempfile.TemporaryDirectory(prefix="desktidy_fftest_") as tmp:
        base = Path(tmp)
        zip_path = base / "bad.zip"
        _make_zip(zip_path, {"docs/readme.txt": b"x", "bin/ffplay.exe": b"x"})
        try:
            install_ffmpeg_from_zip(zip_path, base / "out")
        except FfmpegFetchError:
            pass
        else:
            raise AssertionError("expected FfmpegFetchError for archive without bin/ffmpeg.exe")

    print("test_install_missing_bin_raises OK")


def test_constants_and_guard() -> None:
    from src.ffmpeg_fetch import FFMPEG_CACHE_DIR, fetch_in_flight

    assert FFMPEG_CACHE_DIR == Path.home() / ".desktidy" / "ffmpeg", FFMPEG_CACHE_DIR
    assert fetch_in_flight() is False

    print("test_constants_and_guard OK")


def main() -> int:
    test_install_ffmpeg_from_zip()
    test_install_missing_bin_raises()
    test_constants_and_guard()
    print("selftest_ffmpeg_fetch ALL OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
