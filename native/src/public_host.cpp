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
#include <cstring>
#include <string>

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
  if (!want_visible_ || slots_.empty()) {
    ShowWindow(hwnd_, SW_HIDE);
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachToDesktopLayer();
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
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
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
  const int avail_h = (std::max)(
      1, static_cast<int>(work.bottom - work.top) - 2 * kMargin - kHeaderH);
  const int per_col = (std::max)(1, avail_h / (cell_h + kGapY));
  const int n = static_cast<int>(slots_.size());
  const int cols = (n + per_col - 1) / per_col;
  const int w = (std::max)(kHeaderH, cols * cell_w +
                                        (std::max)(0, cols - 1) * kGapX);
  const int h = kHeaderH + per_col * cell_h +
                (std::max)(0, per_col - 1) * kGapY;
  SetWindowPos(hwnd_, nullptr, work.left + kMargin, work.top + kMargin, w, h,
               SWP_NOZORDER | SWP_NOACTIVATE);
  const int icon_pad = (cell_w - icon) / 2;
  for (int i = 0; i < n; ++i) {
    const int col = i / per_col;
    const int row = i % per_col;
    const int cx = col * (cell_w + kGapX);
    const int cy = kHeaderH + row * (cell_h + kGapY);
    slots_[static_cast<size_t>(i)].bounds = {cx, cy, cx + cell_w, cy + cell_h};
    slots_[static_cast<size_t>(i)].icon_rc = {
        cx + icon_pad, cy + 4, cx + icon_pad + icon, cy + 4 + icon};
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
  {
    EnsureGdiplus();
    Gdiplus::Graphics g0(hdc);
    g0.Clear(Gdiplus::Color(0, 0, 0, 0));
    Gdiplus::SolidBrush head(Gdiplus::Color(28, 255, 255, 255));
    g0.FillRectangle(&head, 0, 0, static_cast<int>(wr.right - wr.left),
                     kHeaderH);
    Gdiplus::FontFamily hf(L"Segoe UI");
    Gdiplus::Font hfont(&hf, 10.f, Gdiplus::FontStyleBold,
                        Gdiplus::UnitPixel);
    Gdiplus::SolidBrush hbr(Gdiplus::Color(235, 245, 245, 245));
    Gdiplus::StringFormat hfmt;
    hfmt.SetAlignment(Gdiplus::StringAlignmentNear);
    hfmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    hfmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    Gdiplus::RectF hbox(8.f, 0.f,
                        static_cast<Gdiplus::REAL>(wr.right - wr.left - 16),
                        static_cast<Gdiplus::REAL>(kHeaderH));
    g0.DrawString(title_.c_str(), -1, &hfont, hbox, &hfmt, &hbr);
  }
  for (const auto& s : slots_) {
    if (s.icon) {
      DrawIconEx(hdc, s.icon_rc.left, s.icon_rc.top, s.icon, icon, icon, 0,
                 nullptr, DI_NORMAL);
    }
  }
  // P1-9: captions via GDI+ so layered alpha stays valid.
  EnsureGdiplus();
  Gdiplus::Graphics g(hdc);
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
  Gdiplus::FontFamily family(L"Segoe UI");
  Gdiplus::Font font(&family, 11.f, Gdiplus::FontStyleRegular,
                     Gdiplus::UnitPixel);
  Gdiplus::SolidBrush br(Gdiplus::Color(230, 245, 245, 245));
  Gdiplus::StringFormat fmt;
  fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
  fmt.SetLineAlignment(Gdiplus::StringAlignmentNear);
  fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
  for (const auto& s : slots_) {
    const std::wstring& name = s.item.name.empty() ? s.item.path : s.item.name;
    Gdiplus::RectF box(
        static_cast<Gdiplus::REAL>(s.bounds.left + 2),
        static_cast<Gdiplus::REAL>(s.icon_rc.bottom + 2),
        static_cast<Gdiplus::REAL>(s.bounds.right - s.bounds.left - 4),
        static_cast<Gdiplus::REAL>(s.bounds.bottom - s.icon_rc.bottom - 2));
    g.DrawString(name.c_str(), -1, &font, box, &fmt, &br);
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
