#include "page_bar.h"

#include "desktop_shell.h"

#include <windowsx.h>

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativePageBar";
constexpr int kW = 120;
constexpr int kH = 36;
bool g_registered = false;
}  // namespace

bool PageBar::Create(HINSTANCE instance) {
  if (!g_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PageBar::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    g_registered = true;
  }
  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName,
      L"Pages", WS_POPUP, (sw - kW) / 2, sh - kH - 12, kW, kH, nullptr, nullptr,
      instance, this);
  if (!hwnd_) {
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 230, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void PageBar::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void PageBar::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachToDesktopLayer();
}

void PageBar::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void PageBar::SetPage(int page) {
  page_ = page;
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
}

void PageBar::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

LRESULT CALLBACK PageBar::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam) {
  PageBar* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<PageBar*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<PageBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT PageBar::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd_, &ps);
      Paint(hdc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_LBUTTONUP: {
      const int x = GET_X_LPARAM(lparam);
      RECT rc{};
      GetClientRect(hwnd_, &rc);
      const int mid = rc.right / 2;
      const int next = (x < mid) ? 0 : 1;
      if (on_page_) {
        on_page_(next);
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

void PageBar::Paint(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);
  SetBkMode(hdc, TRANSPARENT);
  auto draw_tab = [&](int idx, RECT r) {
    const bool on = page_ == idx;
    HBRUSH b = CreateSolidBrush(on ? RGB(70, 130, 200) : RGB(45, 50, 60));
    FillRect(hdc, &r, b);
    DeleteObject(b);
    SetTextColor(hdc, RGB(230, 230, 230));
    wchar_t label[8];
    swprintf_s(label, L"%d", idx + 1);
    DrawTextW(hdc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  };
  RECT left = rc;
  left.right = rc.right / 2 - 2;
  left.left += 4;
  left.top += 4;
  left.bottom -= 4;
  RECT right = rc;
  right.left = rc.right / 2 + 2;
  right.right -= 4;
  right.top += 4;
  right.bottom -= 4;
  draw_tab(0, left);
  draw_tab(1, right);
}
