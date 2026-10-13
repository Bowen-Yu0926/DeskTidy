"""Check GitHub Releases for a newer DeskTidy build.

Sync: ``check_for_update`` (network, blocking) — call only from workers.
Async: ``check_for_update_async`` delivers on the GUI thread via
``call_on_main_thread``. Interactive entry points toast while checking and
prompt to open the release page when a newer tag exists.
"""

from __future__ import annotations

import json
import threading
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

RELEASES_LATEST_URL = (
    "https://api.github.com/repos/Bowen-Yu0926/DeskTidy-Releases/releases/latest"
)
RELEASES_PAGE_URL = "https://github.com/Bowen-Yu0926/DeskTidy-Releases/releases/latest"
_USER_AGENT = "DeskTidy-Updater"
_CACHE_FILE_NAME = "update_check_cache.json"
# Unauthenticated GitHub API is 60 requests/hour per IP, so a shared corporate
# NAT would burn it. Startup checks reuse a fresh result instead of re-fetching.
_CACHE_TTL_SECONDS = 6 * 3600

STATUS_UP_TO_DATE = "up-to-date"
STATUS_UPDATE_AVAILABLE = "update-available"
STATUS_ERROR = "error"


class UpdateCheckError(RuntimeError):
    """Network / API failure while checking for updates."""


@dataclass(frozen=True)
class UpdateCheckResult:
    status: str  # STATUS_* constants
    current: str
    latest: str = ""
    html_url: str = ""
    notes: str = ""
    error: str = ""

    @property
    def has_update(self) -> bool:
        return self.status == STATUS_UPDATE_AVAILABLE


def parse_version(raw: str) -> tuple[int, ...]:
    """``v4.0.26`` / ``4.0.26`` / ``4.0.26.1`` -> numeric tuple for compare.

    ``-rc1`` / ``+build`` metadata is dropped first, otherwise ``4.0.26-rc1``
    would parse as ``(4, 0, 261)`` and a pre-release tag would be reported as
    a newer release than the one it derives from.
    """
    text = str(raw or "").strip().lstrip("vV").split("-", 1)[0].split("+", 1)[0]
    parts: list[int] = []
    for piece in text.split("."):
        # Empty segment (trailing dot / "4..28") is malformed, not a 0 field —
        # treating it as 0 made "4.0.28." parse longer than "4.0.28" and report
        # a bogus upgrade (BUG-04).
        if not piece:
            continue
        digits = "".join(ch for ch in piece if ch.isdigit())
        parts.append(int(digits) if digits else 0)
    if not parts:
        return (0,)
    return tuple(parts)


def is_newer(latest: str, current: str) -> bool:
    """True when *latest* is strictly newer than *current* (numeric compare)."""
    return parse_version(latest) > parse_version(current)


def fetch_latest_release(timeout: float = 8.0) -> dict:
    """GET the latest public release JSON from GitHub.

    Raises ``UpdateCheckError`` on network / HTTP / payload failures.
    """
    from src._version import __version__

    req = urllib.request.Request(
        RELEASES_LATEST_URL,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": f"{_USER_AGENT}/{__version__}",
        },
        method="GET",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        raise UpdateCheckError(f"HTTP {exc.code}") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise UpdateCheckError(str(exc) or exc.__class__.__name__) from exc
    try:
        data = json.loads(raw)
    except ValueError as exc:
        raise UpdateCheckError("invalid JSON from release feed") from exc
    if not isinstance(data, dict):
        raise UpdateCheckError("unexpected release feed payload")
    return data


def _cache_path() -> Path | None:
    try:
        from src.settings import APP_DIR, ensure_app_dir

        ensure_app_dir()
        return APP_DIR / _CACHE_FILE_NAME
    except Exception:
        return None


def _read_cache(now: float) -> UpdateCheckResult | None:
    """Fresh cached result, or None when stale / unreadable."""
    path = _cache_path()
    if path is None:
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            return None
        if float(data.get("checked_at") or 0) + _CACHE_TTL_SECONDS < now:
            return None
        result = UpdateCheckResult(
            status=str(data.get("status") or STATUS_ERROR),
            current=str(data.get("current") or ""),
            latest=str(data.get("latest") or ""),
            html_url=str(data.get("html_url") or ""),
            notes=str(data.get("notes") or ""),
            error=str(data.get("error") or ""),
        )
        # Cache was written under a previous run's version (F1): after an
        # in-TTL upgrade the stale "update-available" must not survive.
        from src._version import __version__

        if result.current != __version__:
            return None
        return result
    except Exception:
        return None


def _write_cache(result: UpdateCheckResult) -> None:
    """Remember a successful result; errors stay uncached so a retry happens."""
    path = _cache_path()
    if path is None or result.status == STATUS_ERROR:
        return
    try:
        path.write_text(
            json.dumps(
                {
                    "checked_at": time.time(),
                    "status": result.status,
                    "current": result.current,
                    "latest": result.latest,
                    "html_url": result.html_url,
                    "notes": result.notes[:4000],
                    "error": result.error,
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )
    except Exception:
        pass


def check_for_update(timeout: float = 8.0) -> UpdateCheckResult:
    """Blocking check — run in a worker thread, never on the GUI thread."""
    from src._version import __version__

    current = __version__
    try:
        data = fetch_latest_release(timeout=timeout)
    except UpdateCheckError as exc:
        return UpdateCheckResult(status=STATUS_ERROR, current=current, error=str(exc))

    tag = str(data.get("tag_name") or "").strip()
    latest = tag.lstrip("vV")
    html_url = str(data.get("html_url") or "").strip() or RELEASES_PAGE_URL
    notes = str(data.get("body") or "")
    if not latest:
        return UpdateCheckResult(
            status=STATUS_ERROR, current=current, error="release feed missing tag"
        )
    if is_newer(latest, current):
        return UpdateCheckResult(
            status=STATUS_UPDATE_AVAILABLE,
            current=current,
            latest=latest,
            html_url=html_url,
            notes=notes,
        )
    return UpdateCheckResult(
        status=STATUS_UP_TO_DATE, current=current, latest=latest, html_url=html_url
    )


_check_inflight = False


def check_for_update_async(
    on_result: Callable[[UpdateCheckResult], None],
    *,
    timeout: float = 8.0,
    force: bool = False,
) -> bool:
    """Background check; *on_result* runs on the GUI thread.

    ``force=True`` bypasses the disk cache (manual entry points). Returns False
    when a check is already in flight, or when the worker thread cannot start.
    """
    global _check_inflight
    if _check_inflight:
        return False
    from src.qt_main_thread import call_on_main_thread, ensure_main_thread_bridge

    ensure_main_thread_bridge()
    _check_inflight = True

    def worker() -> None:
        global _check_inflight
        try:
            result = _fetch_or_cached(timeout, force=force)
        except Exception as exc:  # noqa: BLE001 — defensive; deliver as error
            from src._version import __version__

            result = UpdateCheckResult(
                status=STATUS_ERROR, current=__version__, error=str(exc)
            )
        finally:
            _check_inflight = False
        try:
            call_on_main_thread(lambda: on_result(result))
        except Exception:
            pass

    try:
        threading.Thread(
            target=worker, name="desktidy-update-check", daemon=True
        ).start()
    except Exception:
        # Without this reset the guard would stay True forever and every later
        # check — including the startup check — would silently no-op.
        _check_inflight = False
        return False
    return True


def _fetch_or_cached(timeout: float, *, force: bool) -> UpdateCheckResult:
    """Serve a fresh cached result, else fetch live and cache the success."""
    result = _read_cache(time.time()) if not force else None
    if result is None:
        result = check_for_update(timeout=timeout)
        _write_cache(result)
    return result


def open_download_page(url: str | None = None) -> bool:
    """Open the release page (or given URL) in the system browser."""
    import webbrowser

    target = str(url or "").strip() or RELEASES_PAGE_URL
    try:
        return bool(webbrowser.open(target))
    except Exception:  # noqa: BLE001 — browser launch is best-effort
        return False


def _ask_yes_no_safe(parent, title: str, text: str) -> bool:
    """``ask_yes_no`` that survives a destroyed parent widget.

    The result callback can fire after its parent dialog was closed. QMessageBox
    on a deleted C++ parent raises, which would abort before
    ``update_notified_tag`` is persisted — so the user gets nagged again next
    launch with no way to see what the check found.
    """
    from src.i18n import ask_yes_no

    for target in (parent, None):
        try:
            return ask_yes_no(target, title, text)
        except Exception:
            continue
    return False


def _finish_interactive(
    parent,
    result: UpdateCheckResult,
    settings: dict | None,
    silent_up_to_date: bool,
) -> None:
    from src.ui.toast import show_toast

    if result.status == STATUS_ERROR:
        show_toast(
            "检查更新失败",
            result.error or "网络不可用",
            msec=4500,
            level="warn",
        )
        return
    if not result.has_update:
        if not silent_up_to_date:
            show_toast(
                "检查更新",
                f"已是最新版本（{result.current}）",
                msec=3000,
                level="success",
            )
        return
    if _ask_yes_no_safe(
        parent,
        "发现新版本",
        f"新版本 {result.latest} 已发布（当前 {result.current}）。\n\n"
        "是否打开下载页？",
    ):
        open_download_page(result.html_url)
    if settings is not None:
        # Prompt once per tag — startup check must not nag after a manual check.
        settings["update_notified_tag"] = f"v{result.latest}"
        try:
            from src.settings import save_settings

            save_settings(settings)
        except OSError:
            pass


def check_update_interactive(
    parent=None,
    *,
    settings: dict | None = None,
    silent_up_to_date: bool = False,
) -> bool:
    """Manual entry: toast while checking, then report / prompt.

    Returns False when a check is already running.
    """
    from src.ui.toast import show_toast

    started = check_for_update_async(
        lambda r: _finish_interactive(parent, r, settings, silent_up_to_date),
        force=True,  # manual click should not be answered from the startup cache
    )
    if not started:
        show_toast("检查更新", "正在检查中，请稍候…", msec=2500)
        return False
    show_toast("检查更新", "正在检查…", msec=2500)
    return True


def notify_update_on_startup(parent=None, *, settings: dict | None = None) -> bool:
    """Startup check: silent on failure / no update; prompt once per new tag."""

    def done(result: UpdateCheckResult) -> None:
        if not result.has_update:
            return
        tag = f"v{result.latest}"
        if settings is not None and str(settings.get("update_notified_tag") or "") == tag:
            return
        opened = _ask_yes_no_safe(
            parent,
            "发现新版本",
            f"新版本 {result.latest} 已发布（当前 {result.current}）。\n\n"
            "是否打开下载页？",
        )
        if opened:
            open_download_page(result.html_url)
        if settings is not None:
            # Prompt once per tag — declined or opened, do not nag next launch.
            settings["update_notified_tag"] = tag
            try:
                from src.settings import save_settings

                save_settings(settings)
            except OSError:
                pass

    return check_for_update_async(done)
