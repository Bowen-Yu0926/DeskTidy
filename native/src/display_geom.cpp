#include "display_geom.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <winreg.h>

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

// ---------------------------------------------------------------------------
// G6 / G7 — DPI recalc + Explorer icon grid metrics.
// ---------------------------------------------------------------------------
namespace {

constexpr UINT kSpiIconHorz = 0x000D;
constexpr UINT kSpiIconVert = 0x0018;
constexpr UINT kSpiIconTitleWrap = 0x0019;
constexpr UINT kLvmGetItemSpacing = 0x1000 + 51;  // LVM_FIRST + 51

HWND FindDesktopListView() {
  HWND progman = FindWindowW(L"Progman", nullptr);
  if (progman) {
    HWND def = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (def) {
      HWND lv = FindWindowExW(def, nullptr, L"SysListView32", nullptr);
      if (lv) {
        return lv;
      }
    }
  }
  HWND worker = nullptr;
  while ((worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) !=
         nullptr) {
    HWND def = FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr);
    if (def) {
      HWND lv = FindWindowExW(def, nullptr, L"SysListView32", nullptr);
      if (lv) {
        return lv;
      }
    }
  }
  return nullptr;
}

int DesktopBagIconSize() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\Shell\\Bags\\1\\Desktop", 0,
                    KEY_READ, &key) == ERROR_SUCCESS) {
    DWORD raw = 0;
    DWORD type = 0;
    DWORD size = sizeof(raw);
    const LONG rc = RegQueryValueExW(key, L"IconSize", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&raw), &size);
    RegCloseKey(key);
    if (rc == ERROR_SUCCESS && type == REG_DWORD && raw >= 16 && raw <= 256) {
      return static_cast<int>(raw);
    }
  }
  return 0;
}

int RoundScale(int v, double k) {
  return static_cast<int>(llround(static_cast<double>(v) * k));
}

}  // namespace

int SystemDpi() {
  HDC dc = GetDC(nullptr);
  if (dc) {
    const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    if (dpi > 0) {
      return dpi;
    }
  }
  return 96;
}

int DpiAtPoint(POINT pt) {
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  if (mon) {
    const UINT dpi = MonitorDpi(mon);
    if (dpi > 0) {
      return static_cast<int>(dpi);
    }
  }
  return SystemDpi();
}

void ScaleRectForDpi(int* x, int* y, int* w, int* h, int old_dpi, int new_dpi) {
  if (old_dpi <= 0) {
    old_dpi = 96;
  }
  if (new_dpi <= 0) {
    new_dpi = 96;
  }
  if (old_dpi == new_dpi) {
    return;
  }
  const double k = static_cast<double>(new_dpi) / old_dpi;
  if (x) {
    *x = RoundScale(*x, k);
  }
  if (y) {
    *y = RoundScale(*y, k);
  }
  if (w) {
    *w = (std::max)(1, RoundScale(*w, k));
  }
  if (h) {
    *h = (std::max)(1, RoundScale(*h, k));
  }
}

void ScaleIconGridForDpi(IconGridMetrics* m, int old_dpi, int new_dpi) {
  if (!m) {
    return;
  }
  if (old_dpi <= 0) {
    old_dpi = 96;
  }
  if (new_dpi <= 0) {
    new_dpi = 96;
  }
  if (old_dpi == new_dpi) {
    return;
  }
  const double k = static_cast<double>(new_dpi) / old_dpi;
  m->cell_w = (std::max)(48, RoundScale(m->cell_w, k));
  m->cell_h = (std::max)(48, RoundScale(m->cell_h, k));
  m->icon_size = (std::max)(16, RoundScale(m->icon_size, k));
}

IconGridMetrics QueryIconGridMetrics() {
  // Legacy fixed defaults — preserved when Explorer metrics are unavailable.
  IconGridMetrics out;
  const double scale = (std::max)(1.0, static_cast<double>(SystemDpi()) / 96.0);

  int native_w = 0;
  int native_h = 0;
  int lines = 2;

  // 1) Explorer DefView large-icon spacing (actual desktop pitch).
  const HWND lv = FindDesktopListView();
  if (lv) {
    const LRESULT raw = SendMessageW(lv, kLvmGetItemSpacing, 0, 0);
    const int cw = static_cast<int>(raw & 0xFFFF);
    const int ch = static_cast<int>((raw >> 16) & 0xFFFF);
    if (cw >= 32 && ch >= 32) {
      native_w = cw;
      native_h = ch;
    }
  }

  // 2) SPI icon spacing fallback.
  if (native_w == 0) {
    UINT spi_w = 0;
    UINT spi_h = 0;
    BOOL wrap = FALSE;
    SystemParametersInfoW(kSpiIconHorz, 0, &spi_w, 0);
    SystemParametersInfoW(kSpiIconVert, 0, &spi_h, 0);
    SystemParametersInfoW(kSpiIconTitleWrap, 0, &wrap, 0);
    if (spi_w >= 32 && spi_h >= 32) {
      native_w = static_cast<int>(spi_w);
      native_h = static_cast<int>(spi_h);
      lines = wrap ? 2 : 1;
    }
  }

  if (native_w == 0 || native_h == 0) {
    return out;  // keep legacy defaults
  }

  const int icon = DesktopBagIconSize();
  out.icon_size = icon > 0 ? icon : out.icon_size;
  out.caption_lines = lines < 2 ? 1 : 2;

  // Comfort padding (Python _comfort_cell_size / desktop_icon_footprint_height).
  const int cjk = (std::max)(8, RoundScale(12, scale));
  const int side_chrome = (std::max)(cjk, RoundScale(10, scale));
  const int line_h = (std::max)(12, RoundScale(18, scale));
  const int target_w0 = cjk * 8 + side_chrome;
  const int target_w = (std::max)(target_w0, RoundScale(native_w, 1.55));
  const int margin = (std::max)(2, RoundScale(3, scale));
  const int gap = (std::max)(2, RoundScale(3, scale));
  const int shelf = (std::max)(16, line_h * out.caption_lines + 10);
  const int pitch = (std::max)(8, RoundScale(line_h, 0.7));
  const int footprint = margin + out.icon_size + gap + shelf + margin + pitch;
  const int target_h = (std::max)(native_h, footprint);

  out.cell_w =
      (std::max)(48, (std::max)(native_w, (std::max)(target_w, out.icon_size + cjk * 4)));
  out.cell_h = (std::max)(48, target_h);
  const int min_caption = (std::max)(16, line_h);
  out.cell_h = (std::max)(out.icon_size + min_caption, out.cell_h);
  return out;
}
