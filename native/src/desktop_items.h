#pragma once

#include <Windows.h>

#include <string>
#include <vector>

struct DesktopItem {
  std::wstring path;
  std::wstring name;
  bool is_link = false;
};

// Path (case/slash) + name + is_link — used to skip fence/public icon reloads.
bool DesktopItemIdentityEqual(const DesktopItem& a, const DesktopItem& b);
bool DesktopItemListsIdentityEqual(const std::vector<DesktopItem>& a,
                                   const std::vector<DesktopItem>& b);

// User desktop + Public desktop (.lnk / files / folders). Skips hidden system clutter.
std::vector<DesktopItem> ScanDesktopItems();

// Drop the cached scan so the next ScanDesktopItems re-reads the desktop
// (call after file create/delete/move, or after organize).
void InvalidateDesktopScanCache();

// High-res shell icon (Jumbo/ExtraLarge when available). Caller DestroyIcon.
HICON LoadItemIcon(const std::wstring& path, int size_px = 48);
