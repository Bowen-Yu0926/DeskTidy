#include "rule_store.h"

#include "paths_util.h"
#include "pin_store.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::string Narrow(const std::wstring& ws) {
  if (ws.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), n, nullptr,
                        nullptr);
  }
  return out;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  }
  return out;
}

std::string EscapeJson(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

// Locale-invariant Unicode lowercase (Python str.lower). LCMapStringW covers
// non-ASCII letters that a naive A-Z fold would miss.
std::wstring FoldLower(const std::wstring& s) {
  if (s.empty()) {
    return {};
  }
  const int n = LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, s.c_str(),
                             static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) {
    return s;
  }
  std::wstring out(n, L'\0');
  LCMapStringW(LOCALE_INVARIANT, LCMAP_LOWERCASE, s.c_str(),
               static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::wstring BaseNameOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

// ---------------------------------------------------------------------------
// Case-sensitive fnmatch (Python fnmatch.fnmatchcase). Supports *, ? and [...]
// classes incl. ranges (a-z) and negation ([!...]). Callers pre-fold both
// strings so matching is case-insensitive without per-char locale surprises.
// ---------------------------------------------------------------------------
bool FnmatchImpl(const wchar_t* t, const wchar_t* p) {
  while (*p) {
    if (*p == L'*') {
      do {
        ++p;
      } while (*p == L'*');
      if (!*p) {
        return true;
      }
      while (*t) {
        if (FnmatchImpl(t, p)) {
          return true;
        }
        ++t;
      }
      return FnmatchImpl(t, p);
    }
    if (*p == L'?') {
      if (!*t) {
        return false;
      }
      ++t;
      ++p;
      continue;
    }
    if (*p == L'[') {
      if (!*t) {
        return false;
      }
      ++p;
      bool negate = false;
      if (*p == L'!' || *p == L'^') {
        negate = true;
        ++p;
      }
      bool matched = false;
      const wchar_t c = *t;
      // A leading ']' is a literal.
      if (*p == L']') {
        if (c == L']') {
          matched = true;
        }
        ++p;
      }
      while (*p && *p != L']') {
        if (p[1] == L'-' && p[2] && p[2] != L']') {
          const wchar_t lo = *p;
          const wchar_t hi = p[2];
          if (c >= lo && c <= hi) {
            matched = true;
          }
          p += 3;
        } else {
          if (*p == c) {
            matched = true;
          }
          ++p;
        }
      }
      if (*p != L']') {
        return false;  // unterminated class
      }
      ++p;  // past ']'
      if (matched == negate) {
        return false;
      }
      ++t;
      continue;
    }
    if (!*t) {
      return false;
    }
    if (*t != *p) {
      return false;
    }
    ++t;
    ++p;
  }
  return *t == 0;
}

bool FnmatchFolded(const std::wstring& text, const std::wstring& pattern) {
  const std::wstring ft = FoldLower(text);
  const std::wstring fp = FoldLower(pattern);
  return FnmatchImpl(ft.c_str(), fp.c_str());
}

// Legacy kind: extension tables (Python PRESET_EXTENSION_GROUPS). Used only to
// expand deprecated `kind:X` keys on load; matching never classifies by kind.
const wchar_t* const* KindExtensions(const std::wstring& kind) {
  static const wchar_t* image[] = {L".jpg", L".jpeg", L".png", L".gif", L".bmp",
                                   L".svg", L".webp", L".ico", L".tiff", nullptr};
  static const wchar_t* doc[] = {L".doc", L".docx", L".pdf", L".txt", L".xls",
                                 L".xlsx", L".ppt", L".pptx", L".md", L".rtf",
                                 nullptr};
  static const wchar_t* video[] = {L".mp4", L".avi", L".mkv", L".mov",
                                   L".wmv", L".flv", L".webm", nullptr};
  static const wchar_t* audio[] = {L".mp3", L".wav", L".flac", L".aac",
                                   L".ogg", L".wma", nullptr};
  static const wchar_t* archive[] = {L".zip", L".rar", L".7z", L".tar",
                                     L".gz", L".bz2", nullptr};
  if (kind == L"image") {
    return image;
  }
  if (kind == L"doc") {
    return doc;
  }
  if (kind == L"video") {
    return video;
  }
  if (kind == L"audio") {
    return audio;
  }
  if (kind == L"archive") {
    return archive;
  }
  return nullptr;
}

// An extension rule key is a leading-dot token with no wildcards (".pdf").
// Everything else is a filename rule (glob / plain name / "*").
bool IsExtensionKey(const std::wstring& key) {
  return !key.empty() && key[0] == L'.' &&
         key.find_first_of(L"*?[") == std::wstring::npos;
}

// Map a single loaded key to one or more rule entries. Legacy first-class
// syntax is translated to the current rule model (compat, no data loss):
//   name:=X / name:X -> X (filename rule: fnmatch or substring)
//   kind:other        -> "*"
//   kind:<k>          -> each extension in that kind's group
void InsertNormalizedRule(RuleMap* rules, const std::wstring& key,
                          const std::string& val) {
  if (key.rfind(L"name:=", 0) == 0) {
    const std::wstring rest = key.substr(6);
    if (!rest.empty()) {
      (*rules)[rest] = val;
    }
    return;
  }
  if (key.rfind(L"name:", 0) == 0) {
    const std::wstring rest = key.substr(5);
    if (!rest.empty()) {
      (*rules)[rest] = val;
    }
    return;
  }
  if (key.rfind(L"kind:", 0) == 0) {
    const std::wstring kind = key.substr(5);
    if (kind == L"other") {
      (*rules)[L"*"] = val;
      return;
    }
    const wchar_t* const* exts = KindExtensions(kind);
    if (exts != nullptr) {
      for (int i = 0; exts[i]; ++i) {
        (*rules)[exts[i]] = val;
      }
    }
    return;
  }
  (*rules)[key] = val;
}

}  // namespace

std::wstring ExtOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
    return L"";
  }
  std::wstring ext = path.substr(dot);
  for (auto& ch : ext) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  return ext;
}

std::wstring RuleFilePath() {
  // U4 portable + single source of truth: route through NativeDataDir() so
  // rules.json follows the resolved data root (portable.txt / --portable).
  return JoinPath(NativeDataDir(), L"rules.json");
}

RuleMap DefaultRules() {
  return {
      {L".lnk", "system_software"},
      {L".exe", "system_software"},
      {L".url", "system_software"},
      {L".pdf", "system_common"},
      {L".docx", "system_common"},
      {L".doc", "system_common"},
      {L".xlsx", "system_common"},
      {L".pptx", "system_common"},
      {L".txt", "system_common"},
      {L".md", "system_common"},
      {L".png", "system_common"},
      {L".jpg", "system_common"},
      {L".jpeg", "system_common"},
      {L".zip", "system_common"},
      {L"*", "system_common"},
  };
}

RuleMap LoadRules() {
  std::ifstream in(RuleFilePath());
  if (!in) {
    return DefaultRules();
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  RuleMap rules;
  size_t pos = json.find("\"rules\"");
  if (pos == std::string::npos) {
    return DefaultRules();
  }
  pos = json.find('{', pos);
  if (pos == std::string::npos) {
    return DefaultRules();
  }
  ++pos;
  while (pos < json.size()) {
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' ||
            json[pos] == '\t' || json[pos] == ',')) {
      ++pos;
    }
    if (pos >= json.size() || json[pos] == '}') {
      break;
    }
    if (json[pos] != '"') {
      break;
    }
    ++pos;
    std::string key;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        ++pos;
      }
      key.push_back(json[pos++]);
    }
    if (pos >= json.size()) {
      break;
    }
    ++pos;
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
      break;
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
      break;
    }
    ++pos;
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        ++pos;
      }
      val.push_back(json[pos++]);
    }
    if (!key.empty() && !val.empty()) {
      InsertNormalizedRule(&rules, Widen(key), val);
    }
    if (pos < json.size() && json[pos] == '"') {
      ++pos;
    }
  }
  if (rules.empty()) {
    return DefaultRules();
  }
  return rules;
}

bool SaveRules(const RuleMap& rules) {
  std::ofstream out(RuleFilePath(), std::ios::binary);
  if (!out) {
    return false;
  }
  out << "{\n  \"rules\": {\n";
  size_t i = 0;
  for (const auto& kv : rules) {
    out << "    \"" << EscapeJson(Narrow(kv.first)) << "\":\""
        << EscapeJson(kv.second) << "\"";
    if (++i < rules.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  }\n}\n";
  return true;
}

std::string ValidateRule(const std::wstring& pattern, const std::string& fence_id) {
  if (fence_id.empty()) {
    return "分区 id 为空";
  }
  if (pattern.empty()) {
    return "规则为空";
  }
  if (pattern == L"*") {
    return "";
  }
  if (pattern.rfind(L"name:=", 0) == 0 || pattern.rfind(L"name:", 0) == 0) {
    return "name:/name:= 已废弃：请直接填写文件名或通配符（如 报告、*.pdf）";
  }
  if (pattern.rfind(L"kind:", 0) == 0) {
    return "kind: 已废弃：请改用扩展名规则（如 .pdf）";
  }
  // Glob / extension: reject unterminated character classes.
  int depth = 0;
  for (wchar_t ch : pattern) {
    if (ch == L'[') {
      ++depth;
    } else if (ch == L']') {
      --depth;
    }
    if (depth < 0) {
      return "字符类缺少 '['";
    }
  }
  if (depth != 0) {
    return "字符类缺少 ']'";
  }
  return "";
}

std::string ResolveFenceId(const std::wstring& path, bool is_link,
                           const std::unordered_map<std::wstring, std::string>& pins,
                           const RuleMap& rules,
                           const std::string& fallback_software,
                           const std::string& fallback_common) {
  const auto pit = pins.find(NormalizePinKey(path));
  if (pit != pins.end()) {
    return pit->second;
  }
  const std::wstring name = BaseNameOf(path);
  const std::wstring ext = is_link ? L".lnk" : ExtOf(path);

  // Pass 1: extension exact, case-insensitive (Python match_extension —
  // extension priority; rule extensions are compared after str.lower()).
  {
    const std::wstring want_ext = FoldLower(ext);
    for (const auto& kv : rules) {
      if (IsExtensionKey(kv.first) && FoldLower(kv.first) == want_ext) {
        return kv.second;
      }
    }
  }
  // Pass 2: filename rules — fnmatch OR substring (Python match_filename).
  const std::wstring lower_name = FoldLower(name);
  for (const auto& kv : rules) {
    if (IsExtensionKey(kv.first)) {
      continue;
    }
    const std::wstring pat = FoldLower(kv.first);
    if (pat.empty()) {
      continue;
    }
    if (FnmatchImpl(lower_name.c_str(), pat.c_str()) ||
        lower_name.find(pat) != std::wstring::npos) {
      return kv.second;
    }
  }
  return is_link ? fallback_software : fallback_common;
}

// ---------------------------------------------------------------------------
// U1 organize categories (filename_rules / category_fence_map). Persisted in a
// dedicated organize_categories.json so the pattern->fence `rules` schema and
// these category maps never clobber each other.
// ---------------------------------------------------------------------------
namespace {

std::wstring CategoryFilePath() {
  return JoinPath(NativeDataDir(), L"organize_categories.json");
}

// Extract the `{...}` object that follows a top-level `"key":` in `json`.
bool ExtractObject(const std::string& json, const char* key, std::string* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) {
    return false;
  }
  pos = json.find('{', pos);
  if (pos == std::string::npos) {
    return false;
  }
  int depth = 0;
  size_t i = pos;
  for (; i < json.size(); ++i) {
    const char c = json[i];
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) {
        ++i;
        break;
      }
    }
  }
  if (depth != 0) {
    return false;
  }
  out->assign(json, pos, i - pos);
  return true;
}

CategoryFenceMap ParseStrStrObject(const std::string& obj) {
  CategoryFenceMap out;
  size_t pos = 0;
  while (true) {
    const size_t kq = obj.find('"', pos);
    if (kq == std::string::npos) {
      break;
    }
    size_t i = kq + 1;
    std::string key;
    while (i < obj.size() && obj[i] != '"') {
      if (obj[i] == '\\' && i + 1 < obj.size()) {
        ++i;
      }
      key.push_back(obj[i++]);
    }
    const size_t colon = obj.find(':', i);
    if (colon == std::string::npos) {
      break;
    }
    const size_t vq = obj.find('"', colon + 1);
    if (vq == std::string::npos) {
      break;
    }
    i = vq + 1;
    std::string val;
    while (i < obj.size() && obj[i] != '"') {
      if (obj[i] == '\\' && i + 1 < obj.size()) {
        ++i;
      }
      val.push_back(obj[i++]);
    }
    if (!key.empty() && !val.empty()) {
      out[key] = val;
    }
    pos = i + 1;
  }
  return out;
}

FilenameRulesMap ParseStrListObject(const std::string& obj) {
  FilenameRulesMap out;
  size_t pos = 0;
  while (true) {
    const size_t kq = obj.find('"', pos);
    if (kq == std::string::npos) {
      break;
    }
    size_t i = kq + 1;
    std::string key;
    while (i < obj.size() && obj[i] != '"') {
      if (obj[i] == '\\' && i + 1 < obj.size()) {
        ++i;
      }
      key.push_back(obj[i++]);
    }
    const size_t colon = obj.find(':', i);
    if (colon == std::string::npos) {
      break;
    }
    const size_t lb = obj.find('[', colon + 1);
    if (lb == std::string::npos) {
      break;
    }
    const size_t rb = obj.find(']', lb);
    if (rb == std::string::npos) {
      break;
    }
    const std::string arr = obj.substr(lb + 1, rb - lb - 1);
    std::vector<std::wstring> pats;
    size_t q = 0;
    while (true) {
      const size_t a = arr.find('"', q);
      if (a == std::string::npos) {
        break;
      }
      size_t j = a + 1;
      std::string val;
      while (j < arr.size() && arr[j] != '"') {
        if (arr[j] == '\\' && j + 1 < arr.size()) {
          ++j;
        }
        val.push_back(arr[j++]);
      }
      if (!val.empty()) {
        pats.push_back(Widen(val));
      }
      q = j + 1;
    }
    if (!key.empty()) {
      out[key] = pats;
    }
    pos = rb + 1;
  }
  return out;
}

bool AtomicWriteCategories(const std::string& json) {
  const std::wstring path = CategoryFilePath();
  const std::wstring tmp = path + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(h, json.data(), static_cast<DWORD>(json.size()),
                            &written, nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok || written != json.size()) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
      !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
      (fad.nFileSizeHigh != 0 || fad.nFileSizeLow != 0)) {
    CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

void LoadCategories(CategoryFenceMap* fence_map,
                    FilenameRulesMap* filename_rules) {
  fence_map->clear();
  filename_rules->clear();
  std::ifstream in(CategoryFilePath());
  if (!in) {
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  std::string obj;
  if (ExtractObject(json, "category_fence_map", &obj)) {
    *fence_map = ParseStrStrObject(obj);
  }
  if (ExtractObject(json, "filename_rules", &obj)) {
    *filename_rules = ParseStrListObject(obj);
  }
}

void SaveCategories(const CategoryFenceMap& fence_map,
                    const FilenameRulesMap& filename_rules) {
  std::ostringstream out;
  out << "{\n  \"category_fence_map\": {\n";
  size_t i = 0;
  for (const auto& kv : fence_map) {
    out << "    \"" << EscapeJson(kv.first) << "\":\"" << EscapeJson(kv.second)
        << "\"";
    if (++i < fence_map.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  },\n  \"filename_rules\": {\n";
  i = 0;
  for (const auto& kv : filename_rules) {
    out << "    \"" << EscapeJson(kv.first) << "\":[";
    for (size_t j = 0; j < kv.second.size(); ++j) {
      if (j) {
        out << ",";
      }
      out << "\"" << EscapeJson(Narrow(kv.second[j])) << "\"";
    }
    out << "]";
    if (++i < filename_rules.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  }\n}\n";
  AtomicWriteCategories(out.str());
}

}  // namespace

CategoryFenceMap LoadCategoryFenceMap() {
  CategoryFenceMap fence_map;
  FilenameRulesMap filename_rules;
  LoadCategories(&fence_map, &filename_rules);
  return fence_map;
}

FilenameRulesMap LoadFilenameRules() {
  CategoryFenceMap fence_map;
  FilenameRulesMap filename_rules;
  LoadCategories(&fence_map, &filename_rules);
  return filename_rules;
}

bool SaveCategoryFenceMap(const CategoryFenceMap& map) {
  CategoryFenceMap fence_map = map;
  FilenameRulesMap filename_rules = LoadFilenameRules();
  SaveCategories(fence_map, filename_rules);
  return true;
}

bool SaveFilenameRules(const FilenameRulesMap& map) {
  CategoryFenceMap fence_map = LoadCategoryFenceMap();
  FilenameRulesMap filename_rules = map;
  SaveCategories(fence_map, filename_rules);
  return true;
}

// ---------------------------------------------------------------------------
// U1 editable single-line forms for the settings UI.
// ---------------------------------------------------------------------------
namespace {
std::vector<std::wstring> SplitBy(const std::wstring& s, wchar_t sep) {
  std::vector<std::wstring> out;
  size_t start = 0;
  while (start <= s.size()) {
    const size_t pos = s.find(sep, start);
    const size_t end = pos == std::wstring::npos ? s.size() : pos;
    std::wstring part = s.substr(start, end - start);
    size_t a = part.find_first_not_of(L" \t");
    size_t b = part.find_last_not_of(L" \t");
    if (a != std::wstring::npos) {
      part = part.substr(a, b - a + 1);
    } else {
      part.clear();
    }
    if (!part.empty()) {
      out.push_back(part);
    }
    if (pos == std::wstring::npos) {
      break;
    }
    start = pos + 1;
  }
  return out;
}
}  // namespace

std::wstring SerializeCategoryFenceMap(const CategoryFenceMap& map) {
  std::wstring out;
  for (const auto& kv : map) {
    if (!out.empty()) {
      out += L"; ";
    }
    out += Widen(kv.first) + L"=" + Widen(kv.second);
  }
  return out;
}

CategoryFenceMap ParseCategoryFenceMapText(const std::wstring& text) {
  CategoryFenceMap out;
  for (const auto& part : SplitBy(text, L';')) {
    const size_t eq = part.find(L'=');
    if (eq == std::wstring::npos) {
      continue;
    }
    const std::wstring cat = part.substr(0, eq);
    const std::wstring fence = part.substr(eq + 1);
    if (!cat.empty() && !fence.empty()) {
      out[Narrow(cat)] = Narrow(fence);
    }
  }
  return out;
}

std::wstring SerializeFilenameRules(const FilenameRulesMap& map) {
  std::wstring out;
  for (const auto& kv : map) {
    if (!out.empty()) {
      out += L"; ";
    }
    out += Widen(kv.first) + L"=";
    for (size_t i = 0; i < kv.second.size(); ++i) {
      if (i) {
        out += L",";
      }
      out += kv.second[i];
    }
  }
  return out;
}

FilenameRulesMap ParseFilenameRulesText(const std::wstring& text) {
  FilenameRulesMap out;
  for (const auto& part : SplitBy(text, L';')) {
    const size_t eq = part.find(L'=');
    if (eq == std::wstring::npos) {
      continue;
    }
    const std::wstring cat = part.substr(0, eq);
    std::vector<std::wstring> pats = SplitBy(part.substr(eq + 1), L',');
    if (!cat.empty() && !pats.empty()) {
      out[Narrow(cat)] = pats;
    }
  }
  return out;
}
