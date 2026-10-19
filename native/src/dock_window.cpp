#include "dock_window.h"

#include "desktop_shell.h"

#include <Shellapi.h>
#include <windowsx.h>

#include <algorithm>

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeDock";
constexpr int kH = 64;
constexpr int kIcon = 36;
constexpr int kPad = 12;
bool g_registered = false;
}  // namespace

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

  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  const int w = 420;
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName, L"Dock",
      WS_POPUP, (sw - w) / 2, sh - kH - 48, w, kH, nullptr, nullptr, instance,
      this);
  if (!hwnd_) {
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 220, LWA_ALPHA);
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
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void DockWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachToDesktopLayer();
}

void DockWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void DockWindow::SetVisible(bool on) {
  if (on) {
    Show();
  } else {
    Hide();
  }
}

void DockWindow::AttachToDesktopLayer() {
  if (!hwnd_) {
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
  const size_t n = (std::min)(items.size(), size_t{8});
  slots_.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    Slot s;
    s.item = std::move(items[i]);
    s.icon = LoadItemIcon(s.item.path, kIcon);
    slots_.push_back(std::move(s));
  }
  LayoutSlots();
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
}

void DockWindow::LayoutSlots() {
  if (!hwnd_) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int n = static_cast<int>(slots_.size());
  if (n <= 0) {
    return;
  }
  const int total = n * (kIcon + kPad);
  int x = (rc.right - total) / 2;
  for (auto& s : slots_) {
    s.bounds = {x, (rc.bottom - kIcon) / 2, x + kIcon, (rc.bottom - kIcon) / 2 + kIcon};
    x += kIcon + kPad;
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
      HDC hdc = BeginPaint(hwnd_, &ps);
      Paint(hdc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_LBUTTONUP: {
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0) {
        ShellExecuteW(nullptr, L"open", slots_[hit].item.path.c_str(), nullptr,
                      nullptr, SW_SHOWNORMAL);
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

void DockWindow::Paint(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  HBRUSH bg = CreateSolidBrush(RGB(28, 30, 36));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 78, 92));
  HGDIOBJ old = SelectObject(hdc, pen);
  SelectObject(hdc, GetStockObject(NULL_BRUSH));
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 18, 18);
  SelectObject(hdc, old);
  DeleteObject(pen);
  for (const auto& s : slots_) {
    if (s.icon) {
      DrawIconEx(hdc, s.bounds.left, s.bounds.top, s.icon, kIcon, kIcon, 0,
                 nullptr, DI_NORMAL);
    }
  }
}
