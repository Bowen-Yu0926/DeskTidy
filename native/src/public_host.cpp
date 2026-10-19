#include "public_host.h"

#include "desktop_shell.h"

#include <Shellapi.h>
#include <windowsx.h>

#include <algorithm>

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativePublic";
constexpr int kIcon = 32;
constexpr int kPad = 8;
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
  const int sw = GetSystemMetrics(SM_CXSCREEN);
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClass, L"Public",
      WS_POPUP, sw - 220, 20, 200, 56, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 230, LWA_ALPHA);
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

void PublicHost::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachToDesktopLayer();
}

void PublicHost::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void PublicHost::SetVisible(bool on) {
  if (on) {
    Show();
  } else {
    Hide();
  }
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
  const size_t n = (std::min)(items.size(), size_t{6});
  for (size_t i = 0; i < n; ++i) {
    Slot s;
    s.item = std::move(items[i]);
    s.icon = LoadItemIcon(s.item.path, kIcon);
    slots_.push_back(std::move(s));
  }
  Layout();
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
}

void PublicHost::Layout() {
  int x = kPad;
  for (auto& s : slots_) {
    s.bounds = {x, 10, x + kIcon, 10 + kIcon};
    x += kIcon + kPad;
  }
  if (hwnd_ && !slots_.empty()) {
    SetWindowPos(hwnd_, nullptr, 0, 0, x + kPad, 52,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
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

void PublicHost::Paint(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  HBRUSH bg = CreateSolidBrush(RGB(36, 40, 48));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);
  for (const auto& s : slots_) {
    if (s.icon) {
      DrawIconEx(hdc, s.bounds.left, s.bounds.top, s.icon, kIcon, kIcon, 0,
                 nullptr, DI_NORMAL);
    }
  }
}
