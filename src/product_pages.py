"""Resolve and open bundled product HTML (DeskTidy / DeskNote — separate help)."""

from __future__ import annotations

import re
import sys
from pathlib import Path

from PyQt6.QtCore import QUrl
from PyQt6.QtGui import QDesktopServices


_MANUAL_BLOCK_RE = re.compile(
    r"<!--\s*HELP_MANUAL\s*-->.*?<!--\s*/HELP_MANUAL\s*-->",
    re.DOTALL | re.IGNORECASE,
)


def _candidates(name: str) -> list[Path]:
    """Possible locations for ``docs/<name>.html`` (dev + frozen)."""
    filename = name if name.endswith(".html") else f"{name}.html"
    roots: list[Path] = []
    if getattr(sys, "frozen", False):
        meipass = getattr(sys, "_MEIPASS", None)
        if meipass:
            roots.append(Path(meipass))
        roots.append(Path(sys.executable).resolve().parent)
        roots.append(Path(sys.executable).resolve().parent / "_internal")
    roots.append(Path(__file__).resolve().parent.parent)
    out: list[Path] = []
    for root in roots:
        out.append(root / "docs" / filename)
    return out


def product_page_path(name: str = "desktidy") -> Path | None:
    """Return first existing product HTML path, or None."""
    for path in _candidates(name):
        try:
            if path.is_file():
                return path.resolve()
        except OSError:
            continue
    return None


def product_page_url(name: str = "desktidy") -> QUrl | None:
    path = product_page_path(name)
    if path is None:
        return None
    return QUrl.fromLocalFile(str(path))


def _inject_manual(template: str, section: str) -> str:
    match = _MANUAL_BLOCK_RE.search(template)
    if match is not None:
        # Slice replace — re.sub treats backslashes in handbook HTML as escapes.
        return template[: match.start()] + section + template[match.end() :]
    footer_at = template.rfind("<footer")
    if footer_at >= 0:
        return template[:footer_at] + section + "\n    " + template[footer_at:]
    return template.replace("</body>", section + "\n</body>", 1)


def _handbook_section_html(settings: dict | None = None) -> str:
    """DeskTidy handbook only — DeskNote has its own page."""
    from src.help_content import HELP_TOPICS, topic_inner_html
    from src.i18n import APP_NAME_ZH

    topics = [(tid, title) for tid, title in HELP_TOPICS if tid != "notepad"]
    toc_items = "".join(
        f'<li><a href="#manual-{tid}">{title}</a></li>'
        for tid, title in topics
    )
    chapters: list[str] = []
    for tid, _title in topics:
        body = topic_inner_html(tid, settings)
        chapters.append(
            f'<article class="manual-chapter" id="manual-{tid}">{body}</article>'
        )
    dn_path = product_page_path("desknote")
    if dn_path is not None:
        dn_href = QUrl.fromLocalFile(str(dn_path)).toString()
    else:
        dn_href = "desknote.html"
    chapters.append(
        f"""<article class="manual-chapter" id="manual-desknote-pointer">
<h2>DeskNote（独立帮助）</h2>
<p class="purpose">DeskNote 是独立记事本，使用说明不放在本页。</p>
<p>请打开 DeskNote 后按 <strong>F1</strong>，或打开
<a href="{dn_href}">DeskNote 介绍与使用手册</a>（另开一份网页）。</p>
</article>"""
    )
    return f"""<!-- HELP_MANUAL -->
    <section class="block" id="manual">
      <div class="block-head">
        <p class="comment">/* operations handbook */</p>
        <h2>{APP_NAME_ZH} 使用手册</h2>
        <p>本页只讲 DeskTidy（分区、分页、截图等）。DeskNote 笔记有<strong>单独</strong>的介绍与手册。</p>
      </div>
      <nav class="manual-toc" aria-label="手册目录">
        <h3>目录</h3>
        <ol>
          {toc_items}
          <li><a href="#manual-desknote-pointer">DeskNote（独立帮助）</a></li>
        </ol>
      </nav>
      {"".join(chapters)}
    </section>
    <!-- /HELP_MANUAL -->"""


def _desknote_handbook_section_html() -> str:
    from src.desknote_help import DESKNOTE_HELP_TOPICS, desknote_topic_inner_html

    toc_items = "".join(
        f'<li><a href="#manual-{tid}">{title}</a></li>'
        for tid, title in DESKNOTE_HELP_TOPICS
    )
    chapters: list[str] = []
    for tid, _title in DESKNOTE_HELP_TOPICS:
        body = desknote_topic_inner_html(tid)
        chapters.append(
            f'<article class="manual-chapter" id="manual-{tid}">{body}</article>'
        )
    return f"""<!-- HELP_MANUAL -->
    <section class="block" id="manual">
      <div class="block-head">
        <p class="comment">/* DeskNote handbook */</p>
        <h2>DeskNote 使用手册</h2>
        <p>本页只讲 DeskNote。DeskTidy 桌面分区等请在 DeskTidy 主窗口「帮助」中查看。</p>
      </div>
      <nav class="manual-toc" aria-label="手册目录">
        <h3>目录</h3>
        <ol>
          {toc_items}
        </ol>
      </nav>
      {"".join(chapters)}
    </section>
    <!-- /HELP_MANUAL -->"""


def build_merged_desktidy_help_html(settings: dict | None = None) -> str | None:
    path = product_page_path("desktidy")
    if path is None:
        return None
    try:
        template = path.read_text(encoding="utf-8")
    except OSError:
        return None
    return _inject_manual(template, _handbook_section_html(settings))


def build_merged_desknote_help_html() -> str | None:
    path = product_page_path("desknote")
    if path is None:
        return None
    try:
        template = path.read_text(encoding="utf-8")
    except OSError:
        return None
    return _inject_manual(template, _desknote_handbook_section_html())


def merged_help_output_path() -> Path:
    from src.settings import APP_DIR, ensure_app_dir

    ensure_app_dir()
    return Path(APP_DIR) / "DeskTidy_帮助.html"


def merged_desknote_help_output_path() -> Path:
    from src.settings import APP_DIR, ensure_app_dir

    ensure_app_dir()
    return Path(APP_DIR) / "DeskNote_帮助.html"


def open_desktidy_help(settings: dict | None = None) -> bool:
    """Open DeskTidy-only help (intro + handbook) in the system browser."""
    html = build_merged_desktidy_help_html(settings)
    if not html:
        url = product_page_url("desktidy")
        if url is None or not url.isValid():
            return False
        return bool(QDesktopServices.openUrl(url))
    out = merged_help_output_path()
    try:
        out.write_text(html, encoding="utf-8")
    except OSError:
        return False
    url = QUrl.fromLocalFile(str(out.resolve()))
    return bool(url.isValid() and QDesktopServices.openUrl(url))


def open_desknote_help() -> bool:
    """Open DeskNote-only help (intro + handbook) in the system browser."""
    html = build_merged_desknote_help_html()
    if not html:
        url = product_page_url("desknote")
        if url is None or not url.isValid():
            return False
        return bool(QDesktopServices.openUrl(url))
    out = merged_desknote_help_output_path()
    try:
        out.write_text(html, encoding="utf-8")
    except OSError:
        return False
    url = QUrl.fromLocalFile(str(out.resolve()))
    return bool(url.isValid() and QDesktopServices.openUrl(url))


def open_product_page(name: str = "desktidy") -> bool:
    """Open DeskTidy or DeskNote help in the system browser."""
    key = name[:-5] if name.endswith(".html") else name
    if key == "desktidy":
        return open_desktidy_help()
    if key == "desknote":
        return open_desknote_help()
    url = product_page_url(name)
    if url is None or not url.isValid():
        return False
    return bool(QDesktopServices.openUrl(url))
