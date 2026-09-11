"""Regression checks for bundled fd file search."""

from __future__ import annotations

import inspect
import json
import sys
import tempfile
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
    except Exception as exc:  # noqa: BLE001
        failures.append((name, str(exc) or repr(exc)))
        print(f"  FAIL {name}: {exc}")


def test_defaults_and_hotkey() -> None:
    data = json.loads((ROOT / "config" / "default_settings.json").read_text(encoding="utf-8"))
    assert "file_search" in data
    assert "user_profile" not in (data["file_search"].get("roots") or [])
    assert "enabled" not in data["file_search"]
    assert data["file_search"].get("max_results", 99) <= 60
    assert data["hotkeys"].get("file_search")
    from src.fd_search import MIN_QUERY_CHARS, resolve_search_roots
    from src.hotkey_manager import HOTKEY_IDS, HOTKEY_LABELS, _POLLABLE_ACTIONS

    assert "file_search" in HOTKEY_IDS
    assert "file_search" in _POLLABLE_ACTIONS
    assert HOTKEY_LABELS["file_search"] == "文件搜索"
    assert data["hotkeys"].get("file_search") == "F4"
    assert MIN_QUERY_CHARS >= 2
    roots = resolve_search_roots({"file_search": data["file_search"]})
    assert roots
    keys = [str(r).casefold() for r in roots]
    assert not any("appdata" in k for k in keys)


def test_locator_finds_bundled() -> None:
    from src.fd_locator import clear_fd_cache, find_fd

    clear_fd_cache()
    path = find_fd()
    assert path is not None
    assert path.name.lower() == "fd.exe"
    assert path.is_file()


def test_search_finds_temp_file() -> None:
    from src.fd_search import search_filenames

    td = Path(tempfile.mkdtemp(prefix="desktidy_fd_"))
    target = td / "DeskTidyFdProbeUniqueXYZ.txt"
    target.write_text("x", encoding="utf-8")
    try:
        hits = search_filenames(
            "DeskTidyFdProbeUniqueXYZ",
            settings={
                "file_search": {
                    "roots": [str(td)],
                    "max_results": 20,
                    "search_all_drives": False,
                    "include_open_folders": False,
                }
            },
        )
        assert any(p.name == target.name for p in hits), hits
        assert search_filenames(
            "a",
            settings={
                "file_search": {
                    "roots": [str(td)],
                    "include_open_folders": False,
                }
            },
        ) == []
    finally:
        try:
            target.unlink(missing_ok=True)
            td.rmdir()
        except OSError:
            pass


def test_open_explorer_and_global_depth() -> None:
    """Open Explorer folders join default roots; global depth must reach deep trees."""
    import inspect

    from src import fd_search as fds
    from src.fd_search import (
        DEFAULT_MAX_DEPTH,
        open_explorer_folder_paths,
        resolve_search_roots,
        search_filenames,
    )
    from src.ui.file_search_overlay import FileSearchOverlay

    assert DEFAULT_MAX_DEPTH >= 12
    src = inspect.getsource(fds.search_filenames)
    assert "DEFAULT_MAX_DEPTH" in src
    assert "open_explorer_folder_paths" in inspect.getsource(fds.resolve_search_roots)
    assert "已打开文件夹" in inspect.getsource(FileSearchOverlay._update_scope_label)

    td = Path(tempfile.mkdtemp(prefix="desktidy_fd_open_"))
    target = td / "销售退货探测唯一名.docx"
    target.write_bytes(b"x")
    try:
        real = open_explorer_folder_paths
        fds.open_explorer_folder_paths = lambda **_k: [td]  # type: ignore[assignment]
        try:
            roots = resolve_search_roots(
                {
                    "file_search": {
                        "roots": ["desktop"],
                        "search_all_drives": False,
                        "include_open_folders": True,
                    }
                }
            )
            assert any(p.resolve() == td.resolve() for p in roots), roots
            hits = search_filenames(
                "销售退货探测",
                settings={
                    "file_search": {
                        "roots": ["desktop"],
                        "search_all_drives": False,
                        "include_open_folders": True,
                        "max_results": 20,
                    }
                },
                timeout_sec=5.0,
            )
            assert any(p.name == target.name for p in hits), hits
        finally:
            fds.open_explorer_folder_paths = real  # type: ignore[assignment]
    finally:
        try:
            target.unlink(missing_ok=True)
            td.rmdir()
        except OSError:
            pass


def test_prune_and_cancel() -> None:
    from src.fd_search import cancel_active_searches, resolve_search_roots

    home = Path.home()
    desktop = home / "Desktop"
    roots = resolve_search_roots(
        {
            "file_search": {
                "roots": [str(home), str(desktop), str(desktop)],
                "search_all_drives": False,
                "include_open_folders": False,
            }
        }
    )
    assert len(roots) == 1
    cancel_active_searches()


def test_wiring_contracts() -> None:
    import sys

    from PyQt6.QtGui import QFont
    from PyQt6.QtWidgets import QApplication

    app = QApplication.instance() or QApplication(sys.argv[:1])

    from src.app import DeskTidyApp
    from src.hotkey_manager import HOTKEY_FALLBACKS, HOTKEY_IDS, _ACTION_BY_ID
    from src.tray import TrayManager
    from src.ui import extensions_widget as ew
    from src.ui.file_search_overlay import FileSearchOverlay

    _ = app

    assert _ACTION_BY_ID == {hid: name for name, hid in HOTKEY_IDS.items()}
    setup = inspect.getsource(DeskTidyApp._setup_hotkeys)
    assert "file_search" in setup
    assert "register_with_fallbacks" in setup
    assert "file_search" in HOTKEY_FALLBACKS
    assert "_toggle_file_search" in inspect.getsource(DeskTidyApp)
    removed = inspect.getsource(DeskTidyApp._on_watched_file_removed)
    assert "remove_public_paths(" not in removed
    assert "_remove_public_icon_widget(" not in removed
    assert "prune_missing_public_items" in removed
    assert "_schedule_sticky_missing_reprune" in removed
    assert "unpin_paths_from_all_fences" not in removed
    assert "prune_missing_virtual_items" not in removed
    assert "_schedule_pending_fence_remove" not in removed
    assert "_flush_pending_fence_removes" not in inspect.getsource(DeskTidyApp)
    assert "sticky" in removed.lower()
    assert "_run_sticky_missing_reprune" in inspect.getsource(DeskTidyApp)
    assert "_sticky_reprune_timer" in inspect.getsource(DeskTidyApp.__init__)
    assert "_maybe_reschedule_desktop_watcher" in inspect.getsource(DeskTidyApp)
    from src import app as app_mod

    assert hasattr(app_mod._UiBridge, "file_removed")
    assert "file_search" in inspect.getsource(ew.ExtensionsWidget._build_ui)
    assert "_file_search_action" not in inspect.getsource(TrayManager.__init__)
    assert "Enter" in FileSearchOverlay.__doc__ or "Key_Return" in inspect.getsource(
        FileSearchOverlay._handle_nav_key
    )
    overlay_src = inspect.getsource(FileSearchOverlay)
    overlay_mod = inspect.getsource(
        __import__("src.ui.file_search_overlay", fromlist=["x"])
    )
    assert "installEventFilter" in overlay_src
    assert "_SearchInputFilter" in overlay_mod
    assert "fileSearchCloseBtn" in overlay_src
    assert "close_overlay" in overlay_src
    assert "refresh_theme" in overlay_src
    assert "_repolish_tree" in overlay_src
    from src.ui.styles import build_stylesheet

    assert "fileSearchCard" in build_stylesheet("mint")
    assert "fileSearchGlobalBtn" in build_stylesheet("sky")
    assert "fileSearchGlobalCb" not in build_stylesheet("sky")
    assert "_DragTitleBar" in overlay_mod
    assert "grabMouse" in overlay_mod
    assert "fileSearchTitleBar" in build_stylesheet("mint")
    assert "_clamp_to_screen" in overlay_src
    assert "_saved_pos" in overlay_src
    assert "fileSearchLocation" in overlay_src
    assert "fileSearchCard" in overlay_src
    assert "fileSearchGlobalBtn" in overlay_src
    assert "_run_global_search" in overlay_src
    assert "fileSearchGlobalCb" not in overlay_src
    assert "_on_global_toggled" not in overlay_src
    assert "_try_force_digit_input" in overlay_src
    assert "_show_result_menu" in overlay_src
    assert "打开所在位置" in overlay_src
    assert "CustomContextMenu" in overlay_src
    assert "_reveal_item" in overlay_src
    assert "_result_row_text" in overlay_src
    row = FileSearchOverlay._result_row_text(
        Path(r"E:\demo\1 需求分析\销售退货.doc"),
        max_width=2000,
        font=QFont(),
    )
    assert row.startswith("销售退货.doc")
    assert "1 需求分析" in row
    assert "  ·  " in row
    assert "search_all_drives" in inspect.getsource(
        __import__("src.fd_search", fromlist=["x"]).search_config_slice
    )
    ext_src = inspect.getsource(ew.ExtensionsWidget._build_ui)
    assert "file_search_all_drives_cb" not in ext_src
    apply_theme = inspect.getsource(DeskTidyApp._apply_theme)
    assert "_file_search_overlay" in apply_theme
    assert "refresh_theme" in apply_theme
    iss = (ROOT / "installer" / "DeskTidy.iss").read_text(encoding="utf-8")
    assert r"dist\DeskTidy\*" in iss
    assert r"assets\ffmpeg\*" not in iss
    assert "nocompression" in iss
    assert 'DeskTidyCompress "lzma2/fast"' in iss or "lzma2/fast" in iss
    assert "SolidCompression=no" in iss
    assert 'Excludes: "logs\\*"' in iss or "Excludes: \"logs\\*\"" in iss or "logs\\*" in iss
    bat = (ROOT / "scripts" / "build_installer.bat").read_text(encoding="utf-8")
    assert r"assets\fd\fd.exe" in bat
    assert "DESKTIDY_SKIP_PIP" in bat
    assert "DESKTIDY_ALREADY_STOPPED" in bat
    build_bat = (ROOT / "scripts" / "build.bat").read_text(encoding="utf-8")
    assert r"assets\fd\fd.exe" in build_bat
    assert r"dist\DeskTidy\assets\fd" in build_bat
    assert "DESKTIDY_ALREADY_STOPPED" in build_bat
    assert "prune_release_payload.py" in build_bat
    assert (ROOT / "scripts" / "prune_release_payload.py").is_file()
    sys.path.insert(0, str(ROOT / "scripts"))
    from prune_release_payload import (  # type: ignore
        KEEP_LOCALES,
        _is_debug_asset,
        _is_unused_webengine_locale,
    )

    assert _is_debug_asset(Path("qtwebengine_resources.debug.pak"))
    assert _is_debug_asset(Path("qtwebengine_devtools_resources.pak"))
    assert not _is_debug_asset(Path("qtwebengine_resources.pak"))
    loc = Path("x") / "qtwebengine_locales" / "fr.pak"
    assert _is_unused_webengine_locale(loc)
    assert not _is_unused_webengine_locale(Path("x") / "qtwebengine_locales" / "zh-CN.pak")
    assert "zh-CN.pak" in KEEP_LOCALES
    assert "verify_ffmpeg_bundle" in bat
    assert "prepare_ffmpeg_recording" in bat
    from src import ffmpeg_bundle as fb

    assert "h264_mf" in fb.PREFERRED_ENCODER or fb.PREFERRED_ENCODER == "h264_mf"
    assert "ddagrab" in fb.PERF_DEMUXERS
    assert (ROOT / "scripts" / "prepare_ffmpeg_recording.py").is_file()


def test_desktop_root_and_cache_invalidation() -> None:
    from src.fd_search import (
        _expand_root_token,
        invalidate_search_roots_cache,
        resolve_search_roots,
    )
    from src.settings import get_desktop_paths, invalidate_desktop_paths_cache

    desks = [p for p in get_desktop_paths() if p.is_dir()]
    expanded = _expand_root_token("desktop")
    assert expanded
    if desks:
        assert {p.resolve() for p in expanded} == {p.resolve() for p in desks}
    for token in ("documents", "downloads"):
        paths = _expand_root_token(token)
        assert paths, token
        assert all(hasattr(p, "is_dir") for p in paths), paths

    # Warm roots cache, then invalidating desktop paths must clear it.
    resolve_search_roots({"file_search": {"roots": ["desktop"], "search_all_drives": False}})
    from src import fd_search as fds

    assert fds._roots_cache
    invalidate_desktop_paths_cache()
    assert not fds._roots_cache
    invalidate_search_roots_cache()


def test_watcher_delete_notifies_removed() -> None:
    from src.file_watcher import DesktopEventHandler

    created: list[str] = []
    removed: list[str] = []

    class _Ev:
        def __init__(self, src: str, dest: str = ""):
            self.src_path = src
            self.dest_path = dest
            self.is_directory = False

    handler = DesktopEventHandler(
        {"exclude_patterns": []},
        on_created_file=created.append,
        on_removed_file=removed.append,
    )
    gone = r"C:\Users\x\Desktop\ghost.txt"
    handler.on_deleted(_Ev(gone))
    assert removed and removed[-1].endswith("ghost.txt"), removed
    handler.on_moved(_Ev(r"C:\Users\x\Desktop\a.txt", r"D:\elsewhere\a.txt"))
    assert any(s.endswith("a.txt") for s in removed)
    assert any(s.endswith("a.txt") for s in created)


def test_prune_force_noop_does_not_throttle() -> None:
    import time
    from src.public_desktop import _prune_missing_cache, prune_missing_public_items

    settings = {"public_desktop_items": []}
    _prune_missing_cache["t"] = 0.0
    assert prune_missing_public_items(settings, force=True) is False
    # Force no-op must leave throttle open so a later real prune can run.
    assert float(_prune_missing_cache.get("t", 0.0)) == 0.0
    assert prune_missing_public_items(settings, force=False) is False
    stamped = float(_prune_missing_cache.get("t", 0.0))
    assert stamped > 0.0
    # Immediate non-force again is throttled.
    _prune_missing_cache["t"] = time.monotonic()
    assert prune_missing_public_items(settings, force=False) is False


def test_prune_missing_public_items_sticky() -> None:
    import time

    from src.public_desktop import (
        _missing_since,
        clear_public_missing_tracking,
        get_public_items,
        prune_missing_public_items,
    )

    ghost = r"C:\Users\x\Desktop\DeskTidyGhostFloatXYZ.txt"
    clear_public_missing_tracking()
    settings = {
        "public_desktop_items": [{"path": ghost, "x": 10, "y": 10}],
    }
    try:
        assert prune_missing_public_items(settings, force=True) is False
        items = get_public_items(settings)
        assert len(items) == 1
        assert str(Path(ghost)).casefold() in _missing_since

        assert (
            prune_missing_public_items(
                settings, force=True, sticky_missing_s=30.0
            )
            is False
        )
        assert len(get_public_items(settings)) == 1

        key = str(Path(ghost)).casefold()
        _missing_since[key] = time.monotonic() - 60.0
        assert (
            prune_missing_public_items(
                settings, force=True, sticky_missing_s=30.0
            )
            is True
        )
        assert get_public_items(settings) == []
    finally:
        clear_public_missing_tracking()


def test_unpin_and_prune_missing_virtual_items() -> None:
    import time

    from src.fence_rules import (
        _missing_since,
        clear_virtual_missing_tracking,
        prune_missing_virtual_items,
        unpin_paths_from_virtual_fence,
    )

    ghost = r"C:\Users\x\Desktop\DeskTidyGhostPinXYZ.txt"
    alive = Path(tempfile.mkdtemp(prefix="desktidy_pin_")) / "keep.txt"
    alive.write_text("ok", encoding="utf-8")
    clear_virtual_missing_tracking()
    try:
        settings = {
            "fences": [
                {"id": "a", "virtual_items": [ghost, str(alive)]},
                {"id": "b", "virtual_items": [ghost]},
            ]
        }
        assert unpin_paths_from_virtual_fence(settings["fences"][0], [Path(ghost)]) is True
        assert unpin_paths_from_virtual_fence(settings["fences"][1], [Path(ghost)]) is True
        assert settings["fences"][0]["virtual_items"] == [str(alive)]
        assert settings["fences"][1]["virtual_items"] == []

        # Sticky: first miss only stamps; pin stays.
        settings["fences"][0]["virtual_items"] = [ghost, str(alive)]
        clear_virtual_missing_tracking()
        assert prune_missing_virtual_items(settings, force=True) is False
        assert settings["fences"][0]["virtual_items"] == [ghost, str(alive)]
        assert str(Path(ghost)).casefold() in _missing_since

        # Still within sticky window — keep.
        assert (
            prune_missing_virtual_items(
                settings, force=True, sticky_missing_s=30.0
            )
            is False
        )
        assert settings["fences"][0]["virtual_items"] == [ghost, str(alive)]

        # Sustained absence — drop.
        key = str(Path(ghost)).casefold()
        _missing_since[key] = time.monotonic() - 60.0
        assert (
            prune_missing_virtual_items(
                settings, force=True, sticky_missing_s=30.0
            )
            is True
        )
        assert settings["fences"][0]["virtual_items"] == [str(alive)]
    finally:
        clear_virtual_missing_tracking()
        try:
            alive.unlink(missing_ok=True)
            alive.parent.rmdir()
        except OSError:
            pass


def test_office_save_keeps_sticky_public_float() -> None:
    """Watcher must not immediately scrub public floats (sticky prune owns it)."""
    import inspect
    from types import SimpleNamespace
    from unittest.mock import patch

    from src.app import DeskTidyApp
    from src.public_desktop import (
        clear_public_missing_tracking,
        get_public_items,
        prune_missing_public_items,
    )

    removed = inspect.getsource(DeskTidyApp._on_watched_file_removed)
    assert "remove_public_paths(" not in removed
    assert "_remove_public_icon_widget(" not in removed
    assert "prune_missing_public_items" in removed
    assert "_schedule_sticky_missing_reprune" in removed

    clear_public_missing_tracking()
    tmp_dir = Path(tempfile.mkdtemp(prefix="desktidy_pub_sticky_"))
    target = tmp_dir / "报告草稿.docx"
    target.write_bytes(b"old")
    settings = {
        "fences": [],
        "public_desktop_items": [
            {"path": str(target), "x": 120, "y": 240, "page": 0},
        ],
    }
    try:
        target.unlink()
        scheduled: list[int] = []
        fake = SimpleNamespace(
            settings=settings,
            _loose_sync_needed=False,
            _watch_debounce=type("T", (), {"start": lambda *_a, **_k: None})(),
            _schedule_sticky_missing_reprune=lambda: scheduled.append(1),
        )
        with (
            patch("src.app.invalidate_desktop_scan_cache"),
            patch("src.app.save_settings"),
            patch("src.path_stat_cache.invalidate_path_stat_cache"),
        ):
            DeskTidyApp._on_watched_file_removed(fake, str(target))
        assert scheduled == [1]
        items = get_public_items(settings)
        assert len(items) == 1, items
        assert int(items[0]["x"]) == 120
        assert int(items[0]["y"]) == 240
        # Still within sticky window after watcher prune pass.
        assert (
            prune_missing_public_items(
                settings, force=True, sticky_missing_s=45.0
            )
            is False
        )
        target.write_bytes(b"new")
        assert (
            prune_missing_public_items(
                settings, force=True, sticky_missing_s=45.0
            )
            is False
        )
        assert len(get_public_items(settings)) == 1
    finally:
        clear_public_missing_tracking()
        try:
            target.unlink(missing_ok=True)
            tmp_dir.rmdir()
        except OSError:
            pass


def test_office_save_keeps_sticky_fence_pin() -> None:
    """Watcher must not unpin; brief miss + recreate keeps virtual membership."""
    import inspect
    from types import SimpleNamespace
    from unittest.mock import patch

    from src.app import DeskTidyApp
    from src.fence_rules import clear_virtual_missing_tracking, prune_missing_virtual_items

    removed = inspect.getsource(DeskTidyApp._on_watched_file_removed)
    assert "unpin_paths_from_all_fences" not in removed
    assert "_schedule_pending_fence_remove" not in removed

    clear_virtual_missing_tracking()
    tmp_dir = Path(tempfile.mkdtemp(prefix="desktidy_office_"))
    target = tmp_dir / "微控增项梳理.xlsx"
    target.write_bytes(b"old")
    settings = {
        "fences": [{"id": "system_docs", "virtual_items": [str(target)]}],
        "public_desktop_items": [],
    }
    try:
        # Office save gap: missing briefly, then back — pin remains.
        target.unlink()
        assert prune_missing_virtual_items(settings, force=True, sticky_missing_s=45.0) is False
        assert settings["fences"][0]["virtual_items"] == [str(target)]
        target.write_bytes(b"new")
        assert prune_missing_virtual_items(settings, force=True, sticky_missing_s=45.0) is False
        assert settings["fences"][0]["virtual_items"] == [str(target)]

        # Watcher remove path only sticky-prunes public floats, not fence pins.
        fake = SimpleNamespace(
            settings=settings,
            _loose_sync_needed=False,
            _watch_debounce=type("T", (), {"start": lambda *_a, **_k: None})(),
            _schedule_sticky_missing_reprune=lambda: None,
        )
        with (
            patch("src.app.invalidate_desktop_scan_cache"),
            patch("src.app.save_settings"),
            patch("src.path_stat_cache.invalidate_path_stat_cache"),
            patch("src.public_desktop.prune_missing_public_items", return_value=False),
        ):
            DeskTidyApp._on_watched_file_removed(fake, str(target))
        assert settings["fences"][0]["virtual_items"] == [str(target)]
    finally:
        clear_virtual_missing_tracking()
        try:
            target.unlink(missing_ok=True)
            tmp_dir.rmdir()
        except OSError:
            pass


def test_watcher_fingerprint_helper() -> None:
    from src.file_watcher import desktop_watch_fingerprint
    from src.settings import get_desktop_paths

    fp = desktop_watch_fingerprint()
    desks = [p for p in get_desktop_paths() if p.is_dir()]
    assert len(fp) == len(desks)
    assert list(fp) == sorted(fp)


def test_digit_from_key_pyqt6() -> None:
    from PyQt6.QtCore import Qt

    from src.ui.file_search_overlay import FileSearchOverlay

    assert FileSearchOverlay._digit_from_key(int(Qt.Key.Key_0)) == "0"
    assert FileSearchOverlay._digit_from_key(int(Qt.Key.Key_9)) == "9"
    assert FileSearchOverlay._digit_from_key(99999) is None
    src = inspect.getsource(FileSearchOverlay._digit_from_key)
    assert "Qt.Key.Keypad0" not in src


def main() -> int:
    print("DeskTidy fd file-search regression")
    run("默认配置与热键", test_defaults_and_hotkey)
    run("locator 找到内置 fd", test_locator_finds_bundled)
    run("能搜到临时文件", test_search_finds_temp_file)
    run("已打开文件夹纳入搜索且全局深度足够", test_open_explorer_and_global_depth)
    run("根目录去重与取消安全", test_prune_and_cancel)
    run("app/扩展/托盘/安装脚本接线", test_wiring_contracts)
    run("数字键解析兼容 PyQt6", test_digit_from_key_pyqt6)
    run("desktop 根与缓存失效", test_desktop_root_and_cache_invalidation)
    run("监视器删除通知", test_watcher_delete_notifies_removed)
    run("force prune 空跑不掐死节流", test_prune_force_noop_does_not_throttle)
    run("unpin 与缺失钉选清理", test_unpin_and_prune_missing_virtual_items)
    run("粘性公共浮标保留Office保存", test_office_save_keeps_sticky_public_float)
    run("粘性钉选保留Office保存", test_office_save_keeps_sticky_fence_pin)
    run("监视器路径指纹", test_watcher_fingerprint_helper)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err}")
    try:
        from scripts.cleanup_selftest_artifacts import report_cleanup

        report_cleanup()
    except Exception as exc:
        print(f"测试残留清理失败: {exc}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
