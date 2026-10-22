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
