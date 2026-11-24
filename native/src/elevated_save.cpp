#include "elevated_save.h"

#include "desknote_launch.h"
#include "log.h"
#include "paths_util.h"

#include <Windows.h>
#include <Shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

constexpr wchar_t kPendingName[] = L"elevated_save_pending.json";
constexpr wchar_t kResultName[] = L"elevated_save_result.json";
constexpr wchar_t kHostsName[] = L"hosts";
constexpr double kPendingMaxAgeS = 3600.0;
constexpr int kSeSuccessThreshold = 32;
constexpr INT_PTR kSeCanceled = 1223;  // ERROR_CANCELLED from UAC "No"

DWORD g_last_write_err = 0;

std::string UnescapeJsonUtf8(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      out.push_back(s[i]);
      continue;
    }
    const char n = s[++i];
    switch (n) {
      case '"':
      case '\\':
      case '/':
        out.push_back(n);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u':
        if (i + 4 < s.size()) {
          unsigned code = 0;
          for (int k = 0; k < 4; ++k) {
            const char h = s[++i];
            code <<= 4;
            if (h >= '0' && h <= '9') {
              code |= static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              code |= static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              code |= static_cast<unsigned>(h - 'A' + 10);
            }
          }
          if (code < 0x80) {
            out.push_back(static_cast<char>(code));
          } else {
            // BMP codepoint → UTF-8 (sufficient for our pending payloads).
            const std::wstring w(1, static_cast<wchar_t>(code));
            out += NarrowUtf8(w);
          }
        }
        break;
      default:
        out.push_back(n);
        break;
    }
  }
  return out;
}

bool ExtractJsonStringField(const std::string& json, const char* key,
                            std::string* value) {
  if (!value) {
    return false;
  }
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) {
    return false;
  }
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) {
    return false;
  }
  pos = json.find('"', pos + 1);
  if (pos == std::string::npos) {
    return false;
  }
  ++pos;
  std::string raw;
  while (pos < json.size()) {
    if (json[pos] == '\\' && pos + 1 < json.size()) {
      raw.push_back(json[pos++]);
      raw.push_back(json[pos++]);
      continue;
    }
    if (json[pos] == '"') {
      *value = UnescapeJsonUtf8(raw);
      return true;
    }
    raw.push_back(json[pos++]);
  }
  return false;
}

bool ExtractJsonNumberField(const std::string& json, const char* key,
                            double* value) {
  if (!value) {
    return false;
  }
  const std::string needle = std::string("\"") + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) {
    return false;
  }
  pos = json.find(':', pos + needle.size());
  if (pos == std::string::npos) {
    return false;
  }
  ++pos;
  while (pos < json.size() &&
         (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' ||
          json[pos] == '\n')) {
    ++pos;
  }
  char* end = nullptr;
  const double v = std::strtod(json.c_str() + pos, &end);
  if (!end || end == json.c_str() + pos) {
    return false;
  }
  *value = v;
  return true;
}

std::wstring NormalizePath(const std::wstring& path) {
  wchar_t buf[MAX_PATH * 4] = {};
  constexpr DWORD kBufChars = static_cast<DWORD>(MAX_PATH * 4);
  const DWORD n = GetFullPathNameW(path.c_str(), kBufChars, buf, nullptr);
  if (n == 0 || n >= kBufChars) {
    return path;
  }
  return buf;
}

bool IsHostsTarget(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring name =
      slash == std::wstring::npos ? path : path.substr(slash + 1);
  return _wcsicmp(name.c_str(), kHostsName) == 0;
}

// RichEdit 可能把 CRLF 规范成 LF/CR；提权写回 hosts 时统一恢复 CRLF。
std::wstring HostsTextForWrite(const std::wstring& text) {
  std::wstring out;
  out.reserve(text.size() + 16);
  for (wchar_t c : text) {
    if (c == L'\r') {
      continue;
    }
    if (c == L'\n') {
      out.push_back(L'\r');
    }
    out.push_back(c);
  }
  return out;
}

bool PathsEqual(const std::wstring& a, const std::wstring& b) {
  return _wcsicmp(NormalizePath(a).c_str(), NormalizePath(b).c_str()) == 0;
}

double NowUnixS() {
  FILETIME ft{};
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER uli{};
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;
  // FILETIME epochs at 1601; Unix at 1970.
  constexpr ULONGLONG kEpochDiff = 116444736000000000ULL;
  if (uli.QuadPart < kEpochDiff) {
    return 0.0;
  }
  return static_cast<double>(uli.QuadPart - kEpochDiff) / 10000000.0;
}

bool ReadFileUtf8(const std::wstring& path, std::string* json) {
  if (!json) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  json->assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
  return !json->empty();
}

bool ReadPendingFile(std::string* json) { return ReadFileUtf8(ElevatedPendingPath(), json); }

bool ReadPendingFileEx(const std::wstring& path, std::string* json) {
  return ReadFileUtf8(path, json);
}

// In-place direct write fallback for system-protected paths (like hosts)
// or read-only files where MoveFileExW(REPLACE_EXISTING) is blocked by security filter drivers.
bool DirectWriteFileBytes(const std::wstring& path, const void* data, size_t size) {
  DWORD orig_attrs = GetFileAttributesW(path.c_str());
  bool was_readonly = false;
  if (orig_attrs != INVALID_FILE_ATTRIBUTES && (orig_attrs & FILE_ATTRIBUTE_READONLY)) {
    SetFileAttributesW(path.c_str(), orig_attrs & ~FILE_ATTRIBUTE_READONLY);
    was_readonly = true;
  }

  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND) {
    h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                    nullptr);
  }

  if (h == INVALID_HANDLE_VALUE) {
    g_last_write_err = GetLastError();
    if (was_readonly) {
      SetFileAttributesW(path.c_str(), orig_attrs);
    }
    return false;
  }

  SetFilePointer(h, 0, nullptr, FILE_BEGIN);
  DWORD written = 0;
  const BOOL ok =
      WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr);
  const DWORD write_err = ok ? 0 : GetLastError();
  if (ok && written == size) {
    SetEndOfFile(h);
    FlushFileBuffers(h);
  }
  CloseHandle(h);

  if (was_readonly) {
    SetFileAttributesW(path.c_str(), orig_attrs);
  }

  if (!ok || written != size) {
    g_last_write_err = write_err ? write_err : ERROR_WRITE_FAULT;
    return false;
  }
  g_last_write_err = 0;
  return true;
}

}  // namespace

std::wstring ElevatedPendingPath() {
  return JoinPath(NativeDataDir(), kPendingName);
}

std::wstring ElevatedResultPath() {
  return JoinPath(NativeDataDir(), kResultName);
}

bool WriteElevatedResult(const std::wstring& target, unsigned request_id,
                         bool ok) {
  const std::string target_u8 = NarrowUtf8(NormalizePath(target));
  std::string json = "{\n  \"target\": \"";
  json += EscapeJsonUtf8(target_u8);
  json += "\",\n  \"request\": ";
  json += std::to_string(request_id);
  json += ",\n  \"ok\": ";
  json += ok ? "true" : "false";
  json += "\n}\n";
  return AtomicWriteUtf8File(ElevatedResultPath(), json, false);
}

bool ReadElevatedResult(const std::wstring& target, unsigned request_id,
                        bool* ok) {
  std::string json;
  if (!ReadPendingFileEx(ElevatedResultPath(), &json)) {
    return false;
  }
  std::string result_target;
  if (!ExtractJsonStringField(json, "target", &result_target) ||
      !PathsEqual(WidenUtf8(result_target), target)) {
    return false;
  }
  if (request_id != 0) {
    double found_id = 0;
    if (!ExtractJsonNumberField(json, "request", &found_id) ||
        static_cast<unsigned>(found_id) != request_id) {
      return false;
    }
  }
  size_t pos = json.find("\"ok\"");
  if (pos == std::string::npos) {
    return false;
  }
  pos = json.find(':', pos + 4);
  if (pos == std::string::npos) {
    return false;
  }
  const size_t t = json.find("true", pos + 1);
  const size_t f = json.find("false", pos + 1);
  if (t == std::string::npos && f == std::string::npos) {
    return false;
  }
  if (ok) {
    *ok = t != std::string::npos && (f == std::string::npos || t < f);
  }
  return true;
}

bool ClearElevatedResult() {
  return DeleteFileW(ElevatedResultPath().c_str()) != 0 ||
         GetLastError() == ERROR_FILE_NOT_FOUND;
}

bool IsCurrentProcessElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  TOKEN_ELEVATION elev{};
  DWORD len = 0;
  const BOOL ok =
      GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &len);
  CloseHandle(token);
  return ok && elev.TokenIsElevated != 0;
}

bool LastWriteWasAccessDenied() {
  return g_last_write_err == ERROR_ACCESS_DENIED ||
         g_last_write_err == ERROR_SHARING_VIOLATION ||
         g_last_write_err == ERROR_WRITE_PROTECT;
}

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size) {
  g_last_write_err = 0;
  // Write to a unique temp file and replace: CREATE_ALWAYS on the destination
  // truncated the user's note first, so a crash or power loss mid-write left a
  // partial file with the previous content already gone.
  const std::wstring tmp = AtomicTempPathFor(path);
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                         nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    const BOOL ok =
        WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr);
    const DWORD err = ok ? 0 : GetLastError();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && written == size) {
      if (MoveFileExW(tmp.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        g_last_write_err = 0;
        return true;
      }
      g_last_write_err = GetLastError();
      DeleteFileW(tmp.c_str());
      // MoveFileEx blocked (e.g. system hosts file or readonly target).
      // Fallback to in-place direct write.
      if (DirectWriteFileBytes(path, data, size)) {
        return true;
      }
      return false;
    } else {
      g_last_write_err = err ? err : ERROR_WRITE_FAULT;
      DeleteFileW(tmp.c_str());
    }
  } else {
    g_last_write_err = GetLastError();
  }

  // If temp creation failed (e.g. non-elevated or protected folder), try direct write
  // which will record genuine target error (e.g. ERROR_ACCESS_DENIED).
  if (DirectWriteFileBytes(path, data, size)) {
    return true;
  }
  return false;
}

bool WriteElevatedPending(const std::wstring& target,
                          const std::wstring& text) {
  const std::wstring norm = NormalizePath(target);
  const std::string target_u8 = NarrowUtf8(norm);
  const std::string text_u8 = NarrowUtf8(text);
  char stamp[64];
  std::snprintf(stamp, sizeof(stamp), "%.3f", NowUnixS());
  std::string json = "{\n  \"target\": \"";
  json += EscapeJsonUtf8(target_u8);
  json += "\",\n  \"text\": \"";
  json += EscapeJsonUtf8(text_u8);
  json += "\",\n  \"written_at\": ";
  json += stamp;
  json += "\n}\n";
  return AtomicWriteUtf8File(ElevatedPendingPath(), json, false);
}

bool ClearElevatedPending() {
  return DeleteFileW(ElevatedPendingPath().c_str()) != 0 ||
         GetLastError() == ERROR_FILE_NOT_FOUND;
}

void PurgeOrphanElevatedPending() {
  std::string json;
  if (!ReadPendingFile(&json)) {
    return;
  }
  double written_at = 0.0;
  if (!ExtractJsonNumberField(json, "written_at", &written_at) ||
      (NowUnixS() - written_at) > kPendingMaxAgeS) {
    ClearElevatedPending();
  }
}

bool ConsumeElevatedPending(const std::wstring& target) {
  std::string json;
  if (!ReadPendingFile(&json)) {
    return false;
  }
  std::string target_u8;
  std::string text_u8;
  if (!ExtractJsonStringField(json, "target", &target_u8) ||
      !ExtractJsonStringField(json, "text", &text_u8)) {
    return false;
  }
  const std::wstring pending_target = WidenUtf8(target_u8);
  if (!PathsEqual(pending_target, target)) {
    LogLine(L"[elevated] pending target mismatch");
    return false;
  }
  const std::wstring dest = NormalizePath(target);
  std::wstring text = WidenUtf8(text_u8);

  // hosts 必须保持系统友好的无 BOM UTF-8/ASCII 文本：带 BOM 的 hosts 首行
  // 会被部分 Windows 网络解析组件视为无效并导致后续打开/解析异常。
  // 换行统一为 CRLF。
  const bool hosts = IsHostsTarget(dest);
  if (hosts) {
    text = HostsTextForWrite(text);
  }

  TextFileEncoding enc = TextFileEncoding::Utf8;
  {
    std::ifstream in(dest, std::ios::binary);
    if (in) {
      const std::string raw((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
      std::wstring decoded;
      if (!DecodeTextFileBytes(raw, &decoded, &enc)) {
        LogLine(L"[elevated] refuse rewrite: unknown encoding");
        return false;
      }
      // hosts 固定用无 BOM UTF-8；既有 BOM 也要去掉。
      if (hosts) {
        enc = TextFileEncoding::Utf8;
      }
    }
  }

  std::string bytes;
  if (!EncodeTextFileBytes(text, enc, &bytes)) {
    LogLine(L"[elevated] encode failed for target encoding");
    return false;
  }
  if (!WriteFileBytes(dest, bytes.data(), bytes.size())) {
    LogLine(L"[elevated] write failed err=%lu",
            static_cast<unsigned long>(g_last_write_err));
    return false;
  }
  ClearElevatedPending();
  return true;
}

bool RelaunchDeskNoteElevated(const std::wstring& target, unsigned request_id,
                              bool reopen) {
  const std::wstring exe = DeskNoteExePath();
  if (exe.empty() ||
      GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  const std::wstring norm = NormalizePath(target);
  std::wstring params = reopen ? L"--elevated-save-reopen \""
                               : L"--elevated-save \"";
  params += norm;
  params += L"\" ";
  params += std::to_wstring(request_id);

  const size_t slash = exe.find_last_of(L"\\/");
  const std::wstring cwd =
      slash == std::wstring::npos ? L"." : exe.substr(0, slash);

  LogLine(L"[elevated] relaunch runas exe=%s params=%s", exe.c_str(),
          params.c_str());
  const INT_PTR rc = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"runas", exe.c_str(), params.c_str(), cwd.c_str(),
                    SW_HIDE));
  LogLine(L"[elevated] ShellExecuteW rc=%lld", static_cast<long long>(rc));
  // ShellExecuteW returns >32 even for ERROR_CANCELLED (1223). Treat user
  // denial as a launch failure so the caller restores its original state.
  if (rc == kSeCanceled) {
    return false;
  }
  return rc > kSeSuccessThreshold;
}
