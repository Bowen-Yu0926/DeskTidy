#pragma once

#include <Windows.h>

#include <string>
#include <utility>
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
// Ref form: warm 2.5s cache, no vector copy. ONLY safe for immediate read in the
// same call stack that does not InvalidateDesktopScanCache and does not call
// ScanDesktopItems* again (a cache miss move-assigns and frees the old vector).
// Prefer ScanDesktopItems() when the list is used across helper calls.
const std::vector<DesktopItem>& ScanDesktopItemsRef();
// Owning copy (safe across helpers / workers / long-lived storage).
std::vector<DesktopItem> ScanDesktopItems();

// Drop the cached scan so the next ScanDesktopItems re-reads the desktop
// (call after file create/delete/move, or after organize).
void InvalidateDesktopScanCache();

// Resolve .lnk/.url shell display name with path+mtime cache (avoids repeated
// SHGetFileInfo on scan/refresh storms). stem_fallback used when shell fails.
std::wstring ResolveCachedLinkDisplayName(const std::wstring& path,
                                          const std::wstring& stem_fallback,
                                          ULONGLONG mtime_quad);

// High-res shell icon (Jumbo/ExtraLarge when available). Caller DestroyIcon.
HICON LoadItemIcon(const std::wstring& path, int size_px = 48);

// True when the process-wide master cache already holds this path@size.
bool ItemIconCached(const std::wstring& path, int size_px);

// Posted to App message hwnd when STA preload finishes (fill missing icons).
constexpr UINT kDeskTidyMsgIconsPreloaded = WM_APP + 55;

// Queue cold icons on a dedicated STA worker (single-threaded — shell
// extensions are not safe on ephemeral thread pools). When the batch finishes,
// PostMessage(notify, done_msg, 0, 0) if notify is set.
void PreloadIconsAsync(const std::vector<std::pair<std::wstring, int>>& jobs,
                       HWND notify = nullptr, UINT done_msg = 0);

// Find DeskTidy message HWND and queue cold icons (fence zoom / view mode).
void PreloadIconsNotifyApp(const std::vector<std::pair<std::wstring, int>>& jobs);

// Legacy name: enqueue on the STA worker (non-blocking). Prefer Async + notify.
void PreloadIconsParallel(const std::vector<std::pair<std::wstring, int>>& jobs);

// Join the icon worker and drop the process-wide master cache.
void ClearItemIconCache();
void ShutdownIconPreloadWorker();
