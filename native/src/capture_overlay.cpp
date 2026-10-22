#include "capture_overlay.h"

#include "display_geom.h"
#include "log.h"
#include "paths_util.h"

#include <ShlObj.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

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

// True when DWM has cloaked the window (UWP / virtual-desktop ghost windows
// still report IsWindowVisible == TRUE but render nothing).
bool IsCloaked(HWND hwnd) {
  BOOL cloaked = FALSE;
  HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                     sizeof(cloaked));
  return SUCCEEDED(hr) && cloaked;
}

// Convert a DDB to a packed CF_DIB (BITMAPINFOHEADER + 32bpp bottom-up pixels)
// and place it on the clipboard. Clipboard failures only log — they never
// affect the saved PNG result.
void CopyBitmapToClipboardDib(HBITMAP bmp) {
  if (!bmp) {
    return;
  }
  BITMAP bm{};
  if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth <= 0 ||
      bm.bmHeight <= 0) {
    return;
  }
  const int width = bm.bmWidth;
  const int height = bm.bmHeight;
  const int bpp = 32;
  const int stride = ((width * bpp + 31) / 32) * 4;
  const DWORD img_size = static_cast<DWORD>(stride) * height;

  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(bi);
  bi.biWidth = width;
  bi.biHeight = height;
  bi.biPlanes = 1;
  bi.biBitCount = static_cast<WORD>(bpp);
  bi.biCompression = BI_RGB;
  bi.biSizeImage = img_size;

  HDC screen = GetDC(nullptr);
  if (!screen) {
    LogLineA("[shot] GetDC failed, clipboard skipped");
    return;
  }
  std::vector<BYTE> bits(img_size);
  const int got =
      GetDIBits(screen, bmp, 0, height, bits.data(),
                reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
  ReleaseDC(nullptr, screen);
  if (!got) {
    LogLineA("[shot] GetDIBits failed, clipboard skipped");
    return;
  }

  if (!OpenClipboard(nullptr)) {
    LogLineA("[shot] OpenClipboard failed, clipboard skipped");
    return;
  }
  if (!EmptyClipboard()) {
    LogLineA("[shot] EmptyClipboard failed, clipboard skipped");
    CloseClipboard();
    return;
  }
  const DWORD total = sizeof(bi) + img_size;
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, total);
  if (h) {
    void* dst = GlobalLock(h);
    if (dst) {
      memcpy(dst, &bi, sizeof(bi));
      memcpy(reinterpret_cast<BYTE*>(dst) + sizeof(bi), bits.data(), img_size);
      GlobalUnlock(h);
      SetClipboardData(CF_DIB, h);
    } else {
      GlobalFree(h);
    }
  }
  CloseClipboard();
}

// Save a DDB to disk in PNG / JPEG / BMP chosen by file extension.
// JPEG is re-encoded (quality 85), never a rename-only copy.
bool SaveBitmapToFile(HBITMAP bmp, const std::wstring& path) {
  if (!bmp || path.empty()) {
    return false;
  }
  const size_t dot = path.find_last_of(L'.');
  std::wstring ext = (dot != std::wstring::npos) ? path.substr(dot) : L"";
  for (auto& c : ext) {
    c = static_cast<wchar_t>(towlower(c));
  }
  const wchar_t* mime = L"image/png";
  if (ext == L".jpg" || ext == L".jpeg") {
    mime = L"image/jpeg";
  } else if (ext == L".bmp") {
    mime = L"image/bmp";
  }
  CLSID clsid{};
  if (GetEncoderClsid(mime, &clsid) < 0) {
    return false;
  }
  Gdiplus::Bitmap bitmap(bmp, nullptr);
  if (mime == std::wstring(L"image/jpeg")) {
    Gdiplus::EncoderParameters params;
    params.Count = 1;
    params.Parameter[0].Guid = Gdiplus::EncoderQuality;
    params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    LONG quality = 85;
    params.Parameter[0].Value = &quality;
    return bitmap.Save(path.c_str(), &clsid, &params) == Gdiplus::Ok;
  }
  return bitmap.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

// Show a Save-As dialog (PNG / JPEG); returns the chosen path or empty.
std::wstring ShowSaveAsDialog() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf_s(name, L"截图_%04d%02d%02d_%02d%02d%02d.png", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  wchar_t file[MAX_PATH] = {};
  wcscpy_s(file, name);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"PNG 图片\0*.png\0JPEG 图片\0*.jpg;*.jpeg\0";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  ofn.lpstrDefExt = L"png";
  if (GetSaveFileNameW(&ofn)) {
    return file;
  }
  return {};
}

// Blend a translucent black fill over the window area clipped to `dim` so the
// frozen frame stays original inside the selection but darkens outside it.
void DimRegion(HDC hdc, HRGN dim, int cw, int ch) {
  if (!dim) {
    return;
  }
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = 1;
  bi.bmiHeader.biHeight = 1;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC mem = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!mem || !bmp || !bits) {
    if (bmp) {
      DeleteObject(bmp);
    }
    if (mem) {
      DeleteDC(mem);
    }
    return;
  }
  *static_cast<DWORD*>(bits) = 0xFF000000;  // black
  HGDIOBJ old = SelectObject(mem, bmp);
  SelectClipRgn(hdc, dim);
  BLENDFUNCTION bf{};
  bf.BlendOp = AC_SRC_OVER;
  bf.SourceConstantAlpha = 120;
  bf.AlphaFormat = 0;
  AlphaBlend(hdc, 0, 0, cw, ch, mem, 0, 0, 1, 1, bf);
  SelectClipRgn(hdc, nullptr);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
}
}  // namespace

bool CaptureOverlay::FreezeVirtualDesktop() {
  ReleaseFrozenFrame();
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (width <= 0 || height <= 0) {
    LogLineA("[shot] virtual desktop size invalid, freeze skipped");
    return false;
  }
  HDC screen = GetDC(nullptr);
  if (!screen) {
    LogLineA("[shot] GetDC failed, freeze skipped");
    return false;
  }
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, width, height);
  HGDIOBJ old = SelectObject(mem, bmp);
  const BOOL ok = BitBlt(mem, 0, 0, width, height, screen, left, top, SRCCOPY);
  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  if (!ok) {
    DeleteObject(bmp);
    LogLineA("[shot] freeze BitBlt failed");
    return false;
  }
  bkg_bmp_ = bmp;
  bkg_w_ = width;
  bkg_h_ = height;
  LogLine(L"[shot] frozen virtual desktop %dx%d @ (%d,%d)", width, height, left,
          top);
  return true;
}

void CaptureOverlay::ReleaseFrozenFrame() {
  if (bkg_bmp_) {
    DeleteObject(bkg_bmp_);
    bkg_bmp_ = nullptr;
  }
  bkg_w_ = bkg_h_ = 0;
}

HBITMAP CaptureOverlay::CropSelectionBitmap(const RECT& sel) const {
  if (!bkg_bmp_ || bkg_w_ <= 0 || bkg_h_ <= 0) {
    return nullptr;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int cw = (rc.right - rc.left) > 0 ? (rc.right - rc.left) : bkg_w_;
  const int ch = (rc.bottom - rc.top) > 0 ? (rc.bottom - rc.top) : bkg_h_;
  const double sx = static_cast<double>(bkg_w_) / cw;
  const double sy = static_cast<double>(bkg_h_) / ch;
  int fx = static_cast<int>(std::lround(sel.left * sx));
  int fy = static_cast<int>(std::lround(sel.top * sy));
  int fw = static_cast<int>(std::lround((sel.right - sel.left) * sx));
  int fh = static_cast<int>(std::lround((sel.bottom - sel.top) * sy));
  if (fx < 0) {
    fx = 0;
  }
  if (fy < 0) {
    fy = 0;
  }
  if (fx + fw > bkg_w_) {
    fw = bkg_w_ - fx;
  }
  if (fy + fh > bkg_h_) {
    fh = bkg_h_ - fy;
  }
  if (fw < 1 || fh < 1) {
    return nullptr;
  }
  HDC screen = GetDC(nullptr);
  HDC src = CreateCompatibleDC(screen);
  HDC dst = CreateCompatibleDC(screen);
  HBITMAP out = CreateCompatibleBitmap(screen, fw, fh);
  HGDIOBJ os = SelectObject(src, bkg_bmp_);
  HGDIOBJ od = SelectObject(dst, out);
  BitBlt(dst, 0, 0, fw, fh, src, fx, fy, SRCCOPY);
  SelectObject(src, os);
  SelectObject(dst, od);
  DeleteDC(src);
  DeleteDC(dst);
  ReleaseDC(nullptr, screen);
  return out;
}

bool CaptureOverlay::Begin(HINSTANCE instance, DoneFn on_done, bool auto_pin) {
  Gdi();
  instance_ = instance;
  auto_pin_ = auto_pin;
  on_done_ = std::move(on_done);
  // Freeze BEFORE showing the overlay so the overlay itself is never captured.
  FreezeVirtualDesktop();
  if (!g_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CaptureOverlay::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_reg = true;
  }
  const RECT vs = VirtualScreenRect();
  hwnd_ = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"Capture", WS_POPUP, vs.left,
      vs.top, vs.right - vs.left, vs.bottom - vs.top, nullptr, nullptr,
      instance, this);
  if (!hwnd_) {
    return false;
  }
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
  ReleaseFrozenFrame();
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
  HBITMAP bmp = CropSelectionBitmap(sel);
  if (!bmp) {
    LogLineA("[shot] crop failed");
    return false;
  }
  CopyBitmapToClipboardDib(bmp);

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

  bool ok = SaveBitmapToFile(bmp, path);
  bool pin = auto_pin_;
  if (ok && !auto_pin_) {
    const int choice = MessageBoxW(
        nullptr,
        L"截图已保存并复制到剪贴板。\n\n是 = 钉到桌面\n否 = 另存为…\n取消 = 完成",
        L"区域截图", MB_YESNOCANCEL | MB_ICONINFORMATION);
    if (choice == IDYES) {
      pin = true;
    } else if (choice == IDNO) {
      const std::wstring chosen = ShowSaveAsDialog();
      if (!chosen.empty() && _wcsicmp(chosen.c_str(), path.c_str()) != 0) {
        if (!SaveBitmapToFile(bmp, chosen)) {
          MessageBoxW(nullptr, L"另存为失败，请检查路径或磁盘空间。",
                      L"区域截图", MB_OK | MB_ICONERROR);
        }
      }
    }
  }
  DeleteObject(bmp);
  if (!ok) {
    LogLineA("[shot] save to default path failed");
    MessageBoxW(nullptr, L"截图保存失败，请检查磁盘空间或目录权限。",
                L"区域截图", MB_OK | MB_ICONERROR);
    return false;
  }
  if (on_done_) {
    on_done_(path, pin);
  }
  return true;
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
  const DWORD self_pid = GetCurrentProcessId();
  struct Pack {
    HWND skip;
    POINT pt;
    DWORD self_pid;
    HWND best;
  } pack{hwnd_, screen, self_pid, nullptr};
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* p = reinterpret_cast<Pack*>(lp);
        if (hwnd == p->skip || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
          return TRUE;
        }
        if (IsCloaked(hwnd)) {
          return TRUE;
        }
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW) {
          return TRUE;
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == p->self_pid) {
          return TRUE;
        }
        RECT wr{};
        if (!GetWindowRect(hwnd, &wr) || !PtInRect(&wr, p->pt)) {
          return TRUE;
        }
        if (wr.right - wr.left < 24 || wr.bottom - wr.top < 24) {
          return TRUE;
        }
        // EnumWindows visits top-level windows in Z-order (topmost first), so
        // the first containing the point is the window actually under the cursor.
        p->best = hwnd;
        return FALSE;
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
    case WM_ERASEBKGND:
      return 1;
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
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;

  if (bkg_bmp_ && bkg_w_ > 0 && bkg_h_ > 0) {
    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mem, bkg_bmp_);
    if (cw == bkg_w_ && ch == bkg_h_) {
      BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    } else {
      SetStretchBltMode(hdc, HALFTONE);
      StretchBlt(hdc, 0, 0, cw, ch, mem, 0, 0, bkg_w_, bkg_h_, SRCCOPY);
    }
    SelectObject(mem, old);
    DeleteDC(mem);
  } else {
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rc, black);
    DeleteObject(black);
  }

  RECT hole{};
  bool have_hole = false;
  if (dragging_) {
    hole = {(std::min)(start_.x, end_.x), (std::min)(start_.y, end_.y),
            (std::max)(start_.x, end_.x), (std::max)(start_.y, end_.y)};
    have_hole = true;
  } else if (hover_ok_) {
    hole = hover_;
    have_hole = true;
  }

  HRGN dim = CreateRectRgn(0, 0, cw, ch);
  if (have_hole) {
    HRGN hole_rgn =
        CreateRectRgn(hole.left, hole.top, hole.right, hole.bottom);
    CombineRgn(dim, dim, hole_rgn, RGN_DIFF);
    DeleteObject(hole_rgn);
  }
  DimRegion(hdc, dim, cw, ch);
  DeleteObject(dim);

  if (have_hole) {
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(80, 180, 255));
    HGDIOBJ old = SelectObject(hdc, pen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, hole.left, hole.top, hole.right, hole.bottom);
    SelectObject(hdc, old);
    DeleteObject(pen);
  }

  if (!dragging_ && !hover_ok_) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT hint = rc;
    hint.top += 24;
    hint.bottom = hint.top + 28;
    DrawTextW(hdc, L"单击窗口截取 · 拖拽自由选区 · Esc 取消", -1, &hint,
              DT_CENTER | DT_SINGLELINE);
  }
}
