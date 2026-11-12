#include "shell_clipboard.h"

#include "ole_drag.h"

#include <ShlObj.h>
#include <shellapi.h>
#include <objidl.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

UINT PreferredDropEffectFormat() {
  static UINT fmt = 0;
  if (!fmt) {
    fmt = RegisterClipboardFormatW(L"Preferred DropEffect");
  }
  return fmt;
}

UINT FileNameWFormat() {
  static UINT fmt = 0;
  if (!fmt) {
    fmt = RegisterClipboardFormatW(CFSTR_FILENAMEW);
  }
  return fmt;
}

// CFSTR_FILENAMEW: first item's full absolute path (NUL-terminated UTF-16).
HGLOBAL MakeFileNameW(const std::wstring& path) {
  const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GHND, bytes);
  if (!mem) {
    return nullptr;
  }
  void* locked = GlobalLock(mem);
  if (!locked) {
    GlobalFree(mem);
    return nullptr;
  }
  memcpy(locked, path.c_str(), bytes);
  GlobalUnlock(mem);
  return mem;
}

HGLOBAL MakeHdrop(const std::vector<std::wstring>& paths) {
  size_t chars = 1;
  for (const auto& p : paths) {
    chars += p.size() + 1;
  }
  const SIZE_T bytes =
      sizeof(DROPFILES) + chars * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GHND, bytes);
  if (!mem) {
    return nullptr;
  }
  auto* df = static_cast<DROPFILES*>(GlobalLock(mem));
  if (!df) {
    GlobalFree(mem);
    return nullptr;
  }
  df->pFiles = sizeof(DROPFILES);
  df->fWide = TRUE;
  wchar_t* dest = reinterpret_cast<wchar_t*>(
      reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
  for (const auto& p : paths) {
    const size_t n = p.size() + 1;
    memcpy(dest, p.c_str(), n * sizeof(wchar_t));
    dest += n;
  }
  *dest = L'\0';
  GlobalUnlock(mem);
  return mem;
}

// Legacy Win32 path: CF_HDROP + Preferred DropEffect + FileNameW only.
// Do NOT synthesize CFSTR_SHELLIDLIST here — empty-parent + absolute child
// PIDLs break Win11 Explorer paste (it prefers a bad IDList over HDROP).
bool ClipboardSetFilesWin32(HWND owner, const std::vector<std::wstring>& paths,
                            bool cut) {
  // Drop stale OLE ownership so OpenClipboard/EmptyClipboard can take over.
  OleSetClipboard(nullptr);
  if (!OpenClipboard(owner)) {
    if (!OpenClipboard(nullptr)) {
      return false;
    }
  }
  EmptyClipboard();
  HGLOBAL hdrop = MakeHdrop(paths);
  if (!hdrop) {
    CloseClipboard();
    return false;
  }
  SetClipboardData(CF_HDROP, hdrop);

  const UINT fmt = PreferredDropEffectFormat();
  if (fmt) {
    HGLOBAL effect = GlobalAlloc(GHND, sizeof(DWORD));
    if (effect) {
      auto* pe = static_cast<DWORD*>(GlobalLock(effect));
      if (pe) {
        *pe = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
        GlobalUnlock(effect);
        SetClipboardData(fmt, effect);
      } else {
        GlobalFree(effect);
      }
    }
  }
  const UINT name_fmt = FileNameWFormat();
  if (name_fmt) {
    HGLOBAL name = MakeFileNameW(paths[0]);
    if (name) {
      SetClipboardData(name_fmt, name);
    }
  }
  CloseClipboard();
  return true;
}

}  // namespace

bool ClipboardGetFilesWithEffect(std::vector<std::wstring>* paths, bool* cut) {
  if (cut) {
    *cut = false;
  }
  if (!paths) {
    return false;
  }
  paths->clear();

  auto read_hdrop = [&](HDROP drop) {
    if (!drop) {
      return;
    }
    const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < n; ++i) {
      const UINT len = DragQueryFileW(drop, i, nullptr, 0);
      std::wstring path(len, L'\0');
      DragQueryFileW(drop, i, path.data(), len + 1);
      if (!path.empty()) {
        paths->push_back(std::move(path));
      }
    }
  };
  auto read_effect = [&](IDataObject* data, HANDLE win32_effect) {
    if (!cut) {
      return;
    }
    if (data) {
      const UINT fmt = PreferredDropEffectFormat();
      if (fmt) {
        FORMATETC fe{static_cast<CLIPFORMAT>(fmt), nullptr, DVASPECT_CONTENT, -1,
                     TYMED_HGLOBAL};
        STGMEDIUM stg{};
        if (SUCCEEDED(data->GetData(&fe, &stg)) && stg.hGlobal) {
          auto* pe = static_cast<DWORD*>(GlobalLock(stg.hGlobal));
          if (pe) {
            *cut = (*pe & DROPEFFECT_MOVE) != 0;
            GlobalUnlock(stg.hGlobal);
          }
          ReleaseStgMedium(&stg);
          return;
        }
      }
    }
    if (win32_effect) {
      auto* pe = static_cast<DWORD*>(GlobalLock(win32_effect));
      if (pe) {
        *cut = (*pe & DROPEFFECT_MOVE) != 0;
        GlobalUnlock(win32_effect);
      }
    }
  };

  // Prefer OleGetClipboard — Explorer + our ShellOleSetClipboardFiles path.
  IDataObject* data = nullptr;
  if (SUCCEEDED(OleGetClipboard(&data)) && data) {
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM stg{};
    if (SUCCEEDED(data->GetData(&fe, &stg)) && stg.hGlobal) {
      read_hdrop(static_cast<HDROP>(stg.hGlobal));
      read_effect(data, nullptr);
      ReleaseStgMedium(&stg);
      data->Release();
      return !paths->empty();
    }
    data->Release();
  }

  if (!OpenClipboard(nullptr)) {
    return false;
  }
  HANDLE hdrop = GetClipboardData(CF_HDROP);
  if (!hdrop) {
    CloseClipboard();
    return false;
  }
  read_hdrop(static_cast<HDROP>(hdrop));
  const UINT fmt = PreferredDropEffectFormat();
  HANDLE effect_data = fmt ? GetClipboardData(fmt) : nullptr;
  read_effect(nullptr, effect_data);
  CloseClipboard();
  return !paths->empty();
}

bool ClipboardGetFiles(std::vector<std::wstring>* paths) {
  return ClipboardGetFilesWithEffect(paths, nullptr);
}

bool ClipboardPathIsCut(const std::wstring& path) {
  static DWORD cached_seq = 0xFFFFFFFF;
  static std::vector<std::wstring> cached;
  const DWORD seq = GetClipboardSequenceNumber();
  if (seq != cached_seq) {
    cached_seq = seq;
    cached.clear();
    std::vector<std::wstring> files;
    if (ClipboardGetFiles(&files) && OpenClipboard(nullptr)) {
      const UINT fmt = PreferredDropEffectFormat();
      HANDLE data = fmt ? GetClipboardData(fmt) : nullptr;
      DWORD effect = DROPEFFECT_COPY;
      if (data) {
        auto* pe = static_cast<DWORD*>(GlobalLock(data));
        if (pe) {
          effect = *pe;
          GlobalUnlock(data);
        }
      }
      CloseClipboard();
      if (effect == DROPEFFECT_MOVE) {
        cached = std::move(files);
      }
    }
  }
  for (const auto& p : cached) {
    if (_wcsicmp(p.c_str(), path.c_str()) == 0) {
      return true;
    }
  }
  return false;
}

bool ClipboardSetText(HWND owner, const std::wstring& text) {
  if (!OpenClipboard(owner)) {
    return false;
  }
  EmptyClipboard();
  const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!mem) {
    CloseClipboard();
    return false;
  }
  void* locked = GlobalLock(mem);
  if (!locked) {
    GlobalFree(mem);
    CloseClipboard();
    return false;
  }
  memcpy(locked, text.c_str(), bytes);
  GlobalUnlock(mem);
  SetClipboardData(CF_UNICODETEXT, mem);
  CloseClipboard();
  return true;
}

bool ClipboardSetFiles(HWND owner, const std::vector<std::wstring>& paths,
                       bool cut) {
  if (paths.empty()) {
    return false;
  }
  // Primary: same shell IDataObject as OLE drag — Win11 Explorer paste needs
  // a real Shell IDList Array (Fences / Explorer Ctrl+C parity).
  if (ShellOleSetClipboardFiles(paths, cut)) {
    return true;
  }
  return ClipboardSetFilesWin32(owner, paths, cut);
}

void ClipboardConsumeCut() {
  // Explorer empties the clipboard after a completed cut-paste. Clear both
  // Win32 and OLE views so leftover Preferred DropEffect=MOVE cannot unpin.
  if (OpenClipboard(nullptr)) {
    EmptyClipboard();
    CloseClipboard();
  }
  OleSetClipboard(nullptr);
}
