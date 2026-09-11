"""deskNote Markdown phase-4: local images, Pandoc Word, word count."""

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


def test_md_assets_and_stats() -> None:
    from PyQt6.QtGui import QColor, QImage

    from src.md_assets import (
        copy_image_file,
        count_text_stats,
        is_image_path,
        save_image_bytes,
        save_qimage,
    )

    assert is_image_path("a.PNG")
    assert not is_image_path("a.md")
    chars, words = count_text_stats("你好 world\n")
    assert chars == 7  # 你好world
    assert words == 2

    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        dest, snip = save_image_bytes(b"\x89PNG\r\n\x1a\nfake", base, suffix=".png")
        assert dest.is_file()
        assert dest.parent.name == "assets"
        assert snip.startswith("![](assets/") and snip.endswith(")")

        img = QImage(8, 8, QImage.Format.Format_RGB32)
        img.fill(QColor("red"))
        dest2, snip2 = save_qimage(img, base)
        assert dest2.is_file() and "![](assets/" in snip2

        src = base / "shot.jpg"
        src.write_bytes(b"jpeg-bytes")
        dest3, snip3 = copy_image_file(src, base)
        assert dest3.suffix == ".jpg"
        assert "assets/" in snip3


def test_pandoc_helper() -> None:
    from src.pandoc_export import find_pandoc, pandoc_install_hint

    # May or may not be installed — just ensure API works.
    found = find_pandoc()
    assert found is None or Path(found).name.lower().startswith("pandoc")
    assert "pandoc.org" in pandoc_install_hint()


def test_notepad_phase4_wiring() -> None:
    from PyQt6.QtWidgets import QApplication

    from src.ui.notepad_window import NotepadWindow

    app = QApplication.instance() or QApplication([])
    w = NotepadWindow({"theme": "sky", "notepad": {}})
    assert hasattr(w, "_export_docx_action")
    src = inspect.getsource(NotepadWindow)
    assert "export_docx" in src
    assert "save_qimage" in src or "_try_paste_clipboard_image" in src
    assert "count_text_stats" in src
    # Status includes word count helper path.
    w.new_document()
    page = w._current_page()
    assert page is not None
    page.editor.setPlainText("hello 世界")
    # Pretend markdown path so image helpers use notes dir.
    with tempfile.TemporaryDirectory() as tmp:
        md = Path(tmp) / "note.md"
        md.write_text("# t\n", encoding="utf-8")
        page.path = md
        from src.md_assets import save_image_bytes

        _p, snip = save_image_bytes(b"x", w._md_base_dir(page), suffix=".png")
        w._insert_markdown_snippet(page, snip)
        assert "assets/" in page.editor.toPlainText()
    w._update_status()
    assert "字" in w._pos_label.text()
    w.close()
    w.deleteLater()
    app.processEvents()


def main() -> int:
    print("DeskTidy deskNote Markdown phase-4")
    run("图片落盘 / 字数", test_md_assets_and_stats)
    run("Pandoc 探测", test_pandoc_helper)
    run("记事本 phase4 接线", test_notepad_phase4_wiring)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err.splitlines()[0]}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
