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

// Drop the cached scan so the next ScanDesktopItems re-reads the desktop
// (call after file create/delete/move, or after organize).
void InvalidateDesktopScanCache();

// High-res shell icon (Jumbo/ExtraLarge when available). Caller DestroyIcon.
HICON LoadItemIcon(const std::wstring& path, int size_px = 48);
