#include "public_host.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "icon_actions.h"

#include <Shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativePublic";
// Windows-like grid (public_desktop.py): column-major from the left margin.
constexpr int kIcon = 48;
constexpr int kCellW = 76;
constexpr int kCellH = 76;
constexpr int kMargin = 20;
constexpr int kGapX = 0;
constexpr int kGapY = 6;
bool g_reg = false;
}  // namespace

bool PublicHost::Create(HINSTANCE instance) {
  if (!g_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PublicHost::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_reg = true;
  }
  const RECT work = PrimaryWorkArea();
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClass, L"Public",
      WS_POPUP, work.left + kMargin, work.top + kMargin, kCellW, kCellH,
      nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void PublicHost::Destroy() {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void PublicHost::Show() { SetVisible(true); }

void PublicHost::Hide() { SetVisible(false); }

void PublicHost::ApplyVisibility() {
  if (!hwnd_) {
    return;
  }
  if (!want_visible_ || slots_.empty()) {
    ShowWindow(hwnd_, SW_HIDE);
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachToDesktopLayer();
  Present();
}

void PublicHost::SetVisible(bool on) {
  want_visible_ = on;
  ApplyVisibility();
}

void PublicHost::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

void PublicHost::SetItems(std::vector<DesktopItem> items) {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
  slots_.reserve(items.size());
  for (auto& item : items) {
    if (GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      continue;
    }
    Slot s;
    s.item = std::move(item);
    s.icon = LoadItemIcon(s.item.path, kIcon);
    slots_.push_back(std::move(s));
  }
  Layout();
  ApplyVisibility();
}

void PublicHost::Layout() {
  if (!hwnd_ || slots_.empty()) {
    return;
  }
  const RECT work = PrimaryWorkArea();
  const int avail_h =
      (std::max)(1, static_cast<int>(work.bottom - work.top) - 2 * kMargin);
  const int per_col = (std::max)(1, avail_h / (kCellH + kGapY));
  const int n = static_cast<int>(slots_.size());
  const int cols = (n + per_col - 1) / per_col;
  const int w = cols * kCellW + (std::max)(0, cols - 1) * kGapX;
  const int h = per_col * kCellH + (std::max)(0, per_col - 1) * kGapY;
  SetWindowPos(hwnd_, nullptr, work.left + kMargin, work.top + kMargin, w, h,
               SWP_NOZORDER | SWP_NOACTIVATE);
  const int icon_pad = (kCellW - kIcon) / 2;
  for (int i = 0; i < n; ++i) {
    const int col = i / per_col;
    const int row = i % per_col;
    const int cx = col * (kCellW + kGapX);
    const int cy = row * (kCellH + kGapY);
    slots_[static_cast<size_t>(i)].bounds = {
        cx + icon_pad, cy + 8, cx + icon_pad + kIcon, cy + 8 + kIcon};
  }
  Present();
}

void PublicHost::ShowSlotMenu(int index, POINT screen) {
  if (index < 0 || index >= static_cast<int>(slots_.size())) {
    return;
  }
  const std::wstring path = slots_[static_cast<size_t>(index)].item.path;
  // Matches fence RMB: 打开 / 剪切 / 复制 / 删除 / 属性 / 刷新.
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1, L"打开");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 5, L"剪切");
  AppendMenuW(menu, MF_STRING, 4, L"复制");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 9, L"删除");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 8, L"属性");
  AppendMenuW(menu, MF_STRING, 10, L"刷新");
  SetForegroundWindow(hwnd_);
  const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x,
                                 screen.y, 0, hwnd_, nullptr);
  DestroyMenu(menu);
  switch (cmd) {
    case 1:
      OpenShellPath(path);
      break;
    case 4:
      CopyOrCutShellPath(hwnd_, path, false);
      break;
    case 5:
      CopyOrCutShellPath(hwnd_, path, true);
      break;
    case 8:
      ShowShellPathProperties(hwnd_, path);
      break;
    case 9:
      if (RecycleShellPath(hwnd_, path) == IconFsResult::Ok && on_removed_) {
        on_removed_(path);
      }
      break;
    case 10:
      if (on_refresh_) {
        on_refresh_();
      }
      break;
    default:
      break;
  }
}

int PublicHost::HitTest(int x, int y) const {
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& b = slots_[i].bounds;
    if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

LRESULT CALLBACK PublicHost::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  PublicHost* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<PublicHost*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<PublicHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT PublicHost::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      Present();
      return 0;
    }
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
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void PublicHost::Paint(HDC hdc) {
  for (const auto& s : slots_) {
    if (s.icon) {
      DrawIconEx(hdc, s.bounds.left, s.bounds.top, s.icon, kIcon, kIcon, 0,
                 nullptr, DI_NORMAL);
    }
  }
}

void PublicHost::Present() {
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
  // Zero alpha → transparent; only icon pixels are hit-testable.
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
