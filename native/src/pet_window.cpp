#include "pet_window.h"

#include "desktop_shell.h"
#include "pet_lines.h"
#include "display_geom.h"
#include "pet_ledges.h"
#include "toast.h"
#include "paths_util.h"
#include "tool_paths.h"
#include "log.h"

#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace {
// Ledge scan (EnumWindows) cadence cap — climb probing reuses the last scan
// for this long before re-enumerating top-level windows.
constexpr ULONGLONG kLedgeCacheTtlMs = 1000;
constexpr wchar_t kClassName[] = L"DeskTidyNativePet";
constexpr int kSize = 72;
// Soft pets cap width so HiDPI bitmaps stay light. Pixel pets skip this cap
// (integer-cell art must not be resampled down before nearest upscale).
constexpr int kSpriteMaxW = 220;
constexpr int kPageChipH = 26;
constexpr int kPagePointer = 5;
constexpr int kPageGap = 6;
constexpr int kPageRowGap = 8;
constexpr int kPagePad = 4;
constexpr int kPageMinW = 44;
constexpr int kPageMaxRow = 3;
constexpr int kMenuBtn = 22;
constexpr int kActionBtn = 30;
constexpr int kActionGap = 4;
constexpr int kActionPad = 6;
constexpr int kMenuActions = 5;
// P1/P4/P5 — physics & AI constants aligned with Python pet_widget.py.
constexpr float kGravityPxPerSec = 1600.f;
constexpr float kThrowMinPxPerSec = 180.f;
constexpr float kThrowMaxPxPerSec = 980.f;
constexpr int kDragThresholdPx = 6;
constexpr int kSnapThresholdPx = 24;
constexpr int kCursorNoticePx = 110;
constexpr float kLonelySoftSec = 35.f;
constexpr float kLonelyHardSec = 90.f;
// Design @96 DPI; runtime uses GroundMarginPx() (DPI + sprite-relative).
constexpr int kGroundMargin0 = 48;
bool g_registered = false;

// P2-22: motion / FX aligned with Python pet_anim._motion + paint helpers.
struct PetMotion {
  float bob = 0.f;
  float squash = 1.f;
  float tilt = 0.f;
  float sway = 0.f;
  float stretch = 1.f;
};

bool IsHoodieFamily(const std::wstring& character) {
  return character == L"hoodie";
}

// 有「睡觉/打盹」动作的角色：卫衣家族 + 白衣剑仙（倚剑打盹）。
bool HasSleepAction(const std::wstring& character) {
  return IsHoodieFamily(character) || character == L"xianjian";
}

bool IsPixelArtCharacter(const std::wstring& character) {
  return character == L"gufeng";
}

bool PoseSkipsMotion(const std::wstring& character, const std::wstring& pose) {
  if (IsHoodieFamily(character)) {
    return pose == L"wait" || pose == L"perch" || pose == L"hold" ||
           pose == L"fall" || pose == L"dizzy" || pose == L"trash";
  }
  if (character == L"voyage") {
    return pose == L"wait" || pose == L"perch" || pose == L"hold" ||
           pose == L"fall" || pose == L"dizzy" || pose == L"trash" ||
           pose == L"happy" || pose == L"play";
  }
  return false;
}

PetMotion ComputePetMotion(const std::wstring& character,
                           const std::wstring& pose, int frame) {
  PetMotion m;
  if (PoseSkipsMotion(character, pose)) {
    return m;
  }
  const float t = static_cast<float>(frame) / 10.f;
  // 剑仙睡觉含树干：整体 bob 会让树「飘」——帧内呼吸已够，跳过位移。
  if (character == L"xianjian" && (pose == L"sleep" || pose == L"lie")) {
    return m;
  }
  const bool soft_sleep =
      (IsHoodieFamily(character) || character == L"voyage") &&
      (pose == L"sleep" || pose == L"lie");
  if (soft_sleep) {
    const float breath = std::sin(t * 0.4f);
    m.bob = 0.6f * breath;
    return m;
  }
  if (character == L"langfrog") {
    if (pose == L"sleep" || pose == L"lie") {
      const float breath = std::sin(t * 0.38f);
      m.bob = 0.4f * breath;
      m.squash = 1.f + 0.09f * breath;
      m.sway = 1.2f * breath;
      m.stretch = 1.f + 0.06f * breath;
      return m;
    }
    if (pose == L"climb" || pose == L"climb_crawl" || pose == L"climb_wall") {
      m.bob = 0.8f * std::sin(t * 2.4f);
      m.sway = 0.8f * std::sin(t * 1.2f);
      return m;
    }
  }
  if (pose == L"sleep" || pose == L"lie") {
    const float breath = std::sin(t * 0.5f);
    m.bob = 0.8f * breath;
    m.squash = 1.f + 0.06f * breath;
    m.sway = 0.5f * breath;
    m.stretch = 1.f + 0.04f * breath;
    return m;
  }
  if (pose == L"climb" || pose == L"climb_crawl" || pose == L"climb_wall") {
    m.bob = 0.6f * std::sin(t * 2.2f);
    return m;
  }
  if (pose == L"fall") {
    const float spin = std::fmod(frame * 14.f, 360.f) - 180.f;
    const float tumble = std::sin(t * 3.6f);
    m.bob = 2.f + 3.f * std::fabs(tumble);
    m.squash = 1.08f + 0.08f * tumble;
    m.tilt = spin * 0.22f;
    m.sway = 4.f * tumble;
    m.stretch = 0.92f + 0.1f * std::fabs(tumble);
    return m;
  }
  if (pose == L"happy") {
    const float bounce = std::pow(std::fabs(std::sin(t * 3.f)), 1.3f);
    m.bob = 2.f + 10.f * bounce;
    m.squash = 0.90f + 0.14f * bounce;
    m.tilt = 6.f * std::sin(t * 3.f);
    m.sway = 2.5f * std::sin(t * 1.6f);
    return m;
  }
  if (pose == L"perch" || pose == L"wait") {
    const float wobble = std::sin(t * 0.35f);
    m.bob = 0.7f * wobble;
    m.squash = 1.f + 0.015f * wobble;
    return m;
  }
  if (pose == L"wander" || pose == L"follow" || pose == L"play" ||
      pose == L"walk") {
    const float s = std::fabs(std::sin(t * 1.5f));
    m.bob = 3.2f * s;
    m.squash = 0.95f + 0.06f * s;
    m.tilt = 3.f * std::sin(t * 1.5f);
    return m;
  }
  // idle default — slower breath so soft bob matches the slower cel cadence.
  const float breath = std::sin(t * 0.45f);
  m.bob = 1.5f * breath;
  m.squash = 1.f + 0.02f * breath;
  return m;
}

bool PoseNeedsLivePaint(const std::wstring& character,
                        const std::wstring& pose) {
  // Only re-Present for soft bob (no FX overlays that need continuous paint).
  if (PoseSkipsMotion(character, pose)) {
    return false;
  }
  return pose == L"sleep" || pose == L"lie" || pose == L"idle" ||
         pose == L"wander" || pose == L"follow" || pose == L"walk" ||
         pose == L"happy" || pose == L"play" || pose == L"perch" ||
         pose == L"wait";
}

void PaintSparkles(Gdiplus::Graphics& g, int cw, int ch, int frame) {
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  const float ox[] = {-0.28f, 0.30f, 0.08f};
  const float oy[] = {0.18f, 0.12f, 0.06f};
  for (int i = 0; i < 3; ++i) {
    const float phase = frame * 0.18f + i * 1.7f;
    const int alpha =
        static_cast<int>(90 + 110 * (0.5f + 0.5f * std::sin(phase)));
    const int size =
        3 + static_cast<int>(2 * (0.5f + 0.5f * std::sin(phase * 1.4f)));
    const int x = cw / 2 + static_cast<int>(cw * ox[i]);
    const int y = static_cast<int>(ch * oy[i]) +
                  static_cast<int>(2 * std::sin(phase));
    Gdiplus::SolidBrush br(
        Gdiplus::Color(static_cast<BYTE>(alpha), 255, 214, 80));
    g.FillEllipse(&br, x - size / 2, y - size / 2, size, size);
    Gdiplus::Pen pen(Gdiplus::Color(static_cast<BYTE>(alpha), 255, 255, 230),
                     1.f);
    g.DrawLine(&pen, x - size - 1, y, x + size + 1, y);
    g.DrawLine(&pen, x, y - size - 1, x, y + size + 1);
  }
}

void PaintZzz(Gdiplus::Graphics& g, int cw, int ch, int frame,
              const std::wstring& character) {
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  // 剑仙倚树抱剑：树冠占画幅顶部，Zzz 贴在脸左侧（树外）上飘，勿进树叶。
  float base_x;
  float base_y;
  const bool xj = character == L"xianjian";
  if (xj) {
    base_x = cw * 0.04f;
    base_y = ch * 0.36f;
  } else if (IsHoodieFamily(character)) {
    base_x = cw * 0.54f;
    base_y = (std::max)(4.f, ch * 0.24f);
  } else {
    base_x = cw * 0.58f;
    base_y = (std::max)(4.f, ch * 0.12f);
  }
  const float delays[] = {0.f, 0.34f, 0.68f};
  const int sizes_xj[] = {13, 15, 17};
  const int sizes_def[] = {9, 10, 11};
  const int* sizes = xj ? sizes_xj : sizes_def;
  const int ox[] = {0, 6, 12};
  const int oy0[] = {4, -2, -8};
  const float rise_max = xj ? 16.f : 12.f;
  Gdiplus::FontFamily family(L"Segoe UI");
  for (int i = 0; i < 3; ++i) {
    const float t = std::fmod(frame / 40.f + delays[i], 1.f);
    const int rise = static_cast<int>(t * rise_max);
    const float fade = 1.f - std::fabs(t - 0.45f) * 1.6f;
    const int alpha =
        (std::max)(0, (std::min)(235, static_cast<int>((xj ? 220 : 190) * fade)));
    if (alpha < 20) {
      continue;
    }
    const int x =
        static_cast<int>(base_x + ox[i] + 2 * std::sin(t * 3.14159265f * 2));
    const int y = static_cast<int>(base_y + oy0[i] - rise);
    const Gdiplus::PointF pt(static_cast<Gdiplus::REAL>(x),
                             static_cast<Gdiplus::REAL>(y));
    Gdiplus::Font font(&family, static_cast<Gdiplus::REAL>(sizes[i]),
                       Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    // Soft halo so z stays readable beside dark hair / leaves.
    if (xj) {
      Gdiplus::SolidBrush halo(
          Gdiplus::Color(static_cast<BYTE>((std::min)(160, alpha)), 255, 255,
                         255));
      g.DrawString(L"z", 1, &font,
                   Gdiplus::PointF(pt.X + 1.f, pt.Y + 1.f), &halo);
    }
    Gdiplus::SolidBrush br(
        Gdiplus::Color(static_cast<BYTE>(alpha), 70, 80, 140));
    g.DrawString(L"z", 1, &font, pt, &br);
  }
}

void PaintPlayFx(Gdiplus::Graphics& g, int cw, int ch, int frame) {
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  for (int i = 0; i < 3; ++i) {
    const float phase = frame * 0.25f + i * 2.1f;
    const int alpha =
        static_cast<int>(60 + 100 * (0.5f + 0.5f * std::sin(phase)));
    const int x = cw / 2 + static_cast<int>(std::cos(phase) * cw * 0.28f);
    const int y = ch / 2 + static_cast<int>(std::sin(phase * 1.3f) * ch * 0.18f);
    Gdiplus::SolidBrush br(
        Gdiplus::Color(static_cast<BYTE>(alpha), 120, 200, 120));
    g.FillEllipse(&br, x - 3, y - 3, 6, 6);
  }
}

void EnsureGdiplus() {
  static ULONG_PTR token = 0;
  if (!token) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
  }
}

bool PresentTransparent(HWND hwnd, std::function<void(Gdiplus::Graphics&, int, int)> paint) {
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
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
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

void DrawRoundChip(Gdiplus::Graphics& g, const Gdiplus::RectF& r, bool active,
                   bool soft_border, bool hovered = false) {
  const Gdiplus::REAL rad =
      (std::max)(6.f, (std::min)(r.Height * 0.5f, 12.f));
  Gdiplus::GraphicsPath path;
  path.AddArc(r.X, r.Y, rad * 2, rad * 2, 180, 90);
  path.AddArc(r.X + r.Width - rad * 2, r.Y, rad * 2, rad * 2, 270, 90);
  path.AddArc(r.X + r.Width - rad * 2, r.Y + r.Height - rad * 2, rad * 2,
              rad * 2, 0, 90);
  path.AddArc(r.X, r.Y + r.Height - rad * 2, rad * 2, rad * 2, 90, 90);
  path.CloseFigure();
  Gdiplus::SolidBrush fill(
      active ? Gdiplus::Color(244, 219, 234, 254)
      : hovered ? Gdiplus::Color(250, 219, 234, 254)
                : Gdiplus::Color(230, 255, 255, 255));
  g.FillPath(&fill, &path);
  if (active || soft_border || hovered) {
    Gdiplus::Pen pen(active || hovered ? Gdiplus::Color(220, 37, 99, 235)
                                       : Gdiplus::Color(150, 213, 220, 231),
                     active ? 1.5f : 1.0f);
    g.DrawPath(&pen, &path);
  }
}

int MeasureChipWidth(HDC hdc, const std::wstring& label) {
  SIZE sz{};
  GetTextExtentPoint32W(hdc, label.c_str(), static_cast<int>(label.size()),
                        &sz);
  return (std::max)(kPageMinW, static_cast<int>(sz.cx) + 16);
}

std::wstring ChipLabel(const std::wstring& page_name, int index) {
  // Prefer the real page name (truncate with ellipsis); empty → 1-based index.
  if (page_name.empty()) {
    return std::to_wstring(index + 1);
  }
  if (page_name.size() <= 8) {
    return page_name;
  }
  return page_name.substr(0, 7) + L"…";
}

// Numeric frame index of a {stem}_{N}.png filename; -1 when no numbered suffix
// (i.e. the base "{pose}.png" image, which must not be treated as an anim cel).
int FrameIndexFromName(const std::wstring& path) {
  std::wstring name = path;
  const size_t slash = name.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    name = name.substr(slash + 1);
  }
  const size_t dot = name.rfind(L'.');
  if (dot == std::wstring::npos) {
    return -1;
  }
  const std::wstring stem = name.substr(0, dot);
  const size_t under = stem.rfind(L'_');
  if (under == std::wstring::npos || under + 1 >= stem.size()) {
    return -1;
  }
  const std::wstring tail = stem.substr(under + 1);
  for (wchar_t c : tail) {
    if (c < L'0' || c > L'9') {
      return -1;
    }
  }
  return _wtoi(tail.c_str());
}

// Sort frame paths by their numeric suffix so trash_2 < trash_10 (not string order).
void SortFrameFilesNumeric(std::vector<std::wstring>* files) {
  std::stable_sort(files->begin(), files->end(),
                   [](const std::wstring& a, const std::wstring& b) {
                     const int ia = FrameIndexFromName(a);
                     const int ib = FrameIndexFromName(b);
                     if (ia >= 0 && ib >= 0) {
                       return ia < ib;
                     }
                     if (ia >= 0) {
                       return true;  // numbered before base
                     }
                     if (ib >= 0) {
                       return false;
                     }
                     return a < b;
                   });
}

}  // namespace

namespace {
// Lock PARGB BGRA rows out of a GDI+ bitmap. HBITMAP round-trips flatten
// alpha (see pet_window.h) — ULW must use these pixels, not GetHBITMAP.
bool ExtractPargb(Gdiplus::Bitmap& bmp, int w, int h, std::vector<BYTE>* pargb,
                  std::vector<BYTE>* alpha) {
  if (!pargb || w <= 0 || h <= 0) {
    return false;
  }
  Gdiplus::BitmapData bd{};
  Gdiplus::Rect area(0, 0, w, h);
  Gdiplus::Status st =
      bmp.LockBits(&area, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd);
  const bool locked_pargb = (st == Gdiplus::Ok && bd.Scan0);
  if (!locked_pargb) {
    st = bmp.LockBits(&area, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB,
                      &bd);
  }
  if (st != Gdiplus::Ok || !bd.Scan0) {
    LogLineA("[pet] PARGB LockBits failed st=%d", static_cast<int>(st));
    return false;
  }
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  pargb->assign(n * 4, 0);
  if (alpha) {
    alpha->assign(n, 0);
  }
  const BYTE* base = static_cast<const BYTE*>(bd.Scan0);
  int nonzero = 0;
  for (int y = 0; y < h; ++y) {
    const BYTE* row = base + static_cast<size_t>(y) * bd.Stride;
    BYTE* dst = pargb->data() + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      const BYTE* p = row + static_cast<size_t>(x) * 4;
      BYTE b = p[0], g = p[1], r = p[2], a = p[3];
      // ARGB LockBits fallback: premultiply for ULW_ALPHA.
      if (!locked_pargb && a < 255) {
        if (a == 0) {
          b = g = r = 0;
        } else {
          b = static_cast<BYTE>((b * a) / 255);
          g = static_cast<BYTE>((g * a) / 255);
          r = static_cast<BYTE>((r * a) / 255);
        }
      }
      dst[x * 4 + 0] = b;
      dst[x * 4 + 1] = g;
      dst[x * 4 + 2] = r;
      dst[x * 4 + 3] = a;
      if (alpha) {
        (*alpha)[static_cast<size_t>(y) * w + x] = a;
      }
      if (a) {
        ++nonzero;
      }
    }
  }
  bmp.UnlockBits(&bd);
  static bool logged_first = false;
  if (!logged_first) {
    LogLineA("[pet] mask alpha %dx%d nonzero=%d", w, h, nonzero);
    logged_first = true;
  }
  return nonzero > 0;
}

std::vector<BYTE> ExtractAlphaMap(Gdiplus::Bitmap& bmp, int w, int h) {
  std::vector<BYTE> pargb;
  std::vector<BYTE> alpha;
  ExtractPargb(bmp, w, h, &pargb, &alpha);
  return alpha;
}
}  // namespace

std::vector<std::wstring> PetWindow::PetAssetRoots() {
  return {
      JoinPath(NativeDataDir(), L"pets"),
      JoinPath(ExeDir(), L"pets"),
      JoinPath(ExeDir(), L"assets\\pets"),
  };
}

void PetWindow::TryLoadSprite() {
  const std::wstring candidates[] = {
      JoinPath(NativeDataDir(), L"pet.png"),
      JoinPath(ExeDir(), L"pet.png"),
      JoinPath(ExeDir(), L"assets\\pet.png"),
  };
  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartup(&token, &input, nullptr);
  for (const auto& path : candidates) {
    Gdiplus::Bitmap src(path.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) {
      continue;
    }
    sprite_w_ = static_cast<int>(src.GetWidth());
    sprite_h_ = static_cast<int>(src.GetHeight());
    if (sprite_w_ > kSpriteMaxW) {
      sprite_h_ = sprite_h_ * kSpriteMaxW / sprite_w_;
      sprite_w_ = kSpriteMaxW;
    }
    base_sprite_h_ = sprite_h_;
    Gdiplus::Bitmap scaled(sprite_w_, sprite_h_, PixelFormat32bppPARGB);
    {
      Gdiplus::Graphics g(&scaled);
      g.Clear(Gdiplus::Color(0, 0, 0, 0));
      g.DrawImage(&src, 0, 0, sprite_w_, sprite_h_);
    }
    std::vector<BYTE> pargb;
    std::vector<BYTE> alpha;
    if (!ExtractPargb(scaled, sprite_w_, sprite_h_, &pargb, &alpha)) {
      continue;
    }
    sprite_pargb_ = std::move(pargb);
    sprite_alpha_ = std::move(alpha);
    HBITMAP hb = nullptr;
    scaled.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hb);
    if (hb) {
      sprite_ = hb;
    }
    break;
  }
  Gdiplus::GdiplusShutdown(token);
}

void PetWindow::ClearFrames() {
  ReleaseFlipBuffer();
  for (auto* hb : frames_) {
    if (hb) {
      DeleteObject(hb);
    }
  }
  frames_.clear();
  frames_alpha_.clear();
  frames_pargb_.clear();
  frames_pargb_flip_.clear();
  sprite_pargb_flip_.clear();
  present_content_valid_ = false;
  ++mask_gen_counter_;
  frame_i_ = 0;
  dissolve_ = 0;
}

void PetWindow::LoadPrefs() {
  std::ifstream in(JoinPath(NativeDataDir(), L"pet.json"));
  if (!in) {
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  auto field = [&](const char* key) -> std::string {
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) {
      return {};
    }
    p = json.find('"', json.find(':', p) + 1);
    if (p == std::string::npos) {
      return {};
    }
    ++p;
    std::string v;
    while (p < json.size() && json[p] != '"') {
      if (json[p] == '\\' && p + 1 < json.size()) {
        ++p;
      }
      v.push_back(json[p++]);
    }
    return v;
  };
  auto bool_field = [&](const char* key, bool fallback) -> bool {
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) {
      return fallback;
    }
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) {
      return fallback;
    }
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
      ++p;
    }
    if (p + 4 <= json.size() && json.compare(p, 4, "true") == 0) {
      return true;
    }
    if (p + 5 <= json.size() && json.compare(p, 5, "false") == 0) {
      return false;
    }
    return fallback;
  };
  auto num_field = [&](const char* key, double fallback) -> double {
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) {
      return fallback;
    }
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) {
      return fallback;
    }
    return std::atof(json.c_str() + p + 1);
  };
  const std::string ch = field("character");
  const std::string pose = field("pose");
  if (!ch.empty()) {
    character_ = WidenUtf8(ch);
  }
  if (!pose.empty()) {
    pose_ = WidenUtf8(pose);
  }
  auto_wander_ = bool_field("auto_wander", true);
  follow_cursor_ = bool_field("follow_cursor", false);
  show_page_bubbles_ = bool_field("show_page_bubbles", true);
  const double scale = num_field("size_scale", 1.0);
  if (scale >= 0.5 && scale <= 2.0) {
    size_scale_ = static_cast<float>(scale);
  }
  // P2: restored position (new fields; existing keys untouched).
  const double px = num_field("pos_x", 0.0);
  const double py = num_field("pos_y", 0.0);
  if (px != 0.0 || py != 0.0) {
    pos_x_ = static_cast<float>(px);
    pos_y_ = static_cast<float>(py);
    has_saved_pos_ = true;
  }
  auto load_actions = [&](const wchar_t* char_id, const char* key) {
    const std::string raw = field(key);
    if (raw.empty()) {
      return;
    }
    std::vector<std::string> acts;
    size_t start = 0;
    while (start < raw.size()) {
      size_t comma = raw.find(',', start);
      if (comma == std::string::npos) {
        comma = raw.size();
      }
      std::string part = raw.substr(start, comma - start);
      while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) {
        part.erase(part.begin());
      }
      while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) {
        part.pop_back();
      }
      if (!part.empty()) {
        acts.push_back(part);
      }
      start = comma + 1;
    }
    if (!acts.empty()) {
      SetEnabledActions(char_id, acts);
    }
  };
  load_actions(L"hoodie", "actions_hoodie");
  load_actions(L"voyage", "actions_voyage");
  load_actions(L"pixehood", "actions_pixehood");
  load_actions(L"xianjian", "actions_xianjian");
  EnsureDefaultActions(L"hoodie");
  EnsureDefaultActions(L"voyage");
  EnsureDefaultActions(L"pixehood");
  EnsureDefaultActions(L"xianjian");
}

void PetWindow::PersistPrefs() {
  auto join_acts = [&](const std::wstring& cid) {
    EnsureDefaultActions(cid);
    const auto& acts = enabled_actions_[cid];
    std::string s;
    for (size_t i = 0; i < acts.size(); ++i) {
      if (i) {
        s += ',';
      }
      s += acts[i];
    }
    return s;
  };
  std::ostringstream out;
  out << "{\n  \"character\":\"" << NarrowUtf8(character_) << "\",\n  \"pose\":\""
      << NarrowUtf8(pose_) << "\",\n  \"auto_wander\": "
      << (auto_wander_ ? "true" : "false") << ",\n  \"follow_cursor\": "
      << (follow_cursor_ ? "true" : "false") << ",\n  \"show_page_bubbles\": "
      << (show_page_bubbles_ ? "true" : "false") << ",\n  \"size_scale\": "
      << size_scale_ << ",\n  \"pos_x\": " << static_cast<int>(std::lround(pos_x_))
      << ",\n  \"pos_y\": " << static_cast<int>(std::lround(pos_y_))
      << ",\n  \"actions_hoodie\":\"" << join_acts(L"hoodie")
      << "\",\n  \"actions_voyage\":\"" << join_acts(L"voyage")
      << "\",\n  \"actions_pixehood\":\"" << join_acts(L"pixehood")
      << "\",\n  \"actions_xianjian\":\"" << join_acts(L"xianjian")
      << "\"\n}\n";
  AtomicWriteUtf8File(JoinPath(NativeDataDir(), L"pet.json"), out.str());
}

void PetWindow::EnsureDefaultActions(const std::wstring& character) {
  if (enabled_actions_.count(character) && !enabled_actions_[character].empty()) {
    return;
  }
  if (character == L"xianjian") {
    // 无「息」/闭目养神：休息只保留倚树打盹。
    enabled_actions_[character] = {"pet", "sleep", "fall"};
  } else if (HasSleepAction(character)) {
    enabled_actions_[character] = {"pet", "wait", "sleep", "fall"};
  } else {
    enabled_actions_[character] = {"pet", "wait", "fall"};
  }
}

bool PetWindow::ActionEnabled(const std::wstring& character,
                              const std::string& action) const {
  // 摸摸 / 跌落 always available (Python _DEFAULT_ALWAYS_ON_PET_ACTIONS).
  if (action == "pet" || action == "fall" || action == "hide") {
    return true;
  }
  // 剑仙产品面去掉「息」；旧 pet.json 若仍带 wait 也当作关闭。
  if (character == L"xianjian" && action == "wait") {
    return false;
  }
  auto it = enabled_actions_.find(character);
  if (it == enabled_actions_.end()) {
    if (character == L"xianjian") {
      return action == "sleep";
    }
    if (HasSleepAction(character)) {
      return action == "wait" || action == "sleep";
    }
    return action == "wait";
  }
  return std::find(it->second.begin(), it->second.end(), action) !=
         it->second.end();
}

std::vector<std::string> PetWindow::EnabledActions(
    const std::wstring& character) const {
  auto it = enabled_actions_.find(character);
  if (it != enabled_actions_.end() && !it->second.empty()) {
    if (character == L"xianjian") {
      std::vector<std::string> out;
      for (const auto& a : it->second) {
        if (a != "wait") {
          out.push_back(a);
        }
      }
      if (!out.empty()) {
        return out;
      }
    } else {
      return it->second;
    }
  }
  if (character == L"xianjian") {
    return {"pet", "sleep", "fall"};
  }
  if (HasSleepAction(character)) {
    return {"pet", "wait", "sleep", "fall"};
  }
  return {"pet", "wait", "fall"};
}

void PetWindow::SetEnabledActions(const std::wstring& character,
                                  const std::vector<std::string>& actions) {
  std::vector<std::string> merged = actions;
  auto ensure = [&](const char* id) {
    if (std::find(merged.begin(), merged.end(), id) == merged.end()) {
      merged.push_back(id);
    }
  };
  ensure("pet");
  ensure("fall");
  // Keep stable order: pet, wait, sleep, fall.
  const char* order[] = {"pet", "wait", "sleep", "fall"};
  std::vector<std::string> sorted;
  for (const char* id : order) {
    if (std::find(merged.begin(), merged.end(), id) != merged.end()) {
      // voyage has no sleep toggle in product defaults.
      if (character == L"voyage" && std::string(id) == "sleep") {
        continue;
      }
      sorted.push_back(id);
    }
  }
  enabled_actions_[character] = std::move(sorted);
  PersistPrefs();
  if (menu_) {
    PlaceMenu();
  }
}

std::vector<std::pair<std::string, std::wstring>> PetWindow::SettingsActionOptions(
    const std::wstring& character) const {
  // Settings UI only — 摸摸/跌落 always on and hidden (Python).
  if (character == L"xianjian") {
    return {{"sleep", L"倚树打盹"}};
  }
  if (IsHoodieFamily(character)) {
    return {{"wait", L"玩手机"}, {"sleep", L"睡觉"}};
  }
  return {{"wait", L"刷手机"}};
}

std::vector<int> PetWindow::VisibleMenuIndices() const {
  // Indices: 0 pet, 1 wait, 2 sleep, 3 fall, 4 hide.
  std::vector<int> out;
  out.push_back(0);
  if (ActionEnabled(character_, "wait")) {
    out.push_back(1);
  }
  if (ActionEnabled(character_, "sleep")) {
    out.push_back(2);
  }
  out.push_back(3);
  out.push_back(4);
  return out;
}

void PetWindow::LoadCharacters() {
  character_ids_.clear();
  auto push_unique = [&](const std::wstring& name) {
    if (name.empty() || _wcsicmp(name.c_str(), L"pixehood") == 0) {
      return;
    }
    for (const auto& existing : character_ids_) {
      if (existing == name) {
        return;
      }
    }
    character_ids_.push_back(name);
  };
  const auto roots = PetAssetRoots();
  for (const auto& root : roots) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(root, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      const std::wstring name = fd.cFileName;
      if (name.empty() || name[0] == L'.') {
        continue;
      }
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        // Character dirs contain pose subdirs or pngs; skip known pose names.
        if (name == L"idle" || name == L"walk" || name == L"sleep" ||
            name == L"wait" || name == L"play" || name == L"fall" ||
            name == L"hold" || name == L"trash") {
          continue;
        }
        push_unique(name);
        continue;
      }
      // Flat Python layout: hoodie_idle.png / voyage_walk_0.png
      const size_t under = name.find(L'_');
      const size_t dot = name.rfind(L'.');
      if (under == std::wstring::npos || dot == std::wstring::npos ||
          under >= dot) {
        continue;
      }
      const std::wstring ext = name.substr(dot);
      if (_wcsicmp(ext.c_str(), L".png") != 0) {
        continue;
      }
      push_unique(name.substr(0, under));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    // Keep scanning every root — a user pet in NativeDataDir\pets must not hide
    // the built-in characters in assets\pets (previously showed only one).
  }
  if (character_ids_.empty()) {
    character_ids_.push_back(L"hoodie");
  }
  std::sort(character_ids_.begin(), character_ids_.end());
  // Prefer product default when prefs are empty / unknown.
  bool has_hoodie = false;
  for (const auto& id : character_ids_) {
    if (id == L"hoodie") {
      has_hoodie = true;
      break;
    }
  }
  if (has_hoodie && (character_.empty() || character_ == L"default")) {
    character_ = L"hoodie";
  }
}

void PetWindow::LoadPosePacks() {
  pose_names_.clear();
  pose_files_.clear();
  const auto roots = PetAssetRoots();
  const wchar_t* poses[] = {L"idle", L"walk", L"sleep", L"wait", L"play",
                            L"fall", L"hold", L"trash"};
  auto scan_pose_dir = [&](const std::wstring& dir, const wchar_t* pose) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      return;
    }
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        files.push_back(JoinPath(dir, fd.cFileName));
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!files.empty()) {
      SortFrameFilesNumeric(&files);
      pose_names_.push_back(pose);
      pose_files_.push_back(std::move(files));
    }
  };
  auto scan_flat_prefix = [&](const std::wstring& root, const wchar_t* pose) {
    // Python assets: hoodie_idle_0.png … (base hoodie_idle.png is NOT a cel).
    const std::wstring pattern =
        JoinPath(root, character_ + L"_" + pose + L"*.png");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      return;
    }
    std::vector<std::wstring> numbered;
    std::vector<std::wstring> base;
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        continue;
      }
      const std::wstring path = JoinPath(root, fd.cFileName);
      if (FrameIndexFromName(path) >= 0) {
        numbered.push_back(path);
      } else {
        base.push_back(path);
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::vector<std::wstring> files;
    if (!numbered.empty()) {
      files = std::move(numbered);
      SortFrameFilesNumeric(&files);
    } else if (!base.empty()) {
      // No numbered cels → fall back to the single base image (Python behavior).
      files.push_back(base.front());
    }
    if (!files.empty()) {
      pose_names_.push_back(pose);
      pose_files_.push_back(std::move(files));
    }
  };
  for (const auto& root : roots) {
    // Prefer pets/<character>/<pose>/
    const std::wstring char_root = JoinPath(root, character_);
    for (const wchar_t* pose : poses) {
      scan_pose_dir(JoinPath(char_root, pose), pose);
    }
    // Also pets/<character>_idle.png style (Python product pack)
    if (pose_names_.empty()) {
      for (const wchar_t* pose : poses) {
        scan_flat_prefix(root, pose);
      }
    }
    if (!pose_names_.empty()) {
      break;
    }
    // Flat pets/<pose>/
    for (const wchar_t* pose : poses) {
      scan_pose_dir(JoinPath(root, pose), pose);
    }
    if (!pose_names_.empty()) {
      break;
    }
  }
  // P1 self-check: log pose → numeric frame order (base image excluded).
  for (size_t i = 0; i < pose_names_.size(); ++i) {
    std::wstring joined;
    for (size_t k = 0; k < pose_files_[i].size(); ++k) {
      if (k) {
        joined += L",";
      }
      const auto& f = pose_files_[i][k];
      const size_t slash = f.find_last_of(L"\\/");
      joined += (slash == std::wstring::npos) ? f : f.substr(slash + 1);
    }
    LogLine(L"[pet] pose=%s frames=[%s]", pose_names_[i].c_str(),
            joined.c_str());
  }
}

void PetWindow::SetCharacter(const std::wstring& character) {
  if (character == L"pixehood") {
    character_ = L"hoodie";
  } else {
    character_ = character;
  }
  LoadPosePacks();
  if (!pose_names_.empty()) {
    SetPose(pose_names_[0]);
  }
  PersistPrefs();
}

void PetWindow::SetPose(const std::wstring& pose) {
  pose_ = pose;
  // 剑仙倚树：Doubao 原图已面朝左；勿镜像，也勿继承走路/拖拽朝向。
  // facing_=1 = 按素材原向（朝左）。
  if (character_ == L"xianjian" && (pose == L"sleep" || pose == L"lie")) {
    facing_ = 1;
  }
  ClearFrames();
  std::vector<std::wstring> files;
  for (size_t i = 0; i < pose_names_.size(); ++i) {
    if (pose_names_[i] == pose) {
      files = pose_files_[i];
      break;
    }
  }
  if (files.empty()) {
    return;
  }
  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartup(&token, &input, nullptr);
  // Load every numbered cel (no 16-frame cap — Python scans until missing).
  for (const auto& path : files) {
    Gdiplus::Bitmap src(path.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) {
      continue;
    }
    int w = static_cast<int>(src.GetWidth());
    int h = static_cast<int>(src.GetHeight());
    // Unify soft-pet HEIGHT (not width) so every character renders the same
    // size at a given zoom — previously a 220px width cap made hoodie ~263px
    // tall but a full-body xianjian ~230px tall (pets looked different-sized).
    // Sleep-with-tree needs extra headroom so the canopy is not crushed.
    if (!IsPixelArtCharacter(character_)) {
      const int kSpriteMaxH =
          (character_ == L"xianjian" &&
           (pose == L"sleep" || pose == L"lie"))
              ? 360
              : 280;
      if (h > kSpriteMaxH) {
        w = w * kSpriteMaxH / h;
        h = kSpriteMaxH;
      }
    }
    Gdiplus::Bitmap scaled(w, h, PixelFormat32bppPARGB);
    {
      Gdiplus::Graphics g(&scaled);
      g.Clear(Gdiplus::Color(0, 0, 0, 0));
      // Pixel pets must stay nearest-neighbor or color blocks smear.
      const bool pixel = IsPixelArtCharacter(character_);
      g.SetInterpolationMode(pixel ? Gdiplus::InterpolationModeNearestNeighbor
                                   : Gdiplus::InterpolationModeHighQualityBicubic);
      g.SetPixelOffsetMode(pixel ? Gdiplus::PixelOffsetModeNone
                                 : Gdiplus::PixelOffsetModeHalf);
      g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
      g.DrawImage(&src, 0, 0, w, h);
    }  // destroy Graphics before LockBits (GDI+ refuses while it is alive)
    std::vector<BYTE> pargb;
    std::vector<BYTE> alpha;
    if (!ExtractPargb(scaled, w, h, &pargb, &alpha)) {
      continue;
    }
    HBITMAP hb = nullptr;
    scaled.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hb);
    frames_.push_back(hb);  // may be null; Paint uses frames_pargb_
    frames_alpha_.push_back(std::move(alpha));
    frames_pargb_.push_back(std::move(pargb));
    sprite_w_ = w;
    sprite_h_ = h;
    if (pose == L"idle") {
      base_sprite_h_ = h;
    }
  }
  Gdiplus::GdiplusShutdown(token);
  frame_i_ = 0;
  anim_accum_ms_ = 0;
  present_dirty_ = true;
  chrome_dirty_ = true;
  // No PersistPrefs here: SetPose is also hit by transient state switches
  // (walk↔idle follow-cursor, play/sleep/fall) — writing pet.json on every one
  // of those was high-frequency disk churn. Persist only on explicit picks.
  if (hwnd_) {
    RebaseFloor();
    // Sleep-with-tree (and other taller poses) must lift with the new height,
    // otherwise the window grows downward and ClampRect/work-area clips the
    // canopy — users only see the lower trunk.
    if (!falling_ && !climbing_ && !dragging_ && !on_ledge_) {
      pos_y_ = static_cast<float>(base_y_);
    }
    ApplyScaledWindowSize();
    Present();
    PlacePages();
    PlaceMenu();
  }
}

void PetWindow::LoadFramePack() {
  LoadPrefs();
  LoadCharacters();
  bool known = false;
  for (const auto& id : character_ids_) {
    if (id == character_) {
      known = true;
      break;
    }
  }
  if (!known && !character_ids_.empty()) {
    character_ = character_ids_[0];
  }
  LoadPosePacks();
  if (!pose_names_.empty()) {
    bool pose_ok = false;
    for (const auto& p : pose_names_) {
      if (p == pose_) {
        pose_ok = true;
        break;
      }
    }
    // Never restore into a transient pose (fall/trash/hold) — Python doesn't
    // persist the pose at all; start on idle instead of a frozen mid-throw cel.
    if (pose_ == L"fall" || pose_ == L"trash" || pose_ == L"hold") {
      pose_ok = false;
    }
    SetPose(pose_ok ? pose_ : pose_names_[0]);
    return;
  }
  // Flat pets\*.png fallback (no pose subdirs).
  const auto roots = PetAssetRoots();
  std::vector<std::wstring> files;
  for (const auto& root : roots) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(root, L"*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        files.push_back(JoinPath(root, fd.cFileName));
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!files.empty()) {
      break;
    }
  }
  std::sort(files.begin(), files.end());
  if (files.size() < 2) {
    return;
  }
  pose_files_ = {files};
  pose_names_ = {L"idle"};
  SetPose(L"idle");
}

bool PetWindow::HasPose(const std::wstring& pose) const {
  return std::find(pose_names_.begin(), pose_names_.end(), pose) !=
         pose_names_.end();
}

int PetWindow::FrameIntervalMs(const std::wstring& pose) const {
  const bool voyage = _wcsicmp(character_.c_str(), L"voyage") == 0;
  const bool xj = character_ == L"xianjian";
  if (pose == L"fall") {
    return -1;  // frozen single cel (Python holds one airborne frame)
  }
  if (pose == L"walk") {
    return 80;  // 走路略缓，左右向共用同一间隔
  }
  if (pose == L"wait") {
    // 等待/玩手机（左右镜像共用）：衣袂/刷手机要慢，能辨认再换帧。
    if (voyage) {
      return 1100;
    }
    return xj ? 640 : 560;
  }
  if (pose == L"sleep" || pose == L"lie") {
    // 睡觉换帧左右共用；卫衣原先 132 太跳，与站立衣袂同档放慢。
    return xj ? 200 : 280;
  }
  if (pose == L"hold") {
    return 200;
  }
  if (pose == L"trash") {
    // 投垃圾桶：一镜到底要能看清揉皱/扔出；原 330/360 仍显赶。
    return voyage ? 280 : 520;
  }
  if (pose == L"play") {
    // 摸摸：左右向共用；从容颔首，动作可辨。
    return voyage ? 640 : 280;
  }
  if (pose == L"perch") {
    return 400;
  }
  // 站立 idle（左右镜像共用）：衣袂/发丝像轻风，勿扇叶。
  return xj ? 480 : 360;
}

int PetWindow::PixelDisplayMul() const {
  if (!IsPixelArtCharacter(character_) || sprite_w_ <= 0) {
    return 1;
  }
  const float dpi = current_dpi_ > 0 ? current_dpi_ / 96.0f : 1.0f;
  // 32–36 cell sprites read tiny at 1× — default ~2×, snap to integer cells
  // so nearest-neighbor stays chunky (desktop-mascot / Shimeji class).
  int mul = static_cast<int>(std::lround(size_scale_ * dpi * 2.f));
  if (mul < 1) {
    mul = 1;
  }
  if (mul > 5) {
    mul = 5;
  }
  return mul;
}

int PetWindow::ScaledW() const {
  if (IsPixelArtCharacter(character_) && sprite_w_ > 0) {
    return (std::max)(24, sprite_w_ * PixelDisplayMul());
  }
  const float dpi = current_dpi_ > 0 ? current_dpi_ / 96.0f : 1.0f;
  return (std::max)(24, static_cast<int>(std::lround(sprite_w_ * size_scale_ * dpi)));
}

int PetWindow::ScaledH() const {
  if (IsPixelArtCharacter(character_) && sprite_h_ > 0) {
    return (std::max)(24, sprite_h_ * PixelDisplayMul());
  }
  const float dpi = current_dpi_ > 0 ? current_dpi_ / 96.0f : 1.0f;
  return (std::max)(24, static_cast<int>(std::lround(sprite_h_ * size_scale_ * dpi)));
}

int PetWindow::GroundMarginPx() const {
  const int dpi = current_dpi_ > 0 ? current_dpi_ : SystemDpi();
  const int design = ScalePxForDpi(kGroundMargin0, dpi);
  // Keep a visible band under the sprite on small/HiDPI work areas.
  const int from_sprite = ScaledH() * 12 / 100;
  const int lo = ScalePxForDpi(24, dpi);
  const int hi = ScalePxForDpi(72, dpi);
  return (std::max)(lo, (std::min)(hi, (std::max)(design, from_sprite)));
}

int PetWindow::PageHeadClearancePx(int pet_h) const {
  // Gap from bubble pointer tip to sprite crown. Was 22%/36–80 — left a tall
  // empty band above 剑仙/soft pets (user: 空白太多). Keep a small floor so the
  // tip never sits on hair (display-adaptivity), but stay tight like market
  // desktop-pet chrome.
  const int dpi = current_dpi_ > 0 ? current_dpi_ : SystemDpi();
  int clear = pet_h * 10 / 100;
  const int lo = ScalePxForDpi(12, dpi);
  const int hi = ScalePxForDpi(36, dpi);
  clear = (std::max)(lo, (std::min)(hi, clear));
  return clear;
}

void PetWindow::ApplyScaledWindowSize() {
  if (!hwnd_) {
    return;
  }
  SetOverlayScreenPos(hwnd_, 0, 0, ScaledW(), ScaledH(), SWP_NOMOVE | SWP_NOZORDER);
}

void PetWindow::RebaseFloor() {
  // Floor on the monitor under the pet (not always primary) — P1-15.
  POINT pt{static_cast<LONG>(pos_x_ + ScaledW() / 2),
           static_cast<LONG>(pos_y_ + ScaledH() / 2)};
  if (pt.x == 0 && pt.y == 0 && hwnd_) {
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    pt.x = (wr.left + wr.right) / 2;
    pt.y = (wr.top + wr.bottom) / 2;
  }
  const RECT work = WorkAreaAtPoint(pt);
  base_y_ = work.bottom - ScaledH() - GroundMarginPx();
}

bool PetWindow::RectVisibleOnAnyMonitor(const RECT& rc) const {
  for (const auto& m : EnumMonitors()) {
    RECT inter{};
    if (IntersectRect(&inter, &rc, &m.work) &&
        (inter.right - inter.left) >= 40 && (inter.bottom - inter.top) >= 40) {
      return true;
    }
  }
  return false;
}

void PetWindow::RestorePosition() {
  const int ww = ScaledW();
  const int hh = ScaledH();
  const RECT work = PrimaryWorkArea();
  RebaseFloor();
  if (has_saved_pos_) {
    RECT saved{static_cast<LONG>(pos_x_), static_cast<LONG>(pos_y_),
               static_cast<LONG>(pos_x_ + ww), static_cast<LONG>(pos_y_ + hh)};
    if (RectVisibleOnAnyMonitor(saved)) {
      ClampRectToWorkArea(&saved);
      pos_x_ = static_cast<float>(saved.left);
      pos_y_ = static_cast<float>(saved.top);
      LogLine(L"[pet] restore pos=(%d,%d)", static_cast<int>(pos_x_),
              static_cast<int>(pos_y_));
      return;
    }
    // Saved monitor disconnected → fall back to primary work area bottom-right.
    LogLine(L"[pet] saved monitor offline → fallback pos");
  }
  pos_x_ = static_cast<float>(work.right - ww - 40);
  pos_y_ = static_cast<float>(base_y_);
}

void PetWindow::EnterHappy(DWORD now) {
  last_interact_ms_ = now;
  mood_ = (std::min)(100.f, mood_ + 18.f);
  SpawnHearts(5);
  walking_ = false;
  falling_ = false;
  climbing_ = false;
  on_ledge_ = false;
  if (HasPose(L"play")) {
    SetPose(L"play");
  } else if (HasPose(L"idle")) {
    SetPose(L"idle");
  }
  happy_active_ = true;
  const bool voyage = _wcsicmp(character_.c_str(), L"voyage") == 0;
  state_until_ms_ = now + (voyage ? 3200 : 3000);
  LogLine(L"[pet] click→happy pose=%s hearts=%d", pose_.c_str(),
          static_cast<int>(hearts_.size()));
  Say("pet");
}

void PetWindow::EnterSleep(DWORD now) {
  walking_ = false;
  falling_ = false;
  climbing_ = false;
  on_ledge_ = false;
  if (HasPose(L"sleep")) {
    sleep_anim_dir_ = 1;
    if (character_ == L"xianjian") {
      facing_ = 1;  // 原图朝左，不镜像
    }
    SetPose(L"sleep");
  }
  // Tall sleep (tree) expands under the cursor → would edge-trigger an
  // immediate wake on the next Tick. Treat cursor as already-near.
  cursor_was_near_ = true;
  // 剑仙倚树略长一档（约 18–30s）；其它角色约 14–24s。
  const DWORD base = character_ == L"xianjian" ? 18000u : 14000u;
  const DWORD span = character_ == L"xianjian" ? 12000u : 10000u;
  state_until_ms_ = now + base + static_cast<DWORD>(rand() % span);
  LogLine(L"[pet] idle→sleep");
  Say("sleep");
}

void PetWindow::EnterWait(DWORD now) {
  // 剑仙「休息」应对齐倚树打盹素材；闭目站立 wait 无树，用户会觉得没在睡觉。
  if (character_ == L"xianjian" && ActionEnabled(character_, "sleep") &&
      HasPose(L"sleep")) {
    EnterSleep(now);
    return;
  }
  walking_ = false;
  facing_ = 1;
  falling_ = false;
  climbing_ = false;
  on_ledge_ = false;
  if (HasPose(L"wait")) {
    SetPose(L"wait");
  }
  state_until_ms_ = now + 12000 + static_cast<DWORD>(rand() % 10000);
  LogLine(L"[pet] idle→wait");
  Say("wait");
}

void PetWindow::ScheduleIdle(DWORD now) {
  walking_ = false;
  facing_ = 1;
  falling_ = false;
  climbing_ = false;
  on_ledge_ = false;
  happy_active_ = false;
  if (HasPose(L"idle")) {
    SetPose(L"idle");
  }
  const float lonely = (now - last_interact_ms_) / 1000.0f;
  int dwell_ms;
  if (lonely >= kLonelyHardSec) {
    dwell_ms = 1800 + rand() % 1700;
  } else if (lonely >= kLonelySoftSec) {
    dwell_ms = 2500 + rand() % 2500;
  } else {
    dwell_ms = 3500 + rand() % 4500;
  }
  state_until_ms_ = now + static_cast<DWORD>(dwell_ms);
}

void PetWindow::ChooseNextAfterIdle(DWORD now) {
  // Python: rest chance 18% / 38% / 55% by loneliness — not every idle (P1-18).
  const bool wait_on = ActionEnabled(character_, "wait");
  const bool sleep_on = ActionEnabled(character_, "sleep");
  if (!wait_on && !sleep_on) {
    ScheduleIdle(now);
    return;
  }
  const float lonely = (now - last_interact_ms_) / 1000.0f;
  const float roll = static_cast<float>(rand() % 1000) / 1000.f;
  float chance = 0.18f;
  if (lonely >= kLonelyHardSec) {
    chance = 0.55f;
  } else if (lonely >= kLonelySoftSec) {
    chance = 0.38f;
  }
  if (roll >= chance) {
    ScheduleIdle(now);
    return;
  }
  // 剑仙休息固定倚树；卫衣等仍 wait/sleep 轮换（玩手机 ↔ 睡觉）。
  if (character_ == L"xianjian" && sleep_on) {
    EnterSleep(now);
    return;
  }
  if (wait_on && sleep_on) {
    if (rest_flip_ % 2 == 0) {
      EnterWait(now);
    } else {
      EnterSleep(now);
    }
    rest_flip_++;
    return;
  }
  if (sleep_on) {
    EnterSleep(now);
  } else {
    EnterWait(now);
  }
}

void PetWindow::RunAi(DWORD now) {
  if (dragging_ || !hwnd_ || !IsWindowVisible(hwnd_)) {
    return;
  }
  if (pose_ == L"trash" && now >= state_until_ms_) {
    LogLine(L"[pet] trash→done");
    if (ActionEnabled(character_, "wait") && HasPose(L"wait")) {
      post_trash_wait_ = true;
      EnterWait(now);
    } else {
      ScheduleIdle(now);
    }
    return;
  }
  if (happy_active_ && now >= state_until_ms_) {
    happy_active_ = false;
    ScheduleIdle(now);
    return;
  }
  if (pose_ == L"sleep" && now >= state_until_ms_) {
    ScheduleIdle(now);
    return;
  }
  if (pose_ == L"wait" && now >= state_until_ms_) {
    post_trash_wait_ = false;
    ScheduleIdle(now);
    return;
  }
  if (pose_ == L"idle" && now >= state_until_ms_ && !walking_ && !falling_ &&
      !climbing_) {
    ChooseNextAfterIdle(now);
    return;
  }

  // Cursor reaction: face cursor / wake from sleep on approach (Python _on_ai).
  POINT cur{};
  GetCursorPos(&cur);
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  const int cx = (rc.left + rc.right) / 2;
  // Sleep-with-tree is tall: measure from the character body (lower half),
  // not the canopy center — otherwise expanding under the cursor wakes
  // instantly (flash then "消失").
  const int cy =
      (pose_ == L"sleep" || pose_ == L"lie")
          ? (rc.top + (rc.bottom - rc.top) * 72 / 100)
          : ((rc.top + rc.bottom) / 2);
  const int dx = cur.x - cx;
  const int dy = cur.y - cy;
  const bool cursor_near = dx * dx + dy * dy < kCursorNoticePx * kCursorNoticePx;
  if (cursor_near && !cursor_was_near_) {
    if (pose_ == L"sleep") {
      ScheduleIdle(now);
    }
    mood_ = (std::min)(100.f, mood_ + 1.5f);
  }
  // 等待/站立等非行走状态下保持单一默认自然朝向（facing_ = 1），不跟随鼠标翻转
  if (!walking_ && !dragging_ && facing_ != 1) {
    facing_ = 1;
    present_dirty_ = true;
  }
  cursor_was_near_ = cursor_near;
}

void PetWindow::SpawnHearts(int count) {
  const DWORD now = GetTickCount();
  for (int i = 0; i < count; ++i) {
    Heart h;
    h.fx = 0.15f + static_cast<float>(rand() % 70) / 100.0f;
    h.fy = 0.05f + static_cast<float>(rand() % 40) / 100.0f;
    h.born = now + static_cast<DWORD>(rand() % 120);
    hearts_.push_back(h);
  }
}

void PetWindow::Say(const char* action) {
  const auto line = PetActionLine(character_, action);
  if (!line.empty()) {
    // Clear page-chip cloud so speech sits above it, not on top of it.
    HWND chips = (pages_ && IsWindowVisible(pages_)) ? pages_ : nullptr;
    ShowToastNear(hwnd_, line, chips);
  }
}

void PetWindow::AdvanceFrames(DWORD elapsed_ms, DWORD, bool* frame_changed) {
  *frame_changed = false;
  const int n = static_cast<int>(frames_.size());
  if (n <= 1) {
    return;
  }
  if (pose_ == L"fall") {
    if (frame_i_ != 0) {
      frame_i_ = 0;
      *frame_changed = true;
    }
    return;
  }
  const int interval = FrameIntervalMs(pose_);
  if (interval <= 0) {
    return;
  }
  anim_accum_ms_ += elapsed_ms;
  if (anim_accum_ms_ < static_cast<DWORD>(interval)) {
    return;
  }
  const int steps = static_cast<int>(anim_accum_ms_ / interval);
  anim_accum_ms_ %= interval;
  if (pose_ == L"trash") {
    // One-shot crumple → hold last cel (Python min(n-1, frame//ticks)).
    const int next = (std::min)(n - 1, frame_i_ + steps);
    if (next != frame_i_) {
      frame_i_ = next;
      *frame_changed = true;
    }
    return;
  }
  // 剑仙 sleep：只要「倚树打盹」定格一帧；呼吸感交给 Zzz，不要换 cel 来回动。
  if (pose_ == L"sleep" && character_ == L"xianjian") {
    if (frame_i_ != 0) {
      frame_i_ = 0;
      *frame_changed = true;
    }
    return;
  }
  // Hard cut only: GDI+ HBITMAP + ColorMatrix crossfade left black fringes
  // on transparent edges (layered UpdateLayeredWindow).
  frame_i_ = (frame_i_ + steps) % n;
  *frame_changed = true;
}

void PetWindow::StepPhysics(DWORD elapsed_ms) {
  const float dt = (std::max)(0.001f, elapsed_ms / 1000.0f);
  POINT mid{static_cast<LONG>(pos_x_ + ScaledW() / 2),
            static_cast<LONG>(pos_y_ + ScaledH() / 2)};
  const RECT work = WorkAreaAtPoint(mid);
  const int ww = ScaledW();
  const int hh = ScaledH();

  if (climb_cooldown_ > 0) {
    --climb_cooldown_;
  }

  // P2-21: follow cursor (Python _STATE_FOLLOW @ 140 px/s, stop within 28px).
  if (follow_cursor_ && !dragging_ && !falling_ && !climbing_) {
    POINT cursor{};
    GetCursorPos(&cursor);
    const float cx = pos_x_ + ww * 0.5f;
    const float cy = pos_y_ + hh * 0.5f;
    const float dx = static_cast<float>(cursor.x) - cx;
    const float dy = static_cast<float>(cursor.y) - cy;
    const float dist = std::sqrt(dx * dx + dy * dy);
    constexpr float kStop = 28.f;
    constexpr float kSpeed = 140.f;
    if (dist > kStop) {
      const float step = (std::min)(kSpeed * dt, dist - kStop * 0.4f);
      if (dx != 0.f) {
        facing_ = dx > 0.f ? 1 : -1;
        walk_dir_ = facing_;
      }
      pos_x_ += dx / dist * step;
      pos_y_ += dy / dist * step;
      pos_x_ = (std::max)(static_cast<float>(work.left),
                          (std::min)(pos_x_, static_cast<float>(work.right - ww)));
      pos_y_ = (std::max)(static_cast<float>(work.top),
                          (std::min)(pos_y_, static_cast<float>(work.bottom - hh)));
      if (HasPose(L"walk") && pose_ != L"walk") {
        SetPose(L"walk");
      }
      // ULW Present moves the window — avoid MoveOverlayScreen (SetWindowPos wipe).
      present_dirty_ = true;
      chrome_dirty_ = true;
    } else if (HasPose(L"idle") && pose_ != L"idle") {
      SetPose(L"idle");
    }
    return;
  }

  if (climbing_) {
    pos_y_ -= 150.f * dt;
    pos_x_ += 60.f * static_cast<float>(walk_dir_) * dt;
    if (pos_x_ < static_cast<float>(ledge_left_)) {
      pos_x_ = static_cast<float>(ledge_left_);
    }
    if (pos_x_ + ww > static_cast<float>(ledge_right_)) {
      pos_x_ = static_cast<float>(ledge_right_ - ww);
    }
    if (pos_y_ <= static_cast<float>(ledge_top_ - hh)) {
      pos_y_ = static_cast<float>(ledge_top_ - hh);
      climbing_ = false;
      on_ledge_ = true;
      walking_ = true;
      vel_x_ = 44.f * static_cast<float>(walk_dir_);
    }
    present_dirty_ = true;
    return;
  }

  if (on_ledge_) {
    if (ledge_hwnd_ && !IsWindow(ledge_hwnd_)) {
      on_ledge_ = false;
      StartThrow(0.f, 0.f);
      return;
    }
    if (ledge_hwnd_) {
      RECT lr{};
      if (GetWindowRect(ledge_hwnd_, &lr)) {
        ledge_top_ = lr.top;
        ledge_left_ = lr.left + 12;
        ledge_right_ = lr.right - 12;
      }
    }
    pos_x_ += vel_x_ * dt;
    if (pos_x_ < static_cast<float>(ledge_left_) - 4 ||
        pos_x_ + ww > static_cast<float>(ledge_right_) + 4) {
      on_ledge_ = false;
      StartThrow(0.f, 0.f);
      return;
    }
    pos_y_ = static_cast<float>(ledge_top_ - hh);
    present_dirty_ = true;
    return;
  }

  if (falling_) {
    vel_y_ += kGravityPxPerSec * dt;
    pos_y_ += vel_y_ * dt;
    pos_x_ += vel_x_ * dt;
    if (pos_x_ < static_cast<float>(work.left)) {
      pos_x_ = static_cast<float>(work.left);
      vel_x_ = -vel_x_ * 0.35f;
    }
    if (pos_x_ + ww > static_cast<float>(work.right)) {
      pos_x_ = static_cast<float>(work.right - ww);
      vel_x_ = -vel_x_ * 0.35f;
    }
    if (pos_y_ >= static_cast<float>(base_y_)) {
      pos_y_ = static_cast<float>(base_y_);
      if (std::abs(vel_y_) < 220.f && std::abs(vel_x_) < 120.f) {
        // Soft land → idle (Python enters dizzy; brief idle is acceptable here).
        falling_ = false;
        vel_y_ = 0.f;
        vel_x_ = 0.f;
        RebaseFloor();
        if (HasPose(L"idle")) {
          SetPose(L"idle");
        }
        ScheduleIdle(GetTickCount());
      } else {
        // Bounce (Python vy = -|vy|*0.35, vx *= 0.72).
        vel_y_ = -std::abs(vel_y_) * 0.35f;
        vel_x_ *= 0.72f;
      }
    }
    present_dirty_ = true;
    return;
  }

  if (walking_ && auto_wander_) {
    pos_x_ += vel_x_ * dt;
    if (pos_x_ < static_cast<float>(work.left) ||
        pos_x_ + ww > static_cast<float>(work.right)) {
      walk_dir_ = -walk_dir_;
      facing_ = walk_dir_ >= 0 ? 1 : -1;
      vel_x_ = 90.f * static_cast<float>(walk_dir_);
      pos_x_ = (std::max)(static_cast<float>(work.left),
                          (std::min)(pos_x_, static_cast<float>(work.right - ww)));
    }
    pos_y_ = static_cast<float>(base_y_);
    present_dirty_ = true;
    return;
  }
}

void PetWindow::EndDrag(POINT release) {
  dragging_ = false;
  press_captured_ = false;
  ReleaseCapture();
  const DWORD now = GetTickCount();
  PushDragSample(release);

  float vx = 0.f;
  float vy = 0.f;
  // Velocity from samples inside the trailing 120 ms window (Python _drag_velocity).
  int first = -1;
  int last = -1;
  for (int k = 0; k < drag_sample_n_; ++k) {
    const int idx = (drag_sample_i_ - drag_sample_n_ + k + 8 * 8) % 8;
    const DragSample& s = drag_samples_[idx];
    if (now - s.tick > 120) {
      continue;
    }
    if (first < 0) {
      first = idx;
    }
    last = idx;
  }
  if (first >= 0 && last >= 0 && first != last) {
    const DragSample& a = drag_samples_[first];
    const DragSample& b = drag_samples_[last];
    const float sec = (std::max)(0.016f, static_cast<float>(b.tick - a.tick) / 1000.0f);
    vx = (b.pt.x - a.pt.x) / sec;
    vy = (b.pt.y - a.pt.y) / sec;
  }
  const float speed = std::sqrt(vx * vx + vy * vy);
  if (speed >= kThrowMinPxPerSec) {
    EndDragFloat();
    StartThrow(vx, vy);
    Present();
    return;
  }

  // Prefer pos_ (ULW-updated during drag) over GetWindowRect — region/TOPMOST
  // paths can leave Win32 rect stale relative to the layered blit origin.
  RECT rc{static_cast<LONG>(pos_x_), static_cast<LONG>(pos_y_),
          static_cast<LONG>(pos_x_) + ScaledW(),
          static_cast<LONG>(pos_y_) + ScaledH()};
  // Snap near screen edges (Python snap_geometry, 24 px). P1-15: use the work
  // area of the monitor under the pet, so a secondary-monitor drop stays there
  // instead of being pulled back to the primary monitor.
  POINT center{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
  const RECT work = WorkAreaAtPoint(center);
  if (rc.left - work.left < kSnapThresholdPx) {
    rc.right = work.left + (rc.right - rc.left);
    rc.left = work.left;
  }
  if (work.right - rc.right < kSnapThresholdPx) {
    rc.left = work.right - (rc.right - rc.left);
    rc.right = work.right;
  }
  if (rc.top - work.top < kSnapThresholdPx) {
    rc.bottom = work.top + (rc.bottom - rc.top);
    rc.top = work.top;
  }
  if (work.bottom - rc.bottom < kSnapThresholdPx) {
    rc.top = work.bottom - (rc.bottom - rc.top);
    rc.bottom = work.bottom;
  }
  ClampRectToWorkArea(&rc);
  pos_x_ = static_cast<float>(rc.left);
  pos_y_ = static_cast<float>(rc.top);
  RebaseFloor();
  if (rc.top < base_y_ - 24) {
    EndDragFloat();
    StartThrow(0.f, 80.f);  // dropped above floor → fall (Python)
    Present();
    return;
  }
  base_y_ = work.bottom - (rc.bottom - rc.top) - GroundMarginPx();
  pos_y_ = static_cast<float>(base_y_);
  PersistPrefs();
  const std::wstring restore = pose_before_drag_;
  pose_before_drag_.clear();
  walking_ = false;
  falling_ = false;
  climbing_ = false;
  on_ledge_ = false;
  // Pose restore before EndDragFloat so SetPose's ApplyScaledWindowSize wipe
  // is covered by the final double-Present in EndDragFloat.
  if (restore == L"sleep" && HasPose(L"sleep") &&
      ActionEnabled(character_, "sleep")) {
    sleep_anim_dir_ = 1;
    if (character_ == L"xianjian") {
      facing_ = 1;
    }
    if (pose_ != L"sleep") {
      SetPose(L"sleep");
    }
    cursor_was_near_ = true;
    const DWORD base = character_ == L"xianjian" ? 18000u : 14000u;
    const DWORD span = character_ == L"xianjian" ? 12000u : 10000u;
    state_until_ms_ = now + base + static_cast<DWORD>(rand() % span);
  } else if (restore == L"wait" && HasPose(L"wait") &&
             ActionEnabled(character_, "wait")) {
    if (pose_ != L"wait") {
      SetPose(L"wait");
    }
    state_until_ms_ = now + 12000 + static_cast<DWORD>(rand() % 10000);
  } else if (pose_ != L"idle" && HasPose(L"idle")) {
    ScheduleIdle(now);
  }
  EndDragFloat();
  chrome_dirty_ = true;
  if (pages_) {
    PlacePages();
  }
}

const std::vector<WindowLedge>& PetWindow::CachedWindowLedges() {
  const ULONGLONG now = GetTickCount64();
  if (!ledge_cache_.empty() && now - ledge_cache_ms_ < kLedgeCacheTtlMs) {
    return ledge_cache_;
  }
  ledge_cache_ = ListWindowLedges(hwnd_);
  ledge_cache_ms_ = now;
  return ledge_cache_;
}

void PetWindow::StartClimb() {
  if (!hwnd_) {
    return;
  }
  RECT self{};
  GetWindowRect(hwnd_, &self);
  const auto& ledges = CachedWindowLedges();
  if (ledges.empty()) {
    ShowToast(L"附近没有可攀的窗台");
    return;
  }
  // Prefer a ledge above the pet and horizontally overlapping; else nearest.
  const WindowLedge* pick = nullptr;
  int best_score = 0x7fffffff;
  for (const auto& L : ledges) {
    if (L.y >= self.top) {
      continue;
    }
    const int mid = self.left + (self.right - self.left) / 2;
    const int lx = L.x;
    const int rx = L.x + L.width;
    int dx = 0;
    if (mid < lx) {
      dx = lx - mid;
    } else if (mid > rx) {
      dx = mid - rx;
    }
    const int dy = self.top - L.y;
    const int score = dx * 2 + dy;
    if (score < best_score) {
      best_score = score;
      pick = &L;
    }
  }
  if (!pick) {
    pick = &ledges.front();
  }
  ledge_left_ = pick->x;
  ledge_right_ = pick->x + pick->width;
  ledge_top_ = pick->y;
  ledge_hwnd_ = pick->hwnd;
  climbing_ = true;
  on_ledge_ = false;
  walking_ = false;
  falling_ = false;
  climb_cooldown_ = 40;
  pos_x_ = static_cast<float>(self.left);
  pos_y_ = static_cast<float>(self.top);
  // Walk sideways toward the ledge while climbing.
  if (pos_x_ < static_cast<float>(ledge_left_)) {
    walk_dir_ = 1;
  } else if (pos_x_ + 40 > static_cast<float>(ledge_right_)) {
    walk_dir_ = -1;
  }
  Say("climb");
}

void PetWindow::StartWalk() {
  if (!auto_wander_) {
    return;
  }
  walking_ = true;
  falling_ = false;
  climbing_ = false;
  on_ledge_ = false;
  vel_x_ = 90.f * static_cast<float>(walk_dir_);  // px/s (Python wander pace)
  vel_y_ = 0.f;
  facing_ = walk_dir_ >= 0 ? 1 : -1;
  if (HasPose(L"walk")) {
    SetPose(L"walk");
  }
}

void PetWindow::NudgeClearOfFenceBottoms(
    const std::vector<RECT>& fence_screen_rects) {
  // P1-17: push the pet below any visible fence whose fill overlaps the pet
  // chrome (page chips + crown). Mirrors Python _nudge_clear_of_fence_bottoms.
  if (!hwnd_ || dragging_ || fence_screen_rects.empty()) {
    return;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  const int chrome_top = wr.top;
  const int chrome_bottom = wr.bottom;
  const int spr_l = wr.left;
  const int spr_r = wr.right;
  const int base_h = base_sprite_h_ > 0 ? base_sprite_h_ : sprite_h_;
  const int margin =
      (std::max)(20, static_cast<int>(std::lround(base_h * 0.14 * size_scale_)));
  int need_y = -1;
  for (const RECT& fr : fence_screen_rects) {
    // Horizontal overlap with an 8px inset (Python parity).
    if (spr_r < fr.left + 8 || spr_l > fr.right - 8) {
      continue;
    }
    const int fence_top = fr.top;
    const int fence_bottom = fr.bottom;
    POINT fence_mid{(fr.left + fr.right) / 2, (fr.top + fr.bottom) / 2};
    const RECT fence_work = WorkAreaAtPoint(fence_mid);
    if (fence_bottom >= fence_work.bottom - 16) {
      // Full-height / maximized fence reaching work area bottom — cannot nudge below.
      continue;
    }
    if (chrome_bottom <= fence_top || chrome_top >= fence_bottom) {
      continue;
    }
    const int delta = (fence_bottom + margin) - chrome_top;
    if (delta <= 0) {
      continue;
    }
    const int candidate = static_cast<int>(pos_y_) + delta;
    if (need_y < 0 || candidate > need_y) {
      need_y = candidate;
    }
  }
  if (need_y < 0 || need_y <= static_cast<int>(pos_y_)) {
    return;
  }
  // Clamp to the work area, only moving down.
  POINT mid{spr_l + ScaledW() / 2, need_y + ScaledH() / 2};
  const RECT work = WorkAreaAtPoint(mid);
  const int max_y = work.bottom - ScaledH() - GroundMarginPx();
  if (need_y > max_y) {
    need_y = max_y;
  }
  if (need_y <= static_cast<int>(pos_y_)) {
    return;
  }
  pos_y_ = static_cast<float>(need_y);
  present_dirty_ = true;
  Present();
  PlacePages();
  PlaceMenu();
  PersistPrefs();
  LogLine(L"[pet] nudge clear of fence bottom → y=%d", need_y);
}

void PetWindow::StartFall() {
  // Menu「跌落」: hop up then tumble (Python random vx/vy).
  const float vx = static_cast<float>(rand() % 521 - 260);
  const float vy = static_cast<float>(-520 + rand() % 161);
  StartThrow(vx, vy);
}

void PetWindow::StartThrow(float vx, float vy) {
  falling_ = true;
  walking_ = false;
  climbing_ = false;
  on_ledge_ = false;
  happy_active_ = false;
  vel_x_ = (std::max)(-kThrowMaxPxPerSec, (std::min)(kThrowMaxPxPerSec, vx));
  vel_y_ = (std::max)(-kThrowMaxPxPerSec, (std::min)(kThrowMaxPxPerSec, vy));
  // Keep pos_ (drag/ULW origin). GetWindowRect after SoftSeat/region is the
  // hit-region bbox and would yank the pet or leave Present painting off-box.
  RebaseFloor();
  if (HasPose(L"fall")) {
    SetPose(L"fall");
  }
}

void PetWindow::PushDragSample(POINT screen) {
  drag_samples_[drag_sample_i_].tick = GetTickCount();
  drag_samples_[drag_sample_i_].pt = screen;
  drag_sample_i_ = (drag_sample_i_ + 1) % 8;
  if (drag_sample_n_ < 8) {
    ++drag_sample_n_;
  }
}

void PetWindow::ShowContextMenu(POINT screen) {
  HMENU menu = CreatePopupMenu();
  HMENU poses = CreatePopupMenu();
  for (size_t i = 0; i < pose_names_.size(); ++i) {
    const UINT flags =
        MF_STRING | (pose_ == pose_names_[i] ? MF_CHECKED : 0);
    AppendMenuW(poses, flags, static_cast<UINT>(100 + i),
                pose_names_[i].c_str());
  }
  if (!pose_names_.empty()) {
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(poses), L"姿态");
  }
  HMENU chars = CreatePopupMenu();
  auto get_char_display_name = [](const std::wstring& id) -> std::wstring {
    if (id == L"hoodie") return L"卫衣少年";
    if (id == L"voyage") return L"出海少女";
    if (id == L"xianjian") return L"白衣剑仙";
    return id;
  };
  for (size_t i = 0; i < character_ids_.size(); ++i) {
    const UINT flags =
        MF_STRING | (character_ == character_ids_[i] ? MF_CHECKED : 0);
    const std::wstring label = get_char_display_name(character_ids_[i]);
    AppendMenuW(chars, flags, static_cast<UINT>(200 + i), label.c_str());
  }
  if (!character_ids_.empty()) {
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(chars), L"角色");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (follow_cursor_ ? MF_CHECKED : 0), 5,
              follow_cursor_ ? L"取消跟随光标" : L"跟随光标");
  AppendMenuW(menu, MF_STRING, 1, L"走走");
  AppendMenuW(menu, MF_STRING, 2, L"跌落");
  AppendMenuW(menu, MF_STRING, 3, L"待机");
  // Do not SetForegroundWindow(hwnd_) — pet sits in the desktop band; FG
  // would lift it over APPWINDOWs (same Fences-class rule as fences).
  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                     screen.x, screen.y, 0, hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  DestroyMenu(menu);
  if (cmd == 5) {
    follow_cursor_ = !follow_cursor_;
    walking_ = false;
    falling_ = false;
    climbing_ = false;
    on_ledge_ = false;
    if (follow_cursor_) {
      if (HasPose(L"walk")) {
        SetPose(L"walk");
      }
    } else if (HasPose(L"idle")) {
      SetPose(L"idle");
    }
    PersistPrefs();
  } else if (cmd == 1) {
    follow_cursor_ = false;
    StartWalk();
  } else if (cmd == 2) {
    follow_cursor_ = false;
    StartFall();
    climbing_ = false;
    on_ledge_ = false;
  } else if (cmd == 3) {
    follow_cursor_ = false;
    walking_ = false;
    falling_ = false;
    climbing_ = false;
    on_ledge_ = false;
    if (!pose_names_.empty()) {
      SetPose(L"idle");
    }
    PersistPrefs();
  } else if (cmd >= 100 &&
             cmd < 100 + static_cast<int>(pose_names_.size())) {
    walking_ = false;
    falling_ = false;
    SetPose(pose_names_[static_cast<size_t>(cmd - 100)]);
    PersistPrefs();  // explicit pose pick — persist (SetPose no longer saves)
  } else if (cmd >= 200 &&
             cmd < 200 + static_cast<int>(character_ids_.size())) {
    SetCharacter(character_ids_[static_cast<size_t>(cmd - 200)]);
  }
}

bool PetWindow::Create(HINSTANCE instance) {
  // Seed the shared rand() RNG once so idle→rest rolls (P1-18) and other pet
  // randomness are not deterministic across runs.
  static bool rng_seeded = false;
  if (!rng_seeded) {
    srand(static_cast<unsigned>(GetTickCount()) ^
          static_cast<unsigned>(GetCurrentProcessId()));
    rng_seeded = true;
  }
  if (!g_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PetWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    g_registered = true;
  }
  LoadPrefs();
  TryLoadSprite();
  LoadCharacters();
  LoadFramePack();
  // P6: initialize effective monitor DPI before sizing the window.
  const RECT work = PrimaryWorkArea();
  current_dpi_ = DpiAtPoint({work.left + 4, work.top + 4});
  const int ww = (sprite_ || !frames_.empty()) ? ScaledW() : kSize;
  const int hh = (sprite_ || !frames_.empty()) ? ScaledH() : kSize;
  // P2: restore saved position (falls back to primary bottom-right when the
  // saved monitor is disconnected).
  RestorePosition();
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName, L"Pet",
      WS_POPUP, static_cast<int>(pos_x_), static_cast<int>(pos_y_), ww, hh,
      nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  // Per-pixel alpha via UpdateLayeredWindow in Present() — do not also call
  // SetLayeredWindowAttributes (they conflict once ULW_ALPHA is used).
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  EnsureDropTarget();
  CreateBuoy(instance);
  CreatePages(instance);
  CreateMenu(instance);
  last_tick_ms_ = GetTickCount();
  last_interact_ms_ = last_tick_ms_;
  state_until_ms_ = last_tick_ms_ + 4000;
  present_dirty_ = true;
  chrome_dirty_ = true;
  Present();
  PlacePages();
  PlaceMenu();
  return true;
}

void PetWindow::CreateBuoy(HINSTANCE instance) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PetWindow::BuoyProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = L"DeskTidyNativePetBuoy";
    RegisterClassExW(&wc);
    registered = true;
  }
  const RECT work = PrimaryWorkArea();
  buoy_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
      L"DeskTidyNativePetBuoy", L"PetBuoy", WS_POPUP, work.left + 12,
      work.bottom - 44, 88, 28, nullptr, nullptr, instance, this);
  if (!buoy_) {
    return;
  }
  SetLayeredWindowAttributes(buoy_, 0, 220, LWA_ALPHA);
  SetWindowLongPtrW(buoy_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

void PetWindow::SetToolFlags(bool record, bool minutes, bool calculator,
                             bool todo, bool vault) {
  tool_record_ = record;
  tool_minutes_ = minutes;
  tool_calculator_ = calculator;
  tool_todo_ = todo;
  tool_vault_ = vault;
  RebuildChromeItems();
  if (pages_) {
    PlacePages();
  }
}

void PetWindow::SetPages(const std::vector<std::wstring>& names, int current) {
  const bool names_changed = names != page_names_;
  const bool page_changed = current != current_page_;
  page_names_ = names;
  current_page_ = current;
  // Page-switch only changes the highlight — skip SetWindowPos (page flash).
  if (names_changed) {
    RebuildChromeItems();
    if (pages_) {
      PlacePages();
    }
    return;
  }
  if (page_changed && pages_ && IsWindowVisible(pages_)) {
    PaintPages(nullptr);
  }
}

void PetWindow::SetFolders(const std::vector<FolderChip>& folders) {
  folders_ = folders;
  RebuildChromeItems();
  if (pages_) {
    PlacePages();
  }
}

void PetWindow::SetAutoWander(bool on) {
  auto_wander_ = on;
  if (!on) {
    walking_ = false;
  }
  PersistPrefs();
}

void PetWindow::SetShowPageBubbles(bool on) {
  show_page_bubbles_ = on;
  PersistPrefs();
  if (pages_) {
    PlacePages();
  }
}

void PetWindow::SetSizeScale(float scale) {
  if (scale < 0.5f) {
    scale = 0.5f;
  }
  if (scale > 2.0f) {
    scale = 2.0f;
  }
  size_scale_ = scale;
  PersistPrefs();
  if (hwnd_ && sprite_w_ > 0 && sprite_h_ > 0) {
    // P6: apply scale + DPI immediately without losing it on the next pose swap.
    RebaseFloor();
    ApplyScaledWindowSize();
    present_dirty_ = true;
    chrome_dirty_ = true;
    PlacePages();
    PlaceMenu();
  }
}

void PetWindow::RebuildChromeItems() {
  chrome_.clear();
  if (!show_page_bubbles_) {
    return;
  }
  // Match Python float bar: show up to 12 page chips (was hard-capped at 8).
  const int n = (std::min)(static_cast<int>(page_names_.size()), 12);
  for (int i = 0; i < n; ++i) {
    ChromeItem item;
    item.kind = ChromeItem::Kind::Page;
    item.page_index = i;
    item.label = ChipLabel(page_names_[static_cast<size_t>(i)], i);
    chrome_.push_back(std::move(item));
  }
  for (const auto& f : folders_) {
    if (f.path.empty()) {
      continue;
    }
    ChromeItem item;
    item.kind = ChromeItem::Kind::Folder;
    item.folder_path = f.path;
    item.label = ChipLabel(f.name.empty() ? L"文件夹" : f.name, 0);
    chrome_.push_back(std::move(item));
  }
  // Python float_bar_tool_flags — never +/−/note/search/shot on the float bar.
  auto add_tool = [&](bool on, const wchar_t* id, const wchar_t* label) {
    if (!on) {
      return;
    }
    ChromeItem item;
    item.kind = ChromeItem::Kind::Tool;
    item.tool_id = id;
    item.label = label;
    chrome_.push_back(std::move(item));
  };
  add_tool(tool_record_, L"record", L"录屏");
  add_tool(tool_minutes_, L"minutes", L"纪要");
  add_tool(tool_calculator_, L"calculator", L"计算");
  add_tool(tool_todo_, L"todo", L"待办");
  add_tool(tool_vault_, L"vault", L"账号");
}

void PetWindow::CreatePages(HINSTANCE instance) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PetWindow::PagesProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = L"DeskTidyNativePetPages";
    RegisterClassExW(&wc);
    registered = true;
  }
  pages_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
      L"DeskTidyNativePetPages", L"PetPages", WS_POPUP, 0, 0, 80,
      kPageChipH + kPagePointer + 8, nullptr, nullptr, instance, this);
  if (!pages_) {
    return;
  }
  // Per-pixel alpha via UpdateLayeredWindow (no color-key jaggies).
  SetWindowLongPtrW(pages_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

void PetWindow::LayoutChrome(std::vector<RECT>* out_rects, int* out_w,
                             int* out_h) const {
  if (out_rects) {
    out_rects->clear();
  }
  if (chrome_.empty()) {
    if (out_w) {
      *out_w = 40;
    }
    if (out_h) {
      *out_h = kPageChipH + kPagePointer + 4;
    }
    return;
  }

  HDC hdc = pages_ ? GetDC(pages_) : GetDC(nullptr);
  HFONT font =
      CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                  DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
  HGDIOBJ old = SelectObject(hdc, font);

  std::vector<int> widths(chrome_.size());
  for (size_t i = 0; i < chrome_.size(); ++i) {
    widths[i] = MeasureChipWidth(hdc, chrome_[i].label);
  }
  SelectObject(hdc, old);
  DeleteObject(font);
  ReleaseDC(pages_ ? pages_ : nullptr, hdc);

  // Pack into centered rows (Python: max 3 per row).
  const int row_stride = kPageChipH + kPagePointer + kPageRowGap;
  const int row_budget = 260;
  struct Chip {
    int index;
    int w;
  };
  std::vector<std::vector<Chip>> rows;
  std::vector<Chip> cur;
  int cur_w = 0;
  for (int i = 0; i < static_cast<int>(chrome_.size()); ++i) {
    const int w = widths[static_cast<size_t>(i)];
    const int need = cur.empty() ? w : cur_w + kPageGap + w;
    if (!cur.empty() &&
        (static_cast<int>(cur.size()) >= kPageMaxRow || need > row_budget)) {
      rows.push_back(cur);
      cur.clear();
      cur_w = 0;
    }
    cur.push_back({i, w});
    cur_w = 0;
    for (size_t j = 0; j < cur.size(); ++j) {
      cur_w += cur[j].w;
      if (j + 1 < cur.size()) {
        cur_w += kPageGap;
      }
    }
  }
  if (!cur.empty()) {
    rows.push_back(cur);
  }

  std::vector<RECT> ordered(chrome_.size());
  int max_x = 0;
  int max_y = 0;
  float y = static_cast<float>(kPagePad + 2);
  for (const auto& row : rows) {
    int row_w = 0;
    for (size_t j = 0; j < row.size(); ++j) {
      row_w += row[j].w;
      if (j + 1 < row.size()) {
        row_w += kPageGap;
      }
    }
    // Tentative left; recenter after measuring cloud width.
    float x = static_cast<float>(kPagePad);
    for (const auto& c : row) {
      RECT r{};
      r.left = static_cast<LONG>(x);
      r.top = static_cast<LONG>(y);
      r.right = r.left + c.w;
      r.bottom = r.top + kPageChipH;
      ordered[static_cast<size_t>(c.index)] = r;
      max_x = (std::max)(max_x, static_cast<int>(r.right));
      max_y = (std::max)(max_y, static_cast<int>(r.bottom) + kPagePointer);
      x += static_cast<float>(c.w + kPageGap);
    }
    y += static_cast<float>(row_stride);
  }

  // Center each row inside the final cloud width.
  const int cloud_w = (std::max)(max_x + kPagePad, 80);
  y = static_cast<float>(kPagePad + 2);
  for (const auto& row : rows) {
    int row_w = 0;
    for (size_t j = 0; j < row.size(); ++j) {
      row_w += row[j].w;
      if (j + 1 < row.size()) {
        row_w += kPageGap;
      }
    }
    float x = (cloud_w - row_w) * 0.5f;
    for (const auto& c : row) {
      RECT& r = ordered[static_cast<size_t>(c.index)];
      r.left = static_cast<LONG>(x);
      r.top = static_cast<LONG>(y);
      r.right = r.left + c.w;
      r.bottom = r.top + kPageChipH;
      x += static_cast<float>(c.w + kPageGap);
    }
    y += static_cast<float>(row_stride);
  }

  if (out_rects) {
    *out_rects = std::move(ordered);
  }
  if (out_w) {
    *out_w = cloud_w;
  }
  if (out_h) {
    *out_h = (std::max)(max_y + 2, kPageChipH + kPagePointer + 4);
  }
}

int PetWindow::HitTestChrome(int local_x, int local_y) const {
  std::vector<RECT> rects;
  LayoutChrome(&rects, nullptr, nullptr);
  for (int i = 0; i < static_cast<int>(rects.size()); ++i) {
    const RECT& r = rects[static_cast<size_t>(i)];
    // Include pointer tip under the chip.
    if (local_x >= r.left && local_x < r.right && local_y >= r.top &&
        local_y < r.bottom + kPagePointer) {
      return i;
    }
  }
  return -1;
}

void PetWindow::ActivateChrome(int index) {
  if (index < 0 || index >= static_cast<int>(chrome_.size())) {
    return;
  }
  const ChromeItem& item = chrome_[static_cast<size_t>(index)];
  if (item.kind == ChromeItem::Kind::Page) {
    if (on_page_) {
      on_page_(item.page_index);
    }
    return;
  }
  if (item.kind == ChromeItem::Kind::Folder) {
    if (on_folder_) {
      on_folder_(item.folder_path);
    }
    return;
  }
  if (on_tool_) {
    on_tool_(item.tool_id);
  }
}

void PetWindow::PlacePages() {
  if (!pages_ || !hwnd_) {
    return;
  }
  // Pet hidden ⇒ chrome bubbles must disappear (Python mutual exclusion).
  if (!IsWindowVisible(hwnd_)) {
    ShowWindow(pages_, SW_HIDE);
    return;
  }
  // P5: bubbles hidden while dragging (Python _paint_page_bubbles).
  if (dragging_) {
    ShowWindow(pages_, SW_HIDE);
    return;
  }
  if (chrome_.empty()) {
    RebuildChromeItems();
  }
  if (chrome_.empty()) {
    ShowWindow(pages_, SW_HIDE);
    return;
  }
  // Use pos_/Scaled* — GetWindowRect lags ULW-only moves and left bubbles
  // stranded where the pet used to be after a blanking drop (see recording).
  const int pet_w = ScaledW();
  const int pet_h = ScaledH();
  int w = 80;
  int h = kPageChipH + kPagePointer + 4;
  LayoutChrome(nullptr, &w, &h);
  const int head_clear = PageHeadClearancePx(pet_h);
  const int x = static_cast<int>(pos_x_) + (pet_w - w) / 2;
  const int y = static_cast<int>(pos_y_) - h - head_clear;
  SetWindowPos(pages_, nullptr, x, y, w, h,
               SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
  PaintPages(nullptr);
}

void PetWindow::PaintPages(HDC) {
  if (!pages_) {
    return;
  }
  if (chrome_.empty()) {
    RebuildChromeItems();
  }
  std::vector<RECT> rects;
  LayoutChrome(&rects, nullptr, nullptr);

  PresentTransparent(pages_, [&](Gdiplus::Graphics& g, int, int) {
    Gdiplus::FontFamily family(L"Microsoft YaHei UI");
    const Gdiplus::FontFamily* face =
        family.IsAvailable() ? &family : Gdiplus::FontFamily::GenericSansSerif();
    Gdiplus::Font font(face, 12.f, Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    for (int i = 0; i < static_cast<int>(chrome_.size()); ++i) {
      const ChromeItem& item = chrome_[static_cast<size_t>(i)];
      const RECT& r = rects[static_cast<size_t>(i)];
      const bool current =
          item.kind == ChromeItem::Kind::Page && item.page_index == current_page_;
      const bool hovered = (i == chrome_hover_);
      Gdiplus::RectF body(static_cast<Gdiplus::REAL>(r.left),
                          static_cast<Gdiplus::REAL>(r.top),
                          static_cast<Gdiplus::REAL>(r.right - r.left),
                          static_cast<Gdiplus::REAL>(r.bottom - r.top));
      DrawRoundChip(g, body, current, true, hovered);
      const float mid = body.X + body.Width * 0.5f;
      const float tip = body.Y + body.Height + static_cast<float>(kPagePointer);
      Gdiplus::PointF tri[3] = {
          {mid, tip},
          {mid - 5.f, body.Y + body.Height - 1.f},
          {mid + 5.f, body.Y + body.Height - 1.f},
      };
      Gdiplus::SolidBrush tip_br(current ? Gdiplus::Color(244, 219, 234, 254)
                                         : Gdiplus::Color(230, 255, 255, 255));
      g.FillPolygon(&tip_br, tri, 3);
      Gdiplus::SolidBrush text_br(
          current ? Gdiplus::Color(255, 37, 99, 235)
                  : Gdiplus::Color(255, 31, 41, 55));
      g.DrawString(item.label.c_str(), -1, &font, body, &fmt, &text_br);
    }
  });
}

LRESULT CALLBACK PetWindow::PagesProc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
  auto* self =
      reinterpret_cast<PetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);
    self->PaintPages(nullptr);
    return 0;
  }
  if (msg == WM_MOUSEMOVE) {
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd;
    TrackMouseEvent(&tme);
    const int index =
        self->HitTestChrome(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
    if (index != self->chrome_hover_) {
      self->chrome_hover_ = index;
      self->PaintPages(nullptr);
    }
    return 0;
  }
  if (msg == WM_MOUSELEAVE) {
    if (self->chrome_hover_ >= 0) {
      self->chrome_hover_ = -1;
      self->PaintPages(nullptr);
    }
    return 0;
  }
  if (msg == WM_LBUTTONUP) {
    const int index =
        self->HitTestChrome(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
    self->ActivateChrome(index);
    return 0;
  }
  if (msg == WM_RBUTTONUP) {
    // P1-16: right-click page chip → new fence on that page.
    const int index =
        self->HitTestChrome(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
    if (index >= 0 && index < static_cast<int>(self->chrome_.size())) {
      const ChromeItem& item = self->chrome_[static_cast<size_t>(index)];
      if (item.kind == ChromeItem::Kind::Page && self->on_create_fence_) {
        self->on_create_fence_(item.page_index);
      }
    }
    return 0;
  }
  if (msg == WM_DESTROY) {
    self->pages_ = nullptr;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void PetWindow::PaintBuoy(HDC hdc) {
  RECT rc{};
  GetClientRect(buoy_, &rc);
  HBRUSH bg = CreateSolidBrush(RGB(28, 32, 40));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);
  const wchar_t* labels[] = {L"走", L"落"};
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(230, 230, 230));
  for (int i = 0; i < 2; ++i) {
    RECT r{i * 44 + 2, 2, (i + 1) * 44 - 2, rc.bottom - 2};
    HBRUSH b = CreateSolidBrush(RGB(55, 70, 90));
    FillRect(hdc, &r, b);
    DeleteObject(b);
    DrawTextW(hdc, labels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
}

LRESULT CALLBACK PetWindow::BuoyProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  auto* self =
      reinterpret_cast<PetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    self->PaintBuoy(hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_LBUTTONUP) {
    const int x = GET_X_LPARAM(lparam);
    if (x < 44) {
      self->StartWalk();
    } else {
      self->StartFall();
    }
    return 0;
  }
  if (msg == WM_DESTROY) {
    self->buoy_ = nullptr;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void PetWindow::CreateMenu(HINSTANCE instance) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PetWindow::MenuProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = L"DeskTidyNativePetMenu";
    RegisterClassExW(&wc);
    registered = true;
  }
  menu_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
      L"DeskTidyNativePetMenu", L"PetMenu", WS_POPUP, 0, 0, kMenuBtn, kMenuBtn,
      nullptr, nullptr, instance, this);
  if (!menu_) {
    return;
  }
  SetWindowLongPtrW(menu_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

void PetWindow::PlaceMenu() {
  if (!menu_ || !hwnd_) {
    return;
  }
  if (!IsWindowVisible(hwnd_)) {
    ShowWindow(menu_, SW_HIDE);
    return;
  }
  const int pet_h = ScaledH();
  // Python: ⋯ hugs sprite shoulder, not the crown.
  const int menu_y = static_cast<int>(pos_y_) + (std::max)(8, pet_h / 5);
  const int cols = 2;
  const int n_actions = static_cast<int>(VisibleMenuIndices().size());
  const int rows = (std::max)(1, (n_actions + cols - 1) / cols);
  const int panel_w =
      kActionPad * 2 + cols * kActionBtn + (cols - 1) * kActionGap;
  const int panel_h =
      kActionPad * 2 + rows * kActionBtn + (rows - 1) * kActionGap;
  // Grow to the RIGHT of the pet so actions never cover hair.
  const int w = menu_open_ ? kMenuBtn + 4 + panel_w : kMenuBtn;
  const int h = menu_open_ ? (std::max)(kMenuBtn, panel_h) : kMenuBtn;
  const int x = static_cast<int>(pos_x_) + ScaledW() - kMenuBtn - 2;
  SetWindowPos(menu_, nullptr, x, menu_y, w, h,
               SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
  PaintMenu(nullptr);
}

void PetWindow::PaintMenu(HDC) {
  if (!menu_) {
    return;
  }
  // 白衣剑仙：摸 / 眠(倚树打盹) / 落 / 隐（无「息」）。
  const bool xj = _wcsicmp(character_.c_str(), L"xianjian") == 0;
  const wchar_t* glyphs[kMenuActions] = {
      L"摸",
      pose_ == L"wait" ? L"起" : L"游",
      pose_ == L"sleep" ? L"起" : (xj ? L"眠" : L"睡"),
      L"落",
      L"隐",
  };
  const std::vector<int> visible = VisibleMenuIndices();
  PresentTransparent(menu_, [&](Gdiplus::Graphics& g, int, int) {
    Gdiplus::FontFamily family(L"Microsoft YaHei UI");
    const Gdiplus::FontFamily* face =
        family.IsAvailable() ? &family : Gdiplus::FontFamily::GenericSansSerif();
    Gdiplus::Font bold(face, 11.f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font regular(face, 10.f, Gdiplus::FontStyleBold,
                          Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    auto draw_circle = [&](float x, float y, float d, const wchar_t* text,
                           bool hot, const Gdiplus::Font& font) {
      Gdiplus::SolidBrush fill(hot ? Gdiplus::Color(255, 219, 234, 254)
                                   : Gdiplus::Color(235, 255, 255, 255));
      g.FillEllipse(&fill, x, y, d, d);
      Gdiplus::Pen pen(hot ? Gdiplus::Color(220, 37, 99, 235)
                           : Gdiplus::Color(150, 213, 220, 231),
                       hot ? 1.5f : 1.0f);
      g.DrawEllipse(&pen, x, y, d, d);
      Gdiplus::SolidBrush ink(Gdiplus::Color(255, 55, 65, 81));
      g.DrawString(text, -1, &font, Gdiplus::RectF(x, y, d, d), &fmt, &ink);
    };

    draw_circle(0.f, 0.f, static_cast<float>(kMenuBtn), L"⋯", menu_open_, bold);
    if (!menu_open_) {
      return;
    }
    const float ox = static_cast<float>(kMenuBtn + 4 + kActionPad);
    for (size_t slot = 0; slot < visible.size(); ++slot) {
      const int action = visible[slot];
      if (action < 0 || action >= kMenuActions) {
        continue;
      }
      const int col = static_cast<int>(slot) % 2;
      const int row = static_cast<int>(slot) / 2;
      const float x = ox + static_cast<float>(col * (kActionBtn + kActionGap));
      const float y =
          static_cast<float>(kActionPad + row * (kActionBtn + kActionGap));
      draw_circle(x, y, static_cast<float>(kActionBtn), glyphs[action], false,
                  regular);
    }
  });
}

void PetWindow::RunMenuAction(int index) {
  menu_open_ = false;
  PlaceMenu();
  auto has_pose = [&](const wchar_t* name) {
    return std::find(pose_names_.begin(), pose_names_.end(), name) !=
           pose_names_.end();
  };
  auto stop_motion = [&]() {
    walking_ = false;
    falling_ = false;
    climbing_ = false;
    on_ledge_ = false;
  };
  if (index == 0) {
    stop_motion();
    if (has_pose(L"idle")) {
      SetPose(L"idle");
    }
    Say("pet");
  } else if (index == 1) {
    stop_motion();
    if (pose_ == L"wait" && has_pose(L"idle")) {
      SetPose(L"idle");
      Say("wake");
    } else if (has_pose(L"wait")) {
      SetPose(L"wait");
      Say("wait");
    }
  } else if (index == 2) {
    stop_motion();
    if (pose_ == L"sleep" && has_pose(L"idle")) {
      SetPose(L"idle");
      Say("wake");
    } else if (has_pose(L"sleep")) {
      if (character_ == L"xianjian") {
        facing_ = 1;
      }
      SetPose(L"sleep");
      cursor_was_near_ = true;
      const DWORD now = GetTickCount();
      const DWORD base = character_ == L"xianjian" ? 18000u : 14000u;
      const DWORD span = character_ == L"xianjian" ? 12000u : 10000u;
      state_until_ms_ = now + base + static_cast<DWORD>(rand() % span);
      Say("sleep");
    }
  } else if (index == 3) {
    Say("fall");
    StartFall();
  } else if (index == 4 && on_hide_) {
    on_hide_();
  }
}

LRESULT CALLBACK PetWindow::MenuProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  auto* self =
      reinterpret_cast<PetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);
    self->PaintMenu(nullptr);
    return 0;
  }
  if (msg == WM_LBUTTONUP) {
    const int x = GET_X_LPARAM(lparam);
    const int y = GET_Y_LPARAM(lparam);
    if (x < kMenuBtn && y < kMenuBtn) {
      self->menu_open_ = !self->menu_open_;
      self->PlaceMenu();
      return 0;
    }
    if (!self->menu_open_) {
      return 0;
    }
    const int ox = kMenuBtn + 4 + kActionPad;
    const int col = (x - ox) / (kActionBtn + kActionGap);
    const int row = (y - kActionPad) / (kActionBtn + kActionGap);
    if (col >= 0 && col < 2 && row >= 0) {
      const int slot = row * 2 + col;
      const std::vector<int> visible = self->VisibleMenuIndices();
      if (slot >= 0 && slot < static_cast<int>(visible.size())) {
        self->RunMenuAction(visible[static_cast<size_t>(slot)]);
      }
    }
    return 0;
  }
  if (msg == WM_DESTROY) {
    self->menu_ = nullptr;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void PetWindow::EnsureDropTarget() {
  if (!hwnd_ || drop_target_) {
    return;
  }
  drop_target_ =
      new PathDropTarget([this](const std::vector<std::wstring>& paths,
                                DWORD proposed) -> DWORD {
        auto has_pose = [&](const wchar_t* name) {
          return std::find(pose_names_.begin(), pose_names_.end(), name) !=
                 pose_names_.end();
        };
        walking_ = false;
        falling_ = false;
        bool ok = false;
        if (on_trash_) {
          ok = on_trash_(paths);
        }
        if (ok) {
          Say("trash");
          if (has_pose(L"trash")) {
            SetPose(L"trash");
            // P5: one-shot crumple → hold last cel, then back to idle/wait.
            const bool voyage = _wcsicmp(character_.c_str(), L"voyage") == 0;
            int cels = static_cast<int>(frames_.size());
            int played = cels;
            if (!voyage && cels >= 10) {
              played = cels - 2;  // hoodie skips the last two cels (Python)
            }
            const int interval = FrameIntervalMs(L"trash");
            DWORD dur =
                static_cast<DWORD>(played * interval) + (voyage ? 400 : 0);
            if (dur < 1200) {
              dur = 1200;
            }
            last_interact_ms_ = GetTickCount();
            state_until_ms_ = GetTickCount() + dur;
          } else if (has_pose(L"idle")) {
            SetPose(L"idle");
          }
        }
        // AIGC START
        return ok ? proposed : DROPEFFECT_NONE;
        // AIGC END
      });
  // AIGC START
  drop_target_->SetFileDropSemantics(FileDropSemantics::PreferMove);
  // AIGC END
  if (!RegisterPathDropTarget(hwnd_, drop_target_)) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
}

void PetWindow::ReleasePresentBuffer() {
  if (present_dc_ && present_old_) {
    SelectObject(present_dc_, present_old_);
    present_old_ = nullptr;
  }
  if (present_dib_) {
    DeleteObject(present_dib_);
    present_dib_ = nullptr;
  }
  if (present_dc_) {
    DeleteDC(present_dc_);
    present_dc_ = nullptr;
  }
  present_bits_ = nullptr;
  present_buf_w_ = 0;
  present_buf_h_ = 0;
  present_content_valid_ = false;
  present_frame_i_ = -1;
  present_facing_ = 0;
  present_anim_tick_ = -1;
}

void PetWindow::ReleaseFlipBuffer() {
  if (flip_dc_ && flip_old_) {
    SelectObject(flip_dc_, flip_old_);
    flip_old_ = nullptr;
  }
  if (flip_dc_) {
    DeleteDC(flip_dc_);
    flip_dc_ = nullptr;
  }
  if (flip_dib_) {
    DeleteObject(flip_dib_);
    flip_dib_ = nullptr;
  }
  flip_bits_ = nullptr;
  flip_src_ = nullptr;
  flip_w_ = 0;
  flip_h_ = 0;
}

HDC PetWindow::EnsureFlipBuffer(HDC hdc, HBITMAP bmp, int bw, int bh) {
  if (flip_dc_ && flip_dib_ && flip_bits_ && flip_src_ == bmp &&
      flip_w_ == bw && flip_h_ == bh) {
    return flip_dc_;
  }
  ReleaseFlipBuffer();
  if (!bmp || bw <= 0 || bh <= 0) {
    return nullptr;
  }
  HDC dc = CreateCompatibleDC(hdc);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = bw;
  bi.bmiHeader.biHeight = -bh;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* dst_bits = nullptr;
  HBITMAP dst_dib =
      CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &dst_bits, nullptr, 0);
  if (!dc || !dst_dib || !dst_bits) {
    if (dst_dib) {
      DeleteObject(dst_dib);
    }
    if (dc) {
      DeleteDC(dc);
    }
    return nullptr;
  }
  // Rebuild only (frame/facing/dim changed): copy the source frame into a
  // top-down 32bpp DIB, mirror each row, keep the result selected in `dc`.
  void* src_bits = nullptr;
  HBITMAP src_dib =
      CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &src_bits, nullptr, 0);
  HDC src_frame = CreateCompatibleDC(hdc);
  HDC src_dc = CreateCompatibleDC(hdc);
  if (!src_dib || !src_bits || !src_frame || !src_dc) {
    if (src_dib) {
      DeleteObject(src_dib);
    }
    if (src_frame) {
      DeleteDC(src_frame);
    }
    if (src_dc) {
      DeleteDC(src_dc);
    }
    DeleteObject(dst_dib);
    DeleteDC(dc);
    return nullptr;
  }
  HGDIOBJ old_f = SelectObject(src_frame, bmp);
  HGDIOBJ old_s = SelectObject(src_dc, src_dib);
  BitBlt(src_dc, 0, 0, bw, bh, src_frame, 0, 0, SRCCOPY);
  SelectObject(src_frame, old_f);
  SelectObject(src_dc, old_s);
  auto* s = static_cast<DWORD*>(src_bits);
  auto* d = static_cast<DWORD*>(dst_bits);
  for (int y = 0; y < bh; ++y) {
    const DWORD* row = s + static_cast<size_t>(y) * bw;
    DWORD* out = d + static_cast<size_t>(y) * bw;
    for (int x = 0; x < bw; ++x) {
      out[x] = row[bw - 1 - x];
    }
  }
  DeleteObject(src_dib);
  DeleteDC(src_frame);
  DeleteDC(src_dc);
  flip_old_ = SelectObject(dc, dst_dib);
  flip_dc_ = dc;
  flip_dib_ = dst_dib;
  flip_bits_ = dst_bits;
  flip_src_ = bmp;
  flip_w_ = bw;
  flip_h_ = bh;
  return dc;
}

void PetWindow::EnsureFlippedPargbCel(size_t idx) {
  if (idx >= frames_pargb_.size()) {
    return;
  }
  if (frames_pargb_flip_.size() != frames_pargb_.size()) {
    frames_pargb_flip_.assign(frames_pargb_.size(), {});
  }
  if (!frames_pargb_flip_[idx].empty()) {
    return;
  }
  const auto& src = frames_pargb_[idx];
  const int bw = sprite_w_;
  const int bh = sprite_h_;
  if (bw <= 0 || bh <= 0 ||
      static_cast<int>(src.size()) < bw * bh * 4) {
    return;
  }
  std::vector<BYTE> dst(src.size());
  for (int y = 0; y < bh; ++y) {
    const BYTE* row = src.data() + static_cast<size_t>(y) * bw * 4;
    BYTE* out = dst.data() + static_cast<size_t>(y) * bw * 4;
    for (int x = 0; x < bw; ++x) {
      const BYTE* p = row + (bw - 1 - x) * 4;
      BYTE* q = out + x * 4;
      q[0] = p[0];
      q[1] = p[1];
      q[2] = p[2];
      q[3] = p[3];
    }
  }
  frames_pargb_flip_[idx] = std::move(dst);
}

bool PetWindow::EnsurePresentBuffer(int w, int h) {
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (present_dc_ && present_dib_ && present_bits_ && present_buf_w_ == w &&
      present_buf_h_ == h) {
    return true;
  }
  ReleasePresentBuffer();
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return false;
  }
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib =
      CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!mem || !dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    if (mem) {
      DeleteDC(mem);
    }
    return false;
  }
  present_old_ = SelectObject(mem, dib);
  present_dc_ = mem;
  present_dib_ = dib;
  present_bits_ = bits;
  present_buf_w_ = w;
  present_buf_h_ = h;
  return true;
}

void PetWindow::Destroy() {
  if (buoy_) {
    DestroyWindow(buoy_);
    buoy_ = nullptr;
  }
  if (menu_) {
    DestroyWindow(menu_);
    menu_ = nullptr;
  }
  if (pages_) {
    DestroyWindow(pages_);
    pages_ = nullptr;
  }
  if (hwnd_) {
    RevokePathDropTarget(hwnd_);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (drop_target_) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
  if (sprite_) {
    DeleteObject(sprite_);
    sprite_ = nullptr;
  }
  ClearFrames();
  ReleasePresentBuffer();
  ReleaseFlipBuffer();
}

void PetWindow::Show() {
  if (!hwnd_) {
    return;
  }
  const bool was = IsWindowVisible(hwnd_) != FALSE;
  if (!was) {
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }
  AttachToDesktopLayer();
  // SoftSeat/Raise wipe UpdateLayeredWindow pixels even when already mapped —
  // always re-blit (fence Show does the same; skipping left a blank pet).
  Present();
  // Buoy (走/爬/落) is superseded by the ⋯ action menu, matching Python.
  if (pages_) {
    if (!IsWindowVisible(pages_)) {
      ShowWindow(pages_, SW_SHOWNOACTIVATE);
    }
    // AIGC START
    if (!AttachOverlayToDesktop(pages_, true)) {
      ForceAttachOverlayToDesktop(pages_, true);
    }
    // AIGC END
    PlacePages();
  }
  if (menu_) {
    if (!IsWindowVisible(menu_)) {
      ShowWindow(menu_, SW_SHOWNOACTIVATE);
    }
    // AIGC START
    if (!AttachOverlayToDesktop(menu_, true)) {
      ForceAttachOverlayToDesktop(menu_, true);
    }
    // AIGC END
    PlaceMenu();
  }
}

void PetWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
  if (buoy_) {
    ShowWindow(buoy_, SW_HIDE);
  }
  if (pages_) {
    ShowWindow(pages_, SW_HIDE);
  }
  if (menu_) {
    ShowWindow(menu_, SW_HIDE);
  }
}

void PetWindow::SetVisible(bool on) {
  if (on) {
    Show();
  } else {
    Hide();
  }
}

void PetWindow::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  if (!AttachOverlayToDesktop(hwnd_, true)) {
    ForceAttachOverlayToDesktop(hwnd_, true);
  }
  RaiseOverlayInDesktopBand(hwnd_);
  if (pages_) {
    if (!AttachOverlayToDesktop(pages_, true)) {
      ForceAttachOverlayToDesktop(pages_, true);
    }
    RaiseOverlayInDesktopBand(pages_);
  }
  if (menu_) {
    if (!AttachOverlayToDesktop(menu_, true)) {
      ForceAttachOverlayToDesktop(menu_, true);
    }
    RaiseOverlayInDesktopBand(menu_);
  }
}

void PetWindow::RefreshLayer() {
  if (!hwnd_ || !IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
    return;
  }
  // ULW pptDst + Scaled size in Present() reseats without SetWindowPos wipe.
  Present();
}

void PetWindow::BeginDragFloat() {
  if (!hwnd_) {
    return;
  }
  // Clear hit-region for the drag so GetWindowRect stays full sprite size.
  // No SetWindowPos — ULW pptDst+SIZE in Present reseats without wiping.
  SetWindowRgn(hwnd_, nullptr, FALSE);
  if (mask_rgn_) {
    DeleteObject(mask_rgn_);
    mask_rgn_ = nullptr;
  }
  Present();
}

void PetWindow::EndDragFloat() {
  if (!hwnd_) {
    return;
  }
  if (!IsAttachedToDesktop(hwnd_)) {
    AttachToDesktopLayer();
  }
  Present();
  present_dirty_ = true;
}

int PetWindow::WantedTickMs() const {
  if (!hwnd_ || !IsWindowVisible(hwnd_)) {
    return 1000;
  }
  if (dragging_) {
    return 16;
  }
  // wait = 站立 — same cadence as idle (was 33ms and felt stuttery/blank).
  const bool quiet_pose = pose_ == L"idle" || pose_ == L"sleep" ||
                          pose_ == L"perch" || pose_ == L"wait";
  if (!quiet_pose) {
    return 33;
  }
  POINT pt{};
  GetCursorPos(&pt);
  const RECT rc{static_cast<LONG>(pos_x_), static_cast<LONG>(pos_y_),
                static_cast<LONG>(pos_x_) + ScaledW(),
                static_cast<LONG>(pos_y_) + ScaledH()};
  if (PtInRect(&rc, pt)) {
    return 33;
  }
  // Soft bob / ZZZ only — market desktop pets idle ~1 Hz when unhovered
  // (was ~5 Hz ULW and burned CPU with many apps open).
  return 1000;
}

void PetWindow::Tick() {
  if (!hwnd_) {
    return;
  }
  const DWORD now = GetTickCount();
  const DWORD elapsed =
      (now >= last_tick_ms_) ? (now - last_tick_ms_) : 0;
  last_tick_ms_ = now;

  const bool had_hearts = !hearts_.empty();
  for (size_t i = 0; i < hearts_.size();) {
    if (now - hearts_[i].born > 1100) {
      hearts_.erase(hearts_.begin() + i);
    } else {
      ++i;
    }
  }
  const bool hearts_active = !hearts_.empty();

  if (!IsWindowVisible(hwnd_)) {
    return;
  }

  RunAi(now);
  ++anim_tick_;

  bool frame_changed = false;
  if (dragging_) {
    const bool use_hold = HasPose(L"hold");
    bool pose_switched = false;
    if (use_hold && pose_ != L"hold") {
      SetPose(L"hold");
      pose_switched = true;
    }
    AdvanceFrames(elapsed, now, &frame_changed);
    if (chrome_dirty_) {
      PlacePages();
      PlaceMenu();
      chrome_dirty_ = false;
    }
    // Position is driven by WM_MOUSEMOVE PresentMoveOnly. Full Present only
    // when the cel/pose changes (avoids 2× ULW with mouse moves).
    if (pose_switched || frame_changed || !present_content_valid_) {
      Present();
    }
    present_dirty_ = false;
    return;
  }

  AdvanceFrames(elapsed, now, &frame_changed);
  StepPhysics(elapsed);

  if (chrome_dirty_) {
    PlacePages();
    PlaceMenu();
    chrome_dirty_ = false;
  }

  // Soft bob / ZZZ need live Present when the cel index is still — but not at
  // full tick rate. Cel changes, hearts, and dirty flags still Present ASAP.
  const bool live_fx = PoseNeedsLivePaint(character_, pose_);
  bool need_present =
      present_dirty_ || frame_changed || hearts_active || had_hearts;
  if (!need_present && live_fx) {
    // Standing poses: ~1–2 Hz soft bob. Active walk/play: ~15 Hz.
    const DWORD soft_gap =
        (pose_ == L"idle" || pose_ == L"sleep" || pose_ == L"perch" ||
         pose_ == L"wait")
            ? 1000u
            : 66u;
    if (last_soft_present_ms_ == 0 ||
        now - last_soft_present_ms_ >= soft_gap) {
      // Skip full Present when bob/frame/facing unchanged (still memset+ULW
      // was ~2Hz idle CPU with many apps open).
      const PetMotion motion =
          ComputePetMotion(character_, pose_, anim_tick_);
      const int bob_y = static_cast<int>(std::lround(motion.bob));
      if (present_content_valid_ && present_frame_i_ == frame_i_ &&
          present_facing_ == facing_ && present_bob_y_ == bob_y &&
          present_anim_tick_ == static_cast<int>(anim_tick_)) {
        last_soft_present_ms_ = now;
      } else {
        need_present = true;
      }
    }
  }
  if (need_present) {
    Present();
    if (live_fx) {
      last_soft_present_ms_ = now;
    }
  }
  present_dirty_ = false;
}

LRESULT CALLBACK PetWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam) {
  PetWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<PetWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<PetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT PetWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_SYSCOMMAND && FilterDesktopChromeSysCommand(wparam)) {
    return 0;
  }
  if (msg == WM_WINDOWPOSCHANGING) {
    FilterDesktopChromeWindowPosChanging(reinterpret_cast<WINDOWPOS*>(lparam));
  }
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_SETCURSOR:
      if (dragging_ && LOWORD(lparam) == HTCLIENT) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      Present();
      return 0;
    }
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ClientToScreen(hwnd_, &pt);
      press_origin_ = pt;
      press_captured_ = true;
      dragging_ = false;
      drag_sample_n_ = 0;
      drag_sample_i_ = 0;
      // Offset from authoritative sprite origin (not region bbox GetWindowRect).
      drag_offset_.x = pt.x - static_cast<int>(pos_x_);
      drag_offset_.y = pt.y - static_cast<int>(pos_y_);
      PushDragSample(pt);
      SetCapture(hwnd_);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (press_captured_ && (wparam & MK_LBUTTON)) {
        POINT pt{};
        GetCursorPos(&pt);
        if (!dragging_) {
          // P5: 6px drag threshold — below this is still a click.
          const int dx = pt.x - press_origin_.x;
          const int dy = pt.y - press_origin_.y;
          if (dx * dx + dy * dy < kDragThresholdPx * kDragThresholdPx) {
            return 0;
          }
          dragging_ = true;
          pose_before_drag_ = pose_;
          if (HasPose(L"hold")) {
            SetPose(L"hold");
          }
          last_interact_ms_ = GetTickCount();
          chrome_dirty_ = true;
          if (!drag_size_cursor_) {
            drag_size_cursor_ = LoadCursor(nullptr, IDC_SIZEALL);
          }
          SetCursor(drag_size_cursor_ ? drag_size_cursor_
                                      : LoadCursor(nullptr, IDC_HAND));
          if (pages_) {
            PlacePages();  // hide bubbles while dragging (Python)
          }
          BeginDragFloat();
        }
        if (!drag_size_cursor_) {
          drag_size_cursor_ = LoadCursor(nullptr, IDC_SIZEALL);
        }
        if (drag_size_cursor_) {
          SetCursor(drag_size_cursor_);
        }
        const int new_x = pt.x - drag_offset_.x;
        const int new_y = pt.y - drag_offset_.y;
        if (new_x != static_cast<int>(pos_x_) ||
            new_y != static_cast<int>(pos_y_)) {
          // P1-14: face the drag direction as the pet is pulled left/right.
          bool facing_changed = false;
          if (new_x != static_cast<int>(pos_x_)) {
            const int f = new_x > static_cast<int>(pos_x_) ? 1 : -1;
            if (f != facing_) {
              facing_ = f;
              facing_changed = true;
            }
          }
          pos_x_ = static_cast<float>(new_x);
          pos_y_ = static_cast<float>(new_y);
          PushDragSample(pt);
          // Facing flip needs a full Paint; otherwise move the last DIB only.
          if (facing_changed || !present_content_valid_) {
            Present();
          } else {
            PresentMoveOnly();
          }
        }
      }
      return 0;
    case WM_LBUTTONUP: {
      POINT cur{};
      GetCursorPos(&cur);
      if (press_captured_ && !dragging_) {
        // P3: click = 摸摸 (happy + hearts), not a drag.
        press_captured_ = false;
        ReleaseCapture();
        drag_sample_n_ = 0;
        drag_sample_i_ = 0;
        EnterHappy(GetTickCount());
        return 0;
      }
      if (dragging_) {
        EndDrag(cur);
        return 0;
      }
      press_captured_ = false;
      ReleaseCapture();
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (dragging_) {
        POINT cur{};
        GetCursorPos(&cur);
        EndDrag(cur);
      } else {
        press_captured_ = false;
      }
      return 0;
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ClientToScreen(hwnd_, &pt);
      ShowContextMenu(pt);
      return 0;
    }
    case WM_DPICHANGED: {
      // P6: recalc size on monitor DPI change (Per-Monitor V2).
      const int dpi = HIWORD(wparam);
      current_dpi_ = dpi > 0 ? dpi : SystemDpi();
      const RECT* prc = reinterpret_cast<const RECT*>(lparam);
      if (prc) {
        SetOverlayScreenPos(hwnd_, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top, SWP_NOZORDER);
      }
      RebaseFloor();
      ApplyScaledWindowSize();
      present_dirty_ = true;
      chrome_dirty_ = true;
      return 0;
    }
    case WM_DESTROY:
      if (drop_target_) {
        RevokePathDropTarget(hwnd_);
        drop_target_->Release();
        drop_target_ = nullptr;
      }
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void PetWindow::Paint(HDC hdc) {
  // Sprite geometry only — never GetClientRect (region/SoftSeat can shrink it
  // and leave an empty ULW upload while the pet "stands").
  const int cw = (std::max)(1, ScaledW());
  const int ch = (std::max)(1, ScaledH());

  const BYTE* pargb = nullptr;
  int bw = sprite_w_;
  int bh = sprite_h_;
  if (!frames_pargb_.empty()) {
    const int idx =
        (std::min)(frame_i_, static_cast<int>(frames_pargb_.size()) - 1);
    const auto& cel = frames_pargb_[static_cast<size_t>(idx)];
    if (static_cast<int>(cel.size()) >= bw * bh * 4) {
      pargb = cel.data();
    }
  } else if (!sprite_pargb_.empty() &&
             static_cast<int>(sprite_pargb_.size()) >= bw * bh * 4) {
    pargb = sprite_pargb_.data();
  }

  // Soft bob: integer Y offset only. No scale/rotate/FX overlays — those left
  // a black halo on UpdateLayeredWindow (straight alpha vs premultiplied).
  const PetMotion motion = ComputePetMotion(character_, pose_, anim_tick_);
  const int bob_y = static_cast<int>(std::lround(motion.bob));
  present_bob_y_ = bob_y;

  EnsureGdiplus();
  if (pargb && bw > 0 && bh > 0) {
    // Draw from load-time PARGB bytes — GetHBITMAP loses alpha (header note),
    // which made AlphaBlend/ULW upload a fully transparent pet while page
    // bubbles (GDI+ chips) kept showing.
    if (facing_ < 0) {
      if (!frames_pargb_.empty()) {
        const int idx =
            (std::min)(frame_i_, static_cast<int>(frames_pargb_.size()) - 1);
        EnsureFlippedPargbCel(static_cast<size_t>(idx));
        if (idx >= 0 &&
            static_cast<size_t>(idx) < frames_pargb_flip_.size() &&
            !frames_pargb_flip_[static_cast<size_t>(idx)].empty()) {
          pargb = frames_pargb_flip_[static_cast<size_t>(idx)].data();
        }
      } else if (!sprite_pargb_.empty()) {
        if (sprite_pargb_flip_.empty() &&
            static_cast<int>(sprite_pargb_.size()) >= bw * bh * 4) {
          sprite_pargb_flip_.resize(sprite_pargb_.size());
          for (int y = 0; y < bh; ++y) {
            const BYTE* row =
                sprite_pargb_.data() + static_cast<size_t>(y) * bw * 4;
            BYTE* out =
                sprite_pargb_flip_.data() + static_cast<size_t>(y) * bw * 4;
            for (int x = 0; x < bw; ++x) {
              const BYTE* p = row + (bw - 1 - x) * 4;
              BYTE* q = out + x * 4;
              q[0] = p[0];
              q[1] = p[1];
              q[2] = p[2];
              q[3] = p[3];
            }
          }
        }
        if (!sprite_pargb_flip_.empty()) {
          pargb = sprite_pargb_flip_.data();
        }
      }
    }
    Gdiplus::Bitmap held(bw, bh, bw * 4, PixelFormat32bppPARGB,
                         const_cast<BYTE*>(pargb));
    Gdiplus::Graphics g(hdc);
    const bool pixel = IsPixelArtCharacter(character_);
    g.SetInterpolationMode(pixel ? Gdiplus::InterpolationModeNearestNeighbor
                                 : Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(pixel ? Gdiplus::PixelOffsetModeNone
                               : Gdiplus::PixelOffsetModeHalf);
    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    g.DrawImage(&held, 0, bob_y, cw, ch);
  } else {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush body(Gdiplus::Color(230, 120, 200, 140));
    Gdiplus::Pen outline(Gdiplus::Color(255, 40, 80, 50), 2.0f);
    g.FillEllipse(&body, 8, 10, cw - 16, ch - 16);
    g.DrawEllipse(&outline, 8, 10, cw - 16, ch - 16);
    Gdiplus::SolidBrush eye(Gdiplus::Color(255, 20, 20, 20));
    g.FillEllipse(&eye, 24, 28, 10, 10);
    g.FillEllipse(&eye, 40, 28, 10, 10);
  }

  // Sleep Zzz (esp. 剑仙 fixed-tree sleep — no cel swap, Zzz carries life).
  if (pose_ == L"sleep" || pose_ == L"lie") {
    Gdiplus::Graphics gz(hdc);
    PaintZzz(gz, cw, ch, anim_tick_, character_);
  }

  // Hearts: stamp into the layered DIB via GDI+; Present() premultiplies.
  if (!hearts_.empty()) {
    const DWORD now = GetTickCount();
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    for (const auto& h : hearts_) {
      const float age = static_cast<float>(now - h.born) / 1000.0f;
      const int alpha =
          (std::max)(0, static_cast<int>(220 * (1.0f - age / 1.1f)));
      if (alpha < 16) {
        continue;
      }
      const int px = static_cast<int>(h.fx * cw);
      const int py = static_cast<int>(h.fy * ch - age * 36);
      Gdiplus::SolidBrush br(
          Gdiplus::Color(static_cast<BYTE>(alpha), 255, 90, 120));
      g.FillEllipse(&br, px - 3, py - 3, 6, 6);
    }
  }
}

void PetWindow::ApplyHitRegion(const std::vector<BYTE>& alpha, int bw, int bh,
                              int cw, int ch) {
  if (!hwnd_ || alpha.empty() || bw <= 0 || bh <= 0 || cw <= 0 || ch <= 0) {
    return;
  }
  // 拖拽中保持全窗口可拖（hit-region 已在 BeginDragFloat 清空）。此时若
  // SetWindowRgn，会紧跟 UpdateLayeredWindow 之后，在 Win11 上使分层位图空白
  // （拖拽中宠物消失的根源）。
  if (dragging_) {
    return;
  }
  if (alpha.size() < static_cast<size_t>(bw) * static_cast<size_t>(bh)) {
    return;
  }
  // Cache hit: region already installed — do NOT re-SetWindowRgn. Re-applying
  // after UpdateLayeredWindow blanks the layered bitmap on Win11 (pet vanishes
  // right after drag drop / SoftSeat).
  if (mask_rgn_ && mask_w_ == cw && mask_h_ == ch && mask_facing_ == facing_ &&
      mask_index_ == frame_i_ && mask_gen_ == mask_gen_counter_) {
    return;
  }

  constexpr BYTE kHitAlpha = 16;
  HRGN rgn = CreateRectRgn(0, 0, 0, 0);
  for (int y = 0; y < ch; ++y) {
    const int sy = (std::min)(bh - 1, y * bh / ch);
    const BYTE* row = alpha.data() + static_cast<size_t>(sy) * bw;
    auto alpha_at = [&](int px) -> BYTE {
      int sx = (std::min)(bw - 1, px * bw / cw);
      if (facing_ < 0) {
        sx = bw - 1 - sx;
      }
      return row[sx];
    };
    int x = 0;
    while (x < cw) {
      while (x < cw && alpha_at(x) < kHitAlpha) {
        ++x;
      }
      if (x >= cw) {
        break;
      }
      const int x0 = x;
      while (x < cw && alpha_at(x) >= kHitAlpha) {
        ++x;
      }
      HRGN line = CreateRectRgn(x0, y, x, y + 1);
      CombineRgn(rgn, rgn, line, RGN_OR);
      DeleteObject(line);
    }
  }

  RECT box{};
  if (GetRgnBox(rgn, &box) == NULLREGION || (box.right - box.left) < 2 ||
      (box.bottom - box.top) < 2) {
    DeleteObject(rgn);
    rgn = CreateRectRgn(0, 0, 1, 1);
  }
  if (mask_rgn_) {
    DeleteObject(mask_rgn_);
  }
  mask_rgn_ = rgn;
  mask_w_ = cw;
  mask_h_ = ch;
  mask_facing_ = facing_;
  mask_index_ = frame_i_;
  mask_gen_ = mask_gen_counter_;

  HRGN copy = CreateRectRgn(0, 0, 0, 0);
  CombineRgn(copy, mask_rgn_, mask_rgn_, RGN_COPY);
  SetWindowRgn(hwnd_, copy, FALSE);
}

void PetWindow::PresentMoveOnly() {
  if (!hwnd_ || !present_content_valid_ || !present_dc_ || !present_bits_) {
    Present();
    return;
  }
  const int w = (std::max)(1, ScaledW());
  const int h = (std::max)(1, ScaledH());
  if (w != present_buf_w_ || h != present_buf_h_ ||
      frame_i_ != present_frame_i_ || facing_ != present_facing_) {
    Present();
    return;
  }
  {
    const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if ((ex & WS_EX_LAYERED) == 0) {
      SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    }
  }
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return;
  }
  POINT origin{static_cast<int>(pos_x_), static_cast<int>(pos_y_)};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  if (!UpdateOverlayLayered(hwnd_, screen, &origin, &size, present_dc_, &src, 0,
                            &blend, ULW_ALPHA)) {
    const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    if (!UpdateOverlayLayered(hwnd_, screen, &origin, &size, present_dc_, &src,
                              0, &blend, ULW_ALPHA)) {
      present_content_valid_ = false;
      LogLine(L"[pet] UpdateLayeredWindow(move) failed err=%lu size=%dx%d",
              GetLastError(), w, h);
    }
  }
  ReleaseDC(nullptr, screen);
}

void PetWindow::Present() {
  if (!hwnd_) {
    return;
  }
  // ULW-only path: never SetWindowPos here. SetWindowPos clears the layered
  // bitmap (blank pet) and was also thrashing the desktop band → jank while
  // standing. Position/size are carried by UpdateLayeredWindow pptDst+SIZE.
  // No SetWindowRgn either — ULW_ALPHA already hit-tests by alpha.
  const int w = (std::max)(1, ScaledW());
  const int h = (std::max)(1, ScaledH());
  const int ox = static_cast<int>(pos_x_);
  const int oy = static_cast<int>(pos_y_);

  if (mask_rgn_) {
    SetWindowRgn(hwnd_, nullptr, FALSE);
    DeleteObject(mask_rgn_);
    mask_rgn_ = nullptr;
  }

  // Win11: SoftSeat/Raise can leave WS_EX_LAYERED with a dead ULW surface.
  // Toggle layered (while keeping mapped) so the next blit sticks — same
  // repair as PublicHost::ForceRemapPresent, without a hide flash.
  {
    const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if ((ex & WS_EX_LAYERED) == 0) {
      SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    }
  }

  if (!EnsurePresentBuffer(w, h)) {
    present_content_valid_ = false;
    return;
  }
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return;
  }
  HDC mem = present_dc_;
  void* bits = present_bits_;
  std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

  // Idle soft bob: blit cached PARGB cel with Y offset — skip GDI+ HDC round
  // trip + full-buffer premultiply (was the main idle CPU with pets visible).
  bool soft_pargb = false;
  if (hearts_.empty() && pose_ != L"sleep" && pose_ != L"lie") {
    const BYTE* pargb = nullptr;
    int bw = 0;
    int bh = 0;
    bw = sprite_w_;
    bh = sprite_h_;
    if (!frames_pargb_.empty()) {
      const int idx =
          (std::min)(frame_i_, static_cast<int>(frames_pargb_.size()) - 1);
      if (idx >= 0 &&
          static_cast<size_t>(idx) < frames_pargb_.size() &&
          static_cast<int>(frames_pargb_[static_cast<size_t>(idx)].size()) >=
              bw * bh * 4) {
        pargb = frames_pargb_[static_cast<size_t>(idx)].data();
        if (facing_ < 0) {
          EnsureFlippedPargbCel(static_cast<size_t>(idx));
          if (static_cast<size_t>(idx) < frames_pargb_flip_.size() &&
              !frames_pargb_flip_[static_cast<size_t>(idx)].empty()) {
            pargb = frames_pargb_flip_[static_cast<size_t>(idx)].data();
          }
        }
      }
    } else if (!sprite_pargb_.empty() &&
               static_cast<int>(sprite_pargb_.size()) >= bw * bh * 4) {
      pargb = sprite_pargb_.data();
      if (facing_ < 0) {
        if (sprite_pargb_flip_.empty()) {
          sprite_pargb_flip_.resize(sprite_pargb_.size());
          for (int y = 0; y < bh; ++y) {
            const BYTE* row =
                sprite_pargb_.data() + static_cast<size_t>(y) * bw * 4;
            BYTE* out =
                sprite_pargb_flip_.data() + static_cast<size_t>(y) * bw * 4;
            for (int x = 0; x < bw; ++x) {
              const BYTE* p = row + (bw - 1 - x) * 4;
              BYTE* q = out + x * 4;
              q[0] = p[0];
              q[1] = p[1];
              q[2] = p[2];
              q[3] = p[3];
            }
          }
        }
        if (!sprite_pargb_flip_.empty()) {
          pargb = sprite_pargb_flip_.data();
        }
      }
    }
    // 1:1 pixel pets only — scaled sprites still need GDI+ DrawImage.
    if (pargb && bw > 0 && bh > 0 && bw == w && bh == h) {
      const PetMotion motion =
          ComputePetMotion(character_, pose_, anim_tick_);
      const int bob_y = static_cast<int>(std::lround(motion.bob));
      present_bob_y_ = bob_y;
      auto* dst = static_cast<BYTE*>(bits);
      const int stride = w * 4;
      for (int y = 0; y < bh; ++y) {
        const int dy = y + bob_y;
        if (dy < 0 || dy >= h) {
          continue;
        }
        std::memcpy(dst + static_cast<size_t>(dy) * stride,
                    pargb + static_cast<size_t>(y) * stride,
                    static_cast<size_t>(stride));
      }
      soft_pargb = true;
    }
  }
  if (!soft_pargb) {
    Paint(mem);
    // GDI+ → HDC writes straight alpha; ULW_ALPHA needs premultiplied.
    auto* px = static_cast<BYTE*>(bits);
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < n; ++i) {
      BYTE* p = px + i * 4;
      const unsigned a = p[3];
      if (a == 0) {
        p[0] = p[1] = p[2] = 0;
      } else if (a < 255) {
        if (p[0] > a || p[1] > a || p[2] > a) {
          p[0] = static_cast<BYTE>((p[0] * a) / 255);
          p[1] = static_cast<BYTE>((p[1] * a) / 255);
          p[2] = static_cast<BYTE>((p[2] * a) / 255);
        }
      }
    }
  }
  POINT origin{ox, oy};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  bool ok = UpdateOverlayLayered(hwnd_, screen, &origin, &size, mem, &src, 0,
                                 &blend, ULW_ALPHA);
  if (!ok) {
    // Dead layered surface: reset WS_EX_LAYERED once and retry (PublicHost).
    const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    ok = UpdateOverlayLayered(hwnd_, screen, &origin, &size, mem, &src, 0,
                              &blend, ULW_ALPHA);
    if (!ok) {
      LogLine(L"[pet] UpdateLayeredWindow failed err=%lu size=%dx%d",
              GetLastError(), w, h);
    }
  }
  present_content_valid_ = ok;
  if (ok) {
    present_frame_i_ = frame_i_;
    present_facing_ = facing_;
    present_anim_tick_ = anim_tick_;
  }

  ReleaseDC(nullptr, screen);
}
