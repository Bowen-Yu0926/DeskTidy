#include "public_host.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "i18n.h"
#include "icon_actions.h"
#include "shell_context_menu.h"

#include <Shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativePublic";
constexpr int kMargin = 20;
constexpr int kGapX = 0;
constexpr int kGapY = 6;
constexpr int kHeaderH = 26;   // P1-9: public-area title bar height.
constexpr int kExtPublicRefresh = 1;
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
// glyph pixels, so UpdateLayeredWindow(ULW_ALPHA) made the float icons fully
// transparent: only the header plate showed and clicks missed the icons.
// Compose through a premultiplied GDI+ bitmap (same approach as
// fence_window's DrawIconSharp, duplicated here to avoid a shared header
// change while that file is in flux).
void DrawIconAlpha(Gdiplus::Graphics& g, HICON icon, int x, int y, int dest) {
  if (!icon || dest <= 0) {
    return;
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
    return;
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
    p[0] = static_cast<BYTE>((p[0] * a) / 255);
    p[1] = static_cast<BYTE>((p[1] * a) / 255);
    p[2] = static_cast<BYTE>((p[2] * a) / 255);
    p[3] = a;
  }
  {
    Gdiplus::Bitmap bmp(src, src, src * 4, PixelFormat32bppPARGB,
                        static_cast<BYTE*>(bits));
    g.DrawImage(&bmp, x, y, dest, dest);
  }
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

// Explorer/DefView desktop caption (Python _qt_explorer_caption): white glyph
// + soft dark glow so names stay readable on light wallpaper. Plain near-white
// ink vanishes on pale desktops (「拖到桌面不显示名字」).
void DrawExplorerCaption(Gdiplus::Graphics& g, const std::wstring& text,
                         const Gdiplus::RectF& box, const Gdiplus::Font& font,
                         const Gdiplus::StringFormat& fmt) {
  if (text.empty()) {
    return;
  }
  auto paint_at = [&](BYTE a, float dx, float dy) {
    Gdiplus::SolidBrush br(Gdiplus::Color(a, 0, 0, 0));
    Gdiplus::RectF r = box;
    r.X += dx;
    r.Y += dy;
    g.DrawString(text.c_str(), -1, &font, r, &fmt, &br);
  };
  // Outer soft ring.
  for (int dy = -2; dy <= 2; ++dy) {
    for (int dx = -2; dx <= 2; ++dx) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      if (dx * dx + dy * dy > 4) {
        continue;
      }
      paint_at(70, static_cast<float>(dx), static_cast<float>(dy));
    }
  }
  // Inner ring.
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      paint_at(110, static_cast<float>(dx), static_cast<float>(dy));
    }
  }
  Gdiplus::SolidBrush ink(Gdiplus::Color(255, 255, 255, 255));
  g.DrawString(text.c_str(), -1, &font, box, &fmt, &ink);
}
}  // namespace

bool PublicHost::Create(HINSTANCE instance) {
  if (!g_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PublicHost::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_reg = true;
  }
  metrics_ = QueryIconGridMetrics();
  const RECT work = PrimaryWorkArea();
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClass, L"Public",
      WS_POPUP, work.left + kMargin, work.top + kMargin, metrics_.cell_w,
      kHeaderH + metrics_.cell_h, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void PublicHost::Destroy() {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void PublicHost::Show() { SetVisible(true); }

void PublicHost::Hide() { SetVisible(false); }

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
  if (!IsWindowVisible(hwnd_)) {
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
  if (IsAttachedToDesktop(hwnd_)) {
    return;
  }
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
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

void PublicHost::SetItems(std::vector<DesktopItem> items) {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
  metrics_ = QueryIconGridMetrics();
  slots_.reserve(items.size());
  for (auto& item : items) {
    if (GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      continue;
    }
    Slot s;
    s.item = std::move(item);
    s.icon = LoadItemIcon(s.item.path, metrics_.icon_size);
    slots_.push_back(std::move(s));
  }
  Layout();
  ApplyVisibility();
}

void PublicHost::Layout() {
  if (!hwnd_ || slots_.empty()) {
    return;
  }
  metrics_ = QueryIconGridMetrics();
  const int cell_w = (std::max)(48, metrics_.cell_w);
  const int cell_h = (std::max)(56, metrics_.cell_h);
  const int icon = (std::max)(16, metrics_.icon_size);
  const RECT work = PrimaryWorkArea();
  const int step_x = cell_w + kGapX;
  const int step_y = cell_h + kGapY;

  auto intersects_blocked = [this](const RECT& slot) {
    for (const RECT& b : blocked_) {
      if (slot.left < b.right && slot.right > b.left && slot.top < b.bottom &&
          slot.bottom > b.top) {
        return true;
      }
    }
    return false;
  };

  // Expand search region below/right of fences when the work area is crowded
  // (never place into a blocked cell — Python find_auto_slot parity).
  int search_bottom = work.bottom - kMargin;
  int search_right = work.right - kMargin;
  for (const RECT& b : blocked_) {
    search_bottom = (std::max)(search_bottom, static_cast<int>(b.bottom) + step_y);
    search_right = (std::max)(search_right, static_cast<int>(b.right) + step_x);
  }
  // Cap to virtual screen so we don't walk forever.
  const RECT virt = VirtualScreenRect();
  search_bottom =
      (std::min)(search_bottom, static_cast<int>(virt.bottom) - kMargin);
  search_right =
      (std::min)(search_right, static_cast<int>(virt.right) - kMargin);

  std::vector<RECT> free_slots;
  free_slots.reserve(slots_.size() + 8);
  for (int x = work.left + kMargin; x + cell_w <= search_right; x += step_x) {
    for (int y = work.top + kMargin; y + cell_h <= search_bottom; y += step_y) {
      const RECT slot{x, y, x + cell_w, y + cell_h};
      if (intersects_blocked(slot)) {
        continue;
      }
      free_slots.push_back(slot);
      if (free_slots.size() >= slots_.size()) {
        break;
      }
    }
    if (free_slots.size() >= slots_.size()) {
      break;
    }
  }

  // Last resort: stack just below the lowest fence (still outside blocked).
  if (free_slots.size() < slots_.size()) {
    int y0 = work.top + kMargin;
    for (const RECT& b : blocked_) {
      y0 = (std::max)(y0, static_cast<int>(b.bottom) + kGapY);
    }
    int x = work.left + kMargin;
    int y = y0;
    while (free_slots.size() < slots_.size()) {
      RECT slot{x, y, x + cell_w, y + cell_h};
      if (!intersects_blocked(slot)) {
        free_slots.push_back(slot);
      }
      y += step_y;
      if (y + cell_h > search_bottom) {
        y = y0;
        x += step_x;
        if (x + cell_w > search_right) {
          break;
        }
      }
    }
  }

  if (free_slots.empty()) {
    // Absolute fallback: one cell at work origin (should be rare).
    free_slots.push_back({work.left + kMargin, work.top + kMargin,
                          work.left + kMargin + cell_w,
                          work.top + kMargin + cell_h});
  }
  while (free_slots.size() < slots_.size()) {
    free_slots.push_back(free_slots.back());
  }

  LONG min_l = LONG_MAX;
  LONG min_t = LONG_MAX;
  LONG max_r = LONG_MIN;
  LONG max_b = LONG_MIN;
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& sc = free_slots[i];
    min_l = (std::min)(min_l, sc.left);
    min_t = (std::min)(min_t, sc.top);
    max_r = (std::max)(max_r, sc.right);
    max_b = (std::max)(max_b, sc.bottom);
  }
  // Host hugs placed cells only — never a transparent plate over fences.
  const int origin_x = static_cast<int>(min_l);
  const int origin_y = static_cast<int>(min_t) - kHeaderH;
  const int w =
      (std::max)(kHeaderH, static_cast<int>(max_r) - origin_x);
  const int h = (std::max)(kHeaderH + cell_h,
                           static_cast<int>(max_b) - origin_y);
  SetWindowPos(hwnd_, nullptr, origin_x, origin_y, w, h,
               SWP_NOZORDER | SWP_NOACTIVATE);

  const int icon_pad = (cell_w - icon) / 2;
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& sc = free_slots[i];
    const int cx = sc.left - origin_x;
    const int cy = sc.top - origin_y;
    slots_[i].bounds = {cx, cy, cx + cell_w, cy + cell_h};
    slots_[i].icon_rc = {cx + icon_pad, cy + 4, cx + icon_pad + icon,
                         cy + 4 + icon};
  }
  Present();
}

void PublicHost::ShowSlotMenu(int index, POINT screen) {
  if (index < 0 || index >= static_cast<int>(slots_.size())) {
    return;
  }
  Slot& slot = slots_[static_cast<size_t>(index)];
  const std::wstring path = slot.item.path;
  // P1-7: full Explorer IContextMenu (打开/剪切/复制/重命名/删除/属性/发送到…)
  // with a DeskTidy "刷新" extra — consistent with fence icons, no hand-rolled
  // 6-item menu.
  const std::vector<ShellMenuExtra> extras{{L"刷新", kExtPublicRefresh}};
  ShellVerbFn on_verb = [this](const std::wstring& verb) {
    if (verb == L"refresh" && on_refresh_) {
      on_refresh_();
    }
  };
  const int shell_cmd = TrackShellContextMenu(hwnd_, screen, {path}, extras,
                                              on_verb, nullptr);
  if (shell_cmd == kExtPublicRefresh) {
    if (on_refresh_) {
      on_refresh_();
    }
    return;
  }
  if (shell_cmd == 0) {
    // Shell verb ran (or cancel). Drop floats for paths that vanished and
    // re-scan so rename/delete/new-file land correctly.
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        on_removed_) {
      on_removed_(path);
    }
    if (on_refresh_) {
      on_refresh_();
    }
    return;
  }
  if (shell_cmd < 0) {
    // IContextMenu unavailable — fall back to the hand-rolled menu.
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
      case 7: {
        std::wstring dest;
        if (RenameShellPath(hwnd_, path, &dest) == IconFsResult::Ok) {
          if (on_path_replaced_ && dest != path) {
            on_path_replaced_(path, dest);
          }
          if (on_refresh_) {
            on_refresh_();
          }
        }
        break;
      }
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
  if (y < kHeaderH) {
    return -1;  // P1-9: header is not an icon slot.
  }
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& b = slots_[i].bounds;
    if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void PublicHost::ReorderTo(int from, int to) {
  if (from < 0 || to < 0 || from >= static_cast<int>(slots_.size()) ||
      to >= static_cast<int>(slots_.size()) || from == to) {
    return;
  }
  Slot moving = std::move(slots_[static_cast<size_t>(from)]);
  slots_.erase(slots_.begin() + from);
  slots_.insert(slots_.begin() + to, std::move(moving));
  Layout();
  if (on_refresh_) {
    // Order is in-memory for this session; refresh rebuilds from scan.
    // Keep local order by not calling refresh — caller may persist later.
  }
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
  switch (msg) {
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
      drag_from_ = HitTest(x, y);
      drag_moved_ = false;
      if (drag_from_ >= 0) {
        SetCapture(hwnd_);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (drag_from_ < 0 || (wparam & MK_LBUTTON) == 0) {
        break;
      }
      const int hit =
          HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0 && hit != drag_from_) {
        ReorderTo(drag_from_, hit);
        drag_from_ = hit;
        drag_moved_ = true;
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      const int hit = HitTest(x, y);
      if (GetCapture() == hwnd_) {
        ReleaseCapture();
      }
      if (!drag_moved_ && hit >= 0) {
        OpenShellPath(slots_[static_cast<size_t>(hit)].item.path);
      }
      drag_from_ = -1;
      drag_moved_ = false;
      return 0;
    }
    case WM_RBUTTONUP: {
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0) {
        POINT pt{};
        GetCursorPos(&pt);
        ShowSlotMenu(hit, pt);
      }
      return 0;
    }
    case WM_DESTROY:
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void PublicHost::Paint(HDC hdc) {
  const int icon = (std::max)(16, metrics_.icon_size);
  // P1-9: header/title bar (matches fence chrome, anchors the float area).
  RECT wr{};
  GetClientRect(hwnd_, &wr);
  EnsureGdiplus();
  {
    Gdiplus::Graphics g0(hdc);
    g0.Clear(Gdiplus::Color(0, 0, 0, 0));
    g0.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g0.SetTextContrast(4);
    // Subtle plate so the title anchors; caption itself uses Explorer glow.
    Gdiplus::SolidBrush head(Gdiplus::Color(40, 15, 23, 42));
    g0.FillRectangle(&head, 0, 0, static_cast<int>(wr.right - wr.left),
                     kHeaderH);
    Gdiplus::FontFamily hf(L"Microsoft YaHei UI");
    const Gdiplus::FontFamily* hface =
        hf.IsAvailable() ? &hf : Gdiplus::FontFamily::GenericSansSerif();
    Gdiplus::Font hfont(hface, 11.f, Gdiplus::FontStyleBold,
                        Gdiplus::UnitPixel);
    Gdiplus::StringFormat hfmt;
    hfmt.SetAlignment(Gdiplus::StringAlignmentNear);
    hfmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    hfmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    Gdiplus::RectF hbox(8.f, 0.f,
                        static_cast<Gdiplus::REAL>(wr.right - wr.left - 16),
                        static_cast<Gdiplus::REAL>(kHeaderH));
    DrawExplorerCaption(g0, title_, hbox, hfont, hfmt);
  }
  // Icons must be composited through GDI+ (see DrawIconAlpha) so the alpha
  // channel survives ULW_ALPHA; plain DrawIconEx left them invisible.
  {
    Gdiplus::Graphics gi(hdc);
    for (const auto& s : slots_) {
      if (s.icon) {
        DrawIconAlpha(gi, s.icon, s.icon_rc.left, s.icon_rc.top, icon);
      }
    }
  }
  // Captions: Explorer white + dark glow (readable on light wallpaper).
  Gdiplus::Graphics g(hdc);
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
  g.SetTextContrast(4);
  Gdiplus::FontFamily family(L"Microsoft YaHei UI");
  const Gdiplus::FontFamily* face =
      family.IsAvailable() ? &family : Gdiplus::FontFamily::GenericSansSerif();
  Gdiplus::Font font(face, 12.f, Gdiplus::FontStyleRegular,
                     Gdiplus::UnitPixel);
  Gdiplus::StringFormat fmt;
  fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
  fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
  fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
  fmt.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);
  for (const auto& s : slots_) {
    const std::wstring& name = s.item.name.empty() ? s.item.path : s.item.name;
    Gdiplus::RectF box(
        static_cast<Gdiplus::REAL>(s.bounds.left + 2),
        static_cast<Gdiplus::REAL>(s.icon_rc.bottom + 2),
        static_cast<Gdiplus::REAL>(s.bounds.right - s.bounds.left - 4),
        static_cast<Gdiplus::REAL>(s.bounds.bottom - s.icon_rc.bottom - 2));
    DrawExplorerCaption(g, name, box, font, fmt);
  }
}

void PublicHost::Present() {
  if (!hwnd_) {
    return;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  const int w = (std::max)(1, static_cast<int>(wr.right - wr.left));
  const int h = (std::max)(1, static_cast<int>(wr.bottom - wr.top));
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
  std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  Paint(mem);
  POINT origin{wr.left, wr.top};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateLayeredWindow(hwnd_, screen, &origin, &size, mem, &src, 0, &blend,
                      ULW_ALPHA);
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}
