#include "display_geom.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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
  wcsncpy_s(info.device, mi.szDevice, _TRUNCATE);
  out->push_back(info);
  return TRUE;
}

using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);

UINT MonitorDpi(HMONITOR mon) {
  static GetDpiForMonitorFn fn = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
      fn = reinterpret_cast<GetDpiForMonitorFn>(
          GetProcAddress(shcore, "GetDpiForMonitor"));
      // Keep shcore loaded for process lifetime.
    }
  }
  if (fn && mon) {
    UINT dpi_x = 96;
    UINT dpi_y = 96;
    // MDT_EFFECTIVE_DPI == 0
    if (SUCCEEDED(fn(mon, 0, &dpi_x, &dpi_y)) && dpi_x > 0) {
      return dpi_x;
    }
  }
  return 96;
}

void PhysicalMm(const MonitorInfo& m, int* mm_w, int* mm_h) {
  *mm_w = 520;
  *mm_h = 290;
  HDC dc = CreateDCW(L"DISPLAY", m.device[0] ? m.device : nullptr, nullptr,
                     nullptr);
  if (!dc) {
    return;
  }
  const int w = GetDeviceCaps(dc, HORZSIZE);
  const int h = GetDeviceCaps(dc, VERTSIZE);
  DeleteDC(dc);
  if (w > 0) {
    *mm_w = w;
  }
  if (h > 0) {
    *mm_h = h;
  }
}

std::string FormatDpr(double dpr) {
  char buf[32];
  sprintf_s(buf, "%.2f", dpr);
  std::string s(buf);
  while (!s.empty() && s.back() == '0') {
    s.pop_back();
  }
  if (!s.empty() && s.back() == '.') {
    s.pop_back();
  }
  if (s.empty()) {
    return "1";
  }
  return s;
}

bool ParsePanelPixels(const std::string& part, int* w, int* h) {
  // `{x},{y},{w}x{h}@...`
  const size_t c1 = part.find(',');
  if (c1 == std::string::npos) {
    return false;
  }
  const size_t c2 = part.find(',', c1 + 1);
  if (c2 == std::string::npos) {
    return false;
  }
  const size_t at = part.find('@', c2 + 1);
  const std::string wh =
      part.substr(c2 + 1, at == std::string::npos ? std::string::npos : at - c2 - 1);
  const size_t x = wh.find('x');
  if (x == std::string::npos) {
    return false;
  }
  *w = atoi(wh.substr(0, x).c_str());
  *h = atoi(wh.substr(x + 1).c_str());
  return *w > 0 && *h > 0;
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

std::string DisplayFingerprint() {
  auto mons = EnumMonitors();
  if (mons.empty()) {
    return "0,0,1920x1080@520x290mm@1";
  }
  std::sort(mons.begin(), mons.end(), [](const MonitorInfo& a,
                                         const MonitorInfo& b) {
    if (a.monitor.left != b.monitor.left) {
      return a.monitor.left < b.monitor.left;
    }
    return a.monitor.top < b.monitor.top;
  });

  std::string out;
  for (size_t i = 0; i < mons.size(); ++i) {
    const auto& m = mons[i];
    const int x = m.monitor.left;
    const int y = m.monitor.top;
    const int w = m.monitor.right - m.monitor.left;
    const int h = m.monitor.bottom - m.monitor.top;
    int mm_w = 520;
    int mm_h = 290;
    PhysicalMm(m, &mm_w, &mm_h);
    if (mm_w < 1) {
      mm_w = 520;
    }
    if (mm_h < 1) {
      mm_h = 290;
    }
    const double dpr = static_cast<double>(MonitorDpi(m.handle)) / 96.0;
    char buf[160];
    sprintf_s(buf, "%d,%d,%dx%d@%dx%dmm@%s", x, y, w, h, mm_w, mm_h,
              FormatDpr(dpr).c_str());
    if (i > 0) {
      out.push_back('|');
    }
    out += buf;
  }
  return out;
}

bool FingerprintIsTransient(const std::string& fp) {
  if (fp.empty()) {
    return false;
  }
  size_t start = 0;
  while (start <= fp.size()) {
    size_t end = fp.find('|', start);
    if (end == std::string::npos) {
      end = fp.size();
    }
    const std::string part = fp.substr(start, end - start);
    // trim spaces
    size_t a = 0;
    while (a < part.size() && (part[a] == ' ' || part[a] == '\t')) {
      ++a;
    }
    size_t b = part.size();
    while (b > a && (part[b - 1] == ' ' || part[b - 1] == '\t')) {
      --b;
    }
    if (b > a) {
      int w = 0;
      int h = 0;
      if (ParsePanelPixels(part.substr(a, b - a), &w, &h)) {
        // Primary pixels both < 900, or square ~800 (Qt offscreen / tiny panels).
        if (w < 900 && h < 900) {
          return true;
        }
        if (w == h && w <= 1024) {
          return true;
        }
        // Match Python: any panel narrower than a typical laptop logical width.
        if (w > 0 && w < 1280) {
          return true;
        }
      }
    }
    if (end == fp.size()) {
      break;
    }
    start = end + 1;
  }
  return false;
}
