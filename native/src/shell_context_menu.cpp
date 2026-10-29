#include "shell_context_menu.h"

#include "log.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#ifndef CMIC_MASK_UNICODE
#define CMIC_MASK_UNICODE 0x00004000
#endif
#ifndef CMIC_MASK_PTINVOKE
#define CMIC_MASK_PTINVOKE 0x20000000
#endif
#ifndef CMF_EXTENDEDVERBS
#define CMF_EXTENDEDVERBS 0x00000100
#endif

namespace {

constexpr UINT kShellIdFirst = 100;
constexpr UINT kShellIdLast = 0x7FFF;
constexpr UINT kCustomIdFirst = 1;
constexpr UINT kCustomIdLast = 99;
// GCS_VERBW / GCS_VERBA (GetCommandString uType).
constexpr UINT kGcsVerbW = 4;
constexpr UINT kGcsVerbA = 0;

IContextMenu2* g_pcm2 = nullptr;
IContextMenu3* g_pcm3 = nullptr;
HWND g_host = nullptr;
bool g_host_class = false;

LRESULT CALLBACK MenuHostProc(HWND hwnd, UINT msg, WPARAM wparam,
                              LPARAM lparam) {
  if (g_pcm3) {
    LRESULT lr = 0;
    if (SUCCEEDED(g_pcm3->HandleMenuMsg2(msg, wparam, lparam, &lr))) {
      return lr;
    }
  } else if (g_pcm2) {
    if (SUCCEEDED(g_pcm2->HandleMenuMsg(msg, wparam, lparam))) {
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND EnsureMenuHost(HINSTANCE instance) {
  if (g_host && IsWindow(g_host)) {
    return g_host;
  }
  if (!g_host_class) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MenuHostProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"DeskTidyNativeShellMenuHost";
    RegisterClassExW(&wc);
    g_host_class = true;
  }
  // WS_EX_TOOLWINDOW so SetForegroundWindow(host) counts as desktop chrome.
  g_host = CreateWindowExW(0x80 /*WS_EX_TOOLWINDOW*/, L"DeskTidyNativeShellMenuHost",
                           L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                           nullptr);
  return g_host;
}

std::wstring ParentDir(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

std::wstring BaseName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool SameParent(const std::vector<std::wstring>& paths) {
  if (paths.size() <= 1) {
    return true;
  }
  const std::wstring parent = ParentDir(paths[0]);
  for (size_t i = 1; i < paths.size(); ++i) {
    if (_wcsicmp(ParentDir(paths[i]).c_str(), parent.c_str()) != 0) {
      return false;
    }
  }
  return true;
}

// Bind a directory path to IShellFolder (via the desktop folder). Caller owns
// the returned reference.
HRESULT BindFolderShellFolder(const std::wstring& folder,
                              IShellFolder** out) {
  *out = nullptr;
  PIDLIST_ABSOLUTE pidl = nullptr;
  SFGAOF attrs = 0;
  HRESULT hr = SHParseDisplayName(folder.c_str(), nullptr, &pidl, 0, &attrs);
  if (FAILED(hr) || !pidl) {
    return FAILED(hr) ? hr : E_FAIL;
  }
  IShellFolder* desktop = nullptr;
  hr = SHGetDesktopFolder(&desktop);
  if (FAILED(hr) || !desktop) {
    CoTaskMemFree(pidl);
    return FAILED(hr) ? hr : E_FAIL;
  }
  hr = desktop->BindToObject(pidl, nullptr, IID_IShellFolder,
                             reinterpret_cast<void**>(out));
  CoTaskMemFree(pidl);
  desktop->Release();
  return hr;
}

HRESULT AcquireContextMenu(HWND host, const std::vector<std::wstring>& paths,
                           IContextMenu** out) {
  *out = nullptr;
  if (paths.empty()) {
    return E_INVALIDARG;
  }

  std::vector<std::wstring> use = paths;
  if (use.size() > 1 && !SameParent(use)) {
    use.resize(1);
  }

  // Single path via SHBindToParent (works for desktop special items too).
  if (use.size() == 1) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF attrs = 0;
    HRESULT hr =
        SHParseDisplayName(use[0].c_str(), nullptr, &pidl, 0, &attrs);
    if (FAILED(hr) || !pidl) {
      return FAILED(hr) ? hr : E_FAIL;
    }
    IShellFolder* psf = nullptr;
    PCUITEMID_CHILD child = nullptr;
    hr = SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&psf),
                        &child);
    if (FAILED(hr) || !psf || !child) {
      CoTaskMemFree(pidl);
      return FAILED(hr) ? hr : E_FAIL;
    }
    hr = psf->GetUIObjectOf(host, 1, &child, IID_IContextMenu, nullptr,
                            reinterpret_cast<void**>(out));
    psf->Release();
    CoTaskMemFree(pidl);
    return hr;
  }

  const std::wstring parent = ParentDir(use[0]);
  if (parent.empty()) {
    return E_FAIL;
  }

  IShellFolder* folder = nullptr;
  HRESULT hr = BindFolderShellFolder(parent, &folder);
  if (FAILED(hr) || !folder) {
    return FAILED(hr) ? hr : E_FAIL;
  }

  std::vector<PITEMID_CHILD> children;
  children.reserve(use.size());
  for (const auto& p : use) {
    std::wstring name = BaseName(p);
    std::vector<wchar_t> mutable_name(name.begin(), name.end());
    mutable_name.push_back(L'\0');
    PIDLIST_RELATIVE child = nullptr;
    ULONG eaten = 0;
    hr = folder->ParseDisplayName(host, nullptr, mutable_name.data(), &eaten,
                                  &child, nullptr);
    if (FAILED(hr) || !child) {
      for (auto* c : children) {
        CoTaskMemFree(c);
      }
      folder->Release();
      return FAILED(hr) ? hr : E_FAIL;
    }
    children.push_back(child);
  }

  std::vector<PCUITEMID_CHILD> apidl;
  apidl.reserve(children.size());
  for (auto* c : children) {
    apidl.push_back(c);
  }
  hr = folder->GetUIObjectOf(host, static_cast<UINT>(apidl.size()),
                             apidl.data(), IID_IContextMenu, nullptr,
                             reinterpret_cast<void**>(out));
  for (auto* c : children) {
    CoTaskMemFree(c);
  }
  folder->Release();
  return hr;
}

// Explorer folder-background menu: CreateViewObject(IID_IContextMenu) — the
// same source Explorer uses for blank-area RMB (New / Paste / View /
// Personalize / Refresh / Undo ...).
HRESULT AcquireBackgroundContextMenu(HWND host, const std::wstring& folder,
                                     IContextMenu** out) {
  *out = nullptr;
  IShellFolder* f = nullptr;
  HRESULT hr = BindFolderShellFolder(folder, &f);
  if (FAILED(hr) || !f) {
    return FAILED(hr) ? hr : E_FAIL;
  }
  hr = f->CreateViewObject(host, IID_IContextMenu, reinterpret_cast<void**>(out));
  f->Release();
  return hr;
}

std::wstring CanonicalVerb(IContextMenu* pcm, UINT offset) {
  wchar_t buf[512]{};
  if (SUCCEEDED(pcm->GetCommandString(offset, kGcsVerbW, nullptr,
                                      reinterpret_cast<LPSTR>(buf), 511)) &&
      buf[0]) {
    std::wstring v(buf);
    std::transform(v.begin(), v.end(), v.begin(),
                   [](wchar_t c) { return towlower(c); });
    return v;
  }
  char abuf[512]{};
  if (SUCCEEDED(pcm->GetCommandString(offset, kGcsVerbA, nullptr, abuf, 511)) &&
      abuf[0]) {
    std::wstring v(abuf, abuf + strlen(abuf));
    std::transform(v.begin(), v.end(), v.begin(),
                   [](wchar_t c) { return towlower(c); });
    return v;
  }
  return L"";
}

int TrackCommon(HWND owner, POINT screen, IContextMenu* pcm, UINT cmf_flags,
                bool can_rename, const std::vector<ShellMenuExtra>& extras,
                const ShellVerbFn& on_verb,
                const ShellBeforeInvokeFn& on_before_invoke) {
  HMENU menu = CreatePopupMenu();
  if (!menu) {
    return -1;
  }

  // DeskTidy extras at the top (ids 1..99), then a separator, then shell items.
  UINT index = 0;
  for (const auto& ex : extras) {
    if (ex.id < static_cast<int>(kCustomIdFirst) ||
        ex.id > static_cast<int>(kCustomIdLast) || ex.label.empty()) {
      continue;
    }
    UINT flags = MF_STRING;
    if (!ex.enabled) {
      flags |= MF_GRAYED;
    }
    if (ex.checked) {
      flags |= MF_CHECKED;
    }
    AppendMenuW(menu, flags, static_cast<UINT>(ex.id), ex.label.c_str());
    ++index;
  }
  if (!extras.empty()) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  }

  if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
    cmf_flags |= CMF_EXTENDEDVERBS;
  }

  if (FAILED(pcm->QueryContextMenu(menu, GetMenuItemCount(menu), kShellIdFirst,
                                   kShellIdLast, cmf_flags))) {
    DestroyMenu(menu);
    return -1;
  }

  pcm->QueryInterface(IID_IContextMenu2, reinterpret_cast<void**>(&g_pcm2));
  pcm->QueryInterface(IID_IContextMenu3, reinterpret_cast<void**>(&g_pcm3));

  SetForegroundWindow(owner);
  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y,
                     0, g_host ? g_host : owner, nullptr);
  PostMessageW(owner, WM_NULL, 0, 0);

  int result = 0;
  if (cmd >= static_cast<int>(kCustomIdFirst) &&
      cmd <= static_cast<int>(kCustomIdLast)) {
    result = cmd;
  } else if (cmd >= static_cast<int>(kShellIdFirst)) {
    const UINT offset = static_cast<UINT>(cmd - kShellIdFirst);
    const std::wstring verb = CanonicalVerb(pcm, offset);
    // CMF_CANRENAME: the host must present rename UI — never InvokeCommand.
    if (can_rename && verb == L"rename") {
      if (on_verb) {
        on_verb(verb);
      }
      LogLine(L"[menu] shell verb=rename (host handles inline rename)");
      result = 0;
    } else {
      if (on_before_invoke) {
        on_before_invoke();
      }
      CMINVOKECOMMANDINFO info{};
      info.cbSize = sizeof(info);
      info.fMask = 0;
      info.hwnd = owner;
      info.lpVerb = MAKEINTRESOURCEA(offset);
      info.nShow = SW_SHOWNORMAL;
      pcm->InvokeCommand(&info);
      LogLine(L"[menu] shell verb=%s invoked", verb.c_str());
      // After InvokeCommand: paste/newfolder/undo land files first; refreshing
      // beforehand left fences empty until a manual 刷新 (Fences parity).
      if (on_verb) {
        on_verb(verb);
      }
      result = 0;
    }
  }

  if (g_pcm3) {
    g_pcm3->Release();
    g_pcm3 = nullptr;
  }
  if (g_pcm2) {
    g_pcm2->Release();
    g_pcm2 = nullptr;
  }
  DestroyMenu(menu);
  return result;
}

}  // namespace

int TrackShellContextMenu(HWND owner, POINT screen,
                          const std::vector<std::wstring>& paths,
                          const std::vector<ShellMenuExtra>& extras,
                          ShellVerbFn on_verb,
                          ShellBeforeInvokeFn on_before_invoke) {
  if (paths.empty()) {
    return -1;
  }
  HINSTANCE instance = GetModuleHandleW(nullptr);
  HWND host = EnsureMenuHost(instance);
  if (!host) {
    host = owner;
  }
  if (!host) {
    return -1;
  }

  IContextMenu* pcm = nullptr;
  if (FAILED(AcquireContextMenu(host, paths, &pcm)) || !pcm) {
    LogLine(L"[menu] file context menu acquire failed n=%zu first=%s",
            paths.size(), paths[0].c_str());
    return -1;
  }

  const bool single = paths.size() == 1 || !SameParent(paths);
  const UINT flags = single ? (CMF_NORMAL | CMF_EXPLORE | CMF_CANRENAME)
                            : (CMF_NORMAL | CMF_EXPLORE);
  const int rc =
      TrackCommon(owner, screen, pcm, flags, single, extras, on_verb,
                  on_before_invoke);
  pcm->Release();
  return rc;
}

int TrackShellFolderBackgroundMenu(HWND owner, POINT screen,
                                   const std::wstring& folder,
                                   const std::vector<ShellMenuExtra>& extras,
                                   ShellVerbFn on_verb,
                                   ShellBeforeInvokeFn on_before_invoke) {
  if (folder.empty()) {
    return -1;
  }
  HINSTANCE instance = GetModuleHandleW(nullptr);
  HWND host = EnsureMenuHost(instance);
  if (!host) {
    host = owner;
  }
  if (!host) {
    return -1;
  }

  IContextMenu* pcm = nullptr;
  if (FAILED(AcquireBackgroundContextMenu(host, folder, &pcm)) || !pcm) {
    LogLine(L"[menu] folder background menu acquire failed folder=%s",
            folder.c_str());
    return -1;
  }

  const int rc = TrackCommon(owner, screen, pcm, CMF_NORMAL | CMF_EXPLORE,
                             /*can_rename=*/false, extras, on_verb,
                             on_before_invoke);
  pcm->Release();
  return rc;
}
