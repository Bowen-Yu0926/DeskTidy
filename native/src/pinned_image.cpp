#include "pinned_image.h"

#include "i18n.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativePinImg";
constexpr int kCloseSize = 22;
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
}  // namespace

bool PinnedImage::Create(HINSTANCE instance, const std::wstring& path) {
  if (!g_reg) {
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

  path_ = path;
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
      WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClass,
      L"贴图", WS_POPUP, (sw - bmp_w_) / 2, 80, bmp_w_, bmp_h_, nullptr, nullptr,
      instance, this);
  if (!hwnd_) {
    return false;
  }
  alpha_ = 255;
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
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void PinnedImage::SaveAs() {
  if (path_.empty()) {
    return;
  }
  wchar_t file[MAX_PATH] = L"截图.png";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"PNG 图片\0*.png\0JPEG 图片\0*.jpg;*.jpeg\0";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  ofn.lpstrDefExt = L"png";
  // Drop TOPMOST while modal dialog is open.
  SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  const BOOL ok = GetSaveFileNameW(&ofn);
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  if (!ok) {
    return;
  }
  // Re-encode from the original source (full resolution) — never a rename-only
  // copy, so choosing JPEG actually produces a JPEG payload.
  const std::wstring dest = file;
  const size_t dot = dest.find_last_of(L'.');
  std::wstring ext = (dot != std::wstring::npos) ? dest.substr(dot) : L"";
  for (auto& c : ext) {
    c = static_cast<wchar_t>(towlower(c));
  }
  const wchar_t* mime = L"image/png";
  if (ext == L".jpg" || ext == L".jpeg") {
    mime = L"image/jpeg";
  } else if (ext == L".bmp") {
    mime = L"image/bmp";
  }
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartupInput input;
  Gdiplus::GdiplusStartup(&token, &input, nullptr);
  Gdiplus::Bitmap src(path_.c_str());
  bool saved = false;
  if (src.GetLastStatus() == Gdiplus::Ok) {
    CLSID clsid{};
    if (GetEncoderClsid(mime, &clsid) >= 0) {
      if (mime == std::wstring(L"image/jpeg")) {
        Gdiplus::EncoderParameters params;
        params.Count = 1;
        params.Parameter[0].Guid = Gdiplus::EncoderQuality;
        params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        LONG quality = 85;
        params.Parameter[0].Value = &quality;
        saved = src.Save(dest.c_str(), &clsid, &params) == Gdiplus::Ok;
      } else {
        saved = src.Save(dest.c_str(), &clsid, nullptr) == Gdiplus::Ok;
      }
    }
  }
  Gdiplus::GdiplusShutdown(token);
  if (!saved) {
    MessageBoxW(hwnd_, L"另存为失败，请检查路径或磁盘空间。", L"贴图",
                MB_OK | MB_ICONERROR);
  }
}

void PinnedImage::CopyToClipboard() {
  if (!bmp_ || !OpenClipboard(hwnd_)) {
    return;
  }
  EmptyClipboard();
  // Duplicate bitmap for clipboard ownership.
  BITMAP bm{};
  GetObjectW(bmp_, sizeof(bm), &bm);
  HDC screen = GetDC(nullptr);
  HDC src = CreateCompatibleDC(screen);
  HDC dst = CreateCompatibleDC(screen);
  HBITMAP copy =
      CreateCompatibleBitmap(screen, bm.bmWidth, bm.bmHeight);
  HGDIOBJ os = SelectObject(src, bmp_);
  HGDIOBJ od = SelectObject(dst, copy);
  BitBlt(dst, 0, 0, bm.bmWidth, bm.bmHeight, src, 0, 0, SRCCOPY);
  SelectObject(src, os);
  SelectObject(dst, od);
  DeleteDC(src);
  DeleteDC(dst);
  ReleaseDC(nullptr, screen);
  SetClipboardData(CF_BITMAP, copy);
  CloseClipboard();
}

void PinnedImage::ShowContextMenu(POINT screen) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1, Tr("pin_save_as").c_str());
  AppendMenuW(menu, MF_STRING, 2, Tr("pin_copy").c_str());
  HMENU opa = CreatePopupMenu();
  AppendMenuW(opa, MF_STRING | (alpha_ == 255 ? MF_CHECKED : 0), 10,
              Tr("opacity_opaque").c_str());
  AppendMenuW(opa, MF_STRING | (alpha_ == 200 ? MF_CHECKED : 0), 11, L"80%");
  AppendMenuW(opa, MF_STRING | (alpha_ == 150 ? MF_CHECKED : 0), 12, L"60%");
  AppendMenuW(opa, MF_STRING | (alpha_ == 100 ? MF_CHECKED : 0), 13, L"40%");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opa),
              Tr("fence_opacity").c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 9, Tr("pin_close").c_str());
  SetForegroundWindow(hwnd_);
  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y,
                     0, hwnd_, nullptr);
  DestroyMenu(menu);
  if (cmd == 1) {
    SaveAs();
  } else if (cmd == 2) {
    CopyToClipboard();
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
    case WM_MOUSEMOVE:
      if (dragging_ && (wparam & MK_LBUTTON)) {
        POINT pt{};
        GetCursorPos(&pt);
        SetWindowPos(hwnd_, HWND_TOPMOST, pt.x - drag_offset_.x,
                     pt.y - drag_offset_.y, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
      }
      return 0;
    case WM_LBUTTONUP:
      dragging_ = false;
      ReleaseCapture();
      return 0;
    case WM_LBUTTONDBLCLK:
      Destroy();
      return 0;
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ClientToScreen(hwnd_, &pt);
      ShowContextMenu(pt);
      return 0;
    }
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        Destroy();
        return 0;
      }
      break;
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

  // Top-right close (×) button.
  const int x = bmp_w_ - kCloseSize;
  const int y = 0;
  HBRUSH bg = CreateSolidBrush(RGB(30, 30, 30));
  HPEN pen = CreatePen(PS_SOLID, 2, RGB(235, 235, 235));
  RECT btn{x, y, bmp_w_, kCloseSize};
  FillRect(hdc, &btn, bg);
  HGDIOBJ op = SelectObject(hdc, pen);
  const int inset = 6;
  MoveToEx(hdc, x + inset, y + inset, nullptr);
  LineTo(hdc, x + kCloseSize - inset, y + kCloseSize - inset);
  MoveToEx(hdc, x + kCloseSize - inset, y + inset, nullptr);
  LineTo(hdc, x + inset, y + kCloseSize - inset);
  SelectObject(hdc, op);
  DeleteObject(pen);
  DeleteObject(bg);
}

bool PinnedImage::InCloseButton(POINT client) const {
  return client.x >= bmp_w_ - kCloseSize && client.x <= bmp_w_ &&
         client.y >= 0 && client.y <= kCloseSize;
}
