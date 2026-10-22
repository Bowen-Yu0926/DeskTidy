#include "shell_clipboard.h"

#include <ShlObj.h>
#include <shellapi.h>

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

UINT ShellIdListFormat() {
  static UINT fmt = 0;
  if (!fmt) {
    fmt = RegisterClipboardFormatW(CFSTR_SHELLIDLIST);
  }
  return fmt;
}

// CFSTR_FILENAMEW: Python writes the first item's full absolute path
// (NUL-terminated UTF-16); multi-select uses only paths[0].
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

// CFSTR_SHELLIDLIST (CIDA): aCount + offsets, empty parent PIDL, then one
// absolute child PIDL per path. Matches Python _build_shell_idlist_bytes so
// Explorer can paste into an ordinary folder.
HGLOBAL MakeShellIdList(const std::vector<std::wstring>& paths) {
  if (paths.empty()) {
    return nullptr;
  }
  struct PidlHolder {
    LPITEMIDLIST pidl = nullptr;
    ~PidlHolder() {
      if (pidl) {
        ILFree(pidl);
      }
    }
  };
  std::vector<PidlHolder> holders(paths.size());
  std::vector<UINT> sizes(paths.size(), 0);
  std::vector<const BYTE*> raw(paths.size(), nullptr);
  for (size_t i = 0; i < paths.size(); ++i) {
    holders[i].pidl = ILCreateFromPathW(paths[i].c_str());
    if (!holders[i].pidl) {
      return nullptr;
    }
    sizes[i] = ILGetSize(holders[i].pidl);
    if (sizes[i] == 0) {
      return nullptr;
    }
    raw[i] = reinterpret_cast<const BYTE*>(holders[i].pidl);
  }
  const UINT cidl = static_cast<UINT>(paths.size());
  const size_t header = 4 + 4 * static_cast<size_t>(cidl + 1);
  std::vector<UINT> offsets(cidl + 1, 0);
  size_t pos = header;
  offsets[0] = static_cast<UINT>(pos);
  pos += 2;  // empty parent PIDL
  for (UINT i = 0; i < cidl; ++i) {
    offsets[i + 1] = static_cast<UINT>(pos);
    pos += sizes[i];
  }
  HGLOBAL mem = GlobalAlloc(GHND, pos);
  if (!mem) {
    return nullptr;
  }
  auto* base = static_cast<BYTE*>(GlobalLock(mem));
  if (!base) {
    GlobalFree(mem);
    return nullptr;
  }
  auto put_u32 = [base](size_t off, UINT v) {
    base[off] = static_cast<BYTE>(v & 0xFF);
    base[off + 1] = static_cast<BYTE>((v >> 8) & 0xFF);
    base[off + 2] = static_cast<BYTE>((v >> 16) & 0xFF);
    base[off + 3] = static_cast<BYTE>((v >> 24) & 0xFF);
  };
  put_u32(0, cidl);
  for (size_t i = 0; i < offsets.size(); ++i) {
    put_u32(4 + 4 * i, offsets[i]);
  }
  base[offsets[0]] = 0;
  base[offsets[0] + 1] = 0;
  for (UINT i = 0; i < cidl; ++i) {
    memcpy(base + offsets[i + 1], raw[i], sizes[i]);
  }
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

}  // namespace

bool ClipboardGetFilesWithEffect(std::vector<std::wstring>* paths, bool* cut) {
  if (cut) {
    *cut = false;
  }
  if (!paths || !OpenClipboard(nullptr)) {
    return false;
  }
  paths->clear();
  HANDLE data = GetClipboardData(CF_HDROP);
  if (!data) {
    CloseClipboard();
    return false;
  }
  HDROP drop = static_cast<HDROP>(data);
  const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT i = 0; i < n; ++i) {
    const UINT len = DragQueryFileW(drop, i, nullptr, 0);
    std::wstring path(len, L'\0');
    DragQueryFileW(drop, i, path.data(), len + 1);
    if (!path.empty()) {
      paths->push_back(std::move(path));
    }
  }
  if (cut) {
    const UINT fmt = PreferredDropEffectFormat();
    HANDLE effect_data = fmt ? GetClipboardData(fmt) : nullptr;
    if (effect_data) {
      auto* pe = static_cast<DWORD*>(GlobalLock(effect_data));
      if (pe) {
        *cut = (*pe & DROPEFFECT_MOVE) != 0;
        GlobalUnlock(effect_data);
      }
    }
  }
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
  if (paths.empty() || !OpenClipboard(owner)) {
    return false;
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
  // FileNameW: Explorer shows a single name; multi-select uses only the first.
  const UINT name_fmt = FileNameWFormat();
  if (name_fmt) {
    HGLOBAL name = MakeFileNameW(paths[0]);
    if (name) {
      SetClipboardData(name_fmt, name);
    }
  }
  // Shell IDList Array lets Explorer paste the cut/copied items into a normal
  // folder (the PIDL array carries namespace identity beyond a bare path).
  const UINT idlist_fmt = ShellIdListFormat();
  if (idlist_fmt) {
    HGLOBAL idlist = MakeShellIdList(paths);
    if (idlist) {
      SetClipboardData(idlist_fmt, idlist);
    }
  }
  CloseClipboard();
  return true;
}
