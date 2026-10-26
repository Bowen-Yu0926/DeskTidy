#include "dock_window.h"

#include "app_prefs.h"
#include "desktop_shell.h"
#include "display_geom.h"
#include "icon_actions.h"

#include <CommCtrl.h>
#include <Shellapi.h>
#include <windowsx.h>

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstring>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeDock";
// Match Python DockWidget: 44px buttons, 32px icons, 16px radius.
constexpr int kBtn = 44;
constexpr int kIcon = 32;
constexpr int kGap = 8;
constexpr int kMarginX = 10;
constexpr int kMarginY = 6;
constexpr int kH = kMarginY * 2 + kBtn;
bool g_registered = false;

void AddRoundRect(Gdiplus::GraphicsPath* path, int w, int h, int radius) {
  const int rr = radius * 2;
  const int rw = (std::max)(1, w - 1);
  const int rh = (std::max)(1, h - 1);
  path->AddArc(0, 0, rr, rr, 180, 90);
  path->AddArc(rw - rr, 0, rr, rr, 270, 90);
  path->AddArc(rw - rr, rh - rr, rr, rr, 0, 90);
  path->AddArc(0, rh - rr, rr, rr, 90, 90);
  path->CloseFigure();
}
}  // namespace

std::vector<std::wstring> SplitDockCsv(const std::wstring& csv) {
  std::vector<std::wstring> out;
  size_t start = 0;
  while (start <= csv.size()) {
    size_t comma = csv.find(L',', start);
    if (comma == std::wstring::npos) {
      comma = csv.size();
    }
    std::wstring part = csv.substr(start, comma - start);
    // Trim whitespace (paths may contain spaces but not leading/trailing).
    size_t a = 0;
    while (a < part.size() && (part[a] == L' ' || part[a] == L'\t' ||
                               part[a] == L'\r' || part[a] == L'\n')) {
      ++a;
    }
    size_t b = part.size();
    while (b > a && (part[b - 1] == L' ' || part[b - 1] == L'\t' ||
                     part[b - 1] == L'\r' || part[b - 1] == L'\n')) {
      --b;
    }
    part = part.substr(a, b - a);
    if (!part.empty()) {
      out.push_back(part);
    }
    if (comma >= csv.size()) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

void DockWindow::ApplyPaths(const std::vector<std::wstring>& paths) {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
  for (const auto& path : paths) {
    if (slots_.size() >= 8) {
      break;
    }
    if (path.empty() ||
        GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      continue;
    }
    Slot s;
    s.item.path = path;
    s.item.name = path;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash + 1 < path.size()) {
      s.item.name = path.substr(slash + 1);
    }
    const size_t dot = s.item.name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) {
      const std::wstring ext = s.item.name.substr(dot);
      if (_wcsicmp(ext.c_str(), L".lnk") == 0 ||
          _wcsicmp(ext.c_str(), L".url") == 0) {
        s.item.is_link = true;
        s.item.name.resize(dot);
      }
    }
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME) &&
        sfi.szDisplayName[0]) {
      s.item.name = sfi.szDisplayName;
    }
    s.icon = LoadItemIcon(path, kIcon);
    slots_.push_back(std::move(s));
  }
  LayoutSlots();
  ApplyVisibility();
}

void DockWindow::SetItemsFromCsv(const std::wstring& csv) {
  ApplyPaths(SplitDockCsv(csv));
}

void DockWindow::ReloadFromPrefs() {
  ApplyPaths(LoadAppPrefs().dock_items);
}

bool DockWindow::Create(HINSTANCE instance) {
  if (!g_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DockWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    g_registered = true;
  }

  POINT cursor{};
  GetCursorPos(&cursor);
  const RECT work = WorkAreaAtPoint(cursor);
  const int w = kMarginX * 2 + kBtn;
  const int x = work.left + (work.right - work.left - w) / 2;
  // Match Python DockWidget: 48px above work-area bottom (page bar clearance).
  const int y = work.bottom - kH - 48;
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName, L"Dock",
      WS_POPUP, x, y, w, kH, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void DockWindow::Destroy() {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
      s.icon = nullptr;
    }
  }
  slots_.clear();
  if (tip_) {
    DestroyWindow(tip_);
    tip_ = nullptr;
  }
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void DockWindow::EnsureTooltip() {
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
  SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, 280);
}

void DockWindow::UpdateHoverTip(int index) {
  EnsureTooltip();
  if (!tip_) {
    return;
  }
  TOOLINFOW ti{};
  ti.cbSize = sizeof(ti);
  ti.hwnd = hwnd_;
  ti.uId = 1;
  if (index < 0 || index >= static_cast<int>(slots_.size())) {
    SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    return;
  }
  const auto& item = slots_[static_cast<size_t>(index)].item;
  std::wstring text = item.name.empty() ? item.path : item.name;
  ti.lpszText = const_cast<wchar_t*>(text.c_str());
  SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
  POINT pt{};
  GetCursorPos(&pt);
  SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x + 12, pt.y + 16));
  SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
}

void DockWindow::Show() { SetVisible(true); }

void DockWindow::Hide() { SetVisible(false); }

void DockWindow::ApplyVisibility() {
  if (!hwnd_) {
    return;
  }
  if (!want_visible_ || slots_.empty()) {
    ShowWindow(hwnd_, SW_HIDE);
    return;
  }
  if (!IsWindowVisible(hwnd_)) {
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }
  AttachToDesktopLayer();  // no-op when already seated (avoids page flash)
  Present();
}

void DockWindow::SetVisible(bool on) {
  want_visible_ = on;
  ApplyVisibility();
}

void DockWindow::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  if (IsAttachedToDesktop(hwnd_)) {
    return;
  }
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

void DockWindow::SetItems(std::vector<DesktopItem> items) {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
  slots_.reserve((std::min)(items.size(), size_t{8}));
  for (auto& item : items) {
    if (slots_.size() >= 8) {
      break;
    }
    // Match Python: skip paths that no longer exist.
    if (GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      continue;
    }
    Slot s;
    s.item = std::move(item);
    s.icon = LoadItemIcon(s.item.path, kIcon);
    slots_.push_back(std::move(s));
  }
  LayoutSlots();
  ApplyVisibility();
}

void DockWindow::SetEdge(const std::wstring& edge) {
  if (edge == L"left" || edge == L"right" || edge == L"bottom") {
    edge_ = edge;
  } else {
    edge_ = L"bottom";
  }
  LayoutSlots();
}

void DockWindow::SetOpacity(float opacity) {
  if (opacity < 0.4f) {
    opacity = 0.4f;
  }
  if (opacity > 1.0f) {
    opacity = 1.0f;
  }
  fill_alpha_ = static_cast<BYTE>(opacity * 255.f + 0.5f);
  Present();
}

void DockWindow::LayoutSlots() {
  if (!hwnd_) {
    return;
  }
  if (slots_.empty()) {
    ApplyVisibility();
    return;
  }
  const int n = static_cast<int>(slots_.size());
  POINT cursor{};
  GetCursorPos(&cursor);
  // Prefer the monitor under the cursor (Python work_screen), not primary only.
  const RECT work = WorkAreaAtPoint(cursor);
  const int icon_pad = (kBtn - kIcon) / 2;
  int x = 0, y = 0, w = 0, h = 0;
  if (edge_ == L"left" || edge_ == L"right") {
    w = kMarginY * 2 + kBtn;
    h = kMarginX * 2 + n * kBtn + (std::max)(0, n - 1) * kGap;
    y = work.top + ((work.bottom - work.top) - h) / 2;
    x = edge_ == L"left" ? work.left + 12 : work.right - w - 12;
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    int top = kMarginX;
    for (auto& s : slots_) {
      s.bounds = {kMarginY + icon_pad, top + icon_pad,
                  kMarginY + icon_pad + kIcon, top + icon_pad + kIcon};
      top += kBtn + kGap;
    }
  } else {
    w = kMarginX * 2 + n * kBtn + (std::max)(0, n - 1) * kGap;
    h = kH;
    x = work.left + ((work.right - work.left) - w) / 2;
    y = work.bottom - h - 48;
    SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    int left = kMarginX;
    for (auto& s : slots_) {
      s.bounds = {left + icon_pad, kMarginY + icon_pad, left + icon_pad + kIcon,
                  kMarginY + icon_pad + kIcon};
      left += kBtn + kGap;
    }
  }
  Present();
}

void DockWindow::ShowSlotMenu(int index, POINT screen) {
  if (index < 0 || index >= static_cast<int>(slots_.size())) {
    return;
  }
  const std::wstring path = slots_[static_cast<size_t>(index)].item.path;
  const int cmd = TrackIconShellMenu(hwnd_, screen, false);
  if (cmd == 1) {
    OpenShellPath(path);
  } else if (cmd == 2) {
    RevealShellPath(path);
  } else if (cmd == 4 || cmd == 5) {
    CopyOrCutShellPath(hwnd_, path, cmd == 5);
  } else if (cmd == 6) {
    CopyShellPathText(hwnd_, path);
  } else if (cmd == 7) {
    std::wstring dest;
    if (RenameShellPath(hwnd_, path, &dest) == IconFsResult::Ok &&
        on_path_replaced_) {
      on_path_replaced_(path, dest);
    }
  } else if (cmd == 8) {
    ShowShellPathProperties(hwnd_, path);
  } else if (cmd == 9) {
    if (RecycleShellPath(hwnd_, path) == IconFsResult::Ok && on_removed_) {
      on_removed_(path);
    }
  }
}

int DockWindow::HitTest(int x, int y) const {
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& b = slots_[i].bounds;
    if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

LRESULT CALLBACK DockWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  DockWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<DockWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT DockWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      Present();
      return 0;
    }
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tme{};
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE;
      tme.hwndTrack = hwnd_;
      TrackMouseEvent(&tme);
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit != hover_index_) {
        hover_index_ = hit;
        Present();
        UpdateHoverTip(hit);
      } else if (hit >= 0 && tip_) {
        POINT pt{};
        GetCursorPos(&pt);
        SendMessageW(tip_, TTM_TRACKPOSITION, 0,
                     MAKELPARAM(pt.x + 12, pt.y + 16));
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hover_index_ >= 0) {
        hover_index_ = -1;
        Present();
        UpdateHoverTip(-1);
      }
      return 0;
    case WM_LBUTTONUP: {
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0) {
        OpenShellPath(slots_[static_cast<size_t>(hit)].item.path);
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0) {
        POINT pt{};
        GetCursorPos(&pt);
        ShowSlotMenu(hit, pt);
      }
      return 0;
    }
    case WM_DESTROY:
      tip_ = nullptr;
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void DockWindow::Paint(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int w = (std::max)(1, static_cast<int>(rc.right));
  const int h = (std::max)(1, static_cast<int>(rc.bottom));
  Gdiplus::Graphics g(hdc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  Gdiplus::GraphicsPath path;
  AddRoundRect(&path, w, h, 16);
  // Python default: rgba(22,27,34,0.85) + blue border.
  Gdiplus::SolidBrush fill(Gdiplus::Color(fill_alpha_, 22, 27, 34));
  Gdiplus::Pen edge(Gdiplus::Color(100, 88, 166, 255), 1.0f);
  g.FillPath(&fill, &path);
  g.DrawPath(&edge, &path);
  for (size_t i = 0; i < slots_.size(); ++i) {
    const auto& s = slots_[i];
    if (static_cast<int>(i) == hover_index_) {
      Gdiplus::SolidBrush glow(Gdiplus::Color(90, 120, 170, 255));
      g.FillRectangle(&glow, s.bounds.left - 4, s.bounds.top - 4, kBtn, kBtn);
    }
    if (s.icon) {
      DrawIconEx(hdc, s.bounds.left, s.bounds.top, s.icon, kIcon, kIcon, 0,
                 nullptr, DI_NORMAL);
    }
  }
}

void DockWindow::Present() {
  if (!hwnd_) {
    return;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  const int w = (std::max)(1, static_cast<int>(wr.right - wr.left));
  const int h = (std::max)(1, static_cast<int>(wr.bottom - wr.top));
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
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
