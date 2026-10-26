#include "desktop_shell.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>

#ifndef GWLP_HWNDPARENT
#define GWLP_HWNDPARENT (-8)
#endif

namespace {

constexpr UINT kSwpNoMoveNoSizeNoActivate =
    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;

DefViewHost g_cache{};
ULONGLONG g_cache_tick = 0;
constexpr ULONGLONG kCacheTtlMs = 2000;

HWND g_tracked_defview = nullptr;

bool IsWindowAlive(HWND hwnd) {
  return hwnd && IsWindow(hwnd);
}

HWND OwnerOf(HWND hwnd) {
  return IsWindowAlive(hwnd) ? GetWindow(hwnd, GW_OWNER) : nullptr;
}

HWND ParentOf(HWND hwnd) {
  return IsWindowAlive(hwnd) ? GetParent(hwnd) : nullptr;
}

bool SetOwner(HWND hwnd, HWND owner) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  SetLastError(0);
  const LONG_PTR prev =
      SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
  if (prev == 0 && GetLastError() != 0 && owner != nullptr) {
    return false;
  }
  return true;
}

HWND WindowAboveDefView(HWND defview, HWND host, HWND self) {
  HWND anchor = defview ? defview : host;
  if (!IsWindowAlive(anchor)) {
    return nullptr;
  }
  HWND above = GetWindow(anchor, GW_HWNDPREV);
  if (above == self) {
    above = GetWindow(self, GW_HWNDPREV);
  }
  if (above == self) {
    return nullptr;
  }
  return above;
}

// Win+D minimizes desktop-owned overlays: IsWindowVisible stays TRUE while
// IsIconic is set, and SW_SHOWNOACTIVATE cannot un-minimize a window. Restore
// iconic overlays first, then show hidden ones without stealing focus.
void ShowOverlayNoActivate(HWND hwnd) {
  if (!IsWindowAlive(hwnd)) {
    return;
  }
  if (IsIconic(hwnd)) {
    ShowWindow(hwnd, SW_RESTORE);
    return;
  }
  if (!IsWindowVisible(hwnd)) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  }
}

}  // namespace

void InvalidateDefViewCache() {
  g_cache = {};
  g_cache_tick = 0;
}

void ResetExplorerRebuildTracking() {
  g_tracked_defview = nullptr;
}

bool PollExplorerDesktopRebuild() {
  const DefViewHost host = FindDefViewHost();
  const HWND cur = host.defview;
  if (cur == nullptr) {
    // No DefView yet (Explorer still starting / restarted but not ready).
    return false;
  }
  if (g_tracked_defview == nullptr) {
    g_tracked_defview = cur;
    return false;
  }
  if (g_tracked_defview != cur) {
    g_tracked_defview = cur;
    return true;
  }
  return false;
}

DefViewHost FindDefViewHost() {
  const ULONGLONG now = GetTickCount64();
  if (g_cache.host && g_cache.defview && now - g_cache_tick < kCacheTtlMs &&
      IsWindowAlive(g_cache.host) && IsWindowAlive(g_cache.defview)) {
    return g_cache;
  }

  DefViewHost out{};
  HWND progman = FindWindowW(L"Progman", nullptr);
  if (progman) {
    HWND defview = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defview) {
      out.host = progman;
      out.defview = defview;
      g_cache = out;
      g_cache_tick = now;
      return out;
    }
  }

  HWND worker = nullptr;
  for (int i = 0; i < 64; ++i) {
    worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr);
    if (!worker) {
      break;
    }
    HWND defview = FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defview) {
      out.host = worker;
      out.defview = defview;
      g_cache = out;
      g_cache_tick = now;
      return out;
    }
  }

  g_cache = {};
  g_cache_tick = now;
  return out;
}

bool IsWallpaperWorkerW(HWND hwnd) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  wchar_t cls[64] = {};
  if (GetClassNameW(hwnd, cls, 64) == 0) {
    return false;
  }
  if (wcscmp(cls, L"WorkerW") != 0) {
    return false;
  }
  return FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr) == nullptr;
}

bool IsAttachedToDesktop(HWND hwnd, const DefViewHost* cached) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  DefViewHost host = cached ? *cached : FindDefViewHost();
  if (!host.host) {
    return false;
  }
  const HWND owner = OwnerOf(hwnd);
  const HWND parent = ParentOf(hwnd);
  return owner == host.host || owner == host.defview || parent == host.host ||
         parent == host.defview;
}

bool AttachOverlayToDesktop(HWND hwnd, bool show) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  if (IsWallpaperWorkerW(OwnerOf(hwnd)) || IsWallpaperWorkerW(ParentOf(hwnd))) {
    DetachOverlayFromDesktop(hwnd);
  }

  const DefViewHost host = FindDefViewHost();
  if (!host.host || IsWallpaperWorkerW(host.host)) {
    return false;
  }
  if (IsAttachedToDesktop(hwnd, &host)) {
    if (show) {
      ShowOverlayNoActivate(hwnd);
    }
    return true;
  }

  RECT rect{};
  if (!GetWindowRect(hwnd, &rect)) {
    return false;
  }

  if (!SetOwner(hwnd, host.host)) {
    return false;
  }

  SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left,
               rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);

  PlaceOverlayInDesktopBand(hwnd, true);

  if (show) {
    ShowOverlayNoActivate(hwnd);
  }
  return IsAttachedToDesktop(hwnd, &host);
}

bool ForceAttachOverlayToDesktop(HWND hwnd, bool show) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  // Win+D / wallpaper WorkerW churn can leave a stale owner — detach first.
  InvalidateDefViewCache();
  DetachOverlayFromDesktop(hwnd);
  return AttachOverlayToDesktop(hwnd, show);
}

bool IsExplorerDesktopForeground() {
  // Match Python is_explorer_desktop_foreground: Progman/WorkerW/DefView only.
  // Taskbar / tray must NOT count — clicking the tray icon would otherwise
  // schedule RaiseOverlay and lift fences above open apps.
  HWND fg = GetForegroundWindow();
  if (!IsWindowAlive(fg)) {
    return false;
  }
  HWND cur = fg;
  for (int hop = 0; hop < 6 && IsWindowAlive(cur); ++hop) {
    wchar_t cls[64] = {};
    if (GetClassNameW(cur, cls, 64) > 0) {
      if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
          wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
          wcscmp(cls, L"SysListView32") == 0) {
        return true;
      }
    }
    HWND parent = ParentOf(cur);
    if (!parent) {
      parent = OwnerOf(cur);
    }
    cur = parent;
  }
  return false;
}

bool DetachOverlayFromDesktop(HWND hwnd) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  RECT rect{};
  GetWindowRect(hwnd, &rect);
  SetOwner(hwnd, nullptr);
  if (ParentOf(hwnd)) {
    SetParent(hwnd, nullptr);
  }
  if (rect.right > rect.left && rect.bottom > rect.top) {
    SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left,
                 rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  return true;
}

void PlaceOverlayInDesktopBand(HWND hwnd, bool /*force*/) {
  if (!IsWindowAlive(hwnd)) {
    return;
  }
  // HWND_BOTTOM = 1 — seat under APPWINDOWs (Fences desktop seat).
  SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, kSwpNoMoveNoSizeNoActivate);
}

void RaiseOverlayInDesktopBand(HWND hwnd) {
  if (!IsWindowAlive(hwnd)) {
    return;
  }
  const DefViewHost host = FindDefViewHost();
  if (!IsAttachedToDesktop(hwnd, &host)) {
    return;
  }

  // Place just above DefView: insert-after = window currently above DefView.
  // When DefView is already topmost (Win+D), insert-after HWND_TOP (0) puts
  // the overlay at the top of the non-topmost band — matching Python.
  // Seating HWND_BOTTOM here buries fences under wallpaper after show-desktop.
  HWND above = WindowAboveDefView(host.defview, host.host, hwnd);
  HWND insert_after = (above && above != hwnd) ? above : HWND_TOP;
  const UINT flags = kSwpNoMoveNoSizeNoActivate | SWP_NOREDRAW;
  SetWindowPos(hwnd, insert_after, 0, 0, 0, 0, flags);

  ShowOverlayNoActivate(hwnd);
}

void OverlayWindowBatch::Hide(HWND hwnd) {
  if (!IsWindowAlive(hwnd)) {
    return;
  }
  items_.push_back(Item{kHide, hwnd, 0, 0, 0, 0});
}

void OverlayWindowBatch::Show(HWND hwnd) {
  if (!IsWindowAlive(hwnd)) {
    return;
  }
  items_.push_back(Item{kShow, hwnd, 0, 0, 0, 0});
}

void OverlayWindowBatch::SetRect(HWND hwnd, int x, int y, int w, int h) {
  if (!IsWindowAlive(hwnd) || w <= 0 || h <= 0) {
    return;
  }
  items_.push_back(Item{kMove, hwnd, x, y, w, h});
}

void OverlayWindowBatch::Commit() {
  std::vector<Item> items = std::move(items_);
  items_.clear();
  if (items.empty()) {
    return;
  }
  // Hide leavers before showing arrivers — one-frame overlap flashes.
  std::stable_sort(items.begin(), items.end(),
                   [](const Item& a, const Item& b) {
                     const int oa = a.op == kHide ? 0 : (a.op == kMove ? 1 : 2);
                     const int ob = b.op == kHide ? 0 : (b.op == kMove ? 1 : 2);
                     if (oa != ob) {
                       return oa < ob;
                     }
                     return reinterpret_cast<uintptr_t>(a.hwnd) <
                            reinterpret_cast<uintptr_t>(b.hwnd);
                   });

  auto flags_for = [](Op op) -> UINT {
    const UINT base = SWP_NOZORDER | SWP_NOACTIVATE;
    if (op == kHide) {
      // NOREDRAW on hide avoids intermediate DWM frames.
      return base | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW | SWP_NOREDRAW |
             SWP_NOCOPYBITS;
    }
    if (op == kShow) {
      // No SWP_NOREDRAW / NOCOPYBITS on show — blank or discarded client flash.
      return base | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW;
    }
    return base | SWP_NOREDRAW | SWP_NOCOPYBITS;
  };

  auto apply_one = [&](const Item& it) {
    SetWindowPos(it.hwnd, nullptr, it.x, it.y, it.w, it.h, flags_for(it.op));
  };

  HDWP hdwp = BeginDeferWindowPos(static_cast<int>(items.size()));
  if (!hdwp) {
    for (const auto& it : items) {
      apply_one(it);
    }
    return;
  }
  for (const auto& it : items) {
    hdwp = DeferWindowPos(hdwp, it.hwnd, nullptr, it.x, it.y, it.w, it.h,
                          flags_for(it.op));
    if (!hdwp) {
      for (const auto& it2 : items) {
        apply_one(it2);
      }
      return;
    }
  }
  EndDeferWindowPos(hdwp);
}
