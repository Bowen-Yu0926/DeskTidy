#include "vault_launcher.h"

#include "desktop_shell.h"
#include "display_geom.h"

#include <Windowsx.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstring>
#include <functional>

#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeVaultBuoy";
constexpr int kBaseSize = 56;  // design px @ 96 DPI (match Python)
constexpr int kDragThreshold = 6;

ULONG_PTR g_gdiplus_token = 0;

void EnsureGdiplus() {
  if (g_gdiplus_token) {
    return;
  }
  Gdiplus::GdiplusStartupInput input;
  Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr);
}

UINT WindowDpi(HWND hwnd) {
  using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
  static GetDpiForWindowFn fn = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    fn = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  }
  if (fn && hwnd) {
    const UINT dpi = fn(hwnd);
    if (dpi > 0) {
      return dpi;
    }
  }
  using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
  static GetDpiForMonitorFn mon_fn = nullptr;
  static bool mon_tried = false;
  if (!mon_tried) {
    mon_tried = true;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
      mon_fn = reinterpret_cast<GetDpiForMonitorFn>(
          GetProcAddress(shcore, "GetDpiForMonitor"));
    }
  }
  HMONITOR mon = MonitorFromWindow(hwnd ? hwnd : GetDesktopWindow(),
                                   MONITOR_DEFAULTTONEAREST);
  if (mon_fn && mon) {
    UINT dx = 96, dy = 96;
    if (SUCCEEDED(mon_fn(mon, 0, &dx, &dy)) && dx > 0) {
      return dx;
    }
  }
  return 96;
}

bool PresentLayered(HWND hwnd,
                    std::function<void(Gdiplus::Graphics&, int, int)> paint) {
  if (!hwnd) {
    return false;
  }
  RECT wr{};
  GetWindowRect(hwnd, &wr);
  const int w = (std::max)(1, static_cast<int>(wr.right - wr.left));
  const int h = (std::max)(1, static_cast<int>(wr.bottom - wr.top));
  EnsureGdiplus();
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
    return false;
  }
  HGDIOBJ old = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  {
    Gdiplus::Graphics g(mem);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    paint(g, w, h);
  }
  POINT origin{wr.left, wr.top};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateOverlayLayered(hwnd, screen, &origin, &size, mem, &src, 0, &blend,
                      ULW_ALPHA);
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return true;
}
}  // namespace

int VaultLauncher::BuoySize() const {
  const UINT dpi = WindowDpi(hwnd_);
  return (std::max)(40, MulDiv(kBaseSize, static_cast<int>(dpi), 96));
}

void VaultLauncher::ApplySize() {
  if (!hwnd_) {
    return;
  }
  const int sz = BuoySize();
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  SetOverlayScreenPos(hwnd_, rc.left, rc.top, sz, sz, SWP_NOZORDER);
}

bool VaultLauncher::Create(HINSTANCE instance) {
  instance_ = instance;
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = VaultLauncher::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    registered = true;
  }
  EnsureGdiplus();
  const RECT work = PrimaryWorkArea();
  const int x = work.right - kBaseSize - 24;
  const int y = work.bottom - kBaseSize - 96;
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClass, L"VaultBuoy",
      WS_POPUP, x, y, kBaseSize, kBaseSize, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  AttachOverlayToDesktop(hwnd_, false);
  PlaceOverlayInDesktopBand(hwnd_, true);
  ApplySize();
  Present();
  return true;
}

void VaultLauncher::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void VaultLauncher::Show() {
  visible_ = true;
  if (!hwnd_) {
    return;
  }
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  ApplySize();
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  Present();
}

void VaultLauncher::Hide() {
  visible_ = false;
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void VaultLauncher::SetVisible(bool on) {
  if (on) {
    Show();
  } else {
    Hide();
  }
}

void VaultLauncher::SetPosition(int x, int y) {
  if (!hwnd_) {
    return;
  }
  const int sz = BuoySize();
  if (x < 0 || y < 0) {
    const RECT work = PrimaryWorkArea();
    x = work.right - sz - 24;
    y = work.bottom - sz - 96;
  }
  const POINT p = ClampPos(x, y);
  MoveOverlayScreen(hwnd_, p.x, p.y);
  Present();
}

void VaultLauncher::GetPosition(int* x, int* y) const {
  if (!hwnd_ || !x || !y) {
    return;
  }
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  *x = rc.left;
  *y = rc.top;
}

POINT VaultLauncher::ClampPos(int x, int y) const {
  const RECT work = PrimaryWorkArea();
  const int sz = BuoySize();
  if (x < work.left) {
    x = work.left;
  }
  if (y < work.top) {
    y = work.top;
  }
  if (x + sz > work.right) {
    x = work.right - sz;
  }
  if (y + sz > work.bottom) {
    y = work.bottom - sz;
  }
  return {x, y};
}

void VaultLauncher::Present() {
  if (!hwnd_) {
    return;
  }
  const bool hover = hover_;
  PresentLayered(hwnd_, [&](Gdiplus::Graphics& g, int w, int h) {
    const float margin = (std::max)(2.5f, static_cast<float>(w) * 0.045f);
    Gdiplus::RectF oval(margin, margin, static_cast<float>(w) - margin * 2.f,
                        static_cast<float>(h) - margin * 2.f);
    const BYTE fill_a = hover ? 235 : 210;
    const BYTE rim_a = hover ? 210 : 160;
    Gdiplus::SolidBrush fill(Gdiplus::Color(fill_a, 29, 78, 216));
    g.FillEllipse(&fill, oval);
    Gdiplus::Pen rim(Gdiplus::Color(rim_a, 255, 255, 255),
                     (std::max)(1.5f, static_cast<float>(w) / 28.f));
    g.DrawEllipse(&rim, oval);

    Gdiplus::FontFamily family(L"Microsoft YaHei UI");
    const Gdiplus::FontFamily* face =
        family.IsAvailable() ? &family : Gdiplus::FontFamily::GenericSansSerif();
    const float em = (std::max)(11.f, static_cast<float>(w) * 13.f / 56.f);
    Gdiplus::Font font(face, em, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush ink(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(L"账", -1, &font,
                 Gdiplus::RectF(0.f, 0.f, static_cast<float>(w),
                                static_cast<float>(h)),
                 &fmt, &ink);
  });
}

LRESULT CALLBACK VaultLauncher::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                        LPARAM lparam) {
  VaultLauncher* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<VaultLauncher*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<VaultLauncher*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT VaultLauncher::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_LBUTTONDOWN:
      SetCapture(hwnd_);
      press_.x = GET_X_LPARAM(lparam);
      press_.y = GET_Y_LPARAM(lparam);
      {
        POINT screen = press_;
        ClientToScreen(hwnd_, &screen);
        RECT rc{};
        GetWindowRect(hwnd_, &rc);
        drag_offset_.x = screen.x - rc.left;
        drag_offset_.y = screen.y - rc.top;
      }
      dragging_ = false;
      return 0;
    case WM_MOUSEMOVE: {
      if (!(wparam & MK_LBUTTON)) {
        if (!hover_) {
          hover_ = true;
          Present();
        }
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        TrackMouseEvent(&tme);
      }
      if ((wparam & MK_LBUTTON) && hwnd_ == GetCapture()) {
        POINT cur{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (!dragging_) {
          if (abs(cur.x - press_.x) >= kDragThreshold ||
              abs(cur.y - press_.y) >= kDragThreshold) {
            dragging_ = true;
          }
        }
        if (dragging_) {
          POINT screen = cur;
          ClientToScreen(hwnd_, &screen);
          const POINT p =
              ClampPos(screen.x - drag_offset_.x, screen.y - drag_offset_.y);
          MoveOverlayScreen(hwnd_, p.x, p.y);
          Present();
        }
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      hover_ = false;
      Present();
      return 0;
    case WM_LBUTTONUP:
      ReleaseCapture();
      if (dragging_) {
        dragging_ = false;
        if (on_moved_) {
          int x = 0, y = 0;
          GetPosition(&x, &y);
          on_moved_(x, y);
        }
      } else if (on_activate_) {
        on_activate_();
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      Present();
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_DPICHANGED: {
      const RECT* suggested = reinterpret_cast<RECT*>(lparam);
      if (suggested) {
        const int sz = BuoySize();
        SetOverlayScreenPos(hwnd_, suggested->left, suggested->top, sz, sz, SWP_NOZORDER);
      } else {
        ApplySize();
      }
      Present();
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
