"""Session-regression: blank RMB extras gone, keyboard gate, dissolve header, capture_screen."""

from __future__ import annotations

import inspect
import os
import sys
import tempfile
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

passes: list[str] = []
failures: list[tuple[str, str]] = []


def run(name: str, fn) -> None:
    try:
        fn()
        passes.append(name)
        print(f"  OK  {name}")
    except Exception as exc:
        failures.append((name, f"{exc}\n{traceback.format_exc()}"))
        print(f"  FAIL {name}: {exc}")


def test_blank_menu_clean() -> None:
    from src.ui.fence_widget import FenceWidget

    src = inspect.getsource(FenceWidget._show_fence_context_menu)
    assert "_fence_shell_extra_commands" in src
    assert "图标放大" not in src
    assert "刷新分区" not in src
    assert "排序：" not in src
    extra = inspect.getsource(FenceWidget._fence_shell_extra_commands)
    assert "解散分区" in extra


def test_dissolve_header_emit() -> None:
    from src.ui.fence_widget import FenceWidget

    hdr = inspect.getsource(FenceWidget._on_header_context_menu)
    assert "解散分区" in hdr
    assert "dissolve_requested.emit" in hdr
    wire = inspect.getsource(FenceWidget._wire_fence_context_menus)
    assert "_on_header_context_menu" in wire
    assert "_on_chrome_context_menu" in wire


def test_keyboard_gate_perf_and_safety() -> None:
    from src.ui import fence_icon_item as fii

    resolve = inspect.getsource(fii.resolve_desktop_key_anchor)
    assert "is_desktop_foreground" in resolve
    assert "_fence_under_cursor" in resolve
    assert "_public_under_cursor" in resolve
    pub = inspect.getsource(fii._public_under_cursor)
    assert "hit_test_region" in pub
    ll = inspect.getsource(fii._on_ll_desktop_icon_key)
    assert "_LL_VK_CTRL_CHORDS" in ll
    assert "_ll_should_claim_chord_hook" in ll
    assert "resolve_desktop_key_anchor" not in ll
    # Early VK reject must happen before ctrl_physically_down.
    assert ll.find("vk not in _LL_VK_CTRL_CHORDS") < ll.find("ctrl_physically_down")


def test_clipboard_and_dispatch() -> None:
    from unittest.mock import patch

    from PyQt6.QtWidgets import QApplication, QWidget

    from src.ui.fence_icon_item import (
        dispatch_desktop_icon_chord,
        selected_paths_from_anchor,
    )

    _ = QApplication.instance() or QApplication([])
    tmp = Path(tempfile.mkdtemp(prefix="desktidy_sess_"))
    a = tmp / "a.txt"
    a.write_text("x", encoding="utf-8")

    class _Icon(QWidget):
        def __init__(self) -> None:
            super().__init__()
            self.file_path = a

        def is_selected(self) -> bool:
            return True

    icon = _Icon()
    assert selected_paths_from_anchor(icon) == [a]
    with patch("src.shell_clipboard.clipboard_set_files", return_value=True) as mocked:
        assert dispatch_desktop_icon_chord("copy", icon)
        mocked.assert_called()
        args, kwargs = mocked.call_args
        assert [Path(p) for p in args[0]] == [a]
        assert kwargs.get("cut") is False


def test_capture_screen_wired() -> None:
    import inspect

    from src.screen_record_manager import (
        ScreenRecordManager,
        capture_screen_choices,
        normalize_capture_screen,
        resolve_capture_region,
    )
    from src.ui.record_screen_picker import RecordScreenPickerDialog, pick_capture_screen

    assert normalize_capture_screen("primary") == "primary"
    assert normalize_capture_screen("extended") == "extended"
    assert normalize_capture_screen("current") == "current"
    labels = {v for v, _ in capture_screen_choices()}
    assert "primary" in labels
    assert labels <= {"primary", "extended"}
    assert resolve_capture_region("all") is None or isinstance(
        resolve_capture_region("all"), dict
    )
    ext = resolve_capture_region("extended")
    assert ext is None or isinstance(ext, dict)
    # Screen choice is a start-time picker, not an extensions setting.
    assert callable(pick_capture_screen)
    assert "选择要录制的屏幕" in inspect.getsource(RecordScreenPickerDialog)
    start = inspect.getsource(ScreenRecordManager.start_recording)
    assert "pick_capture_screen" in start
    from src.ui.extensions_widget import ExtensionsWidget

    ext = inspect.getsource(ExtensionsWidget._build_ui)
    assert "screen_record_capture_combo" not in ext
    assert "开始时会先选择" in ext


def test_ll_hook_install() -> None:
    from PyQt6.QtWidgets import QApplication

    from src.desktop_ll_keyboard import ll_keyboard_hook_active, shutdown_ll_keyboard
    from src.ui.fence_icon_item import ensure_desktop_icon_key_hook

    _ = QApplication.instance() or QApplication([])
    ensure_desktop_icon_key_hook()
    assert ll_keyboard_hook_active()
    shutdown_ll_keyboard()


def test_market_aligned_shell_contracts() -> None:
    """Core desktop verbs stay on shell APIs; known divergences are documented."""
    import inspect

    from src import desktop_ll_keyboard as llk
    from src import shell_clipboard as clip
    from src import shell_file_menu as sfm
    from src.help_content import help_html
    from src.ui.desktop_layout_widget import DesktopLayoutWidget
    from src.ui.main_window import _PAGE_SUBTITLES

    # Match: file / folder-background menus are real IContextMenu.
    assert "GetUIObjectOf" in inspect.getsource(sfm.show_file_context_menu) or (
        "GetUIObjectOf" in inspect.getsource(sfm)
    )
    assert "CreateViewObject" in inspect.getsource(sfm.show_folder_background_menu)
    assert "CF_HDROP" in inspect.getsource(clip) or clip.CF_HDROP == 15

    # Documented intentional divergence: DeskTidy pages ≠ Win virtual desktops.
    assert "虚拟桌面" in help_html("pages")
    assert "虚拟桌面" in (_PAGE_SUBTITLES.get("fences") or "")
    layout_src = inspect.getsource(DesktopLayoutWidget._build_ui)
    assert "虚拟桌面" in layout_src
    assert "_make_page_card_widget" in inspect.getsource(DesktopLayoutWidget)
    assert "_make_fence_card_widget" in inspect.getsource(DesktopLayoutWidget)
    assert "_toggle_fence_view_mode" in inspect.getsource(DesktopLayoutWidget)
    assert "page_view" in layout_src and "fence_view" in layout_src
    ctx_src = inspect.getsource(DesktopLayoutWidget._on_fence_context_menu)
    assert "切换为" in ctx_src
    assert "移动到分页" in ctx_src
    toggle_src = inspect.getsource(DesktopLayoutWidget._toggle_fence_view_mode)
    assert 'style["view_mode"]' in toggle_src or "view_mode" in toggle_src
    assert "fences_changed.emit" in toggle_src

    # Documented intentional divergence: LL keyboard for NOACTIVATE overlays.
    assert "DefView" in (llk.__doc__ or "") or "ListView" in (llk.__doc__ or "")
    assert "not stolen" in (llk.__doc__ or "") or "抢" in help_html("fences")


def main() -> int:
    print("DeskTidy session regression")
    started = time.perf_counter()
    run("空白右键无 DeskTidy 杂项", test_blank_menu_clean)
    run("标题栏可解散分区", test_dissolve_header_emit)
    run("快捷键闸门与性能契约", test_keyboard_gate_perf_and_safety)
    run("复制 dispatch", test_clipboard_and_dispatch)
    run("录屏 capture_screen 接线", test_capture_screen_wired)
    run("LL 键盘钩子安装", test_ll_hook_install)
    run("市场对齐契约", test_market_aligned_shell_contracts)
    elapsed = time.perf_counter() - started
    print(f"\n通过 {len(passes)}  失败 {len(failures)}  耗时 {elapsed:.2f}s")
    for name, err in failures:
        print(f"  - {name}: {err}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
