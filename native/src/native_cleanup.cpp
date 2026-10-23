#include "native_cleanup.h"

#include "autostart.h"
#include "file_assoc.h"
#include "log.h"
#include "namespace_icons.h"
#include "paths_util.h"
#include "shell_icons.h"
#include "shell_verbs.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <TlHelp32.h>

#include <string>

namespace {

bool WipeDirectory(const std::wstring& dir) {
  if (dir.empty() ||
      (dir.find(L"DeskTidyNative") == std::wstring::npos &&
       !IsPortableMode())) {
    return false;
  }
  std::wstring from = dir;
  from.push_back(L'\0');
  from.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_ALLOWUNDO;
  return SHFileOperationW(&op) == 0;
}

void KillOtherNativeInstances() {
  const DWORD self = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return;
  }
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (pe.th32ProcessID == self) {
        continue;
      }
      if (_wcsicmp(pe.szExeFile, L"desktidy_native.exe") != 0) {
        continue;
      }
      HANDLE proc =
          OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
      if (proc) {
        TerminateProcess(proc, 0);
        CloseHandle(proc);
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
}

void RemoveDesktopShortcuts() {
  PWSTR desk = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk)) ||
      !desk) {
    return;
  }
  const std::wstring folder = desk;
  CoTaskMemFree(desk);
  const wchar_t* names[] = {L"DeskTidy.lnk", L"DeskTidy Native.lnk",
                            L"Desktidy.lnk"};
  for (const wchar_t* name : names) {
    const std::wstring path = folder + L"\\" + name;
    DeleteFileW(path.c_str());
  }
}

// Remove HKCU Run / StartupApproved values matching DeskTidy legacy + native
// names. The native build normally autostarts via a Startup folder .lnk (see
// autostart.cpp), but old installs / the Python suite may have left Run keys.
void DeleteRunValues() {
  const wchar_t* names[] = {L"DeskTidy Native", L"DeskTidy", L"Desktidy",
                            L"桌面整理（私人版）"};
  const wchar_t* subs[] = {
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\"
      L"Run",
  };
  for (const wchar_t* sub : subs) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
      continue;
    }
    for (const wchar_t* name : names) {
      RegDeleteValueW(key, name);
    }
    RegCloseKey(key);
  }
}

// namespace_icons.cpp exposes RestoreDesktopIconsIfHidden — a state-machine
// restore that returns each hosted icon to its recorded original visibility
// (precise, never guesses defaults). Delegate to it.
void RestoreNamespaceIcons() {
  RestoreDesktopIconsIfHidden();
}

void ClearReadOnlyRecursive(const std::wstring& dir) {
  WIN32_FIND_DATAW fd{};
  const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) {
      continue;
    }
    const std::wstring p = dir + L"\\" + fd.cFileName;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      ClearReadOnlyRecursive(p);
    } else {
      SetFileAttributesW(p.c_str(),
                         fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
}

void DeleteDirBestEffort(const std::wstring& dir) {
  if (dir.empty()) {
    return;
  }
  ClearReadOnlyRecursive(dir);
  std::wstring from = dir;
  from.push_back(L'\0');
  from.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
  SHFileOperationW(&op);
}

void DeleteFileBestEffort(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
  DeleteFileW(path.c_str());
}

// Purge config / cache / snapshots / logs / webview2-* while preserving user
// documents (笔记 / 录屏 / 纪要 / notes / todos / vault).
void CleanNativeDataDir() {
  const std::wstring base = NativeDataDir();
  // P2-2: portable mode uses `<exe>\data` (no DeskTidyNative in path).
  if (base.empty() ||
      (base.find(L"DeskTidyNative") == std::wstring::npos &&
       !IsPortableMode())) {
    LogLineA("[cleanup] refuse to clean suspicious data dir");
    return;
  }
  const wchar_t* dirs[] = {L"logs",      L"snapshots", L"wallpapers",
                           L".public_system", L"settings",  L"md_preview",
                           L"pets"};
  for (const wchar_t* d : dirs) {
    DeleteDirBestEffort(JoinPath(base, d));
  }
  WIN32_FIND_DATAW fd{};
  const HANDLE h = FindFirstFileW((base + L"\\webview2-*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        DeleteDirBestEffort(base + L"\\" + fd.cFileName);
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  const wchar_t* files[] = {
      L"app_prefs.json",
      L"hotkeys.json",
      L"ui_lang.txt",
      L"minutes.json",
      L"page_folders.json",
      L"whitelist.json",
      L"firstrun.done",
      L"preview_note.html",
      L"python_layout_migrated.done",
      L"wallpaper.json",
      L"update_check_cache.json",
      L"last_settings_push.json",
      L"last_settings_push_meta.txt",
      L"last_toggle.txt",
      L"todos_geo.json",
      L"pet.json",
      L"pet.png",
  };
  for (const wchar_t* f : files) {
    DeleteFileBestEffort(JoinPath(base, f));
  }
  LogLine(L"[cleanup] 已清理数据目录（保留 笔记/录屏/纪要 等用户文档）：%s",
          base.c_str());
}

}  // namespace

void RunNativeCleanup(HWND owner) {
  const int wipe = MessageBoxW(
      owner,
      L"卸载清理 DeskTidy Native：\n\n"
      L"将恢复桌面图标、取消右键动词、关闭开机启动，并结束其它实例。\n\n"
      L"是 = 同时删除数据目录（可进回收站）\n"
      L"否 = 仅清理注册项与开机启动\n"
      L"取消 = 返回",
      L"卸载清理", MB_YESNOCANCEL | MB_ICONWARNING);
  if (wipe == IDCANCEL) {
    return;
  }

  KillOtherNativeInstances();
  SetDesktopIconsVisible(true);
  UnregisterNativeShellVerbs();
  SetNativeAutostart(false);
  RemoveDesktopShortcuts();

  if (wipe == IDYES) {
    WipeDirectory(NativeDataDir());
    MessageBoxW(owner,
                L"已恢复桌面图标、清理注册项与开机启动，并尝试删除数据目录。",
                L"卸载清理", MB_OK | MB_ICONINFORMATION);
  } else {
    MessageBoxW(owner,
                L"已恢复桌面图标，并取消右键动词与开机启动。数据目录保留。",
                L"卸载清理", MB_OK | MB_ICONINFORMATION);
  }
}

int RunUninstallCleanup() {
  LogLineA("[uninstall] cleanup started");
  KillOtherNativeInstances();
  LogLineA("[uninstall] other instances stopped");
  SetDesktopIconsVisible(true);
  LogLineA("[uninstall] desktop icons restored");
  RestoreNamespaceIcons();
  LogLineA("[uninstall] namespace icons restored (HideDesktopIcons registry)");
  UnregisterNativeShellVerbs();
  LogLineA("[uninstall] shell verbs unregistered");
  UnregisterNoteFileAssociations();
  LogLineA("[uninstall] note file associations unregistered");
  SetNativeAutostart(false);
  LogLineA("[uninstall] autostart disabled");
  DeleteRunValues();
  LogLineA("[uninstall] Run registry values removed");
  RemoveDesktopShortcuts();
  LogLineA("[uninstall] desktop shortcuts removed");
  CleanNativeDataDir();
  LogLineA("[uninstall] cleanup finished");
  return 0;
}
