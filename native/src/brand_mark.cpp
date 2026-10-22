#include "brand_mark.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")

namespace {

void EnsureGdiplus() {
  static ULONG_PTR token = 0;
  if (!token) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
  }
}

void AddRoundRect(Gdiplus::GraphicsPath* path, float x, float y, float w,
                  float h, float radius) {
  const float r = (std::min)(radius, (std::min)(w, h) * 0.5f);
  path->AddArc(x, y, r * 2, r * 2, 180, 90);
  path->AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
  path->AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
  path->AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90);
  path->CloseFigure();
}

Gdiplus::Color WithAlpha(COLORREF c, int alpha) {
  return Gdiplus::Color(static_cast<BYTE>((std::max)(0, (std::min)(255, alpha))),
                        GetRValue(c), GetGValue(c), GetBValue(c));
}

COLORREF Lighten(COLORREF c, int percent) {
  // percent > 100 lightens toward white (Qt lighter(118) ≈ +18%).
  const int r = GetRValue(c);
  const int g = GetGValue(c);
  const int b = GetBValue(c);
  const int nr = r + (255 - r) * (percent - 100) / 100;
  const int ng = g + (255 - g) * (percent - 100) / 100;
  const int nb = b + (255 - b) * (percent - 100) / 100;
  return RGB((std::min)(255, nr), (std::min)(255, ng), (std::min)(255, nb));
}

}  // namespace

HICON CreateBrandMarkIcon(int size, COLORREF accent) {
  size = (std::max)(8, size);
  EnsureGdiplus();

  Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
  Gdiplus::Graphics g(&bmp);
  g.Clear(Gdiplus::Color(0, 0, 0, 0));
  g.SetSmoothingMode(size >= 20 ? Gdiplus::SmoothingModeAntiAlias
                                : Gdiplus::SmoothingModeNone);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

  constexpr float kBase = 40.f;
  const float scale = static_cast<float>(size) / kBase;
  const float radius = (std::max)(1.f, 6.f * scale);

  // (x, y, w, h, alpha) on 40×40 canvas — back → front (Python brand_mark).
  const struct {
    float x, y, w, h;
    int alpha;
  } layers[] = {
      {6, 10, 28, 22, 70},
      {4, 6, 28, 22, 140},
      {2, 2, 28, 22, 255},
  };

  for (const auto& layer : layers) {
    const float sx = layer.x * scale;
    const float sy = layer.y * scale;
    const float sw = layer.w * scale;
    const float sh = layer.h * scale;
    Gdiplus::GraphicsPath path;
    AddRoundRect(&path, sx, sy, sw, sh, radius);
    const COLORREF top_c = Lighten(accent, 118);
    Gdiplus::LinearGradientBrush brush(
        Gdiplus::PointF(sx, sy), Gdiplus::PointF(sx + sw, sy + sh),
        WithAlpha(top_c, layer.alpha), WithAlpha(accent, layer.alpha));
    g.FillPath(&brush, &path);
    Gdiplus::Pen pen(Gdiplus::Color(
                         static_cast<BYTE>((std::min)(180, layer.alpha)), 255, 255,
                         255),
                     (std::max)(0.8f, 1.f * scale));
    g.DrawPath(&pen, &path);
  }

  // White status pip.
  const float px = 22.f * scale;
  const float py = 8.f * scale;
  const float pw = (std::max)(1.f, 5.f * scale);
  const float ph = (std::max)(1.f, 5.f * scale);
  Gdiplus::SolidBrush pip(Gdiplus::Color(230, 255, 255, 255));
  g.FillEllipse(&pip, px, py, pw, ph);

  HICON icon = nullptr;
  if (bmp.GetHICON(&icon) != Gdiplus::Ok) {
    return nullptr;
  }
  return icon;
}

void ApplyBrandWindowIcons(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) {
    return;
  }
  // Process-lifetime shared icons (destroyed on process exit).
  // Avoid names "small"/"big" — rpcndr.h macros break MSVC.
  static HICON icon_lg = nullptr;
  static HICON icon_sm = nullptr;
  if (!icon_lg) {
    icon_lg = CreateBrandMarkIcon(32);
  }
  if (!icon_sm) {
    icon_sm = CreateBrandMarkIcon(16);
  }
  if (icon_lg) {
    SendMessageW(hwnd, WM_SETICON, ICON_BIG,
                 reinterpret_cast<LPARAM>(icon_lg));
  }
  if (icon_sm) {
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                 reinterpret_cast<LPARAM>(icon_sm));
  }
}
