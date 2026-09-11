"""deskNote Markdown phase-1: assets, WebEngine preview, sync scroll, export."""

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


def test_assets_present() -> None:
    from src.markdown_preview import md_preview_assets_dir

    assets = md_preview_assets_dir()
    assert assets is not None, "assets/md_preview/viewer.html missing"
    assert (assets / "viewer.html").is_file()
    assert (assets / "bridge.js").is_file()
    vendor = assets / "vendor"
    for name in (
        "marked.min.js",
        "highlight.min.js",
        "katex.min.js",
        "katex.min.css",
        "mermaid.min.js",
    ):
        path = vendor / name
        assert path.is_file() and path.stat().st_size > 1000, name


def test_front_matter_and_outline() -> None:
    from src.markdown_preview import extract_outline, strip_yaml_front_matter

    raw = "---\ntitle: t\n---\n# Hello\n\n## World\n"
    body = strip_yaml_front_matter(raw)
    assert not body.startswith("---")
    assert "# Hello" in body
    items = extract_outline(body)
    assert [i.title for i in items] == ["Hello", "World"]


def test_fallback_html_export() -> None:
    from src.markdown_preview import export_standalone_html, render_html

    html = render_html("# Hi\n\npara")
    assert "<h1" in html and "Hi" in html
    doc = export_standalone_html("# T", title="demo")
    assert "<title>demo</title>" in doc


def test_preview_pane_and_notepad_wiring() -> None:
    from PyQt6.QtWidgets import QApplication

    from src.markdown_preview import webengine_available
    from src.ui.markdown_pane import MarkdownPreviewPane
    from src.ui.notepad_window import NotepadWindow

    app = QApplication.instance() or QApplication([])
    pane = MarkdownPreviewPane()
    assert pane.engine in ("web", "fallback")
    if webengine_available() and (ROOT / "assets" / "md_preview" / "viewer.html").is_file():
        # Prefer web when both deps and assets exist.
        assert pane.engine == "web"
    pane.set_markdown("# x\n\n```python\nprint(1)\n```\n")
    pane.close()
    pane.deleteLater()

    w = NotepadWindow({"theme": "sky", "notepad": {"md_sync_scroll": True}})
    assert hasattr(w, "_md_sync_scroll") and w._md_sync_scroll is True
    assert hasattr(w, "_export_html_action")
    assert hasattr(w, "_export_pdf_action")
    assert "同步滚动" in inspect.getsource(NotepadWindow._build_menus)
    assert "export_html" in inspect.getsource(NotepadWindow)
    assert "export_pdf" in inspect.getsource(NotepadWindow)
    assert "md_sync_scroll" in inspect.getsource(NotepadWindow._persist_md_settings)
    # Spec packaging
    spec = (ROOT / "DeskTidy.spec").read_text(encoding="utf-8")
    assert "assets/md_preview" in spec
    assert "PyQt6.QtWebEngineWidgets" in spec
    w.close()
    w.deleteLater()
    app.processEvents()


def main() -> int:
    print("DeskTidy deskNote Markdown phase-1")
    run("预览资源齐全", test_assets_present)
    run("front matter / 大纲", test_front_matter_and_outline)
    run("降级 HTML 导出", test_fallback_html_export)
    run("预览控件与记事本接线", test_preview_pane_and_notepad_wiring)
    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    for name, err in failures:
        print(f"  - {name}: {err.splitlines()[0]}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
