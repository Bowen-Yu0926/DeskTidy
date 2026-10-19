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

// High-res shell icon (Jumbo/ExtraLarge when available). Caller DestroyIcon.
HICON LoadItemIcon(const std::wstring& path, int size_px = 48);
