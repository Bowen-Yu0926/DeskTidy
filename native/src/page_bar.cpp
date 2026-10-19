#include "page_bar.h"

#include "desktop_shell.h"
#include "display_geom.h"

#include <windowsx.h>

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativePageBar";
constexpr int kW = 280;
constexpr int kH = 36;
// Slots: [0]=page1 [1]=page2 [2]=notes [3]=search [4]=shot
constexpr int kSlots = 5;
bool g_registered = false;
}  // namespace

bool PageBar::Create(HINSTANCE instance) {
  if (!g_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PageBar::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    g_registered = true;
  }
  const RECT work = PrimaryWorkArea();
  const int x = work.left + (work.right - work.left - kW) / 2;
  const int y = work.bottom - kH - 4;
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName,
      L"Pages", WS_POPUP, x, y, kW, kH, nullptr, nullptr, instance, this);
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

int PageBar::HitTest(int x) const {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int slot = rc.right / kSlots;
  if (slot <= 0) {
    return -1;
  }
  int i = x / slot;
  if (i < 0) {
    i = 0;
  }
  if (i >= kSlots) {
    i = kSlots - 1;
  }
  return i;
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
      const int hit = HitTest(GET_X_LPARAM(lparam));
      if (hit == 0 && on_page_) {
        on_page_(0);
      } else if (hit == 1 && on_page_) {
        on_page_(1);
      } else if (hit == 2 && on_notes_) {
        on_notes_();
      } else if (hit == 3 && on_search_) {
        on_search_();
      } else if (hit == 4 && on_capture_) {
        on_capture_();
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

  const wchar_t* labels[kSlots] = {L"1", L"2", L"记", L"搜", L"截"};
  const int slot = rc.right / kSlots;
  for (int i = 0; i < kSlots; ++i) {
    RECT r{i * slot + 3, 4, (i + 1) * slot - 3, rc.bottom - 4};
    const bool on = (i < 2) && (page_ == i);
    HBRUSH b = CreateSolidBrush(on ? RGB(70, 130, 200) : RGB(45, 50, 60));
    FillRect(hdc, &r, b);
    DeleteObject(b);
    SetTextColor(hdc, RGB(230, 230, 230));
    DrawTextW(hdc, labels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
}
