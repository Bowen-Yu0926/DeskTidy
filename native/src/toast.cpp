#include "toast.h"

#include "display_geom.h"

#include <string>

namespace {

constexpr wchar_t kClass[] = L"DeskTidyNativeToast";
HWND g_hwnd = nullptr;
UINT_PTR g_timer = 0;
bool g_reg = false;
std::wstring g_text;

LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      HBRUSH br = CreateSolidBrush(RGB(32, 36, 44));
      FillRect(hdc, &rc, br);
      DeleteObject(br);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(240, 242, 245));
      DrawTextW(hdc, g_text.c_str(), -1, &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_TIMER:
      if (g_timer) {
        KillTimer(hwnd, g_timer);
        g_timer = 0;
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      g_hwnd = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

void ShowToast(const std::wstring& text) {
  if (text.empty()) {
    return;
  }
  g_text = text;
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
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
  const RECT work = PrimaryWorkArea();
  const int ww = 320;
  const int hh = 40;
  const int x = work.left + (work.right - work.left - ww) / 2;
  const int y = work.top + 28;
  g_hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
      kClass, L"", WS_POPUP, x, y, ww, hh, nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) {
    return;
  }
  SetLayeredWindowAttributes(g_hwnd, 0, 230, LWA_ALPHA);
  ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
  InvalidateRect(g_hwnd, nullptr, TRUE);
  g_timer = SetTimer(g_hwnd, 1, 2200, nullptr);
}
