"""Shimeji-style smooth pet animation: frame loops + pose crossfade."""

from __future__ import annotations

import math
import time
from collections import OrderedDict
from dataclasses import dataclass, field

from PyQt6.QtCore import QRect, QRectF, Qt
from PyQt6.QtGui import QColor, QFont, QImage, QPainter, QPainterPath, QPen, QPixmap

from src.desktop_pet import normalize_pet_character, pet_sprite_path

_POSE_IDLE = "idle"
_POSE_WALK = "walk"
_POSE_STAND = "stand"
_POSE_SLEEP = "sleep"
_POSE_PLAY = "play"
_POSE_CRAWL = "crawl"
_POSE_WAIT = "wait"
_POSE_HOLD = "hold"
_POSE_FALL = "fall"
_POSE_TRASH = "trash"

_ALL_POSES = (
    _POSE_IDLE,
    _POSE_WALK,
    _POSE_STAND,
    _POSE_SLEEP,
    _POSE_PLAY,
    _POSE_CRAWL,
    _POSE_WAIT,
    _POSE_HOLD,
    _POSE_FALL,
    _POSE_TRASH,
)
_CROSSFADE_SEC = 0.38
# Dizzy sit → standing idle: longer fade so the stance change doesn't flash.
_CROSSFADE_RECOVER_SEC = 0.95
# Voyage cheer & wait cels are busy — slower pose blends read calmer.
_CROSSFADE_SEC_BY_CHARACTER: dict[str, float] = {
    "voyage": 0.62,
}
# Engine tick matches pet_widget._TICK_MS (≈30 Hz). Market Shimeji / Desktop Goose
# hold loops run ~8–12 FPS — slower than idle, but not 2 FPS (that reads as 卡顿).
_TICK_MS_REF = 33
_HOLD_TICKS_PER_FRAME = 4    # ~132 ms/cel ≈ 7.5 FPS — smooth collar sway
_WAIT_TICKS_PER_FRAME = 8    # ~264 ms/cel ≈ 3.8 FPS — leisurely wait loop
# Voyage wait cels are busier — hold longer so each pose reads.
_WAIT_TICKS_BY_CHARACTER: dict[str, int] = {
    "voyage": 20,  # ~660 ms/cel + gentle dissolve between matched cels
}
# Click cheer (happy/play): voyage skips squat cels — hold each jump cel longer.
_VOYAGE_PLAY_SKIP = 2
_CHEER_TICKS_BY_CHARACTER: dict[str, int] = {
    "voyage": 18,  # ~594 ms/cel
}
_TRASH_TICKS_PER_FRAME = 4  # default ~132 ms/cel
_TRASH_TICKS_BY_CHARACTER: dict[str, int] = {
    # 10×33ms/cel — 10 flight cels ≈ 3.3s; shorter holds read smoother (less freeze-cut).
    "hoodie": 10,
}
_TRASH_MAX_CELS = 32  # loader scans trash_0.. until missing
_MAX_WIDTH_OVER_DEST_TRASH = 1.85  # full cel + plane; pads carry most flight room
_MAX_WIDTH_OVER_DEST_HOODIE_REST = 1.55  # prone phone / pillow sleep — feet extend sideways
# Prone phone: slight downscale vs filling dest by opaque height (head reads large).
_HOODIE_WAIT_FIT_SPAN_MULT = 1.22
# Sleep uses height-only span; keep 1.0 unless prone pillow still reads oversized.
_HOODIE_SLEEP_FIT_SPAN_MULT = 1.0
# Hoodie reference loop lengths — other characters scale ticks when they carry more cels.
_HOODIE_LOOP_REFS: dict[str, tuple[int, int]] = {
    "wait": (8, _WAIT_TICKS_PER_FRAME),
    "happy": (4, 3),  # ~99 ms/cel — click cheer; was 2 (felt rushed)
    "play": (4, 2),
    "hold": (6, _HOLD_TICKS_PER_FRAME),
    "sleep": (4, 4),
    "idle": (4, 3),
    "wander": (6, 2),
}
# Fall: freeze one airborne cel while in flight; swapping cels mid-air reads as
# shake. Land pose / idle starts only after _STATE_FALL ends.
_FALL_TICKS_PER_FRAME = 5    # kept for tests / legacy; fall no longer advances
# Landed stun: voyage fall sheet steps through slowly.
_DIZZY_TICKS_BY_CHARACTER: dict[str, int] = {
    "voyage": 12,
}
_CEL_BLEND_MIN_TICKS = 6  # shorter loops keep hard cuts (hold sway is only 4 ticks/cel)
_CEL_BLEND_FRAC_DEFAULT = 0.32
# Voyage wait: hard cuts — matched cels + no dissolve (overlap still flashes).
_CEL_BLEND_FRAC_BY_CHARACTER: dict[str, float] = {}
_CEL_CROSSFADE_STATES = frozenset({"wait", "perch", "sleep", "lie", "hold"})
_NO_CROSSFADE_POSES = frozenset({_POSE_HOLD, _POSE_FALL, _POSE_TRASH})
# Leaving these must not dissolve into wait/idle (size/pad mismatch = shrink flash).
_NO_CROSSFADE_OUTGOING = frozenset({_POSE_HOLD, _POSE_TRASH})
# Match the longer opaque side (not height alone). Height-only made sleep/play
# as tall as standing idle — reclining art looked oversized. Max-span keeps the
# same character scale: upright fills height; sleep stays shorter and wider.
_CONTENT_HEIGHT_FILL = 0.90  # used as span fill vs dest height
# Soft clamp for extremely wide poses; fall/dizzy also widen the dest box.
_MAX_WIDTH_OVER_DEST = 1.12
# Hold art includes pinch fingers above the hood — fit the full cel when painting.
_HOLD_BODY_TOP_FRAC = 0.0
_HOLD_FIT_BOOST = 1.0


@dataclass
class PetAnimState:
    """Tracks pose blending so state changes don't hard-cut sprites."""

    pose: str = _POSE_IDLE
    prev_pose: str = _POSE_IDLE
    blend: float = 1.0  # 0=prev, 1=current
    blend_sec: float = _CROSSFADE_SEC
    # Hold idle on first cel while fading out the land sit (avoids twitchy stand-up).
    freeze_frame_until_blend: bool = False
    frame: int = 0
    last_tick: float = field(default_factory=time.monotonic)

    def set_pose(self, pose: str, *, character: str = "") -> None:
        pose = pose if pose in _ALL_POSES else _POSE_IDLE
        char_id = normalize_pet_character(character) if character else ""
        crossfade = _CROSSFADE_SEC_BY_CHARACTER.get(char_id, _CROSSFADE_SEC)
        if pose == self.pose and self.blend >= 0.99:
            return
        if pose != self.pose:
            outgoing = self.pose if self.blend >= 0.5 else self.prev_pose
            self.prev_pose = outgoing
            self.pose = pose
            # Hard-cut into trash/hold/fall, and out of trash/hold — dissolving a
            # padded throw dest into wait/idle reads as a shrink flash.
            if pose in _NO_CROSSFADE_POSES or outgoing in _NO_CROSSFADE_OUTGOING:
                self.prev_pose = pose
                self.blend = 1.0
                self.blend_sec = crossfade
                self.freeze_frame_until_blend = False
            else:
                self.blend = 0.0
                # Fall/dizzy both use the fall sheet — stand-up needs a slower dissolve.
                if outgoing == _POSE_FALL:
                    self.blend_sec = _CROSSFADE_RECOVER_SEC
                    self.freeze_frame_until_blend = True
                    self.frame = 0
                else:
                    self.blend_sec = crossfade
                    self.freeze_frame_until_blend = False

    def tick(self, dt: float | None = None) -> None:
        now = time.monotonic()
        if dt is None:
            dt = max(0.0, now - self.last_tick)
        self.last_tick = now
        recovering = self.freeze_frame_until_blend and self.blend < 1.0
        if not recovering:
            self.frame = (self.frame + 1) % 10_000
        if self.blend < 1.0:
            sec = max(0.05, float(self.blend_sec) or _CROSSFADE_SEC)
            self.blend = min(1.0, self.blend + dt / sec)
            if self.blend >= 1.0:
                self.freeze_frame_until_blend = False
                self.blend_sec = _CROSSFADE_SEC


def invalidate_pet_pose_cache(character: str | None = None) -> None:
    """Drop cached pose sheets (all chars, or one after asset rebuild)."""
    if character is None:
        _POSES_CACHE.clear()
        _POSE_FIT_SPAN.clear()
        _FIT_CACHE.clear()
        _BBOX_CACHE.clear()
        return
    char_id = normalize_pet_character(character)
    _POSES_CACHE.pop(char_id, None)
    for key in list(_POSE_FIT_SPAN):
        if key[0] == char_id:
            del _POSE_FIT_SPAN[key]


def _rebuild_pose_fit_spans(char_id: str, poses: dict[str, list[QPixmap]]) -> None:
    for key in list(_POSE_FIT_SPAN):
        if key[0] == char_id:
            del _POSE_FIT_SPAN[key]
    for pose, frames in poses.items():
        if pose not in _UNIFIED_FIT_POSES:
            continue
        if len(frames) < 2 and not (char_id == "hoodie" and pose == _POSE_WAIT):
            continue
        max_span = 0
        for pix in frames:
            bb = _opaque_bbox(pix)
            if bb.isEmpty() or bb.width() < 8 or bb.height() < 8:
                continue
            # Fall/dizzy/play jump / wait/sleep prone / trash: unify on height so
            # visual size stays close to idle (max-span made sleep ~22% smaller).
            if pose in {_POSE_FALL, _POSE_PLAY, _POSE_WAIT, _POSE_SLEEP, _POSE_TRASH}:
                # Trash: unify on body height — width handled by widened dest box.
                max_span = max(max_span, bb.height())
            else:
                max_span = max(max_span, bb.width(), bb.height())
        if max_span > 0:
            _POSE_FIT_SPAN[(char_id, pose)] = max_span


def pose_fit_span(character: str | None, pose: str) -> int | None:
    """Shared opaque span for a multi-cel pose (tests / diagnostics)."""
    char_id = normalize_pet_character(character) if character else ""
    return _POSE_FIT_SPAN.get((char_id, pose))


def _effective_fit_span(char_id: str, pose: str) -> int | None:
    """Content span used when painting.

    Hoodie wait / sleep / trash sheets are authored on smaller canvases than
    idle (e.g. wait 540×376 vs idle 1024×2048). Reusing idle's opaque span as
    ``fixed_content_span`` scales those sheets to a speck — always use the
    pose's own span. Wait applies a mild mult so the prone head does not read
    oversized once the opaque height fills the dest.
    """
    own = _POSE_FIT_SPAN.get((char_id, pose))
    if char_id == "hoodie" and pose == _POSE_WAIT and own:
        return int(round(own * _HOODIE_WAIT_FIT_SPAN_MULT))
    if char_id == "hoodie" and pose == _POSE_SLEEP and own:
        return int(round(own * _HOODIE_SLEEP_FIT_SPAN_MULT))
    return own


def _frame_has_visible_ink(pix: QPixmap) -> bool:
    """False for blank / fully transparent placeholder sheets."""
    if pix.isNull():
        return False
    bb = _opaque_bbox(pix)
    return (not bb.isEmpty()) and bb.width() >= 8 and bb.height() >= 8


def load_pet_poses(character: str | None) -> dict[str, list[QPixmap]]:
    """Load pose frame lists. Missing poses fall back to idle."""
    char_id = normalize_pet_character(character)
    cached = _POSES_CACHE.get(char_id)
    if cached is not None:
        return cached

    poses: dict[str, list[QPixmap]] = {}
    for pose in _ALL_POSES:
        frames: list[QPixmap] = []
        # Prefer numbered frames: name_pose_0.png …
        max_cels = _TRASH_MAX_CELS if pose == _POSE_TRASH else 32
        for i in range(max_cels):
            path = pet_sprite_path(char_id, f"{pose}_{i}")
            if not path.is_file():
                break
            pix = QPixmap(str(path))
            if not pix.isNull() and _frame_has_visible_ink(pix):
                frames.append(pix)
        if not frames:
            path = pet_sprite_path(char_id, pose)
            pix = QPixmap(str(path))
            if not pix.isNull() and _frame_has_visible_ink(pix):
                frames.append(pix)
        poses[pose] = frames

    idle = poses.get(_POSE_IDLE) or []
    if not idle:
        # Legacy single sprite
        pix = QPixmap(str(pet_sprite_path(char_id, "idle")))
        idle = [pix] if not pix.isNull() else [QPixmap()]
        poses[_POSE_IDLE] = idle
    for pose in _ALL_POSES:
        if not poses.get(pose):
            if pose == _POSE_WALK and poses.get(_POSE_STAND):
                poses[pose] = list(poses[_POSE_STAND])
            elif pose == _POSE_PLAY and poses.get(_POSE_WALK):
                poses[pose] = list(poses[_POSE_WALK])
            elif pose == _POSE_CRAWL and poses.get(_POSE_WALK):
                poses[pose] = list(poses[_POSE_WALK])
            elif pose == _POSE_WAIT and poses.get(_POSE_STAND):
                poses[pose] = list(poses[_POSE_STAND])
            elif pose == _POSE_HOLD and poses.get(_POSE_IDLE):
                poses[pose] = list(poses[_POSE_IDLE])
            elif pose == _POSE_TRASH and poses.get(_POSE_IDLE):
                poses[pose] = list(poses[_POSE_IDLE])
            elif pose == _POSE_FALL and poses.get(_POSE_PLAY):
                poses[pose] = list(poses[_POSE_PLAY])
            elif pose == _POSE_SLEEP and idle:
                poses[pose] = list(idle)
            else:
                poses[pose] = list(idle)
    _rebuild_pose_fit_spans(char_id, poses)
    _POSES_CACHE[char_id] = poses
    return poses


def pose_for_state(state: str, *, character: str = "") -> str:
    char = normalize_pet_character(character) if character else ""
    if state in {"wander", "follow"}:
        return _POSE_WALK
    if state in {"climb", "climb_crawl", "climb_wall"}:
        return _POSE_CRAWL
    if state in {"play", "happy"}:
        return _POSE_PLAY
    if state == "fall":
        return _POSE_FALL
    if state == "dizzy":
        return _POSE_FALL
    if state == "hold":
        return _POSE_HOLD
    if state == "trash":
        return _POSE_TRASH
    if state == "sleep":
        return _POSE_SLEEP
    if state in {"wait", "perch"}:
        return _POSE_WAIT
    if state == "lie":
        return _POSE_SLEEP
    return _POSE_IDLE


def _ease(t: float) -> float:
    # Smoothstep
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def _pose_cels(frames: list[QPixmap], state: str, character: str) -> list[QPixmap]:
    """Per-character cel subsets (e.g. voyage cheer skips squat wind-up)."""
    char = normalize_pet_character(character) if character else ""
    if (
        char == "voyage"
        and state in {"happy", "play"}
        and len(frames) >= 6
    ):
        return frames[_VOYAGE_PLAY_SKIP:]
    return frames


def _wait_ticks_per_frame(character: str = "") -> int:
    char = normalize_pet_character(character) if character else ""
    return _WAIT_TICKS_BY_CHARACTER.get(char, _WAIT_TICKS_PER_FRAME)


def _loop_ticks(state: str, n: int, *, character: str = "") -> int:
    """Ticks per cel so multi-frame poses match hoodie loop rhythm."""
    char = normalize_pet_character(character) if character else ""
    key = "wait" if state in {"wait", "perch"} else state
    if key == "play" and char == "hoodie":
        return 1  # legacy self-play cadence for the reference character
    ref = _HOODIE_LOOP_REFS.get(key)
    if ref is None:
        return 3
    ref_n, ref_t = ref
    if key in {"wait", "perch"} and char in _WAIT_TICKS_BY_CHARACTER:
        ref_t = _WAIT_TICKS_BY_CHARACTER[char]
    if key in {"happy", "play"} and char in _CHEER_TICKS_BY_CHARACTER:
        ref_t = _CHEER_TICKS_BY_CHARACTER[char]
    n = max(1, n)
    if n <= ref_n:
        return ref_t
    # More cels than hoodie — hold each longer so the loop does not feel rushed.
    return max(ref_t, int(math.ceil(n / ref_n * ref_t)))


def _frame_index(frames: list[QPixmap], frame: int, state: str, *, character: str = "") -> int:
    cels = _pose_cels(frames, state, character)
    n = max(1, len(cels))
    if state in {"sleep", "lie"}:
        ticks = _loop_ticks("sleep", n, character=character)
        return (frame // ticks) % n
    if state in {"wait", "perch"}:
        ticks = _loop_ticks("wait", n, character=character)
        return (frame // ticks) % n
    if state == "hold":
        ticks = _loop_ticks("hold", n, character=character)
        return (frame // ticks) % n
    if state == "trash":
        ticks = _trash_ticks_per_frame(character=character)
        return min(n - 1, frame // ticks)
    if state == "fall":
        # Hold one airborne cel for the whole flight — mid-air cel swaps feel shaky.
        return 0
    if state == "dizzy":
        # Landed stun: step multi-cel fall sheets slowly, then hold the dizzy pose.
        char = normalize_pet_character(character) if character else ""
        ticks = _DIZZY_TICKS_BY_CHARACTER.get(char)
        if ticks and n >= 2:
            return min(n - 1, frame // ticks)
        return n - 1
    if state == "idle":
        ticks = _loop_ticks("idle", n, character=character)
        return (frame // ticks) % n
    if state in {"climb", "climb_crawl", "climb_wall"}:
        # Dedicated crawl sprites already encode the wriggle — slower loop.
        return (frame // 3) % n
    if state == "happy":
        ticks = _loop_ticks("happy", n, character=character)
        return (frame // ticks) % n
    if state in {"wander", "follow", "play"}:
        if state == "play":
            ticks = _loop_ticks("play", n, character=character)
            if ticks <= 1:
                return frame % n
            return (frame // ticks) % n
        ticks = _loop_ticks("wander", n, character=character)
        return (frame // ticks) % n
    ticks = _loop_ticks("idle", n, character=character)
    return (frame // ticks) % n


def _ticks_per_cel(frames: list[QPixmap], state: str, *, character: str = "") -> int:
    cels = _pose_cels(frames, state, character)
    n = max(1, len(cels))
    if state in {"wait", "perch"}:
        return _loop_ticks("wait", n, character=character)
    if state in {"sleep", "lie"}:
        return _loop_ticks("sleep", n, character=character)
    if state == "hold":
        return _loop_ticks("hold", n, character=character)
    return 3


def _trash_ticks_per_frame(*, character: str = "") -> int:
    char = normalize_pet_character(character) if character else ""
    return _TRASH_TICKS_BY_CHARACTER.get(char, _TRASH_TICKS_PER_FRAME)


def trash_anim_sec(character: str, n_cels: int) -> float:
    """One-shot trash duration from cel count + per-character pacing.

    Hoodie: play through the flight cels only — no end hold on the idle-like
    last frames (that hold + pose blend looked like a shrink flash). Wait starts
    immediately when the state timer ends.
    """
    char = normalize_pet_character(character) if character else ""
    n = max(1, int(n_cels))
    ticks = _trash_ticks_per_frame(character=character)
    # Skip trailing idle-match cels for hoodie (plane already gone → wait).
    if char == "hoodie" and n >= 10:
        n = n - 2  # drop last two idle-like cels from the timed playback
    hold = 0.0 if char == "hoodie" else 0.4
    return max(1.2, n * ticks * (_TICK_MS_REF / 1000.0) + hold)


def _trash_cel_crossfade(
    frames: list[QPixmap],
    frame: int,
    *,
    character: str = "",
) -> tuple[int, int, float]:
    """Trash uses pre-authored cels — hard cuts only (runtime dissolve = shadow)."""
    cels = _pose_cels(frames, "trash", "")
    n = len(cels)
    if n <= 1:
        return 0, 0, 1.0
    idx = _frame_index(frames, frame, "trash", character=character)
    return idx, idx, 1.0


def _cel_blend_fraction(character: str, ticks: int, state: str = "") -> float:
    if ticks < _CEL_BLEND_MIN_TICKS:
        return 0.0
    char = normalize_pet_character(character) if character else ""
    if char in {"voyage", "hoodie"} and state in {"wait", "perch"}:
        return 0.0
    return _CEL_BLEND_FRAC_BY_CHARACTER.get(char, _CEL_BLEND_FRAC_DEFAULT)


def _cel_crossfade(
    frames: list[QPixmap],
    frame: int,
    state: str,
    *,
    character: str = "",
) -> tuple[int, int, float]:
    """Intra-pose cel dissolve: (from_idx, to_idx, blend_to).

    ``blend_to`` runs 0→1 during the tail of each cel hold (smoothstep).
    When ``from_idx == to_idx``, only one cel is shown.
    """
    cels = _pose_cels(frames, state, character)
    n = len(cels)
    if n <= 1 or state not in _CEL_CROSSFADE_STATES:
        idx = _frame_index(frames, frame, state, character=character)
        return idx, idx, 1.0

    ticks = _ticks_per_cel(frames, state, character=character)
    blend_frac = _cel_blend_fraction(character, ticks, state)
    if blend_frac <= 0.0:
        idx = _frame_index(frames, frame, state, character=character)
        return idx, idx, 1.0

    loop_len = max(1, ticks * n)
    pos = frame % loop_len
    cel_idx = pos // ticks
    sub = pos % ticks
    blend_ticks = max(2, min(ticks - 1, int(round(ticks * blend_frac))))
    hold_ticks = ticks - blend_ticks
    if sub < hold_ticks:
        return cel_idx, cel_idx, 1.0

    t = _ease((sub - hold_ticks) / float(blend_ticks))
    return cel_idx, (cel_idx + 1) % n, t


def _hop_envelope(t: float, *, pace: float, power: float = 2.2) -> float:
    """Sharp hop: long ground contact, quick air time."""
    cycle = (t * pace) % (math.pi)
    return abs(math.sin(cycle)) ** power


def _motion(state: str, frame: int, *, character: str = "") -> tuple[float, float, float, float, float]:
    """Return (bob, squash_y, tilt_deg, sway_x, stretch_x)."""
    char = normalize_pet_character(character) if character else ""
    if char == "langfrog":
        return _motion_langfrog(state, frame)
    if char == "hoodie" and state in {"wait", "perch"}:
        return 0.0, 1.0, 0.0, 0.0, 1.0
    if char == "hoodie" and state in {"sleep", "lie"}:
        # Sleep cels are already a lying pose — no squash/stretch (that reads as
        # the whole pet shrinking then growing). Soft bob only.
        t = frame / 10.0
        breath = math.sin(t * 0.4)
        return 0.6 * breath, 1.0, 0.0, 0.0, 1.0
    if char == "hoodie" and state == "hold":
        # Collar-lift cels carry the motion — no extra bob/sway (avoids drag ghosting).
        return 0.0, 1.0, 0.0, 0.0, 1.0
    if char == "hoodie" and state == "fall":
        # Fall/bounce GIF is self-contained — keep the HWND stable between cels.
        return 0.0, 1.0, 0.0, 0.0, 1.0
    if char == "hoodie" and state == "dizzy":
        return 0.0, 1.0, 0.0, 0.0, 1.0
    if char == "hoodie" and state == "trash":
        return 0.0, 1.0, 0.0, 0.0, 1.0
    if char == "voyage" and state in {"wait", "perch"}:
        return 0.0, 1.0, 0.0, 0.0, 1.0
    if char == "voyage" and state in {"sleep", "lie"}:
        t = frame / 10.0
        breath = math.sin(t * 0.4)
        return 0.6 * breath, 1.0, 0.0, 0.0, 1.0
    if char == "voyage" and state in {"happy", "play"}:
        # Jump-cheer cels carry the motion — keep HWND stable (no double-bounce).
        return 0.0, 1.0, 0.0, 0.0, 1.0
    if char == "voyage" and state == "hold":
        return 0.0, 1.0, 0.0, 0.0, 1.0
    if char == "voyage" and state == "fall":
        return 0.0, 1.0, 0.0, 0.0, 1.0
    if char == "voyage" and state == "dizzy":
        return 0.0, 1.0, 0.0, 0.0, 1.0
    if char == "voyage" and state == "trash":
        return 0.0, 1.0, 0.0, 0.0, 1.0
    return _motion_default(state, frame)


def _motion_default(state: str, frame: int) -> tuple[float, float, float, float, float]:
    t = frame / 10.0
    if state == "sleep" or state == "lie":
        breath = math.sin(t * 0.5)
        return 0.8 * breath, 1.0 + 0.06 * breath, 0.0, 0.5 * breath, 1.0 + 0.04 * breath
    if state in {"climb", "climb_crawl", "climb_wall"}:
        # Real crawl sprite frames do the animation — only a soft bob (no warp).
        bob = 0.6 * math.sin(t * 2.2)
        return bob, 1.0, 0.0, 0.0, 1.0
    if state == "fall":
        spin = (frame * 14.0) % 360.0 - 180.0
        tumble = math.sin(t * 3.6)
        return 2.0 + 3.0 * abs(tumble), 1.08 + 0.08 * tumble, spin * 0.22, 4.0 * tumble, 0.92 + 0.1 * abs(tumble)
    if state == "happy":
        bounce = abs(math.sin(t * 3.0)) ** 1.3
        return 2.0 + 10.0 * bounce, 0.90 + 0.14 * bounce, 6.0 * math.sin(t * 3.0), 2.5 * math.sin(t * 1.6), 1.0
    if state == "perch" or state == "wait":
        wobble = math.sin(t * 0.7)
        return 1.0 * wobble, 1.0 + 0.02 * wobble, 0.0, 0.0, 1.0
    if state in {"wander", "follow", "play"}:
        bob = 4.0 * abs(math.sin(t * 2.0))
        return bob, 0.94 + 0.08 * abs(math.sin(t * 2.0)), 4.0 * math.sin(t * 2.0), 0.0, 1.0
    return 2.0 * math.sin(t * 0.9), 1.0 + 0.03 * math.sin(t * 0.9), 0.0, 0.0, 1.0


def _motion_langfrog(state: str, frame: int) -> tuple[float, float, float, float, float]:
    """浪浪山小蛙：睡觉 / 爬行 / 跌落 / 摸摸 差异拉大。"""
    t = frame / 10.0
    if state in {"sleep", "lie"}:
        breath = math.sin(t * 0.38)
        return (
            0.4 * breath,
            1.0 + 0.09 * breath,
            0.0,
            1.2 * breath,
            1.0 + 0.06 * breath,
        )
    if state in {"climb", "climb_crawl", "climb_wall"}:
        # 多帧爬行精灵负责蠕动；这里只留轻微起伏，禁止压扁拉长图标。
        bob = 0.8 * math.sin(t * 2.4)
        return bob, 1.0, 0.0, 0.8 * math.sin(t * 1.2), 1.0
    if state == "fall":
        spin = (frame * 16.0) % 360.0 - 180.0
        tumble = math.sin(t * 4.5)
        return (
            1.0 + 3.5 * abs(tumble),
            1.05 + 0.12 * tumble,
            spin * 0.28,
            5.0 * tumble,
            0.88 + 0.16 * abs(tumble),
        )
    if state == "happy":
        bounce = abs(math.sin(t * 3.4)) ** 1.25
        twist = math.sin(t * 3.4)
        return (
            2.0 + 14.0 * bounce,
            0.86 + 0.18 * bounce,
            10.0 * twist,
            3.5 * twist,
            1.0 + 0.06 * (1.0 - bounce),
        )
    if state == "perch" or state == "wait":
        wobble = math.sin(t * 0.35) * (0.35 + 0.65 * abs(math.sin(t * 0.12)))
        return 0.4 * wobble, 1.0 + 0.012 * wobble, 1.2 * wobble, 0.0, 1.0
    if state == "follow":
        hop = _hop_envelope(t, pace=3.4, power=1.6)
        return 2.2 + 3.8 * hop, 0.94 + 0.08 * (1.0 - hop), 5.0 + 2.5 * hop, -1.2 * hop, 1.0
    if state == "wander":
        hop = _hop_envelope(t, pace=1.55, power=2.6)
        return 1.0 + 9.5 * hop, 0.92 + 0.12 * hop, 2.5 * math.sin(t * 1.55), 0.0, 1.0
    if state == "play":
        hop = _hop_envelope(t, pace=2.2, power=1.8)
        flip = math.sin(t * 2.2)
        return 2.0 + 11.0 * hop, 0.90 + 0.16 * hop, 12.0 * flip, 3.0 * flip, 1.0
    breath = math.sin(t * 0.75)
    return 1.4 * breath, 1.0 + 0.045 * breath, 2.2 * math.sin(t * 0.55), 1.8 * math.sin(t * 0.4), 1.0


def _painter_dpr(painter: QPainter) -> float:
    try:
        dev = painter.device()
        if dev is not None:
            dpr = float(dev.devicePixelRatioF())
            if dpr >= 0.99:
                return dpr
    except Exception:
        pass
    try:
        from PyQt6.QtGui import QGuiApplication

        screen = QGuiApplication.primaryScreen()
        if screen is not None:
            return max(1.0, float(screen.devicePixelRatio()))
    except Exception:
        pass
    return 1.0


# Logical-size fits at device pixel ratio — avoid soft upsample every paint.
_FIT_CACHE: OrderedDict[tuple, QPixmap] = OrderedDict()
_FIT_CACHE_MAX = 96
_BBOX_CACHE: OrderedDict[int, QRect] = OrderedDict()
_BBOX_CACHE_MAX = 256
# Multi-cel poses: one content span for the whole loop (per-frame bbox fit flashes).
_POSE_FIT_SPAN: dict[tuple[str, str], int] = {}
_POSES_CACHE: dict[str, dict[str, list[QPixmap]]] = {}
_UNIFIED_FIT_POSES = frozenset(
    {
        _POSE_IDLE,
        _POSE_WALK,
        _POSE_SLEEP,
        _POSE_PLAY,
        _POSE_CRAWL,
        _POSE_WAIT,
        _POSE_HOLD,
        _POSE_FALL,
        _POSE_TRASH,
    }
)


def trim_fit_cache(*, keep: int | None = None) -> None:
    """LRU-trim fitted sprite pixmaps (tray/background memory pressure)."""
    target = int(keep) if keep is not None else max(16, _FIT_CACHE_MAX // 2)
    while len(_FIT_CACHE) > target:
        _FIT_CACHE.popitem(last=False)
    bbox_target = max(32, _BBOX_CACHE_MAX // 2)
    while len(_BBOX_CACHE) > bbox_target:
        _BBOX_CACHE.popitem(last=False)


def _opaque_bbox(pix: QPixmap) -> QRect:
    """Bounding rect of non-transparent pixels (cached per pixmap)."""
    key = int(pix.cacheKey())
    hit = _BBOX_CACHE.get(key)
    if hit is not None:
        _BBOX_CACHE.move_to_end(key)
        return hit
    if pix.isNull():
        return QRect()
    img = pix.toImage().convertToFormat(QImage.Format.Format_ARGB32)
    w, h = img.width(), img.height()
    if w < 1 or h < 1:
        return QRect()
    min_x, min_y, max_x, max_y = w, h, -1, -1
    try:
        import numpy as np

        row_bytes = int(img.bytesPerLine())
        buf = np.frombuffer(img.constBits().asarray(img.sizeInBytes()), dtype=np.uint8)
        alpha = buf.reshape(h, row_bytes)[:, 3 : w * 4 : 4]
        ys, xs = np.where(alpha > 24)
        if len(xs):
            min_x, max_x = int(xs.min()), int(xs.max())
            min_y, max_y = int(ys.min()), int(ys.max())
    except Exception:
        for y in range(h):
            for x in range(w):
                if (img.pixel(x, y) >> 24) & 0xFF > 24:
                    if x < min_x:
                        min_x = x
                    if y < min_y:
                        min_y = y
                    if x > max_x:
                        max_x = x
                    if y > max_y:
                        max_y = y
    if max_x < min_x:
        # Fully transparent placeholder — empty content. Do NOT fall back to the
        # full canvas (that poisoned idle fit span when idle_0 was a blank 2k sheet).
        rect = QRect()
    else:
        rect = QRect(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1)
    _BBOX_CACHE[key] = rect
    _BBOX_CACHE.move_to_end(key)
    while len(_BBOX_CACHE) > _BBOX_CACHE_MAX:
        _BBOX_CACHE.popitem(last=False)
    return rect


def _fit_sprite(
    pix: QPixmap,
    logical_w: int,
    logical_h: int,
    dpr: float,
    *,
    scale_boost: float = 1.0,
    body_top_frac: float = 0.0,
    fixed_content_span: int | None = None,
    pose: str = "",
    character: str = "",
) -> QPixmap:
    """Scale so opaque content span matches across poses (idle ≈ sleep ≈ play)."""
    if pix.isNull() or logical_w < 1 or logical_h < 1:
        return pix
    dpr = max(1.0, float(dpr))
    boost = max(0.5, float(scale_boost) or 1.0)
    top_frac = max(0.0, min(0.6, float(body_top_frac) or 0.0))
    span_key = int(fixed_content_span) if fixed_content_span else 0
    key = (
        int(pix.cacheKey()),
        int(logical_w),
        int(logical_h),
        round(dpr, 2),
        round(boost, 3),
        round(top_frac, 3),
        span_key,
        pose,
        char_id if (char_id := normalize_pet_character(character) if character else "") else "",
    )
    hit = _FIT_CACHE.get(key)
    if hit is not None and not hit.isNull():
        _FIT_CACHE.move_to_end(key)
        return hit

    bbox = _opaque_bbox(pix)
    content_w = max(1, bbox.width())
    content_h = max(1, bbox.height())
    if top_frac > 0.0 and content_h > 16:
        # Pinch fingers live in the top of the hold sheet; scale to the body.
        content_h = max(8, content_h - int(round(content_h * top_frac)))
    if fixed_content_span is not None and fixed_content_span > 0:
        content_span = int(fixed_content_span)
        clamp_w = float(content_span)
    else:
        content_span = max(content_w, content_h)
        clamp_w = float(content_w)
    fill_h = float(logical_h)
    char_id = normalize_pet_character(character) if character else ""
    if pose == _POSE_TRASH:
        # Dest height = body slot + flight headroom (pad_t ≈ body_h). Fill only
        # the body slot so idle→trash does not pop ~1.4× larger (开场闪烁).
        fill_h = max(96.0, float(logical_h) / 2.05)
    target_span = max(1.0, fill_h * _CONTENT_HEIGHT_FILL)
    scale = (target_span / float(content_span)) * boost
    # Extreme wide frames: don't let content width dwarf the dest box.
    out_content_w = clamp_w * scale
    if pose == _POSE_TRASH:
        max_w_mult = _MAX_WIDTH_OVER_DEST_TRASH
    elif pose in {_POSE_WAIT, _POSE_SLEEP} and char_id == "hoodie":
        max_w_mult = _MAX_WIDTH_OVER_DEST_HOODIE_REST
    else:
        max_w_mult = _MAX_WIDTH_OVER_DEST
    max_w = logical_w * max_w_mult
    if out_content_w > max_w:
        scale = max_w / clamp_w
    # Reclining sheets keep transparent headroom above the body bbox — scaling
    # only on body height can blow the full cel past the dest (hoodie wait).
    out_h = pix.height() * scale
    max_h = logical_h * 0.96
    if out_h > max_h:
        scale = max_h / float(pix.height())
    out_content_w = clamp_w * scale
    if out_content_w > max_w:
        scale = max_w / clamp_w

    # Trash draws the full cel pixmap (not just opaque bbox). Clamp using full
    # canvas size so foot-anchored wide throw frames do not overflow the dest
    # on HiDPI (was clipped + smeared at edges).
    if pose == _POSE_TRASH:
        max_log_h = logical_h * 0.96
        max_log_w = logical_w * max_w_mult
        if pix.height() * scale > max_log_h:
            scale = max_log_h / float(pix.height())
        if pix.width() * scale > max_log_w:
            scale = max_log_w / float(pix.width())

    phys_w = max(1, int(round(pix.width() * scale * dpr)))
    phys_h = max(1, int(round(pix.height() * scale * dpr)))
    # Smooth for all poses (incl. trash). Fast/nearest made throw cels look
    # low-res / 马赛克 vs wait/idle which already used Smooth.
    xform = Qt.TransformationMode.SmoothTransformation
    fitted = pix.scaled(
        phys_w,
        phys_h,
        Qt.AspectRatioMode.IgnoreAspectRatio,  # already chose uniform scale
        xform,
    )
    if not fitted.isNull():
        fitted.setDevicePixelRatio(dpr)
    if pose == _POSE_TRASH and not fitted.isNull():
        max_lw = logical_w * max_w_mult
        max_lh = logical_h * 0.96
        lw, lh = float(fitted.width()), float(fitted.height())
        if lw > max_lw + 0.5 or lh > max_lh + 0.5:
            shrunk = fitted.scaled(
                max(1, int(round(max_lw))),
                max(1, int(round(max_lh))),
                Qt.AspectRatioMode.KeepAspectRatio,
                Qt.TransformationMode.SmoothTransformation,
            )
            if not shrunk.isNull():
                fitted = shrunk
                fitted.setDevicePixelRatio(dpr)
    _FIT_CACHE[key] = fitted
    _FIT_CACHE.move_to_end(key)
    while len(_FIT_CACHE) > _FIT_CACHE_MAX:
        _FIT_CACHE.popitem(last=False)
    return fitted


def _fitted_foot_offset(
    pix: QPixmap,
    fitted: QPixmap,
    *,
    body_top_frac: float = 0.0,
) -> tuple[float, float]:
    """Opaque bottom-center in logical fitted coordinates (stabilizes multi-cel loops)."""
    del body_top_frac  # foot uses full opaque bbox; top crop only affects scale in _fit_sprite
    bbox = _opaque_bbox(pix)
    if pix.width() < 1 or pix.height() < 1 or fitted.width() < 1 or fitted.height() < 1:
        return fitted.width() / 2.0, float(fitted.height())
    sx = fitted.width() / float(pix.width())
    sy = fitted.height() / float(pix.height())
    foot_x = (bbox.x() + bbox.width() / 2.0) * sx
    foot_y = (bbox.y() + bbox.height()) * sy
    return foot_x, foot_y


def _draw_one(
    painter: QPainter,
    pix: QPixmap,
    *,
    dest: QRect,
    opacity: float,
    bob: float,
    squash: float,
    tilt: float,
    sway: float,
    facing: int,
    stretch: float = 1.0,
    scale_boost: float = 1.0,
    body_top_frac: float = 0.0,
    pose: str = "",
    character: str = "",
    trash_foot_xy: tuple[float, float] | None = None,
) -> None:
    if pix.isNull() or opacity <= 0.01:
        return
    dpr = _painter_dpr(painter)
    char_id = normalize_pet_character(character) if character else ""
    try:
        from src.desktop_pet import pet_character_draw_scale

        scale_boost *= pet_character_draw_scale(char_id)
    except Exception:
        pass
    fixed_span = _effective_fit_span(char_id, pose) if pose else None
    fitted = _fit_sprite(
        pix,
        dest.width(),
        dest.height(),
        dpr,
        scale_boost=scale_boost,
        body_top_frac=body_top_frac,
        fixed_content_span=fixed_span,
        pose=pose,
        character=char_id,
    )
    if fitted.isNull():
        return
    lw = fitted.width()
    lh = fitted.height()
    face = facing if facing != 0 else 1
    stretch = max(0.55, min(1.55, float(stretch)))
    floor_bias = max(0.0, (1.0 - squash) * lh * 0.18)
    use_foot_anchor = fixed_span is not None or (
        char_id == "hoodie" and pose in {_POSE_WAIT, _POSE_SLEEP}
    )
    if pose == _POSE_TRASH:
        # Cels are authored with feet at canvas center — center-draw like idle.
        use_foot_anchor = False
    if use_foot_anchor:
        if trash_foot_xy is not None:
            foot_x, foot_y = trash_foot_xy
        else:
            foot_x, foot_y = _fitted_foot_offset(pix, fitted, body_top_frac=body_top_frac)
        if pose == _POSE_WAIT and char_id == "hoodie":
            # Prone phone: feet extend past torso — bias anchor for facing.
            if face < 0:
                anchor_x = dest.left() + int(dest.width() * 0.58)
            else:
                anchor_x = dest.left() + int(dest.width() * 0.42)
            cx = anchor_x + int(sway * face) - int(face * stretch * (foot_x - lw / 2))
        else:
            cx = dest.center().x() + int(sway * face) - int(
                face * stretch * (foot_x - lw / 2)
            )
        if pose in {_POSE_WAIT, _POSE_SLEEP} and char_id == "hoodie":
            # Prone rest poses — pin pixmap bottom to dest floor.
            cy = dest.bottom() + int(bob) - lh // 2
        else:
            cy = dest.bottom() + int(bob) + int(floor_bias) - int(
                squash * (foot_y - lh / 2)
            )
    else:
        cx = dest.center().x() + int(sway * face)
        cy = dest.bottom() - lh // 2 + int(bob) + int(floor_bias)
    painter.save()
    painter.setOpacity(max(0.0, min(1.0, opacity)))
    # Idle stays crisp; any squash/tilt/stretch uses smooth sampling to kill 锯齿.
    needs_smooth = (
        abs(tilt) > 0.4
        or abs(squash - 1.0) > 0.015
        or abs(stretch - 1.0) > 0.02
    )
    painter.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform, needs_smooth)
    painter.setRenderHint(
        QPainter.RenderHint.Antialiasing,
        False if pose == _POSE_TRASH else True,
    )
    painter.translate(cx, cy + dest.height() * (1.0 - squash) * 0.10)
    # Rotate first, then scale — scale-then-rotate with non-uniform stretch
    # shears climb poses into unreadable diagonals.
    if abs(tilt) > 0.15:
        painter.rotate(tilt)
    painter.scale(face * stretch, squash)
    painter.drawPixmap(-lw // 2, -lh // 2, fitted)
    painter.restore()


def _draw_sprite_cels(
    painter: QPainter,
    cels: list[QPixmap],
    from_idx: int,
    to_idx: int,
    blend_to: float,
    *,
    dest: QRect,
    layer_opacity: float,
    bob: float,
    squash: float,
    tilt: float,
    sway: float,
    facing: int,
    stretch: float = 1.0,
    scale_boost: float = 1.0,
    body_top_frac: float = 0.0,
    pose: str = "",
    character: str = "",
    trash_foot_xy: tuple[float, float] | None = None,
) -> None:
    """Draw one cel or a dissolved pair within the same pose loop."""
    if not cels:
        return
    from_idx = max(0, min(len(cels) - 1, int(from_idx)))
    to_idx = max(0, min(len(cels) - 1, int(to_idx)))
    layer_opacity = max(0.0, min(1.0, float(layer_opacity)))
    if layer_opacity <= 0.01:
        return
    char_id = normalize_pet_character(character) if character else ""
    common = dict(
        dest=dest,
        bob=bob,
        squash=squash,
        tilt=tilt,
        sway=sway,
        facing=facing,
        stretch=stretch,
        scale_boost=scale_boost,
        body_top_frac=body_top_frac,
        pose=pose,
        character=character,
        trash_foot_xy=trash_foot_xy,
    )
    if from_idx == to_idx or blend_to <= 0.01:
        _draw_one(painter, cels[from_idx], opacity=layer_opacity, **common)
        return
    if blend_to >= 0.99:
        _draw_one(painter, cels[to_idx], opacity=layer_opacity, **common)
        return
    _draw_one(painter, cels[from_idx], opacity=layer_opacity * (1.0 - blend_to), **common)
    _draw_one(painter, cels[to_idx], opacity=layer_opacity * blend_to, **common)


def paint_pet_sprite(
    painter: QPainter,
    poses: dict[str, list[QPixmap]] | dict[str, QPixmap],
    *,
    dest: QRect,
    state: str,
    frame: int,
    facing: int,
    now: float = 0.0,
    anim: PetAnimState | None = None,
    character: str | None = None,
) -> None:
    del now
    char_id = normalize_pet_character(character) if character else ""
    # Normalize legacy dict[str, QPixmap] → frame lists
    norm: dict[str, list[QPixmap]] = {}
    for key, val in poses.items():
        if isinstance(val, list):
            norm[key] = val
        else:
            norm[key] = [val] if val is not None else []

    target = pose_for_state(state)
    if anim is not None:
        anim.set_pose(target, character=char_id)
        blend = _ease(anim.blend)
        cur_pose = anim.pose
        prev_pose = anim.prev_pose
        fr = anim.frame
    else:
        blend = 1.0
        cur_pose = target
        prev_pose = target
        fr = frame

    # Hold/fall/dizzy: never stack two cels (crossfade + drag move = double image).
    if state in {"hold", "fall", "dizzy", "trash"}:
        blend = 1.0
        prev_pose = cur_pose

    bob, squash, tilt, sway, stretch = _motion(state, fr, character=char_id)
    cur_frames = norm.get(cur_pose) or norm.get(_POSE_IDLE) or []
    prev_frames = norm.get(prev_pose) or cur_frames
    if not cur_frames:
        return

    def _boost_for(pose: str) -> float:
        return _HOLD_FIT_BOOST if pose == _POSE_HOLD else 1.0

    def _top_frac(pose: str) -> float:
        return _HOLD_BODY_TOP_FRAC if pose == _POSE_HOLD else 0.0

    def _draw_layer(
        frames: list[QPixmap],
        pose: str,
        motion_state: str,
        layer_opacity: float,
        *,
        tilt_scale: float = 1.0,
        sway_scale: float = 1.0,
        single_pix: QPixmap | None = None,
    ) -> None:
        if layer_opacity <= 0.01:
            return
        if single_pix is not None:
            _draw_one(
                painter,
                single_pix,
                dest=dest,
                opacity=layer_opacity,
                bob=bob,
                squash=squash,
                tilt=tilt * tilt_scale,
                sway=sway * sway_scale,
                facing=facing,
                stretch=stretch,
                scale_boost=_boost_for(pose),
                body_top_frac=_top_frac(pose),
                pose=pose,
                character=char_id,
            )
            return
        cels = _pose_cels(frames, motion_state, char_id)
        if not cels:
            return
        if motion_state == "trash":
            idx = _frame_index(frames, fr, motion_state, character=char_id)
            from_i, to_i, blend_to = idx, idx, 1.0
        elif motion_state in _CEL_CROSSFADE_STATES:
            from_i, to_i, blend_to = _cel_crossfade(
                frames, fr, motion_state, character=char_id
            )
        else:
            idx = _frame_index(frames, fr, motion_state, character=char_id)
            from_i, to_i, blend_to = idx, idx, 1.0
        _draw_sprite_cels(
            painter,
            cels,
            from_i,
            to_i,
            blend_to,
            dest=dest,
            layer_opacity=layer_opacity,
            bob=bob,
            squash=squash,
            tilt=tilt * tilt_scale,
            sway=sway * sway_scale,
            facing=facing,
            stretch=stretch,
            scale_boost=_boost_for(pose),
            body_top_frac=_top_frac(pose),
            pose=pose,
            character=char_id,
        )

    if blend < 0.999 and prev_frames:
        # Index the outgoing sheet by its own pose — using the *new* state here
        # (e.g. idle timing on fall cels) cycles the land sheet and flashes.
        if prev_pose == _POSE_FALL:
            _draw_layer(
                prev_frames,
                prev_pose,
                prev_pose,
                1.0 - blend,
                tilt_scale=0.5,
                sway_scale=0.5,
                single_pix=prev_frames[-1],
            )
        else:
            prev_key = state if prev_pose == _POSE_PLAY and state in {"happy", "play"} else prev_pose
            _draw_layer(
                prev_frames,
                prev_pose,
                prev_key,
                1.0 - blend,
                tilt_scale=0.5,
                sway_scale=0.5,
            )
        _draw_layer(cur_frames, cur_pose, state, blend)
    else:
        _draw_layer(cur_frames, cur_pose, state, 1.0)

    if char_id == "langfrog":
        _paint_langfrog_fx(painter, dest, state, fr, facing, blend)
    else:
        if state in {"idle", "happy"} and blend > 0.6:
            _paint_sparkles(painter, dest, fr)
        if state in {"sleep", "lie"}:
            _paint_zzz(painter, dest, fr, character=char_id)
        if state == "play" and blend > 0.5:
            _paint_play_fx(painter, dest, fr)


def _paint_langfrog_fx(
    painter: QPainter,
    dest: QRect,
    state: str,
    frame: int,
    facing: int,
    blend: float,
) -> None:
    if blend < 0.45:
        return
    painter.save()
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    face = 1 if facing >= 0 else -1
    cx = dest.center().x()
    cy = dest.center().y()
    top = dest.top()
    bot = dest.bottom()

    if state == "idle":
        # 山间落叶
        for i, (ox, oy) in enumerate(((-0.32, 0.22), (0.34, 0.14), (0.02, 0.08), (0.22, 0.28))):
            phase = frame * 0.12 + i * 1.4
            alpha = int(70 + 90 * (0.5 + 0.5 * math.sin(phase)))
            x = cx + int(dest.width() * ox) + int(2 * math.sin(phase * 0.7))
            y = top + int(dest.height() * oy) + int(3 * math.sin(phase))
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(90, 150, 70, alpha))
            painter.drawEllipse(x - 2, y - 3, 5, 7)
            painter.setBrush(QColor(140, 190, 90, alpha))
            painter.drawEllipse(x - 1, y - 2, 3, 4)
    elif state == "happy":
        for i in range(3):
            phase = frame * 0.22 + i * 2.0
            alpha = int(120 + 100 * (0.5 + 0.5 * math.sin(phase)))
            x = cx + int(math.cos(phase) * dest.width() * 0.30)
            y = top + 10 + int(i * 8) - int((frame % 20) * 0.6)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(255, 110, 140, alpha))
            painter.drawEllipse(x - 4, y - 3, 8, 7)
            painter.drawEllipse(x - 1, y - 5, 5, 5)
        if frame % 18 < 10:
            painter.setPen(QColor(60, 120, 70, 210))
            painter.setFont(QFont("Microsoft YaHei UI", 9, QFont.Weight.Bold))
            painter.drawText(dest.right() - 28, top + 14, "呱")
    elif state == "play":
        # 翻滚残影点 + 星光
        for i in range(5):
            phase = frame * 0.28 + i * 1.25
            alpha = int(50 + 90 * (0.5 + 0.5 * math.sin(phase)))
            x = cx + int(math.cos(phase) * dest.width() * 0.36)
            y = cy + int(math.sin(phase * 1.4) * dest.height() * 0.22)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(255, 220, 90, alpha))
            painter.drawEllipse(x - 2, y - 2, 4, 4)
            painter.setBrush(QColor(110, 190, 110, alpha // 2))
            painter.drawEllipse(x + 6, y + 4, 5, 5)
        # 短促运动线
        painter.setPen(QPen(QColor(70, 130, 80, 90), 2))
        for i in range(3):
            y = cy - 10 + i * 8
            painter.drawLine(cx - face * 28, y, cx - face * 14, y - 2)
    elif state in {"climb", "climb_crawl", "climb_wall"}:
        # 贴地蠕动：身下细尘点随伸缩节奏冒出
        wave = math.sin(frame / 5.0)
        if wave > 0.2:
            for i in range(3):
                alpha = int(50 + 40 * wave) - i * 12
                x = cx - face * (6 + i * 7)
                y = bot - 3
                painter.setPen(Qt.PenStyle.NoPen)
                painter.setBrush(QColor(150, 135, 100, max(20, alpha)))
                painter.drawEllipse(x, y, 5 + i, 2)
    elif state == "wander":
        hop = _hop_envelope(frame / 10.0, pace=1.55, power=2.6)
        if hop < 0.18:
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(150, 130, 90, 100))
            painter.drawEllipse(cx - 10, bot - 5, 20, 5)
    elif state == "follow":
        painter.setPen(QPen(QColor(100, 160, 110, 70), 2))
        for i in range(3):
            x0 = cx - face * (18 + i * 7)
            y0 = cy - 4 + i * 3
            painter.drawLine(x0, y0, x0 - face * 6, y0 + 1)
    elif state in {"sleep", "lie"}:
        _paint_zzz(painter, dest, frame, character="langfrog")
    elif state == "fall":
        for i in range(4):
            phase = frame * 0.4 + i * 1.1
            alpha = int(80 + 70 * (0.5 + 0.5 * math.sin(phase)))
            x = cx + int(math.cos(phase * 1.7) * dest.width() * 0.25)
            y = cy + int(math.sin(phase) * dest.height() * 0.2)
            painter.setPen(QPen(QColor(255, 240, 180, alpha), 1))
            painter.drawLine(x - 3, y, x + 3, y)
            painter.drawLine(x, y - 3, x, y + 3)
    elif state == "perch":
        # 静坐时偶尔冒小泡
        if (frame // 12) % 5 == 0:
            phase = (frame % 12) / 12.0
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(QColor(180, 220, 160, int(140 * (1.0 - phase))))
            painter.drawEllipse(cx + face * 16, top + 8 - int(phase * 10), 5, 5)

    # 玩耍/开心时偶尔吐舌
    if state in {"play", "happy"} and (frame % 16) in {2, 3, 4}:
        tongue = QRectF(
            cx + face * (dest.width() * 0.12),
            cy + dest.height() * 0.08,
            7,
            5,
        )
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(230, 90, 110, 200))
        painter.drawEllipse(tongue)

    painter.restore()


def _paint_sparkles(painter: QPainter, dest: QRect, frame: int) -> None:
    painter.save()
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    for i, (ox, oy) in enumerate(((-0.28, 0.18), (0.30, 0.12), (0.08, 0.06))):
        phase = frame * 0.18 + i * 1.7
        alpha = int(90 + 110 * (0.5 + 0.5 * math.sin(phase)))
        size = 3 + int(2 * (0.5 + 0.5 * math.sin(phase * 1.4)))
        x = dest.center().x() + int(dest.width() * ox)
        y = dest.top() + int(dest.height() * oy) + int(2 * math.sin(phase))
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(255, 214, 80, alpha))
        painter.drawEllipse(x - size // 2, y - size // 2, size, size)
        painter.setPen(QPen(QColor(255, 255, 230, alpha), 1))
        painter.drawLine(x - size - 1, y, x + size + 1, y)
        painter.drawLine(x, y - size - 1, x, y + size + 1)
    painter.restore()


def _paint_zzz(painter: QPainter, dest: QRect, frame: int, *, character: str = "") -> None:
    """Floating Z near the snoozing head — kept close to the sprite."""
    painter.save()
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    char = normalize_pet_character(character) if character else ""
    if char == "hoodie":
        # One Z stack above the pillow (sprite no longer bakes zzz).
        base_x = dest.center().x() + dest.width() * 0.04
        base_y = dest.top() + max(4, int(dest.height() * 0.24))
    else:
        base_x = dest.center().x() + dest.width() * 0.08
        base_y = dest.top() + max(4, int(dest.height() * 0.12))
    specs = (
        (0.0, 9, -4, 2),
        (0.34, 10, 4, -2),
        (0.68, 11, 10, -6),
    )
    for delay, size, ox, oy0 in specs:
        t = ((frame / 40.0) + delay) % 1.0
        rise = int(t * 12)
        fade = 1.0 - abs(t - 0.45) * 1.6
        alpha = max(0, min(220, int(190 * fade)))
        if alpha < 20:
            continue
        x = int(base_x + ox + 2 * math.sin(t * math.pi * 2))
        y = int(base_y + oy0 - rise)
        painter.setPen(QColor(70, 80, 140, alpha))
        painter.setFont(QFont("Segoe UI", size, QFont.Weight.Bold))
        painter.drawText(x, y, "z")
    painter.restore()


def _paint_play_fx(painter: QPainter, dest: QRect, frame: int) -> None:
    painter.save()
    painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    for i in range(3):
        phase = frame * 0.25 + i * 2.1
        alpha = int(60 + 100 * (0.5 + 0.5 * math.sin(phase)))
        x = dest.center().x() + int(math.cos(phase) * dest.width() * 0.28)
        y = dest.center().y() + int(math.sin(phase * 1.3) * dest.height() * 0.18)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(QColor(120, 200, 120, alpha))
        painter.drawEllipse(x - 3, y - 3, 6, 6)
    painter.restore()
