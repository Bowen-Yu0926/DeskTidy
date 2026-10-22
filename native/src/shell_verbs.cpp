#include "shell_verbs.h"

#include "autostart.h"

#include <Windows.h>

#include <string>

namespace {
constexpr wchar_t kRoot[] =
    L"Software\\Classes\\Directory\\Background\\shell";
constexpr wchar_t kParent[] = L"DeskTidyNative";

struct ChildVerb {
  const wchar_t* key;
  const wchar_t* label;
  const wchar_t* args;
};

// Match Python cascade: one submenu, nested shell\\* children.
// Python parity verbs: new-fence / refresh-fences / show-window. The extra
// C++ verbs below (organize/notes/search/shot/todos/peek) are native-only.
constexpr ChildVerb kChildren[] = {
    {L"new_fence", L"新建分区", L"--new-fence"},
    {L"refresh_fences", L"刷新所有分区", L"--refresh-fences"},
    {L"show_window", L"显示主窗口", L"--show-window"},
    {L"organize", L"一键整理", L"--organize"},
    {L"notes", L"打开便签", L"--notes"},
    {L"search", L"文件搜索", L"--search"},
    {L"shot", L"区域截图", L"--shot"},
    {L"todos", L"打开待办", L"--todos"},
    {L"peek", L"Peek 分区", L"--peek"},
};

// Flat verbs from older Native builds — must be removed so they do not crowd.
constexpr const wchar_t* kObsoleteFlat[] = {
    L"DeskTidyNative.settings", L"DeskTidyNative.refresh",
    L"DeskTidyNative.organize", L"DeskTidyNative.notes",
    L"DeskTidyNative.search",   L"DeskTidyNative.shot",
    L"DeskTidyNative.todos",    L"DeskTidyNative.peek",
};

void DeleteKeyTree(const std::wstring& root) {
  const std::wstring shell_sub = root + L"\\shell";
  HKEY sk = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, shell_sub.c_str(), 0, KEY_READ, &sk) ==
      ERROR_SUCCESS) {
    for (;;) {
      wchar_t name[256] = {};
      DWORD name_len = 256;
      if (RegEnumKeyExW(sk, 0, name, &name_len, nullptr, nullptr, nullptr,
                        nullptr) != ERROR_SUCCESS) {
        break;
      }
      const std::wstring child = shell_sub + L"\\" + name;
      RegDeleteTreeW(HKEY_CURRENT_USER, (child + L"\\command").c_str());
      RegDeleteTreeW(HKEY_CURRENT_USER, child.c_str());
    }
    RegCloseKey(sk);
    RegDeleteTreeW(HKEY_CURRENT_USER, shell_sub.c_str());
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, (root + L"\\command").c_str());
  RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
}

void DeleteFlatObsolete() {
  for (const wchar_t* name : kObsoleteFlat) {
    const std::wstring path = std::wstring(kRoot) + L"\\" + name;
    RegDeleteTreeW(HKEY_CURRENT_USER, (path + L"\\command").c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
  }
  // Also wipe old parent root key used by early Native builds.
  RegDeleteTreeW(HKEY_CURRENT_USER,
                 L"Software\\Classes\\Directory\\Background\\shell\\"
                 L"DeskTidyNative");
}

bool WriteChild(const std::wstring& parent, const ChildVerb& c) {
  const std::wstring child = parent + L"\\shell\\" + c.key;
  HKEY hk = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, child.c_str(), 0, nullptr, 0,
                      KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  RegSetValueExW(hk, nullptr, 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(c.label),
                 static_cast<DWORD>((wcslen(c.label) + 1) * sizeof(wchar_t)));
  RegSetValueExW(hk, L"MUIVerb", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(c.label),
                 static_cast<DWORD>((wcslen(c.label) + 1) * sizeof(wchar_t)));
  RegCloseKey(hk);

  HKEY cmd = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, (child + L"\\command").c_str(), 0,
                      nullptr, 0, KEY_WRITE, nullptr, &cmd,
                      nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const std::wstring cmdline = L"\"" + NativeExePath() + L"\" " + c.args;
  RegSetValueExW(cmd, nullptr, 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(cmdline.c_str()),
                 static_cast<DWORD>((cmdline.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(cmd);
  return true;
}
}  // namespace

bool RegisterNativeShellVerbs() {
  DeleteFlatObsolete();
  const std::wstring parent = std::wstring(kRoot) + L"\\" + kParent;
  DeleteKeyTree(parent);

  HKEY hk = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, parent.c_str(), 0, nullptr, 0,
                      KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const wchar_t* label = L"DeskTidy";
  RegSetValueExW(hk, nullptr, 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(label),
                 static_cast<DWORD>((wcslen(label) + 1) * sizeof(wchar_t)));
  RegSetValueExW(hk, L"MUIVerb", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(label),
                 static_cast<DWORD>((wcslen(label) + 1) * sizeof(wchar_t)));
  // Empty SubCommands → nested shell\\* children (cascade menu).
  const wchar_t empty[] = L"";
  RegSetValueExW(hk, L"SubCommands", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(empty), sizeof(wchar_t));
  const wchar_t pos[] = L"Top";
  RegSetValueExW(hk, L"Position", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(pos),
                 static_cast<DWORD>(sizeof(pos)));
  const std::wstring exe = NativeExePath();
  RegSetValueExW(hk, L"Icon", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(exe.c_str()),
                 static_cast<DWORD>((exe.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(hk);

  for (const auto& c : kChildren) {
    WriteChild(parent, c);
  }
  return true;
}

bool UnregisterNativeShellVerbs() {
  DeleteFlatObsolete();
  DeleteKeyTree(std::wstring(kRoot) + L"\\" + kParent);
  return true;
}

void SyncNativeShellVerbs(bool enabled) {
  if (enabled) {
    RegisterNativeShellVerbs();
  } else {
    UnregisterNativeShellVerbs();
  }
}
