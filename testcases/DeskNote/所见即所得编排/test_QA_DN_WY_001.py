# -*- coding: utf-8 -*-
"""QA-DN-WY-001: toolbar above title; title/body split-join."""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
HTML = ROOT / "native" / "assets" / "md_preview" / "wysiwyg_native.html"
RELEASE_HTML = (
    ROOT
    / "native"
    / "build"
    / "Release"
    / "assets"
    / "md_preview"
    / "wysiwyg_native.html"
)
FIXTURES = ROOT / "testcases" / "DeskNote" / "_fixtures" / "title_extract_cases.json"


def _html() -> str:
    assert HTML.is_file(), HTML
    return HTML.read_text(encoding="utf-8", errors="ignore")


def extract_markdown_title_cpp_mirror(md: str) -> str:
    """Mirror native ExtractMarkdownTitle (notes_window.cpp)."""
    if md.startswith("\ufeff"):
        md = md[1:]
    lines = md.split("\n")
    # Normalize to keep line ends logic similar (strip CR).
    lines = [ln[:-1] if ln.endswith("\r") else ln for ln in lines]
    i = 0
    if i < len(lines) and lines[i].strip() == "---":
        i += 1
        while i < len(lines):
            if lines[i].strip() == "---":
                i += 1
                break
            i += 1
    while i < len(lines):
        t = lines[i].strip()
        i += 1
        if not t:
            continue
        hashes = 0
        while hashes < len(t) and t[hashes] == "#":
            hashes += 1
        if 1 <= hashes <= 6 and hashes < len(t) and t[hashes] in " \t":
            title = t[hashes + 1 :].strip()
            while title.endswith("#"):
                title = title[:-1]
            return title.strip()
        break
    return ""


def test_layout_order_toolbar_title_editor():
    text = _html()
    i_tb = text.find('id="toolbar-host"')
    i_title = text.find('id="title-wrap"')
    i_editor = text.find('id="editor"')
    assert i_tb >= 0 and i_title >= 0 and i_editor >= 0
    assert i_tb < i_title < i_editor
    assert "placeToolbarAboveTitle" in text
    assert 'id="note-title"' in text


def test_release_asset_in_sync_when_present():
    if not RELEASE_HTML.is_file():
        pytest.skip("Release wysiwyg asset not built")
    src = _html()
    rel = RELEASE_HTML.read_text(encoding="utf-8", errors="ignore")
    assert 'id="toolbar-host"' in rel
    assert 'id="note-title"' in rel
    for marker in ("placeToolbarAboveTitle", "splitTitleBody", "joinTitleBody"):
        assert marker in src and marker in rel


def _js_helpers() -> str:
    html = _html()
    m_trim = re.search(r"function trimStr\(s\) \{.*?\n  \}", html, flags=re.S)
    m_split = re.search(
        r"function splitTitleBody\(md\) \{.*?\n  \}", html, flags=re.S
    )
    m_join = re.search(
        r"function joinTitleBody\(title, body\) \{.*?\n  \}", html, flags=re.S
    )
    assert m_trim and m_split and m_join
    return (
        "var emitTitleLine = true;\n"
        + m_trim.group(0)
        + "\n"
        + m_split.group(0)
        + "\n"
        + m_join.group(0)
        + "\n"
    )


def test_shared_title_extract_vectors_js_and_cpp_mirror():
    fixtures = json.loads(FIXTURES.read_text(encoding="utf-8"))
    helpers = _js_helpers()
    payload = json.dumps([{"md": c["md"], "title": c["title"]} for c in fixtures],
                         ensure_ascii=False)
    script = (
        helpers
        + "const fixtures = "
        + json.dumps(json.loads(payload), ensure_ascii=False)
        + """;
const out = [];
for (const f of fixtures) {
  const p = splitTitleBody(f.md);
  out.push({md: f.md, expected: f.title, js: p.title});
}
process.stdout.write(JSON.stringify(out));
"""
    )
    proc = subprocess.run(
        ["node", "-e", script],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        cwd=str(ROOT),
        check=False,
    )
    assert proc.returncode == 0, proc.stderr or proc.stdout
    rows = json.loads(proc.stdout)
    for row in rows:
        cpp = extract_markdown_title_cpp_mirror(row["md"])
        assert row["js"] == row["expected"], row
        assert cpp == row["expected"], (cpp, row)


def test_title_body_split_join_contracts():
    helpers = _js_helpers()
    script = (
        helpers
        + """
const cases = [];
function push(name, ok, detail) { cases.push({name, ok: !!ok, detail: detail || ""}); }

let p = splitTitleBody("# 会议纪要\\n\\n第一段\\n\\n第二段");
push("split_title", p.title === "会议纪要", JSON.stringify(p));
push("split_body_no_h1", !p.body.startsWith("#"), JSON.stringify(p));
let joined = joinTitleBody(p.title, p.body);
push("join_starts_h1", joined.startsWith("# 会议纪要"), joined.slice(0, 40));

p = splitTitleBody("# \\n\\n");
push("empty_title_seed", p.title === "", JSON.stringify(p));

p = splitTitleBody("正文开头\\n\\n## 不是标题\\n");
push("no_leading_h1", p.title === "", JSON.stringify(p));
push("body_keeps_h2", p.body.indexOf("## 不是标题") >= 0, JSON.stringify(p));

p = splitTitleBody("# only-title");
push("title_only_line", p.title === "only-title" && p.body === "", JSON.stringify(p));

p = splitTitleBody("纯正文\\n第二行");
push("body_only_split", p.title === "" && p.body.indexOf("纯正文") === 0, JSON.stringify(p));
let bodyOnlyJoin = joinTitleBody("", p.body);
push("body_only_no_invented_h1", bodyOnlyJoin.indexOf("#") !== 0 && bodyOnlyJoin.indexOf("纯正文") === 0, bodyOnlyJoin.slice(0, 40));

p = splitTitleBody("# \\n\\n");
let seedJoin = joinTitleBody("", "");
push("seed_still_emits_title_line", seedJoin.indexOf("# ") === 0, seedJoin);

process.stdout.write(JSON.stringify(cases));
"""
    )
    proc = subprocess.run(
        ["node", "-e", script],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        cwd=str(ROOT),
        check=False,
    )
    assert proc.returncode == 0, proc.stderr or proc.stdout
    cases = json.loads(proc.stdout)
    failed = [c for c in cases if not c["ok"]]
    assert not failed, failed
