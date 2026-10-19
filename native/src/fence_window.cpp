#include "fence_window.h"

#include "desktop_shell.h"

#include <windowsx.h>

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeFence";
constexpr COLORREF kPanelBg = RGB(32, 36, 44);
constexpr COLORREF kPanelBorder = RGB(70, 78, 92);
bool g_class_registered = false;
}  // namespace

bool FenceWindow::Create(HINSTANCE instance, const FenceLayout& layout) {
  id_ = layout.id;
  title_ = layout.title;

  if (!g_class_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
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
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName,
      title_.c_str(), WS_POPUP, layout.x, layout.y, layout.w, layout.h, nullptr,
      nullptr, instance, this);

  if (!hwnd_) {
    return false;
  }

  SetLayeredWindowAttributes(hwnd_, 0, 230, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void FenceWindow::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void FenceWindow::SetTitle(const std::wstring& title) {
  title_ = title;
  if (hwnd_) {
    SetWindowTextW(hwnd_, title_.c_str());
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
}

FenceLayout FenceWindow::CurrentLayout() const {
  FenceLayout fl;
  fl.id = id_;
  fl.title = title_;
  if (hwnd_) {
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    fl.x = rc.left;
    fl.y = rc.top;
    fl.w = rc.right - rc.left;
    fl.h = rc.bottom - rc.top;
  }
  return fl;
}

void FenceWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachToDesktopLayer();
}

void FenceWindow::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
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
    self = reinterpret_cast<FenceWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT FenceWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd_, &ps);
      Paint(hdc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      dragging_ = true;
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ClientToScreen(hwnd_, &pt);
      RECT rc{};
      GetWindowRect(hwnd_, &rc);
      drag_offset_.x = pt.x - rc.left;
      drag_offset_.y = pt.y - rc.top;
      SetCapture(hwnd_);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!dragging_ || !(wparam & MK_LBUTTON)) {
        return 0;
      }
      POINT pt{};
      GetCursorPos(&pt);
      SetWindowPos(hwnd_, nullptr, pt.x - drag_offset_.x, pt.y - drag_offset_.y, 0,
                   0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_LBUTTONUP: {
      if (dragging_) {
        dragging_ = false;
        ReleaseCapture();
        AttachToDesktopLayer();
        if (on_moved_) {
          on_moved_(this);
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

void FenceWindow::Paint(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  HBRUSH bg = CreateSolidBrush(kPanelBg);
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);

  HPEN pen = CreatePen(PS_SOLID, 1, kPanelBorder);
  HGDIOBJ old_pen = SelectObject(hdc, pen);
  HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 16, 16);
  SelectObject(hdc, old_pen);
  SelectObject(hdc, old_brush);
  DeleteObject(pen);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(220, 224, 230));
  RECT text = rc;
  text.top += 12;
  DrawTextW(hdc, title_.c_str(), -1, &text, DT_CENTER | DT_TOP | DT_SINGLELINE);

  RECT hint = rc;
  hint.top = rc.bottom / 2 - 10;
  DrawTextW(hdc, L"Native fence (Fences layer)", -1, &hint,
            DT_CENTER | DT_TOP | DT_SINGLELINE);
}
