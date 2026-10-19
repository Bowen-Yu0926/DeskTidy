#include "desktop_shell.h"

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

}  // namespace

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
    if (show && !IsWindowVisible(hwnd)) {
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
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

  if (show && !IsWindowVisible(hwnd)) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  }
  return IsAttachedToDesktop(hwnd, &host);
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

  HWND above = WindowAboveDefView(host.defview, host.host, hwnd);
  const UINT flags = kSwpNoMoveNoSizeNoActivate | SWP_NOREDRAW;

  if (above && above != hwnd) {
    // Place just below `above` => just above DefView. Never insert_after=0.
    SetWindowPos(hwnd, above, 0, 0, 0, 0, flags);
  } else {
    // No safe neighbor — seat only. Do NOT HWND_TOP (covers 管理端 / apps).
    PlaceOverlayInDesktopBand(hwnd, true);
    above = WindowAboveDefView(host.defview, host.host, hwnd);
    if (above && above != hwnd) {
      SetWindowPos(hwnd, above, 0, 0, 0, 0, flags);
    }
  }

  if (!IsWindowVisible(hwnd)) {
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  }
}
