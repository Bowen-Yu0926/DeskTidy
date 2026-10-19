#include "capture_overlay.h"

#include "display_geom.h"
#include "paths_util.h"

#include <ShlObj.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>

#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeCapture";
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

struct GdiOnce {
  ULONG_PTR token = 0;
  GdiOnce() {
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&token, &in, nullptr);
  }
};
GdiOnce& Gdi() {
  static GdiOnce g;
  return g;
}
}  // namespace

bool CaptureOverlay::Begin(HINSTANCE instance, DoneFn on_done) {
  Gdi();
  instance_ = instance;
  on_done_ = std::move(on_done);
  if (!g_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CaptureOverlay::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_reg = true;
  }
  const RECT vs = VirtualScreenRect();
  hwnd_ = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClass, L"Capture",
      WS_POPUP, vs.left, vs.top, vs.right - vs.left, vs.bottom - vs.top, nullptr,
      nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 120, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
  SetCapture(hwnd_);
  return true;
}

void CaptureOverlay::Cancel() {
  if (hwnd_) {
    ReleaseCapture();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

bool CaptureOverlay::SaveSelection() {
  RECT sel{
      (std::min)(start_.x, end_.x), (std::min)(start_.y, end_.y),
      (std::max)(start_.x, end_.x), (std::max)(start_.y, end_.y)};
  const int drag_w = sel.right - sel.left;
  const int drag_h = sel.bottom - sel.top;
  if (drag_w < 8 && drag_h < 8 && hover_ok_) {
    sel = hover_;
  }
  if (sel.right - sel.left < 4 || sel.bottom - sel.top < 4) {
    return false;
  }
  const int cw = sel.right - sel.left;
  const int ch = sel.bottom - sel.top;
  POINT origin{sel.left, sel.top};
  ClientToScreen(hwnd_, &origin);
  ShowWindow(hwnd_, SW_HIDE);
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, cw, ch);
  HGDIOBJ old = SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, cw, ch, screen, origin.x, origin.y, SRCCOPY);

  PWSTR pics = nullptr;
  std::wstring dir = NativeDataDir();
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics)) &&
      pics) {
    dir = JoinPath(pics, L"DeskTidyNative");
    CoTaskMemFree(pics);
    CreateDirectoryW(dir.c_str(), nullptr);
  }
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf_s(name, L"region_%04d%02d%02d_%02d%02d%02d.png", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  const std::wstring path = JoinPath(dir, name);

  Gdiplus::Bitmap bitmap(bmp, nullptr);
  CLSID clsid{};
  bool ok = false;
  if (GetEncoderClsid(L"image/png", &clsid) >= 0) {
    ok = bitmap.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);

  if (ok && on_done_) {
    const int choice = MessageBoxW(
        nullptr, L"截图已保存。\n\n是 = 钉到桌面\n否 = 仅保存", L"区域截图",
        MB_YESNO | MB_ICONINFORMATION);
    on_done_(path, choice == IDYES);
  }
  return ok;
}

LRESULT CALLBACK CaptureOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam) {
  CaptureOverlay* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<CaptureOverlay*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<CaptureOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

void CaptureOverlay::UpdateHover(POINT client) {
  POINT screen = client;
  ClientToScreen(hwnd_, &screen);
  struct Pack {
    HWND skip;
    POINT pt;
    HWND best;
    LONG best_area;
  } pack{hwnd_, screen, nullptr, 0x7fffffff};
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* p = reinterpret_cast<Pack*>(lp);
        if (hwnd == p->skip || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
          return TRUE;
        }
        RECT wr{};
        if (!GetWindowRect(hwnd, &wr) || !PtInRect(&wr, p->pt)) {
          return TRUE;
        }
        const LONG area =
            (wr.right - wr.left) * (wr.bottom - wr.top);
        if (area < 64 * 64 || area >= p->best_area) {
          return TRUE;
        }
        p->best = hwnd;
        p->best_area = area;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&pack));
  hover_ok_ = false;
  if (!pack.best) {
    return;
  }
  RECT wr{};
  if (!GetWindowRect(pack.best, &wr)) {
    return;
  }
  POINT tl{wr.left, wr.top};
  POINT br{wr.right, wr.bottom};
  ScreenToClient(hwnd_, &tl);
  ScreenToClient(hwnd_, &br);
  hover_ = {tl.x, tl.y, br.x, br.y};
  hover_ok_ = true;
}

LRESULT CaptureOverlay::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd_, &ps);
      Paint(hdc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN:
      dragging_ = true;
      start_ = end_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      SetCapture(hwnd_);
      return 0;
    case WM_MOUSEMOVE:
      if (dragging_) {
        end_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      } else {
        UpdateHover({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      }
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_LBUTTONUP:
      if (dragging_) {
        dragging_ = false;
        end_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ReleaseCapture();
        SaveSelection();
        Cancel();
      }
      return 0;
    case WM_RBUTTONUP:
    case WM_KEYDOWN:
      if (msg == WM_RBUTTONUP || wparam == VK_ESCAPE) {
        Cancel();
      }
      return 0;
    case WM_DESTROY:
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void CaptureOverlay::Paint(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  HBRUSH dim = CreateSolidBrush(RGB(0, 0, 0));
  FillRect(hdc, &rc, dim);
  DeleteObject(dim);
  if (!dragging_) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(240, 240, 240));
    RECT hint = rc;
    hint.top += 24;
    hint.bottom = hint.top + 28;
    DrawTextW(hdc, L"单击窗口截取 · 拖拽自由选区 · Esc 取消", -1, &hint,
              DT_CENTER | DT_SINGLELINE);
    if (hover_ok_) {
      HPEN pen = CreatePen(PS_SOLID, 2, RGB(80, 180, 255));
      HGDIOBJ old = SelectObject(hdc, pen);
      SelectObject(hdc, GetStockObject(NULL_BRUSH));
      Rectangle(hdc, hover_.left, hover_.top, hover_.right, hover_.bottom);
      SelectObject(hdc, old);
      DeleteObject(pen);
    }
    return;
  }
  RECT sel{(std::min)(start_.x, end_.x), (std::min)(start_.y, end_.y),
           (std::max)(start_.x, end_.x), (std::max)(start_.y, end_.y)};
  // Clear selection area (show through) by drawing inverted — with LWA_ALPHA
  // just draw a border.
  HPEN pen = CreatePen(PS_SOLID, 2, RGB(80, 180, 255));
  HGDIOBJ old = SelectObject(hdc, pen);
  SelectObject(hdc, GetStockObject(NULL_BRUSH));
  Rectangle(hdc, sel.left, sel.top, sel.right, sel.bottom);
  SelectObject(hdc, old);
  DeleteObject(pen);
}
