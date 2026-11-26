#include "update_check.h"

#include "paths_util.h"
#include "update_json.h"
#include "update_verify.h"

#include <Shellapi.h>
#include <bcrypt.h>
#include <winhttp.h>

#include "log.h"
#include "toast.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr wchar_t kHost[] = L"api.github.com";
constexpr wchar_t kPath[] =
    L"/repos/Bowen-Yu0926/DeskTidy/releases/latest";
constexpr wchar_t kReleasesPage[] =
    L"https://github.com/Bowen-Yu0926/DeskTidy/releases/latest";
constexpr const wchar_t* kInstallerPrefix = L"DeskTidy_Setup_";

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

std::wstring ToLowerW(const std::wstring& s) {
  std::wstring o = s;
  for (auto& c : o) {
    if (c >= L'A' && c <= L'Z') {
      c = c - L'A' + L'a';
    }
  }
  return o;
}

std::wstring UrlFilename(const std::wstring& url) {
  const size_t slash = url.find_last_of(L'/');
  std::wstring name = (slash == std::wstring::npos) ? url : url.substr(slash + 1);
  const size_t q = name.find_first_of(L"?#");
  if (q != std::wstring::npos) {
    name.resize(q);
  }
  return name;
}

// Whitelist hosts for installer/digest bytes (mirrors Python update_installer).
bool AllowedDownloadHost(const std::wstring& url) {
  URL_COMPONENTS uc{};
  wchar_t host[256] = {};
  uc.dwStructSize = sizeof(uc);
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) ||
      uc.nScheme != INTERNET_SCHEME_HTTPS) {
    return false;
  }
  const std::wstring h = ToLowerW(host);
  const std::wstring suffix = L".githubusercontent.com";
  return h == L"github.com" || h == L"objects.githubusercontent.com" ||
         (h.size() >= suffix.size() &&
          h.compare(h.size() - suffix.size(), suffix.size(), suffix) == 0);
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

// Generic HTTPS GET (text) restricted to the allow-list — used for digest files.
bool HttpGetTextUrl(const std::wstring& url, std::string* body,
                    std::wstring* err) {
  if (!AllowedDownloadHost(url)) {
    *err = L"拒绝从非白名单主机下载";
    return false;
  }
  URL_COMPONENTS uc{};
  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  uc.dwStructSize = sizeof(uc);
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
    *err = L"无法解析链接";
    return false;
  }
  HINTERNET session =
      WinHttpOpen(L"DeskTidyNative-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *err = L"WinHttpOpen failed";
    return false;
  }
  WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
  HINTERNET conn =
      WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET req = conn ? WinHttpOpenRequest(
                             conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                       : nullptr;
  if (!req) {
    *err = L"open request failed";
    if (conn) {
      WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return false;
  }
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

std::string FindHex64(const std::string& text) {
  size_t i = 0;
  while (i < text.size()) {
    if (isxdigit(static_cast<unsigned char>(text[i]))) {
      size_t j = i;
      while (j < text.size() && isxdigit(static_cast<unsigned char>(text[j]))) {
        ++j;
      }
      if (j - i == 64) {
        return text.substr(i, 64);
      }
      i = j;
    } else {
      ++i;
    }
  }
  return {};
}

std::string FindSha256InText(const std::string& text,
                             const std::string& asset_name) {
  // Prefer a hash on the same line as the asset name: sha256sum-style notes
  // put "<hash>  <file>", so scanning forward from the name picked up the
  // *next* asset's digest and made every download fail verification.
  if (!asset_name.empty()) {
    size_t p = 0;
    while ((p = text.find(asset_name, p)) != std::string::npos) {
      size_t line_begin = text.rfind('\n', p);
      line_begin = (line_begin == std::string::npos) ? 0 : line_begin + 1;
      size_t line_end = text.find('\n', p + asset_name.size());
      if (line_end == std::string::npos) {
        line_end = text.size();
      }
      const std::string tok =
          FindHex64(text.substr(line_begin, line_end - line_begin));
      if (!tok.empty()) {
        return tok;
      }
      p += asset_name.size();
    }
  }
  // No per-line match: a single-asset body with one hash is still unambiguous.
  const std::string only = FindHex64(text);
  if (!only.empty()) {
    size_t first = 0;
    size_t hits = 0;
    while ((first = text.find(only, first)) != std::string::npos) {
      ++hits;
      first += only.size();
    }
    if (hits == 1) {
      return only;
    }
  }
  return {};
}

// sha256 for an asset: release body near the name, then a sidecar digest file.
std::string FindChecksum(const std::string& json, const std::string& asset_name) {
  const std::string body = JsonStringField(json, "body");
  std::string tok = FindSha256InText(body, asset_name);
  if (!tok.empty()) {
    return tok;
  }
  const std::wstring sidecars[] = {
      Widen(asset_name) + L".sha256", Widen(asset_name) + L".sha256sum",
      Widen(asset_name) + L".sha256.txt"};
  std::string found;
  ForEachArrayObject(json, "assets", [&](const std::string& obj) {
    if (!found.empty()) {
      return;
    }
    const std::string nm = JsonStringField(obj, "name");
    if (nm.empty()) {
      return;
    }
    bool match = false;
    for (const auto& s : sidecars) {
      if (ToLowerW(Widen(nm)) == ToLowerW(s)) {
        match = true;
        break;
      }
    }
    if (!match) {
      return;
    }
    const std::string ur = JsonStringField(obj, "browser_download_url");
    if (ur.empty()) {
      return;
    }
    std::string txt;
    std::wstring err;
    if (HttpGetTextUrl(Widen(ur), &txt, &err)) {
      const std::string tok = FindHex64(txt);
      if (!tok.empty()) {
        found = tok;
      }
    } else {
      LogLine(L"[update] sidecar digest fetch failed: %s", err.c_str());
    }
  });
  return found;
}

// Chunked streaming download to dir (atomic .part rename) with progress posts.
bool DownloadAsset(const std::wstring& url, const std::wstring& dir,
                   HWND progress_hwnd, std::wstring* err,
                   std::wstring* out_path, long long* out_bytes) {
  if (!AllowedDownloadHost(url)) {
    *err = L"拒绝从非白名单主机下载";
    return false;
  }
  URL_COMPONENTS uc{};
  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  uc.dwStructSize = sizeof(uc);
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
    *err = L"无法解析下载地址";
    return false;
  }
  std::wstring fname = UrlFilename(url);
  if (fname.empty()) {
    fname = L"DeskTidy_Setup_download.exe";
  }
  const std::wstring final_path = JoinPath(dir, fname);
  const std::wstring tmp_path = final_path + L".part";
  HINTERNET session =
      WinHttpOpen(L"DeskTidyNative-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *err = L"WinHttpOpen failed";
    return false;
  }
  WinHttpSetTimeouts(session, 8000, 8000, 120000, 120000);
  HINTERNET conn =
      WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET req = conn ? WinHttpOpenRequest(
                             conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                       : nullptr;
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  if (req) {
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy,
                     sizeof(policy));
  }
  if (!req || !WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(req, nullptr)) {
    *err = L"下载失败";
    if (req) {
      WinHttpCloseHandle(req);
    }
    if (conn) {
      WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return false;
  }
  DWORD content_len = 0;
  DWORD cl_sz = sizeof(content_len);
  WinHttpQueryHeaders(req,
                      WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &content_len, &cl_sz,
                      WINHTTP_NO_HEADER_INDEX);
  std::ofstream out(tmp_path, std::ios::binary);
  if (!out) {
    *err = L"无法写入安装包";
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return false;
  }
  long long done = 0;
  int last_milestone = -1;
  bool ok = true;
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail)) {
      ok = false;
      break;
    }
    if (avail == 0) {
      break;
    }
    std::string chunk(avail, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) {
      ok = false;
      break;
    }
    if (!out.write(chunk.data(), read)) {
      ok = false;
      break;
    }
    done += read;
    if (content_len > 0 && progress_hwnd) {
      const int pct = static_cast<int>(done * 100 / content_len);
      // Every 5% (smoother than 10%) without flooding the UI thread.
      const int milestone = pct - (pct % 5);
      if (milestone != last_milestone) {
        last_milestone = milestone;
        PostMessageW(progress_hwnd, kMsgUpdateProgress,
                     static_cast<WPARAM>(pct), 0);
      }
    }
  }
  out.close();
  if (!out.good()) {
    ok = false;
  }
  WinHttpCloseHandle(req);
  WinHttpCloseHandle(conn);
  WinHttpCloseHandle(session);
  if (!ok) {
    DeleteFileW(tmp_path.c_str());
    *err = L"下载中断";
    return false;
  }
  if (!MoveFileExW(tmp_path.c_str(), final_path.c_str(),
                   MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp_path.c_str());
    *err = L"保存安装包失败";
    return false;
  }
  if (progress_hwnd) {
    PostMessageW(progress_hwnd, kMsgUpdateProgress, 100, 0);
  }
  if (out_path) {
    *out_path = final_path;
  }
  if (out_bytes) {
    *out_bytes = done;
  }
  return true;
}

bool ComputeSha256Hex(const std::wstring& path, std::string* hex) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) !=
      0) {
    return false;
  }
  BCRYPT_HASH_HANDLE h = nullptr;
  if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) {
    BCryptCloseAlgorithmProvider(alg, 0);
    return false;
  }
  char buf[65536];
  while (in) {
    in.read(buf, sizeof(buf));
    const std::streamsize n = in.gcount();
    if (n > 0) {
      BCryptHashData(h, reinterpret_cast<PUCHAR>(buf), static_cast<ULONG>(n), 0);
    }
  }
  BYTE digest[32] = {};
  BCryptFinishHash(h, digest, 32, 0);
  BCryptDestroyHash(h);
  BCryptCloseAlgorithmProvider(alg, 0);
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (int i = 0; i < 32; ++i) {
    out.push_back(kHex[digest[i] >> 4]);
    out.push_back(kHex[digest[i] & 15]);
  }
  *hex = out;
  return true;
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
      const unsigned long long ticks =
          std::strtoull(json.c_str() + c + 1, nullptr, 10);
      const unsigned long long ttl = 6ULL * 3600ULL * 10000000ULL;
      *fresh = NowFileTime() < ticks + ttl;
    }
  }
  return true;
}

std::string EscapeJsonUtf8(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (c >= 0x20) {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

void SaveUpdateCache(const std::wstring& latest, const std::wstring& html) {
  std::ofstream out(JoinPath(NativeDataDir(), L"update_check_cache.json"),
                    std::ios::binary);
  if (!out) {
    return;
  }
  out << "{\n  \"ticks\": " << NowFileTime() << ",\n  \"tag\": \""
      << EscapeJsonUtf8(NarrowUtf8(latest)) << "\",\n  \"html\": \""
      << EscapeJsonUtf8(NarrowUtf8(html)) << "\"\n}\n";
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
    return L"4.0.57";
  }
  std::vector<BYTE> buf(size);
  if (!GetFileVersionInfoW(path, 0, size, buf.data())) {
    return L"4.0.57";
  }
  VS_FIXEDFILEINFO* fi = nullptr;
  UINT len = 0;
  if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) ||
      !fi) {
    return L"4.0.57";
  }
  wchar_t ver[64];
  swprintf_s(ver, L"%u.%u.%u", HIWORD(fi->dwFileVersionMS),
             LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
  return ver;
}

std::wstring NativeDisplayVersion() {
  std::wstring v = NativeProductVersion();
  if (IsDevRuntime()) {
    v += L"-dev";
  }
  return v;
}

// The App message window whose WndProc handles kMsgUpdate*. Set once at
// startup; guarded because startup and UI-thread checks both read it.
std::atomic<HWND> g_update_result_window{nullptr};

void SetUpdateResultWindow(HWND hwnd) {
  g_update_result_window.store(hwnd);
}

HWND UpdateResultWindow(HWND fallback) {
  HWND h = g_update_result_window.load();
  if (h && IsWindow(h)) {
    return h;
  }
  return fallback;
}

void StartUpdateCheckAsync(HWND owner, bool force, bool interactive) {
  static std::atomic<bool> g_checking{false};
  if (g_checking.exchange(true)) {
    LogLine(L"[update] check already in progress, ignoring duplicate request");
    return;
  }
  std::thread([owner, force, interactive]() {
    struct ResetGuard {
      ~ResetGuard() { g_checking.store(false); }
    } reset_guard;
    auto* r = new UpdateCheckResult;
    r->interactive = interactive;
    r->current = NativeProductVersion();
    bool fresh = false;
    if (!force && LoadUpdateCache(&r->latest, &r->html_url, &fresh) && fresh &&
        !r->latest.empty()) {
      LogLine(L"[update] served cached result tag=%s", r->latest.c_str());
    } else {
      std::string body;
      std::wstring err;
      if (HttpGetJson(&body, &err)) {
        r->latest = Widen(JsonStringField(body, "tag_name"));
        r->html_url = Widen(JsonStringField(body, "html_url"));
        if (!r->latest.empty()) {
          SaveUpdateCache(r->latest, r->html_url);
        }
      } else {
        r->error = err;
      }
    }
    if (r->latest.empty() || r->current.empty()) {
      r->status = 0;
      if (r->error.empty()) {
        r->error = L"无法解析最新版本信息";
      }
    } else if (IsNewer(r->latest, r->current)) {
      r->status = 2;
    } else {
      r->status = 1;
    }
    if (!PostMessageW(owner, kMsgUpdateCheckDone, 0, reinterpret_cast<LPARAM>(r))) {
      delete r;
    }
  }).detach();
}

void StartUpdateInstallAsync(HWND owner) {
  static std::atomic<bool> g_installing{false};
  if (g_installing.exchange(true)) {
    LogLine(L"[update] install already in progress, ignoring duplicate request");
    return;
  }
  std::thread([owner]() {
    struct ResetGuard {
      std::atomic<bool>* flag;
      ~ResetGuard() { flag->store(false); }
    } reset_guard{&g_installing};
    auto* r = new UpdateInstallResult;
    std::string body;
    std::wstring err;
    if (!HttpGetJson(&body, &err)) {
      r->error = L"获取更新信息失败：" + err;
    } else {
      std::string asset_name;
      std::wstring asset_url;
      long long asset_size = 0;
      if (!FindExeAsset(body, &asset_name, &asset_url, &asset_size)) {
        r->error = L"该版本未提供 Windows 安装包";
      } else {
        const std::string checksum = FindChecksum(body, asset_name);
        const std::wstring dir = JoinPath(NativeDataDir(), L"updates");
        CreateDirectoryW(dir.c_str(), nullptr);
        // P2-7: purge stale packages before a new download.
        {
          WIN32_FIND_DATAW fd{};
          const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
          if (h != INVALID_HANDLE_VALUE) {
            do {
              if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
              }
              DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
          }
        }
        std::wstring dest;
        long long bytes = 0;
        if (!DownloadAsset(asset_url, dir, owner, &err, &dest, &bytes)) {
          r->error = L"下载失败：" + err;
          if (!dest.empty()) {
            DeleteFileW(dest.c_str());
          }
        } else {
          r->path = dest;
          // Python: no published checksum → refuse install and delete the file
          // (never fall back to size-only / unconditional pass) — P0-10.
          if (checksum.empty()) {
            r->verify_ok = false;
            r->has_checksum = false;
            DeleteFileW(dest.c_str());
            r->path.clear();
            r->error =
                L"该版本未提供 sha256 校验值，已取消安装并删除下载文件";
            LogLine(L"[update] no digest — refuse install, deleted package");
          } else {
            r->has_checksum = true;
            std::string hex;
            r->verify_ok = ComputeSha256Hex(dest, &hex) &&
                           ToLowerW(Widen(hex)) == ToLowerW(Widen(checksum));
            LogLine(L"[update] sha256 verify %s",
                    r->verify_ok ? L"ok" : L"FAIL");

            // Signature check: the digest and the exe come from the same
            // release, so a compromised release can swap both. Only the
            // Ed25519 manifest (signed with the key compiled into this build)
            // proves the installer is ours. Required once a key is embedded.
            if (r->verify_ok) {
              std::wstring manifest_url;
              std::wstring sig_url;
              const std::string man_name = asset_name + ".manifest";
              const std::string sig_name = asset_name + ".sig";
              std::string manifest, sig_b64;
              std::wstring serr;
              bool sig_ok = false;
              bool sig_skipped = false;
              if (!FindAssetByName(body, man_name, &manifest_url) ||
                  !FindAssetByName(body, sig_name, &sig_url)) {
                sig_ok = false;
                r->error = L"该版本未提供更新签名（manifest/.sig），已取消安装";
                LogLine(L"[update] manifest/sig asset missing — refuse");
              } else if (!HttpGetTextUrl(manifest_url, &manifest, &serr) ||
                         !HttpGetTextUrl(sig_url, &sig_b64, &serr)) {
                sig_ok = false;
                r->error = L"获取更新签名失败：" + serr;
                LogLine(L"[update] manifest fetch failed: %s", serr.c_str());
              } else {
                // Trim trailing CR/LF the .sig file may carry.
                while (!sig_b64.empty() && (sig_b64.back() == '\n' ||
                                            sig_b64.back() == '\r' ||
                                            sig_b64.back() == ' ')) {
                  sig_b64.pop_back();
                }
                sig_ok = VerifyManifestWithBuiltinKey(manifest, sig_b64,
                                                      &sig_skipped);
                if (sig_skipped) {
                  LogLine(L"[update] builtin signing key not configured — "
                          L"signature check SKIPPED (digest only)");
                } else {
                  LogLine(L"[update] manifest signature %s",
                          sig_ok ? L"ok" : L"FAIL");
                  if (!sig_ok) {
                    r->error = L"更新签名校验未通过，已拒绝安装该更新";
                  }
                }
              }
              if (!sig_ok) {
                r->verify_ok = false;
              }
            }

            if (!r->verify_ok) {
              DeleteFileW(dest.c_str());
              r->path.clear();
              if (r->error.empty()) {
                r->error = L"安装包 sha256 校验未通过，已删除文件";
              }
            }
          }
          (void)asset_size;
          (void)bytes;
        }
      }
    }
    if (r->verify_ok && r->error.empty()) {
      r->ok = true;
    }
    g_installing.store(false);
    if (!PostMessageW(owner, kMsgUpdateInstallDone, 0, reinterpret_cast<LPARAM>(r))) {
      delete r;
    }
  }).detach();
}

bool LaunchInstallerSilent(const std::wstring& path, std::wstring* err) {
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    if (err) {
      *err = L"安装包不存在：" + path;
    }
    return false;
  }
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"open";
  sei.lpFile = path.c_str();
  // /SILENT: skip prompts but KEEP Inno's progress window (market: VS Code /
  // Discord updater). /VERYSILENT would hide progress entirely.
  sei.lpParameters = L"/SILENT /NORESTART /CLOSEAPPLICATIONS";
  sei.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&sei)) {
    if (err) {
      *err = L"启动安装程序失败（错误码 " +
             std::to_wstring(GetLastError()) + L"）";
    }
    // P2-7: failed launch — drop the package so it cannot accumulate.
    DeleteFileW(path.c_str());
    return false;
  }
  // Wait until the installer UI is up before we quit DeskTidy — otherwise the
  // progress window may flash behind a dying process or never paint.
  if (sei.hProcess) {
    WaitForInputIdle(sei.hProcess, 12000);
    CloseHandle(sei.hProcess);
  } else {
    Sleep(800);
  }
  // Do NOT DeleteFile the Setup exe here: Inno may still be reading it.
  // Next update download replaces/purges the cache directory.
  return true;
}

void StartInstallerLaunchAsync(HWND owner, const std::wstring& path) {
  std::thread([owner, path]() {
    auto* r = new UpdateInstallResult;
    r->path = path;
    std::wstring err;
    r->ok = LaunchInstallerSilent(path, &err);
    r->error = std::move(err);
    if (!PostMessageW(owner, kMsgUpdateInstallLaunched, 0,
                      reinterpret_cast<LPARAM>(r))) {
      delete r;
    }
  }).detach();
}

void CheckForUpdates(HWND owner) {
  // Manual entry (tools_misc OpenUpdatePage): forced refresh + interactive UI.
  // Retarget to the App message window — manual-check callers pass the
  // settings window / help dialog, which never handle kMsgUpdateCheckDone.
  StartUpdateCheckAsync(UpdateResultWindow(owner), /*force=*/true,
                        /*interactive=*/true);
}
