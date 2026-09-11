"""Convert HTML clipboard fragments to Markdown (smart paste)."""

from __future__ import annotations

import html as html_lib
import re
from html.parser import HTMLParser


_BLOCK_CLOSE = frozenset(
    {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "tr", "pre", "blockquote"}
)


class _HtmlToMarkdown(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []
        self._stack: list[str] = []
        self._list_type: list[str] = []
        self._li_index: list[int] = []
        self._in_pre = False
        self._skip = 0

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        tag = tag.lower()
        if tag in {"script", "style", "head", "meta", "link"}:
            self._skip += 1
            return
        if self._skip:
            return
        attr = {k.lower(): (v or "") for k, v in attrs}
        if tag in {"b", "strong"}:
            self.parts.append("**")
            self._stack.append(tag)
        elif tag in {"i", "em"}:
            self.parts.append("*")
            self._stack.append(tag)
        elif tag == "code" and not self._in_pre:
            self.parts.append("`")
            self._stack.append(tag)
        elif tag == "a":
            href = attr.get("href", "")
            self._stack.append(f"a:{href}")
            self.parts.append("[")
        elif tag in {"h1", "h2", "h3", "h4", "h5", "h6"}:
            level = int(tag[1])
            self._ensure_nl()
            self.parts.append("#" * level + " ")
            self._stack.append(tag)
        elif tag == "br":
            self.parts.append("\n")
        elif tag == "p":
            self._ensure_nl()
            self._stack.append(tag)
        elif tag == "blockquote":
            self._ensure_nl()
            self._stack.append(tag)
        elif tag == "ul":
            self._ensure_nl()
            self._list_type.append("ul")
            self._li_index.append(0)
            self._stack.append(tag)
        elif tag == "ol":
            self._ensure_nl()
            self._list_type.append("ol")
            self._li_index.append(0)
            self._stack.append(tag)
        elif tag == "li":
            self._ensure_nl()
            if self._list_type and self._list_type[-1] == "ol":
                self._li_index[-1] += 1
                self.parts.append(f"{self._li_index[-1]}. ")
            else:
                self.parts.append("- ")
            self._stack.append(tag)
        elif tag == "pre":
            self._ensure_nl()
            self.parts.append("```\n")
            self._in_pre = True
            self._stack.append(tag)
        elif tag == "hr":
            self._ensure_nl()
            self.parts.append("---\n")
        elif tag == "img":
            alt = attr.get("alt", "")
            src = attr.get("src", "")
            self.parts.append(f"![{alt}]({src})")

    def handle_endtag(self, tag: str) -> None:
        tag = tag.lower()
        if tag in {"script", "style", "head", "meta", "link"}:
            self._skip = max(0, self._skip - 1)
            return
        if self._skip:
            return
        if tag in {"b", "strong", "i", "em"}:
            self.parts.append("**" if tag in {"b", "strong"} else "*")
            self._pop(tag)
        elif tag == "code" and not self._in_pre:
            self.parts.append("`")
            self._pop(tag)
        elif tag == "a":
            href = ""
            if self._stack and self._stack[-1].startswith("a:"):
                href = self._stack.pop()[2:]
            else:
                self._pop("a")
            self.parts.append(f"]({href})")
        elif tag in {"h1", "h2", "h3", "h4", "h5", "h6", "p"}:
            self.parts.append("\n\n")
            self._pop(tag)
        elif tag == "blockquote":
            # Prefix lines roughly.
            self.parts.append("\n")
            self._pop(tag)
        elif tag in {"ul", "ol"}:
            if self._list_type:
                self._list_type.pop()
            if self._li_index:
                self._li_index.pop()
            self.parts.append("\n")
            self._pop(tag)
        elif tag == "li":
            self.parts.append("\n")
            self._pop(tag)
        elif tag == "pre":
            self.parts.append("\n```\n\n")
            self._in_pre = False
            self._pop(tag)

    def handle_data(self, data: str) -> None:
        if self._skip:
            return
        if not data:
            return
        if self._in_pre:
            self.parts.append(data)
            return
        # Collapse whitespace outside pre.
        text = re.sub(r"[ \t\r\f\v]+", " ", data)
        text = text.replace("\n", " ")
        if self._stack and self._stack[-1] == "blockquote":
            for line in text.splitlines() or [text]:
                self.parts.append("> " + line)
            return
        self.parts.append(text)

    def _pop(self, tag: str) -> None:
        if self._stack and (
            self._stack[-1] == tag or self._stack[-1].startswith(f"{tag}:")
        ):
            self._stack.pop()

    def _ensure_nl(self) -> None:
        if self.parts and not self.parts[-1].endswith("\n"):
            self.parts.append("\n")

    def result(self) -> str:
        text = "".join(self.parts)
        text = html_lib.unescape(text)
        text = re.sub(r"\n{3,}", "\n\n", text)
        return text.strip() + ("\n" if text.strip() else "")


def html_to_markdown(html: str) -> str:
    """Best-effort HTML → Markdown for clipboard smart paste."""
    if not html or not html.strip():
        return ""
    # Strip Word / browser wrappers noise.
    cleaned = re.sub(r"(?is)<(script|style)[^>]*>.*?</\1>", "", html)
    parser = _HtmlToMarkdown()
    try:
        parser.feed(cleaned)
        parser.close()
    except Exception:
        return ""
    return parser.result()


def clipboard_html_to_markdown(html: str, plain: str | None = None) -> str:
    """Prefer HTML conversion; fall back to plain text."""
    md = html_to_markdown(html)
    if md.strip():
        return md
    return plain or ""
