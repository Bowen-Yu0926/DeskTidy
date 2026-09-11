"""Selftest: desktop pet module + wiring."""

from __future__ import annotations

import inspect
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
import scripts.selftest_env  # noqa: F401, E402

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")


def main() -> None:
    data = json.loads((ROOT / "config" / "default_settings.json").read_text(encoding="utf-8"))
    assert "desktop_pet" in data
    assert data["desktop_pet"].get("enabled") is True
    assert data["desktop_pet"].get("character") == "hoodie"
    assert data["desktop_pet"].get("auto_wander") is True
    assert data["desktop_pet"].get("show_page_bubbles") is True

    assert data["desktop_pet"].get("enabled_actions") == ["pet", "wait", "fall"]
    by_char = data["desktop_pet"].get("enabled_actions_by_character")
    assert isinstance(by_char, dict)
    assert set(by_char.keys()) == {"hoodie", "voyage"}
    assert by_char["hoodie"] == ["pet", "wait", "sleep", "fall"]
    assert float(data["desktop_pet"].get("size_scale", 1.0)) == 1.0

    from src.desktop_pet import (
        desktop_pet_enabled,
        desktop_pet_hosts_float_bar,
        desktop_pet_settings,
        desktop_pet_shows_page_bubbles,
        desktop_pet_visible,
        float_bar_tool_flags,
        normalize_pet_character,
        normalize_pet_enabled_actions,
        normalize_pet_size_scale,
        pet_action_enabled,
        pet_action_lines,
        pet_catalog,
        pet_character_ids,
        pet_enabled_actions,
        pet_lines,
        pet_size_scale,
        pet_sprite_path,
        random_pet_line,
    )

    assert len(pet_character_ids()) == 2
    catalog = pet_catalog()
    assert set(catalog) == {"hoodie", "voyage"}
    assert normalize_pet_character("gorilla") == "hoodie"
    assert normalize_pet_character("weasel") == "hoodie"
    assert normalize_pet_character("frog") == "hoodie"
    assert normalize_pet_character("pig") == "hoodie"
    assert normalize_pet_character("langfrog") == "hoodie"
    assert normalize_pet_character("duoduo") == "hoodie"
    assert normalize_pet_character("hoodie") == "hoodie"
    assert normalize_pet_character("voyage") == "voyage"
    for char_id in pet_character_ids():
        path = pet_sprite_path(char_id)
        assert path.is_file(), f"missing sprite: {path}"
        for pose in ("idle", "walk", "stand"):
            pose_path = pet_sprite_path(char_id, pose)
            assert pose_path.is_file(), f"missing pose {pose}: {pose_path}"
    # Multi-frame sleep/play for all characters.
    for char_id in pet_character_ids():
        assert pet_sprite_path(char_id, "sleep").is_file(), f"missing sleep: {char_id}"
        assert pet_sprite_path(char_id, "sleep_0").is_file(), f"missing sleep_0: {char_id}"
        assert pet_sprite_path(char_id, "play_0").is_file(), f"missing play_0: {char_id}"
    assert pet_sprite_path("hoodie", "wait").is_file()
    assert pet_sprite_path("hoodie", "wait_0").is_file()
    assert pet_sprite_path("voyage", "wait").is_file()
    assert pet_sprite_path("voyage", "wait_0").is_file()
    assert pet_sprite_path("voyage", "play_0").is_file()
    assert pet_sprite_path("voyage", "fall_0").is_file()
    assert pet_sprite_path("voyage", "trash_7").is_file()
    assert Path(__file__).resolve().parents[1].joinpath(
        "scripts", "import_voyage_play_frames.py"
    ).is_file()
    from PyQt6.QtWidgets import QApplication

    _ = QApplication.instance() or QApplication([])
    from src.ui.pet_anim import PetAnimState, load_pet_poses, pose_for_state

    for char_id in pet_character_ids():
        poses = load_pet_poses(char_id)
        assert len(poses["sleep"]) >= 2, char_id
        assert len(poses["play"]) >= 2, char_id
    hoodie_poses = load_pet_poses("hoodie")
    voyage_poses = load_pet_poses("voyage")
    import numpy as _np
    from PIL import Image as _Image
    from src.ui.pet_anim import invalidate_pet_pose_cache, pose_fit_span, _fit_sprite

    # Blank idle_0 (1024×2048 transparent) used to poison idle span → standing speck.
    _idle0_a = _np.array(_Image.open(pet_sprite_path("hoodie", "idle_0")).convert("RGBA"))[:, :, 3]
    assert float((_idle0_a > 24).mean()) > 0.05, "hoodie_idle_0 must not be a blank placeholder"
    _idle_span = pose_fit_span("hoodie", "idle")
    assert _idle_span is not None and 200 <= _idle_span <= 600, _idle_span
    _idle_fit = _fit_sprite(
        hoodie_poses["idle"][0],
        172,
        220,
        1.0,
        pose="idle",
        character="hoodie",
        fixed_content_span=_idle_span,
    )
    assert _idle_fit.height() >= 140, (_idle_fit.width(), _idle_fit.height())

    assert load_pet_poses("hoodie") is hoodie_poses
    invalidate_pet_pose_cache("hoodie")
    assert len(hoodie_poses["wait"]) == 1, len(hoodie_poses["wait"])

    _wa = _np.array(_Image.open(pet_sprite_path("hoodie", "wait_0")).convert("RGBA"))[:, :, 3]
    _ys = _np.where(_wa > 24)[0]
    assert int(_wa.shape[0] - int(_ys.max()) - 1) <= 20, "hoodie wait foot should sit on canvas bottom"
    assert len(voyage_poses["wait"]) >= 8, len(voyage_poses["wait"])
    assert len(voyage_poses["play"]) >= 4, len(voyage_poses["play"])
    assert len(hoodie_poses["trash"]) >= 12, len(hoodie_poses["trash"])
    assert len(hoodie_poses["trash"]) <= 16, len(hoodie_poses["trash"])
    # Wait page-turn holds each cel longer than sleep breath.
    from src.ui.pet_anim import (
        _TRASH_TICKS_PER_FRAME,
        _WAIT_TICKS_PER_FRAME,
        _frame_index,
        _loop_ticks,
        _wait_ticks_per_frame,
    )

    assert _WAIT_TICKS_PER_FRAME >= 8
    assert _wait_ticks_per_frame("hoodie") == _WAIT_TICKS_PER_FRAME
    assert _wait_ticks_per_frame("voyage") > _wait_ticks_per_frame("hoodie")
    from src.ui.pet_anim import _CHEER_TICKS_BY_CHARACTER, _pose_cels

    assert _CHEER_TICKS_BY_CHARACTER["voyage"] > _loop_ticks("happy", 4, character="hoodie")
    eff_play = _pose_cels(voyage_poses["play"], "happy", "voyage")
    assert len(eff_play) >= 2
    if len(voyage_poses["play"]) >= 6:
        assert len(eff_play) == len(voyage_poses["play"]) - 2
    else:
        assert len(eff_play) == len(voyage_poses["play"])
    assert _loop_ticks("happy", 6, character="voyage") > _loop_ticks("happy", 4, character="hoodie")
    assert _frame_index(hoodie_poses["wait"], 0, "wait", character="hoodie") == 0
    assert _frame_index(hoodie_poses["wait"], _WAIT_TICKS_PER_FRAME - 1, "wait", character="hoodie") == 0
    assert _frame_index(hoodie_poses["wait"], _WAIT_TICKS_PER_FRAME, "wait", character="hoodie") == 0
    voyage_wait_ticks = _loop_ticks("wait", len(voyage_poses["wait"]), character="voyage")
    hoodie_wait_ticks = _loop_ticks("wait", len(hoodie_poses["wait"]), character="hoodie")
    assert voyage_wait_ticks > hoodie_wait_ticks
    assert _frame_index(voyage_poses["wait"], voyage_wait_ticks - 1, "wait", character="voyage") == 0
    assert _frame_index(voyage_poses["wait"], voyage_wait_ticks, "wait", character="voyage") == 1
    from src.ui.pet_anim import _cel_crossfade

    i0, i1, tv = _cel_crossfade(voyage_poses["wait"], voyage_wait_ticks - 1, "wait", character="voyage")
    assert i0 == i1 == 0 and tv >= 0.99
    from src.ui.pet_anim import pose_fit_span, invalidate_pet_pose_cache, load_pet_poses

    invalidate_pet_pose_cache("voyage")
    load_pet_poses("voyage")
    voyage_wait_span = pose_fit_span("voyage", "wait")
    assert voyage_wait_span is not None and voyage_wait_span >= 250
    invalidate_pet_pose_cache("hoodie")
    load_pet_poses("hoodie")
    hoodie_wait_span = pose_fit_span("hoodie", "wait")
    assert hoodie_wait_span is not None and hoodie_wait_span >= 180
    from src.ui.pet_anim import _HOODIE_WAIT_FIT_SPAN_MULT, _effective_fit_span, _fit_sprite

    _hw = hoodie_poses["wait"][0]
    _dest_w = int(round(180 * 1.55))
    _dest_h = 220
    _idle_span = pose_fit_span("hoodie", "idle")
    _idle0 = hoodie_poses["idle"][0]
    _idle_fit = _fit_sprite(
        _idle0,
        172,
        _dest_h,
        1.0,
        pose="idle",
        character="hoodie",
        fixed_content_span=_idle_span,
    )
    _eff = _effective_fit_span("hoodie", "wait")
    # Must use wait's own canvas span — idle's 2k span shrinks wait to ~30px.
    assert _eff == int(round(hoodie_wait_span * _HOODIE_WAIT_FIT_SPAN_MULT))
    assert _idle_span is None or _eff != int(round(_idle_span * _HOODIE_WAIT_FIT_SPAN_MULT)) or (
        hoodie_wait_span == _idle_span
    )
    _fitted_wait = _fit_sprite(
        _hw, _dest_w, _dest_h, 1.0, pose="wait", character="hoodie", fixed_content_span=_eff
    )
    assert _fitted_wait.width() <= _dest_w * 1.55 + 2, _fitted_wait.width()
    assert _fitted_wait.height() <= _dest_h + 2, _fitted_wait.height()
    assert _fitted_wait.height() >= 80, (_fitted_wait.width(), _fitted_wait.height())
    assert abs(_fitted_wait.height() - _idle_fit.height()) <= 40, (
        _fitted_wait.height(),
        _idle_fit.height(),
    )
    _sleep_eff = _effective_fit_span("hoodie", "sleep")
    _sleep_own = pose_fit_span("hoodie", "sleep")
    from src.ui.pet_anim import _HOODIE_SLEEP_FIT_SPAN_MULT, _opaque_bbox

    assert _sleep_eff is not None and _sleep_own is not None
    assert _sleep_eff == int(round(_sleep_own * _HOODIE_SLEEP_FIT_SPAN_MULT))
    # Sleep span is height-only (not max(w,h)) so prone pillow matches idle size.
    _sleep0 = hoodie_poses["sleep"][0]
    _sleep_bb = _opaque_bbox(_sleep0)
    assert abs(_sleep_own - _sleep_bb.height()) <= 2, (
        _sleep_own,
        _sleep_bb.height(),
        _sleep_bb.width(),
    )
    _dest_sleep_w = int(round(180 * 1.42))
    _fitted_sleep = _fit_sprite(
        _sleep0,
        _dest_sleep_w,
        _dest_h,
        1.0,
        pose="sleep",
        character="hoodie",
        fixed_content_span=_sleep_eff,
    )
    assert abs(_fitted_sleep.height() - _idle_fit.height()) <= 55, (
        _fitted_sleep.height(),
        _idle_fit.height(),
    )
    try:
        import numpy as _np
        from PIL import Image as _Image

        opaques = [
            int((_np.array(_Image.open(pet_sprite_path("voyage", f"wait_{wi}")).convert("RGBA"))[:, :, 3] > 24).sum())
            for wi in range(8)
        ]
        assert max(opaques) - min(opaques) < 8_000, opaques

        def _pair_diff(a, b):
            m = (a[:, :, 3] > 24) & (b[:, :, 3] > 24)
            return float(_np.mean(_np.abs(a[:, :, :3][m].astype(float) - b[:, :, :3][m].astype(float)))) if m.any() else 999.0

        waits = [_np.array(_Image.open(pet_sprite_path("voyage", f"wait_{wi}")).convert("RGBA")) for wi in range(8)]
        for wi in range(8):
            wim = waits[wi]
            wa = wim[:, :, 3]
            assert int((wa > 24).sum()) >= 8_000, f"voyage wait_{wi} too empty"
        for wi in range(7):
            assert _pair_diff(waits[wi], waits[wi + 1]) < 55.0, wi
        assert _pair_diff(waits[7], waits[0]) < 55.0
        from scripts.import_voyage_wait_frames import _green_count
        from PIL import Image as _Image2

        for wi in range(8):
            gpx = _green_count(_Image2.open(pet_sprite_path("voyage", f"wait_{wi}")))
            assert gpx < 500, f"voyage wait_{wi} green fringe {gpx}"
    except ImportError:
        pass
    voyage_play_ticks = _loop_ticks("play", len(voyage_poses["play"]), character="voyage")
    assert voyage_play_ticks >= 3
    assert _frame_index(voyage_poses["play"], voyage_play_ticks - 1, "play", character="voyage") == 0
    trash_last = len(hoodie_poses["trash"]) - 1
    from src.ui.pet_anim import _trash_ticks_per_frame

    hoodie_trash_ticks = _trash_ticks_per_frame(character="hoodie")
    assert hoodie_trash_ticks == 10
    # Import pipeline must reject non-green / waist-cropped gens.
    import_src = (ROOT / "scripts" / "import_trash_gen_frames.py").read_text(encoding="utf-8")
    assert "_qa_gen_green" in import_src and "_qa_feet_near_bottom" in import_src
    assert "_MIN_GEN_GREEN_FRAC" in import_src
    assert "_darken_hair_highlights" in import_src
    assert "_fix_chroma_spill" in import_src
    assert "Do NOT treat pure white" in import_src
    story = (ROOT / "scripts" / "hoodie_trash_24_storyboard.py").read_text(encoding="utf-8")
    assert "PLANE" in story and "#00FF00" in story and "FULL_BODY" in story
    assert "pure white" in story.lower() or "#FFFFFF" in story
    assert (
        "NO white hair" in story
        or "SOLID dark black" in story
        or "deep charcoal" in story
        or "dark black-brown" in story
    )
    assert "caramel" in story.lower() or "pale tips" in story.lower()
    # Throw cels must keep the paper plane (import must not chroma-eat white).
    from PIL import Image as _Pil
    import numpy as _np

    for _ti in (4, 5, 6, 7, 8, 9):
        _ta = _np.array(_Pil.open(pet_sprite_path("hoodie", f"trash_{_ti}")).convert("RGBA"))
        _th, _tw = _ta.shape[:2]
        _tr, _tg, _tb, _tal = (
            _ta[:, :, 0].astype(int),
            _ta[:, :, 1].astype(int),
            _ta[:, :, 2].astype(int),
            _ta[:, :, 3],
        )
        _paper = (
            (_tal > 100)
            & (_tr > 190)
            & (_tg > 190)
            & (_tb > 190)
        )
        _upper = _paper.copy()
        _upper[_th // 2 :, :] = False
        if _ti == 4:
            # Wind-up: plane held by the right ear (mid-upper), not yet far UR.
            _held = _upper.copy()
            _held[:, : int(_tw * 0.40)] = False
            assert int(_held.sum()) >= 80, (
                f"trash_4 missing held paper plane ({int(_held.sum())})"
            )
        else:
            _ur = _upper.copy()
            _ur[:, : _tw // 2] = False
            assert int(_ur.sum()) >= 80, (
                f"trash_{_ti} missing upper-right paper plane ({int(_ur.sum())})"
            )
        # Flight cels must keep bright paper mass on the right half (no leftward throw).
        _ys, _xs = _np.where(_upper)
        assert len(_xs) > 0, f"trash_{_ti} no upper paper"
        if _ti >= 5:
            _ur_mass = _upper.copy()
            _ur_mass[:, : _tw // 2] = False
            _uys, _uxs = _np.where(_ur_mass)
            assert len(_uxs) >= 80, f"trash_{_ti} UR plane too small ({len(_uxs)})"
            assert float(_uxs.mean()) / float(_tw) >= 0.55, (
                f"trash_{_ti} plane mass drifts left ({float(_uxs.mean()) / float(_tw):.2f})"
            )
    # Dark hair locked across throw cels (pale regen hair caused 闪烁).
    from scripts.import_trash_gen_frames import _hair_crown_mask as _trash_hair_mask

    for _ti in range(12):
        _ta = _np.array(_Pil.open(pet_sprite_path("hoodie", f"trash_{_ti}")).convert("RGBA"))
        _lum = _ta[:, :, :3].astype(_np.float32).mean(axis=2)
        _crown, _dark, _peak = _trash_hair_mask(_ta)
        assert int(_dark.sum()) >= 2000, f"trash_{_ti} missing dark hair ({int(_dark.sum())})"
        assert float(_lum[_dark].mean()) <= 45.0, (
            f"trash_{_ti} hair too light ({float(_lum[_dark].mean()):.1f})"
        )
    from scripts.import_trash_gen_frames import _crown_half_masses as _trash_crown

    for _ti in range(12):
        _tot, _cl, _cr = _trash_crown(
            _Pil.open(pet_sprite_path("hoodie", f"trash_{_ti}")).convert("RGBA")
        )
        assert _tot >= 80, f"trash_{_ti} crown empty ({_tot})"
        assert _cl >= 25 and _cr >= 25, (
            f"trash_{_ti} crown side missing L={_cl} R={_cr}"
        )
    assert "_unify_trash_hair_tone" in import_src
    assert "_scrub_hair_edge_halo" in import_src
    assert "_qa_hair_not_chalky" in import_src
    assert "_qa_crown_complete" in import_src
    # Chalk-white hair tips must stay below idle-ish budget on every throw cel.
    from scripts.import_trash_gen_frames import _count_pale_hair_px as _trash_pale

    for _ti in range(12):
        _pale, _white = _trash_pale(
            _Pil.open(pet_sprite_path("hoodie", f"trash_{_ti}")).convert("RGBA")
        )
        assert _white <= 320, f"trash_{_ti} chalk-white hair ({_white})"
        assert _pale <= 1600, f"trash_{_ti} pale hair mass ({_pale})"
    _idle_pale, _idle_white = _trash_pale(
        _Pil.open(pet_sprite_path("hoodie", "idle")).convert("RGBA")
    )
    assert _idle_white <= 40, f"idle chalk-white hair ({_idle_white})"
    assert "_lock_sprite_hair" in import_src
    assert "_qa_gen_hair_headroom" in import_src
    assert "_largest_blob_bbox" in import_src
    assert "_scrub_residual_screen_green" in import_src
    from src.ui.pet_widget import DesktopPetWidget

    start_trash = inspect.getsource(DesktopPetWidget._start_file_trash)
    assert "self._facing = 1" in start_trash
    paint_src = inspect.getsource(DesktopPetWidget.paintEvent)
    assert "facing = 1" in paint_src or "_STATE_TRASH" in paint_src
    slop_src = inspect.getsource(DesktopPetWidget._trash_paint_slop)
    assert "// 10" in slop_src or "width() // 10" in slop_src
    assert "max(16" in slop_src
    assert "_trash_erase_rect" in inspect.getsource(DesktopPetWidget)
    assert "_trash_erase_prev" in inspect.getsource(DesktopPetWidget.__init__)
    tick_src = inspect.getsource(DesktopPetWidget._on_tick)
    assert "_STATE_TRASH" in tick_src
    assert "self.update(self._trash_repaint_rect())" not in tick_src
    paint_trash = inspect.getsource(DesktopPetWidget.paintEvent)
    assert "painter.fillRect(self.rect()" in paint_trash or "fillsRect(self.rect()" in paint_trash
    assert "_seal_body_ink_outline" in import_src
    rebuild_src = (ROOT / "scripts" / "rebuild_hoodie_trash_locked.py").read_text(
        encoding="utf-8"
    )
    assert "_lock_sprite_hair" in rebuild_src
    assert "_scrub_green_halos" in rebuild_src
    # Shipped trash cels: no pale silhouette fringe / muted green-screen halo.
    import numpy as _np
    from PIL import Image as _Image

    for _ti in range(12):
        _arr = _np.array(
            _Image.open(ROOT / "assets" / "pets" / f"hoodie_trash_{_ti}.png").convert(
                "RGBA"
            )
        )
        _a = _arr[:, :, 3]
        _r = _arr[:, :, 0].astype(_np.int16)
        _g = _arr[:, :, 1].astype(_np.int16)
        _b = _arr[:, :, 2].astype(_np.int16)
        _op = _a >= 250
        _up = _np.zeros_like(_op)
        _up[1:] = _op[:-1]
        _dn = _np.zeros_like(_op)
        _dn[:-1] = _op[1:]
        _lf = _np.zeros_like(_op)
        _lf[:, 1:] = _op[:, :-1]
        _rt = _np.zeros_like(_op)
        _rt[:, :-1] = _op[:, 1:]
        _edge = _op & ~(_up & _dn & _lf & _rt)
        _lum = (
            0.2126 * _arr[:, :, 0]
            + 0.7152 * _arr[:, :, 1]
            + 0.0722 * _arr[:, :, 2]
        )
        _pale = int((((_lum[_edge] > 120) & (_lum[_edge] < 250)).sum()))
        # Soft AA (wait-like) leaves pale fringe — binary zero was for charcoal-sealed rims.
        assert _pale <= 2800, f"hoodie_trash_{_ti} pale silhouette edge {_pale}px"
        _soft_a = int(((_a > 0) & (_a < 255)).sum())
        assert _soft_a <= 12000, f"hoodie_trash_{_ti} soft alpha {_soft_a}"
        _clear = _a < 40
        _pad = _np.pad(_clear, 3, constant_values=True)
        _near = _np.zeros_like(_clear)
        for _dy in range(7):
            for _dx in range(7):
                if abs(_dy - 3) + abs(_dx - 3) > 4:
                    continue
                _near |= _pad[
                    _dy : _dy + _clear.shape[0], _dx : _dx + _clear.shape[1]
                ]
        _h, _w = _a.shape
        _logo = _np.zeros_like(_op)
        _logo[int(_h * 0.34) : int(_h * 0.64), int(_w * 0.30) : int(_w * 0.70)] = True
        _green = (_g > _r + 20) & (_g > _b + 10) & (_g > 55) & (_g < 190)
        _fringe = int((_op & _near & _green & ~_logo).sum())
        # Frame 2 soft AA sits ~80px; keep a small headroom above charcoal seal.
        assert _fringe <= 100, f"hoodie_trash_{_ti} green-screen fringe {_fringe}px"

    widget_src = (ROOT / "src" / "ui" / "pet_widget.py").read_text(encoding="utf-8")
    assert "pad = max(pad, self._sprite_dest_rect().width() // 2)" in widget_src
    assert _frame_index(hoodie_poses["trash"], 0, "trash", character="hoodie") == 0
    assert (
        _frame_index(
            hoodie_poses["trash"],
            hoodie_trash_ticks * trash_last,
            "trash",
            character="hoodie",
        )
        == trash_last
    )
    assert _frame_index(hoodie_poses["trash"], 9_999, "trash", character="hoodie") == trash_last
    assert pose_for_state("sleep") == "sleep"
    assert pose_for_state("wait") == "wait"
    assert pose_for_state("perch") == "wait"
    assert pose_for_state("trash") == "trash"
    from src.ui.pet_widget import _IDLE_TICK_STATES, _STATE_SLEEP, _STATE_TRASH, _STATE_WAIT

    # Page-flip / crumple must keep full tick rate (not idle 10fps).
    assert _STATE_WAIT not in _IDLE_TICK_STATES
    assert _STATE_TRASH not in _IDLE_TICK_STATES
    assert _STATE_SLEEP in _IDLE_TICK_STATES
    anim = PetAnimState()
    anim.set_pose("sleep")
    assert anim.blend < 1.0
    anim.tick(0.5)
    assert anim.blend >= 1.0 or anim.blend > 0.5

    s: dict = {}
    cfg = desktop_pet_settings(s)
    assert cfg is s["desktop_pet"]
    assert cfg["character"] == "hoodie"
    assert cfg.get("auto_wander") is True
    assert cfg.get("show_page_bubbles") is True
    assert cfg.get("enabled_actions") == ["pet", "wait", "sleep", "fall"]
    by_char = cfg.get("enabled_actions_by_character")
    assert isinstance(by_char, dict)
    assert set(by_char.keys()) == {"hoodie", "voyage"}
    assert by_char["hoodie"] == ["pet", "wait", "sleep", "fall"]
    assert normalize_pet_enabled_actions(None) == ["pet", "wait", "fall"]
    assert normalize_pet_enabled_actions(["pet"]) == ["pet", "wait", "fall"]
    assert normalize_pet_enabled_actions(["feed", "pet", "nope"]) == [
        "pet",
        "wait",
        "fall",
    ]
    assert normalize_pet_enabled_actions(["pet", "climb", "sleep", "wait", "fall"]) == [
        "pet",
        "wait",
        "sleep",
        "fall",
    ]
    assert normalize_pet_enabled_actions(["pet", "climb", "sleep"]) == [
        "pet",
        "sleep",
        "fall",
    ]
    assert normalize_pet_enabled_actions(["wait"]) == ["pet", "wait", "fall"]
    assert pet_enabled_actions(s) == ["pet", "wait", "sleep", "fall"]
    assert pet_action_enabled(s, "pet") is True
    assert pet_action_enabled(s, "wait") is True
    assert pet_action_enabled(s, "sleep") is True
    assert pet_action_enabled(s, "feed") is False
    assert pet_action_enabled(s, "climb") is False
    assert pet_action_enabled(s, "fall") is True
    assert pet_action_enabled(s, "hide") is True  # always on
    cfg["enabled_actions_by_character"] = {
        "hoodie": ["pet", "fall"],
        "voyage": ["pet", "wait"],
    }
    cfg["character"] = "voyage"
    cfg["enabled_actions"] = ["pet", "wait"]
    assert pet_enabled_actions(s, character="hoodie") == ["pet", "fall"]
    assert pet_enabled_actions(s, character="voyage") == ["pet", "wait", "fall"]
    assert pet_action_enabled(s, "wait", character="hoodie") is False
    assert pet_action_enabled(s, "wait", character="voyage") is True
    assert pet_action_enabled(s, "fall", character="voyage") is True
    assert pet_size_scale(s) == 1.0
    assert normalize_pet_size_scale(0.2) == 0.5
    assert normalize_pet_size_scale(9) == 1.6
    cfg["size_scale"] = 1.25
    assert pet_size_scale(s) == 1.25
    assert desktop_pet_enabled(s) is True  # first-install default on
    cfg["enabled"] = False
    assert desktop_pet_enabled(s) is False
    cfg["enabled"] = True
    assert desktop_pet_enabled(s) is True
    assert desktop_pet_visible(s) is True
    assert desktop_pet_hosts_float_bar(s) is True
    assert desktop_pet_shows_page_bubbles(s) is True
    cfg["show_page_bubbles"] = False
    assert desktop_pet_hosts_float_bar(s) is False
    cfg["show_page_bubbles"] = True
    cfg["visible"] = False
    assert desktop_pet_hosts_float_bar(s) is False
    assert desktop_pet_visible(s) is False
    cfg["visible"] = True
    assert len(pet_lines(s)) >= 3
    cfg["character"] = "hoodie"
    assert normalize_pet_character(cfg["character"]) == "hoodie"
    assert any("Hood" in line or "嗨" in line for line in pet_lines(s))
    assert any("纸飞机" in line and "拖" in line for line in pet_lines(s))
    assert random_pet_line(s, seed=1) in pet_lines(s)
    wait_line = random_pet_line(s, action="wait", seed=2)
    assert wait_line in pet_action_lines(s, "wait")
    assert len(pet_action_lines(s, "fall")) >= 1
    assert len(pet_action_lines(s, "pet")) >= 1
    assert len(pet_action_lines(s, "sleep")) >= 1
    assert len(pet_action_lines(s, "trash")) >= 1
    assert "纸飞机" in pet_action_lines(s, "trash")[0] or "飞" in pet_action_lines(s, "trash")[0]
    cfg["character"] = "unknown"
    assert normalize_pet_character(cfg["character"]) == "hoodie"

    from src.app import DeskTidyApp
    from src.ui.main_window import _NAV_ITEMS, MainWindow
    from src.ui.page_indicator import PageIndicatorWidget
    from src.ui.pet_settings_widget import PetCharCard, PetSettingsWidget
    from src.ui.pet_widget import DesktopPetWidget

    assert "_setup_desktop_pet" in inspect.getsource(DeskTidyApp)
    assert "_on_pet_requested" in inspect.getsource(DeskTidyApp)
    assert "_on_pet_settings_changed" in inspect.getsource(DeskTidyApp)
    pet_set_src = inspect.getsource(DeskTidyApp._on_pet_settings_changed)
    assert "reload_settings" in pet_set_src
    assert "desktop_pet_visible" in pet_set_src
    # Size while settings open must not Qt-raise DefView overlays (lifts fences).
    assert "pet.raise_()" not in pet_set_src
    assert "_keep_overlays_under_apps" in pet_set_src
    setup_pet_src = inspect.getsource(DeskTidyApp._setup_desktop_pet)
    assert "_desk_app_ui_open" in setup_pet_src
    assert "_keep_overlays_under_apps" in setup_pet_src
    assert "pet_widget" in inspect.getsource(DeskTidyApp._iter_overlay_widgets)
    assert "pet_requested" in inspect.getsource(PageIndicatorWidget)
    assert "_make_pet_button" in inspect.getsource(PageIndicatorWidget)
    tool_flags = inspect.getsource(PageIndicatorWidget._tool_flags)
    assert "float_bar_tool_flags" in tool_flags
    defs = inspect.getsource(DesktopPetWidget._action_defs)
    assert '("say"' not in defs
    assert "_next_chatter" in inspect.getsource(DesktopPetWidget._on_ai)
    assert "_choose_next_after_idle" in inspect.getsource(DesktopPetWidget)
    assert "_last_interact_at" in inspect.getsource(DesktopPetWidget.__init__)
    assert "_reload_chrome_from_settings" in inspect.getsource(DesktopPetWidget)
    assert "_activate_chrome_item" in inspect.getsource(DesktopPetWidget)
    assert "desktop_pet_hosts_float_bar" in inspect.getsource(DeskTidyApp._setup_page_indicator)
    flags = float_bar_tool_flags({"notepad": {"enabled": True}, "screen_record": {"enabled": False}})
    assert flags["note"] is False
    assert flags["record"] is False
    assert flags["pet"] is False
    assert any(pid == "pet" for pid, _name in _NAV_ITEMS)
    mw = inspect.getsource(MainWindow)
    assert "_build_pet_page" in mw
    assert "pet_settings_changed = pyqtSignal()" in mw
    mw_pet = inspect.getsource(MainWindow._on_pet_settings_changed)
    assert "pet_settings_changed.emit" in mw_pet
    assert "extensions_changed" not in mw_pet
    pet_settings_src = inspect.getsource(PetSettingsWidget)
    assert "PetCharCard" in pet_settings_src
    assert "pet_settings_changed" in pet_settings_src
    assert "选择宠物形象" in pet_settings_src
    assert "显示到桌面" in pet_settings_src
    assert "_on_show_clicked" in pet_settings_src
    assert "show_page_bubbles" in pet_settings_src
    assert "page_bubbles_cb" in pet_settings_src
    assert "_on_card_actions_changed" in pet_settings_src
    assert "petCardActionHint" in pet_settings_src
    card_src = inspect.getsource(PetCharCard)
    assert "pet_settings_action_options" in card_src
    assert "actions_changed" in card_src
    assert "size_slider" in pet_settings_src
    assert "size_scale" in pet_settings_src
    # Character picker is static (no live idle timer).
    assert "QTimer" not in card_src
    assert "_on_tick" not in card_src
    assert "pet_sprite_path" in card_src
    assert "KeepAspectRatio" in card_src
    assert 'cfg["visible"] = True' in inspect.getsource(DeskTidyApp._on_pet_requested)
    assert "_paint_zzz" in inspect.getsource(
        __import__("src.ui.pet_anim", fromlist=["_paint_zzz"])
    )
    assert '"z"' in inspect.getsource(
        __import__("src.ui.pet_anim", fromlist=["_paint_zzz"])
    )
    assert "_fit_sprite" in inspect.getsource(
        __import__("src.ui.pet_anim", fromlist=["_fit_sprite"])
    )
    assert "_opaque_bbox" in inspect.getsource(
        __import__("src.ui.pet_anim", fromlist=["_opaque_bbox"])
    )
    assert "_CONTENT_HEIGHT_FILL" in inspect.getsource(
        __import__("src.ui.pet_anim", fromlist=["_CONTENT_HEIGHT_FILL"])
    )
    assert "setDevicePixelRatio" in inspect.getsource(
        __import__("src.ui.pet_anim", fromlist=["_fit_sprite"])
    )
    # Uniform on-screen size: max opaque span (not height-only — that bloated sleep).
    pet_anim = __import__("src.ui.pet_anim", fromlist=["_fit_sprite", "_opaque_bbox"])
    fit_src = inspect.getsource(pet_anim._fit_sprite)
    assert "IgnoreAspectRatio" in fit_src
    assert "content_span" in fit_src
    logical_w, logical_h = 172, 220
    for char_id in ("hoodie", "voyage"):
        poses = pet_anim.load_pet_poses(char_id)
        spans: list[float] = []
        for pose in ("idle", "walk", "stand", "sleep", "play"):
            frames = poses.get(pose) or []
            assert frames, f"{char_id}/{pose} missing"
            pix = frames[0]
            bb = pet_anim._opaque_bbox(pix)
            fitted = pet_anim._fit_sprite(pix, logical_w, logical_h, 1.0)
            scale = fitted.width() / max(1.0, float(pix.width()))
            span = max(bb.width(), bb.height()) * scale
            spans.append(span)
        assert max(spans) > 0
        # All poses share ~same character scale (sleep must not tower over idle).
        assert max(spans) / min(spans) < 1.12, (char_id, spans)
    src = inspect.getsource(DesktopPetWidget)
    assert "_sprite_h" in src
    assert "pet_size_scale" in src
    assert "show_pet" in src
    assert "hide_pet" in src
    assert "page_changed" in src
    assert "_paint_page_bubbles" in src
    assert "if self._dragging:" in inspect.getsource(DesktopPetWidget._paint_page_bubbles)
    assert "_apply_drag_window_extra" in src
    assert "_hold_drag_headroom" in src
    assert "_clear_drag_layout" in src
    assert "_show_page_bubbles" in src
    assert "_layout_page_bubbles" in src
    # Float-bar chips float above the head (cloud), not a right-side column.
    assert "index * (_PAGE_BUBBLE_H" not in src
    assert "_PAGE_BUBBLE_STAGGER" in src
    assert "desktop_pet_hosts_float_bar" in inspect.getsource(
        PageIndicatorWidget._rebuild_buttons
    )
    assert "_reload_sprite" in src
    assert "paint_pet_sprite" in src
    assert "_action_pet" in src
    assert "_action_fall" in src
    assert "_action_sleep_toggle" in src
    assert "_action_wait_toggle" in src
    assert "_STATE_WAIT" in src
    assert "_STATE_TRASH" in src
    assert "_start_file_trash" in src
    assert "delete_to_trash" in src
    assert "_accept_file_drop" in src
    assert "preferred_drop_action_for_mime" in src
    assert "_cleanup_pins_after_trash" in src
    assert "_PetFileDropZone" in src
    assert "_sync_drop_zone" in src
    assert "_sync_input_mask" in src
    assert "setMask" in src
    assert "QColor(0, 0, 0, 8)" not in src
    mask_src = inspect.getsource(DesktopPetWidget._sync_input_mask)
    assert "_sprite_dest_rect" in mask_src
    assert "_schedule_public_host_mask_refresh" in mask_src
    move_src = inspect.getsource(DesktopPetWidget._move_clamped)
    assert "_schedule_public_host_mask_refresh" in move_src
    # Regression: after one drag, stale public-host exclusion swallowed the sprite.
    from PyQt6.QtCore import QPoint, QRect
    from PyQt6.QtGui import QRegion
    from PyQt6.QtWidgets import QApplication

    from src.ui.public_icon_host import PublicIconHost

    app = QApplication.instance() or QApplication([])
    host = PublicIconHost()
    host.setGeometry(0, 0, 800, 600)
    host.show()
    pet = DesktopPetWidget({"desktop_pet": {"enabled": True}})
    pet.setGeometry(40, 40, 180, 240)
    pet._sync_input_mask()
    pet.show()
    app.processEvents()

    class _Desk:
        _public_icon_host = host

    app._desktidy_app = _Desk()  # type: ignore[attr-defined]
    host.refresh_click_mask()
    app.processEvents()
    pet_mask = pet.mask()
    assert not pet_mask.isNull() and not pet_mask.isEmpty()
    old_excl = pet_mask.translated(pet.frameGeometry().topLeft() - host.geometry().topLeft())
    # Point inside old pet hole must be excluded from host hit region.
    sample_old = old_excl.boundingRect().center()
    assert not host.hit_test_region().contains(sample_old), "pet hole missing at start"

    pet._move_clamped(500, 300)
    app.processEvents()
    # Coalesced timer (interval 0) — flush so exclusion tracks the new rect.
    if host._mask_timer.isActive():
        host._mask_timer.stop()
        host.refresh_click_mask()
    app.processEvents()
    new_excl = pet.mask().translated(pet.frameGeometry().topLeft() - host.geometry().topLeft())
    sample_new = new_excl.boundingRect().center()
    assert not host.hit_test_region().contains(sample_new), (
        "after move, public plate still covers pet — second drag would fail"
    )
    # Old location must no longer be reserved for the pet (may be plate or empty).
    assert sample_old != sample_new
    pet.close()
    host.close()
    delattr(app, "_desktidy_app")
    # Parent overlay must not register OLE — page chips would steal folder→desktop.
    assert "setAcceptDrops(False)" in inspect.getsource(DesktopPetWidget.__init__)
    zone_cls = __import__("src.ui.pet_widget", fromlist=["_PetFileDropZone"])._PetFileDropZone
    zone_src = inspect.getsource(zone_cls)
    assert "setAcceptDrops(True)" in zone_src
    assert "dropEvent" in zone_src
    assert "_accept_file_drop" in zone_src
    sync_zone = inspect.getsource(DesktopPetWidget._sync_drop_zone)
    assert "setAcceptDrops(True)" in sync_zone
    # Custom drag bridge: desktop floats are non-OLE and need hit-test → trash.
    assert "accepts_trash_at" in src
    import src.ui.pet_widget as pet_mod

    mod_src = inspect.getsource(pet_mod)
    assert "find_pet_trash_target" in mod_src
    assert "deliver_paths_to_pet_trash" in mod_src
    assert "_recycle_paths_now" in mod_src
    assert "raise_pet_above_fences_in_band" in mod_src
    assert "_restack_fences_after_pet_raise" in mod_src
    recycle_src = inspect.getsource(DesktopPetWidget._recycle_paths_now)
    assert "clear_organize_suppress" in recycle_src
    start_trash = inspect.getsource(DesktopPetWidget._start_file_trash)
    assert "_recycle_paths_now" in start_trash
    # Must delete before / as anim starts — not wait for mid-cel (left desktop files).
    assert "self._trash_recycled = True" in start_trash
    assert callable(pet_mod.find_pet_trash_target)
    assert callable(pet_mod.deliver_paths_to_pet_trash)
    show_src = inspect.getsource(DesktopPetWidget.showEvent)
    assert "setAcceptDrops(False)" in show_src
    assert "_sync_drop_zone" in show_src
    assert "_restack_fences_after_pet_raise" in show_src
    ensure_live = inspect.getsource(DeskTidyApp.ensure_live_fences_interactive)
    assert "raise_overlay_in_desktop_band" in ensure_live
    assert 'getattr(self, "pet_widget"' in ensure_live
    assert "_sink_below_fence(getattr(self, \"pet_widget\"" not in ensure_live
    # No live fences must still sink public host under pet (full 框选 plate).
    assert "if not first_fence_hwnd and pet_hwnd:" in ensure_live
    assert "refresh_click_mask" in ensure_live
    assert "_STATE_WANDER" in src
    assert "_on_ai" in src
    assert "_toggle_action_menu" in src
    assert "_action_defs" in src
    assert "pet_action_enabled" in src
    assert "_STATUS_H = 0" in inspect.getsource(__import__("src.ui.pet_widget", fromlist=["DesktopPetWidget"]))
    # Pet must raise in the desktop band — HWND_BOTTOM sinks under wallpaper.
    assert "_desktidy_raise_band = True" in src
    assert "raise_band=True" in inspect.getsource(DesktopPetWidget._update_drag)
    chrome_src = inspect.getsource(DeskTidyApp._ensure_page_chrome_visible)
    assert "pet_widget" in chrome_src
    # ⋯ menu: optional wait/sleep only + 隐藏 (摸摸=点击, 跌落=拖拽)
    from src.desktop_pet import (
        DEFAULT_ENABLED_ACTIONS,
        HOODIE_DEFAULT_ENABLED_ACTIONS,
        PET_ACTION_OPTIONS,
        pet_action_options,
        pet_menu_action_options,
    )

    assert [a for a, _ in PET_ACTION_OPTIONS] == ["pet", "wait", "sleep", "fall"]
    assert [a for a, _ in pet_menu_action_options("hoodie")] == ["wait", "sleep"]
    assert [a for a, _ in pet_menu_action_options("voyage")] == ["wait"]
    assert dict(pet_action_options("hoodie"))["wait"] == "玩手机"
    assert dict(pet_action_options("hoodie"))["sleep"] == "睡觉"
    assert list(DEFAULT_ENABLED_ACTIONS) == ["pet", "wait", "fall"]
    assert list(HOODIE_DEFAULT_ENABLED_ACTIONS) == ["pet", "wait", "sleep", "fall"]
    pet_w = DesktopPetWidget({"desktop_pet": {"enabled": True}})
    normal_rect = pet_w._sprite_dest_rect()
    from src.ui.pet_widget import _STATE_WAIT

    pet_hoodie_wait = DesktopPetWidget(
        {"desktop_pet": {"enabled": True, "character": "hoodie"}}
    )
    pet_hoodie_wait._state = _STATE_WAIT
    pet_hoodie_wait._apply_size()
    wait_dest = pet_hoodie_wait._sprite_dest_rect()
    assert wait_dest.left() >= 0, wait_dest
    assert wait_dest.right() <= pet_hoodie_wait.width(), (
        wait_dest.right(),
        pet_hoodie_wait.width(),
    )
    assert pet_hoodie_wait._layout_body_width() > pet_hoodie_wait._body_width()
    pet_w._dragging = True
    drag_rect = pet_w._sprite_dest_rect()
    assert drag_rect.top() < normal_rect.top()
    assert drag_rect.height() > normal_rect.height()
    headroom = pet_w._hold_drag_headroom()
    base_geo = pet_w.geometry()
    pet_w._apply_drag_window_extra(headroom)
    assert pet_w.geometry().height() == base_geo.height() + headroom
    assert pet_w.geometry().y() == base_geo.y() - headroom
    pet_w._apply_drag_window_extra(0)
    assert pet_w.geometry().height() == base_geo.height()
    # Throw path: clear drag headroom before fall/say — no half-height clip.
    pet_throw = DesktopPetWidget({"desktop_pet": {"enabled": True}})
    pet_throw._dragging = True
    pet_throw._apply_drag_window_extra(pet_throw._hold_drag_headroom())
    pet_throw._dragging = False
    pet_throw._clear_drag_layout()
    normal_h = pet_throw.height()
    from src.ui.pet_widget import _STATE_FALL

    pet_throw._state = _STATE_FALL
    pet_throw.say("飞啦!", msec=1400)
    assert pet_throw.height() >= 280, pet_throw.height()
    # idle/sleep → trash must pin body *before* pad/width change (else jumps right).
    enter_src = inspect.getsource(DesktopPetWidget._enter)
    apply_src = inspect.getsource(DesktopPetWidget._apply_size)
    assert "pin_global = self.mapToGlobal(self._body_pin_local())" in enter_src
    assert "self._apply_size(pin_global=pin_global)" in enter_src
    assert "_state = state" in enter_src
    assert enter_src.index("pin_global = self.mapToGlobal") < enter_src.index("self._state = state")
    assert "pin_global: QPoint | None = None" in apply_src
    assert "def _body_pin_local" in inspect.getsource(DesktopPetWidget)
    # Page chips must track sprite left pad — else trash pin leaves them sliding left.
    layout_src = inspect.getsource(DesktopPetWidget._layout_page_bubbles)
    assert "spr_pad_l" in layout_src
    assert "left_pad + spr_pad_l" in layout_src
    from src.ui.pet_widget import _STATE_IDLE, _STATE_TRASH

    pet_chrome = DesktopPetWidget(
        {
            "desktop_pet": {"enabled": True, "show_page_bubbles": True, "character": "hoodie"},
            "desktop_pages": [
                {"id": 0, "name": "工作"},
                {"id": 1, "name": "文档"},
                {"id": 2, "name": "微控"},
            ],
        }
    )
    pet_chrome._reload_chrome_from_settings()
    pet_chrome._state = _STATE_IDLE
    pet_chrome._apply_size()
    assert pet_chrome._show_page_bubbles() and len(pet_chrome._chrome_items) >= 3
    pin_x0 = pet_chrome._body_pin_local().x()
    dx0 = [
        pet_chrome._page_bubble_rect(i).center().x() - pin_x0
        for i in range(len(pet_chrome._chrome_items))
    ]
    pet_chrome._state = _STATE_TRASH
    pet_chrome._apply_size()
    pin_x1 = pet_chrome._body_pin_local().x()
    assert pin_x1 > pin_x0, (pin_x0, pin_x1)  # trash left pad appears in local coords
    dx1 = [
        pet_chrome._page_bubble_rect(i).center().x() - pin_x1
        for i in range(len(pet_chrome._chrome_items))
    ]
    for i, (a, b) in enumerate(zip(dx0, dx1)):
        assert abs(a - b) <= 2, (i, a, b, "page chip slid vs body on trash pads")
    aids = [a[0] for a in pet_w._action_defs()]
    assert aids == ["wait", "sleep", "hide"]
    assert any(g == "游" for _a, g, _t in pet_w._action_defs())
    # Resting: only the active rest action is 「起」— never two identical wake buttons.
    from src.ui.pet_widget import _STATE_WAIT, _STATE_SLEEP

    pet_rest = DesktopPetWidget({"desktop_pet": {"enabled": True, "character": "hoodie"}})
    pet_rest._state = _STATE_WAIT
    wait_glyphs = [g for _a, g, _t in pet_rest._action_defs()]
    assert wait_glyphs == ["起", "睡", "隐"], wait_glyphs
    assert wait_glyphs.count("起") == 1
    pet_rest._state = _STATE_SLEEP
    sleep_glyphs = [g for _a, g, _t in pet_rest._action_defs()]
    assert sleep_glyphs == ["游", "起", "隐"], sleep_glyphs
    assert sleep_glyphs.count("起") == 1
    wait_toggle = inspect.getsource(DesktopPetWidget._action_wait_toggle)
    sleep_toggle = inspect.getsource(DesktopPetWidget._action_sleep_toggle)
    assert "self._state == _STATE_WAIT" in wait_toggle
    assert "self._start_auto_wait()" in wait_toggle
    assert "self._state == _STATE_SLEEP" in sleep_toggle
    assert "self._start_auto_sleep()" in sleep_toggle
    # Regression: action panel hugs body; right-column buttons stay inside widget + mask.
    pet_menu = DesktopPetWidget(
        {"desktop_pet": {"enabled": True, "show_page_bubbles": True}}
    )
    pet_menu._reload_chrome_from_settings()
    pet_menu._set_menu_open(True)
    left_pad, _band, total_w = pet_menu._chrome_layout_metrics()
    body_right = left_pad + pet_menu._body_width()
    panel_w = pet_menu._action_panel_width()
    assert pet_menu._action_panel_origin_x() == body_right
    assert pet_menu.width() >= body_right + panel_w, (
        pet_menu.width(),
        body_right,
        panel_w,
        total_w,
    )
    widget_rect = pet_menu.rect()
    defs = pet_menu._action_defs()
    for i, (aid, _glyph, _tip) in enumerate(defs):
        btn = pet_menu._action_button_rect(i)
        assert widget_rect.contains(btn), (aid, i, btn, pet_menu.width(), pet_menu.height())
        if i % 2 == 1:
            assert btn.right() <= pet_menu.width(), (aid, btn.right(), pet_menu.width())
        assert pet_menu._hit_action_id(btn.center()) == aid
    panel = pet_menu._action_panel_rect()
    assert not panel.isNull() and widget_rect.contains(panel)
    assert "爬" not in "".join(g for _a, g, _t in pet_w._action_defs())
    assert "报" not in "".join(g for _a, g, _t in pet_w._action_defs())
    assert "睡" in "".join(g for _a, g, _t in pet_w._action_defs())
    base_h = pet_w._sprite_h()
    pet_big = DesktopPetWidget(
        {"desktop_pet": {"enabled": True, "enabled_actions": ["pet"], "size_scale": 1.5}}
    )
    assert pet_big._sprite_h() > base_h
    pet_w2 = DesktopPetWidget(
        {
            "desktop_pet": {
                "enabled": True,
                "character": "hoodie",
                "enabled_actions_by_character": {
                    "hoodie": ["pet", "sleep"],
                    "voyage": ["pet", "wait", "fall"],
                },
            }
        }
    )
    assert [a[0] for a in pet_w2._action_defs()] == ["sleep", "hide"]
    pet_voyage = DesktopPetWidget(
        {
            "desktop_pet": {
                "enabled": True,
                "character": "voyage",
                "enabled_actions_by_character": {
                    "hoodie": ["pet", "sleep"],
                    "voyage": ["pet", "wait", "fall"],
                },
            }
        }
    )
    assert [a[0] for a in pet_voyage._action_defs()] == ["wait", "hide"]
    assert "_paint_menu_chrome" in src
    assert "_STATE_FALL" in src
    assert '"climb"' not in inspect.getsource(DesktopPetWidget._action_defs)
    assert '"climb"' not in inspect.getsource(DesktopPetWidget._run_action)
    choose_src = inspect.getsource(DesktopPetWidget._choose_next_after_idle)
    assert "_start_self_play" not in choose_src
    assert "_start_perch" not in choose_src
    assert "_start_wander" not in choose_src
    assert "_start_climb" not in choose_src
    assert "_start_auto_rest" in choose_src
    assert "_start_auto_wait" in inspect.getsource(DesktopPetWidget)
    assert "_rest_flip" in inspect.getsource(DesktopPetWidget.__init__)
    rest_src = inspect.getsource(DesktopPetWidget._start_auto_rest)
    assert "_start_auto_wait" in rest_src and "_start_auto_sleep" in rest_src
    assert "pet_sleep_on_rest" in rest_src
    from src.desktop_pet import pet_sleep_on_rest

    assert pet_sleep_on_rest("hoodie") is True
    assert pet_sleep_on_rest("voyage") is False
    from src.desktop_pet import pet_character_draw_scale

    assert pet_character_draw_scale("hoodie") == 1.0
    wait_src = inspect.getsource(DesktopPetWidget._action_wait_toggle)
    assert "_start_auto_wait" in wait_src
    assert "坐下刷手机" in inspect.getsource(DesktopPetWidget._wait_action_tip)
    assert "趴着玩手机" in inspect.getsource(DesktopPetWidget._wait_action_tip)
    assert "_sleep_action_tip" in inspect.getsource(DesktopPetWidget)
    assert "_hoodie_rest_alternates" in inspect.getsource(DesktopPetWidget)
    sprite_w = inspect.getsource(DesktopPetWidget._sprite_w)
    assert "_STATE_WAIT" in sprite_w
    draw_src = inspect.getsource(pet_anim._draw_one)
    assert "pet_character_draw_scale" in draw_src
    assert "_POSE_WAIT" in draw_src
    assert "_menu_open" in inspect.getsource(DesktopPetWidget._step_motion)
    assert "_menu_open" in inspect.getsource(DesktopPetWidget._on_ai)
    assert "_clear_follow_cursor" in src
    # 「⋯」opens on press so follow mode does not steal the release click
    press_src = inspect.getsource(DesktopPetWidget.mousePressEvent)
    assert "_toggle_action_menu()" in press_src
    assert "QMenu" not in src
    assert "_poses" in src

    # Pose mapping: climb uses dedicated crawl sprites
    from src.ui.pet_anim import (
        _FALL_TICKS_PER_FRAME,
        _HOLD_TICKS_PER_FRAME,
        _NO_CROSSFADE_POSES,
        _TRASH_TICKS_PER_FRAME,
        _frame_index,
        _motion,
        pose_for_state,
    )
    from src.ui.pet_anim import PetAnimState

    assert pose_for_state("climb") == "crawl"
    assert pose_for_state("climb_crawl") == "crawl"
    assert pose_for_state("fall") == "fall"
    assert pose_for_state("dizzy") == "fall"
    assert pose_for_state("hold") == "hold"
    assert pose_for_state("trash") == "trash"
    assert pose_for_state("happy") == "play"
    # Hoodie climb falls back to walk/crawl frames when dedicated crawl art is absent.
    crawl_pose = pet_anim.load_pet_poses("hoodie").get("crawl") or []
    assert len(crawl_pose) >= 1, "hoodie crawl fallback frames"
    hold_pose = pet_anim.load_pet_poses("hoodie").get("hold") or []
    voyage_hold = pet_anim.load_pet_poses("voyage").get("hold") or []
    fall_pose = pet_anim.load_pet_poses("hoodie").get("fall") or []
    voyage_fall = pet_anim.load_pet_poses("voyage").get("fall") or []
    trash_pose = pet_anim.load_pet_poses("hoodie").get("trash") or []
    assert len(hold_pose) >= 4, "hoodie hold sway multi-frames"
    assert len(voyage_hold) >= 4, "voyage hold sway multi-frames"
    assert len(fall_pose) >= 2, "hoodie fall multi-frames"
    assert len(voyage_fall) >= 2, "voyage fall multi-frames"
    assert len(trash_pose) >= 12, "hoodie trash paper-plane keyframes"
    assert len(trash_pose) <= 16, len(trash_pose)
    voyage_trash = pet_anim.load_pet_poses("voyage").get("trash") or []
    assert len(voyage_trash) >= 8, "voyage trash crumple multi-frames"
    trash_sizes = {(p.width(), p.height()) for p in trash_pose}
    voyage_trash_sizes = {(p.width(), p.height()) for p in voyage_trash}
    assert len(trash_sizes) == 1, trash_sizes
    assert len(voyage_trash_sizes) == 1, voyage_trash_sizes
    from src.ui.pet_anim import _trash_ticks_per_frame, _trash_cel_crossfade

    ticks = _trash_ticks_per_frame(character="hoodie")
    fi, ti, blend = _trash_cel_crossfade(trash_pose, 0, character="hoodie")
    assert fi == 0 and ti == 0 and blend == 1.0
    fi3, ti3, blend3 = _trash_cel_crossfade(trash_pose, ticks - 1, character="hoodie")
    assert fi3 == 0 and ti3 == 0 and blend3 == 1.0
    fi4, ti4, blend4 = _trash_cel_crossfade(trash_pose, ticks, character="hoodie")
    assert fi4 == 1 and ti4 == 1 and blend4 == 1.0
    # Hoodie trash: paper-plane cels share foot anchor + head inside canvas.
    try:
        import numpy as _np
        from PIL import Image as _Image

        ht = [_np.array(_Image.open(pet_sprite_path("hoodie", f"trash_{i}")).convert("RGBA")) for i in range(len(trash_pose))]
        from scripts.rebuild_hoodie_trash_locked import _foot_xy as _trash_foot

        feet = [_trash_foot(_Image.open(pet_sprite_path("hoodie", f"trash_{i}"))) for i in range(len(trash_pose))]
        # Feet authored at horizontal canvas center — runtime center-draws.
        canvas_w = ht[0].shape[1]
        for i, (fx, fy) in enumerate(feet):
            assert abs(fx - canvas_w / 2.0) <= 1.5, (i, fx, canvas_w / 2.0)
        foot_y0 = feet[0][1]
        for i in range(1, len(feet)):
            _fx, fy = feet[i]
            assert abs(fy - foot_y0) <= 1.5, (i, fy, foot_y0)
        for i, arr in enumerate(ht):
            # Body (largest blob) must stay inside canvas; flying plane may use top pad.
            from scripts.import_trash_gen_frames import _largest_blob_bbox as _body_bb

            _bl, body_t, _br, _bbtm = _body_bb(_Image.fromarray(arr, "RGBA"))
            assert int(body_t) >= 28, (i, int(body_t), "body head clipped")
            ys = _np.where(arr[:, :, 3] > 20)[0]
            assert ys.size and int(ys.min()) >= 0, (i, int(ys.min()))
        assert len({tuple(im.shape) for im in ht}) == 1
        # Neighbor frames should not hard-cut into unrelated poses (identity flicker).
        for i in range(len(ht) - 1):
            a, b = ht[i], ht[i + 1]
            both = (a[:, :, 3] > 24) & (b[:, :, 3] > 24)
            if both.any():
                d = float(_np.mean(_np.abs(a[:, :, :3][both].astype(float) - b[:, :, :3][both].astype(float))))
                # Fold / plane-exit beats change paper mass a lot; keep identity
                # flicker guard but allow authored action jumps.
                assert d < 115.0, (i, d)
        # Trash dest: idle-sized body + *equal* side pads (asymmetric L/R slid the boy).
        body_w = 180 - 8
        side = max(100, body_w)
        dest_w = body_w + side + side
        pad_src = inspect.getsource(DesktopPetWidget._trash_dest_pad)
        assert "side + max" not in pad_src
        assert "return side, side, top" in pad_src
        span = pet_anim._effective_fit_span("hoodie", "trash")
        trash_span = pet_anim.pose_fit_span("hoodie", "trash")
        idle_span = pet_anim.pose_fit_span("hoodie", "idle")
        # Must not reuse idle's 2k canvas span on the smaller trash sheet.
        assert span == trash_span and span is not None
        assert idle_span is None or span != idle_span or trash_span == idle_span
        draw_src = inspect.getsource(pet_anim._draw_one)
        assert 'pose == _POSE_TRASH' in draw_src or 'pose == "trash"' in draw_src or "_POSE_TRASH" in draw_src
        assert "use_foot_anchor = False" in draw_src
        for dpr in (1.0, 2.0):
            for pix in trash_pose:
                fit = pet_anim._fit_sprite(
                    pix,
                    dest_w,
                    220,
                    dpr,
                    pose="trash",
                    character="hoodie",
                    fixed_content_span=span,
                )
                assert fit.height() <= 220 * 0.97, (dpr, fit.width(), fit.height())
                assert fit.width() <= dest_w * 1.86, (dpr, fit.width(), fit.height())
                # Regression: idle-span-as-fixed made every trash cel ~18–35px tall.
                assert fit.height() >= 80, (dpr, fit.width(), fit.height())
        # Trash must not inflate vs idle (padded dest used to fill → ~1.8× boy).
        idle0 = (pet_anim.load_pet_poses("hoodie").get("idle") or [None])[0]
        assert idle0 is not None
        idle_fit = pet_anim._fit_sprite(
            idle0,
            172,
            220,
            1.0,
            pose="idle",
            character="hoodie",
            fixed_content_span=pet_anim.pose_fit_span("hoodie", "idle"),
        )
        trash_fit0 = pet_anim._fit_sprite(
            trash_pose[0],
            dest_w,
            220 + max(64, (220 * 3) // 4),
            1.0,
            pose="trash",
            character="hoodie",
            fixed_content_span=span,
        )
        assert abs(trash_fit0.height() - idle_fit.height()) <= 24, (
            trash_fit0.height(),
            idle_fit.height(),
        )
    except ImportError:
        pass
    # Hold cels must share canvas size — width jumps looked like stutter/ghosting.
    hold_sizes = {(p.width(), p.height()) for p in hold_pose}
    voyage_hold_sizes = {(p.width(), p.height()) for p in voyage_hold}
    assert len(hold_sizes) == 1, hold_sizes
    assert len(voyage_hold_sizes) == 1, voyage_hold_sizes
    assert "1.55" in inspect.getsource(DesktopPetWidget._sprite_w)
    # Source GIF motion-blur frames were rejected; synthesized sway stays crisp.
    assert "_hold_sway_frames" in Path(__file__).resolve().parents[1].joinpath(
        "scripts", "build_hoodie_pet.py"
    ).read_text(encoding="utf-8")
    assert Path(__file__).resolve().parents[1].joinpath(
        "scripts", "build_voyage_pet.py"
    ).is_file()
    assert Path(__file__).resolve().parents[1].joinpath(
        "scripts", "rebuild_voyage_hold_fall.py"
    ).is_file()
    assert _HOLD_TICKS_PER_FRAME <= 5
    assert _FALL_TICKS_PER_FRAME <= 6
    assert pet_sprite_path("hoodie", "hold_0").is_file()
    assert pet_sprite_path("hoodie", "fall_0").is_file()
    assert pet_sprite_path("hoodie", "trash_0").is_file()
    assert pet_sprite_path("hoodie", "trash_11").is_file()
    assert not pet_sprite_path("hoodie", "trash_12").is_file()
    # Hold finger gap must not keep a floating white paper seam.
    # White hoodie fabric in the pinch is expected — only count short pale
    # runs that are "floating" (mostly empty alpha above and below).
    try:
        import numpy as np
        from PIL import Image as _Image
    except ImportError:
        np = None  # type: ignore[assignment]
        _Image = None  # type: ignore[assignment]
    if np is not None and _Image is not None:
        hold_im = _Image.open(pet_sprite_path("hoodie", "hold_0")).convert("RGBA")
        ha = np.array(hold_im)
        hh, hw = ha.shape[:2]
        y_max = max(48, int(hh * 0.28))
        alpha = ha[:, :, 3]
        seam = 0
        for y in range(2, y_max):
            pales = []
            for x in range(hw):
                r, g, b, al = map(int, ha[y, x])
                if al < 25:
                    continue
                luma = (r + g + b) / 3.0
                sat = max(r, g, b) - min(r, g, b)
                pale = (
                    (r > 235 and g > 228 and b > 218)
                    or (r > 245 and g > 235 and 150 < b < 220 and abs(r - g) < 25)
                    or (r > 240 and g > 215 and 120 < b < 200 and (g - b) > 40 and abs(r - g) < 45)
                    or (luma > 205 and sat < 40 and g > 185 and b > 180 and (r - b) < 35)
                )
                if pale:
                    pales.append(x)
            if not pales:
                continue
            start = prev = pales[0]
            for x in pales[1:] + [None]:
                if x is not None and x == prev + 1:
                    prev = x
                    continue
                run_w = prev - start + 1
                if run_w <= 14:
                    # Floating scrap: above/below the run are mostly transparent.
                    xs = slice(start, prev + 1)
                    above = int((alpha[max(0, y - 2) : y, xs] < 25).sum())
                    below = int((alpha[y + 1 : min(hh, y + 3), xs] < 25).sum())
                    area = run_w * 2
                    if area > 0 and above >= int(0.7 * area) and below >= int(0.7 * area):
                        seam += run_w
                if x is None:
                    break
                start = prev = x
        assert seam < 35, seam
        # Grab pose must not keep a full-width lilac floor stroke under the feet.
        r = ha[:, :, 0].astype(np.int16)
        gch = ha[:, :, 1].astype(np.int16)
        bch = ha[:, :, 2].astype(np.int16)
        al = ha[:, :, 3]
        floor = (al > 20) & (bch > 195) & ((bch - gch) > 25) & (r > 160)
        bot = floor[int(hh * 0.78) :]
        assert int(bot.sum()) < 80, int(bot.sum())
    # Fall cels must not retain yellow/cream paper plate (flashes on mouse-release).
    try:
        import numpy as np
        from PIL import Image
    except ImportError:
        np = None  # type: ignore[assignment]
        Image = None  # type: ignore[assignment]
    if np is not None and Image is not None:
        for i in range(8):
            fp = pet_sprite_path("hoodie", f"fall_{i}")
            if not fp.is_file():
                break
            a = np.array(Image.open(fp).convert("RGBA"))
            h, w = a.shape[:2]
            m = max(4, int(min(h, w) * 0.08))
            edge = np.zeros((h, w), dtype=bool)
            edge[:m, :] = True
            edge[-m:, :] = True
            edge[:, :m] = True
            edge[:, -m:] = True
            r, g, b, al = (
                a[:, :, 0].astype(np.int16),
                a[:, :, 1].astype(np.int16),
                a[:, :, 2].astype(np.int16),
                a[:, :, 3],
            )
            yellow_paper = (
                (al > 40)
                & (r > 230)
                & (g > 175)
                & (g < 235)
                & (b > 90)
                & (b < 185)
                & ((r - b) > 50)
                & ((g - b) > 25)
            )
            cream = (al > 200) & (r > 240) & (g > 235) & (b > 215) & (b < 245)
            leak = int((edge & (yellow_paper | cream)).sum())
            assert leak < 80, (fp.name, leak)
    dummy = hold_pose[:4] or [None, None, None, None]
    hold_hold = _HOLD_TICKS_PER_FRAME - 1
    assert _frame_index(dummy, 0, "hold") == _frame_index(dummy, hold_hold, "hold")
    assert _frame_index(dummy, _HOLD_TICKS_PER_FRAME, "hold") != _frame_index(
        dummy, 0, "hold"
    ) or len(dummy) == 1
    fall_dummy = fall_pose[:4] or [None, None, None, None]
    # Fall freezes on cel 0 for the whole flight (no mid-air cel swap).
    assert _frame_index(fall_dummy, 0, "fall") == 0
    assert _frame_index(fall_dummy, _FALL_TICKS_PER_FRAME, "fall") == 0
    assert _frame_index(fall_dummy, 99, "fall") == 0
    assert "return 0" in inspect.getsource(_frame_index).split('if state == "fall":', 1)[-1].split("if state", 1)[0]
    # Landed stun: hoodie holds last cel; voyage steps through the fall sheet.
    assert _frame_index(fall_dummy, 0, "dizzy") == max(0, len(fall_dummy) - 1)
    assert _frame_index(fall_dummy, 50, "dizzy") == max(0, len(fall_dummy) - 1)
    from src.ui.pet_anim import _DIZZY_TICKS_BY_CHARACTER

    voyage_ticks = _DIZZY_TICKS_BY_CHARACTER["voyage"]
    assert _frame_index(voyage_fall, 0, "dizzy", character="voyage") == 0
    assert _frame_index(voyage_fall, voyage_ticks - 1, "dizzy", character="voyage") == 0
    assert _frame_index(voyage_fall, voyage_ticks, "dizzy", character="voyage") == 1
    assert "_STATE_DIZZY" in inspect.getsource(
        __import__("src.ui.pet_widget", fromlist=["DesktopPetWidget"]).DesktopPetWidget
    )
    dizzy_src = Path(__file__).resolve().parents[1].joinpath("src", "ui", "pet_widget.py").read_text(
        encoding="utf-8"
    )
    assert "_DIZZY_SEC" in dizzy_src or "pet_dizzy_sec" in dizzy_src
    assert "_STATE_DIZZY" in dizzy_src
    assert "_enter_dizzy" in dizzy_src
    assert "晕晕的" in dizzy_src
    from src.desktop_pet import pet_dizzy_sec

    assert pet_dizzy_sec("hoodie") == (1.0, 1.5)
    assert pet_dizzy_sec("voyage")[1] > pet_dizzy_sec("hoodie")[1]
    from src.desktop_pet import pet_happy_sec

    assert pet_happy_sec("voyage") > pet_happy_sec("hoodie")
    assert "hold" in _NO_CROSSFADE_POSES and "fall" in _NO_CROSSFADE_POSES
    assert "trash" in _NO_CROSSFADE_POSES
    from src.ui.pet_anim import _CROSSFADE_RECOVER_SEC, _CROSSFADE_SEC, _NO_CROSSFADE_OUTGOING

    assert "trash" in _NO_CROSSFADE_OUTGOING and "hold" in _NO_CROSSFADE_OUTGOING
    assert "fall" not in _NO_CROSSFADE_OUTGOING
    # Trash → wait: hard cut (no shrink dissolve).
    trash_to_wait = PetAnimState(pose="trash", blend=1.0)
    trash_to_wait.set_pose("wait")
    assert trash_to_wait.blend >= 0.99 and trash_to_wait.prev_pose == "wait"
    from src.ui.pet_anim import trash_anim_sec, _TICK_MS_REF, _trash_ticks_per_frame

    # Hoodie: no end hold; drops last two idle-like cels from the timer.
    _ht = _trash_ticks_per_frame(character="hoodie")
    _hood = trash_anim_sec("hoodie", 12)
    _old_full = 12 * _ht * (_TICK_MS_REF / 1000.0) + 0.55
    assert _hood < _old_full - 0.4
    assert abs(_hood - 10 * _ht * (_TICK_MS_REF / 1000.0)) < 0.05
    assert "_finish_trash_to_wait" in inspect.getsource(DesktopPetWidget._on_ai)
    ai_src = inspect.getsource(DesktopPetWidget._on_ai)
    assert "_STATE_TRASH" in ai_src and "_finish_trash_to_wait" in ai_src
    finish_src = inspect.getsource(DesktopPetWidget._finish_trash_to_wait)
    assert "_STATE_WAIT" in finish_src
    assert "_post_trash_wait" in finish_src
    assert "6.0" in finish_src or "random.uniform(6" in finish_src
    assert "_start_auto_wait" not in finish_src
    wait_ai = inspect.getsource(DesktopPetWidget._on_ai)
    assert "_post_trash_wait" in wait_ai
    assert _CROSSFADE_RECOVER_SEC > _CROSSFADE_SEC
    snap = PetAnimState(pose="idle")
    snap.set_pose("hold")
    assert snap.blend >= 0.99 and snap.prev_pose == "hold"
    # Fall/dizzy → idle: slow recover fade, freeze first idle cel, hold last fall cel out.
    recover = PetAnimState(pose="fall", blend=1.0)
    recover.set_pose("idle")
    assert recover.prev_pose == "fall"
    assert recover.blend == 0.0
    assert recover.blend_sec == _CROSSFADE_RECOVER_SEC
    assert recover.freeze_frame_until_blend is True
    assert recover.frame == 0
    recover.tick(0.2)
    assert recover.frame == 0  # frozen during stand-up
    assert 0.0 < recover.blend < 1.0
    recover.tick(_CROSSFADE_RECOVER_SEC)
    assert recover.blend >= 0.99
    assert recover.freeze_frame_until_blend is False
    hold_motion = _motion("hold", 12, character="hoodie")
    assert hold_motion == (0.0, 1.0, 0.0, 0.0, 1.0)
    from src.ui.pet_anim import _HOLD_BODY_TOP_FRAC, _HOLD_FIT_BOOST

    assert _HOLD_FIT_BOOST == 1.0
    assert _HOLD_BODY_TOP_FRAC == 0.0
    hold0 = hold_pose[0]
    full = pet_anim._fit_sprite(hold0, 120, 140, 1.0, body_top_frac=0.0)
    assert full.width() > 0 and full.height() > 0
    hbb = pet_anim._opaque_bbox(hold0)
    assert hbb.top() < hbb.center().y(), "hold cel should include fingers above body"
    fall_motion = _motion("fall", 12, character="hoodie")
    assert fall_motion == (0.0, 1.0, 0.0, 0.0, 1.0)
    dizzy_motion = _motion("dizzy", 12, character="hoodie")
    assert dizzy_motion == (0.0, 1.0, 0.0, 0.0, 1.0)

    # Climb motion must NOT warp the sprite (no squash/stretch gif effect).
    samples = {
        s: _motion(s, 12, character="hoodie")
        for s in ("idle", "climb_crawl", "happy", "sleep", "fall")
    }
    crawl = samples["climb_crawl"]
    assert abs(crawl[1] - 1.0) < 0.02
    assert abs(crawl[4] - 1.0) < 0.02
    assert abs(crawl[2]) < 1.0
    assert abs(samples["sleep"][2]) < 1.0
    assert samples["sleep"][1] == 1.0 and samples["sleep"][4] == 1.0
    wait_motion = _motion("wait", 12, character="hoodie")
    assert abs(wait_motion[0]) < 0.02
    assert abs(wait_motion[1] - 1.0) < 0.02
    assert abs(wait_motion[4] - 1.0) < 0.02
    hoodie_wait_ticks = _loop_ticks("wait", len(hoodie_poses["wait"]), character="hoodie")
    i0, i1, th = _cel_crossfade(hoodie_poses["wait"], hoodie_wait_ticks - 1, "wait", character="hoodie")
    assert i0 == i1 == 0 and th >= 0.99
    assert samples["happy"][0] > samples["sleep"][0]
    # Sleep cels share one opaque span so fit-sprite does not pulse size.
    from src.ui.pet_anim import _opaque_bbox

    sleep_pose = load_pet_poses("hoodie").get("sleep") or []
    assert len(sleep_pose) >= 4
    sleep_spans = {
        max(_opaque_bbox(p).width(), _opaque_bbox(p).height()) for p in sleep_pose
    }
    assert len(sleep_spans) == 1, sleep_spans
    zzz_src = inspect.getsource(
        __import__("src.ui.pet_anim", fromlist=["_paint_zzz"])._paint_zzz
    )
    assert "dest.width() * 0.08" in zzz_src
    assert '"ZZZ"' not in zzz_src
    draw_src = inspect.getsource(
        __import__("src.ui.pet_anim", fromlist=["_draw_one"])._draw_one
    )
    rot = draw_src.find("painter.rotate")
    scale = draw_src.find("painter.scale")
    assert 0 <= rot < scale

    from src.help_content import help_html

    pet_help = help_html("pet")
    assert "摸摸" in pet_help and "等待" in pet_help and "跌落" in pet_help
    assert "玩手机" in pet_help
    assert "睡觉" in pet_help
    assert "回收站" in pet_help
    assert "拖文件到宠物" in pet_help
    assert "爬行" not in pet_help
    assert "不会自己走动" in pet_help or "不会自己走动或爬边" in pet_help
    assert "往边框爬" not in pet_help
    assert "轮流" in pet_help
    assert "喂食" not in pet_help
    assert "自己逛逛" not in pet_help

    import tempfile

    from PyQt6.QtWidgets import QApplication

    from src.ui.pet_widget import DesktopPetWidget as PetW
    from src.win_shell import delete_to_trash

    sample = Path(tempfile.gettempdir()) / "_selftest_pet_recycle.txt"
    sample.write_text("recycle-me", encoding="utf-8")
    delete_to_trash(sample)
    assert not sample.exists(), "delete_to_trash must send the file to Recycle Bin"

    # Pet trash must remove the file immediately (not mid-animation).
    app = QApplication.instance() or QApplication([])
    pet = PetW({"desktop_pet": {"enabled": True, "visible": True}})
    sample2 = Path(tempfile.gettempdir()) / "_selftest_pet_recycle_now.txt"
    sample2.write_text("now", encoding="utf-8")
    recycled = pet._start_file_trash([sample2])
    assert sample2 in recycled or any(p.name == sample2.name for p in recycled)
    assert not sample2.exists(), "pet trash must delete before animation finishes"
    assert pet._trash_recycled is True
    pet.close()

    print("selftest_desktop_pet: OK")


if __name__ == "__main__":
    main()
