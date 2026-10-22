#pragma once

#include <Windows.h>

#include <string>
#include <vector>

struct MonitorInfo {
  HMONITOR handle = nullptr;
  RECT monitor{};
  RECT work{};
  bool primary = false;
  wchar_t device[CCHDEVICENAME]{};
};

std::vector<MonitorInfo> EnumMonitors();
RECT VirtualScreenRect();
RECT PrimaryWorkArea();
// Monitor containing pt (or nearest).
RECT WorkAreaAtPoint(POINT pt);
// Clamp window rect into nearest monitor work area (keeps size if possible).
void ClampRectToWorkArea(RECT* rc);

// Stable key: sorted monitors as `{x},{y},{w}x{h}@{mmW}x{mmH}mm@{dpr}` joined by `|`.
std::string DisplayFingerprint();
// True for plug/unplug intermediates and tiny/square fake panels (~800x800).
bool FingerprintIsTransient(const std::string& fp);

// ---------------------------------------------------------------------------
// G6 — runtime DPI change recalc.
// ---------------------------------------------------------------------------
// Effective DPI of the monitor containing pt (nearest), else system DPI.
int DpiAtPoint(POINT pt);
int SystemDpi();
// Scale a fence rect from one DPI to another (ratio round). No-op when equal.
void ScaleRectForDpi(int* x, int* y, int* w, int* h, int old_dpi, int new_dpi);

// ---------------------------------------------------------------------------
// G7 — Explorer icon grid metrics (Python desktop_icon_metrics.py).
// ---------------------------------------------------------------------------
struct IconGridMetrics {
  int cell_w = 96;   // grid pitch (logical px @ 96 DPI)
  int cell_h = 102;
  int icon_size = 40;
  int caption_lines = 2;
};
// Explorer DefView LVM spacing → SPI icon spacing → safe fallback, with
// DPI/font-scaled comfort padding. Falls back to the legacy fixed values.
IconGridMetrics QueryIconGridMetrics();
// Scale metrics from one DPI to another (ratio round). No-op when equal.
void ScaleIconGridForDpi(IconGridMetrics* m, int old_dpi, int new_dpi);
