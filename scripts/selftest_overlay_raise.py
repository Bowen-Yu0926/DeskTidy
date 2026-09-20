"""Regression: fence/chrome band-raise must not cover normal apps.

Common paths that used to HWND_TOP overlays over WeChat/VS Code:
tray organize, Explorer→desktop drop refresh, keepalive, page chrome.
"""

from __future__ import annotations

import inspect
import sys
import traceback
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

passes: list[str] = []
failures: list[tuple[str, str]] = []


def run(name: str, fn) -> None:
    try:
        fn()
        passes.append(name)
        print(f"  OK  {name}")
    except Exception as exc:
        failures.append((name, f"{exc}\n{traceback.format_exc()}"))
        print(f"FAIL  {name}: {exc}")


def test_raise_owner_contracts() -> None:
    from src.app import DeskTidyApp
    from src.desktop_shell_host import raise_overlay_in_desktop_band
    from src.win_shell import configure_desktop_overlay

    live = inspect.getsource(DeskTidyApp.ensure_live_fences_interactive)
    assert "is_explorer_desktop_foreground" in live
    assert "skip_raise" in live
    assert "raise_overlay_in_desktop_band" in live
    # Gate must be Explorer-desktop only — not loose is_desktop_foreground
    # (tray/taskbar would still raise over apps).
    assert "is_desktop_foreground()" not in live
    assert live.index("is_explorer_desktop_foreground") < live.index(
        "raise_overlay_in_desktop_band"
    )

    raise_src = inspect.getsource(raise_overlay_in_desktop_band)
    assert "is_attached_to_desktop" in raise_src
    assert "HWND_TOP" in raise_src
    assert "SWP_NOOWNERZORDER" in raise_src
    assert raise_src.index("is_attached_to_desktop") < raise_src.index("SetWindowPos")

    chrome = inspect.getsource(DeskTidyApp._ensure_page_chrome_visible)
    assert "_foreign_app_owns_foreground" in chrome
    assert "raise_band = False" in chrome

    attach = inspect.getsource(DeskTidyApp._ensure_shell_attachments)
    assert "_foreign_app_owns_foreground" in attach
    assert "_sink_overlays_for_foreign_app" in attach

    sync = inspect.getsource(DeskTidyApp._sync_overlays_to_foreground)
    assert "is_explorer_desktop_foreground" in sync
    assert "_sink_overlays_for_foreign_app" in sync

    visible = inspect.getsource(DeskTidyApp._ensure_desktop_overlays_visible)
    assert "_foreign_app_owns_foreground" in visible
    assert "is_explorer_desktop_foreground" in visible

    cfg = inspect.getsource(configure_desktop_overlay)
    assert "raise_overlay_in_desktop_band" in cfg


def test_common_callers_go_through_gated_ensure_live() -> None:
    from src.app import DeskTidyApp

    org = inspect.getsource(DeskTidyApp._on_organize_requested)
    assert "refresh_fences(refresh_public=True)" in org

    impl = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "ensure_live_fences_interactive" in impl
    # Non-page-switch present path must re-assert live (and inherit FG gate).
    present_block = impl.split("host.present(force_attach=False)")[1][:400]
    assert "ensure_live_fences_interactive" in present_block

    flush = inspect.getsource(DeskTidyApp._flush_public_refresh_after_drag)
    assert "refresh_public_desktop" in flush

    end_drag = inspect.getsource(DeskTidyApp._end_overlay_drag)
    assert "_sync_overlays_to_foreground" in end_drag

    single = inspect.getsource(DeskTidyApp._ensure_single_public_float)
    assert "ensure_live_fences_interactive" in single

    pending = inspect.getsource(DeskTidyApp._run_pending_force_shell_attach)
    assert "ensure_live_fences_interactive" in pending
    assert "_foreign_app_owns_foreground" in pending
    assert "_overlay_capture_freeze_active" in pending
    # Capture freeze must cancel — not reschedule through snip teardown.
    freeze_branch = pending.split("if self._overlay_restack_blocked():", 1)[1]
    assert "if self._overlay_capture_freeze_active():" in freeze_branch
    assert freeze_branch.index("if self._overlay_capture_freeze_active():") < freeze_branch.index(
        "_schedule_force_shell_attach(150)"
    )

    restore = inspect.getsource(DeskTidyApp._restore_arriving_fences_interactive)
    assert "ensure_live_fences_interactive" in restore


def test_ensure_live_skips_raise_when_not_explorer_fg() -> None:
    """Tray / foreign / settings FG: opaque only — zero band raises."""
    from src.app import DeskTidyApp

    raised: list[int] = []

    fake_fence = SimpleNamespace(
        _desktidy_soft_parked=False,
        winId=lambda: 4242,
    )

    fake = object.__new__(DeskTidyApp)
    fake.fences = [fake_fence]
    fake.pet_widget = None
    fake._public_icon_host = None
    fake._fence_hwnd = lambda _f: 4242  # type: ignore[method-assign]
    fake._soft_show_fence_for_page = lambda _f: None  # type: ignore[method-assign]

    with (
        mock.patch(
            "src.win_shell.is_explorer_desktop_foreground", return_value=False
        ),
        mock.patch(
            "src.win_shell.set_overlay_mouse_passthrough",
            lambda *_a, **_k: None,
        ),
        mock.patch(
            "src.desktop_shell_host.raise_overlay_in_desktop_band",
            lambda hwnd: raised.append(int(hwnd)),
        ),
    ):
        DeskTidyApp.ensure_live_fences_interactive(fake)

    assert raised == [], f"expected no HWND_TOP, got {raised}"


def test_ensure_live_raises_only_when_explorer_fg() -> None:
    from src.app import DeskTidyApp

    raised: list[int] = []

    fake_fence = SimpleNamespace(
        _desktidy_soft_parked=False,
        winId=lambda: 5151,
        isVisible=lambda: True,
    )

    fake = object.__new__(DeskTidyApp)
    fake.fences = [fake_fence]
    fake.pet_widget = None
    fake._public_icon_host = None
    fake._fence_hwnd = lambda _f: 5151  # type: ignore[method-assign]
    fake._soft_show_fence_for_page = lambda _f: None  # type: ignore[method-assign]

    with (
        mock.patch(
            "src.win_shell.is_explorer_desktop_foreground", return_value=True
        ),
        mock.patch(
            "src.win_shell.set_overlay_mouse_passthrough",
            lambda *_a, **_k: None,
        ),
        mock.patch(
            "src.desktop_shell_host.raise_overlay_in_desktop_band",
            lambda hwnd: raised.append(int(hwnd)),
        ),
    ):
        DeskTidyApp.ensure_live_fences_interactive(fake)

    assert raised == [5151], raised


def test_raise_refuses_detached_hwnd() -> None:
    from src import desktop_shell_host as host

    calls: list[tuple] = []

    with (
        mock.patch.object(host, "is_attached_to_desktop", return_value=False),
        mock.patch.object(
            host.user32,
            "IsWindow",
            return_value=True,
        ),
        mock.patch.object(
            host.user32,
            "SetWindowPos",
            lambda *a, **k: calls.append(("SetWindowPos", a, k)),
        ),
    ):
        host.raise_overlay_in_desktop_band(999001)

    assert calls == [], "detached HWND must not SetWindowPos HWND_TOP"


def test_raise_attached_hwnd_uses_noownerzorder() -> None:
    from src import desktop_shell_host as host

    calls: list[tuple] = []

    def _swp(hwnd, insert_after, *_rest):
        calls.append((int(hwnd), int(insert_after)))
        return 1

    with (
        mock.patch.object(host, "is_attached_to_desktop", return_value=True),
        mock.patch.object(host.user32, "IsWindow", return_value=True),
        mock.patch.object(host.user32, "IsWindowVisible", return_value=True),
        mock.patch.object(host.user32, "SetWindowPos", _swp),
    ):
        host.raise_overlay_in_desktop_band(888002)

    assert calls == [(888002, 0)], calls  # HWND_TOP == 0
    # Flag check via source (SetWindowPos spy does not capture flags easily
    # across cdecl); contract already asserts SWP_NOOWNERZORDER in source.


def test_page_chrome_raises_cleared_on_foreign_fg() -> None:
    from src.app import DeskTidyApp

    seen: list[bool | None] = []

    tip = SimpleNamespace(
        isVisible=lambda: True,
        show=lambda: None,
        raise_=lambda: None,
        winId=lambda: 7001,
    )

    fake = object.__new__(DeskTidyApp)
    fake._page_chrome_may_show = lambda: True  # type: ignore[method-assign]
    fake._foreign_app_owns_foreground = lambda: True  # type: ignore[method-assign]
    fake.settings = {
        "show_page_indicator": True,
        "dock": {"enabled": False},
    }
    fake.page_indicator = tip
    fake.dock = None
    fake.todo_panel = None
    fake.pet_widget = None

    def _spy(widget, *, raise_band=None, **kwargs):
        seen.append(raise_band)
        return None

    with (
        mock.patch("src.win_shell.configure_desktop_overlay", _spy),
        # Not attached → must call configure (healthy+attached would skip).
        mock.patch(
            "src.desktop_shell_host.is_attached_to_desktop", return_value=False
        ),
        mock.patch("src.win_shell.overlay_win32_visible", return_value=False),
    ):
        DeskTidyApp._ensure_page_chrome_visible(fake, raise_band=True)

    assert seen, "expected configure_desktop_overlay"
    assert all(flag is False for flag in seen), seen


def main() -> int:
    print("DeskTidy overlay raise matrix")
    run("raise owner contracts", test_raise_owner_contracts)
    run("common callers gated", test_common_callers_go_through_gated_ensure_live)
    run("skip raise when not explorer FG", test_ensure_live_skips_raise_when_not_explorer_fg)
    run("raise when explorer FG", test_ensure_live_raises_only_when_explorer_fg)
    run("refuse detached HWND_TOP", test_raise_refuses_detached_hwnd)
    run("attached HWND_TOP", test_raise_attached_hwnd_uses_noownerzorder)
    run("page chrome foreign FG", test_page_chrome_raises_cleared_on_foreign_fg)
    print()
    print(f"通过 {len(passes)}  失败 {len(failures)}")
    for name, detail in failures:
        print(f"\n--- {name} ---\n{detail}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
