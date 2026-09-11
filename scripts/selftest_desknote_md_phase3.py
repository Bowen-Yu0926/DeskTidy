"""deskNote Markdown phase-3: notes library sidebar + search."""

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


def test_library_search() -> None:
    from src.notes_library import keep_filename_suffix, search_notes_library

    assert keep_filename_suffix("未命名.md", "笔记") == "笔记.md"
    assert keep_filename_suffix("a.md", "b.txt") == "b.md"
    assert keep_filename_suffix("a.md", "b.md") == "b.md"
    assert keep_filename_suffix("a.md", "") == "a.md"
    assert keep_filename_suffix("my.notes.md", "my.notes") == "my.notes.md"

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "alpha.md").write_text("# Hello\nunique_token_xyz\n", encoding="utf-8")
        (root / "sub").mkdir()
        (root / "sub" / "beta.txt").write_text("other line\n", encoding="utf-8")
        hits = search_notes_library(root, "unique_token_xyz")
        assert hits and hits[0].path.name == "alpha.md"
        assert hits[0].line == 1
        name_hits = search_notes_library(root, "beta")
        assert any(h.path.name == "beta.txt" for h in name_hits)


def test_library_pane_and_wiring() -> None:
    from PyQt6.QtWidgets import QApplication

    from src.desknote_help import DESKNOTE_HELP_TOPICS, desknote_help_html
    from src.ui.notes_library_pane import NotesLibraryPane
    from src.ui.notepad_window import NotepadWindow

    app = QApplication.instance() or QApplication([])
    with tempfile.TemporaryDirectory() as tmp:
        notes = Path(tmp)
        (notes / "demo.md").write_text("# Demo\nbody\n", encoding="utf-8")
        w = NotepadWindow(
            {
                "theme": "sky",
                "notepad": {
                    "folder": str(notes),
                    "library_sidebar": True,
                },
            }
        )
        assert "KeepSuffixRenameDelegate" in inspect.getsource(NotesLibraryPane) or (
            "_KeepSuffixRenameDelegate" in inspect.getsource(
                __import__("src.ui.notes_library_pane", fromlist=["x"])
            )
        )
        assert "keep_filename_suffix" in inspect.getsource(NotepadWindow._on_library_rename)
        assert "src.stem" in inspect.getsource(NotepadWindow._on_library_rename)
        assert not w.library.isHidden()
        assert hasattr(w.library, "_btn_refresh")
        assert not hasattr(w.library, "_btn_more")
        assert not hasattr(w.library, "_btn_folder")
        assert "reveal_requested" in inspect.getsource(NotesLibraryPane)
        assert "打开文件所在目录" in inspect.getsource(NotesLibraryPane._file_actions_menu)
        pane_src = inspect.getsource(
            __import__("src.ui.notes_library_pane", fromlist=["x"])
        )
        assert "_CreationTimeSortProxy" in pane_src
        assert "DescendingOrder" in pane_src
        assert "_path_ctime" in pane_src
        assert "invalidate_ctime_cache" in pane_src
        assert "_ctime_cache" in pane_src
        assert "setDynamicSortFilter(False)" in pane_src
        assert "desknote-library-search" in pane_src
        assert "call_on_main_thread" in pane_src
        # Behavioral: newer file appears above older in the tree.
        import time

        from PyQt6.QtCore import Qt

        older = notes / "older.md"
        newer = notes / "newer.md"
        older.write_text("# old\n", encoding="utf-8")
        time.sleep(0.05)
        newer.write_text("# new\n", encoding="utf-8")
        w.library.set_root(notes)
        app.processEvents()
        # Force resort after filesystem model populates.
        w.library._tree.sortByColumn(0, Qt.SortOrder.DescendingOrder)
        for _ in range(30):
            app.processEvents()
            time.sleep(0.02)
            root = w.library._tree.rootIndex()
            rows = w.library._proxy.rowCount(root)
            if rows >= 2:
                break
        names: list[str] = []
        root = w.library._tree.rootIndex()
        for row in range(w.library._proxy.rowCount(root)):
            idx = w.library._proxy.index(row, 0, root)
            src = w.library._proxy.mapToSource(idx)
            names.append(Path(w.library._model.filePath(src)).name)
        assert "newer.md" in names and "older.md" in names, names
        assert names.index("newer.md") < names.index("older.md"), names
        assert "打开文件所在目录" in inspect.getsource(NotepadWindow._on_tab_context_menu)
        assert "_TabCloseButton" in inspect.getsource(NotepadWindow)
        assert "_install_close_button" in inspect.getsource(NotepadWindow)
        assert "文件树" in inspect.getsource(NotepadWindow._build_menus)
        assert "library_sidebar" in inspect.getsource(NotepadWindow._persist_md_settings)
        assert "patch_notepad_settings" in inspect.getsource(NotepadWindow._persist_md_settings)
        assert "focus_library_search" in inspect.getsource(NotepadWindow)

        w._activate_or_open_path(notes / "demo.md")
        page = w._current_page()
        assert page is not None and page.path is not None
        assert page.path.name == "demo.md"

        # Second activate should reuse tab.
        before = len(w._pages)
        w._activate_or_open_path(notes / "demo.md")
        assert len(w._pages) == before

        w._set_library_sidebar(False)
        assert w.library.isHidden()
        w._set_library_sidebar(True)
        assert not w.library.isHidden()

        w._set_focus_mode(True)
        assert w.library.isHidden()
        w._set_focus_mode(False)
        assert not w.library.isHidden()

        topic_ids = {t[0] for t in DESKNOTE_HELP_TOPICS}
        assert "library" in topic_ids
        assert "笔记库" in desknote_help_html("library")

        w.close()
        w.deleteLater()
        app.processEvents()


def main() -> int:
    print("DeskTidy deskNote Markdown phase-3")
    run("库内搜索", test_library_search)
    run("侧栏接线", test_library_pane_and_wiring)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err.splitlines()[0]}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
