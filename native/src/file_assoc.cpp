#include "file_assoc.h"

#include "autostart.h"
#include "log.h"

#include <Windows.h>

#include <string>

namespace {

constexpr wchar_t kProgId[] = L"DeskTidyNative.Note";
constexpr wchar_t kProgIdLabel[] = L"DeskNote 记事本文档";

// Extensions from Python notepad.py NOTE_OPEN_SUFFIXES (openable by the editor).
constexpr const wchar_t* kExtensions[] = {
    L".txt",    L".text",     L".md",        L".markdown", L".log",
    L".csv",    L".tsv",      L".json",      L".xml",      L".yml",
    L".yaml",   L".toml",     L".ini",       L".cfg",      L".conf",
    L".config", L".env",      L".properties", L".py",      L".pyw",
    L".pyi",    L".js",       L".mjs",       L".cjs",      L".ts",
    L".tsx",    L".jsx",      L".css",       L".scss",     L".less",
    L".html",   L".htm",      L".xhtml",     L".vue",      L".java",
    L".c",      L".h",        L".cpp",       L".hpp",      L".cc",
    L".cs",     L".go",       L".rs",        L".rb",       L".php",
    L".sql",    L".sh",       L".bash",      L".zsh",      L".ps1",
    L".psm1",   L".bat",      L".cmd",       L".vbs",      L".gitignore",
    L".gitattributes", L".editorconfig", L".dockerfile", L".makefile",
    L".cmake",  L".gradle",   L".r",         L".lua",      L".pl",
    L".swift",  L".kt",       L".kts",       L".dart",     L".jsonc",
    L".json5",  L".svg",      L".tex",       L".rst",      L".adoc",
    L".nfo",    L".diz",      L".reg",       L".inf",      L".url",
    L".eml",    L".ics",
};

void SetString(HKEY key, const wchar_t* name, const wchar_t* value) {
  RegSetValueExW(key, name, 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(value),
                 static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
}

// Write the ProgId and its open command.
bool WriteProgId() {
  const std::wstring exe = NativeExePath();
  const std::wstring cmdline = L"\"" + exe + L"\" --notes \"%1\"";

  HKEY prog = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER,
                      (std::wstring(L"Software\\Classes\\") + kProgId).c_str(),
                      0, nullptr, 0, KEY_WRITE, nullptr, &prog,
                      nullptr) != ERROR_SUCCESS) {
    return false;
  }
  SetString(prog, nullptr, kProgIdLabel);
  RegCloseKey(prog);

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

}  // namespace

bool RegisterNoteFileAssociations() {
  if (!WriteProgId()) {
    LogLineA("[assoc] WriteProgId FAILED");
    return false;
  }
  for (const wchar_t* ext : kExtensions) {
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
  // Also register under Applications so "Open with" lists the exe directly.
  HKEY apps = nullptr;
  if (RegCreateKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\Classes\\Applications\\desktidy_native.exe\\shell\\open\\command",
          0, nullptr, 0, KEY_WRITE, nullptr, &apps, nullptr) == ERROR_SUCCESS) {
    const std::wstring exe = NativeExePath();
    const std::wstring cmdline = L"\"" + exe + L"\" --notes \"%1\"";
    SetString(apps, nullptr, cmdline.c_str());
    RegCloseKey(apps);
  }
  LogLineA("[assoc] registered DeskNote file associations");
  return true;
}

bool UnregisterNoteFileAssociations() {
  RegDeleteTreeW(HKEY_CURRENT_USER,
                 (std::wstring(L"Software\\Classes\\") + kProgId).c_str());
  for (const wchar_t* ext : kExtensions) {
    const std::wstring base =
        std::wstring(L"Software\\Classes\\") + ext + L"\\OpenWithProgids";
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, KEY_SET_VALUE, &hk) ==
        ERROR_SUCCESS) {
      RegDeleteValueW(hk, kProgId);
      RegCloseKey(hk);
    }
  }
  RegDeleteTreeW(
      HKEY_CURRENT_USER,
      L"Software\\Classes\\Applications\\desktidy_native.exe\\shell\\open");
  LogLineA("[assoc] unregistered DeskNote file associations");
  return true;
}
