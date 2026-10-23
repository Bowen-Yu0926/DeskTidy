#include "autostart.h"

#include "log.h"

#include <ShlObj.h>
#include <shobjidl.h>

#include <string>

namespace {
// U5: mirror Python settings.py — HKCU Run value named "DeskTidy" is the single
// source of truth (APP_NAME). Legacy Startup-folder shortcuts are migrated away.
constexpr wchar_t kRunValueName[] = L"DeskTidy";
constexpr wchar_t kShortcutName[] = L"DeskTidy Native.lnk";
constexpr wchar_t kRunKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

// Legacy shortcuts to clean (Python LEGACY_APP_NAMES + the old native lnk).
constexpr const wchar_t* kLegacyShortcutNames[] = {
    L"DeskTidy Native.lnk", L"DeskTidy.lnk", L"Desktidy.lnk",
    L"桌面整理（私人版）.lnk", L"DeskTidy-桌面守护.lnk",
    L"Desktidy-桌面守护.lnk", L"桌面整理（私人版）-桌面守护.lnk",
};

std::wstring StartupDir() {
  PWSTR p = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p)) || !p) {
    return {};
  }
  std::wstring out = p;
  CoTaskMemFree(p);
  return out;
}

void SetRunValue(const std::wstring& command) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS ||
      !key) {
    LogLine(L"[autostart] cannot open Run key");
    return;
  }
  const std::wstring data = command;
  const LONG rc = RegSetValueExW(
      key, kRunValueName, 0, REG_SZ,
      reinterpret_cast<const BYTE*>(data.c_str()),
      static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  LogLine(L"[autostart] Run value write rc=%d", static_cast<int>(rc));
}

void RemoveRunValue() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) !=
      ERROR_SUCCESS) {
    return;
  }
  RegDeleteValueW(key, kRunValueName);
  RegCloseKey(key);
}

// StartupApproved\Run\<name>: write an "enabled" entry so Task Manager's startup
// tab lists it as on (Python removes legacy entries; an explicit enabled flag is
// harmless and keeps the toggle state consistent with the Run value).
void WriteApprovedEnabled() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kApprovedKeyPath, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS ||
      !key) {
    return;
  }
  const BYTE approved[12] = {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  RegSetValueExW(key, kRunValueName, 0, REG_BINARY, approved,
                 sizeof(approved));
  RegCloseKey(key);
}

void RemoveApproved() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKeyPath, 0, KEY_SET_VALUE,
                    &key) != ERROR_SUCCESS) {
    return;
  }
  RegDeleteValueW(key, kRunValueName);
  RegCloseKey(key);
}

// Also drop StartupApproved entries left by older Python / native names.
void RemoveLegacyApproved() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKeyPath, 0, KEY_SET_VALUE,
                    &key) != ERROR_SUCCESS) {
    return;
  }
  static const wchar_t* kLegacy[] = {
      L"DeskTidy Native", L"Desktidy", L"桌面整理（私人版）",
      L"DeskTidy-桌面守护", L"Desktidy-桌面守护",
  };
  for (const wchar_t* name : kLegacy) {
    RegDeleteValueW(key, name);
  }
  RegCloseKey(key);
}

void RemoveLegacyShortcuts() {
  const std::wstring dir = StartupDir();
  if (dir.empty()) {
    return;
  }
  for (const wchar_t* name : kLegacyShortcutNames) {
    const std::wstring link = dir + L"\\" + name;
    const DWORD attrs = GetFileAttributesW(link.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      DeleteFileW(link.c_str());
      LogLine(L"[autostart] removed legacy shortcut %s", name);
    }
  }
}

bool RunValuePresent() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) !=
      ERROR_SUCCESS) {
    return false;
  }
  const LONG rc = RegQueryValueExW(key, kRunValueName, nullptr, nullptr, nullptr,
                                   nullptr);
  RegCloseKey(key);
  return rc == ERROR_SUCCESS;
}
}  // namespace

std::wstring NativeExePath() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return path;
}

bool IsNativeAutostartEnabled() {
  // U5: state = HKCU Run value presence (Python is_auto_start_enabled). Fall
  // back to the legacy Startup lnk only for migration detection on old installs.
  if (RunValuePresent()) {
    return true;
  }
  const std::wstring dir = StartupDir();
  if (dir.empty()) {
    return false;
  }
  const std::wstring link = dir + L"\\" + kShortcutName;
  const DWORD attrs = GetFileAttributesW(link.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool SetNativeAutostart(bool enable) {
  if (!enable) {
    RemoveRunValue();
    RemoveApproved();
    RemoveLegacyApproved();
    RemoveLegacyShortcuts();
    return true;
  }

  const std::wstring exe = NativeExePath();
  const std::wstring command = L"\"" + exe + L"\"";
  SetRunValue(command);
  WriteApprovedEnabled();
  RemoveLegacyShortcuts();
  RemoveLegacyApproved();
  return RunValuePresent();
}

// P2-6: Python shipped a Startup-folder shortcut named "DeskTidy.lnk" (or
// "Desktidy.lnk"); its presence is the legacy enabled flag and must be read
// before it is deleted.
bool PythonStartupShortcutPresent() {
  const std::wstring dir = StartupDir();
  if (dir.empty()) {
    return false;
  }
  static const wchar_t* kPythonNames[] = {L"DeskTidy.lnk", L"Desktidy.lnk"};
  for (const wchar_t* name : kPythonNames) {
    const std::wstring link = dir + L"\\" + name;
    const DWORD attrs = GetFileAttributesW(link.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      return true;
    }
  }
  return false;
}

void MigratePythonAutostartOnce() {
  // Capture the legacy enabled state FIRST — deleting the Python lnk before
  // reading it would lose the flag and skip the Run retarget.
  const bool run_present = RunValuePresent();
  const bool python_lnk = PythonStartupShortcutPresent();
  const bool native_lnk = IsNativeAutostartEnabled();
  const bool was_enabled = run_present || python_lnk || native_lnk;

  RemoveLegacyShortcuts();
  RemoveLegacyApproved();

  // If the user already had DeskTidy autostart (Run value or a leftover lnk),
  // retarget Run to this native exe so Task Manager stays consistent.
  if (was_enabled) {
    const std::wstring exe = NativeExePath();
    SetRunValue(L"\"" + exe + L"\"");
    WriteApprovedEnabled();
    LogLine(L"[autostart] migrated Run value → %s (run=%d py_lnk=%d)",
            exe.c_str(), run_present ? 1 : 0, python_lnk ? 1 : 0);
  }
}
