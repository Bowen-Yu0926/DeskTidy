#include "toast.h"

#include "desktop_shell.h"
#include "display_geom.h"

#include <algorithm>
#include <string>

namespace {

constexpr wchar_t kClass[] = L"DeskTidyNativeToast";
HWND g_hwnd = nullptr;
UINT_PTR g_timer = 0;
bool g_reg = false;
std::wstring g_text;
HWND g_follow = nullptr;      // optional pet HWND — reposition while visible
HWND g_clear_above = nullptr;  // page-chip cloud etc. — keep speech above it

void PlaceToastWindow(HWND hwnd, HWND anchor, HWND clear_above) {
  if (!hwnd) {
    return;
  }
  const int ww = 280;
  const int hh = 36;
  int x = 0;
  int y = 0;
  if (anchor && IsWindow(anchor)) {
    RECT ar{};
    GetWindowRect(anchor, &ar);
    const RECT work = WorkAreaAtPoint(
        {(ar.left + ar.right) / 2, (ar.top + ar.bottom) / 2});
    x = (ar.left + ar.right) / 2 - ww / 2;
    // Stack top = pet top, or page-bubble cloud top when that chrome is up.
    int stack_top = ar.top;
    if (clear_above && IsWindow(clear_above) && IsWindowVisible(clear_above)) {
      RECT cr{};
      GetWindowRect(clear_above, &cr);
      stack_top = (std::min)(stack_top, static_cast<int>(cr.top));
    }
    y = stack_top - hh - 10;
    if (y < work.top + 4) {
      // Near top of screen → hang below the pet instead (still clear of chips).
      y = ar.bottom + 8;
    }
    x = (std::max)(static_cast<int>(work.left + 4),
                   (std::min)(x, static_cast<int>(work.right - ww - 4)));
    y = (std::max)(static_cast<int>(work.top + 4),
                   (std::min)(y, static_cast<int>(work.bottom - hh - 4)));
  } else {
    const RECT work = PrimaryWorkArea();
    x = work.left + (work.right - work.left - ww) / 2;
    y = work.top + 28;
  }
  SetWindowPos(hwnd, nullptr, x, y, ww, hh,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      // Soft speech-bubble chrome.
      HBRUSH br = CreateSolidBrush(RGB(255, 252, 245));
      FillRect(hdc, &rc, br);
      DeleteObject(br);
      HPEN pen = CreatePen(PS_SOLID, 1, RGB(210, 190, 150));
      HGDIOBJ old_pen = SelectObject(hdc, pen);
      HGDIOBJ old_br = SelectObject(hdc, GetStockObject(NULL_BRUSH));
      Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
      SelectObject(hdc, old_br);
      SelectObject(hdc, old_pen);
      DeleteObject(pen);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(40, 36, 30));
      RECT text = rc;
      InflateRect(&text, -8, -4);
      DrawTextW(hdc, g_text.c_str(), -1, &text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_TIMER:
      if (wparam == 1) {
        if (g_timer) {
          KillTimer(hwnd, g_timer);
          g_timer = 0;
        }
        DestroyWindow(hwnd);
      } else if (wparam == 2 && g_follow && IsWindow(g_follow)) {
        // Keep bubble above a moving pet (and above page chips).
        PlaceToastWindow(hwnd, g_follow, g_clear_above);
        if (IsAttachedToDesktop(g_follow)) {
          AttachOverlayToDesktop(hwnd, true);
          RaiseOverlayInDesktopBand(hwnd);
        }
      }
      return 0;
    case WM_DESTROY:
      g_hwnd = nullptr;
      g_follow = nullptr;
      g_clear_above = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ShowToastImpl(HWND anchor, HWND clear_above, const std::wstring& text) {
  if (text.empty()) {
    return;
  }
  g_text = text;
  g_follow = (anchor && IsWindow(anchor)) ? anchor : nullptr;
  g_clear_above =
      (clear_above && IsWindow(clear_above)) ? clear_above : nullptr;
  HINSTANCE inst = GetModuleHandleW(nullptr);
  if (!g_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToastProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_reg = true;
  }
  if (g_hwnd) {
    if (g_timer) {
      KillTimer(g_hwnd, g_timer);
      g_timer = 0;
    }
    KillTimer(g_hwnd, 2);
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
  const int ww = 280;
  const int hh = 36;
  g_hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClass, L"",
      WS_POPUP, 0, 0, ww, hh, nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) {
    return;
  }
  PlaceToastWindow(g_hwnd, g_follow, g_clear_above);
  SetLayeredWindowAttributes(g_hwnd, 0, 235, LWA_ALPHA);
  // Seat with the pet in the desktop band so it is not a TOPMOST strip
  // floating over fences at the top of the screen.
  if (g_follow && IsAttachedToDesktop(g_follow)) {
    AttachOverlayToDesktop(g_hwnd, true);
    RaiseOverlayInDesktopBand(g_hwnd);
  } else {
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  InvalidateRect(g_hwnd, nullptr, TRUE);
  g_timer = SetTimer(g_hwnd, 1, 2200, nullptr);
  if (g_follow) {
    SetTimer(g_hwnd, 2, 50, nullptr);
  }
}

}  // namespace

void ShowToast(const std::wstring& text) {
  ShowToastImpl(nullptr, nullptr, text);
}

void ShowToastNear(HWND anchor, const std::wstring& text, HWND clear_above) {
  ShowToastImpl(anchor, clear_above, text);
}
