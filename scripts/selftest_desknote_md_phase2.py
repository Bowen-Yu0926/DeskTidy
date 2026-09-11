"""deskNote Markdown phase-2: smart paste, focus/typewriter, Toast UI WYSIWYG."""

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


def test_html_to_markdown() -> None:
    from src.html_to_markdown import clipboard_html_to_markdown, html_to_markdown

    md = html_to_markdown("<p>Hello <b>world</b></p><ul><li>a</li><li>b</li></ul>")
    assert "**world**" in md
    assert "- a" in md and "- b" in md
    assert "# Title" in html_to_markdown("<h1>Title</h1>")
    assert "[x](https://e.com)" in html_to_markdown('<a href="https://e.com">x</a>')
    fallback = clipboard_html_to_markdown("<script></script>", "plain")
    assert fallback.strip() == "plain" or fallback == "plain\n" or "plain" in fallback


def test_wysiwyg_assets() -> None:
    from src.markdown_preview import md_preview_assets_dir

    assets = md_preview_assets_dir()
    assert assets is not None
    assert (assets / "wysiwyg.html").is_file()
    assert (assets / "vendor" / "toastui-editor-all.min.js").is_file()
    assert (assets / "vendor" / "toastui-editor.min.css").is_file()
    html = (assets / "wysiwyg.html").read_text(encoding="utf-8")
    assert "__desknoteWysiwygSet" in html
    assert "__desknoteWysiwygGet" in html
    assert "__desknoteWysiwygInsert" in html


def test_notepad_phase2_wiring() -> None:
    from PyQt6.QtWidgets import QApplication

    from src.desknote_help import DESKNOTE_HELP_TOPICS, desknote_help_html
    from src.ui.markdown_wysiwyg import MarkdownWysiwygPane
    from src.ui.notepad_window import NotepadWindow

    app = QApplication.instance() or QApplication([])
    w = NotepadWindow(
        {
            "theme": "sky",
            "notepad": {
                "md_edit_mode": "source",
                "typewriter_mode": True,
                "md_preview": True,
                "md_outline": True,
            },
        }
    )
    assert w._md_edit_mode == "source"
    assert w._typewriter_mode is True
    assert hasattr(w, "_mode_wysiwyg_action")
    assert hasattr(w, "_focus_action")
    assert hasattr(w, "_typewriter_action")
    src = inspect.getsource(NotepadWindow)
    assert "smart_paste" in src
    assert "_set_focus_mode" in src
    assert "_set_typewriter_mode" in src
    assert "md_edit_mode" in inspect.getsource(NotepadWindow._persist_md_settings)

    page = w._current_page()
    assert page is not None
    assert isinstance(page.wysiwyg, MarkdownWysiwygPane)
    assert page.stack.count() == 2

    w._set_focus_mode(True)
    assert page.outline.isVisible() is False
    assert page.preview.isVisible() is False
    w._set_focus_mode(False)

    topic_ids = {t[0] for t in DESKNOTE_HELP_TOPICS}
    assert "edit_modes" in topic_ids
    help_html = desknote_help_html("edit_modes")
    assert "所见即所得" in help_html
    assert "专注模式" in help_html
    assert "打字机" in help_html

    w.close()
    w.deleteLater()
    app.processEvents()


def main() -> int:
    print("DeskTidy deskNote Markdown phase-2")
    run("HTML→Markdown 智能粘贴", test_html_to_markdown)
    run("WYSIWYG 离线资源", test_wysiwyg_assets)
    run("记事本 phase2 接线", test_notepad_phase2_wiring)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err.splitlines()[0]}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
