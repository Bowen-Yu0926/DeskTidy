"""Find / replace helpers for the built-in notepad."""

from __future__ import annotations

from PyQt6.QtGui import QTextCursor, QTextDocument


def _find_flags(*, case_sensitive: bool, whole_word: bool) -> QTextDocument.FindFlag:
    flags = QTextDocument.FindFlag(0)
    if case_sensitive:
        flags |= QTextDocument.FindFlag.FindCaseSensitively
    if whole_word:
        flags |= QTextDocument.FindFlag.FindWholeWords
    return flags


def find_next(
    doc: QTextDocument,
    cursor: QTextCursor,
    needle: str,
    *,
    case_sensitive: bool = False,
    whole_word: bool = False,
    wrap: bool = True,
) -> QTextCursor | None:
    if not needle:
        return None
    flags = _find_flags(case_sensitive=case_sensitive, whole_word=whole_word)
    found = doc.find(needle, cursor, flags)
    if not found.isNull():
        return found
    if not wrap:
        return None
    wrapped = doc.find(needle, 0, flags)
    if wrapped.isNull():
        return None
    return wrapped


def find_previous(
    doc: QTextDocument,
    cursor: QTextCursor,
    needle: str,
    *,
    case_sensitive: bool = False,
    whole_word: bool = False,
    wrap: bool = True,
) -> QTextCursor | None:
    if not needle:
        return None
    flags = _find_flags(case_sensitive=case_sensitive, whole_word=whole_word)
    start = max(0, cursor.selectionStart() if cursor.hasSelection() else cursor.position() - 1)
    probe = QTextCursor(doc)
    probe.setPosition(start)
    found = doc.find(needle, probe, flags | QTextDocument.FindFlag.FindBackward)
    if not found.isNull():
        return found
    if not wrap:
        return None
    probe = QTextCursor(doc)
    probe.movePosition(QTextCursor.MoveOperation.End)
    wrapped = doc.find(needle, probe, flags | QTextDocument.FindFlag.FindBackward)
    if wrapped.isNull():
        return None
    return wrapped


def replace_once(
    editor_cursor: QTextCursor,
    needle: str,
    replacement: str,
    *,
    case_sensitive: bool = False,
    whole_word: bool = False,
) -> bool:
    """Replace current selection when it matches needle."""
    if not needle:
        return False
    selected = editor_cursor.selectedText().replace("\u2029", "\n")
    if not selected:
        return False
    if case_sensitive:
        if selected != needle:
            return False
    elif selected.casefold() != needle.casefold():
        return False
    if whole_word:
        doc = editor_cursor.document()
        flags = _find_flags(case_sensitive=case_sensitive, whole_word=True)
        at = doc.find(needle, editor_cursor.selectionStart(), flags)
        if at.isNull() or at.selectionStart() != editor_cursor.selectionStart():
            return False
    editor_cursor.insertText(replacement)
    return True


def replace_all_in_document(
    doc: QTextDocument,
    needle: str,
    replacement: str,
    *,
    case_sensitive: bool = False,
    whole_word: bool = False,
) -> int:
    if not needle:
        return 0
    flags = _find_flags(case_sensitive=case_sensitive, whole_word=whole_word)
    cursor = QTextCursor(doc)
    cursor.beginEditBlock()
    count = 0
    pos = 0
    while True:
        hit = doc.find(needle, pos, flags)
        if hit.isNull():
            break
        hit.insertText(replacement)
        count += 1
        pos = hit.position()
    cursor.endEditBlock()
    return count
