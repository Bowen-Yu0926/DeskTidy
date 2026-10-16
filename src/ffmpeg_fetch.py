"""Runtime on-demand FFmpeg fetch for screen recording (Sprint 3 / REQ-P2-1).

When no bundled/system FFmpeg is available, this module downloads the same
BtbN win64 gpl-shared build the release pipeline uses (see
``scripts/prepare_ffmpeg_recording.py``), extracts only ``bin/ffmpeg.exe`` +
its runtime DLLs, validates the result with ``ffmpeg_bundle`` and installs it
under ``%USERPROFILE%\\.desktidy\\ffmpeg\\``.

Network and extraction run on a worker thread; every Qt touch (toast / dialog)
is marshalled back to the GUI thread via ``src.qt_main_thread.call_on_main_thread``.
"""

from __future__ import annotations

import shutil
import tempfile
import threading
import urllib.request
import zipfile
from pathlib import Path

from src.ffmpeg_locator import clear_ffmpeg_cache, find_ffmpeg

# Runtime cache dir — Path.home() avoids importing src.settings (no circular import).
FFMPEG_CACHE_DIR = Path.home() / ".desktidy" / "ffmpeg"

# Same source as the build-time bundle (BtbN gpl-shared win64 latest).
DEFAULT_URL = (
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/"
    "ffmpeg-master-latest-win64-gpl-shared.zip"
)

_SKIP_BIN_NAMES = ("ffplay.exe", "ffprobe.exe")

_fetch_lock = threading.Lock()
_fetch_in_flight = False


class FfmpegFetchError(RuntimeError):
    """Download / extraction / validation failure with a user-readable message."""


def install_ffmpeg_from_zip(zip_path: Path, dest_dir: Path) -> Path:
    """Extract ``bin/ffmpeg.exe`` + its DLLs from a BtbN zip into *dest_dir*.

    Skips ffplay/ffprobe and any non-``bin/`` entry. Returns the installed
    ``ffmpeg.exe`` path. Raises ``FfmpegFetchError`` when the archive holds no
    ``bin/ffmpeg.exe``.
    """
    dest = Path(dest_dir)
    dest.mkdir(parents=True, exist_ok=True)
    found_exe = False

    with zipfile.ZipFile(zip_path) as zf:
        members = [
            name
            for name in zf.namelist()
            if "/bin/" in name.replace("\\", "/") and not name.endswith("/")
        ]
        for name in members:
            base = Path(name.replace("\\", "/")).name
            if not base or base in _SKIP_BIN_NAMES:
                continue
            if base != "ffmpeg.exe" and not base.lower().endswith(".dll"):
                continue
            if base == "ffmpeg.exe":
                found_exe = True
            target = dest / base
            with zf.open(name) as src, target.open("wb") as out:
                shutil.copyfileobj(src, out)

    if not found_exe:
        raise FfmpegFetchError("压缩包中未找到 bin/ffmpeg.exe")
    return dest / "ffmpeg.exe"


def _download(url: str, dest: Path, *, on_progress=None) -> None:
    """Stream *url* to *dest*, reporting ``on_progress(downloaded, total_or_none)``."""
    request = urllib.request.Request(
        url, headers={"User-Agent": "DeskTidy-FFmpeg-Fetch/1.0"}
    )
    with urllib.request.urlopen(request, timeout=120) as resp:
        raw_total = resp.headers.get("Content-Length")
        total = int(raw_total) if raw_total and raw_total.isdigit() else None
        downloaded = 0
        dest.parent.mkdir(parents=True, exist_ok=True)
        with dest.open("wb") as out:
            while True:
                chunk = resp.read(1024 * 256)
                if not chunk:
                    break
                out.write(chunk)
                downloaded += len(chunk)
                if on_progress is not None:
                    try:
                        on_progress(downloaded, total)
                    except Exception:
                        pass


def fetch_ffmpeg(*, dest_dir: Path | None = None, url: str | None = None, on_progress=None) -> Path:
    """Download → extract → validate → install; return the ready ``ffmpeg.exe``.

    Raises ``FfmpegFetchError`` on any failure (network, extraction, or the
    ``validate_recording_ffmpeg`` gate). Never leaves a half-installed bundle:
    extraction happens in a staging dir that is only moved into place after
    validation passes.
    """
    from src.ffmpeg_bundle import validate_recording_ffmpeg

    dest = Path(dest_dir) if dest_dir is not None else FFMPEG_CACHE_DIR
    source_url = url or DEFAULT_URL
    staging = dest.with_name(dest.name + ".staging")

    if staging.exists():
        shutil.rmtree(staging, ignore_errors=True)

    try:
        with tempfile.TemporaryDirectory(prefix="desktidy-ffmpeg-") as tmp:
            zip_path = Path(tmp) / "ffmpeg.zip"
            _download(source_url, zip_path, on_progress=on_progress)
            staged_exe = install_ffmpeg_from_zip(zip_path, staging)

        report = validate_recording_ffmpeg(staged_exe, bundle_dir=staging)
        if not report.ok:
            detail = "; ".join(report.errors) or "unknown validation failure"
            shutil.rmtree(staging, ignore_errors=True)
            raise FfmpegFetchError(f"下载的 FFmpeg 校验失败：{detail}")

        if dest.exists():
            shutil.rmtree(dest, ignore_errors=True)
        try:
            staging.rename(dest)
        except OSError:
            shutil.copytree(staging, dest)
            shutil.rmtree(staging, ignore_errors=True)

        clear_ffmpeg_cache()
        return dest / "ffmpeg.exe"
    except FfmpegFetchError:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    except Exception as exc:
        shutil.rmtree(staging, ignore_errors=True)
        raise FfmpegFetchError(f"FFmpeg 下载或安装失败：{exc}") from exc


def _toast_main(title: str, body: str, *, level: str = "info", msec: int = 4000) -> None:
    """Post a toast onto the GUI thread (safe from a worker)."""
    from src.qt_main_thread import call_on_main_thread

    def _show() -> None:
        try:
            from src.ui.toast import show_toast

            show_toast(title, body, msec=msec, level=level)
        except Exception:
            pass

    try:
        call_on_main_thread(_show)
    except Exception:
        pass


def _ask_yes_no_safe(parent, title: str, text: str) -> bool:
    """``ask_yes_no`` that survives a destroyed parent widget."""
    from src.i18n import ask_yes_no

    for target in (parent, None):
        try:
            return ask_yes_no(target, title, text)
        except Exception:
            continue
    return False


def _start_background_fetch(*, parent, on_ready) -> None:
    """Kick off a worker-thread fetch and report progress/result on the GUI thread."""
    global _fetch_in_flight

    from src.qt_main_thread import call_on_main_thread, ensure_main_thread_bridge

    ensure_main_thread_bridge()
    with _fetch_lock:
        if _fetch_in_flight:
            return
        _fetch_in_flight = True

    _toast_main("录屏", "正在下载 FFmpeg（约 80 MB）…", msec=5000)

    def _worker() -> None:
        global _fetch_in_flight
        exe = None
        error: BaseException | None = None
        try:
            exe = fetch_ffmpeg()
        except Exception as exc:  # noqa: BLE001 — deliver as user-facing failure
            error = exc
        finally:
            _fetch_in_flight = False

        if exe is not None:

            def _success() -> None:
                _toast_main("FFmpeg 已就绪", "现在可以开始录屏了。", level="success", msec=3200)
                if callable(on_ready):
                    try:
                        on_ready(exe)
                    except Exception:
                        pass

            try:
                call_on_main_thread(_success)
            except Exception:
                pass
        else:
            detail = str(error) if error is not None else "未知错误"

            def _fail() -> None:
                from src.i18n import show_info

                try:
                    show_info(
                        parent,
                        "下载 FFmpeg 失败",
                        f"{detail}\n\n建议手动安装：winget install Gyan.FFmpeg",
                    )
                except Exception:
                    pass

            try:
                call_on_main_thread(_fail)
            except Exception:
                pass

    threading.Thread(target=_worker, name="DeskTidyFFmpegFetch", daemon=True).start()


def fetch_in_flight() -> bool:
    """True while a background FFmpeg download is running (dedupe / UX gate)."""
    return _fetch_in_flight


def ensure_ffmpeg(parent=None, settings=None, *, on_ready=None) -> Path | None:
    """Return a usable FFmpeg exe, prompting to download when none is found.

    - Already present → return the path immediately (no prompt).
    - User declines, or a fetch is already running → ``None``.
    - User accepts → start a background download and return ``None``; the result
      is reported via toast / dialog and ``on_ready(exe)`` fires on the GUI
      thread on success. Failures degrade gracefully (never raise).
    """
    exe = find_ffmpeg()
    if exe is not None:
        return exe

    global _fetch_in_flight
    if _fetch_in_flight:
        return None

    if not _ask_yes_no_safe(
        parent, "录屏需要 FFmpeg", "未找到 FFmpeg，是否自动下载（约 80 MB）？"
    ):
        return None

    _start_background_fetch(parent=parent, on_ready=on_ready)
    return None
