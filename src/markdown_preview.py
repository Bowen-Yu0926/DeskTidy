"""Markdown preview helpers: assets path, WebEngine probe, front matter."""

from __future__ import annotations

import html
import json
import re
from pathlib import Path

_FRONT_MATTER = re.compile(r"\A---\s*\n.*?\n---\s*\n?", re.DOTALL)
_MD_SUFFIXES = frozenset({".md", ".markdown"})


def is_markdown_path(path: Path | str | None) -> bool:
    if path is None:
        return False
    try:
        return Path(path).suffix.casefold() in _MD_SUFFIXES
    except (TypeError, ValueError, OSError):
        return False


def md_preview_assets_dir() -> Path | None:
    """Resolve ``assets/md_preview`` next to the package or frozen root."""
    here = Path(__file__).resolve()
    candidates = [
        here.parents[1] / "assets" / "md_preview",
        Path.cwd() / "assets" / "md_preview",
    ]
    try:
        import sys

        if getattr(sys, "frozen", False):
            candidates.insert(0, Path(sys._MEIPASS) / "assets" / "md_preview")  # type: ignore[attr-defined]
            candidates.insert(0, Path(sys.executable).resolve().parent / "assets" / "md_preview")
    except Exception:
        pass
    for path in candidates:
        if (path / "viewer.html").is_file():
            return path
    return None


def webengine_available() -> bool:
    try:
        from PyQt6.QtWebEngineWidgets import QWebEngineView  # noqa: F401

        return True
    except Exception:
        return False


def strip_yaml_front_matter(md: str) -> str:
    if not md.startswith("---"):
        return md
    return _FRONT_MATTER.sub("", md, count=1)


def js_string_literal(value: str) -> str:
    """JSON-encode a string for safe embedding in runJavaScript."""
    return json.dumps(value, ensure_ascii=False)


# Re-export outline / render from the original module surface after merge.
# (Implementation continues in this file for a single import path.)

from dataclasses import dataclass  # noqa: E402

_ATX_HEADING = re.compile(r"^(#{1,6})\s*(.+?)\s*#*\s*$")
_ATX_MISSING_SPACE = re.compile(r"^(#{1,6})(\S.*)$")


@dataclass(frozen=True)
class OutlineItem:
    level: int
    title: str
    line: int  # 0-based


def normalize_atx_headings(md: str) -> str:
    """Insert a space after ``#``…``######`` when the title starts immediately."""
    lines = md.splitlines(keepends=True)
    out: list[str] = []
    in_fence = False
    for line in lines:
        bare = line.rstrip("\r\n")
        ending = line[len(bare) :]
        stripped = bare.lstrip()
        if stripped.startswith("```") or stripped.startswith("~~~"):
            in_fence = not in_fence
            out.append(line)
            continue
        if not in_fence:
            m = _ATX_MISSING_SPACE.match(bare)
            if m and not bare.startswith("#" * 7):
                hashes, rest = m.group(1), m.group(2)
                if not rest.startswith("#"):
                    bare = f"{hashes} {rest}"
                    line = bare + ending
        out.append(line)
    return "".join(out)


def extract_outline(md: str) -> list[OutlineItem]:
    items: list[OutlineItem] = []
    for i, raw in enumerate(normalize_atx_headings(md).splitlines()):
        m = _ATX_HEADING.match(raw)
        if not m:
            continue
        title = m.group(2).strip()
        if not title or set(title) <= {"#"}:
            continue
        items.append(OutlineItem(level=len(m.group(1)), title=title, line=i))
    return items


def _rewrite_relative_images(html_body: str, base_dir: Path | None) -> str:
    if base_dir is None:
        return html_body

    def _repl(match: re.Match[str]) -> str:
        prefix, src, suffix = match.group(1), match.group(2), match.group(3)
        src_stripped = src.strip()
        if not src_stripped or src_stripped.startswith(
            ("http://", "https://", "data:", "file:", "#")
        ):
            return match.group(0)
        try:
            full = (base_dir / src_stripped).resolve()
            uri = full.as_uri()
        except OSError:
            return match.group(0)
        return f"{prefix}{uri}{suffix}"

    return re.sub(
        r'(<img\b[^>]*\bsrc=")([^"]+)(")',
        _repl,
        html_body,
        flags=re.IGNORECASE,
    )


def render_html(md: str, *, base_dir: Path | None = None) -> str:
    """Fallback HTML for QTextBrowser when WebEngine is unavailable."""
    md = normalize_atx_headings(strip_yaml_front_matter(md))
    try:
        import markdown as md_lib
    except ImportError:
        escaped = html.escape(md)
        body = f"<pre>{escaped}</pre>"
        return _wrap_document(body)

    extensions = [
        "extra",
        "tables",
        "fenced_code",
        "sane_lists",
        "toc",
        "nl2br",
        "footnotes",
    ]
    try:
        body = md_lib.markdown(md, extensions=extensions)
    except Exception:
        body = f"<pre>{html.escape(md)}</pre>"
    body = _rewrite_relative_images(body, base_dir)
    return _wrap_document(body)


def _wrap_document(body: str) -> str:
    return f"""<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
body {{
  font-family: "Segoe UI", "Microsoft YaHei UI", sans-serif;
  font-size: 14px;
  line-height: 1.55;
  color: #1f2937;
  margin: 12px 16px;
  background: #ffffff;
}}
h1,h2,h3,h4,h5,h6 {{ margin: 1.1em 0 0.45em; font-weight: 600; }}
h1 {{ font-size: 1.7em; border-bottom: 1px solid #e5e7eb; padding-bottom: 0.25em; }}
h2 {{ font-size: 1.4em; }}
h3 {{ font-size: 1.2em; }}
code, pre {{
  font-family: Consolas, "Cascadia Mono", "Courier New", monospace;
  font-size: 0.92em;
}}
code {{ background: #f3f4f6; padding: 0.1em 0.35em; border-radius: 3px; }}
pre {{ background: #f3f4f6; padding: 10px 12px; border-radius: 6px; overflow-x: auto; }}
pre code {{ background: transparent; padding: 0; }}
blockquote {{
  margin: 0.6em 0; padding: 0.2em 0.9em; border-left: 3px solid #93c5fd;
  color: #4b5563; background: #f8fafc;
}}
table {{ border-collapse: collapse; margin: 0.8em 0; }}
th, td {{ border: 1px solid #d1d5db; padding: 6px 10px; }}
th {{ background: #f3f4f6; }}
img {{ max-width: 100%; height: auto; }}
a {{ color: #2563eb; }}
hr {{ border: none; border-top: 1px solid #e5e7eb; margin: 1.2em 0; }}
</style></head><body>{body}</body></html>"""


def export_standalone_html(
    md: str,
    *,
    base_dir: Path | None = None,
    title: str = "export",
) -> str:
    """Self-contained HTML via fallback renderer (always available)."""
    doc = render_html(md, base_dir=base_dir)
    # Inject title.
    return doc.replace("<head>", f"<head><title>{html.escape(title)}</title>", 1)
