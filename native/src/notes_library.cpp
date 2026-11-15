#include "notes_library.h"

#include "app_prefs.h"
#include "mist_msg.h"
#include "mist_webview.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "tool_paths.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstring>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Mirrors notes_window.cpp IsOpenableSuffix (Python notepad.py NOTE_OPEN_SUFFIXES).
bool IsOpenableSuffix(const std::wstring& lower) {
  static const wchar_t* kSfx[] = {
      L".txt",    L".text",  L".md",     L".markdown", L".log",   L".csv",
      L".tsv",    L".json",  L".xml",    L".yml",      L".yaml",  L".toml",
      L".ini",    L".cfg",   L".conf",   L".config",   L".env",   L".properties",
      L".py",     L".pyw",   L".pyi",    L".js",       L".mjs",   L".cjs",
      L".ts",     L".tsx",   L".jsx",    L".css",      L".scss",  L".less",
      L".html",   L".htm",   L".xhtml",  L".vue",      L".java",  L".c",
      L".h",      L".cpp",   L".hpp",    L".cc",       L".cs",    L".go",
      L".rs",     L".rb",    L".php",    L".sql",      L".sh",    L".bash",
      L".zsh",    L".ps1",   L".psm1",   L".bat",      L".cmd",   L".vbs",
      L".gitignore", L".gitattributes", L".editorconfig", L".dockerfile",
      L".makefile", L".cmake", L".gradle", L".r",        L".lua",   L".pl",
      L".swift",  L".kt",    L".kts",    L".dart",     L".jsonc", L".json5",
      L".svg",    L".tex",   L".rst",    L".adoc",     L".nfo",   L".diz",
      L".reg",    L".inf",   L".url",    L".eml",      L".ics",   L".rtf",
  };
  for (const wchar_t* sfx : kSfx) {
    if (lower == sfx) {
      return true;
    }
  }
  return false;
}

std::wstring LowerSuffix(const std::wstring& name) {
  const size_t dot = name.find_last_of(L'.');
  if (dot == std::wstring::npos) {
    return {};
  }
  std::wstring sfx = name.substr(dot);
  for (auto& c : sfx) {
    if (c >= L'A' && c <= L'Z') {
      c += (L'a' - L'A');
    }
  }
  return sfx;
}

bool DirExists(const std::wstring& dir) {
  const DWORD a = GetFileAttributesW(dir.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Case-insensitive lowercase for search (ASCII + CJK-safe passthrough).
std::wstring WideLower(const std::wstring& s) {
  if (s.empty()) {
    return {};
  }
  const int len = static_cast<int>(s.size());
  std::wstring out(static_cast<size_t>(len) + 1, L'\0');
  const int n = LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, s.c_str(), len,
                             out.data(), static_cast<int>(out.size()));
  if (n > 0) {
    out.resize(static_cast<size_t>(n));
    return out;
  }
  // Fallback: ASCII-only lower.
  std::wstring r = s;
  for (auto& c : r) {
    if (c >= L'A' && c <= L'Z') {
      c += (L'a' - L'A');
    }
  }
  return r;
}

bool DecodeBytes(const std::string& data, std::wstring* out) {
  if (data.empty()) {
    out->clear();
    return true;
  }
  const auto* raw = reinterpret_cast<const BYTE*>(data.data());
  const size_t size = data.size();
  // UTF-16 BOM detection (LE / BE).
  if (size >= 2) {
    if (raw[0] == 0xFF && raw[1] == 0xFE) {
      const size_t units = (size - 2) / 2;
      out->assign(reinterpret_cast<const wchar_t*>(raw + 2),
                  reinterpret_cast<const wchar_t*>(raw + 2) + units);
      return true;
    }
    if (raw[0] == 0xFE && raw[1] == 0xFF) {
      const size_t units = (size - 2) / 2;
      out->resize(units);
      for (size_t i = 0; i < units; ++i) {
        const wchar_t c = static_cast<wchar_t>((raw[2 + i * 2] << 8) |
                                               raw[2 + i * 2 + 1]);
        (*out)[i] = c;
      }
      return true;
    }
  }
  // Strip UTF-8 BOM.
  const char* begin = data.data();
  int utf8_len = static_cast<int>(size);
  if (size >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
    begin += 3;
    utf8_len -= 3;
  }
  // Try UTF-8 (strict).
  int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, begin, utf8_len,
                                 nullptr, 0);
  if (wlen > 0) {
    out->resize(static_cast<size_t>(wlen));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, begin, utf8_len,
                        out->data(), wlen);
    return true;
  }
  // Try GBK (codepage 936).
  wlen = MultiByteToWideChar(936, 0, data.data(), static_cast<int>(size), nullptr,
                             0);
  if (wlen > 0) {
    out->resize(static_cast<size_t>(wlen));
    MultiByteToWideChar(936, 0, data.data(), static_cast<int>(size), out->data(),
                        wlen);
    return true;
  }
  // Try UTF-16 LE without BOM (even length).
  if (size % 2 == 0) {
    const size_t units = size / 2;
    out->assign(reinterpret_cast<const wchar_t*>(raw),
                reinterpret_cast<const wchar_t*>(raw) + units);
    return true;
  }
  return false;
}

bool HasNul(const std::wstring& s) {
  return s.find(L'\0') != std::wstring::npos;
}

bool ReadFileBytes(const std::wstring& path, std::string* out, size_t* out_size) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff len = in.tellg();
  if (len < 0) {
    return false;
  }
  if (out_size) {
    *out_size = static_cast<size_t>(len);
  }
  in.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(len));
  if (len > 0) {
    in.read(out->data(), len);
  }
  return true;
}

// Last-write time of a directory (FindFirstFileW on the dir itself).
FILETIME DirMtime(const std::wstring& dir) {
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(dir.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    FILETIME zero{};
    return zero;
  }
  FILETIME ft = fd.ftLastWriteTime;
  FindClose(h);
  return ft;
}

struct DirStamp {
  std::wstring path;
  FILETIME mtime{};
};

// Process-level cache of the recursive file listing. Search runs on the UI
// thread, but the mutex keeps it safe if a future caller detaches a thread.
std::mutex g_notes_files_mutex;
bool g_notes_files_valid = false;
std::wstring g_notes_files_dir;
std::vector<std::wstring> g_notes_files;
std::vector<DirStamp> g_notes_dir_stamps;

// Recursive listing of openable files (skips hidden/junk dirs and files).
void WalkOpenable(const std::wstring& dir, int depth, int max_files,
                  std::vector<std::wstring>* out,
                  std::vector<DirStamp>* stamps) {
  if (static_cast<int>(out->size()) >= max_files || depth > 12) {
    return;
  }
  if (stamps) {
    stamps->push_back({dir, DirMtime(dir)});
  }
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (static_cast<int>(out->size()) >= max_files) {
      break;
    }
    const std::wstring name = fd.cFileName;
    if (name == L"." || name == L"..") {
      continue;
    }
    const std::wstring full = JoinPath(dir, name);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      const std::wstring low = WideLower(name);
      if (low == L".git" || low == L"node_modules" || low == L"__pycache__" ||
          low == L".venv" || low == L"venv" || low == L".svn" ||
          low == L"assets") {
        continue;
      }
      WalkOpenable(full, depth + 1, max_files, out, stamps);
      continue;
    }
    if (!name.empty() && (name[0] == L'.' || name.rfind(L"_sess_", 0) == 0)) {
      continue;
    }
    if (IsOpenableSuffix(LowerSuffix(name))) {
      out->push_back(full);
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

std::vector<std::wstring> ListNoteFilesCached(const std::wstring& dir) {
  {
    std::lock_guard<std::mutex> lock(g_notes_files_mutex);
    if (g_notes_files_valid && g_notes_files_dir == dir) {
      bool changed = false;
      for (const auto& s : g_notes_dir_stamps) {
        const FILETIME mt = DirMtime(s.path);
        if (CompareFileTime(&mt, &s.mtime) != 0) {
          changed = true;
          break;
        }
      }
      if (!changed) {
        return g_notes_files;
      }
    }
  }
  std::vector<std::wstring> files;
  std::vector<DirStamp> stamps;
  WalkOpenable(dir, 0, 5000, &files, &stamps);
  std::lock_guard<std::mutex> lock(g_notes_files_mutex);
  g_notes_files_dir = dir;
  g_notes_files = files;
  g_notes_dir_stamps = std::move(stamps);
  g_notes_files_valid = true;
  return files;
}

bool WideContains(const std::wstring& hay, const std::wstring& needle) {
  return hay.find(needle) != std::wstring::npos;
}

std::wstring RunPickDialog(HWND owner, const std::wstring& dir,
                           const std::vector<std::wstring>& files,
                           const wchar_t* title, bool allow_new) {
  std::wstring result;
  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.frameless = true;
  dlg.user_data_subdir = L"webview2-notes-pick";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\pick_list.html");
  if (dlg.html_path.empty()) {
    MistAlert(owner, title ? title : L"笔记库", L"找不到 pick_list.html",
              MistMsgTone::Warn);
    return {};
  }
  dlg.build_state = [&]() {
    std::wstring json = L"{\"type\":\"state\",\"title\":\"";
    json += MistJsonEscape(title ? title : L"笔记库");
    json += L"\",\"hint\":\"";
    json += MistJsonEscape(dir);
    json += L"\",\"open_label\":\"打开\",\"allow_new\":";
    json += allow_new ? L"true" : L"false";
    json += L",\"sel\":0,\"items\":[";
    for (size_t i = 0; i < files.size(); ++i) {
      if (i) {
        json += L',';
      }
      json += L'"';
      json += MistJsonEscape(files[i]);
      json += L'"';
    }
    json += L"]}";
    return json;
  };
  constexpr WPARAM kDeferNew = 1;
  dlg.on_deferred = [&](WPARAM op, LPARAM) {
    if (op != kDeferNew || !allow_new) {
      return;
    }
    HWND parent = dlg.hwnd ? dlg.hwnd : owner;
    const std::wstring name = PromptText(
        parent, L"新建笔记", L"文件名（不含路径，如 idea.md）：", L"note.md");
    if (!name.empty()) {
      result = JoinPath(dir, name);
      MistWebDialogClose(&dlg);
    }
  };
  dlg.on_message = [&](const std::wstring& json) -> bool {
    const std::wstring type = MistJsonStringField(json, L"type");
    if (type != L"action") {
      return false;
    }
    const std::wstring id = MistJsonStringField(json, L"id");
    if (id == L"ready") {
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"open") {
      const int index = MistJsonIntField(json, L"index", -1);
      if (index >= 0 && index < static_cast<int>(files.size())) {
        result = JoinPath(dir, files[static_cast<size_t>(index)]);
        return true;
      }
      return false;
    }
    if (id == L"new" && allow_new) {
      // Leave WebMessageReceived before Mist prompt (nested host freezes).
      if (dlg.hwnd) {
        PostMessageW(dlg.hwnd, WM_APP + 81, kDeferNew, 0);
      }
      return false;
    }
    if (id == L"cancel") {
      return true;
    }
    return false;
  };

  MistWebDialogRun(&dlg, title ? title : L"笔记库", 520, 420);
  return result;
}

// True if the directory contains any entry besides . / ..
bool DirHasEntries(const std::wstring& dir) {
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool found = false;
  do {
    if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
      found = true;
      break;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return found;
}

void EnsureDirRecursive(const std::wstring& dir) {
  if (dir.empty() || DirExists(dir)) {
    return;
  }
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    EnsureDirRecursive(dir.substr(0, slash));
  }
  CreateDirectoryW(dir.c_str(), nullptr);
}

void CopyDirRecursive(const std::wstring& src, const std::wstring& dst) {
  EnsureDirRecursive(dst);
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(JoinPath(src, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
      continue;
    }
    const std::wstring s = JoinPath(src, fd.cFileName);
    const std::wstring d = JoinPath(dst, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      CopyDirRecursive(s, d);
    } else {
      CopyFileW(s.c_str(), d.c_str(), FALSE);
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

std::wstring LegacyNotesDir() { return JoinPath(NativeDataDir(), L"notes"); }

// Configured notes folder; else prefer 文档\DeskTidy\notes when present; else exe\笔记.
std::wstring ResolveNotesFolder(const AppPrefs& prefs) {
  if (!prefs.notepad_folder.empty()) {
    return prefs.notepad_folder;
  }
  const std::wstring docs_notes = DocumentsDeskTidyNotesDir();
  if (!docs_notes.empty() && DirExists(docs_notes)) {
    return docs_notes;
  }
  return JoinPath(ExeDir(), L"笔记");
}

// One-shot: copy legacy DeskTidyNative\notes into the configured folder.
void MaybeMigrateNotes(const std::wstring& new_dir, AppPrefs* prefs) {
  if (prefs->notes_migrated) {
    return;
  }
  if (DirHasEntries(new_dir)) {
    return;  // already has notes — treat as migrated
  }
  const std::wstring old_dir = LegacyNotesDir();
  if (_wcsicmp(old_dir.c_str(), new_dir.c_str()) == 0) {
    return;
  }
  if (!DirExists(old_dir) || !DirHasEntries(old_dir)) {
    return;
  }
  CopyDirRecursive(old_dir, new_dir);
  prefs->notes_migrated = true;
  SaveAppPrefs(*prefs);
  InvalidateNotesFileCache();
}

std::wstring PreviewOf(const std::wstring& line) {
  std::wstring p = line;
  size_t a = 0;
  while (a < p.size() && (p[a] == L' ' || p[a] == L'\t')) {
    ++a;
  }
  size_t b = p.size();
  while (b > a && (p[b - 1] == L'\r' || p[b - 1] == L'\n' || p[b - 1] == L' ' ||
                   p[b - 1] == L'\t')) {
    --b;
  }
  p = p.substr(a, b - a);
  if (p.size() > 120) {
    p = p.substr(0, 117) + L"…";
  }
  return p;
}

}  // namespace

std::vector<std::wstring> ListNoteFiles(const std::wstring& dir) {
  return ListNoteFilesCached(dir);
}

void InvalidateNotesFileCache() {
  std::lock_guard<std::mutex> lock(g_notes_files_mutex);
  g_notes_files_valid = false;
  g_notes_files.clear();
  g_notes_dir_stamps.clear();
}

std::wstring DocumentsDeskTidyNotesDir() {
  PWSTR docs = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) ||
      !docs) {
    return {};
  }
  std::wstring p = JoinPath(docs, L"DeskTidy\\notes");
  CoTaskMemFree(docs);
  return p;
}

std::wstring NotesLibraryDir(bool ensure) {
  // Load prefs once per call — ResolveNotesFolder + MaybeMigrateNotes both
  // used to call LoadAppPrefs() independently (2 disk reads + JSON parses).
  AppPrefs prefs = LoadAppPrefs();
  std::wstring dir = ResolveNotesFolder(prefs);
  if (ensure) {
    EnsureDirRecursive(dir);
    MaybeMigrateNotes(dir, &prefs);
  }
  return dir;
}

std::wstring PickNoteFromLibrary(HWND owner) {
  const std::wstring dir = NotesLibraryDir(true);
  return RunPickDialog(owner, dir, ListNoteFiles(dir), L"笔记库", true);
}

std::vector<NoteSearchHit> SearchNotesLibrary(const std::wstring& dir,
                                              const std::wstring& query,
                                              bool case_sensitive, int max_hits,
                                              int max_file_bytes) {
  std::vector<NoteSearchHit> hits;
  const std::wstring q = query;
  if (q.empty() || !DirExists(dir)) {
    return hits;
  }
  const std::wstring q_low = case_sensitive ? q : WideLower(q);
  for (const auto& path : ListNoteFiles(dir)) {
    if (static_cast<int>(hits.size()) >= max_hits) {
      break;
    }
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name =
        slash == std::wstring::npos ? path : path.substr(slash + 1);
    const std::wstring name_low = case_sensitive ? name : WideLower(name);
    if (WideContains(name_low, q_low)) {
      hits.push_back({path, -1, name});
      if (static_cast<int>(hits.size()) >= max_hits) {
        break;
      }
    }
    size_t size = 0;
    std::string data;
    if (!ReadFileBytes(path, &data, &size)) {
      continue;
    }
    if (size == 0 || size > static_cast<size_t>(max_file_bytes)) {
      continue;
    }
    std::wstring text;
    if (!DecodeBytes(data, &text) || HasNul(text)) {
      continue;  // binary / undecodable
    }
    // Split lines (LF or CRLF). For case-insensitive search, lowercase the
    // whole decoded text once (a single LCMapStringW) instead of allocating a
    // lowered copy per line. `text` keeps the original for the preview; `hay`
    // is what we match against (== text when case_sensitive).
    std::wstring text_low;
    if (!case_sensitive) {
      text_low = WideLower(text);
    }
    const std::wstring& hay = case_sensitive ? text : text_low;
    size_t i = 0;  // offset into `text`
    size_t j = 0;  // offset into `hay` (== i when case_sensitive)
    int line_no = 0;
    while (i <= text.size()) {
      size_t end = text.find(L'\n', i);
      if (end == std::wstring::npos) {
        end = text.size();
      }
      size_t jend = hay.find(L'\n', j);
      if (jend == std::wstring::npos) {
        jend = hay.size();
      }
      // Exclude a trailing '\r' from both the searched text and the preview.
      size_t text_end = end;
      if (text_end > i && text[text_end - 1] == L'\r') {
        --text_end;
      }
      size_t hay_end = jend;
      if (hay_end > j && hay[hay_end - 1] == L'\r') {
        --hay_end;
      }
      bool hit = false;
      if (hay_end > j) {
        const size_t p = hay.find(q_low, j);
        hit = (p != std::wstring::npos && p + q_low.size() <= hay_end);
      }
      if (hit) {
        hits.push_back(
            {path, line_no, PreviewOf(text.substr(i, text_end - i))});
        if (static_cast<int>(hits.size()) >= max_hits) {
          return hits;
        }
        break;  // one content hit per file
      }
      ++line_no;
      if (end >= text.size()) {
        break;
      }
      i = end + 1;
      j = jend + 1;
    }
  }
  return hits;
}

NoteSearchHit SearchNotesInLibrary(HWND owner) {
  const std::wstring query =
      PromptText(owner, L"搜索笔记", L"文件名或正文关键词：", L"");
  if (query.empty()) {
    return {};
  }
  const std::wstring dir = NotesLibraryDir(true);
  const AppPrefs prefs = LoadAppPrefs();
  bool case_sensitive = prefs.notes_search_case_sensitive;
  const int max_hits = prefs.notes_search_max_hits;
  const int max_file_bytes = prefs.notes_search_max_file_bytes;

  std::vector<NoteSearchHit> hits =
      SearchNotesLibrary(dir, query, case_sensitive, max_hits, max_file_bytes);

  NoteSearchHit picked;
  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.frameless = true;
  dlg.user_data_subdir = L"webview2-notes-search";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\pick_list.html");
  if (dlg.html_path.empty()) {
    MistAlert(owner, L"搜索笔记", L"找不到 pick_list.html", MistMsgTone::Warn);
    return {};
  }

  auto build = [&]() {
    std::wstring json = L"{\"type\":\"state\",\"title\":\"搜索结果\",\"hint\":\"";
    json += MistJsonEscape(query);
    json += L"\",\"open_label\":\"打开\",\"allow_new\":false,\"case_toggle\":true,\"case_on\":";
    json += case_sensitive ? L"true" : L"false";
    json += L",\"sel\":0,\"items\":[";
    for (size_t i = 0; i < hits.size(); ++i) {
      if (i) {
        json += L',';
      }
      const size_t slash = hits[i].path.find_last_of(L"\\/");
      json += L'"';
      json += MistJsonEscape(slash == std::wstring::npos
                                 ? hits[i].path
                                 : hits[i].path.substr(slash + 1));
      json += L'"';
    }
    json += L"],\"subs\":[";
    for (size_t i = 0; i < hits.size(); ++i) {
      if (i) {
        json += L',';
      }
      std::wstring sub = hits[i].line >= 0 ? L"行 " +
                                                  std::to_wstring(hits[i].line + 1) +
                                                  L"："
                                            : L"文件名匹配";
      if (!hits[i].preview.empty()) {
        sub += hits[i].preview;
      }
      json += L'"';
      json += MistJsonEscape(sub);
      json += L'"';
    }
    json += L"]}";
    return json;
  };

  dlg.build_state = build;
  dlg.on_message = [&](const std::wstring& json) -> bool {
    const std::wstring type = MistJsonStringField(json, L"type");
    if (type != L"action") {
      return false;
    }
    const std::wstring id = MistJsonStringField(json, L"id");
    if (id == L"ready") {
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"toggle_case") {
      case_sensitive = !case_sensitive;
      hits = SearchNotesLibrary(dir, query, case_sensitive, max_hits,
                                max_file_bytes);
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"open") {
      const int index = MistJsonIntField(json, L"index", -1);
      if (index >= 0 && index < static_cast<int>(hits.size())) {
        picked = hits[static_cast<size_t>(index)];
        return true;
      }
      return false;
    }
    if (id == L"cancel") {
      return true;
    }
    return false;
  };

  MistWebDialogRun(&dlg, L"搜索笔记", 560, 440);
  return picked;
}
