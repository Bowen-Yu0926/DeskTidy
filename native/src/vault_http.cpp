#include "vault_http.h"

#include "paths_util.h"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

constexpr wchar_t kDefaultBase[] =
    L"https://bowen-yu0926.byethost10.com/desktidy-vault/";
constexpr wchar_t kUserAgent[] = L"Mozilla/5.0 DeskTidy-Vault/1";

std::wstring SessionPath() {
  return JoinPath(NativeDataDir(), L"account_vault_session.json");
}

std::string EscapeJson(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '\\' || c == '"') {
      o.push_back('\\');
      o.push_back(static_cast<char>(c));
    } else if (c == '\n') {
      o += "\\n";
    } else if (c == '\r') {
      o += "\\r";
    } else if (c == '\t') {
      o += "\\t";
    } else {
      o.push_back(static_cast<char>(c));
    }
  }
  return o;
}

bool FindStringField(const std::string& obj, const char* key, std::string* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = obj.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = obj.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  ++p;
  while (p < obj.size() &&
         (obj[p] == ' ' || obj[p] == '\t' || obj[p] == '\r' || obj[p] == '\n')) {
    ++p;
  }
  if (p >= obj.size() || obj[p] != '"') {
    return false;
  }
  ++p;
  std::string v;
  while (p < obj.size() && obj[p] != '"') {
    if (obj[p] == '\\' && p + 1 < obj.size()) {
      ++p;
      const char e = obj[p++];
      if (e == 'n') {
        v.push_back('\n');
      } else if (e == 'r') {
        v.push_back('\r');
      } else if (e == 't') {
        v.push_back('\t');
      } else {
        v.push_back(e);
      }
      continue;
    }
    v.push_back(obj[p++]);
  }
  *out = v;
  return true;
}

bool FindIntField(const std::string& obj, const char* key, int* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = obj.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = obj.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  ++p;
  while (p < obj.size() &&
         (obj[p] == ' ' || obj[p] == '\t' || obj[p] == '\r' || obj[p] == '\n')) {
    ++p;
  }
  if (p >= obj.size()) {
    return false;
  }
  char* end = nullptr;
  const long v = std::strtol(obj.c_str() + p, &end, 10);
  if (end == obj.c_str() + p) {
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}

bool JsonOkTrue(const std::string& json) {
  const size_t p = json.find("\"ok\"");
  if (p == std::string::npos) {
    return false;
  }
  const size_t c = json.find(':', p);
  if (c == std::string::npos) {
    return false;
  }
  const size_t t = json.find("true", c);
  const size_t f = json.find("false", c);
  if (t == std::string::npos) {
    return false;
  }
  if (f != std::string::npos && f < t) {
    return false;
  }
  return true;
}

std::vector<BYTE> HexToBytes(const std::string& hex) {
  std::vector<BYTE> out;
  if (hex.size() % 2 != 0) {
    return out;
  }
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
      }
      return -1;
    };
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return {};
    }
    out.push_back(static_cast<BYTE>((hi << 4) | lo));
  }
  return out;
}

std::string BytesToHex(const BYTE* data, size_t len) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out[i * 2] = kHex[(data[i] >> 4) & 0xF];
    out[i * 2 + 1] = kHex[data[i] & 0xF];
  }
  return out;
}

bool Aes128CbcDecrypt(const std::vector<BYTE>& key, const std::vector<BYTE>& iv,
                      const std::vector<BYTE>& cipher, std::vector<BYTE>* plain) {
  if (key.size() != 16 || iv.size() != 16 || cipher.empty() ||
      (cipher.size() % 16) != 0 || !plain) {
    return false;
  }
  BCRYPT_ALG_HANDLE h_alg = nullptr;
  BCRYPT_KEY_HANDLE h_key = nullptr;
  NTSTATUS st = BCryptOpenAlgorithmProvider(&h_alg, BCRYPT_AES_ALGORITHM, nullptr,
                                            0);
  if (st != 0) {
    return false;
  }
  bool ok = false;
  do {
    wchar_t chain_mode[] = BCRYPT_CHAIN_MODE_CBC;
    st = BCryptSetProperty(h_alg, BCRYPT_CHAINING_MODE,
                           reinterpret_cast<PUCHAR>(chain_mode), sizeof(chain_mode),
                           0);
    if (st != 0) {
      break;
    }
    struct {
      BCRYPT_KEY_DATA_BLOB_HEADER hdr;
      BYTE key_bytes[16];
    } blob{};
    blob.hdr.dwMagic = BCRYPT_KEY_DATA_BLOB_MAGIC;
    blob.hdr.dwVersion = BCRYPT_KEY_DATA_BLOB_VERSION1;
    blob.hdr.cbKeyData = 16;
    memcpy(blob.key_bytes, key.data(), 16);
    st = BCryptImportKey(h_alg, nullptr, BCRYPT_KEY_DATA_BLOB, &h_key, nullptr, 0,
                         reinterpret_cast<PUCHAR>(&blob), sizeof(blob), 0);
    if (st != 0) {
      break;
    }
    std::vector<BYTE> iv_copy = iv;
    plain->assign(cipher.size(), 0);
    ULONG out_len = 0;
    st = BCryptDecrypt(h_key, const_cast<PUCHAR>(cipher.data()),
                       static_cast<ULONG>(cipher.size()), nullptr, iv_copy.data(),
                       static_cast<ULONG>(iv_copy.size()), plain->data(),
                       static_cast<ULONG>(plain->size()), &out_len, 0);
    if (st != 0) {
      break;
    }
    plain->resize(out_len);
    ok = true;
  } while (false);
  if (h_key) {
    BCryptDestroyKey(h_key);
  }
  if (h_alg) {
    BCryptCloseAlgorithmProvider(h_alg, 0);
  }
  return ok;
}

std::string SolveByethostTestCookie(const std::string& html) {
  if (html.find("toNumbers(") == std::string::npos ||
      html.find("__test") == std::string::npos) {
    return {};
  }
  std::vector<std::string> nums;
  const std::string marker = "toNumbers(\"";
  size_t pos = 0;
  while (nums.size() < 3) {
    const size_t a = html.find(marker, pos);
    if (a == std::string::npos) {
      break;
    }
    const size_t b = a + marker.size();
    const size_t c = html.find('"', b);
    if (c == std::string::npos) {
      break;
    }
    nums.push_back(html.substr(b, c - b));
    pos = c + 1;
  }
  if (nums.size() < 3) {
    return {};
  }
  const auto key = HexToBytes(nums[0]);
  const auto iv = HexToBytes(nums[1]);
  const auto cipher = HexToBytes(nums[2]);
  std::vector<BYTE> plain;
  if (!Aes128CbcDecrypt(key, iv, cipher, &plain) || plain.empty()) {
    return {};
  }
  return BytesToHex(plain.data(), plain.size());
}

struct UrlParts {
  bool https = true;
  std::wstring host;
  INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
  std::wstring path;  // begins with /
};

bool CrackUrl(const std::wstring& url, UrlParts* out) {
  if (!out) {
    return false;
  }
  URL_COMPONENTSW uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256]{};
  wchar_t path[2048]{};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
    return false;
  }
  out->https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
  out->host.assign(host, uc.dwHostNameLength);
  out->port = uc.nPort;
  out->path.assign(path, uc.dwUrlPathLength);
  if (out->path.empty()) {
    out->path = L"/";
  }
  return !out->host.empty();
}

std::wstring NormalizeBase(const std::wstring& base) {
  std::wstring b = base.empty() ? kDefaultBase : base;
  while (!b.empty() && (b.back() == L' ' || b.back() == L'\t')) {
    b.pop_back();
  }
  if (!b.empty() && b.back() != L'/') {
    b.push_back(L'/');
  }
  if (b.empty()) {
    b = kDefaultBase;
  }
  return b;
}

struct Session {
  std::wstring api_base = kDefaultBase;
  std::wstring login;
  std::wstring token;
};

Session LoadSession() {
  Session s;
  std::ifstream in(SessionPath());
  if (!in) {
    return s;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  std::string api, login, token;
  if (FindStringField(json, "api_base", &api) && !api.empty()) {
    s.api_base = NormalizeBase(WidenUtf8(api));
  }
  if (FindStringField(json, "login", &login)) {
    s.login = WidenUtf8(login);
  }
  if (FindStringField(json, "token", &token)) {
    s.token = WidenUtf8(token);
  }
  return s;
}

bool SaveSession(const Session& s) {
  std::ofstream out(SessionPath(), std::ios::binary);
  if (!out) {
    return false;
  }
  out << "{\n  \"api_base\": \""
      << EscapeJson(NarrowUtf8(NormalizeBase(s.api_base))) << "\",\n"
      << "  \"login\": \"" << EscapeJson(NarrowUtf8(s.login)) << "\",\n"
      << "  \"token\": \"" << EscapeJson(NarrowUtf8(s.token)) << "\"\n}\n";
  return true;
}

void ClearSessionFile() { DeleteFileW(SessionPath().c_str()); }

struct HttpResponse {
  DWORD status = 0;
  std::string body;
};

class VaultClient {
 public:
  explicit VaultClient(const std::wstring& api_base)
      : base_(NormalizeBase(api_base)) {}

  ~VaultClient() {
    if (session_) {
      WinHttpCloseHandle(session_);
    }
  }

  bool EnsureSession() {
    if (session_) {
      return true;
    }
    session_ = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_) {
      return false;
    }
    WinHttpSetTimeouts(session_, 15000, 15000, 20000, 20000);
    return true;
  }

  void SetToken(const std::wstring& token) { token_ = token; }

  bool EnsureByethost() {
    if (!test_cookie_.empty()) {
      return true;
    }
    HttpResponse resp;
    if (!RawRequest(L"GET", base_, nullptr, false, &resp, false)) {
      return false;
    }
    const std::string tok = SolveByethostTestCookie(resp.body);
    if (!tok.empty()) {
      test_cookie_ = tok;
    }
    return true;
  }

  bool Request(const wchar_t* method, const std::wstring& rel_path,
               const std::string* body_utf8, bool auth, HttpResponse* out) {
    if (!EnsureSession()) {
      return false;
    }
    EnsureByethost();
    std::wstring url = base_;
    if (!rel_path.empty()) {
      if (rel_path.front() == L'/') {
        url += rel_path.substr(1);
      } else {
        url += rel_path;
      }
    }
    if (!RawRequest(method, url, body_utf8, auth, out, true)) {
      return false;
    }
    const std::string tok = SolveByethostTestCookie(out->body);
    if (!tok.empty()) {
      test_cookie_ = tok;
      // Retry once after solving challenge page.
      if (out->body.find("aes.js") != std::string::npos ||
          out->body.find("This site requires Javascript") != std::string::npos ||
          !JsonOkTrue(out->body)) {
        return RawRequest(method, url, body_utf8, auth, out, true);
      }
    }
    return true;
  }

 private:
  bool RawRequest(const wchar_t* method, const std::wstring& url,
                  const std::string* body_utf8, bool auth, HttpResponse* out,
                  bool send_cookie) {
    if (!out || !EnsureSession()) {
      return false;
    }
    UrlParts parts;
    if (!CrackUrl(url, &parts)) {
      return false;
    }
    HINTERNET conn =
        WinHttpConnect(session_, parts.host.c_str(), parts.port, 0);
    if (!conn) {
      return false;
    }
    DWORD flags = parts.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req =
        WinHttpOpenRequest(conn, method, parts.path.c_str(), nullptr,
                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
      WinHttpCloseHandle(conn);
      return false;
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (body_utf8) {
      headers += L"Content-Type: application/json; charset=utf-8\r\n";
    }
    if (auth && !token_.empty()) {
      headers += L"Authorization: Bearer ";
      headers += token_;
      headers += L"\r\nX-Vault-Token: ";
      headers += token_;
      headers += L"\r\n";
    }
    if (send_cookie && !test_cookie_.empty()) {
      headers += L"Cookie: __test=";
      headers += WidenUtf8(test_cookie_);
      headers += L"\r\n";
    }
    WinHttpAddRequestHeaders(req, headers.c_str(), static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD);

    const void* body_ptr = body_utf8 ? body_utf8->data() : nullptr;
    const DWORD body_len =
        body_utf8 ? static_cast<DWORD>(body_utf8->size()) : 0;
    BOOL sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   const_cast<void*>(body_ptr), body_len, body_len,
                                   0);
    if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
      WinHttpCloseHandle(req);
      WinHttpCloseHandle(conn);
      return false;
    }
    DWORD status = 0;
    DWORD status_len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len,
                        WINHTTP_NO_HEADER_INDEX);
    out->status = status;
    out->body.clear();
    for (;;) {
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(req, &avail)) {
        break;
      }
      if (avail == 0) {
        break;
      }
      std::string chunk(avail, '\0');
      DWORD read = 0;
      if (!WinHttpReadData(req, chunk.data(), avail, &read)) {
        break;
      }
      chunk.resize(read);
      out->body.append(chunk);
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return true;
  }

  std::wstring base_;
  std::wstring token_;
  std::string test_cookie_;
  HINTERNET session_ = nullptr;
};

std::wstring MatchKey(const VaultHttpCredential& e) {
  return e.title + L"\n" + e.username;
}

VaultHttpCredential ParseCredentialObj(const std::string& obj) {
  VaultHttpCredential c;
  FindIntField(obj, "id", &c.id);
  std::string title, user, pass, url;
  if (FindStringField(obj, "title", &title)) {
    c.title = WidenUtf8(title);
  }
  if (FindStringField(obj, "username", &user)) {
    c.username = WidenUtf8(user);
  }
  if (FindStringField(obj, "password", &pass)) {
    c.password = WidenUtf8(pass);
  }
  if (FindStringField(obj, "url", &url)) {
    c.url = WidenUtf8(url);
  }
  int enc = 0;
  FindIntField(obj, "enc_ver", &enc);
  // enc_ver 0: plaintext fields. enc_ver 1: keep raw ciphertext strings.
  (void)enc;
  return c;
}

std::vector<VaultHttpCredential> ParseCredentialItems(const std::string& json) {
  std::vector<VaultHttpCredential> out;
  size_t items = json.find("\"items\"");
  if (items == std::string::npos) {
    items = 0;
  }
  size_t pos = items;
  while (true) {
    const size_t start = json.find('{', pos);
    if (start == std::string::npos) {
      break;
    }
    const size_t end = json.find('}', start);
    if (end == std::string::npos) {
      break;
    }
    const std::string obj = json.substr(start, end - start + 1);
    pos = end + 1;
    if (obj.find("\"title\"") == std::string::npos &&
        obj.find("\"username\"") == std::string::npos) {
      continue;
    }
    out.push_back(ParseCredentialObj(obj));
  }
  return out;
}

std::string MakeCredBody(const std::wstring& title, const std::wstring& user,
                         const std::wstring& pass, const std::wstring& url) {
  std::ostringstream ss;
  ss << "{\"title\":\"" << EscapeJson(NarrowUtf8(title)) << "\","
     << "\"username\":\"" << EscapeJson(NarrowUtf8(user)) << "\","
     << "\"password\":\"" << EscapeJson(NarrowUtf8(pass)) << "\","
     << "\"url\":\"" << EscapeJson(NarrowUtf8(url)) << "\","
     << "\"note\":\"\","
     << "\"enc_ver\":0}";
  return ss.str();
}

}  // namespace

bool VaultHttpHealth() {
  Session s = LoadSession();
  VaultClient client(s.api_base);
  HttpResponse resp;
  if (!client.Request(L"GET", L"health", nullptr, false, &resp)) {
    return false;
  }
  return JsonOkTrue(resp.body) || resp.body.find("\"service\"") != std::string::npos;
}

bool VaultHttpLogin(const std::wstring& login, const std::wstring& password) {
  Session s = LoadSession();
  VaultClient client(s.api_base);
  std::ostringstream body;
  body << "{\"login\":\"" << EscapeJson(NarrowUtf8(login)) << "\","
       << "\"password\":\"" << EscapeJson(NarrowUtf8(password)) << "\"}";
  const std::string payload = body.str();
  HttpResponse resp;
  if (!client.Request(L"POST", L"auth/login", &payload, false, &resp)) {
    return false;
  }
  if (!JsonOkTrue(resp.body)) {
    return false;
  }
  std::string token;
  if (!FindStringField(resp.body, "token", &token) || token.empty()) {
    return false;
  }
  std::string login_echo;
  FindStringField(resp.body, "login", &login_echo);
  s.login = login_echo.empty() ? login : WidenUtf8(login_echo);
  s.token = WidenUtf8(token);
  s.api_base = NormalizeBase(s.api_base);
  return SaveSession(s);
}

bool VaultHttpLogout() {
  Session s = LoadSession();
  if (!s.token.empty()) {
    VaultClient client(s.api_base);
    client.SetToken(s.token);
    const std::string empty = "{}";
    HttpResponse resp;
    client.Request(L"POST", L"auth/logout", &empty, true, &resp);
  }
  ClearSessionFile();
  return true;
}

std::vector<VaultHttpCredential> VaultHttpListCredentials() {
  Session s = LoadSession();
  if (s.token.empty()) {
    return {};
  }
  VaultClient client(s.api_base);
  client.SetToken(s.token);
  HttpResponse resp;
  if (!client.Request(L"GET", L"credentials", nullptr, true, &resp) ||
      !JsonOkTrue(resp.body)) {
    return {};
  }
  return ParseCredentialItems(resp.body);
}

bool VaultHttpUpsertCredential(const std::wstring& title,
                               const std::wstring& user,
                               const std::wstring& pass,
                               const std::wstring& url) {
  Session s = LoadSession();
  if (s.token.empty()) {
    return false;
  }
  VaultClient client(s.api_base);
  client.SetToken(s.token);

  int existing_id = 0;
  HttpResponse list_resp;
  if (client.Request(L"GET", L"credentials", nullptr, true, &list_resp) &&
      JsonOkTrue(list_resp.body)) {
    for (const auto& r : ParseCredentialItems(list_resp.body)) {
      if (_wcsicmp(r.title.c_str(), title.c_str()) == 0 &&
          _wcsicmp(r.username.c_str(), user.c_str()) == 0) {
        existing_id = r.id;
        break;
      }
    }
  }

  const std::string payload = MakeCredBody(title, user, pass, url);
  HttpResponse resp;
  if (existing_id > 0) {
    const std::wstring path = L"credentials/" + std::to_wstring(existing_id);
    if (!client.Request(L"PUT", path, &payload, true, &resp)) {
      return false;
    }
  } else {
    if (!client.Request(L"POST", L"credentials", &payload, true, &resp)) {
      return false;
    }
  }
  return JsonOkTrue(resp.body);
}

bool VaultHttpPushAll(const std::vector<VaultHttpCredential>& entries) {
  Session s = LoadSession();
  if (s.token.empty()) {
    return false;
  }
  bool any_ok = true;
  for (const auto& e : entries) {
    if (e.title.empty() && e.username.empty()) {
      continue;
    }
    if (!VaultHttpUpsertCredential(e.title, e.username, e.password, e.url)) {
      any_ok = false;
    }
  }
  return any_ok;
}

bool VaultHttpPullMerge(std::vector<VaultHttpCredential>* entries) {
  if (!entries) {
    return false;
  }
  const auto remote = VaultHttpListCredentials();
  if (remote.empty() && LoadSession().token.empty()) {
    return false;
  }
  // Index local by title+username.
  for (const auto& r : remote) {
    bool found = false;
    for (auto& local : *entries) {
      if (_wcsicmp(MatchKey(local).c_str(), MatchKey(r).c_str()) == 0) {
        local.id = r.id;
        local.password = r.password;
        local.url = r.url;
        if (local.title.empty()) {
          local.title = r.title;
        }
        if (local.username.empty()) {
          local.username = r.username;
        }
        found = true;
        break;
      }
    }
    if (!found) {
      entries->push_back(r);
    }
  }
  return true;
}
