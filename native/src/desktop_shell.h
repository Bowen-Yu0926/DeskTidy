#pragma once

#include <Windows.h>

#include <vector>

// Fences / Rainmeter desktop-layer shell helpers (native).
//
// Seat: Progman/WorkerW-owned top-level WS_POPUP | WS_EX_LAYERED (Rainmeter
// OnDesktop / classic Fences-class). HWND_BOTTOM under APPWINDOWs; ownership
// keeps chrome above DefView icons.
//
// Win11 24H2+ raised desktop (Progman WS_EX_NOREDIRECTIONBITMAP): DefView
// *children* do not compose UpdateLayeredWindow pixels — do NOT SetParent to
// DefView. Soft-heal owner only (no hard Detach flash).
//
// Peek: DetachOverlayFromDesktop + HWND_TOPMOST.

struct DefViewHost {
  HWND host = nullptr;     // Progman or WorkerW containing DefView
  HWND defview = nullptr;  // SHELLDLL_DefView
};

DefViewHost FindDefViewHost();
void InvalidateDefViewCache();
bool IsWallpaperWorkerW(HWND hwnd);
// Win11 24H2+ "raised desktop": Progman has WS_EX_NOREDIRECTIONBITMAP.
// Runtime detect (Fences-class) — one binary for Win10 + Win11, no split builds.
bool IsRaisedDesktopShell();
bool IsAttachedToDesktop(HWND hwnd, const DefViewHost* cached = nullptr);
bool AttachOverlayToDesktop(HWND hwnd, bool show = true);
bool ForceAttachOverlayToDesktop(HWND hwnd, bool show = true);
bool DetachOverlayFromDesktop(HWND hwnd);
void PlaceOverlayInDesktopBand(HWND hwnd, bool force = false);
void RaiseOverlayInDesktopBand(HWND hwnd);
bool IsOverlayAboveDefView(HWND hwnd, const DefViewHost* cached = nullptr);
bool IsExplorerDesktopForeground();
bool IsShowDesktopActive();
// True when normal apps own FG (not Explorer desktop / Win+D). Fences-class
// quiet chrome: skip band restack probes. Avoids EnumWindows cover scans on
// the common multi-app path (cover only matters for TOPMOST show-desktop).
bool IsAppsForegroundQuiet();
// Boot / diagnostics: OS build + DefView host + raised-desktop bit (one line).
void LogDesktopShellCompatProfile();
// Monitor-covering APPWINDOW still visible (borderless/fullscreen game).
// Fences/Rainmeter: do not ride DefView TOPMOST over a live cover — that only
// "lifts fences" without actually showing the desktop.
HWND FindFullscreenCoverWindow();
bool HasVisibleFullscreenCover();
bool FilterDesktopChromeSysCommand(WPARAM wparam);
bool FilterDesktopChromeWindowPosChanging(WINDOWPOS* wp);

bool SetOverlayScreenPos(HWND hwnd, int x, int y, int w, int h, UINT flags);
bool MoveOverlayScreen(HWND hwnd, int x, int y);

// Layered blit. If hwnd is unexpectedly parented, omit pptDst (child ULW
// treats screen coords as client and drifts).
bool UpdateOverlayLayered(HWND hwnd, HDC hdc_dst, const POINT* screen_origin,
                          const SIZE* size, HDC hdc_src, const POINT* ppt_src,
                          COLORREF key, BLENDFUNCTION* blend, DWORD flags);

class OverlayWindowBatch {
 public:
  void Hide(HWND hwnd);
  void Show(HWND hwnd);
  void SetRect(HWND hwnd, int x, int y, int w, int h);
  void Commit();

 private:
  enum Op : int { kHide = 1, kShow = 2, kMove = 3 };
  struct Item {
    Op op;
    HWND hwnd;
    int x = 0, y = 0, w = 0, h = 0;
  };
  std::vector<Item> items_;
};

bool PollExplorerDesktopRebuild();
void ResetExplorerRebuildTracking();
