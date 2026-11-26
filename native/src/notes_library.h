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
// result list with a case-sensitivity toggle. Returns the chosen hit (empty
// path on cancel). line is 0-based, or -1 for filename-only matches (P1-19).
NoteSearchHit SearchNotesInLibrary(HWND owner);
std::wstring NotesLibraryDir(bool ensure = true);
// 文档\DeskTidy\notes (legacy / Explorer path users still open).
std::wstring DocumentsDeskTidyNotesDir();

// Encoding-aware recursive search over the library. `case_sensitive` defaults
// to the prefs switch when left false (the UI toggles it live). Returns up to
// `max_hits` matches; files larger than `max_file_bytes` are skipped.
std::vector<NoteSearchHit> SearchNotesLibrary(
    const std::wstring& dir, const std::wstring& query, bool case_sensitive,
    int max_hits, int max_file_bytes);

// Drop the cached note-file listing so the next search re-scans. Call after
// any note create / delete / rename (the cached listing is keyed on directory
// mtimes, so this is a belt-and-suspenders guard for coarse mtime filesystems).
void InvalidateNotesFileCache();
// Monotonic counter bumped by InvalidateNotesFileCache(). Secondary caches
// (e.g. the sidebar title peek) compare this to know when to drop entries;
// mtime+size alone cannot catch a same-length rewrite inside one second.
unsigned long long NotesFileCacheGeneration();

// Every openable note file under `dir`, recursively, as absolute paths. Shares
// the listing cache above (keyed on directory mtimes), so repeated calls while
// nothing changed cost one FindFirstFile per directory. This is the single
// source of truth for "what notes exist" — rails must read it instead of
// keeping their own copy that only one renderer happens to refresh.
std::vector<std::wstring> ListNoteFiles(const std::wstring& dir);
