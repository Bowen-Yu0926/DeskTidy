"""Regression: public floats share one shell HWND (PublicIconHost)."""

from __future__ import annotations

import inspect
import sys
import tempfile
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


def test_host_contracts() -> None:
    from src.app import DeskTidyApp
    from src.ui import public_icon_host, public_icon_widget
    from src.ui.fence_icon_item import find_item_widget_for_path

    assert hasattr(public_icon_host, "PublicIconHost")
    host_src = inspect.getsource(public_icon_host.PublicIconHost)
    assert "setMask" in host_src
    assert "_park_mask_region" in host_src
    assert "OFFSCREEN_PARK_POS" in host_src or "_park_mask_region" in host_src
    assert "configure_desktop_overlay" in host_src
    assert "present" in host_src
    assert "begin_mask_batch" in host_src
    assert "mask_only" in host_src
    assert "session_owns_desktop_drops" in host_src
    assert "dropEvent" in host_src
    assert "setAcceptDrops(owns)" in inspect.getsource(
        public_icon_host.PublicIconHost.refresh_click_mask
    )
    drop_src = inspect.getsource(public_icon_host.PublicIconHost.dropEvent)
    assert "drop_files_to_public_from_mime" in drop_src
    assert "DESKTIDY_VIRTUAL_MIME" in drop_src
    hit_src = inspect.getsource(public_icon_host.PublicIconHost.hit_test_region)
    assert "_icon_hit_region" in inspect.getsource(public_icon_host.PublicIconHost)
    assert "_full_host_region" not in inspect.getsource(public_icon_host.PublicIconHost)
    assert "_fence_exclusion_region" in inspect.getsource(public_icon_host.PublicIconHost)
    assert "_pet_exclusion_region" in inspect.getsource(public_icon_host.PublicIconHost)
    assert "_work_area_region_local" in hit_src
    assert "virtual_desktop_work_region" in inspect.getsource(
        public_icon_host.PublicIconHost._work_area_region_local
    )
    from src.fence_layout import virtual_desktop_work_region

    assert callable(virtual_desktop_work_region)
    fence_excl = inspect.getsource(public_icon_host.PublicIconHost._fence_exclusion_region)
    assert "_desktidy_soft_parked" in fence_excl
    assert "frameGeometry" in fence_excl
    paint_src = inspect.getsource(public_icon_host.PublicIconHost.paintEvent)
    assert "_HIT_PLATE" in paint_src and ("fillRect" in paint_src or "drawPath" in paint_src)
    assert "_hit_plate_region" in paint_src
    assert "_painting_plate" in paint_src
    assert "QPainterPath" in paint_src
    assert "addRegion" in paint_src
    refresh_src = inspect.getsource(public_icon_host.PublicIconHost.refresh_click_mask)
    assert "_last_mask_region" in refresh_src
    assert "_last_plate_sig" in refresh_src
    assert "mask_same" in refresh_src and "plate_same" in refresh_src
    assert "_plate_content_signature" in inspect.getsource(
        public_icon_host.PublicIconHost
    )
    # Stuck 框选 grabMouse stole all desktop/app clicks until VD remount.
    assert "_abort_marquee" in inspect.getsource(public_icon_host.PublicIconHost)
    assert "_abort_marquee" in inspect.getsource(
        public_icon_host.PublicIconHost.hideEvent
    )
    # Owned plate empty click must clear fence selection (LL blank monitor misses it).
    assert "_clear_desktop_selections_for_empty_click" in inspect.getsource(
        public_icon_host.PublicIconHost
    )
    begin_src = inspect.getsource(public_icon_host.PublicIconHost._begin_marquee)
    assert "_clear_desktop_selections_for_empty_click" in begin_src
    plate_src = inspect.getsource(public_icon_host.PublicIconHost._hit_plate_region)
    assert "hit_test_region" in plate_src
    # Shelf gaps only — full-cluster plate stole fence clicks under layered alpha.
    assert "_content_hit_region" in plate_src
    assert "subtracted" in plate_src
    assert "ensure_live_fences_interactive" in inspect.getsource(
        DeskTidyApp._refresh_public_desktop_impl
    )
    attach_src = inspect.getsource(public_icon_host.PublicIconHost.ensure_shell_attached)
    assert "session_owns_desktop_drops" in attach_src
    app_src = inspect.getsource(DeskTidyApp)
    assert "_ensure_public_drop_surface" in app_src
    owns_src = inspect.getsource(public_icon_host.PublicIconHost.session_owns_desktop_drops)
    assert "are_desktop_icons_visible" in owns_src
    from src.ui.fence_icon_item import drop_files_to_public_from_mime

    drop_fn = inspect.getsource(drop_files_to_public_from_mime)
    assert "paste_files_to_public" in drop_fn
    assert "collect_drop_paths" in drop_fn
    # Empty mask must not hide() the host (page-switch whole-desktop flash).
    assert "self.hide()" not in inspect.getsource(
        public_icon_host.PublicIconHost.refresh_click_mask
    )

    iter_src = inspect.getsource(DeskTidyApp._iter_overlay_widgets)
    assert "_public_icon_host" in iter_src
    assert "for icon in self.public_icons" in iter_src  # legacy fallback kept

    refresh = inspect.getsource(DeskTidyApp._refresh_public_desktop_impl)
    assert "parent=host" in refresh
    assert "move_to_screen" in refresh
    assert "_ensure_public_icon_host" in refresh

    find_src = inspect.getsource(find_item_widget_for_path)
    assert "PublicIconHost" in find_src

    init_src = inspect.getsource(public_icon_widget.PublicIconWidget.__init__)
    assert "parent is None" in init_src


def test_host_owns_explorer_drops_when_shell_hidden() -> None:
    from types import SimpleNamespace
    from unittest.mock import patch

    from PyQt6.QtCore import QPoint, QRect
    from PyQt6.QtWidgets import QApplication

    from src.ui.public_icon_host import PublicIconHost

    app = QApplication.instance() or QApplication([])
    host = PublicIconHost()
    host.setGeometry(QRect(0, 0, 800, 600))
    fake = SimpleNamespace(
        _exiting=False,
        _icons_hidden=False,
        settings={"hide_shell_icons": True},
    )
    app._desktidy_app = fake  # type: ignore[attr-defined]
    try:
        with patch("src.win_shell.are_desktop_icons_visible", return_value=False):
            assert host.session_owns_desktop_drops() is True
            host.refresh_click_mask()
            assert host.acceptDrops() is True
            # No public icons → empty mouse mask (Explorer owns empty desktop).
            region = host.hit_test_region()
            assert region.isEmpty()
            class FenceWidget:
                def isVisible(self):
                    return True

                def mapToGlobal(self, p):
                    return QPoint(100, 120)

                def geometry(self):
                    from PyQt6.QtCore import QRect

                    return QRect(100, 120, 240, 160)

                def frameGeometry(self):
                    return self.geometry()

                def size(self):
                    from PyQt6.QtCore import QSize

                    return QSize(240, 160)

            # With icons + shell icons hidden: full host minus fences (框选 on wallpaper).
            from src.ui.public_icon_widget import PublicIconWidget
            import tempfile
            from pathlib import Path

            td = Path(tempfile.mkdtemp(prefix="desktidy_hostmask_"))
            p = td / "a.txt"
            p.write_text("x", encoding="utf-8")
            icon = PublicIconWidget(p, 40, 50, parent=host)
            icon.setGeometry(40, 50, 96, 112)
            icon.show()
            host.invalidate_icon_cache()

            class DesktopPetWidget:
                def isVisible(self):
                    return True

                def geometry(self):
                    from PyQt6.QtCore import QRect

                    # Large window with transparent padding (wait/trash pads).
                    return QRect(500, 400, 280, 320)

                def frameGeometry(self):
                    return self.geometry()

                def mask(self):
                    from PyQt6.QtCore import QRect
                    from PyQt6.QtGui import QRegion

                    # Shaped hit = sprite only, not the full frame.
                    return QRegion(QRect(24, 40, 96, 120))

                def size(self):
                    from PyQt6.QtCore import QSize

                    return QSize(280, 320)

            with patch.object(
                QApplication,
                "topLevelWidgets",
                return_value=[FenceWidget(), DesktopPetWidget(), host],
            ):
                region = host.hit_test_region()
                assert not region.isEmpty()
                assert region.contains(QPoint(50, 60))  # on icon
                assert not region.contains(QPoint(120, 140))  # inside fence exclusion
                # Pet sprite (frame 500,400 + mask 24,40) → host (524, 440)
                assert not region.contains(QPoint(524, 440))
                # Transparent padding of the pet window must stay host-hittable
                # (public floats under wait/trash pads were undraggable before).
                assert region.contains(QPoint(720, 550))
                assert region.contains(QPoint(700, 500))  # far empty → our 框选 plate
            pet_excl_src = inspect.getsource(
                __import__(
                    "src.ui.public_icon_host", fromlist=["PublicIconHost"]
                ).PublicIconHost._pet_exclusion_region
            )
            assert "mask()" in pet_excl_src or ".mask()" in pet_excl_src
            assert "frameGeometry" in pet_excl_src
            icon.close()
            icon.deleteLater()
            p.unlink(missing_ok=True)
            try:
                td.rmdir()
            except OSError:
                pass
            fake.settings["hide_shell_icons"] = False
            assert host.session_owns_desktop_drops() is False
            host.refresh_click_mask()
            assert host.acceptDrops() is False
            # Shell icons visible: cluster-only mask — far wallpaper stays Explorer's.
            td2 = Path(tempfile.mkdtemp(prefix="desktidy_hostmask2_"))
            p2 = td2 / "b.txt"
            p2.write_text("y", encoding="utf-8")
            icon2 = PublicIconWidget(p2, 40, 50, parent=host)
            icon2.setGeometry(40, 50, 96, 112)
            icon2.show()
            host.invalidate_icon_cache()
            region = host.hit_test_region()
            assert region.contains(QPoint(50, 60))
            assert not region.contains(QPoint(700, 500))  # far empty → Explorer
            icon2.close()
            icon2.deleteLater()
            p2.unlink(missing_ok=True)
            try:
                td2.rmdir()
            except OSError:
                pass
            host.invalidate_icon_cache()
            assert host.hit_test_region().isEmpty()
    finally:
        if hasattr(app, "_desktidy_app"):
            delattr(app, "_desktidy_app")
        host.close()
        host.deleteLater()
        app.processEvents()


def test_hosted_icons_share_one_toplevel() -> None:
    from PyQt6.QtWidgets import QApplication

    from src.ui.public_icon_host import PublicIconHost
    from src.ui.public_icon_widget import PublicIconWidget

    app = QApplication.instance() or QApplication([])
    td = Path(tempfile.mkdtemp())
    files = []
    for i in range(3):
        p = td / f"float_{i}.txt"
        p.write_text("x", encoding="utf-8")
        files.append(p)

    host = PublicIconHost()
    # Do not show empty host — mask/HWND must be established with children.
    icons = [
        PublicIconWidget(p, 80 + i * 100, 120, parent=host) for i, p in enumerate(files)
    ]
    for icon in icons:
        icon.show()
    host.present()
    app.processEvents()

    assert all(icon.is_hosted() for icon in icons)
    assert all(icon.parentWidget() is host for icon in icons)
    assert all(not icon.isWindow() for icon in icons)
    # One shell-facing top-level for the group.
    assert host.isWindow()
    assert host.isVisible()
    tops = {icon.window() for icon in icons}
    assert tops == {host}

    # Standalone still works for unit tests / legacy.
    solo = PublicIconWidget(files[0], 0, 0, parent=None)
    assert solo.isWindow()
    assert not solo.is_hosted()
    solo.close()
    solo.deleteLater()

    for icon in icons:
        icon.close()
        icon.deleteLater()
    host.close()
    host.deleteLater()
    app.processEvents()
    for p in files:
        p.unlink(missing_ok=True)


def test_find_item_under_host() -> None:
    from PyQt6.QtWidgets import QApplication

    from src.ui.fence_icon_item import find_item_widget_for_path
    from src.ui.public_icon_host import PublicIconHost
    from src.ui.public_icon_widget import PublicIconWidget

    app = QApplication.instance() or QApplication([])
    td = Path(tempfile.mkdtemp())
    p = td / "hosted_find.txt"
    p.write_text("y", encoding="utf-8")
    host = PublicIconHost()
    icon = PublicIconWidget(p, 50, 50, parent=host)
    icon.show()
    host.present()
    app.processEvents()
    found = find_item_widget_for_path(p)
    assert found is icon, found
    icon.close()
    icon.deleteLater()
    host.close()
    host.deleteLater()
    app.processEvents()
    p.unlink(missing_ok=True)


def test_ensure_desktop_loose_floats_adds_unclaimed() -> None:
    from src.fence_rules import all_fence_pinned_keys
    from src.public_desktop import apply_loose_desktop_scan, ensure_desktop_loose_floats

    settings = {
        "hide_shell_icons": True,
        "enable_public_desktop": True,
        "current_page": 0,
        "fences": [{"id": "f", "virtual_items": [r"C:\desk\pinned.txt"]}],
        "public_desktop_items": [],
    }
    pinned = all_fence_pinned_keys(settings)
    assert r"c:\desk\pinned.txt".casefold() in pinned

    drop = Path(r"C:\desk\loose.txt")
    changed = apply_loose_desktop_scan(settings, {str(drop).casefold(): drop})
    assert changed
    assert settings["public_desktop_items"]
    assert settings["public_desktop_items"][0]["path"].casefold().endswith("loose.txt")
    # Second call may still touch stale/missing loose entries on a live desktop.
    ensure_desktop_loose_floats(settings)
    assert any(
        str(entry.get("path", "")).casefold().endswith("loose.txt")
        for entry in settings["public_desktop_items"]
        if isinstance(entry, dict)
    )


def test_host_mask_excludes_taskbar_gap() -> None:
    """Multi-mon bounding available rect must not steal primary taskbar clicks."""
    from types import SimpleNamespace
    from unittest.mock import patch

    from PyQt6.QtCore import QPoint, QRect
    from PyQt6.QtGui import QRegion
    from PyQt6.QtWidgets import QApplication

    from src.ui.public_icon_host import PublicIconHost

    app = QApplication.instance() or QApplication([])
    host = PublicIconHost()
    # Simulate virtual bounding box that covers primary taskbar strip
    # (secondary work area taller → united rect fills y=1032..1079).
    host.setGeometry(QRect(-1920, 0, 3840, 1080))
    fake = SimpleNamespace(
        _exiting=False,
        _icons_hidden=False,
        settings={"hide_shell_icons": True},
    )
    app._desktidy_app = fake  # type: ignore[attr-defined]

    primary_work = QRect(0, 0, 1920, 1032)
    secondary_work = QRect(-1920, 0, 1920, 1080)
    work_region = QRegion(primary_work).united(QRegion(secondary_work))

    from src.ui.public_icon_widget import PublicIconWidget
    import tempfile
    from pathlib import Path

    td = Path(tempfile.mkdtemp(prefix="desktidy_taskbar_mask_"))
    p = td / "a.txt"
    p.write_text("x", encoding="utf-8")
    icon = PublicIconWidget(p, 40, 50, parent=host)
    icon.setGeometry(40, 50, 96, 112)
    icon.show()
    host.invalidate_icon_cache()
    try:
        with patch("src.win_shell.are_desktop_icons_visible", return_value=False), patch(
            "src.fence_layout.virtual_desktop_work_region", return_value=work_region
        ), patch.object(QApplication, "topLevelWidgets", return_value=[host]):
            region = host.hit_test_region()
            # Host-local: global (960, 1050) taskbar → local x = 960-(-1920)=2880
            assert not region.contains(QPoint(2880, 1050)), "taskbar strip must stay click-through"
            assert region.contains(QPoint(100, 100)), "work-area wallpaper still ours"
            assert region.contains(QPoint(50, 60)), "public icon still hittable"
    finally:
        icon.close()
        icon.deleteLater()
        p.unlink(missing_ok=True)
        try:
            td.rmdir()
        except OSError:
            pass
        try:
            delattr(app, "_desktidy_app")
        except Exception:
            pass
        host.close()
        host.deleteLater()


def test_loose_sync_uses_shared_public_area() -> None:
    from src.public_desktop import apply_loose_desktop_scan, is_shared_public_entry

    settings = {
        "enable_public_desktop": True,
        "current_page": 0,
        "fences": [],
        "public_desktop_items": [],
    }
    path = Path(r"C:\Users\demo\Desktop\new_drop.txt")
    changed = apply_loose_desktop_scan(settings, {str(path).casefold(): path})
    assert changed
    entry = settings["public_desktop_items"][0]
    assert is_shared_public_entry(entry)
    assert "loose" not in entry


def test_empty_plate_rmb_passes_to_defview() -> None:
    """Owned plate must open DefView menu — not swallow RMB / thin CreateViewObject."""
    import inspect

    from src.ui.public_icon_host import PublicIconHost
    from src.win_shell import request_desktop_background_menu

    src = inspect.getsource(PublicIconHost)
    assert "HTTRANSPARENT" in src
    assert "_show_desktop_background_menu" not in src
    assert "show_folder_background_menu" not in inspect.getsource(
        PublicIconHost.eventFilter
    )
    # Owned plate must not punch through to a hidden ListView.
    ne = inspect.getsource(PublicIconHost.nativeEvent)
    assert "session_owns_desktop_drops" in ne
    assert "_VK_RBUTTON" in ne or "0x02" in ne
    assert "from_address" in ne
    assert "super().nativeEvent" not in ne
    # Leftover empty RMB → DefView; freeze until menu dismisses (not end_later 250).
    ef = inspect.getsource(PublicIconHost.eventFilter)
    assert "_forward_empty_plate_rmb" in ef
    fwd = inspect.getsource(PublicIconHost._forward_empty_plate_rmb)
    assert "request_desktop_background_menu" in fwd
    assert "_arm_explorer_menu_freeze" in fwd
    assert "_end_desktop_popup_later" not in fwd
    assert "end_later(" not in fwd
    helper = inspect.getsource(request_desktop_background_menu)
    assert "WM_CONTEXTMENU" in helper
    assert "PostMessageW" in helper
    assert "CreateViewObject" not in helper.split('"""', 2)[-1]


def test_request_desktop_background_menu_posts_to_defview() -> None:
    """Helper posts WM_CONTEXTMENU to DefView/ListView at screen coords."""
    from unittest.mock import patch

    from src import win_shell

    with (
        patch.object(win_shell, "_find_shell_def_view", return_value=0x100),
        patch.object(win_shell, "_find_desktop_listview", return_value=0x200),
        patch.object(win_shell.user32, "PostMessageW", return_value=1) as post,
    ):
        assert win_shell.request_desktop_background_menu(10, 20) is True
        assert post.call_count == 1
        hwnd, msg, wparam, lparam = post.call_args[0]
        assert hwnd == 0x200
        assert msg == win_shell.WM_CONTEXTMENU
        assert wparam == 0x200
        assert (lparam & 0xFFFF) == 10
        assert ((lparam >> 16) & 0xFFFF) == 20

    with (
        patch.object(win_shell, "_find_shell_def_view", return_value=0),
        patch.object(win_shell.user32, "PostMessageW") as post,
    ):
        assert win_shell.request_desktop_background_menu(1, 2) is False
        post.assert_not_called()


def test_plate_refresh_and_marquee_abort() -> None:
    """Owned plate must repaint after caption churn; hide must drop grabMouse.

    Regression: mask early-out skipped alpha update → stale ink stole clicks;
    unfinished 框选 left grabMouse → files/apps ignored until VD remount.
    """
    import tempfile
    from pathlib import Path
    from types import SimpleNamespace
    from unittest.mock import patch

    from PyQt6.QtCore import QPoint
    from PyQt6.QtGui import QRegion
    from PyQt6.QtWidgets import QApplication, QWidget

    from src.ui.public_icon_host import PublicIconHost
    from src.ui.public_icon_widget import PublicIconWidget

    app = QApplication.instance() or QApplication([])
    td = Path(tempfile.mkdtemp(prefix="desktidy_hitsteal_"))
    f = td / "a.txt"
    f.write_text("x", encoding="utf-8")
    host = PublicIconHost()
    host.setGeometry(0, 0, 800, 600)
    icon = PublicIconWidget(f, 100, 100, parent=host)
    icon.show()
    host.present()
    app.processEvents()

    fake = SimpleNamespace(
        _exiting=False, _icons_hidden=False, settings={"hide_shell_icons": True}
    )
    app._desktidy_app = fake  # type: ignore[attr-defined]
    updates = {"n": 0}
    orig_update = host.update

    def _spy(*args, **kwargs):
        updates["n"] += 1
        return orig_update(*args, **kwargs)

    host.update = _spy  # type: ignore[method-assign]
    try:
        with patch("src.win_shell.are_desktop_icons_visible", return_value=False):
            assert host.session_owns_desktop_drops()
            host.refresh_click_mask()
            app.processEvents()
            icon.set_selected(True)
            icon._reload_label()
            app.processEvents()
            icon.set_selected(False)
            icon._reload_label()
            app.processEvents()
            host.invalidate_icon_cache()
            # Outer owned mask unchanged — old bug returned here without update().
            host._last_mask_region = QRegion(host.hit_test_region())
            host._last_plate_sig = ("stale",)
            updates["n"] = 0
            host.refresh_click_mask()
            app.processEvents()
            assert updates["n"] > 0, "plate must refresh when content sig changes"
            glyph = icon.mapTo(host, icon.icon_label.geometry().center())
            assert not host._hit_plate_region().contains(glyph)

            # Keepalive path: same mask + same plate sig must not thrash update.
            updates["n"] = 0
            host.refresh_click_mask()
            assert updates["n"] == 0
    finally:
        if hasattr(app, "_desktidy_app"):
            delattr(app, "_desktidy_app")

    host._begin_marquee(QPoint(10, 10), additive=False)
    assert QWidget.mouseGrabber() is host
    host.hide()
    app.processEvents()
    assert QWidget.mouseGrabber() is not host
    assert host._marquee_origin is None

    icon.close()
    icon.deleteLater()
    host.close()
    host.deleteLater()
    app.processEvents()
    f.unlink(missing_ok=True)


def test_empty_plate_clears_fence_selection() -> None:
    """Owned wallpaper click must clear partition icon selection too."""
    from types import SimpleNamespace
    from unittest.mock import MagicMock

    from PyQt6.QtCore import QPoint
    from PyQt6.QtWidgets import QApplication

    from src.ui.public_icon_host import PublicIconHost

    app = QApplication.instance() or QApplication([])
    host = PublicIconHost()
    host.setGeometry(0, 0, 400, 300)
    host.show()
    app.processEvents()

    fence = MagicMock()
    fence.clear_item_selection = MagicMock()
    fake = SimpleNamespace(fences=[fence])
    app._desktidy_app = fake  # type: ignore[attr-defined]
    try:
        assert hasattr(host, "_clear_desktop_selections_for_empty_click")
        host._begin_marquee(QPoint(20, 20), additive=False)
        fence.clear_item_selection.assert_called()
        host._abort_marquee()
        fence.clear_item_selection.reset_mock()
        host._clear_desktop_selections_for_empty_click()
        fence.clear_item_selection.assert_called_once()
    finally:
        if hasattr(app, "_desktidy_app"):
            delattr(app, "_desktidy_app")
        host.close()
        host.deleteLater()
        app.processEvents()


def main() -> int:
    print("DeskTidy public icon host (HWND budget)")
    run("host contracts", test_host_contracts)
    run("host owns explorer drops when shell hidden", test_host_owns_explorer_drops_when_shell_hidden)
    run("host mask excludes taskbar gap", test_host_mask_excludes_taskbar_gap)
    run("empty plate RMB passes to DefView", test_empty_plate_rmb_passes_to_defview)
    run(
        "request_desktop_background_menu posts WM_CONTEXTMENU",
        test_request_desktop_background_menu_posts_to_defview,
    )
    run("loose sync shared public", test_loose_sync_uses_shared_public_area)
    run("ensure loose floats honors pins", test_ensure_desktop_loose_floats_adds_unclaimed)
    run("hosted icons share one toplevel", test_hosted_icons_share_one_toplevel)
    run("find_item under host", test_find_item_under_host)
    run("plate refresh + marquee abort", test_plate_refresh_and_marquee_abort)
    run("empty plate clears fence selection", test_empty_plate_clears_fence_selection)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
