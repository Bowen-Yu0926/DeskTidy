#include "whitelist.h"

#include "mist_msg.h"
#include "mist_webview.h"
#include "paths_util.h"

#include <Windows.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::wstring WhitelistFile() {
  return JoinPath(NativeDataDir(), L"whitelist.json");
}

std::wstring BaseName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

// Locale-invariant Unicode lowercase. Approximates Python str.casefold; Windows
// exposes no full casefold (no ß→ss), so that remains a known residual gap.
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

// ---------------------------------------------------------------------------
// Case-sensitive fnmatch (Python fnmatch.fnmatchcase). Supports *, ?, [...],
// ranges (a-z), negation ([!...]) and multiple '*'. Callers pre-fold inputs.
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
        return false;
      }
      ++p;
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

bool HasWildcard(const std::wstring& s) {
  return s.find_first_of(L"*?[") != std::wstring::npos;
}

// Absolute, slash-normalized, case-folded path (Python
// Path(p).resolve().as_posix().casefold()) for full-path whitelist equality.
std::wstring CanonicalPosix(const std::wstring& s) {
  wchar_t buf[32768];
  const DWORD n = GetFullPathNameW(s.c_str(), 32768, buf, nullptr);
  if (n == 0 || n >= 32768) {
    return {};
  }
  std::wstring out(buf, n);
  for (auto& ch : out) {
    if (ch == L'\\') {
      ch = L'/';
    }
  }
  return FoldLower(out);
}

// Match one path against one pattern. Mirrors Python is_organize_whitelisted:
//   wildcard (*?[) -> fnmatch against the basename only (no full-path glob)
//   exact          -> basename equality, or resolved full-path equality
// No directory-prefix matching (Python has none).
bool MatchOne(const std::wstring& path, const std::wstring& pattern) {
  if (pattern.empty()) {
    return false;
  }
  const std::wstring name = BaseName(path);
  const std::wstring name_f = FoldLower(name);
  const std::wstring pat_f = FoldLower(pattern);
  if (HasWildcard(pattern)) {
    return FnmatchImpl(name_f.c_str(), pat_f.c_str());
  }
  if (name_f == pat_f) {
    return true;
  }
  const std::wstring path_cf = CanonicalPosix(path);
  const std::wstring pat_cf = CanonicalPosix(pattern);
  return !path_cf.empty() && path_cf == pat_cf;
}

void Dedup(std::vector<std::wstring>* items) {
  std::vector<std::wstring> out;
  std::vector<std::wstring> keys;
  for (const auto& it : *items) {
    const std::wstring key = FoldLower(it);
    bool dup = false;
    for (const auto& k : keys) {
      if (k == key) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      keys.push_back(key);
      out.push_back(it);
    }
  }
  *items = std::move(out);
}

struct Ctx {
  std::vector<std::wstring> names;
  bool saved = false;
};

std::wstring BuildState(Ctx* ctx) {
  std::wstring json = L"{\"type\":\"state\",\"items\":[";
  for (size_t i = 0; i < ctx->names.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += MistJsonEscape(ctx->names[i]);
    json += L'"';
  }
  json += L"]}";
  return json;
}

}  // namespace

std::vector<std::wstring> LoadWhitelist() {
  // Short cache: icon RMB used to re-read+parse JSON every click.
  static std::vector<std::wstring> cached;
  static FILETIME cached_write{};
  static bool have_cache = false;
  const std::wstring path = WhitelistFile();
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  FILETIME write{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
    write = fad.ftLastWriteTime;
    if (have_cache && CompareFileTime(&write, &cached_write) == 0) {
      return cached;
    }
  } else if (have_cache && cached_write.dwLowDateTime == 0 &&
             cached_write.dwHighDateTime == 0) {
    return cached;
  }

  std::vector<std::wstring> out;
  std::ifstream in(path);
  if (!in) {
    cached.clear();
    cached_write = {};
    have_cache = true;
    return out;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  size_t pos = 0;
  while (true) {
    const size_t q1 = json.find('"', pos);
    if (q1 == std::string::npos) {
      break;
    }
    size_t q2 = q1 + 1;
    std::string val;
    while (q2 < json.size() && json[q2] != '"') {
      if (json[q2] == '\\' && q2 + 1 < json.size()) {
        ++q2;
      }
      val.push_back(json[q2++]);
    }
    if (q2 >= json.size()) {
      break;
    }
    pos = q2 + 1;
    // Skip object keys: native "names" and Python "organize_whitelist".
    if (val == "names" || val == "organize_whitelist") {
      continue;
    }
    if (!val.empty()) {
      out.push_back(WidenUtf8(val));
    }
  }
  // Strip whitespace and drop empties (Python get_organize_whitelist), then
  // case-fold dedup.
  std::vector<std::wstring> cleaned;
  for (auto& item : out) {
    size_t a = 0;
    size_t b = item.size();
    while (a < b && (item[a] == L' ' || item[a] == L'\t' || item[a] == L'\r' ||
                     item[a] == L'\n')) {
      ++a;
    }
    while (b > a && (item[b - 1] == L' ' || item[b - 1] == L'\t' ||
                     item[b - 1] == L'\r' || item[b - 1] == L'\n')) {
      --b;
    }
    if (a < b) {
      cleaned.push_back(item.substr(a, b - a));
    }
  }
  Dedup(&cleaned);
  cached = cleaned;
  cached_write = write;
  have_cache = true;
  return cleaned;
}

bool SaveWhitelist(const std::vector<std::wstring>& names) {
  std::vector<std::wstring> dedup = names;
  Dedup(&dedup);
  std::ostringstream out;
  out << "{\n  \"names\": [\n";
  for (size_t i = 0; i < dedup.size(); ++i) {
    out << "    \"" << EscapeJsonUtf8(NarrowUtf8(dedup[i])) << "\"";
    if (i + 1 < dedup.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
  const bool ok = AtomicWriteUtf8File(WhitelistFile(), out.str());
  // Bust LoadWhitelist cache after write.
  if (ok) {
    LoadWhitelist();
  }
  return ok;
}

bool IsWhitelisted(const std::wstring& path_or_name,
                   const std::vector<std::wstring>& names) {
  for (const auto& p : names) {
    if (MatchOne(path_or_name, p)) {
      return true;
    }
  }
  return false;
}

void EditWhitelistDialog(HWND owner) {
  Ctx ctx;
  ctx.names = LoadWhitelist();

  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.user = &ctx;
  dlg.user_data_subdir = L"webview2-whitelist";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\whitelist.html");
  if (dlg.html_path.empty()) {
    MistAlert(owner, L"整理白名单", L"找不到 whitelist.html", MistMsgTone::Warn);
    return;
  }
  dlg.build_state = [&ctx]() { return BuildState(&ctx); };
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
    if (id == L"add") {
      const std::wstring value = MistJsonStringField(json, L"value");
      if (!value.empty()) {
        ctx.names.push_back(value);
        Dedup(&ctx.names);
        MistWebDialogPushState(&dlg);
      }
      return false;
    }
    if (id == L"edit") {
      const int index = MistJsonIntField(json, L"index", -1);
      const std::wstring value = MistJsonStringField(json, L"value");
      if (index >= 0 && index < static_cast<int>(ctx.names.size()) &&
          !value.empty()) {
        ctx.names[static_cast<size_t>(index)] = value;
        Dedup(&ctx.names);
        MistWebDialogPushState(&dlg);
      }
      return false;
    }
    if (id == L"del") {
      const int index = MistJsonIntField(json, L"index", -1);
      if (index >= 0 && index < static_cast<int>(ctx.names.size())) {
        ctx.names.erase(ctx.names.begin() + index);
        MistWebDialogPushState(&dlg);
      }
      return false;
    }
    if (id == L"save") {
      SaveWhitelist(ctx.names);
      ctx.saved = true;
      return true;
    }
    if (id == L"cancel") {
      return true;
    }
    return false;
  };

  MistWebDialogRun(&dlg, L"整理白名单", 640, 500);
}
