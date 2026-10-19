#include "fence_window.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "fence_style.h"
#include "icon_actions.h"
#include "shell_clipboard.h"
#include "toast.h"

#include <ShlObj.h>
#include <Shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeFence";
constexpr int kHeaderH0 = 32;
constexpr int kPad0 = 10;
constexpr int kCellW0 = 84;
constexpr int kCellH0 = 96;
constexpr int kIconSize0 = 40;  // match Python FenceIconItem.DEFAULT_ICON_SIZE
constexpr int kDragThreshold = 6;
constexpr int kEdge = 8;
bool g_class_registered = false;

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

int S(HWND hwnd, int px) { return MulDiv(px, FenceDpi(hwnd), 96); }
}  // namespace

void FenceWindow::ApplyStyle() {
  const auto* preset = FenceStyleById(style_id_);
  style_id_ = preset->id;
  bg_ = preset->background;
  accent_ = preset->accent;
  title_color_ = preset->title;
  caption_color_ = preset->title;
  panel_alpha_ = preset->alpha;
  radius_ = preset->radius;
  light_panel_ = preset->light;
  if (hwnd_) {
    // Full window opacity: panel translucency is painted per-pixel so icons stay sharp.
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    PresentLayered();
  }
}

void FenceWindow::PresentLayered() {
  if (!hwnd_) {
    return;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  const int w = (std::max)(1, static_cast<int>(wr.right - wr.left));
  const int h = (std::max)(1, static_cast<int>(wr.bottom - wr.top));

  static ULONG_PTR gdi_token = 0;
  if (!gdi_token) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&gdi_token, &input, nullptr);
  }

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
    return;
  }
  HGDIOBJ old = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

  const int radius = (std::max)(0, S(hwnd_, radius_));
  Gdiplus::Graphics g(mem);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  const BYTE a = panel_alpha_;
  const BYTE pr = static_cast<BYTE>((a * GetRValue(bg_)) / 255);
  const BYTE pg = static_cast<BYTE>((a * GetGValue(bg_)) / 255);
  const BYTE pb = static_cast<BYTE>((a * GetBValue(bg_)) / 255);
  Gdiplus::SolidBrush fill(Gdiplus::Color(a, pr, pg, pb));
  Gdiplus::GraphicsPath path;
  const int rr = radius * 2;
  const int rw = (std::max)(1, w - 1);
  const int rh = (std::max)(1, h - 1);
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
  COLORREF border = light_panel_ ? RGB(15, 23, 42) : RGB(255, 255, 255);
  const BYTE ba = light_panel_ ? 48 : 40;
  Gdiplus::Pen edge(Gdiplus::Color(ba, GetRValue(border), GetGValue(border),
                                   GetBValue(border)),
                    1.0f);
  if (rr > 2 && rr < rw && rr < rh) {
    g.DrawPath(&edge, &path);
  } else {
    g.DrawRectangle(&edge, 0, 0, rw - 1, rh - 1);
  }

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

void FenceWindow::SetStyleId(const std::string& style_id) {
  style_id_ = NormalizeFenceStyleId(style_id);
  ApplyStyle();
}

bool FenceWindow::Create(HINSTANCE instance, const FenceLayout& layout) {
  id_ = layout.id;
  title_ = layout.title;
  page_ = layout.page;
  style_id_ = NormalizeFenceStyleId(layout.style_id);
  portal_path_ = layout.portal_path;

  if (!g_class_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
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
      layout.h, nullptr, nullptr, instance, this);

  if (!hwnd_) {
    return false;
  }

  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ApplyStyle();
  EnsureDropTarget();
  return true;
}

void FenceWindow::EnsureDropTarget() {
  if (!hwnd_ || drop_target_) {
    return;
  }
  drop_target_ =
      new PathDropTarget([this](const std::vector<std::wstring>& paths) {
        if (on_paths_dropped_) {
          on_paths_dropped_(this, paths);
        }
      });
  if (!RegisterPathDropTarget(hwnd_, drop_target_)) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
}

void FenceWindow::Destroy() {
  ClearIcons();
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

void FenceWindow::ClearIcons() {
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
    out.push_back(c.item);
  }
  return out;
}

void FenceWindow::SetTitle(const std::wstring& title) {
  title_ = title;
  if (hwnd_) {
    SetWindowTextW(hwnd_, title_.c_str());
    PresentLayered();
  }
}

FenceLayout FenceWindow::CurrentLayout() const {
  FenceLayout fl;
  fl.id = id_;
  fl.title = title_;
  fl.page = page_;
  fl.style_id = style_id_;
  fl.portal_path = portal_path_;
  if (hwnd_) {
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    fl.x = rc.left;
    fl.y = rc.top;
    fl.w = rc.right - rc.left;
    fl.h = rc.bottom - rc.top;
  }
  return fl;
}

void FenceWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachToDesktopLayer();
  PresentLayered();
}

void FenceWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void FenceWindow::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

void FenceWindow::SetItems(std::vector<DesktopItem> items) {
  ClearIcons();
  cells_.reserve(items.size());
  for (auto& it : items) {
    Cell cell;
    cell.item = std::move(it);
    cell.icon = LoadItemIcon(cell.item.path, S(hwnd_, kIconSize0));
    cells_.push_back(std::move(cell));
  }
  scroll_y_ = 0;
  selected_.assign(cells_.size(), 0);
  selection_anchor_ = -1;
  LayoutCells();
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::LayoutCells() {
  if (!hwnd_) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int pad = S(hwnd_, kPad0);
  const int header = S(hwnd_, kHeaderH0);
  const int cell_w = S(hwnd_, kCellW0);
  const int cell_h = S(hwnd_, kCellH0);
  const int inner_l = rc.left + pad;
  const int inner_t = rc.top + header - scroll_y_;
  const int inner_r = rc.right - pad;
  const int cols = (std::max)(1, (inner_r - inner_l) / cell_w);
  for (size_t i = 0; i < cells_.size(); ++i) {
    const int col = static_cast<int>(i % static_cast<size_t>(cols));
    const int row = static_cast<int>(i / static_cast<size_t>(cols));
    RECT& b = cells_[i].bounds;
    b.left = inner_l + col * cell_w;
    b.top = inner_t + row * cell_h;
    b.right = b.left + cell_w;
    b.bottom = b.top + cell_h;
  }
  const int rows =
      cells_.empty()
          ? 0
          : static_cast<int>((cells_.size() + static_cast<size_t>(cols) - 1) /
                             static_cast<size_t>(cols));
  content_h_ = rows * cell_h;
  ClampScroll();
}

void FenceWindow::ClampScroll() {
  RECT rc{};
  if (!hwnd_) {
    return;
  }
  GetClientRect(hwnd_, &rc);
  const int view = (std::max)(
      0, static_cast<int>(rc.bottom) - S(hwnd_, kHeaderH0) - S(hwnd_, kPad0));
  const int max_scroll = (std::max)(0, content_h_ - view);
  if (scroll_y_ < 0) {
    scroll_y_ = 0;
  }
  if (scroll_y_ > max_scroll) {
    scroll_y_ = max_scroll;
  }
}

int FenceWindow::HitTestIcon(int x, int y) const {
  if (y < S(hwnd_, kHeaderH0)) {
    return -1;
  }
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& b = cells_[i].bounds;
    if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

FenceWindow::Edge FenceWindow::HitTestEdge(int x, int y) const {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  int e = 0;
  if (x <= kEdge) {
    e |= static_cast<int>(Edge::Left);
  }
  if (x >= rc.right - kEdge) {
    e |= static_cast<int>(Edge::Right);
  }
  if (y <= kEdge) {
    e |= static_cast<int>(Edge::Top);
  }
  if (y >= rc.bottom - kEdge) {
    e |= static_cast<int>(Edge::Bottom);
  }
  return static_cast<Edge>(e);
}

void FenceWindow::OpenItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  ShellExecuteW(nullptr, L"open",
                cells_[static_cast<size_t>(index)].item.path.c_str(), nullptr,
                nullptr, SW_SHOWNORMAL);
}

void FenceWindow::RevealItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring arg =
      L"/select,\"" + cells_[static_cast<size_t>(index)].item.path + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr,
                SW_SHOWNORMAL);
}

void FenceWindow::BeginIconDrag(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
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
}

void FenceWindow::RenameItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring old_path = cells_[static_cast<size_t>(index)].item.path;
  std::wstring dest;
  if (RenameShellPath(hwnd_, old_path, &dest) == IconFsResult::Ok &&
      on_path_replaced_) {
    on_path_replaced_(old_path, dest);
  }
}

void FenceWindow::RecycleItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring path = cells_[static_cast<size_t>(index)].item.path;
  if (RecycleShellPath(hwnd_, path) == IconFsResult::Ok && on_unpin_) {
    on_unpin_(this, path);
  }
}

void FenceWindow::ShowItemProperties(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  ShowShellPathProperties(hwnd_, cells_[static_cast<size_t>(index)].item.path);
}

void FenceWindow::ClearSelection() {
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::SelectOnly(int index) {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  if (index >= 0 && index < static_cast<int>(selected_.size())) {
    selected_[static_cast<size_t>(index)] = 1;
    selection_anchor_ = index;
  }
  if (hwnd_) {
    PresentLayered();
  }
}

void FenceWindow::ToggleSelected(int index) {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  if (index < 0 || index >= static_cast<int>(selected_.size())) {
    return;
  }
  selected_[static_cast<size_t>(index)] =
      selected_[static_cast<size_t>(index)] ? 0 : 1;
  selection_anchor_ = index;
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
  }
  const int lo = (std::min)(a, index);
  const int hi = (std::max)(a, index);
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
  for (int i = lo; i <= hi; ++i) {
    selected_[static_cast<size_t>(i)] = 1;
  }
  if (hwnd_) {
    PresentLayered();
  }
}

bool FenceWindow::IsSelected(int index) const {
  return index >= 0 && index < static_cast<int>(selected_.size()) &&
         selected_[static_cast<size_t>(index)] != 0;
}

std::vector<std::wstring> FenceWindow::SelectedPaths() const {
  std::vector<std::wstring> paths;
  const size_t n = (std::min)(selected_.size(), cells_.size());
  for (size_t i = 0; i < n; ++i) {
    if (selected_[i]) {
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
    for (const auto& p : paths) {
      if (on_unpin_) {
        on_unpin_(this, p);
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
        on_unpin_(this, p);
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

void FenceWindow::SetPortalPath(const std::wstring& path) {
  portal_path_ = path;
}

void FenceWindow::CopySelection() {
  const auto paths = SelectedPaths();
  if (!paths.empty()) {
    ClipboardSetFiles(hwnd_, paths, false);
    ShowToast(L"已复制到剪贴板");
  }
}

void FenceWindow::CutSelection() {
  const auto paths = SelectedPaths();
  if (!paths.empty() && ClipboardSetFiles(hwnd_, paths, true)) {
    ShowToast(L"已剪切到剪贴板");
    PresentLayered();
  }
}

void FenceWindow::DeleteSelection() {
  const auto paths = SelectedPaths();
  if (paths.empty()) {
    return;
  }
  if (RecycleShellPaths(hwnd_, paths) != IconFsResult::Ok) {
    return;
  }
  for (const auto& p : paths) {
    if (on_unpin_) {
      on_unpin_(this, p);
    }
  }
}

void FenceWindow::SelectAll() {
  if (selected_.size() != cells_.size()) {
    selected_.assign(cells_.size(), 0);
  }
  std::fill(selected_.begin(), selected_.end(), static_cast<char>(1));
  if (!cells_.empty()) {
    selection_anchor_ = 0;
  }
  PresentLayered();
}

namespace {
constexpr int kRenameEdit = 77;
LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                LPARAM lparam) {
  auto* self = reinterpret_cast<FenceWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE)) {
    if (self) {
      self->FinishInlineRename(wparam == VK_RETURN);
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

void FenceWindow::BeginInlineRename() {
  int index = -1;
  int count = 0;
  for (size_t i = 0; i < selected_.size() && i < cells_.size(); ++i) {
    if (selected_[i]) {
      index = static_cast<int>(i);
      ++count;
    }
  }
  if (count != 1 || index < 0) {
    return;
  }
  if (rename_edit_) {
    DestroyWindow(rename_edit_);
    rename_edit_ = nullptr;
  }
  rename_index_ = index;
  const RECT& b = cells_[static_cast<size_t>(index)].bounds;
  const int icon = S(hwnd_, kIconSize0);
  const int cell_w = S(hwnd_, kCellW0);
  rename_edit_ = CreateWindowExW(
      0, L"EDIT", cells_[static_cast<size_t>(index)].item.name.c_str(),
      WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, b.left + 2,
      b.top + 4 + icon, cell_w - 4, S(hwnd_, 20), hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRenameEdit)), nullptr,
      nullptr);
  if (!rename_edit_) {
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
  if (!rename_edit_) {
    return;
  }
  wchar_t buf[260] = {};
  GetWindowTextW(rename_edit_, buf, 260);
  const int index = rename_index_;
  DestroyWindow(rename_edit_);
  rename_edit_ = nullptr;
  rename_index_ = -1;
  if (!commit || index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring old_path = cells_[static_cast<size_t>(index)].item.path;
  const size_t slash = old_path.find_last_of(L"\\/");
  const std::wstring parent =
      slash == std::wstring::npos ? L"" : old_path.substr(0, slash);
  const std::wstring name =
      slash == std::wstring::npos ? old_path : old_path.substr(slash + 1);
  const std::wstring neu = buf;
  if (neu.empty() || neu == name) {
    return;
  }
  const std::wstring dest = parent.empty() ? neu : parent + L"\\" + neu;
  if (!MoveFileW(old_path.c_str(), dest.c_str())) {
    ShowToast(L"无法重命名");
    return;
  }
  if (on_path_replaced_) {
    on_path_replaced_(old_path, dest);
  }
}

void FenceWindow::PasteClipboardFiles() {
  std::vector<std::wstring> paths;
  if (!ClipboardGetFiles(&paths) || paths.empty()) {
    ShowToast(L"剪贴板里没有文件");
    return;
  }
  if (on_paths_dropped_) {
    on_paths_dropped_(this, paths);
    ShowToast(L"已粘贴 " + std::to_wstring(paths.size()) + L" 项");
  }
}

void FenceWindow::ShowFenceMenu(POINT screen) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 10, L"重命名分区…");
  AppendMenuW(menu, MF_STRING, 13, L"粘贴文件");
  AppendMenuW(menu, MF_STRING, 14,
              portal_path_.empty() ? L"设为门户文件夹…" : L"更换门户文件夹…");
  if (!portal_path_.empty()) {
    AppendMenuW(menu, MF_STRING, 15, L"取消门户");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU pages = CreatePopupMenu();
  if (page_names_.empty()) {
    page_names_ = {L"第 1 页", L"第 2 页"};
  }
  for (size_t i = 0; i < page_names_.size() && i < 12; ++i) {
    const UINT flags = MF_STRING | (page_ == static_cast<int>(i) ? MF_CHECKED : 0);
    AppendMenuW(pages, flags, 400 + static_cast<UINT>(i), page_names_[i].c_str());
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pages), L"所在页面");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU styles = CreatePopupMenu();
  const auto all = AllFenceStyles();
  for (size_t i = 0; i < all.size(); ++i) {
    const UINT flags =
        MF_STRING | (style_id_ == all[i]->id ? MF_CHECKED : 0);
    AppendMenuW(styles, flags, 100 + static_cast<UINT>(i), all[i]->name);
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(styles), L"分区样式");
  SetForegroundWindow(hwnd_);
  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0,
                     hwnd_, nullptr);
  DestroyMenu(menu);
  if (cmd == 10 && on_rename_) {
    on_rename_(this);
  } else if (cmd == 13) {
    PasteClipboardFiles();
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
          portal_path_ = path;
          if (on_portal_) {
            on_portal_(this, portal_path_);
          }
        }
        CoTaskMemFree(pidl);
      }
    }
  } else if (cmd >= 400 && cmd < 400 + static_cast<int>(page_names_.size()) &&
             on_set_page_) {
    on_set_page_(this, cmd - 400);
  } else if (cmd >= 100 && cmd < 100 + static_cast<int>(all.size())) {
    SetStyleId(all[static_cast<size_t>(cmd - 100)]->id);
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
    const bool ctrl = lparam != 0;
    if (vk == VK_DELETE) {
      DeleteSelection();
    } else if (vk == VK_F2) {
      BeginInlineRename();
    } else if (ctrl && vk == 'C') {
      CopySelection();
    } else if (ctrl && vk == 'X') {
      CutSelection();
    } else if (ctrl && vk == 'V') {
      PasteClipboardFiles();
    } else if (ctrl && vk == 'A') {
      SelectAll();
    }
    return 0;
  }
  switch (msg) {
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
      LayoutCells();
      PresentLayered();
      return 0;
    case WM_KEYDOWN:
      if (wparam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        PasteClipboardFiles();
        return 0;
      }
      break;
    case WM_MOUSEWHEEL: {
      const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
      scroll_y_ -= delta / 2;
      ClampScroll();
      LayoutCells();
      PresentLayered();
      return 0;
    }
    case WM_SETCURSOR: {
      if (LOWORD(lparam) == HTCLIENT) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
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
        if (v & (static_cast<int>(Edge::Left) | static_cast<int>(Edge::Right))) {
          SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
          return TRUE;
        }
        if (v & (static_cast<int>(Edge::Top) | static_cast<int>(Edge::Bottom))) {
          SetCursor(LoadCursor(nullptr, IDC_SIZENS));
          return TRUE;
        }
      }
      break;
    }
    case WM_LBUTTONDOWN: {
      press_client_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const Edge edge = HitTestEdge(press_client_.x, press_client_.y);
      if (edge != Edge::None) {
        resizing_ = true;
        resize_edge_ = edge;
        GetWindowRect(hwnd_, &resize_start_rc_);
        GetCursorPos(&resize_start_pt_);
        SetCapture(hwnd_);
        return 0;
      }
      const int icon = HitTestIcon(press_client_.x, press_client_.y);
      if (icon >= 0) {
        dragging_ = false;
        drag_moved_ = false;
        rubber_ = false;
        icon_press_ = true;
        icon_press_index_ = icon;
        SetCapture(hwnd_);
        return 0;
      }
      icon_press_ = false;
      icon_press_index_ = -1;
      if (press_client_.y >= S(hwnd_, kHeaderH0)) {
        rubber_ = true;
        dragging_ = false;
        rubber_a_ = rubber_b_ = press_client_;
        SetCapture(hwnd_);
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
      if (resizing_ && (wparam & MK_LBUTTON)) {
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
        if (r - l < 160) {
          if (ev & static_cast<int>(Edge::Left)) {
            l = r - 160;
          } else {
            r = l + 160;
          }
        }
        if (b - t < 120) {
          if (ev & static_cast<int>(Edge::Top)) {
            t = b - 120;
          } else {
            b = t + 120;
          }
        }
        SetWindowPos(hwnd_, nullptr, l, t, r - l, b - t,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      if (rubber_ && (wparam & MK_LBUTTON)) {
        rubber_b_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        PresentLayered();
        return 0;
      }
      if (icon_press_ && (wparam & MK_LBUTTON)) {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        if (abs(x - press_client_.x) >= kDragThreshold ||
            abs(y - press_client_.y) >= kDragThreshold) {
          BeginIconDrag(icon_press_index_);
        }
        return 0;
      }
      if (!dragging_ || !(wparam & MK_LBUTTON)) {
        return 0;
      }
      POINT pt{};
      GetCursorPos(&pt);
      SetWindowPos(hwnd_, nullptr, pt.x - drag_offset_.x, pt.y - drag_offset_.y,
                   0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      drag_moved_ = true;
      return 0;
    }
    case WM_LBUTTONUP: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      ReleaseCapture();
      if (rubber_) {
        rubber_ = false;
        const int l = (std::min)(rubber_a_.x, rubber_b_.x);
        const int t = (std::min)(rubber_a_.y, rubber_b_.y);
        const int r = (std::max)(rubber_a_.x, rubber_b_.x);
        const int b = (std::max)(rubber_a_.y, rubber_b_.y);
        if (selected_.size() != cells_.size()) {
          selected_.assign(cells_.size(), 0);
        }
        std::fill(selected_.begin(), selected_.end(), static_cast<char>(0));
        for (size_t i = 0; i < cells_.size(); ++i) {
          const RECT& cell = cells_[i].bounds;
          const bool hit = !(cell.right < l || cell.left > r || cell.bottom < t ||
                             cell.top > b);
          if (hit) {
            selected_[i] = 1;
            selection_anchor_ = static_cast<int>(i);
          }
        }
        PresentLayered();
        return 0;
      }
      if (resizing_) {
        resizing_ = false;
        resize_edge_ = Edge::None;
        AttachToDesktopLayer();
        if (on_moved_) {
          on_moved_(this);
        }
        return 0;
      }
      if (icon_press_) {
        const int idx = icon_press_index_;
        icon_press_ = false;
        icon_press_index_ = -1;
        if (HitTestIcon(x, y) == idx) {
          const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
          const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
          if (ctrl) {
            ToggleSelected(idx);
          } else if (shift) {
            SelectRange(idx);
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
          RECT rc{};
          GetWindowRect(hwnd_, &rc);
          ClampRectToWorkArea(&rc);
          SetWindowPos(hwnd_, nullptr, rc.left, rc.top, 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
          AttachToDesktopLayer();
          if (on_moved_) {
            on_moved_(this);
          }
        }
        return 0;
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int icon = HitTestIcon(pt.x, pt.y);
      ClientToScreen(hwnd_, &pt);
      if (icon >= 0) {
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
        OpenItem(icon);
        return 0;
      }
      if (y < S(hwnd_, kHeaderH0) && HitTestEdge(x, y) == Edge::None &&
          on_rename_) {
        on_rename_(this);
      }
      return 0;
    }
    case WM_DESTROY:
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
  RECT rc{};
  GetClientRect(hwnd_, &rc);

  const int header = S(hwnd_, kHeaderH0);
  const int pad = S(hwnd_, kPad0);
  const int cell_w = S(hwnd_, kCellW0);
  const int icon = S(hwnd_, kIconSize0);
  const int font_px = S(hwnd_, 12);
  const int title_px = S(hwnd_, 15);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, title_color_);
  HFONT title_font =
      CreateFontW(-title_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                  L"Microsoft YaHei UI");
  HGDIOBJ old_title = SelectObject(hdc, title_font);
  RECT title_rc = rc;
  title_rc.top += S(hwnd_, 6);
  title_rc.bottom = title_rc.top + title_px + 4;
  DrawTextW(hdc, title_.c_str(), -1, &title_rc,
            DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(hdc, old_title);
  DeleteObject(title_font);

  const int view = (std::max)(0, static_cast<int>(rc.bottom) - header - pad);
  if (content_h_ > view && view > 0) {
    const int track_top = header + 2;
    const int track_h = (std::max)(8, static_cast<int>(rc.bottom) - track_top - 4);
    const int thumb_h =
        (std::max)(16, track_h * view / (std::max)(1, content_h_));
    const int max_scroll = (std::max)(1, content_h_ - view);
    int thumb_y = track_top + (track_h - thumb_h) * scroll_y_ / max_scroll;
    if (thumb_y + thumb_h > track_top + track_h) {
      thumb_y = track_top + track_h - thumb_h;
    }
    RECT thumb{rc.right - 7, thumb_y, rc.right - 3, thumb_y + thumb_h};
    HBRUSH thumb_br = CreateSolidBrush(accent_);
    FillRect(hdc, &thumb, thumb_br);
    DeleteObject(thumb_br);
  }

  IntersectClipRect(hdc, rc.left, header, rc.right, rc.bottom);

  HFONT font = CreateFontW(-font_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  HGDIOBJ old_font = SelectObject(hdc, font);
  SetTextColor(hdc, caption_color_);

  for (size_t i = 0; i < cells_.size(); ++i) {
    const auto& cell = cells_[i];
    const RECT& b = cell.bounds;
    if (b.bottom < header || b.top > rc.bottom) {
      continue;
    }
    if (i < selected_.size() && selected_[i]) {
      HPEN sp = CreatePen(PS_SOLID, 2, accent_);
      HGDIOBJ op = SelectObject(hdc, sp);
      HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
      RoundRect(hdc, b.left + 2, b.top + 1, b.right - 2, b.bottom - 2, 10, 10);
      SelectObject(hdc, op);
      SelectObject(hdc, ob);
      DeleteObject(sp);
    }
    if (cell.icon) {
      const int ix = b.left + (cell_w - icon) / 2;
      const int iy = b.top + 4;
      if (ClipboardPathIsCut(cell.item.path)) {
        DrawStateW(hdc, nullptr, nullptr,
                   reinterpret_cast<LPARAM>(cell.icon), 0, ix, iy, icon, icon,
                   DST_ICON | DSS_DISABLED);
      } else {
        DrawIconEx(hdc, ix, iy, cell.icon, icon, icon, 0, nullptr, DI_NORMAL);
      }
    }
    RECT label = b;
    label.top = b.top + 4 + icon + 2;
    label.left += 2;
    label.right -= 2;
    DrawTextW(hdc, cell.item.name.c_str(), -1, &label,
              DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
  }

  SelectObject(hdc, old_font);
  DeleteObject(font);
  SelectClipRgn(hdc, nullptr);
  if (rubber_) {
    const int l = (std::min)(rubber_a_.x, rubber_b_.x);
    const int t = (std::min)(rubber_a_.y, rubber_b_.y);
    const int r = (std::max)(rubber_a_.x, rubber_b_.x);
    const int b = (std::max)(rubber_a_.y, rubber_b_.y);
    HPEN sp = CreatePen(PS_SOLID, 1, accent_);
    HGDIOBJ op = SelectObject(hdc, sp);
    HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, l, t, r, b);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(sp);
  }
}
