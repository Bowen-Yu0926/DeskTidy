#pragma once

#include <Windows.h>

#include <string>
#include <vector>

struct DesktopItem {
  std::wstring path;
  std::wstring name;
  bool is_link = false;
};

// User desktop + Public desktop (.lnk / files / folders). Skips hidden system clutter.
std::vector<DesktopItem> ScanDesktopItems();

// Extract a small icon for painting; caller DestroyIcon when done (or keep cached).
HICON LoadItemIcon(const std::wstring& path, int size_px = 32);
