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

bool ClipboardGetFiles(std::vector<std::wstring>* paths) {
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
  CloseClipboard();
  return !paths->empty();
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
  CloseClipboard();
  return true;
}
