#include "public_host.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "i18n.h"
#include "icon_actions.h"
#include "log.h"
#include "ole_drag.h"
#include "organize_suppress.h"
#include "pin_store.h"
#include "shell_clipboard.h"
#include "shell_context_menu.h"
#include "toast.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <commctrl.h>
#include <imm.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "imm32.lib")

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativePublic";
constexpr wchar_t kMarqueeClass[] = L"DeskTidyNativeMarquee";
// Design @96 DPI — Layout() scales with the host monitor.
constexpr int kMargin0 = 20;
constexpr int kGapX0 = 0;
constexpr int kGapY0 = 6;
// Fences / Explorer loose icons have no host title chrome. A dark header plate
// used to appear as a「黑影」above unpinned floats (especially near top-left).
constexpr int kHeaderH = 0;
constexpr int kExtPublicRefresh = 1;
constexpr int kExtPublicMovePage = 2;
constexpr int kExtPublicAlignGrid = 3;
constexpr int kDragThreshold = 6;
bool g_reg = false;
ULONG_PTR g_gdiplus = 0;

void EnsureGdiplus() {
  if (g_gdiplus) {
    return;
  }
  Gdiplus::GdiplusStartupInput input;
  Gdiplus::GdiplusStartup(&g_gdiplus, &input, nullptr);
}

// DrawIconEx straight into the 32bpp ULW bitmap leaves alpha at 0 for the
// glyph pixels, so UpdateOverlayLayered(ULW_ALPHA) made the float icons fully
// transparent: only the header plate showed and clicks missed the icons.
// Compose through a premultiplied GDI+ bitmap (same approach as
// fence_window's DrawIconSharp, duplicated here to avoid a shared header
// change while that file is in flux).
// Builds an owned premultiplied 32bpp bitmap for `icon` drawn at `dest`
// (source size = the icon's native color width clamped to [dest, 256]).
// Returns nullptr on failure; the caller owns the returned Gdiplus::Bitmap.
Gdiplus::Bitmap* BuildPremultipliedIcon(HICON icon, int dest) {
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
  // Match fence_window: native-size DrawIconEx, then bicubic to dest×dest.
  // Never draw below dest; never cap to dest*2 (that used StretchBlt smear).
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
  for (int i = 0; i < n; ++i) {
    BYTE* p = px + i * 4;
    BYTE a = p[3];
    if (a == 0 && (p[0] | p[1] | p[2])) {
      a = 255;
    }
    if (src != dest) {
      if (a > 0 && a < 255) {
        p[0] = static_cast<BYTE>((std::min)(255, (p[0] * 255) / a));
        p[1] = static_cast<BYTE>((std::min)(255, (p[1] * 255) / a));
        p[2] = static_cast<BYTE>((std::min)(255, (p[2] * 255) / a));
      }
      p[3] = a;
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

// Explorer/DefView: icon label color follows wallpaper luminance (light desk →
// near-black ink; dark desk → white ink), with a soft opposing glow.
COLORREF ReadDesktopSolidBackground() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Colors", 0, KEY_READ,
                    &key) != ERROR_SUCCESS) {
    return RGB(0, 0, 0);
  }
  wchar_t buf[64] = {};
  DWORD type = 0;
  DWORD bytes = sizeof(buf);
  const LONG rc =
      RegQueryValueExW(key, L"Background", nullptr, &type,
                       reinterpret_cast<LPBYTE>(buf), &bytes);
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
    return RGB(0, 0, 0);
  }
  int r = 0;
  int g = 0;
  int b = 0;
  if (swscanf_s(buf, L"%d %d %d", &r, &g, &b) != 3) {
    return RGB(0, 0, 0);
  }
  return RGB(r, g, b);
}

int SampleWallpaperLuminance() {
  wchar_t path[MAX_PATH] = {};
  SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
  if (path[0] && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
    Gdiplus::Bitmap bmp(path);
    if (bmp.GetLastStatus() == Gdiplus::Ok && bmp.GetWidth() > 0 &&
        bmp.GetHeight() > 0) {
      const UINT w = bmp.GetWidth();
      const UINT h = bmp.GetHeight();
      // Sparse grid — enough for light/dark decision, cheap on 4K photos.
      double sum = 0;
      int n = 0;
      for (UINT y = h / 10; y < h && n < 64; y += (std::max)(1u, h / 8)) {
        for (UINT x = w / 10; x < w && n < 64; x += (std::max)(1u, w / 8)) {
          Gdiplus::Color c;
          if (bmp.GetPixel(static_cast<INT>(x), static_cast<INT>(y), &c) !=
              Gdiplus::Ok) {
            continue;
          }
          // Rec. 601 luma.
          sum += 0.299 * c.GetR() + 0.587 * c.GetG() + 0.114 * c.GetB();
          ++n;
        }
      }
      if (n > 0) {
        return static_cast<int>(sum / n);
      }
    }
  }
  const COLORREF solid = ReadDesktopSolidBackground();
  return static_cast<int>(0.299 * GetRValue(solid) + 0.587 * GetGValue(solid) +
                          0.114 * GetBValue(solid));
}

bool DesktopCaptionUseDarkInk() {
  static ULONGLONG s_tick = 0;
  static wchar_t s_path[MAX_PATH] = {};
  static bool s_dark_ink = false;
  wchar_t path[MAX_PATH] = {};
  SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
  const ULONGLONG now = GetTickCount64();
  if (s_tick != 0 && now - s_tick < 3000 && wcscmp(path, s_path) == 0) {
    return s_dark_ink;
  }
  wcscpy_s(s_path, path);
  s_tick = now;
  // Explorer threshold sits near mid-grey; bias slightly toward dark ink so
  // pale wallpapers match black desktop labels.
  s_dark_ink = SampleWallpaperLuminance() >= 140;
  return s_dark_ink;
}

void DrawExplorerCaption(Gdiplus::Graphics& g, const std::wstring& text,
                         const Gdiplus::RectF& box, const Gdiplus::Font& font,
                         const Gdiplus::StringFormat& fmt,
                         BYTE opacity = 255) {
  if (text.empty()) {
    return;
  }
  const bool dark_ink = DesktopCaptionUseDarkInk();
  const BYTE glow_a =
      static_cast<BYTE>((110 * static_cast<int>(opacity)) / 255);
  const BYTE ink_a = opacity;
  const BYTE glow = dark_ink ? 255 : 0;
  Gdiplus::SolidBrush glow_br(Gdiplus::Color(glow_a, glow, glow, glow));
  Gdiplus::SolidBrush ink_br(
      dark_ink ? Gdiplus::Color(ink_a, 0, 0, 0)
               : Gdiplus::Color(ink_a, 255, 255, 255));
  auto paint_at = [&](float dx, float dy) {
    Gdiplus::RectF r = box;
    r.X += dx;
    r.Y += dy;
    g.DrawString(text.c_str(), -1, &font, r, &fmt, &glow_br);
  };
  // 4-direction glow + main ink (Explorer drop-shadow readability).
  paint_at(-1.f, 0.f);
  paint_at(1.f, 0.f);
  paint_at(0.f, -1.f);
  paint_at(0.f, 1.f);
  g.DrawString(text.c_str(), -1, &font, box, &fmt, &ink_br);
}

std::wstring PublicLowerPath(std::wstring s) {
  for (auto& c : s) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  return s;
}

// Cut-path set cached by clipboard sequence (same idea as fence Present).
DWORD g_public_cut_seq = 0xFFFFFFFF;
std::unordered_set<std::wstring> g_public_cut_set;

const std::unordered_set<std::wstring>& PublicCutPathSet() {
  const DWORD seq = GetClipboardSequenceNumber();
  if (seq != g_public_cut_seq) {
    g_public_cut_seq = seq;
    g_public_cut_set.clear();
    std::vector<std::wstring> files;
    bool cut = false;
    if (ClipboardGetFilesWithEffect(&files, &cut) && cut) {
      for (auto& p : files) {
        g_public_cut_set.insert(PublicLowerPath(std::move(p)));
      }
    }
  }
  return g_public_cut_set;
}
}  // namespace

void PublicHost::ClearIconCache() {
  for (auto& kv : icon_cache_) {
    delete kv.second;
  }
  icon_cache_.clear();
}

void PublicHost::DrawIconCached(Gdiplus::Graphics& g, HICON icon, int x, int y,
                                int dest, BYTE opacity) {
  if (!icon || dest <= 0) {
    return;
  }
  const IconCacheKey key{icon, dest};
  auto it = icon_cache_.find(key);
  Gdiplus::Bitmap* bmp = nullptr;
  if (it == icon_cache_.end()) {
    bmp = BuildPremultipliedIcon(icon, dest);
    if (!bmp) {
      return;
    }
    icon_cache_.emplace(key, bmp);
  } else {
    bmp = it->second;
  }
  // Override Paint's HighQualityBicubic — PARGB 1:1 must stay nearest.
  const Gdiplus::InterpolationMode prev = g.GetInterpolationMode();
  const Gdiplus::PixelOffsetMode prev_px = g.GetPixelOffsetMode();
  g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
  if (opacity >= 255) {
    g.DrawImage(bmp, x, y, dest, dest);
  } else {
    // PARGB cut ghost ≈0.42: scale RGB+A together (alpha-only matrix fringe).
    const Gdiplus::REAL a = static_cast<Gdiplus::REAL>(opacity) / 255.f;
    Gdiplus::ColorMatrix cm = {
        {{a, 0, 0, 0, 0},
         {0, a, 0, 0, 0},
         {0, 0, a, 0, 0},
         {0, 0, 0, a, 0},
         {0, 0, 0, 0, 1}}};
    Gdiplus::ImageAttributes attrs;
    attrs.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault,
                         Gdiplus::ColorAdjustTypeBitmap);
    const Gdiplus::Rect dest_rc(x, y, dest, dest);
    g.DrawImage(bmp, dest_rc, 0, 0, dest, dest, Gdiplus::UnitPixel, &attrs);
  }
  g.SetInterpolationMode(prev);
  g.SetPixelOffsetMode(prev_px);
}

bool PublicHost::Create(HINSTANCE instance) {
  if (!g_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;  // Explorer: double-click opens floats.
    wc.lpfnWndProc = PublicHost::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_reg = true;
  }
  metrics_ = QueryIconGridMetrics();
  const RECT work = PrimaryWorkArea();
  const int margin = ScalePxForDpi(kMargin0, SystemDpi());
  // Drop NOACTIVATE so Del/F2 work after click (Explorer DefView activates too).
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClass, L"Public", WS_POPUP,
      work.left + margin, work.top + margin, metrics_.cell_w,
      kHeaderH + metrics_.cell_h, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  EnsureDropTarget();
  return true;
}

void PublicHost::EnsureDropTarget() {
  if (!hwnd_ || drop_target_) {
    return;
  }
  drop_target_ = new PathDropTarget(
      [this](const std::vector<std::wstring>& paths, DWORD proposed) -> DWORD {
        if (on_paths_dropped_) {
          return on_paths_dropped_(paths);
        }
        // Fallback: Explorer same-volume MOVE / cross-volume COPY into Desktop.
        PWSTR desk_raw = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) ||
            !desk_raw) {
          return DROPEFFECT_NONE;
        }
        const std::wstring desk = desk_raw;
        CoTaskMemFree(desk_raw);
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        int ok = 0;
        int moved = 0;
        for (const auto& src : paths) {
          bool move = SameVolume(src, desk);
          if (ctrl) {
            move = false;
          }
          if (shift) {
            move = true;
          }
          std::wstring dest;
          if (MoveOrCopyFileIntoDir(hwnd_, src, desk, move, &dest)) {
            ++ok;
            if (move) {
              ++moved;
            }
          }
        }
        if (on_refresh_) {
          on_refresh_();
        }
        if (ok <= 0) {
          return DROPEFFECT_NONE;
        }
        if (moved == ok) {
          return DROPEFFECT_MOVE;
        }
        (void)proposed;
        return DROPEFFECT_COPY;
      });
  // AIGC START
  drop_target_->SetFileDropSemantics(FileDropSemantics::Physical);
  drop_target_->SetDestDirProvider([]() -> std::wstring {
    PWSTR desk_raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) ||
        !desk_raw) {
      return {};
    }
    std::wstring desk = desk_raw;
    CoTaskMemFree(desk_raw);
    return desk;
  });
  // AIGC END
  if (!RegisterPathDropTarget(hwnd_, drop_target_)) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
}

void PublicHost::EnsureCaptionFonts() {
  EnsureGdiplus();
  if (!caption_family_) {
    caption_family_ = new Gdiplus::FontFamily(L"Microsoft YaHei UI");
  }
  if (!caption_font_) {
    const Gdiplus::FontFamily* face =
        caption_family_->IsAvailable()
            ? caption_family_
            : Gdiplus::FontFamily::GenericSansSerif();
    caption_font_ = new Gdiplus::Font(face, 12.f, Gdiplus::FontStyleRegular,
                                      Gdiplus::UnitPixel);
  }
  if (!caption_fmt_) {
    caption_fmt_ = new Gdiplus::StringFormat();
    caption_fmt_->SetAlignment(Gdiplus::StringAlignmentCenter);
    caption_fmt_->SetLineAlignment(Gdiplus::StringAlignmentNear);
    caption_fmt_->SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
    caption_fmt_->SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);
  }
}

void PublicHost::ReleaseCaptionFonts() {
  delete caption_fmt_;
  caption_fmt_ = nullptr;
  delete caption_font_;
  caption_font_ = nullptr;
  delete caption_family_;
  caption_family_ = nullptr;
}

void PublicHost::Destroy() {
  if (rename_edit_) {
    FinishInlineRename(false);
  }
  if (tip_) {
    DestroyWindow(tip_);
    tip_ = nullptr;
  }
  tip_index_ = -1;
  tip_text_.clear();
  HideMarqueeBand();
  if (marquee_hwnd_ && IsWindow(marquee_hwnd_)) {
    DestroyWindow(marquee_hwnd_);
  }
  marquee_hwnd_ = nullptr;
  ClearRubberBaseCache();
  ReleasePresentBuffer();
  ReleaseCaptionFonts();
  ClearIconCache();
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
  rubber_ = false;
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

void PublicHost::Show() { SetVisible(true); }

void PublicHost::Hide() { SetVisible(false); }

void PublicHost::BeginDeferPresent() { defer_present_ = true; }

void PublicHost::EndDeferPresent() { defer_present_ = false; }

void PublicHost::ForceRemapPresent() {
  if (!hwnd_ || slots_.empty()) {
    return;
  }
  defer_present_ = false;
  want_visible_ = true;
  int drawable = 0;
  for (const auto& s : slots_) {
    if (s.icon) {
      ++drawable;
    }
  }
  if (drawable <= 0) {
    LogLine(L"[public] ForceRemapPresent abort slots=%d drawable=0",
            static_cast<int>(slots_.size()));
    return;
  }
  // Stay hidden while repairing the layered surface — toggling WS_EX_LAYERED
  // while visible caused the flash; skipping the reset left Win11 blank ULW.
  if (IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
    ShowWindow(hwnd_, SW_HIDE);
  }
  if (!IsAttachedToDesktop(hwnd_)) {
    if (!AttachOverlayToDesktop(hwnd_, /*show=*/false)) {
      ForceAttachOverlayToDesktop(hwnd_, /*show=*/false);
    }
  }
  {
    const LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex | WS_EX_LAYERED);
  }
  ReleasePresentBuffer();
  layer_dirty_ = true;
  // One visible transition: map then blit (pixels ready on first frame).
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  RaiseOverlayInDesktopBand(hwnd_);
  Present();
}

void PublicHost::ApplyVisibility() {
  if (!hwnd_) {
    return;
  }
  // Hide when nothing can actually be drawn: an empty slot list, or slots
  // whose icon failed to load. Either case used to leave a bare header plate
  // floating over the wallpaper that also swallowed clicks.
  bool drawable = false;
  for (const auto& s : slots_) {
    if (s.icon) {
      drawable = true;
      break;
    }
  }
  if (!want_visible_ || slots_.empty() || !drawable) {
    ShowWindow(hwnd_, SW_HIDE);
    return;
  }
  // During OLE-unpin defer: keep hidden + dirty. Mapping an empty ULW first
  // then Present was the visible flash; Reveal does one Show+Present.
  if (defer_present_) {
    layer_dirty_ = true;
    return;
  }
  // Map first, then ULW — never UpdateLayeredWindow while hidden (Win11
  // blank-after-Show; click Present was the only recovery).
  if (IsIconic(hwnd_)) {
    ShowWindow(hwnd_, SW_RESTORE);
  } else if (!IsWindowVisible(hwnd_)) {
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }
  AttachToDesktopLayer();  // no-op when already seated (avoids page flash)
  Present();
}

void PublicHost::SetVisible(bool on) {
  want_visible_ = on;
  ApplyVisibility();
}

void PublicHost::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  // Fences keepalive: Raise only when Z-order actually needs a move.
  if (!AttachOverlayToDesktop(hwnd_, true)) {
    ForceAttachOverlayToDesktop(hwnd_, true);
  }
  RaiseOverlayInDesktopBand(hwnd_);
}

void PublicHost::SetBlockedRects(std::vector<RECT> rects) {
  blocked_ = std::move(rects);
}

void PublicHost::Reflow() {
  if (!hwnd_ || slots_.empty()) {
    return;
  }
  Layout();
  ApplyVisibility();
}

void PublicHost::SetPlacements(FloatPlacementMap placements) {
  placements_ = std::move(placements);
}

void PublicHost::SetItems(std::vector<DesktopItem> items) {
  if (rename_edit_) {
    FinishInlineRename(false);
  }
  // Premultiplied bitmap cache is keyed by HICON — clear only when icons are
  // replaced/destroyed so same-seat desktop watches keep warm Present caches.
  std::vector<DesktopItem> before;
  before.reserve(slots_.size());
  for (const auto& s : slots_) {
    before.push_back(s.item);
  }
  if (!items.empty() && DesktopItemListsIdentityEqual(before, items) &&
      slots_.size() == items.size()) {
    bool all_icons = true;
    for (const auto& s : slots_) {
      if (!s.icon) {
        all_icons = false;
        break;
      }
    }
    if (all_icons) {
      return;
    }
  }
  std::unordered_map<std::wstring, HICON> reuse;
  reuse.reserve(slots_.size());
  for (auto& s : slots_) {
    if (s.icon) {
      reuse.emplace(NormalizePinKey(s.item.path), s.icon);
      s.icon = nullptr;
    }
  }
  slots_.clear();
  metrics_ = QueryIconGridMetrics();
  slots_.reserve(items.size());
  bool loaded_any = false;
  for (auto& item : items) {
    // Caller (RebuildPublicItems / ScanDesktopItems) already filtered live
    // paths — skip per-item GetFileAttributesW (large desktops hitch UI).
    Slot s;
    s.item = std::move(item);
    const std::wstring key = NormalizePinKey(s.item.path);
    auto hit = reuse.find(key);
    if (hit != reuse.end()) {
      s.icon = hit->second;
      reuse.erase(hit);
    } else if (ItemIconCached(s.item.path, metrics_.icon_size)) {
      s.icon = LoadItemIcon(s.item.path, metrics_.icon_size);
      loaded_any = true;
    } else {
      s.icon = nullptr;
      loaded_any = true;
    }
    slots_.push_back(std::move(s));
  }
  const bool destroyed_unused = !reuse.empty();
  for (auto& kv : reuse) {
    if (kv.second) {
      DestroyIcon(kv.second);
    }
  }
  std::vector<DesktopItem> after;
  after.reserve(slots_.size());
  for (const auto& s : slots_) {
    after.push_back(s.item);
  }
  const bool same_seats =
      !loaded_any && DesktopItemListsIdentityEqual(before, after);
  // Evict only the cache entries bound to destroyed HICONs (removed items).
  // Same-seat desktop watches keep their warm premultiplied bitmaps instead of
  // a full cache flush on every unrelated item change.
  if (destroyed_unused) {
    for (auto& kv : reuse) {
      if (!kv.second) {
        continue;
      }
      for (auto it = icon_cache_.begin(); it != icon_cache_.end();) {
        if (it->first.icon == kv.second) {
          delete it->second;
          it = icon_cache_.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  if (same_seats) {
    // Same icons: keep selection, but reflow — blocked fence rects / placements
    // are applied just before SetItems in RebuildPublicItems.
    Layout();
    ApplyVisibility();
    return;
  }
  selected_.clear();
  selection_anchor_ = -1;
  rubber_ = false;
  rubber_additive_ = false;
  HideMarqueeBand();
  ClearRubberBaseCache();
  EnsureSelSize();
  Layout();
  ApplyVisibility();
}

bool PublicHost::FillMissingIcons() {
  const int icon_sz = (std::max)(16, metrics_.icon_size);
  bool any = false;
  for (auto& s : slots_) {
    if (s.icon || s.item.path.empty()) {
      continue;
    }
    if (!ItemIconCached(s.item.path, icon_sz)) {
      continue;
    }
    s.icon = LoadItemIcon(s.item.path, icon_sz);
    if (s.icon) {
      any = true;
    }
  }
  if (!any) {
    return false;
  }
  if (hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    RefreshLayer();
  }
  return true;
}

void PublicHost::Layout() {
  if (!hwnd_ || slots_.empty()) {
    return;
  }
  metrics_ = QueryIconGridMetrics();
  const int cell_w = (std::max)(48, metrics_.cell_w);
  const int cell_h = (std::max)(56, metrics_.cell_h);
  const int icon = (std::max)(16, metrics_.icon_size);
  (void)icon;

  auto intersects_blocked = [](const std::vector<RECT>& blk, const RECT& slot) {
    for (const RECT& b : blk) {
      if (slot.left < b.right && slot.right > b.left && slot.top < b.bottom &&
          slot.bottom > b.top) {
        return true;
      }
    }
    return false;
  };

  // Scan every monitor work area (Fences: icons stay on the screen they land).
  // Prefer primary first — EnumDisplayMonitors often returns the left secondary
  // ahead of primary, so unpositioned new floats (AI/save-to-Desktop) used to
  // land off the main screen while fences stayed on primary (looks「没显示」).
  constexpr size_t kMaxCandidates = 768;
  std::vector<RECT> candidates;
  candidates.reserve((std::min)(kMaxCandidates, slots_.size() * 8 + 32));
  auto monitors = EnumMonitors();
  std::stable_partition(monitors.begin(), monitors.end(),
                        [](const MonitorInfo& m) { return m.primary; });
  int step_x = cell_w;
  int step_y = cell_h;
  for (const auto& mon : monitors) {
    const RECT& work = mon.work;
    POINT mid{(work.left + work.right) / 2, (work.top + work.bottom) / 2};
    const int dpi = DpiAtPoint(mid);
    const int margin = ScalePxForDpi(kMargin0, dpi);
    step_x = cell_w + ScalePxForDpi(kGapX0, dpi);
    step_y = cell_h + ScalePxForDpi(kGapY0, dpi);
    int search_bottom = work.bottom - margin;
    int search_right = work.right - margin;
    for (const RECT& b : blocked_) {
      if (b.left >= work.right || b.right <= work.left ||
          b.top >= work.bottom || b.bottom <= work.top) {
        continue;
      }
      search_bottom =
          (std::max)(search_bottom, static_cast<int>(b.bottom) + step_y);
      search_right =
          (std::max)(search_right, static_cast<int>(b.right) + step_x);
    }
    search_bottom =
        (std::min)(search_bottom, static_cast<int>(work.bottom) + step_y * 8);
    search_right =
        (std::min)(search_right, static_cast<int>(work.right) + step_x * 4);
    // Pre-filter blocked rects to this monitor's search region: a rect that
    // provably cannot intersect any candidate is dropped from the inner scan
    // (results unchanged — candidates are all within [sx0,search_right]×[sy0,search_bottom]).
    const int sx0 = work.left + margin;
    const int sy0 = work.top + margin;
    std::vector<RECT> mon_blocked;
    mon_blocked.reserve(blocked_.size());
    for (const RECT& b : blocked_) {
      if (b.right <= sx0 || b.left >= search_right || b.bottom <= sy0 ||
          b.top >= search_bottom) {
        continue;
      }
      mon_blocked.push_back(b);
    }
    for (int x = work.left + margin; x + cell_w <= search_right; x += step_x) {
      for (int y = work.top + margin; y + cell_h <= search_bottom;
           y += step_y) {
        const RECT slot{x, y, x + cell_w, y + cell_h};
        if (intersects_blocked(mon_blocked, slot)) {
          continue;
        }
        candidates.push_back(slot);
        if (candidates.size() >= kMaxCandidates) {
          break;
        }
      }
      if (candidates.size() >= kMaxCandidates) {
        break;
      }
    }
    if (candidates.size() >= kMaxCandidates) {
      break;
    }
  }
  if (candidates.empty()) {
    const RECT work = PrimaryWorkArea();
    const int margin = ScalePxForDpi(kMargin0, SystemDpi());
    candidates.push_back({work.left + margin, work.top + margin,
                          work.left + margin + cell_w,
                          work.top + margin + cell_h});
  }
  while (candidates.size() < slots_.size()) {
    RECT last = candidates.back();
    OffsetRect(&last, step_x, 0);
    candidates.push_back(last);
  }

  std::vector<char> used(candidates.size(), 0);
  std::vector<RECT> assigned(slots_.size());
  auto nearest_free = [&](POINT prefer) -> size_t {
    size_t best = SIZE_MAX;
    long long best_d = LLONG_MAX;
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (used[i]) {
        continue;
      }
      const RECT& c = candidates[i];
      const long long cx = (c.left + c.right) / 2;
      const long long cy = (c.top + c.bottom) / 2;
      const long long dx = cx - prefer.x;
      const long long dy = cy - prefer.y;
      const long long d = dx * dx + dy * dy;
      if (d < best_d) {
        best_d = d;
        best = i;
      }
    }
    return best;
  };

  // Pass 1: items with saved positions claim the nearest free cell.
  for (size_t i = 0; i < slots_.size(); ++i) {
    const auto it = placements_.find(NormalizePinKey(slots_[i].item.path));
    if (it == placements_.end() || !it->second.has_pos) {
      continue;
    }
    const size_t idx = nearest_free(FloatScreenPoint(it->second));
    if (idx == SIZE_MAX) {
      continue;
    }
    used[idx] = 1;
    assigned[i] = candidates[idx];
  }
  // Pass 2: remaining items take the next free cells in scan order.
  size_t next = 0;
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (assigned[i].right > assigned[i].left) {
      continue;
    }
    while (next < candidates.size() && used[next]) {
      ++next;
    }
    if (next >= candidates.size()) {
      assigned[i] = candidates.back();
      continue;
    }
    used[next] = 1;
    assigned[i] = candidates[next++];
  }

  // AIGC START
  // Fences-class: host stays a tight icon bbox. Marquee is a separate band
  // window — never SetWindowPos the icon host during multi-select (flash).
  LONG min_l = LONG_MAX;
  LONG min_t = LONG_MAX;
  LONG max_r = LONG_MIN;
  LONG max_b = LONG_MIN;
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& sc = assigned[i];
    min_l = (std::min)(min_l, sc.left);
    min_t = (std::min)(min_t, sc.top);
    max_r = (std::max)(max_r, sc.right);
    max_b = (std::max)(max_b, sc.bottom);
  }
  const int pad = (std::max)(48, cell_w / 2);
  const RECT work = PrimaryWorkArea();
  // Invariant: float host bounds must NEVER bleed into the taskbar or outside work area
  LONG hl = (std::max)(work.left, static_cast<LONG>(min_l - pad));
  LONG ht = (std::max)(work.top, static_cast<LONG>(min_t - pad));
  LONG hr = (std::min)(work.right, static_cast<LONG>(max_r + pad));
  LONG hb = (std::min)(work.bottom, static_cast<LONG>(max_b + pad));
  if (hr < hl + cell_w) hr = (std::min)(work.right, hl + cell_w);
  if (hb < ht + cell_h) hb = (std::min)(work.bottom, ht + cell_h);
  const RECT host_rc = {hl, ht, hr, hb};
  const int origin_x = host_rc.left;
  const int origin_y = host_rc.top;
  const int w =
      (std::max)(cell_w, static_cast<int>(host_rc.right - host_rc.left));
  const int h =
      (std::max)(cell_h, static_cast<int>(host_rc.bottom - host_rc.top));
  const int icon_pad = (cell_w - icon) / 2;
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& sc = assigned[i];
    const int cx = sc.left - origin_x;
    const int cy = sc.top - origin_y;
    slots_[i].bounds = {cx, cy, cx + cell_w, cy + cell_h};
    slots_[i].icon_rc = {cx + icon_pad, cy + 4, cx + icon_pad + icon,
                         cy + 4 + icon};
  }
  ClearRubberBaseCache();
  SetOverlayScreenPos(hwnd_, origin_x, origin_y, w, h,
                      SWP_NOZORDER | SWP_NOREDRAW);
  // AIGC END
  // Geometry only while hidden — Present after Show (ApplyVisibility).
  if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    Present();
  } else {
    layer_dirty_ = true;
  }
}

void PublicHost::PasteClipboardFiles() {
  if (on_paste_) {
    on_paste_();
    return;
  }
  if (PasteClipboardToDesktopFolder(hwnd_, nullptr) <= 0) {
    ShowToast(L"剪贴板里没有文件");
    return;
  }
  if (on_refresh_) {
    on_refresh_();
  }
}

void PublicHost::ShowBackgroundMenu(POINT screen) {
  PWSTR desk_raw = nullptr;
  std::wstring desk;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) &&
      desk_raw) {
    desk = desk_raw;
    CoTaskMemFree(desk_raw);
  }
  if (desk.empty()) {
    return;
  }
  const std::vector<ShellMenuExtra> extras{
      {L"刷新", kExtPublicRefresh},
      {L"对齐图标到网格", kExtPublicAlignGrid},
  };
  // Snapshot Desktop membership so New can SuppressOrganize (Fences: new
  // loose icons stay as floats; watch must not vacuum them into a fence).
  std::vector<std::wstring> desk_before;
  auto list_desk = [&desk]() {
    std::vector<std::wstring> out;
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
  ShellBeforeInvokeFn on_before = [&]() { desk_before = list_desk(); };
  // Fences / Ctrl+V path: Cut→blank desktop must release virtual pins.
  // Shell Paste alone is a Desktop self-MOVE no-op and leaves the pin.
  ShellInterceptFn on_intercept = [this](const std::wstring& verb) {
    if (verb == L"paste" || verb == L"pastelink" || verb == L"pasteto") {
      PasteClipboardFiles();
      return true;
    }
    return false;
  };
  ShellVerbFn on_verb = [this, &desk_before, &list_desk](const std::wstring& verb) {
    if (verb == L"paste" || verb == L"pastelink" || verb == L"pasteto") {
      return;  // handled by intercept
    }
    if (!ShellVerbNeedsDesktopRescan(verb)) {
      return;
    }
    if (verb == L"newfolder" || verb.rfind(L"new", 0) == 0) {
      const auto after = list_desk();
      for (const auto& p : after) {
        bool was = false;
        for (const auto& b : desk_before) {
          if (_wcsicmp(b.c_str(), p.c_str()) == 0) {
            was = true;
            break;
          }
        }
        if (!was) {
          // Match paste: keep New as a public float under auto-organize watch.
          SuppressOrganizePath(p, 24.0 * 3600.0);
        }
      }
    }
    if (on_refresh_) {
      on_refresh_();
    }
  };
  const int rc = TrackShellFolderBackgroundMenu(hwnd_, screen, desk, extras,
                                                on_verb, on_before, on_intercept);
  if (rc == kExtPublicRefresh && on_refresh_) {
    on_refresh_();
  } else if (rc == kExtPublicAlignGrid) {
    AlignIconsToGrid();
  }
}

void PublicHost::ShowMoveToPageFenceMenu(POINT screen) {
  auto paths = SelectedPaths();
  if (paths.empty() || !on_move_to_fence_) {
    return;
  }
  std::vector<MoveFenceMenuTarget> targets;
  if (on_query_fences_) {
    targets = on_query_fences_();
  }
  const std::string fence_id =
      TrackMoveToPageThenFenceMenu(hwnd_, screen, page_names_, targets);
  if (!fence_id.empty()) {
    on_move_to_fence_(paths, fence_id);
  }
}

void PublicHost::ShowSlotMenu(int index, POINT screen) {
  if (index < 0 || index >= static_cast<int>(slots_.size())) {
    return;
  }
  // Keep marquee multi-select; only collapse when the click was outside it.
  if (!IsSelected(index)) {
    SelectOnly(index);
  }
  auto paths = SelectedPaths();
  if (paths.empty()) {
    paths.push_back(slots_[static_cast<size_t>(index)].item.path);
  }
  // P1-7: full Explorer IContextMenu with DeskTidy extras (refresh + move page).
  // Operate on the whole selection (Fences / Explorer multi-select).
  const std::vector<ShellMenuExtra> extras{
      {Tr("menu_move_page"), kExtPublicMovePage},
      {L"刷新", kExtPublicRefresh},
  };
  // Fences: IContextMenu + quiet cancel; rescan only for mutating verbs.
  bool rename_picked = false;
  bool verb_ran = false;
  bool needs_rescan = false;
  std::wstring last_verb;
  ShellVerbFn on_verb = [&](const std::wstring& verb) {
    if (verb == L"rename") {
      rename_picked = true;
      return;
    }
    verb_ran = true;
    last_verb = verb;
    needs_rescan = ShellVerbNeedsDesktopRescan(verb);
  };
  const int shell_cmd =
      TrackShellContextMenu(hwnd_, screen, paths, extras, on_verb, nullptr);
  if (shell_cmd == kExtPublicMovePage) {
    ShowMoveToPageFenceMenu(screen);
    return;
  }
  if (shell_cmd == kExtPublicRefresh) {
    if (on_refresh_) {
      on_refresh_();
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
    bool any_gone = false;
    for (const auto& p : paths) {
      if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        any_gone = true;
        if (on_removed_) {
          on_removed_(p);
        }
      }
    }
    if ((needs_rescan || any_gone) && on_refresh_) {
      on_refresh_();
    } else if (last_verb == L"cut" || last_verb == L"copy") {
      Present();
    }
    return;
  }
  if (shell_cmd < 0) {
    // IContextMenu unavailable — fall back to the hand-rolled menu.
    const std::wstring& path = paths.front();
    const int cmd = TrackIconShellMenu(hwnd_, screen, false);
    switch (cmd) {
      case 1:
        OpenShellPath(path);
        break;
      case 2:
        RevealShellPath(path);
        break;
      case 4:
        CopyOrCutShellPath(hwnd_, path, false);
        break;
      case 5:
        CopyOrCutShellPath(hwnd_, path, true);
        break;
      case 6:
        CopyShellPathText(hwnd_, path);
        break;
      case 7:
        BeginInlineRename();
        break;
      case 8:
        ShowShellPathProperties(hwnd_, path);
        break;
      case 9:
        if (RecycleShellPath(hwnd_, path) == IconFsResult::Ok && on_removed_) {
          on_removed_(path);
        }
        break;
      default:
        break;
    }
  }
}

int PublicHost::HitTest(int x, int y) const {
  // Fences / Explorer: only glyph + caption are "the icon". Cell padding and
  // empty host chrome stay blank so rubber-band multi-select can start
  // (full-cell hit made nearly every pixel an icon — no batch select).
  const int font_px = 12;
  const int line_h = font_px + 5;
  const int label_h = line_h * 2 + 4;
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& b = slots_[i].bounds;
    const RECT& ir = slots_[i].icon_rc;
    const int content_l = (std::min)(ir.left, b.left + 2);
    const int content_t = (std::min)(ir.top, b.top + 2);
    const int content_r = (std::max)(ir.right, b.right - 2);
    const int content_b =
        (std::min)(b.bottom - 2, ir.bottom + 2 + label_h);
    if (x >= content_l && x < content_r && y >= content_t && y < content_b) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void PublicHost::PersistSlotPlacements() {
  if (!hwnd_ || slots_.empty()) {
    return;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  for (const auto& s : slots_) {
    POINT mid{(s.bounds.left + s.bounds.right) / 2,
              (s.bounds.top + s.bounds.bottom) / 2};
    mid.x += wr.left;
    mid.y += wr.top;
    UpsertFloatPlacement(&placements_, s.item.path, current_page_, mid);
  }
  if (on_placements_changed_) {
    on_placements_changed_(placements_);
  }
}

void PublicHost::AlignIconsToGrid() {
  // Explorer 「对齐图标到网格」/ Fences: keep relative seats, snap each float
  // to the nearest free Explorer grid cell (fence rects stay blocked).
  if (!hwnd_ || slots_.empty()) {
    return;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  for (const auto& s : slots_) {
    POINT mid{(s.bounds.left + s.bounds.right) / 2 + wr.left,
              (s.bounds.top + s.bounds.bottom) / 2 + wr.top};
    UpsertFloatPlacement(&placements_, s.item.path, current_page_, mid);
  }
  Layout();
  PersistSlotPlacements();
  if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    Present();
  }
}

void PublicHost::ReorderTo(int from, int to) {
  if (from < 0 || to < 0 || from >= static_cast<int>(slots_.size()) ||
      to >= static_cast<int>(slots_.size()) || from == to) {
    return;
  }
  // Swap preferred positions so Layout keeps visual order after persist.
  RECT wa = slots_[static_cast<size_t>(from)].bounds;
  RECT wb = slots_[static_cast<size_t>(to)].bounds;
  Slot moving = std::move(slots_[static_cast<size_t>(from)]);
  slots_.erase(slots_.begin() + from);
  slots_.insert(slots_.begin() + to, std::move(moving));
  // Assign screen midpoints of the two cells before reflow.
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  auto mid_of = [&](const RECT& b) {
    POINT p{(b.left + b.right) / 2 + wr.left, (b.top + b.bottom) / 2 + wr.top};
    return p;
  };
  if (from < static_cast<int>(slots_.size()) &&
      to < static_cast<int>(slots_.size())) {
    // After erase/insert, `to` holds the moved item; update both ends from
    // the pre-move cell rects so nearest-slot layout sticks.
    UpsertFloatPlacement(&placements_, slots_[static_cast<size_t>(to)].item.path,
                         current_page_, mid_of(wb));
    if (from != to && from < static_cast<int>(slots_.size())) {
      // Best-effort: leave others; PersistSlotPlacements after Layout.
      (void)wa;
    }
  }
  Layout();
  PersistSlotPlacements();
}

bool PublicHost::HasSelection() const {
  for (char c : selected_) {
    if (c) {
      return true;
    }
  }
  return false;
}

void PublicHost::EnsureSelSize() {
  if (selected_.size() != slots_.size()) {
    selected_.assign(slots_.size(), 0);
  }
}

void PublicHost::ClearSelection() {
  EnsureSelSize();
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  selection_anchor_ = -1;
}

void PublicHost::SelectOnly(int index) {
  EnsureSelSize();
  ClearSelection();
  if (index >= 0 && index < static_cast<int>(selected_.size())) {
    selected_[static_cast<size_t>(index)] = 1;
    selection_anchor_ = index;
  }
}

void PublicHost::ToggleSelected(int index) {
  EnsureSelSize();
  if (index < 0 || index >= static_cast<int>(selected_.size())) {
    return;
  }
  selected_[static_cast<size_t>(index)] =
      selected_[static_cast<size_t>(index)] ? 0 : 1;
  selection_anchor_ = index;
}

void PublicHost::SelectRange(int from, int to) {
  EnsureSelSize();
  if (from > to) {
    std::swap(from, to);
  }
  from = (std::max)(0, from);
  to = (std::min)(to, static_cast<int>(selected_.size()) - 1);
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  for (int i = from; i <= to; ++i) {
    selected_[static_cast<size_t>(i)] = 1;
  }
}

void PublicHost::SelectIntersecting(RECT client_rc) {
  EnsureSelSize();
  if (client_rc.left > client_rc.right) {
    std::swap(client_rc.left, client_rc.right);
  }
  if (client_rc.top > client_rc.bottom) {
    std::swap(client_rc.top, client_rc.bottom);
  }
  // Ctrl+rubber keeps prior selection (Explorer / Fences additive marquee).
  if (!rubber_additive_) {
    std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
    selection_anchor_ = -1;
  }
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& b = slots_[i].bounds;
    if (b.left < client_rc.right && b.right > client_rc.left &&
        b.top < client_rc.bottom && b.bottom > client_rc.top) {
      selected_[i] = 1;
      if (selection_anchor_ < 0) {
        selection_anchor_ = static_cast<int>(i);
      }
    }
  }
}

// AIGC START
void PublicHost::ReleasePresentBuffer() {
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
  present_w_ = 0;
  present_h_ = 0;
}

bool PublicHost::EnsurePresentBuffer(int w, int h) {
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (present_dc_ && present_dib_ && present_bits_ && present_w_ == w &&
      present_h_ == h) {
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
  HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
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
  present_w_ = w;
  present_h_ = h;
  return true;
}

void PublicHost::ClearRubberBaseCache() {
  if (rubber_base_dib_) {
    DeleteObject(rubber_base_dib_);
    rubber_base_dib_ = nullptr;
  }
  rubber_base_bits_ = nullptr;
  rubber_base_w_ = 0;
  rubber_base_h_ = 0;
}

bool PublicHost::SelectionFingerprint(std::vector<char>* out) const {
  if (!out) {
    return false;
  }
  *out = selected_;
  return true;
}

void PublicHost::HideMarqueeBand() {
  if (marquee_hwnd_ && IsWindow(marquee_hwnd_)) {
    ShowWindow(marquee_hwnd_, SW_HIDE);
  }
}

void PublicHost::PresentMarqueeBand() {
  if (!hwnd_ || !rubber_) {
    HideMarqueeBand();
    return;
  }
  RECT r = rubber_rc_;
  if (r.left > r.right) {
    std::swap(r.left, r.right);
  }
  if (r.top > r.bottom) {
    std::swap(r.top, r.bottom);
  }
  POINT tl{r.left, r.top};
  POINT br{r.right, r.bottom};
  ClientToScreen(hwnd_, &tl);
  ClientToScreen(hwnd_, &br);
  const int bw = br.x - tl.x;
  const int bh = br.y - tl.y;
  if (bw < 2 || bh < 2) {
    HideMarqueeBand();
    return;
  }
  static bool marquee_class = false;
  if (!marquee_class) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kMarqueeClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    marquee_class = true;
  }
  if (!marquee_hwnd_ || !IsWindow(marquee_hwnd_)) {
    marquee_hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOPMOST,
        kMarqueeClass, L"", WS_POPUP, tl.x, tl.y, bw, bh, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!marquee_hwnd_) {
      return;
    }
  }
  SetWindowPos(marquee_hwnd_, HWND_TOPMOST, tl.x, tl.y, bw, bh,
               SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW);
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = bw;
  bi.bmiHeader.biHeight = -bh;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ old = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(bw) * static_cast<size_t>(bh) * 4);
  EnsureGdiplus();
  {
    Gdiplus::Graphics g(mem);
    Gdiplus::SolidBrush fill(Gdiplus::Color(40, 0, 120, 215));
    Gdiplus::Pen pen(Gdiplus::Color(200, 0, 120, 215), 1.f);
    g.FillRectangle(&fill, 0, 0, bw, bh);
    g.DrawRectangle(&pen, 0, 0, bw - 1, bh - 1);
  }
  POINT origin{tl.x, tl.y};
  SIZE size{bw, bh};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateOverlayLayered(marquee_hwnd_, screen, &origin, &size, mem, &src, 0,
                      &blend, ULW_ALPHA);
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

bool PublicHost::EnsureRubberBaseCache(int w, int h) {
  if (rubber_base_dib_ && rubber_base_w_ == w && rubber_base_h_ == h &&
      rubber_base_bits_) {
    return true;
  }
  ClearRubberBaseCache();
  HDC screen = GetDC(nullptr);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    ReleaseDC(nullptr, screen);
    return false;
  }
  HDC mem = CreateCompatibleDC(screen);
  HGDIOBJ old = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  // Base = hit plate + icons + captions (no selection / rubber).
  Paint(mem, /*draw_selection=*/false, /*draw_rubber=*/false);
  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  rubber_base_dib_ = dib;
  rubber_base_bits_ = bits;
  rubber_base_w_ = w;
  rubber_base_h_ = h;
  return true;
}

void PublicHost::BeginRubberFromScreen(POINT screen, bool additive) {
  if (!hwnd_ || !IsWindowVisible(hwnd_) || slots_.empty()) {
    return;
  }
  // Do NOT EnterMarqueeCover on button-down — click-to-deselect on DefView
  // blank was Expand→Present→Leave→Present and flashed icons. Cover expands
  // only after the drag passes threshold (same as in-host blank chrome).
  POINT client = screen;
  ScreenToClient(hwnd_, &client);
  if (HitTest(client.x, client.y) >= 0) {
    return;
  }
  SetFocus(hwnd_);
  drag_from_ = -1;
  drag_moved_ = false;
  const bool had_sel = HasSelection();
  if (!additive) {
    ClearSelection();
  }
  rubber_ = true;
  rubber_additive_ = additive;
  rubber_origin_ = client;
  rubber_rc_ = {client.x, client.y, client.x, client.y};
  if (had_sel && !additive) {
    Present();
  }
  SetCapture(hwnd_);
}
// AIGC END

std::vector<std::wstring> PublicHost::SelectedPaths() const {
  std::vector<std::wstring> paths;
  const size_t n = (std::min)(selected_.size(), slots_.size());
  for (size_t i = 0; i < n; ++i) {
    if (selected_[i]) {
      paths.push_back(slots_[i].item.path);
    }
  }
  return paths;
}

std::vector<std::pair<std::wstring, RECT>> PublicHost::SlotScreenRects() const {
  std::vector<std::pair<std::wstring, RECT>> out;
  if (!hwnd_) {
    return out;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  out.reserve(slots_.size());
  for (const auto& s : slots_) {
    RECT r{wr.left + s.bounds.left, wr.top + s.bounds.top,
           wr.left + s.bounds.right, wr.top + s.bounds.bottom};
    out.emplace_back(s.item.path, r);
  }
  return out;
}

void PublicHost::CopySelection() {
  const auto paths = SelectedPaths();
  if (!paths.empty() && ClipboardSetFiles(hwnd_, paths, false)) {
    ShowToast(L"已复制到剪贴板");
    // Clear cut-ghost dimming after Preferred DropEffect flips to copy.
    Present();
  }
}

int PublicHost::FocusedSlotIndex() const {
  if (selection_anchor_ >= 0 &&
      selection_anchor_ < static_cast<int>(slots_.size())) {
    return selection_anchor_;
  }
  for (size_t i = 0; i < selected_.size() && i < slots_.size(); ++i) {
    if (selected_[i]) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void PublicHost::SelectBetween(int from, int to) {
  EnsureSelSize();
  if (from < 0 || to < 0 || from >= static_cast<int>(slots_.size()) ||
      to >= static_cast<int>(slots_.size())) {
    return;
  }
  const RECT& a = slots_[static_cast<size_t>(from)].bounds;
  const RECT& b = slots_[static_cast<size_t>(to)].bounds;
  const RECT box{(std::min)(a.left, b.left), (std::min)(a.top, b.top),
                 (std::max)(a.right, b.right), (std::max)(a.bottom, b.bottom)};
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& r = slots_[i].bounds;
    if (!(r.right < box.left || r.left > box.right || r.bottom < box.top ||
          r.top > box.bottom)) {
      selected_[i] = 1;
    }
  }
  selection_anchor_ = from;
}

void PublicHost::NavigateSelection(int dx, int dy, bool extend) {
  if (slots_.empty()) {
    return;
  }
  int cur = FocusedSlotIndex();
  if (cur < 0) {
    SelectOnly(0);
    Present();
    return;
  }
  const RECT& cb = slots_[static_cast<size_t>(cur)].bounds;
  const int cx = (cb.left + cb.right) / 2;
  const int cy = (cb.top + cb.bottom) / 2;
  int best = -1;
  long long best_score = LLONG_MAX;
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (static_cast<int>(i) == cur) {
      continue;
    }
    const RECT& b = slots_[i].bounds;
    const int ox = (b.left + b.right) / 2 - cx;
    const int oy = (b.top + b.bottom) / 2 - cy;
    if (dx > 0 && ox <= 0) {
      continue;
    }
    if (dx < 0 && ox >= 0) {
      continue;
    }
    if (dy > 0 && oy <= 0) {
      continue;
    }
    if (dy < 0 && oy >= 0) {
      continue;
    }
    // Prefer primary-axis progress; penalize secondary-axis drift (Explorer).
    const long long primary = dx != 0 ? std::llabs(static_cast<long long>(ox))
                                      : std::llabs(static_cast<long long>(oy));
    const long long secondary = dx != 0
                                    ? std::llabs(static_cast<long long>(oy))
                                    : std::llabs(static_cast<long long>(ox));
    const long long score = primary * primary + secondary * secondary * 4;
    if (score < best_score) {
      best_score = score;
      best = static_cast<int>(i);
    }
  }
  if (best < 0) {
    return;
  }
  if (extend) {
    const int anchor = selection_anchor_ >= 0 ? selection_anchor_ : cur;
    SelectBetween(anchor, best);
  } else {
    SelectOnly(best);
  }
  Present();
}

void PublicHost::TypeAheadFind(wchar_t ch) {
  if (!ch || slots_.empty()) {
    return;
  }
  const DWORD now = GetTickCount();
  if (now - typeahead_tick_ > 1000) {
    typeahead_.clear();
  }
  typeahead_tick_ = now;
  typeahead_.push_back(static_cast<wchar_t>(towlower(ch)));

  auto lower_name = [](std::wstring s) {
    for (auto& c : s) {
      c = static_cast<wchar_t>(towlower(c));
    }
    return s;
  };
  auto matches = [&](size_t i) {
    const std::wstring n = lower_name(slots_[i].item.name);
    return n.size() >= typeahead_.size() &&
           n.compare(0, typeahead_.size(), typeahead_) == 0;
  };

  int start = selection_anchor_ + 1;
  if (start < 0 || start > static_cast<int>(slots_.size())) {
    start = 0;
  }
  for (int pass = 0; pass < 2; ++pass) {
    const int from = (pass == 0) ? start : 0;
    const int to =
        (pass == 0) ? static_cast<int>(slots_.size()) : start;
    for (int i = from; i < to; ++i) {
      if (matches(static_cast<size_t>(i))) {
        SelectOnly(i);
        Present();
        return;
      }
    }
  }
  if (typeahead_.size() > 1) {
    typeahead_.assign(1, static_cast<wchar_t>(towlower(ch)));
    for (size_t i = 0; i < slots_.size(); ++i) {
      if (matches(i)) {
        SelectOnly(static_cast<int>(i));
        Present();
        return;
      }
    }
  }
}

void PublicHost::HandlePublicHotkey(int vk, bool ctrl, bool shift) {
  if (rename_edit_) {
    return;
  }
  if (vk == VK_ESCAPE) {
    if (rubber_) {
      rubber_ = false;
      rubber_additive_ = false;
      HideMarqueeBand();
      if (GetCapture() == hwnd_) {
        ReleaseCapture();
      }
    }
    ClearSelection();
    typeahead_.clear();
    Present();
    return;
  }
  if (vk == VK_RETURN) {
    OpenSelection();
    return;
  }
  if (vk == VK_DELETE) {
    // Explorer: Del → recycle; Shift+Del → permanent.
    DeleteSelection(shift);
    return;
  }
  if (vk == VK_F2) {
    BeginInlineRename();
    return;
  }
  if (vk == VK_SPACE) {
    // Ctrl+Space is Peek — do not toggle selection here.
    if (ctrl) {
      return;
    }
    int idx = FocusedSlotIndex();
    if (idx < 0 || idx >= static_cast<int>(slots_.size())) {
      return;
    }
    SelectOnly(idx);
    Present();
    return;
  }
  if (ctrl && vk == L'C') {
    CopySelection();
    return;
  }
  if (ctrl && vk == L'X') {
    CutSelection();
    return;
  }
  if (ctrl && vk == L'V') {
    PasteClipboardFiles();
    return;
  }
  if (ctrl && vk == L'A') {
    EnsureSelSize();
    std::fill(selected_.begin(), selected_.end(), static_cast<char>(1));
    if (!slots_.empty()) {
      selection_anchor_ = 0;
    }
    Present();
    return;
  }
  if (vk == VK_HOME || vk == VK_END) {
    typeahead_.clear();
    if (slots_.empty()) {
      return;
    }
    int best = 0;
    long long best_key = vk == VK_HOME ? LLONG_MAX : LLONG_MIN;
    for (size_t i = 0; i < slots_.size(); ++i) {
      const RECT& b = slots_[i].bounds;
      const long long key =
          static_cast<long long>(b.top) * 100000LL + b.left;
      if (vk == VK_HOME) {
        if (key < best_key) {
          best_key = key;
          best = static_cast<int>(i);
        }
      } else if (key > best_key) {
        best_key = key;
        best = static_cast<int>(i);
      }
    }
    if (shift) {
      const int anchor =
          selection_anchor_ >= 0 ? selection_anchor_ : FocusedSlotIndex();
      SelectBetween(anchor < 0 ? best : anchor, best);
    } else {
      SelectOnly(best);
    }
    Present();
    return;
  }
  if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) {
    typeahead_.clear();
  }
  if (vk == VK_LEFT) {
    NavigateSelection(-1, 0, shift);
  } else if (vk == VK_RIGHT) {
    NavigateSelection(1, 0, shift);
  } else if (vk == VK_UP) {
    NavigateSelection(0, -1, shift);
  } else if (vk == VK_DOWN) {
    NavigateSelection(0, 1, shift);
  }
}

void PublicHost::CutSelection() {
  const auto paths = SelectedPaths();
  if (!paths.empty() && ClipboardSetFiles(hwnd_, paths, true)) {
    ShowToast(L"已剪切到剪贴板");
    Present();
  }
}

void PublicHost::DeleteSelection(bool permanent) {
  auto paths = SelectedPaths();
  if (paths.empty()) {
    return;
  }
  if (RecycleShellPaths(hwnd_, paths, permanent) != IconFsResult::Ok) {
    return;
  }
  for (const auto& p : paths) {
    if (on_removed_) {
      on_removed_(p);
    }
  }
  if (on_refresh_) {
    on_refresh_();
  }
}

void PublicHost::RenameSelection() {
  BeginInlineRename();
}

void PublicHost::EnsureTooltip() {
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

bool PublicHost::CaptionFitsLabel(const std::wstring& text,
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
  HFONT font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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

void PublicHost::UpdateHoverTip(int index) {
  EnsureTooltip();
  tip_index_ =
      (index >= 0 && index < static_cast<int>(slots_.size())) ? index : -1;
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
  const std::wstring& name = slots_[static_cast<size_t>(tip_index_)].item.name;
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
  SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x + 12, pt.y + 16));
  SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
}

namespace {
bool PublicRenameEditImeComposing(HWND hwnd) {
  HIMC himc = ImmGetContext(hwnd);
  if (!himc) {
    return false;
  }
  const LONG bytes =
      ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
  ImmReleaseContext(hwnd, himc);
  return bytes > 0;
}

LRESULT CALLBACK PublicRenameEditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
  auto* self =
      reinterpret_cast<PublicHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE)) {
    if (wparam == VK_RETURN && PublicRenameEditImeComposing(hwnd)) {
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

bool PublicHost::IconLabelRect(size_t index, RECT* out) const {
  if (!out || index >= slots_.size()) {
    return false;
  }
  const RECT& b = slots_[index].bounds;
  const RECT& ir = slots_[index].icon_rc;
  const int font_px = 12;
  const int line_h = font_px + 5;
  const int label_h = line_h * 2 + 4;
  out->left = b.left + 2;
  out->top = ir.bottom + 2;
  out->right = b.right - 2;
  out->bottom = (std::min)(b.bottom - 2, ir.bottom + 2 + label_h);
  return out->bottom > out->top && out->right > out->left;
}

void PublicHost::BeginInlineRename() {
  EnsureSelSize();
  int index = -1;
  int count = 0;
  for (size_t i = 0; i < selected_.size() && i < slots_.size(); ++i) {
    if (selected_[i]) {
      index = static_cast<int>(i);
      ++count;
    }
  }
  if (count != 1 || index < 0 || !hwnd_) {
    return;
  }
  if (rename_edit_) {
    DestroyWindow(rename_edit_);
    rename_edit_ = nullptr;
  }
  rename_index_ = index;
  RECT label{};
  if (!IconLabelRect(static_cast<size_t>(index), &label)) {
    label = slots_[static_cast<size_t>(index)].bounds;
  }
  POINT origin{label.left, label.top};
  ClientToScreen(hwnd_, &origin);
  const int edit_w =
      (std::max)(72, static_cast<int>(label.right - label.left));
  const int edit_h = (std::max)(22, 20);
  // Popup (not WS_CHILD): UpdateLayeredWindow hosts hide child EDIT controls.
  rename_edit_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"EDIT",
      slots_[static_cast<size_t>(index)].item.name.c_str(),
      WS_POPUP | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, origin.x, origin.y,
      edit_w, edit_h, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!rename_edit_) {
    return;
  }
  rename_proc_ = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(rename_edit_, GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(PublicRenameEditProc)));
  SetPropW(rename_edit_, L"oldproc", reinterpret_cast<HANDLE>(rename_proc_));
  SetWindowLongPtrW(rename_edit_, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(this));
  const std::wstring& name = slots_[static_cast<size_t>(index)].item.name;
  const std::wstring& path = slots_[static_cast<size_t>(index)].item.path;
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

void PublicHost::FinishInlineRename(bool commit) {
  if (!rename_edit_) {
    return;
  }
  HWND edit = rename_edit_;
  rename_edit_ = nullptr;
  wchar_t buf[260] = {};
  GetWindowTextW(edit, buf, 260);
  const int index = rename_index_;
  rename_index_ = -1;
  DestroyWindow(edit);
  if (!commit || index < 0 || index >= static_cast<int>(slots_.size())) {
    return;
  }
  const std::wstring old_path = slots_[static_cast<size_t>(index)].item.path;
  const std::wstring neu = buf;
  const RenamePlan plan = PlanRename(old_path, neu);
  if (!plan.valid) {
    if (!neu.empty()) {
      ShowToast(plan.error);
    }
    return;
  }
  if (GetFileAttributesW(plan.dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
    ShowToast(L"该名称已存在");
    return;
  }
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
  if (on_refresh_) {
    on_refresh_();
  }
}

void PublicHost::OpenSelection() {
  for (const auto& p : SelectedPaths()) {
    OpenShellPath(p);
  }
}

void PublicHost::BeginOleDrag() {
  std::vector<std::wstring> paths = SelectedPaths();
  if (paths.empty() && drag_from_ >= 0 &&
      drag_from_ < static_cast<int>(slots_.size())) {
    paths.push_back(slots_[static_cast<size_t>(drag_from_)].item.path);
  }
  if (paths.empty()) {
    return;
  }
  drag_from_ = -1;
  drag_moved_ = false;
  rubber_ = false;
  rubber_additive_ = false;
  HideMarqueeBand();
  if (GetCapture() == hwnd_) {
    ReleaseCapture();
  }
  // AIGC START
  // Fences/Explorer: shell drag the selection. Do not rewrite float geometry
  // aggressively after DoDragDrop — that stacked multi-select on one point or
  // erased placements so Refresh looked like「文件消失」.
  bool dropped = ShellDoDragFiles(paths, /*cancel_desktop_self_drop=*/true);
  POINT pt{};
  GetCursorPos(&pt);
  HWND under = WindowFromPoint(pt);
  for (HWND cur = under; cur; cur = GetParent(cur)) {
    if (cur == hwnd_ || cur == marquee_hwnd_) {
      // Dropped back on our float chrome — treat as cancel (no-op move).
      dropped = false;
      break;
    }
  }
  bool placements_dirty = false;
  for (const auto& p : paths) {
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
      EraseFloatPlacement(&placements_, p);
      placements_dirty = true;
      if (on_removed_) {
        on_removed_(p);
      }
      continue;
    }
    if (!dropped && paths.size() == 1) {
      // Single-icon cancel/desktop release: park under cursor (Explorer).
      UpsertFloatPlacement(&placements_, p, current_page_, pt);
      placements_dirty = true;
    }
    // Multi cancel: keep prior seats. Successful drop while file still exists
    // (e.g. pinned by a fence): RebuildPublicItems heals via FindPin — do not
    // EraseFloat here or icons vanish for a frame / forever if heal races.
  }
  if (placements_dirty && on_placements_changed_) {
    on_placements_changed_(placements_);
  }
  if (on_refresh_) {
    on_refresh_();
  }
  // AIGC END
}

LRESULT CALLBACK PublicHost::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  PublicHost* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<PublicHost*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<PublicHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT PublicHost::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_SYSCOMMAND && FilterDesktopChromeSysCommand(wparam)) {
    return 0;
  }
  if (msg == WM_WINDOWPOSCHANGING) {
    FilterDesktopChromeWindowPosChanging(reinterpret_cast<WINDOWPOS*>(lparam));
  }
  if (msg == WM_NCHITTEST) {
    // While capturing / rubber-banding, keep all moves.
    if (rubber_ || GetCapture() == hwnd_) {
      return HTCLIENT;
    }
    POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    POINT client = pt;
    ScreenToClient(hwnd_, &client);
    if (HitTest(client.x, client.y) >= 0) {
      return HTCLIENT;
    }
    // Invariant: Never intercept clicks outside the work area (e.g. taskbar, multi-mon borders)
    const RECT work = PrimaryWorkArea();
    if (pt.y >= work.bottom || pt.y < work.top || pt.x < work.left ||
        pt.x >= work.right) {
      return HTTRANSPARENT;
    }
    // Pass clicks through fence frames under our bounding box so partitions
    // stay clickable (host spans min→max float cells and often overlaps).
    for (const RECT& b : blocked_) {
      if (pt.x >= b.left && pt.x < b.right && pt.y >= b.top &&
          pt.y < b.bottom) {
        return HTTRANSPARENT;
      }
    }
    return HTCLIENT;
  }
  if (msg == WM_APP + 60) {
    const int vk = static_cast<int>(wparam);
    const bool ctrl = (lparam & 1) != 0;
    const bool shift = (lparam & 2) != 0;
    const bool typeahead = (lparam & 4) != 0;
    if (typeahead) {
      wchar_t ch = 0;
      if (vk >= 'A' && vk <= 'Z') {
        ch = static_cast<wchar_t>(vk);
      } else if (vk >= '0' && vk <= '9') {
        ch = static_cast<wchar_t>(vk);
      } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        ch = static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0));
      }
      if (ch) {
        TypeAheadFind(ch);
      }
      return 0;
    }
    HandlePublicHotkey(vk, ctrl, shift);
    return 0;
  }
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      Present();
      return 0;
    }
    case WM_LBUTTONDOWN: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      SetFocus(hwnd_);
      drag_from_ = HitTest(x, y);
      drag_moved_ = false;
      drag_press_ = {x, y};
      rubber_ = false;
      rubber_additive_ = false;
      const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      if (drag_from_ >= 0) {
        HideMarqueeBand();
        EnsureSelSize();
        if (shift && selection_anchor_ >= 0) {
          SelectRange(selection_anchor_, drag_from_);
        } else if (ctrl) {
          ToggleSelected(drag_from_);
        } else if (!IsSelected(drag_from_)) {
          SelectOnly(drag_from_);
        }
        Present();
        SetCapture(hwnd_);
      } else {
        // Explorer/Fences: blank click clears selection; host geometry fixed.
        const bool had_sel = HasSelection();
        if (!ctrl) {
          ClearSelection();
        }
        rubber_ = true;
        rubber_additive_ = ctrl;
        rubber_origin_ = {x, y};
        rubber_rc_ = {x, y, x, y};
        rubber_sel_finger_.clear();
        if (had_sel && !ctrl) {
          Present();
        }
        SetCapture(hwnd_);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      if ((wparam & MK_LBUTTON) == 0) {
        const int hit =
            HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        if (hit != tip_index_) {
          UpdateHoverTip(hit);
        } else if (hit >= 0 && tip_) {
          POINT pt{};
          GetCursorPos(&pt);
          SendMessageW(tip_, TTM_TRACKPOSITION, 0,
                       MAKELPARAM(pt.x + 12, pt.y + 16));
        }
        break;
      }
      UpdateHoverTip(-1);
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      if (rubber_) {
        rubber_rc_ = {rubber_origin_.x, rubber_origin_.y, x, y};
        const int bw = std::abs(rubber_rc_.right - rubber_rc_.left);
        const int bh = std::abs(rubber_rc_.bottom - rubber_rc_.top);
        if (bw >= kDragThreshold || bh >= kDragThreshold) {
          SelectIntersecting(rubber_rc_);
        }
        PresentRubberFast();
        return 0;
      }
      if (drag_from_ < 0) {
        break;
      }
      if (!drag_moved_ &&
          (std::abs(x - drag_press_.x) >= kDragThreshold ||
           std::abs(y - drag_press_.y) >= kDragThreshold)) {
        drag_moved_ = true;
        EnsureSelSize();
        if (!IsSelected(drag_from_)) {
          SelectOnly(drag_from_);
        }
      }
      if (!drag_moved_) {
        return 0;
      }
      // AIGC START
      // Fences/Explorer: multi-select → shell drag as soon as threshold is met
      // (do not wait for "outside host"; glyph padding used to fire OLE early
      // then wipe placements).
      int sel_count = 0;
      for (char c : selected_) {
        if (c) {
          ++sel_count;
        }
      }
      if (sel_count > 1) {
        BeginOleDrag();
        return 0;
      }
      const int hit = HitTest(x, y);
      POINT screen{x, y};
      ClientToScreen(hwnd_, &screen);
      RECT wr{};
      GetWindowRect(hwnd_, &wr);
      const bool outside_host =
          screen.x < wr.left || screen.x >= wr.right || screen.y < wr.top ||
          screen.y >= wr.bottom;
      if (outside_host || hit < 0) {
        BeginOleDrag();
        return 0;
      }
      if (hit != drag_from_) {
        ReorderTo(drag_from_, hit);
        drag_from_ = hit;
        SelectOnly(hit);
      }
      // AIGC END
      return 0;
    }
    case WM_LBUTTONUP: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      if (GetCapture() == hwnd_) {
        ReleaseCapture();
      }
      if (rubber_) {
        rubber_rc_ = {rubber_origin_.x, rubber_origin_.y, x, y};
        // Tiny drag = click blank, not a marquee (Explorer threshold).
        const int bw = std::abs(rubber_rc_.right - rubber_rc_.left);
        const int bh = std::abs(rubber_rc_.bottom - rubber_rc_.top);
        const bool real_marquee =
            bw >= kDragThreshold || bh >= kDragThreshold;
        if (real_marquee) {
          SelectIntersecting(rubber_rc_);
        }
        rubber_ = false;
        rubber_additive_ = false;
        HideMarqueeBand();
        if (real_marquee) {
          Present();
        }
        drag_from_ = -1;
        drag_moved_ = false;
        return 0;
      }
      if (!drag_moved_) {
        const int hit = HitTest(x, y);
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        // Explorer/Fences: plain click collapses; slow second click on the
        // *name* starts inline rename (glyph re-click keeps selection).
        if (hit >= 0 && !ctrl && !shift) {
          const bool was_selected = IsSelected(hit);
          const DWORD now = GetTickCount();
          const UINT dbl = GetDoubleClickTime();
          if (hit == last_icon_click_ &&
              now - last_icon_click_tick_ <= dbl) {
            last_icon_click_ = -1;
            last_icon_click_tick_ = 0;
            SelectOnly(hit);
            Present();
          } else {
            last_icon_click_ = hit;
            last_icon_click_tick_ = now;
            if (was_selected) {
              RECT label{};
              if (IconLabelRect(static_cast<size_t>(hit), &label) &&
                  x >= label.left && x < label.right && y >= label.top &&
                  y < label.bottom) {
                BeginInlineRename();
              } else {
                SelectOnly(hit);
                Present();
              }
            } else {
              SelectOnly(hit);
              Present();
            }
          }
        } else if (hit < 0) {
          last_icon_click_ = -1;
          last_icon_click_tick_ = 0;
          ClearSelection();
          Present();
        }
      }
      drag_from_ = -1;
      drag_moved_ = false;
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      last_icon_click_ = -1;
      last_icon_click_tick_ = 0;
      const int hit =
          HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0 && hit < static_cast<int>(slots_.size())) {
        SelectOnly(hit);
        OpenShellPath(slots_[static_cast<size_t>(hit)].item.path);
        Present();
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      POINT pt{};
      GetCursorPos(&pt);
      if (hit >= 0) {
        EnsureSelSize();
        if (!IsSelected(hit)) {
          SelectOnly(hit);
          Present();
        }
        ShowSlotMenu(hit, pt);
      } else {
        // Blank float chrome: Explorer desktop background menu (Paste / New…).
        ShowBackgroundMenu(pt);
      }
      return 0;
    }
    case WM_KEYDOWN: {
      const int vk = static_cast<int>(wparam);
      const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      HandlePublicHotkey(vk, ctrl, shift);
      return 0;
    }
    case WM_DESTROY:
      if (hwnd_) {
        RevokePathDropTarget(hwnd_);
      }
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

bool PublicHost::IsSelected(int index) const {
  return index >= 0 && index < static_cast<int>(selected_.size()) &&
         selected_[static_cast<size_t>(index)] != 0;
}

void PublicHost::Paint(HDC hdc, bool draw_selection, bool draw_rubber) {
  const int icon = (std::max)(16, metrics_.icon_size);
  EnsureGdiplus();
  // Process-lifetime brushes — Present/Paint allocates these every frame under
  // rubber-band / selection storms (many apps open → ULW tax).
  static Gdiplus::SolidBrush* s_hit = nullptr;
  static Gdiplus::SolidBrush* s_sel = nullptr;
  static Gdiplus::SolidBrush* s_rubber_fill = nullptr;
  static Gdiplus::Pen* s_rubber_pen = nullptr;
  if (!s_hit) {
    s_hit = new Gdiplus::SolidBrush(Gdiplus::Color(1, 0, 0, 0));
    s_sel = new Gdiplus::SolidBrush(Gdiplus::Color(90, 0, 120, 215));
    s_rubber_fill = new Gdiplus::SolidBrush(Gdiplus::Color(40, 0, 120, 215));
    s_rubber_pen = new Gdiplus::Pen(Gdiplus::Color(200, 0, 120, 215), 1.f);
  }
  {
    Gdiplus::Graphics g0(hdc);
    g0.Clear(Gdiplus::Color(0, 0, 0, 0));
    // ULW_ALPHA hit-tests by alpha: fully clear pixels never get mouse input,
    // so rubber-band multi-select on "blank" could not start. Alpha=1 plate is
    // invisible but hittable (Explorer/Fences-class loose-icon host).
    RECT wr{};
    GetClientRect(hwnd_, &wr);
    g0.FillRectangle(s_hit, 0, 0, static_cast<int>(wr.right - wr.left),
                     static_cast<int>(wr.bottom - wr.top));
  }
  // Cut ghost opacity matches fence / Python _CUT_GHOST_OPACITY ≈ 0.42.
  constexpr BYTE kCutOpacity = 107;
  const auto& cut_set = PublicCutPathSet();
  // Icons must be composited through GDI+ (see DrawIconCached) so the alpha
  // channel survives ULW_ALPHA; plain DrawIconEx left them invisible.
  {
    Gdiplus::Graphics gi(hdc);
    for (size_t i = 0; i < slots_.size(); ++i) {
      const auto& s = slots_[i];
      if (draw_selection && i < selected_.size() && selected_[i]) {
        gi.FillRectangle(s_sel,
                         static_cast<INT>(s.bounds.left),
                         static_cast<INT>(s.bounds.top),
                         static_cast<INT>(s.bounds.right - s.bounds.left),
                         static_cast<INT>(s.bounds.bottom - s.bounds.top));
      }
      if (s.icon) {
        const bool is_cut =
            !cut_set.empty() &&
            cut_set.find(PublicLowerPath(s.item.path)) != cut_set.end();
        DrawIconCached(gi, s.icon, s.icon_rc.left, s.icon_rc.top, icon,
                       is_cut ? kCutOpacity : 255);
      }
    }
    if (draw_rubber && rubber_) {
      RECT r = rubber_rc_;
      if (r.left > r.right) {
        std::swap(r.left, r.right);
      }
      if (r.top > r.bottom) {
        std::swap(r.top, r.bottom);
      }
      gi.FillRectangle(s_rubber_fill, static_cast<INT>(r.left),
                       static_cast<INT>(r.top),
                       static_cast<INT>(r.right - r.left),
                       static_cast<INT>(r.bottom - r.top));
      gi.DrawRectangle(s_rubber_pen, static_cast<INT>(r.left),
                       static_cast<INT>(r.top),
                       static_cast<INT>(r.right - r.left),
                       static_cast<INT>(r.bottom - r.top));
    }
  }
  // Captions: Explorer white + dark glow (readable on light wallpaper).
  EnsureCaptionFonts();
  Gdiplus::Graphics g(hdc);
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
  g.SetTextContrast(4);
  for (const auto& s : slots_) {
    const std::wstring& name = s.item.name.empty() ? s.item.path : s.item.name;
    Gdiplus::RectF box(
        static_cast<Gdiplus::REAL>(s.bounds.left + 2),
        static_cast<Gdiplus::REAL>(s.icon_rc.bottom + 2),
        static_cast<Gdiplus::REAL>(s.bounds.right - s.bounds.left - 4),
        static_cast<Gdiplus::REAL>(s.bounds.bottom - s.icon_rc.bottom - 2));
    const bool is_cut =
        !cut_set.empty() &&
        cut_set.find(PublicLowerPath(s.item.path)) != cut_set.end();
    DrawExplorerCaption(g, name, box, *caption_font_, *caption_fmt_,
                        is_cut ? kCutOpacity : 255);
  }
}

// AIGC START
void PublicHost::PresentRubberFast() {
  if (!hwnd_ || !rubber_) {
    return;
  }
  // Fences: rubber band is its own overlay — icon host never moves.
  PresentMarqueeBand();

  const ULONGLONG now = GetTickCount64();
  if (rubber_present_ms_ && now - rubber_present_ms_ < 16) {
    return;
  }
  rubber_present_ms_ = now;

  std::vector<char> finger;
  SelectionFingerprint(&finger);
  if (finger == rubber_sel_finger_) {
    return;
  }
  rubber_sel_finger_ = std::move(finger);

  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  const int w = (std::max)(1, static_cast<int>(wr.right - wr.left));
  const int h = (std::max)(1, static_cast<int>(wr.bottom - wr.top));
  if (!EnsureRubberBaseCache(w, h)) {
    HDC screen = GetDC(nullptr);
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
    if (dib && bits) {
      HGDIOBJ old = SelectObject(mem, dib);
      std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
      Paint(mem, true, /*draw_rubber=*/false);
      POINT origin{wr.left, wr.top};
      SIZE size{w, h};
      POINT src{0, 0};
      BLENDFUNCTION blend{};
      blend.BlendOp = AC_SRC_OVER;
      blend.SourceConstantAlpha = 255;
      blend.AlphaFormat = AC_SRC_ALPHA;
      UpdateOverlayLayered(hwnd_, screen, &origin, &size, mem, &src, 0, &blend,
                          ULW_ALPHA);
      SelectObject(mem, old);
      DeleteObject(dib);
    } else if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return;
  }

  // Reuse the cached Present back-buffer for the composite (base + selection)
  // instead of a fresh CreateCompatibleDC + CreateDIBSection every frame.
  if (!EnsurePresentBuffer(w, h)) {
    return;
  }
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return;
  }
  HDC mem = present_dc_;
  const size_t nbytes =
      static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
  std::memcpy(present_bits_, rubber_base_bits_, nbytes);
  EnsureGdiplus();
  {
    static Gdiplus::SolidBrush* s_sel_fast = nullptr;
    if (!s_sel_fast) {
      s_sel_fast = new Gdiplus::SolidBrush(Gdiplus::Color(90, 0, 120, 215));
    }
    Gdiplus::Graphics gi(mem);
    for (size_t i = 0; i < slots_.size(); ++i) {
      if (i >= selected_.size() || !selected_[i]) {
        continue;
      }
      const auto& s = slots_[i];
      gi.FillRectangle(s_sel_fast, static_cast<INT>(s.bounds.left),
                       static_cast<INT>(s.bounds.top),
                       static_cast<INT>(s.bounds.right - s.bounds.left),
                       static_cast<INT>(s.bounds.bottom - s.bounds.top));
    }
  }
  POINT origin{wr.left, wr.top};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateOverlayLayered(hwnd_, screen, &origin, &size, mem, &src, 0, &blend,
                      ULW_ALPHA);
  ReleaseDC(nullptr, screen);
}
// AIGC END

void PublicHost::Present() {
  if (!hwnd_) {
    return;
  }
  if (defer_present_) {
    layer_dirty_ = true;
    return;
  }
  // Soft-park / pre-Show: never ULW while hidden (Win11 blank-after-Show).
  if (!IsWindowVisible(hwnd_) || IsIconic(hwnd_)) {
    layer_dirty_ = true;
    return;
  }
  if (rubber_) {
    PresentRubberFast();
    return;
  }
  HideMarqueeBand();
  ClearRubberBaseCache();
  rubber_sel_finger_.clear();
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  const int w = (std::max)(1, static_cast<int>(wr.right - wr.left));
  const int h = (std::max)(1, static_cast<int>(wr.bottom - wr.top));
  if (!EnsurePresentBuffer(w, h)) {
    return;
  }
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return;
  }
  HDC mem = present_dc_;
  std::memset(present_bits_, 0,
              static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  Paint(mem, /*draw_selection=*/true, /*draw_rubber=*/false);
  POINT origin{wr.left, wr.top};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  const BOOL ok =
      UpdateOverlayLayered(hwnd_, screen, &origin, &size, mem, &src, 0, &blend,
                           ULW_ALPHA)
          ? TRUE
          : FALSE;
  ReleaseDC(nullptr, screen);
  if (!ok) {
    LogLine(L"[public] UpdateLayeredWindow failed err=%lu size=%dx%d",
            GetLastError(), w, h);
    layer_dirty_ = true;
    return;
  }
  layer_dirty_ = false;
}

void PublicHost::RefreshLayer() {
  if (!hwnd_ || slots_.empty()) {
    return;
  }
  if (defer_present_) {
    layer_dirty_ = true;
    return;
  }
  // Map first when wanted, then blit. Raise/SoftSeat can clear ULW while
  // IsWindowVisible stays TRUE — always re-upload after seating.
  if (want_visible_) {
    if (IsIconic(hwnd_)) {
      ShowWindow(hwnd_, SW_RESTORE);
    } else if (!IsWindowVisible(hwnd_)) {
      ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    AttachToDesktopLayer();
  }
  if (want_visible_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    Present();
  } else if (want_visible_) {
    layer_dirty_ = true;
  }
}
