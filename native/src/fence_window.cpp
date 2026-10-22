#include "fence_window.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "fence_style.h"
#include "icon_actions.h"
#include "log.h"
#include "shell_clipboard.h"
#include "i18n.h"
#include "shell_context_menu.h"
#include "toast.h"
#include "whitelist.h"

#include <ShlObj.h>
#include <Shellapi.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeFence";
constexpr int kHeaderH0 = 32;
constexpr int kCollapsedH0 = 44;
constexpr int kPad0 = 10;
// Wider cells + 2-line captions (Explorer / Fences / Python FenceIconItem).
constexpr int kCellW0 = 96;
constexpr int kCellH0 = 102;
constexpr int kIconSize0 = 40;  // match Python FenceIconItem.DEFAULT_ICON_SIZE
constexpr int kListRowH0 = 28;
constexpr int kListIcon0 = 20;
constexpr int kDragThreshold = 6;
constexpr int kEdge = 8;
// DeskTidy extras prepended to the hosted shell menu (ids 1..99).
constexpr int kExtFenceRefresh = 1;
constexpr int kExtFenceHide = 2;
constexpr int kExtFenceDissolve = 3;
constexpr int kExtUnpin = 1;
constexpr int kExtWhitelist = 2;
constexpr int kExtMoveFence = 3;
constexpr int kExtMovePage = 4;
constexpr int kExtOpenNotes = 5;
bool g_class_registered = false;

std::wstring DesktopDirectory() {
  PWSTR p = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p) {
    std::wstring dir(p);
    CoTaskMemFree(p);
    return dir;
  }
  return L"";
}

int FenceDpi(HWND hwnd) {
  using Fn = UINT(WINAPI*)(HWND);
  static Fn get_dpi = reinterpret_cast<Fn>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (get_dpi && hwnd) {
    const UINT dpi = get_dpi(hwnd);
    if (dpi > 0) {
      return static_cast<int>(dpi);
    }
  }
  HDC dc = GetDC(nullptr);
  const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(nullptr, dc);
  return dpi > 0 ? dpi : 96;
}

// Explorer / Fences style: translucent accent plate. Must use GDI+ (not GDI
// RoundRect) so per-pixel alpha stays valid for UpdateLayeredWindow.
void PaintSelectionChrome(Gdiplus::Graphics& g, int left, int top, int right,
                          int bottom, COLORREF accent, int radius,
                          BYTE fill_a = 110, BYTE edge_a = 220) {
  const int w = right - left;
  const int h = bottom - top;
  if (w < 4 || h < 4) {
    return;
  }
  Gdiplus::GraphicsPath path;
  const int rr = (std::min)(radius * 2, (std::min)(w, h));
  if (rr > 2) {
    path.AddArc(left, top, rr, rr, 180.f, 90.f);
    path.AddArc(right - rr, top, rr, rr, 270.f, 90.f);
    path.AddArc(right - rr, bottom - rr, rr, rr, 0.f, 90.f);
    path.AddArc(left, bottom - rr, rr, rr, 90.f, 90.f);
    path.CloseFigure();
  } else {
    path.AddRectangle(Gdiplus::Rect(left, top, w, h));
  }
  Gdiplus::SolidBrush fill(Gdiplus::Color(
      fill_a, GetRValue(accent), GetGValue(accent), GetBValue(accent)));
  g.FillPath(&fill, &path);
  if (edge_a > 0) {
    Gdiplus::Pen edge(
        Gdiplus::Color(edge_a, GetRValue(accent), GetGValue(accent),
                       GetBValue(accent)),
        1.5f);
    edge.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawPath(&edge, &path);
  }
}

int S(HWND hwnd, int px) { return MulDiv(px, FenceDpi(hwnd), 96); }

// opacity: 255 = full; ~107 (~0.42) matches Python cut ghost.
void DrawIconSharp(Gdiplus::Graphics& g, HICON icon, int x, int y, int dest,
                   BYTE opacity = 255) {
  if (!icon || dest <= 0) {
    return;
  }
  ICONINFO ii{};
  int src = dest;
  if (GetIconInfo(icon, &ii)) {
    BITMAP bm{};
    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm) == sizeof(bm) &&
        bm.bmWidth > 0) {
      src = bm.bmWidth;
    }
    if (ii.hbmColor) {
      DeleteObject(ii.hbmColor);
    }
    if (ii.hbmMask) {
      DeleteObject(ii.hbmMask);
    }
  }
  src = (std::max)(dest, (std::min)(src, 256));

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = src;
  bmi.bmiHeader.biHeight = -src;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ old = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(src) * src * 4);
  DrawIconEx(mem, 0, 0, icon, src, src, 0, nullptr, DI_NORMAL);
  auto* px = static_cast<BYTE*>(bits);
  const int n = src * src;
  for (int i = 0; i < n; ++i) {
    BYTE* p = px + i * 4;
    BYTE a = p[3];
    if (a == 0 && (p[0] | p[1] | p[2])) {
      a = 255;
    }
    if (opacity < 255) {
      a = static_cast<BYTE>((a * opacity) / 255);
    }
    p[0] = static_cast<BYTE>((p[0] * a) / 255);
    p[1] = static_cast<BYTE>((p[1] * a) / 255);
    p[2] = static_cast<BYTE>((p[2] * a) / 255);
    p[3] = a;
  }
  Gdiplus::Bitmap bmp(src, src, src * 4, PixelFormat32bppPARGB,
                      static_cast<BYTE*>(bits));
  g.DrawImage(&bmp, x, y, dest, dest);
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}
}  // namespace

void FenceWindow::ApplyStyle() {
  const auto* preset = FenceStyleById(style_id_);
  style_id_ = preset->id;
  bg_ = preset->background;
  accent_ = preset->accent;
  title_color_ = preset->title;
  caption_color_ = preset->title;
  panel_alpha_ = preset->alpha;
  if (opacity_override_ >= 0) {
    panel_alpha_ =
        static_cast<BYTE>((std::min)(255, (std::max)(40, opacity_override_)));
  }
  radius_ = preset->radius;
  light_panel_ = preset->light;
  if (hwnd_) {
    // Per-pixel alpha via UpdateLayeredWindow only — do not call
    // SetLayeredWindowAttributes (it disables subsequent ULW updates).
    PresentLayered();
  }
}

void FenceWindow::PresentLayered() {
  if (!hwnd_) {
    return;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  const int w = (std::max)(1, static_cast<int>(wr.right - wr.left));
  const int h = (std::max)(1, static_cast<int>(wr.bottom - wr.top));

  static ULONG_PTR gdi_token = 0;
  if (!gdi_token) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&gdi_token, &input, nullptr);
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ old = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

  const int radius = (std::max)(0, S(hwnd_, radius_));
  {
    // Scope the background Graphics. Leaving it alive sets a half-pixel
    // world transform on this DC, which blurs later GDI text and icons.
    Gdiplus::Graphics g(mem);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    const BYTE a = panel_alpha_;
    Gdiplus::SolidBrush fill(Gdiplus::Color(a, GetRValue(bg_), GetGValue(bg_),
                                            GetBValue(bg_)));
    Gdiplus::GraphicsPath path;
    const int rr = radius * 2;
    const int rw = (std::max)(1, w - 1);
    const int rh = (std::max)(1, h - 1);
    if (rr > 2 && rr < rw && rr < rh) {
      path.AddArc(0, 0, rr, rr, 180, 90);
      path.AddArc(rw - rr, 0, rr, rr, 270, 90);
      path.AddArc(rw - rr, rh - rr, rr, rr, 0, 90);
      path.AddArc(0, rh - rr, rr, rr, 90, 90);
      path.CloseFigure();
      g.FillPath(&fill, &path);
    } else {
      g.FillRectangle(&fill, 0, 0, rw, rh);
    }
    COLORREF border = light_panel_ ? RGB(15, 23, 42) : RGB(255, 255, 255);
    const BYTE ba = light_panel_ ? 48 : 40;
    Gdiplus::Pen edge(Gdiplus::Color(ba, GetRValue(border), GetGValue(border),
                                     GetBValue(border)),
                      1.0f);
    if (rr > 2 && rr < rw && rr < rh) {
      g.DrawPath(&edge, &path);
    } else {
      g.DrawRectangle(&edge, 0, 0, rw - 1, rh - 1);
    }
  }
  SetGraphicsMode(mem, GM_COMPATIBLE);

  Paint(mem);

  POINT origin{wr.left, wr.top};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateLayeredWindow(hwnd_, screen, &origin, &size, mem, &src, 0, &blend,
                      ULW_ALPHA);

  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

void FenceWindow::SetStyleId(const std::string& style_id) {
  style_id_ = NormalizeFenceStyleId(style_id);
  ApplyStyle();
}

void FenceWindow::SetOpacity(int opacity) {
  if (opacity < 0) {
    opacity_override_ = -1;
  } else {
    opacity_override_ = (std::min)(255, (std::max)(40, opacity));
  }
  ApplyStyle();
}

int FenceWindow::IconSizePx() const {
  if (view_mode_ == "list") {
    return S(hwnd_, kListIcon0);
  }
  return S(hwnd_, MulDiv(grid_metrics_.icon_size, icon_zoom_percent_, 100));
}

int FenceWindow::CellWidthPx() const {
  return S(hwnd_, MulDiv(grid_metrics_.cell_w, icon_zoom_percent_, 100));
}

int FenceWindow::CellHeightPx() const {
  return S(hwnd_, MulDiv(grid_metrics_.cell_h, icon_zoom_percent_, 100));
}

void FenceWindow::SetIconZoom(int percent) {
  // Match Python _icon_zoom_bounds(): 0.7 .. 1.6.
  const int clamped = (std::min)(160, (std::max)(70, percent));
  if (clamped == icon_zoom_percent_) {
    return;
  }
  icon_zoom_percent_ = clamped;
  footer_zoom_hint_until_ = GetTickCount() + 900;
  RebuildIconsForZoom();
  if (on_zoom_changed_) {
    on_zoom_changed_(this, icon_zoom_percent_);
  }
}

void FenceWindow::RebuildIconsForZoom() {
  const int icon_sz = IconSizePx();
  for (auto& cell : cells_) {
    if (cell.icon) {
      DestroyIcon(cell.icon);
      cell.icon = nullptr;
    }
    cell.icon = LoadItemIcon(cell.item.path, icon_sz);
  }
  LayoutCells();
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::ApplyZoomWheel(int delta) {
  if (view_mode_ == "list" || delta == 0) {
    return;
  }
  // Python: 0.15 step per notch → 15 percentage points.
  const int notches = delta / WHEEL_DELTA;
  if (notches == 0) {
    return;
  }
  SetIconZoom(icon_zoom_percent_ + 15 * notches);
}

void FenceWindow::SetPinOrder(std::vector<std::wstring> order) {
  pin_order_ = std::move(order);
}

void FenceWindow::InsertPinOrder(const std::vector<std::wstring>& paths,
                                 int at) {
  std::vector<std::wstring> filtered;
  filtered.reserve(pin_order_.size());
  for (const auto& p : pin_order_) {
    bool dropped = false;
    for (const auto& d : paths) {
      if (p == d) {
        dropped = true;
        break;
      }
    }
    if (!dropped) {
      filtered.push_back(p);
    }
  }
  const size_t n = filtered.size();
  size_t idx = at < 0 ? n : static_cast<size_t>(at);
  if (idx > n) {
    idx = n;
  }
  filtered.insert(filtered.begin() + static_cast<ptrdiff_t>(idx), paths.begin(),
                  paths.end());
  pin_order_ = std::move(filtered);
}

void FenceWindow::SetPageGeometry(int page, const FencePageGeometry& geo) {
  page_geometries_[page] = geo;
}

FencePageGeometry FenceWindow::GeometryForPage(int page) const {
  return FenceGeometryForPage(CurrentLayout(), page);
}

void FenceWindow::ApplyPageGeometry(const FencePageGeometry& geo) {
  const int w = (std::max)(geo.w, 120);
  const int h = (std::max)(geo.h, 80);
  collapsed_ = geo.collapsed;
  expanded_h_ = (std::max)(h, 180);
  if (hwnd_) {
    const int win_h = collapsed_ ? S(hwnd_, kCollapsedH0) : expanded_h_;
    SetWindowPos(hwnd_, nullptr, geo.x, geo.y, w, win_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutCells();
    PresentLayered();
  }
}

bool FenceWindow::VisibleOnPage(int page) const {
  const auto it = page_geometries_.find(page);
  return it != page_geometries_.end() ? it->second.visible : visible_;
}

void FenceWindow::SetOrganizeKinds(std::vector<std::string> kinds) {
  organize_kinds_ = std::move(kinds);
}

void FenceWindow::SetIconGrid(const IconGridMetrics& m) {
  grid_metrics_ = m;
  RebuildIconsForZoom();
}

void FenceWindow::ScaleGeometryForDpi(int old_dpi, int new_dpi) {
  RECT rc{};
  int x = 0;
  int y = 0;
  int w = 0;
  int h = expanded_h_;
  if (hwnd_) {
    GetWindowRect(hwnd_, &rc);
    x = rc.left;
    y = rc.top;
    w = rc.right - rc.left;
    if (!collapsed_) {
      h = rc.bottom - rc.top;
    }
  }
  ScaleRectForDpi(&x, &y, &w, &h, old_dpi, new_dpi);
  expanded_h_ = (std::max)(h, 180);
  if (hwnd_) {
    const int win_h = collapsed_ ? S(hwnd_, kCollapsedH0) : h;
    SetWindowPos(hwnd_, nullptr, x, y, w, win_h, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  for (auto& kv : page_geometries_) {
    ScaleRectForDpi(&kv.second.x, &kv.second.y, &kv.second.w, &kv.second.h,
                    old_dpi, new_dpi);
  }
}

bool FenceWindow::Create(HINSTANCE instance, const FenceLayout& layout) {
  id_ = layout.id;
  title_ = layout.title;
  page_ = layout.page;
  style_id_ = NormalizeFenceStyleId(layout.style_id);
  portal_path_ = layout.portal_path;
  collapsed_ = layout.collapsed;
  position_locked_ = layout.position_locked;
  sort_by_ = layout.sort_by.empty() ? "name" : layout.sort_by;
  view_mode_ = (layout.view_mode == "list") ? "list" : "grid";
  opacity_override_ =
      (layout.opacity >= 0 && layout.opacity <= 255) ? layout.opacity : -1;
  visible_ = layout.visible;
  organize_kinds_ = layout.organize_kinds;
  page_geometries_ = layout.page_geometries;
  pin_order_ = layout.pin_order;
  icon_zoom_percent_ = (std::min)(160, (std::max)(70, layout.icon_zoom));
  grid_metrics_ = QueryIconGridMetrics();
  expanded_h_ = (std::max)(layout.h, 180);
  const int win_h = collapsed_ ? kCollapsedH0 : layout.h;

  if (!g_class_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // CS_DBLCLKS required for WM_LBUTTONDBLCLK (open on double-click).
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = FenceWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc)) {
      if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
      }
    }
    g_class_registered = true;
  }

  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_ACCEPTFILES,
      kClassName, title_.c_str(), WS_POPUP, layout.x, layout.y, layout.w,
      win_h, nullptr, nullptr, instance, this);

  if (!hwnd_) {
    return false;
  }

  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ApplyStyle();
  EnsureDropTarget();
  if (collapsed_) {
    PresentLayered();
  }
  return true;
}

void FenceWindow::SetCollapsed(bool collapsed) {
  if (!hwnd_) {
    collapsed_ = collapsed;
    return;
  }
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int cur_h = rc.bottom - rc.top;
  if (collapsed) {
    if (!collapsed_) {
      expanded_h_ = (std::max)(cur_h, 180);
    }
    collapsed_ = true;
    SetWindowPos(hwnd_, nullptr, rc.left, rc.top, w, S(hwnd_, kCollapsedH0),
                 SWP_NOZORDER | SWP_NOACTIVATE);
  } else {
    collapsed_ = false;
    const int h = (std::max)(expanded_h_, 180);
    SetWindowPos(hwnd_, nullptr, rc.left, rc.top, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }
  LayoutCells();
  PresentLayered();
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::ToggleCollapsed() { SetCollapsed(!collapsed_); }

void FenceWindow::SetPositionLocked(bool locked) {
  if (position_locked_ == locked) {
    return;
  }
  position_locked_ = locked;
  if (hwnd_) {
    PresentLayered();
  }
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::EnsureDropTarget() {
  if (!hwnd_ || drop_target_) {
    return;
  }
  drop_target_ =
      new PathDropTarget([this](const std::vector<std::wstring>& paths) {
        if (on_paths_dropped_at_) {
          // F1: derive insertion index from the drop point (cursor is still at
          // the drop location when PathDropTarget::Drop fires).
          POINT pt{};
          GetCursorPos(&pt);
          ScreenToClient(hwnd_, &pt);
          const int at = InsertIndexAt(pt.x, pt.y);
          on_paths_dropped_at_(this, paths, at);
          return;
        }
        if (on_paths_dropped_) {
          on_paths_dropped_(this, paths);
        }
      });
  if (!RegisterPathDropTarget(hwnd_, drop_target_)) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
}

void FenceWindow::Destroy() {
  ClearIcons();
  if (tip_) {
    DestroyWindow(tip_);
    tip_ = nullptr;
  }
  tip_index_ = -1;
  if (hwnd_) {
    RevokePathDropTarget(hwnd_);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (drop_target_) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
}

void FenceWindow::EnsureTooltip() {
  if (tip_ || !hwnd_) {
    return;
  }
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
  InitCommonControlsEx(&icc);
  tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                         WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                         hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!tip_) {
    return;
  }
  SetWindowPos(tip_, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  TOOLINFOW ti{};
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
  ti.hwnd = hwnd_;
  ti.uId = 1;
  ti.lpszText = const_cast<wchar_t*>(L"");
  SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
  SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, 320);
}

void FenceWindow::UpdateHoverTip(int index) {
  const int prev = tip_index_;
  const int next =
      (index >= 0 && index < static_cast<int>(cells_.size())) ? index : -1;
  EnsureTooltip();
  if (tip_) {
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwnd_;
    ti.uId = 1;
    if (next < 0) {
      tip_index_ = -1;
      SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE,
                   reinterpret_cast<LPARAM>(&ti));
    } else {
      const auto& item = cells_[static_cast<size_t>(next)].item;
      const std::wstring& text = item.name.empty() ? item.path : item.name;
      ti.lpszText = const_cast<wchar_t*>(text.c_str());
      SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
      POINT pt{};
      GetCursorPos(&pt);
      SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x + 12, pt.y + 16));
      SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE,
                   reinterpret_cast<LPARAM>(&ti));
      tip_index_ = next;
    }
  } else {
    tip_index_ = next;
  }
  // Hover chrome needs a repaint when the under-cursor icon changes.
  if (tip_index_ != prev && hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::ClearIcons() {
  for (auto& c : cells_) {
    if (c.icon) {
      DestroyIcon(c.icon);
      c.icon = nullptr;
    }
  }
  cells_.clear();
}

const std::vector<DesktopItem> FenceWindow::ItemSnapshot() const {
  std::vector<DesktopItem> out;
  out.reserve(cells_.size());
  for (const auto& c : cells_) {
    out.push_back(c.item);
  }
  return out;
}

void FenceWindow::SetTitle(const std::wstring& title) {
  title_ = title;
  if (hwnd_) {
    SetWindowTextW(hwnd_, title_.c_str());
    PresentLayered();
  }
}

FenceLayout FenceWindow::CurrentLayout() const {
  FenceLayout fl;
  fl.id = id_;
  fl.title = title_;
  fl.page = page_;
  fl.style_id = style_id_;
  fl.portal_path = portal_path_;
  fl.collapsed = collapsed_;
  fl.position_locked = position_locked_;
  fl.sort_by = sort_by_;
  fl.view_mode = view_mode_;
  fl.opacity = opacity_override_;
  fl.visible = visible_;
  fl.organize_kinds = organize_kinds_;
  fl.page_geometries = page_geometries_;
  fl.icon_zoom = icon_zoom_percent_;
  fl.pin_order = pin_order_;
  if (hwnd_) {
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    fl.x = rc.left;
    fl.y = rc.top;
    fl.w = rc.right - rc.left;
    fl.h = collapsed_ ? expanded_h_ : (rc.bottom - rc.top);
  } else {
    fl.h = expanded_h_;
  }
  return fl;
}

void FenceWindow::Show() {
  if (!hwnd_) {
    return;
  }
  const bool was_visible = IsWindowVisible(hwnd_) != FALSE;
  if (!was_visible) {
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }
  if (!IsAttachedToDesktop(hwnd_)) {
    AttachToDesktopLayer();
  }
  // Do not RaiseOverlay on already-visible fences: band-settle timers used to
  // Show() every tick → raise/sink fight → desktop flash and settings covered.
  if (!was_visible) {
    PresentLayered();
  }
}

void FenceWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void FenceWindow::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

void FenceWindow::RefreshLayer() { PresentLayered(); }

void FenceWindow::SetViewMode(const std::string& mode) {
  const std::string next = (mode == "list") ? "list" : "grid";
  if (view_mode_ == next) {
    return;
  }
  view_mode_ = next;
  // Reload icons at the size matching the view.
  const int sz = IconSizePx();
  for (auto& cell : cells_) {
    if (cell.icon) {
      DestroyIcon(cell.icon);
      cell.icon = nullptr;
    }
    cell.icon = LoadItemIcon(cell.item.path, sz);
  }
  LayoutCells();
  if (hwnd_) {
    PresentLayered();
  }
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::SetItems(std::vector<DesktopItem> items) {
  ClearIcons();
  cells_.reserve(items.size());
  const int icon_sz = IconSizePx();
  for (auto& it : items) {
    Cell cell;
    cell.item = std::move(it);
    cell.icon = LoadItemIcon(cell.item.path, icon_sz);
    cells_.push_back(std::move(cell));
  }
  SortCells();
  scroll_y_ = 0;
  selected_.assign(cells_.size(), 0);
  selection_anchor_ = -1;
  LayoutCells();
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::SetSortBy(const std::string& sort_by) {
  sort_by_ = sort_by.empty() ? "name" : sort_by;
  SortCells();
  selected_.assign(cells_.size(), 0);
  selection_anchor_ = -1;
  LayoutCells();
  if (hwnd_) {
    PresentLayered();
  }
  if (on_moved_) {
    on_moved_(this);  // persist layout (incl. sort_by)
  }
}

void FenceWindow::SortCells() {
  if (cells_.size() < 2 || sort_by_ == "manual") {
    return;
  }
  auto ext_of = [](const std::wstring& path) -> std::wstring {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos ||
        (slash != std::wstring::npos && dot < slash)) {
      return {};
    }
    std::wstring e = path.substr(dot);
    for (auto& c : e) {
      if (c >= L'A' && c <= L'Z') {
        c = static_cast<wchar_t>(c - L'A' + L'a');
      }
    }
    return e;
  };
  auto lower = [](std::wstring s) {
    for (auto& c : s) {
      if (c >= L'A' && c <= L'Z') {
        c = static_cast<wchar_t>(c - L'A' + L'a');
      }
    }
    return s;
  };
  if (sort_by_ == "date") {
    std::stable_sort(cells_.begin(), cells_.end(), [](const Cell& a,
                                                      const Cell& b) {
      WIN32_FILE_ATTRIBUTE_DATA fa{}, fb{};
      const BOOL oka =
          GetFileAttributesExW(a.item.path.c_str(), GetFileExInfoStandard, &fa);
      const BOOL okb =
          GetFileAttributesExW(b.item.path.c_str(), GetFileExInfoStandard, &fb);
      if (!oka || !okb) {
        return oka && !okb;
      }
      return CompareFileTime(&fa.ftLastWriteTime, &fb.ftLastWriteTime) > 0;
    });
  } else if (sort_by_ == "size") {
    std::stable_sort(cells_.begin(), cells_.end(), [](const Cell& a,
                                                      const Cell& b) {
      WIN32_FILE_ATTRIBUTE_DATA fa{}, fb{};
      GetFileAttributesExW(a.item.path.c_str(), GetFileExInfoStandard, &fa);
      GetFileAttributesExW(b.item.path.c_str(), GetFileExInfoStandard, &fb);
      ULARGE_INTEGER sa{}, sb{};
      sa.LowPart = fa.nFileSizeLow;
      sa.HighPart = fa.nFileSizeHigh;
      sb.LowPart = fb.nFileSizeLow;
      sb.HighPart = fb.nFileSizeHigh;
      return sa.QuadPart > sb.QuadPart;
    });
  } else if (sort_by_ == "type") {
    std::stable_sort(cells_.begin(), cells_.end(),
                     [&](const Cell& a, const Cell& b) {
                       const std::wstring ea = ext_of(a.item.path);
                       const std::wstring eb = ext_of(b.item.path);
                       if (ea != eb) {
                         return ea < eb;
                       }
                       return lower(a.item.name) < lower(b.item.name);
                     });
  } else {
    std::stable_sort(cells_.begin(), cells_.end(),
                     [&](const Cell& a, const Cell& b) {
                       return lower(a.item.name) < lower(b.item.name);
                     });
  }
}

void FenceWindow::LayoutCells() {
  if (!hwnd_) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int pad = S(hwnd_, kPad0);
  const int header = S(hwnd_, kHeaderH0);
  const int inner_l = rc.left + pad;
  const int inner_t = rc.top + header - scroll_y_;
  const int inner_r = rc.right - pad;
  if (view_mode_ == "list") {
    const int row_h = S(hwnd_, kListRowH0);
    const int width = (std::max)(40, inner_r - inner_l);
    for (size_t i = 0; i < cells_.size(); ++i) {
      RECT& b = cells_[i].bounds;
      b.left = inner_l;
      b.top = inner_t + static_cast<int>(i) * row_h;
      b.right = b.left + width;
      b.bottom = b.top + row_h;
    }
    content_h_ = static_cast<int>(cells_.size()) * row_h;
  } else {
    const int cell_w = CellWidthPx();
    const int cell_h = CellHeightPx();
    const int cols = (std::max)(1, (inner_r - inner_l) / cell_w);
    for (size_t i = 0; i < cells_.size(); ++i) {
      const int col = static_cast<int>(i % static_cast<size_t>(cols));
      const int row = static_cast<int>(i / static_cast<size_t>(cols));
      RECT& b = cells_[i].bounds;
      b.left = inner_l + col * cell_w;
      b.top = inner_t + row * cell_h;
      b.right = b.left + cell_w;
      b.bottom = b.top + cell_h;
    }
    const int rows =
        cells_.empty()
            ? 0
            : static_cast<int>((cells_.size() + static_cast<size_t>(cols) - 1) /
                               static_cast<size_t>(cols));
    content_h_ = rows * cell_h;
  }
  ClampScroll();
}

void FenceWindow::ClampScroll() {
  RECT rc{};
  if (!hwnd_) {
    return;
  }
  GetClientRect(hwnd_, &rc);
  const int view = (std::max)(
      0, static_cast<int>(rc.bottom) - S(hwnd_, kHeaderH0) - S(hwnd_, kPad0));
  const int max_scroll = (std::max)(0, content_h_ - view);
  if (scroll_y_ < 0) {
    scroll_y_ = 0;
  }
  if (scroll_y_ > max_scroll) {
    scroll_y_ = max_scroll;
  }
}

int FenceWindow::HitTestIcon(int x, int y) const {
  if (collapsed_) {
    return -1;
  }
  if (y < S(hwnd_, kHeaderH0)) {
    return -1;
  }
  const bool list = view_mode_ == "list";
  const int icon = IconSizePx();
  const int cell_w = CellWidthPx();
  const int font_px = S(hwnd_, list ? 13 : 12);
  const int line_h = font_px + S(hwnd_, 5);
  const int idle_label_h = line_h * 2 + S(hwnd_, 4);
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& b = cells_[i].bounds;
    if (list) {
      if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
        return static_cast<int>(i);
      }
      continue;
    }
    // Grid: hit glyph + caption shelf only — gaps between cells are blank
    // (Explorer / Fences: click empty chrome clears selection).
    const int ix = b.left + (cell_w - icon) / 2;
    const int iy = b.top + 4;
    const bool on_icon =
        x >= ix && x < ix + icon && y >= iy && y < iy + icon;
    const int label_top = iy + icon + 2;
    const bool on_label = x >= b.left + 3 && x < b.right - 3 &&
                          y >= label_top && y < label_top + idle_label_h;
    if (on_icon || on_label) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int FenceWindow::GridColumnCount() const {
  if (!hwnd_ || view_mode_ == "list") {
    return 1;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int pad = S(hwnd_, kPad0);
  const int cell_w = CellWidthPx();
  const int inner = (std::max)(1, static_cast<int>(rc.right - rc.left) - pad * 2);
  return (std::max)(1, inner / cell_w);
}

bool FenceWindow::ScrollbarVisible() const {
  if (!hwnd_ || collapsed_) {
    return false;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int view = (std::max)(
      0, static_cast<int>(rc.bottom) - S(hwnd_, kHeaderH0) - S(hwnd_, kPad0));
  return content_h_ > view && view > 0;
}

int FenceWindow::HitTestScrollbar(int x, int y) const {
  if (!ScrollbarVisible()) {
    return 0;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int header = S(hwnd_, kHeaderH0);
  if (x < rc.right - 8 || x >= rc.right) {
    return 0;
  }
  const int track_top = header + 2;
  const int track_h = (std::max)(8, static_cast<int>(rc.bottom) - track_top - 4);
  if (y < track_top || y > track_top + track_h) {
    return 0;
  }
  const int view = (std::max)(
      0, static_cast<int>(rc.bottom) - header - S(hwnd_, kPad0));
  const int thumb_h =
      (std::max)(16, track_h * view / (std::max)(1, content_h_));
  const int max_scroll = (std::max)(1, content_h_ - view);
  const int thumb_y = track_top + (track_h - thumb_h) * scroll_y_ / max_scroll;
  if (y >= thumb_y && y <= thumb_y + thumb_h) {
    return 1;  // thumb
  }
  return 2;  // track
}

void FenceWindow::ScrollbarPage(int dir) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int view = (std::max)(
      0, static_cast<int>(rc.bottom) - S(hwnd_, kHeaderH0) - S(hwnd_, kPad0));
  scroll_y_ += dir * view;
  ClampScroll();
  LayoutCells();
  PresentLayered();
}

void FenceWindow::PreviewStyleId(const std::string& style_id) {
  if (!style_preview_active_) {
    preview_style_id_ = style_id_;
    style_preview_active_ = true;
  }
  SetStyleId(style_id);
}

void FenceWindow::RevertStylePreview() {
  if (!style_preview_active_) {
    return;
  }
  style_preview_active_ = false;
  SetStyleId(preview_style_id_);
  preview_style_id_.clear();
}

void FenceWindow::ApplyRubberBandSelection() {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  if (!rubber_additive_) {
    std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  }
  const int l = (std::min)(rubber_a_.x, rubber_b_.x);
  const int t = (std::min)(rubber_a_.y, rubber_b_.y);
  const int r = (std::max)(rubber_a_.x, rubber_b_.x);
  const int b = (std::max)(rubber_a_.y, rubber_b_.y);
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& cell = cells_[i].bounds;
    const bool hit =
        !(cell.right < l || cell.left > r || cell.bottom < t || cell.top > b);
    if (hit) {
      selected_[i] = 1;
      selection_anchor_ = static_cast<int>(i);
    }
  }
}

void FenceWindow::OpenSelectedItems() {
  const auto paths = SelectedPaths();
  if (paths.empty()) {
    return;
  }
  for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
    if (selected_[i]) {
      OpenItem(static_cast<int>(i));
    }
  }
}

void FenceWindow::NavigateSelection(int dx, int dy, bool extend) {
  if (cells_.empty()) {
    return;
  }
  const int cols = GridColumnCount();
  int cur = selection_anchor_;
  if (cur < 0 || cur >= static_cast<int>(cells_.size())) {
    cur = -1;
    for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
      if (selected_[i]) {
        cur = static_cast<int>(i);
        break;
      }
    }
  }
  if (cur < 0) {
    SelectOnly(0);
    return;
  }
  int next = cur + dy * cols + dx;
  if (next < 0) {
    next = 0;
  }
  if (next >= static_cast<int>(cells_.size())) {
    next = static_cast<int>(cells_.size()) - 1;
  }
  if (extend) {
    SelectRange(next);
  } else {
    SelectOnly(next);
  }
  EnsureCellVisible(next);
}

void FenceWindow::EnsureCellVisible(int index) {
  if (!hwnd_ || index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int header = S(hwnd_, kHeaderH0);
  const RECT& b = cells_[static_cast<size_t>(index)].bounds;
  bool moved = false;
  if (b.top < header) {
    scroll_y_ -= (header - b.top);
    moved = true;
  } else if (b.bottom > rc.bottom) {
    scroll_y_ += (b.bottom - rc.bottom);
    moved = true;
  }
  if (moved) {
    ClampScroll();
    LayoutCells();
    PresentLayered();
  }
}

void FenceWindow::HandleFenceHotkey(int vk, bool ctrl, bool shift) {
  if (rename_edit_) {
    return;
  }
  if (vk == VK_ESCAPE) {
    if (rubber_) {
      rubber_ = false;
      ReleaseCapture();
    }
    ClearSelection();
    return;
  }
  if (vk == VK_RETURN) {
    OpenSelectedItems();
    return;
  }
  if (vk == VK_DELETE) {
    DeleteSelection();
    return;
  }
  if (vk == VK_F2) {
    BeginInlineRename();
    return;
  }
  if (vk == VK_SPACE) {
    // Explorer-like: Space selects focused/hovered; Ctrl+Space toggles.
    int idx = tip_index_;
    if (idx < 0 || idx >= static_cast<int>(cells_.size())) {
      idx = selection_anchor_;
    }
    if (idx < 0 || idx >= static_cast<int>(cells_.size())) {
      for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
        if (selected_[i]) {
          idx = static_cast<int>(i);
          break;
        }
      }
    }
    if (idx < 0 || idx >= static_cast<int>(cells_.size())) {
      return;
    }
    if (ctrl) {
      ToggleSelected(idx);
    } else {
      SelectOnly(idx);
    }
    return;
  }
  if (ctrl && vk == 'C') {
    CopySelection();
    return;
  }
  if (ctrl && vk == 'X') {
    CutSelection();
    return;
  }
  if (ctrl && vk == 'V') {
    PasteClipboardFiles();
    return;
  }
  if (ctrl && vk == 'A') {
    SelectAll();
    return;
  }
  if (vk == VK_HOME) {
    if (cells_.empty()) {
      return;
    }
    if (shift) {
      if (selection_anchor_ < 0) {
        selection_anchor_ = 0;
      }
      SelectRange(0);
    } else {
      SelectOnly(0);
    }
    EnsureCellVisible(0);
    return;
  }
  if (vk == VK_END) {
    if (cells_.empty()) {
      return;
    }
    const int last = static_cast<int>(cells_.size()) - 1;
    if (shift) {
      if (selection_anchor_ < 0) {
        selection_anchor_ = last;
      }
      SelectRange(last);
    } else {
      SelectOnly(last);
    }
    EnsureCellVisible(last);
    return;
  }
  if (vk == VK_LEFT) {
    NavigateSelection(-1, 0, shift);
  } else if (vk == VK_RIGHT) {
    NavigateSelection(1, 0, shift);
  } else if (vk == VK_UP) {
    NavigateSelection(0, -1, shift);
  } else if (vk == VK_DOWN) {
    NavigateSelection(0, 1, shift);
  }
}

FenceWindow::Edge FenceWindow::HitTestEdge(int x, int y) const {
  if (position_locked_) {
    return Edge::None;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  int e = 0;
  if (x <= kEdge) {
    e |= static_cast<int>(Edge::Left);
  }
  if (x >= rc.right - kEdge) {
    e |= static_cast<int>(Edge::Right);
  }
  // Collapsed: only horizontal resize (keep chrome height fixed).
  if (!collapsed_) {
    if (y <= kEdge) {
      e |= static_cast<int>(Edge::Top);
    }
    if (y >= rc.bottom - kEdge) {
      e |= static_cast<int>(Edge::Bottom);
    }
  }
  return static_cast<Edge>(e);
}

void FenceWindow::OpenItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  OpenShellPath(cells_[static_cast<size_t>(index)].item.path);
}

void FenceWindow::RevealItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring arg =
      L"/select,\"" + cells_[static_cast<size_t>(index)].item.path + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr,
                SW_SHOWNORMAL);
}

void FenceWindow::BeginIconDrag(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  // Portal fences mirror a real folder: keep the OLE file drag (physical move/
  // copy out). Plain fences pin virtual items → manual in-app drag (reorder /
  // drag-out unpin), matching Python's virtual semantics.
  if (!portal_path_.empty()) {
    std::vector<std::wstring> paths;
    if (IsSelected(index)) {
      paths = SelectedPaths();
    }
    if (paths.empty()) {
      paths.push_back(cells_[static_cast<size_t>(index)].item.path);
    }
    icon_press_ = false;
    icon_press_index_ = -1;
    ReleaseCapture();
    ShellDoDragFiles(paths);
    if (on_drag_done_) {
      on_drag_done_(this);
    }
    return;
  }
  StartManualIconDrag(index);
}

void FenceWindow::StartManualIconDrag(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  icon_press_ = false;
  icon_press_index_ = -1;
  drag_paths_.clear();
  drag_indices_.clear();
  if (IsSelected(index)) {
    for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
      if (selected_[i]) {
        drag_indices_.push_back(static_cast<int>(i));
        drag_paths_.push_back(cells_[i].item.path);
      }
    }
  }
  if (drag_indices_.empty()) {
    drag_indices_.push_back(index);
    drag_paths_.push_back(cells_[static_cast<size_t>(index)].item.path);
  }
  icon_dragging_ = true;
  drag_insert_ = -1;
  drag_left_ = false;
  SetCapture(hwnd_);
  UpdateHoverTip(-1);
  LogLine(L"[fence] manual drag start: %d items from %s",
          static_cast<int>(drag_paths_.size()), title_.c_str());
}

void FenceWindow::UpdateManualIconDrag(int x, int y) {
  if (!icon_dragging_) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const bool outside = x < 0 || y < 0 || x >= rc.right || y >= rc.bottom;
  drag_left_ = outside;
  drag_insert_ = outside ? -1 : InsertIndexAt(x, y);
  PresentLayered();
}

void FenceWindow::FinishManualIconDrag(int x, int y) {
  if (!icon_dragging_) {
    return;
  }
  icon_dragging_ = false;
  ReleaseCapture();
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const bool outside =
      x < 0 || y < 0 || x >= rc.right || y >= rc.bottom;
  if (outside) {
    CommitDragOutUnpin();
  } else if (y >= S(hwnd_, kHeaderH0)) {
    const int at = InsertIndexAt(x, y);
    CommitReorder(at);
  }
  drag_paths_.clear();
  drag_indices_.clear();
  drag_insert_ = -1;
  drag_left_ = false;
  PresentLayered();
}

int FenceWindow::InsertIndexAt(int x, int y) const {
  if (cells_.empty()) {
    return 0;
  }
  // Map to the nearest cell midpoint: before/after by grid axis (Python
  // _insert_index_at uses the cell midpoint along the dominant axis).
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& b = cells_[i].bounds;
    if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
      if (view_mode_ == "list") {
        return static_cast<int>(i) +
               (y >= (b.top + b.bottom) / 2 ? 1 : 0);
      }
      return static_cast<int>(i) +
             (x >= (b.left + b.right) / 2 ? 1 : 0);
    }
  }
  // Empty gutter: nearest cell edge (prefer same row in grid).
  int best = static_cast<int>(cells_.size());
  long long best_d = LLONG_MAX;
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& b = cells_[i].bounds;
    const int anchors_x[2] = {b.left, b.right};
    const int anchor_iy[2] = {static_cast<int>(i), static_cast<int>(i) + 1};
    for (int k = 0; k < 2; ++k) {
      const long long dx = static_cast<long long>(x - anchors_x[k]);
      const long long dy = static_cast<long long>(y - ((b.top + b.bottom) / 2));
      const long long d = (view_mode_ == "list") ? dy * dy + dx * dx * 9
                                                  : dx * dx + dy * dy * 9;
      if (d < best_d) {
        best_d = d;
        best = anchor_iy[k];
      }
    }
  }
  return best;
}

void FenceWindow::CommitReorder(int insert_index) {
  if (drag_indices_.empty() || cells_.empty()) {
    return;
  }
  // Extract dragged cells (in index order), remove them, then re-insert as a
  // contiguous block at the adjusted position.
  std::vector<Cell> dragged;
  dragged.reserve(drag_indices_.size());
  for (int idx : drag_indices_) {
    if (idx >= 0 && idx < static_cast<int>(cells_.size())) {
      dragged.push_back(std::move(cells_[static_cast<size_t>(idx)]));
    }
  }
  if (dragged.empty()) {
    return;
  }
  // Remove in descending order to keep indices valid.
  std::vector<int> sorted = drag_indices_;
  std::sort(sorted.begin(), sorted.end(), std::greater<int>());
  for (int idx : sorted) {
    if (idx >= 0 && idx < static_cast<int>(cells_.size())) {
      cells_.erase(cells_.begin() + idx);
    }
  }
  // Adjust insert position for the removed block (all before insert shift it).
  int at = insert_index;
  for (int idx : sorted) {
    if (idx < at) {
      --at;
    }
  }
  at = (std::min)(at, static_cast<int>(cells_.size()));
  at = (std::max)(0, at);
  cells_.insert(cells_.begin() + at, std::make_move_iterator(dragged.begin()),
                std::make_move_iterator(dragged.end()));
  selected_.assign(cells_.size(), 0);
  selection_anchor_ = -1;
  sort_by_ = "manual";  // manual order now reflects the new arrangement
  LayoutCells();
  if (on_moved_) {
    on_moved_(this);  // persist sort_by=manual (pin order needs app-side storage)
  }
  if (on_reorder_) {
    std::vector<std::wstring> order;
    order.reserve(cells_.size());
    for (const auto& c : cells_) {
      order.push_back(c.item.path);
    }
    on_reorder_(this, order);
  }
  LogLine(L"[fence] reorder committed: %d items -> insert %d",
          static_cast<int>(dragged.size()), at);
}

void FenceWindow::CommitDragOutUnpin() {
  // Drag-out of a plain (virtual) fence unpins; files stay on the desktop.
  LogLine(L"[fence] drag-out unpin: %d items from %s",
          static_cast<int>(drag_paths_.size()), title_.c_str());
  for (const auto& p : drag_paths_) {
    if (on_unpin_) {
      on_unpin_(this, p);
    }
  }
}

void FenceWindow::RenameItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring old_path = cells_[static_cast<size_t>(index)].item.path;
  std::wstring dest;
  if (RenameShellPath(hwnd_, old_path, &dest) == IconFsResult::Ok &&
      on_path_replaced_) {
    on_path_replaced_(old_path, dest);
  }
}

void FenceWindow::RecycleItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring path = cells_[static_cast<size_t>(index)].item.path;
  if (RecycleShellPath(hwnd_, path) == IconFsResult::Ok && on_unpin_) {
    on_unpin_(this, path);
  }
}

void FenceWindow::ShowItemProperties(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  ShowShellPathProperties(hwnd_, cells_[static_cast<size_t>(index)].item.path);
}

void FenceWindow::ClearSelection() {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  } else {
    std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  }
  selection_anchor_ = -1;
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::SelectOnly(int index) {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  if (index >= 0 && index < static_cast<int>(selected_.size())) {
    selected_[static_cast<size_t>(index)] = 1;
    selection_anchor_ = index;
  }
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::ToggleSelected(int index) {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  if (index < 0 || index >= static_cast<int>(selected_.size())) {
    return;
  }
  selected_[static_cast<size_t>(index)] =
      selected_[static_cast<size_t>(index)] ? 0 : 1;
  selection_anchor_ = index;
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::SelectRange(int index) {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  if (index < 0 || index >= static_cast<int>(selected_.size())) {
    return;
  }
  int a = selection_anchor_;
  if (a < 0 || a >= static_cast<int>(selected_.size())) {
    a = index;
  }
  const int lo = (std::min)(a, index);
  const int hi = (std::max)(a, index);
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  for (int i = lo; i <= hi; ++i) {
    selected_[static_cast<size_t>(i)] = 1;
  }
  if (hwnd_) {
    PresentLayered();
  }
}

bool FenceWindow::IsSelected(int index) const {
  return index >= 0 && index < static_cast<int>(selected_.size()) &&
         selected_[static_cast<size_t>(index)] != 0;
}

std::vector<std::wstring> FenceWindow::SelectedPaths() const {
  std::vector<std::wstring> paths;
  const size_t n = (std::min)(selected_.size(), cells_.size());
  for (size_t i = 0; i < n; ++i) {
    if (selected_[i]) {
      paths.push_back(cells_[i].item.path);
    }
  }
  return paths;
}

void FenceWindow::ApplySelectionCommand(int cmd, int clicked) {
  if (clicked < 0 || clicked >= static_cast<int>(cells_.size())) {
    return;
  }
  std::vector<std::wstring> paths;
  if (IsSelected(clicked)) {
    paths = SelectedPaths();
  }
  if (paths.empty()) {
    paths.push_back(cells_[static_cast<size_t>(clicked)].item.path);
  }
  if (cmd == 3) {
    for (const auto& p : paths) {
      if (on_unpin_) {
        on_unpin_(this, p);
      }
    }
    return;
  }
  if (cmd == 4 || cmd == 5) {
    if (ClipboardSetFiles(hwnd_, paths, cmd == 5)) {
      ShowToast(cmd == 5 ? L"已剪切到剪贴板" : L"已复制到剪贴板");
      PresentLayered();
    }
    return;
  }
  if (cmd == 6) {
    std::wstring text;
    for (size_t i = 0; i < paths.size(); ++i) {
      if (i) {
        text += L"\r\n";
      }
      text += paths[i];
    }
    CopyShellPathText(hwnd_, text);
    return;
  }
  if (cmd == 9 && RecycleShellPaths(hwnd_, paths) == IconFsResult::Ok) {
    for (const auto& p : paths) {
      if (on_unpin_) {
        on_unpin_(this, p);
      }
    }
  }
}

void FenceWindow::ShowIconMenu(int index, POINT screen) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  if (!IsSelected(index)) {
    SelectOnly(index);
  }
  auto paths = SelectedPaths();
  if (paths.empty()) {
    paths.push_back(cells_[static_cast<size_t>(index)].item.path);
  }
  bool whitelisted = false;
  if (!paths.empty()) {
    whitelisted = IsWhitelisted(paths[0], LoadWhitelist());
  }
  const std::vector<ShellMenuExtra> extras{
      {Tr("menu_unpin"), kExtUnpin},
      {whitelisted ? Tr("wl_remove_item") : Tr("wl_add_item"), kExtWhitelist},
      {Tr("menu_move_fence"), kExtMoveFence},
      {Tr("menu_move_page"), kExtMovePage},
      {Tr("notes_open_in_notepad"), kExtOpenNotes},
  };
  const int shell_cmd =
      TrackShellContextMenu(hwnd_, screen, paths, extras);
  if (shell_cmd == kExtUnpin) {
    ApplySelectionCommand(3, index);
    return;
  }
  if (shell_cmd == kExtWhitelist) {
    if (on_whitelist_) {
      on_whitelist_(this, paths, !whitelisted);
    }
    return;
  }
  if (shell_cmd == kExtMoveFence) {
    ShowMoveToFenceMenu(screen);
    return;
  }
  if (shell_cmd == kExtMovePage) {
    ShowMoveToPageMenu(screen);
    return;
  }
  if (shell_cmd == kExtOpenNotes) {
    if (on_open_notes_) {
      on_open_notes_(this, paths);
    }
    return;
  }
  if (shell_cmd == 0) {
    // Shell verb ran (or cancel). Drop pins for paths that vanished.
    for (const auto& p : paths) {
      if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES &&
          on_unpin_) {
        on_unpin_(this, p);
      }
    }
    if (on_drag_done_) {
      on_drag_done_(this);
    }
    return;
  }
  // Fallback hand-rolled menu when IContextMenu unavailable.
  const int cmd = TrackIconShellMenu(hwnd_, screen, true);
  if (cmd == 1) {
    OpenItem(index);
  } else if (cmd == 2) {
    RevealItem(index);
  } else if (cmd == 3 || cmd == 4 || cmd == 5 || cmd == 6 || cmd == 9) {
    ApplySelectionCommand(cmd, index);
  } else if (cmd == 7) {
    RenameItem(index);
  } else if (cmd == 8) {
    ShowItemProperties(index);
  }
}

void FenceWindow::ShowMoveToFenceMenu(POINT screen) {
  const auto paths = SelectedPaths();
  if (paths.empty()) {
    return;
  }
  std::vector<FenceMoveTarget> targets;
  if (on_query_fences_) {
    for (auto& t : on_query_fences_()) {
      if (t.id != id_) {
        targets.push_back(std::move(t));
      }
    }
  }
  HMENU menu = CreatePopupMenu();
  for (size_t i = 0; i < targets.size(); ++i) {
    const std::wstring label =
        targets[i].title.empty() ? std::wstring(L"(未命名)") : targets[i].title;
    AppendMenuW(menu, MF_STRING, static_cast<UINT>(1 + i), label.c_str());
  }
  if (targets.empty()) {
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 1, L"没有其它分区");
  }
  SetForegroundWindow(hwnd_);
  const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x,
                                 screen.y, 0, hwnd_, nullptr);
  DestroyMenu(menu);
  if (cmd >= 1 && cmd <= static_cast<int>(targets.size()) && on_move_to_fence_) {
    on_move_to_fence_(this, paths, targets[static_cast<size_t>(cmd - 1)].id);
  }
}

void FenceWindow::ShowMoveToPageMenu(POINT screen) {
  const auto paths = SelectedPaths();
  if (paths.empty()) {
    return;
  }
  if (page_names_.empty()) {
    page_names_ = {L"工作", L"文档"};
  }
  HMENU menu = CreatePopupMenu();
  for (size_t i = 0; i < page_names_.size() && i < 12; ++i) {
    AppendMenuW(menu, MF_STRING, static_cast<UINT>(1 + i),
                page_names_[i].c_str());
  }
  SetForegroundWindow(hwnd_);
  const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x,
                                 screen.y, 0, hwnd_, nullptr);
  DestroyMenu(menu);
  if (cmd >= 1 && cmd <= static_cast<int>(page_names_.size()) && on_move_to_page_) {
    on_move_to_page_(this, paths, cmd - 1);
  }
}

void FenceWindow::SetPortalPath(const std::wstring& path) {
  portal_path_ = path;
}

void FenceWindow::CopySelection() {
  const auto paths = SelectedPaths();
  if (!paths.empty()) {
    ClipboardSetFiles(hwnd_, paths, false);
    ShowToast(L"已复制到剪贴板");
    // Clear cut-ghost dimming after Preferred DropEffect flips to copy.
    PresentLayered();
  }
}

void FenceWindow::CutSelection() {
  const auto paths = SelectedPaths();
  if (!paths.empty() && ClipboardSetFiles(hwnd_, paths, true)) {
    ShowToast(L"已剪切到剪贴板");
    PresentLayered();
  }
}

void FenceWindow::DeleteSelection() {
  const auto paths = SelectedPaths();
  if (paths.empty()) {
    return;
  }
  if (RecycleShellPaths(hwnd_, paths) != IconFsResult::Ok) {
    return;
  }
  for (const auto& p : paths) {
    if (on_unpin_) {
      on_unpin_(this, p);
    }
  }
}

void FenceWindow::SelectAll() {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(1));
  if (!cells_.empty()) {
    selection_anchor_ = 0;
  }
  PresentLayered();
}

namespace {
constexpr int kRenameEdit = 77;
LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                LPARAM lparam) {
  auto* self = reinterpret_cast<FenceWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE)) {
    if (self) {
      self->FinishInlineRename(wparam == VK_RETURN);
    }
    return 0;
  }
  WNDPROC old = reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"oldproc"));
  if (old) {
    return CallWindowProcW(old, hwnd, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}  // namespace

void FenceWindow::BeginInlineRename() {
  int index = -1;
  int count = 0;
  for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
    if (selected_[i]) {
      index = static_cast<int>(i);
      ++count;
    }
  }
  if (count != 1 || index < 0) {
    return;
  }
  if (rename_edit_) {
    DestroyWindow(rename_edit_);
    rename_edit_ = nullptr;
  }
  rename_index_ = index;
  const RECT& b = cells_[static_cast<size_t>(index)].bounds;
  const int icon = IconSizePx();
  const int cell_w = CellWidthPx();
  rename_edit_ = CreateWindowExW(
      0, L"EDIT", cells_[static_cast<size_t>(index)].item.name.c_str(),
      WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, b.left + 2,
      b.top + 4 + icon, cell_w - 4, S(hwnd_, 20), hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRenameEdit)), nullptr,
      nullptr);
  if (!rename_edit_) {
    return;
  }
  rename_proc_ = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(rename_edit_, GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(RenameEditProc)));
  SetPropW(rename_edit_, L"oldproc", reinterpret_cast<HANDLE>(rename_proc_));
  SetWindowLongPtrW(rename_edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  SendMessageW(rename_edit_, EM_SETSEL, 0, -1);
  SetFocus(rename_edit_);
}

void FenceWindow::FinishInlineRename(bool commit) {
  if (!rename_edit_) {
    return;
  }
  wchar_t buf[260] = {};
  GetWindowTextW(rename_edit_, buf, 260);
  const int index = rename_index_;
  DestroyWindow(rename_edit_);
  rename_edit_ = nullptr;
  rename_index_ = -1;
  if (!commit || index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring old_path = cells_[static_cast<size_t>(index)].item.path;
  const std::wstring neu = buf;
  const RenamePlan plan = PlanRename(old_path, neu);
  if (!plan.valid) {
    if (!neu.empty()) {
      ShowToast(plan.error);
    }
    return;
  }
  if (GetFileAttributesW(plan.dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
    ShowToast(L"该名称已存在");
    return;
  }
  if (!MoveFileW(old_path.c_str(), plan.dest.c_str())) {
    const DWORD err = GetLastError();
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
      ShowToast(L"文件正在使用中，请先关闭占用程序后重试");
    } else {
      ShowToast(L"无法重命名，文件可能被占用");
    }
    return;
  }
  SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW, old_path.c_str(),
                 plan.dest.c_str());
  if (on_path_replaced_) {
    on_path_replaced_(old_path, plan.dest);
  }
}

void FenceWindow::PasteClipboardFiles() {
  std::vector<std::wstring> paths;
  bool cut = false;
  if (!ClipboardGetFilesWithEffect(&paths, &cut) || paths.empty()) {
    ShowToast(L"剪贴板里没有文件");
    return;
  }
  if (on_pasted_) {
    on_pasted_(this, paths, cut);
  }
}

void FenceWindow::ShowFenceMenu(POINT screen) {
  // Host Explorer's folder-background menu (New / Paste / View / Refresh /
  // Undo / Sort ...) with DeskTidy extras prepended.
  const std::vector<ShellMenuExtra> extras{
      {Tr("fence_refresh"), kExtFenceRefresh},
      {Tr("fence_hide"), kExtFenceHide},
      {Tr("fence_dissolve"), kExtFenceDissolve},
  };
  ShellVerbFn on_verb = [this](const std::wstring& verb) {
    if (verb == L"refresh" && on_refresh_) {
      on_refresh_(this);
    }
  };
  const int rc = TrackShellFolderBackgroundMenu(
      hwnd_, screen, DesktopDirectory(), extras, on_verb, nullptr);
  if (rc >= 0) {
    if (rc == kExtFenceRefresh) {
      if (on_refresh_) {
        on_refresh_(this);
      }
    } else if (rc == kExtFenceHide) {
      Hide();
    } else if (rc == kExtFenceDissolve) {
      if (on_dissolve_) {
        on_dissolve_(this);
      }
    }
    return;
  }
  // Hosted menu unavailable — fall back to the hand-rolled fence menu.
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 10, Tr("fence_rename").c_str());
  AppendMenuW(menu, MF_STRING, 11,
              collapsed_ ? Tr("fence_expand").c_str()
                         : Tr("fence_collapse").c_str());
  AppendMenuW(menu, MF_STRING, 12,
              position_locked_ ? Tr("fence_unlock").c_str()
                               : Tr("fence_lock").c_str());
  AppendMenuW(menu, MF_STRING, 13, Tr("fence_paste").c_str());
  AppendMenuW(menu, MF_STRING, 14,
              portal_path_.empty() ? L"设为门户文件夹…" : L"更换门户文件夹…");
  if (!portal_path_.empty()) {
    AppendMenuW(menu, MF_STRING, 15, L"取消门户");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU pages = CreatePopupMenu();
  if (page_names_.empty()) {
    page_names_ = {L"工作", L"文档"};
  }
  for (size_t i = 0; i < page_names_.size() && i < 12; ++i) {
    const UINT flags = MF_STRING | (page_ == static_cast<int>(i) ? MF_CHECKED : 0);
    AppendMenuW(pages, flags, 400 + static_cast<UINT>(i), page_names_[i].c_str());
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pages), L"所在页面");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU sortm = CreatePopupMenu();
  auto sort_item = [&](UINT id, const char* key, const wchar_t* label) {
    const UINT flags =
        MF_STRING | (sort_by_ == key ? MF_CHECKED : 0);
    AppendMenuW(sortm, flags, id, label);
  };
  sort_item(500, "name", L"按名称");
  sort_item(501, "date", L"按修改日期");
  sort_item(502, "size", L"按大小");
  sort_item(503, "type", L"按类型");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortm),
              Tr("fence_sort").c_str());
  HMENU viewm = CreatePopupMenu();
  AppendMenuW(viewm, MF_STRING | (view_mode_ != "list" ? MF_CHECKED : 0), 510,
              Tr("view_grid").c_str());
  AppendMenuW(viewm, MF_STRING | (view_mode_ == "list" ? MF_CHECKED : 0), 511,
              Tr("view_list").c_str());
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(viewm),
              Tr("fence_view").c_str());
  HMENU opam = CreatePopupMenu();
  auto opa_item = [&](UINT id, int alpha, const wchar_t* label) {
    const bool on = (alpha < 0 && opacity_override_ < 0) ||
                    (alpha >= 0 && opacity_override_ == alpha);
    AppendMenuW(opam, MF_STRING | (on ? MF_CHECKED : 0), id, label);
  };
  opa_item(520, -1, Tr("opacity_follow").c_str());
  opa_item(521, 102, L"40%");
  opa_item(522, 140, L"55%");
  opa_item(523, 178, L"70%");
  opa_item(524, 217, L"85%");
  opa_item(525, 255, Tr("opacity_opaque").c_str());
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opam),
              Tr("fence_opacity").c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU styles = CreatePopupMenu();
  const auto all = AllFenceStyles();
  for (size_t i = 0; i < all.size(); ++i) {
    const UINT flags =
        MF_STRING | (style_id_ == all[i]->id ? MF_CHECKED : 0);
    AppendMenuW(styles, flags, 100 + static_cast<UINT>(i), all[i]->name);
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(styles),
              Tr("fence_style").c_str());
  SetForegroundWindow(hwnd_);
  // F8: remember original style so hover preview can revert on cancel.
  preview_style_id_ = style_id_;
  style_preview_active_ = false;
  style_menu_open_ = true;
  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0,
                     hwnd_, nullptr);
  style_menu_open_ = false;
  DestroyMenu(menu);
  if (cmd == 0) {
    RevertStylePreview();
    return;
  }
  if (cmd == 10 && on_rename_) {
    on_rename_(this);
  } else if (cmd == 11) {
    ToggleCollapsed();
  } else if (cmd == 12) {
    SetPositionLocked(!position_locked_);
  } else if (cmd == 13) {
    PasteClipboardFiles();
  } else if (cmd == 500) {
    SetSortBy("name");
  } else if (cmd == 501) {
    SetSortBy("date");
  } else if (cmd == 502) {
    SetSortBy("size");
  } else if (cmd == 503) {
    SetSortBy("type");
  } else if (cmd == 510) {
    SetViewMode("grid");
  } else if (cmd == 511) {
    SetViewMode("list");
  } else if (cmd == 520) {
    SetOpacity(-1);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 521) {
    SetOpacity(102);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 522) {
    SetOpacity(140);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 523) {
    SetOpacity(178);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 524) {
    SetOpacity(217);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 525) {
    SetOpacity(255);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 14 || cmd == 15) {
    if (cmd == 15) {
      portal_path_.clear();
      if (on_portal_) {
        on_portal_(this, L"");
      }
    } else {
      BROWSEINFOW bi{};
      bi.hwndOwner = hwnd_;
      bi.lpszTitle = L"选择要镜像的文件夹";
      bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
      PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
      if (pidl) {
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, path)) {
          portal_path_ = path;
          if (on_portal_) {
            on_portal_(this, portal_path_);
          }
        }
        CoTaskMemFree(pidl);
      }
    }
  } else if (cmd >= 400 && cmd < 400 + static_cast<int>(page_names_.size()) &&
             on_set_page_) {
    on_set_page_(this, cmd - 400);
  } else if (cmd >= 100 && cmd < 100 + static_cast<int>(all.size())) {
    SetStyleId(all[static_cast<size_t>(cmd - 100)]->id);
    style_preview_active_ = false;
    preview_style_id_.clear();
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  }
}

LRESULT CALLBACK FenceWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
  FenceWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<FenceWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<FenceWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT FenceWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_APP + 60) {
    const int vk = static_cast<int>(wparam);
    const bool ctrl = (lparam & 1) != 0;
    const bool shift = (lparam & 2) != 0;
    HandleFenceHotkey(vk, ctrl, shift);
    return 0;
  }
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      PresentLayered();
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      LayoutCells();
      PresentLayered();
      return 0;
    case WM_KEYDOWN:
      if (wparam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        PasteClipboardFiles();
        return 0;
      }
      break;
    case WM_MOUSEWHEEL: {
      if (collapsed_) {
        return 0;
      }
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
      // F4: Alt/Ctrl + wheel zooms this fence's icons (matches Python).
      const bool zoom_mod =
          (GetKeyState(VK_MENU) & 0x8000) != 0 ||
          (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      if (zoom_mod) {
        ApplyZoomWheel(delta);
        return 0;
      }
      scroll_y_ -= delta / 2;
      ClampScroll();
      LayoutCells();
      PresentLayered();
      return 0;
    }
    case WM_MENUSELECT: {
      // F8: hover over a style item previews it (reverted on cancel).
      if (style_menu_open_ && !(HIWORD(wparam) & MF_POPUP)) {
        const UINT id = LOWORD(wparam);
        const auto all = AllFenceStyles();
        if (id >= 100 && id < 100 + all.size()) {
          PreviewStyleId(all[id - 100]->id);
          return 0;
        }
      }
      break;
    }
    case WM_SETCURSOR: {
      if (LOWORD(lparam) == HTCLIENT) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        const Edge e = HitTestEdge(pt.x, pt.y);
        const int v = static_cast<int>(e);
        if (v == (static_cast<int>(Edge::Left) | static_cast<int>(Edge::Top)) ||
            v == (static_cast<int>(Edge::Right) |
                 static_cast<int>(Edge::Bottom))) {
          SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
          return TRUE;
        }
        if (v == (static_cast<int>(Edge::Right) | static_cast<int>(Edge::Top)) ||
            v == (static_cast<int>(Edge::Left) |
                 static_cast<int>(Edge::Bottom))) {
          SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
          return TRUE;
        }
        if (v & (static_cast<int>(Edge::Left) | static_cast<int>(Edge::Right))) {
          SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
          return TRUE;
        }
        if (v & (static_cast<int>(Edge::Top) | static_cast<int>(Edge::Bottom))) {
          SetCursor(LoadCursor(nullptr, IDC_SIZENS));
          return TRUE;
        }
      }
      break;
    }
    case WM_LBUTTONDOWN: {
      press_client_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      // Scrollbar wins over the east resize strip when content overflows.
      const int sb = HitTestScrollbar(press_client_.x, press_client_.y);
      if (sb == 1) {
        thumb_dragging_ = true;
        thumb_start_y_ = press_client_.y;
        thumb_start_scroll_ = scroll_y_;
        SetCapture(hwnd_);
        return 0;
      }
      if (sb == 2) {
        // Click above/below thumb → page scroll.
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int header = S(hwnd_, kHeaderH0);
        const int view = (std::max)(0, static_cast<int>(rc.bottom) - header -
                                           S(hwnd_, kPad0));
        const int track_top = header + 2;
        const int track_h =
            (std::max)(8, static_cast<int>(rc.bottom) - track_top - 4);
        const int thumb_h =
            (std::max)(16, track_h * view / (std::max)(1, content_h_));
        const int max_scroll = (std::max)(1, content_h_ - view);
        int thumb_y = track_top + (track_h - thumb_h) * scroll_y_ / max_scroll;
        ScrollbarPage(press_client_.y < thumb_y ? -1 : 1);
        return 0;
      }
      const Edge edge = HitTestEdge(press_client_.x, press_client_.y);
      const int icon = HitTestIcon(press_client_.x, press_client_.y);
      const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      // Explorer / Fences: click empty panel (or header/edge) clears selection.
      // Ctrl keeps selection for additive rubber-band.
      if (icon < 0 && !ctrl) {
        ClearSelection();
      }
      if (edge != Edge::None) {
        resizing_ = true;
        resize_edge_ = edge;
        GetWindowRect(hwnd_, &resize_start_rc_);
        GetCursorPos(&resize_start_pt_);
        SetCapture(hwnd_);
        return 0;
      }
      if (icon >= 0) {
        dragging_ = false;
        drag_moved_ = false;
        rubber_ = false;
        rubber_additive_ = false;
        icon_press_ = true;
        icon_press_index_ = icon;
        SetCapture(hwnd_);
        return 0;
      }
      icon_press_ = false;
      icon_press_index_ = -1;
      if (press_client_.y >= S(hwnd_, kHeaderH0)) {
        if (collapsed_) {
          return 0;
        }
        rubber_ = true;
        rubber_additive_ = ctrl;
        dragging_ = false;
        rubber_a_ = rubber_b_ = press_client_;
        SetCapture(hwnd_);
        return 0;
      }
      if (position_locked_) {
        return 0;
      }
      rubber_ = false;
      dragging_ = true;
      drag_moved_ = false;
      POINT pt = press_client_;
      ClientToScreen(hwnd_, &pt);
      RECT rc{};
      GetWindowRect(hwnd_, &rc);
      drag_offset_.x = pt.x - rc.left;
      drag_offset_.y = pt.y - rc.top;
      SetCapture(hwnd_);
      return 0;
    }
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tme{};
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE;
      tme.hwndTrack = hwnd_;
      TrackMouseEvent(&tme);
      if (resizing_ && (wparam & MK_LBUTTON)) {
        UpdateHoverTip(-1);
        POINT pt{};
        GetCursorPos(&pt);
        const int dx = pt.x - resize_start_pt_.x;
        const int dy = pt.y - resize_start_pt_.y;
        int l = resize_start_rc_.left;
        int t = resize_start_rc_.top;
        int r = resize_start_rc_.right;
        int b = resize_start_rc_.bottom;
        const int ev = static_cast<int>(resize_edge_);
        if (ev & static_cast<int>(Edge::Left)) {
          l += dx;
        }
        if (ev & static_cast<int>(Edge::Right)) {
          r += dx;
        }
        if (ev & static_cast<int>(Edge::Top)) {
          t += dy;
        }
        if (ev & static_cast<int>(Edge::Bottom)) {
          b += dy;
        }
        if (r - l < 160) {
          if (ev & static_cast<int>(Edge::Left)) {
            l = r - 160;
          } else {
            r = l + 160;
          }
        }
        if (b - t < 120) {
          if (ev & static_cast<int>(Edge::Top)) {
            t = b - 120;
          } else {
            b = t + 120;
          }
        }
        SetWindowPos(hwnd_, nullptr, l, t, r - l, b - t,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      if (rubber_ && (wparam & MK_LBUTTON)) {
        UpdateHoverTip(-1);
        rubber_b_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        // Live marquee selection like Explorer / Fences.
        const int moved =
            abs(rubber_b_.x - rubber_a_.x) + abs(rubber_b_.y - rubber_a_.y);
        if (moved >= kDragThreshold) {
          ApplyRubberBandSelection();
        }
        PresentLayered();
        return 0;
      }
      if (icon_press_ && (wparam & MK_LBUTTON)) {
        UpdateHoverTip(-1);
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        if (abs(x - press_client_.x) >= kDragThreshold ||
            abs(y - press_client_.y) >= kDragThreshold) {
          BeginIconDrag(icon_press_index_);
        }
        return 0;
      }
      if (icon_dragging_ && (wparam & MK_LBUTTON)) {
        UpdateManualIconDrag(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
      }
      if (thumb_dragging_ && (wparam & MK_LBUTTON)) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int dy = pt.y - thumb_start_y_;
        const int header = S(hwnd_, kHeaderH0);
        const int view = (std::max)(0, static_cast<int>(rc.bottom) - header -
                                           S(hwnd_, kPad0));
        const int track_top = header + 2;
        const int track_h =
            (std::max)(8, static_cast<int>(rc.bottom) - track_top - 4);
        const int thumb_h =
            (std::max)(16, track_h * view / (std::max)(1, content_h_));
        const int max_scroll = (std::max)(1, content_h_ - view);
        const int track_span = (std::max)(1, track_h - thumb_h);
        scroll_y_ = thumb_start_scroll_ + dy * max_scroll / track_span;
        ClampScroll();
        LayoutCells();
        PresentLayered();
        return 0;
      }
      if (!dragging_ || !(wparam & MK_LBUTTON)) {
        const int hit =
            HitTestIcon(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        if (hit != tip_index_) {
          UpdateHoverTip(hit);
        } else if (hit >= 0 && tip_) {
          POINT pt{};
          GetCursorPos(&pt);
          SendMessageW(tip_, TTM_TRACKPOSITION, 0,
                       MAKELPARAM(pt.x + 12, pt.y + 16));
        }
        return 0;
      }
      UpdateHoverTip(-1);
      POINT pt{};
      GetCursorPos(&pt);
      SetWindowPos(hwnd_, nullptr, pt.x - drag_offset_.x, pt.y - drag_offset_.y,
                   0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      drag_moved_ = true;
      return 0;
    }
    case WM_MOUSELEAVE:
      UpdateHoverTip(-1);
      return 0;
    case WM_LBUTTONUP: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      ReleaseCapture();
      if (thumb_dragging_) {
        thumb_dragging_ = false;
        return 0;
      }
      if (icon_dragging_) {
        FinishManualIconDrag(x, y);
        return 0;
      }
      if (rubber_) {
        rubber_ = false;
        const int moved =
            abs(rubber_b_.x - rubber_a_.x) + abs(rubber_b_.y - rubber_a_.y);
        // Click (no drag) on blank = deselect only (already cleared on press).
        if (moved < kDragThreshold && !rubber_additive_) {
          rubber_additive_ = false;
          ClearSelection();
          return 0;
        }
        ApplyRubberBandSelection();
        rubber_additive_ = false;
        PresentLayered();
        return 0;
      }
      if (resizing_) {
        resizing_ = false;
        resize_edge_ = Edge::None;
        AttachToDesktopLayer();
        if (on_moved_) {
          on_moved_(this);
        }
        return 0;
      }
      if (icon_press_) {
        const int idx = icon_press_index_;
        icon_press_ = false;
        icon_press_index_ = -1;
        if (HitTestIcon(x, y) == idx) {
          const bool was_selected = IsSelected(idx);
          const DWORD now = GetTickCount();
          const UINT dbl = GetDoubleClickTime();
          if (idx == last_icon_click_ &&
              now - last_icon_click_tick_ <= dbl) {
            last_icon_click_ = -1;
            last_icon_click_tick_ = 0;
            OpenItem(idx);
            return 0;
          }
          last_icon_click_ = idx;
          last_icon_click_tick_ = now;
          const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
          const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
          if (ctrl) {
            ToggleSelected(idx);
          } else if (shift) {
            SelectRange(idx);
          } else if (was_selected) {
            // Re-click on an already-selected icon/label starts inline rename.
            BeginInlineRename();
          } else {
            SelectOnly(idx);
          }
        }
        return 0;
      }
      if (dragging_) {
        dragging_ = false;
        if (!drag_moved_) {
          ClearSelection();
        }
        if (drag_moved_) {
          AttachToDesktopLayer();
          if (on_geometry_moved_) {
            on_geometry_moved_(this);
          } else if (on_moved_) {
            RECT rc{};
            GetWindowRect(hwnd_, &rc);
            ClampRectToWorkArea(&rc);
            SetWindowPos(hwnd_, nullptr, rc.left, rc.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            on_moved_(this);
          }
        }
        return 0;
      }
      return 0;
    }
    case WM_RBUTTONDOWN: {
      // Match Explorer: right-press on empty cancels rubber / keeps chrome clean.
      if (rubber_) {
        rubber_ = false;
        rubber_additive_ = false;
        ReleaseCapture();
        PresentLayered();
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int icon = HitTestIcon(pt.x, pt.y);
      ClientToScreen(hwnd_, &pt);
      if (icon >= 0) {
        // ShowIconMenu keeps multi-select when the clicked item is already selected.
        ShowIconMenu(icon, pt);
      } else {
        ShowFenceMenu(pt);
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      const int icon = HitTestIcon(x, y);
      if (icon >= 0) {
        last_icon_click_ = -1;
        last_icon_click_tick_ = 0;
        OpenItem(icon);
        return 0;
      }
      if (y < S(hwnd_, kHeaderH0) && HitTestEdge(x, y) == Edge::None) {
        // Title double-click toggles collapse (rename stays in context menu).
        ToggleCollapsed();
      }
      return 0;
    }
    case WM_DESTROY:
      tip_ = nullptr;
      tip_index_ = -1;
      ClearIcons();
      if (drop_target_) {
        RevokePathDropTarget(hwnd_);
        drop_target_->Release();
        drop_target_ = nullptr;
      }
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void FenceWindow::Paint(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);

  const int header = S(hwnd_, kHeaderH0);
  const int pad = S(hwnd_, kPad0);
  const bool list = view_mode_ == "list";
  const int cell_w = CellWidthPx();
  const int icon = IconSizePx();
  const int font_px = S(hwnd_, list ? 13 : 12);
  const int title_px = S(hwnd_, 15);

  SetGraphicsMode(hdc, GM_COMPATIBLE);
  Gdiplus::Graphics g(hdc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
  g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
  // Grid-fit grayscale AA stays sharp on a layered (premultiplied) surface.
  // ClearType assumes an opaque background and fringes here.
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
  g.SetTextContrast(4);

  Gdiplus::FontFamily family(L"Microsoft YaHei UI");
  const Gdiplus::FontFamily* face =
      family.IsAvailable() ? &family : Gdiplus::FontFamily::GenericSansSerif();
  Gdiplus::Font title_font(face, static_cast<Gdiplus::REAL>(title_px),
                           Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
  Gdiplus::SolidBrush title_brush(
      Gdiplus::Color(255, GetRValue(title_color_), GetGValue(title_color_),
                     GetBValue(title_color_)));
  Gdiplus::StringFormat title_fmt;
  title_fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
  title_fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
  title_fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
  title_fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
  const int title_top = S(hwnd_, 6);
  {
    std::wstring caption;
    if (collapsed_) {
      caption += L"▸  ";
    }
    // F6: on narrow fences drop the lock prefix so the title keeps room.
    if (position_locked_ && rc.right >= S(hwnd_, 132)) {
      caption += L"[锁] ";
    }
    caption += title_;
    Gdiplus::RectF title_box(0.f, static_cast<Gdiplus::REAL>(title_top),
                             static_cast<Gdiplus::REAL>(rc.right),
                             static_cast<Gdiplus::REAL>(title_px + 6));
    g.DrawString(caption.c_str(), -1, &title_font, title_box, &title_fmt,
                 &title_brush);
  }
  if (collapsed_) {
    return;
  }

  const int view = (std::max)(0, static_cast<int>(rc.bottom) - header - pad);
  if (content_h_ > view && view > 0) {
    const int track_top = header + 2;
    const int track_h = (std::max)(8, static_cast<int>(rc.bottom) - track_top - 4);
    const int thumb_h =
        (std::max)(16, track_h * view / (std::max)(1, content_h_));
    const int max_scroll = (std::max)(1, content_h_ - view);
    int thumb_y = track_top + (track_h - thumb_h) * scroll_y_ / max_scroll;
    if (thumb_y + thumb_h > track_top + track_h) {
      thumb_y = track_top + track_h - thumb_h;
    }
    RECT thumb{rc.right - 7, thumb_y, rc.right - 3, thumb_y + thumb_h};
    HBRUSH thumb_br = CreateSolidBrush(accent_);
    FillRect(hdc, &thumb, thumb_br);
    DeleteObject(thumb_br);
  }

  g.SetClip(Gdiplus::Rect(rc.left, header, rc.right - rc.left,
                          rc.bottom - header));

  Gdiplus::Font font(face, static_cast<Gdiplus::REAL>(font_px),
                     Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
  Gdiplus::StringFormat list_fmt;
  list_fmt.SetAlignment(Gdiplus::StringAlignmentNear);
  list_fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
  list_fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
  list_fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

  // Idle grid: wrap up to 2 lines with ellipsis (Explorer / Fences style).
  Gdiplus::StringFormat grid_fmt;
  grid_fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
  grid_fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
  grid_fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
  grid_fmt.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);

  // Selected: full name, no ellipsis — grow vertically like Explorer.
  Gdiplus::StringFormat sel_fmt;
  sel_fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
  sel_fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
  sel_fmt.SetTrimming(Gdiplus::StringTrimmingNone);

  const int line_h = font_px + S(hwnd_, 5);
  const int idle_label_h = line_h * 2 + S(hwnd_, 4);
  const int max_sel_lines = 8;

  // Cut ghost opacity matches Python _CUT_GHOST_OPACITY ≈ 0.42.
  constexpr BYTE kCutOpacity = 107;

  auto paint_cell = [&](size_t i, bool selected_pass) {
    if (i >= cells_.size()) {
      return;
    }
    const bool is_sel =
        i < selected_.size() && selected_[i] != 0;
    if (selected_pass != is_sel) {
      return;
    }
    const auto& cell = cells_[i];
    const RECT& b = cell.bounds;
    if (b.bottom < header || b.top > rc.bottom) {
      return;
    }
    const bool is_cut = ClipboardPathIsCut(cell.item.path);
    const bool is_hover =
        !is_sel && tip_index_ >= 0 &&
        static_cast<size_t>(tip_index_) == i;
    const BYTE icon_op = is_cut ? kCutOpacity : 255;
    Gdiplus::SolidBrush name_brush(
        Gdiplus::Color(is_cut ? kCutOpacity : 255, GetRValue(caption_color_),
                       GetGValue(caption_color_), GetBValue(caption_color_)));
    if (list) {
      if (is_sel) {
        PaintSelectionChrome(g, b.left + 2, b.top + 1, b.right - 2,
                             b.bottom - 1, accent_, S(hwnd_, 4));
      } else if (is_hover) {
        PaintSelectionChrome(g, b.left + 2, b.top + 1, b.right - 2,
                             b.bottom - 1, accent_, S(hwnd_, 4), 48, 90);
      }
      const int iy = b.top + ((b.bottom - b.top) - icon) / 2;
      if (cell.icon) {
        DrawIconSharp(g, cell.icon, b.left + 4, iy, icon, icon_op);
      }
      const int lw =
          (std::max)(1, static_cast<int>(b.right - b.left) - icon - 12);
      Gdiplus::RectF label(static_cast<Gdiplus::REAL>(b.left + 8 + icon),
                           static_cast<Gdiplus::REAL>(b.top),
                           static_cast<Gdiplus::REAL>(lw),
                           static_cast<Gdiplus::REAL>(b.bottom - b.top));
      g.DrawString(cell.item.name.c_str(), -1, &font, label, &list_fmt,
                   &name_brush);
      return;
    }

    const int label_w = (std::max)(1, static_cast<int>(b.right - b.left) - 6);
    const int label_top = b.top + 4 + icon + 2;
    int label_h = idle_label_h;
    int ink_h = idle_label_h;
    if (is_sel) {
      Gdiplus::RectF measure(
          0.f, 0.f, static_cast<Gdiplus::REAL>(label_w),
          static_cast<Gdiplus::REAL>(line_h * max_sel_lines + 8));
      Gdiplus::RectF bound;
      g.MeasureString(cell.item.name.c_str(), -1, &font, measure, &sel_fmt,
                      &bound);
      ink_h = static_cast<int>(bound.Height + 0.5f) + 2;
      if (ink_h < line_h) {
        ink_h = line_h;
      }
      label_h = ink_h;
      if (label_h < idle_label_h) {
        label_h = idle_label_h;
      }
      const int cap = line_h * max_sel_lines + S(hwnd_, 8);
      if (label_h > cap) {
        label_h = cap;
      }
      if (ink_h > label_h) {
        ink_h = label_h;
      }
      // Hug glyph + caption ink (not empty 2-line shelf). Translucent accent.
      const int chrome_b = label_top + ink_h + S(hwnd_, 3);
      PaintSelectionChrome(g, b.left + 3, b.top + 2, b.right - 3, chrome_b,
                           accent_, S(hwnd_, 6));
    } else if (is_hover) {
      const int chrome_b = label_top + idle_label_h + S(hwnd_, 2);
      PaintSelectionChrome(g, b.left + 3, b.top + 2, b.right - 3, chrome_b,
                           accent_, S(hwnd_, 6), 48, 90);
    }

    if (cell.icon) {
      const int ix = b.left + (cell_w - icon) / 2;
      const int iy = b.top + 4;
      DrawIconSharp(g, cell.icon, ix, iy, icon, icon_op);
    }
    Gdiplus::RectF label(static_cast<Gdiplus::REAL>(b.left + 3),
                         static_cast<Gdiplus::REAL>(label_top),
                         static_cast<Gdiplus::REAL>(label_w),
                         static_cast<Gdiplus::REAL>(label_h));
    if (is_sel) {
      // If still overflowing max lines, fall back to ellipsis on last line.
      Gdiplus::StringFormat* fmt = &sel_fmt;
      Gdiplus::StringFormat ellipsis_fmt;
      if (label_h >= line_h * max_sel_lines) {
        ellipsis_fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        ellipsis_fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
        ellipsis_fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        ellipsis_fmt.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);
        fmt = &ellipsis_fmt;
      }
      g.DrawString(cell.item.name.c_str(), -1, &font, label, fmt,
                   &name_brush);
    } else {
      g.DrawString(cell.item.name.c_str(), -1, &font, label, &grid_fmt,
                   &name_brush);
    }
  };

  // Idle cells first, then selected on top (expanded captions overlay neighbors).
  for (size_t i = 0; i < cells_.size(); ++i) {
    paint_cell(i, false);
  }
  for (size_t i = 0; i < cells_.size(); ++i) {
    paint_cell(i, true);
  }

  g.ResetClip();
  if (rubber_) {
    const int l = (std::min)(rubber_a_.x, rubber_b_.x);
    const int t = (std::min)(rubber_a_.y, rubber_b_.y);
    const int r = (std::max)(rubber_a_.x, rubber_b_.x);
    const int b = (std::max)(rubber_a_.y, rubber_b_.y);
    if (r - l >= 2 && b - t >= 2) {
      // Explorer-style translucent blue-ish band (use accent; keep alpha valid
      // for UpdateLayeredWindow — no GDI Rectangle).
      Gdiplus::SolidBrush fill(Gdiplus::Color(
          56, GetRValue(accent_), GetGValue(accent_), GetBValue(accent_)));
      g.FillRectangle(&fill, l, t, r - l, b - t);
      Gdiplus::Pen edge(
          Gdiplus::Color(200, GetRValue(accent_), GetGValue(accent_),
                         GetBValue(accent_)),
          1.0f);
      g.DrawRectangle(&edge, l, t, r - l - 1, b - t - 1);
    }
  }

  // F2: live insert indicator during manual icon drag.
  if (icon_dragging_ && !drag_left_ && drag_insert_ >= 0) {
    const int insert = (std::min)(drag_insert_, static_cast<int>(cells_.size()));
    Gdiplus::Pen pen(Gdiplus::Color(235, GetRValue(accent_), GetGValue(accent_),
                                    GetBValue(accent_)),
                     2.0f);
    if (view_mode_ == "list") {
      const int y = insert < static_cast<int>(cells_.size())
                        ? cells_[static_cast<size_t>(insert)].bounds.top
                        : (cells_.empty() ? header + pad
                                          : cells_.back().bounds.bottom);
      g.DrawLine(&pen, rc.left + pad, y, rc.right - pad, y);
    } else {
      const int x = insert < static_cast<int>(cells_.size())
                        ? cells_[static_cast<size_t>(insert)].bounds.left
                        : (cells_.empty() ? rc.left + pad
                                          : cells_.back().bounds.right);
      g.DrawLine(&pen, x, header + pad, x, rc.bottom - pad);
    }
  }

  // F5: bottom count / zoom hint band (hidden when narrow or collapsed).
  if (!collapsed_ && rc.right >= S(hwnd_, 200)) {
    const int footer_h = S(hwnd_, 22);
    const int fy = rc.bottom - footer_h;
    std::wstring footer;
    if (GetTickCount() < footer_zoom_hint_until_) {
      footer = L"图标大小 " + std::to_wstring(icon_zoom_percent_) + L"%";
    } else {
      const int count = (std::min)(static_cast<int>(cells_.size()), 60);
      if (count > 0) {
        footer = L"共 " + std::to_wstring(count) + L" 项";
      }
    }
    if (!footer.empty()) {
      Gdiplus::Font f_font(face, static_cast<Gdiplus::REAL>(S(hwnd_, 11)),
                           Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
      Gdiplus::SolidBrush f_brush(Gdiplus::Color(
          190, GetRValue(caption_color_), GetGValue(caption_color_),
          GetBValue(caption_color_)));
      Gdiplus::StringFormat f_fmt;
      f_fmt.SetAlignment(Gdiplus::StringAlignmentNear);
      f_fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
      f_fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
      f_fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
      Gdiplus::RectF f_box(static_cast<Gdiplus::REAL>(pad),
                           static_cast<Gdiplus::REAL>(fy),
                           static_cast<Gdiplus::REAL>(rc.right - pad - 12),
                           static_cast<Gdiplus::REAL>(footer_h));
      g.DrawString(footer.c_str(), -1, &f_font, f_box, &f_fmt, &f_brush);
    }
  }
}
