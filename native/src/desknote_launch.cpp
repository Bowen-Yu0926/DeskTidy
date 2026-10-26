#include "desknote_launch.h"

#include "paths_util.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shobjidl.h>

#include <string>
#include <vector>

#pragma comment(lib, "propsys.lib")

namespace {

HANDLE g_desknote_mutex = nullptr;
// Match Python instance_lock.NOTEPAD_MUTEX_NAME.
constexpr wchar_t kDeskNoteMutex[] = L"Local\\DeskTidyNotepadInstance";

bool WriteShellLink(const std::wstring& link, const std::wstring& target,
                    const std::wstring& args, const std::wstring& workdir,
                    const wchar_t* desc, const std::wstring& icon) {
  IShellLinkW* psl = nullptr;
  HRESULT hr =
      CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                       IID_IShellLinkW, reinterpret_cast<void**>(&psl));
  if (FAILED(hr) || !psl) {
    return false;
  }
  psl->SetPath(target.c_str());
  if (!args.empty()) {
    psl->SetArguments(args.c_str());
  }
  if (!workdir.empty()) {
    psl->SetWorkingDirectory(workdir.c_str());
  }
  if (desc) {
    psl->SetDescription(desc);
  }
  if (!icon.empty()) {
    psl->SetIconLocation(icon.c_str(), 0);
  }
  psl->SetShowCmd(SW_SHOWNORMAL);
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

std::wstring ModuleDir() {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring p = exe;
  const size_t slash = p.find_last_of(L'\\');
  if (slash != std::wstring::npos) {
    p.resize(slash);
  }
  return p;
}

bool FileExists(const std::wstring& path) {
  return !path.empty() &&
         GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Match Python desknote_icon_path / _icon_location: prefer branded ICO next to
// the exe (or repo assets in portable/dev), never a missing icons\desknote.ico.
std::wstring ResolveDeskNoteIconFile() {
  const std::wstring work = ModuleDir();
  const std::wstring candidates[] = {
      JoinPath(work, L"assets\\desknote_icon.ico"),
      // native/build/Release → ../../../assets
      JoinPath(work, L"..\\..\\..\\assets\\desknote_icon.ico"),
      // native/build/Release → ../../../../assets (extra hop if nested)
      JoinPath(work, L"..\\..\\..\\..\\assets\\desknote_icon.ico"),
  };
  for (const auto& c : candidates) {
    if (FileExists(c)) {
      return c;
    }
  }
  return {};
}

void NotifyShortcutChanged(const std::wstring& link) {
  if (!link.empty()) {
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                   link.c_str(), nullptr);
  }
}

}  // namespace

std::wstring DeskNoteIconPath() {
  const std::wstring ico = ResolveDeskNoteIconFile();
  if (!ico.empty()) {
    return ico;
  }
  // Frozen / post-build: PE embeds IDI_ICON1 from desknote_icon.rc.
  if (DeskNoteExeExists()) {
    return DeskNoteExePath();
  }
  wchar_t host[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, host, MAX_PATH);
  return host;
}

std::wstring DeskNoteExePath() {
  return JoinPath(ModuleDir(), L"deskNote.exe");
}

bool DeskNoteExeExists() {
  return GetFileAttributesW(DeskNoteExePath().c_str()) !=
         INVALID_FILE_ATTRIBUTES;
}

bool AcquireDeskNoteInstanceLock() {
  g_desknote_mutex = CreateMutexW(nullptr, TRUE, kDeskNoteMutex);
  if (!g_desknote_mutex) {
    return false;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(g_desknote_mutex);
    g_desknote_mutex = nullptr;
    return false;
  }
  return true;
}

void ReleaseDeskNoteInstanceLock() {
  if (g_desknote_mutex) {
    ReleaseMutex(g_desknote_mutex);
    CloseHandle(g_desknote_mutex);
    g_desknote_mutex = nullptr;
  }
}

bool IsDeskNoteRunning() {
  HANDLE m = CreateMutexW(nullptr, FALSE, kDeskNoteMutex);
  if (!m) {
    return false;
  }
  const bool exists = (GetLastError() == ERROR_ALREADY_EXISTS);
  CloseHandle(m);
  return exists;
}

bool ForwardToDeskNote(const std::wstring& action) {
  HWND hwnd = FindWindowW(kDeskNoteMsgClass, kDeskNoteMsgTitle);
  if (!hwnd || action.empty()) {
    return false;
  }
  COPYDATASTRUCT cds{};
  cds.dwData = kDeskNoteCliMsg;
  cds.cbData = static_cast<DWORD>((action.size() + 1) * sizeof(wchar_t));
  cds.lpData = const_cast<wchar_t*>(action.c_str());
  SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                      SMTO_ABORTIFHUNG, 4000, nullptr);
  return true;
}

bool LaunchOrFocusDeskNote(const std::vector<std::wstring>& paths) {
  std::wstring action = L"--focus";
  if (!paths.empty()) {
    action = L"--open";
    for (const auto& p : paths) {
      action.push_back(L'\n');
      action += p;
    }
  }

  if (IsDeskNoteRunning() && ForwardToDeskNote(action)) {
    return true;
  }

  const std::wstring exe = DeskNoteExePath();
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    MessageBoxW(nullptr,
                L"未找到 deskNote.exe。\n请重新编译 native（会同时生成 "
                L"deskNote.exe）。",
                L"DeskNote", MB_OK | MB_ICONWARNING);
    return false;
  }

  std::wstring args;
  if (!paths.empty()) {
    for (size_t i = 0; i < paths.size(); ++i) {
      if (i) {
        args += L' ';
      }
      args += L'"';
      args += paths[i];
      args += L'"';
    }
  }

  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"open";
  sei.lpFile = exe.c_str();
  sei.lpParameters = args.empty() ? nullptr : args.c_str();
  sei.lpDirectory = ModuleDir().c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&sei)) {
    return false;
  }
  if (sei.hProcess) {
    // Wait briefly for IPC window so a follow-up focus is reliable.
    for (int i = 0; i < 50; ++i) {
      if (FindWindowW(kDeskNoteMsgClass, kDeskNoteMsgTitle)) {
        break;
      }
      Sleep(40);
    }
    CloseHandle(sei.hProcess);
  }
  if (!paths.empty()) {
    ForwardToDeskNote(action);
  }
  return true;
}

void SyncDeskNoteShortcuts(bool enabled) {
  PWSTR desktop = nullptr;
  PWSTR programs = nullptr;
  std::wstring desk_link;
  std::wstring start_link;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) &&
      desktop) {
    desk_link = JoinPath(desktop, L"DeskNote.lnk");
    CoTaskMemFree(desktop);
  }
  if (SUCCEEDED(
          SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)) &&
      programs) {
    const std::wstring folder = JoinPath(programs, L"DeskTidy");
    CreateDirectoryW(folder.c_str(), nullptr);
    start_link = JoinPath(folder, L"DeskNote.lnk");
    DeleteFileW(JoinPath(programs, L"DeskNote.lnk").c_str());
    CoTaskMemFree(programs);
  }
  if (!enabled) {
    if (!desk_link.empty()) {
      DeleteFileW(desk_link.c_str());
    }
    if (!start_link.empty()) {
      DeleteFileW(start_link.c_str());
    }
    if (!desk_link.empty()) {
      const size_t slash = desk_link.find_last_of(L'\\');
      if (slash != std::wstring::npos) {
        DeleteFileW((desk_link.substr(0, slash + 1) + L"deskNote.lnk").c_str());
      }
    }
    return;
  }

  const std::wstring target = DeskNoteExePath();
  if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
    // Fall back to host exe --notes only if deskNote.exe not built yet.
    wchar_t host[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, host, MAX_PATH);
    std::wstring work = host;
    const size_t slash = work.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
      work.resize(slash);
    }
    const std::wstring icon = DeskNoteIconPath();
    if (!desk_link.empty()) {
      WriteShellLink(desk_link, host, L"--notes", work, L"DeskNote 记事本",
                     icon);
      NotifyShortcutChanged(desk_link);
    }
    if (!start_link.empty()) {
      WriteShellLink(start_link, host, L"--notes", work, L"DeskNote 记事本",
                     icon);
      NotifyShortcutChanged(start_link);
    }
    return;
  }

  const std::wstring work = ModuleDir();
  const std::wstring icon = DeskNoteIconPath();
  if (!desk_link.empty()) {
    WriteShellLink(desk_link, target, L"", work, L"DeskNote 记事本", icon);
    ApplyShortcutAppUserModelId(desk_link, kDeskNoteAumid);
    NotifyShortcutChanged(desk_link);
  }
  if (!start_link.empty()) {
    WriteShellLink(start_link, target, L"", work, L"DeskNote 记事本", icon);
    ApplyShortcutAppUserModelId(start_link, kDeskNoteAumid);
    NotifyShortcutChanged(start_link);
  }
}

bool SetProcessAppUserModelId(const wchar_t* aumid) {
  if (!aumid || !*aumid) {
    return false;
  }
  return SUCCEEDED(SetCurrentProcessExplicitAppUserModelID(aumid));
}

bool ApplyShortcutAppUserModelId(const std::wstring& lnk_path,
                                 const wchar_t* aumid) {
  if (lnk_path.empty() || !aumid || !*aumid) {
    return false;
  }
  if (GetFileAttributesW(lnk_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  IPropertyStore* store = nullptr;
  HRESULT hr = SHGetPropertyStoreFromParsingName(
      lnk_path.c_str(), nullptr, GPS_READWRITE, IID_PPV_ARGS(&store));
  if (FAILED(hr) || !store) {
    return false;
  }
  PROPVARIANT pv{};
  hr = InitPropVariantFromString(aumid, &pv);
  bool ok = false;
  if (SUCCEEDED(hr)) {
    ok = SUCCEEDED(store->SetValue(PKEY_AppUserModel_ID, pv)) &&
         SUCCEEDED(store->Commit());
    PropVariantClear(&pv);
  }
  store->Release();
  return ok;
}
