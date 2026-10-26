#pragma once

#include <Windows.h>

#include <vector>

// Fences desktop-layer shell helpers (native).
// Overlays are owned by the DefView host and sit just above DefView
// (under normal APPWINDOWs). After Win+D, when DefView is topmost,
// raise uses HWND_TOP of the non-topmost band so fences are not buried.

struct DefViewHost {
  HWND host = nullptr;     // Progman or WorkerW containing DefView
  HWND defview = nullptr;  // SHELLDLL_DefView
};

DefViewHost FindDefViewHost();
void InvalidateDefViewCache();
bool IsWallpaperWorkerW(HWND hwnd);
bool IsAttachedToDesktop(HWND hwnd, const DefViewHost* cached = nullptr);
bool AttachOverlayToDesktop(HWND hwnd, bool show = true);
bool ForceAttachOverlayToDesktop(HWND hwnd, bool show = true);
bool DetachOverlayFromDesktop(HWND hwnd);
void PlaceOverlayInDesktopBand(HWND hwnd, bool force = false);
void RaiseOverlayInDesktopBand(HWND hwnd);  // DefView-adjacent only
bool IsExplorerDesktopForeground();

// Market pattern (Fences / Python OverlayWindowBatch): one DeferWindowPos for
// page-switch hide+show. N individual ShowWindow calls flash the desktop.
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

// Explorer-restart guard: returns true once when the DefView HWND changes
// (Explorer crashed / restarted the desktop shell). Cheap — FindWindow only.
// First call records the current HWND and returns false.
bool PollExplorerDesktopRebuild();
// Forget the tracked DefView HWND (e.g. after a rebuild has been handled or
// when overlays are intentionally re-seated).
void ResetExplorerRebuildTracking();
