"""Regression: update checker — version compare, release feed parse, wiring."""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

passes: list[str] = []
failures: list[tuple[str, str]] = []


def run(name: str, fn) -> None:
    try:
        fn()
        passes.append(name)
        print(f"  OK  {name}")
    except Exception as exc:
        failures.append((name, str(exc)))
        print(f"  FAIL {name}: {exc}")


def test_parse_version() -> None:
    from src.update_checker import is_newer, parse_version

    assert parse_version("v4.0.26") == (4, 0, 26)
    assert parse_version("4.0.25") == (4, 0, 25)
    assert parse_version("") == (0,)
    assert parse_version("4.0.26.1") == (4, 0, 26, 1)
    assert is_newer("4.0.26", "4.0.25")
    assert not is_newer("4.0.25", "4.0.25")
    assert not is_newer("4.0.24", "4.0.25")
    # Numeric compare — "4.0.9" < "4.0.10" (string compare would say otherwise).
    assert is_newer("4.0.10", "4.0.9")
    assert not is_newer("v4.0.25", "4.0.25")


def test_check_for_update_parses_feed() -> None:
    import src.update_checker as uc

    from src._version import __version__

    fake = {
        "tag_name": "v99.0.0",
        "html_url": "https://example.invalid/release",
        "body": "notes here",
    }
    orig = uc.fetch_latest_release
    uc.fetch_latest_release = lambda timeout=8.0: fake  # type: ignore[assignment]
    try:
        result = uc.check_for_update(timeout=1.0)
    finally:
        uc.fetch_latest_release = orig  # type: ignore[assignment]
    assert result.status == uc.STATUS_UPDATE_AVAILABLE
    assert result.has_update
    assert result.latest == "99.0.0"
    assert result.current == __version__
    assert result.html_url == "https://example.invalid/release"
    assert result.notes == "notes here"


def test_check_for_update_up_to_date() -> None:
    import src.update_checker as uc

    from src._version import __version__

    fake = {"tag_name": f"v{__version__}", "html_url": "", "body": ""}
    orig = uc.fetch_latest_release
    uc.fetch_latest_release = lambda timeout=8.0: fake  # type: ignore[assignment]
    try:
        result = uc.check_for_update(timeout=1.0)
    finally:
        uc.fetch_latest_release = orig  # type: ignore[assignment]
    assert result.status == uc.STATUS_UP_TO_DATE
    assert not result.has_update


def test_check_for_update_network_error() -> None:
    import src.update_checker as uc

    def boom(timeout=8.0):
        raise uc.UpdateCheckError("timeout")

    orig = uc.fetch_latest_release
    uc.fetch_latest_release = boom  # type: ignore[assignment]
    try:
        result = uc.check_for_update(timeout=1.0)
    finally:
        uc.fetch_latest_release = orig  # type: ignore[assignment]
    assert result.status == uc.STATUS_ERROR
    assert "timeout" in result.error


def test_defaults_keys() -> None:
    data = json.loads(
        (ROOT / "config" / "default_settings.json").read_text(encoding="utf-8")
    )
    assert data.get("check_update_on_startup") is True
    assert data.get("update_notified_tag") == ""


def test_wiring() -> None:
    import inspect

    import src.i18n as i18n
    import src.tray as tray
    from src.app import DeskTidyApp
    from src.ui.help_dialog import HelpHomePage
    from src.ui.main_window import MainWindow

    about_src = inspect.getsource(i18n.show_about)
    assert "检查更新" in about_src
    assert "check_update_interactive" in about_src

    tray_src = inspect.getsource(tray.TrayManager.__init__)
    assert "检查更新" in tray_src
    assert "_check_update" in inspect.getsource(tray.TrayManager)

    home_src = inspect.getsource(HelpHomePage)
    assert "检查更新" in home_src
    assert "_check_update" in home_src

    build = inspect.getsource(MainWindow._build_settings_page)
    assert "check_update_cb" in build
    assert "启动时检查更新" in build
    assert callable(getattr(MainWindow, "_on_check_update_changed", None))

    sched = inspect.getsource(DeskTidyApp._schedule_startup_deferred)
    assert "_startup_deferred_update_check" in sched
    deferred = inspect.getsource(DeskTidyApp._startup_deferred_update_check)
    assert "check_update_on_startup" in deferred
    assert "notify_update_on_startup" in deferred


def test_single_flight_guard() -> None:
    import src.update_checker as uc

    orig_inflight = uc._check_inflight
    uc._check_inflight = True
    try:
        # Async guard refuses a second check before touching Qt/toast.
        assert uc.check_for_update_async(lambda r: None) is False
    finally:
        uc._check_inflight = orig_inflight


def main() -> int:
    print("DeskTidy update checker regression")
    run("parse_version / is_newer", test_parse_version)
    run("check_for_update 新版本", test_check_for_update_parses_feed)
    run("check_for_update 已最新", test_check_for_update_up_to_date)
    run("check_for_update 网络错误", test_check_for_update_network_error)
    run("default_settings 键", test_defaults_keys)
    run("UI/启动接线", test_wiring)
    run("single-flight 防重入", test_single_flight_guard)

    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    if failures:
        for name, err in failures:
            print(f"  - {name}: {err}")
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
