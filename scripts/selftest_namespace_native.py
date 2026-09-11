"""Regression: system icons pin into「常用」under hide_shell_icons (first install)."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def test_hosted_mode_pins_into_common_fence(tmp_path: Path | None = None) -> None:
    from src.organizer import ensure_hosted_system_namespace_public_icons
    from src.system_defaults import SYSTEM_COMMON_FENCE_ID

    root = tmp_path or Path(tempfile.mkdtemp(prefix="desktidy_ns_"))
    storage = root / "storage"
    system_dir = storage / ".public_system"
    system_dir.mkdir(parents=True, exist_ok=True)

    settings = {
        "hide_shell_icons": True,
        "enable_public_desktop": False,
        "fences": [
            {
                "id": SYSTEM_COMMON_FENCE_ID,
                "name": "常用",
                "virtual_items": [r"C:\Users\demo\app.exe"],
            }
        ],
        "public_desktop_items": [
            {"path": r"C:\Users\demo\note.txt", "x": 20, "y": 20},
        ],
        "hosted_namespace_icons": {},
        "desktop_pages": [{"id": 0, "name": "工作", "locked": True}],
    }

    def fake_clsid(path: Path):
        text = str(path)
        if "此电脑" in text:
            return "{20D04FE0-3AEA-1069-A2D8-08002B30309D}"
        if "回收站" in text:
            return "{645FF040-5081-101B-9F08-00AA002F954E}"
        return None

    def fake_create(target_dir: Path, parsing_name: str, display_name: str = ""):
        target_dir.mkdir(parents=True, exist_ok=True)
        name = display_name or "ns"
        dest = target_dir / f"{name}.lnk"
        dest.write_text("stub", encoding="utf-8")
        return dest

    with mock.patch("src.settings.get_fence_storage_root", return_value=storage), mock.patch(
        "src.win_shell.create_namespace_shortcut", side_effect=fake_create
    ), mock.patch(
        "src.win_shell.get_lnk_namespace_clsid", side_effect=fake_clsid
    ), mock.patch(
        "src.win_shell.host_namespace_icon_in_fence"
    ), mock.patch(
        "src.fence_rules.assign_paths_to_virtual_fence",
        side_effect=lambda fence, _settings, paths: (
            fence.setdefault("virtual_items", []).extend(str(p) for p in paths) or list(paths)
        ),
    ), mock.patch(
        "src.system_defaults.ensure_system_defaults", return_value=False
    ):
        ensure_hosted_system_namespace_public_icons(settings, apply=True)

    pins = [str(p) for p in settings["fences"][0]["virtual_items"]]
    assert any("此电脑" in p for p in pins)
    assert any("回收站" in p for p in pins)
    assert r"C:\Users\demo\app.exe" in pins
    # Must not only live as floats when「常用」exists.
    assert not any("此电脑" in str(e.get("path")) for e in settings["public_desktop_items"])
    assert not any("回收站" in str(e.get("path")) for e in settings["public_desktop_items"])


def test_hosted_mode_keeps_existing_fence_pins(tmp_path: Path | None = None) -> None:
    from src.organizer import ensure_hosted_system_namespace_public_icons
    from src.system_defaults import SYSTEM_COMMON_FENCE_ID

    root = tmp_path or Path(tempfile.mkdtemp(prefix="desktidy_ns_"))
    storage = root / "storage"
    system_dir = storage / ".public_system"
    system_dir.mkdir(parents=True, exist_ok=True)
    pc = system_dir / "此电脑.lnk"
    pc.write_text("stub", encoding="utf-8")

    settings = {
        "hide_shell_icons": True,
        "enable_public_desktop": True,
        "fences": [
            {
                "id": SYSTEM_COMMON_FENCE_ID,
                "name": "常用",
                "virtual_items": [str(pc), r"C:\Users\demo\app.exe"],
            }
        ],
        "public_desktop_items": [
            {"path": r"C:\Users\demo\note.txt", "x": 20, "y": 20},
        ],
        "hosted_namespace_icons": {},
        "desktop_pages": [{"id": 0, "name": "工作", "locked": True}],
    }

    def fake_clsid(path: Path):
        text = str(path)
        if "此电脑" in text:
            return "{20D04FE0-3AEA-1069-A2D8-08002B30309D}"
        if "回收站" in text:
            return "{645FF040-5081-101B-9F08-00AA002F954E}"
        return None

    def fake_create(target_dir: Path, parsing_name: str, display_name: str = ""):
        target_dir.mkdir(parents=True, exist_ok=True)
        name = display_name or "ns"
        dest = target_dir / f"{name}.lnk"
        dest.write_text("stub", encoding="utf-8")
        return dest

    with mock.patch("src.settings.get_fence_storage_root", return_value=storage), mock.patch(
        "src.win_shell.create_namespace_shortcut", side_effect=fake_create
    ), mock.patch(
        "src.win_shell.get_lnk_namespace_clsid", side_effect=fake_clsid
    ), mock.patch(
        "src.win_shell.host_namespace_icon_in_fence"
    ), mock.patch(
        "src.fence_rules.assign_paths_to_virtual_fence",
        side_effect=lambda fence, _settings, paths: (
            fence.setdefault("virtual_items", []).extend(str(p) for p in paths) or list(paths)
        ),
    ), mock.patch(
        "src.system_defaults.ensure_system_defaults", return_value=False
    ):
        ensure_hosted_system_namespace_public_icons(settings, apply=True)

    assert str(pc) in settings["fences"][0]["virtual_items"]
    assert r"C:\Users\demo\app.exe" in settings["fences"][0]["virtual_items"]
    assert any("回收站" in str(p) for p in settings["fences"][0]["virtual_items"])
    assert not any("此电脑" in str(e.get("path")) for e in settings["public_desktop_items"])


def test_assign_paths_accepts_namespace_shortcuts(tmp_path: Path | None = None) -> None:
    from src.fence_rules import assign_paths_to_virtual_fence

    root = tmp_path or Path(tempfile.mkdtemp(prefix="desktidy_ns_pin_"))
    ns = root / "此电脑.lnk"
    ns.write_text("stub", encoding="utf-8")
    settings = {
        "fences": [{"id": "f0", "name": "常用", "virtual_items": []}],
        "public_desktop_items": [{"path": str(ns), "x": 1, "y": 1}],
    }
    fence = settings["fences"][0]

    with mock.patch(
        "src.win_shell.get_lnk_namespace_clsid",
        return_value="{20D04FE0-3AEA-1069-A2D8-08002B30309D}",
    ), mock.patch("src.win_shell.host_namespace_icon_in_fence") as host_fn, mock.patch(
        "src.fence_rules._canonicalize_virtual_pin_path", side_effect=lambda p: Path(p)
    ), mock.patch(
        "src.fence_rules._lnk_target_file", return_value=None
    ), mock.patch(
        "src.fence_rules._strip_resolved_target_pins"
    ), mock.patch(
        "src.fence_rules._heal_fence_virtual_pins"
    ), mock.patch(
        "src.public_desktop.remove_public_paths", return_value=True
    ):
        added = assign_paths_to_virtual_fence(fence, settings, [ns])

    assert added == [ns]
    assert any("此电脑" in str(p) for p in fence["virtual_items"])
    assert host_fn.called


def test_restore_native_clears_hosted_copies() -> None:
    from src.organizer import restore_native_system_namespace_icons
    from src.system_defaults import SYSTEM_COMMON_FENCE_ID

    ns_pc = r"C:\Users\demo\.desktidy\storage\.public_system\此电脑.lnk"
    ns_bin = r"C:\Users\demo\.desktidy\storage\.public_system\回收站.lnk"
    settings = {
        "fences": [
            {
                "id": SYSTEM_COMMON_FENCE_ID,
                "name": "软件",
                "virtual_items": [ns_pc, ns_bin, r"C:\Users\demo\app.exe"],
            }
        ],
        "public_desktop_items": [
            {"path": ns_pc, "x": 10, "y": 10},
            {"path": r"C:\Users\demo\note.txt", "x": 20, "y": 20},
        ],
        "hosted_namespace_icons": {
            "{20D04FE0-3AEA-1069-A2D8-08002B30309D}": {
                "name": "此电脑",
                "lnk": ns_pc,
            },
        },
    }

    with mock.patch("src.win_shell.restore_all_hosted_namespace_icons", return_value=1), mock.patch(
        "src.win_shell.set_desktop_namespace_icon_visible"
    ):
        assert restore_native_system_namespace_icons(settings, apply=True)

    fence = settings["fences"][0]
    assert fence["virtual_items"] == [r"C:\Users\demo\app.exe"]
    assert len(settings["public_desktop_items"]) == 1
    assert settings["public_desktop_items"][0]["path"] == r"C:\Users\demo\note.txt"
    assert settings["hosted_namespace_icons"] == {}


def test_move_namespace_allows_this_pc() -> None:
    from src.win_shell import move_namespace_icon_to_folder

    src = __import__("inspect").getsource(move_namespace_icon_to_folder)
    assert "return_namespace_icon_to_desktop" not in src
    assert "create_namespace_shortcut" in src


def test_app_uses_hosted_when_shell_hidden() -> None:
    import inspect

    from src.app import DeskTidyApp
    from src import organizer as org_mod

    src = inspect.getsource(DeskTidyApp._ensure_native_system_namespace_icons)
    assert "ensure_hosted_system_namespace_public_icons" in src
    assert "hide_shell_icons" in src
    org_src = inspect.getsource(org_mod._organize_virtual)
    assert "ensure_hosted_system_namespace_public_icons" in org_src
    # Must not always strip hosted icons while shell plate is owned by DeskTidy.
    apply_block = org_src.split("if dry_run:", 1)[1]
    assert "hide_shell_icons" in apply_block
    assert apply_block.index("ensure_hosted_system_namespace_public_icons") < apply_block.index(
        "restore_native_system_namespace_icons"
    )


def main() -> int:
    test_hosted_mode_pins_into_common_fence()
    test_hosted_mode_keeps_existing_fence_pins()
    test_assign_paths_accepts_namespace_shortcuts()
    test_restore_native_clears_hosted_copies()
    test_move_namespace_allows_this_pc()
    test_app_uses_hosted_when_shell_hidden()
    print("selftest_namespace_native: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
