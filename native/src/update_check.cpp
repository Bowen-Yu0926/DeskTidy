#include "update_check.h"

#include "paths_util.h"

#include <Shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "version.lib")

namespace {

constexpr wchar_t kHost[] = L"api.github.com";
constexpr wchar_t kPath[] =
    L"/repos/Bowen-Yu0926/DeskTidy-Releases/releases/latest";
constexpr wchar_t kReleasesPage[] =
    L"https://github.com/Bowen-Yu0926/DeskTidy-Releases/releases/latest";

std::vector<int> ParseVersion(const std::wstring& raw) {
  std::wstring t = raw;
  while (!t.empty() && (t.front() == L'v' || t.front() == L'V')) {
    t.erase(t.begin());
  }
  const size_t dash = t.find(L'-');
  if (dash != std::wstring::npos) {
    t.resize(dash);
  }
  const size_t plus = t.find(L'+');
  if (plus != std::wstring::npos) {
    t.resize(plus);
  }
  std::vector<int> parts;
  size_t i = 0;
  while (i < t.size()) {
    while (i < t.size() && t[i] == L'.') {
      ++i;
    }
    if (i >= t.size()) {
      break;
    }
    int n = 0;
    bool any = false;
    while (i < t.size() && t[i] >= L'0' && t[i] <= L'9') {
      n = n * 10 + (t[i] - L'0');
      any = true;
      ++i;
    }
    if (any) {
      parts.push_back(n);
    } else {
      ++i;
    }
  }
  if (parts.empty()) {
    parts.push_back(0);
  }
  return parts;
}

bool IsNewer(const std::wstring& latest, const std::wstring& current) {
  const auto a = ParseVersion(latest);
  const auto b = ParseVersion(current);
  const size_t n = (std::max)(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const int av = i < a.size() ? a[i] : 0;
    const int bv = i < b.size() ? b[i] : 0;
    if (av != bv) {
      return av > bv;
    }
  }
  return false;
}

bool HttpGetJson(std::string* body, std::wstring* err) {
  HINTERNET session =
      WinHttpOpen(L"DeskTidyNative-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *err = L"WinHttpOpen failed";
    return false;
  }
  WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
  HINTERNET conn = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (!conn) {
    *err = L"connect failed";
    WinHttpCloseHandle(session);
    return false;
  }
  HINTERNET req =
      WinHttpOpenRequest(conn, L"GET", kPath, nullptr, WINHTTP_NO_REFERER,
                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
  if (!req) {
    *err = L"open request failed";
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return false;
  }
  WinHttpAddRequestHeaders(
      req,
      L"Accept: application/vnd.github+json\r\nUser-Agent: DeskTidyNative-Updater\r\n",
      static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
  if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(req, nullptr)) {
    *err = L"request failed";
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return false;
  }
  DWORD status = 0;
  DWORD status_sz = sizeof(status);
  WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz,
                      WINHTTP_NO_HEADER_INDEX);
  std::string out;
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) {
      break;
    }
    std::string chunk(avail, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(req, chunk.data(), avail, &read)) {
      break;
    }
    chunk.resize(read);
    out += chunk;
  }
  WinHttpCloseHandle(req);
  WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  if (status < 200 || status >= 300) {
    *err = L"HTTP " + std::to_wstring(status);
    return false;
  }
  *body = std::move(out);
  return true;
}

std::string JsonStringField(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return {};
  }
  p = json.find('"', json.find(':', p) + 1);
  if (p == std::string::npos) {
    return {};
  }
  ++p;
  std::string v;
  while (p < json.size() && json[p] != '"') {
    if (json[p] == '\\' && p + 1 < json.size()) {
      ++p;
    }
    v.push_back(json[p++]);
  }
  return v;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                    static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      out.data(), n);
  return out;
}

}  // namespace

std::wstring NativeProductVersion() {
  wchar_t path[MAX_PATH] = {};
  if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
    return L"0.0.0";
  }
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(path, &handle);
  if (!size) {
    return L"4.0.30";
  }
  std::vector<BYTE> buf(size);
  if (!GetFileVersionInfoW(path, 0, size, buf.data())) {
    return L"4.0.30";
  }
  VS_FIXEDFILEINFO* fi = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) ||
      !fi) {
    return L"4.0.30";
  }
  wchar_t ver[64];
  swprintf_s(ver, L"%u.%u.%u", HIWORD(fi->dwFileVersionMS),
             LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
  return ver;
}

ULONGLONG NowFileTime() {
  FILETIME ft{};
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u{};
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return u.QuadPart;
}

bool LoadUpdateCache(std::wstring* latest, std::wstring* html, bool* fresh) {
  *fresh = false;
  std::ifstream in(JoinPath(NativeDataDir(), L"update_check_cache.json"));
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  const std::string tag = JsonStringField(json, "tag");
  const std::string url = JsonStringField(json, "html");
  if (tag.empty()) {
    return false;
  }
  *latest = Widen(tag);
  *html = Widen(url);
  const auto p = json.find("\"ticks\"");
  if (p != std::string::npos) {
    const auto c = json.find(':', p);
    if (c != std::string::npos) {
      const unsigned long long ticks = std::strtoull(json.c_str() + c + 1, nullptr, 10);
      const unsigned long long ttl = 6ULL * 3600ULL * 10000000ULL;
      *fresh = NowFileTime() < ticks + ttl;
    }
  }
  return true;
}

void SaveUpdateCache(const std::wstring& latest, const std::wstring& html) {
  std::ofstream out(JoinPath(NativeDataDir(), L"update_check_cache.json"),
                    std::ios::binary);
  if (!out) {
    return;
  }
  out << "{\n  \"ticks\": " << NowFileTime() << ",\n  \"tag\": \""
      << NarrowUtf8(latest) << "\",\n  \"html\": \"" << NarrowUtf8(html)
      << "\"\n}\n";
}

void CheckForUpdates(HWND owner) {
  const std::wstring current = NativeProductVersion();
  std::wstring latest;
  std::wstring html;
  bool fresh = false;
  if (LoadUpdateCache(&latest, &html, &fresh) && fresh && !latest.empty()) {
    // Use cached release within 6 hours.
  } else {
    std::string body;
    std::wstring err;
    if (!HttpGetJson(&body, &err)) {
      MessageBoxW(owner, (L"检查更新失败：\n" + err).c_str(), L"检查更新",
                  MB_OK | MB_ICONWARNING);
      return;
    }
    latest = Widen(JsonStringField(body, "tag_name"));
    html = Widen(JsonStringField(body, "html_url"));
    if (!latest.empty()) {
      SaveUpdateCache(latest, html);
    }
  }
  if (latest.empty()) {
    MessageBoxW(owner, L"无法解析最新版本信息。", L"检查更新",
                MB_OK | MB_ICONWARNING);
    return;
  }
  if (!IsNewer(latest, current)) {
    MessageBoxW(owner,
                (L"已是最新版本。\n当前：" + current + L"\n远端：" + latest)
                    .c_str(),
                L"检查更新", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const std::wstring msg = L"发现新版本 " + latest + L"（当前 " + current +
                           L"）。\n\n是否打开发布页？";
  if (MessageBoxW(owner, msg.c_str(), L"检查更新",
                  MB_YESNO | MB_ICONQUESTION) == IDYES) {
    ShellExecuteW(nullptr, L"open",
                  html.empty() ? kReleasesPage : html.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
  }
}
