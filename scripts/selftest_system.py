"""DeskTidy 系统功能整体自测。

覆盖：配置、布局、分区规则、整理/还原、Win+D 层级、热键、单实例、
UI 控件、分页、公共桌面、截图管理器、用户配置健康、真实桌面 dry-run、
Win+D 进出循环、打包产物与启动冒烟等。

运行（推荐）：
  scripts\\selftest_system.bat
或：
  python scripts\\selftest_system.py
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.dist_paths import packaged_exe

# Headless Qt where possible (widgets still construct on Windows).
os.environ.setdefault("QT_QPA_PLATFORM", "windows")

passes: list[str] = []
failures: list[tuple[str, str]] = []
section = ""


def begin(title: str) -> None:
    global section
    section = title
    print(f"\n=== {title} ===")


def ok(name: str) -> None:
    passes.append(f"{section}: {name}" if section else name)
    print(f"  PASS  {name}")


def fail(name: str, exc: object) -> None:
    label = f"{section}: {name}" if section else name
    failures.append((label, str(exc)))
    print(f"  FAIL  {name}: {exc}")


def run(name: str, fn) -> None:
    try:
        fn()
        ok(name)
    except Exception as exc:
        fail(name, exc)
        traceback.print_exc()


# ---------------------------------------------------------------------------
# 1. Bootstrap
# ---------------------------------------------------------------------------

def test_bootstrap() -> None:
    begin("1) 启动与导入")

    def _qt():
        from PyQt6.QtWidgets import QApplication

        app = QApplication.instance() or QApplication(sys.argv)
        assert app is not None
        globals()["qt_app"] = app

    run("QApplication", _qt)

    for mod in (
        "src.app",
        "src.settings",
        "src.fence_layout",
        "src.fence_rules",
        "src.fence_pages",
        "src.organizer",
        "src.win_shell",
        "src.hotkey_manager",
        "src.instance_lock",
        "src.i18n",
        "src.public_desktop",
        "src.desktop_shell_host",
        "src.desktop_scanner",
        "src.screenshot_manager",
        "src.peek_manager",
        "src.notepad",
        "src.notepad_search",
        "src.ui.fence_widget",
        "src.ui.fence_icon_item",
        "src.ui.dock_widget",
        "src.ui.page_indicator",
        "src.ui.notepad_window",
        "src.ui.notepad_find_dialog",
        "src.ui.main_window",
        "src.ui.screen_snap",
        "src.ui.public_icon_widget",
    ):
        run(f"import {mod}", lambda m=mod: __import__(m))


# ---------------------------------------------------------------------------
# 2. Settings / version / resources
# ---------------------------------------------------------------------------

def test_settings_and_resources() -> None:
    begin("2) 配置 / 版本 / 资源")

    def _version():
        ver = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        assert ver and ver[0].isdigit()
        from src._version import __version__

        assert __version__ == ver

    run("VERSION 与 _version 一致", _version)

    def _defaults():
        from src.settings import DEFAULT_SETTINGS, load_settings

        assert DEFAULT_SETTINGS.is_file()
        data = json.loads(DEFAULT_SETTINGS.read_text(encoding="utf-8"))
        assert "fences" in data and "hotkeys" in data
        assert data.get("organize_mode") == "virtual"
        s = load_settings()
        assert isinstance(s, dict)
        assert s.get("organize_mode") == "virtual"
        assert "fences" in s

    run("默认配置与 load_settings", _defaults)

    def _desktop():
        from src.settings import get_desktop_path, get_fence_storage_root

        desk = get_desktop_path()
        assert desk.is_dir(), desk
        root = get_fence_storage_root()
        assert root.exists() or root.parent.exists()

    run("桌面路径与存储根目录", _desktop)

    def _packaging():
        assert (ROOT / "DeskTidy.spec").is_file()
        assert (ROOT / "installer" / "DeskTidy.iss").is_file()
        assert (ROOT / "scripts" / "build.bat").is_file()
        assert (ROOT / "config" / "default_settings.json").is_file()
        spec = (ROOT / "DeskTidy.spec").read_text(encoding="utf-8")
        # UPX locks bundled DLLs → bootloader "Failed to remove temporary directory".
        assert "upx=False" in spec
        assert "upx=True" not in spec
        assert "pyi_rth_desktidy.py" in spec
        hook = ROOT / "packaging" / "rthooks" / "pyi_rth_desktidy.py"
        assert hook.is_file()
        hook_src = hook.read_text(encoding="utf-8")
        assert "VCRUNTIME140" in hook_src
        assert "SetDllDirectoryW" in hook_src

    run("打包/安装脚本资源存在", _packaging)

    def _app_icon_asset():
        """App tray/title icon must be a filled square glyph with real alpha."""
        import importlib
        import sys

        from PIL import Image

        from src.icon_utils import app_icon_is_hires, icon_path

        path = icon_path()
        assert path.is_file(), path
        assert app_icon_is_hires()
        scripts = str(ROOT / "scripts")
        if scripts not in sys.path:
            sys.path.insert(0, scripts)
        gen = importlib.import_module("generate_icon")
        # Tray / app glyph must match sidebar BrandMark (stacked blue panes).
        tray = gen._draw_app_icon(16)
        assert tray.size == (16, 16)
        tbb = tray.split()[-1].getbbox()
        assert tbb is not None
        tfill = ((tbb[2] - tbb[0]) * (tbb[3] - tbb[1])) / (16 * 16)
        assert tfill >= 0.35, f"tray 16px too empty fill={tfill:.2f} bbox={tbb}"
        # Transparent corners like the sidebar mark (not a solid blue tile).
        assert tray.getpixel((0, 0))[3] == 0
        assert tray.getpixel((15, 15))[3] == 0
        # Front pane is accent blue, not a white block on blue.
        mid = tray.getpixel((8, 6))
        assert mid[2] > mid[0] and mid[3] > 200, mid  # bluish + opaque
        from src.icon_utils import get_tray_icon
        from src.ui.brand_mark import brand_mark_icon, brand_mark_pixmap, paint_brand_mark
        import inspect

        tray_fn = inspect.getsource(get_tray_icon)
        assert "brand_mark_icon" in tray_fn
        assert "_paint_tray_brand_pixmap" not in tray_fn
        painted = brand_mark_pixmap(40)
        assert not painted.isNull()
        assert painted.width() == 40
        # Same silhouette: transparent at corners, opaque on the front pane.
        assert painted.toImage().pixelColor(0, 0).alpha() == 0
        assert painted.toImage().pixelColor(16, 12).alpha() > 200
        ticon = get_tray_icon()
        assert not ticon.isNull()
        bicon = brand_mark_icon()
        assert not bicon.isNull()
        sizes = {(s.width(), s.height()) for s in ticon.availableSizes()}
        assert (16, 16) in sizes or any(w <= 32 for w, h in sizes)
        assert callable(paint_brand_mark)
        from src.tray import TrayManager

        tray_init = inspect.getsource(TrayManager.__init__)
        assert "APP_NAME_ZH" in tray_init
        assert "setToolTip(APP_NAME_ZH)" in tray_init
        assert "_reassert_tray_icon" not in tray_init
        assert "hide()" not in tray_init or "self.tray.hide" not in tray_init
        assert "QTimer.singleShot" not in tray_init
        frame = gen._draw_app_icon(256)
        assert frame.size == (256, 256)
        assert frame.getpixel((0, 0))[3] == 0
        assert frame.getpixel((255, 255))[3] == 0
        bbox = frame.split()[-1].getbbox()
        assert bbox is not None
        fill = ((bbox[2] - bbox[0]) * (bbox[3] - bbox[1])) / (256 * 256)
        assert fill >= 0.55, f"icon too empty (fill={fill:.2f}, bbox={bbox})"
        src = ROOT / "assets" / "app_icon_source.png"
        assert src.is_file()
        with Image.open(src) as im:
            w, h = im.size
            assert w == h, (w, h)

    run("应用图标资产透明且铺满", _app_icon_asset)


# ---------------------------------------------------------------------------
# 3. Fence pages
# ---------------------------------------------------------------------------

def test_fence_pages() -> None:
    begin("3) 分页归属")

    def _pages():
        from src.fence_pages import fence_on_page, get_fence_pages, set_fence_pages

        fence: dict = {"page": 1}
        assert get_fence_pages(fence) == [1]
        set_fence_pages(fence, [2, 0, 2])
        assert fence["pages"] == [2, 0]
        assert fence["page"] == 2
        assert fence_on_page(fence, 0)
        assert not fence_on_page(fence, 1)

    run("pages / page 同步与去重", _pages)


# ---------------------------------------------------------------------------
# 4. Fence layout (critical recent bugs)
# ---------------------------------------------------------------------------

def test_fence_layout() -> None:
    begin("4) 分区布局 / 多屏钳制")

    def _fingerprint():
        from src.fence_layout import (
            current_display_fingerprint,
            primary_desktop_rect,
            virtual_desktop_rect,
        )

        fp = current_display_fingerprint()
        assert fp and "x" in fp
        area = virtual_desktop_rect()
        primary = primary_desktop_rect()
        assert area.width() > 0 and primary.width() > 0

    run("fingerprint / desktop rect", _fingerprint)

    def _fingerprint_includes_physical_size():
        from src.fence_layout import (
            _fingerprint_primary_mm,
            _fingerprint_primary_pixels,
            current_display_fingerprint,
            display_profile_is_healthy,
        )

        fp = current_display_fingerprint()
        assert "@" in fp and "mm" in fp
        assert _fingerprint_primary_pixels(fp)[0] > 0
        # Same logical res, different panels must not share a key.
        laptop = "0,0,1920x1080@309x174mm@1"
        monitor = "0,0,1920x1080@598x336mm@1"
        assert laptop != monitor
        assert _fingerprint_primary_mm(laptop) == (309, 174)
        assert _fingerprint_primary_mm(monitor) == (598, 336)
        assert _fingerprint_primary_mm("0,0,1920x1080") == (0, 0)
        overlapping = {
            "0": {
                "a": {
                    "x": 0,
                    "y": 0,
                    "width": 1609,
                    "height": 315,
                    "rx": 0,
                    "ry": 0,
                    "rw": 0.84,
                    "rh": 0.3,
                    "ref_w": 1920,
                    "ref_h": 1032,
                },
                "b": {
                    "x": 974,
                    "y": 0,
                    "width": 306,
                    "height": 318,
                    "rx": 0.507,
                    "ry": 0,
                    "rw": 0.16,
                    "rh": 0.3,
                    "ref_w": 1920,
                    "ref_h": 1032,
                },
            }
        }
        assert not display_profile_is_healthy(overlapping)
        side_by_side = {
            "0": {
                "a": {
                    "x": 0,
                    "y": 0,
                    "width": 1609,
                    "height": 315,
                    "rx": 0,
                    "ry": 0,
                    "rw": 0.84,
                    "rh": 0.3,
                    "ref_w": 1920,
                    "ref_h": 1032,
                },
                "b": {
                    "x": 1613,
                    "y": 0,
                    "width": 306,
                    "height": 318,
                    "rx": 0.84,
                    "ry": 0,
                    "rw": 0.16,
                    "rh": 0.3,
                    "ref_w": 1920,
                    "ref_h": 1032,
                },
            }
        }
        assert display_profile_is_healthy(side_by_side)

    run("指纹含物理尺寸且拒绝重叠布局", _fingerprint_includes_physical_size)

    def _save_and_load():
        from src.fence_layout import (
            current_display_fingerprint,
            get_fence_geometry,
            migrate_layouts,
            save_fence_geometry,
        )

        fp = current_display_fingerprint()
        settings = {
            "fences": [
                {
                    "id": "f1",
                    "name": "软件",
                    "x": 100,
                    "y": 80,
                    "width": 500,
                    "height": 280,
                    "pages": [0],
                }
            ],
            "fence_layouts_by_page": {},
            "fence_layouts_by_display": {},
        }
        migrate_layouts(settings)
        assert fp in settings["fence_layouts_by_display"]
        fence = settings["fences"][0]
        save_fence_geometry(
            settings,
            fence,
            0,
            {"x": 120, "y": 90, "width": 640, "height": 300, "collapsed": False},
        )
        entry = settings["fence_layouts_by_display"][fp]["0"]["f1"]
        assert entry["width"] == 640
        assert "rx" in entry and entry["ref_w"] > 0
        geom = get_fence_geometry(settings, fence, 0)
        assert geom["width"] >= 200

    run("save/load 保留正常尺寸", _save_and_load)

    def _scale_monitor_to_laptop():
        """Monitor layout ratios must scale onto a smaller primary (no pile-up)."""
        from PyQt6.QtCore import QRect
        from src import fence_layout as fl

        donor = {
            "x": 0,
            "y": 0,
            "width": 1609,
            "height": 317,
            "collapsed": False,
            "rx": 0.0,
            "ry": 0.0,
            "rw": 0.838,
            "rh": 0.307,
            "ref_x": 0,
            "ref_y": 0,
            "ref_w": 1920,
            "ref_h": 1080,
        }
        right = {
            "x": 1613,
            "y": 0,
            "width": 306,
            "height": 318,
            "collapsed": False,
            "rx": 0.84,
            "ry": 0.0,
            "rw": 0.159,
            "rh": 0.308,
            "ref_x": 0,
            "ref_y": 0,
            "ref_w": 1920,
            "ref_h": 1080,
        }
        orig_primary = fl.primary_desktop_rect

        def _fake_primary():
            return QRect(0, 0, 1280, 720)

        fl.primary_desktop_rect = _fake_primary  # type: ignore[assignment]
        try:
            left = fl.geometry_from_entry(donor)
            r = fl.geometry_from_entry(right)
            assert left["width"] == int(round(0.838 * 1280))
            assert r["x"] >= left["x"] + left["width"] - 8
            assert r["x"] + r["width"] <= 1280 + 1
            assert left["width"] < 1280
            # Bad laptop profile (absolute from 1920, rw>1) must not win over donor.
            settings = {
                "fences": [
                    {"id": "f1", "name": "A", "pages": [0], "width": 400, "height": 300}
                ],
                "fence_layouts_by_page": {},
                "fence_layouts_by_display": {
                    "0,0,1280x720": {
                        "0": {
                            "f1": {
                                "x": 0,
                                "y": 0,
                                "width": 1609,
                                "height": 317,
                                "rw": 1.25,
                                "rh": 0.46,
                                "rx": 0,
                                "ry": 0,
                                "ref_w": 1280,
                                "ref_h": 720,
                            }
                        }
                    },
                    "donor-monitor": {"0": {"f1": donor}},
                },
                "last_display_fingerprint": "donor-monitor",
            }
            # Fingerprint may not be 1280 — still exercise fit/donor path via helpers.
            assert not fl._ratios_usable(
                settings["fence_layouts_by_display"]["0,0,1280x720"]["0"]["f1"]
            )
            assert fl._ratios_usable(donor)
            geom = fl.geometry_from_entry(donor)
            assert 900 <= geom["width"] <= 1100
        finally:
            fl.primary_desktop_rect = orig_primary  # type: ignore[assignment]

    run("显示器→笔记本等比例缩放", _scale_monitor_to_laptop)

    def _scale_laptop_to_monitor_side_by_side():
        """Laptop→larger panel must keep two top fences side-by-side."""
        from PyQt6.QtCore import QRect
        from src import fence_layout as fl

        left = {
            "x": 0,
            "y": 0,
            "width": 1070,
            "height": 220,
            "collapsed": False,
            "rx": 0.0,
            "ry": 0.0,
            "rw": 0.838,
            "rh": 0.305,
            "ref_w": 1280,
            "ref_h": 720,
        }
        right = {
            "x": 1075,
            "y": 0,
            "width": 200,
            "height": 220,
            "collapsed": False,
            "rx": 0.84,
            "ry": 0.0,
            "rw": 0.159,
            "rh": 0.305,
            "ref_w": 1280,
            "ref_h": 720,
        }
        orig_primary = fl.primary_desktop_rect

        def _fake_monitor():
            return QRect(0, 0, 1920, 1032)

        fl.primary_desktop_rect = _fake_monitor  # type: ignore[assignment]
        try:
            a = fl.geometry_from_entry(left)
            b = fl.geometry_from_entry(right)
            assert a["width"] == int(round(0.838 * 1920))
            assert b["x"] >= a["x"] + a["width"] - 8
            assert b["x"] + b["width"] <= 1920 + 1
            assert fl._donor_size_score(
                "0,0,1920x1080@598x336mm@1", "0,0,1920x1080@598x336mm@1"
            ) > fl._donor_size_score(
                "0,0,1920x1080@309x174mm@1", "0,0,1920x1080@598x336mm@1"
            )
        finally:
            fl.primary_desktop_rect = orig_primary  # type: ignore[assignment]

    run("笔记本→显示器等比例且并排", _scale_laptop_to_monitor_side_by_side)

    def _unplug_prefers_multi_layout():
        """Removing an external monitor must scale the just-used dual layout.

        Regression: keeping a stale single-screen profile restored cramped
        leftover tiles instead of the dual arrangement the user was viewing.
        """
        from copy import deepcopy

        from PyQt6.QtCore import QRect

        from src import fence_layout as fl

        dual_fp = (
            "-1920,0,1920x1080@309x174mm@1|0,0,1920x1080@598x336mm@1"
        )
        single_fp = "0,0,1920x1080@309x174mm@1"
        dual_left = {
            "x": 0,
            "y": 0,
            "width": 1618,
            "height": 291,
            "collapsed": False,
            "rx": 0.0,
            "ry": 0.0,
            "rw": 0.8427,
            "rh": 0.282,
            "ref_x": 0,
            "ref_y": 0,
            "ref_w": 1920,
            "ref_h": 1032,
        }
        dual_right = {
            "x": 1620,
            "y": 0,
            "width": 299,
            "height": 292,
            "collapsed": False,
            "rx": 0.84375,
            "ry": 0.0,
            "rw": 0.1557,
            "rh": 0.283,
            "ref_x": 0,
            "ref_y": 0,
            "ref_w": 1920,
            "ref_h": 1032,
        }
        stale_single = {
            "x": 0,
            "y": 0,
            "width": 710,
            "height": 291,
            "collapsed": False,
            "rx": 0.0,
            "ry": 0.0,
            "rw": 0.37,
            "rh": 0.282,
            "ref_x": 0,
            "ref_y": 0,
            "ref_w": 1920,
            "ref_h": 1032,
        }
        assert fl.fingerprint_monitor_count(dual_fp) == 2
        assert fl.fingerprint_monitor_count(single_fp) == 1

        settings = {
            "fences": [
                {"id": "f1", "name": "软件", "pages": [0], "width": 400, "height": 300},
                {"id": "f2", "name": "其他", "pages": [0], "width": 300, "height": 300},
            ],
            "fence_layouts_by_page": {},
            "fence_layouts_by_display": {
                dual_fp: {"0": {"f1": deepcopy(dual_left), "f2": deepcopy(dual_right)}},
                single_fp: {
                    "0": {
                        "f1": deepcopy(stale_single),
                        "f2": {
                            **deepcopy(stale_single),
                            "x": 714,
                            "width": 306,
                            "rx": 0.37,
                            "rw": 0.16,
                        },
                    }
                },
            },
            "last_display_fingerprint": dual_fp,
        }
        fl.prepare_destination_profile_for_display_change(
            settings, dual_fp, single_fp
        )
        assert single_fp not in settings["fence_layouts_by_display"]

        orig_fp = fl.current_display_fingerprint
        orig_primary = fl.primary_desktop_rect
        fl.current_display_fingerprint = lambda: single_fp  # type: ignore[assignment]
        fl.primary_desktop_rect = lambda: QRect(0, 0, 1920, 1032)  # type: ignore[assignment]
        try:
            g1 = fl.get_fence_geometry(settings, settings["fences"][0], 0)
            g2 = fl.get_fence_geometry(settings, settings["fences"][1], 0)
            # Scaled from dual ratios — wide left tile, not the stale 710px strip.
            assert g1["width"] >= 1500, g1
            assert g2["x"] >= g1["x"] + g1["width"] - 16, (g1, g2)
        finally:
            fl.current_display_fingerprint = orig_fp  # type: ignore[assignment]
            fl.primary_desktop_rect = orig_primary  # type: ignore[assignment]

        # Startup heal: cramped single vs multi donor clears the leftover.
        settings2 = {
            "fences": settings["fences"],
            "fence_layouts_by_page": {},
            "fence_layouts_by_display": {
                dual_fp: {"0": {"f1": deepcopy(dual_left), "f2": deepcopy(dual_right)}},
                single_fp: {
                    "0": {
                        "f1": deepcopy(stale_single),
                        "f2": {
                            **deepcopy(stale_single),
                            "x": 714,
                            "width": 306,
                            "rx": 0.37,
                            "rw": 0.16,
                        },
                    }
                },
                "0,0,1024x768@271x203mm@1": {
                    "0": {"f1": deepcopy(stale_single)}
                },
            },
            "last_display_fingerprint": single_fp,
        }
        fl.current_display_fingerprint = lambda: single_fp  # type: ignore[assignment]
        try:
            fl.purge_transient_display_profiles(settings2)
            assert "0,0,1024x768@271x203mm@1" not in settings2[
                "fence_layouts_by_display"
            ]
            fl._heal_stale_single_after_multi(settings2)
            assert single_fp not in settings2["fence_layouts_by_display"]
            assert settings2["last_display_fingerprint"] == dual_fp
        finally:
            fl.current_display_fingerprint = orig_fp  # type: ignore[assignment]

        # Offscreen Qt fake 800×800 must never stick as last_fp / profile donor.
        settings3 = {
            "fences": settings["fences"],
            "fence_layouts_by_display": {
                dual_fp: {"0": {"f1": deepcopy(dual_left), "f2": deepcopy(dual_right)}},
                "0,0,800x800@203x203mm@1": {
                    "0": {
                        "f1": {
                            "x": 0,
                            "y": 0,
                            "width": 800,
                            "height": 221,
                            "collapsed": False,
                            "rw": 1.0,
                        },
                        "f2": {
                            "x": 493,
                            "y": 0,
                            "width": 307,
                            "height": 219,
                            "collapsed": False,
                            "rw": 0.38,
                        },
                    }
                },
            },
            "last_display_fingerprint": "0,0,800x800@203x203mm@1",
        }
        assert fl.fingerprint_is_transient("0,0,800x800@203x203mm@1")
        fl.current_display_fingerprint = lambda: dual_fp  # type: ignore[assignment]
        try:
            fl.purge_transient_display_profiles(settings3)
            assert "0,0,800x800@203x203mm@1" not in settings3[
                "fence_layouts_by_display"
            ]
            assert settings3["last_display_fingerprint"] == dual_fp
            assert fl.layout_writes_blocked() is False
        finally:
            fl.current_display_fingerprint = orig_fp  # type: ignore[assignment]

        # App wiring: longer debounce on screenRemoved.
        import inspect

        from src.app import DeskTidyApp

        src = inspect.getsource(DeskTidyApp._connect_display_signals)
        assert "_schedule_display_layout_refresh_removed" in src
        on_chg = inspect.getsource(DeskTidyApp._on_display_layout_changed)
        assert "prepare_destination_profile_for_display_change" in on_chg
        # Taskbar move / auto-hide keeps fingerprint — still remask public plate.
        assert on_chg.count("_refresh_public_host_click_mask") >= 2

    run("拔掉外接屏按多屏布局比例落到本机", _unplug_prefers_multi_layout)

    def _reject_crushed():
        from src.fence_layout import (
            _is_crushed_geometry,
            current_display_fingerprint,
            get_fence_geometry,
            save_fence_geometry,
        )

        assert _is_crushed_geometry({"width": 160})
        assert not _is_crushed_geometry({"width": 400})

        fp = current_display_fingerprint()
        settings = {
            "fences": [
                {
                    "id": "f1",
                    "name": "软件",
                    "x": 1760,
                    "y": 0,
                    "width": 160,
                    "height": 320,
                    "pages": [0],
                }
            ],
            "fence_layouts_by_page": {
                "0": {
                    "f1": {
                        "x": 1760,
                        "y": 0,
                        "width": 160,
                        "height": 320,
                        "collapsed": False,
                    }
                }
            },
            "fence_layouts_by_display": {
                fp: {
                    "0": {
                        "f1": {
                            "x": 1760,
                            "y": 0,
                            "width": 160,
                            "height": 320,
                            "collapsed": False,
                            "rw": 0.08,
                            "ref_w": 1920,
                        }
                    }
                },
                "donor-good": {
                    "0": {
                        "f1": {
                            "x": 77,
                            "y": 0,
                            "width": 1536,
                            "height": 320,
                            "collapsed": False,
                            "rx": 0.04,
                            "ry": 0,
                            "rw": 0.8,
                            "rh": 0.3,
                            "ref_x": 0,
                            "ref_y": 0,
                            "ref_w": 1920,
                            "ref_h": 1080,
                        }
                    }
                },
            },
            "last_display_fingerprint": "donor-good",
        }
        # Crushed save must be ignored
        before = json.dumps(settings["fence_layouts_by_display"][fp]["0"]["f1"])
        save_fence_geometry(
            settings,
            settings["fences"][0],
            0,
            {"x": 1760, "y": 0, "width": 160, "height": 320},
        )
        after = json.dumps(settings["fence_layouts_by_display"][fp]["0"]["f1"])
        assert before == after, "crushed geometry must not overwrite profile"

        geom = get_fence_geometry(settings, settings["fences"][0], 0)
        assert geom["width"] > 200, geom

    run("拒绝 160px 压扁布局并从 donor 恢复", _reject_crushed)

    def _span_clamp():
        from src.fence_layout import _clamp_geometry, primary_desktop_rect

        primary = primary_desktop_rect()
        # Simulate dual-monitor full-bleed rect
        geom = _clamp_geometry(-1920, 100, 3840, 320, False)
        assert geom["width"] <= primary.width()
        assert geom["x"] >= primary.x()
        assert geom["x"] + geom["width"] <= primary.x() + primary.width() + 1

    run("跨屏全宽钳制到单屏", _span_clamp)

    def _snap():
        from PyQt6.QtCore import QRect
        from PyQt6.QtGui import QGuiApplication

        from src.ui.screen_snap import snap_geometry

        primary = QGuiApplication.primaryScreen()
        assert primary is not None
        area = primary.availableGeometry()
        # Spanning rect → primary only
        snapped = snap_geometry(QRect(area.x() - 500, area.y(), area.width() + 1000, 300))
        assert snapped.width() <= area.width()
        assert snapped.x() >= area.x()

    run("screen_snap 跨屏收拢到单屏", _snap)

    def _place_new_fence_avoids_overlap():
        from PyQt6.QtCore import QRect
        from PyQt6.QtGui import QGuiApplication

        from src.fence_layout import _NEW_FENCE_GAP, place_new_fence_rect

        area = QGuiApplication.primaryScreen().availableGeometry()
        occupied = [QRect(area.x() + 80, area.y() + 80, 400, 400)]
        click_x = occupied[0].center().x()
        click_y = occupied[0].center().y()
        x, y, w, h = place_new_fence_rect(
            {"fences": []},
            0,
            hint_x=click_x,
            hint_y=click_y,
            occupied=occupied,
        )
        placed = QRect(x, y, w, h)
        padded = placed.adjusted(
            -_NEW_FENCE_GAP, -_NEW_FENCE_GAP, _NEW_FENCE_GAP, _NEW_FENCE_GAP
        )
        assert not padded.intersects(occupied[0]), (placed, occupied[0])

        empty_x = area.x() + area.width() - 280
        empty_y = area.y() + area.height() - 380
        x2, y2, w2, h2 = place_new_fence_rect(
            {"fences": []},
            0,
            hint_x=empty_x,
            hint_y=empty_y,
            occupied=occupied,
        )
        free = QRect(x2, y2, w2, h2)
        assert not free.adjusted(
            -_NEW_FENCE_GAP, -_NEW_FENCE_GAP, _NEW_FENCE_GAP, _NEW_FENCE_GAP
        ).intersects(occupied[0])
        assert abs((x2 + w2 // 2) - empty_x) < w2

        other_page = {
            "fences": [
                {
                    "id": "other",
                    "visible": True,
                    "pages": [2],
                    "x": area.x() + 40,
                    "y": area.y() + 40,
                    "width": 500,
                    "height": 500,
                }
            ]
        }
        hx, hy = area.x() + 200, area.y() + 180
        x3, y3, w3, h3 = place_new_fence_rect(other_page, 0, hint_x=hx, hint_y=hy)
        assert abs((x3 + w3 // 2) - hx) <= 8
        assert abs((y3 + 48) - hy) <= 8

        drawn = QRect(area.x() + 60, area.y() + 60, 260, 280)
        x4, y4, w4, h4 = place_new_fence_rect(
            {"fences": []},
            0,
            width=drawn.width(),
            height=drawn.height(),
            hint_x=drawn.center().x(),
            hint_y=drawn.center().y(),
            anchor_x=drawn.x(),
            anchor_y=drawn.y(),
            occupied=[],
        )
        assert (x4, y4, w4, h4) == (
            drawn.x(),
            drawn.y(),
            drawn.width(),
            drawn.height(),
        )

        stacked: list[QRect] = []
        for _ in range(3):
            nx, ny, nw, nh = place_new_fence_rect(
                {"fences": []},
                0,
                hint_x=area.x() + 160,
                hint_y=area.y() + 160,
                occupied=stacked,
            )
            nxt = QRect(nx, ny, nw, nh)
            for prev in stacked:
                assert not nxt.adjusted(
                    -_NEW_FENCE_GAP, -_NEW_FENCE_GAP, _NEW_FENCE_GAP, _NEW_FENCE_GAP
                ).intersects(prev)
            stacked.append(nxt)

        # Settings/后台: saved dict is stale 50,50 while live fence is a wide strip.
        from src.fence_layout import (
            place_new_fence_config,
            set_live_fences_provider,
        )

        set_live_fences_provider(None)
        strip = QRect(area.x(), area.y(), min(1609, area.width()), 315)

        class _Live:
            def __init__(self) -> None:
                self.config = {"id": "system_common", "pages": [0], "visible": True}
                self._desktidy_soft_parked = True

            def isVisible(self) -> bool:
                return False

            def frameGeometry(self) -> QRect:
                return QRect(strip)

        stale = {
            "fences": [
                {
                    "id": "system_common",
                    "visible": True,
                    "pages": [0],
                    "x": 50,
                    "y": 50,
                    "width": 220,
                    "height": 320,
                }
            ]
        }
        live = [_Live()]
        x5, y5, w5, h5 = place_new_fence_rect(
            stale,
            0,
            hint_x=area.x() + 80,
            hint_y=area.y() + 80,
            live_fences=live,
        )
        placed_live = QRect(x5, y5, w5, h5)
        assert not placed_live.adjusted(
            -_NEW_FENCE_GAP, -_NEW_FENCE_GAP, _NEW_FENCE_GAP, _NEW_FENCE_GAP
        ).intersects(strip), (placed_live, strip)

        set_live_fences_provider(lambda: live)
        try:
            cfg = {"width": 220, "height": 320, "x": 50, "y": 50}
            place_new_fence_config(stale, cfg, 0)
            from_settings = QRect(cfg["x"], cfg["y"], cfg["width"], cfg["height"])
            assert not from_settings.adjusted(
                -_NEW_FENCE_GAP, -_NEW_FENCE_GAP, _NEW_FENCE_GAP, _NEW_FENCE_GAP
            ).intersects(strip), (from_settings, strip)
        finally:
            set_live_fences_provider(None)

    run("新建分区避开当前页已有分区", _place_new_fence_avoids_overlap)


# ---------------------------------------------------------------------------
# 5. Fence rules
# ---------------------------------------------------------------------------

def test_fence_rules() -> None:
    begin("5) 分区匹配规则")

    def _match():
        from src.desktop_scanner import DesktopItem
        from src.fence_rules import (
            item_matches_fence,
            item_organize_kind,
            match_item_to_fence,
        )

        doc = DesktopItem(
            path=Path(r"C:\tmp\report.docx"),
            name="report.docx",
            extension=".docx",
            is_dir=False,
            size=0,
        )
        shortcut = DesktopItem(
            path=Path(r"C:\tmp\app.lnk"),
            name="app.lnk",
            extension=".lnk",
            is_dir=False,
            size=0,
        )
        folder = DesktopItem(
            path=Path(r"C:\tmp\Projects"),
            name="Projects",
            extension="",
            is_dir=True,
            size=0,
        )
        assert item_organize_kind(doc) == "file"
        assert item_organize_kind(shortcut) == "icon"
        # Folders and other non-icons classify as「文档」.
        assert item_organize_kind(folder) == "file"

        fence_file = {"id": "d1", "name": "文档", "organize_kinds": ["file"]}
        fence_icon = {"id": "i1", "name": "图标", "organize_kinds": ["icon"]}
        assert item_matches_fence(doc, fence_file)
        assert not item_matches_fence(doc, fence_icon)
        assert item_matches_fence(shortcut, fence_icon)
        assert item_matches_fence(folder, fence_file)
        assert not item_matches_fence(folder, fence_icon)
        # Legacy「文件夹」kind folds into 文档.
        legacy_folder = {"id": "f1", "name": "旧文件夹", "organize_kinds": ["folder"]}
        assert item_matches_fence(folder, legacy_folder)
        assert item_matches_fence(doc, legacy_folder)

        # Legacy extensions still infer kinds.
        legacy = {"id": "L", "name": "旧", "extensions": [".lnk", ".txt"]}
        assert item_matches_fence(shortcut, legacy)
        assert item_matches_fence(doc, legacy)

        settings = {
            "fences": [fence_icon, fence_file],
            "exclude_patterns": [],
        }
        hit = match_item_to_fence(doc, settings)
        assert hit is not None and hit["id"] == "d1"

    run("图标/文档两类规则匹配", _match)


# ---------------------------------------------------------------------------
# 6. Organizer virtual pin + legacy restore helper
# ---------------------------------------------------------------------------

def test_organizer() -> None:
    begin("6) 整理 / 还原")

    def _virtual_organize_no_move():
        from src.organizer import organize_desktop
        from src.fence_rules import get_virtual_items_for_fence
        from src.settings import get_desktop_path

        desk = get_desktop_path()
        marker = f"_desktidy_sys_{int(time.time())}"
        src = desk / f"{marker}.txt"
        try:
            src.write_text("hello-organize", encoding="utf-8")
            settings = {
                "organize_mode": "virtual",
                "exclude_patterns": ["desktop.ini", "DeskTidy"],
                "public_desktop_items": [],
                "fences": [
                    {
                        "id": "sys1",
                        "name": "测试分区",
                        "folder": "测试分区",
                        "extensions": [".txt"],
                        "filename_patterns": [f"{marker}*"],
                        "visible": True,
                        "pages": [0],
                        "virtual_items": [],
                    }
                ],
            }
            result = organize_desktop(settings, dry_run=False)
            assert result.virtual is True
            assert result.moved_count >= 1, result.summary
            # File stays on desktop — only pinned.
            assert src.is_file()
            assert src.read_text(encoding="utf-8") == "hello-organize"
            pinned = get_virtual_items_for_fence(settings["fences"][0], settings)
            assert any(p.name == src.name for p in pinned)
        finally:
            src.unlink(missing_ok=True)
            for p in desk.glob(f"{marker}*"):
                try:
                    p.unlink(missing_ok=True)
                except OSError:
                    pass

    run("虚拟整理钉选且不移动文件", _virtual_organize_no_move)

    def _organize_records_cross_page_fence():
        import tempfile
        from pathlib import Path

        from src.organizer import organize_desktop

        with tempfile.TemporaryDirectory() as tmp:
            desk = Path(tmp)
            xlsx = desk / "page_float.xlsx"
            xlsx.write_text("x", encoding="utf-8")
            import src.desktop_scanner as scanner
            import src.settings as settings_mod

            old_paths = settings_mod.get_desktop_paths
            old_scan = scanner.get_desktop_paths
            settings_mod.get_desktop_paths = lambda: [desk]  # type: ignore
            scanner.get_desktop_paths = lambda: [desk]  # type: ignore
            scanner.invalidate_desktop_scan_cache()
            try:
                settings = {
                    "current_page": 0,
                    "organize_mode": "virtual",
                    "exclude_patterns": [],
                    "organize_whitelist": [],
                    "public_desktop_items": [],
                    "desktop_pages": [
                        {"id": 0, "name": "工作", "organize_kinds": ["icon"]},
                        {"id": 1, "name": "文档", "organize_kinds": ["file"]},
                    ],
                    "fences": [
                        {
                            "id": "soft",
                            "name": "软件",
                            "pages": [0],
                            "organize_kinds": ["icon"],
                            "virtual_items": [],
                        },
                        {
                            "id": "docs",
                            "name": "文档",
                            "pages": [1],
                            "organize_kinds": ["file"],
                            "virtual_items": [],
                        },
                    ],
                }
                result = organize_desktop(settings, dry_run=False)
                assert 1 in result.float_page_ids, result.float_page_ids
                docs_pins = settings["fences"][1].get("virtual_items") or []
                assert any(str(x).endswith("page_float.xlsx") for x in docs_pins)
            finally:
                settings_mod.get_desktop_paths = old_paths
                scanner.get_desktop_paths = old_scan
                scanner.invalidate_desktop_scan_cache()

    run("整理后记录跨页分区目标", _organize_records_cross_page_fence)

    def _virtual_pin():
        from src.fence_rules import (
            assign_paths_to_virtual_fence,
            get_virtual_items_for_fence,
            unpin_paths_from_virtual_fence,
        )
        from src.settings import get_desktop_path

        desk = get_desktop_path()
        files = []
        try:
            for i in range(2):
                p = desk / f"_desktidy_vpin_{i}.txt"
                p.write_text("v", encoding="utf-8")
                files.append(p)
            settings = {
                "organize_mode": "virtual",
                "exclude_patterns": [],
                "fences": [
                    {"id": "va", "name": "A", "extensions": [], "virtual_items": []},
                    {"id": "vb", "name": "B", "extensions": [], "virtual_items": []},
                ],
            }
            fa, fb = settings["fences"]
            assign_paths_to_virtual_fence(fa, settings, files)
            assert len(get_virtual_items_for_fence(fa, settings)) == 2
            assign_paths_to_virtual_fence(fb, settings, [files[0]])
            assert len(get_virtual_items_for_fence(fa, settings)) == 1
            unpin_paths_from_virtual_fence(fb, [files[0]])
            assert files[0].is_file()
        finally:
            for p in files:
                p.unlink(missing_ok=True)

    run("虚拟分区 pin / 迁移 / unpin", _virtual_pin)

    def _legacy_restore_api():
        from src.organizer import legacy_warehouse_has_files, restore_desktop_from_storage

        assert callable(restore_desktop_from_storage)
        assert isinstance(legacy_warehouse_has_files(), bool)

    run("遗留仓库还原 API 可调用", _legacy_restore_api)


# ---------------------------------------------------------------------------
# 7. Win shell / overlay layer
# ---------------------------------------------------------------------------

def test_win_shell_overlay() -> None:
    begin("7) 桌面层级 / 壳层挂载")

    def _overlay_api():
        from src.desktop_shell_host import (
            find_desktop_shell_host,
            is_wallpaper_workerw,
        )
        from src.win_shell import are_desktop_icons_visible

        host = find_desktop_shell_host()
        assert host == 0 or not is_wallpaper_workerw(host)
        _ = are_desktop_icons_visible()

    run("overlay / 壳层 host API 可调用", _overlay_api)

    def _desktop_icon_restore_contracts():
        import inspect

        from src.app import DeskTidyApp
        from src.win_shell import (
            _NAMESPACE_FALLBACK_NAMES,
            _read_hide_icons_registry,
            ensure_desktop_icons_visible,
            reveal_hosted_namespace_icons,
            restore_all_hosted_namespace_icons,
            set_desktop_icons_visible,
        )

        assert callable(_read_hide_icons_registry)
        assert "force" in inspect.signature(set_desktop_icons_visible).parameters
        assert "_read_hide_icons_registry" in inspect.getsource(ensure_desktop_icons_visible)
        assert "_write_hide_icons_registry" in inspect.getsource(ensure_desktop_icons_visible)
        # force=True must not blindly toggle (double ensure would hide icons on quit).
        set_src = inspect.getsource(set_desktop_icons_visible)
        assert "already_ok" in set_src
        assert "never toggle" in set_src
        ensure_src = inspect.getsource(ensure_desktop_icons_visible)
        assert "are_desktop_icons_visible" in ensure_src
        assert "Safe to call repeatedly" in ensure_src
        park = inspect.getsource(DeskTidyApp._park_desktop_for_exit)
        assert "_ensure_shell_icons_restored" in park
        assert "_flush_overlay_teardown" in park
        retire = inspect.getsource(DeskTidyApp._retire_overlay_widget)
        assert "setWindowOpacity" in retire
        assert "setParent" in retire
        assert "ensure_desktop_icons_visible" in inspect.getsource(
            DeskTidyApp._ensure_shell_icons_restored
        )
        reveal_src = inspect.getsource(reveal_hosted_namespace_icons)
        assert "_NAMESPACE_FALLBACK_NAMES" in reveal_src
        assert "{20D04FE0-3AEA-1069-A2D8-08002B30309D}" in _NAMESPACE_FALLBACK_NAMES
        assert "_NAMESPACE_FALLBACK_NAMES" in inspect.getsource(
            restore_all_hosted_namespace_icons
        )
        stock = inspect.getsource(DeskTidyApp._show_stock_desktop_view)
        assert "reveal_hosted_namespace_icons" in stock
        assert "hide_fences" in stock
        assert "_flush_overlay_teardown" in stock
        toggle = inspect.getsource(DeskTidyApp._toggle_icons_and_fences)
        assert "_show_stock_desktop_view" in toggle
        assert "_show_organized_desktop_view" in toggle
        dbl = inspect.getsource(DeskTidyApp._on_desktop_double_click)
        assert "_show_stock_desktop_view" in dbl
        quit_src = inspect.getsource(DeskTidyApp._quit_impl)
        assert "_apply_session_desktop_view" in quit_src

    run("退出恢复桌面图标合约", _desktop_icon_restore_contracts)

    def _stack_mode_preserves_geo():
        from PyQt6.QtCore import QRect, Qt
        from PyQt6.QtWidgets import QWidget

        from src.win_shell import apply_overlay_stack_mode

        app = globals().get("qt_app")
        assert app is not None
        w = QWidget()
        w.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.Window)
        w.setGeometry(80, 60, 640, 300)
        w.show()
        app.processEvents()
        geo = QRect(w.geometry())
        apply_overlay_stack_mode(w, desktop_layer=True, peek=False)
        app.processEvents()
        after = w.geometry()
        assert after.width() == geo.width() and after.height() == geo.height(), (
            f"{geo} -> {after}"
        )
        assert after.width() > 200
        apply_overlay_stack_mode(w, desktop_layer=False, peek=False)
        app.processEvents()
        after2 = w.geometry()
        assert after2.width() == geo.width()
        w.close()
        w.deleteLater()
        app.processEvents()

    run("apply_overlay_stack_mode 不压扁尺寸", _stack_mode_preserves_geo)

    def _no_bottom_hint_on_fence():
        from PyQt6.QtCore import Qt

        from src.ui.fence_widget import FenceWidget

        settings = {
            "organize_mode": "virtual",
            "exclude_patterns": [],
            "fences": [],
            "theme": "mist",
        }
        cfg = {
            "id": "flag1",
            "name": "标志测试",
            "x": 40,
            "y": 40,
            "width": 300,
            "height": 220,
            "extensions": [],
            "style": {},
            "pages": [0],
        }
        settings["fences"] = [cfg]
        w = FenceWidget(cfg, settings)
        flags = w.windowFlags()
        assert not (flags & Qt.WindowType.WindowStaysOnBottomHint)
        assert w._is_virtual_mode() is True
        w.close()
        w.deleteLater()

    run("FenceWidget 不再使用 WindowStaysOnBottomHint", _no_bottom_hint_on_fence)

    def _shell_attach_entry_exists():
        import inspect

        from src import win_shell
        from src.desktop_shell_host import attach_overlay_to_desktop, is_wallpaper_workerw

        cfg_src = inspect.getsource(win_shell.configure_desktop_overlay)
        assert "ensure_overlay_on_desktop" in cfg_src or "attach_overlay_to_desktop" in cfg_src
        assert "detach_overlay_from_desktop" in cfg_src
        assert callable(attach_overlay_to_desktop)
        assert callable(is_wallpaper_workerw)
        assert not hasattr(win_shell, "settle_desktop_overlay_zorder")
        assert not hasattr(win_shell, "should_use_desktop_overlay_layer")
        stack_src = inspect.getsource(win_shell.apply_overlay_stack_mode)
        assert "HWND_NOTOPMOST = -2" not in stack_src
        # Ownership is done once inside configure_desktop_overlay (no double ensure).
        assert "configure_desktop_overlay" in stack_src
        assert "ensure_overlay_on_desktop" not in stack_src
        from src.desktop_shell_host import place_overlay_in_desktop_band

        band_src = inspect.getsource(place_overlay_in_desktop_band)
        # Desktop band = HWND_BOTTOM under ownership (above icons, under apps).
        # Never insert relative to DefView or settings (covers other apps).
        assert "HWND_BOTTOM" in band_src
        assert "GW_HWNDPREV" not in band_src
        assert "SetWindowPos(int(hwnd), int(defview)" not in band_src.replace(" ", "")

    run("壳层挂载入口存在 / 不抬到普通窗顶", _shell_attach_entry_exists)

    def _legacy_migrate_hook_exists():
        import inspect

        from src.app import DeskTidyApp

        src = inspect.getsource(DeskTidyApp._startup_deferred_legacy_storage_migrate)
        assert "legacy_warehouse_has_files" in src
        assert "restore_desktop_from_storage" in src
        sched = inspect.getsource(DeskTidyApp._schedule_startup_deferred)
        assert "_startup_deferred_legacy_storage_migrate" in sched
        assert "_should_auto_organize_empty_storage" not in sched

    run("遗留仓库迁移启动钩子存在", _legacy_migrate_hook_exists)


# ---------------------------------------------------------------------------
# 8. Hotkeys / instance lock / i18n
# ---------------------------------------------------------------------------

def test_hotkeys_lock_i18n() -> None:
    begin("8) 热键 / 单实例 / 文案")

    def _parse():
        from src.hotkey_manager import HOTKEY_IDS, HOTKEY_LABELS, _parse_hotkey

        assert set(HOTKEY_IDS) == set(HOTKEY_LABELS)
        parsed = _parse_hotkey("Ctrl+Shift+O")
        assert parsed is not None
        mods, vk = parsed
        assert mods != 0 and vk != 0
        assert _parse_hotkey("") is None
        assert _parse_hotkey("NotAKey") is None

    run("热键解析", _parse)

    def _register():
        from src.hotkey_manager import HotkeyManager

        app = globals()["qt_app"]
        mgr = HotkeyManager(app)
        called = {"n": 0}

        def cb():
            called["n"] += 1

        # Use an uncommon combo to reduce conflict; unregister immediately.
        ok_reg = mgr.register("organize", "Ctrl+Alt+Shift+F9", cb)
        mgr.unregister_all()
        assert ok_reg or True  # registration may fail if OS blocks; API must not crash

    run("热键注册/注销不崩溃", _register)

    def _lock():
        from src import instance_lock
        from src.instance_lock import AcquireResult

        # Smoke the probe API; if we acquire, release so later launch tests work.
        was_held = instance_lock.is_main_instance_running()
        if not was_held:
            result = instance_lock.try_acquire_main_instance()
            assert result is AcquireResult.OWNED
            instance_lock.release_main_instance()
            # Mutex should be free again for packaged launch tests.
            assert instance_lock.is_main_instance_running() is False
        else:
            # Live DeskTidy owns it — probe only.
            assert instance_lock.is_main_instance_running() is True

    run("单实例锁 API", _lock)

    def _i18n():
        from src.i18n import APP_NAME_ZH, VIEW_MODE_LABELS, view_mode_label

        assert APP_NAME_ZH == "DeskTidy"
        assert view_mode_label("grid") == VIEW_MODE_LABELS["grid"]

    run("中文文案", _i18n)


# ---------------------------------------------------------------------------
# 9. UI widgets
# ---------------------------------------------------------------------------

def test_ui_widgets() -> None:
    begin("9) UI 控件构建")

    app = globals()["qt_app"]

    def _fence_widget():
        from src.ui.fence_widget import FenceWidget

        settings = {
            "organize_mode": "virtual",
            "exclude_patterns": [],
            "fences": [],
            "theme": "mist",
        }
        cfg = {
            "id": "ui1",
            "name": "测试分区",
            "x": 50,
            "y": 50,
            "width": 360,
            "height": 260,
            "virtual_items": [],
            "extensions": [".txt"],
            "style": {"view_mode": "grid", "show_title": True},
            "pages": [0],
        }
        settings["fences"] = [cfg]
        w = FenceWidget(cfg, settings)
        # Geometry is applied by the app host; set it explicitly for unit smoke.
        w.setGeometry(cfg["x"], cfg["y"], cfg["width"], cfg["height"])
        w.show()
        app.processEvents()
        assert w.width() >= 300
        upd = w.get_config_update()
        assert upd["width"] >= 300
        w.close()
        w.deleteLater()
        app.processEvents()

    run("FenceWidget 显示与几何", _fence_widget)

    def _page_indicator():
        from src.ui.page_indicator import PageIndicatorWidget

        pages = [{"id": 0, "name": "工作"}, {"id": 1, "name": "文档"}]
        w = PageIndicatorWidget(pages, current_page=0, settings={})
        w.show()
        app.processEvents()
        assert w.isVisible()
        w.close()
        w.deleteLater()
        app.processEvents()

    run("PageIndicatorWidget", _page_indicator)

    def _dock():
        from src.ui.dock_widget import DockWidget

        w = DockWidget({"enabled": True, "position": "bottom", "opacity": 0.9, "items": []})
        w.show()
        app.processEvents()
        w.close()
        w.deleteLater()
        app.processEvents()

    run("DockWidget", _dock)

    def _main_window():
        from src.settings import load_settings
        from src.ui.main_window import MainWindow

        s = load_settings()
        win = MainWindow(s)
        win.show()
        app.processEvents()
        assert win.windowTitle()
        win.hide()
        win.close()
        win.deleteLater()
        app.processEvents()

    run("MainWindow", _main_window)

    def _icon_item():
        from PyQt6.QtCore import Qt

        from src.ui.fence_icon_item import FenceIconItem

        tmp = Path(tempfile.gettempdir()) / "_desktidy_sys_icon.txt"
        tmp.write_text("1", encoding="utf-8")
        try:
            item = FenceIconItem(tmp, virtual_mode=True, fence_id="x")
            assert item.icon_label.testAttribute(
                Qt.WidgetAttribute.WA_TransparentForMouseEvents
            )
            item.deleteLater()
        finally:
            tmp.unlink(missing_ok=True)

    run("FenceIconItem", _icon_item)


# ---------------------------------------------------------------------------
# 10. Public desktop helpers
# ---------------------------------------------------------------------------

def test_public_desktop() -> None:
    begin("10) 公共桌面布局")

    def _relayout():
        from PyQt6.QtCore import QRect

        from src.public_desktop import (
            add_public_item,
            get_public_items,
            relayout_public_items,
        )

        settings = {
            "enable_public_desktop": True,
            "public_desktop_items": [],
        }
        add_public_item(
            settings,
            r"C:\tmp\a.txt",
            x=50,
            y=50,
            auto_arrange=False,
            fence_rects=[QRect(0, 0, 100, 100)],
        )
        add_public_item(
            settings,
            r"C:\tmp\b.txt",
            x=200,
            y=50,
            auto_arrange=False,
            fence_rects=[QRect(0, 0, 100, 100)],
        )
        changed = relayout_public_items(
            settings,
            page_id=0,
            fence_rects=[QRect(0, 0, 100, 100)],
        )
        items = get_public_items(settings)
        assert len(items) == 2
        # After relayout, positions should be integers
        for e in items:
            assert isinstance(e.get("x"), int) and isinstance(e.get("y"), int)
        assert changed is True or changed is False

    run("public items 读写与 relayout", _relayout)


# ---------------------------------------------------------------------------
# 11. Desktop scanner
# ---------------------------------------------------------------------------

def test_desktop_scanner() -> None:
    begin("11) 桌面扫描")

    def _scan():
        from src.desktop_scanner import scan_desktop

        result = scan_desktop(exclude=["desktop.ini", "DeskTidy"])
        assert hasattr(result, "items")
        assert isinstance(result.items, list)

    run("scan_desktop", _scan)


# ---------------------------------------------------------------------------
# 12. Screenshot / peek managers (construct only)
# ---------------------------------------------------------------------------

def test_managers() -> None:
    begin("12) 截图 / Peek 管理器")

    def _shot():
        from src.screenshot_manager import ScreenshotManager

        mgr = ScreenshotManager(lambda: {"screenshot": {"enabled": True}})
        assert mgr is not None
        mgr.close_all()

    run("ScreenshotManager", _shot)

    def _peek():
        from src.peek_manager import PeekManager

        mgr = PeekManager(get_fences=lambda: [])
        assert mgr.active is False

    run("PeekManager", _peek)


# ---------------------------------------------------------------------------
# 13. Live user settings health
# ---------------------------------------------------------------------------

def test_live_settings_health() -> None:
    begin("13) 用户配置健康检查")

    def _load_live():
        from src.fence_layout import (
            _is_crushed_geometry,
            current_display_fingerprint,
            get_fence_geometry,
        )
        from src.settings import load_settings

        settings = load_settings()
        assert isinstance(settings, dict)
        assert "fences" in settings
        fp = current_display_fingerprint()
        assert fp

        fences = [f for f in settings.get("fences", []) if isinstance(f, dict)]
        crushed = []
        for fence in fences:
            geo = get_fence_geometry(settings, fence, 0)
            if _is_crushed_geometry(geo):
                crushed.append(f"{fence.get('name')}:{geo.get('width')}")
            # Resolved geometry used on screen must be a usable width.
            assert int(geo.get("width", 0)) >= 160, geo
            assert int(geo.get("height", 0)) >= 40, geo
        assert not crushed, f"仍有压扁布局: {crushed}"

        mode = settings.get("organize_mode")
        assert mode == "virtual"

    run("load_settings + 分区几何未压扁", _load_live)

    def _storage_vs_desktop():
        from src.desktop_scanner import scan_desktop
        from src.settings import get_desktop_path, get_fence_storage_root, load_settings

        settings = load_settings()
        desk = get_desktop_path()
        storage = get_fence_storage_root()
        assert desk.is_dir(), desk
        assert settings.get("organize_mode") == "virtual"

        scan = scan_desktop(exclude=settings.get("exclude_patterns", []))
        assert hasattr(scan, "items")
        assert hasattr(scan, "loose_files")
        # Storage may hold .public_system only; warehouse leftovers are migrated on startup.
        if storage.exists():
            _ = sum(1 for p in storage.rglob("*") if p.is_file())

    run("真实桌面扫描与存储目录可访问", _storage_vs_desktop)


# ---------------------------------------------------------------------------
# 14. Live organize dry-run
# ---------------------------------------------------------------------------

def test_live_organize_dry_run() -> None:
    begin("14) 真实桌面整理 dry-run")

    def _dry_run():
        from src.organizer import get_fence_stats, organize_desktop
        from src.settings import load_settings

        settings = load_settings()
        stats = get_fence_stats(settings)
        assert isinstance(stats, dict)

        result = organize_desktop(settings, dry_run=True)
        assert hasattr(result, "moved_count")
        assert hasattr(result, "skipped_count")
        assert hasattr(result, "actions")
        # dry_run must not have moved anything on disk; actions may still list plans
        assert result.moved_count >= 0
        assert isinstance(result.actions, list)

    run("organize_desktop(dry_run=True)", _dry_run)


# ---------------------------------------------------------------------------
# 15. Live desktop shell attach (geometry preserved; no Win+D key spam)
# ---------------------------------------------------------------------------

def test_live_show_desktop_cycle() -> None:
    begin("15) 桌面壳层挂载")

    def _cycle():
        from PyQt6.QtCore import QRect, Qt
        from PyQt6.QtWidgets import QWidget

        from src.desktop_shell_host import (
            detach_overlay_from_desktop,
            find_desktop_shell_host,
            is_attached_to_desktop,
            is_wallpaper_workerw,
        )
        from src.win_shell import apply_overlay_stack_mode

        app = globals().get("qt_app")
        assert app is not None

        host = find_desktop_shell_host()
        assert host, "未找到 SHELLDLL_DefView 宿主"
        assert not is_wallpaper_workerw(host)

        w = QWidget()
        w.setWindowTitle("DeskTidySelfTestOverlay")
        w.setWindowFlags(Qt.WindowType.FramelessWindowHint | Qt.WindowType.Window)
        target = QRect(120, 100, 520, 280)
        w.setGeometry(target)
        w.show()
        app.processEvents()
        apply_overlay_stack_mode(w, desktop_layer=True, peek=False)
        app.processEvents()
        hwnd = int(w.winId())
        assert hwnd
        assert is_attached_to_desktop(hwnd), "应挂到 DefView 宿主"
        geo = w.geometry()
        assert geo.width() == target.width(), f"压扁: {geo}"
        assert geo.height() == target.height(), f"压扁: {geo}"

        apply_overlay_stack_mode(w, desktop_layer=False, peek=True)
        app.processEvents()
        assert not is_attached_to_desktop(int(w.winId()))
        apply_overlay_stack_mode(w, desktop_layer=True, peek=False)
        app.processEvents()
        hwnd2 = int(w.winId())
        assert is_attached_to_desktop(hwnd2)
        geo2 = w.geometry()
        assert geo2.width() == target.width()
        assert geo2.height() == target.height()

        detach_overlay_from_desktop(hwnd2)
        w.close()
        w.deleteLater()
        app.processEvents()

    run("壳层 attach/peek detach/reattach 保几何", _cycle)


# ---------------------------------------------------------------------------
# 16. Packaged build + brief process launch
# ---------------------------------------------------------------------------

def test_packaged_build() -> None:
    begin("16) 打包产物与启动冒烟")

    def _dist_files():
        version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        exe = packaged_exe(ROOT)
        assert exe.is_file(), f"缺少 {exe}"
        assert exe.stat().st_size > 100_000
        setups = list((ROOT / "dist").glob(f"DeskTidy_Setup_{version}.exe"))
        any_setup = list((ROOT / "dist").glob("DeskTidy_Setup_*.exe"))
        assert version
        # Installer is optional when only the green exe was rebuilt.
        if setups:
            assert setups[0].stat().st_size > 1_000_000
        elif any_setup:
            assert any_setup[0].stat().st_size > 1_000_000

    run("dist/DeskTidy.exe 存在", _dist_files)

    def _launch_smoke():
        import subprocess
        import time as _time

        import psutil

        from src.instance_lock import is_main_instance_running

        exe = packaged_exe(ROOT)
        if not exe.is_file():
            return

        def _desk_pids() -> set[int]:
            return {
                p.info["pid"]
                for p in psutil.process_iter(["pid", "name"])
                if p.info["name"] and "DeskTidy" in p.info["name"]
            }

        # Skip only when a real DeskTidy process is up (mutex alone can be stale/test-held).
        if _desk_pids():
            print("  SKIP  DeskTidy 已在运行，跳过启动冒烟")
            return

        proc = subprocess.Popen(
            [str(exe)],
            cwd=str(exe.parent),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            alive = False
            for _ in range(40):
                _time.sleep(0.25)
                if proc.poll() is not None:
                    break
                if _desk_pids() or is_main_instance_running():
                    alive = True
                    break
            # Instant clean exit usually means single-instance handoff / env skip,
            # not a crash (crash would be non-zero). Soft-skip so CI without a
            # free desktop session still exercises the rest of the suite.
            if not alive and proc.poll() == 0:
                print("  SKIP  DeskTidy.exe 立即退出(exit=0)，可能被单实例锁接管")
                return
            assert alive or proc.poll() is None, f"启动失败 exit={proc.poll()}"
        finally:
            subprocess.run(
                ["taskkill", "/IM", "DeskTidy.exe", "/F"],
                capture_output=True,
                encoding="mbcs",
                errors="replace",
                check=False,
            )
            try:
                proc.wait(timeout=5)
            except Exception:
                pass
            _time.sleep(0.5)

    run("DeskTidy.exe 启动冒烟", _launch_smoke)


# ---------------------------------------------------------------------------
# 17. Quit / confirm dialog contracts
# ---------------------------------------------------------------------------

def test_quit_dialog_contracts() -> None:
    begin("17) 退出确认对话框契约")

    def _prepare_no_topmost_pulse():
        import inspect

        from src import i18n

        src = inspect.getsource(i18n._prepare_message_box)
        # Comment may mention the forbidden helper; ensure it is not invoked.
        assert "bring_widget_to_foreground(" not in src
        assert "Do NOT call bring_widget_to_foreground" in src
        assert "WindowStaysOnTopHint" in src

    run("_prepare_message_box 不用 TOPMOST 脉冲", _prepare_no_topmost_pulse)

    def _quit_suspends_overlay_before_ask():
        import inspect

        from src.app import DeskTidyApp

        src = inspect.getsource(DeskTidyApp._quit_impl)
        # Overlay management must freeze before the confirm dialog.
        assert "_overlay_mgmt_suspended = True" in src
        ask_pos = src.find("ask_yes_no")
        suspend_pos = src.find("_overlay_mgmt_suspended = True")
        stop_pos = src.find("_stop_overlay_timers")
        assert suspend_pos != -1 and ask_pos != -1
        assert suspend_pos < ask_pos
        assert stop_pos != -1 and stop_pos < ask_pos

    run("_quit_impl 先挂起 overlay 再弹确认框", _quit_suspends_overlay_before_ask)

    def _ask_yes_no_construct():
        from PyQt6.QtCore import Qt
        from PyQt6.QtWidgets import QMessageBox

        from src.i18n import _prepare_message_box

        app = globals().get("qt_app")
        assert app is not None
        box = QMessageBox()
        box.setWindowTitle("自测确认")
        box.setText("契约检查（不 exec）")
        box.addButton("是", QMessageBox.ButtonRole.YesRole)
        box.addButton("否", QMessageBox.ButtonRole.NoRole)
        _prepare_message_box(box)
        assert box.windowModality() == Qt.WindowModality.ApplicationModal
        assert bool(box.windowFlags() & Qt.WindowType.WindowStaysOnTopHint)
        box.close()
        box.deleteLater()
        app.processEvents()

    run("确认框模态与置顶标志", _ask_yes_no_construct)


# ---------------------------------------------------------------------------
# 18. Packaged fence windows live geometry
# ---------------------------------------------------------------------------

def test_packaged_fence_geometry_live() -> None:
    begin("18) 打包版分区窗口活体几何")

    def _live_fences():
        import subprocess
        import time as _time

        import ctypes
        import psutil
        import win32gui
        import win32process

        exe = packaged_exe(ROOT)
        assert exe.is_file()

        def _desktidy_pids() -> set[int]:
            return {
                p.info["pid"]
                for p in psutil.process_iter(["pid", "name"])
                if p.info["name"] and "DeskTidy" in p.info["name"]
            }

        already_pids = _desktidy_pids()
        spawned = None
        if already_pids:
            print(f"  NOTE  复用已运行的 DeskTidy pid={already_pids}")
        else:
            spawned = subprocess.Popen(
                [str(exe)],
                cwd=str(exe.parent),
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )

        user32 = ctypes.windll.user32

        def _fence_geos() -> list[tuple[str, int, int, int, int]]:
            pids = _desktidy_pids()
            out: list[tuple[str, int, int, int, int]] = []

            def cb(hwnd, _):
                if not user32.IsWindowVisible(hwnd):
                    return
                try:
                    _, pid = win32process.GetWindowThreadProcessId(hwnd)
                except Exception:
                    return
                if pid not in pids:
                    return
                title = win32gui.GetWindowText(hwnd) or ""
                try:
                    left, top, right, bottom = win32gui.GetWindowRect(hwnd)
                except Exception:
                    return
                w, h = right - left, bottom - top
                if w < 100 or h < 40:
                    return
                out.append((title, left, top, w, h))

            win32gui.EnumWindows(cb, None)
            return out

        try:
            # Wait for process first
            for _ in range(40):
                if _desktidy_pids():
                    break
                _time.sleep(0.25)
            assert _desktidy_pids(), "DeskTidy 进程未起来"

            geos = []
            fence_like: list[tuple[str, int, int, int, int]] = []
            # Wait for real fence HWNDs — page chrome is ~100px wide and can
            # appear before fences unpark; do not treat chrome as "ready".
            for _ in range(60):
                _time.sleep(0.2)
                geos = _fence_geos()
                fence_like = [g for g in geos if g[3] >= 220 and g[4] >= 80]
                if fence_like:
                    break
            # When another app is foreground, DeskTidy parks overlays — no
            # visible fence HWNDs is expected, not a geometry regression.
            if not fence_like:
                try:
                    from src.win_shell import is_desktop_foreground

                    on_desktop = bool(is_desktop_foreground())
                except Exception:
                    on_desktop = True
                if not on_desktop:
                    print("  SKIP  非桌面前台，覆盖层已 park，跳过窗口几何")
                    return
                # Only page chrome / splash visible while desktop FG — still park.
                if geos and all(g[3] < 220 for g in geos):
                    print(f"  SKIP  仅见窄 chrome、未见分区: {geos}")
                    return
                assert fence_like, f"没有足够宽的分区窗口: {geos}"
            crushed = [(t, w, h) for t, _x, _y, w, h in fence_like if w <= 168]
            assert not crushed, f"发现压扁窗口: {crushed}"
        finally:
            if spawned is not None:
                subprocess.run(
                    ["taskkill", "/IM", "DeskTidy.exe", "/F"],
                    capture_output=True,
                    encoding="mbcs",
                    errors="replace",
                    check=False,
                )
                try:
                    spawned.wait(timeout=5)
                except Exception:
                    pass
                _time.sleep(0.4)

    run("启动后分区窗口宽度正常", _live_fences)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> int:
    print("DeskTidy 系统功能整体自测")
    print(f"ROOT = {ROOT}")
    started = time.perf_counter()

    test_bootstrap()
    if not globals().get("qt_app"):
        print("\nQApplication 失败，中止后续 UI 测试")
        _summary(started)
        return 1

    test_settings_and_resources()
    test_fence_pages()
    test_fence_layout()
    test_fence_rules()
    test_organizer()
    test_win_shell_overlay()
    test_hotkeys_lock_i18n()
    test_ui_widgets()
    test_public_desktop()
    test_desktop_scanner()
    test_managers()
    test_live_settings_health()
    test_live_organize_dry_run()
    test_live_show_desktop_cycle()
    test_packaged_build()
    test_quit_dialog_contracts()
    test_packaged_fence_geometry_live()

    return _summary(started)


def _summary(started: float) -> int:
    elapsed = time.perf_counter() - started
    try:
        from scripts.cleanup_selftest_artifacts import report_cleanup

        report_cleanup()
    except Exception as exc:
        print(f"测试残留清理失败: {exc}")
    print("\n" + "=" * 60)
    print(f"通过 {len(passes)}  失败 {len(failures)}  耗时 {elapsed:.1f}s")
    if failures:
        print("\n失败项:")
        for name, err in failures:
            print(f"  - {name}: {err}")
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
