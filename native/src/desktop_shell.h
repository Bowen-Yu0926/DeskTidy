#pragma once

#include <Windows.h>

// Fences desktop-layer shell helpers (native).
// Overlays are owned by the DefView host and stay DefView-adjacent -
// never HWND_TOP of the non-topmost app band.

struct DefViewHost {
  HWND host = nullptr;     // Progman or WorkerW containing DefView
  HWND defview = nullptr;  // SHELLDLL_DefView
};

DefViewHost FindDefViewHost();
bool IsWallpaperWorkerW(HWND hwnd);
bool IsAttachedToDesktop(HWND hwnd, const DefViewHost* cached = nullptr);
bool AttachOverlayToDesktop(HWND hwnd, bool show = true);
bool DetachOverlayFromDesktop(HWND hwnd);
void PlaceOverlayInDesktopBand(HWND hwnd, bool force = false);
void RaiseOverlayInDesktopBand(HWND hwnd);  // DefView-adjacent only
