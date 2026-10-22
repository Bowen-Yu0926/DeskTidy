"""Copy Python DeskTidy layout + pins into the native data dir.

Reads %USERPROFILE%\\.desktidy\\settings.json and writes
%LOCALAPPDATA%\\DeskTidyNative\\layout.json + pins.json.
Existing native files are copied to *.bak-python-import first.
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path


def _native_dir() -> Path:
    return Path.home() / "AppData" / "Local" / "DeskTidyNative"


def _pin_key(path: str) -> str:
    return path.replace("/", "\\").lower()


def _backup(path: Path) -> None:
    if path.is_file():
        shutil.copy2(path, path.with_suffix(path.suffix + ".bak-python-import"))


def _geo_from_layouts(settings: dict, fence_id: str, page: int) -> dict | None:
    """Prefer fence_layouts_by_page, then fence object fields."""
    by_page = settings.get("fence_layouts_by_page") or {}
    page_map = by_page.get(str(page)) or by_page.get(page) or {}
    if isinstance(page_map, dict):
        geo = page_map.get(fence_id)
        if isinstance(geo, dict) and (
            geo.get("width") or geo.get("w") or geo.get("height") or geo.get("h")
        ):
            return geo
    return None


def main() -> int:
    src = Path.home() / ".desktidy" / "settings.json"
    if not src.is_file():
        print("no python settings:", src)
        return 1
    settings = json.loads(src.read_text(encoding="utf-8"))
    pages_raw = settings.get("desktop_pages") or []
    page_names = []
    for page in pages_raw:
        if isinstance(page, dict) and page.get("name"):
            page_names.append(str(page["name"]))
    if len(page_names) < 2:
        page_names = ["工作", "文档"]

    fences = []
    pins: dict[str, str] = {}
    for fence in settings.get("fences") or []:
        if not isinstance(fence, dict):
            continue
        fid = str(fence.get("id") or "")
        if not fid:
            continue
        # Normalize legacy ids to current Python system ids.
        if fid == "system_software":
            fid = "system_software_side"
        elif fid == "system_page2":
            fid = "system_docs"
        title = str(fence.get("name") or fence.get("title") or fid)
        if fid == "system_software_side" and title in ("软件", ""):
            title = "其他"
        if fid == "system_docs" and title in ("第二页", ""):
            title = "文档"
        page = int(fence.get("page") or 0)
        if fid == "system_docs":
            page = 1
        elif fid in ("system_common", "system_software_side"):
            page = 0

        geo = _geo_from_layouts(settings, str(fence.get("id") or fid), page) or {}
        x = int(geo.get("x", fence.get("x") or 0))
        y = int(geo.get("y", fence.get("y") or 0))
        w = int(
            geo.get("width")
            or geo.get("w")
            or fence.get("width")
            or fence.get("w")
            or 280
        )
        h = int(
            geo.get("height")
            or geo.get("h")
            or fence.get("height")
            or fence.get("h")
            or 200
        )
        collapsed = bool(geo.get("collapsed", fence.get("collapsed", False)))

        style = fence.get("style")
        style_id = "charcoal"
        if isinstance(style, str) and style:
            style_id = style
        elif isinstance(style, dict) and style.get("id"):
            style_id = str(style["id"])
        elif settings.get("fence_style_preset"):
            style_id = str(settings["fence_style_preset"])

        fences.append(
            {
                "id": fid,
                "title": title,
                "x": x,
                "y": y,
                "w": w,
                "h": h,
                "page": page,
                "style": style_id,
                "portal": str(fence.get("folder") or "")
                if fence.get("folder")
                else "",
                "collapsed": collapsed,
                "position_locked": bool(fence.get("position_locked", False)),
                "sort_by": str(fence.get("sort_by") or "name"),
                "view_mode": str(fence.get("view_mode") or "grid"),
                "opacity": -1,
            }
        )
        for item in fence.get("virtual_items") or []:
            path = item if isinstance(item, str) else ""
            if isinstance(item, dict):
                path = str(item.get("path") or "")
            if path:
                pins[_pin_key(path)] = fid

    current = int(settings.get("current_page") or 0)
    if current < 0 or current >= len(page_names):
        current = 0
    dock = settings.get("dock") if isinstance(settings.get("dock"), dict) else {}
    pet = (
        settings.get("desktop_pet")
        if isinstance(settings.get("desktop_pet"), dict)
        else {}
    )

    dest = _native_dir()
    dest.mkdir(parents=True, exist_ok=True)
    layout_path = dest / "layout.json"
    pins_path = dest / "pins.json"
    _backup(layout_path)
    _backup(pins_path)

    fingerprint = ""
    if layout_path.is_file():
        try:
            old = json.loads(layout_path.read_text(encoding="utf-8"))
            fingerprint = str(old.get("last_display_fingerprint") or "")
        except json.JSONDecodeError:
            fingerprint = ""

    layout = {
        "current_page": current,
        "dock_visible": False,  # Dock UI hidden in native for now
        "pet_visible": bool(pet.get("visible", True)),
        "last_display_fingerprint": fingerprint,
        "pages": page_names,
        "fences": fences,
        "profiles": [],
    }
    layout_path.write_text(
        json.dumps(layout, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    pins_path.write_text(
        json.dumps({"pins": pins}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    pet_path = dest / "pet.json"
    _backup(pet_path)
    character = str(pet.get("character") or "hoodie")
    pet_path.write_text(
        json.dumps(
            {"character": character, "pose": "idle"}, ensure_ascii=False, indent=2
        )
        + "\n",
        encoding="utf-8",
    )

    prefs_path = dest / "app_prefs.json"
    preset = str(settings.get("fence_style_preset") or "charcoal")
    prefs = {}
    if prefs_path.is_file():
        try:
            prefs = json.loads(prefs_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            prefs = {}
    if not isinstance(prefs, dict):
        prefs = {}
    _backup(prefs_path)
    prefs["theme_id"] = preset
    prefs["enable_public_desktop"] = False
    prefs_path.write_text(
        json.dumps(prefs, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    (dest / "imported_from_python.txt").write_text(
        f"fences={len(fences)} pins={len(pins)} pages={len(page_names)}\n",
        encoding="utf-8",
    )
    print(f"imported fences={len(fences)} pins={len(pins)} pages={page_names}")
    for f in fences:
        print(
            f"  {f['id']}: {f['title']!r} page={f['page']} "
            f"xywh=({f['x']},{f['y']},{f['w']},{f['h']})"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
