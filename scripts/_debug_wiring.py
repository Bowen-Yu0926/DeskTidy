import inspect
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from PyQt6.QtWidgets import QApplication

QApplication(sys.argv[:1])

from src.app import DeskTidyApp
from src.hotkey_manager import HOTKEY_FALLBACKS, HOTKEY_IDS, _ACTION_BY_ID
from src.tray import TrayManager
from src.ui import extensions_widget as ew
from src.ui.file_search_overlay import FileSearchOverlay
from src.ui.styles import build_stylesheet


def test(name: str, cond: bool) -> None:
    if not cond:
        print("FAIL", name)


test("_ACTION_BY_ID", _ACTION_BY_ID == {hid: name for name, hid in HOTKEY_IDS.items()})
setup = inspect.getsource(DeskTidyApp._setup_hotkeys)
test("file_search setup", "file_search" in setup)
test("register_with_fallbacks", "register_with_fallbacks" in setup)
test("file_search HOTKEY_FALLBACKS", "file_search" in HOTKEY_FALLBACKS)
test("_toggle_file_search", "_toggle_file_search" in inspect.getsource(DeskTidyApp))
removed = inspect.getsource(DeskTidyApp._on_watched_file_removed)
test("no remove_public_paths on watcher", "remove_public_paths(" not in removed)
test("no _remove_public_icon_widget on watcher", "_remove_public_icon_widget(" not in removed)
test("prune_missing_public_items", "prune_missing_public_items" in removed)
test("schedule sticky reprune", "_schedule_sticky_missing_reprune" in removed)
test("no unpin", "unpin_paths_from_all_fences" not in removed)
test("no prune virtual", "prune_missing_virtual_items" not in removed)
test("no schedule", "_schedule_pending_fence_remove" not in removed)
test("no flush", "_flush_pending_fence_removes" not in inspect.getsource(DeskTidyApp))
test("sticky", "sticky" in removed.lower())
test("_maybe_reschedule", "_maybe_reschedule_desktop_watcher" in inspect.getsource(DeskTidyApp))
from src import app as app_mod

test("file_removed", hasattr(app_mod._UiBridge, "file_removed"))
test("ext file_search", "file_search" in inspect.getsource(ew.ExtensionsWidget._build_ui))
test("tray no file search action", "_file_search_action" not in inspect.getsource(TrayManager.__init__))
test(
    "Enter nav",
    "Enter" in (FileSearchOverlay.__doc__ or "")
    or "Key_Return" in inspect.getsource(FileSearchOverlay._handle_nav_key),
)
overlay_src = inspect.getsource(FileSearchOverlay)
overlay_mod = inspect.getsource(__import__("src.ui.file_search_overlay", fromlist=["x"]))
for n, c in [
    ("installEventFilter", "installEventFilter" in overlay_src),
    ("_SearchInputFilter", "_SearchInputFilter" in overlay_mod),
    ("fileSearchCloseBtn", "fileSearchCloseBtn" in overlay_src),
    ("close_overlay", "close_overlay" in overlay_src),
    ("refresh_theme", "refresh_theme" in overlay_src),
    ("_repolish_tree", "_repolish_tree" in overlay_src),
]:
    test(n, c)
test("fileSearchCard mint", "fileSearchCard" in build_stylesheet("mint"))
test("fileSearchGlobalBtn sky", "fileSearchGlobalBtn" in build_stylesheet("sky"))
test("no fileSearchGlobalCb sky", "fileSearchGlobalCb" not in build_stylesheet("sky"))
test("_DragTitleBar", "_DragTitleBar" in overlay_mod)
test("grabMouse", "grabMouse" in overlay_mod)
test("fileSearchTitleBar mint", "fileSearchTitleBar" in build_stylesheet("mint"))
for token in (
    "_clamp_to_screen",
    "_saved_pos",
    "fileSearchLocation",
    "fileSearchCard",
    "fileSearchGlobalBtn",
    "_run_global_search",
    "_try_force_digit_input",
    "_show_result_menu",
    "打开所在位置",
    "CustomContextMenu",
    "_reveal_item",
    "_result_row_text",
):
    test(token, token in overlay_src)
test("no fileSearchGlobalCb overlay", "fileSearchGlobalCb" not in overlay_src)
test("no _on_global_toggled", "_on_global_toggled" not in overlay_src)
test(
    "search_all_drives",
    "search_all_drives"
    in inspect.getsource(__import__("src.fd_search", fromlist=["x"]).search_config_slice),
)
ext_src = inspect.getsource(ew.ExtensionsWidget._build_ui)
test("no file_search_all_drives_cb", "file_search_all_drives_cb" not in ext_src)
apply_theme = inspect.getsource(DeskTidyApp._apply_theme)
test("_file_search_overlay apply", "_file_search_overlay" in apply_theme)
test("refresh_theme apply", "refresh_theme" in apply_theme)
iss = (ROOT / "installer" / "DeskTidy.iss").read_text(encoding="utf-8")
test("iss fd", r"assets\fd\fd.exe" in iss)
test("iss nocompression", "nocompression" in iss)
test("iss lzma2", 'DeskTidyCompress "lzma2/fast"' in iss or "lzma2/fast" in iss)
test("iss SolidCompression", "SolidCompression=no" in iss)
bat = (ROOT / "scripts" / "build_installer.bat").read_text(encoding="utf-8")
test("bat fd", r"assets\fd\fd.exe" in bat)
test("bat SKIP_PIP", "DESKTIDY_SKIP_PIP" in bat)
test("bat ALREADY_STOPPED", "DESKTIDY_ALREADY_STOPPED" in bat)
build_bat = (ROOT / "scripts" / "build.bat").read_text(encoding="utf-8")
test("build fd", r"assets\fd\fd.exe" in build_bat)
test("build dist fd", r"dist\assets\fd" in build_bat)
test("build ALREADY_STOPPED", "DESKTIDY_ALREADY_STOPPED" in build_bat)
print("done")
