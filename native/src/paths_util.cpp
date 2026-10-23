#include "paths_util.h"

#include "log.h"

#include <Windows.h>
#include <ShlObj.h>
#include <Shellapi.h>

#include <cstring>

namespace {

std::wstring g_exe_dir;
bool g_exe_dir_init = false;

std::wstring ExeDir() {
  if (g_exe_dir_init) {
    return g_exe_dir;
  }
  g_exe_dir_init = true;
  wchar_t mod[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
  std::wstring p(mod, n > 0 ? n : 0);
  const size_t slash = p.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    p.resize(slash);
  }
  g_exe_dir = p;
  return g_exe_dir;
}

bool PortableFileExists() {
  return GetFileAttributesW((ExeDir() + L"\\portable.txt").c_str()) !=
         INVALID_FILE_ATTRIBUTES;
}

bool PortableFlagOnCommandLine() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return false;
  }
  bool found = false;
  for (int i = 0; i < argc; ++i) {
    if (argv[i] && wcscmp(argv[i], L"--portable") == 0) {
      found = true;
      break;
    }
  }
  LocalFree(argv);
  return found;
}

bool g_portable_checked = false;
bool g_portable = false;

// One-shot log of the resolved data root. Deferred to the second NativeDataDir()
// call: the first call happens inside LogInit() before the logger is ready, so
// LogLine would be a no-op. The resolved dir is deterministic, so logging it on
// the following call is equivalent.
bool g_root_first_call = false;
bool g_root_log_done = false;

}  // namespace

std::wstring NativeExeDir() { return ExeDir(); }

bool IsPortableMode() {
  if (!g_portable_checked) {
    g_portable_checked = true;
    g_portable = PortableFlagOnCommandLine() || PortableFileExists();
  }
  return g_portable;
}

std::wstring NativeDataDir() {
  std::wstring dir;
  if (IsPortableMode()) {
    dir = ExeDir() + L"\\data";
  } else {
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                    &appdata)) ||
        !appdata) {
      dir = L".";
    } else {
      dir = appdata;
      CoTaskMemFree(appdata);
      dir += L"\\DeskTidyNative";
    }
  }
  CreateDirectoryW(dir.c_str(), nullptr);

  if (!g_root_log_done) {
    if (g_root_first_call) {
      // Set before LogLine: LogLine -> LogDirPath -> NativeDataDir recursion.
      g_root_log_done = true;
      LogLine(L"[paths] data root = %s (portable=%s)", dir.c_str(),
              IsPortableMode() ? L"yes" : L"no");
    } else {
      g_root_first_call = true;
    }
  }
  return dir;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) {
    return b;
  }
  if (a.back() == L'\\' || a.back() == L'/') {
    return a + b;
  }
  return a + L"\\" + b;
}

std::string NarrowUtf8(const std::wstring& ws) {
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

std::wstring WidenUtf8(const std::string& s) {
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

namespace {

bool DecodeCodePage(UINT cp, DWORD flags, const char* data, int len,
                    std::wstring* out) {
  if (!out || len < 0) {
    return false;
  }
  if (len == 0) {
    out->clear();
    return true;
  }
  const int n = MultiByteToWideChar(cp, flags, data, len, nullptr, 0);
  if (n <= 0) {
    return false;
  }
  out->assign(static_cast<size_t>(n), L'\0');
  return MultiByteToWideChar(cp, flags, data, len, out->data(), n) > 0;
}

bool EncodeCodePage(UINT cp, const std::wstring& text, std::string* out) {
  if (!out) {
    return false;
  }
  if (text.empty()) {
    out->clear();
    return true;
  }
  const int n = WideCharToMultiByte(cp, 0, text.c_str(),
                                    static_cast<int>(text.size()), nullptr, 0,
                                    nullptr, nullptr);
  if (n <= 0) {
    return false;
  }
  out->assign(static_cast<size_t>(n), '\0');
  return WideCharToMultiByte(cp, 0, text.c_str(), static_cast<int>(text.size()),
                             out->data(), n, nullptr, nullptr) > 0;
}

}  // namespace

bool DecodeTextFileBytes(const std::string& raw, std::wstring* out,
                         TextFileEncoding* detected) {
  if (!out) {
    return false;
  }
  TextFileEncoding enc = TextFileEncoding::Utf8;
  if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
      static_cast<unsigned char>(raw[1]) == 0xBB &&
      static_cast<unsigned char>(raw[2]) == 0xBF) {
    if (DecodeCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data() + 3,
                       static_cast<int>(raw.size() - 3), out)) {
      enc = TextFileEncoding::Utf8Bom;
      if (detected) {
        *detected = enc;
      }
      return true;
    }
  }
  if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF &&
      static_cast<unsigned char>(raw[1]) == 0xFE) {
    const size_t n = (raw.size() - 2) / 2;
    out->resize(n);
    std::memcpy(out->data(), raw.data() + 2, n * sizeof(wchar_t));
    enc = TextFileEncoding::Utf16Le;
    if (detected) {
      *detected = enc;
    }
    return true;
  }
  if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFE &&
      static_cast<unsigned char>(raw[1]) == 0xFF) {
    const size_t n = (raw.size() - 2) / 2;
    out->resize(n);
    for (size_t i = 0; i < n; ++i) {
      const unsigned char hi =
          static_cast<unsigned char>(raw[2 + i * 2]);
      const unsigned char lo =
          static_cast<unsigned char>(raw[2 + i * 2 + 1]);
      (*out)[i] = static_cast<wchar_t>((hi << 8) | lo);
    }
    enc = TextFileEncoding::Utf16Be;
    if (detected) {
      *detected = enc;
    }
    return true;
  }
  if (DecodeCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(),
                     static_cast<int>(raw.size()), out)) {
    enc = TextFileEncoding::Utf8;
    if (detected) {
      *detected = enc;
    }
    return true;
  }
  if (DecodeCodePage(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()),
                     out)) {
    enc = TextFileEncoding::Acp;
    if (detected) {
      *detected = enc;
    }
    return true;
  }
  return false;
}

bool EncodeTextFileBytes(const std::wstring& text, TextFileEncoding enc,
                         std::string* out) {
  if (!out) {
    return false;
  }
  switch (enc) {
    case TextFileEncoding::Utf8Bom: {
      std::string body;
      if (!EncodeCodePage(CP_UTF8, text, &body)) {
        return false;
      }
      out->assign("\xEF\xBB\xBF");
      out->append(body);
      return true;
    }
    case TextFileEncoding::Utf8:
      return EncodeCodePage(CP_UTF8, text, out);
    case TextFileEncoding::Acp:
      return EncodeCodePage(CP_ACP, text, out);
    case TextFileEncoding::Utf16Le: {
      out->assign("\xFF\xFE", 2);
      out->append(reinterpret_cast<const char*>(text.data()),
                  text.size() * sizeof(wchar_t));
      return true;
    }
    case TextFileEncoding::Utf16Be: {
      out->assign("\xFE\xFF", 2);
      for (wchar_t ch : text) {
        out->push_back(static_cast<char>((ch >> 8) & 0xFF));
        out->push_back(static_cast<char>(ch & 0xFF));
      }
      return true;
    }
  }
  return false;
}

bool AtomicWriteUtf8File(const std::wstring& path, const std::string& utf8,
                         bool keep_bak) {
  if (path.empty()) {
    return false;
  }
  const std::wstring tmp = path + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok =
      WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok || written != utf8.size()) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  if (keep_bak) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
        !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        (fad.nFileSizeHigh != 0 || fad.nFileSizeLow != 0)) {
      CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
    }
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}
