#include "fence_window.h"

#include "fence_keys.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "fence_style.h"
#include "icon_actions.h"
#include "log.h"
#include "namespace_icons.h"
#include "pin_store.h"
#include "shell_clipboard.h"
#include "i18n.h"
#include "shell_context_menu.h"
#include "toast.h"
#include "whitelist.h"

#include <ShlObj.h>
#include <Shellapi.h>
#include <commctrl.h>
#include <imm.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "imm32.lib")

namespace {
// Defined with the other EDIT subclasses below; the filter box is created
// long before that point in the file.
LRESULT CALLBACK FilterEditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                LPARAM lparam);
constexpr wchar_t kClassName[] = L"DeskTidyNativeFence";
constexpr int kHeaderH0 = 32;
constexpr int kCollapsedH0 = 44;
constexpr int kPad0 = 10;
// Wider cells + 2-line captions (Explorer / Fences / Python FenceIconItem).
constexpr int kCellW0 = 96;
constexpr int kCellH0 = 102;
constexpr int kIconSize0 = 40;  // match Python FenceIconItem.DEFAULT_ICON_SIZE
constexpr int kListRowH0 = 28;
constexpr int kListIcon0 = 20;
constexpr int kDragThreshold = 6;
constexpr int kEdge = 6;
// DeskTidy extras prepended to the hosted shell menu (ids 1..99).
constexpr int kExtFenceRefresh = 1;
constexpr int kExtFenceHide = 2;
constexpr int kExtFenceDissolve = 3;
constexpr int kExtFenceLock = 4;
// Freezes icon set/order (Fences keeps this separate from position lock).
constexpr int kExtFenceLockContent = 16;
constexpr int kExtFenceRename = 5;
constexpr int kExtFenceCollapse = 6;
// Formal blank-menu extras (were fallback-only) — ids stay in 1..99.
constexpr int kExtFencePortalSet = 7;
constexpr int kExtFencePortalClear = 8;
constexpr int kExtFencePortalUp = 9;
constexpr int kExtFenceSortName = 10;
constexpr int kExtFenceAutoHide = 15;
constexpr int kExtFenceFadeIdle = 18;
constexpr UINT kTimerAutoHide = 77;
// Idle fade animation tick + how dim a fence goes when the pointer is away.
constexpr UINT kTimerIdleFade = 79;
constexpr BYTE kIdleFadeAlpha = 150;
constexpr int kIdleFadeStep = 18;
constexpr UINT kIdleFadeIntervalMs = 30;
// Drag-to-edge auto-scroll tick (Explorer keeps scrolling while the cursor
// rests against the edge; mouse-move alone would stop as soon as it settles).
constexpr UINT kTimerDragScroll = 78;
constexpr int kExtFenceSortDate = 11;
constexpr int kExtFenceSortSize = 12;
constexpr int kExtFenceSortType = 13;
constexpr int kExtFenceSortManual = 14;
constexpr int kExtFenceSortCreated = 17;
// Per-fence grid pitch presets (percent of the desktop's own cell metrics).
constexpr int kExtFenceSpacingCompact = 91;
constexpr int kExtFenceSpacingNormal = 92;
constexpr int kExtFenceSpacingWide = 93;
constexpr int kExtFenceSpacingXl = 94;
constexpr int kExtFenceShowTitle = 95;
constexpr int kExtFenceViewGrid = 20;
constexpr int kExtFenceViewList = 21;
// Quick filter box (Fences-style type-to-narrow).
constexpr int kExtFenceFilter = 22;
constexpr int kExtFenceOpaFollow = 30;
constexpr int kExtFenceOpa40 = 31;
constexpr int kExtFenceOpa55 = 32;
constexpr int kExtFenceOpa70 = 33;
constexpr int kExtFenceOpa85 = 34;
constexpr int kExtFenceOpaOpaque = 35;
constexpr int kExtFencePageBase = 40;   // 40..51 → page 0..11
constexpr int kExtFenceStyleBase = 60;  // 60..67 → style presets
constexpr int kExtUnpin = 1;
constexpr int kExtWhitelist = 2;
constexpr int kExtMovePage = 3;
constexpr int kExtOpenNotes = 4;
bool g_class_registered = false;

std::wstring DesktopDirectory() {
  PWSTR p = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p) {
    std::wstring dir(p);
    CoTaskMemFree(p);
    return dir;
  }
  return L"";
}

int FenceDpi(HWND hwnd) {
  using Fn = UINT(WINAPI*)(HWND);
  static Fn get_dpi = reinterpret_cast<Fn>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (get_dpi && hwnd) {
    const UINT dpi = get_dpi(hwnd);
    if (dpi > 0) {
      return static_cast<int>(dpi);
    }
  }
  HDC dc = GetDC(nullptr);
  const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(nullptr, dc);
  return dpi > 0 ? dpi : 96;
}

std::wstring Widen(const std::string& s) {
  std::wstring out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  }
  return out;
}

// Explorer / Fences style: translucent accent plate. Must use GDI+ (not GDI
// RoundRect) so per-pixel alpha stays valid for UpdateLayeredWindow.
void PaintSelectionChrome(Gdiplus::Graphics& g, int left, int top, int right,
                          int bottom, COLORREF accent, int radius,
                          BYTE fill_a = 110, BYTE edge_a = 220) {
  const int w = right - left;
  const int h = bottom - top;
  if (w < 4 || h < 4) {
    return;
  }
  Gdiplus::GraphicsPath path;
  const int rr = (std::min)(radius * 2, (std::min)(w, h));
  if (rr > 2) {
    path.AddArc(left, top, rr, rr, 180.f, 90.f);
    path.AddArc(right - rr, top, rr, rr, 270.f, 90.f);
    path.AddArc(right - rr, bottom - rr, rr, rr, 0.f, 90.f);
    path.AddArc(left, bottom - rr, rr, rr, 90.f, 90.f);
    path.CloseFigure();
  } else {
    path.AddRectangle(Gdiplus::Rect(left, top, w, h));
  }
  Gdiplus::SolidBrush fill(Gdiplus::Color(
      fill_a, GetRValue(accent), GetGValue(accent), GetBValue(accent)));
  g.FillPath(&fill, &path);
  if (edge_a > 0) {
    Gdiplus::Pen edge(
        Gdiplus::Color(edge_a, GetRValue(accent), GetGValue(accent),
                       GetBValue(accent)),
        1.5f);
    edge.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawPath(&edge, &path);
  }
}

int S(HWND hwnd, int px) { return MulDiv(px, FenceDpi(hwnd), 96); }

// opacity: 255 = full; ~107 (~0.42) matches Python cut ghost.
// Builds an owned premultiplied 32bpp bitmap for `icon` drawn at `dest`
// (source size = the icon's native color width clamped to [dest, 256]).
// Returns nullptr on failure; the caller owns the returned Gdiplus::Bitmap.
Gdiplus::Bitmap* BuildPremultipliedIcon(HICON icon, int dest, BYTE opacity) {
  if (!icon || dest <= 0) {
    return nullptr;
  }
  ICONINFO ii{};
  int src = dest;
  if (GetIconInfo(icon, &ii)) {
    BITMAP bm{};
    if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm) == sizeof(bm) &&
        bm.bmWidth > 0) {
      src = bm.bmWidth;
    }
    if (ii.hbmColor) {
      DeleteObject(ii.hbmColor);
    }
    if (ii.hbmMask) {
      DeleteObject(ii.hbmMask);
    }
  }
  // Draw at the icon's native size (after jumbo trim), never below dest.
  // Capping to dest*2 forced DrawIconEx's StretchBlt downscale — soft Chrome /
  // Cursor. Cache only dest×dest; the native DIB is temporary.
  src = (std::max)(dest, (std::min)(src, 256));

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = src;
  bmi.bmiHeader.biHeight = -src;
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
    return nullptr;
  }
  HGDIOBJ old = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(src) * static_cast<size_t>(src) * 4);
  DrawIconEx(mem, 0, 0, icon, src, src, 0, nullptr, DI_NORMAL);
  auto* px = static_cast<BYTE*>(bits);
  const int n = src * src;
  // DrawIconEx writes PARGB. Downscale: un-premultiply so bicubic sees straight
  // color. 1:1: keep PARGB as-is (re-multiplying would darken).
  for (int i = 0; i < n; ++i) {
    BYTE* p = px + i * 4;
    BYTE a = p[3];
    if (a == 0 && (p[0] | p[1] | p[2])) {
      a = 255;  // legacy 24bpp source with bogus 0 alpha
    }
    if (src != dest) {
      if (a > 0 && a < 255) {
        p[0] = static_cast<BYTE>((std::min)(255, (p[0] * 255) / a));
        p[1] = static_cast<BYTE>((std::min)(255, (p[1] * 255) / a));
        p[2] = static_cast<BYTE>((std::min)(255, (p[2] * 255) / a));
      }
      if (opacity < 255) {
        a = static_cast<BYTE>((a * opacity) / 255);
      }
      p[3] = a;
    } else if (opacity < 255) {
      p[0] = static_cast<BYTE>((p[0] * opacity) / 255);
      p[1] = static_cast<BYTE>((p[1] * opacity) / 255);
      p[2] = static_cast<BYTE>((p[2] * opacity) / 255);
      p[3] = static_cast<BYTE>((a * opacity) / 255);
    }
  }
  Gdiplus::Bitmap* out = new Gdiplus::Bitmap(dest, dest, PixelFormat32bppPARGB);
  {
    Gdiplus::Graphics g2(out);
    g2.Clear(Gdiplus::Color(0, 0, 0, 0));
    if (src == dest) {
      Gdiplus::Bitmap ref(src, src, src * 4, PixelFormat32bppPARGB,
                          static_cast<BYTE*>(bits));
      g2.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
      g2.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
      g2.DrawImage(&ref, 0, 0, dest, dest);
    } else {
      Gdiplus::Bitmap ref(src, src, src * 4, PixelFormat32bppARGB,
                          static_cast<BYTE*>(bits));
      g2.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
      g2.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
      g2.DrawImage(&ref, 0, 0, dest, dest);
    }
  }
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return out;
}

std::wstring LowerPath(std::wstring s) {
  for (auto& c : s) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  return s;
}

// Cut-path set cached by clipboard sequence number (avoids the per-cell
// O(cut) linear _wcsicmp walk inside ClipboardPathIsCut during Paint).
DWORD g_cut_seq = 0xFFFFFFFF;
std::unordered_set<std::wstring> g_cut_set;

const std::unordered_set<std::wstring>& CutPathSet() {
  const DWORD seq = GetClipboardSequenceNumber();
  if (seq != g_cut_seq) {
    g_cut_seq = seq;
    g_cut_set.clear();
    std::vector<std::wstring> files;
    bool cut = false;
    if (ClipboardGetFilesWithEffect(&files, &cut) && cut) {
      for (auto& p : files) {
        g_cut_set.insert(LowerPath(std::move(p)));
      }
    }
  }
  return g_cut_set;
}
}  // namespace

void FenceWindow::ClearIconCache() {
  for (auto& kv : icon_cache_) {
    delete kv.second;
  }
  icon_cache_.clear();
}

void FenceWindow::DrawIconCached(Gdiplus::Graphics& g, HICON icon, int x, int y,
                                 int dest, BYTE opacity) {
  if (!icon || dest <= 0) {
    return;
  }
  const IconCacheKey key{icon, dest, opacity};
  auto it = icon_cache_.find(key);
  Gdiplus::Bitmap* bmp = nullptr;
  if (it == icon_cache_.end()) {
    bmp = BuildPremultipliedIcon(icon, dest, opacity);
    if (!bmp) {
      return;
    }
    icon_cache_.emplace(key, bmp);
  } else {
    bmp = it->second;
  }
  // 1:1 nearest; downscale (≤2× cache) with bilinear. Global HighQualityBicubic
  // on PARGB uses negative weights and dark-fringe-blurs icons.
  const Gdiplus::InterpolationMode prev = g.GetInterpolationMode();
  const Gdiplus::PixelOffsetMode prev_px = g.GetPixelOffsetMode();
  if (static_cast<int>(bmp->GetWidth()) == dest &&
      static_cast<int>(bmp->GetHeight()) == dest) {
    g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
  } else {
    g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
  }
  g.DrawImage(bmp, x, y, dest, dest);
  g.SetInterpolationMode(prev);
  g.SetPixelOffsetMode(prev_px);
}

void FenceWindow::EnsureFonts(int title_px, int caption_px, int footer_px) {
  if (!caption_family_) {
    caption_family_ = new Gdiplus::FontFamily(L"Microsoft YaHei UI");
    if (!caption_family_->IsAvailable()) {
      delete caption_family_;
      // GenericSansSerif() returns a process-lifetime static; borrow it.
      caption_family_ = const_cast<Gdiplus::FontFamily*>(
          Gdiplus::FontFamily::GenericSansSerif());
      caption_family_borrowed_ = true;
    }
  }
  if (!title_font_ || cached_title_px_ != title_px) {
    delete title_font_;
    title_font_ = new Gdiplus::Font(
        caption_family_, static_cast<Gdiplus::REAL>(title_px),
        Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    cached_title_px_ = title_px;
  }
  if (!caption_font_ || cached_caption_px_ != caption_px) {
    delete caption_font_;
    caption_font_ = new Gdiplus::Font(
        caption_family_, static_cast<Gdiplus::REAL>(caption_px),
        Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    cached_caption_px_ = caption_px;
  }
  if (!footer_font_ || cached_footer_px_ != footer_px) {
    delete footer_font_;
    footer_font_ = new Gdiplus::Font(
        caption_family_, static_cast<Gdiplus::REAL>(footer_px),
        Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    cached_footer_px_ = footer_px;
  }
}

void FenceWindow::ReleaseFonts() {
  delete caption_font_;
  delete title_font_;
  delete footer_font_;
  if (caption_family_ && !caption_family_borrowed_) {
    delete caption_family_;
  }
  caption_font_ = nullptr;
  title_font_ = nullptr;
  footer_font_ = nullptr;
  caption_family_ = nullptr;
  caption_family_borrowed_ = false;
  cached_title_px_ = 0;
  cached_caption_px_ = 0;
  cached_footer_px_ = 0;
}

void FenceWindow::ApplyStyle() {
  const auto* preset = FenceStyleById(style_id_);
  style_id_ = preset->id;
  if (!has_custom_colors_) {
    bg_ = preset->background;
    accent_ = preset->accent;
  }
  title_color_ = preset->title;
  caption_color_ = preset->title;
  panel_alpha_ = preset->alpha;
  if (opacity_override_ >= 0) {
    panel_alpha_ =
        static_cast<BYTE>((std::min)(255, (std::max)(40, opacity_override_)));
  }
  radius_ = radius_override_ >= 0 ? radius_override_ : preset->radius;
  light_panel_ = preset->light;
  if (hwnd_) {
    // Per-pixel alpha via UpdateLayeredWindow only — do not call
    // SetLayeredWindowAttributes (it disables subsequent ULW updates).
    // Soft-park: never ULW while hidden (Win11 blank-after-Show).
    if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
      PresentLayered();
    } else {
      layer_dirty_ = true;
    }
  }
}

void FenceWindow::SetPage(int page) {
  if (page < 0) {
    page = 0;
  }
  page_ = page;
  // Settings / page-bar 「移动到页」: replace membership (true move).
  // Multi-page add/remove stays on TogglePageMembership (fence menu).
  pages_ = {page_};
}

void FenceWindow::SetPages(std::vector<int> pages) {
  pages_.clear();
  std::vector<char> seen(64, 0);
  for (int p : pages) {
    if (p < 0 || p >= 64 || seen[static_cast<size_t>(p)]) {
      continue;
    }
    seen[static_cast<size_t>(p)] = 1;
    pages_.push_back(p);
  }
  if (pages_.empty()) {
    pages_.push_back(page_);
  }
  page_ = pages_[0];
}

bool FenceWindow::OnPage(int page) const {
  if (pages_.empty()) {
    return page == page_;
  }
  for (int p : pages_) {
    if (p == page) {
      return true;
    }
  }
  return false;
}

void FenceWindow::TogglePageMembership(int page) {
  if (page < 0) {
    return;
  }
  if (OnPage(page)) {
    if (pages_.size() <= 1) {
      return;  // keep at least one page
    }
    pages_.erase(std::remove(pages_.begin(), pages_.end(), page),
                 pages_.end());
    if (!OnPage(page_)) {
      page_ = pages_.empty() ? 0 : pages_[0];
    }
  } else {
    pages_.push_back(page);
  }
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::RemapPagesAfterDelete(int deleted) {
  if (deleted < 0) {
    return;
  }
  std::vector<int> src = pages_.empty() ? std::vector<int>{page_} : pages_;
  std::vector<int> next;
  next.reserve(src.size());
  std::vector<char> seen(64, 0);
  for (int p : src) {
    if (p == deleted) {
      continue;
    }
    if (p > deleted) {
      --p;
    }
    if (p < 0 || p >= 64 || seen[static_cast<size_t>(p)]) {
      continue;
    }
    seen[static_cast<size_t>(p)] = 1;
    next.push_back(p);
  }
  if (next.empty()) {
    next.push_back(0);
  }
  pages_ = std::move(next);
  if (page_ == deleted || !OnPage(page_)) {
    page_ = pages_[0];
  } else if (page_ > deleted) {
    --page_;
  }
  std::map<int, FencePageGeometry> geos;
  for (const auto& kv : page_geometries_) {
    int p = kv.first;
    if (p == deleted) {
      continue;
    }
    if (p > deleted) {
      --p;
    }
    geos[p] = kv.second;
  }
  page_geometries_ = std::move(geos);
}

bool FenceWindow::ResolveLayerSize(int* out_w, int* out_h) const {
  if (!out_w || !out_h || !hwnd_ || !IsWindow(hwnd_) || IsIconic(hwnd_)) {
    return false;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  int w = static_cast<int>(wr.right - wr.left);
  int h = static_cast<int>(wr.bottom - wr.top);
  const int chrome_min = S(hwnd_, kCollapsedH0);
  const int owned_h =
      collapsed_ ? chrome_min : (std::max)(expanded_h_, kFenceExpandedFloor);
  // Explorer create/copy/attach can report a transient tiny HWND rect.
  // UpdateLayeredWindow would then permanently shrink/blank the fence
  // (Fences keep an application-owned size for the layered surface).
  if (w < 80) {
    const FencePageGeometry g = GeometryForPage(page_);
    w = (std::max)(120, g.w > 0 ? g.w : 200);
  }
  // Height is application-owned (Fences-class). Trust HWND only while the
  // user is live-dragging an edge — otherwise shell churn / a bad persist
  // (display-scale ×10) would stretch the layered frame to full screen.
  if (collapsed_) {
    h = chrome_min;
  } else if (resizing_) {
    if (h < 100) {
      h = owned_h;
    }
  } else {
    h = owned_h;
  }
  *out_w = (std::max)(1, w);
  *out_h = (std::max)(1, h);
  return true;
}

void FenceWindow::InvalidatePanelChromeCache() {
  panel_chrome_.clear();
  panel_chrome_w_ = 0;
  panel_chrome_h_ = 0;
  panel_chrome_radius_ = -1;
}

void FenceWindow::ReleaseLayerBuffer() {
  if (layer_dc_ && layer_old_) {
    SelectObject(layer_dc_, layer_old_);
    layer_old_ = nullptr;
  }
  if (layer_dib_) {
    DeleteObject(layer_dib_);
    layer_dib_ = nullptr;
  }
  if (layer_dc_) {
    DeleteDC(layer_dc_);
    layer_dc_ = nullptr;
  }
  layer_bits_ = nullptr;
  layer_buf_w_ = 0;
  layer_buf_h_ = 0;
  InvalidatePanelChromeCache();
}

bool FenceWindow::EnsureLayerBuffer(int w, int h) {
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (layer_dc_ && layer_dib_ && layer_bits_ && layer_buf_w_ == w &&
      layer_buf_h_ == h) {
    return true;
  }
  ReleaseLayerBuffer();
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return false;
  }
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
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
  layer_old_ = SelectObject(mem, dib);
  layer_dc_ = mem;
  layer_dib_ = dib;
  layer_bits_ = bits;
  layer_buf_w_ = w;
  layer_buf_h_ = h;
  return true;
}

void FenceWindow::PresentLayered() {
  if (!hwnd_) {
    return;
  }
  int w = 0;
  int h = 0;
  if (!ResolveLayerSize(&w, &h)) {
    return;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  if ((wr.right - wr.left) != w || (wr.bottom - wr.top) != h) {
    SetOverlayScreenPos(hwnd_, wr.left, wr.top, w, h,
                        SWP_NOZORDER | SWP_NOREDRAW);
  }
  // Always layout for the exact frame we blit — never paint stale cell bounds
  // after a desktop watch / organize refresh (blank body,「共 N 项」left).
  LayoutCellsTo(w, h);

  static ULONG_PTR gdi_token = 0;
  if (!gdi_token) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&gdi_token, &input, nullptr);
  }

  if (!EnsureLayerBuffer(w, h)) {
    return;
  }
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return;
  }
  HDC mem = layer_dc_;
  void* bits = layer_bits_;
  const size_t nbytes =
      static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;

  const int radius = (std::max)(0, S(hwnd_, radius_));
  const bool chrome_hit =
      panel_chrome_w_ == w && panel_chrome_h_ == h &&
      panel_chrome_bg_ == bg_ && panel_chrome_accent_ == accent_ &&
      panel_chrome_alpha_ == panel_alpha_ &&
      panel_chrome_light_ == light_panel_ &&
      panel_chrome_radius_ == radius && panel_chrome_.size() == nbytes;
  if (chrome_hit) {
    std::memcpy(bits, panel_chrome_.data(), nbytes);
  } else {
    std::memset(bits, 0, nbytes);
    // Scope the background Graphics. Leaving it alive sets a half-pixel
    // world transform on this DC, which blurs later GDI text and icons.
    Gdiplus::Graphics g(mem);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    const BYTE a = panel_alpha_;
    const int rr = radius * 2;
    const int rw = (std::max)(1, w - 1);
    const int rh = (std::max)(1, h - 1);

    // 360-style glossy vertical frosted acrylic wash
    const BYTE r_val = GetRValue(bg_);
    const BYTE g_val = GetGValue(bg_);
    const BYTE b_val = GetBValue(bg_);
    const BYTE top_a = static_cast<BYTE>((std::min)(255, static_cast<int>(a) + 10));
    const BYTE top_r = static_cast<BYTE>((std::min)(255, static_cast<int>(r_val) + (light_panel_ ? -3 : 8)));
    const BYTE top_g = static_cast<BYTE>((std::min)(255, static_cast<int>(g_val) + (light_panel_ ? -3 : 8)));
    const BYTE top_b = static_cast<BYTE>((std::min)(255, static_cast<int>(b_val) + (light_panel_ ? -3 : 12)));
    Gdiplus::LinearGradientBrush fill(
        Gdiplus::Point(0, 0), Gdiplus::Point(0, rh),
        Gdiplus::Color(top_a, top_r, top_g, top_b),
        Gdiplus::Color(a, r_val, g_val, b_val));

    Gdiplus::GraphicsPath path;
    if (rr > 2 && rr < rw && rr < rh) {
      path.AddArc(0, 0, rr, rr, 180, 90);
      path.AddArc(rw - rr, 0, rr, rr, 270, 90);
      path.AddArc(rw - rr, rh - rr, rr, rr, 0, 90);
      path.AddArc(0, rh - rr, rr, rr, 90, 90);
      path.CloseFigure();
      g.FillPath(&fill, &path);
    } else {
      g.FillRectangle(&fill, 0, 0, rw, rh);
    }
    // Soft header wash only (Fences-style: no left accent stripe).
    const int header_h = S(hwnd_, kHeaderH0);
    const bool rounded = rr > 2 && rr < rw && rr < rh;
    {
      // Glossy vertical gradient wash for frosted glass depth
      Gdiplus::LinearGradientBrush wash(
          Gdiplus::Point(0, 0), Gdiplus::Point(0, header_h),
          Gdiplus::Color(light_panel_ ? 75 : 38, 255, 255, 255),
          Gdiplus::Color(light_panel_ ? 35 : 16, 255, 255, 255));
      if (rounded) {
        g.SetClip(&path);
        g.FillRectangle(&wash, 0, 0, rw, header_h);
        g.ResetClip();
      } else {
        g.FillRectangle(&wash, 0, 0, rw, header_h);
      }
    }

    // Luminous separator bar between header and content (融入强调色微光，柔和通透)
    const BYTE ar = GetRValue(accent_);
    const BYTE ag = GetGValue(accent_);
    const BYTE ab = GetBValue(accent_);
    if (h > header_h + 2) {
      Gdiplus::Color mid_col = light_panel_
          ? Gdiplus::Color(40, 15, 23, 42)
          : Gdiplus::Color(60, static_cast<BYTE>((235 * 3 + ar) / 4),
                               static_cast<BYTE>((245 * 3 + ag) / 4),
                               static_cast<BYTE>((255 * 3 + ab) / 4));
      Gdiplus::Color trans_col(0, mid_col.GetR(), mid_col.GetG(), mid_col.GetB());
      Gdiplus::LinearGradientBrush sep(
          Gdiplus::Point(0, header_h), Gdiplus::Point(rw, header_h),
          trans_col, trans_col);
      Gdiplus::Color colors[3] = {trans_col, mid_col, trans_col};
      Gdiplus::REAL positions[3] = {0.0f, 0.5f, 1.0f};
      sep.SetInterpolationColors(colors, positions, 3);
      Gdiplus::Pen sep_pen(&sep, 1.0f);
      g.DrawLine(&sep_pen, 0, header_h, rw, header_h);
    }

    // Subtle specular inner rim along the very top edge (360/Fluent 晶润高光)
    {
      Gdiplus::Color rim_mid = light_panel_
          ? Gdiplus::Color(80, 255, 255, 255)
          : Gdiplus::Color(95, 255, 255, 255);
      Gdiplus::Color rim_ends(0, 255, 255, 255);
      Gdiplus::LinearGradientBrush top_rim(
          Gdiplus::Point(0, 1), Gdiplus::Point(rw, 1),
          rim_ends, rim_ends);
      Gdiplus::Color rim_colors[3] = {rim_ends, rim_mid, rim_ends};
      Gdiplus::REAL rim_pos[3] = {0.0f, 0.5f, 1.0f};
      top_rim.SetInterpolationColors(rim_colors, rim_pos, 3);
      Gdiplus::Pen rim_pen(&top_rim, 1.0f);
      const int inset = rounded ? radius : 2;
      g.DrawLine(&rim_pen, inset, 1, rw - inset, 1);
    }

    // 现代精致微光光晕边框（融入 preset accent 强调色微光，通透柔和，告别发硬白框）
    Gdiplus::Color edge_col = light_panel_
        ? Gdiplus::Color(45, 15, 23, 42)
        : Gdiplus::Color(55, static_cast<BYTE>((255 * 7 + ar * 3) / 10),
                             static_cast<BYTE>((255 * 7 + ag * 3) / 10),
                             static_cast<BYTE>((255 * 7 + ab * 3) / 10));
    Gdiplus::Pen edge(edge_col, 1.0f);
    if (rr > 2 && rr < rw && rr < rh) {
      g.DrawPath(&edge, &path);
    } else {
      g.DrawRectangle(&edge, 0, 0, rw - 1, rh - 1);
    }
    panel_chrome_.resize(nbytes);
    std::memcpy(panel_chrome_.data(), bits, nbytes);
    panel_chrome_w_ = w;
    panel_chrome_h_ = h;
    panel_chrome_bg_ = bg_;
    panel_chrome_accent_ = accent_;
    panel_chrome_alpha_ = panel_alpha_;
    panel_chrome_light_ = light_panel_;
    panel_chrome_radius_ = radius;
  }
  SetGraphicsMode(mem, GM_COMPATIBLE);

  Paint(mem);

  GetWindowRect(hwnd_, &wr);
  POINT origin{wr.left, wr.top};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = fade_alpha_;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateOverlayLayered(hwnd_, screen, &origin, &size, mem, &src, 0, &blend,
                      ULW_ALPHA);

  ReleaseDC(nullptr, screen);
  layer_dirty_ = false;
}

void FenceWindow::BlitLayerAtAlpha(BYTE alpha) {
  // Fade steps must not repaint: Paint() walks every cell (icons + captions)
  // and doing that per animation frame is what makes ULW overlays stutter.
  // The layer bitmap already holds the last full render, so only the constant
  // alpha changes here.
  if (!hwnd_ || layer_dirty_ || !IsWindowVisible(hwnd_)) {
    return;
  }
  int w = 0;
  int h = 0;
  if (!ResolveLayerSize(&w, &h)) {
    return;
  }
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  POINT origin{wr.left, wr.top};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = alpha;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateOverlayLayered(hwnd_, screen, &origin, &size, layer_dc_, &src, 0,
                       &blend, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
}

void FenceWindow::RefreshLayer() {
  // Re-upload visible fence pixels. Fixes hybrid-GPU (Optimus) machines where
  // the initial UpdateLayeredWindow blit is lost after the driver/DWM swaps
  // adapters — the fence window reports visible but renders blank until some
  // other message triggers a re-Present.
  if (hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    PresentLayered();
  }
}

void FenceWindow::SetStyleId(const std::string& style_id) {
  style_id_ = NormalizeFenceStyleId(style_id);
  has_custom_colors_ = false;
  opacity_override_ = -1;
  radius_override_ = -1;
  ApplyStyle();
  if (hwnd_ && IsWindowVisible(hwnd_)) {
    PresentLayered();
  }
}

void FenceWindow::SetOpacity(int opacity) {
  if (opacity < 0) {
    opacity_override_ = -1;
  } else {
    opacity_override_ = (std::min)(255, (std::max)(40, opacity));
  }
  ApplyStyle();
}

void FenceWindow::SetCustomColors(bool enabled, COLORREF bg, COLORREF accent) {
  // Explicit custom colours override the preset's bg/accent but keep the
  // preset's title colour, alpha and light/dark flag (ApplyStyle handles that).
  has_custom_colors_ = enabled;
  if (enabled) {
    bg_ = bg;
    accent_ = accent;
  }
  ApplyStyle();
  if (hwnd_ && IsWindowVisible(hwnd_)) {
    PresentLayered();
  }
}

int FenceWindow::IconSizePx() const {
  if (view_mode_ == "list") {
    return S(hwnd_, kListIcon0);  // kListIcon0 is a 96-dpi design value.
  }
  // grid_metrics_ is already physical px (queried from the DPI-aware desktop
  // ListView / registry). Re-scaling by DPI/96 double-counts and inflates icons
  // on scaled displays (150% laptops showed oversized / clipped icons).
  return MulDiv(grid_metrics_.icon_size, icon_zoom_percent_, 100);
}

int FenceWindow::CellWidthPx() const {
  return MulDiv(MulDiv(grid_metrics_.cell_w, cell_scale_, 100),
                icon_zoom_percent_, 100);
}

int FenceWindow::CellHeightPx() const {
  return MulDiv(MulDiv(grid_metrics_.cell_h, cell_scale_, 100),
                icon_zoom_percent_, 100);
}

void FenceWindow::SetCellScale(int percent) {
  const int clamped = (std::min)(150, (std::max)(85, percent));
  if (clamped == cell_scale_) {
    return;
  }
  cell_scale_ = clamped;
  LayoutCells();
  PresentLayered();
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::SetFadeOnIdle(bool on) {
  if (on == fade_on_idle_) {
    return;
  }
  fade_on_idle_ = on;
  if (hwnd_ && !on) {
    KillTimer(hwnd_, kTimerIdleFade);
    if (fade_alpha_ != 255) {
      fade_alpha_ = 255;
      BlitLayerAtAlpha(fade_alpha_);
    }
  }
  // Persist here rather than in the caller: every other per-fence setter owns
  // its own persistence, and a caller that forgets loses the toggle on restart.
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::TickIdleFade() {
  // Target: dimmed while the pointer is outside, full while inside.
  POINT pt{};
  GetCursorPos(&pt);
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  const BYTE target = PtInRect(&rc, pt) ? 255 : kIdleFadeAlpha;
  if (fade_alpha_ == target) {
    KillTimer(hwnd_, kTimerIdleFade);
    return;
  }
  const int step = (target > fade_alpha_) ? kIdleFadeStep : -kIdleFadeStep;
  int next = static_cast<int>(fade_alpha_) + step;
  next = (std::min)(255, (std::max)(static_cast<int>(kIdleFadeAlpha), next));
  if ((step > 0 && next > target) || (step < 0 && next < target)) {
    next = target;
  }
  fade_alpha_ = static_cast<BYTE>(next);
  BlitLayerAtAlpha(fade_alpha_);
  if (fade_alpha_ == target) {
    KillTimer(hwnd_, kTimerIdleFade);
  }
}

void FenceWindow::SetShowTitle(bool show) {
  if (show == show_title_) {
    return;
  }
  show_title_ = show;
  // The header strip is part of the cell layout origin, so a title toggle has
  // to re-lay out, not just repaint.
  LayoutCells();
  PresentLayered();
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::SetIconZoom(int percent) {
  // Match Python _icon_zoom_bounds(): 0.7 .. 1.6.
  const int clamped = (std::min)(160, (std::max)(70, percent));
  if (clamped == icon_zoom_percent_) {
    return;
  }
  icon_zoom_percent_ = clamped;
  footer_zoom_hint_until_ = GetTickCount() + 900;
  RebuildIconsForZoom();
  if (on_zoom_changed_) {
    on_zoom_changed_(this, icon_zoom_percent_);
  }
}

void FenceWindow::SetLabelStyle(int lines, bool show) {
  const int clamped = (std::min)(3, (std::max)(1, lines));
  if (clamped == label_lines_ && show == show_labels_) {
    return;
  }
  label_lines_ = clamped;
  show_labels_ = show;
  LayoutCells();
  PresentLayered();
  if (on_moved_) {
    on_moved_(this);
  }
}

bool FenceWindow::NameMatchesFilter(const std::wstring& name) const {
  if (filter_text_.empty()) {
    return true;
  }
  // Case-insensitive substring, same feel as Explorer's type-to-find.
  const auto it = std::search(
      name.begin(), name.end(), filter_text_.begin(), filter_text_.end(),
      [](wchar_t a, wchar_t b) { return towlower(a) == towlower(b); });
  return it != name.end();
}

void FenceWindow::BeginFilter() {
  if (!hwnd_ || collapsed_) {
    return;
  }  if (filter_edit_) {
    SetFocus(filter_edit_);
    SendMessageW(filter_edit_, EM_SETSEL, 0, -1);
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int edit_h = S(hwnd_, 24);
  // Pin the box to the fence's top strip so the filtered grid below it stays
  // readable; popup (not WS_CHILD) because ULW hosts hide child EDITs.
  POINT origin{S(hwnd_, 6), S(hwnd_, 4)};
  ClientToScreen(hwnd_, &origin);
  const int edit_w =
      (std::max)(S(hwnd_, 120), static_cast<int>(rc.right - S(hwnd_, 12)));
  filter_edit_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"EDIT", filter_text_.c_str(),
      WS_POPUP | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, origin.x, origin.y,
      edit_w, edit_h, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!filter_edit_) {
    return;
  }
  filter_proc_ = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(filter_edit_, GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(FilterEditProc)));
  SetPropW(filter_edit_, L"oldproc",
           reinterpret_cast<HANDLE>(filter_proc_));
  // Marks this EDIT for the key hook: it has no input queue of its own, so
  // typed characters keep arriving through the hook instead of the control.
  SetPropW(filter_edit_, L"desktidy_fence_filter",
           reinterpret_cast<HANDLE>(1));
  SetWindowLongPtrW(filter_edit_, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(this));
  SetFocus(filter_edit_);
}

void FenceWindow::ApplyFilter(const std::wstring& text) {
  if (text == filter_text_) {
    return;
  }
  filter_text_ = text;
  std::vector<DesktopItem> narrowed;
  narrowed.reserve(all_items_.size());
  for (const auto& it : all_items_) {
    if (NameMatchesFilter(it.name)) {
      narrowed.push_back(it);
    }
  }
  SetItems(std::move(narrowed));
}

void FenceWindow::CloseFilterEdit() {
  if (!filter_edit_) {
    return;
  }
  HWND edit = filter_edit_;
  filter_edit_ = nullptr;
  filter_proc_ = nullptr;
  DestroyWindow(edit);
}

void FenceWindow::EndFilter() {
  if (filter_edit_) {
    HWND edit = filter_edit_;
    filter_edit_ = nullptr;
    filter_proc_ = nullptr;
    DestroyWindow(edit);
  }
  if (!filter_text_.empty()) {
    filter_text_.clear();
    SetItems(all_items_);
  }
}

void FenceWindow::RebuildIconsForZoom() {
  ClearIconCache();
  const int icon_sz = IconSizePx();
  std::vector<std::pair<std::wstring, int>> cold;
  for (auto& cell : cells_) {
    if (cell.icon) {
      DestroyIcon(cell.icon);
      cell.icon = nullptr;
    }
    if (ItemIconCached(cell.item.path, icon_sz)) {
      cell.icon = LoadItemIcon(cell.item.path, icon_sz);
    } else if (!cell.item.path.empty()) {
      cold.emplace_back(cell.item.path, icon_sz);
    }
  }
  LayoutCells();
  if (!cold.empty()) {
    PreloadIconsNotifyApp(cold);
  }
  if (!hwnd_) {
    return;
  }
  if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    PresentLayered();
  } else {
    layer_dirty_ = true;
  }
}

void FenceWindow::ApplyZoomWheel(int delta) {
  if (view_mode_ == "list" || delta == 0) {
    return;
  }
  // Python: 0.15 step per notch → 15 percentage points.
  const int notches = delta / WHEEL_DELTA;
  if (notches == 0) {
    return;
  }
  SetIconZoom(icon_zoom_percent_ + 15 * notches);
}

void FenceWindow::SetPinOrder(std::vector<std::wstring> order) {
  pin_order_ = std::move(order);
}

void FenceWindow::InsertPinOrder(const std::vector<std::wstring>& paths,
                                 int at) {
  // Drop index is relative to the current visual cells_ (Fences-class). Splice
  // against that order — an empty/stale pin_order_ made `at` clamp to 0 and the
  // icon jump to the front / name-sort tail instead of the drop slot.
  std::vector<std::wstring> base;
  if (!cells_.empty()) {
    base.reserve(cells_.size());
    for (const auto& c : cells_) {
      if (!c.item.path.empty()) {
        base.push_back(c.item.path);
      }
    }
  } else {
    for (const auto& p : pin_order_) {
      if (!p.empty()) {
        base.push_back(p);
      }
    }
  }
  auto is_dropped = [&](const std::wstring& p) {
    if (p.empty()) {
      return false;
    }
    const std::wstring key = NormalizePinKey(p);
    for (const auto& d : paths) {
      if (!d.empty() && NormalizePinKey(d) == key) {
        return true;
      }
    }
    return false;
  };
  std::vector<std::wstring> filtered;
  filtered.reserve(base.size() + paths.size());
  for (const auto& p : base) {
    if (!is_dropped(p)) {
      filtered.push_back(p);
    }
  }
  const size_t n = filtered.size();
  size_t idx = at < 0 ? n : static_cast<size_t>(at);
  if (idx > n) {
    idx = n;
  }
  filtered.insert(filtered.begin() + static_cast<ptrdiff_t>(idx), paths.begin(),
                  paths.end());
  pin_order_ = std::move(filtered);
}

void FenceWindow::RemovePinOrder(const std::vector<std::wstring>& paths) {
  // Fences packs to top-left: mid-remove collapses the seat (no empty holes).
  auto is_dropped = [&](const std::wstring& p) {
    if (p.empty()) {
      return true;  // also strip any legacy "" sentinels
    }
    const std::wstring key = NormalizePinKey(p);
    for (const auto& d : paths) {
      if (!d.empty() && NormalizePinKey(d) == key) {
        return true;
      }
    }
    return false;
  };
  std::vector<std::wstring> filtered;
  filtered.reserve(pin_order_.size());
  for (const auto& p : pin_order_) {
    if (!is_dropped(p)) {
      filtered.push_back(p);
    }
  }
  pin_order_ = std::move(filtered);
}

void FenceWindow::RetargetPinOrder(const std::wstring& from,
                                   const std::wstring& to) {
  if (from.empty() || to.empty()) {
    return;
  }
  const std::wstring from_key = NormalizePinKey(from);
  const std::wstring to_key = NormalizePinKey(to);
  if (from_key == to_key) {
    return;
  }
  // Drop any pre-existing `to` seat so the renamed path keeps a single slot.
  // Leave "" holes untouched.
  for (auto it = pin_order_.begin(); it != pin_order_.end();) {
    if (it->empty()) {
      ++it;
      continue;
    }
    if (NormalizePinKey(*it) == to_key) {
      it = pin_order_.erase(it);
    } else {
      ++it;
    }
  }
  bool hit = false;
  for (auto& p : pin_order_) {
    if (p.empty()) {
      continue;
    }
    if (NormalizePinKey(p) == from_key) {
      p = to;
      hit = true;
    }
  }
  if (!hit) {
    return;
  }
  // Keep cells_ in sync until the next SetItems (avoids a one-frame jump).
  for (auto& c : cells_) {
    if (NormalizePinKey(c.item.path) == from_key) {
      c.item.path = to;
      const size_t slash = to.find_last_of(L"\\/");
      c.item.name = slash == std::wstring::npos ? to : to.substr(slash + 1);
    }
  }
}

void FenceWindow::SetDropHighlight(bool on) {
  // Whole-fence wash replaced by insert-bar / recycle-icon preview.
  if (!on) {
    ClearOleDropPreview();
    return;
  }
  if (drop_highlight_ == on) {
    return;
  }
  drop_highlight_ = on;
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::UpdateOleDropPreview(int x, int y) {
  const bool recycle = IsRecycleBinIconAt(x, y);
  const bool folder = !recycle && !DirectoryIconAt(x, y).empty();
  const int insert = (recycle || folder) ? -1 : InsertIndexAt(x, y);
  if (drop_preview_ && drop_over_recycle_ == recycle &&
      drop_over_folder_ == folder && drop_insert_ == insert &&
      drop_pt_.x == x && drop_pt_.y == y) {
    return;
  }
  const bool structural =
      !drop_preview_ || drop_over_recycle_ != recycle ||
      drop_over_folder_ != folder || drop_insert_ != insert;
  drop_preview_ = true;
  drop_over_recycle_ = recycle;
  drop_over_folder_ = folder;
  drop_insert_ = insert;
  drop_pt_ = {x, y};
  drop_highlight_ = false;
  if (!hwnd_) {
    return;
  }
  // Insert/recycle/folder changes: immediate. Ghost cursor only: ~30fps.
  const DWORD now = GetTickCount();
  if (structural || last_ole_preview_present_ms_ == 0 ||
      now - last_ole_preview_present_ms_ >= 33u) {
    PresentLayered();
    last_ole_preview_present_ms_ = now;
  }
}

void FenceWindow::ClearOleDropPreview() {
  if (!drop_preview_ && !drop_highlight_ && !drop_over_recycle_ &&
      !drop_over_folder_ && drop_insert_ < 0) {
    return;
  }
  drop_preview_ = false;
  drop_over_recycle_ = false;
  drop_over_folder_ = false;
  drop_insert_ = -1;
  drop_pt_ = {};
  drop_highlight_ = false;
  last_ole_preview_present_ms_ = 0;
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::SetPageGeometry(int page, const FencePageGeometry& geo) {
  page_geometries_[page] = geo;
}

FencePageGeometry FenceWindow::GeometryForPage(int page) const {
  return FenceGeometryForPage(CurrentLayout(), page);
}

void FenceWindow::ApplyPageGeometry(const FencePageGeometry& geo,
                                    bool present) {
  const int w = (std::max)(geo.w, 120);
  const int h = (std::max)(geo.h, 80);
  collapsed_ = geo.collapsed;
  expanded_h_ = (std::max)(h, kFenceExpandedFloor);
  if (hwnd_) {
    const int win_h = collapsed_ ? S(hwnd_, kCollapsedH0) : expanded_h_;
    SetOverlayScreenPos(hwnd_, geo.x, geo.y, w, win_h,
                        SWP_NOZORDER | SWP_NOREDRAW);
    LayoutCells();
    // Skip expensive layered present for fences that are (or will stay) hidden
    // on this page — page switches used to redraw every fence twice.
    if (present && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
      PresentLayered();
    }
  }
}

bool FenceWindow::VisibleOnPage(int page) const {
  if (!OnPage(page)) {
    return false;
  }
  const auto it = page_geometries_.find(page);
  return it != page_geometries_.end() ? it->second.visible : visible_;
}

void FenceWindow::SetOrganizeKinds(std::vector<std::string> kinds) {
  organize_kinds_ = std::move(kinds);
}

void FenceWindow::SetIconGrid(const IconGridMetrics& m) {
  grid_metrics_ = m;
  RebuildIconsForZoom();
}

void FenceWindow::ScaleGeometryForDpi(int old_dpi, int new_dpi) {
  RECT rc{};
  int x = 0;
  int y = 0;
  int w = 0;
  int h = expanded_h_;
  if (hwnd_) {
    GetWindowRect(hwnd_, &rc);
    x = rc.left;
    y = rc.top;
    w = rc.right - rc.left;
    if (!collapsed_) {
      h = rc.bottom - rc.top;
    }
  }
  ScaleRectForDpi(&x, &y, &w, &h, old_dpi, new_dpi);
  expanded_h_ = (std::max)(h, kFenceExpandedFloor);
  if (hwnd_) {
    const int win_h = collapsed_ ? S(hwnd_, kCollapsedH0) : h;
    SetOverlayScreenPos(hwnd_, x, y, w, win_h, SWP_NOZORDER);
  }
  for (auto& kv : page_geometries_) {
    ScaleRectForDpi(&kv.second.x, &kv.second.y, &kv.second.w, &kv.second.h,
                    old_dpi, new_dpi);
  }
}

bool FenceWindow::Create(HINSTANCE instance, const FenceLayout& layout) {
  id_ = layout.id;
  title_ = layout.title;
  page_ = layout.page;
  if (!layout.pages.empty()) {
    SetPages(layout.pages);
  } else {
    pages_ = {layout.page};
  }
  style_id_ = NormalizeFenceStyleId(layout.style_id);
  portal_path_ = layout.portal_path;
  collapsed_ = layout.collapsed;
  auto_hide_ = layout.auto_hide;
  position_locked_ = layout.position_locked;
  content_locked_ = layout.content_locked;
  label_lines_ = (std::min)(3, (std::max)(1, layout.label_lines));
  cell_scale_ = (std::min)(150, (std::max)(85, layout.cell_scale));
  fade_on_idle_ = layout.fade_on_idle;
  show_labels_ = layout.show_labels;
  sort_by_ = layout.sort_by.empty() ? "name" : layout.sort_by;
  view_mode_ = (layout.view_mode == "list") ? "list" : "grid";
  show_title_ = layout.show_title;
  collapsible_ = layout.collapsible;
  radius_override_ = layout.radius;
  has_custom_colors_ = layout.has_custom_colors;
  if (has_custom_colors_) {
    bg_ = layout.custom_bg;
    accent_ = layout.custom_accent;
  }
  opacity_override_ =
      (layout.opacity >= 0 && layout.opacity <= 255) ? layout.opacity : -1;
  visible_ = layout.visible;
  organize_kinds_ = layout.organize_kinds;
  page_geometries_ = layout.page_geometries;
  pin_order_ = layout.pin_order;
  icon_zoom_percent_ = (std::min)(160, (std::max)(70, layout.icon_zoom));
  grid_metrics_ = QueryIconGridMetrics();
  expanded_h_ = (std::max)(layout.h, kFenceExpandedFloor);
  const int win_h = collapsed_ ? kCollapsedH0 : layout.h;

  if (!g_class_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // CS_DBLCLKS required for WM_LBUTTONDBLCLK (open on double-click).
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = FenceWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc)) {
      if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
      }
    }
    g_class_registered = true;
  }

  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_ACCEPTFILES,
      kClassName, title_.c_str(), WS_POPUP, layout.x, layout.y, layout.w,
      win_h, nullptr, nullptr, instance, this);

  if (!hwnd_) {
    return false;
  }

  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ApplyStyle();
  EnsureDropTarget();
  // First paint happens on Show / WarmLayerOffscreen / SetItems while mapped.
  layer_dirty_ = true;
  return true;
}

void FenceWindow::SetCollapsed(bool collapsed, bool persist) {
  if (!hwnd_) {
    collapsed_ = collapsed;
    return;
  }
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int cur_h = rc.bottom - rc.top;
  if (collapsed) {
    if (!collapsed_) {
      expanded_h_ = (std::max)(cur_h, kFenceExpandedFloor);
    }
    collapsed_ = true;
    EndFilter();
    SetOverlayScreenPos(hwnd_, rc.left, rc.top, w, S(hwnd_, kCollapsedH0),
                        SWP_NOZORDER);
  } else {
    collapsed_ = false;
    const int h = (std::max)(expanded_h_, kFenceExpandedFloor);
    SetOverlayScreenPos(hwnd_, rc.left, rc.top, w, h, SWP_NOZORDER);
  }
  LayoutCells();
  PresentLayered();
  // Auto-hide hover expand/collapse must not rewrite layout.json every move.
  if (persist && on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::ToggleCollapsed() {
  if (!collapsible_) {
    return;
  }
  SetCollapsed(!collapsed_, /*persist=*/true);
}

void FenceWindow::SetAutoHide(bool on) {
  if (auto_hide_ == on) {
    return;
  }
  auto_hide_ = on;
  if (hwnd_) {
    KillTimer(hwnd_, kTimerAutoHide);
  }
  // Fences: enabling Auto-hide rests in the collapsed title strip.
  if (auto_hide_ && collapsible_ && !collapsed_) {
    SetCollapsed(true, /*persist=*/false);
  }
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::SetPositionLocked(bool locked) {
  if (position_locked_ == locked) {
    return;
  }
  position_locked_ = locked;
  if (hwnd_) {
    PresentLayered();
  }
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::SetContentLocked(bool locked) {
  if (content_locked_ == locked) {
    return;
  }
  content_locked_ = locked;
  if (hwnd_) {
    PresentLayered();
  }
  // Persist through the same layout path position lock uses.
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::EnsureDropTarget() {
  if (!hwnd_ || drop_target_) {
    return;
  }
  drop_target_ =
      new PathDropTarget(
          [this](const std::vector<std::wstring>& paths,
                 DWORD proposed) -> DWORD {
            // Content lock freezes membership: refuse inbound drops so the
            // source keeps its files (DROPEFFECT_NONE, never a fake MOVE).
            if (content_locked_) {
              return DROPEFFECT_NONE;
            }
            // Fences/Explorer: drop on Recycle Bin icon deletes, never pins
            // into the fence that hosts the bin (desktop batch-drag bug).
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            if (IsRecycleBinIconAt(pt.x, pt.y)) {
              return RecycleDroppedPaths(
                         std::vector<std::wstring>(paths))
                         ? DROPEFFECT_MOVE
                         : DROPEFFECT_NONE;
            }
            // Explorer: drop onto a folder icon → import into that folder.
            const std::wstring folder = DirectoryIconAt(pt.x, pt.y);
            if (!folder.empty()) {
              return ImportPathsIntoDirectory(paths, folder, proposed);
            }
            if (on_paths_dropped_at_) {
              // F1: derive insertion index from the drop point (cursor is still
              // at the drop location when PathDropTarget::Drop fires).
              const int at = InsertIndexAt(pt.x, pt.y);
              return on_paths_dropped_at_(this, paths, at);
            }
            if (on_paths_dropped_) {
              return on_paths_dropped_(this, paths);
            }
            return proposed;
          },
          [this](const std::wstring& src, const std::vector<std::wstring>& paths,
                 bool commit) -> DWORD {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            if (IsRecycleBinIconAt(pt.x, pt.y)) {
              if (!commit) {
                return DROPEFFECT_MOVE;
              }
              return RecycleDroppedPaths(
                         std::vector<std::wstring>(paths))
                         ? DROPEFFECT_MOVE
                         : DROPEFFECT_NONE;
            }
            const std::wstring folder = DirectoryIconAt(pt.x, pt.y);
            if (!folder.empty()) {
              // Explorer: Ctrl=copy, Shift=move, Alt/Ctrl+Shift=.lnk.
              DWORD effect = DROPEFFECT_MOVE;
              if (WantDropLinkEffect()) {
                effect = DROPEFFECT_LINK;
              } else if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                         (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
                effect = DROPEFFECT_COPY;
              }
              if (!commit) {
                return effect;
              }
              return ImportPathsIntoDirectory(paths, folder, effect);
            }
            if (on_pin_drop_) {
              // Fences: drop seat decides pin_order insert (not always append).
              const int at = InsertIndexAt(pt.x, pt.y);
              return on_pin_drop_(this, src, paths, commit, at);
            }
            return DROPEFFECT_NONE;
          });
  drop_target_->SetHighlightFn([this](bool on) {
    if (!on) {
      ClearOleDropPreview();
      if (auto_hide_ && !collapsed_ && collapsible_ && !dragging_ &&
          !resizing_ && !icon_dragging_ && !rubber_ && !rename_edit_) {
        SetTimer(hwnd_, kTimerAutoHide, 350, nullptr);
      }
      return;
    }
    if (auto_hide_) {
      KillTimer(hwnd_, kTimerAutoHide);
      if (collapsed_ && collapsible_) {
        SetCollapsed(false, /*persist=*/false);
      }
    }
  });
  drop_target_->SetDragOverFn([this](POINTL spt, bool active) {
    if (!active) {
      ClearOleDropPreview();
      // Fences Auto-hide: OLE leave → same delayed collapse as WM_MOUSELEAVE.
      if (auto_hide_ && !collapsed_ && collapsible_ && !dragging_ &&
          !resizing_ && !icon_dragging_ && !rubber_ && !rename_edit_) {
        SetTimer(hwnd_, kTimerAutoHide, 350, nullptr);
      }
      return;
    }
    // Fences Auto-hide: expand on drag-over so insert seats are visible.
    if (auto_hide_) {
      KillTimer(hwnd_, kTimerAutoHide);
      if (collapsed_ && collapsible_ && !dragging_ && !resizing_ &&
          !icon_dragging_ && !rubber_ && !rename_edit_) {
        SetCollapsed(false, /*persist=*/false);
      }
    }
    POINT pt{spt.x, spt.y};
    ScreenToClient(hwnd_, &pt);
    UpdateOleDropPreview(pt.x, pt.y);
  });
  // AIGC START
  // Plain fence = virtual pin (COPY so Explorer keeps the source). Portal =
  // physical import with Explorer volume/modifier rules. Hovering the hosted
  // Recycle Bin advertises MOVE (delete) like Explorer.
  drop_target_->SetFileDropSemanticsProvider([this]() {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    if (IsRecycleBinIconAt(pt.x, pt.y)) {
      return FileDropSemantics::PreferMove;
    }
    // Folder icon: physical import (volume/Ctrl/Shift), not virtual pin.
    if (!DirectoryIconAt(pt.x, pt.y).empty()) {
      return FileDropSemantics::Physical;
    }
    return portal_path_.empty() ? FileDropSemantics::VirtualPin
                                : FileDropSemantics::Physical;
  });
  drop_target_->SetDestDirProvider([this]() {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd_, &pt);
    const std::wstring folder = DirectoryIconAt(pt.x, pt.y);
    if (!folder.empty()) {
      return folder;
    }
    return PortalBrowsePath();
  });
  // AIGC END
  if (!RegisterPathDropTarget(hwnd_, drop_target_)) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
}

void FenceWindow::Destroy() {
  if (GetFenceKeyTarget() == hwnd_) {
    ClearFenceKeyTarget();
  }
  ClearIcons();
  ReleaseFonts();
  ReleaseLayerBuffer();
  if (tip_) {
    DestroyWindow(tip_);
    tip_ = nullptr;
  }
  tip_index_ = -1;
  if (hwnd_) {
    RevokePathDropTarget(hwnd_);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (drop_target_) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
}

void FenceWindow::EnsureTooltip() {
  if (tip_ || !hwnd_) {
    return;
  }
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
  InitCommonControlsEx(&icc);
  tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                         WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                         hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!tip_) {
    return;
  }
  SetWindowPos(tip_, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  TOOLINFOW ti{};
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
  ti.hwnd = hwnd_;
  ti.uId = 1;
  ti.lpszText = const_cast<wchar_t*>(L"");
  SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
  SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, 320);
}

bool FenceWindow::TitleLabelRect(RECT* out) const {
  if (!out || !hwnd_ || !show_title_) {
    return false;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  RECT collapse_rc{}, lock_rc{}, dissolve_rc{}, portal_up_rc{};
  HeaderBtnRects(&collapse_rc, &lock_rc, &dissolve_rc, &portal_up_rc);
  int chrome_right = rc.right;
  auto consider = [&](const RECT& r) {
    if (r.right > r.left && r.left < chrome_right) {
      chrome_right = r.left;
    }
  };
  consider(collapse_rc);
  consider(lock_rc);
  consider(dissolve_rc);
  chrome_right = (std::max)(0, chrome_right - 6);
  const int title_left =
      (portal_up_rc.right > portal_up_rc.left) ? portal_up_rc.right + 4
                                               : S(hwnd_, 6);
  const int title_top = S(hwnd_, 6);
  const int title_px = S(hwnd_, 13);
  out->left = title_left;
  out->top = title_top;
  out->right = chrome_right;
  out->bottom = title_top + title_px + 6;
  return out->right > out->left && out->bottom > out->top;
}

bool FenceWindow::CaptionFitsLabel(const std::wstring& text,
                                   const RECT& label) const {
  if (text.empty() || !hwnd_) {
    return true;
  }
  const int box_w = label.right - label.left;
  if (box_w <= 0) {
    return true;
  }
  HDC hdc = GetDC(hwnd_);
  if (!hdc) {
    return true;
  }
  const int font_px = S(hwnd_, 11);
  HFONT font = CreateFontW(-font_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
  SIZE sz{};
  GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &sz);
  if (old) {
    SelectObject(hdc, old);
  }
  if (font) {
    DeleteObject(font);
  }
  ReleaseDC(hwnd_, hdc);
  return sz.cx <= box_w;
}

void FenceWindow::UpdateHoverTip(int index) {
  // Explorer/Fences: show full name tip only when the caption is truncated.
  EnsureTooltip();
  const int prev = tip_index_;
  tip_index_ =
      (index >= 0 && index < static_cast<int>(cells_.size())) ? index : -1;
  if (tip_index_ != prev && hwnd_) {
    PresentLayered();
  }
  if (!tip_) {
    return;
  }
  TOOLINFOW ti{};
  ti.cbSize = sizeof(ti);
  ti.hwnd = hwnd_;
  ti.uId = 1;
  if (tip_index_ < 0) {
    SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    tip_text_.clear();
    return;
  }
  RECT label{};
  if (!IconLabelRect(static_cast<size_t>(tip_index_), &label)) {
    SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    return;
  }
  const std::wstring& name = cells_[static_cast<size_t>(tip_index_)].item.name;
  if (CaptionFitsLabel(name, label)) {
    SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    tip_text_.clear();
    return;
  }
  tip_text_ = name;
  ti.lpszText = tip_text_.data();
  SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
  POINT pt{};
  GetCursorPos(&pt);
  SendMessageW(tip_, TTM_TRACKPOSITION, 0,
               MAKELPARAM(pt.x + 12, pt.y + 16));
  SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
}

void FenceWindow::ClearIcons() {
  ClearIconCache();
  for (auto& c : cells_) {
    if (c.icon) {
      DestroyIcon(c.icon);
      c.icon = nullptr;
    }
  }
  cells_.clear();
}

const std::vector<DesktopItem> FenceWindow::ItemSnapshot() const {
  std::vector<DesktopItem> out;
  out.reserve(cells_.size());
  for (const auto& c : cells_) {
    if (c.item.path.empty()) {
      continue;  // sparse seats are not membership
    }
    out.push_back(c.item);
  }
  return out;
}

void FenceWindow::SetTitle(const std::wstring& title) {
  title_ = title;
  if (hwnd_) {
    SetWindowTextW(hwnd_, DisplayTitle().c_str());
    PresentLayered();
  }
}

std::wstring FenceWindow::DisplayTitle() const {
  // Fences Folder Portal: root keeps the fence title; drilled-in shows the
  // current folder leaf (same idea as Explorer's address / portal chrome).
  if (portal_path_.empty() || portal_cwd_.empty()) {
    return title_;
  }
  std::wstring cur = portal_cwd_;
  while (!cur.empty() && (cur.back() == L'\\' || cur.back() == L'/')) {
    cur.pop_back();
  }
  const size_t slash = cur.find_last_of(L"\\/");
  const std::wstring leaf =
      slash == std::wstring::npos ? cur : cur.substr(slash + 1);
  return leaf.empty() ? title_ : leaf;
}

FenceLayout FenceWindow::CurrentLayout() const {
  FenceLayout fl;
  fl.id = id_;
  fl.title = title_;
  fl.page = page_;
  fl.pages = pages_.empty() ? std::vector<int>{page_} : pages_;
  fl.style_id = style_id_;
  fl.portal_path = portal_path_;
  fl.collapsed = collapsed_;
  fl.auto_hide = auto_hide_;
  fl.position_locked = position_locked_;
  fl.content_locked = content_locked_;
  fl.label_lines = label_lines_;
  fl.cell_scale = cell_scale_;
  fl.fade_on_idle = fade_on_idle_;
  fl.show_labels = show_labels_;
  fl.sort_by = sort_by_;
  fl.view_mode = view_mode_;
  fl.opacity = opacity_override_;
  fl.radius = radius_override_;
  fl.show_title = show_title_;
  fl.collapsible = collapsible_;
  fl.has_custom_colors = has_custom_colors_;
  fl.custom_bg = bg_;
  fl.custom_accent = accent_;
  fl.visible = visible_;
  fl.organize_kinds = organize_kinds_;
  fl.page_geometries = page_geometries_;
  fl.icon_zoom = icon_zoom_percent_;
  fl.pin_order = pin_order_;
  // Height always from owned expanded_h_ (collapse chrome is not layout.h).
  fl.h = (std::max)(expanded_h_, kFenceExpandedFloor);
  if (hwnd_) {
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    fl.x = rc.left;
    fl.y = rc.top;
    fl.w = rc.right - rc.left;
  }
  return fl;
}

bool FenceWindow::Show() {
  if (!hwnd_) {
    return false;
  }
  // Callers (SetPage / RestoreDesktopChrome) already filter VisibleOnPage for
  // the *current* app page. Do not gate on page_ alone — that is only the
  // primary membership and no-op'd multi-page / soft-park restores.
  // Fence-level show: clear hide on every membership page (Fences parity —
  // Hide must not resurrect when switching to another pages_ entry).
  visible_ = true;
  for (auto& kv : page_geometries_) {
    kv.second.visible = true;
  }
  if (page_geometries_.find(page_) == page_geometries_.end()) {
    FencePageGeometry geo = GeometryForPage(page_);
    geo.visible = true;
    page_geometries_[page_] = geo;
  }
  // Win+D minimizes desktop-owned overlays: IsWindowVisible stays TRUE while
  // IsIconic is set, and SW_SHOWNOACTIVATE cannot un-minimize. Treat iconic as
  // not-visible and restore it (Python overlay_win32_visible does the same).
  const bool was_iconic = IsIconic(hwnd_) != FALSE;
  const bool was_visible = IsWindowVisible(hwnd_) != FALSE && !was_iconic;
  const bool buried =
      was_visible && IsAttachedToDesktop(hwnd_) && !IsOverlayAboveDefView(hwnd_);

  // Fences-class: 分层位图先就绪并提交显存，确保映射第一帧饱满无暇，杜绝先显示后绘制的白屏/黑边闪烁
  if (!was_visible || was_iconic || buried || layer_dirty_) {
    LayoutCells();
    PresentLayered();
  }

  if (!IsAttachedToDesktop(hwnd_)) {
    AttachToDesktopLayer();
  } else if (buried || !IsOverlayAboveDefView(hwnd_)) {
    RaiseOverlayInDesktopBand(hwnd_);
  }

  if (was_iconic) {
    ShowWindow(hwnd_, SW_RESTORE);
  } else if (!was_visible) {
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }
  return true;
}

void FenceWindow::Hide() {
  // A hidden fence must not leave its filter box floating on the desktop.
  EndFilter();
  // Drop any in-flight idle fade: a dimmed layer would come back dim on Show.
  if (fade_alpha_ != 255) {
    fade_alpha_ = 255;
  }
  if (hwnd_) {
    KillTimer(hwnd_, kTimerIdleFade);
  }
  // Persist user-hide so ApplyPageVisibility does not resurrect (P1-2).
  // Fence-level: clear every page_geometries_ row — multi-page membership
  // used to leave sibling pages visible=true and resurrect on page switch.
  visible_ = false;
  for (auto& kv : page_geometries_) {
    kv.second.visible = false;
  }
  if (page_geometries_.find(page_) == page_geometries_.end()) {
    FencePageGeometry geo = GeometryForPage(page_);
    geo.visible = false;
    page_geometries_[page_] = geo;
  }
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::HideTransient() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void FenceWindow::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  // Fences keepalive: seat first, then Raise only if Z-order actually moved
  // (RaiseOverlay skips no-op SetWindowPos under many open apps).
  if (!AttachOverlayToDesktop(hwnd_, true)) {
    ForceAttachOverlayToDesktop(hwnd_, true);
  }
  RaiseOverlayInDesktopBand(hwnd_);
}

void FenceWindow::WarmLayerOffscreen() {
  if (!hwnd_ || !IsWindow(hwnd_) || !layer_dirty_) {
    return;
  }
  if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    PresentLayered();
    return;
  }
  RECT wr{};
  if (!GetWindowRect(hwnd_, &wr) || wr.right <= wr.left ||
      wr.bottom <= wr.top) {
    return;
  }
  const int w = wr.right - wr.left;
  const int h = wr.bottom - wr.top;
  // Show off-screen so ULW sticks, then soft-hide at the real seat.
  // If SetPage maps us mid-warm, do NOT Hide — that was "click desktop to see".
  SetWindowPos(hwnd_, nullptr, -32000, -32000, w, h,
               SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW);
  PresentLayered();
  RECT now{};
  if (!GetWindowRect(hwnd_, &now)) {
    return;
  }
  // Still parked off-screen ⇒ soft-hide back to the saved seat.
  if (now.left <= -16000 && now.top <= -16000) {
    SetWindowPos(hwnd_, nullptr, wr.left, wr.top, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_NOREDRAW);
  }
}

void FenceWindow::SetViewMode(const std::string& mode) {
  const std::string next = (mode == "list") ? "list" : "grid";
  if (view_mode_ == next) {
    return;
  }
  view_mode_ = next;
  // Reload icons at the size matching the view (cache-hit sync; cold async).
  const int sz = IconSizePx();
  std::vector<std::pair<std::wstring, int>> cold;
  for (auto& cell : cells_) {
    if (cell.icon) {
      DestroyIcon(cell.icon);
      cell.icon = nullptr;
    }
    if (ItemIconCached(cell.item.path, sz)) {
      cell.icon = LoadItemIcon(cell.item.path, sz);
    } else if (!cell.item.path.empty()) {
      cold.emplace_back(cell.item.path, sz);
    }
  }
  LayoutCells();
  if (!cold.empty()) {
    PreloadIconsNotifyApp(cold);
  }
  if (hwnd_) {
    PresentLayered();
  }
  if (on_moved_) {
    on_moved_(this);
  }
}

void FenceWindow::SetItems(std::vector<DesktopItem> items) {
  // A refresh (watch tick / organize) must not silently widen a live filter:
  // remember the unfiltered set and narrow the incoming list the same way.
  all_items_ = items;
  if (!filter_text_.empty()) {
    std::vector<DesktopItem> narrowed;
    narrowed.reserve(items.size());
    for (auto& it : items) {
      if (NameMatchesFilter(it.name)) {
        narrowed.push_back(std::move(it));
      }
    }
    items = std::move(narrowed);
  }
  // Keep HICONs across refresh when the path set is stable (desktop watch /
  // organize often re-pushes the same pins). Avoid SHGetFileInfo storms.
  // Premultiplied bitmap cache is keyed by HICON — only drop it when icons
  // are actually replaced/destroyed (same-seat refresh used to wipe it every
  // watch tick and force cold Present).
  const std::vector<DesktopItem> before = ItemSnapshot();
  // Dock-class early-out: identical membership + every filled cell has an
  // icon → skip clear/rebuild/sort/layout (RefreshDesktopIcons spam).
  // Sparse seats: cells_.size() may exceed items.size() (empty holes).
  {
    size_t filled = 0;
    bool all_icons = true;
    for (const auto& c : cells_) {
      if (c.item.path.empty()) {
        continue;
      }
      ++filled;
      if (!c.icon) {
        all_icons = false;
      }
    }
    if (!items.empty() && filled == items.size() &&
        DesktopItemListsIdentityEqual(before, items) && all_icons) {
      return;
    }
  }
  std::unordered_map<std::wstring, HICON> reuse;
  reuse.reserve(cells_.size());
  for (auto& c : cells_) {
    if (c.icon && !c.item.path.empty()) {
      reuse.emplace(NormalizePinKey(c.item.path), c.icon);
      c.icon = nullptr;
    }
  }
  cells_.clear();
  cells_.reserve(items.size());
  const int icon_sz = IconSizePx();
  bool loaded_any = false;
  for (auto& it : items) {
    Cell cell;
    cell.item = std::move(it);
    const std::wstring key = NormalizePinKey(cell.item.path);
    auto hit = reuse.find(key);
    if (hit != reuse.end()) {
      cell.icon = hit->second;
      reuse.erase(hit);
    } else if (ItemIconCached(cell.item.path, icon_sz)) {
      cell.icon = LoadItemIcon(cell.item.path, icon_sz);
      loaded_any = true;
    } else {
      // Cold icon: leave empty and let STA preload + FillMissingIcons fill in
      // (keeps RefreshDesktopIcons off the UI-thread SHGetFileInfo storm).
      cell.icon = nullptr;
      loaded_any = true;
    }
    cells_.push_back(std::move(cell));
  }
  const bool destroyed_unused = !reuse.empty();
  for (auto& kv : reuse) {
    if (kv.second) {
      DestroyIcon(kv.second);
    }
  }
  // Manual: honor pin_order_ (drop insert / drag reorder). Else name/date/…
  // Fences packs from top-left — skip empty/missing entries (no sparse seats).
  if (sort_by_ == "manual" && !pin_order_.empty()) {
    std::vector<Cell> ordered;
    ordered.reserve(cells_.size());
    std::vector<char> used(cells_.size(), 0);
    auto match = [&](const std::wstring& want) -> int {
      if (want.empty()) {
        return -1;
      }
      const std::wstring key = NormalizePinKey(want);
      for (size_t i = 0; i < cells_.size(); ++i) {
        if (used[i] || cells_[i].item.path.empty()) {
          continue;
        }
        if (NormalizePinKey(cells_[i].item.path) == key) {
          return static_cast<int>(i);
        }
      }
      return -1;
    };
    for (const auto& p : pin_order_) {
      const int i = match(p);
      if (i < 0) {
        continue;
      }
      used[static_cast<size_t>(i)] = 1;
      ordered.push_back(std::move(cells_[static_cast<size_t>(i)]));
    }
    for (size_t i = 0; i < cells_.size(); ++i) {
      if (!used[i] && !cells_[i].item.path.empty()) {
        ordered.push_back(std::move(cells_[i]));
      }
    }
    cells_ = std::move(ordered);
  } else {
    SortCells();
  }
  // Same seats: keep scroll + selection. Soft-parked fences may skip the
  // layered blit (buffer preserved). Visible fences must still Present —
  // skipping blit while mapped left HWND visible but fully transparent
  // (「文档」page looked empty after folder→Desktop refresh storms).
  const bool same_seats =
      !loaded_any && DesktopItemListsIdentityEqual(before, ItemSnapshot());
  if (!same_seats || destroyed_unused) {
    ClearIconCache();
  }
  if (!same_seats) {
    scroll_y_ = 0;
    selected_.assign(cells_.size(), 0);
    selection_anchor_ = -1;
    selection_focus_ = -1;
  }
  LayoutCells();
  if (!hwnd_) {
    return;
  }
  // Never UpdateLayeredWindow while hidden (Win11 blank-after-Show). Mark dirty
  // so SetPage / Show / WarmLayerOffscreen can paint after mapping.
  if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
    if (!same_seats) {
      layer_dirty_ = true;
    }
    return;
  }
  // SoftSeat no longer FRAMECHANGEs on every raise — same seats keep a valid
  // ULW buffer. Desktop-watch / organize used to force N× full blit even when
  // membership was unchanged (Fences-class: only repaint on dirty content).
  if (same_seats && !layer_dirty_) {
    return;
  }
  PresentLayered();
}

bool FenceWindow::FillMissingIcons() {
  const int icon_sz = IconSizePx();
  bool any = false;
  for (auto& cell : cells_) {
    if (cell.icon || cell.item.path.empty()) {
      continue;
    }
    if (!ItemIconCached(cell.item.path, icon_sz)) {
      continue;
    }
    cell.icon = LoadItemIcon(cell.item.path, icon_sz);
    if (cell.icon) {
      any = true;
    }
  }
  if (!any) {
    return false;
  }
  if (hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    PresentLayered();
  } else {
    layer_dirty_ = true;
  }
  return true;
}

void FenceWindow::SetSortBy(const std::string& sort_by) {
  const std::string next = sort_by.empty() ? "name" : sort_by;
  // Content lock freezes the icon set AND its order, so re-sorting is a
  // content mutation. Guarding here covers every caller (menu, settings page).
  if (content_locked_ && next != sort_by_) {
    ShowToast(L"分区内容已锁定，请先解锁");
    return;
  }
  const bool entering_manual = (next == "manual" && sort_by_ != "manual");
  sort_by_ = next;
  // Fences: switching to Manual freezes the current visual order — seed
  // pin_order_ now so the next SetItems/watch refresh cannot scramble seats.
  if (entering_manual && !cells_.empty()) {
    pin_order_.clear();
    pin_order_.reserve(cells_.size());
    for (const auto& c : cells_) {
      pin_order_.push_back(c.item.path);
    }
    if (on_reorder_) {
      on_reorder_(this, pin_order_);
    }
  }
  SortCells();
  selected_.assign(cells_.size(), 0);
  selection_anchor_ = -1;
  selection_focus_ = -1;
  LayoutCells();
  if (hwnd_) {
    PresentLayered();
  }
  if (on_moved_) {
    on_moved_(this);  // persist layout (incl. sort_by)
  }
}

void FenceWindow::SortCells() {
  if (cells_.size() < 2 || sort_by_ == "manual") {
    return;
  }
  // Short-TTL attr cache across refresh/sort storms (same paths re-queried).
  struct AttrRec {
    ULONGLONG mtime = 0;
    ULONGLONG ctime = 0;
    ULONGLONG size = 0;
    bool ok = false;
    ULONGLONG tick = 0;
  };
  static std::unordered_map<std::wstring, AttrRec> s_attr;
  constexpr ULONGLONG kAttrTtlMs = 4000;
  auto lookup_attr = [&](const std::wstring& path) -> AttrRec {
    const ULONGLONG now = GetTickCount64();
    auto it = s_attr.find(path);
    if (it != s_attr.end() && now - it->second.tick < kAttrTtlMs) {
      return it->second;
    }
    AttrRec rec;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    rec.ok = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) != 0;
    if (rec.ok) {
      ULARGE_INTEGER mt{};
      mt.LowPart = fa.ftLastWriteTime.dwLowDateTime;
      mt.HighPart = fa.ftLastWriteTime.dwHighDateTime;
      rec.mtime = mt.QuadPart;
      ULARGE_INTEGER ct{};
      ct.LowPart = fa.ftCreationTime.dwLowDateTime;
      ct.HighPart = fa.ftCreationTime.dwHighDateTime;
      rec.ctime = ct.QuadPart;
      ULARGE_INTEGER sz{};
      sz.LowPart = fa.nFileSizeLow;
      sz.HighPart = fa.nFileSizeHigh;
      rec.size = sz.QuadPart;
    }
    rec.tick = now;
    if (s_attr.size() > 1024) {
      s_attr.clear();
    }
    s_attr[path] = rec;
    return rec;
  };
  auto ext_of = [](const std::wstring& path) -> std::wstring {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos ||
        (slash != std::wstring::npos && dot < slash)) {
      return {};
    }
    std::wstring e = path.substr(dot);
    for (auto& c : e) {
      if (c >= L'A' && c <= L'Z') {
        c = static_cast<wchar_t>(c - L'A' + L'a');
      }
    }
    return e;
  };
  auto lower = [](std::wstring s) {
    for (auto& c : s) {
      if (c >= L'A' && c <= L'Z') {
        c = static_cast<wchar_t>(c - L'A' + L'a');
      }
    }
    return s;
  };
  if (sort_by_ == "date" || sort_by_ == "created") {
    // Hoist GetFileAttributesExW out of the comparator (was N× per comparison).
    const bool by_created = sort_by_ == "created";
    struct Rec {
      size_t idx;
      ULONGLONG ft;
      bool ok;
    };
    std::vector<Rec> recs;
    recs.reserve(cells_.size());
    for (size_t i = 0; i < cells_.size(); ++i) {
      const AttrRec a = lookup_attr(cells_[i].item.path);
      recs.push_back({i, by_created ? a.ctime : a.mtime, a.ok});
    }
    std::stable_sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
      if (!a.ok || !b.ok) {
        return a.ok && !b.ok;
      }
      return a.ft > b.ft;  // newest first (matches CompareFileTime > 0)
    });
    std::vector<Cell> ordered;
    ordered.reserve(cells_.size());
    for (const auto& r : recs) {
      ordered.push_back(std::move(cells_[r.idx]));
    }
    cells_ = std::move(ordered);
  } else if (sort_by_ == "size") {
    struct Rec {
      size_t idx;
      ULONGLONG size;
    };
    std::vector<Rec> recs;
    recs.reserve(cells_.size());
    for (size_t i = 0; i < cells_.size(); ++i) {
      recs.push_back({i, lookup_attr(cells_[i].item.path).size});
    }
    std::stable_sort(recs.begin(), recs.end(),
                     [](const Rec& a, const Rec& b) { return a.size > b.size; });
    std::vector<Cell> ordered;
    ordered.reserve(cells_.size());
    for (const auto& r : recs) {
      ordered.push_back(std::move(cells_[r.idx]));
    }
    cells_ = std::move(ordered);
  } else if (sort_by_ == "type") {
    std::stable_sort(cells_.begin(), cells_.end(),
                     [&](const Cell& a, const Cell& b) {
                       const std::wstring ea = ext_of(a.item.path);
                       const std::wstring eb = ext_of(b.item.path);
                       if (ea != eb) {
                         return ea < eb;
                       }
                       return lower(a.item.name) < lower(b.item.name);
                     });
  } else {
    std::stable_sort(cells_.begin(), cells_.end(),
                     [&](const Cell& a, const Cell& b) {
                       return lower(a.item.name) < lower(b.item.name);
                     });
  }
}

void FenceWindow::LayoutCells() {
  int w = 0;
  int h = 0;
  if (!ResolveLayerSize(&w, &h)) {
    return;
  }
  LayoutCellsTo(w, h);
}

void FenceWindow::ClampScrollForView(int view_h) {
  const int max_scroll = (std::max)(0, content_h_ - (std::max)(0, view_h));
  if (scroll_y_ < 0) {
    scroll_y_ = 0;
  }
  if (scroll_y_ > max_scroll) {
    scroll_y_ = max_scroll;
  }
}

void FenceWindow::ClampScroll() {
  int w = 0;
  int h = 0;
  if (!ResolveLayerSize(&w, &h)) {
    return;
  }
  const int view =
      (std::max)(0, h - S(hwnd_, kHeaderH0) - S(hwnd_, kPad0));
  ClampScrollForView(view);
}

void FenceWindow::LayoutCellsTo(int w, int h) {
  if (!hwnd_ || w <= 0 || h <= 0) {
    return;
  }
  RECT rc{0, 0, w, h};
  const int pad = S(hwnd_, kPad0);
  const int header = S(hwnd_, kHeaderH0);
  const int inner_l = rc.left + pad;
  const int inner_r = rc.right - pad;

  // Measure → clamp scroll → place (never place then clamp: that left bounds
  // at the old offset while scroll_y_ shrank, and Paint skipped every cell).
  int cell_w = 0;
  int cell_h = 0;
  int cols = 1;
  int row_h = 0;
  if (view_mode_ == "list") {
    row_h = S(hwnd_, kListRowH0);
    content_h_ = static_cast<int>(cells_.size()) * row_h;
  } else {
    cell_w = (std::max)(1, CellWidthPx());
    cell_h = (std::max)(1, CellHeightPx());
    cols = (std::max)(1, (inner_r - inner_l) / cell_w);
    const int rows =
        cells_.empty()
            ? 0
            : static_cast<int>((cells_.size() + static_cast<size_t>(cols) - 1) /
                               static_cast<size_t>(cols));
    content_h_ = rows * cell_h;
  }
  const int footer_h =
      (!collapsed_ && (inner_r - inner_l) >= S(hwnd_, 180) && !cells_.empty())
          ? S(hwnd_, 24)
          : 0;
  const int view = (std::max)(0, h - header - pad - footer_h);
  ClampScrollForView(view);

  const int inner_t = rc.top + header - scroll_y_;
  if (view_mode_ == "list") {
    const int width = (std::max)(40, inner_r - inner_l);
    for (size_t i = 0; i < cells_.size(); ++i) {
      RECT& b = cells_[i].bounds;
      b.left = inner_l;
      b.top = inner_t + static_cast<int>(i) * row_h;
      b.right = b.left + width;
      b.bottom = b.top + row_h;
    }
  } else {
    for (size_t i = 0; i < cells_.size(); ++i) {
      const int col = static_cast<int>(i % static_cast<size_t>(cols));
      const int row = static_cast<int>(i / static_cast<size_t>(cols));
      RECT& b = cells_[i].bounds;
      b.left = inner_l + col * cell_w;
      b.top = inner_t + row * cell_h;
      b.right = b.left + cell_w;
      b.bottom = b.top + cell_h;
    }
  }
}

bool FenceWindow::IconLabelRect(size_t index, RECT* out) const {
  if (!out || index >= cells_.size()) {
    return false;
  }
  const RECT& b = cells_[index].bounds;
  if (view_mode_ == "list") {
    // List: text starts after the glyph — approximate right of icon + pad.
    const int icon = IconSizePx();
    out->left = b.left + icon + S(hwnd_, 8);
    out->top = b.top;
    out->right = b.right;
    out->bottom = b.bottom;
    return out->right > out->left;
  }
  const int icon = IconSizePx();
  const int font_px = S(hwnd_, 11);
  const int line_h = font_px + S(hwnd_, 5);
  const int idle_label_h = line_h * label_lines_ + S(hwnd_, 4);
  const int iy = b.top + 4;
  const int label_top = iy + icon + 2;
  const int label_bottom = label_top + idle_label_h + S(hwnd_, 2);
  out->left = b.left + 2;
  out->top = label_top;
  out->right = b.right - 2;
  out->bottom = (std::min)(label_bottom, static_cast<int>(b.bottom) - 2);
  return out->bottom > out->top;
}

bool FenceWindow::IconContentRect(size_t index, RECT* out) const {
  if (!out || index >= cells_.size()) {
    return false;
  }
  // Empty seats take grid space but are not hittable (Fences sparse grid).
  if (cells_[index].item.path.empty()) {
    return false;
  }
  const RECT& b = cells_[index].bounds;
  if (view_mode_ == "list") {
    // List rows: whole row is the hit target (Explorer details-style).
    *out = b;
    return true;
  }
  const int icon = IconSizePx();
  const int font_px = S(hwnd_, 11);
  const int line_h = font_px + S(hwnd_, 5);
  const int idle_label_h = line_h * label_lines_ + S(hwnd_, 4);
  const int iy = b.top + 4;
  const int label_top = iy + icon + 2;
  // Glyph + idle caption shelf. Selected paint may grow taller, but
  // expanding the hit box to the cell bottom made "blank" clicks inside the
  // same cell re-hit the icon (rename) instead of clearing selection.
  const int label_bottom = label_top + idle_label_h + S(hwnd_, 2);
  out->left = b.left + 2;
  out->top = iy - 2;
  out->right = b.right - 2;
  out->bottom = (std::min)(label_bottom, static_cast<int>(b.bottom) - 2);
  return true;
}

int FenceWindow::HitTestIcon(int x, int y) const {
  if (collapsed_) {
    return -1;
  }
  if (y < S(hwnd_, kHeaderH0)) {
    return -1;
  }
  // Explorer / Fences: only glyph + caption are "icons". Cell padding and empty
  // panel chrome stay blank so a click clears selection (full-cell hit made
  // nearly every grid pixel an icon — blank clicks could not deselect).
  for (size_t i = 0; i < cells_.size(); ++i) {
    RECT content{};
    if (!IconContentRect(i, &content)) {
      continue;
    }
    if (x >= content.left && x < content.right && y >= content.top &&
        y < content.bottom) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int FenceWindow::GridColumnCount() const {
  if (!hwnd_ || view_mode_ == "list") {
    return 1;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int pad = S(hwnd_, kPad0);
  const int cell_w = CellWidthPx();
  const int inner = (std::max)(1, static_cast<int>(rc.right - rc.left) - pad * 2);
  return (std::max)(1, inner / cell_w);
}

bool FenceWindow::ScrollbarVisible() const {
  if (!hwnd_ || collapsed_) {
    return false;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int view = (std::max)(
      0, static_cast<int>(rc.bottom) - S(hwnd_, kHeaderH0) - S(hwnd_, kPad0));
  return content_h_ > view && view > 0;
}

int FenceWindow::HitTestScrollbar(int x, int y) const {
  if (!ScrollbarVisible()) {
    return 0;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int header = S(hwnd_, kHeaderH0);
  // Keep scrollbar clear of the east resize strip ([rc.right - kEdge, rc.right))
  const int edge = kEdge;
  if (x < rc.right - edge - 7 || x >= rc.right - edge) {
    return 0;
  }
  const int track_top = header + 2;
  const int track_h = (std::max)(8, static_cast<int>(rc.bottom) - track_top - 4);
  if (y < track_top || y > track_top + track_h) {
    return 0;
  }
  const int view = (std::max)(
      0, static_cast<int>(rc.bottom) - header - S(hwnd_, kPad0));
  const int thumb_h =
      (std::max)(16, track_h * view / (std::max)(1, content_h_));
  const int max_scroll = (std::max)(1, content_h_ - view);
  const int thumb_y = track_top + (track_h - thumb_h) * scroll_y_ / max_scroll;
  if (y >= thumb_y && y <= thumb_y + thumb_h) {
    return 1;  // thumb
  }
  return 2;  // track
}

void FenceWindow::ScrollbarPage(int dir) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int view = (std::max)(
      0, static_cast<int>(rc.bottom) - S(hwnd_, kHeaderH0) - S(hwnd_, kPad0));
  scroll_y_ += dir * view;
  ClampScroll();
  LayoutCells();
  PresentLayered();
}

void FenceWindow::PreviewStyleId(const std::string& style_id) {
  if (!style_preview_active_) {
    preview_style_id_ = style_id_;
    style_preview_active_ = true;
  }
  SetStyleId(style_id);
}

void FenceWindow::RevertStylePreview() {
  if (!style_preview_active_) {
    return;
  }
  style_preview_active_ = false;
  SetStyleId(preview_style_id_);
  preview_style_id_.clear();
}

void FenceWindow::ApplyRubberBandSelection() {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  if (!rubber_additive_) {
    std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
    selection_anchor_ = -1;
    selection_focus_ = -1;
  }
  const int l = (std::min)(rubber_a_.x, rubber_b_.x);
  const int t = (std::min)(rubber_a_.y, rubber_b_.y);
  const int r = (std::max)(rubber_a_.x, rubber_b_.x);
  const int b = (std::max)(rubber_a_.y, rubber_b_.y);
  // Intersect glyph+caption only. Full-cell marquee re-selected the icon under
  // a blank click after a few pixels of jitter (felt like deselect never worked).
  for (size_t i = 0; i < cells_.size(); ++i) {
    RECT content{};
    if (!IconContentRect(i, &content)) {
      continue;
    }
    const bool hit = !(content.right < l || content.left > r ||
                       content.bottom < t || content.top > b);
    if (hit) {
      selected_[i] = 1;
      // Ctrl+marquee: keep mark; only move focus to the last hit.
      if (selection_anchor_ < 0) {
        selection_anchor_ = static_cast<int>(i);
      }
      selection_focus_ = static_cast<int>(i);
    }
  }
}

void FenceWindow::OpenSelectedItems() {
  if (SelectedPaths().empty()) {
    return;
  }
  // Shift+Enter: open externally (portal folders → Explorer, not drill-in).
  // Ctrl+Enter: new Explorer window for folders (same as Ctrl+dblclick).
  const bool external = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool ctrl_new =
      !external && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
    if (!selected_[i] || cells_[i].item.path.empty()) {
      continue;
    }
    if (ctrl_new) {
      ExploreShellPath(cells_[i].item.path);
    } else {
      OpenItem(static_cast<int>(i), external);
    }
  }
}

void FenceWindow::NavigateSelection(int dx, int dy, bool extend,
                                    bool focus_only) {
  if (cells_.empty()) {
    return;
  }
  const int cols = GridColumnCount();
  const int n = static_cast<int>(cells_.size());
  auto occupied = [&](int i) {
    return i >= 0 && i < n && !cells_[static_cast<size_t>(i)].item.path.empty();
  };
  // Shift-extend moves the focus end; plain arrows move from focus/anchor.
  int cur = selection_focus_;
  if (cur < 0 || cur >= n || !occupied(cur)) {
    cur = selection_anchor_;
  }
  if (cur < 0 || cur >= n || !occupied(cur)) {
    cur = -1;
    for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
      if (selected_[i] && occupied(static_cast<int>(i))) {
        cur = static_cast<int>(i);
        break;
      }
    }
  }
  if (cur < 0) {
    // First occupied seat (sparse pin grids may start with empties).
    for (int i = 0; i < n; ++i) {
      if (occupied(i)) {
        if (focus_only) {
          selection_focus_ = i;
          if (selection_anchor_ < 0) {
            selection_anchor_ = i;
          }
          EnsureCellVisible(i);
          PresentLayered();
        } else {
          SelectOnly(i);
          EnsureCellVisible(i);
        }
        return;
      }
    }
    return;
  }
  // Step along the primary axis, skipping empty sparse seats (Explorer).
  const int step = dy * cols + dx;
  if (step == 0) {
    return;
  }
  int next = cur + step;
  while (next >= 0 && next < n && !occupied(next)) {
    next += step;
  }
  if (next < 0 || next >= n) {
    return;
  }
  if (focus_only) {
    // Explorer: Ctrl+Arrow moves focus without changing the selection set.
    selection_focus_ = next;
    EnsureCellVisible(next);
    PresentLayered();
    return;
  }
  if (extend) {
    if (selection_anchor_ < 0 || !occupied(selection_anchor_)) {
      selection_anchor_ = cur;
    }
    SelectRange(next);
  } else {
    SelectOnly(next);
  }
  EnsureCellVisible(next);
}

void FenceWindow::EnsureCellVisible(int index) {
  if (!hwnd_ || index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int header = S(hwnd_, kHeaderH0);
  const RECT& b = cells_[static_cast<size_t>(index)].bounds;
  bool moved = false;
  if (b.top < header) {
    scroll_y_ -= (header - b.top);
    moved = true;
  } else if (b.bottom > rc.bottom) {
    scroll_y_ += (b.bottom - rc.bottom);
    moved = true;
  }
  if (moved) {
    ClampScroll();
    LayoutCells();
    PresentLayered();
  }
}

void FenceWindow::TypeAheadFind(wchar_t ch) {
  if (!ch || all_items_.empty()) {
    return;
  }
  const DWORD now = GetTickCount();
  if (now - typeahead_tick_ > 1000) {
    typeahead_.clear();
  }
  typeahead_tick_ = now;
  typeahead_.push_back(ch);
  CharLowerBuffW(typeahead_.data() + (typeahead_.size() - 1), 1);
  // Type-to-find narrows the visible set (Fences quick filter) instead of only
  // moving the selection: with a 40-icon fence, jumping the anchor still left
  // the user hunting through the same wall of names.
  if (filter_edit_) {
    SetWindowTextW(filter_edit_, typeahead_.c_str());
    SendMessageW(filter_edit_, EM_SETSEL, typeahead_.size(),
                 typeahead_.size());
  }
  ApplyFilter(typeahead_);
  // No prefix match — restart from just this character (Explorer).
  if (typeahead_.size() > 1 && cells_.empty()) {
    typeahead_.assign(1, ch);
    CharLowerBuffW(typeahead_.data(), 1);
    if (filter_edit_) {
      SetWindowTextW(filter_edit_, typeahead_.c_str());
    }
    ApplyFilter(typeahead_);
  }
}

void FenceWindow::TypeAheadFindNext() {
  if (typeahead_.empty() || cells_.empty()) {
    return;
  }
  typeahead_tick_ = GetTickCount();
  auto lower_name = [](std::wstring s) {
    if (!s.empty()) {
      CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    }
    return s;
  };
  auto matches = [&](size_t i) {
    if (cells_[i].item.path.empty()) {
      return false;
    }
    const std::wstring n = lower_name(cells_[i].item.name);
    return n.size() >= typeahead_.size() &&
           n.compare(0, typeahead_.size(), typeahead_) == 0;
  };
  int start = selection_anchor_ + 1;
  if (start < 0 || start > static_cast<int>(cells_.size())) {
    start = 0;
  }
  for (int pass = 0; pass < 2; ++pass) {
    const int from = (pass == 0) ? start : 0;
    const int to = (pass == 0) ? static_cast<int>(cells_.size()) : start;
    for (int i = from; i < to; ++i) {
      if (matches(static_cast<size_t>(i))) {
        SelectOnly(i);
        EnsureCellVisible(i);
        PresentLayered();
        return;
      }
    }
  }
}

bool FenceWindow::TypeAheadBackspace() {
  if (typeahead_.empty()) {
    return false;
  }
  typeahead_.pop_back();
  typeahead_tick_ = GetTickCount();
  if (filter_edit_) {
    SetWindowTextW(filter_edit_, typeahead_.c_str());
    SendMessageW(filter_edit_, EM_SETSEL, typeahead_.size(),
                 typeahead_.size());
  }
  ApplyFilter(typeahead_);
  return true;
}

namespace {

std::wstring UniqueNewFolderPath(const std::wstring& parent) {
  if (parent.empty()) {
    return L"";
  }
  const LANGID lang = PRIMARYLANGID(GetUserDefaultUILanguage());
  const wchar_t* base =
      (lang == LANG_CHINESE) ? L"新建文件夹" : L"New folder";
  std::wstring path = parent + L"\\" + base;
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return path;
  }
  for (int i = 2; i < 10000; ++i) {
    path = parent + L"\\" + base + L" (" + std::to_wstring(i) + L")";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return path;
    }
  }
  return L"";
}

}  // namespace

void FenceWindow::CreateNewFolder() {
  if (rename_edit_) {
    return;
  }
  if (content_locked_) {
    ShowToast(L"分区内容已锁定，请先解锁");
    return;
  }
  const std::wstring parent =
      portal_path_.empty() ? DesktopDirectory() : PortalBrowsePath();
  const std::wstring path = UniqueNewFolderPath(parent);
  if (path.empty()) {
    return;
  }
  if (!CreateDirectoryW(path.c_str(), nullptr)) {
    return;
  }
  typeahead_.clear();
  if (portal_path_.empty()) {
    // Virtual fence: Desktop land → pin (Fences New / Ctrl+Shift+N).
    if (on_created_) {
      on_created_(this, {path});
    } else if (on_pasted_) {
      on_pasted_(this, {path}, /*cut=*/false, /*as_link=*/false);
    } else if (on_refresh_) {
      on_refresh_(this);
    }
  } else if (on_refresh_) {
    on_refresh_(this);
  }
  for (size_t i = 0; i < cells_.size(); ++i) {
    if (_wcsicmp(cells_[i].item.path.c_str(), path.c_str()) == 0) {
      SelectOnly(static_cast<int>(i));
      EnsureCellVisible(static_cast<int>(i));
      BeginInlineRename();
      return;
    }
  }
}

void FenceWindow::ShowSelectionContextMenu() {
  if (rename_edit_) {
    return;
  }
  int index = -1;
  if (selection_anchor_ >= 0 &&
      selection_anchor_ < static_cast<int>(cells_.size()) &&
      IsSelected(selection_anchor_)) {
    index = selection_anchor_;
  } else {
    for (size_t i = 0; i < cells_.size(); ++i) {
      if (IsSelected(static_cast<int>(i)) && !cells_[i].item.path.empty()) {
        index = static_cast<int>(i);
        break;
      }
    }
  }
  // Explorer: Apps / Shift+F10 with no selection → folder background menu.
  if (index < 0) {
    POINT pt{};
    GetCursorPos(&pt);
    ShowFenceMenu(pt);
    return;
  }
  POINT pt{};
  const RECT& b = cells_[static_cast<size_t>(index)].bounds;
  pt.x = (b.left + b.right) / 2;
  pt.y = (b.top + b.bottom) / 2;
  ClientToScreen(hwnd_, &pt);
  ShowIconMenu(index, pt);
}

void FenceWindow::HandleFenceHotkey(int vk, bool ctrl, bool shift) {
  if (rename_edit_) {
    return;
  }
  if (vk == 'F' && ctrl && !shift) {
    BeginFilter();
    return;
  }
  if (vk == VK_ESCAPE) {
    // Explorer: Esc first clears typeahead; then cancels drag/rubber; then selection.
    if (!typeahead_.empty()) {
      typeahead_.clear();
      return;
    }
    if (rubber_) {
      rubber_ = false;
      ReleaseCapture();
      PresentLayered();
      return;
    }
    if (icon_dragging_) {
      icon_dragging_ = false;
      drag_paths_.clear();
      drag_indices_.clear();
      drag_insert_ = -1;
      drag_left_ = false;
      last_manual_drag_present_ms_ = 0;
      if (GetCapture() == hwnd_) {
        ReleaseCapture();
      }
      PresentLayered();
      return;
    }
    ClearSelection();
    return;
  }
  if (vk == VK_BACK || vk == VK_BROWSER_BACK) {
    // Explorer: Backspace first edits typeahead; else portal goes up.
    if (vk == VK_BACK && TypeAheadBackspace()) {
      return;
    }
    // Fences portal: Backspace / mouse-back → up one folder level.
    if (PortalGoUp()) {
      typeahead_.clear();
      if (on_refresh_) {
        on_refresh_(this);
      }
    }
    return;
  }
  if (vk == VK_F3) {
    // Explorer: F3 → next typeahead match (same prefix).
    typeahead_tick_ = GetTickCount();
    TypeAheadFindNext();
    return;
  }
  // Explorer: Alt+↑ / Alt+← → up one folder (portal; do not steal plain arrows).
  if ((vk == VK_UP || vk == VK_LEFT) &&
      (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 && !ctrl && !shift) {
    if (PortalGoUp()) {
      typeahead_.clear();
      if (on_refresh_) {
        on_refresh_(this);
      }
    }
    return;
  }
  // Explorer / browser: Alt+→ / BrowserForward → portal forward history.
  if ((vk == VK_RIGHT || vk == VK_BROWSER_FORWARD) &&
      ((vk == VK_BROWSER_FORWARD) ||
       ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0 && !ctrl && !shift))) {
    if (PortalGoForward()) {
      typeahead_.clear();
      if (on_refresh_) {
        on_refresh_(this);
      }
    }
    return;
  }
  // Explorer / Fences: Ctrl+/- → icon zoom (same 15pp steps as wheel).
  if (ctrl && !shift &&
      (vk == VK_OEM_PLUS || vk == VK_ADD || vk == VK_OEM_MINUS ||
       vk == VK_SUBTRACT)) {
    typeahead_.clear();
    const int delta =
        (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) ? -15 : 15;
    SetIconZoom(icon_zoom_percent_ + delta);
    return;
  }
  // Explorer: Ctrl+0 / Ctrl+NumPad0 → reset icon zoom to 100%.
  if (ctrl && !shift && (vk == '0' || vk == VK_NUMPAD0)) {
    typeahead_.clear();
    SetIconZoom(100);
    return;
  }
  if (vk == VK_RETURN) {
    // Explorer / Fences: Alt+Enter → Properties; Ctrl+Enter → new window;
    // Shift+Enter → open externally (portal skips drill-in); Enter → open.
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
      typeahead_.clear();
      const auto paths = SelectedPaths();
      if (!paths.empty()) {
        ShowShellPathsProperties(hwnd_, paths);
      }
      return;
    }
    typeahead_.clear();
    OpenSelectedItems();
    return;
  }
  if (vk == VK_DELETE) {
    // Explorer: Del → recycle; Shift+Del → permanent.
    typeahead_.clear();
    DeleteSelection(shift);
    return;
  }
  if (vk == VK_F2) {
    typeahead_.clear();
    BeginInlineRename();
    return;
  }
  if (vk == VK_F5) {
    // Explorer / Fences: F5 refreshes the fence contents.
    typeahead_.clear();
    if (on_refresh_) {
      on_refresh_(this);
    }
    return;
  }
  if (vk == VK_SPACE) {
    // Plain Space selects focused/hovered. Ctrl+Space is Peek (default chord) —
    // fence_keys lets it pass through; do not steal here either.
    if (ctrl) {
      return;
    }
    typeahead_.clear();
    // Prefer keyboard focus (Ctrl+Arrow), then hover, then anchor.
    int idx = selection_focus_;
    if (idx < 0 || idx >= static_cast<int>(cells_.size()) ||
        cells_[static_cast<size_t>(idx)].item.path.empty()) {
      idx = tip_index_;
    }
    if (idx < 0 || idx >= static_cast<int>(cells_.size())) {
      idx = selection_anchor_;
    }
    if (idx < 0 || idx >= static_cast<int>(cells_.size())) {
      for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
        if (selected_[i]) {
          idx = static_cast<int>(i);
          break;
        }
      }
    }
    if (idx < 0 || idx >= static_cast<int>(cells_.size())) {
      return;
    }
    SelectOnly(idx);
    return;
  }
  if (ctrl && shift && vk == 'C') {
    // Explorer: Ctrl+Shift+C → copy path(s) as text.
    typeahead_.clear();
    CopySelectionPathsAsText();
    return;
  }
  if ((ctrl && vk == 'C') || (ctrl && !shift && vk == VK_INSERT)) {
    // Explorer: Ctrl+C / Ctrl+Insert → copy.
    typeahead_.clear();
    CopySelection();
    return;
  }
  if (ctrl && vk == 'X') {
    typeahead_.clear();
    CutSelection();
    return;
  }
  if ((ctrl && vk == 'V') || (shift && vk == VK_INSERT)) {
    // Explorer: Ctrl+V / Shift+Insert → paste; +Ctrl → 粘贴快捷方式.
    typeahead_.clear();
    PasteClipboardFiles(/*as_link=*/ctrl && shift);
    return;
  }
  if (ctrl && vk == 'A') {
    SelectAll();
    return;
  }
  if (ctrl && shift && vk == 'N') {
    // Explorer / Fences: Ctrl+Shift+N → new folder (virtual pin or portal cwd).
    CreateNewFolder();
    return;
  }
  if (ctrl && !shift && vk == 'Z') {
    // Explorer: Ctrl+Z undoes in the active folder (portal cwd vs Desktop).
    const std::wstring folder =
        portal_path_.empty() ? DesktopDirectory() : PortalBrowsePath();
    if (!folder.empty()) {
      InvokeShellFolderVerb(hwnd_, folder, L"undo");
    }
    if (portal_path_.empty() && on_undo_) {
      on_undo_();
    } else if (!portal_path_.empty() && on_refresh_) {
      on_refresh_(this);
    }
    return;
  }
  if (ctrl && !shift && vk == 'Y') {
    // Explorer: Ctrl+Y redo in the active folder.
    const std::wstring folder =
        portal_path_.empty() ? DesktopDirectory() : PortalBrowsePath();
    if (!folder.empty()) {
      InvokeShellFolderVerb(hwnd_, folder, L"redo");
    }
    if (on_refresh_) {
      on_refresh_(this);
    }
    return;
  }
  if (vk == VK_APPS || (shift && !ctrl && vk == VK_F10)) {
    ShowSelectionContextMenu();
    return;
  }
  if (vk == VK_HOME) {
    typeahead_.clear();
    if (cells_.empty()) {
      return;
    }
    int first = -1;
    for (int i = 0; i < static_cast<int>(cells_.size()); ++i) {
      if (!cells_[static_cast<size_t>(i)].item.path.empty()) {
        first = i;
        break;
      }
    }
    if (first < 0) {
      return;
    }
    if (shift) {
      if (selection_anchor_ < 0) {
        selection_anchor_ = first;
      }
      SelectRange(first);
    } else {
      SelectOnly(first);
    }
    EnsureCellVisible(first);
    return;
  }
  if (vk == VK_END) {
    typeahead_.clear();
    if (cells_.empty()) {
      return;
    }
    int last = -1;
    for (int i = static_cast<int>(cells_.size()) - 1; i >= 0; --i) {
      if (!cells_[static_cast<size_t>(i)].item.path.empty()) {
        last = i;
        break;
      }
    }
    if (last < 0) {
      return;
    }
    if (shift) {
      if (selection_anchor_ < 0) {
        selection_anchor_ = last;
      }
      SelectRange(last);
    } else {
      SelectOnly(last);
    }
    EnsureCellVisible(last);
    return;
  }
  if (vk == VK_PRIOR || vk == VK_NEXT) {
    // Explorer / Fences: Page Up/Down moves by roughly one viewport of rows.
    typeahead_.clear();
    if (cells_.empty() || !hwnd_) {
      return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int header = S(hwnd_, kHeaderH0);
    const int view_h = (std::max)(1, static_cast<int>(rc.bottom) - header);
    int cell_h = 48;
    if (!cells_.empty()) {
      const RECT& b = cells_[0].bounds;
      cell_h = (std::max)(1, static_cast<int>(b.bottom - b.top));
    }
    const int rows = (std::max)(1, view_h / cell_h);
    NavigateSelection(0, vk == VK_PRIOR ? -rows : rows, shift);
    return;
  }
  if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) {
    typeahead_.clear();
    // Explorer: Ctrl+Arrow moves focus only; Shift extends; plain selects.
    const bool focus_only = ctrl && !shift;
    if (vk == VK_LEFT) {
      NavigateSelection(-1, 0, shift, focus_only);
    } else if (vk == VK_RIGHT) {
      NavigateSelection(1, 0, shift, focus_only);
    } else if (vk == VK_UP) {
      NavigateSelection(0, -1, shift, focus_only);
    } else if (vk == VK_DOWN) {
      NavigateSelection(0, 1, shift, focus_only);
    }
  }
}

FenceWindow::Edge FenceWindow::HitTestEdge(int x, int y) const {
  if (position_locked_) {
    return Edge::None;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int header = S(hwnd_, kHeaderH0);
  // Title bar hosts lock / collapse / delete — never treat L/R as resize there
  // (otherwise the right-edge strip steals button clicks).
  if (y < header) {
    if (!collapsed_ && y <= kEdge) {
      return Edge::Top;
    }
    return Edge::None;
  }
  int e = 0;
  if (x <= kEdge) {
    e |= static_cast<int>(Edge::Left);
  }
  if (x >= rc.right - kEdge) {
    e |= static_cast<int>(Edge::Right);
  }
  // Collapsed: only horizontal resize (keep chrome height fixed).
  if (!collapsed_) {
    if (y <= kEdge) {
      e |= static_cast<int>(Edge::Top);
    }
    if (y >= rc.bottom - kEdge) {
      e |= static_cast<int>(Edge::Bottom);
    }
  }
  return static_cast<Edge>(e);
}

bool FenceWindow::IsSystemFence() const {
  return id_.rfind("system_", 0) == 0;
}

void FenceWindow::HeaderBtnRects(RECT* collapse, RECT* lock, RECT* dissolve,
                                RECT* portal_up) const {
  if (collapse) {
    *collapse = {};
  }
  if (lock) {
    *lock = {};
  }
  if (dissolve) {
    *dissolve = {};
  }
  if (portal_up) {
    *portal_up = {};
  }
  if (!hwnd_) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int header = S(hwnd_, kHeaderH0);
  const int btn = S(hwnd_, 22);
  const int gap = S(hwnd_, 2);
  int right = rc.right - 2;
  auto place = [&](RECT* out, bool enabled) {
    if (!out || !enabled) {
      return;
    }
    out->right = right;
    out->left = right - btn;
    out->top = 2;
    out->bottom = header - 2;
    right = out->left - gap;
  };
  // Right → left: collapse, lock, dissolve (delete).
  place(collapse, collapsible_);
  place(lock, true);
  place(dissolve, !IsSystemFence());
  // Fences Folder Portal: up chevron on the left when drilled below root.
  if (portal_up && PortalCanGoUp()) {
    portal_up->left = 2;
    portal_up->right = 2 + btn;
    portal_up->top = 2;
    portal_up->bottom = header - 2;
  }
}

FenceWindow::HeaderBtn FenceWindow::HitTestHeaderBtn(int x, int y) const {
  RECT collapse{}, lock{}, dissolve{}, portal_up{};
  HeaderBtnRects(&collapse, &lock, &dissolve, &portal_up);
  auto hit = [&](const RECT& r) {
    return r.right > r.left && x >= r.left && x < r.right && y >= r.top &&
           y < r.bottom;
  };
  if (hit(portal_up)) {
    return HeaderBtn::PortalUp;
  }
  if (hit(collapse)) {
    return HeaderBtn::Collapse;
  }
  if (hit(lock)) {
    return HeaderBtn::Lock;
  }
  if (hit(dissolve)) {
    return HeaderBtn::Dissolve;
  }
  return HeaderBtn::None;
}

void FenceWindow::ActivateHeaderBtn(HeaderBtn btn) {
  if (btn == HeaderBtn::Collapse) {
    ToggleCollapsed();
  } else if (btn == HeaderBtn::Lock) {
    SetPositionLocked(!position_locked_);
  } else if (btn == HeaderBtn::Dissolve) {
    if (on_dissolve_) {
      on_dissolve_(this);
    }
  } else if (btn == HeaderBtn::PortalUp) {
    if (PortalGoUp() && on_refresh_) {
      on_refresh_(this);
    }
  }
}

void FenceWindow::ResetPortalBrowseChrome() {
  // Explorer: entering a folder clears selection / typeahead and scrolls top.
  typeahead_.clear();
  scroll_y_ = 0;
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  } else {
    std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  }
  selection_anchor_ = -1;
  selection_focus_ = -1;
}

void FenceWindow::OpenItem(int index, bool external) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring& path = cells_[static_cast<size_t>(index)].item.path;
  if (path.empty()) {
    return;
  }
  // Explorer: Ctrl+dblclick folder → new Explorer window (even in portal).
  const bool ctrl_new =
      (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  if (ctrl_new && !external) {
    ExploreShellPath(path);
    return;
  }
  // Fences Folder Portal: double-click / Enter drills in; Shift+Enter opens
  // Explorer (external) like Explorer's open-in-new-window habit.
  if (!external && !portal_path_.empty()) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES &&
        (attr & FILE_ATTRIBUTE_DIRECTORY) &&
        !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
      if (NavigatePortalInto(path)) {
        if (on_refresh_) {
          on_refresh_(this);
        }
        return;
      }
    }
  }
  OpenShellPath(path);
}

void FenceWindow::RevealItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring& path = cells_[static_cast<size_t>(index)].item.path;
  if (path.empty()) {
    return;
  }
  const std::wstring arg = L"/select,\"" + path + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr,
                SW_SHOWNORMAL);
}

void FenceWindow::BeginIconDrag(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  if (content_locked_) {
    ShowToast(L"分区内容已锁定，请先解锁");
    return;
  }
  if (cells_[static_cast<size_t>(index)].item.path.empty()) {
    return;
  }
  // Portal fences mirror a real folder: keep the OLE file drag (physical move/
  // copy out). Plain fences pin virtual items → manual in-app drag (reorder /
  // drag-out unpin), matching Python's virtual semantics.
  if (!portal_path_.empty()) {
    std::vector<std::wstring> paths;
    if (IsSelected(index)) {
      paths = SelectedPaths();
    }
    if (paths.empty()) {
      paths.push_back(cells_[static_cast<size_t>(index)].item.path);
    }
    icon_press_ = false;
    icon_press_index_ = -1;
    ReleaseCapture();
    ShellDoDragFiles(paths);
    if (on_drag_done_) {
      on_drag_done_(this);
    }
    return;
  }
  StartManualIconDrag(index);
}

void FenceWindow::StartManualIconDrag(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  if (content_locked_) {
    return;
  }
  icon_press_ = false;
  icon_press_index_ = -1;
  drag_paths_.clear();
  drag_indices_.clear();
  if (IsSelected(index)) {
    for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
      if (selected_[i]) {
        drag_indices_.push_back(static_cast<int>(i));
        drag_paths_.push_back(cells_[i].item.path);
      }
    }
  }
  if (drag_indices_.empty()) {
    drag_indices_.push_back(index);
    drag_paths_.push_back(cells_[static_cast<size_t>(index)].item.path);
  }
  icon_dragging_ = true;
  drag_insert_ = -1;
  drag_left_ = false;
  drag_pt_ = {};
  last_manual_drag_present_ms_ = 0;
  {
    POINT cur{};
    if (GetCursorPos(&cur) && ScreenToClient(hwnd_, &cur)) {
      drag_pt_ = cur;
    }
  }
  SetCapture(hwnd_);
  UpdateHoverTip(-1);
  LogLine(L"[fence] manual drag start: %d items from %s",
          static_cast<int>(drag_paths_.size()), title_.c_str());
  PresentLayered();
  last_manual_drag_present_ms_ = GetTickCount();
}

void FenceWindow::UpdateManualIconDrag(int x, int y) {
  if (!icon_dragging_) {
    return;
  }
  const int prev_insert = drag_insert_;
  const bool prev_left = drag_left_;
  drag_pt_ = {x, y};
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const bool outside = x < 0 || y < 0 || x >= rc.right || y >= rc.bottom;
  if (outside && !drag_left_) {
    // P1-13: pointer left the fence mid-drag → hand off to the OLE pin-transfer
    // drag (cross-fence transfer / Explorer / Recycle Bin / desktop).
    drag_left_ = true;
    icon_dragging_ = false;
    std::vector<std::wstring> paths = std::move(drag_paths_);
    drag_paths_.clear();
    drag_indices_.clear();
    drag_insert_ = -1;
    last_manual_drag_present_ms_ = 0;
    ReleaseCapture();
    PresentLayered();
    CommitDragOutPins(std::move(paths));
    return;
  }
  drag_left_ = outside;
  // Explorer/Fences: dragging an icon to the top/bottom edge scrolls the fence
  // so it can be dropped at a seat outside the current viewport. The marquee
  // path already did this; icon drags could not reach a scrolled-out seat.
  bool scrolled = false;
  if (!outside) {
    const int header = S(hwnd_, kHeaderH0);
    const int margin = S(hwnd_, 28);
    const bool at_edge = y < header + margin || y > rc.bottom - margin;
    // Keep a ticker running while parked on the edge: a mouse that stops
    // moving must still keep scrolling, as it does in Explorer.
    if (at_edge) {
      SetTimer(hwnd_, kTimerDragScroll, 60, nullptr);
    } else {
      KillTimer(hwnd_, kTimerDragScroll);
    }
    scrolled = ScrollIfAtDragEdge(x, y);
  } else {
    KillTimer(hwnd_, kTimerDragScroll);
  }
  drag_insert_ = outside ? -1 : InsertIndexAt(x, y);
  const bool structural =
      drag_insert_ != prev_insert || drag_left_ != prev_left;
  const DWORD now = GetTickCount();
  if (structural || scrolled || last_manual_drag_present_ms_ == 0 ||
      now - last_manual_drag_present_ms_ >= 33u) {
    PresentLayered();
    last_manual_drag_present_ms_ = now;
  }
}

bool FenceWindow::ScrollIfAtDragEdge(int x, int y) {
  if (!icon_dragging_) {
    return false;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  if (x < 0 || y < 0 || x >= rc.right || y >= rc.bottom) {
    return false;
  }
  const int header = S(hwnd_, kHeaderH0);
  const int margin = S(hwnd_, 28);
  const int step = S(hwnd_, 24);
  const int old_scroll = scroll_y_;
  if (y < header + margin) {
    scroll_y_ -= step;
  } else if (y > rc.bottom - margin) {
    scroll_y_ += step;
  }
  if (scroll_y_ == old_scroll) {
    return false;
  }
  ClampScroll();
  // Cells move with the viewport; the drag cursor is fixed, so the insert
  // seat is recomputed by the caller against the new bounds.
  LayoutCells();
  return true;
}

void FenceWindow::FinishManualIconDrag(int x, int y) {
  if (!icon_dragging_) {
    return;
  }
  KillTimer(hwnd_, kTimerDragScroll);
  icon_dragging_ = false;
  last_manual_drag_present_ms_ = 0;
  ReleaseCapture();
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const bool outside =
      x < 0 || y < 0 || x >= rc.right || y >= rc.bottom;
  if (outside) {
    // P1-13: hand off to OLE pin-transfer (cross-fence / Explorer / apps).
    std::vector<std::wstring> paths = std::move(drag_paths_);
    drag_paths_.clear();
    drag_indices_.clear();
    drag_insert_ = -1;
    drag_left_ = false;
    PresentLayered();
    CommitDragOutPins(std::move(paths));
    return;
  }
  if (y >= S(hwnd_, kHeaderH0)) {
    // Dropped onto the Recycle Bin icon → recycle (delete) instead of reorder.
    if (IsRecycleBinIconAt(x, y)) {
      std::vector<std::wstring> paths = std::move(drag_paths_);
      drag_paths_.clear();
      drag_indices_.clear();
      drag_insert_ = -1;
      drag_left_ = false;
      PresentLayered();
      RecycleDroppedPaths(std::move(paths));
      return;
    }
    // Drop onto a folder icon → import into that folder (Explorer).
    // Self-only (drag folder onto itself) → fall through to reorder.
    const std::wstring folder = DirectoryIconAt(x, y);
    if (!folder.empty()) {
      std::vector<std::wstring> to_import;
      to_import.reserve(drag_paths_.size());
      for (const auto& p : drag_paths_) {
        if (p.empty() || _wcsicmp(p.c_str(), folder.c_str()) == 0) {
          continue;
        }
        if (IsStrictPathPrefix(p, folder)) {
          continue;  // folder→own child
        }
        to_import.push_back(p);
      }
      if (!to_import.empty()) {
        DWORD effect = DROPEFFECT_MOVE;
        if (WantDropLinkEffect()) {
          effect = DROPEFFECT_LINK;
        } else if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
                   (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
          effect = DROPEFFECT_COPY;
        }
        drag_paths_.clear();
        drag_indices_.clear();
        drag_insert_ = -1;
        drag_left_ = false;
        PresentLayered();
        ImportPathsIntoDirectory(to_import, folder, effect);
        return;
      }
    }
    const int at = InsertIndexAt(x, y);
    // Explorer: Ctrl+drag within view → copy; Alt / Ctrl+Shift → .lnk.
    // Portal lands in browse cwd; virtual lands on Desktop then pins at seat.
    const bool want_link = WantDropLinkEffect();
    const bool want_copy =
        !want_link && (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
        (GetKeyState(VK_SHIFT) & 0x8000) == 0;
    if ((want_link || want_copy) && !drag_paths_.empty()) {
      std::vector<std::wstring> paths = std::move(drag_paths_);
      drag_paths_.clear();
      drag_indices_.clear();
      drag_insert_ = -1;
      drag_left_ = false;
      PresentLayered();
      const std::wstring dest_dir =
          portal_path_.empty() ? DesktopDirectory() : PortalBrowsePath();
      if (dest_dir.empty()) {
        return;
      }
      std::vector<std::wstring> landed;
      landed.reserve(paths.size());
      for (const auto& src : paths) {
        if (src.empty()) {
          continue;
        }
        std::wstring dest;
        const bool ok =
            want_link
                ? CreateShortcutIntoDir(hwnd_, src, dest_dir, &dest)
                : MoveOrCopyFileIntoDir(hwnd_, src, dest_dir, /*move=*/false,
                                        &dest);
        if (ok && !dest.empty()) {
          landed.push_back(dest);
        }
      }
      if (landed.empty()) {
        ShowToast(want_link ? L"无法创建快捷方式" : L"无法复制");
        return;
      }
      if (portal_path_.empty()) {
        SetSortBy("manual");
        InsertPinOrder(landed, at);
        if (on_created_) {
          on_created_(this, landed);
        } else if (on_refresh_) {
          on_refresh_(this);
        }
      } else if (on_refresh_) {
        on_refresh_(this);
      }
      ShowToast((want_link ? L"已创建快捷方式 " : L"已复制 ") +
                std::to_wstring(landed.size()) + L" 项");
      return;
    }
    CommitReorder(at);
  }
  drag_paths_.clear();
  drag_indices_.clear();
  drag_insert_ = -1;
  drag_left_ = false;
  PresentLayered();
}

bool FenceWindow::IsRecycleBinIconAt(int x, int y) const {
  const int over = HitTestIcon(x, y);
  if (over < 0 || over >= static_cast<int>(cells_.size())) {
    return false;
  }
  const std::wstring clsid =
      GetLnkNamespaceClsid(cells_[static_cast<size_t>(over)].item.path);
  return clsid == L"{645FF040-5081-101B-9F08-00AA002F954E}";
}

std::wstring FenceWindow::DirectoryIconAt(int x, int y) const {
  if (IsRecycleBinIconAt(x, y)) {
    return L"";
  }
  const int over = HitTestIcon(x, y);
  if (over < 0 || over >= static_cast<int>(cells_.size())) {
    return L"";
  }
  const std::wstring& path = cells_[static_cast<size_t>(over)].item.path;
  if (path.empty()) {
    return L"";
  }
  const DWORD attr = GetFileAttributesW(path.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    return L"";
  }
  return path;
}

DWORD FenceWindow::ImportPathsIntoDirectory(
    const std::vector<std::wstring>& paths, const std::wstring& dest_dir,
    DWORD proposed) {
  if (paths.empty() || dest_dir.empty()) {
    return DROPEFFECT_NONE;
  }
  const bool want_link =
      (proposed & DROPEFFECT_LINK) != 0 || WantDropLinkEffect();
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  int ok = 0;
  int moved = 0;
  std::vector<std::wstring> moved_paths;
  moved_paths.reserve(paths.size());
  for (const auto& src : paths) {
    if (src.empty()) {
      continue;
    }
    // Don't import a folder into itself or into its own descendant.
    if (_wcsicmp(src.c_str(), dest_dir.c_str()) == 0 ||
        IsStrictPathPrefix(src, dest_dir)) {
      continue;
    }
    std::wstring dest;
    if (want_link) {
      if (CreateShortcutIntoDir(hwnd_, src, dest_dir, &dest) && !dest.empty()) {
        ++ok;
      }
      continue;
    }
    // Explorer: Ctrl=COPY, Shift=MOVE, else same-volume MOVE / cross COPY.
    bool move = SameVolume(src, dest_dir);
    if (ctrl && !shift) {
      move = false;
    } else if (shift && !ctrl) {
      move = true;
    } else if ((proposed & DROPEFFECT_MOVE) != 0 &&
               (proposed & DROPEFFECT_COPY) == 0) {
      move = true;
    } else if ((proposed & DROPEFFECT_COPY) != 0 &&
               (proposed & DROPEFFECT_MOVE) == 0) {
      move = false;
    }
    if (MoveOrCopyFileIntoDir(hwnd_, src, dest_dir, move, &dest) &&
        !dest.empty()) {
      ++ok;
      if (move) {
        ++moved;
        moved_paths.push_back(src);
      }
    }
  }
  if (ok <= 0) {
    return DROPEFFECT_NONE;
  }
  // File left the Desktop/fence seat — drop pin immediately (no sticky ghost).
  if (!moved_paths.empty()) {
    if (on_unpin_many_) {
      on_unpin_many_(this, moved_paths, nullptr);
    } else {
      for (const auto& p : moved_paths) {
        if (on_unpin_) {
          on_unpin_(this, p, nullptr);
        }
      }
    }
  }
  if (on_refresh_) {
    on_refresh_(this);
  } else if (on_drag_done_) {
    on_drag_done_(this);
  }
  if (want_link) {
    ShowToast(L"已创建快捷方式 " + std::to_wstring(ok) + L" 项");
    return DROPEFFECT_LINK;
  }
  ShowToast(L"已放入文件夹 " + std::to_wstring(ok) + L" 项");
  return (moved == ok) ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
}

bool FenceWindow::RecycleDroppedPaths(std::vector<std::wstring> paths) {
  if (paths.empty()) {
    return false;
  }
  const bool ok = RecycleShellPaths(hwnd_, paths) == IconFsResult::Ok;
  if (ok) {
    // Drop may include paths not pinned here (desktop float / other fence);
    // on_unpin_ ClearPins globally and RemovePinOrder is a no-op when absent.
    RemovePinOrder(paths);
    for (const auto& p : paths) {
      if (on_unpin_) {
        // File is gone — no float seat.
        on_unpin_(this, p, nullptr);
      }
    }
  }
  if (on_pin_drag_end_) {
    on_pin_drag_end_(this);
  }
  if (on_drag_done_) {
    on_drag_done_(this);
  }
  return ok;
}

int FenceWindow::InsertIndexAt(int x, int y) const {
  if (cells_.empty()) {
    return 0;
  }
  // Map to the nearest cell midpoint: before/after by grid axis (Python
  // _insert_index_at uses the cell midpoint along the dominant axis).
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& b = cells_[i].bounds;
    if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
      if (view_mode_ == "list") {
        return static_cast<int>(i) +
               (y >= (b.top + b.bottom) / 2 ? 1 : 0);
      }
      return static_cast<int>(i) +
             (x >= (b.left + b.right) / 2 ? 1 : 0);
    }
  }
  // Empty gutter: nearest cell edge (prefer same row in grid).
  int best = static_cast<int>(cells_.size());
  long long best_d = LLONG_MAX;
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& b = cells_[i].bounds;
    const int anchors_x[2] = {b.left, b.right};
    const int anchor_iy[2] = {static_cast<int>(i), static_cast<int>(i) + 1};
    for (int k = 0; k < 2; ++k) {
      const long long dx = static_cast<long long>(x - anchors_x[k]);
      const long long dy = static_cast<long long>(y - ((b.top + b.bottom) / 2));
      const long long d = (view_mode_ == "list") ? dy * dy + dx * dx * 9
                                                  : dx * dx + dy * dy * 9;
      if (d < best_d) {
        best_d = d;
        best = anchor_iy[k];
      }
    }
  }
  return best;
}

void FenceWindow::CommitReorder(int insert_index) {
  if (drag_indices_.empty() || cells_.empty()) {
    return;
  }
  // Extract dragged cells (in index order), remove them, then re-insert as a
  // contiguous block at the adjusted position.
  std::vector<Cell> dragged;
  dragged.reserve(drag_indices_.size());
  for (int idx : drag_indices_) {
    if (idx >= 0 && idx < static_cast<int>(cells_.size())) {
      dragged.push_back(std::move(cells_[static_cast<size_t>(idx)]));
    }
  }
  if (dragged.empty()) {
    return;
  }
  // Remove in descending order to keep indices valid.
  std::vector<int> sorted = drag_indices_;
  std::sort(sorted.begin(), sorted.end(), std::greater<int>());
  for (int idx : sorted) {
    if (idx >= 0 && idx < static_cast<int>(cells_.size())) {
      cells_.erase(cells_.begin() + idx);
    }
  }
  // Adjust insert position for the removed block (all before insert shift it).
  int at = insert_index;
  for (int idx : sorted) {
    if (idx < at) {
      --at;
    }
  }
  at = (std::min)(at, static_cast<int>(cells_.size()));
  at = (std::max)(0, at);
  cells_.insert(cells_.begin() + at, std::make_move_iterator(dragged.begin()),
                std::make_move_iterator(dragged.end()));
  selected_.assign(cells_.size(), 0);
  selection_anchor_ = -1;
  selection_focus_ = -1;
  sort_by_ = "manual";  // manual order now reflects the new arrangement
  LayoutCells();
  if (on_moved_) {
    on_moved_(this);  // persist sort_by=manual (pin order needs app-side storage)
  }
  if (on_reorder_) {
    std::vector<std::wstring> order;
    order.reserve(cells_.size());
    for (const auto& c : cells_) {
      order.push_back(c.item.path);
    }
    on_reorder_(this, order);
  }
  LogLine(L"[fence] reorder committed: %d items -> insert %d",
          static_cast<int>(dragged.size()), at);
}

void FenceWindow::CommitDragOutPins(std::vector<std::wstring> paths) {
  // P1-13 / Fences-class: only unpin when the pin truly left the fence —
  // desktop/public release, or OLE MOVE (Recycle / move-to-folder). Sharing
  // to WeChat (COPY) or Esc cancel must keep the pin (old code unpinned on
  // every non-fence drop → icons reappeared on the desktop).
  PinDragResult result = PinDragResult::KeptInFence;
  if (!paths.empty()) {
    result = ShellDoDragPins(paths, Widen(id_));
    LogLine(L"[fence] drag-out done paths=%d result=%d",
            static_cast<int>(paths.size()), static_cast<int>(result));
    if (result == PinDragResult::Unpinned) {
      RemovePinOrder(paths);
      POINT drop{};
      GetCursorPos(&drop);
      // Fences-class: cascade from the drop seat — same POINT for every path
      // used to clamp into one left-edge stack after multi-unpin.
      if (on_unpin_many_) {
        on_unpin_many_(this, paths, &drop);
      } else if (on_unpin_) {
        for (const auto& p : paths) {
          on_unpin_(this, p, &drop);
        }
      }
    }
  }
  if (on_pin_drag_end_) {
    on_pin_drag_end_(this);
  }
  if (on_drag_done_) {
    on_drag_done_(this);
  }
  // KeptInFence only: OLE can leave a blank ULW while cells remain, and
  // SetItems same_seats skips Present. Skip after Unpinned — on_drag_done
  // already refreshed membership; an extra blit flashed the fence.
  if (result == PinDragResult::KeptInFence && hwnd_ &&
      IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    PresentLayered();
  }
}

void FenceWindow::RenameItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  // Fences / Explorer: context Rename → inline label EDIT (dialog only if
  // BeginInlineRename cannot start — e.g. multi-select / missing cell).
  SelectOnly(index);
  BeginInlineRename();
}

void FenceWindow::RecycleItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring path = cells_[static_cast<size_t>(index)].item.path;
  if (RecycleShellPath(hwnd_, path) == IconFsResult::Ok && on_unpin_) {
    on_unpin_(this, path, nullptr);
  }
}

void FenceWindow::ShowItemProperties(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  ShowShellPathProperties(hwnd_, cells_[static_cast<size_t>(index)].item.path);
}

void FenceWindow::ClearSelection() {
  // AIGC START
  // Blank-desktop clicks clear every fence; skip Present when nothing was
  // selected — otherwise each click re-blits layered chrome (visible flash).
  bool any = false;
  for (char c : selected_) {
    if (c) {
      any = true;
      break;
    }
  }
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  } else {
    std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  }
  selection_anchor_ = -1;
  selection_focus_ = -1;
  if (any && hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    PresentLayered();
  }
  // AIGC END
}

void FenceWindow::SelectOnly(int index) {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  selection_anchor_ = -1;
  selection_focus_ = -1;
  if (index >= 0 && index < static_cast<int>(selected_.size()) &&
      index < static_cast<int>(cells_.size()) &&
      !cells_[static_cast<size_t>(index)].item.path.empty()) {
    selected_[static_cast<size_t>(index)] = 1;
    selection_anchor_ = index;
    selection_focus_ = index;
  }
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::ToggleSelected(int index) {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  if (index < 0 || index >= static_cast<int>(selected_.size()) ||
      index >= static_cast<int>(cells_.size()) ||
      cells_[static_cast<size_t>(index)].item.path.empty()) {
    return;
  }
  selected_[static_cast<size_t>(index)] =
      selected_[static_cast<size_t>(index)] ? 0 : 1;
  // Ctrl+click: move focus only; keep selection mark for later Shift-extend.
  selection_focus_ = index;
  if (selection_anchor_ < 0) {
    selection_anchor_ = index;
  }
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::SelectRange(int index) {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  if (index < 0 || index >= static_cast<int>(selected_.size())) {
    return;
  }
  int a = selection_anchor_;
  if (a < 0 || a >= static_cast<int>(selected_.size())) {
    a = index;
    selection_anchor_ = index;
  }
  const int lo = (std::min)(a, index);
  const int hi = (std::max)(a, index);
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  for (int i = lo; i <= hi; ++i) {
    if (i < static_cast<int>(cells_.size()) &&
        !cells_[static_cast<size_t>(i)].item.path.empty()) {
      selected_[static_cast<size_t>(i)] = 1;
    }
  }
  selection_focus_ = index;
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::SelectPaths(const std::vector<std::wstring>& paths) {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  } else {
    std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  }
  selection_anchor_ = -1;
  selection_focus_ = -1;
  for (const auto& want : paths) {
    if (want.empty()) {
      continue;
    }
    for (size_t i = 0; i < cells_.size(); ++i) {
      if (!cells_[i].item.path.empty() &&
          _wcsicmp(cells_[i].item.path.c_str(), want.c_str()) == 0) {
        selected_[i] = 1;
        if (selection_anchor_ < 0) {
          selection_anchor_ = static_cast<int>(i);
        }
        selection_focus_ = static_cast<int>(i);
        break;
      }
    }
  }
  if (selection_focus_ >= 0) {
    EnsureCellVisible(selection_focus_);
  }
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::CopySelectionPathsAsText() {
  const auto paths = SelectedPaths();
  if (paths.empty()) {
    return;
  }
  std::wstring text;
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i) {
      text += L"\r\n";
    }
    text += paths[i];
  }
  CopyShellPathText(hwnd_, text);
}

bool FenceWindow::IsSelected(int index) const {
  return index >= 0 && index < static_cast<int>(selected_.size()) &&
         selected_[static_cast<size_t>(index)] != 0;
}

std::vector<std::wstring> FenceWindow::SelectedPaths() const {
  std::vector<std::wstring> paths;
  const size_t n = (std::min)(selected_.size(), cells_.size());
  for (size_t i = 0; i < n; ++i) {
    if (selected_[i] && !cells_[i].item.path.empty()) {
      paths.push_back(cells_[i].item.path);
    }
  }
  return paths;
}

void FenceWindow::ApplySelectionCommand(int cmd, int clicked) {
  if (clicked < 0 || clicked >= static_cast<int>(cells_.size())) {
    return;
  }
  std::vector<std::wstring> paths;
  if (IsSelected(clicked)) {
    paths = SelectedPaths();
  }
  if (paths.empty()) {
    paths.push_back(cells_[static_cast<size_t>(clicked)].item.path);
  }
  if (cmd == 3) {
    // Menu「移出分区」: auto-pack floats (Fences); do not seat at menu cursor.
    for (const auto& p : paths) {
      if (on_unpin_) {
        on_unpin_(this, p, nullptr);
      }
    }
    return;
  }
  if (cmd == 4 || cmd == 5) {
    if (ClipboardSetFiles(hwnd_, paths, cmd == 5)) {
      ShowToast(cmd == 5 ? L"已剪切到剪贴板" : L"已复制到剪贴板");
      PresentLayered();
    }
    return;
  }
  if (cmd == 6) {
    std::wstring text;
    for (size_t i = 0; i < paths.size(); ++i) {
      if (i) {
        text += L"\r\n";
      }
      text += paths[i];
    }
    CopyShellPathText(hwnd_, text);
    return;
  }
  if (cmd == 9 && RecycleShellPaths(hwnd_, paths) == IconFsResult::Ok) {
    for (const auto& p : paths) {
      if (on_unpin_) {
        on_unpin_(this, p, nullptr);
      }
    }
  }
}

void FenceWindow::ShowIconMenu(int index, POINT screen) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  if (!IsSelected(index)) {
    SelectOnly(index);
  }
  auto paths = SelectedPaths();
  if (paths.empty()) {
    paths.push_back(cells_[static_cast<size_t>(index)].item.path);
  }
  bool whitelisted = false;
  if (!paths.empty()) {
    whitelisted = IsWhitelisted(paths[0], LoadWhitelist());
  }
  // Single entry「移动到分页」→ page → fence (not two flat move items).
  const std::vector<ShellMenuExtra> extras{
      {Tr("menu_unpin"), kExtUnpin},
      {whitelisted ? Tr("wl_remove_item") : Tr("wl_add_item"), kExtWhitelist},
      {Tr("menu_move_page"), kExtMovePage},
      {Tr("notes_open_in_notepad"), kExtOpenNotes},
  };
  // Fences: IContextMenu + quiet cancel; rescan only for mutating verbs.
  // P1-11: snapshot namespace CLSIDs only immediately before InvokeCommand.
  bool rename_picked = false;
  bool verb_ran = false;
  bool needs_rescan = false;
  std::wstring last_verb;
  std::vector<std::wstring> ns_clsids;
  std::vector<std::wstring> desk_before;
  auto list_desk = []() {
    std::vector<std::wstring> out;
    const std::wstring desk = DesktopDirectory();
    if (desk.empty()) {
      return out;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((desk + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
      return out;
    }
    do {
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
        continue;
      }
      out.push_back(desk + L"\\" + fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return out;
  };
  ShellBeforeInvokeFn on_before = [&]() {
    ns_clsids.clear();
    ns_clsids.reserve(paths.size());
    for (const auto& p : paths) {
      ns_clsids.push_back(GetLnkNamespaceClsid(p));
    }
    // Virtual fence: snapshot Desktop so Create shortcut can pin the new .lnk.
    if (portal_path_.empty()) {
      desk_before = list_desk();
    }
  };
  ShellVerbFn on_verb = [&](const std::wstring& verb) {
    if (verb == L"rename") {
      rename_picked = true;
      return;
    }
    // Content lock freezes the icon set: any verb that would rescan the
    // desktop (delete / paste / createlink / move / undo…) is a content
    // mutation, so it must not run while locked. Same predicate the caller
    // already uses to decide whether a rescan is needed.
    if (content_locked_ && ShellVerbNeedsDesktopRescan(verb)) {
      ShowToast(L"分区内容已锁定，请先解锁");
      verb_ran = false;
      last_verb.clear();
      return;
    }
    verb_ran = true;
    last_verb = verb;
    needs_rescan = ShellVerbNeedsDesktopRescan(verb);
  };
  const int shell_cmd = TrackShellContextMenu(hwnd_, screen, paths, extras,
                                              on_verb, on_before);
  if (shell_cmd == kExtUnpin) {
    ApplySelectionCommand(3, index);
    return;
  }
  if (shell_cmd == kExtWhitelist) {
    if (on_whitelist_) {
      on_whitelist_(this, paths, !whitelisted);
    }
    return;
  }
  if (shell_cmd == kExtMovePage) {
    ShowMoveToPageFenceMenu(screen);
    return;
  }
  if (shell_cmd == kExtOpenNotes) {
    if (on_open_notes_) {
      on_open_notes_(this, paths);
    }
    return;
  }
  if (shell_cmd == 0) {
    if (rename_picked) {
      BeginInlineRename();
      return;
    }
    if (!verb_ran) {
      return;
    }
    // Drop pins for vanished paths (delete); restore hosted namespace icons.
    bool any_gone = false;
    for (size_t i = 0; i < paths.size(); ++i) {
      const auto& p = paths[i];
      if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
        continue;
      }
      any_gone = true;
      if (i < ns_clsids.size() && !ns_clsids[i].empty()) {
        ReturnNamespaceIconToDesktop(ns_clsids[i]);
      }
      if (on_unpin_) {
        on_unpin_(this, p, nullptr);
      }
    }
    // Explorer 「创建快捷方式」on Desktop: pin the new .lnk into this fence
    // (same New→pin path as blank-menu newfolder).
    if (portal_path_.empty() &&
        (last_verb == L"link" || last_verb == L"createlink") &&
        (on_created_ || on_pasted_)) {
      const auto after = list_desk();
      std::vector<std::wstring> neu;
      for (const auto& p : after) {
        bool was = false;
        for (const auto& b : desk_before) {
          if (_wcsicmp(b.c_str(), p.c_str()) == 0) {
            was = true;
            break;
          }
        }
        if (!was) {
          neu.push_back(p);
        }
      }
      if (!neu.empty()) {
        if (on_created_) {
          on_created_(this, neu);
        } else {
          on_pasted_(this, neu, /*cut=*/false, /*as_link=*/false);
        }
        return;
      }
    }
    if ((needs_rescan || any_gone) && on_drag_done_) {
      on_drag_done_(this);
    } else if (last_verb == L"cut" || last_verb == L"copy") {
      // Cut ghost / selection chrome only — no full desktop rebuild.
      PresentLayered();
    }
    return;
  }
  // Fallback hand-rolled menu when IContextMenu unavailable.
  const int cmd = TrackIconShellMenu(hwnd_, screen, true);
  if (cmd == 1) {
    OpenItem(index);
  } else if (cmd == 2) {
    RevealItem(index);
  } else if (cmd == 3 || cmd == 4 || cmd == 5 || cmd == 6 || cmd == 9) {
    ApplySelectionCommand(cmd, index);
  } else if (cmd == 7) {
    RenameItem(index);
  } else if (cmd == 8) {
    ShowItemProperties(index);
  }
}

void FenceWindow::ShowMoveToPageFenceMenu(POINT screen) {
  const auto paths = SelectedPaths();
  if (paths.empty() || !on_move_to_fence_) {
    return;
  }
  std::vector<MoveFenceMenuTarget> targets;
  if (on_query_fences_) {
    for (auto& t : on_query_fences_()) {
      MoveFenceMenuTarget m;
      m.id = std::move(t.id);
      m.title = std::move(t.title);
      m.pages = std::move(t.pages);
      targets.push_back(std::move(m));
    }
  }
  const std::string fence_id = TrackMoveToPageThenFenceMenu(
      hwnd_, screen, page_names_, targets, /*exclude_fence_id=*/id_);
  if (!fence_id.empty()) {
    on_move_to_fence_(this, paths, fence_id);
  }
}

void FenceWindow::SetPortalPath(const std::wstring& path) {
  portal_path_ = path;
  portal_cwd_.clear();
  portal_forward_.clear();
}

const std::wstring& FenceWindow::PortalBrowsePath() const {
  if (!portal_cwd_.empty()) {
    return portal_cwd_;
  }
  return portal_path_;
}

bool FenceWindow::PortalCanGoUp() const {
  if (portal_path_.empty() || portal_cwd_.empty()) {
    return false;
  }
  // Still below root when cwd is a strict child of portal_path_.
  if (portal_cwd_.size() <= portal_path_.size()) {
    return false;
  }
  return _wcsnicmp(portal_cwd_.c_str(), portal_path_.c_str(),
                   portal_path_.size()) == 0;
}

bool FenceWindow::PortalCanGoForward() const {
  return !portal_path_.empty() && !portal_forward_.empty();
}

bool FenceWindow::NavigatePortalInto(const std::wstring& folder) {
  if (portal_path_.empty() || folder.empty()) {
    return false;
  }
  const DWORD attr = GetFileAttributesW(folder.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES ||
      !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    return false;
  }
  // Must stay under the portal root (Fences portal sandbox).
  std::wstring root = portal_path_;
  while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) {
    root.pop_back();
  }
  std::wstring next = folder;
  while (!next.empty() && (next.back() == L'\\' || next.back() == L'/')) {
    next.pop_back();
  }
  if (_wcsicmp(next.c_str(), root.c_str()) != 0 &&
      (next.size() <= root.size() ||
       _wcsnicmp(next.c_str(), root.c_str(), root.size()) != 0 ||
       (next[root.size()] != L'\\' && next[root.size()] != L'/'))) {
    return false;
  }
  // New drill-in clears forward history (browser / Explorer).
  portal_forward_.clear();
  if (_wcsicmp(next.c_str(), root.c_str()) == 0) {
    portal_cwd_.clear();
  } else {
    portal_cwd_ = next;
  }
  ResetPortalBrowseChrome();
  if (hwnd_) {
    SetWindowTextW(hwnd_, DisplayTitle().c_str());
    // Caption + portal-up chrome change before items rescan lands.
    PresentLayered();
  }
  return true;
}

bool FenceWindow::PortalGoUp() {
  if (!PortalCanGoUp()) {
    return false;
  }
  std::wstring cur = portal_cwd_;
  while (!cur.empty() && (cur.back() == L'\\' || cur.back() == L'/')) {
    cur.pop_back();
  }
  // Remember the folder we leave so Alt+→ / XBUTTON2 can restore it.
  if (!cur.empty()) {
    portal_forward_.push_back(cur);
  }
  const size_t slash = cur.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    portal_cwd_.clear();
    ResetPortalBrowseChrome();
    if (hwnd_) {
      SetWindowTextW(hwnd_, DisplayTitle().c_str());
      PresentLayered();
    }
    return true;
  }
  std::wstring parent = cur.substr(0, slash);
  std::wstring root = portal_path_;
  while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) {
    root.pop_back();
  }
  if (_wcsicmp(parent.c_str(), root.c_str()) == 0 || parent.size() < root.size()) {
    portal_cwd_.clear();
  } else {
    portal_cwd_ = parent;
  }
  ResetPortalBrowseChrome();
  if (hwnd_) {
    SetWindowTextW(hwnd_, DisplayTitle().c_str());
    PresentLayered();
  }
  return true;
}

bool FenceWindow::PortalGoForward() {
  if (!PortalCanGoForward()) {
    return false;
  }
  std::wstring next = portal_forward_.back();
  portal_forward_.pop_back();
  while (!next.empty() && (next.back() == L'\\' || next.back() == L'/')) {
    next.pop_back();
  }
  if (next.empty()) {
    return false;
  }
  const DWORD attr = GetFileAttributesW(next.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES ||
      !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    return false;
  }
  std::wstring root = portal_path_;
  while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) {
    root.pop_back();
  }
  if (_wcsicmp(next.c_str(), root.c_str()) == 0) {
    portal_cwd_.clear();
  } else if (next.size() > root.size() &&
             _wcsnicmp(next.c_str(), root.c_str(), root.size()) == 0 &&
             (next[root.size()] == L'\\' || next[root.size()] == L'/')) {
    portal_cwd_ = next;
  } else {
    return false;
  }
  ResetPortalBrowseChrome();
  if (hwnd_) {
    SetWindowTextW(hwnd_, DisplayTitle().c_str());
    PresentLayered();
  }
  return true;
}

void FenceWindow::CopySelection() {
  const auto paths = SelectedPaths();
  if (!paths.empty() && ClipboardSetFiles(hwnd_, paths, false)) {
    ShowToast(L"已复制到剪贴板");
    // Clear cut-ghost dimming after Preferred DropEffect flips to copy.
    PresentLayered();
  }
}

void FenceWindow::CutSelection() {
  const auto paths = SelectedPaths();
  if (!paths.empty() && ClipboardSetFiles(hwnd_, paths, true)) {
    ShowToast(L"已剪切到剪贴板");
    PresentLayered();
  }
}

void FenceWindow::DeleteSelection(bool permanent) {
  const auto paths = SelectedPaths();
  if (paths.empty()) {
    return;
  }
  if (content_locked_) {
    ShowToast(L"分区内容已锁定，请先解锁");
    return;
  }
  // Explorer: after delete, keep keyboard focus on the next surviving icon.
  int first_sel = -1;
  for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
    if (selected_[i] && !cells_[i].item.path.empty()) {
      first_sel = static_cast<int>(i);
      break;
    }
  }
  std::wstring next_path;
  if (first_sel >= 0) {
    for (int i = first_sel + 1; i < static_cast<int>(cells_.size()); ++i) {
      if (i < static_cast<int>(selected_.size()) && selected_[static_cast<size_t>(i)]) {
        continue;
      }
      if (!cells_[static_cast<size_t>(i)].item.path.empty()) {
        next_path = cells_[static_cast<size_t>(i)].item.path;
        break;
      }
    }
    if (next_path.empty()) {
      for (int i = first_sel - 1; i >= 0; --i) {
        if (i < static_cast<int>(selected_.size()) &&
            selected_[static_cast<size_t>(i)]) {
          continue;
        }
        if (!cells_[static_cast<size_t>(i)].item.path.empty()) {
          next_path = cells_[static_cast<size_t>(i)].item.path;
          break;
        }
      }
    }
  }
  if (RecycleShellPaths(hwnd_, paths, permanent) != IconFsResult::Ok) {
    return;
  }
  for (const auto& p : paths) {
    if (on_unpin_) {
      on_unpin_(this, p, nullptr);
    }
  }
  if (!next_path.empty()) {
    for (size_t i = 0; i < cells_.size(); ++i) {
      if (_wcsicmp(cells_[i].item.path.c_str(), next_path.c_str()) == 0) {
        SelectOnly(static_cast<int>(i));
        EnsureCellVisible(static_cast<int>(i));
        PresentLayered();
        break;
      }
    }
  }
}

void FenceWindow::SelectAll() {
  typeahead_.clear();
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  selection_anchor_ = -1;
  selection_focus_ = -1;
  for (size_t i = 0; i < cells_.size(); ++i) {
    if (cells_[i].item.path.empty()) {
      selected_[i] = 0;
      continue;
    }
    selected_[i] = 1;
    if (selection_anchor_ < 0) {
      selection_anchor_ = static_cast<int>(i);
    }
    selection_focus_ = static_cast<int>(i);
  }
  PresentLayered();
}

std::wstring FenceWindow::SelectedSingleDirectory() const {
  std::wstring dir;
  int n = 0;
  const size_t lim = (std::min)(selected_.size(), cells_.size());
  for (size_t i = 0; i < lim; ++i) {
    if (!selected_[i] || cells_[i].item.path.empty()) {
      continue;
    }
    const DWORD attr = GetFileAttributesW(cells_[i].item.path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES ||
        !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
      return L"";
    }
    dir = cells_[i].item.path;
    ++n;
    if (n > 1) {
      return L"";
    }
  }
  return n == 1 ? dir : L"";
}

namespace {
constexpr int kRenameEdit = 77;

bool RenameEditImeComposing(HWND hwnd) {
  HIMC himc = ImmGetContext(hwnd);
  if (!himc) {
    return false;
  }
  const LONG bytes =
      ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
  ImmReleaseContext(hwnd, himc);
  return bytes > 0;
}

LRESULT CALLBACK FilterEditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                LPARAM lparam) {
  auto* self =
      reinterpret_cast<FenceWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  auto call_old = [&]() -> LRESULT {
    WNDPROC old = reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"oldproc"));
    if (old) {
      return CallWindowProcW(old, hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  };
  if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) {
    // Esc abandons the filter entirely and restores the full set.
    if (self) {
      self->EndFilter();
    }
    return 0;
  }
  if (msg == WM_KEYDOWN && wparam == VK_RETURN) {
    // Enter keeps the narrowed result but hands the keyboard back to the
    // desktop — a popup EDIT that kept focus swallowed every later key.
    if (self) {
      self->CloseFilterEdit();
    }
    return 0;
  }
  if (msg == WM_CHAR && wparam == VK_ESCAPE) {
    return 0;
  }
  const LRESULT r = call_old();
  if (msg == WM_CHAR && self) {
    wchar_t buf[260] = {};
    GetWindowTextW(hwnd, buf, 260);
    self->ApplyFilter(buf);
  }
  return r;
}

LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                LPARAM lparam) {
  auto* self = reinterpret_cast<FenceWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_KEYDOWN && wparam == VK_TAB) {
    // Explorer: Tab / Shift+Tab commits and renames next / previous.
    if (self && !RenameEditImeComposing(hwnd)) {
      const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      self->CommitRenameAndAdvance(back ? -1 : 1);
    }
    return 0;
  }
  if (msg == WM_KEYDOWN && wparam == VK_F2) {
    // Explorer: second F2 toggles stem selection ↔ full name.
    wchar_t buf[520] = {};
    GetWindowTextW(hwnd, buf, 520);
    const int len = static_cast<int>(wcslen(buf));
    int stem_end = len;
    const wchar_t* dot = wcsrchr(buf, L'.');
    if (dot && dot > buf) {
      stem_end = static_cast<int>(dot - buf);
    }
    DWORD start = 0;
    DWORD end = 0;
    SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&start),
                 reinterpret_cast<LPARAM>(&end));
    if (start == 0 && static_cast<int>(end) == len) {
      SendMessageW(hwnd, EM_SETSEL, 0, stem_end);
    } else {
      SendMessageW(hwnd, EM_SETSEL, 0, -1);
    }
    return 0;
  }
  if (msg == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE)) {
    // Explorer/IME: Enter while composing only commits the candidate —
    // do not finish rename yet (stealing Enter left caret dead / number-only).
    if (wparam == VK_RETURN && RenameEditImeComposing(hwnd)) {
      WNDPROC old = reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"oldproc"));
      if (old) {
        return CallWindowProcW(old, hwnd, msg, wparam, lparam);
      }
      return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (self) {
      self->FinishInlineRename(wparam == VK_RETURN);
    }
    return 0;
  }
  // Explorer: leaving the edit commits (Esc already handled above).
  if (msg == WM_KILLFOCUS) {
    if (self) {
      self->FinishInlineRename(true);
    }
    return 0;
  }
  WNDPROC old = reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"oldproc"));
  if (old) {
    return CallWindowProcW(old, hwnd, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}  // namespace

void FenceWindow::CommitRenameAndAdvance(int delta) {
  const int from = rename_index_;
  FinishInlineRename(true);
  if (from < 0 || cells_.empty() || delta == 0) {
    return;
  }
  const int n = static_cast<int>(cells_.size());
  for (int step = 1; step <= n; ++step) {
    int i = (from + delta * step) % n;
    if (i < 0) {
      i += n;
    }
    if (cells_[static_cast<size_t>(i)].item.path.empty()) {
      continue;
    }
    SelectOnly(i);
    EnsureCellVisible(i);
    BeginInlineRename();
    return;
  }
}

void FenceWindow::BeginInlineRename() {
  if (content_locked_) {
    ShowToast(L"分区内容已锁定，请先解锁");
    return;
  }
  // Explorer / Fences: F2 renames the anchor (or first selected) even when
  // multiple icons are selected — never a silent no-op.
  int index = -1;
  if (selection_anchor_ >= 0 &&
      selection_anchor_ < static_cast<int>(selected_.size()) &&
      selection_anchor_ < static_cast<int>(cells_.size()) &&
      selected_[static_cast<size_t>(selection_anchor_)]) {
    index = selection_anchor_;
  } else {
    for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
      if (selected_[i]) {
        index = static_cast<int>(i);
        break;
      }
    }
  }
  if (index < 0 || cells_[static_cast<size_t>(index)].item.path.empty()) {
    return;
  }
  SelectOnly(index);
  if (rename_edit_) {
    DestroyWindow(rename_edit_);
    rename_edit_ = nullptr;
  }
  rename_index_ = index;
  RECT label{};
  if (!IconLabelRect(static_cast<size_t>(index), &label)) {
    const RECT& b = cells_[static_cast<size_t>(index)].bounds;
    label = b;
  }
  POINT origin{label.left, label.top};
  ClientToScreen(hwnd_, &origin);
  const int edit_w =
      (std::max)(S(hwnd_, 72), static_cast<int>(label.right - label.left));
  const int edit_h = (std::max)(S(hwnd_, 22), S(hwnd_, 20));
  // Popup (not WS_CHILD): UpdateLayeredWindow hosts hide child EDIT controls.
  rename_edit_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"EDIT",
      cells_[static_cast<size_t>(index)].item.name.c_str(),
      WS_POPUP | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, origin.x, origin.y,
      edit_w, edit_h, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!rename_edit_) {
    return;
  }
  rename_proc_ = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(rename_edit_, GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(RenameEditProc)));
  SetPropW(rename_edit_, L"oldproc", reinterpret_cast<HANDLE>(rename_proc_));
  SetWindowLongPtrW(rename_edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  // Explorer: select stem, leave extension unselected for files.
  const std::wstring& name = cells_[static_cast<size_t>(index)].item.name;
  const std::wstring& path = cells_[static_cast<size_t>(index)].item.path;
  const DWORD attr = GetFileAttributesW(path.c_str());
  const bool is_dir =
      attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
  int sel_end = -1;
  if (!is_dir) {
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
      sel_end = static_cast<int>(dot);
    }
  }
  SendMessageW(rename_edit_, EM_SETSEL, 0, sel_end);
  SetFocus(rename_edit_);
}

void FenceWindow::BeginTitleInlineRename() {
  if (!hwnd_ || !show_title_) {
    return;
  }
  if (rename_edit_) {
    DestroyWindow(rename_edit_);
    rename_edit_ = nullptr;
  }
  // -2 = title rename (distinct from icon cell index).
  rename_index_ = -2;
  RECT label{};
  if (!TitleLabelRect(&label)) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    label = {S(hwnd_, 6), S(hwnd_, 6), rc.right - S(hwnd_, 80),
             S(hwnd_, kHeaderH0) - 2};
  }
  POINT origin{label.left, label.top};
  ClientToScreen(hwnd_, &origin);
  const int edit_w =
      (std::max)(S(hwnd_, 96), static_cast<int>(label.right - label.left));
  const int edit_h =
      (std::max)(S(hwnd_, 22), static_cast<int>(label.bottom - label.top));
  rename_edit_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"EDIT", title_.c_str(),
      WS_POPUP | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, origin.x, origin.y,
      edit_w, edit_h, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!rename_edit_) {
    rename_index_ = -1;
    return;
  }
  rename_proc_ = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(rename_edit_, GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(RenameEditProc)));
  SetPropW(rename_edit_, L"oldproc", reinterpret_cast<HANDLE>(rename_proc_));
  SetWindowLongPtrW(rename_edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  SendMessageW(rename_edit_, EM_SETSEL, 0, -1);
  SetFocus(rename_edit_);
}

void FenceWindow::FinishInlineRename(bool commit) {
  if (!rename_edit_ || rename_finishing_) {
    return;
  }
  rename_finishing_ = true;
  HWND edit = rename_edit_;
  wchar_t buf[260] = {};
  GetWindowTextW(edit, buf, 260);
  const int index = rename_index_;
  auto close_edit = [&]() {
    rename_edit_ = nullptr;
    rename_index_ = -1;
    DestroyWindow(edit);
  };
  if (index == -2) {
    if (!commit) {
      close_edit();
      rename_finishing_ = false;
      return;
    }
    std::wstring neu = buf;
    while (!neu.empty() && (neu.back() == L' ' || neu.back() == L'\t')) {
      neu.pop_back();
    }
    size_t start = 0;
    while (start < neu.size() && (neu[start] == L' ' || neu[start] == L'\t')) {
      ++start;
    }
    neu = neu.substr(start);
    close_edit();
    rename_finishing_ = false;
    if (neu.empty() || neu == title_) {
      return;
    }
    SetTitle(neu);
    if (on_rename_) {
      on_rename_(this);
    }
    PresentLayered();
    return;
  }
  if (!commit || index < 0 || index >= static_cast<int>(cells_.size())) {
    close_edit();
    rename_finishing_ = false;
    return;
  }
  const std::wstring old_path = cells_[static_cast<size_t>(index)].item.path;
  const std::wstring neu = buf;
  const RenamePlan plan = PlanRename(old_path, neu);
  // Explorer: invalid/duplicate name keeps the edit box open.
  if (!plan.valid) {
    if (!neu.empty()) {
      ShowToast(plan.error);
    }
    rename_finishing_ = false;
    SetFocus(edit);
    return;
  }
  if (GetFileAttributesW(plan.dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
    ShowToast(L"该名称已存在");
    rename_finishing_ = false;
    SetFocus(edit);
    return;
  }
  close_edit();
  rename_finishing_ = false;
  if (!MoveFileW(old_path.c_str(), plan.dest.c_str())) {
    const DWORD err = GetLastError();
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
      ShowToast(L"文件正在使用中，请先关闭占用程序后重试");
    } else {
      ShowToast(L"无法重命名，文件可能被占用");
    }
    return;
  }
  SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW, old_path.c_str(),
                 plan.dest.c_str());
  if (on_path_replaced_) {
    on_path_replaced_(old_path, plan.dest);
  }
}

void FenceWindow::PasteClipboardFiles(bool as_link) {
  if (content_locked_) {
    ShowToast(L"分区内容已锁定，请先解锁");
    return;
  }
  std::vector<std::wstring> paths;
  bool cut = false;
  if (!ClipboardGetFilesWithEffect(&paths, &cut) || paths.empty()) {
    ShowToast(L"剪贴板里没有文件");
    return;
  }
  if (on_pasted_) {
    // pastelink ignores Preferred DropEffect MOVE (source stays put).
    on_pasted_(this, paths, as_link ? false : cut, as_link);
  }
}

void FenceWindow::ShowFenceMenu(POINT screen) {
  // Host Explorer's folder-background menu with DeskTidy cascading extras
  // (Sort / View / Opacity / Pages / Style / Portal) — Fences blank-menu parity.
  if (hwnd_) {
    KillTimer(hwnd_, kTimerAutoHide);
  }
  if (page_names_.empty()) {
    page_names_ = {L"工作", L"文档"};
  }
  const auto all_styles = AllFenceStyles();

  auto leaf = [](const std::wstring& label, int id, bool checked = false) {
    ShellMenuExtra ex;
    ex.label = label;
    ex.id = id;
    ex.checked = checked;
    return ex;
  };
  auto submenu = [](const std::wstring& label,
                    std::vector<ShellMenuExtra> kids) {
    ShellMenuExtra ex;
    ex.label = label;
    ex.children = std::move(kids);
    return ex;
  };

  std::vector<ShellMenuExtra> sort_kids = {
      leaf(L"按名称", kExtFenceSortName, sort_by_ == "name"),
      leaf(L"按修改日期", kExtFenceSortDate, sort_by_ == "date"),
      leaf(L"按创建日期", kExtFenceSortCreated, sort_by_ == "created"),
      leaf(L"按大小", kExtFenceSortSize, sort_by_ == "size"),
      leaf(L"按类型", kExtFenceSortType, sort_by_ == "type"),
      leaf(L"手动", kExtFenceSortManual, sort_by_ == "manual"),
  };
  // Fences: per-fence grid pitch. The desktop's own metrics stay the default;
  // these scale this fence's cells so a dense fence and a sparse one can coexist.
  std::vector<ShellMenuExtra> spacing_kids = {
      leaf(L"紧凑 85%", kExtFenceSpacingCompact, cell_scale_ == 85),
      leaf(L"标准 100%", kExtFenceSpacingNormal, cell_scale_ == 100),
      leaf(L"宽松 125%", kExtFenceSpacingWide, cell_scale_ == 125),
      leaf(L"超宽 150%", kExtFenceSpacingXl, cell_scale_ == 150),
  };
  std::vector<ShellMenuExtra> view_kids = {
      leaf(Tr("view_grid"), kExtFenceViewGrid, view_mode_ != "list"),
      leaf(Tr("view_list"), kExtFenceViewList, view_mode_ == "list"),
  };
  auto opa_on = [this](int alpha) {
    return (alpha < 0 && opacity_override_ < 0) ||
           (alpha >= 0 && opacity_override_ == alpha);
  };
  std::vector<ShellMenuExtra> opa_kids = {
      leaf(Tr("opacity_follow"), kExtFenceOpaFollow, opa_on(-1)),
      leaf(L"40%", kExtFenceOpa40, opa_on(102)),
      leaf(L"55%", kExtFenceOpa55, opa_on(140)),
      leaf(L"70%", kExtFenceOpa70, opa_on(178)),
      leaf(L"85%", kExtFenceOpa85, opa_on(217)),
      leaf(Tr("opacity_opaque"), kExtFenceOpaOpaque, opa_on(255)),
  };
  std::vector<ShellMenuExtra> page_kids;
  for (size_t i = 0; i < page_names_.size() && i < 12; ++i) {
    page_kids.push_back(leaf(page_names_[i],
                             kExtFencePageBase + static_cast<int>(i),
                             OnPage(static_cast<int>(i))));
  }
  std::vector<ShellMenuExtra> style_kids;
  for (size_t i = 0; i < all_styles.size() && i < 8; ++i) {
    style_kids.push_back(leaf(all_styles[i]->name,
                              kExtFenceStyleBase + static_cast<int>(i),
                              style_id_ == all_styles[i]->id));
  }

  std::vector<ShellMenuExtra> extras{
      leaf(Tr("fence_rename"), kExtFenceRename),
      leaf(collapsed_ ? Tr("fence_expand") : Tr("fence_collapse"),
           kExtFenceCollapse),
      leaf(L"离开自动折叠", kExtFenceAutoHide, auto_hide_),
      leaf(L"离开时淡化", kExtFenceFadeIdle, fade_on_idle_),
      leaf(Tr("fence_refresh"), kExtFenceRefresh),
      leaf(L"筛选图标…", kExtFenceFilter),
      leaf(Tr("fence_hide"), kExtFenceHide),
      leaf(position_locked_ ? Tr("fence_unlock") : Tr("fence_lock"),
           kExtFenceLock),
      leaf(content_locked_ ? Tr("fence_unlock_content")
                           : Tr("fence_lock_content"),
           kExtFenceLockContent),
      leaf(Tr("fence_dissolve"), kExtFenceDissolve),
      leaf(portal_path_.empty() ? L"设为门户文件夹…" : L"更换门户文件夹…",
           kExtFencePortalSet),
  };
  if (!portal_path_.empty()) {
    extras.push_back(leaf(L"取消门户", kExtFencePortalClear));
  }
  if (PortalCanGoUp()) {
    extras.push_back(leaf(L"上一级", kExtFencePortalUp));
  }
  extras.push_back(submenu(L"所在页面", std::move(page_kids)));
  extras.push_back(submenu(Tr("fence_sort"), std::move(sort_kids)));
  extras.push_back(submenu(L"图标间距", std::move(spacing_kids)));
  extras.push_back(leaf(L"显示标题栏", kExtFenceShowTitle, show_title_));
  extras.push_back(submenu(Tr("fence_view"), std::move(view_kids)));
  extras.push_back(submenu(Tr("fence_opacity"), std::move(opa_kids)));
  extras.push_back(submenu(Tr("fence_style"), std::move(style_kids)));

  // Fences: portal blank menu owns the mirrored folder; virtual fence Paste
  // pins (Ctrl+V path) instead of dumping onto the Desktop as a float.
  const std::wstring menu_folder =
      portal_path_.empty() ? DesktopDirectory() : PortalBrowsePath();
  std::vector<std::wstring> desk_before;
  auto list_desk = []() {
    std::vector<std::wstring> out;
    const std::wstring desk = DesktopDirectory();
    if (desk.empty()) {
      return out;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((desk + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
      return out;
    }
    do {
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
        continue;
      }
      out.push_back(desk + L"\\" + fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return out;
  };
  ShellBeforeInvokeFn on_before = [&]() {
    if (portal_path_.empty()) {
      desk_before = list_desk();
    }
  };
  ShellInterceptFn on_intercept = [this](const std::wstring& verb) {
    // Portal: let Explorer paste/pastelink into the mirrored folder.
    if (!portal_path_.empty()) {
      return false;
    }
    if (verb == L"paste" || verb == L"pasteto") {
      PasteClipboardFiles(/*as_link=*/false);
      return true;
    }
    if (verb == L"pastelink") {
      PasteClipboardFiles(/*as_link=*/true);
      return true;
    }
    return false;
  };
  ShellVerbFn on_verb = [this, &desk_before, &list_desk](const std::wstring& verb) {
    if (portal_path_.empty() &&
        (verb == L"paste" || verb == L"pastelink" || verb == L"pasteto")) {
      // Already handled by intercept (pin paste / paste-shortcut).
      return;
    }
    if (verb == L"undo" && on_undo_) {
      // Shell already undid; App re-pins remembered fence seats.
      on_undo_();
    }
    if (!ShellVerbNeedsDesktopRescan(verb)) {
      return;
    }
    // Virtual fence New → land on Desktop then pin as-is (not Copy→Paste FO_COPY).
    if (portal_path_.empty() &&
        (verb == L"newfolder" || verb.rfind(L"new", 0) == 0) &&
        (on_created_ || on_pasted_)) {
      const auto after = list_desk();
      std::vector<std::wstring> neu;
      for (const auto& p : after) {
        bool was = false;
        for (const auto& b : desk_before) {
          if (_wcsicmp(b.c_str(), p.c_str()) == 0) {
            was = true;
            break;
          }
        }
        if (!was) {
          neu.push_back(p);
        }
      }
      if (!neu.empty()) {
        if (on_created_) {
          on_created_(this, neu);
        } else {
          on_pasted_(this, neu, /*cut=*/false, /*as_link=*/false);
        }
        return;
      }
    }
    if (on_refresh_) {
      on_refresh_(this);
    }
  };
  const int rc = TrackShellFolderBackgroundMenu(
      hwnd_, screen, menu_folder, extras, on_verb, on_before, on_intercept);
  if (rc >= 0) {
    auto apply_opacity = [this](int alpha) {
      SetOpacity(alpha);
      if (on_style_changed_) {
        on_style_changed_(this);
      }
    };
    if (rc == kExtFenceRefresh) {
      if (on_refresh_) {
        on_refresh_(this);
      }
    } else if (rc == kExtFenceHide) {
      Hide();
    } else if (rc == kExtFenceLock) {
      SetPositionLocked(!position_locked_);
    } else if (rc == kExtFenceLockContent) {
      SetContentLocked(!content_locked_);
    } else if (rc == kExtFenceDissolve) {
      if (on_dissolve_) {
        on_dissolve_(this);
      }
    } else if (rc == kExtFenceRename) {
      BeginTitleInlineRename();
    } else if (rc == kExtFenceCollapse) {
      SetCollapsed(!collapsed_);
    } else if (rc == kExtFenceAutoHide) {
      SetAutoHide(!auto_hide_);
    } else if (rc == kExtFenceFadeIdle) {
      SetFadeOnIdle(!fade_on_idle_);
    } else if (rc == kExtFencePortalUp) {
      if (PortalGoUp() && on_refresh_) {
        on_refresh_(this);
      }
    } else if (rc == kExtFenceSortName) {
      SetSortBy("name");
    } else if (rc == kExtFenceSortDate) {
      SetSortBy("date");
    } else if (rc == kExtFenceSortSize) {
      SetSortBy("size");
    } else if (rc == kExtFenceSortType) {
      SetSortBy("type");
    } else if (rc == kExtFenceSortManual) {
      SetSortBy("manual");
    } else if (rc == kExtFenceSortCreated) {
      SetSortBy("created");
    } else if (rc == kExtFenceSpacingCompact ||
               rc == kExtFenceSpacingNormal ||
               rc == kExtFenceSpacingWide ||
               rc == kExtFenceSpacingXl) {
      static const int kScale[] = {85, 100, 125, 150};
      SetCellScale(kScale[rc - kExtFenceSpacingCompact]);
    } else if (rc == kExtFenceShowTitle) {
      SetShowTitle(!show_title_);
    } else if (rc == kExtFenceViewGrid) {
      SetViewMode("grid");
    } else if (rc == kExtFenceViewList) {
      SetViewMode("list");
    } else if (rc == kExtFenceFilter) {
      BeginFilter();
    } else if (rc == kExtFenceOpaFollow) {
      apply_opacity(-1);
    } else if (rc == kExtFenceOpa40) {
      apply_opacity(102);
    } else if (rc == kExtFenceOpa55) {
      apply_opacity(140);
    } else if (rc == kExtFenceOpa70) {
      apply_opacity(178);
    } else if (rc == kExtFenceOpa85) {
      apply_opacity(217);
    } else if (rc == kExtFenceOpaOpaque) {
      apply_opacity(255);
    } else if (rc == kExtFencePortalClear) {
      portal_path_.clear();
      portal_cwd_.clear();
      if (on_portal_) {
        on_portal_(this, L"");
      }
    } else if (rc == kExtFencePortalSet) {
      BROWSEINFOW bi{};
      bi.hwndOwner = hwnd_;
      bi.lpszTitle = L"选择要镜像的文件夹";
      bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
      PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
      if (pidl) {
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, path)) {
          SetPortalPath(path);
          if (on_portal_) {
            on_portal_(this, portal_path_);
          }
        }
        CoTaskMemFree(pidl);
      }
    } else if (rc >= kExtFencePageBase &&
               rc < kExtFencePageBase + static_cast<int>(page_names_.size()) &&
               rc < kExtFencePageBase + 12) {
      TogglePageMembership(rc - kExtFencePageBase);
    } else if (rc >= kExtFenceStyleBase &&
               rc < kExtFenceStyleBase + static_cast<int>(all_styles.size()) &&
               rc < kExtFenceStyleBase + 8) {
      SetStyleId(all_styles[static_cast<size_t>(rc - kExtFenceStyleBase)]->id);
      if (on_style_changed_) {
        on_style_changed_(this);
      }
    }
    return;
  }
  // Hosted menu unavailable — fall back to the hand-rolled fence menu.
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 10, Tr("fence_rename").c_str());
  AppendMenuW(menu, MF_STRING, 11,
              collapsed_ ? Tr("fence_expand").c_str()
                         : Tr("fence_collapse").c_str());
  AppendMenuW(menu,
              MF_STRING | (auto_hide_ ? MF_CHECKED : 0), 16, L"离开自动折叠");
  AppendMenuW(menu, MF_STRING, 12,
              position_locked_ ? Tr("fence_unlock").c_str()
                               : Tr("fence_lock").c_str());
  AppendMenuW(menu, MF_STRING, 13, Tr("fence_paste").c_str());
  AppendMenuW(menu, MF_STRING, 14,
              portal_path_.empty() ? L"设为门户文件夹…" : L"更换门户文件夹…");
  if (!portal_path_.empty()) {
    AppendMenuW(menu, MF_STRING, 15, L"取消门户");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU pages = CreatePopupMenu();
  if (page_names_.empty()) {
    page_names_ = {L"工作", L"文档"};
  }
  for (size_t i = 0; i < page_names_.size() && i < 12; ++i) {
    const UINT flags =
        MF_STRING | (OnPage(static_cast<int>(i)) ? MF_CHECKED : 0);
    AppendMenuW(pages, flags, 400 + static_cast<UINT>(i), page_names_[i].c_str());
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pages), L"所在页面");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU sortm = CreatePopupMenu();
  auto sort_item = [&](UINT id, const char* key, const wchar_t* label) {
    const UINT flags =
        MF_STRING | (sort_by_ == key ? MF_CHECKED : 0);
    AppendMenuW(sortm, flags, id, label);
  };
  sort_item(500, "name", L"按名称");
  sort_item(501, "date", L"按修改日期");
  sort_item(502, "size", L"按大小");
  sort_item(503, "type", L"按类型");
  sort_item(504, "manual", L"手动");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortm),
              Tr("fence_sort").c_str());
  HMENU viewm = CreatePopupMenu();
  AppendMenuW(viewm, MF_STRING | (view_mode_ != "list" ? MF_CHECKED : 0), 510,
              Tr("view_grid").c_str());
  AppendMenuW(viewm, MF_STRING | (view_mode_ == "list" ? MF_CHECKED : 0), 511,
              Tr("view_list").c_str());
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(viewm),
              Tr("fence_view").c_str());
  HMENU opam = CreatePopupMenu();
  auto opa_item = [&](UINT id, int alpha, const wchar_t* label) {
    const bool on = (alpha < 0 && opacity_override_ < 0) ||
                    (alpha >= 0 && opacity_override_ == alpha);
    AppendMenuW(opam, MF_STRING | (on ? MF_CHECKED : 0), id, label);
  };
  opa_item(520, -1, Tr("opacity_follow").c_str());
  opa_item(521, 102, L"40%");
  opa_item(522, 140, L"55%");
  opa_item(523, 178, L"70%");
  opa_item(524, 217, L"85%");
  opa_item(525, 255, Tr("opacity_opaque").c_str());
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(opam),
              Tr("fence_opacity").c_str());
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU styles = CreatePopupMenu();
  const auto all = AllFenceStyles();
  for (size_t i = 0; i < all.size(); ++i) {
    const UINT flags =
        MF_STRING | (style_id_ == all[i]->id ? MF_CHECKED : 0);
    AppendMenuW(styles, flags, 100 + static_cast<UINT>(i), all[i]->name);
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(styles),
              Tr("fence_style").c_str());
  // Do not SetForegroundWindow(hwnd_) — lifts the fence over apps. Fallback
  // menu tracks against the fence hwnd for messages only (no FG steal).
  // F8: remember original style so hover preview can revert on cancel.
  preview_style_id_ = style_id_;
  style_preview_active_ = false;
  style_menu_open_ = true;
  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                     screen.x, screen.y, 0, hwnd_, nullptr);
  style_menu_open_ = false;
  DestroyMenu(menu);
  if (cmd == 0) {
    RevertStylePreview();
    return;
  }
  if (cmd == 10) {
    BeginTitleInlineRename();
  } else if (cmd == 11) {
    ToggleCollapsed();
  } else if (cmd == 16) {
    SetAutoHide(!auto_hide_);
  } else if (cmd == 12) {
    SetPositionLocked(!position_locked_);
  } else if (cmd == 13) {
    PasteClipboardFiles();
  } else if (cmd == 500) {
    SetSortBy("name");
  } else if (cmd == 501) {
    SetSortBy("date");
  } else if (cmd == 502) {
    SetSortBy("size");
  } else if (cmd == 503) {
    SetSortBy("type");
  } else if (cmd == 504) {
    SetSortBy("manual");
  } else if (cmd == 510) {
    SetViewMode("grid");
  } else if (cmd == 511) {
    SetViewMode("list");
  } else if (cmd == 520) {
    SetOpacity(-1);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 521) {
    SetOpacity(102);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 522) {
    SetOpacity(140);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 523) {
    SetOpacity(178);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 524) {
    SetOpacity(217);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 525) {
    SetOpacity(255);
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  } else if (cmd == 14 || cmd == 15) {
    if (cmd == 15) {
      portal_path_.clear();
      if (on_portal_) {
        on_portal_(this, L"");
      }
    } else {
      BROWSEINFOW bi{};
      bi.hwndOwner = hwnd_;
      bi.lpszTitle = L"选择要镜像的文件夹";
      bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
      PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
      if (pidl) {
        wchar_t path[MAX_PATH] = {};
        if (SHGetPathFromIDListW(pidl, path)) {
          SetPortalPath(path);
          if (on_portal_) {
            on_portal_(this, portal_path_);
          }
        }
        CoTaskMemFree(pidl);
      }
    }
  } else if (cmd >= 400 &&
             cmd < 400 + static_cast<int>(page_names_.size())) {
    // P2-11: toggle multi-page membership (keep ≥1 page).
    TogglePageMembership(cmd - 400);
  } else if (cmd >= 100 && cmd < 100 + static_cast<int>(all.size())) {
    SetStyleId(all[static_cast<size_t>(cmd - 100)]->id);
    style_preview_active_ = false;
    preview_style_id_.clear();
    if (on_style_changed_) {
      on_style_changed_(this);
    }
  }
}

LRESULT CALLBACK FenceWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
  FenceWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<FenceWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<FenceWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT FenceWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_APP + 60) {
    const int vk = static_cast<int>(wparam);
    const bool ctrl = (lparam & 1) != 0;
    const bool shift = (lparam & 2) != 0;
    const bool typeahead = (lparam & 4) != 0;
    if (typeahead) {
      // fence_keys posts Unicode char in wParam (ToUnicodeEx).
      if (wparam) {
        TypeAheadFind(static_cast<wchar_t>(wparam));
      }
      return 0;
    }
    HandleFenceHotkey(vk, ctrl, shift);
    return 0;
  }
  // Desktop chrome ≠ APPWINDOW (Fences / Rainmeter): never minimize with Win+D.
  if (msg == WM_SYSCOMMAND && FilterDesktopChromeSysCommand(wparam)) {
    return 0;
  }
  if (msg == WM_WINDOWPOSCHANGING) {
    FilterDesktopChromeWindowPosChanging(reinterpret_cast<WINDOWPOS*>(lparam));
  }
  switch (msg) {
    case WM_MOUSEACTIVATE:
      // Desktop overlay invariant: never activate or change Z-order on click,
      // keeping fences strictly below all app windows, prompts, and dialogs.
      //
      // MA_NOACTIVATE, never MA_NOACTIVATEANDEAT. The fence is WS_EX_NOACTIVATE,
      // so every click arrives here; ANDEAT discards the mouse message
      // (documented), which would keep the fence from ever seeing
      // WM_LBUTTONDOWN — no icon selection, no drag. MA_NOACTIVATE keeps this
      // window unactivated and still delivers the click.
      return MA_NOACTIVATE;
    case WM_ACTIVATE:
      if (wparam != WA_INACTIVE) {
        RefreshLayer();
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      PresentLayered();
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      // Win+D minimizes overlays: painting the iconic size blanks the layered
      // buffer (count chrome may survive a later stretch, icons do not).
      if (wparam == SIZE_MINIMIZED) {
        return 0;
      }
      LayoutCells();
      PresentLayered();
      return 0;
    case WM_KEYDOWN:
      if (wparam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        PasteClipboardFiles(/*as_link=*/shift);
        return 0;
      }
      break;
    case WM_MOUSEWHEEL: {
      if (collapsed_) {
        return 0;
      }
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
      // F4: Alt/Ctrl + wheel zooms this fence's icons (matches Python).
      const bool zoom_mod =
          (GetKeyState(VK_MENU) & 0x8000) != 0 ||
          (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      if (zoom_mod) {
        ApplyZoomWheel(delta);
        return 0;
      }
      scroll_y_ -= delta / 2;
      ClampScroll();
      LayoutCells();
      PresentLayered();
      return 0;
    }
    case WM_MENUSELECT: {
      // F8: hover over a style item previews it (reverted on cancel).
      if (style_menu_open_ && !(HIWORD(wparam) & MF_POPUP)) {
        const UINT id = LOWORD(wparam);
        const auto all = AllFenceStyles();
        if (id >= 100 && id < 100 + all.size()) {
          PreviewStyleId(all[id - 100]->id);
          return 0;
        }
      }
      break;
    }
    case WM_SETCURSOR: {
      if (LOWORD(lparam) == HTCLIENT) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        // Icons own the cursor over resize strips (Python fence_widget parity).
        if (HitTestIcon(pt.x, pt.y) < 0) {
          const Edge e = HitTestEdge(pt.x, pt.y);
          const int v = static_cast<int>(e);
          if (v == (static_cast<int>(Edge::Left) | static_cast<int>(Edge::Top)) ||
              v == (static_cast<int>(Edge::Right) |
                   static_cast<int>(Edge::Bottom))) {
            SetCursor(LoadCursor(nullptr, IDC_SIZENWSE));
            return TRUE;
          }
          if (v == (static_cast<int>(Edge::Right) | static_cast<int>(Edge::Top)) ||
              v == (static_cast<int>(Edge::Left) |
                   static_cast<int>(Edge::Bottom))) {
            SetCursor(LoadCursor(nullptr, IDC_SIZENESW));
            return TRUE;
          }
          if (v &
              (static_cast<int>(Edge::Left) | static_cast<int>(Edge::Right))) {
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            return TRUE;
          }
          if (v &
              (static_cast<int>(Edge::Top) | static_cast<int>(Edge::Bottom))) {
            SetCursor(LoadCursor(nullptr, IDC_SIZENS));
            return TRUE;
          }
        }
      }
      break;
    }
    case WM_LBUTTONDOWN: {
      // Fences: click activates this fence for keyboard chords (Ctrl+C…).
      SetFenceKeyTarget(hwnd_);
      if (on_activated_) {
        on_activated_(this);
      }
      press_client_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      header_press_ = HeaderBtn::None;
      // Title-bar chrome (collapse / lock / delete) before resize / drag.
      {
        const HeaderBtn hb =
            HitTestHeaderBtn(press_client_.x, press_client_.y);
        if (hb != HeaderBtn::None) {
          header_press_ = hb;
          dragging_ = false;
          rubber_ = false;
          icon_press_ = false;
          SetCapture(hwnd_);
          return 0;
        }
      }
      // Resize edge takes absolute precedence over inner scrollbar on border strips
      const Edge edge = HitTestEdge(press_client_.x, press_client_.y);
      const int icon = HitTestIcon(press_client_.x, press_client_.y);
      if (edge != Edge::None && icon < 0) {
        ClearSelection();
        resizing_ = true;
        resize_edge_ = edge;
        GetWindowRect(hwnd_, &resize_start_rc_);
        GetCursorPos(&resize_start_pt_);
        SetCapture(hwnd_);
        return 0;
      }

      // Scrollbar handling (clear of resize strip)
      const int sb = HitTestScrollbar(press_client_.x, press_client_.y);
      if (sb == 1) {
        thumb_dragging_ = true;
        thumb_start_y_ = press_client_.y;
        thumb_start_scroll_ = scroll_y_;
        SetCapture(hwnd_);
        return 0;
      }
      if (sb == 2) {
        // Click above/below thumb → page scroll.
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int header = S(hwnd_, kHeaderH0);
        const int view = (std::max)(0, static_cast<int>(rc.bottom) - header -
                                           S(hwnd_, kPad0));
        const int track_top = header + 2;
        const int track_h =
            (std::max)(8, static_cast<int>(rc.bottom) - track_top - 4);
        const int thumb_h =
            (std::max)(16, track_h * view / (std::max)(1, content_h_));
        const int max_scroll = (std::max)(1, content_h_ - view);
        int thumb_y = track_top + (track_h - thumb_h) * scroll_y_ / max_scroll;
        ScrollbarPage(press_client_.y < thumb_y ? -1 : 1);
        return 0;
      }

      const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      // Explorer / Fences: click empty panel (or header/edge) clears selection.
      // Ctrl keeps selection for additive rubber-band.
      if (icon < 0 && !ctrl) {
        typeahead_.clear();
        ClearSelection();
      }
      // Prefer icon over resize edge (bottom/side strips used to steal last-row
      // and edge-column clicks — Python clears resize when over an icon).
      if (icon >= 0) {
        dragging_ = false;
        drag_moved_ = false;
        rubber_ = false;
        rubber_additive_ = false;
        icon_press_ = true;
        icon_press_index_ = icon;
        // Explorer/Fences: Ctrl/Shift select on press so a following drag
        // includes the new target (public host already does this).
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift && selection_anchor_ >= 0) {
          SelectRange(icon);
          PresentLayered();
        } else if (ctrl) {
          ToggleSelected(icon);
          PresentLayered();
        }
        SetCapture(hwnd_);
        return 0;
      }
      if (edge != Edge::None) {
        resizing_ = true;
        resize_edge_ = edge;
        GetWindowRect(hwnd_, &resize_start_rc_);
        GetCursorPos(&resize_start_pt_);
        SetCapture(hwnd_);
        return 0;
      }
      icon_press_ = false;
      icon_press_index_ = -1;
      if (press_client_.y >= S(hwnd_, kHeaderH0)) {
        if (collapsed_) {
          return 0;
        }
        rubber_ = true;
        rubber_additive_ = ctrl;
        dragging_ = false;
        rubber_a_ = rubber_b_ = press_client_;
        SetCapture(hwnd_);
        return 0;
      }
      if (position_locked_) {
        return 0;
      }
      rubber_ = false;
      dragging_ = true;
      drag_moved_ = false;
      POINT pt = press_client_;
      ClientToScreen(hwnd_, &pt);
      RECT rc{};
      GetWindowRect(hwnd_, &rc);
      drag_offset_.x = pt.x - rc.left;
      drag_offset_.y = pt.y - rc.top;
      SetCapture(hwnd_);
      return 0;
    }
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tme{};
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE;
      tme.hwndTrack = hwnd_;
      TrackMouseEvent(&tme);
      // Fences Auto-hide: cancel pending collapse and expand on hover.
      if (auto_hide_) {
        KillTimer(hwnd_, kTimerAutoHide);
        if (collapsed_ && collapsible_ && !dragging_ && !resizing_ &&
            !icon_dragging_ && !rubber_ && !rename_edit_) {
          SetCollapsed(false, /*persist=*/false);
        }
      }
      if (fade_on_idle_ && fade_alpha_ != 255) {
        SetTimer(hwnd_, kTimerIdleFade, kIdleFadeIntervalMs, nullptr);
      }
      if (resizing_ && (wparam & MK_LBUTTON)) {
        UpdateHoverTip(-1);
        POINT pt{};
        GetCursorPos(&pt);
        const int dx = pt.x - resize_start_pt_.x;
        const int dy = pt.y - resize_start_pt_.y;
        int l = resize_start_rc_.left;
        int t = resize_start_rc_.top;
        int r = resize_start_rc_.right;
        int b = resize_start_rc_.bottom;
        const int ev = static_cast<int>(resize_edge_);
        if (ev & static_cast<int>(Edge::Left)) {
          l += dx;
        }
        if (ev & static_cast<int>(Edge::Right)) {
          r += dx;
        }
        if (ev & static_cast<int>(Edge::Top)) {
          t += dy;
        }
        if (ev & static_cast<int>(Edge::Bottom)) {
          b += dy;
        }
        if (r - l < kFenceMinW) {
          if (ev & static_cast<int>(Edge::Left)) {
            l = r - kFenceMinW;
          } else {
            r = l + kFenceMinW;
          }
        }
        if (b - t < kFenceMinH) {
          if (ev & static_cast<int>(Edge::Top)) {
            t = b - kFenceMinH;
          } else {
            b = t + kFenceMinH;
          }
        }
        // Live edge snap to monitor work area (Python snap_geometry on resize).
        RECT snapped{l, t, r, b};
        SnapRectToWorkArea(&snapped, 0);
        SetOverlayScreenPos(hwnd_, snapped.left, snapped.top,
                            snapped.right - snapped.left,
                            snapped.bottom - snapped.top, SWP_NOZORDER);
        // Keep owned height in lockstep so PresentLayered does not snap back.
        if (!collapsed_) {
          expanded_h_ = (std::max)(
              kFenceExpandedFloor,
              static_cast<int>(snapped.bottom - snapped.top));
        }
        return 0;
      }
      if (rubber_ && (wparam & MK_LBUTTON)) {
        UpdateHoverTip(-1);
        rubber_b_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        // Explorer: auto-scroll when the marquee nears the viewport edge.
        {
          RECT rc{};
          GetClientRect(hwnd_, &rc);
          const int header = S(hwnd_, kHeaderH0);
          const int margin = S(hwnd_, 28);
          const int step = S(hwnd_, 24);
          const int old_scroll = scroll_y_;
          if (rubber_b_.y < header + margin) {
            scroll_y_ -= step;
          } else if (rubber_b_.y > rc.bottom - margin) {
            scroll_y_ += step;
          }
          if (scroll_y_ != old_scroll) {
            ClampScroll();
            // Keep content-relative origin stable while cells reflow.
            rubber_a_.y += (old_scroll - scroll_y_);
            LayoutCells();
          }
        }
        // Live marquee like Explorer / Fences. Require a real rectangle so
        // click-jitter on blank does not re-select the nearest icon.
        const int bw = abs(rubber_b_.x - rubber_a_.x);
        const int bh = abs(rubber_b_.y - rubber_a_.y);
        // Explorer/public: either axis past threshold counts (thin marquees).
        if (bw >= kDragThreshold || bh >= kDragThreshold) {
          ApplyRubberBandSelection();
        }
        PresentLayered();
        return 0;
      }
      if (rbutton_press_ && (wparam & MK_RBUTTON) && rbutton_icon_ >= 0 &&
          !rbutton_dragged_) {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        if (abs(x - rbutton_press_pt_.x) >= kDragThreshold ||
            abs(y - rbutton_press_pt_.y) >= kDragThreshold) {
          // Explorer right-drag → OLE; sinks show Copy/Move/Create shortcut.
          rbutton_dragged_ = true;
          rbutton_press_ = false;
          std::vector<std::wstring> paths = SelectedPaths();
          if (paths.empty() &&
              rbutton_icon_ < static_cast<int>(cells_.size())) {
            paths.push_back(
                cells_[static_cast<size_t>(rbutton_icon_)].item.path);
          }
          rbutton_icon_ = -1;
          if (GetCapture() == hwnd_) {
            ReleaseCapture();
          }
          if (!paths.empty()) {
            CommitDragOutPins(std::move(paths));
          }
          return 0;
        }
      }
      if (icon_press_ && (wparam & MK_LBUTTON)) {
        UpdateHoverTip(-1);
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        if (abs(x - press_client_.x) >= kDragThreshold ||
            abs(y - press_client_.y) >= kDragThreshold) {
          BeginIconDrag(icon_press_index_);
        }
        return 0;
      }
      if (icon_dragging_ && (wparam & MK_LBUTTON)) {
        UpdateManualIconDrag(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
      }
      if (thumb_dragging_ && (wparam & MK_LBUTTON)) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        const int dy = pt.y - thumb_start_y_;
        const int header = S(hwnd_, kHeaderH0);
        const int view = (std::max)(0, static_cast<int>(rc.bottom) - header -
                                           S(hwnd_, kPad0));
        const int track_top = header + 2;
        const int track_h =
            (std::max)(8, static_cast<int>(rc.bottom) - track_top - 4);
        const int thumb_h =
            (std::max)(16, track_h * view / (std::max)(1, content_h_));
        const int max_scroll = (std::max)(1, content_h_ - view);
        const int track_span = (std::max)(1, track_h - thumb_h);
        scroll_y_ = thumb_start_scroll_ + dy * max_scroll / track_span;
        ClampScroll();
        LayoutCells();
        PresentLayered();
        return 0;
      }
      if (!dragging_ || !(wparam & MK_LBUTTON)) {
        const int hit =
            HitTestIcon(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        if (hit != tip_index_) {
          UpdateHoverTip(hit);
        } else if (hit >= 0 && tip_) {
          POINT pt{};
          GetCursorPos(&pt);
          SendMessageW(tip_, TTM_TRACKPOSITION, 0,
                       MAKELPARAM(pt.x + 12, pt.y + 16));
        }
        return 0;
      }
      UpdateHoverTip(-1);
      POINT pt{};
      GetCursorPos(&pt);
      RECT cur{};
      GetWindowRect(hwnd_, &cur);
      const int w = cur.right - cur.left;
      const int h = cur.bottom - cur.top;
      RECT next{pt.x - drag_offset_.x, pt.y - drag_offset_.y,
                pt.x - drag_offset_.x + w, pt.y - drag_offset_.y + h};
      // Live edge snap while dragging (Python fence_widget mouseMove).
      SnapRectToWorkArea(&next, 0);
      MoveOverlayScreen(hwnd_, next.left, next.top);
      drag_moved_ = true;
      return 0;
    }
    case WM_MOUSELEAVE:
      UpdateHoverTip(-1);
      if (fade_on_idle_) {
        SetTimer(hwnd_, kTimerIdleFade, kIdleFadeIntervalMs, nullptr);
      }
      // Fences Auto-hide: short delay so title-button / menu hops don't flicker.
      if (auto_hide_ && !collapsed_ && collapsible_ && !dragging_ &&
          !resizing_ && !icon_dragging_ && !rubber_ && !rename_edit_ &&
          header_press_ == HeaderBtn::None) {
        SetTimer(hwnd_, kTimerAutoHide, 350, nullptr);
      }
      return 0;
    case WM_TIMER:
      if (wparam == kTimerIdleFade) {
        TickIdleFade();
        return 0;
      }
      if (wparam == kTimerAutoHide) {
        KillTimer(hwnd_, kTimerAutoHide);
        if (auto_hide_ && !collapsed_ && collapsible_ && !dragging_ &&
            !resizing_ && !icon_dragging_ && !rubber_ && !rename_edit_ &&
            !style_menu_open_ && !drop_preview_) {
          POINT pt{};
          GetCursorPos(&pt);
          RECT rc{};
          GetWindowRect(hwnd_, &rc);
          if (!PtInRect(&rc, pt)) {
            SetCollapsed(true, /*persist=*/false);
          }
        }
        return 0;
      }
      if (wparam == kTimerDragScroll) {
        if (!icon_dragging_) {
          KillTimer(hwnd_, kTimerDragScroll);
          return 0;
        }
        // Cursor is parked against an edge: keep scrolling on every tick.
        POINT cur{};
        if (GetCursorPos(&cur) && ScreenToClient(hwnd_, &cur) &&
            ScrollIfAtDragEdge(cur.x, cur.y)) {
          drag_insert_ = InsertIndexAt(cur.x, cur.y);
          PresentLayered();
        }
        return 0;
      }
      break;
    case WM_LBUTTONUP: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      ReleaseCapture();
      if (header_press_ != HeaderBtn::None) {
        const HeaderBtn pressed = header_press_;
        header_press_ = HeaderBtn::None;
        if (HitTestHeaderBtn(x, y) == pressed && !dragging_ && !rubber_ &&
            !icon_dragging_ && !thumb_dragging_ && !resizing_) {
          ActivateHeaderBtn(pressed);
        }
        return 0;
      }
      if (thumb_dragging_) {
        thumb_dragging_ = false;
        return 0;
      }
      if (icon_dragging_) {
        FinishManualIconDrag(x, y);
        return 0;
      }
      if (rubber_) {
        rubber_ = false;
        const int bw = abs(rubber_b_.x - rubber_a_.x);
        const int bh = abs(rubber_b_.y - rubber_a_.y);
        // Click / tiny jitter on blank = deselect only (cleared on press).
        if (bw < kDragThreshold && bh < kDragThreshold && !rubber_additive_) {
          rubber_additive_ = false;
          ClearSelection();
          return 0;
        }
        ApplyRubberBandSelection();
        rubber_additive_ = false;
        PresentLayered();
        return 0;
      }
      if (resizing_) {
        resizing_ = false;
        // Commit owned height before Present/persist (ResolveLayerSize uses
        // expanded_h_ once resizing_ is clear).
        {
          RECT rc{};
          GetWindowRect(hwnd_, &rc);
          SnapRectToWorkArea(&rc, 0);

          // Honor exact user-dragged bounds without artificial discrete step jump
          int nw = (std::max)(kFenceMinW, static_cast<int>(rc.right - rc.left));
          int nh = (std::max)(kFenceMinH, static_cast<int>(rc.bottom - rc.top));
          const int ev = static_cast<int>(resize_edge_);
          if (ev & static_cast<int>(Edge::Bottom)) {
            // Snap to full icon row height to avoid cutting icons horizontally
            const int header = S(hwnd_, kHeaderH0);
            const int pad = S(hwnd_, kPad0);
            const int footer_h = S(hwnd_, 24);
            const int cell_h = CellHeightPx();
            if (cell_h > 0) {
              const int content_h = nh - header - pad * 2 - footer_h;
              const int rows = (std::max)(1, (content_h + cell_h / 2) / cell_h);
              const int snapped_h = header + pad * 2 + footer_h + rows * cell_h;
              if (std::abs(nh - snapped_h) <= 24) {
                nh = snapped_h;
              }
            }
          }
          if (ev & static_cast<int>(Edge::Left)) {
            rc.left = rc.right - nw;
          } else {
            rc.right = rc.left + nw;
          }
          if (ev & static_cast<int>(Edge::Top)) {
            rc.top = rc.bottom - nh;
          } else {
            rc.bottom = rc.top + nh;
          }
          SnapRectToWorkArea(&rc, 0);

          if (!collapsed_) {
            expanded_h_ = (std::max)(kFenceExpandedFloor, static_cast<int>(rc.bottom - rc.top));
          }
          SetOverlayScreenPos(hwnd_, rc.left, rc.top, rc.right - rc.left,
                              collapsed_ ? S(hwnd_, kCollapsedH0) : expanded_h_,
                              SWP_NOZORDER);
        }
        resize_edge_ = Edge::None;
        AttachToDesktopLayer();
        PresentLayered();
        // Final snap + sibling resolve (same path as drag-end).
        if (on_geometry_moved_) {
          on_geometry_moved_(this);
        } else if (on_moved_) {
          on_moved_(this);
        }
        return 0;
      }
      if (icon_press_) {
        const int idx = icon_press_index_;
        icon_press_ = false;
        icon_press_index_ = -1;
        // Commit if release stays on the same cell, or never left the drag
        // threshold (small slip off glyph/padding still counts as a click).
        if (idx >= 0 && (HitTestIcon(x, y) == idx || !drag_moved_)) {
          const bool was_selected = IsSelected(idx);
          const DWORD now = GetTickCount();
          const UINT dbl = GetDoubleClickTime();
          if (idx == last_icon_click_ &&
              now - last_icon_click_tick_ <= dbl) {
            last_icon_click_ = -1;
            last_icon_click_tick_ = 0;
            // Alt+dblclick → Properties; Shift+dblclick → open externally.
            if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
              SelectOnly(idx);
              ShowShellPathsProperties(hwnd_, SelectedPaths());
              return 0;
            }
            const bool external =
                (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            OpenItem(idx, external);
            return 0;
          }
          last_icon_click_ = idx;
          last_icon_click_tick_ = now;
          const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
          const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
          // Ctrl/Shift already applied on LBUTTONDOWN (drag sees new selection).
          if (ctrl || shift) {
            return 0;
          }
          if (was_selected) {
            // Explorer: second click on the *name* (after dbl-click window)
            // starts rename; glyph re-click keeps selection.
            RECT label{};
            if (IconLabelRect(static_cast<size_t>(idx), &label) &&
                x >= label.left && x < label.right && y >= label.top &&
                y < label.bottom) {
              BeginInlineRename();
            }
          } else {
            SelectOnly(idx);
          }
        }
        return 0;
      }
      if (dragging_) {
        dragging_ = false;
        if (!drag_moved_) {
          ClearSelection();
        }
        if (drag_moved_) {
          AttachToDesktopLayer();
          if (on_geometry_moved_) {
            on_geometry_moved_(this);
          } else if (on_moved_) {
            RECT rc{};
            GetWindowRect(hwnd_, &rc);
            SnapRectToWorkArea(&rc, 0);
            MoveOverlayScreen(hwnd_, rc.left, rc.top);
            on_moved_(this);
          }
        }
        return 0;
      }
      return 0;
    }
    case WM_RBUTTONDOWN: {
      SetFenceKeyTarget(hwnd_);
      if (on_activated_) {
        on_activated_(this);
      }
      // Match Explorer: right-press on empty cancels rubber / keeps chrome clean.
      if (rubber_) {
        rubber_ = false;
        rubber_additive_ = false;
        ReleaseCapture();
        PresentLayered();
      }
      rbutton_press_ = true;
      rbutton_dragged_ = false;
      rbutton_press_pt_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      rbutton_icon_ = HitTestIcon(rbutton_press_pt_.x, rbutton_press_pt_.y);
      if (rbutton_icon_ >= 0) {
        if (!IsSelected(rbutton_icon_)) {
          SelectOnly(rbutton_icon_);
          PresentLayered();
        }
        SetCapture(hwnd_);
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      const bool was_drag = rbutton_dragged_;
      rbutton_press_ = false;
      rbutton_dragged_ = false;
      rbutton_icon_ = -1;
      if (GetCapture() == hwnd_) {
        ReleaseCapture();
      }
      if (was_drag) {
        return 0;  // OLE right-drag already handled drop / cancel
      }
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int icon = HitTestIcon(pt.x, pt.y);
      ClientToScreen(hwnd_, &pt);
      if (icon >= 0) {
        // ShowIconMenu keeps multi-select when the clicked item is already selected.
        ShowIconMenu(icon, pt);
      } else {
        ShowFenceMenu(pt);
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      const int icon = HitTestIcon(x, y);
      if (icon >= 0) {
        last_icon_click_ = -1;
        last_icon_click_tick_ = 0;
        // Alt+dblclick → Properties; Shift+dblclick → open externally;
        // Ctrl+dblclick → new window (handled inside OpenItem).
        if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
          SelectOnly(icon);
          ShowShellPathsProperties(hwnd_, SelectedPaths());
          return 0;
        }
        const bool external = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        OpenItem(icon, external);
        return 0;
      }
      if (y < S(hwnd_, kHeaderH0) && HitTestEdge(x, y) == Edge::None) {
        // Title double-click toggles collapse (rename stays in context menu).
        ToggleCollapsed();
      }
      return 0;
    }
    case WM_MBUTTONUP: {
      // Explorer: middle-click folder → new window; files open normally.
      const int icon =
          HitTestIcon(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (icon >= 0 && icon < static_cast<int>(cells_.size()) &&
          !cells_[static_cast<size_t>(icon)].item.path.empty()) {
        if (!IsSelected(icon)) {
          SelectOnly(icon);
        }
        ExploreShellPath(cells_[static_cast<size_t>(icon)].item.path);
      }
      return 0;
    }
    case WM_XBUTTONUP: {
      // Explorer / browser: XBUTTON1 Back → up; XBUTTON2 Forward → restore.
      if (GET_XBUTTON_WPARAM(wparam) == XBUTTON1) {
        if (PortalGoUp()) {
          typeahead_.clear();
          if (on_refresh_) {
            on_refresh_(this);
          }
        }
        return TRUE;
      }
      if (GET_XBUTTON_WPARAM(wparam) == XBUTTON2) {
        if (PortalGoForward()) {
          typeahead_.clear();
          if (on_refresh_) {
            on_refresh_(this);
          }
        }
        return TRUE;
      }
      break;
    }
    case WM_DESTROY:
      tip_ = nullptr;
      tip_index_ = -1;
      // A popup EDIT outlives its owner's HWND: destroy it here or it floats
      // over the desktop after the fence is removed.
      CloseFilterEdit();
      ClearIcons();
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

void FenceWindow::Paint(HDC hdc) {
  int lw = 0;
  int lh = 0;
  if (!ResolveLayerSize(&lw, &lh)) {
    return;
  }
  RECT rc{0, 0, lw, lh};

  const int header = S(hwnd_, kHeaderH0);
  const int pad = S(hwnd_, kPad0);
  const bool list = view_mode_ == "list";
  const int cell_w = CellWidthPx();
  const int icon = IconSizePx();
  const int font_px = S(hwnd_, list ? 13 : 12);
  const int title_px = S(hwnd_, 15);

  SetGraphicsMode(hdc, GM_COMPATIBLE);
  Gdiplus::Graphics g(hdc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
  g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
  // Grid-fit grayscale AA stays sharp on a layered (premultiplied) surface.
  // ClearType assumes an opaque background and fringes here.
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
  g.SetTextContrast(4);

  EnsureFonts(title_px, font_px, S(hwnd_, 11));
  Gdiplus::SolidBrush title_brush(
      Gdiplus::Color(255, GetRValue(title_color_), GetGValue(title_color_),
                     GetBValue(title_color_)));
  Gdiplus::StringFormat title_fmt;
  title_fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
  title_fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
  title_fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
  title_fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
  const int title_top = S(hwnd_, 6);
  RECT collapse_rc{}, lock_rc{}, dissolve_rc{}, portal_up_rc{};
  HeaderBtnRects(&collapse_rc, &lock_rc, &dissolve_rc, &portal_up_rc);
  const int title_left =
      (portal_up_rc.right > portal_up_rc.left) ? portal_up_rc.right + 4
                                               : S(hwnd_, 6);
  const int chrome_right = [&]() {
    int leftmost = rc.right;
    auto consider = [&](const RECT& r) {
      if (r.right > r.left && r.left < leftmost) {
        leftmost = r.left;
      }
    };
    consider(collapse_rc);
    consider(lock_rc);
    consider(dissolve_rc);
    return (std::max)(title_left, leftmost - 6);
  }();
  if (show_title_) {
    Gdiplus::RectF title_box(
        static_cast<Gdiplus::REAL>(title_left),
        static_cast<Gdiplus::REAL>(title_top),
        static_cast<Gdiplus::REAL>((std::max)(0, chrome_right - title_left)),
        static_cast<Gdiplus::REAL>(title_px + 6));
    // Soft shadow for readability on glass (360-like caption).
    Gdiplus::SolidBrush shadow_br(Gdiplus::Color(
        light_panel_ ? 40 : 90,
        light_panel_ ? 255 : 0, light_panel_ ? 255 : 0,
        light_panel_ ? 255 : 0));
    Gdiplus::RectF shadow_box = title_box;
    shadow_box.Y += 1.f;
    const std::wstring caption = DisplayTitle();
    g.DrawString(caption.c_str(), -1, title_font_, shadow_box, &title_fmt,
                 &shadow_br);
    g.DrawString(caption.c_str(), -1, title_font_, title_box, &title_fmt,
                 &title_brush);
  }
  // Title-bar icons only (no text): collapse · lock · delete. Edit stays in settings.
  {
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(title_color_),
                                    GetGValue(title_color_),
                                    GetBValue(title_color_)),
                     1.6f);
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                   Gdiplus::DashCapRound);
    auto mid = [](const RECT& r) {
      return Gdiplus::PointF(
          (r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f);
    };
    auto draw_btn_backing = [&](const RECT& r) {
      if (r.right > r.left) {
        const auto c = mid(r);
        const float br = (std::min)(11.0f, (r.bottom - r.top) * 0.44f);
        Gdiplus::SolidBrush chip_bg(Gdiplus::Color(
            light_panel_ ? 16 : 24, 255, 255, 255));
        g.FillEllipse(&chip_bg, c.X - br, c.Y - br, br * 2.0f, br * 2.0f);
      }
    };
    draw_btn_backing(portal_up_rc);
    draw_btn_backing(collapse_rc);
    draw_btn_backing(lock_rc);
    draw_btn_backing(dissolve_rc);
    // Portal up (Fences Folder Portal — left of title when drilled in).
    if (portal_up_rc.right > portal_up_rc.left) {
      const auto c = mid(portal_up_rc);
      const float s = 4.5f;
      g.DrawLine(&pen, c.X - s, c.Y + s * 0.2f, c.X, c.Y - s * 0.6f);
      g.DrawLine(&pen, c.X, c.Y - s * 0.6f, c.X + s, c.Y + s * 0.2f);
    }
    // Collapse chevron
    if (collapse_rc.right > collapse_rc.left) {
      const auto c = mid(collapse_rc);
      const float s = 4.5f;
      if (collapsed_) {
        g.DrawLine(&pen, c.X - s, c.Y - s * 0.4f, c.X, c.Y + s * 0.5f);
        g.DrawLine(&pen, c.X, c.Y + s * 0.5f, c.X + s, c.Y - s * 0.4f);
      } else {
        g.DrawLine(&pen, c.X - s, c.Y + s * 0.4f, c.X, c.Y - s * 0.5f);
        g.DrawLine(&pen, c.X, c.Y - s * 0.5f, c.X + s, c.Y + s * 0.4f);
      }
    }
    // Lock / unlock padlock. Two independent locks share this glyph:
    //   position lock → closed shackle
    //   content lock  → filled body (set + order frozen)
    if (lock_rc.right > lock_rc.left) {
      const auto c = mid(lock_rc);
      const float bw = 7.f, bh = 6.f;
      Gdiplus::RectF body(c.X - bw * 0.5f, c.Y - 0.5f, bw, bh);
      if (content_locked_) {
        g.FillRectangle(&title_brush, body);
      } else {
        g.DrawRectangle(&pen, body);
      }
      // Shackle
      Gdiplus::REAL sh_top = c.Y - 6.5f;
      Gdiplus::REAL sh_bot = c.Y - 0.5f;
      if (position_locked_ || content_locked_) {
        g.DrawArc(&pen, c.X - 3.2f, sh_top, 6.4f, 7.f, 200.f, 140.f);
      } else {
        // Open shackle — arc shifted right
        g.DrawArc(&pen, c.X - 1.2f, sh_top, 6.4f, 7.f, 200.f, 140.f);
        g.DrawLine(&pen, c.X + 5.f, sh_bot - 1.f, c.X + 5.f, sh_top + 2.f);
      }
      // Keyhole (inverted when the body is filled, to stay visible)
      if (content_locked_) {
        Gdiplus::SolidBrush knock(Gdiplus::Color(
            light_panel_ ? 40 : 200, 0, 0, 0));
        g.FillEllipse(&knock, c.X - 1.1f, c.Y + 0.8f, 2.2f, 2.2f);
      } else {
        g.FillEllipse(&title_brush, c.X - 1.1f, c.Y + 0.8f, 2.2f, 2.2f);
      }
      g.DrawLine(&pen, c.X, c.Y + 2.5f, c.X, c.Y + 4.2f);
    }
    // Delete = X
    if (dissolve_rc.right > dissolve_rc.left) {
      const auto c = mid(dissolve_rc);
      const float s = 4.2f;
      g.DrawLine(&pen, c.X - s, c.Y - s, c.X + s, c.Y + s);
      g.DrawLine(&pen, c.X + s, c.Y - s, c.X - s, c.Y + s);
    }
  }
  if (collapsed_) {
    return;
  }

  const int view = (std::max)(0, static_cast<int>(rc.bottom) - header - pad);
  // Modern 360-style ultra-thin pill scrollbar (no background track)
  if (content_h_ > view && view > 0) {
    const int track_top = header + 4;
    const int track_h = (std::max)(8, static_cast<int>(rc.bottom) - track_top - 6);
    const int thumb_h =
        (std::max)(22, track_h * view / (std::max)(1, content_h_));
    const int max_scroll = (std::max)(1, content_h_ - view);
    int thumb_y = track_top + (track_h - thumb_h) * scroll_y_ / max_scroll;
    if (thumb_y + thumb_h > track_top + track_h) {
      thumb_y = track_top + track_h - thumb_h;
    }
    const float pill_x = static_cast<float>(rc.right - kEdge - 5);
    const float pill_w = 3.2f;
    const float pill_y = static_cast<float>(thumb_y);
    const float pill_h = static_cast<float>(thumb_h);

    Gdiplus::GraphicsPath pill_path;
    pill_path.AddArc(pill_x, pill_y, pill_w, pill_w, 180, 180);
    pill_path.AddArc(pill_x, pill_y + pill_h - pill_w, pill_w, pill_w, 0, 180);
    pill_path.CloseFigure();

    Gdiplus::SolidBrush pill_br(Gdiplus::Color(
        light_panel_ ? 95 : 125,
        GetRValue(accent_), GetGValue(accent_), GetBValue(accent_)));
    g.FillPath(&pill_br, &pill_path);
  }

  const int footer_reserve =
      (!collapsed_ && (rc.right - rc.left) >= S(hwnd_, 180) && !cells_.empty())
          ? S(hwnd_, 24)
          : 0;
  const int clip_bottom = (std::max)(header, static_cast<int>(rc.bottom) - footer_reserve);
  g.SetClip(Gdiplus::Rect(rc.left, header, rc.right - rc.left,
                          clip_bottom - header));

  Gdiplus::StringFormat list_fmt;
  list_fmt.SetAlignment(Gdiplus::StringAlignmentNear);
  list_fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
  list_fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
  list_fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

  // Idle grid: wrap up to 2 lines with ellipsis (Explorer / Fences style).
  Gdiplus::StringFormat grid_fmt;
  grid_fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
  grid_fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
  grid_fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
  grid_fmt.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);

  // Selected: full name, no ellipsis — grow vertically like Explorer.
  Gdiplus::StringFormat sel_fmt;
  sel_fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
  sel_fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
  sel_fmt.SetTrimming(Gdiplus::StringTrimmingNone);

  const int line_h = font_px + S(hwnd_, 5);
  const int idle_label_h = line_h * label_lines_ + S(hwnd_, 4);
  const int max_sel_lines = 8;

  // Cut ghost opacity matches Python _CUT_GHOST_OPACITY ≈ 0.42.
  constexpr BYTE kCutOpacity = 107;
  // Resolve the cut set once per Present instead of per cell (was O(cell×cut)).
  const auto& cut_set = CutPathSet();

  // Pre-build caption brushes once per Present (avoid per-cell GDI+ churn).
  Gdiplus::SolidBrush name_brush_normal(
      Gdiplus::Color(255, GetRValue(caption_color_), GetGValue(caption_color_),
                     GetBValue(caption_color_)));
  Gdiplus::SolidBrush name_brush_cut(
      Gdiplus::Color(kCutOpacity, GetRValue(caption_color_),
                     GetGValue(caption_color_), GetBValue(caption_color_)));
  Gdiplus::SolidBrush cap_shadow(
      Gdiplus::Color(light_panel_ ? 48 : 96,
                     light_panel_ ? 255 : 0, light_panel_ ? 255 : 0,
                     light_panel_ ? 255 : 0));

  auto paint_cell = [&](size_t i, bool selected_pass) {
    if (i >= cells_.size()) {
      return;
    }
    const auto& cell = cells_[i];
    if (cell.item.path.empty()) {
      return;  // sparse seat — blank chrome only
    }
    const bool is_sel =
        i < selected_.size() && selected_[i] != 0;
    if (selected_pass != is_sel) {
      return;
    }
    const RECT& b = cell.bounds;
    if (b.bottom < header || b.top > rc.bottom) {
      return;
    }
    // Short-circuit: no cut/ghost set → skip the per-cell lowercase+alloc.
    const bool is_cut =
        !cut_set.empty() &&
        cut_set.find(LowerPath(cell.item.path)) != cut_set.end();
    bool is_drag_src = false;
    if (icon_dragging_) {
      for (int di : drag_indices_) {
        if (di == static_cast<int>(i)) {
          is_drag_src = true;
          break;
        }
      }
    }
    const bool is_hover =
        !is_sel && tip_index_ >= 0 &&
        static_cast<size_t>(tip_index_) == i;
    const bool is_focus =
        !is_sel && selection_focus_ == static_cast<int>(i);
    // Source cells stay as faint placeholders while the cursor ghost moves
    // (Explorer / Fences-style; same opacity as cut ghost).
    const BYTE icon_op =
        (is_cut || is_drag_src) ? kCutOpacity : 255;
    Gdiplus::SolidBrush* name_brush =
        (is_cut || is_drag_src) ? &name_brush_cut : &name_brush_normal;
    if (list) {
      if (is_sel) {
        PaintSelectionChrome(g, b.left + 2, b.top + 1, b.right - 2,
                             b.bottom - 1, accent_, S(hwnd_, 4));
      } else if (is_focus) {
        // Ctrl+Arrow focus (not selected) — lighter ring than full selection.
        PaintSelectionChrome(g, b.left + 2, b.top + 1, b.right - 2,
                             b.bottom - 1, accent_, S(hwnd_, 4), 36, 110);
      } else if (is_hover) {
        PaintSelectionChrome(g, b.left + 2, b.top + 1, b.right - 2,
                             b.bottom - 1, accent_, S(hwnd_, 4), 48, 90);
      }
      const int iy = b.top + ((b.bottom - b.top) - icon) / 2;
      if (cell.icon) {
        DrawIconCached(g, cell.icon, b.left + 4, iy, icon, icon_op);
      }
      const int lw =
          (std::max)(1, static_cast<int>(b.right - b.left) - icon - 12);
      Gdiplus::RectF label(static_cast<Gdiplus::REAL>(b.left + 8 + icon),
                           static_cast<Gdiplus::REAL>(b.top),
                           static_cast<Gdiplus::REAL>(lw),
                           static_cast<Gdiplus::REAL>(b.bottom - b.top));
      g.DrawString(cell.item.name.c_str(), -1, caption_font_, label, &list_fmt,
                   name_brush);
      return;
    }

    const int label_w = (std::max)(1, static_cast<int>(b.right - b.left) - 6);
    const int label_top = b.top + 4 + icon + 2;
    int label_h = idle_label_h;
    int ink_h = idle_label_h;
    if (is_sel) {
      Gdiplus::RectF measure(
          0.f, 0.f, static_cast<Gdiplus::REAL>(label_w),
          static_cast<Gdiplus::REAL>(line_h * max_sel_lines + 8));
      Gdiplus::RectF bound;
      g.MeasureString(cell.item.name.c_str(), -1, caption_font_, measure,
                      &sel_fmt, &bound);
      ink_h = static_cast<int>(bound.Height + 0.5f) + 2;
      if (ink_h < line_h) {
        ink_h = line_h;
      }
      label_h = ink_h;
      if (label_h < idle_label_h) {
        label_h = idle_label_h;
      }
      const int cap = line_h * max_sel_lines + S(hwnd_, 8);
      if (label_h > cap) {
        label_h = cap;
      }
      if (ink_h > label_h) {
        ink_h = label_h;
      }
      // Hug glyph + caption ink (not empty 2-line shelf). Translucent accent.
      const int chrome_b = label_top + ink_h + S(hwnd_, 3);
      PaintSelectionChrome(g, b.left + 3, b.top + 2, b.right - 3, chrome_b,
                           accent_, S(hwnd_, 6));
    } else if (is_focus) {
      const int chrome_b = label_top + idle_label_h + S(hwnd_, 2);
      PaintSelectionChrome(g, b.left + 3, b.top + 2, b.right - 3, chrome_b,
                           accent_, S(hwnd_, 6), 36, 110);
    } else if (is_hover) {
      const int chrome_b = label_top + idle_label_h + S(hwnd_, 2);
      PaintSelectionChrome(g, b.left + 3, b.top + 2, b.right - 3, chrome_b,
                           accent_, S(hwnd_, 6), 48, 90);
    }

    if (cell.icon) {
      const int ix = b.left + (cell_w - icon) / 2;
      const int iy = b.top + 4;
      DrawIconCached(g, cell.icon, ix, iy, icon, icon_op);
    }
    Gdiplus::RectF label(static_cast<Gdiplus::REAL>(b.left + 3),
                         static_cast<Gdiplus::REAL>(label_top),
                         static_cast<Gdiplus::REAL>(label_w),
                         static_cast<Gdiplus::REAL>(label_h));
    Gdiplus::StringFormat* fmt = is_sel ? &sel_fmt : &grid_fmt;
    Gdiplus::StringFormat ellipsis_fmt;
    if (is_sel && label_h >= line_h * max_sel_lines) {
      // If still overflowing max lines, fall back to ellipsis on last line.
      ellipsis_fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
      ellipsis_fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
      ellipsis_fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
      ellipsis_fmt.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);
      fmt = &ellipsis_fmt;
    }
    // Soft caption shadow (readability on light wallpapers).
    if (show_labels_) {
      Gdiplus::RectF sb = label;
      sb.Y += 1.f;
      g.DrawString(cell.item.name.c_str(), -1, caption_font_, sb, fmt,
                   &cap_shadow);
      g.DrawString(cell.item.name.c_str(), -1, caption_font_, label, fmt,
                   name_brush);
    }
  };

  // Idle cells first, then selected on top (expanded captions overlay neighbors).
  for (size_t i = 0; i < cells_.size(); ++i) {
    paint_cell(i, false);
  }
  for (size_t i = 0; i < cells_.size(); ++i) {
    paint_cell(i, true);
  }

  g.ResetClip();
  if (rubber_) {
    const int l = (std::min)(rubber_a_.x, rubber_b_.x);
    const int t = (std::min)(rubber_a_.y, rubber_b_.y);
    const int r = (std::max)(rubber_a_.x, rubber_b_.x);
    const int b = (std::max)(rubber_a_.y, rubber_b_.y);
    if (r - l >= 2 && b - t >= 2) {
      // Explorer-style translucent blue-ish band (use accent; keep alpha valid
      // for UpdateLayeredWindow — no GDI Rectangle).
      Gdiplus::SolidBrush fill(Gdiplus::Color(
          56, GetRValue(accent_), GetGValue(accent_), GetBValue(accent_)));
      g.FillRectangle(&fill, l, t, r - l, b - t);
      Gdiplus::Pen edge(
          Gdiplus::Color(200, GetRValue(accent_), GetGValue(accent_),
                         GetBValue(accent_)),
          1.0f);
      g.DrawRectangle(&edge, l, t, r - l - 1, b - t - 1);
    }
  }

  // Recycle Bin / folder hover (OLE inbound): accent that cell only.
  if (drop_preview_ && (drop_over_recycle_ || drop_over_folder_)) {
    const int over = HitTestIcon(drop_pt_.x, drop_pt_.y);
    if (over >= 0 && over < static_cast<int>(cells_.size())) {
      const RECT& b = cells_[static_cast<size_t>(over)].bounds;
      Gdiplus::SolidBrush fill(Gdiplus::Color(
          72, GetRValue(accent_), GetGValue(accent_), GetBValue(accent_)));
      g.FillRectangle(&fill, static_cast<INT>(b.left),
                      static_cast<INT>(b.top),
                      static_cast<INT>(b.right - b.left),
                      static_cast<INT>(b.bottom - b.top));
      Gdiplus::Pen edge(
          Gdiplus::Color(230, GetRValue(accent_), GetGValue(accent_),
                         GetBValue(accent_)),
          2.0f);
      g.DrawRectangle(&edge, static_cast<INT>(b.left),
                      static_cast<INT>(b.top),
                      static_cast<INT>(b.right - b.left - 1),
                      static_cast<INT>(b.bottom - b.top - 1));
    }
  }

  // F2 / OLE: insert bar ≈ icon length (manual reorder or external drop).
  const bool show_insert =
      (icon_dragging_ && !drag_left_ && drag_insert_ >= 0) ||
      (drop_preview_ && !drop_over_recycle_ && !drop_over_folder_ &&
       drop_insert_ >= 0);
  if (show_insert) {
    const int raw_insert =
        (icon_dragging_ && !drag_left_) ? drag_insert_ : drop_insert_;
    const int insert =
        (std::min)(raw_insert, static_cast<int>(cells_.size()));
    const POINT tip =
        (icon_dragging_ && !drag_left_) ? drag_pt_ : drop_pt_;
    Gdiplus::Pen pen(Gdiplus::Color(235, GetRValue(accent_), GetGValue(accent_),
                                    GetBValue(accent_)),
                     2.0f);
    const RECT* ref = nullptr;
    if (insert < static_cast<int>(cells_.size())) {
      ref = &cells_[static_cast<size_t>(insert)].bounds;
    } else if (!cells_.empty()) {
      ref = &cells_.back().bounds;
    }
    const int bar = icon + S(hwnd_, 8);
    if (view_mode_ == "list") {
      const int y = ref ? (insert < static_cast<int>(cells_.size()) ? ref->top
                                                                   : ref->bottom)
                        : (header + pad);
      int mid_x = ref ? (ref->left + ref->right) / 2
                      : (rc.left + pad + cell_w / 2);
      if (tip.x > 0) {
        mid_x = tip.x;
      }
      g.DrawLine(&pen, mid_x - bar / 2, y, mid_x + bar / 2, y);
    } else {
      const int x = ref ? (insert < static_cast<int>(cells_.size()) ? ref->left
                                                                   : ref->right)
                        : (rc.left + pad);
      int mid_y = ref ? (ref->top + 4 + icon / 2)
                      : (header + pad + icon / 2);
      if (tip.y > header) {
        mid_y = tip.y;
      }
      g.DrawLine(&pen, x, mid_y - bar / 2, x, mid_y + bar / 2);
    }
  }

  // Fences-style drag ghost: translucent icon(s) + count badge under cursor.
  if (icon_dragging_ && !drag_left_ && !drag_indices_.empty()) {
    constexpr BYTE kGhostOp = 160;
    const int total = static_cast<int>(drag_indices_.size());
    const int stack = (std::min)(3, total);
    int stack_right = drag_pt_.x - icon / 2;
    int stack_bottom = drag_pt_.y - icon / 2;
    for (int s = stack - 1; s >= 0; --s) {
      const int di = drag_indices_[static_cast<size_t>(s)];
      if (di < 0 || di >= static_cast<int>(cells_.size())) {
        continue;
      }
      HICON ic = cells_[static_cast<size_t>(di)].icon;
      if (!ic) {
        continue;
      }
      const int ox = drag_pt_.x - icon / 2 + s * S(hwnd_, 4);
      const int oy = drag_pt_.y - icon / 2 + s * S(hwnd_, 4);
      stack_right = (std::max)(stack_right, ox + icon);
      stack_bottom = (std::max)(stack_bottom, oy + icon);
      DrawIconCached(g, ic, ox, oy, icon, kGhostOp);
    }
    if (total > 1) {
      const int badge = (std::max)(S(hwnd_, 18), icon / 3);
      const int bx = stack_right - badge + S(hwnd_, 2);
      const int by = stack_bottom - badge + S(hwnd_, 2);
      Gdiplus::SolidBrush fill(Gdiplus::Color(230, 0, 120, 215));
      g.FillEllipse(&fill, bx, by, badge, badge);
      Gdiplus::Pen ring(Gdiplus::Color(255, 255, 255, 255), 1.f);
      g.DrawEllipse(&ring, bx, by, badge, badge);
      const std::wstring label = std::to_wstring(total);
      Gdiplus::FontFamily fam(L"Segoe UI");
      Gdiplus::Font font(&fam, static_cast<Gdiplus::REAL>((std::max)(10, badge * 2 / 3)),
                         Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
      Gdiplus::SolidBrush ink(Gdiplus::Color(255, 255, 255, 255));
      Gdiplus::StringFormat fmt;
      fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
      fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
      Gdiplus::RectF box(static_cast<Gdiplus::REAL>(bx),
                         static_cast<Gdiplus::REAL>(by),
                         static_cast<Gdiplus::REAL>(badge),
                         static_cast<Gdiplus::REAL>(badge));
      g.DrawString(label.c_str(), -1, &font, box, &fmt, &ink);
    }
  }

  // F5: bottom count / zoom hint band (hidden when narrow or collapsed).
  if (!collapsed_ && rc.right >= S(hwnd_, 180)) {
    const int footer_h = S(hwnd_, 20);
    const int fy = rc.bottom - footer_h - S(hwnd_, 4);
    std::wstring footer;
    if (GetTickCount() < footer_zoom_hint_until_) {
      footer = L"图标大小 " + std::to_wstring(icon_zoom_percent_) + L"%";
    } else {
      int filled = 0;
      for (const auto& c : cells_) {
        if (!c.item.path.empty()) {
          ++filled;
        }
      }
      const int count = (std::min)(filled, 60);
      if (count > 0) {
        footer = L"共 " + std::to_wstring(count) + L" 项";
      }
    }
    if (!footer.empty()) {
      Gdiplus::SolidBrush f_brush(Gdiplus::Color(
          190, GetRValue(caption_color_), GetGValue(caption_color_),
          GetBValue(caption_color_)));
      Gdiplus::StringFormat f_fmt;
      f_fmt.SetAlignment(Gdiplus::StringAlignmentNear);
      f_fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
      f_fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
      f_fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
      Gdiplus::RectF f_box(static_cast<Gdiplus::REAL>(pad),
                           static_cast<Gdiplus::REAL>(fy),
                           static_cast<Gdiplus::REAL>(rc.right - pad - 12),
                           static_cast<Gdiplus::REAL>(footer_h));
      g.DrawString(footer.c_str(), -1, footer_font_, f_box, &f_fmt, &f_brush);
    }
  }
}
