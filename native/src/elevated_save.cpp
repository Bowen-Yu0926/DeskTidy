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
constexpr double kPendingMaxAgeS = 3600.0;
constexpr int kSeSuccessThreshold = 32;

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

bool ReadPendingFile(std::string* json) {
  if (!json) {
    return false;
  }
  std::ifstream in(ElevatedPendingPath(), std::ios::binary);
  if (!in) {
    return false;
  }
  json->assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
  return !json->empty();
}

}  // namespace

std::wstring ElevatedPendingPath() {
  return JoinPath(NativeDataDir(), kPendingName);
}

bool LastWriteWasAccessDenied() {
  return g_last_write_err == ERROR_ACCESS_DENIED ||
         g_last_write_err == ERROR_SHARING_VIOLATION ||
         g_last_write_err == ERROR_WRITE_PROTECT;
}

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size) {
  g_last_write_err = 0;
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    g_last_write_err = GetLastError();
    return false;
  }
  DWORD written = 0;
  const BOOL ok =
      WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr);
  const DWORD err = ok ? 0 : GetLastError();
  FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok || written != size) {
    g_last_write_err = err ? err : ERROR_WRITE_FAULT;
    return false;
  }
  return true;
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
  const std::wstring text = WidenUtf8(text_u8);

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

bool RelaunchDeskNoteElevated(const std::wstring& target) {
  const std::wstring exe = DeskNoteExePath();
  if (exe.empty() ||
      GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  const std::wstring norm = NormalizePath(target);
  std::wstring params = L"--elevated-save \"";
  params += norm;
  params += L"\"";

  const size_t slash = exe.find_last_of(L"\\/");
  const std::wstring cwd =
      slash == std::wstring::npos ? L"." : exe.substr(0, slash);

  LogLine(L"[elevated] relaunch runas exe=%s params=%s", exe.c_str(),
          params.c_str());
  const INT_PTR rc = reinterpret_cast<INT_PTR>(
      ShellExecuteW(nullptr, L"runas", exe.c_str(), params.c_str(), cwd.c_str(),
                    SW_SHOWNORMAL));
  LogLine(L"[elevated] ShellExecuteW rc=%lld", static_cast<long long>(rc));
  return rc > kSeSuccessThreshold;
}
