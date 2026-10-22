#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// A single search hit: the file, the 0-based line (-1 = filename-only match),
// and a trimmed one-line context preview.
struct NoteSearchHit {
  std::wstring path;
  int line = -1;
  std::wstring preview;
};

// Pick / create a note file under the notes library. Returns empty on cancel.
std::wstring PickNoteFromLibrary(HWND owner);
// Search note filenames and text (encoding-aware, line + context); opens a
// result list with a case-sensitivity toggle. Returns the chosen file or empty.
std::wstring SearchNotesInLibrary(HWND owner);
std::wstring NotesLibraryDir(bool ensure = true);

// Encoding-aware recursive search over the library. `case_sensitive` defaults
// to the prefs switch when left false (the UI toggles it live). Returns up to
// `max_hits` matches; files larger than `max_file_bytes` are skipped.
std::vector<NoteSearchHit> SearchNotesLibrary(
    const std::wstring& dir, const std::wstring& query, bool case_sensitive,
    int max_hits, int max_file_bytes);
