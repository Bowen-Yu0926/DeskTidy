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
