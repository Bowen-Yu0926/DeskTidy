#include "public_host.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "i18n.h"
#include "icon_actions.h"
#include "ole_drag.h"
#include "pin_store.h"
#include "shell_clipboard.h"
#include "shell_context_menu.h"
#include "toast.h"

#include <Shellapi.h>
#include <ShlObj.h>
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
  // Drop NOACTIVATE so Del/F2 work after click (Explorer DefView activates too).
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClass, L"Public", WS_POPUP,
      work.left + kMargin, work.top + kMargin, metrics_.cell_w,
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
  drop_target_ = new PathDropTarget([this](const std::vector<std::wstring>& paths) {
    if (on_paths_dropped_) {
      on_paths_dropped_(paths);
      return;
    }
    // Fallback: copy into Desktop then refresh floats.
    PWSTR desk_raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) ||
        !desk_raw) {
      return;
    }
    const std::wstring desk = desk_raw;
    CoTaskMemFree(desk_raw);
    for (const auto& src : paths) {
      std::wstring dest;
      MoveOrCopyFileIntoDir(hwnd_, src, desk, /*move=*/false, &dest);
    }
    if (on_refresh_) {
      on_refresh_();
    }
  });
  if (!RegisterPathDropTarget(hwnd_, drop_target_)) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
}

void PublicHost::Destroy() {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
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

void PublicHost::SetPlacements(FloatPlacementMap placements) {
  placements_ = std::move(placements);
}

void PublicHost::SetItems(std::vector<DesktopItem> items) {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
  selected_.clear();
  selection_anchor_ = -1;
  rubber_ = false;
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
  EnsureSelSize();
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

  // Scan every monitor work area (Fences: icons stay on the screen they land).
  constexpr size_t kMaxCandidates = 768;
  std::vector<RECT> candidates;
  candidates.reserve((std::min)(kMaxCandidates, slots_.size() * 8 + 32));
  const auto monitors = EnumMonitors();
  for (const auto& mon : monitors) {
    const RECT& work = mon.work;
    int search_bottom = work.bottom - kMargin;
    int search_right = work.right - kMargin;
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
    for (int x = work.left + kMargin; x + cell_w <= search_right; x += step_x) {
      for (int y = work.top + kMargin; y + cell_h <= search_bottom;
           y += step_y) {
        const RECT slot{x, y, x + cell_w, y + cell_h};
        if (intersects_blocked(slot)) {
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
    candidates.push_back({work.left + kMargin, work.top + kMargin,
                          work.left + kMargin + cell_w,
                          work.top + kMargin + cell_h});
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
    const RECT& sc = assigned[i];
    const int cx = sc.left - origin_x;
    const int cy = sc.top - origin_y;
    slots_[i].bounds = {cx, cy, cx + cell_w, cy + cell_h};
    slots_[i].icon_rc = {cx + icon_pad, cy + 4, cx + icon_pad + icon,
                         cy + 4 + icon};
  }
  Present();
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
  const std::vector<ShellMenuExtra> extras{{L"刷新", kExtPublicRefresh}};
  ShellVerbFn on_verb = [this](const std::wstring& verb) {
    if ((verb == L"refresh" || verb == L"paste" || verb == L"newfolder" ||
         verb == L"undo") &&
        on_refresh_) {
      on_refresh_();
    }
  };
  const int rc = TrackShellFolderBackgroundMenu(hwnd_, screen, desk, extras,
                                                on_verb, nullptr);
  if (rc == kExtPublicRefresh && on_refresh_) {
    on_refresh_();
  }
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
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  selection_anchor_ = -1;
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
  }
}

void PublicHost::CutSelection() {
  const auto paths = SelectedPaths();
  if (!paths.empty() && ClipboardSetFiles(hwnd_, paths, true)) {
    ShowToast(L"已剪切到剪贴板");
    Present();
  }
}

void PublicHost::DeleteSelection() {
  auto paths = SelectedPaths();
  if (paths.empty()) {
    return;
  }
  if (RecycleShellPaths(hwnd_, paths) != IconFsResult::Ok) {
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
  EnsureSelSize();
  int index = -1;
  int count = 0;
  for (size_t i = 0; i < selected_.size() && i < slots_.size(); ++i) {
    if (selected_[i]) {
      index = static_cast<int>(i);
      ++count;
    }
  }
  if (count != 1 || index < 0) {
    return;
  }
  const std::wstring path = slots_[static_cast<size_t>(index)].item.path;
  std::wstring dest;
  if (RenameShellPath(hwnd_, path, &dest) == IconFsResult::Ok) {
    if (on_path_replaced_ && dest != path) {
      on_path_replaced_(path, dest);
    }
    if (on_refresh_) {
      on_refresh_();
    }
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
  if (GetCapture() == hwnd_) {
    ReleaseCapture();
  }
  ShellDoDragFiles(paths);
  POINT pt{};
  GetCursorPos(&pt);
  for (const auto& p : paths) {
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
      if (on_removed_) {
        on_removed_(p);
      }
      continue;
    }
    UpsertFloatPlacement(&placements_, p, current_page_, pt);
  }
  if (on_placements_changed_) {
    on_placements_changed_(placements_);
  }
  if (on_refresh_) {
    on_refresh_();
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
      SetFocus(hwnd_);
      drag_from_ = HitTest(x, y);
      drag_moved_ = false;
      drag_press_ = {x, y};
      rubber_ = false;
      const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      if (drag_from_ >= 0) {
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
        // Empty chrome: start rubber-band (Explorer / Fences).
        ClearSelection();
        rubber_ = true;
        rubber_origin_ = {x, y};
        rubber_rc_ = {x, y, x, y};
        Present();
        SetCapture(hwnd_);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      if ((wparam & MK_LBUTTON) == 0) {
        break;
      }
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      if (rubber_) {
        rubber_rc_ = {rubber_origin_.x, rubber_origin_.y, x, y};
        Present();
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
      const int hit = HitTest(x, y);
      POINT screen{x, y};
      ClientToScreen(hwnd_, &screen);
      RECT wr{};
      GetWindowRect(hwnd_, &wr);
      const bool outside =
          screen.x < wr.left || screen.x >= wr.right || screen.y < wr.top ||
          screen.y >= wr.bottom ||
          (hit < 0 && y >= kHeaderH);
      if (outside) {
        BeginOleDrag();
        return 0;
      }
      if (hit >= 0 && hit != drag_from_ && SelectedPaths().size() <= 1) {
        ReorderTo(drag_from_, hit);
        drag_from_ = hit;
        SelectOnly(hit);
      }
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
        SelectIntersecting(rubber_rc_);
        rubber_ = false;
        Present();
        drag_from_ = -1;
        drag_moved_ = false;
        return 0;
      }
      if (!drag_moved_) {
        const int hit = HitTest(x, y);
        if (hit < 0 && !HasSelection()) {
          ClearSelection();
          Present();
        }
      }
      drag_from_ = -1;
      drag_moved_ = false;
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
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
        }
        Present();
        ShowSlotMenu(hit, pt);
      } else {
        // Blank float chrome: Explorer desktop background menu (Paste / New…).
        ShowBackgroundMenu(pt);
      }
      return 0;
    }
    case WM_KEYDOWN: {
      const int vk = static_cast<int>(wparam);
      if (vk == VK_DELETE) {
        DeleteSelection();
        return 0;
      }
      if (vk == VK_F2) {
        RenameSelection();
        return 0;
      }
      if (vk == VK_RETURN) {
        OpenSelection();
        return 0;
      }
      if (vk == VK_ESCAPE) {
        ClearSelection();
        Present();
        return 0;
      }
      if (vk == L'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        EnsureSelSize();
        std::fill(selected_.begin(), selected_.end(), static_cast<char>(1));
        Present();
        return 0;
      }
      if (vk == L'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        CopySelection();
        return 0;
      }
      if (vk == L'X' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        CutSelection();
        return 0;
      }
      if (vk == L'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        PasteClipboardFiles();
        return 0;
      }
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
    for (size_t i = 0; i < slots_.size(); ++i) {
      const auto& s = slots_[i];
      if (i < selected_.size() && selected_[i]) {
        Gdiplus::SolidBrush sel(Gdiplus::Color(90, 0, 120, 215));
        gi.FillRectangle(&sel,
                         static_cast<INT>(s.bounds.left),
                         static_cast<INT>(s.bounds.top),
                         static_cast<INT>(s.bounds.right - s.bounds.left),
                         static_cast<INT>(s.bounds.bottom - s.bounds.top));
      }
      if (s.icon) {
        DrawIconAlpha(gi, s.icon, s.icon_rc.left, s.icon_rc.top, icon);
      }
    }
    if (rubber_) {
      RECT r = rubber_rc_;
      if (r.left > r.right) {
        std::swap(r.left, r.right);
      }
      if (r.top > r.bottom) {
        std::swap(r.top, r.bottom);
      }
      Gdiplus::SolidBrush fill(Gdiplus::Color(40, 0, 120, 215));
      Gdiplus::Pen pen(Gdiplus::Color(200, 0, 120, 215), 1.f);
      gi.FillRectangle(&fill, static_cast<INT>(r.left), static_cast<INT>(r.top),
                       static_cast<INT>(r.right - r.left),
                       static_cast<INT>(r.bottom - r.top));
      gi.DrawRectangle(&pen, static_cast<INT>(r.left), static_cast<INT>(r.top),
                       static_cast<INT>(r.right - r.left),
                       static_cast<INT>(r.bottom - r.top));
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
