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

// Monitor list cache (drag/resize hot path): EnumDisplayMonitors +
// GetMonitorInfoW per monitor was re-enumerated on every SnapRectToWorkArea
// during a drag. Reuse for a short TTL, invalidating early when the monitor
// count changes (plug/unplug) — same short-TTL pattern as icon grid metrics.
std::vector<MonitorInfo> g_mon_cache;
ULONGLONG g_mon_tick = 0;
bool g_mon_valid = false;
constexpr ULONGLONG kMonCacheTtlMs = 500;

const std::vector<MonitorInfo>& CachedMonitors() {
  const ULONGLONG now = GetTickCount64();
  const int count = GetSystemMetrics(SM_CMONITORS);
  if (g_mon_valid && now - g_mon_tick < kMonCacheTtlMs &&
      static_cast<int>(g_mon_cache.size()) == count) {
    return g_mon_cache;
  }
  g_mon_cache = EnumMonitors();
  g_mon_tick = now;
  g_mon_valid = true;
  return g_mon_cache;
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
  for (const auto& m : CachedMonitors()) {
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

RECT WorkAreaForRect(const RECT& rc) {
  // Match Python snap_geometry: pick by overlap area; dual-overlap → primary.
  const auto& mons = CachedMonitors();
  if (mons.empty()) {
    return PrimaryWorkArea();
  }
  struct Hit {
    LONGLONG area = 0;
    RECT work{};
    bool primary = false;
  };
  std::vector<Hit> hits;
  hits.reserve(mons.size());
  for (const auto& m : mons) {
    const int l = (std::max)(rc.left, m.work.left);
    const int t = (std::max)(rc.top, m.work.top);
    const int r = (std::min)(rc.right, m.work.right);
    const int b = (std::min)(rc.bottom, m.work.bottom);
    if (r <= l || b <= t) {
      continue;
    }
    Hit h;
    h.area = static_cast<LONGLONG>(r - l) * static_cast<LONGLONG>(b - t);
    h.work = m.work;
    h.primary = m.primary;
    hits.push_back(h);
  }
  if (hits.size() >= 2) {
    for (const auto& h : hits) {
      if (h.primary) {
        return h.work;
      }
    }
  }
  if (!hits.empty()) {
    std::sort(hits.begin(), hits.end(),
              [](const Hit& a, const Hit& b) { return a.area > b.area; });
    return hits.front().work;
  }
  // Fully off-screen / unplugged: nearest to center, else primary.
  POINT center{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
  return WorkAreaAtPoint(center);
}

void ClampRectToWorkArea(RECT* rc) {
  if (!rc) {
    return;
  }
  const RECT work = WorkAreaForRect(*rc);
  const int max_w = (std::max)(1, static_cast<int>(work.right - work.left));
  const int max_h = (std::max)(1, static_cast<int>(work.bottom - work.top));
  // Cap size first (Fences/snap_geometry): a partition cannot outgrow its
  // monitor work area — otherwise hotplug scale bugs persist as "fullscreen".
  int w = rc->right - rc->left;
  int h = rc->bottom - rc->top;
  if (w < 1) {
    w = 1;
  }
  if (h < 1) {
    h = 1;
  }
  if (w > max_w) {
    w = max_w;
  }
  if (h > max_h) {
    h = max_h;
  }
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

void SnapRectToWorkArea(RECT* rc, int threshold) {
  if (!rc) {
    return;
  }
  const RECT work = WorkAreaForRect(*rc);
  POINT center{(rc->left + rc->right) / 2, (rc->top + rc->bottom) / 2};
  int thr = threshold;
  if (thr <= 0) {
    const int dpi = DpiAtPoint(center);
    thr = (std::max)(24, MulDiv(24, dpi, 96));
  }
  const int top_thr = (std::min)(8, thr);

  int x = rc->left;
  int y = rc->top;
  int w = rc->right - rc->left;
  int h = rc->bottom - rc->top;
  if (w < 1) {
    w = 1;
  }
  if (h < 1) {
    h = 1;
  }

  auto AbsI = [](int v) { return v < 0 ? -v : v; };
  if (AbsI(x - work.left) <= thr) {
    x = work.left;
  } else if (AbsI((x + w) - work.right) <= thr) {
    x = work.right - w;
  }
  // Tighter top snap — loose top + desktop parenting left fences stuck at y=0.
  if (AbsI(y - work.top) <= top_thr) {
    y = work.top;
  } else if (AbsI((y + h) - work.bottom) <= thr) {
    y = work.bottom - h;
  }

  const int max_w = work.right - work.left;
  const int max_h = work.bottom - work.top;
  if (w > max_w) {
    w = max_w;
  }
  if (h > max_h) {
    h = max_h;
  }
  if (x < work.left) {
    x = work.left;
  }
  if (y < work.top) {
    y = work.top;
  }
  if (x + w > work.right) {
    x = work.right - w;
  }
  if (y + h > work.bottom) {
    y = work.bottom - h;
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
        // Tiny / square fake panels during plug-unplug (Qt offscreen ~800²).
        // Do NOT treat every w<1280 panel as transient — that killed profiles for
        // portrait secondaries, 1280-wide laptops, and projectors (Fences keeps
        // those topologies).
        if (w < 900 && h < 900) {
          return true;
        }
        if (w == h && w <= 1024) {
          return true;
        }
        // Hotplug stub: absurdly short work height on a normal-width panel.
        if (w >= 640 && h > 0 && h < 400) {
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

namespace {
IconGridMetrics g_grid_cache{};
ULONGLONG g_grid_tick = 0;
bool g_grid_valid = false;
// Public SetItems + Layout often query twice in one refresh — DefView walk +
// SendMessage + registry IconSize is too heavy for every Layout tick.
constexpr ULONGLONG kGridCacheTtlMs = 750;
}  // namespace

void InvalidateIconGridMetricsCache() {
  g_grid_valid = false;
  g_grid_tick = 0;
}

IconGridMetrics QueryIconGridMetrics() {
  const ULONGLONG now = GetTickCount64();
  if (g_grid_valid && now - g_grid_tick < kGridCacheTtlMs) {
    return g_grid_cache;
  }

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
    g_grid_cache = out;
    g_grid_tick = now;
    g_grid_valid = true;
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
  g_grid_cache = out;
  g_grid_tick = now;
  g_grid_valid = true;
  return out;
}

int MinFenceHeightForIconRows(int rows, const IconGridMetrics* metrics) {
  const IconGridMetrics local = metrics ? *metrics : QueryIconGridMetrics();
  const int n = (std::max)(1, rows);
  // Match fence_window chrome: header 32 + bottom pad 12 @ 96dpi, then DPI.
  const int dpi = SystemDpi();
  const int header = MulDiv(32, dpi, 96);
  const int pad = MulDiv(12, dpi, 96);
  const int cell = (std::max)(48, local.cell_h);
  return header + pad + n * cell;
}

int ScalePxForDpi(int px, int dpi) {
  if (dpi <= 0) {
    dpi = SystemDpi();
  }
  if (dpi == 96) {
    return px;
  }
  return MulDiv(px, dpi, 96);
}

void ScaleDialogSizeForPoint(POINT at, int* width, int* height) {
  if (!width || !height) {
    return;
  }
  const int dpi = DpiAtPoint(at);
  *width = ScalePxForDpi(*width, dpi);
  *height = ScalePxForDpi(*height, dpi);
  const RECT work = WorkAreaAtPoint(at);
  const int max_w = (std::max)(200, static_cast<int>(work.right - work.left) * 90 / 100);
  const int max_h = (std::max)(160, static_cast<int>(work.bottom - work.top) * 90 / 100);
  if (*width > max_w) {
    *width = max_w;
  }
  if (*height > max_h) {
    *height = max_h;
  }
  if (*width < 200) {
    *width = 200;
  }
  if (*height < 120) {
    *height = 120;
  }
}
