#include "page_bar.h"

#include "desktop_shell.h"
#include "display_geom.h"

#include <windowsx.h>

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativePageBar";
constexpr int kH = 36;
constexpr int kSlot = 36;
// After page buttons: add, remove, notes, search, shot.
constexpr int kTools = 5;
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
  const int w = kSlot * (page_count_ + kTools);
  const int x = work.left + (work.right - work.left - w) / 2;
  const int y = work.bottom - kH - 4;
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName,
      L"Pages", WS_POPUP, x, y, w, kH, nullptr, nullptr, instance, this);
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

void PageBar::SetPageCount(int count) {
  if (count < 1) {
    count = 1;
  }
  if (count > 12) {
    count = 12;
  }
  page_count_ = count;
  if (!hwnd_) {
    return;
  }
  const RECT work = PrimaryWorkArea();
  const int w = kSlot * (page_count_ + kTools);
  const int x = work.left + (work.right - work.left - w) / 2;
  const int y = work.bottom - kH - 4;
  SetWindowPos(hwnd_, nullptr, x, y, w, kH, SWP_NOZORDER | SWP_NOACTIVATE);
  InvalidateRect(hwnd_, nullptr, TRUE);
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
  int i = x / kSlot;
  const int n = page_count_ + kTools;
  if (i < 0) {
    i = 0;
  }
  if (i >= n) {
    i = n - 1;
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
      if (hit >= 0 && hit < page_count_ && on_page_) {
        on_page_(hit);
      } else if (hit == page_count_ && on_add_page_) {
        on_add_page_();
      } else if (hit == page_count_ + 1 && on_remove_page_) {
        on_remove_page_();
      } else if (hit == page_count_ + 2 && on_notes_) {
        on_notes_();
      } else if (hit == page_count_ + 3 && on_search_) {
        on_search_();
      } else if (hit == page_count_ + 4 && on_capture_) {
        on_capture_();
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      const int hit = HitTest(GET_X_LPARAM(lparam));
      if (hit >= 0 && hit < page_count_ && on_page_) {
        on_page_(hit);
        if (on_rename_page_) {
          on_rename_page_();
        }
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

  const int n = page_count_ + kTools;
  for (int i = 0; i < n; ++i) {
    RECT r{i * kSlot + 3, 4, (i + 1) * kSlot - 3, rc.bottom - 4};
    const bool on = i < page_count_ && page_ == i;
    HBRUSH b = CreateSolidBrush(on ? RGB(70, 130, 200) : RGB(45, 50, 60));
    FillRect(hdc, &r, b);
    DeleteObject(b);
    wchar_t label[8] = L"?";
    if (i < page_count_) {
      swprintf_s(label, L"%d", i + 1);
    } else if (i == page_count_) {
      wcscpy_s(label, L"+");
    } else if (i == page_count_ + 1) {
      wcscpy_s(label, L"-");
    } else if (i == page_count_ + 2) {
      wcscpy_s(label, L"记");
    } else if (i == page_count_ + 3) {
      wcscpy_s(label, L"搜");
    } else {
      wcscpy_s(label, L"截");
    }
    SetTextColor(hdc, RGB(230, 230, 230));
    DrawTextW(hdc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  (void)rc;
}
