#include "capture_overlay.h"

#include "display_geom.h"
#include "log.h"
#include "mist_msg.h"
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
constexpr wchar_t kTbClass[] = L"DeskTidyNativeCaptureTb";
constexpr wchar_t kTipClass[] = L"DeskTidyNativeCaptureTip";
bool g_reg = false;
bool g_tb_reg = false;
bool g_tip_reg = false;

// WeChat / Snipaste parity: Esc must cancel even when toolbar stole focus or
// the overlay lost keyboard focus after ReleaseCapture.
CaptureOverlay* g_active_capture = nullptr;
HHOOK g_capture_esc_hook = nullptr;

LRESULT CALLBACK CaptureEscHookProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION &&
      (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) && g_active_capture) {
    const auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (kb && kb->vkCode == VK_ESCAPE) {
      HWND target = g_active_capture->Hwnd();
      if (target && IsWindow(target)) {
        PostMessageW(target, WM_KEYDOWN, VK_ESCAPE, 0);
        return 1;  // swallow — overlay owns Esc while capturing
      }
    }
  }
  return CallNextHookEx(g_capture_esc_hook, code, wparam, lparam);
}

void InstallCaptureEscHook(CaptureOverlay* self) {
  g_active_capture = self;
  if (!g_capture_esc_hook) {
    g_capture_esc_hook =
        SetWindowsHookExW(WH_KEYBOARD_LL, CaptureEscHookProc, nullptr, 0);
  }
}

void UninstallCaptureEscHook(CaptureOverlay* self) {
  if (g_active_capture == self) {
    g_active_capture = nullptr;
  }
  if (g_capture_esc_hook && !g_active_capture) {
    UnhookWindowsHookEx(g_capture_esc_hook);
    g_capture_esc_hook = nullptr;
  }
}

constexpr COLORREF kColors[] = {
    RGB(250, 81, 81),   // red
    RGB(250, 157, 59),  // orange
    RGB(20, 133, 238),  // blue
    RGB(7, 193, 96),    // green
    RGB(25, 25, 25),    // black
    RGB(255, 255, 255), // white
};
constexpr int kWidths[] = {2, 4, 7};
constexpr int kHandle = 8;
constexpr int kMinSel = 8;
constexpr int kEdgeHit = 6;

enum TbId {
  TbRect = 1,
  TbEllipse,
  TbArrow,
  TbBrush,
  TbMosaic,
  TbText,
  TbUndo,
  TbColor,
  TbWidth,
  TbCopy,
  TbSave,
  TbCancel,
  TbCount = TbCancel
};

// WeChat-style toolbar metrics @96 DPI (larger hit targets + glyphs).
constexpr int kTbBtn96 = 40;
constexpr int kTbPad96 = 8;
constexpr int kTbGap96 = 4;
constexpr int kTbSep96 = 8;
constexpr int kTbIconStroke96 = 2;
constexpr int kTbTipFont96 = 13;

int TbDpi() {
  const int d = SystemDpi();
  return d > 0 ? d : 96;
}

int TbSx(int px96) { return MulDiv(px96, TbDpi(), 96); }
int TbBtn() { return TbSx(kTbBtn96); }
int TbPad() { return TbSx(kTbPad96); }
int TbGap() { return TbSx(kTbGap96); }
int TbSep() { return TbSx(kTbSep96); }

LRESULT CALLBACK CaptureTipProc(HWND hwnd, UINT msg, WPARAM wparam,
                                LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      HBRUSH bg = CreateSolidBrush(RGB(45, 45, 45));
      FillRect(hdc, &rc, bg);
      DeleteObject(bg);
      wchar_t text[64]{};
      GetWindowTextW(hwnd, text, 64);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(255, 255, 255));
      HFONT tip_font = CreateFontW(
          -TbSx(kTbTipFont96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
      HGDIOBJ oldf = SelectObject(hdc, tip_font);
      DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(hdc, oldf);
      DeleteObject(tip_font);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

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

bool GetVisibleFrame(HWND hwnd, RECT* out) {
  if (!hwnd || !out) {
    return false;
  }
  // Top-level: DWM visible frame (excludes drop shadow). Children: GetWindowRect.
  HWND root = GetAncestor(hwnd, GA_ROOT);
  if (hwnd == root) {
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, out,
                                       sizeof(*out));
    if (SUCCEEDED(hr) && out->right > out->left && out->bottom > out->top) {
      return true;
    }
  }
  return GetWindowRect(hwnd, out) != FALSE;
}

bool IsDesktopShell(HWND hwnd) {
  wchar_t cls[64] = {};
  if (!GetClassNameW(hwnd, cls, 64)) {
    return false;
  }
  return _wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0 ||
         _wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
         _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
         _wcsicmp(cls, L"NotifyIconOverflowWindow") == 0;
}

bool RectEqual(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right &&
         a.bottom == b.bottom;
}

void InflatePaintRect(RECT* r) {
  if (!r) {
    return;
  }
  // Border + size badge above the hole.
  InflateRect(r, 6, 6);
  r->top -= 28;
}

HWND DeepestVisibleChild(HWND root, POINT screen) {
  if (!root) {
    return nullptr;
  }
  HWND cur = root;
  POINT pt = screen;
  ScreenToClient(cur, &pt);
  for (int guard = 0; guard < 32; ++guard) {
    HWND child = ChildWindowFromPointEx(
        cur, pt, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
    if (!child || child == cur) {
      break;
    }
    MapWindowPoints(cur, child, &pt, 1);
    cur = child;
  }
  return cur;
}

HWND PickHighlightWindow(HWND leaf) {
  if (!leaf) {
    return nullptr;
  }
  HWND root = GetAncestor(leaf, GA_ROOT);
  HWND cur = leaf;
  // Prefer the deepest window with a usable size (WeChat control snap). Walk
  // up past tiny chrome fragments.
  while (cur) {
    RECT r{};
    if (GetVisibleFrame(cur, &r)) {
      const int w = r.right - r.left;
      const int h = r.bottom - r.top;
      if (w >= 20 && h >= 16) {
        return cur;
      }
    }
    if (cur == root) {
      break;
    }
    HWND parent = GetParent(cur);
    if (!parent) {
      break;
    }
    cur = parent;
  }
  return root ? root : leaf;
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
  // WeChat order: tools | undo color width | ✓ save ✕
  const int order[] = {TbRect,  TbEllipse, TbArrow, TbBrush, TbMosaic,
                       TbText,  TbUndo,    TbColor, TbWidth, TbCopy,
                       TbSave,  TbCancel};
  int slot = 0;
  for (int i = 0; i < 12; ++i) {
    if (order[i] == id) {
      slot = i;
      break;
    }
  }
  const int btn = TbBtn();
  const int pad = TbPad();
  const int gap = TbGap();
  const int sep = TbSep();
  int x = pad + slot * (btn + gap);
  if (slot >= 6) {
    x += sep;
  }
  if (slot >= 9) {
    x += sep;
  }
  return {x, pad, x + btn, pad + btn};
}

int ToolbarWidth() {
  RECT last = TbButtonRect(TbCancel);
  return last.right + TbPad();
}

int ToolbarHeight() { return TbPad() * 2 + TbBtn(); }

Gdiplus::Color GpColor(COLORREF c, BYTE a = 255) {
  return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

const wchar_t* TbTipText(int id) {
  switch (id) {
    case TbRect:
      return L"矩形";
    case TbEllipse:
      return L"椭圆";
    case TbArrow:
      return L"箭头";
    case TbBrush:
      return L"画笔";
    case TbMosaic:
      return L"马赛克";
    case TbText:
      return L"文字";
    case TbUndo:
      return L"撤销";
    case TbColor:
      return L"颜色";
    case TbWidth:
      return L"粗细";
    case TbCopy:
      return L"完成";
    case TbSave:
      return L"另存为";
    case TbCancel:
      return L"退出";
    default:
      return L"";
  }
}

void DrawTbGlyph(Gdiplus::Graphics& g, int id, const RECT& r, bool active,
                 COLORREF swatch, int width_value) {
  const Gdiplus::REAL cx =
      static_cast<Gdiplus::REAL>(r.left + r.right) * 0.5f;
  const Gdiplus::REAL cy =
      static_cast<Gdiplus::REAL>(r.top + r.bottom) * 0.5f;
  const Gdiplus::REAL cell =
      static_cast<Gdiplus::REAL>((std::min)(r.right - r.left, r.bottom - r.top));
  // WeChat icons fill ~55% of the cell (was ~unicode 11px in 32px → too small).
  const Gdiplus::REAL s = cell * 0.28f;
  const Gdiplus::REAL stroke =
      static_cast<Gdiplus::REAL>((std::max)(2, TbSx(kTbIconStroke96)));
  const Gdiplus::Color ink =
      active ? Gdiplus::Color(255, 7, 193, 96) : Gdiplus::Color(255, 45, 45, 45);
  Gdiplus::Pen pen(ink, stroke);
  pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                 Gdiplus::DashCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  Gdiplus::SolidBrush br(ink);

  switch (id) {
    case TbRect:
      g.DrawRectangle(&pen, cx - s, cy - s * 0.75f, s * 2.f, s * 1.5f);
      break;
    case TbEllipse:
      g.DrawEllipse(&pen, cx - s, cy - s * 0.75f, s * 2.f, s * 1.5f);
      break;
    case TbArrow: {
      g.DrawLine(&pen, cx - s, cy + s * 0.55f, cx + s * 0.35f, cy - s * 0.55f);
      Gdiplus::PointF head[3] = {
          {cx + s, cy - s * 0.75f},
          {cx + s * 0.15f, cy - s * 0.55f},
          {cx + s * 0.55f, cy - s * 0.05f},
      };
      g.FillPolygon(&br, head, 3);
      break;
    }
    case TbBrush: {
      // Pen tip + barrel (WeChat-like).
      g.DrawLine(&pen, cx - s * 0.75f, cy + s * 0.75f, cx + s * 0.35f,
                 cy - s * 0.45f);
      Gdiplus::PointF tip[3] = {
          {cx + s * 0.85f, cy - s * 0.85f},
          {cx + s * 0.15f, cy - s * 0.55f},
          {cx + s * 0.55f, cy - s * 0.15f},
      };
      g.FillPolygon(&br, tip, 3);
      break;
    }
    case TbMosaic: {
      const Gdiplus::REAL step = s * 0.7f;
      for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
          if ((row + col) & 1) {
            continue;
          }
          g.FillRectangle(&br, cx - s + col * step, cy - s + row * step,
                          step * 0.85f, step * 0.85f);
        }
      }
      break;
    }
    case TbText: {
      Gdiplus::FontFamily fam(L"Segoe UI");
      Gdiplus::Font font(&fam, cell * 0.42f, Gdiplus::FontStyleBold,
                         Gdiplus::UnitPixel);
      Gdiplus::StringFormat fmt;
      fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
      fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
      Gdiplus::RectF box(static_cast<Gdiplus::REAL>(r.left),
                         static_cast<Gdiplus::REAL>(r.top),
                         static_cast<Gdiplus::REAL>(r.right - r.left),
                         static_cast<Gdiplus::REAL>(r.bottom - r.top));
      g.DrawString(L"A", -1, &font, box, &fmt, &br);
      break;
    }
    case TbUndo: {
      Gdiplus::REAL rad = s * 0.95f;
      g.DrawArc(&pen, cx - rad, cy - rad * 0.85f, rad * 2.f, rad * 2.f, 210.f,
                240.f);
      Gdiplus::PointF arrow[3] = {
          {cx - rad * 0.95f, cy - rad * 0.15f},
          {cx - rad * 0.25f, cy - rad * 0.85f},
          {cx - rad * 0.15f, cy - rad * 0.05f},
      };
      g.FillPolygon(&br, arrow, 3);
      break;
    }
    case TbColor: {
      const Gdiplus::REAL d = s * 1.55f;
      Gdiplus::SolidBrush fill(GpColor(swatch));
      g.FillEllipse(&fill, cx - d * 0.5f, cy - d * 0.5f, d, d);
      Gdiplus::Pen rim(Gdiplus::Color(160, 120, 120, 120), 1.f);
      g.DrawEllipse(&rim, cx - d * 0.5f, cy - d * 0.5f, d, d);
      break;
    }
    case TbWidth: {
      wchar_t buf[8];
      swprintf_s(buf, L"%d", width_value);
      Gdiplus::FontFamily fam(L"Segoe UI");
      Gdiplus::Font font(&fam, cell * 0.38f, Gdiplus::FontStyleBold,
                         Gdiplus::UnitPixel);
      Gdiplus::StringFormat fmt;
      fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
      fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
      Gdiplus::RectF box(static_cast<Gdiplus::REAL>(r.left),
                         static_cast<Gdiplus::REAL>(r.top),
                         static_cast<Gdiplus::REAL>(r.right - r.left),
                         static_cast<Gdiplus::REAL>(r.bottom - r.top));
      g.DrawString(buf, -1, &font, box, &fmt, &br);
      break;
    }
    case TbCopy: {
      Gdiplus::PointF chk[3] = {{cx - s * 0.85f, cy},
                                {cx - s * 0.15f, cy + s * 0.7f},
                                {cx + s * 0.95f, cy - s * 0.75f}};
      g.DrawLines(&pen, chk, 3);
      break;
    }
    case TbSave: {
      // Floppy / save glyph.
      g.DrawRectangle(&pen, cx - s, cy - s, s * 2.f, s * 2.f);
      g.FillRectangle(&br, cx - s * 0.55f, cy - s, s * 1.1f, s * 0.7f);
      Gdiplus::SolidBrush hole(Gdiplus::Color(255, 245, 245, 245));
      g.FillRectangle(&hole, cx - s * 0.55f, cy + s * 0.15f, s * 1.1f, s * 0.7f);
      break;
    }
    case TbCancel: {
      g.DrawLine(&pen, cx - s * 0.75f, cy - s * 0.75f, cx + s * 0.75f,
                 cy + s * 0.75f);
      g.DrawLine(&pen, cx + s * 0.75f, cy - s * 0.75f, cx - s * 0.75f,
                 cy + s * 0.75f);
      break;
    }
    default:
      break;
  }
}

}  // namespace

bool CaptureOverlay::BuildDimmedBackground() {
  if (dim_bmp_) {
    DeleteObject(dim_bmp_);
    dim_bmp_ = nullptr;
  }
  if (!bkg_bmp_ || bkg_w_ <= 0 || bkg_h_ <= 0) {
    return false;
  }
  HDC screen = GetDC(nullptr);
  HDC src = CreateCompatibleDC(screen);
  HDC dst = CreateCompatibleDC(screen);
  HBITMAP dim = CreateCompatibleBitmap(screen, bkg_w_, bkg_h_);
  if (!src || !dst || !dim) {
    if (dim) {
      DeleteObject(dim);
    }
    if (src) {
      DeleteDC(src);
    }
    if (dst) {
      DeleteDC(dst);
    }
    ReleaseDC(nullptr, screen);
    return false;
  }
  HGDIOBJ os = SelectObject(src, bkg_bmp_);
  HGDIOBJ od = SelectObject(dst, dim);
  BitBlt(dst, 0, 0, bkg_w_, bkg_h_, src, 0, 0, SRCCOPY);
  // One-shot full dim — hover paints reuse this instead of AlphaBlend every move.
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = 1;
  bi.bmiHeader.biHeight = 1;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC pixel_dc = CreateCompatibleDC(screen);
  HBITMAP pixel =
      CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (pixel_dc && pixel && bits) {
    *static_cast<DWORD*>(bits) = 0xFF000000;
    HGDIOBJ op = SelectObject(pixel_dc, pixel);
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 120;
    bf.AlphaFormat = 0;
    AlphaBlend(dst, 0, 0, bkg_w_, bkg_h_, pixel_dc, 0, 0, 1, 1, bf);
    SelectObject(pixel_dc, op);
  }
  if (pixel) {
    DeleteObject(pixel);
  }
  if (pixel_dc) {
    DeleteDC(pixel_dc);
  }
  SelectObject(src, os);
  SelectObject(dst, od);
  DeleteDC(src);
  DeleteDC(dst);
  ReleaseDC(nullptr, screen);
  dim_bmp_ = dim;
  return true;
}

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
  BuildDimmedBackground();
  return true;
}

void CaptureOverlay::ReleaseFrozenFrame() {
  if (bkg_bmp_) {
    DeleteObject(bkg_bmp_);
    bkg_bmp_ = nullptr;
  }
  if (dim_bmp_) {
    DeleteObject(dim_bmp_);
    dim_bmp_ = nullptr;
  }
  if (back_bmp_) {
    DeleteObject(back_bmp_);
    back_bmp_ = nullptr;
  }
  bkg_w_ = bkg_h_ = 0;
  back_w_ = back_h_ = 0;
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

void CaptureOverlay::RequestRepaint() {
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void CaptureOverlay::RequestRepaintRects(const RECT* a, const RECT* b) {
  if (!hwnd_) {
    return;
  }
  RECT u{};
  bool have = false;
  if (a && a->right > a->left && a->bottom > a->top) {
    u = *a;
    InflatePaintRect(&u);
    have = true;
  }
  if (b && b->right > b->left && b->bottom > b->top) {
    RECT bb = *b;
    InflatePaintRect(&bb);
    if (have) {
      UnionRect(&u, &u, &bb);
    } else {
      u = bb;
      have = true;
    }
  }
  if (!have) {
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  InvalidateRect(hwnd_, &u, FALSE);
}

HWND CaptureOverlay::HitCaptureTarget(POINT screen) const {
  // Do NOT skip whole self PID — DeskTidy settings/fences must stay hittable.
  // Only ignore the fullscreen overlay + its toolbar.
  struct Pack {
    HWND skip1;
    HWND skip2;
    POINT pt;
    HWND best;
  } pack{hwnd_, toolbar_, screen, nullptr};

  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* p = reinterpret_cast<Pack*>(lp);
        if (hwnd == p->skip1 || hwnd == p->skip2 || !IsWindowVisible(hwnd) ||
            IsIconic(hwnd)) {
          return TRUE;
        }
        if (IsCloaked(hwnd) || IsDesktopShell(hwnd)) {
          return TRUE;
        }
        LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) {
          return TRUE;
        }
        if (ex & WS_EX_TRANSPARENT) {
          return TRUE;
        }
        // Full-screen invisible click-through shells (no caption, no frame).
        LONG style = GetWindowLongW(hwnd, GWL_STYLE);
        if (!(style & WS_CAPTION) && (ex & WS_EX_NOACTIVATE) &&
            (ex & WS_EX_LAYERED)) {
          RECT fr{};
          if (GetVisibleFrame(hwnd, &fr)) {
            const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if ((fr.right - fr.left) >= vw - 2 &&
                (fr.bottom - fr.top) >= vh - 2) {
              return TRUE;
            }
          }
        }
        RECT r{};
        if (!GetVisibleFrame(hwnd, &r)) {
          return TRUE;
        }
        if (p->pt.x < r.left || p->pt.x >= r.right || p->pt.y < r.top ||
            p->pt.y >= r.bottom) {
          return TRUE;
        }
        p->best = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&pack));

  if (!pack.best) {
    return nullptr;
  }
  HWND leaf = DeepestVisibleChild(pack.best, screen);
  return PickHighlightWindow(leaf ? leaf : pack.best);
}

bool CaptureOverlay::ClientRectOfWindow(HWND target, RECT* out_client) const {
  if (!target || !out_client || !hwnd_) {
    return false;
  }
  RECT wr{};
  if (!GetVisibleFrame(target, &wr)) {
    return false;
  }
  POINT tl{wr.left, wr.top};
  POINT br{wr.right, wr.bottom};
  ScreenToClient(hwnd_, &tl);
  ScreenToClient(hwnd_, &br);
  *out_client = {tl.x, tl.y, br.x, br.y};
  return out_client->right > out_client->left &&
         out_client->bottom > out_client->top;
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
  resizing_ = false;
  resize_edge_ = ResizeEdge::None;
  hover_ok_ = false;
  hover_hwnd_ = nullptr;
  DestroyInlineText();
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
  InstallCaptureEscHook(this);
  return true;
}

void CaptureOverlay::Cancel() {
  UninstallCaptureEscHook(this);
  DestroyInlineText();
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
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kTbClass, L"",
      WS_POPUP, 0, 0, ToolbarWidth(), ToolbarHeight(), hwnd_, nullptr,
      instance_, this);
  if (toolbar_) {
    SetWindowLongPtrW(toolbar_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  }
}

void CaptureOverlay::DestroyToolbar() {
  HideToolbarTip();
  tb_hover_id_ = 0;
  if (toolbar_) {
    DestroyWindow(toolbar_);
    toolbar_ = nullptr;
  }
}

void CaptureOverlay::HideToolbarTip() {
  if (tip_hwnd_) {
    DestroyWindow(tip_hwnd_);
    tip_hwnd_ = nullptr;
  }
}

void CaptureOverlay::ShowToolbarTip(int id) {
  const wchar_t* text = TbTipText(id);
  if (!text || !text[0] || !toolbar_) {
    HideToolbarTip();
    return;
  }
  RECT btn = TbButtonRect(id);
  POINT origin{btn.left + (btn.right - btn.left) / 2, btn.bottom};
  ClientToScreen(toolbar_, &origin);

  HDC screen = GetDC(nullptr);
  HFONT font = CreateFontW(-TbSx(kTbTipFont96), 0, 0, 0, FW_SEMIBOLD, FALSE,
                           FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
  HGDIOBJ old = SelectObject(screen, font);
  SIZE sz{};
  GetTextExtentPoint32W(screen, text, static_cast<int>(wcslen(text)), &sz);
  SelectObject(screen, old);
  ReleaseDC(nullptr, screen);
  DeleteObject(font);

  const int pad_x = TbSx(10);
  const int pad_y = TbSx(6);
  const int tw = sz.cx + pad_x * 2;
  const int th = sz.cy + pad_y * 2;
  int x = origin.x - tw / 2;
  int y = origin.y + TbSx(6);
  const RECT vs = VirtualScreenRect();
  if (x < vs.left) {
    x = vs.left;
  }
  if (x + tw > vs.right) {
    x = vs.right - tw;
  }
  if (y + th > vs.bottom) {
    POINT above{btn.left + (btn.right - btn.left) / 2, btn.top};
    ClientToScreen(toolbar_, &above);
    y = above.y - th - TbSx(6);
  }

  if (!g_tip_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CaptureTipProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kTipClass;
    RegisterClassExW(&wc);
    g_tip_reg = true;
  }
  if (!tip_hwnd_) {
    tip_hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kTipClass, text, WS_POPUP, x, y, tw, th, toolbar_, nullptr, instance_,
        nullptr);
    if (tip_hwnd_) {
      SetLayeredWindowAttributes(tip_hwnd_, 0, 235, LWA_ALPHA);
    }
  } else {
    SetWindowTextW(tip_hwnd_, text);
    SetWindowPos(tip_hwnd_, HWND_TOPMOST, x, y, tw, th,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(tip_hwnd_, nullptr, FALSE);
  }
  if (tip_hwnd_) {
    ShowWindow(tip_hwnd_, SW_SHOWNOACTIVATE);
  }
}

void CaptureOverlay::UpdateToolbarHover(int id) {
  if (id == tb_hover_id_) {
    return;
  }
  tb_hover_id_ = id;
  if (toolbar_) {
    InvalidateRect(toolbar_, nullptr, FALSE);
  }
  if (id) {
    ShowToolbarTip(id);
  } else {
    HideToolbarTip();
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
  int y = origin.y + sel_.bottom + TbSx(10);
  const RECT vs = VirtualScreenRect();
  if (x + tw > vs.right) {
    x = vs.right - tw;
  }
  if (x < vs.left) {
    x = vs.left;
  }
  if (y + th > vs.bottom) {
    y = origin.y + sel_.top - th - TbSx(10);
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
  // Keep keyboard on the overlay so Esc / Ctrl+Z / Ctrl+C keep working
  // (toolbar is WS_EX_NOACTIVATE; hook is the fallback).
  if (hwnd_) {
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
  }
  RequestRepaint();
}

void CaptureOverlay::LeaveAnnotateMode() {
  DestroyInlineText();
  DestroyToolbar();
  mode_ = Mode::Selecting;
  tool_ = Tool::None;
  has_draft_ = false;
  drawing_ = false;
  resizing_ = false;
  resize_edge_ = ResizeEdge::None;
  annotations_.clear();
  sel_ok_ = false;
  hover_hwnd_ = nullptr;
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
    RequestRepaint();
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

bool CaptureOverlay::UpdateDraft(POINT pt) {
  if (!has_draft_) {
    return false;
  }
  bool changed = (draft_.end.x != pt.x || draft_.end.y != pt.y);
  draft_.end = pt;
  if (draft_.kind == Annotation::kBrush || draft_.kind == Annotation::kMosaic) {
    if (draft_.points.empty() ||
        abs(pt.x - draft_.points.back().x) + abs(pt.y - draft_.points.back().y) >=
            (draft_.kind == Annotation::kMosaic ? 8 : 2)) {
      draft_.points.push_back(pt);
      changed = true;
    }
  }
  return changed;
}

void CaptureOverlay::CommitDraft() {
  if (!has_draft_) {
    return;
  }
  annotations_.push_back(std::move(draft_));
  has_draft_ = false;
  drawing_ = false;
  RequestRepaint();
}

CaptureOverlay::ResizeEdge CaptureOverlay::HitResizeEdge(POINT pt) const {
  if (!sel_ok_) {
    return ResizeEdge::None;
  }
  const RECT& r = sel_;
  const bool near_l = abs(pt.x - r.left) <= kEdgeHit;
  const bool near_r = abs(pt.x - r.right) <= kEdgeHit;
  const bool near_t = abs(pt.y - r.top) <= kEdgeHit;
  const bool near_b = abs(pt.y - r.bottom) <= kEdgeHit;
  const bool in_x = pt.x >= r.left - kEdgeHit && pt.x <= r.right + kEdgeHit;
  const bool in_y = pt.y >= r.top - kEdgeHit && pt.y <= r.bottom + kEdgeHit;
  if (!in_x || !in_y) {
    return ResizeEdge::None;
  }
  if (near_t && near_l) {
    return ResizeEdge::NW;
  }
  if (near_t && near_r) {
    return ResizeEdge::NE;
  }
  if (near_b && near_l) {
    return ResizeEdge::SW;
  }
  if (near_b && near_r) {
    return ResizeEdge::SE;
  }
  if (near_t) {
    return ResizeEdge::N;
  }
  if (near_b) {
    return ResizeEdge::S;
  }
  if (near_l) {
    return ResizeEdge::W;
  }
  if (near_r) {
    return ResizeEdge::E;
  }
  return ResizeEdge::None;
}

void CaptureOverlay::BeginResize(ResizeEdge edge, POINT pt) {
  resizing_ = true;
  resize_edge_ = edge;
  resize_grab_ = pt;
  resize_origin_ = sel_;
  SetCapture(hwnd_);
}

bool CaptureOverlay::ApplyResize(POINT pt) {
  if (!resizing_ || resize_edge_ == ResizeEdge::None) {
    return false;
  }
  RECT r = resize_origin_;
  const int dx = pt.x - resize_grab_.x;
  const int dy = pt.y - resize_grab_.y;
  switch (resize_edge_) {
    case ResizeEdge::N:
      r.top += dy;
      break;
    case ResizeEdge::S:
      r.bottom += dy;
      break;
    case ResizeEdge::E:
      r.right += dx;
      break;
    case ResizeEdge::W:
      r.left += dx;
      break;
    case ResizeEdge::NE:
      r.top += dy;
      r.right += dx;
      break;
    case ResizeEdge::NW:
      r.top += dy;
      r.left += dx;
      break;
    case ResizeEdge::SE:
      r.bottom += dy;
      r.right += dx;
      break;
    case ResizeEdge::SW:
      r.bottom += dy;
      r.left += dx;
      break;
    default:
      break;
  }
  if (r.left > r.right) {
    std::swap(r.left, r.right);
  }
  if (r.top > r.bottom) {
    std::swap(r.top, r.bottom);
  }
  if (r.right - r.left < kMinSel) {
    if (resize_edge_ == ResizeEdge::W || resize_edge_ == ResizeEdge::NW ||
        resize_edge_ == ResizeEdge::SW) {
      r.left = r.right - kMinSel;
    } else {
      r.right = r.left + kMinSel;
    }
  }
  if (r.bottom - r.top < kMinSel) {
    if (resize_edge_ == ResizeEdge::N || resize_edge_ == ResizeEdge::NE ||
        resize_edge_ == ResizeEdge::NW) {
      r.top = r.bottom - kMinSel;
    } else {
      r.bottom = r.top + kMinSel;
    }
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  r.left = (std::max)(client.left, (std::min)(r.left, client.right - kMinSel));
  r.top = (std::max)(client.top, (std::min)(r.top, client.bottom - kMinSel));
  r.right = (std::max)(r.left + kMinSel, (std::min)(r.right, client.right));
  r.bottom = (std::max)(r.top + kMinSel, (std::min)(r.bottom, client.bottom));
  if (RectEqual(r, sel_)) {
    return false;
  }
  sel_ = r;
  start_ = {sel_.left, sel_.top};
  end_ = {sel_.right, sel_.bottom};
  return true;
}

void CaptureOverlay::EndResize() {
  resizing_ = false;
  resize_edge_ = ResizeEdge::None;
  ReleaseCapture();
  PlaceToolbar();
}

void CaptureOverlay::UpdateResizeCursor(POINT pt) const {
  if (mode_ != Mode::Annotating || !sel_ok_) {
    return;
  }
  LPCWSTR idc = IDC_ARROW;
  switch (HitResizeEdge(pt)) {
    case ResizeEdge::N:
    case ResizeEdge::S:
      idc = IDC_SIZENS;
      break;
    case ResizeEdge::E:
    case ResizeEdge::W:
      idc = IDC_SIZEWE;
      break;
    case ResizeEdge::NE:
    case ResizeEdge::SW:
      idc = IDC_SIZENESW;
      break;
    case ResizeEdge::NW:
    case ResizeEdge::SE:
      idc = IDC_SIZENWSE;
      break;
    default:
      idc = IDC_ARROW;
      break;
  }
  SetCursor(LoadCursor(nullptr, idc));
}

void CaptureOverlay::DestroyInlineText() {
  if (text_edit_) {
    if (text_edit_prev_) {
      SetWindowLongPtrW(text_edit_, GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(text_edit_prev_));
      text_edit_prev_ = nullptr;
    }
    DestroyWindow(text_edit_);
    text_edit_ = nullptr;
  }
}

void CaptureOverlay::BeginInlineText(POINT pt) {
  DestroyInlineText();
  text_anchor_ = pt;
  const int w = 200;
  const int h = 28;
  text_edit_ = CreateWindowExW(
      0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, pt.x,
      pt.y, w, h, hwnd_, nullptr, instance_, nullptr);
  if (!text_edit_) {
    return;
  }
  SetWindowLongPtrW(text_edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  text_edit_prev_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      text_edit_, GWLP_WNDPROC,
      reinterpret_cast<LONG_PTR>(CaptureOverlay::TextEditProc)));
  HFONT font = CreateFontW(
      16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  if (font) {
    SendMessageW(text_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    // Font owned by edit for its life; delete on destroy via GWLP.
    SetPropW(text_edit_, L"DeskTidyTextFont", font);
  }
  SetFocus(text_edit_);
}

void CaptureOverlay::CommitInlineText() {
  if (!text_edit_) {
    return;
  }
  const int n = GetWindowTextLengthW(text_edit_);
  std::wstring text;
  if (n > 0) {
    text.resize(static_cast<size_t>(n) + 1);
    GetWindowTextW(text_edit_, text.data(), n + 1);
    text.resize(static_cast<size_t>(n));
  }
  DestroyInlineText();
  if (text.empty()) {
    return;
  }
  Annotation a;
  a.kind = Annotation::kText;
  a.start = a.end = text_anchor_;
  a.color = stroke_color_;
  a.width = stroke_width_;
  a.text = std::move(text);
  annotations_.push_back(std::move(a));
  RequestRepaint();
}

void CaptureOverlay::CancelInlineText() { DestroyInlineText(); }

LRESULT CALLBACK CaptureOverlay::TextEditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                              LPARAM lparam) {
  auto* self =
      reinterpret_cast<CaptureOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  WNDPROC prev = self ? self->text_edit_prev_ : nullptr;
  if (msg == WM_KEYDOWN) {
    if (wparam == VK_RETURN) {
      if (self) {
        self->CommitInlineText();
      }
      return 0;
    }
    if (wparam == VK_ESCAPE) {
      if (self) {
        self->CancelInlineText();
      }
      return 0;
    }
  }
  if (msg == WM_DESTROY) {
    HFONT font = reinterpret_cast<HFONT>(RemovePropW(hwnd, L"DeskTidyTextFont"));
    if (font) {
      DeleteObject(font);
    }
  }
  if (prev) {
    return CallWindowProcW(prev, hwnd, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int CaptureOverlay::HitToolbar(int x, int y) const {
  for (int id = TbRect; id <= TbCancel; ++id) {
    RECT r = TbButtonRect(id);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
      return id;
    }
  }
  return 0;
}

void CaptureOverlay::OnToolbarCommand(int id) {
  if (text_edit_) {
    CommitInlineText();
  }
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
    case TbCopy:
      FinishSelection(FinishAction::Copy);
      Cancel();
      return;
    case TbSave:
      FinishSelection(FinishAction::Save);
      Cancel();
      return;
    case TbCancel:
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
    g.DrawLine(&pen, static_cast<INT>(a.start.x), static_cast<INT>(a.start.y),
               static_cast<INT>(a.end.x), static_cast<INT>(a.end.y));
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
  } else if (a.kind == Annotation::kText) {
    const int px = 12 + a.width;
    Gdiplus::FontFamily fam(L"Microsoft YaHei UI");
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
  Gdiplus::Graphics g(mem);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.ScaleTransform(static_cast<Gdiplus::REAL>(cw) / sw,
                   static_cast<Gdiplus::REAL>(ch) / sh);
  g.TranslateTransform(static_cast<Gdiplus::REAL>(-sel.left),
                       static_cast<Gdiplus::REAL>(-sel.top));

  for (const auto& a : annotations_) {
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
      g.DrawLine(&pen, static_cast<INT>(a.start.x), static_cast<INT>(a.start.y),
                 static_cast<INT>(a.end.x), static_cast<INT>(a.end.y));
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
    } else if (a.kind == Annotation::kText) {
      const int px = 12 + a.width;
      Gdiplus::FontFamily fam(L"Microsoft YaHei UI");
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
  CommitInlineText();
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
    swprintf_s(name, L"shot_%04d%02d%02d_%02d%02d%02d.png", st.wYear, st.wMonth,
               st.wDay, st.wHour, st.wMinute, st.wSecond);
    return JoinPath(dir, name);
  };

  std::wstring path;
  bool pin = false;
  if (action == FinishAction::Copy) {
    // Clipboard already filled; only persist when auto-pin is on.
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
      MistAlert(nullptr, L"区域截图", L"截图保存失败，请检查路径或磁盘空间。",
                MistMsgTone::Danger);
      return false;
    }
  } else {
    path = default_shot_path();
    if (!SaveBitmapToFile(bmp, path)) {
      DeleteObject(bmp);
      MistAlert(nullptr, L"区域截图", L"截图保存失败，无法钉到桌面。",
                MistMsgTone::Danger);
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

bool CaptureOverlay::UpdateHover(POINT client) {
  POINT screen = client;
  ClientToScreen(hwnd_, &screen);
  HWND target = HitCaptureTarget(screen);

  if (!target) {
    if (!hover_ok_) {
      return false;
    }
    hover_ok_ = false;
    hover_hwnd_ = nullptr;
    hover_ = {};
    return true;
  }
  // Same HWND → keep rect (avoids DWM bounds jitter flicker while moving).
  if (hover_ok_ && target == hover_hwnd_) {
    return false;
  }
  RECT next{};
  if (!ClientRectOfWindow(target, &next)) {
    if (!hover_ok_) {
      return false;
    }
    hover_ok_ = false;
    hover_hwnd_ = nullptr;
    hover_ = {};
    return true;
  }
  if (hover_ok_ && RectEqual(hover_, next)) {
    hover_hwnd_ = target;
    return false;
  }
  hover_ = next;
  hover_ok_ = true;
  hover_hwnd_ = target;
  return true;
}

void CaptureOverlay::PaintToolbar(HDC hdc) {
  RECT rc{};
  GetClientRect(toolbar_, &rc);
  Gdiplus::Graphics g(hdc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

  // Soft card like WeChat capture bar.
  const Gdiplus::REAL radius =
      static_cast<Gdiplus::REAL>((std::max)(6, TbSx(8)));
  Gdiplus::GraphicsPath path;
  const Gdiplus::REAL w = static_cast<Gdiplus::REAL>(rc.right);
  const Gdiplus::REAL h = static_cast<Gdiplus::REAL>(rc.bottom);
  path.AddArc(0.f, 0.f, radius * 2, radius * 2, 180.f, 90.f);
  path.AddArc(w - radius * 2, 0.f, radius * 2, radius * 2, 270.f, 90.f);
  path.AddArc(w - radius * 2, h - radius * 2, radius * 2, radius * 2, 0.f,
              90.f);
  path.AddArc(0.f, h - radius * 2, radius * 2, radius * 2, 90.f, 90.f);
  path.CloseFigure();
  Gdiplus::SolidBrush bg(Gdiplus::Color(250, 250, 250, 250));
  g.FillPath(&bg, &path);
  Gdiplus::Pen edge(Gdiplus::Color(200, 200, 200), 1.f);
  g.DrawPath(&edge, &path);

  auto paint_btn = [&](int id, bool active) {
    RECT r = TbButtonRect(id);
    const bool hover = (tb_hover_id_ == id);
    if (active || hover) {
      Gdiplus::SolidBrush hi(active ? Gdiplus::Color(55, 7, 193, 96)
                                    : Gdiplus::Color(40, 0, 0, 0));
      Gdiplus::GraphicsPath cell;
      const Gdiplus::REAL x = static_cast<Gdiplus::REAL>(r.left);
      const Gdiplus::REAL y = static_cast<Gdiplus::REAL>(r.top);
      const Gdiplus::REAL bw =
          static_cast<Gdiplus::REAL>(r.right - r.left);
      const Gdiplus::REAL bh =
          static_cast<Gdiplus::REAL>(r.bottom - r.top);
      const Gdiplus::REAL rad =
          static_cast<Gdiplus::REAL>((std::max)(4, TbSx(6)));
      cell.AddArc(x, y, rad * 2, rad * 2, 180.f, 90.f);
      cell.AddArc(x + bw - rad * 2, y, rad * 2, rad * 2, 270.f, 90.f);
      cell.AddArc(x + bw - rad * 2, y + bh - rad * 2, rad * 2, rad * 2, 0.f,
                  90.f);
      cell.AddArc(x, y + bh - rad * 2, rad * 2, rad * 2, 90.f, 90.f);
      cell.CloseFigure();
      g.FillPath(&hi, &cell);
    }
    DrawTbGlyph(g, id, r, active, stroke_color_, stroke_width_);
  };

  paint_btn(TbRect, tool_ == Tool::Rect);
  paint_btn(TbEllipse, tool_ == Tool::Ellipse);
  paint_btn(TbArrow, tool_ == Tool::Arrow);
  paint_btn(TbBrush, tool_ == Tool::Brush);
  paint_btn(TbMosaic, tool_ == Tool::Mosaic);
  paint_btn(TbText, tool_ == Tool::Text);
  paint_btn(TbUndo, false);
  paint_btn(TbColor, false);
  paint_btn(TbWidth, false);
  paint_btn(TbCopy, false);
  paint_btn(TbSave, false);
  paint_btn(TbCancel, false);
}

LRESULT CALLBACK CaptureOverlay::ToolbarProc(HWND hwnd, UINT msg, WPARAM wparam,
                                             LPARAM lparam) {
  CaptureOverlay* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<CaptureOverlay*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
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
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tme{};
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE;
      tme.hwndTrack = toolbar_;
      TrackMouseEvent(&tme);
      UpdateToolbarHover(
          HitToolbar(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)));
      return 0;
    }
    case WM_MOUSELEAVE:
      UpdateToolbarHover(0);
      return 0;
    case WM_LBUTTONUP: {
      const int id = HitToolbar(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (id) {
        OnToolbarCommand(id);
      }
      if (hwnd_) {
        SetFocus(hwnd_);
      }
      return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (wparam == VK_ESCAPE) {
        Cancel();
        return 0;
      }
      break;
    case WM_DESTROY:
      HideToolbarTip();
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
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT && mode_ == Mode::Annotating) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        UpdateResizeCursor(pt);
        return TRUE;
      }
      break;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd_, &ps);
      Paint(hdc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (text_edit_) {
        RECT er{};
        GetWindowRect(text_edit_, &er);
        POINT sp = pt;
        ClientToScreen(hwnd_, &sp);
        if (sp.x < er.left || sp.x >= er.right || sp.y < er.top ||
            sp.y >= er.bottom) {
          CommitInlineText();
        } else {
          return 0;
        }
      }
      if (mode_ == Mode::Annotating && sel_ok_) {
        const ResizeEdge edge = HitResizeEdge(pt);
        if (edge != ResizeEdge::None) {
          BeginResize(edge, pt);
          return 0;
        }
        if (pt.x >= sel_.left && pt.x < sel_.right && pt.y >= sel_.top &&
            pt.y < sel_.bottom) {
          if (tool_ == Tool::Text) {
            BeginInlineText(pt);
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
        LeaveAnnotateMode();
        RequestRepaint();
      }
      dragging_ = true;
      start_ = end_ = pt;
      SetCapture(hwnd_);
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (resizing_) {
        if (ApplyResize(pt)) {
          PlaceToolbar();
          RequestRepaint();
        }
        return 0;
      }
      if (drawing_ && has_draft_) {
        if (UpdateDraft(pt)) {
          RequestRepaint();
        }
        return 0;
      }
      if (dragging_) {
        if (end_.x != pt.x || end_.y != pt.y) {
          end_ = pt;
          RequestRepaint();
        }
        return 0;
      }
      if (mode_ == Mode::Selecting) {
        const RECT old_hover = hover_;
        const bool old_ok = hover_ok_;
        if (UpdateHover(pt)) {
          // Dirty union only — full-screen invalidate every move caused flicker.
          RequestRepaintRects(old_ok ? &old_hover : nullptr,
                              hover_ok_ ? &hover_ : nullptr);
        }
        return 0;
      }
      if (mode_ == Mode::Annotating) {
        UpdateResizeCursor(pt);
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (resizing_) {
        ApplyResize(pt);
        EndResize();
        RequestRepaint();
        return 0;
      }
      if (drawing_ && has_draft_) {
        UpdateDraft(pt);
        CommitDraft();
        ReleaseCapture();
        return 0;
      }
      if (dragging_) {
        end_ = pt;
        dragging_ = false;
        ReleaseCapture();
        CommitSelection();
        return 0;
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      if (mode_ == Mode::Annotating && sel_ok_) {
        FinishSelection(FinishAction::Copy);
        Cancel();
      }
      return 0;
    }
    case WM_RBUTTONUP:
      Cancel();
      return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (wparam == VK_ESCAPE) {
        // WeChat: Esc exits capture. While typing, first Esc only closes the
        // text box; a second Esc (or Esc with no editor) cancels the shot.
        if (text_edit_) {
          CancelInlineText();
          return 0;
        }
        Cancel();
        return 0;
      }
      if (msg == WM_SYSKEYDOWN) {
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
        if (text_edit_) {
          CommitInlineText();
          return 0;
        }
        FinishSelection(auto_pin_ ? FinishAction::Pin : FinishAction::Copy);
        Cancel();
        return 0;
      }
      return 0;
    case WM_DESTROY:
      UninstallCaptureEscHook(this);
      DestroyInlineText();
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
  const int cw = (std::max)(1, static_cast<int>(rc.right - rc.left));
  const int ch = (std::max)(1, static_cast<int>(rc.bottom - rc.top));

  // Reuse backbuffer — recreating a virtual-screen DIB every mouse-move paints
  // was a primary flicker source.
  if (!back_bmp_ || back_w_ != cw || back_h_ != ch) {
    if (back_bmp_) {
      DeleteObject(back_bmp_);
      back_bmp_ = nullptr;
    }
    HDC screen = GetDC(nullptr);
    back_bmp_ = CreateCompatibleBitmap(screen ? screen : hdc, cw, ch);
    if (screen) {
      ReleaseDC(nullptr, screen);
    }
    back_w_ = cw;
    back_h_ = ch;
  }
  if (!back_bmp_) {
    PaintContent(hdc);
    return;
  }
  HDC mem = CreateCompatibleDC(hdc);
  HGDIOBJ old = SelectObject(mem, back_bmp_);
  PaintContent(mem);
  BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old);
  DeleteDC(mem);
}

void CaptureOverlay::PaintContent(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int cw = rc.right - rc.left;
  const int ch = rc.bottom - rc.top;

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

  HDC mem = CreateCompatibleDC(hdc);
  // Prefer pre-dimmed freeze: one BitBlt, no per-frame AlphaBlend.
  HBITMAP base = dim_bmp_ ? dim_bmp_ : bkg_bmp_;
  if (base && bkg_w_ > 0 && bkg_h_ > 0) {
    HGDIOBJ old = SelectObject(mem, base);
    if (cw == bkg_w_ && ch == bkg_h_) {
      BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    } else {
      SetStretchBltMode(hdc, COLORONCOLOR);
      StretchBlt(hdc, 0, 0, cw, ch, mem, 0, 0, bkg_w_, bkg_h_, SRCCOPY);
    }
    SelectObject(mem, old);
    if (have_hole && bkg_bmp_) {
      // Punch bright hole from pristine freeze (1:1 when sizes match).
      SelectObject(mem, bkg_bmp_);
      const int hl = (std::max)(0, static_cast<int>(hole.left));
      const int ht = (std::max)(0, static_cast<int>(hole.top));
      const int hole_r = (std::min)(cw, static_cast<int>(hole.right));
      const int hb = (std::min)(ch, static_cast<int>(hole.bottom));
      const int hw = hole_r - hl;
      const int hh = hb - ht;
      if (hw > 0 && hh > 0) {
        if (cw == bkg_w_ && ch == bkg_h_) {
          BitBlt(hdc, hl, ht, hw, hh, mem, hl, ht, SRCCOPY);
        } else {
          const int sx = hl * bkg_w_ / cw;
          const int sy = ht * bkg_h_ / ch;
          const int sw = hw * bkg_w_ / cw;
          const int sh = hh * bkg_h_ / ch;
          StretchBlt(hdc, hl, ht, hw, hh, mem, sx, sy, sw, sh, SRCCOPY);
        }
      }
      SelectObject(mem, old);
    } else if (!dim_bmp_ && bkg_bmp_) {
      // Fallback: live dim if prebuild failed.
      HRGN dim = CreateRectRgn(0, 0, cw, ch);
      if (have_hole) {
        HRGN hole_rgn =
            CreateRectRgn(hole.left, hole.top, hole.right, hole.bottom);
        CombineRgn(dim, dim, hole_rgn, RGN_DIFF);
        DeleteObject(hole_rgn);
      }
      DimRegion(hdc, dim, cw, ch);
      DeleteObject(dim);
    }
  } else {
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(hdc, &rc, black);
    DeleteObject(black);
  }
  DeleteDC(mem);

  if (have_hole) {
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

    if (sel_ok_ || dragging_) {
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
      // Mid-edge handles (WeChat 8-way resize).
      const POINT mids[4] = {
          {(hole.left + hole.right) / 2, hole.top},
          {(hole.left + hole.right) / 2, hole.bottom - 1},
          {hole.left, (hole.top + hole.bottom) / 2},
          {hole.right - 1, (hole.top + hole.bottom) / 2},
      };
      for (const POINT& c : mids) {
        Rectangle(hdc, c.x - kHandle / 2, c.y - kHandle / 2,
                  c.x + kHandle / 2 + 1, c.y + kHandle / 2 + 1);
      }
      SelectObject(hdc, old_hpen);
      SelectObject(hdc, old_hbr);
      DeleteObject(handle_pen);
      DeleteObject(handle_br);
    }

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
              L"拖拽选区 · 悬停窗口单击 · 松手后标注 · Ctrl+C 复制 · Esc 取消",
              -1, &hint, DT_CENTER | DT_SINGLELINE);
  }
}
