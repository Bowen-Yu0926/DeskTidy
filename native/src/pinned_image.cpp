#include "pinned_image.h"

#include "desktop_shell.h"

#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativePinImg";
bool g_reg = false;
}  // namespace

bool PinnedImage::Create(HINSTANCE instance, const std::wstring& path) {
  if (!g_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PinnedImage::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_reg = true;
  }

  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartup(&token, &input, nullptr);
  Gdiplus::Bitmap src(path.c_str());
  if (src.GetLastStatus() != Gdiplus::Ok) {
    Gdiplus::GdiplusShutdown(token);
    return false;
  }
  bmp_w_ = static_cast<int>(src.GetWidth());
  bmp_h_ = static_cast<int>(src.GetHeight());
  if (bmp_w_ > 640) {
    bmp_h_ = bmp_h_ * 640 / bmp_w_;
    bmp_w_ = 640;
  }
  Gdiplus::Bitmap scaled(bmp_w_, bmp_h_);
  Gdiplus::Graphics g(&scaled);
  g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  g.DrawImage(&src, 0, 0, bmp_w_, bmp_h_);
  HBITMAP hbmp = nullptr;
  scaled.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hbmp);
  bmp_ = hbmp;
  Gdiplus::GdiplusShutdown(token);
  if (!bmp_) {
    return false;
  }

  const int sw = GetSystemMetrics(SM_CXSCREEN);
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"贴图",
      WS_POPUP, (sw - bmp_w_) / 2, 80, bmp_w_, bmp_h_, nullptr, nullptr,
      instance, this);
  if (!hwnd_) {
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void PinnedImage::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (bmp_) {
    DeleteObject(bmp_);
    bmp_ = nullptr;
  }
}

void PinnedImage::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

LRESULT CALLBACK PinnedImage::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
  PinnedImage* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<PinnedImage*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<PinnedImage*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT PinnedImage::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
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
      drag_offset_ = {pt.x - rc.left, pt.y - rc.top};
      SetCapture(hwnd_);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (dragging_ && (wparam & MK_LBUTTON)) {
        POINT pt{};
        GetCursorPos(&pt);
        SetWindowPos(hwnd_, nullptr, pt.x - drag_offset_.x,
                     pt.y - drag_offset_.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      }
      return 0;
    case WM_LBUTTONUP:
      dragging_ = false;
      ReleaseCapture();
      AttachOverlayToDesktop(hwnd_, true);
      PlaceOverlayInDesktopBand(hwnd_, true);
      return 0;
    case WM_RBUTTONUP:
      Destroy();
      return 0;
    case WM_DESTROY:
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void PinnedImage::Paint(HDC hdc) {
  if (!bmp_) {
    return;
  }
  HDC mem = CreateCompatibleDC(hdc);
  HGDIOBJ old = SelectObject(mem, bmp_);
  BitBlt(hdc, 0, 0, bmp_w_, bmp_h_, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old);
  DeleteDC(mem);
}
