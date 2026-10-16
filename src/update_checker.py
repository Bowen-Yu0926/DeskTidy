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


def _ask_update_action(parent, latest: str, current: str) -> str:
    """Three-way「发现新版本」prompt; returns ``open`` / ``install`` / ``cancel``."""
    from PyQt6.QtCore import Qt
    from PyQt6.QtWidgets import QMessageBox

    box = QMessageBox(parent)
    box.setIcon(QMessageBox.Icon.Information)
    box.setWindowTitle("发现新版本")
    box.setText(f"新版本 {latest} 已发布（当前 {current}）。")
    box.setInformativeText(
        "可打开下载页手动下载，或由 DeskTidy 下载安装包、校验 sha256 后静默安装。"
    )
    open_btn = box.addButton("打开下载页", QMessageBox.ButtonRole.ActionRole)
    install_btn = box.addButton("下载并安装", QMessageBox.ButtonRole.AcceptRole)
    box.addButton("取消", QMessageBox.ButtonRole.RejectRole)
    box.setDefaultButton(install_btn)
    box.setWindowModality(Qt.WindowModality.ApplicationModal)
    box.setWindowFlag(Qt.WindowType.WindowStaysOnTopHint, True)
    box.raise_()
    box.activateWindow()
    box.exec()
    clicked = box.clickedButton()
    if clicked is install_btn:
        return "install"
    if clicked is open_btn:
        return "open"
    return "cancel"


def _ask_update_action_safe(parent, latest: str, current: str) -> str:
    """``_ask_update_action`` that survives a destroyed parent widget."""
    for target in (parent, None):
        try:
            return _ask_update_action(target, latest, current)
        except Exception:
            continue
    return "cancel"


_install_inflight = False


def _installer_download_dir() -> Path:
    from src.settings import APP_DIR, ensure_app_dir

    ensure_app_dir()
    target = APP_DIR / "updates"
    target.mkdir(parents=True, exist_ok=True)
    return target


def _report_toast(title: str, body: str, *, level: str = "info", msec: int = 4000) -> None:
    from src.qt_main_thread import call_on_main_thread
    from src.ui.toast import show_toast

    try:
        call_on_main_thread(lambda: show_toast(title, body, msec=msec, level=level))
    except Exception:
        pass


def download_and_install(parent=None, *, current: str = "") -> bool:
    """Download the latest installer in a worker thread, verify it, then prompt.

    Returns False when another download/install is already in flight. Never
    touches the network on the GUI thread.
    """
    global _install_inflight
    if _install_inflight:
        return False
    from src.qt_main_thread import ensure_main_thread_bridge

    ensure_main_thread_bridge()
    _install_inflight = True

    def worker() -> None:
        global _install_inflight
        try:
            _do_download_and_install(parent, current)
        finally:
            _install_inflight = False

    try:
        threading.Thread(
            target=worker, name="desktidy-update-install", daemon=True
        ).start()
    except Exception:
        _install_inflight = False
        return False
    return True


def _do_download_and_install(parent, current: str) -> None:
    from src import update_installer

    try:
        release = fetch_latest_release(timeout=15.0)
    except UpdateCheckError as exc:
        _report_toast("下载失败", f"获取更新信息失败：{exc}", level="warn")
        return

    asset = update_installer.select_installer_asset(release)
    if asset is None:
        _report_toast("下载失败", "该版本未提供 Windows 安装包。", level="warn")
        return

    checksum = update_installer.find_checksum(release, asset["name"])

    try:
        dest = _installer_download_dir()
    except Exception as exc:
        _report_toast("下载失败", f"无法准备下载目录：{exc}", level="warn")
        return
    try:
        update_installer.cleanup_old_installers(dest)
    except Exception:
        pass

    _report_toast("正在下载", f"正在下载 {asset['name']} …", msec=4000)
    last_milestone = -1

    def progress(done: int, total: int) -> None:
        nonlocal last_milestone
        if total and total > 0:
            pct = int(done * 100 / total)
            milestone = pct - (pct % 10)
            if milestone != last_milestone:
                last_milestone = milestone
                _report_toast("正在下载", f"已下载 {pct}%", msec=2000)

    try:
        path = update_installer.download_asset(
            asset["url"], dest, progress=progress, timeout=120.0
        )
    except update_installer.UpdateInstallError as exc:
        _report_toast("下载失败", f"下载安装包失败：{exc}", level="warn", msec=5000)
        return

    if not checksum:
        # No published checksum — refuse auto-install rather than run unverified code.
        try:
            path.unlink(missing_ok=True)
        except OSError:
            pass
        _report_toast(
            "无法校验",
            "该版本未提供 sha256 校验值，已取消自动安装。请打开下载页手动安装。",
            level="warn",
            msec=6000,
        )
        return

    try:
        ok = update_installer.verify_sha256(path, checksum)
    except Exception:
        ok = False
    if not ok:
        try:
            path.unlink(missing_ok=True)
        except OSError:
            pass
        _report_toast(
            "校验失败",
            "安装包 sha256 校验未通过，已拒绝安装并删除文件。",
            level="warn",
            msec=6000,
        )
        return

    from src.qt_main_thread import call_on_main_thread

    try:
        call_on_main_thread(lambda: _prompt_install(parent, path))
    except Exception:
        pass


def _prompt_install(parent, path: Path) -> None:
    from src import update_installer
    from src.ui.toast import show_toast

    if not Path(path).exists():
        show_toast("安装失败", "安装包已丢失，请重新下载。", msec=4500, level="warn")
        return
    if _ask_yes_no_safe(
        parent,
        "下载完成",
        "安装包已下载并通过校验。\n\n"
        "点击「是」将关闭 DeskTidy 并开始静默安装。是否立即安装？",
    ):
        try:
            update_installer.launch_installer(path, silent=True, wait=False)
        except update_installer.UpdateInstallError as exc:
            show_toast("安装失败", f"启动安装程序失败：{exc}", msec=5000, level="warn")
            return
        _quit_app_for_install()


def _quit_app_for_install() -> None:
    from PyQt6.QtWidgets import QApplication

    app = QApplication.instance()
    desk = getattr(app, "_desktidy_app", None) if app else None
    if desk is not None and hasattr(desk, "quit"):
        try:
            desk.quit(silent=True)
            return
        except Exception:
            pass
    if app is not None:
        app.quit()


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
    action = _ask_update_action_safe(parent, result.latest, result.current)
    if action == "install":
        download_and_install(parent, current=result.current)
    elif action == "open":
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
        action = _ask_update_action_safe(parent, result.latest, result.current)
        if action == "install":
            download_and_install(parent, current=result.current)
        elif action == "open":
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
