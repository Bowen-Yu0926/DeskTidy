"""Drag-drop feature matrix — folder/desktop/fence/pet OLE boundaries.

Regression for:
- Explorer folder → desktop (PublicIconHost when shell icons hidden)
- Explorer / float → pet trash (sprite-only OLE child)
- DeskTidy custom drag → pet / folder / fence (non-OLE bridge)
"""

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


def test_public_host_owns_explorer_desktop() -> None:
    from src.ui import public_icon_host
    from src.ui.fence_icon_item import drop_files_to_public_from_mime, paste_files_to_public

    host_src = inspect.getsource(public_icon_host.PublicIconHost)
    assert "session_owns_desktop_drops" in host_src
    assert "dropEvent" in host_src
    assert "drop_files_to_public_from_mime" in host_src
    attach_src = inspect.getsource(public_icon_host.PublicIconHost.ensure_shell_attached)
    assert "session_owns_desktop_drops" in attach_src
    mask_src = inspect.getsource(public_icon_host.PublicIconHost.refresh_click_mask)
    assert "setAcceptDrops(owns)" in mask_src
    hit_src = inspect.getsource(public_icon_host.PublicIconHost.hit_test_region)
    host_cls = inspect.getsource(public_icon_host.PublicIconHost)
    assert "_icon_hit_region" in host_cls
    assert "_full_host_region" not in host_cls
    assert "icon cluster" in hit_src.lower() or "_icon_hit_region" in hit_src

    drop_fn = inspect.getsource(drop_files_to_public_from_mime)
    assert "collect_drop_paths" in drop_fn
    assert "paste_files_to_public" in drop_fn
    paste_fn = inspect.getsource(paste_files_to_public)
    assert "add_public_item" in paste_fn


def test_pet_sprite_only_ole() -> None:
    from src.ui import pet_widget
    from src.ui.pet_widget import DesktopPetWidget, _PetFileDropZone

    parent_src = inspect.getsource(DesktopPetWidget.__init__)
    assert "setAcceptDrops(False)" in parent_src
    zone_src = inspect.getsource(_PetFileDropZone)
    assert "setAcceptDrops(True)" in zone_src
    assert "dropEvent" in zone_src
    assert "_accept_file_drop" in zone_src
    assert "_cleanup_pins_after_trash" in inspect.getsource(DesktopPetWidget)
    sync = inspect.getsource(DesktopPetWidget._sync_drop_zone)
    assert "setAcceptDrops(True)" in sync
    mask_src = inspect.getsource(DesktopPetWidget._sync_input_mask)
    assert "_sprite_dest_rect" in mask_src
    assert "folder→desktop" in mask_src or "从文件夹拖到桌面" in mask_src

    mod_src = inspect.getsource(pet_widget)
    assert "deliver_paths_to_pet_trash" in mod_src
    assert "find_pet_trash_target" in mod_src
    assert "accepts_trash_at" in mod_src


def test_custom_drag_bridges() -> None:
    from src.ui import fence_icon_item as fii

    src = inspect.getsource(fii)
    assert "_try_deliver_drag_to_pet_trash" in src
    assert "folder_drop_target_at" in src
    assert "find_pet_trash_target" in src
    pet_block = inspect.getsource(fii.folder_drop_target_at)
    assert "find_pet_trash_target" in pet_block

    deliver = inspect.getsource(fii._try_deliver_drag_to_pet_trash)
    assert "deliver_paths_to_pet_trash" in deliver
    assert "remove_public_paths" in deliver


def test_post_drop_fence_interactive() -> None:
    from src.app import DeskTidyApp
    from src.ui.fence_widget import FenceWidget

    app_src = inspect.getsource(DeskTidyApp.ensure_live_fences_interactive)
    assert "WS_EX_TRANSPARENT" in app_src or "transparent" in app_src.lower()
    finish = inspect.getsource(FenceWidget._ensure_drop_target_interactive)
    assert "ensure_live_fences_interactive" in finish


def test_pet_zone_live() -> None:
    from types import SimpleNamespace

    from PyQt6.QtWidgets import QApplication

    from src.ui.pet_widget import DesktopPetWidget, _PetFileDropZone

    app = QApplication.instance() or QApplication([])
    pet = DesktopPetWidget({"desktop_pet": {"enabled": True, "visible": True}})
    pet.show()
    app.processEvents()
    zone = pet._drop_zone
    assert isinstance(zone, _PetFileDropZone)
    assert zone.acceptDrops() is True
    assert pet.acceptDrops() is False
    assert zone.geometry().intersects(pet._sprite_dest_rect())
    pet.close()
    pet.deleteLater()
    app.processEvents()


def test_public_host_drop_live() -> None:
    from types import SimpleNamespace
    from unittest.mock import patch

    from PyQt6.QtWidgets import QApplication

    from src.ui.public_icon_host import PublicIconHost

    app = QApplication.instance() or QApplication([])
    app._desktidy_app = SimpleNamespace(  # type: ignore[attr-defined]
        _exiting=False,
        _icons_hidden=False,
        settings={"hide_shell_icons": True},
    )
    host = PublicIconHost()
    host.resize(640, 480)
    with patch("src.win_shell.are_desktop_icons_visible", return_value=False):
        host.refresh_click_mask()
        assert host.acceptDrops() is True
        # No icons → empty mouse mask (park), not a full-screen steal.
        assert host.hit_test_region().isEmpty()
    delattr(app, "_desktidy_app")
    host.close()
    host.deleteLater()
    app.processEvents()


def main() -> int:
    print("DeskTidy drag-drop matrix")
    run("public host Explorer desktop", test_public_host_owns_explorer_desktop)
    run("pet sprite-only OLE", test_pet_sprite_only_ole)
    run("custom drag bridges", test_custom_drag_bridges)
    run("post-drop fence interactive", test_post_drop_fence_interactive)
    run("pet drop zone live", test_pet_zone_live)
    run("public host drop live", test_public_host_drop_live)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err.splitlines()[0]}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
