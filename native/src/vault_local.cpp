#include "vault_local.h"

#include "notes_library.h"
#include "paths_util.h"

#include <Windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace {

constexpr wchar_t kFileName[] = L"账号库.md";
constexpr char kMarker[] = "<!-- desktidy-vault-accounts -->";

std::wstring VaultDpapiPath() {
  return JoinPath(NativeDataDir(), L"vault.dpapi");
}

bool UnprotectBlob(const std::vector<BYTE>& enc, std::string* plain) {
  DATA_BLOB in{};
  in.pbData = const_cast<BYTE*>(enc.data());
  in.cbData = static_cast<DWORD>(enc.size());
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
    return false;
  }
  plain->assign(reinterpret_cast<char*>(out.pbData), out.cbData);
  LocalFree(out.pbData);
  return true;
}

std::wstring MaskPass(size_t n) {
  const size_t bullets = n == 0 ? 8u : (std::max)(size_t{4}, (std::min)(n, size_t{12}));
  return std::wstring(bullets, L'\u2022');  // •
}

std::wstring NormKey(std::wstring s) {
  while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) {
    s.pop_back();
  }
  size_t i = 0;
  while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) {
    ++i;
  }
  if (i) {
    s.erase(0, i);
  }
  for (wchar_t& c : s) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  return s;
}

}  // namespace

std::wstring VaultAccountsNoteFileName() { return kFileName; }

bool IsVaultAccountsNotePath(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring name =
      (slash == std::wstring::npos) ? path : path.substr(slash + 1);
  return _wcsicmp(name.c_str(), kFileName) == 0;
}

std::vector<VaultLocalEntry> LoadVaultLocalEntries() {
  std::vector<VaultLocalEntry> entries;
  std::ifstream in(VaultDpapiPath(), std::ios::binary);
  if (!in) {
    return entries;
  }
  std::vector<BYTE> enc((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  std::string plain;
  if (!UnprotectBlob(enc, &plain)) {
    return entries;
  }
  std::istringstream ss(plain);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) {
      continue;
    }
    VaultLocalEntry e;
    size_t p0 = 0;
    auto next = [&](std::wstring* field) {
      const size_t p1 = line.find('\t', p0);
      const std::string part =
          (p1 == std::string::npos) ? line.substr(p0) : line.substr(p0, p1 - p0);
      *field = WidenUtf8(part);
      p0 = (p1 == std::string::npos) ? line.size() : p1 + 1;
    };
    next(&e.title);
    next(&e.user);
    next(&e.pass);
    next(&e.url);
    if (p0 < line.size()) {
      next(&e.note);
    }
    if (p0 < line.size()) {
      std::wstring pinned;
      next(&pinned);
      e.pinned = (pinned == L"1" || pinned == L"true");
    }
    entries.push_back(std::move(e));
  }
  // Pinned first (stable).
  std::stable_sort(entries.begin(), entries.end(),
                   [](const VaultLocalEntry& a, const VaultLocalEntry& b) {
                     return a.pinned && !b.pinned;
                   });
  return entries;
}

std::string EscapeJsonUtf8Local(const std::wstring& ws) {
  const std::string s = NarrowUtf8(ws);
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\':
        o += "\\\\";
        break;
      case '"':
        o += "\\\"";
        break;
      case '\n':
        o += "\\n";
        break;
      case '\r':
        o += "\\r";
        break;
      case '\t':
        o += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          sprintf_s(buf, "\\u%04x", c);
          o += buf;
        } else {
          o.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return o;
}

std::wstring BuildVaultAccountsMarkdown(
    const std::vector<VaultLocalEntry>& entries) {
  std::wostringstream out;
  out << WidenUtf8(kMarker) << L"\n";
  out << L"# 账号库\n\n";
  out << L"> DeskTidy 账号功能同步（在 DeskNote 中以网页视图打开，可点复制）\n\n";
  if (entries.empty()) {
    out << L"暂无账号条目。在 DeskTidy「设置 → 账号管理」中添加后，"
           L"重新打开本笔记即可同步。\n";
    return out.str();
  }
  for (const auto& e : entries) {
    const std::wstring title = e.title.empty() ? L"（未命名）" : e.title;
    out << L"## " << title << L"\n\n";
    if (!e.user.empty()) {
      out << L"- 用户名: `" << e.user << L"`\n";
    }
    out << L"- 密码: `" << MaskPass(e.pass.size()) << L"`\n";
    if (!e.url.empty()) {
      out << L"- 网址: " << e.url << L"\n";
    }
    if (!e.note.empty()) {
      out << L"- 备注: " << e.note << L"\n";
    }
    out << L"\n";
  }
  return out.str();
}

std::wstring BuildVaultAccountsUiJson(
    const std::vector<VaultLocalEntry>& entries) {
  std::ostringstream out;
  out << '[';
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    if (i) {
      out << ',';
    }
    out << "{\"title\":\""
        << EscapeJsonUtf8Local(e.title.empty() ? L"（未命名）" : e.title)
        << "\",\"user\":\"" << EscapeJsonUtf8Local(e.user)
        << "\",\"passMask\":\"" << EscapeJsonUtf8Local(MaskPass(e.pass.size()))
        << "\",\"url\":\"" << EscapeJsonUtf8Local(e.url) << "\",\"note\":\""
        << EscapeJsonUtf8Local(e.note) << "\"}";
  }
  out << ']';
  return WidenUtf8(out.str());
}

std::wstring BuildVaultAccountsUiJson() {
  return BuildVaultAccountsUiJson(LoadVaultLocalEntries());
}

std::wstring SyncVaultAccountsNote(bool vault_enabled) {
  const std::wstring dir = NotesLibraryDir(true);
  if (dir.empty()) {
    return {};
  }
  const std::wstring path = JoinPath(dir, kFileName);
  if (!vault_enabled) {
    return {};
  }
  const auto entries = LoadVaultLocalEntries();
  const std::wstring md = BuildVaultAccountsMarkdown(entries);
  if (!AtomicWriteUtf8File(path, NarrowUtf8(md), false)) {
    return {};
  }
  InvalidateNotesFileCache();
  return path;
}

std::wstring FindVaultLocalPassword(const std::wstring& title,
                                    const std::wstring& user) {
  const std::wstring want_title = NormKey(title);
  const std::wstring want_user = NormKey(user);
  if (want_title.empty()) {
    return {};
  }
  const auto entries = LoadVaultLocalEntries();
  const VaultLocalEntry* fallback = nullptr;
  for (const auto& e : entries) {
    const std::wstring et = NormKey(e.title.empty() ? L"（未命名）" : e.title);
    if (et != want_title) {
      continue;
    }
    if (want_user.empty() || NormKey(e.user) == want_user) {
      return e.pass;
    }
    if (!fallback) {
      fallback = &e;
    }
  }
  return fallback ? fallback->pass : std::wstring{};
}

std::wstring FindVaultLocalField(int index, const std::wstring& field) {
  if (index < 0 || field.empty()) {
    return {};
  }
  const auto entries = LoadVaultLocalEntries();
  if (index >= static_cast<int>(entries.size())) {
    return {};
  }
  const auto& e = entries[static_cast<size_t>(index)];
  if (field == L"user" || field == L"username") {
    return e.user;
  }
  if (field == L"pass" || field == L"password") {
    return e.pass;
  }
  if (field == L"url") {
    return e.url;
  }
  if (field == L"note") {
    return e.note;
  }
  return {};
}
