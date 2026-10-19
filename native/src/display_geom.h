#pragma once

#include <Windows.h>

#include <vector>

struct MonitorInfo {
  HMONITOR handle = nullptr;
  RECT monitor{};
  RECT work{};
  bool primary = false;
};

std::vector<MonitorInfo> EnumMonitors();
RECT VirtualScreenRect();
RECT PrimaryWorkArea();
// Monitor containing pt (or nearest).
RECT WorkAreaAtPoint(POINT pt);
// Clamp window rect into nearest monitor work area (keeps size if possible).
void ClampRectToWorkArea(RECT* rc);
