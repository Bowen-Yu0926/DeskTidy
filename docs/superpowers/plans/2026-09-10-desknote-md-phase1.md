# deskNote MD Phase 1 Implementation Plan

> **For agentic workers:** Execute task-by-task. Steps use checkbox syntax.

**Goal:** WebEngine Markdown preview (highlight/KaTeX/Mermaid/TOC/footnotes), sync scroll, export PDF/HTML, with QTextBrowser fallback.

**Architecture:** Offline `assets/md_preview` viewer page; `MarkdownPreviewPane` wraps WebEngine or fallback; notepad wires scroll + File→Export.

**Tech Stack:** PyQt6-WebEngine, QWebChannel, marked, highlight.js, KaTeX, Mermaid (vendored).

**Spec:** `docs/superpowers/specs/2026-09-10-desknote-md-phase1-design.md`

## Global Constraints

- No runtime CDN fetches in production viewer.
- Keep source-only editing (no WYSIWYG this phase).
- Do not bump VERSION unless asked.
- Prefer root-cause wiring; keep fallback path.

---

### Task 1: Vendor assets + viewer.html

- [ ] Create `assets/md_preview/viewer.html` + CSS + bridge JS
- [ ] Download vendor min.js/css into `assets/md_preview/vendor/`
- [ ] Add datas entry in `DeskTidy.spec`

### Task 2: Preview pane + markdown helpers

- [ ] `webengine_available()`, `md_preview_assets_dir()`, front-matter strip, TOC/footnotes via JS stack
- [ ] `MarkdownPreviewPane` with `set_markdown`, scroll ratio signals, `print_to_pdf`
- [ ] Update `refresh_outline_and_preview` to accept pane protocol

### Task 3: Wire notepad_window

- [ ] Sync scroll setting + menu
- [ ] Export HTML/PDF actions
- [ ] Theme pass-through

### Task 4: Selftest + help + restart

- [ ] `scripts/selftest_desknote_md_preview.py`
- [ ] Help blurb update
- [ ] Run tests, cleanup, source restart
