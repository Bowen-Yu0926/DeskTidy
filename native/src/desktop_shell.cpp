#include "desktop_shell.h"

#include "log.h"

#include <algorithm>
#include <cstdint>
#include <cwchar>

#ifndef GWLP_HWNDPARENT
#define GWLP_HWNDPARENT (-8)
#endif
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif

namespace {

constexpr UINT kSwpNoMoveNoSizeNoActivate =
    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;

DefViewHost g_cache{};
ULONGLONG g_cache_tick = 0;
// DefView host 定位结果几乎不随 watch/keepalive 的 2s tick 变化；缓存 TTL
// 提高到 8s，避免每个 tick 都全量 FindWindowW+FindWindowExW 扫描（最多 64 个
// WorkerW）。窗口失效由命中前的 IsWindowAlive 兜底，主动失效由
// InvalidateDefViewCache() 触发（页面切换 / chrome 不健康 / explorer 重建）。
constexpr ULONGLONG kCacheTtlMs = 8000;

HWND g_tracked_defview = nullptr;

bool IsWindowAlive(HWND hwnd) {
  return hwnd && IsWindow(hwnd);
}

HWND OwnerOf(HWND hwnd) {
  return IsWindowAlive(hwnd) ? GetWindow(hwnd, GW_OWNER) : nullptr;
}

HWND ParentOf(HWND hwnd) {
  // GetParent() on a WS_POPUP returns the *owner* — that made SoftSeat think
  // every Progman-owned fence was a child and fail IsAttached forever.
  if (!IsWindowAlive(hwnd)) {
    return nullptr;
  }
  if ((GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CHILD) == 0) {
    return nullptr;
  }
  return GetParent(hwnd);
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

void EnsureTopLevelPopupStyle(HWND hwnd) {
  if (!IsWindowAlive(hwnd)) {
    return;
  }
  LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  if ((style & WS_CHILD) || !(style & WS_POPUP)) {
    style = (style & ~static_cast<LONG_PTR>(WS_CHILD)) | WS_POPUP;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
  }
}

// Soft-retarget owner without clearing it first. Detach→null blanks ULW.
bool SoftSeatToOwner(HWND hwnd, HWND owner, bool show) {
  if (!IsWindowAlive(hwnd) || !IsWindowAlive(owner)) {
    return false;
  }
  RECT screen{};
  if (!GetWindowRect(hwnd, &screen)) {
    return false;
  }
  // Migrate off a mistaken DefView-child seat (blank ULW on raised desktop).
  bool need_frame = false;
  if (ParentOf(hwnd)) {
    SetParent(hwnd, nullptr);
    need_frame = true;
  }
  const LONG_PTR style_before = GetWindowLongPtrW(hwnd, GWL_STYLE);
  EnsureTopLevelPopupStyle(hwnd);
  if (GetWindowLongPtrW(hwnd, GWL_STYLE) != style_before) {
    need_frame = true;
  }
  if (OwnerOf(hwnd) != owner) {
    if (!SetOwner(hwnd, owner)) {
      return false;
    }
    need_frame = true;
  }
  // SWP_FRAMECHANGED on every SoftSeat used to wipe UpdateLayeredWindow pixels
  // (attached+visible but fully transparent — page bar still showed). Only
  // reframe when parent/style/owner actually changed.
  if (!need_frame && !show) {
    return OwnerOf(hwnd) == owner && !ParentOf(hwnd);
  }
  const int w = screen.right - screen.left;
  const int h = screen.bottom - screen.top;
  // SWP_NOREDRAW: FRAMECHANGED alone still retargets owner; skipping redraw
  // avoids an extra DWM clear of UpdateLayeredWindow pixels (pet/fence blank
  // after Peek-style Detach→SoftSeat). Caller must Present/RefreshLayer after.
  UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW;
  if (need_frame) {
    flags |= SWP_FRAMECHANGED;
  }
  if (show) {
    flags |= SWP_SHOWWINDOW;
  }
  SetWindowPos(hwnd, nullptr, screen.left, screen.top, w, h, flags);
  return OwnerOf(hwnd) == owner && !ParentOf(hwnd);
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

bool IsRaisedDesktopShell() {
  // Win11 24H2+ servicing: Progman created with WS_EX_NOREDIRECTIONBITMAP;
  // DefView is a layered child. GDI/ULW children of DefView go blank — seat
  // must stay top-level owned by Progman/WorkerW (never SetParent→DefView).
  HWND progman = FindWindowW(L"Progman", nullptr);
  if (!IsWindowAlive(progman)) {
    return false;
  }
  return (GetWindowLongPtrW(progman, GWL_EXSTYLE) &
          WS_EX_NOREDIRECTIONBITMAP) != 0;
}

void LogDesktopShellCompatProfile() {
  OSVERSIONINFOEXW vi{};
  vi.dwOSVersionInfoSize = sizeof(vi);
  // RtlGetVersion is honest on Win10+ (GetVersionEx is capped / lied).
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  DWORD major = 0;
  DWORD minor = 0;
  DWORD build = 0;
  if (ntdll) {
    auto rtl = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion"));
    if (rtl && rtl(&vi) == 0) {
      major = vi.dwMajorVersion;
      minor = vi.dwMinorVersion;
      build = vi.dwBuildNumber;
    }
  }
  const DefViewHost host = FindDefViewHost();
  wchar_t host_cls[64] = {};
  if (host.host) {
    GetClassNameW(host.host, host_cls, 64);
  }
  const bool raised = IsRaisedDesktopShell();
  const bool show_desk = IsShowDesktopActive();
  LogLine(L"[compat] os=%u.%u.%u raised_desktop=%d defview_host=%s "
          L"show_desktop=%d strategy=single-binary-runtime-branch",
          major, minor, build, raised ? 1 : 0,
          host_cls[0] ? host_cls : L"(none)", show_desk ? 1 : 0);
}

bool IsAttachedToDesktop(HWND hwnd, const DefViewHost* cached) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  DefViewHost host = cached ? *cached : FindDefViewHost();
  if (!host.host) {
    return false;
  }
  // Must be top-level owned by the DefView host — not a DefView child
  // (children blank under UpdateLayeredWindow on raised desktop).
  if (ParentOf(hwnd)) {
    return false;
  }
  const HWND owner = OwnerOf(hwnd);
  return owner == host.host || owner == host.defview;
}

bool SetOverlayScreenPos(HWND hwnd, int x, int y, int w, int h, UINT flags) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  HWND parent = ParentOf(hwnd);
  int cx = x;
  int cy = y;
  if (parent && !(flags & SWP_NOMOVE)) {
    POINT pt{x, y};
    if (ScreenToClient(parent, &pt)) {
      cx = pt.x;
      cy = pt.y;
    }
  }
  return SetWindowPos(hwnd, nullptr, cx, cy, w, h, flags | SWP_NOACTIVATE) !=
         FALSE;
}

bool MoveOverlayScreen(HWND hwnd, int x, int y) {
  return SetOverlayScreenPos(hwnd, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

bool UpdateOverlayLayered(HWND hwnd, HDC hdc_dst, const POINT* screen_origin,
                          const SIZE* size, HDC hdc_src, const POINT* ppt_src,
                          COLORREF key, BLENDFUNCTION* blend, DWORD flags) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  const POINT* ppt = screen_origin;
  if (ParentOf(hwnd)) {
    ppt = nullptr;
  }
  return UpdateLayeredWindow(hwnd, hdc_dst, const_cast<POINT*>(ppt),
                             const_cast<SIZE*>(size), hdc_src,
                             const_cast<POINT*>(ppt_src), key, blend,
                             flags) != FALSE;
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
    // Raised desktop (24H2+): prefer Progman/WorkerW owner, not DefView —
    // DefView-owned seats were a migration leftover and can blank ULW.
    if (IsRaisedDesktopShell() && host.host &&
        OwnerOf(hwnd) == host.defview && host.defview != host.host) {
      SoftSeatToOwner(hwnd, host.host, show);
    }
    // Already seated: do not Raise on every Attach (Fences/Rainmeter keep
    // chrome quiet under open apps). Callers that need a band flip Raise
    // explicitly after show-desktop / burial recovery.
    if (show) {
      ShowOverlayNoActivate(hwnd);
    }
    return IsAttachedToDesktop(hwnd, &host);
  }

  if (!SoftSeatToOwner(hwnd, host.host, show)) {
    return false;
  }
  if (show) {
    RaiseOverlayInDesktopBand(hwnd);
    ShowOverlayNoActivate(hwnd);
  }
  return IsAttachedToDesktop(hwnd, &host);
}

bool ForceAttachOverlayToDesktop(HWND hwnd, bool show) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  InvalidateDefViewCache();
  if (IsWallpaperWorkerW(OwnerOf(hwnd)) || IsWallpaperWorkerW(ParentOf(hwnd)) ||
      ParentOf(hwnd)) {
    DetachOverlayFromDesktop(hwnd);
  }
  if (AttachOverlayToDesktop(hwnd, show)) {
    return true;
  }
  DetachOverlayFromDesktop(hwnd);
  return AttachOverlayToDesktop(hwnd, show);
}

bool IsExplorerDesktopForeground() {
  HWND fg = GetForegroundWindow();
  if (!IsWindowAlive(fg)) {
    return false;
  }
  // 若前台窗口属于本进程（如 DeskTidy 设置后台、提示框等），绝不是桌面
  DWORD fg_pid = 0;
  GetWindowThreadProcessId(fg, &fg_pid);
  if (fg_pid == GetCurrentProcessId()) {
    return false;
  }

  HWND cur = fg;
  for (int hop = 0; hop < 8 && IsWindowAlive(cur); ++hop) {
    wchar_t cls[64] = {};
    if (GetClassNameW(cur, cls, 64) > 0) {
      // Taskbar / XAML islands are never the desktop (Win10 tray + Win11
      // centered taskbar / Start).
      if (wcscmp(cls, L"Shell_TrayWnd") == 0 ||
          wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
          wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0 ||
          wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") ==
              0) {
        return false;
      }
      // Only Progman / WorkerW / DefView count as desktop shell hosts.
      // SysListView32 / DirectUIHWND also appear inside Explorer folder
      // windows (Win10 + Win11) — requiring an ancestor host avoids treating
      // "open Folder" as show-desktop / band-lift.
      if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
          wcscmp(cls, L"SHELLDLL_DefView") == 0) {
        return true;
      }
    }
    // Climb real parent first (child SysListView32/DirectUI → DefView), then
    // owner. Avoid GetParent()'s owner-for-popup quirk via ParentOf().
    HWND parent = ParentOf(cur);
    if (!parent) {
      // WS_CHILD under DefView: ParentOf uses style bit; some shell children
      // still need GetAncestor when style is atypical.
      parent = GetAncestor(cur, GA_PARENT);
      if (parent == GetDesktopWindow()) {
        parent = nullptr;
      }
    }
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
  if (ParentOf(hwnd)) {
    SetParent(hwnd, nullptr);
  }
  EnsureTopLevelPopupStyle(hwnd);
  SetOwner(hwnd, nullptr);
  if (rect.right > rect.left && rect.bottom > rect.top) {
    SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left,
                 rect.bottom - rect.top,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }
  return true;
}

void RaiseOverlayInDesktopBand(HWND hwnd);

void PlaceOverlayInDesktopBand(HWND hwnd, bool /*force*/) {
  RaiseOverlayInDesktopBand(hwnd);
}

DesktopBandState QueryDesktopBandState() {
  DesktopBandState state;
  // Both facts are queried independently: a cover must not mask show-desktop,
  // otherwise Win+D over a (dismissing) cover reports "neither" and the
  // overlays get sunk instead of lifted. The raise branch checks the cover
  // first, so an actual cover still wins.
  state.fullscreen_cover = HasVisibleFullscreenCover();
  state.show_desktop = IsShowDesktopActive();
  return state;
}

bool IsShowDesktopActive() {
  const DefViewHost host = FindDefViewHost();
  if (!host.defview || !IsWindowAlive(host.defview)) {
    return false;
  }
  // Classic Win+D: shell marks DefView / Progman TOPMOST.
  if ((GetWindowLongPtrW(host.defview, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
    return true;
  }
  if (host.host &&
      (GetWindowLongPtrW(host.host, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
    return true;
  }
  // Modern Win10/11 often skips DefView TOPMOST. Do NOT equate Explorer FG
  // with show-desktop: clicking blank wallpaper focuses Progman while
  // APPWINDOWs stay above the band — Fences keeps overlays under those apps.
  // Only treat desktop FG as show-desktop when no restored non-shell window
  // sits above the DefView host (apps minimized / covered by Win+D).
  if (!IsExplorerDesktopForeground()) {
    return false;
  }
  HWND anchor = host.host ? host.host : host.defview;
  if (!anchor || !IsWindowAlive(anchor)) {
    return false;
  }
  HWND cur = anchor;
  for (int i = 0; i < 128; ++i) {
    HWND prev = GetWindow(cur, GW_HWNDPREV);
    if (!prev || prev == cur) {
      break;
    }
    cur = prev;
    if (!IsWindowVisible(cur) || IsIconic(cur)) {
      continue;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(cur, &pid);
    if (pid == GetCurrentProcessId()) {
      continue;
    }
    wchar_t cls[64] = {};
    GetClassNameW(cur, cls, 64);
    if (wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Progman") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 ||
        wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
      continue;
    }
    return false;
  }
  return true;
}

bool IsAppsForegroundQuiet() {
  // Equivalent to:
  //   !IsExplorerDesktopForeground() &&
  //   !(IsShowDesktopActive() && !HasVisibleFullscreenCover())
  // but skips EnumWindows cover scan unless DefView is TOPMOST (Win+D).
  if (IsExplorerDesktopForeground()) {
    return false;
  }
  const DefViewHost host = FindDefViewHost();
  const bool topmost_desk =
      (host.defview && IsWindowAlive(host.defview) &&
       (GetWindowLongPtrW(host.defview, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) ||
      (host.host && IsWindowAlive(host.host) &&
       (GetWindowLongPtrW(host.host, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
  if (!topmost_desk) {
    return true;
  }
  return HasVisibleFullscreenCover();
}

namespace {

// One restore pass raises N overlays; reuse the same "window above DefView"
// answer for ~80ms so we do not walk Z-order once per fence/pet/page-bar.
HWND g_above_cache = nullptr;
HWND g_above_host = nullptr;
ULONGLONG g_above_tick = 0;
constexpr ULONGLONG kAboveCacheTtlMs = 80;

HWND g_cover_cache = nullptr;
ULONGLONG g_cover_tick = 0;
bool g_cover_valid = false;
constexpr ULONGLONG kCoverCacheTtlMs = 400;
// Window we just asked to minimize (Win+D over a cover). Ignored as a cover
// until the animation lands, bounded by kCoverDismissGraceMs.
HWND g_cover_dismissing = nullptr;
ULONGLONG g_cover_dismiss_tick = 0;
constexpr ULONGLONG kCoverDismissGraceMs = 1500;

HWND WindowAboveDefView(HWND defview, HWND host, HWND self) {
  HWND anchor = host ? host : defview;
  if (!anchor || !IsWindow(anchor)) {
    return nullptr;
  }
  const ULONGLONG now = GetTickCount64();
  if (g_above_host == anchor && now - g_above_tick < kAboveCacheTtlMs &&
      (!g_above_cache || IsWindow(g_above_cache))) {
    return (g_above_cache == self) ? nullptr : g_above_cache;
  }
  HWND cur = anchor;
  HWND found = nullptr;
  for (int i = 0; i < 128; ++i) {
    HWND prev = GetWindow(cur, GW_HWNDPREV);
    if (!prev || prev == cur) {
      break;
    }
    cur = prev;
    if (cur == self) {
      continue;
    }
    if (IsWindowVisible(cur) && !IsIconic(cur)) {
      DWORD pid = 0;
      GetWindowThreadProcessId(cur, &pid);
      if (pid != GetCurrentProcessId()) {
        wchar_t cls[64] = {};
        GetClassNameW(cur, cls, 64);
        if (wcscmp(cls, L"WorkerW") != 0 && wcscmp(cls, L"Progman") != 0 &&
            wcscmp(cls, L"Shell_TrayWnd") != 0 &&
            wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0) {
          found = cur;
          break;
        }
      }
    }
  }
  g_above_host = anchor;
  g_above_cache = found;
  g_above_tick = now;
  return found;
}

bool AlreadyStackedAfter(HWND hwnd, HWND insert_after) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  if (insert_after == HWND_TOP) {
    return GetWindow(hwnd, GW_HWNDPREV) == nullptr;
  }
  if (insert_after == HWND_BOTTOM) {
    return GetWindow(hwnd, GW_HWNDNEXT) == nullptr;
  }
  if (!insert_after || !IsWindow(insert_after)) {
    return false;
  }
  return GetWindow(hwnd, GW_HWNDPREV) == insert_after;
}

bool CoversMonitorRect(HWND hwnd, const RECT& mon) {
  RECT wr{};
  if (!GetWindowRect(hwnd, &wr)) {
    return false;
  }
  constexpr int kTol = 8;
  return (wr.right - wr.left) > 0 && (wr.bottom - wr.top) > 0 &&
         wr.left <= mon.left + kTol && wr.top <= mon.top + kTol &&
         wr.right >= mon.right - kTol && wr.bottom >= mon.bottom - kTol;
}

bool IsShellFullscreenClass(HWND hwnd) {
  wchar_t cls[128]{};
  GetClassNameW(hwnd, cls, 128);
  return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
         wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
         wcscmp(cls, L"SysListView32") == 0 ||
         wcscmp(cls, L"DirectUIHWND") == 0 ||
         wcscmp(cls, L"Shell_TrayWnd") == 0 ||
         wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
         wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0 ||
         wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0 ||
         wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0;
}

struct CoverEnumCtx {
  RECT mon{};
  DWORD own_pid = 0;
  HWND found = nullptr;
};

BOOL CALLBACK EnumFullscreenCoverProc(HWND hwnd, LPARAM lp) {
  auto* ctx = reinterpret_cast<CoverEnumCtx*>(lp);
  if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
    return TRUE;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == ctx->own_pid) {
    return TRUE;
  }
  if (IsShellFullscreenClass(hwnd)) {
    return TRUE;
  }
  const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if (ex & WS_EX_TOOLWINDOW) {
    return TRUE;
  }
  if (CoversMonitorRect(hwnd, ctx->mon)) {
    ctx->found = hwnd;
    return FALSE;
  }
  return TRUE;
}

}  // namespace

HWND FindFullscreenCoverWindow() {
  const ULONGLONG now = GetTickCount64();
  if (g_cover_valid && now - g_cover_tick < kCoverCacheTtlMs &&
      (!g_cover_cache || (IsWindow(g_cover_cache) && IsWindowVisible(g_cover_cache) &&
                          !IsIconic(g_cover_cache)))) {
    return g_cover_cache;
  }
  HWND fg = GetForegroundWindow();
  HMONITOR mon = MonitorFromWindow(fg ? fg : GetDesktopWindow(),
                                   MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  if (!GetMonitorInfoW(mon, &mi)) {
    g_cover_cache = nullptr;
    g_cover_valid = true;
    g_cover_tick = now;
    return nullptr;
  }
  if (fg && IsWindowVisible(fg) && !IsIconic(fg) &&
      !IsShellFullscreenClass(fg) && CoversMonitorRect(fg, mi.rcMonitor)) {
    g_cover_cache = fg;
    g_cover_valid = true;
    g_cover_tick = now;
    return fg;
  }
  CoverEnumCtx ctx;
  ctx.mon = mi.rcMonitor;
  ctx.own_pid = GetCurrentProcessId();
  EnumWindows(EnumFullscreenCoverProc, reinterpret_cast<LPARAM>(&ctx));
  g_cover_cache = ctx.found;
  g_cover_valid = true;
  g_cover_tick = now;
  return ctx.found;
}

bool HasVisibleFullscreenCover() {
  if (IsCoverBeingDismissed()) {
    return false;
  }
  HWND cover = FindFullscreenCoverWindow();
  return cover != nullptr && IsWindowVisible(cover) && !IsIconic(cover);
}

void InvalidateFullscreenCoverCache() {
  // A minimize request (Win+D over a borderless game) is asynchronous: the
  // window keeps answering "visible, not iconic" for a few frames while the
  // animation runs. The 400ms cache then reports a cover that is on its way
  // out, which sinks the overlays instead of lifting them. Callers that just
  // issued the minimize drop the cache and mark the window as dismissing so
  // the re-enumeration does not simply find it again.
  g_cover_valid = false;
  g_cover_cache = nullptr;
  g_cover_tick = 0;
  g_cover_dismissing = FindFullscreenCoverWindow();
  g_cover_dismiss_tick = GetTickCount64();
}

bool IsCoverBeingDismissed() {
  if (!g_cover_dismissing) {
    return false;
  }
  const ULONGLONG now = GetTickCount64();
  // Bounded: a window that refuses to minimize must not stay ignored forever.
  if (now - g_cover_dismiss_tick > kCoverDismissGraceMs ||
      !IsWindowAlive(g_cover_dismissing) || IsIconic(g_cover_dismissing)) {
    g_cover_dismissing = nullptr;
    return false;
  }
  return true;
}

bool FilterDesktopChromeSysCommand(WPARAM wparam) {
  return (wparam & 0xFFF0) == SC_MINIMIZE;
}

bool FilterDesktopChromeWindowPosChanging(WINDOWPOS* wp) {
  if (!wp) {
    return false;
  }
  if (!(wp->flags & SWP_NOMOVE) && (wp->x <= -16000 || wp->y <= -16000)) {
    wp->flags |= SWP_NOMOVE | SWP_NOSIZE;
    return true;
  }
  return false;
}

void RaiseOverlayInDesktopBand(HWND hwnd) {
  RaiseOverlayInDesktopBandWith(hwnd, QueryDesktopBandState());
}

void RaiseOverlayInDesktopBandWith(HWND hwnd, const DesktopBandState& state) {
  if (!IsWindowAlive(hwnd)) {
    return;
  }
  DefViewHost host = FindDefViewHost();
  if (IsWallpaperWorkerW(OwnerOf(hwnd)) || IsWallpaperWorkerW(ParentOf(hwnd)) ||
      ParentOf(hwnd)) {
    DetachOverlayFromDesktop(hwnd);
    InvalidateDefViewCache();
    host = FindDefViewHost();
  }
  if (!host.host || IsWallpaperWorkerW(host.host)) {
    InvalidateDefViewCache();
    host = FindDefViewHost();
    if (!host.host || IsWallpaperWorkerW(host.host)) {
      return;
    }
  }

  const bool want_show =
      IsWindowVisible(hwnd) != FALSE && IsIconic(hwnd) == FALSE;
  if (!IsAttachedToDesktop(hwnd, &host)) {
    if (!SoftSeatToOwner(hwnd, host.host, want_show)) {
      return;
    }
  }

  const UINT flags = kSwpNoMoveNoSizeNoActivate | SWP_NOREDRAW;
  const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  const bool self_topmost = (ex & WS_EX_TOPMOST) != 0;

  // Root architectural invariant: Desktop overlays MUST NEVER be WS_EX_TOPMOST.
  // The Taskbar (Shell_TrayWnd) is WS_EX_TOPMOST; elevating desktop overlays to TOPMOST
  // causes them to sit above the taskbar and swallow mouse events.
  if (self_topmost) {
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
  }
  // A visible fullscreen cover (game / video / fullscreen browser) owns the
  // whole monitor: whatever the Z-order walk concludes, the overlay must stay
  // under it. Sinking to the desktop host is the only correct seat, and the
  // show-desktop TOP branch below must never fire over a cover.
  HWND insert_after = HWND_BOTTOM;
  if (state.fullscreen_cover) {
    insert_after = host.host ? host.host : HWND_BOTTOM;
  } else if (state.show_desktop) {
    // Win+D show-desktop → TOP with the shell. Mere desktop FG (click wallpaper
    // with apps still restored) must keep overlays under APPWINDOWs (Fences).
    insert_after = HWND_TOP;
  } else {
    HWND above = WindowAboveDefView(host.defview, host.host, hwnd);
    if (above && above != hwnd) {
      insert_after = above;
    } else {
      HWND fg = GetForegroundWindow();
      if (fg && IsWindowVisible(fg) && !IsIconic(fg)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid != GetCurrentProcessId() && !IsShellFullscreenClass(fg)) {
          insert_after = fg;
        } else {
          insert_after = host.host ? host.host : HWND_BOTTOM;
        }
      } else {
        insert_after = host.host ? host.host : HWND_BOTTOM;
      }
    }
  }
  // Skip no-op restacks: many open apps + N overlays × settle ticks used to
  // thrash DWM and make window drags feel stuck.
  if (!AlreadyStackedAfter(hwnd, insert_after)) {
    SetWindowPos(hwnd, insert_after, 0, 0, 0, 0, flags);
  }
  if (want_show) {
    ShowOverlayNoActivate(hwnd);
  }
}

bool IsOverlayAboveDefView(HWND hwnd, const DefViewHost* cached) {
  if (!IsWindowAlive(hwnd)) {
    return false;
  }
  const DefViewHost host = cached ? *cached : FindDefViewHost();
  if (!host.defview || !host.host) {
    return true;
  }
  if (!IsAttachedToDesktop(hwnd, &host)) {
    return false;
  }
  if (IsWallpaperWorkerW(OwnerOf(hwnd)) || IsWallpaperWorkerW(ParentOf(hwnd))) {
    return false;
  }
  if (IsIconic(hwnd)) {
    return false;
  }
  return true;
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
  // Fences-class 无闪烁切换：Show 优先于 Move，Hide 最后执行，避免新窗口出现前露出桌面底色
  std::stable_sort(items.begin(), items.end(),
                   [](const Item& a, const Item& b) {
                     const int oa = a.op == kShow ? 0 : (a.op == kMove ? 1 : 2);
                     const int ob = b.op == kShow ? 0 : (b.op == kMove ? 1 : 2);
                     if (oa != ob) {
                       return oa < ob;
                     }
                     return reinterpret_cast<uintptr_t>(a.hwnd) <
                            reinterpret_cast<uintptr_t>(b.hwnd);
                   });

  for (auto& it : items) {
    if (it.op != kMove) {
      continue;
    }
    HWND parent = ParentOf(it.hwnd);
    if (parent) {
      POINT pt{it.x, it.y};
      if (ScreenToClient(parent, &pt)) {
        it.x = pt.x;
        it.y = pt.y;
      }
    }
  }

  auto flags_for = [](Op op) -> UINT {
    const UINT base = SWP_NOZORDER | SWP_NOACTIVATE;
    if (op == kHide) {
      // Soft-park: keep UpdateLayeredWindow pixels. SWP_NOCOPYBITS used to
      // discard the buffer so every page switch cold-painted every fence.
      return base | SWP_NOMOVE | SWP_NOSIZE | SWP_HIDEWINDOW | SWP_NOREDRAW;
    }
    if (op == kShow) {
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
