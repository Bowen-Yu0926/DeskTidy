#include "capture_overlay.h"

#include "display_geom.h"
#include "log.h"
#include "paths_util.h"
#include "prompt_text.h"

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
constexpr wchar_t kTbClass[] = L"DeskTidyNativeCaptureTb";
bool g_reg = false;
bool g_tb_reg = false;

constexpr COLORREF kColors[] = {
    RGB(250, 81, 81),   // red
    RGB(250, 157, 59),  // orange
    RGB(20, 133, 238),  // blue
    RGB(7, 193, 96),    // green
    RGB(25, 25, 25),    // black
    RGB(255, 255, 255), // white
};
constexpr int kWidths[] = {2, 4, 7};
constexpr const wchar_t* kEmojis[] = {L"😀", L"👍", L"❤️", L"🔥",
                                      L"⭐", L"👀", L"🎉", L"❗"};

enum TbId {
  TbRect = 1,
  TbEllipse,
  TbArrow,
  TbBrush,
  TbMosaic,
  TbText,
  TbEmoji,
  TbUndo,
  TbColor,
  TbWidth,
  TbSave,
  TbCancel,
  TbCopy,
  TbCount = TbCopy
};

constexpr int kTbBtn = 32;
constexpr int kTbPad = 6;
constexpr int kTbGap = 2;

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

bool IsCloaked(HWND hwnd) {
  BOOL cloaked = FALSE;
  HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                     sizeof(cloaked));
  return SUCCEEDED(hr) && cloaked;
}

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
    return;
  }
  std::vector<BYTE> bits(img_size);
  const int got =
      GetDIBits(screen, bmp, 0, height, bits.data(),
                reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
  ReleaseDC(nullptr, screen);
  if (!got) {
    return;
  }
  if (!OpenClipboard(nullptr)) {
    return;
  }
  EmptyClipboard();
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
  *static_cast<DWORD*>(bits) = 0xFF000000;
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

RECT TbButtonRect(int id) {
  // Layout: tools | undo color width | save cancel copy
  const int order[] = {TbRect,  TbEllipse, TbArrow, TbBrush, TbMosaic,
                       TbText,  TbEmoji,   TbUndo,  TbColor, TbWidth,
                       TbSave,  TbCancel,  TbCopy};
  int slot = 0;
  for (int i = 0; i < 13; ++i) {
    if (order[i] == id) {
      slot = i;
      break;
    }
  }
  int x = kTbPad + slot * (kTbBtn + kTbGap);
  if (slot >= 7) {
    x += 6;  // gap before undo group
  }
  if (slot >= 10) {
    x += 6;  // gap before actions
  }
  return {x, kTbPad, x + kTbBtn, kTbPad + kTbBtn};
}

int ToolbarWidth() {
  RECT last = TbButtonRect(TbCopy);
  return last.right + kTbPad;
}

int ToolbarHeight() { return kTbPad * 2 + kTbBtn; }

Gdiplus::Color GpColor(COLORREF c, BYTE a = 255) {
  return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

}  // namespace

bool CaptureOverlay::FreezeVirtualDesktop() {
  ReleaseFrozenFrame();
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (width <= 0 || height <= 0) {
    return false;
  }
  HDC screen = GetDC(nullptr);
  if (!screen) {
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
    return false;
  }
  bkg_bmp_ = bmp;
  bkg_w_ = width;
  bkg_h_ = height;
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

COLORREF CaptureOverlay::SampleFrozen(int client_x, int client_y) const {
  if (!bkg_bmp_ || bkg_w_ <= 0) {
    return RGB(128, 128, 128);
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int cw = (std::max)(1, static_cast<int>(rc.right - rc.left));
  const int ch = (std::max)(1, static_cast<int>(rc.bottom - rc.top));
  const int fx = client_x * bkg_w_ / cw;
  const int fy = client_y * bkg_h_ / ch;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HGDIOBJ old = SelectObject(mem, bkg_bmp_);
  const COLORREF c = GetPixel(mem, fx, fy);
  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return c == CLR_INVALID ? RGB(128, 128, 128) : c;
}

bool CaptureOverlay::Begin(HINSTANCE instance, DoneFn on_done, bool auto_pin) {
  Gdi();
  instance_ = instance;
  auto_pin_ = auto_pin;
  on_done_ = std::move(on_done);
  mode_ = Mode::Selecting;
  tool_ = Tool::None;
  sel_ok_ = false;
  annotations_.clear();
  has_draft_ = false;
  drawing_ = false;
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
  SetFocus(hwnd_);
  SetCapture(hwnd_);
  return true;
}

void CaptureOverlay::Cancel() {
  LeaveAnnotateMode();
  if (hwnd_) {
    ReleaseCapture();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  ReleaseFrozenFrame();
}

void CaptureOverlay::CreateToolbar() {
  if (toolbar_ || !hwnd_) {
    return;
  }
  if (!g_tb_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CaptureOverlay::ToolbarProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kTbClass;
    RegisterClassExW(&wc);
    g_tb_reg = true;
  }
  toolbar_ = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kTbClass, L"", WS_POPUP, 0, 0,
      ToolbarWidth(), ToolbarHeight(), hwnd_, nullptr, instance_, this);
  if (toolbar_) {
    SetWindowLongPtrW(toolbar_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  }
}

void CaptureOverlay::DestroyToolbar() {
  if (toolbar_) {
    DestroyWindow(toolbar_);
    toolbar_ = nullptr;
  }
}

void CaptureOverlay::PlaceToolbar() {
  if (!toolbar_ || !sel_ok_) {
    return;
  }
  POINT origin{};
  ClientToScreen(hwnd_, &origin);
  const int tw = ToolbarWidth();
  const int th = ToolbarHeight();
  int x = origin.x + sel_.left + (sel_.right - sel_.left - tw) / 2;
  int y = origin.y + sel_.bottom + 8;
  const RECT vs = VirtualScreenRect();
  if (x + tw > vs.right) {
    x = vs.right - tw;
  }
  if (x < vs.left) {
    x = vs.left;
  }
  if (y + th > vs.bottom) {
    y = origin.y + sel_.top - th - 8;
  }
  if (y < vs.top) {
    y = vs.top;
  }
  SetWindowPos(toolbar_, HWND_TOPMOST, x, y, tw, th,
               SWP_SHOWWINDOW | SWP_NOACTIVATE);
  InvalidateRect(toolbar_, nullptr, FALSE);
}

void CaptureOverlay::EnterAnnotateMode() {
  mode_ = Mode::Annotating;
  tool_ = Tool::Rect;
  ReleaseCapture();
  CreateToolbar();
  PlaceToolbar();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void CaptureOverlay::LeaveAnnotateMode() {
  DestroyToolbar();
  mode_ = Mode::Selecting;
  tool_ = Tool::None;
  has_draft_ = false;
  drawing_ = false;
  annotations_.clear();
  sel_ok_ = false;
}

void CaptureOverlay::CommitSelection() {
  RECT sel = CurrentSelectionRect();
  if (sel.right - sel.left < 4 || sel.bottom - sel.top < 4) {
    return;
  }
  sel_ = sel;
  sel_ok_ = true;
  start_ = {sel.left, sel.top};
  end_ = {sel.right, sel.bottom};
  EnterAnnotateMode();
}

RECT CaptureOverlay::CurrentSelectionRect() const {
  if (sel_ok_) {
    return sel_;
  }
  RECT sel{(std::min)(start_.x, end_.x), (std::min)(start_.y, end_.y),
           (std::max)(start_.x, end_.x), (std::max)(start_.y, end_.y)};
  const int drag_w = sel.right - sel.left;
  const int drag_h = sel.bottom - sel.top;
  if (drag_w < 8 && drag_h < 8 && hover_ok_) {
    sel = hover_;
  }
  return sel;
}

void CaptureOverlay::UndoAnnotation() {
  if (!annotations_.empty()) {
    annotations_.pop_back();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void CaptureOverlay::BeginDraft(Tool tool, POINT pt) {
  draft_ = {};
  draft_.color = stroke_color_;
  draft_.width = stroke_width_;
  draft_.start = draft_.end = pt;
  draft_.points = {pt};
  switch (tool) {
    case Tool::Rect:
      draft_.kind = Annotation::kRect;
      break;
    case Tool::Ellipse:
      draft_.kind = Annotation::kEllipse;
      break;
    case Tool::Arrow:
      draft_.kind = Annotation::kArrow;
      break;
    case Tool::Brush:
      draft_.kind = Annotation::kBrush;
      break;
    case Tool::Mosaic:
      draft_.kind = Annotation::kMosaic;
      break;
    default:
      return;
  }
  has_draft_ = true;
  drawing_ = true;
}

void CaptureOverlay::UpdateDraft(POINT pt) {
  if (!has_draft_) {
    return;
  }
  draft_.end = pt;
  if (draft_.kind == Annotation::kBrush || draft_.kind == Annotation::kMosaic) {
    if (draft_.points.empty() ||
        abs(pt.x - draft_.points.back().x) + abs(pt.y - draft_.points.back().y) >=
            (draft_.kind == Annotation::kMosaic ? 8 : 2)) {
      draft_.points.push_back(pt);
    }
  }
}

void CaptureOverlay::CommitDraft() {
  if (!has_draft_) {
    return;
  }
  annotations_.push_back(std::move(draft_));
  has_draft_ = false;
  drawing_ = false;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

int CaptureOverlay::HitToolbar(int x, int y) const {
  for (int id = TbRect; id <= TbCopy; ++id) {
    RECT r = TbButtonRect(id);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
      return id;
    }
  }
  return 0;
}

void CaptureOverlay::OnToolbarCommand(int id) {
  switch (id) {
    case TbRect:
      tool_ = Tool::Rect;
      break;
    case TbEllipse:
      tool_ = Tool::Ellipse;
      break;
    case TbArrow:
      tool_ = Tool::Arrow;
      break;
    case TbBrush:
      tool_ = Tool::Brush;
      break;
    case TbMosaic:
      tool_ = Tool::Mosaic;
      break;
    case TbText:
      tool_ = Tool::Text;
      break;
    case TbEmoji:
      tool_ = Tool::Emoji;
      break;
    case TbUndo:
      UndoAnnotation();
      break;
    case TbColor:
      color_index_ = (color_index_ + 1) %
                     static_cast<int>(sizeof(kColors) / sizeof(kColors[0]));
      stroke_color_ = kColors[color_index_];
      break;
    case TbWidth:
      width_index_ = (width_index_ + 1) % 3;
      stroke_width_ = kWidths[width_index_];
      break;
    case TbSave:
      FinishSelection(FinishAction::Save);
      Cancel();
      return;
    case TbCancel:
      Cancel();
      return;
    case TbCopy:
      FinishSelection(FinishAction::Copy);
      Cancel();
      return;
    default:
      break;
  }
  if (toolbar_) {
    InvalidateRect(toolbar_, nullptr, FALSE);
  }
}

void CaptureOverlay::DrawOneAnnotation(HDC hdc, const Annotation& a,
                                       bool /*draft*/) {
  Gdiplus::Graphics g(hdc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  Gdiplus::Pen pen(GpColor(a.color), static_cast<Gdiplus::REAL>(a.width));
  pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                 Gdiplus::DashCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);

  if (a.kind == Annotation::kRect) {
    const int l = (std::min)(a.start.x, a.end.x);
    const int t = (std::min)(a.start.y, a.end.y);
    const int w = abs(a.end.x - a.start.x);
    const int h = abs(a.end.y - a.start.y);
    g.DrawRectangle(&pen, l, t, w, h);
  } else if (a.kind == Annotation::kEllipse) {
    const int l = (std::min)(a.start.x, a.end.x);
    const int t = (std::min)(a.start.y, a.end.y);
    const int w = abs(a.end.x - a.start.x);
    const int h = abs(a.end.y - a.start.y);
    g.DrawEllipse(&pen, l, t, w, h);
  } else if (a.kind == Annotation::kArrow) {
    g.DrawLine(&pen, static_cast<INT>(a.start.x), static_cast<INT>(a.start.y), static_cast<INT>(a.end.x), static_cast<INT>(a.end.y));
    const double angle =
        atan2(static_cast<double>(a.end.y - a.start.y),
              static_cast<double>(a.end.x - a.start.x));
    const double alen = 10.0 + a.width * 2.0;
    const POINT left{
        static_cast<LONG>(a.end.x - alen * cos(angle - 3.14159265 / 6)),
        static_cast<LONG>(a.end.y - alen * sin(angle - 3.14159265 / 6))};
    const POINT right{
        static_cast<LONG>(a.end.x - alen * cos(angle + 3.14159265 / 6)),
        static_cast<LONG>(a.end.y - alen * sin(angle + 3.14159265 / 6))};
    Gdiplus::SolidBrush br(GpColor(a.color));
    Gdiplus::Point pts[3] = {{a.end.x, a.end.y},
                             {left.x, left.y},
                             {right.x, right.y}};
    g.FillPolygon(&br, pts, 3);
  } else if (a.kind == Annotation::kBrush) {
    for (size_t i = 1; i < a.points.size(); ++i) {
      g.DrawLine(&pen, static_cast<INT>(a.points[i - 1].x),
                 static_cast<INT>(a.points[i - 1].y),
                 static_cast<INT>(a.points[i].x),
                 static_cast<INT>(a.points[i].y));
    }
  } else if (a.kind == Annotation::kMosaic) {
    const int block = (std::max)(6, a.width * 3);
    for (const auto& pt : a.points) {
      const int gx = (pt.x / block) * block;
      const int gy = (pt.y / block) * block;
      const COLORREF c = SampleFrozen(gx + block / 2, gy + block / 2);
      Gdiplus::SolidBrush br(GpColor(c));
      g.FillRectangle(&br, gx, gy, block, block);
    }
  } else if (a.kind == Annotation::kText || a.kind == Annotation::kEmoji) {
    const int px = 12 + a.width + (a.kind == Annotation::kEmoji ? 6 : 0);
    Gdiplus::FontFamily fam(a.kind == Annotation::kEmoji ? L"Segoe UI Emoji"
                                                         : L"Microsoft YaHei UI");
    Gdiplus::Font font(&fam, static_cast<Gdiplus::REAL>(px),
                       Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush br(GpColor(a.color));
    g.DrawString(a.text.c_str(), -1, &font,
                 Gdiplus::PointF(static_cast<Gdiplus::REAL>(a.start.x),
                                 static_cast<Gdiplus::REAL>(a.start.y)),
                 &br);
  }
}

void CaptureOverlay::DrawAnnotations(HDC hdc) {
  for (const auto& a : annotations_) {
    DrawOneAnnotation(hdc, a, false);
  }
  if (has_draft_) {
    DrawOneAnnotation(hdc, draft_, true);
  }
}

HBITMAP CaptureOverlay::CompositeAnnotatedCrop(const RECT& sel) const {
  HBITMAP base = CropSelectionBitmap(sel);
  if (!base) {
    return nullptr;
  }
  if (annotations_.empty()) {
    return base;
  }
  BITMAP bm{};
  GetObjectW(base, sizeof(bm), &bm);
  const int cw = bm.bmWidth;
  const int ch = bm.bmHeight;
  const int sw = (std::max)(1, static_cast<int>(sel.right - sel.left));
  const int sh = (std::max)(1, static_cast<int>(sel.bottom - sel.top));

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HGDIOBJ old = SelectObject(mem, base);
  // Temporarily map annotation coords: draw relative to selection origin,
  // scaled into crop pixels via SetWorldTransform.
  Gdiplus::Graphics g(mem);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.ScaleTransform(static_cast<Gdiplus::REAL>(cw) / sw,
                   static_cast<Gdiplus::REAL>(ch) / sh);
  g.TranslateTransform(static_cast<Gdiplus::REAL>(-sel.left),
                       static_cast<Gdiplus::REAL>(-sel.top));

  // Re-draw annotations into the crop DC via a helper that uses Graphics.
  // We can't call DrawOneAnnotation (uses full-overlay coords) without offset —
  // already applied via transform, so call drawing with overlay coords.
  for (const auto& a : annotations_) {
    // Local copy of draw logic with `g` already transformed.
    Gdiplus::Pen pen(GpColor(a.color), static_cast<Gdiplus::REAL>(a.width));
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                   Gdiplus::DashCapRound);
    if (a.kind == Annotation::kRect) {
      const int l = (std::min)(a.start.x, a.end.x);
      const int t = (std::min)(a.start.y, a.end.y);
      g.DrawRectangle(&pen, l, t, abs(a.end.x - a.start.x),
                      abs(a.end.y - a.start.y));
    } else if (a.kind == Annotation::kEllipse) {
      const int l = (std::min)(a.start.x, a.end.x);
      const int t = (std::min)(a.start.y, a.end.y);
      g.DrawEllipse(&pen, l, t, abs(a.end.x - a.start.x),
                    abs(a.end.y - a.start.y));
    } else if (a.kind == Annotation::kArrow) {
      g.DrawLine(&pen, static_cast<INT>(a.start.x), static_cast<INT>(a.start.y), static_cast<INT>(a.end.x), static_cast<INT>(a.end.y));
      const double angle =
          atan2(static_cast<double>(a.end.y - a.start.y),
                static_cast<double>(a.end.x - a.start.x));
      const double alen = 10.0 + a.width * 2.0;
      Gdiplus::SolidBrush br(GpColor(a.color));
      Gdiplus::Point pts[3] = {
          {a.end.x, a.end.y},
          {static_cast<INT>(a.end.x - alen * cos(angle - 3.14159265 / 6)),
           static_cast<INT>(a.end.y - alen * sin(angle - 3.14159265 / 6))},
          {static_cast<INT>(a.end.x - alen * cos(angle + 3.14159265 / 6)),
           static_cast<INT>(a.end.y - alen * sin(angle + 3.14159265 / 6))}};
      g.FillPolygon(&br, pts, 3);
    } else if (a.kind == Annotation::kBrush) {
      for (size_t i = 1; i < a.points.size(); ++i) {
        g.DrawLine(&pen, static_cast<INT>(a.points[i - 1].x),
                   static_cast<INT>(a.points[i - 1].y),
                   static_cast<INT>(a.points[i].x),
                   static_cast<INT>(a.points[i].y));
      }
    } else if (a.kind == Annotation::kMosaic) {
      const int block = (std::max)(6, a.width * 3);
      for (const auto& pt : a.points) {
        const int gx = (pt.x / block) * block;
        const int gy = (pt.y / block) * block;
        const COLORREF c = SampleFrozen(gx + block / 2, gy + block / 2);
        Gdiplus::SolidBrush br(GpColor(c));
        g.FillRectangle(&br, gx, gy, block, block);
      }
    } else if (a.kind == Annotation::kText || a.kind == Annotation::kEmoji) {
      const int px = 12 + a.width + (a.kind == Annotation::kEmoji ? 6 : 0);
      Gdiplus::FontFamily fam(a.kind == Annotation::kEmoji ? L"Segoe UI Emoji"
                                                           : L"Microsoft YaHei UI");
      Gdiplus::Font font(&fam, static_cast<Gdiplus::REAL>(px),
                         Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
      Gdiplus::SolidBrush br(GpColor(a.color));
      g.DrawString(a.text.c_str(), -1, &font,
                   Gdiplus::PointF(static_cast<Gdiplus::REAL>(a.start.x),
                                   static_cast<Gdiplus::REAL>(a.start.y)),
                   &br);
    }
  }

  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return base;
}

bool CaptureOverlay::FinishSelection(FinishAction action) {
  const RECT sel = CurrentSelectionRect();
  if (sel.right - sel.left < 4 || sel.bottom - sel.top < 4) {
    return false;
  }
  HBITMAP bmp = CompositeAnnotatedCrop(sel);
  if (!bmp) {
    LogLineA("[shot] crop failed");
    return false;
  }
  CopyBitmapToClipboardDib(bmp);

  auto default_shot_path = []() -> std::wstring {
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
    swprintf_s(name, L"region_%04d%02d%02d_%02d%02d%02d.png", st.wYear,
               st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return JoinPath(dir, name);
  };

  std::wstring path;
  bool pin = false;
  if (action == FinishAction::Copy) {
    if (auto_pin_) {
      path = default_shot_path();
      if (SaveBitmapToFile(bmp, path)) {
        pin = true;
      } else {
        path.clear();
      }
    }
  } else if (action == FinishAction::Save) {
    path = ShowSaveAsDialog();
    if (path.empty()) {
      path = default_shot_path();
    }
    if (!SaveBitmapToFile(bmp, path)) {
      DeleteObject(bmp);
      MessageBoxW(nullptr, L"截图保存失败，请检查路径或磁盘空间。", L"区域截图",
                  MB_OK | MB_ICONERROR);
      return false;
    }
  } else {
    path = default_shot_path();
    if (!SaveBitmapToFile(bmp, path)) {
      DeleteObject(bmp);
      MessageBoxW(nullptr, L"截图保存失败，无法钉到桌面。", L"区域截图",
                  MB_OK | MB_ICONERROR);
      return false;
    }
    pin = true;
  }
  DeleteObject(bmp);
  if (on_done_) {
    on_done_(path, pin);
  }
  return true;
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
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == p->self_pid) {
          return TRUE;
        }
        if (IsCloaked(hwnd)) {
          return TRUE;
        }
        RECT r{};
        if (!GetWindowRect(hwnd, &r)) {
          return TRUE;
        }
        if (p->pt.x < r.left || p->pt.x >= r.right || p->pt.y < r.top ||
            p->pt.y >= r.bottom) {
          return TRUE;
        }
        LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW) {
          return TRUE;
        }
        p->best = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&pack));
  if (!pack.best) {
    hover_ok_ = false;
    return;
  }
  RECT wr{};
  GetWindowRect(pack.best, &wr);
  POINT tl{wr.left, wr.top};
  POINT br{wr.right, wr.bottom};
  ScreenToClient(hwnd_, &tl);
  ScreenToClient(hwnd_, &br);
  hover_ = {tl.x, tl.y, br.x, br.y};
  hover_ok_ = true;
}

void CaptureOverlay::PaintToolbar(HDC hdc) {
  RECT rc{};
  GetClientRect(toolbar_, &rc);
  Gdiplus::Graphics g(hdc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  Gdiplus::SolidBrush bg(Gdiplus::Color(240, 245, 245, 245));
  g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
  Gdiplus::Pen edge(Gdiplus::Color(180, 180, 180), 1);
  g.DrawRectangle(&edge, 0, 0, rc.right - 1, rc.bottom - 1);

  auto label = [&](int id, const wchar_t* text, bool active) {
    RECT r = TbButtonRect(id);
    if (active) {
      Gdiplus::SolidBrush hi(Gdiplus::Color(60, 7, 193, 96));
      g.FillRectangle(&hi, static_cast<INT>(r.left), static_cast<INT>(r.top), static_cast<INT>(r.right - r.left), static_cast<INT>(r.bottom - r.top));
    }
    Gdiplus::FontFamily fam(L"Segoe UI");
    Gdiplus::Font font(&fam, 11, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush br(active ? Gdiplus::Color(255, 7, 193, 96)
                                  : Gdiplus::Color(255, 40, 40, 40));
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF box(static_cast<Gdiplus::REAL>(r.left),
                       static_cast<Gdiplus::REAL>(r.top),
                       static_cast<Gdiplus::REAL>(r.right - r.left),
                       static_cast<Gdiplus::REAL>(r.bottom - r.top));
    g.DrawString(text, -1, &font, box, &fmt, &br);
  };

  label(TbRect, L"□", tool_ == Tool::Rect);
  label(TbEllipse, L"○", tool_ == Tool::Ellipse);
  label(TbArrow, L"→", tool_ == Tool::Arrow);
  label(TbBrush, L"✎", tool_ == Tool::Brush);
  label(TbMosaic, L"▦", tool_ == Tool::Mosaic);
  label(TbText, L"A", tool_ == Tool::Text);
  label(TbEmoji, L"☺", tool_ == Tool::Emoji);
  label(TbUndo, L"↶", false);
  {
    RECT r = TbButtonRect(TbColor);
    Gdiplus::SolidBrush br(GpColor(stroke_color_));
    g.FillEllipse(&br, r.left + 8, r.top + 8, 16, 16);
  }
  {
    wchar_t buf[8];
    swprintf_s(buf, L"%d", stroke_width_);
    label(TbWidth, buf, false);
  }
  label(TbSave, L"💾", false);
  label(TbCancel, L"✕", false);
  label(TbCopy, L"✓", false);
}

LRESULT CALLBACK CaptureOverlay::ToolbarProc(HWND hwnd, UINT msg, WPARAM wparam,
                                             LPARAM lparam) {
  CaptureOverlay* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<CaptureOverlay*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    // toolbar_ must be valid before HandleToolbar's default branch forwards to
    // DefWindowProcW(toolbar_, …); otherwise WM_NCCREATE → DefWindowProcW(NULL)
    // → ERROR_INVALID_WINDOW_HANDLE (1400) and the window is never created.
    self->toolbar_ = hwnd;
  } else {
    self =
        reinterpret_cast<CaptureOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleToolbar(msg, wparam, lparam);
}

LRESULT CaptureOverlay::HandleToolbar(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(toolbar_, &ps);
      PaintToolbar(hdc);
      EndPaint(toolbar_, &ps);
      return 0;
    }
    case WM_LBUTTONUP: {
      const int id = HitToolbar(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (id) {
        OnToolbarCommand(id);
      }
      return 0;
    }
    case WM_DESTROY:
      toolbar_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(toolbar_, msg, wparam, lparam);
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
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (mode_ == Mode::Annotating && sel_ok_) {
        if (pt.x >= sel_.left && pt.x < sel_.right && pt.y >= sel_.top &&
            pt.y < sel_.bottom) {
          if (tool_ == Tool::Text) {
            const std::wstring text =
                PromptText(hwnd_, L"文字标注", L"输入文字：", L"");
            if (!text.empty()) {
              Annotation a;
              a.kind = Annotation::kText;
              a.start = a.end = pt;
              a.color = stroke_color_;
              a.width = stroke_width_;
              a.text = text;
              annotations_.push_back(std::move(a));
              InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
          }
          if (tool_ == Tool::Emoji) {
            Annotation a;
            a.kind = Annotation::kEmoji;
            a.start = a.end = pt;
            a.color = stroke_color_;
            a.width = stroke_width_;
            a.text = kEmojis[emoji_index_ % 8];
            ++emoji_index_;
            annotations_.push_back(std::move(a));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
          }
          if (tool_ == Tool::Rect || tool_ == Tool::Ellipse ||
              tool_ == Tool::Arrow || tool_ == Tool::Brush ||
              tool_ == Tool::Mosaic) {
            BeginDraft(tool_, pt);
            SetCapture(hwnd_);
            return 0;
          }
        }
        // Click outside selection → restart select.
        LeaveAnnotateMode();
      }
      dragging_ = true;
      start_ = end_ = pt;
      SetCapture(hwnd_);
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (drawing_ && has_draft_) {
        UpdateDraft(pt);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      if (dragging_) {
        end_ = pt;
      } else if (mode_ == Mode::Selecting) {
        UpdateHover(pt);
      }
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (drawing_ && has_draft_) {
        UpdateDraft(pt);
        CommitDraft();
        ReleaseCapture();
        return 0;
      }
      if (dragging_ && mode_ == Mode::Selecting) {
        dragging_ = false;
        end_ = pt;
        ReleaseCapture();
        CommitSelection();
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK:
      if (mode_ == Mode::Annotating) {
        FinishSelection(FinishAction::Copy);
        Cancel();
      }
      return 0;
    case WM_RBUTTONUP:
      Cancel();
      return 0;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        Cancel();
        return 0;
      }
      if (wparam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        UndoAnnotation();
        return 0;
      }
      if (wparam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        FinishSelection(FinishAction::Copy);
        Cancel();
        return 0;
      }
      if (wparam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        FinishSelection(FinishAction::Save);
        Cancel();
        return 0;
      }
      if (wparam == VK_RETURN) {
        FinishSelection(auto_pin_ ? FinishAction::Pin : FinishAction::Copy);
        Cancel();
        return 0;
      }
      return 0;
    case WM_DESTROY:
      DestroyToolbar();
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
  if (sel_ok_) {
    hole = sel_;
    have_hole = true;
  } else if (dragging_) {
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
    // Outer + inner border (Python _draw_selection_chrome).
    HPEN outer = CreatePen(PS_SOLID, 1, RGB(0, 120, 60));
    HGDIOBJ old_pen = SelectObject(hdc, outer);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, hole.left, hole.top, hole.right, hole.bottom);
    HPEN pen = CreatePen(PS_SOLID, 2, RGB(7, 193, 96));
    SelectObject(hdc, pen);
    Rectangle(hdc, hole.left + 1, hole.top + 1, hole.right - 1,
              hole.bottom - 1);
    SelectObject(hdc, old_pen);
    DeleteObject(pen);
    DeleteObject(outer);

    // Corner handles (skip hover-only chrome — handles only after commit or drag).
    if (sel_ok_ || dragging_) {
      constexpr int kHandle = 8;
      const POINT corners[4] = {
          {hole.left, hole.top},
          {hole.right - 1, hole.top},
          {hole.left, hole.bottom - 1},
          {hole.right - 1, hole.bottom - 1},
      };
      HBRUSH handle_br = CreateSolidBrush(RGB(7, 193, 96));
      HPEN handle_pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
      HGDIOBJ old_hpen = SelectObject(hdc, handle_pen);
      HGDIOBJ old_hbr = SelectObject(hdc, handle_br);
      for (const POINT& c : corners) {
        Rectangle(hdc, c.x - kHandle / 2, c.y - kHandle / 2,
                  c.x + kHandle / 2 + 1, c.y + kHandle / 2 + 1);
      }
      SelectObject(hdc, old_hpen);
      SelectObject(hdc, old_hbr);
      DeleteObject(handle_pen);
      DeleteObject(handle_br);
    }

    // Size badge: "W × H" above (or inside) the selection.
    {
      const int w = hole.right - hole.left;
      const int h = hole.bottom - hole.top;
      wchar_t label[64];
      swprintf_s(label, L"%d × %d", w, h);
      SIZE sz{};
      GetTextExtentPoint32W(hdc, label, static_cast<int>(wcslen(label)), &sz);
      const int pad_x = 8;
      const int pad_y = 4;
      const int bw = sz.cx + pad_x * 2;
      const int bh = sz.cy + pad_y * 2;
      int bx = hole.left;
      int by = hole.top - bh - 4;
      if (by < 0) {
        by = hole.top + 4;
      }
      RECT badge{bx, by, bx + bw, by + bh};
      HBRUSH badge_br = CreateSolidBrush(RGB(28, 28, 32));
      FillRect(hdc, &badge, badge_br);
      DeleteObject(badge_br);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(240, 240, 240));
      DrawTextW(hdc, label, -1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }

  if (sel_ok_) {
    DrawAnnotations(hdc);
  }

  if (mode_ == Mode::Selecting && !dragging_ && !hover_ok_) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT hint = rc;
    hint.top += 24;
    hint.bottom = hint.top + 28;
    DrawTextW(hdc,
              L"拖拽选区 · 松手后标注 · Ctrl+C 复制 · Ctrl+S 保存 · Esc 取消",
              -1, &hint, DT_CENTER | DT_SINGLELINE);
  }
}
