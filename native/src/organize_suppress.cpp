#include "organize_suppress.h"

#include "paths_util.h"

#include <Windows.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

std::mutex g_mu;
std::unordered_map<std::string, ULONGLONG> g_until;  // key -> GetTickCount64

std::string NormKey(const std::wstring& path) {
  std::wstring s = path;
  // Trim
  while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) {
    s.pop_back();
  }
  size_t i = 0;
  while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) {
    ++i;
  }
  s = s.substr(i);
  if (s.empty()) {
    return {};
  }
  for (wchar_t& c : s) {
    if (c == L'/') {
      c = L'\\';
    }
  }
  CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
  return NarrowUtf8(s);
}

void SweepLocked(ULONGLONG now) {
  for (auto it = g_until.begin(); it != g_until.end();) {
    if (it->second <= now) {
      it = g_until.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace

void SuppressOrganizePath(const std::wstring& path, double seconds) {
  const std::string key = NormKey(path);
  if (key.empty()) {
    return;
  }
  const ULONGLONG ms =
      static_cast<ULONGLONG>((std::max)(1.0, seconds) * 1000.0);
  const ULONGLONG until = GetTickCount64() + ms;
  std::lock_guard<std::mutex> lock(g_mu);
  g_until[key] = until;
}

void ClearOrganizeSuppress(const std::wstring& path) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (path.empty()) {
    g_until.clear();
    return;
  }
  const std::string key = NormKey(path);
  if (!key.empty()) {
    g_until.erase(key);
  }
}

bool IsOrganizeSuppressed(const std::wstring& path) {
  const std::string key = NormKey(path);
  if (key.empty()) {
    return false;
  }
  const ULONGLONG now = GetTickCount64();
  std::lock_guard<std::mutex> lock(g_mu);
  SweepLocked(now);
  const auto it = g_until.find(key);
  return it != g_until.end() && it->second > now;
}
