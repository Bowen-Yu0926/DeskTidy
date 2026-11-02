# -*- coding: utf-8 -*-
"""QA-DN-TB-001: tab title hint, new-tab no leak, sidebar thumbtack."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def test_tab_label_prefers_document_title_hint():
    text = _read("notes_window.cpp")
    header = (NATIVE / "notes_window.h").read_text(encoding="utf-8", errors="ignore")
    assert "title_hint" in header
    assert "SyncCurrentTabTitleHint" in text
    assert "ExtractMarkdownTitle" in text or "title_hint" in text
    # TabLabel uses title_hint when present.
    assert "tab_states_[i].title_hint" in text or "title_hint.empty()" in text
    assert "TabLabel" in text


def test_new_tab_and_editor_push_from_edit_buffer_not_stale_cache():
    text = _read("notes_window.cpp")
    # NewTab clears state / pushes seed; EnsureMarkdownEditor reads RichEdit.
    assert "void NotesWindow::NewTab()" in text or "NotesWindow::NewTab(" in text
    assert 'u8"# \\n\\n"' in text or 'L"# \\n\\n"' in text
    assert "EnsureMarkdownEditor" in text
    # Authoritative push from edit buffer (regression: old Toast cache leak).
    assert "GetWindowTextW(edit_" in text
    # Comment or nearby logic must not prefer CurrentPlainText while wysiwyg on
    # for the EnsureMarkdownEditor load path — look for edit-buffer block.
    idx = text.find("void NotesWindow::EnsureMarkdownEditor")
    assert idx >= 0
    chunk = text[idx : idx + 2500]
    assert "GetWindowTextW(edit_" in chunk
    assert "web_.SetMarkdown(md)" in chunk or "web_.SetMarkdown(plain)" in chunk


def test_sidebar_pin_uses_thumbtack_not_star_prefix():
    text = _read("notes_window.cpp")
    assert "DrawTabPushPin" in text
    # Pinned tree labels must not use ★ prefix anymore.
    assert 'L"★ "' not in text
    # Still paints pin on sidebar custom-draw path.
    assert "CDDS_ITEMPOSTPAINT" in text
    assert "PathIsPinned" in text


def test_tab_session_persists_title_hint():
    text = _read("notes_window.cpp")
    persist = text[text.find("void NotesWindow::PersistTabs()") :]
    persist = persist[: persist.find("void NotesWindow::LoadTabSession()")]
    # C++ source writes JSON key via \"title\" escapes.
    assert "title" in persist and "title_hint" in persist
    assert "\\\"title\\\"" in persist or '\\"title\\"' in persist
    load = text[text.find("void NotesWindow::LoadTabSession()") :]
    load = load[:5000]
    assert "session_titles" in load
    assert 'field("title"' in load
