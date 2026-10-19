#include "display_geom.h"

namespace {

BOOL CALLBACK EnumProc(HMONITOR mon, HDC, LPRECT, LPARAM lparam) {
  auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi)) {
    return TRUE;
  }
  MonitorInfo info;
  info.handle = mon;
  info.monitor = mi.rcMonitor;
  info.work = mi.rcWork;
  info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  out->push_back(info);
  return TRUE;
}

}  // namespace

std::vector<MonitorInfo> EnumMonitors() {
  std::vector<MonitorInfo> out;
  EnumDisplayMonitors(nullptr, nullptr, EnumProc,
                      reinterpret_cast<LPARAM>(&out));
  return out;
}

RECT VirtualScreenRect() {
  RECT rc{};
  rc.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  rc.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  rc.right = rc.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  rc.bottom = rc.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (rc.right <= rc.left || rc.bottom <= rc.top) {
    rc.left = 0;
    rc.top = 0;
    rc.right = GetSystemMetrics(SM_CXSCREEN);
    rc.bottom = GetSystemMetrics(SM_CYSCREEN);
  }
  return rc;
}

RECT PrimaryWorkArea() {
  for (const auto& m : EnumMonitors()) {
    if (m.primary) {
      return m.work;
    }
  }
  RECT rc{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
  return rc;
}

RECT WorkAreaAtPoint(POINT pt) {
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(mon, &mi)) {
    return mi.rcWork;
  }
  return PrimaryWorkArea();
}

void ClampRectToWorkArea(RECT* rc) {
  if (!rc) {
    return;
  }
  POINT center{(rc->left + rc->right) / 2, (rc->top + rc->bottom) / 2};
  const RECT work = WorkAreaAtPoint(center);
  const int w = rc->right - rc->left;
  const int h = rc->bottom - rc->top;
  int x = rc->left;
  int y = rc->top;
  if (x + w > work.right) {
    x = work.right - w;
  }
  if (y + h > work.bottom) {
    y = work.bottom - h;
  }
  if (x < work.left) {
    x = work.left;
  }
  if (y < work.top) {
    y = work.top;
  }
  rc->left = x;
  rc->top = y;
  rc->right = x + w;
  rc->bottom = y + h;
}
