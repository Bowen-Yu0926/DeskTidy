#include "desktop_items.h"

#include "namespace_icons.h"

#include <CommCtrl.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <commoncontrols.h>

#include <algorithm>

#pragma comment(lib, "comctl32.lib")

namespace {

#ifndef SHIL_LARGE
#define SHIL_LARGE 0x0
#endif
#ifndef SHIL_SMALL
#define SHIL_SMALL 0x1
#endif
#ifndef SHIL_EXTRALARGE
#define SHIL_EXTRALARGE 0x2
#endif
#ifndef SHIL_SYSSMALL
#define SHIL_SYSSMALL 0x3
#endif
#ifndef SHIL_JUMBO
#define SHIL_JUMBO 0x4
#endif

bool EndsWithIgnoreCase(const std::wstring& s, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  if (s.size() < n) {
    return false;
  }
  return _wcsicmp(s.c_str() + (s.size() - n), suffix) == 0;
}

// Office lock/temp files (~$foo.docx) — matched by Python is_temp_desktop_file.
bool IsOfficeTempName(const wchar_t* name) {
  return name[0] == L'~' && name[1] == L'$';
}

// 2.5s scan cache (Python _SCAN_CACHE_TTL_S).
constexpr ULONGLONG kScanCacheTtlMs = 2500;
ULONGLONG g_scan_tick = 0;
std::vector<DesktopItem> g_scan_cache;

void AppendFolder(const std::wstring& folder, std::vector<DesktopItem>* out) {
  if (folder.empty()) {
    return;
  }
  const std::wstring pattern = folder + L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (fd.cFileName[0] == L'.' &&
        (fd.cFileName[1] == L'\0' ||
         (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
      continue;
    }
    if (IsOfficeTempName(fd.cFileName)) {
      continue;
    }
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) {
      continue;
    }
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          !EndsWithIgnoreCase(fd.cFileName, L".lnk")) {
        continue;
      }
    }
    DesktopItem item;
    item.path = folder + L"\\" + fd.cFileName;
    item.name = fd.cFileName;
    item.is_link = EndsWithIgnoreCase(item.name, L".lnk");
    if (item.is_link && item.name.size() > 4) {
      item.name.resize(item.name.size() - 4);
    }
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(item.path.c_str(), 0, &sfi, sizeof(sfi),
                       SHGFI_DISPLAYNAME)) {
      if (sfi.szDisplayName[0] != L'\0') {
        item.name = sfi.szDisplayName;
      }
    }
    out->push_back(std::move(item));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) {
    return {};
  }
  std::wstring out = p;
  CoTaskMemFree(p);
  return out;
}

HICON IconFromImageList(int shil, int index) {
  IImageList* list = nullptr;
  if (FAILED(SHGetImageList(shil, IID_IImageList,
                            reinterpret_cast<void**>(&list))) ||
      !list) {
    return nullptr;
  }
  HICON icon = nullptr;
  list->GetIcon(index, ILD_TRANSPARENT, &icon);
  list->Release();
  return icon;
}

}  // namespace

std::vector<DesktopItem> ScanDesktopItems() {
  const ULONGLONG now = GetTickCount64();
  if (!g_scan_cache.empty() && now - g_scan_tick < kScanCacheTtlMs) {
    return g_scan_cache;
  }

  std::vector<DesktopItem> items;
  AppendFolder(KnownFolder(FOLDERID_Desktop), &items);
  AppendFolder(KnownFolder(FOLDERID_PublicDesktop), &items);
  // Hosted This PC / Recycle Bin shortcuts (Python .public_system).
  AppendPublicSystemItems(&items);

  std::sort(items.begin(), items.end(),
            [](const DesktopItem& a, const DesktopItem& b) {
              return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
            });
  std::vector<DesktopItem> unique;
  for (auto& it : items) {
    bool dup = false;
    for (const auto& u : unique) {
      if (_wcsicmp(u.path.c_str(), it.path.c_str()) == 0) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      unique.push_back(std::move(it));
    }
  }
  g_scan_cache = unique;
  g_scan_tick = now;
  return unique;
}

void InvalidateDesktopScanCache() {
  g_scan_cache.clear();
  g_scan_tick = 0;
}

HICON LoadItemIcon(const std::wstring& path, int size_px) {
  if (path.empty()) {
    return nullptr;
  }
  SHFILEINFOW sfi{};
  const DWORD_PTR ok = SHGetFileInfoW(
      path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX);
  if (!ok) {
    // Fallback: stock large/small icon (often soft on HiDPI).
    UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
    if (size_px < 28) {
      flags = SHGFI_ICON | SHGFI_SMALLICON;
    }
    if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags)) {
      return nullptr;
    }
    return sfi.hIcon;
  }

  // Prefer a source larger than the draw size so DrawIconEx downscales sharp.
  const int order[] = {
      size_px >= 64 ? SHIL_JUMBO : SHIL_EXTRALARGE,
      SHIL_JUMBO,
      SHIL_EXTRALARGE,
      SHIL_LARGE,
      SHIL_SMALL,
  };
  for (int shil : order) {
    HICON icon = IconFromImageList(shil, sfi.iIcon);
    if (icon) {
      return icon;
    }
  }

  UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
  if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags)) {
    return sfi.hIcon;
  }
  return nullptr;
}
