#include "desktop_items.h"

#include <ShlObj.h>
#include <Shellapi.h>

#include <algorithm>

namespace {

bool EndsWithIgnoreCase(const std::wstring& s, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  if (s.size() < n) {
    return false;
  }
  return _wcsicmp(s.c_str() + (s.size() - n), suffix) == 0;
}

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
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) {
      continue;
    }
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) {
      // Keep known desktop shortcuts; skip pure system debris.
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
    // Prefer shell display name when available.
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

}  // namespace

std::vector<DesktopItem> ScanDesktopItems() {
  std::vector<DesktopItem> items;
  AppendFolder(KnownFolder(FOLDERID_Desktop), &items);
  AppendFolder(KnownFolder(FOLDERID_PublicDesktop), &items);

  std::sort(items.begin(), items.end(),
            [](const DesktopItem& a, const DesktopItem& b) {
              return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
            });
  // De-dupe by path (case-insensitive).
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
  return unique;
}

HICON LoadItemIcon(const std::wstring& path, int size_px) {
  SHFILEINFOW sfi{};
  UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
  if (size_px >= 40) {
    flags = SHGFI_ICON | SHGFI_LARGEICON;
  }
  if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags)) {
    return nullptr;
  }
  return sfi.hIcon;
}
