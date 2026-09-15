"""Desktop pet extension — settings helpers and character catalog."""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any

DEFAULT_CHARACTER = "hoodie"

# Interactive menu / click actions the user can enable in settings.
# ``hide`` is always available so the pet can be tucked away.
PET_ACTION_OPTIONS: tuple[tuple[str, str], ...] = (
    ("pet", "摸摸"),
    ("wait", "等待"),
    ("sleep", "睡觉"),
    ("fall", "跌落"),
)
_PET_ACTION_IDS = frozenset(aid for aid, _label in PET_ACTION_OPTIONS)
# Always enabled; not shown in settings (click / physics defaults).
_DEFAULT_ALWAYS_ON_PET_ACTIONS = frozenset({"pet", "fall"})
DEFAULT_ENABLED_ACTIONS: tuple[str, ...] = ("pet", "wait", "fall")
HOODIE_DEFAULT_ENABLED_ACTIONS: tuple[str, ...] = ("pet", "wait", "sleep", "fall")
# Old settings stored a combined「等待」; only ``wait`` maps to the legacy bucket.
_LEGACY_WAIT_ACTION_IDS = frozenset({"wait"})
_ALWAYS_ON_ACTIONS = frozenset({"hide"})
DEFAULT_SIZE_SCALE = 1.0
MIN_SIZE_SCALE = 0.5
MAX_SIZE_SCALE = 1.6

_PET_CHARACTERS: dict[str, dict[str, Any]] = {
    "hoodie": {
        "name": "卫衣少年",
        "sprite": "hoodie_idle.png",
        "poses": {
            "idle": "hoodie_idle.png",
            "stand": "hoodie_stand.png",
            "walk": "hoodie_walk.png",
            "sleep": "hoodie_sleep.png",
            "play": "hoodie_play.png",
            "wait": "hoodie_wait.png",
            "hold": "hoodie_hold.png",
            "fall": "hoodie_fall.png",
            "trash": "hoodie_trash.png",
        },
        "lines": (
            "嗨～今天也要加油！",
            "Love Your Hood～",
            "拖我可以换位置。",
            "把文件拖给我，我帮你折成纸飞机飞走。",
            "困了就让我睡一会儿。",
        ),
        "pet": ("嘿嘿，好啦好啦。", "谢谢你～", "再摸摸！"),
        "sleep": ("先眯一会儿……", "Zzz……"),
        "wake": ("醒啦！", "伸个懒腰～"),
        "wait": ("趴着打会儿游戏～", "等你哦～", "这关马上过！"),
        "climb": ("我去边上趴会儿～", "慢慢挪过去。"),
        "fall": ("哇——！", "落地落地！"),
        "trash": ("折个纸飞机～", "飞走啦！", "帮你删掉咯。"),
        "sleep_on_rest": True,  # 等待时与「玩手机」轮流切换
    },
    "voyage": {
        "name": "出海少女",
        "sprite": "voyage_idle.png",
        "poses": {
            "idle": "voyage_idle.png",
            "stand": "voyage_stand.png",
            "walk": "voyage_walk.png",
            "sleep": "voyage_sleep.png",
            "play": "voyage_play.png",
            "wait": "voyage_wait.png",
            "hold": "voyage_hold.png",
            "fall": "voyage_fall.png",
            "trash": "voyage_trash.png",
        },
        "lines": (
            "今天也要乘风破浪～",
            "海风好舒服！",
            "拖我可以换位置。",
            "把文件拖给我，我帮你扔进回收站。",
            "耶！出海啦！",
        ),
        "pet": ("耶！好开心！", "谢谢你～", "再摸摸！", "跳起来！"),
        "sleep": ("在甲板上眯一会儿……", "Zzz……"),
        "wake": ("醒啦！", "伸个懒腰～"),
        "wait": ("刷刷手机～", "这条好有趣！", "等你哦，我再看一眼。"),
        "climb": ("我去边上靠会儿～", "慢慢挪过去。"),
        "fall": ("哇——飞起来啦！", "噗通，落地！"),
        "trash": ("揉成一团～", "进回收站啦。", "帮你扔掉。"),
        "sleep_on_rest": False,
        "dizzy_sec": (2.2, 3.0),
        "happy_sec": 3.2,
    },
}

_COMMON_LINES: tuple[str, ...] = (
    "你好呀～",
    "今天也要加油！",
    "单击摸摸我，点右上角「⋯」有更多互动。",
    "把文件拖给我，我帮你扔进回收站。",
    "拖我甩出去会跌落哦。",
)

_ACTION_KEYS = ("pet", "sleep", "wake", "wait", "climb", "fall", "trash")


def _resource_base() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys._MEIPASS)
    return Path(__file__).resolve().parent.parent


def pet_catalog() -> dict[str, dict[str, Any]]:
    return dict(_PET_CHARACTERS)


def pet_character_ids() -> tuple[str, ...]:
    return tuple(_PET_CHARACTERS.keys())


def normalize_pet_character(character: str | None) -> str:
    # Retired ids (e.g. duoduo 金多多) fall back to default.
    if character and character in _PET_CHARACTERS:
        return character
    return DEFAULT_CHARACTER


def pet_action_options(character: str | None = None) -> tuple[tuple[str, str], ...]:
    """Per-character interactive menu / settings labels."""
    char_id = normalize_pet_character(character or DEFAULT_CHARACTER)
    labels = dict(PET_ACTION_OPTIONS)
    if char_id == "hoodie":
        labels["wait"] = "玩手机"
        labels["sleep"] = "睡觉"
        return tuple((aid, labels[aid]) for aid in ("pet", "wait", "sleep", "fall"))
    if char_id == "voyage":
        labels["wait"] = "刷手机"
    return tuple((aid, labels[aid]) for aid in ("pet", "wait", "fall"))


def pet_menu_action_options(character: str | None = None) -> tuple[tuple[str, str], ...]:
    """⋯ menu entries — 摸摸 is click-only; 跌落 is drag/physics only."""
    return tuple(
        (aid, label)
        for aid, label in pet_action_options(character)
        if aid not in _DEFAULT_ALWAYS_ON_PET_ACTIONS
    )


def pet_settings_action_options(character: str | None = None) -> tuple[tuple[str, str], ...]:
    """Settings UI toggles only — 摸摸/跌落 are always on and hidden."""
    return pet_menu_action_options(character)


def _ensure_default_pet_actions(chosen: list[str]) -> list[str]:
    order = {aid: i for i, (aid, _) in enumerate(PET_ACTION_OPTIONS)}
    merged = set(chosen) | _DEFAULT_ALWAYS_ON_PET_ACTIONS
    return sorted(merged, key=lambda a: order.get(a, 99))


def pet_sleep_on_rest(character: str | None) -> bool:
    """Whether auto-rest alternates into sleep (some chars only wait)."""
    meta = _PET_CHARACTERS.get(normalize_pet_character(character), {})
    return bool(meta.get("sleep_on_rest", True))


def pet_dizzy_sec(character: str | None) -> tuple[float, float]:
    """How long the pet stays stunned after landing (seconds)."""
    meta = _PET_CHARACTERS.get(normalize_pet_character(character), {})
    raw = meta.get("dizzy_sec")
    if isinstance(raw, (list, tuple)) and len(raw) >= 2:
        return float(raw[0]), float(raw[1])
    return 1.0, 1.5


def pet_happy_sec(character: str | None) -> float:
    """How long click-cheer (pet/feed) plays before returning to idle."""
    meta = _PET_CHARACTERS.get(normalize_pet_character(character), {})
    raw = meta.get("happy_sec")
    if isinstance(raw, (int, float)) and float(raw) > 0:
        return float(raw)
    return 2.0


def pet_character_draw_scale(character: str | None) -> float:
    """Per-character sprite scale on the shared dest box (1.0 = default)."""
    meta = _PET_CHARACTERS.get(normalize_pet_character(character), {})
    try:
        scale = float(meta.get("draw_scale", 1.0))
    except (TypeError, ValueError):
        return 1.0
    return max(0.72, min(1.15, scale))


def pet_character_name(character: str | None) -> str:
    meta = _PET_CHARACTERS.get(normalize_pet_character(character))
    return str(meta.get("name", "桌面宠物")) if meta else "桌面宠物"


def pet_sprite_path(character: str | None, pose: str = "idle") -> Path:
    """Resolve a pose or numbered frame path under assets/pets/."""
    char_id = normalize_pet_character(character)
    meta = _PET_CHARACTERS[char_id]
    poses = meta.get("poses") if isinstance(meta.get("poses"), dict) else {}
    base_dir = _resource_base() / "assets" / "pets"

    # Explicit pose map hit (idle / walk / sleep / play_0 …)
    if pose in poses:
        path = base_dir / str(poses[pose])
        if path.is_file():
            return path

    # Numbered frame: sleep_0 → <stem>_0.png from poses["sleep"]
    if "_" in pose:
        base_pose, maybe_idx = pose.rsplit("_", 1)
        if maybe_idx.isdigit():
            template = poses.get(base_pose)
            if template:
                stem = Path(str(template)).stem
                numbered = base_dir / f"{stem}_{maybe_idx}.png"
                if numbered.is_file():
                    return numbered
                # Missing frame — stop here so loaders don't fall back to idle/sprite.
                return numbered
            alt = base_dir / f"{char_id}_{pose}.png"
            return alt if alt.is_file() else alt

    # Convention: {char}_{pose}.png
    path = base_dir / f"{char_id}_{pose}.png"
    if path.is_file():
        return path

    filename = str(poses.get(pose) or meta.get("sprite") or f"{char_id}.png")
    path = base_dir / filename
    if not path.is_file() and pose != "idle":
        return pet_sprite_path(char_id, "idle")
    return path


def desktop_pet_settings(settings: dict[str, Any] | None) -> dict[str, Any]:
    if settings is None:
        return {"enabled": True}
    raw = settings.get("desktop_pet")
    if not isinstance(raw, dict):
        raw = {"enabled": True}
        settings["desktop_pet"] = raw
    raw.setdefault("enabled", True)
    raw.setdefault("visible", True)
    raw.setdefault("follow_cursor", False)
    raw.setdefault("auto_wander", True)
    raw.setdefault("show_page_bubbles", True)
    raw.setdefault("character", DEFAULT_CHARACTER)
    raw.setdefault("size_scale", DEFAULT_SIZE_SCALE)
    raw["character"] = normalize_pet_character(str(raw.get("character", DEFAULT_CHARACTER)))
    legacy_actions = raw.get("enabled_actions")
    raw["enabled_actions_by_character"] = normalize_pet_actions_by_character(
        raw.get("enabled_actions_by_character"),
        legacy_global=legacy_actions,
    )
    # Mirror active character for older readers / exports.
    raw["enabled_actions"] = list(raw["enabled_actions_by_character"][raw["character"]])
    raw["size_scale"] = normalize_pet_size_scale(raw.get("size_scale"))
    return raw


def normalize_pet_size_scale(raw: Any) -> float:
    """Pet on-screen scale; 1.0 = default. Clamped to a sensible range."""
    try:
        value = float(raw)
    except (TypeError, ValueError):
        return DEFAULT_SIZE_SCALE
    if value != value:  # NaN
        return DEFAULT_SIZE_SCALE
    return max(MIN_SIZE_SCALE, min(MAX_SIZE_SCALE, value))


def pet_size_scale(settings: dict[str, Any] | None) -> float:
    return normalize_pet_size_scale(desktop_pet_settings(settings).get("size_scale"))


def normalize_pet_actions_by_character(
    raw: Any,
    *,
    legacy_global: Any = None,
) -> dict[str, list[str]]:
    """Normalize per-character enabled action lists; fill gaps from legacy global."""
    char_ids = pet_character_ids()
    legacy = normalize_pet_enabled_actions(legacy_global)
    result: dict[str, list[str]] = {}

    if isinstance(raw, dict):
        for cid in char_ids:
            if cid in raw:
                result[cid] = normalize_pet_enabled_actions(raw[cid])

    for cid in char_ids:
        result.setdefault(cid, list(legacy))
        if cid == "hoodie" and "sleep" not in result[cid] and "wait" in result[cid]:
            result[cid] = sorted(
                set(result[cid]) | {"sleep"},
                key=lambda a: {aid: i for i, (aid, _) in enumerate(PET_ACTION_OPTIONS)}.get(a, 99),
            )

    return result


def normalize_pet_enabled_actions(raw: Any) -> list[str]:
    """Return enabled interactive action ids; default is 摸摸/等待/跌落."""
    if isinstance(raw, dict):
        chosen = [aid for aid, _ in PET_ACTION_OPTIONS if bool(raw.get(aid))]
        return _ensure_default_pet_actions(chosen if chosen else list(DEFAULT_ENABLED_ACTIONS))
    if isinstance(raw, (list, tuple, set)):
        raw_list = [str(x) for x in raw]
        raw_set = set(raw_list)
        # Old shipped defaults included climb and/or split sleep+wait.
        if raw_set in (
            {"pet", "climb", "sleep", "wait", "fall"},
            {"pet", "sleep", "wait", "fall"},
        ):
            return list(HOODIE_DEFAULT_ENABLED_ACTIONS)
        chosen = [x for x in raw_list if x in _PET_ACTION_IDS]
        if raw_set & _LEGACY_WAIT_ACTION_IDS and "wait" not in chosen:
            chosen.append("wait")
        order = {aid: i for i, (aid, _) in enumerate(PET_ACTION_OPTIONS)}
        chosen = sorted(set(chosen), key=lambda a: order.get(a, 99))
        # Old pet-only default → promote to the current interactive set.
        if chosen == ["pet"]:
            return list(DEFAULT_ENABLED_ACTIONS)
        return _ensure_default_pet_actions(chosen if chosen else list(DEFAULT_ENABLED_ACTIONS))
    return list(DEFAULT_ENABLED_ACTIONS)


def pet_enabled_actions(
    settings: dict[str, Any] | None,
    *,
    character: str | None = None,
) -> list[str]:
    cfg = desktop_pet_settings(settings)
    char_id = normalize_pet_character(character or str(cfg.get("character", DEFAULT_CHARACTER)))
    by_char = cfg.get("enabled_actions_by_character")
    if isinstance(by_char, dict) and char_id in by_char:
        return normalize_pet_enabled_actions(by_char[char_id])
    return normalize_pet_enabled_actions(cfg.get("enabled_actions"))


def set_pet_enabled_actions_for_character(
    cfg: dict[str, Any],
    character: str,
    actions: list[str],
) -> None:
    """Write enabled actions for one character; keeps map + legacy mirror in sync."""
    char_id = normalize_pet_character(character)
    chosen = normalize_pet_enabled_actions(actions)
    by_char = cfg.get("enabled_actions_by_character")
    if not isinstance(by_char, dict):
        by_char = normalize_pet_actions_by_character(None, legacy_global=cfg.get("enabled_actions"))
    by_char = dict(by_char)
    by_char[char_id] = chosen
    cfg["enabled_actions_by_character"] = by_char
    if normalize_pet_character(str(cfg.get("character", DEFAULT_CHARACTER))) == char_id:
        cfg["enabled_actions"] = list(chosen)


def pet_action_enabled(
    settings: dict[str, Any] | None,
    action_id: str,
    *,
    character: str | None = None,
) -> bool:
    aid = str(action_id or "")
    if aid in _ALWAYS_ON_ACTIONS:
        return True
    enabled = set(pet_enabled_actions(settings, character=character))
    return aid in enabled


def desktop_pet_enabled(settings: dict[str, Any] | None) -> bool:
    return bool(desktop_pet_settings(settings).get("enabled", True))


def desktop_pet_visible(settings: dict[str, Any] | None) -> bool:
    cfg = desktop_pet_settings(settings)
    return bool(cfg.get("enabled", False)) and bool(cfg.get("visible", True))


def desktop_pet_hosts_float_bar(settings: dict[str, Any] | None) -> bool:
    """True when the right-edge float bar lives on the pet (edge bar hidden)."""
    if not desktop_pet_visible(settings):
        return False
    return bool(desktop_pet_settings(settings).get("show_page_bubbles", True))


def desktop_pet_shows_page_bubbles(settings: dict[str, Any] | None) -> bool:
    """Compat alias — pet hosts the full float bar (pages + tools + folders)."""
    return desktop_pet_hosts_float_bar(settings)


def float_bar_tool_flags(settings: dict[str, Any] | None) -> dict[str, bool]:
    """Which float-bar tool chips are enabled (shared by edge bar and pet)."""
    settings = settings if isinstance(settings, dict) else {}
    from src.calculator import calculator_enabled
    from src.meeting_minutes import meeting_minutes_enabled
    from src.todos import desktop_todos_enabled

    rec = settings.get("screen_record", {})
    rec_on = bool(rec.get("enabled", False)) if isinstance(rec, dict) else False
    return {
        "record": rec_on,
        # Notepad moved to deskNote.lnk (Desktop + Start Menu); never on float bar.
        "note": False,
        "minutes": meeting_minutes_enabled(settings),
        "calculator": calculator_enabled(settings),
        "todo": desktop_todos_enabled(settings),
        # Account vault uses Doubao-style desktop launcher; never on float bar.
        "vault": False,
        "pet": False,
    }

def pet_lines(settings: dict[str, Any] | None = None) -> tuple[str, ...]:
    cfg = desktop_pet_settings(settings)
    char_id = normalize_pet_character(str(cfg.get("character", DEFAULT_CHARACTER)))
    meta = _PET_CHARACTERS[char_id]
    lines = meta.get("lines")
    if isinstance(lines, (list, tuple)) and lines:
        return tuple(str(line) for line in lines)
    return _COMMON_LINES


def pet_action_lines(
    settings: dict[str, Any] | None,
    action: str,
) -> tuple[str, ...]:
    cfg = desktop_pet_settings(settings)
    char_id = normalize_pet_character(str(cfg.get("character", DEFAULT_CHARACTER)))
    meta = _PET_CHARACTERS[char_id]
    key = action if action in _ACTION_KEYS else ""
    lines = meta.get(key) if key else None
    if isinstance(lines, (list, tuple)) and lines:
        return tuple(str(line) for line in lines)
    return pet_lines(settings)


def random_pet_line(
    settings: dict[str, Any] | None = None,
    *,
    action: str | None = None,
    seed: int | None = None,
) -> str:
    import random

    rng = random.Random(seed)
    pool = pet_action_lines(settings, action) if action else pet_lines(settings)
    return rng.choice(pool)
