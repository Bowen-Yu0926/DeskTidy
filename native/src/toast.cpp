#include "toast.h"

#include "desktop_shell.h"
#include "display_geom.h"

#include <algorithm>
#include <functional>
#include <string>

namespace {

constexpr wchar_t kClass[] = L"DeskTidyNativeToast";
constexpr UINT kDefaultMs = 2200;
constexpr UINT kActionMs = 8000;

HWND g_hwnd = nullptr;
UINT_PTR g_timer = 0;
bool g_reg = false;
std::wstring g_text;
HWND g_follow = nullptr;       // optional pet HWND — reposition while visible
HWND g_clear_above = nullptr;  // page-chip cloud etc. — keep speech above it
std::function<void()> g_on_click;
bool g_clickable = false;
int g_ww = 280;
int g_hh = 38;
int g_rgn_ww = 0;
int g_rgn_hh = 0;

int MeasureToastWidth(HDC hdc, const std::wstring& text) {
  RECT m{0, 0, 0, 0};
  HFONT font = CreateFontW(-13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  HGDIOBJ old_f = SelectObject(hdc, font);
  DrawTextW(hdc, text.c_str(), -1, &m, DT_CALCRECT | DT_SINGLELINE);
  SelectObject(hdc, old_f);
  DeleteObject(font);
  const int need = (m.right - m.left) + 36;
  return (std::max)(280, (std::min)(need, 540));
}

void PlaceToastWindow(HWND hwnd, HWND anchor, HWND clear_above) {
  if (!hwnd) {
    return;
  }
  const int ww = g_ww;
  const int hh = g_hh;
  int x = 0;
  int y = 0;
  if (anchor && IsWindow(anchor)) {
    RECT ar{};
    GetWindowRect(anchor, &ar);
    const RECT work = WorkAreaAtPoint(
        {(ar.left + ar.right) / 2, (ar.top + ar.bottom) / 2});
    x = (ar.left + ar.right) / 2 - ww / 2;
    int stack_top = ar.top;
    if (clear_above && IsWindow(clear_above) && IsWindowVisible(clear_above)) {
      RECT cr{};
      if (GetWindowRect(clear_above, &cr)) {
        if (cr.bottom >= ar.top - 200 && cr.top < ar.top) {
          stack_top = (std::min)(stack_top, static_cast<int>(cr.top));
        }
      }
    }
    y = stack_top - hh - 10;
    if (y < work.top + 4) {
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
  SetWindowPos(hwnd, nullptr, x, y, ww, hh, SWP_NOZORDER | SWP_NOACTIVATE);
  if (ww != g_rgn_ww || hh != g_rgn_hh) {
    HRGN rgn = CreateRoundRectRgn(0, 0, ww + 1, hh + 1, 16, 16);
    SetWindowRgn(hwnd, rgn, TRUE);
    g_rgn_ww = ww;
    g_rgn_hh = hh;
  }
}

void ArmDismissTimer(HWND hwnd, UINT ms) {
  if (g_timer) {
    KillTimer(hwnd, g_timer);
    g_timer = 0;
  }
  if (ms == 0) {
    ms = kDefaultMs;
  }
  g_timer = SetTimer(hwnd, 1, ms, nullptr);
}

LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_WINDOWPOSCHANGING) {
    FilterDesktopChromeWindowPosChanging(reinterpret_cast<WINDOWPOS*>(lparam));
  }
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_SETCURSOR:
      if (g_clickable) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_LBUTTONUP:
      if (g_clickable && g_on_click) {
        auto fn = std::move(g_on_click);
        g_on_click = nullptr;
        g_clickable = false;
        if (g_timer) {
          KillTimer(hwnd, g_timer);
          g_timer = 0;
        }
        DestroyWindow(hwnd);
        fn();
        return 0;
      }
      break;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);

      // Deep space acrylic capsule background with glow border
      HBRUSH br = CreateSolidBrush(RGB(20, 25, 36));
      HPEN pen = CreatePen(PS_SOLID, 1,
                           g_clickable ? RGB(59, 130, 246) : RGB(62, 76, 106));
      HGDIOBJ old_br = SelectObject(hdc, br);
      HGDIOBJ old_pen = SelectObject(hdc, pen);
      RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 16, 16);
      SelectObject(hdc, old_pen);
      SelectObject(hdc, old_br);
      DeleteObject(pen);
      DeleteObject(br);

      // Subtle top inner specular rim
      HPEN glow = CreatePen(PS_SOLID, 1, RGB(78, 96, 132));
      HGDIOBJ old_glow = SelectObject(hdc, glow);
      MoveToEx(hdc, rc.left + 16, rc.top + 1, nullptr);
      LineTo(hdc, rc.right - 16, rc.top + 1);
      SelectObject(hdc, old_glow);
      DeleteObject(glow);

      HFONT font = CreateFontW(-13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE,
                               L"Microsoft YaHei UI");
      HGDIOBJ old_f = SelectObject(hdc, font);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, g_clickable ? RGB(96, 165, 250) : RGB(241, 245, 255));
      RECT text = rc;
      InflateRect(&text, -12, -4);
      DrawTextW(hdc, g_text.c_str(), -1, &text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      SelectObject(hdc, old_f);
      DeleteObject(font);
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
        PlaceToastWindow(hwnd, g_follow, g_clear_above);
        // Keep speech in the desktop band while following the pet — owner-only
        // popups float in the app Z-order and cover browsers (market: Fences /
        // Rainmeter OnDesktop chrome stays under APPWINDOWs).
        if (!IsAttachedToDesktop(hwnd)) {
          AttachOverlayToDesktop(hwnd, true);
        }
        RaiseOverlayInDesktopBand(hwnd);
      }
      return 0;
    case WM_DESTROY:
      if (hwnd == g_hwnd) {
        g_hwnd = nullptr;
        g_follow = nullptr;
        g_clear_above = nullptr;
        g_on_click = nullptr;
        g_clickable = false;
        g_rgn_ww = 0;
        g_rgn_hh = 0;
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ShowToastImpl(HWND anchor, HWND clear_above, const std::wstring& text,
                   UINT duration_ms, std::function<void()> on_click) {
  if (text.empty()) {
    return;
  }
  // 1. 先安全销毁旧 toast 实例，避免 DestroyWindow 同步触发 WM_DESTROY 冲掉新状态
  if (g_hwnd) {
    HWND old = g_hwnd;
    g_hwnd = nullptr;
    if (g_timer) {
      KillTimer(old, g_timer);
      g_timer = 0;
    }
    KillTimer(old, 2);
    DestroyWindow(old);
  }
  // 2. 赋值本次最新的跟随与显示状态
  g_text = text;
  g_follow = (anchor && IsWindow(anchor)) ? anchor : nullptr;
  g_clear_above =
      (clear_above && IsWindow(clear_above)) ? clear_above : nullptr;
  g_on_click = std::move(on_click);
  g_clickable = static_cast<bool>(g_on_click);
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
  {
    HDC screen = GetDC(nullptr);
    g_ww = MeasureToastWidth(screen, g_text);
    ReleaseDC(nullptr, screen);
  }
  // Pet speech must SoftSeat to Progman/WorkerW (same as the pet). Creating
  // with pet as owner does NOT put the popup in the desktop band — it still
  // rises in the normal APPWINDOW Z-order and covers the browser.
  g_hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClass, L"",
      WS_POPUP, 0, 0, g_ww, g_hh, nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) {
    return;
  }
  PlaceToastWindow(g_hwnd, g_follow, g_clear_above);
  SetLayeredWindowAttributes(g_hwnd, 0, 235, LWA_ALPHA);
  if (g_follow) {
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    AttachOverlayToDesktop(g_hwnd, true);
    RaiseOverlayInDesktopBand(g_hwnd);
  } else {
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  }
  InvalidateRect(g_hwnd, nullptr, TRUE);
  ArmDismissTimer(g_hwnd, duration_ms ? duration_ms
                                      : (g_clickable ? kActionMs : kDefaultMs));
  if (g_follow) {
    SetTimer(g_hwnd, 2, 50, nullptr);
  }
}

void UpdateToastTextImpl(const std::wstring& text, UINT duration_ms) {
  if (text.empty()) {
    return;
  }
  if (!g_hwnd || !IsWindow(g_hwnd)) {
    ShowToastImpl(nullptr, nullptr, text, duration_ms ? duration_ms : 4000,
                  nullptr);
    return;
  }
  g_text = text;
  g_on_click = nullptr;
  g_clickable = false;
  HDC screen = GetDC(nullptr);
  const int nw = MeasureToastWidth(screen, g_text);
  ReleaseDC(nullptr, screen);
  if (nw != g_ww) {
    g_ww = nw;
    PlaceToastWindow(g_hwnd, g_follow, g_clear_above);
  }
  InvalidateRect(g_hwnd, nullptr, TRUE);
  ArmDismissTimer(g_hwnd, duration_ms ? duration_ms : 4000);
}

}  // namespace

void ShowToast(const std::wstring& text) {
  ShowToastImpl(nullptr, nullptr, text, kDefaultMs, nullptr);
}

void ShowToastNear(HWND anchor, const std::wstring& text, HWND clear_above) {
  ShowToastImpl(anchor, clear_above, text, kDefaultMs, nullptr);
}

void ShowToastTimed(const std::wstring& text, UINT duration_ms) {
  ShowToastImpl(nullptr, nullptr, text, duration_ms, nullptr);
}

void ShowActionToast(const std::wstring& text, std::function<void()> on_click) {
  ShowToastImpl(nullptr, nullptr, text, kActionMs, std::move(on_click));
}

void UpdateToastText(const std::wstring& text, UINT duration_ms) {
  UpdateToastTextImpl(text, duration_ms);
}
