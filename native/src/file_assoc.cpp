#include "file_assoc.h"

#include "desknote_launch.h"
#include "log.h"

#include <Windows.h>

#include <string>

namespace {

// Align with Python desknote_open_with: ProgId only for .md/.markdown.
constexpr wchar_t kProgId[] = L"DeskNote.Document";
constexpr wchar_t kLegacyProgId[] = L"DeskTidyNative.Note";
constexpr wchar_t kProgIdLabel[] = L"DeskNote 文档";
constexpr wchar_t kAppKey[] = L"deskNote.exe";

constexpr const wchar_t* kProgIdSuffixes[] = {L".md", L".markdown"};

// Broader list for Applications\\SupportedTypes (Open with → more apps).
constexpr const wchar_t* kSupportedTypes[] = {
    L".md",   L".markdown", L".txt",  L".text", L".log",  L".csv",
    L".tsv",  L".json",     L".xml",  L".yml",  L".yaml", L".toml",
    L".ini",  L".cfg",      L".conf", L".py",   L".pyw",  L".js",
    L".ts",   L".tsx",      L".jsx",  L".html", L".htm",  L".css",
    L".vue",  L".java",     L".c",    L".h",    L".cpp",  L".cs",
    L".go",   L".rs",       L".sql",  L".bat",  L".cmd",  L".ps1",
    L".sh",
};

void SetString(HKEY key, const wchar_t* name, const wchar_t* value) {
  RegSetValueExW(key, name, 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(value),
                 static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
}

std::wstring NoteExe() {
  if (DeskNoteExeExists()) {
    return DeskNoteExePath();
  }
  // Fallback: same folder as caller (DeskTidy) expecting sibling later.
  return DeskNoteExePath();
}

bool WriteProgId() {
  const std::wstring exe = NoteExe();
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    LogLineA("[assoc] deskNote.exe missing — skip ProgId");
    return false;
  }
  const std::wstring cmdline = L"\"" + exe + L"\" \"%1\"";

  HKEY prog = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER,
                      (std::wstring(L"Software\\Classes\\") + kProgId).c_str(),
                      0, nullptr, 0, KEY_WRITE, nullptr, &prog,
                      nullptr) != ERROR_SUCCESS) {
    return false;
  }
  SetString(prog, nullptr, kProgIdLabel);
  SetString(prog, L"FriendlyTypeName", kProgIdLabel);
  RegCloseKey(prog);

  HKEY icon = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER,
                      (std::wstring(L"Software\\Classes\\") + kProgId +
                       L"\\DefaultIcon")
                          .c_str(),
                      0, nullptr, 0, KEY_WRITE, nullptr, &icon,
                      nullptr) == ERROR_SUCCESS) {
    // Prefer branded ICO / embedded PE icon (Python desknote_open_with).
    const std::wstring ico = DeskNoteIconPath() + L",0";
    SetString(icon, nullptr, ico.c_str());
    RegCloseKey(icon);
  }

  HKEY cmd = nullptr;
  if (RegCreateKeyExW(
          HKEY_CURRENT_USER,
          (std::wstring(L"Software\\Classes\\") + kProgId +
           L"\\shell\\open\\command")
              .c_str(),
          0, nullptr, 0, KEY_WRITE, nullptr, &cmd, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  SetString(cmd, nullptr, cmdline.c_str());
  RegCloseKey(cmd);
  return true;
}

void WriteOpenWithProgids() {
  for (const wchar_t* ext : kProgIdSuffixes) {
    const std::wstring base =
        std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids";
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
      const wchar_t empty[] = L"";
      RegSetValueExW(hk, kProgId, 0, REG_SZ,
                     reinterpret_cast<const BYTE*>(empty), sizeof(wchar_t));
      RegCloseKey(hk);
    }
  }
}

void WriteApplicationsKey() {
  const std::wstring exe = NoteExe();
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return;
  }
  const std::wstring root =
      std::wstring(L"Software\\Classes\\Applications\\") + kAppKey;

  HKEY app = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, root.c_str(), 0, nullptr, 0, KEY_WRITE,
                      nullptr, &app, nullptr) == ERROR_SUCCESS) {
    SetString(app, L"FriendlyAppName", L"DeskNote");
    RegCloseKey(app);
  }

  HKEY cmd = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER,
                      (root + L"\\shell\\open\\command").c_str(), 0, nullptr, 0,
                      KEY_WRITE, nullptr, &cmd, nullptr) == ERROR_SUCCESS) {
    const std::wstring cmdline = L"\"" + exe + L"\" \"%1\"";
    SetString(cmd, nullptr, cmdline.c_str());
    RegCloseKey(cmd);
  }

  HKEY types = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, (root + L"\\SupportedTypes").c_str(), 0,
                      nullptr, 0, KEY_WRITE, nullptr, &types,
                      nullptr) == ERROR_SUCCESS) {
    const wchar_t empty[] = L"";
    for (const wchar_t* ext : kSupportedTypes) {
      RegSetValueExW(types, ext, 0, REG_SZ,
                     reinterpret_cast<const BYTE*>(empty), sizeof(wchar_t));
    }
    RegCloseKey(types);
  }

  // App Paths so Explorer resolves deskNote.exe.
  HKEY paths = nullptr;
  if (RegCreateKeyExW(
          HKEY_CURRENT_USER,
          (std::wstring(
               L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") +
           kAppKey)
              .c_str(),
          0, nullptr, 0, KEY_WRITE, nullptr, &paths, nullptr) == ERROR_SUCCESS) {
    SetString(paths, nullptr, exe.c_str());
    const size_t slash = exe.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
      SetString(paths, L"Path", exe.substr(0, slash).c_str());
    }
    RegCloseKey(paths);
  }
}

void ClearLegacy() {
  RegDeleteTreeW(HKEY_CURRENT_USER,
                 (std::wstring(L"Software\\Classes\\") + kLegacyProgId).c_str());
  RegDeleteTreeW(
      HKEY_CURRENT_USER,
      L"Software\\Classes\\Applications\\desktidy_native.exe");
  // Remove legacy ProgId from every OpenWithProgids we may have polluted.
  static const wchar_t* kLegacyExt[] = {
      L".txt", L".text", L".md", L".markdown", L".log", L".py", L".js",
  };
  for (const wchar_t* ext : kLegacyExt) {
    const std::wstring base =
        std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids";
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, KEY_SET_VALUE, &hk) ==
        ERROR_SUCCESS) {
      RegDeleteValueW(hk, kLegacyProgId);
      RegCloseKey(hk);
    }
  }
}

}  // namespace

bool RegisterNoteFileAssociations() {
  ClearLegacy();
  if (!WriteProgId()) {
    LogLineA("[assoc] WriteProgId FAILED (deskNote.exe missing?)");
    return false;
  }
  WriteOpenWithProgids();
  WriteApplicationsKey();
  LogLineA("[assoc] registered DeskNote.Document + Applications\\deskNote.exe");
  return true;
}

bool UnregisterNoteFileAssociations() {
  ClearLegacy();
  RegDeleteTreeW(HKEY_CURRENT_USER,
                 (std::wstring(L"Software\\Classes\\") + kProgId).c_str());
  for (const wchar_t* ext : kProgIdSuffixes) {
    const std::wstring base =
        std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids";
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, KEY_SET_VALUE, &hk) ==
        ERROR_SUCCESS) {
      RegDeleteValueW(hk, kProgId);
      RegCloseKey(hk);
    }
  }
  RegDeleteTreeW(HKEY_CURRENT_USER,
                 (std::wstring(L"Software\\Classes\\Applications\\") + kAppKey)
                     .c_str());
  RegDeleteTreeW(
      HKEY_CURRENT_USER,
      (std::wstring(
           L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") +
       kAppKey)
          .c_str());
  LogLineA("[assoc] unregistered DeskNote file associations");
  return true;
}
