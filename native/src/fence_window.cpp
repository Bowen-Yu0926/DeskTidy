#include "fence_window.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "fence_style.h"
#include "prompt_text.h"
#include "shell_clipboard.h"
#include "toast.h"

#include <Shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeFence";
constexpr int kHeaderH = 36;
constexpr int kPad = 10;
constexpr int kCellW = 76;
constexpr int kCellH = 86;
constexpr int kIconSize = 32;
constexpr int kDragThreshold = 6;
constexpr int kEdge = 8;
bool g_class_registered = false;
}  // namespace

void FenceWindow::ApplyStyle() {
  const auto* preset = FenceStyleById(style_id_);
  style_id_ = preset->id;
  bg_ = preset->background;
  accent_ = preset->accent;
  title_color_ = preset->title;
  if (hwnd_) {
    SetLayeredWindowAttributes(hwnd_, 0, preset->alpha, LWA_ALPHA);
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
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
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
}

FenceLayout FenceWindow::CurrentLayout() const {
  FenceLayout fl;
  fl.id = id_;
  fl.title = title_;
  fl.page = page_;
  fl.style_id = style_id_;
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
    cell.icon = LoadItemIcon(cell.item.path, kIconSize);
    cells_.push_back(std::move(cell));
  }
  scroll_y_ = 0;
  LayoutCells();
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
}

void FenceWindow::LayoutCells() {
  if (!hwnd_) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int inner_l = rc.left + kPad;
  const int inner_t = rc.top + kHeaderH - scroll_y_;
  const int inner_r = rc.right - kPad;
  const int cols = (std::max)(1, (inner_r - inner_l) / kCellW);
  for (size_t i = 0; i < cells_.size(); ++i) {
    const int col = static_cast<int>(i % static_cast<size_t>(cols));
    const int row = static_cast<int>(i / static_cast<size_t>(cols));
    RECT& b = cells_[i].bounds;
    b.left = inner_l + col * kCellW;
    b.top = inner_t + row * kCellH;
    b.right = b.left + kCellW;
    b.bottom = b.top + kCellH;
  }
  const int rows =
      cells_.empty()
          ? 0
          : static_cast<int>((cells_.size() + static_cast<size_t>(cols) - 1) /
                             static_cast<size_t>(cols));
  content_h_ = rows * kCellH;
  ClampScroll();
}

void FenceWindow::ClampScroll() {
  RECT rc{};
  if (!hwnd_) {
    return;
  }
  GetClientRect(hwnd_, &rc);
  const int view = (std::max)(0, static_cast<int>(rc.bottom) - kHeaderH - kPad);
  const int max_scroll = (std::max)(0, content_h_ - view);
  if (scroll_y_ < 0) {
    scroll_y_ = 0;
  }
  if (scroll_y_ > max_scroll) {
    scroll_y_ = max_scroll;
  }
}

int FenceWindow::HitTestIcon(int x, int y) const {
  if (y < kHeaderH) {
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
  const std::wstring path = cells_[static_cast<size_t>(index)].item.path;
  icon_press_ = false;
  icon_press_index_ = -1;
  ReleaseCapture();
  ShellDoDragFiles({path});
  if (on_drag_done_) {
    on_drag_done_(this);
  }
}

void FenceWindow::RenameItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring old_path = cells_[static_cast<size_t>(index)].item.path;
  const size_t slash = old_path.find_last_of(L"\\/");
  const std::wstring parent =
      slash == std::wstring::npos ? L"" : old_path.substr(0, slash);
  const std::wstring name =
      slash == std::wstring::npos ? old_path : old_path.substr(slash + 1);
  const std::wstring neu =
      PromptText(hwnd_, L"重命名", L"新名称：", name);
  if (neu.empty() || neu == name || neu.find_first_of(L"\\/:*?\"<>|") !=
                                         std::wstring::npos) {
    if (!neu.empty() && neu != name) {
      ShowToast(L"名称包含非法字符");
    }
    return;
  }
  const std::wstring dest = parent.empty() ? neu : parent + L"\\" + neu;
  if (!MoveFileW(old_path.c_str(), dest.c_str())) {
    ShowToast(L"无法重命名，文件可能被占用");
    return;
  }
  if (on_path_replaced_) {
    on_path_replaced_(old_path, dest);
  }
  ShowToast(L"已重命名");
}

void FenceWindow::RecycleItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring path = cells_[static_cast<size_t>(index)].item.path;
  if (MessageBoxW(hwnd_, L"将此项移到回收站？", L"删除",
                  MB_YESNO | MB_ICONQUESTION) != IDYES) {
    return;
  }
  std::wstring from = path;
  from.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.hwnd = hwnd_;
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  const int rc = SHFileOperationW(&op);
  if (rc != 0 || op.fAnyOperationsAborted) {
    ShowToast(L"未能移到回收站");
    return;
  }
  if (on_unpin_) {
    on_unpin_(this, path);
  }
}

void FenceWindow::ShowItemProperties(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const std::wstring& path = cells_[static_cast<size_t>(index)].item.path;
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_INVOKEIDLIST;
  sei.hwnd = hwnd_;
  sei.lpVerb = L"properties";
  sei.lpFile = path.c_str();
  sei.nShow = SW_SHOW;
  if (!ShellExecuteExW(&sei)) {
    ShowToast(L"无法打开属性");
  }
}

void FenceWindow::ShowIconMenu(int index, POINT screen) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1, L"打开");
  AppendMenuW(menu, MF_STRING, 2, L"在资源管理器中显示");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 4, L"复制");
  AppendMenuW(menu, MF_STRING, 5, L"剪切");
  AppendMenuW(menu, MF_STRING, 6, L"复制路径");
  AppendMenuW(menu, MF_STRING, 7, L"重命名…");
  AppendMenuW(menu, MF_STRING, 8, L"属性");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 9, L"删除");
  AppendMenuW(menu, MF_STRING, 3, L"取消钉选");
  SetForegroundWindow(hwnd_);
  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0,
                     hwnd_, nullptr);
  DestroyMenu(menu);
  if (cmd == 1) {
    OpenItem(index);
  } else if (cmd == 2) {
    RevealItem(index);
  } else if (cmd == 3 && on_unpin_) {
    on_unpin_(this, cells_[static_cast<size_t>(index)].item.path);
  } else if ((cmd == 4 || cmd == 5) &&
             index >= 0 && index < static_cast<int>(cells_.size())) {
    const std::wstring& path = cells_[static_cast<size_t>(index)].item.path;
    if (ClipboardSetFiles(hwnd_, {path}, cmd == 5)) {
      ShowToast(cmd == 5 ? L"已剪切到剪贴板" : L"已复制到剪贴板");
    }
  } else if (cmd == 6 && index >= 0 &&
             index < static_cast<int>(cells_.size())) {
    const std::wstring& path = cells_[static_cast<size_t>(index)].item.path;
    if (ClipboardSetText(hwnd_, path)) {
      ShowToast(L"路径已复制");
    }
  } else if (cmd == 7) {
    RenameItem(index);
  } else if (cmd == 8) {
    ShowItemProperties(index);
  } else if (cmd == 9) {
    RecycleItem(index);
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
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (page_ == 0 ? MF_CHECKED : 0), 11,
              L"放到第 1 页");
  AppendMenuW(menu, MF_STRING | (page_ == 1 ? MF_CHECKED : 0), 12,
              L"放到第 2 页");
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
  } else if (cmd == 11 && on_set_page_) {
    on_set_page_(this, 0);
  } else if (cmd == 12 && on_set_page_) {
    on_set_page_(this, 1);
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
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd_, &ps);
      Paint(hdc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_SIZE:
      LayoutCells();
      InvalidateRect(hwnd_, nullptr, TRUE);
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
      InvalidateRect(hwnd_, nullptr, TRUE);
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
        icon_press_ = true;
        icon_press_index_ = icon;
        SetCapture(hwnd_);
        return 0;
      }
      icon_press_ = false;
      icon_press_index_ = -1;
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
          OpenItem(idx);
        }
        return 0;
      }
      if (dragging_) {
        dragging_ = false;
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
      const int icon = HitTestIcon(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (icon >= 0) {
        OpenItem(icon);
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
  HBRUSH bg = CreateSolidBrush(bg_);
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);

  HPEN pen = CreatePen(PS_SOLID, 1, accent_);
  HGDIOBJ old_pen = SelectObject(hdc, pen);
  HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 16, 16);
  SelectObject(hdc, old_pen);
  SelectObject(hdc, old_brush);
  DeleteObject(pen);

  // Accent bar under title.
  RECT bar = rc;
  bar.top = kHeaderH - 3;
  bar.bottom = kHeaderH - 1;
  bar.left += 12;
  bar.right -= 12;
  HBRUSH accent_br = CreateSolidBrush(accent_);
  FillRect(hdc, &bar, accent_br);
  DeleteObject(accent_br);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, title_color_);
  RECT title_rc = rc;
  title_rc.top += 8;
  title_rc.bottom = title_rc.top + 24;
  wchar_t title_buf[256];
  swprintf_s(title_buf, L"%s  · p%d", title_.c_str(), page_ + 1);
  DrawTextW(hdc, title_buf, -1, &title_rc,
            DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

  IntersectClipRect(hdc, rc.left, kHeaderH, rc.right, rc.bottom);

  HFONT font = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HGDIOBJ old_font = SelectObject(hdc, font);

  for (const auto& cell : cells_) {
    const RECT& b = cell.bounds;
    if (b.bottom < kHeaderH || b.top > rc.bottom) {
      continue;
    }
    if (cell.icon) {
      const int ix = b.left + (kCellW - kIconSize) / 2;
      const int iy = b.top + 4;
      DrawIconEx(hdc, ix, iy, cell.icon, kIconSize, kIconSize, 0, nullptr,
                 DI_NORMAL);
    }
    RECT label = b;
    label.top = b.top + 4 + kIconSize + 2;
    label.left += 2;
    label.right -= 2;
    DrawTextW(hdc, cell.item.name.c_str(), -1, &label,
              DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
  }

  SelectObject(hdc, old_font);
  DeleteObject(font);
}
