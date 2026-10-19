#include "dock_window.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "icon_actions.h"

#include <Shellapi.h>
#include <windowsx.h>

#include <algorithm>

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeDock";
constexpr int kH = 64;
constexpr int kIcon = 36;
constexpr int kPad = 12;
bool g_registered = false;
}  // namespace

bool DockWindow::Create(HINSTANCE instance) {
  if (!g_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DockWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    g_registered = true;
  }

  const RECT work = PrimaryWorkArea();
  const int w = 420;
  const int x = work.left + (work.right - work.left - w) / 2;
  const int y = work.bottom - kH - 16;
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName, L"Dock",
      WS_POPUP, x, y, w, kH, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 220, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void DockWindow::Destroy() {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
      s.icon = nullptr;
    }
  }
  slots_.clear();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void DockWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachToDesktopLayer();
}

void DockWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void DockWindow::SetVisible(bool on) {
  if (on) {
    Show();
  } else {
    Hide();
  }
}

void DockWindow::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

void DockWindow::SetItems(std::vector<DesktopItem> items) {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
  const size_t n = (std::min)(items.size(), size_t{8});
  slots_.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    Slot s;
    s.item = std::move(items[i]);
    s.icon = LoadItemIcon(s.item.path, kIcon);
    slots_.push_back(std::move(s));
  }
  LayoutSlots();
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
}

void DockWindow::LayoutSlots() {
  if (!hwnd_) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int n = static_cast<int>(slots_.size());
  if (n <= 0) {
    return;
  }
  const int total = n * (kIcon + kPad);
  int x = (rc.right - total) / 2;
  for (auto& s : slots_) {
    s.bounds = {x, (rc.bottom - kIcon) / 2, x + kIcon, (rc.bottom - kIcon) / 2 + kIcon};
    x += kIcon + kPad;
  }
}

void DockWindow::ShowSlotMenu(int index, POINT screen) {
  if (index < 0 || index >= static_cast<int>(slots_.size())) {
    return;
  }
  const std::wstring path = slots_[static_cast<size_t>(index)].item.path;
  const int cmd = TrackIconShellMenu(hwnd_, screen, false);
  if (cmd == 1) {
    OpenShellPath(path);
  } else if (cmd == 2) {
    RevealShellPath(path);
  } else if (cmd == 4 || cmd == 5) {
    CopyOrCutShellPath(hwnd_, path, cmd == 5);
  } else if (cmd == 6) {
    CopyShellPathText(hwnd_, path);
  } else if (cmd == 7) {
    std::wstring dest;
    if (RenameShellPath(hwnd_, path, &dest) == IconFsResult::Ok &&
        on_path_replaced_) {
      on_path_replaced_(path, dest);
    }
  } else if (cmd == 8) {
    ShowShellPathProperties(hwnd_, path);
  } else if (cmd == 9) {
    if (RecycleShellPath(hwnd_, path) == IconFsResult::Ok && on_removed_) {
      on_removed_(path);
    }
  }
}

int DockWindow::HitTest(int x, int y) const {
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& b = slots_[i].bounds;
    if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

LRESULT CALLBACK DockWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  DockWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<DockWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT DockWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd_, &ps);
      Paint(hdc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_LBUTTONUP: {
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0) {
        OpenShellPath(slots_[static_cast<size_t>(hit)].item.path);
      }
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

void DockWindow::Paint(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  HBRUSH bg = CreateSolidBrush(RGB(28, 30, 36));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 78, 92));
  HGDIOBJ old = SelectObject(hdc, pen);
  SelectObject(hdc, GetStockObject(NULL_BRUSH));
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 18, 18);
  SelectObject(hdc, old);
  DeleteObject(pen);
  for (const auto& s : slots_) {
    if (s.icon) {
      DrawIconEx(hdc, s.bounds.left, s.bounds.top, s.icon, kIcon, kIcon, 0,
                 nullptr, DI_NORMAL);
    }
  }
}
