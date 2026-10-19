#include "shell_verbs.h"

#include "autostart.h"

#include <Windows.h>

#include <string>

namespace {
constexpr wchar_t kRoot[] =
    L"Software\\Classes\\Directory\\Background\\shell\\DeskTidyNative";

const wchar_t* kVerbKeys[] = {
    L"settings", L"refresh", L"organize", L"notes",
    L"search",   L"shot",    L"todos",    L"peek",
};

bool WriteVerb(const wchar_t* key_suffix, const wchar_t* label,
               const wchar_t* args) {
  const std::wstring key = std::wstring(kRoot) + L"." + key_suffix;
  HKEY hk = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE,
                      nullptr, &hk, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  RegSetValueExW(hk, nullptr, 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(label),
                 static_cast<DWORD>((wcslen(label) + 1) * sizeof(wchar_t)));
  HKEY cmd = nullptr;
  if (RegCreateKeyExW(hk, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &cmd,
                      nullptr) == ERROR_SUCCESS) {
    const std::wstring exe = NativeExePath();
    std::wstring cmdline = L"\"" + exe + L"\" " + args;
    RegSetValueExW(cmd, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(cmdline.c_str()),
                   static_cast<DWORD>((cmdline.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(cmd);
  }
  RegCloseKey(hk);
  return true;
}
}  // namespace

bool RegisterNativeShellVerbs() {
  HKEY root = nullptr;
  RegCreateKeyExW(HKEY_CURRENT_USER, kRoot, 0, nullptr, 0, KEY_WRITE, nullptr,
                  &root, nullptr);
  if (root) {
    const wchar_t* label = L"DeskTidy Native";
    RegSetValueExW(root, nullptr, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(label),
                   static_cast<DWORD>((wcslen(label) + 1) * sizeof(wchar_t)));
    // Cascade submenu under Directory\\Background\\shell
    const wchar_t* sub = L"DesktopBackground\\Shell\\DeskTidyNative";
    (void)sub;
    RegCloseKey(root);
  }
  WriteVerb(L"settings", L"打开设置", L"--settings");
  WriteVerb(L"refresh", L"刷新桌面图标", L"--refresh");
  WriteVerb(L"organize", L"虚拟整理", L"--organize");
  WriteVerb(L"notes", L"打开便签", L"--notes");
  WriteVerb(L"search", L"文件搜索", L"--search");
  WriteVerb(L"shot", L"区域截图", L"--shot");
  WriteVerb(L"todos", L"打开待办", L"--todos");
  WriteVerb(L"peek", L"Peek 分区", L"--peek");
  return true;
}

bool UnregisterNativeShellVerbs() {
  RegDeleteTreeW(HKEY_CURRENT_USER, kRoot);
  for (const wchar_t* k : kVerbKeys) {
    const std::wstring path =
        std::wstring(
            L"Software\\Classes\\Directory\\Background\\shell\\DeskTidyNative.") +
        k;
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  }
  return true;
}
