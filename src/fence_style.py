"""Fence visual style presets (360桌面助手-like translucent panels).

Presets only touch paint keys (background / accent / opacity / radius).
Per-fence view_mode / show_title / collapsible stay intact.
Icon caption colors are intentionally NOT part of presets.
"""

from __future__ import annotations

from typing import Any

# Visual keys written by presets / global apply.
FENCE_STYLE_VISUAL_KEYS: tuple[str, ...] = (
    "opacity",
    "background",
    "accent",
    "border_radius",
)

# Soft translucent packs — closer to 360桌面助手 color chips (tint + alpha).
FENCE_STYLE_PRESETS: dict[str, dict[str, Any]] = {
    "charcoal": {
        "name": "深空灰",
        "hint": "半透明深灰（默认）",
        "opacity": 0.70,
        "background": "#2A2E35",
        "accent": "#4C8DFF",
        "border_radius": 8,
    },
    "slate": {
        "name": "岩灰",
        "hint": "中性灰半透明",
        "opacity": 0.66,
        "background": "#3D4450",
        "accent": "#7DD3FC",
        "border_radius": 8,
    },
    "frost": {
        "name": "霜透白",
        "hint": "浅白半透明",
        "opacity": 0.52,
        "background": "#F4F6F8",
        "accent": "#3B82F6",
        "border_radius": 8,
    },
    "mist_blue": {
        "name": "晴空蓝",
        "hint": "淡蓝半透明",
        "opacity": 0.55,
        "background": "#C5E1F5",
        "accent": "#1D7BB8",
        "border_radius": 8,
    },
    "forest": {
        "name": "清新绿",
        "hint": "浅绿半透明",
        "opacity": 0.55,
        "background": "#C8EBD6",
        "accent": "#1E9B55",
        "border_radius": 8,
    },
    "sunset": {
        "name": "暖沙",
        "hint": "浅杏半透明",
        "opacity": 0.55,
        "background": "#F8D5B5",
        "accent": "#E07A2F",
        "border_radius": 8,
    },
}

DEFAULT_FENCE_STYLE_PRESET = "charcoal"


def normalize_fence_style_preset(preset_id: str | None) -> str:
    key = str(preset_id or "").strip()
    if key in FENCE_STYLE_PRESETS:
        return key
    return DEFAULT_FENCE_STYLE_PRESET


def fence_style_preset_ids() -> list[str]:
    return list(FENCE_STYLE_PRESETS.keys())


def fence_style_preset(preset_id: str | None) -> dict[str, Any]:
    key = normalize_fence_style_preset(preset_id)
    return dict(FENCE_STYLE_PRESETS[key])


def fence_style_visual_from_preset(preset_id: str | None) -> dict[str, Any]:
    """Paint-only dict suitable for merging into fence ``style``."""
    preset = fence_style_preset(preset_id)
    return {k: preset[k] for k in FENCE_STYLE_VISUAL_KEYS}


def merge_fence_style_visual(
    style: dict[str, Any] | None,
    visual: dict[str, Any] | None,
) -> dict[str, Any]:
    """Merge visual keys into an existing fence style (keep view / title flags)."""
    out = dict(style) if isinstance(style, dict) else {}
    if not isinstance(visual, dict):
        return out
    for key in FENCE_STYLE_VISUAL_KEYS:
        if key not in visual:
            continue
        value = visual[key]
        if key == "opacity":
            try:
                out[key] = max(0.25, min(1.0, float(value)))
            except (TypeError, ValueError):
                continue
        elif key == "border_radius":
            try:
                out[key] = max(0, min(28, int(value)))
            except (TypeError, ValueError):
                continue
        elif key in ("background", "accent"):
            text = str(value or "").strip()
            if text.startswith("#") and len(text) >= 7:
                out[key] = text[:7].upper() if len(text) == 7 else text
        else:
            out[key] = value
    return out


def apply_preset_to_fence_config(fence: dict[str, Any], preset_id: str | None) -> bool:
    """Write preset visuals onto one fence config. Returns True if changed."""
    if not isinstance(fence, dict):
        return False
    key = normalize_fence_style_preset(preset_id)
    visual = fence_style_visual_from_preset(key)
    before = dict(fence.get("style") or {})
    merged = merge_fence_style_visual(before, visual)
    changed = merged != before or fence.get("style_preset") != key
    fence["style"] = merged
    fence["style_preset"] = key
    return changed


def match_fence_style_preset(fence: dict[str, Any] | None) -> str:
    """Resolve which catalog preset a fence currently looks like."""
    if not isinstance(fence, dict):
        return DEFAULT_FENCE_STYLE_PRESET
    keyed = str(fence.get("style_preset") or "").strip()
    if keyed in FENCE_STYLE_PRESETS:
        return keyed
    style = fence.get("style") if isinstance(fence.get("style"), dict) else {}
    bg = str(style.get("background") or "").strip().upper()
    if bg:
        for preset_id, meta in FENCE_STYLE_PRESETS.items():
            if str(meta.get("background") or "").strip().upper() == bg:
                return preset_id
    return DEFAULT_FENCE_STYLE_PRESET


def apply_preset_to_settings(settings: dict[str, Any], preset_id: str | None) -> bool:
    """Apply preset to every fence + remember ``fence_style_preset``."""
    key = normalize_fence_style_preset(preset_id)
    changed = False
    if settings.get("fence_style_preset") != key:
        settings["fence_style_preset"] = key
        changed = True
    fences = settings.get("fences")
    if not isinstance(fences, list):
        return changed
    for fence in fences:
        if apply_preset_to_fence_config(fence, key):
            changed = True
    return changed


def settings_fence_style_preset(settings: dict[str, Any] | None) -> str:
    if not isinstance(settings, dict):
        return DEFAULT_FENCE_STYLE_PRESET
    return normalize_fence_style_preset(settings.get("fence_style_preset"))


_LEGACY_LIGHT_BACKGROUNDS = frozenset(
    {
        "#FFFFFF",
        "#F8FAFC",
        "#FFFBEB",
        "#FEF3C7",
        "#EFF6FF",
    }
)


def migrate_legacy_fence_styles(user_raw: dict[str, Any], merged: dict[str, Any]) -> bool:
    """First boot after style-honor fix: keep charcoal look for legacy light JSON.

    Older builds stored light ``background`` hexes but coerced paint to charcoal.
    Without migration, honoring style would suddenly flash all fences white.
    """
    if not isinstance(merged, dict):
        return False
    if isinstance(user_raw, dict) and "fence_style_preset" in user_raw:
        return False
    if merged.get("fence_style_preset"):
        # Came from defaults merge — still rewrite legacy light fence paints once.
        pass
    merged["fence_style_preset"] = DEFAULT_FENCE_STYLE_PRESET
    changed = True
    fences = merged.get("fences")
    if not isinstance(fences, list):
        return changed
    for fence in fences:
        if not isinstance(fence, dict):
            continue
        style = fence.get("style")
        if not isinstance(style, dict):
            apply_preset_to_fence_config(fence, DEFAULT_FENCE_STYLE_PRESET)
            continue
        bg = str(style.get("background") or "").strip().upper()
        if not bg or bg in _LEGACY_LIGHT_BACKGROUNDS:
            apply_preset_to_fence_config(fence, DEFAULT_FENCE_STYLE_PRESET)
    return changed
