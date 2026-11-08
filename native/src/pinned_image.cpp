#include "pinned_image.h"

#include "i18n.h"
#include "mist_msg.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativePinImg";
constexpr int kCloseSize = 24;
bool g_reg = false;

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
  UINT num = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&num, &size);
  if (!size) {
    return -1;
  }
  auto* info = static_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
  if (!info) {
    return -1;
  }
  Gdiplus::GetImageEncoders(num, size, info);
  for (UINT i = 0; i < num; ++i) {
    if (wcscmp(info[i].MimeType, format) == 0) {
      *pClsid = info[i].Clsid;
      free(info);
      return static_cast<int>(i);
    }
  }
  free(info);
  return -1;
}

void RegisterPinClass(HINSTANCE instance) {
  if (g_reg) return;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = PinnedImage::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
  g_reg = true;
}
}  // namespace

PinnedImage::PinnedImage() = default;

PinnedImage::~PinnedImage() {
  Destroy();
}

bool PinnedImage::Create(HINSTANCE instance, const std::wstring& path) {
  RegisterPinClass(instance);
  path_ = path;

  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartup(&token, &input, nullptr);
  Gdiplus::Bitmap src(path.c_str());
  if (src.GetLastStatus() != Gdiplus::Ok) {
    Gdiplus::GdiplusShutdown(token);
    return false;
  }
  orig_w_ = static_cast<int>(src.GetWidth());
  orig_h_ = static_cast<int>(src.GetHeight());
  HBITMAP hbmp = nullptr;
  src.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hbmp);
  bmp_ = hbmp;
  Gdiplus::GdiplusShutdown(token);

  if (!bmp_ || orig_w_ <= 0 || orig_h_ <= 0) {
    return false;
  }

  zoom_ = 1.0f;
  alpha_ = 255;

  static int g_stack = 0;
  const int stagger = (g_stack++ % 8) * 28;
  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  int init_w = orig_w_;
  int init_h = orig_h_;

  // 若图片大于屏幕 80%，初次贴图适度等比缩放到 80% 以免遮挡全屏
  if (init_w > sw * 0.8f || init_h > sh * 0.8f) {
    const float scale_w = (sw * 0.8f) / init_w;
    const float scale_h = (sh * 0.8f) / init_h;
    zoom_ = (std::min)(scale_w, scale_h);
    init_w = (std::max)(64, static_cast<int>(orig_w_ * zoom_));
    init_h = (std::max)(64, static_cast<int>(orig_h_ * zoom_));
  }

  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST, kClass, L"贴图",
      WS_POPUP, (sw - init_w) / 2 + stagger, (sh - init_h) / 3 + stagger,
      init_w, init_h, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  ApplyAlpha();
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

bool PinnedImage::CreateFromHBitmap(HINSTANCE instance, HBITMAP hbmp,
                                     const std::wstring& cache_path) {
  RegisterPinClass(instance);
  path_ = cache_path;

  if (!hbmp) return false;
  BITMAP bm{};
  if (!GetObjectW(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
    return false;
  }

  orig_w_ = bm.bmWidth;
  orig_h_ = bm.bmHeight;
  bmp_ = static_cast<HBITMAP>(CopyImage(hbmp, IMAGE_BITMAP, 0, 0, LR_COPYRETURNORG));
  if (!bmp_) return false;

  zoom_ = 1.0f;
  alpha_ = 255;

  static int g_stack = 0;
  const int stagger = (g_stack++ % 8) * 28;
  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  int init_w = orig_w_;
  int init_h = orig_h_;

  if (init_w > sw * 0.8f || init_h > sh * 0.8f) {
    const float scale_w = (sw * 0.8f) / init_w;
    const float scale_h = (sh * 0.8f) / init_h;
    zoom_ = (std::min)(scale_w, scale_h);
    init_w = (std::max)(64, static_cast<int>(orig_w_ * zoom_));
    init_h = (std::max)(64, static_cast<int>(orig_h_ * zoom_));
  }

  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST, kClass, L"贴图",
      WS_POPUP, (sw - init_w) / 2 + stagger, (sh - init_h) / 3 + stagger,
      init_w, init_h, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  ApplyAlpha();
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

void PinnedImage::ApplyAlpha() {
  if (hwnd_) {
    SetLayeredWindowAttributes(hwnd_, 0, alpha_, LWA_ALPHA);
  }
}

void PinnedImage::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOW);
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetForegroundWindow(hwnd_);
  SetFocus(hwnd_);
}

void PinnedImage::Zoom(float factor, POINT anchor_screen) {
  if (!hwnd_ || orig_w_ <= 0 || orig_h_ <= 0) return;
  float next_zoom = zoom_ * factor;
  if (next_zoom < 0.15f) next_zoom = 0.15f;
  if (next_zoom > 6.0f) next_zoom = 6.0f;
  if (std::abs(next_zoom - zoom_) < 0.002f) return;

  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  const int old_w = (std::max)(1, static_cast<int>(rc.right - rc.left));
  const int old_h = (std::max)(1, static_cast<int>(rc.bottom - rc.top));

  const int new_w = (std::max)(40, static_cast<int>(orig_w_ * next_zoom));
  const int new_h = (std::max)(40, static_cast<int>(orig_h_ * next_zoom));

  // 以鼠标在屏幕上的物理坐标为不动点锚点进行缩放，视觉体验极其平滑自然
  const float rx = static_cast<float>(anchor_screen.x - rc.left) / old_w;
  const float ry = static_cast<float>(anchor_screen.y - rc.top) / old_h;

  const int new_x = anchor_screen.x - static_cast<int>(new_w * rx);
  const int new_y = anchor_screen.y - static_cast<int>(new_h * ry);

  zoom_ = next_zoom;
  SetWindowPos(hwnd_, HWND_TOPMOST, new_x, new_y, new_w, new_h,
               SWP_NOACTIVATE);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void PinnedImage::ResetZoom() {
  if (!hwnd_ || orig_w_ <= 0 || orig_h_ <= 0) return;
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  POINT center{rc.left + (rc.right - rc.left) / 2, rc.top + (rc.bottom - rc.top) / 2};
  const float factor = 1.0f / zoom_;
  Zoom(factor, center);
}

void PinnedImage::SaveAs() {
  wchar_t file[MAX_PATH] = L"贴图_截图.png";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"PNG 图片\0*.png\0JPEG 图片\0*.jpg;*.jpeg\0BMP 图片\0*.bmp\0";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  ofn.lpstrDefExt = L"png";

  SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  const BOOL ok = GetSaveFileNameW(&ofn);
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  if (!ok) return;

  const std::wstring dest = file;
  const size_t dot = dest.find_last_of(L'.');
  std::wstring ext = (dot != std::wstring::npos) ? dest.substr(dot) : L"";
  for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
  const wchar_t* mime = L"image/png";
  if (ext == L".jpg" || ext == L".jpeg") {
    mime = L"image/jpeg";
  } else if (ext == L".bmp") {
    mime = L"image/bmp";
  }

  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartupInput input;
  Gdiplus::GdiplusStartup(&token, &input, nullptr);
  {
    CLSID clsid{};
    if (GetEncoderClsid(mime, &clsid) >= 0 && bmp_) {
      Gdiplus::Bitmap gbmp(bmp_, nullptr);
      gbmp.Save(dest.c_str(), &clsid, nullptr);
    }
  }
  Gdiplus::GdiplusShutdown(token);
}

void PinnedImage::CopyToClipboard() {
  if (!bmp_ || !OpenClipboard(hwnd_)) return;
  EmptyClipboard();
  HBITMAP copy = static_cast<HBITMAP>(CopyImage(bmp_, IMAGE_BITMAP, 0, 0, LR_COPYRETURNORG));
  if (copy) {
    SetClipboardData(CF_BITMAP, copy);
  }
  CloseClipboard();
}

void PinnedImage::ShowContextMenu(POINT screen) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1, L"另存为... (Ctrl+S)");
  AppendMenuW(menu, MF_STRING, 2, L"复制图片 (Ctrl+C)");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 3, L"100% 原始大小 (Ctrl+0)");
  AppendMenuW(menu, MF_STRING, 4, L"放大 (滚轮向上)");
  AppendMenuW(menu, MF_STRING, 5, L"缩小 (滚轮向下)");

  HMENU opa = CreatePopupMenu();
  AppendMenuW(opa, MF_STRING | (alpha_ == 255 ? MF_CHECKED : 0), 10, L"100% (不透明)");
  AppendMenuW(opa, MF_STRING | (alpha_ == 200 ? MF_CHECKED : 0), 11, L"80%");
  AppendMenuW(opa, MF_STRING | (alpha_ == 150 ? MF_CHECKED : 0), 12, L"60%");
  AppendMenuW(opa, MF_STRING | (alpha_ == 100 ? MF_CHECKED : 0), 13, L"40%");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opa), L"透明度 (Ctrl+滚轮)");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 9, L"关闭贴图 (Esc)");

  SetForegroundWindow(hwnd_);
  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y,
                     0, hwnd_, nullptr);
  DestroyMenu(menu);

  if (cmd == 1) {
    SaveAs();
  } else if (cmd == 2) {
    CopyToClipboard();
  } else if (cmd == 3) {
    ResetZoom();
  } else if (cmd == 4) {
    Zoom(1.15f, screen);
  } else if (cmd == 5) {
    Zoom(0.85f, screen);
  } else if (cmd == 10) {
    alpha_ = 255;
    ApplyAlpha();
  } else if (cmd == 11) {
    alpha_ = 200;
    ApplyAlpha();
  } else if (cmd == 12) {
    alpha_ = 150;
    ApplyAlpha();
  } else if (cmd == 13) {
    alpha_ = 100;
    ApplyAlpha();
  } else if (cmd == 9) {
    Destroy();
  }
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
    self = reinterpret_cast<PinnedImage*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
    case WM_MOUSEWHEEL: {
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
      if (GetKeyState(VK_CONTROL) & 0x8000) {
        // Ctrl + 滚轮：无级调节透明度 (30 ~ 255)
        int na = static_cast<int>(alpha_) + (delta > 0 ? 25 : -25);
        if (na < 35) na = 35;
        if (na > 255) na = 255;
        alpha_ = static_cast<BYTE>(na);
        ApplyAlpha();
      } else {
        // 滚轮：自由无级平滑缩放
        POINT pt{};
        GetCursorPos(&pt);
        const float factor = delta > 0 ? 1.12f : 0.88f;
        Zoom(factor, pt);
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (InCloseButton(pt)) {
        Destroy();
        return 0;
      }
      dragging_ = true;
      ClientToScreen(hwnd_, &pt);
      RECT rc{};
      GetWindowRect(hwnd_, &rc);
      drag_offset_ = {pt.x - rc.left, pt.y - rc.top};
      SetCapture(hwnd_);
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const bool hover = InCloseButton(pt);
      if (hover != close_hover_) {
        close_hover_ = hover;
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      if (dragging_ && (wparam & MK_LBUTTON)) {
        POINT cur{};
        GetCursorPos(&cur);
        SetWindowPos(hwnd_, HWND_TOPMOST, cur.x - drag_offset_.x,
                     cur.y - drag_offset_.y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
      }
      return 0;
    }
    case WM_LBUTTONUP:
      dragging_ = false;
      ReleaseCapture();
      return 0;
    case WM_LBUTTONDBLCLK: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (InCloseButton(pt)) {
        Destroy();
        return 0;
      }
      // 双击：若当前不是 100% 原始大小则一键还原；若已是 100% 则快速缩略到 50%
      RECT rc{};
      GetWindowRect(hwnd_, &rc);
      POINT center{rc.left + (rc.right - rc.left) / 2, rc.top + (rc.bottom - rc.top) / 2};
      if (std::abs(zoom_ - 1.0f) > 0.05f) {
        ResetZoom();
      } else {
        Zoom(0.5f, center);
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ClientToScreen(hwnd_, &pt);
      ShowContextMenu(pt);
      return 0;
    }
    case WM_KEYDOWN: {
      if (wparam == VK_ESCAPE || wparam == VK_DELETE) {
        Destroy();
        return 0;
      }
      const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      if (ctrl && wparam == 'C') {
        CopyToClipboard();
        return 0;
      }
      if (ctrl && wparam == 'S') {
        SaveAs();
        return 0;
      }
      if (ctrl && (wparam == '0' || wparam == VK_NUMPAD0)) {
        ResetZoom();
        return 0;
      }
      break;
    }
    case WM_DESTROY:
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void PinnedImage::Paint(HDC hdc) {
  if (!bmp_ || orig_w_ <= 0 || orig_h_ <= 0) {
    return;
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  const int w = client.right - client.left;
  const int h = client.bottom - client.top;
  if (w <= 0 || h <= 0) return;

  // 使用 Gdiplus 高质量插值平滑缩放绘制位图
  Gdiplus::Graphics g(hdc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

  Gdiplus::Bitmap gbmp(bmp_, nullptr);
  g.DrawImage(&gbmp, 0, 0, w, h);

  // 精致微光边框：内圈半透明白，外圈微阴影
  Gdiplus::Pen outer_pen(Gdiplus::Color(55, 0, 0, 0), 1.0f);
  g.DrawRectangle(&outer_pen, 0, 0, w - 1, h - 1);
  Gdiplus::Pen inner_pen(Gdiplus::Color(80, 255, 255, 255), 1.0f);
  g.DrawRectangle(&inner_pen, 1, 1, w - 3, h - 3);

  // 右上角现代悬浮毛玻璃关闭按钮
  const float btn_w = static_cast<float>(kCloseSize);
  const float btn_h = static_cast<float>(kCloseSize);
  const float bx = static_cast<float>(w - kCloseSize - 4);
  const float by = 4.0f;

  Gdiplus::SolidBrush btn_bg(close_hover_
      ? Gdiplus::Color(220, 239, 68, 68)     // hover 鲜艳红
      : Gdiplus::Color(140, 15, 23, 42));    // 平时半透明深色磨砂
  g.FillEllipse(&btn_bg, bx, by, btn_w, btn_h);

  Gdiplus::Pen cross_pen(Gdiplus::Color(240, 255, 255, 255), 1.6f);
  cross_pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
  const float cx = bx + btn_w * 0.5f;
  const float cy = by + btn_h * 0.5f;
  const float s = 4.5f;
  g.DrawLine(&cross_pen, cx - s, cy - s, cx + s, cy + s);
  g.DrawLine(&cross_pen, cx + s, cy - s, cx - s, cy + s);
}

bool PinnedImage::InCloseButton(POINT client) const {
  if (!hwnd_) return false;
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int bx = w - kCloseSize - 4;
  const int by = 4;
  return client.x >= bx && client.x <= bx + kCloseSize &&
         client.y >= by && client.y <= by + kCloseSize;
}
