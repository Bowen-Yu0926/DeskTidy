#include "fence_window.h"

#include "desktop_shell.h"

#include <Shellapi.h>
#include <windowsx.h>

#include <algorithm>

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeFence";
constexpr COLORREF kPanelBg = RGB(32, 36, 44);
constexpr COLORREF kPanelBorder = RGB(70, 78, 92);
constexpr int kHeaderH = 36;
constexpr int kPad = 10;
constexpr int kCellW = 76;
constexpr int kCellH = 86;
constexpr int kIconSize = 32;
bool g_class_registered = false;
}  // namespace

bool FenceWindow::Create(HINSTANCE instance, const FenceLayout& layout) {
  id_ = layout.id;
  title_ = layout.title;

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
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName,
      title_.c_str(), WS_POPUP, layout.x, layout.y, layout.w, layout.h, nullptr,
      nullptr, instance, this);

  if (!hwnd_) {
    return false;
  }

  SetLayeredWindowAttributes(hwnd_, 0, 235, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void FenceWindow::Destroy() {
  ClearIcons();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
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
  const int inner_t = rc.top + kHeaderH;
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
}

int FenceWindow::HitTestIcon(int x, int y) const {
  for (size_t i = 0; i < cells_.size(); ++i) {
    const RECT& b = cells_[i].bounds;
    if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void FenceWindow::OpenItem(int index) {
  if (index < 0 || index >= static_cast<int>(cells_.size())) {
    return;
  }
  const auto& path = cells_[static_cast<size_t>(index)].item.path;
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
    self = reinterpret_cast<FenceWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
    case WM_LBUTTONDOWN: {
      press_client_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int icon = HitTestIcon(press_client_.x, press_client_.y);
      if (icon >= 0) {
        // Icon press: wait for up / dblclk; don't start panel drag.
        dragging_ = false;
        drag_moved_ = false;
        SetCapture(hwnd_);
        return 0;
      }
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
      if (dragging_) {
        dragging_ = false;
        if (drag_moved_) {
          AttachToDesktopLayer();
          if (on_moved_) {
            on_moved_(this);
          }
        }
        return 0;
      }
      const int icon = HitTestIcon(x, y);
      if (icon >= 0) {
        OpenItem(icon);
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
  HBRUSH bg = CreateSolidBrush(kPanelBg);
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);

  HPEN pen = CreatePen(PS_SOLID, 1, kPanelBorder);
  HGDIOBJ old_pen = SelectObject(hdc, pen);
  HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 16, 16);
  SelectObject(hdc, old_pen);
  SelectObject(hdc, old_brush);
  DeleteObject(pen);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(220, 224, 230));
  RECT title_rc = rc;
  title_rc.top += 8;
  title_rc.bottom = title_rc.top + 24;
  DrawTextW(hdc, title_.c_str(), -1, &title_rc,
            DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

  HFONT font = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HGDIOBJ old_font = SelectObject(hdc, font);

  for (const auto& cell : cells_) {
    const RECT& b = cell.bounds;
    if (b.bottom > rc.bottom - 4) {
      continue;  // clipped by fence height for MVP
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
