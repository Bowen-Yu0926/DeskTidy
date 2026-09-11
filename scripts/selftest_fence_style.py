"""Fence style presets / preview / apply contracts."""

from __future__ import annotations

import inspect
import sys
import traceback
from pathlib import Path

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


def test_preset_catalog() -> None:
    from src.fence_style import (
        DEFAULT_FENCE_STYLE_PRESET,
        FENCE_STYLE_PRESETS,
        apply_preset_to_settings,
        fence_style_visual_from_preset,
        merge_fence_style_visual,
        normalize_fence_style_preset,
    )

    assert DEFAULT_FENCE_STYLE_PRESET in FENCE_STYLE_PRESETS
    assert len(FENCE_STYLE_PRESETS) >= 4
    for key, meta in FENCE_STYLE_PRESETS.items():
        assert meta.get("name")
        visual = fence_style_visual_from_preset(key)
        assert visual["background"].startswith("#")
        assert visual["accent"].startswith("#")
        assert 0.35 <= float(visual["opacity"]) <= 1.0
    assert normalize_fence_style_preset("nope") == DEFAULT_FENCE_STYLE_PRESET

    style = {
        "opacity": 0.5,
        "background": "#111111",
        "accent": "#FFFFFF",
        "border_radius": 4,
        "view_mode": "list",
        "show_title": False,
        "collapsible": False,
    }
    merged = merge_fence_style_visual(style, fence_style_visual_from_preset("frost"))
    assert merged["view_mode"] == "list"
    assert merged["show_title"] is False
    assert merged["background"] == FENCE_STYLE_PRESETS["frost"]["background"].upper() or merged[
        "background"
    ].upper() == FENCE_STYLE_PRESETS["frost"]["background"].upper()

    settings = {
        "fences": [
            {"id": "a", "style": dict(style)},
            {"id": "b", "style": {"view_mode": "grid", "show_title": True}},
        ]
    }
    assert apply_preset_to_settings(settings, "charcoal") is True
    assert settings["fence_style_preset"] == "charcoal"
    assert settings["fences"][0]["style"]["view_mode"] == "list"
    assert settings["fences"][0]["style"]["background"].upper() == "#2A2E35"
    assert settings["fences"][0].get("style_preset") == "charcoal"
    assert settings["fences"][1]["style"]["view_mode"] == "grid"

    from src.fence_style import match_fence_style_preset

    assert match_fence_style_preset(settings["fences"][0]) == "charcoal"

    # Legacy upgrade: missing preset + light backgrounds → charcoal.
    from src.fence_style import migrate_legacy_fence_styles

    legacy = {
        "fences": [
            {"id": "x", "style": {"background": "#FFFFFF", "view_mode": "grid"}},
        ]
    }
    merged = dict(legacy)
    assert migrate_legacy_fence_styles({}, merged) is True
    assert merged["fence_style_preset"] == "charcoal"
    assert merged["fences"][0]["style"]["background"].upper() == "#2A2E35"
    # Already keyed — do not rewrite custom dark styles.
    custom = {
        "fence_style_preset": "forest",
        "fences": [
            {"id": "y", "style": {"background": "#14532D", "view_mode": "grid"}},
        ],
    }
    assert migrate_legacy_fence_styles(custom, dict(custom)) is False


def test_fence_widget_honors_light_style() -> None:
    import os

    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PyQt6.QtWidgets import QApplication

    from src.ui.fence_widget import FenceWidget

    app = QApplication.instance() or QApplication([])
    cfg = {
        "id": "style_test",
        "name": "样式",
        "folder": "样式",
        "x": 0,
        "y": 0,
        "width": 320,
        "height": 200,
        "style": {
            "opacity": 0.52,
            "background": "#F4F6F8",
            "accent": "#3B82F6",
            "border_radius": 8,
            "show_title": True,
            "view_mode": "grid",
            "collapsible": True,
        },
    }
    fence = FenceWidget(cfg, settings={})
    css = fence.container.styleSheet()
    assert "244, 246, 248" in css or "248, 250, 252" in css or "#F4F6F8" in css.upper()
    assert "8px" in css or "14px" in css
    # Light panels use dark icon captions (white-on-mint was unreadable).
    assert "QLabel#fenceItem" in css
    item_block = css.split("QLabel#fenceItem")[1].split("}")[0]
    assert "#0F172A" in item_block.upper() or "#0f172a" in item_block
    assert "HEADER_OVERLAY" in inspect.getsource(FenceWidget._build_ui) or True
    build_src = inspect.getsource(FenceWidget._build_ui)
    assert "HEADER_OVERLAY_HEIGHT" in build_src
    assert "_sync_body_top_inset" in inspect.getsource(FenceWidget._apply_style)
    # Dark preset still works — white captions on dark panels.
    cfg2 = dict(cfg)
    cfg2["style"] = {
        "opacity": 0.70,
        "background": "#2A2E35",
        "accent": "#4C8DFF",
        "border_radius": 8,
        "show_title": True,
        "view_mode": "grid",
        "collapsible": True,
    }
    fence.apply_config(cfg2, refresh_icons=False, apply_style=True)
    css2 = fence.container.styleSheet()
    assert "42, 46, 53" in css2 or "32, 36, 44" in css2
    item_block2 = css2.split("QLabel#fenceItem")[1].split("}")[0]
    assert "#FFFFFF" in item_block2.upper() or "#ffffff" in item_block2
    fence.close()
    fence.deleteLater()
    app.processEvents()


def test_style_cache_is_detached_from_config() -> None:
    """Mutating fence._style must not rewrite settings via a shared dict alias."""
    import os

    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PyQt6.QtWidgets import QApplication

    from src.ui.fence_widget import FenceWidget

    app = QApplication.instance() or QApplication([])
    style = {
        "opacity": 0.7,
        "background": "#2A2E35",
        "accent": "#4C8DFF",
        "border_radius": 8,
        "show_title": True,
        "view_mode": "grid",
        "collapsible": True,
    }
    cfg = {
        "id": "detach",
        "name": "常用",
        "folder": "常用",
        "x": 0,
        "y": 0,
        "width": 300,
        "height": 200,
        "style": style,
    }
    fence = FenceWidget(cfg, settings={})
    assert fence._style is not style
    assert fence._style is not cfg["style"]
    fence._style["background"] = "#DEADBE"
    assert cfg["style"]["background"].upper() == "#2A2E35"
    fence.apply_config(cfg, refresh_icons=False, apply_style=True)
    assert fence._style is not cfg["style"]
    fence.close()
    fence.deleteLater()
    app.processEvents()


def test_picker_click_updates_live_stylesheet() -> None:
    """Selecting a pack must change the live fence container QSS (not only settings)."""
    import os

    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    from PyQt6.QtWidgets import QApplication

    from src.fence_style import apply_preset_to_settings, normalize_fence_style_preset
    from src.ui.fence_style_picker import FenceStylePickerWidget
    from src.ui.fence_widget import FenceWidget

    app = QApplication.instance() or QApplication([])
    settings = {
        "fence_style_preset": "charcoal",
        "fences": [
            {
                "id": "a",
                "name": "常用",
                "folder": "常用",
                "x": 0,
                "y": 0,
                "width": 300,
                "height": 200,
                "style": {
                    "opacity": 0.7,
                    "background": "#2A2E35",
                    "accent": "#4C8DFF",
                    "border_radius": 8,
                    "show_title": True,
                    "view_mode": "grid",
                    "collapsible": True,
                },
            }
        ],
    }
    fence = FenceWidget(settings["fences"][0], settings=settings)

    def preview(visual: dict) -> None:
        from src.fence_style import merge_fence_style_visual

        # Mirror App: always assign a detached dict so re-picks stay independent.
        fence._style = dict(merge_fence_style_visual(fence._style, visual))
        fence._apply_style()

    def apply(preset_id: str) -> None:
        key = normalize_fence_style_preset(preset_id)
        apply_preset_to_settings(settings, key)
        cfg = settings["fences"][0]
        fence.config = cfg
        fence.apply_config(cfg, refresh_icons=False, apply_style=True)
        flush = getattr(fence, "_flush_style_paint", None)
        if callable(flush):
            flush()

    picker = FenceStylePickerWidget(settings)
    picker.style_preview_requested.connect(preview)
    picker.style_apply_requested.connect(apply)
    picker._on_preset_clicked("frost")
    css = fence.container.styleSheet()
    assert settings["fence_style_preset"] == "frost"
    assert settings["fences"][0]["style"]["background"].upper() == "#F4F6F8"
    assert "244, 246, 248" in css
    picker._on_preset_clicked("sunset")
    assert "248, 213, 181" in fence.container.styleSheet()
    # Third pick after apply still works (re-selectability).
    picker._on_preset_clicked("charcoal")
    assert "42, 46, 53" in fence.container.styleSheet() or "32, 36, 44" in fence.container.styleSheet()
    fence.close()
    fence.deleteLater()
    app.processEvents()


def test_ui_and_app_wiring() -> None:
    from src.app import DeskTidyApp
    from src.ui import desktop_layout_widget, fence_style_picker, main_window
    from src.ui.styles import fence_theme_defaults
    from PyQt6.QtCore import QPoint, QRect
    from PyQt6.QtWidgets import QApplication

    defaults = fence_theme_defaults()
    assert defaults["background"].upper() in ("#20242C", "#2A2E35")
    # Global picker removed from layout — presets live on each fence card.
    dl_src = inspect.getsource(desktop_layout_widget.DesktopLayoutWidget)
    assert "FenceStylePickerWidget" not in dl_src
    assert "fence_style_apply_one_requested" in dl_src
    assert "fence_opacity_changed" in dl_src
    assert "_fence_chip_rects" in inspect.getsource(desktop_layout_widget)
    assert "_apply_fence_style_preset" in dl_src
    assert "_paint_opacity_slider" in inspect.getsource(desktop_layout_widget)
    picker_src = inspect.getsource(fence_style_picker.FenceStylePickerWidget)
    assert "style_preview_requested" in picker_src
    assert "style_apply_requested" in picker_src
    assert "WA_TransparentForMouseEvents" in inspect.getsource(
        fence_style_picker.FenceStylePresetCard
    )
    # Preview must not write fence.config (shared with settings) — that locked re-picks.
    preview_src = inspect.getsource(DeskTidyApp._preview_fence_style)
    assert 'fence.config["style"]' not in preview_src
    apply_src = inspect.getsource(DeskTidyApp._apply_fence_style_preset)
    assert "apply_config" in apply_src
    one_src = inspect.getsource(DeskTidyApp._apply_fence_style_preset_one)
    assert "apply_preset_to_fence_config" in one_src
    assert "apply_config" in one_src
    assert "_apply_fence_opacity_one" in inspect.getsource(DeskTidyApp)
    opac_src = inspect.getsource(DeskTidyApp._apply_fence_opacity_one)
    assert "apply_config" in opac_src
    assert "opacity" in opac_src
    # Icon captions adapt: dark ink on light panels, white on dark.
    style_src = inspect.getsource(
        __import__("src.ui.fence_widget", fromlist=["x"]).FenceWidget._apply_style
    )
    assert 'item_css = "#0F172A"' in style_src or "item_css = '#0F172A'" in style_src
    assert "item_css = _DEFAULT_FENCE_TEXT_COLOR" in style_src
    assert "_flush_style_paint" in style_src
    assert "_sync_body_top_inset" in style_src
    flush_src = inspect.getsource(
        __import__("src.ui.fence_widget", fromlist=["x"]).FenceWidget._flush_style_paint
    )
    # Translucent overlays: use redraw_overlay_hwnd (no RDW_ERASE), never raw erase.
    assert "redraw_overlay_hwnd" in flush_src
    assert "0x0004" not in flush_src
    assert "windowHandle" in flush_src
    # Style-only paths must not restack (SWP_NOREDRAW discards the new QSS).
    assert "ensure_live_fences_interactive" not in preview_src
    assert "ensure_live_fences_interactive" not in apply_src
    build_src = inspect.getsource(
        __import__("src.ui.fence_widget", fromlist=["x"]).FenceWidget._build_ui
    )
    assert "WA_StyledBackground" in build_src
    assert "_flush_style_paint" in apply_src
    assert "_flush_style_paint" in preview_src
    # Widget paint cache must be a detached dict — shared alias locked re-picks.
    init_src = inspect.getsource(
        __import__("src.ui.fence_widget", fromlist=["x"]).FenceWidget.__init__
    )
    assert "self._style = dict(config.get(\"style\")" in init_src.replace("'", '"')
    mw = inspect.getsource(main_window.MainWindow)
    assert "fence_style_preview_requested" in mw
    assert "fence_style_apply_requested" in mw
    assert "fence_style_apply_one_requested" in mw
    assert "fence_opacity_changed" in mw
    assert "fence_opacity_changed" in inspect.getsource(main_window.MainWindow._build_fences_page)
    app_src = inspect.getsource(DeskTidyApp)
    assert "_preview_fence_style" in app_src
    assert "_apply_fence_style_preset" in app_src
    assert "_apply_fence_style_preset_one" in app_src
    assert "_revert_fence_style_preview" in app_src
    edit = inspect.getsource(
        __import__("src.ui.fence_editor", fromlist=["x"]).FenceEditDialog
    )
    assert "外观预设" in edit
    assert "QColorDialog" in edit

    # Card chip hit-test + per-fence apply (settings only).
    app = QApplication.instance() or QApplication([])
    from src.fence_style import FENCE_STYLE_PRESETS, match_fence_style_preset
    from src.ui.desktop_layout_widget import DesktopLayoutWidget, _fence_chip_rects, _thumb_rect

    settings = {
        "current_page": 0,
        "desktop_pages": [{"id": 0, "name": "工作"}],
        "fences": [
            {
                "id": "f0",
                "name": "常用",
                "pages": [0],
                "visible": True,
                "style": {
                    "opacity": 0.7,
                    "background": "#2A2E35",
                    "accent": "#4C8DFF",
                    "view_mode": "grid",
                },
                "style_preset": "charcoal",
            }
        ],
        "show_page_indicator": True,
        "theme": "sky",
    }
    w = DesktopLayoutWidget(settings)
    w.select_page_id(0)
    app.processEvents()
    assert w.fence_view.count() == 2  # fence card + trailing add card
    item = w.fence_view.item(0)
    card = item.listWidget().visualItemRect(item)
    # Force a known geometry for chip hit testing (matches current card metrics).
    from src.ui.desktop_layout_widget import (
        _FENCE_H,
        _FENCE_W,
        _THUMB_H,
        _THUMB_W,
        _fence_opacity_track_rect,
        _opacity_from_track_x,
    )

    thumb = _thumb_rect(
        QRect(0, 0, _FENCE_W, _FENCE_H), thumb_w=_THUMB_W, thumb_h=_THUMB_H
    )
    chips = _fence_chip_rects(thumb)
    assert set(chips) == set(FENCE_STYLE_PRESETS)
    track = _fence_opacity_track_rect(thumb)
    assert track.width() > 40
    assert abs(_opacity_from_track_x(track, track.left()) - 0.35) < 0.02
    assert abs(_opacity_from_track_x(track, track.right()) - 1.0) < 0.02
    assert match_fence_style_preset(settings["fences"][0]) == "charcoal"
    w._apply_fence_style_preset("f0", "forest")
    assert settings["fences"][0]["style_preset"] == "forest"
    assert settings["fences"][0]["style"]["background"].upper() == "#C8EBD6"
    # Second fence can keep a different preset when applied separately.
    settings["fences"].append(
        {
            "id": "f1",
            "name": "其他",
            "pages": [0],
            "visible": True,
            "style": dict(settings["fences"][0]["style"]),
            "style_preset": "forest",
        }
    )
    w._apply_fence_style_preset("f1", "sunset")
    assert settings["fences"][0]["style_preset"] == "forest"
    assert settings["fences"][1]["style_preset"] == "sunset"
    w.close()
    w.deleteLater()
    app.processEvents()
    _ = QPoint  # keep import used for future hit tests / lint


def main() -> int:
    print("DeskTidy fence style presets")
    run("预设目录与合并", test_preset_catalog)
    run("分区吃浅色样式", test_fence_widget_honors_light_style)
    run("样式缓存与 config 解耦", test_style_cache_is_detached_from_config)
    run("点选预设更新现场 QSS", test_picker_click_updates_live_stylesheet)
    run("设置页与 App 接线", test_ui_and_app_wiring)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
