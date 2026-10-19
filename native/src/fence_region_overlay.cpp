#include "fence_region_overlay.h"

#include "display_geom.h"

#include <windowsx.h>

#include <algorithm>

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeFenceRegion";
constexpr int kMinW = 160;
constexpr int kMinH = 180;
bool g_reg = false;
}  // namespace

bool FenceRegionOverlay::Begin(HINSTANCE instance, DoneFn on_done) {
  on_done_ = std::move(on_done);
  if (!g_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = FenceRegionOverlay::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_reg = true;
  }
  const RECT vs = VirtualScreenRect();
  hwnd_ = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClass, L"FenceRegion",
      WS_POPUP, vs.left, vs.top, vs.right - vs.left, vs.bottom - vs.top, nullptr,
      nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 110, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
  SetCapture(hwnd_);
  return true;
}

void FenceRegionOverlay::Cancel() {
  if (hwnd_) {
    ReleaseCapture();
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void FenceRegionOverlay::Finish() {
  RECT sel{(std::min)(start_.x, end_.x), (std::min)(start_.y, end_.y),
           (std::max)(start_.x, end_.x), (std::max)(start_.y, end_.y)};
  POINT tl{sel.left, sel.top};
  POINT br{sel.right, sel.bottom};
  ClientToScreen(hwnd_, &tl);
  ClientToScreen(hwnd_, &br);
  RECT screen{tl.x, tl.y, br.x, br.y};
  auto cb = std::move(on_done_);
  Cancel();
  if (screen.right - screen.left >= kMinW &&
      screen.bottom - screen.top >= kMinH && cb) {
    cb(screen);
  }
}

LRESULT CALLBACK FenceRegionOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                             LPARAM lparam) {
  FenceRegionOverlay* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<FenceRegionOverlay*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<FenceRegionOverlay*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT FenceRegionOverlay::HandleMessage(UINT msg, WPARAM wparam,
                                          LPARAM lparam) {
  switch (msg) {
    case WM_LBUTTONDOWN:
      dragging_ = true;
      start_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      end_ = start_;
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_MOUSEMOVE:
      if (dragging_) {
        end_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP:
      if (dragging_) {
        dragging_ = false;
        end_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        Finish();
      }
      return 0;
    case WM_RBUTTONUP:
    case WM_KEYDOWN:
      if (msg == WM_KEYDOWN && wparam != VK_ESCAPE) {
        break;
      }
      Cancel();
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd_, &ps);
      Paint(hdc);
      EndPaint(hwnd_, &ps);
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

void FenceRegionOverlay::Paint(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  HBRUSH dim = CreateSolidBrush(RGB(20, 20, 28));
  FillRect(hdc, &rc, dim);
  DeleteObject(dim);
  if (!dragging_ && start_.x == end_.x) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(240, 240, 240));
    DrawTextW(hdc, L"拖拽框选新建分区 · Esc 取消", -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return;
  }
  RECT sel{(std::min)(start_.x, end_.x), (std::min)(start_.y, end_.y),
           (std::max)(start_.x, end_.x), (std::max)(start_.y, end_.y)};
  HBRUSH clear = CreateSolidBrush(RGB(60, 120, 200));
  FillRect(hdc, &sel, clear);
  DeleteObject(clear);
  HPEN pen = CreatePen(PS_SOLID, 2, RGB(120, 200, 255));
  HGDIOBJ old = SelectObject(hdc, pen);
  SelectObject(hdc, GetStockObject(NULL_BRUSH));
  Rectangle(hdc, sel.left, sel.top, sel.right, sel.bottom);
  SelectObject(hdc, old);
  DeleteObject(pen);
}
