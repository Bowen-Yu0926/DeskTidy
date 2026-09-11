"""Notes library helpers: walk folder and search filename / content."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from src.notepad import NOTE_OPEN_SUFFIXES, is_notepad_openable_path

# Skip huge files during content search (bytes).
MAX_SEARCH_FILE_BYTES = 512 * 1024
# Cap results so UI stays responsive.
MAX_SEARCH_HITS = 200


@dataclass(frozen=True)
class LibrarySearchHit:
    path: Path
    line: int  # 0-based; -1 = filename-only match
    preview: str


def notes_name_filters() -> list[str]:
    """QFileSystemModel name filters for openable note/code files."""
    return sorted({f"*{suf}" for suf in NOTE_OPEN_SUFFIXES})


def keep_filename_suffix(old_name: str, typed: str) -> str:
    """Build a new basename that always keeps ``old_name``'s suffix (Explorer-style).

    Users edit only the stem; a typed matching/other openable extension is
    stripped and the original suffix is re-applied. Empty input → ``old_name``.
    """
    old = Path(str(old_name or ""))
    suffix = old.suffix
    raw = str(typed or "").strip().replace("\\", "/").split("/")[-1]
    if not raw:
        return old.name or str(old_name)
    lower = raw.lower()
    if suffix and lower.endswith(suffix.lower()):
        raw = raw[: -len(suffix)]
    else:
        # e.g. renaming a.md but typing "note.txt" → keep .md, not .txt.md
        for cand in sorted(NOTE_OPEN_SUFFIXES, key=len, reverse=True):
            if lower.endswith(cand):
                raw = raw[: -len(cand)]
                break
    raw = raw.rstrip(". ").strip()
    if not raw:
        return old.name or str(old_name)
    return raw + suffix


def _decode_for_search(data: bytes) -> str:
    for enc in ("utf-8-sig", "utf-8", "gbk", "utf-16"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            continue
    return data.decode("utf-8", errors="replace")


def iter_openable_files(root: Path, *, max_files: int = 5000) -> list[Path]:
    """Recursive list of openable files under *root* (skips hidden / backup litter)."""
    if not root.is_dir():
        return []
    out: list[Path] = []
    try:
        for path in root.rglob("*"):
            if len(out) >= max_files:
                break
            try:
                if not path.is_file():
                    continue
                name = path.name
                if name.startswith(".") or name.startswith("_sess_"):
                    continue
                # Skip common junk / VCS.
                parts = {p.casefold() for p in path.parts}
                if parts & {".git", "node_modules", "__pycache__", ".venv", "venv"}:
                    continue
                if not is_notepad_openable_path(path):
                    continue
                out.append(path.resolve())
            except OSError:
                continue
    except OSError:
        return out
    return out


def search_notes_library(
    root: Path,
    query: str,
    *,
    max_hits: int = MAX_SEARCH_HITS,
    max_file_bytes: int = MAX_SEARCH_FILE_BYTES,
) -> list[LibrarySearchHit]:
    """Search filename and text content under *root*."""
    q = (query or "").strip()
    if not q or not root.is_dir():
        return []
    q_cf = q.casefold()
    hits: list[LibrarySearchHit] = []
    for path in iter_openable_files(root):
        if len(hits) >= max_hits:
            break
        name_hit = q_cf in path.name.casefold()
        if name_hit:
            hits.append(LibrarySearchHit(path=path, line=-1, preview=path.name))
            if len(hits) >= max_hits:
                break
        try:
            size = path.stat().st_size
        except OSError:
            continue
        if size <= 0 or size > max_file_bytes:
            continue
        try:
            text = _decode_for_search(path.read_bytes())
        except OSError:
            continue
        for i, line in enumerate(text.splitlines()):
            if q_cf in line.casefold():
                preview = line.strip()
                if len(preview) > 120:
                    preview = preview[:117] + "…"
                hits.append(LibrarySearchHit(path=path, line=i, preview=preview))
                if len(hits) >= max_hits:
                    return hits
                # One content hit per file is enough for browsing; keep scanning
                # other files. (Still allow filename hit + one content hit.)
                break
    return hits
