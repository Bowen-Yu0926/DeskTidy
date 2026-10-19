#include "autostart.h"

#include <ShlObj.h>
#include <shobjidl.h>

#include <string>

namespace {
constexpr wchar_t kShortcutName[] = L"DeskTidy Native.lnk";

std::wstring StartupDir() {
  PWSTR p = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p)) || !p) {
    return {};
  }
  std::wstring out = p;
  CoTaskMemFree(p);
  return out;
}

std::wstring ShortcutPath() {
  const std::wstring dir = StartupDir();
  if (dir.empty()) {
    return {};
  }
  return dir + L"\\" + kShortcutName;
}
}  // namespace

std::wstring NativeExePath() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return path;
}

bool IsNativeAutostartEnabled() {
  const std::wstring link = ShortcutPath();
  if (link.empty()) {
    return false;
  }
  const DWORD attrs = GetFileAttributesW(link.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool SetNativeAutostart(bool enable) {
  const std::wstring link = ShortcutPath();
  if (link.empty()) {
    return false;
  }
  if (!enable) {
    return DeleteFileW(link.c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
  }

  const std::wstring exe = NativeExePath();
  IShellLinkW* psl = nullptr;
  HRESULT hr =
      CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                       IID_IShellLinkW, reinterpret_cast<void**>(&psl));
  if (FAILED(hr) || !psl) {
    return false;
  }
  psl->SetPath(exe.c_str());
  const size_t slash = exe.find_last_of(L'\\');
  if (slash != std::wstring::npos) {
    psl->SetWorkingDirectory(exe.substr(0, slash).c_str());
  }
  psl->SetDescription(L"DeskTidy Native");

  IPersistFile* ppf = nullptr;
  hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
  bool ok = false;
  if (SUCCEEDED(hr) && ppf) {
    ok = SUCCEEDED(ppf->Save(link.c_str(), TRUE));
    ppf->Release();
  }
  psl->Release();
  return ok;
}
