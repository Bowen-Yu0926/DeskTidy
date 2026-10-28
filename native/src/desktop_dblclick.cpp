#include "desktop_dblclick.h"

#include <cstring>

namespace {

HHOOK g_mouse_hook = nullptr;
HHOOK g_key_hook = nullptr;
HWND g_notify = nullptr;
UINT g_msg = 0;
UINT g_down_msg = 0;
UINT g_paste_msg = 0;
DWORD g_last_fire_ms = 0;
DWORD g_last_down_ms = 0;
DWORD g_last_paste_ms = 0;
POINT g_last_rclick{-1, -1};

bool IsOwnOverlay(HWND hwnd) {
  wchar_t cls[128]{};
  while (hwnd) {
    cls[0] = 0;
    GetClassNameW(hwnd, cls, 128);
    if (wcsncmp(cls, L"DeskTidyNative", 14) == 0) {
      return true;
    }
    hwnd = GetParent(hwnd);
  }
  return false;
}

bool ClassIsDesktop(const wchar_t* cls) {
  return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
         wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
         wcscmp(cls, L"SysListView32") == 0 ||
         wcscmp(cls, L"DesktopWindowContentBridge") == 0 ||
         wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") ==
             0;
}

bool ForegroundIsDesktopShell() {
  HWND fg = GetForegroundWindow();
  if (!fg) {
    return false;
  }
  // Never steal Ctrl+V from Explorer folders, editors, or DeskTidy chrome
  // that already handles paste (fences / notes / public host).
  if (IsOwnOverlay(fg)) {
    return false;
  }
  wchar_t cls[128]{};
  HWND cur = fg;
  while (cur) {
    cls[0] = 0;
    GetClassNameW(cur, cls, 128);
    if (ClassIsDesktop(cls)) {
      return true;
    }
    cur = GetParent(cur);
  }
  HWND root = GetAncestor(fg, GA_ROOT);
  if (root) {
    cls[0] = 0;
    GetClassNameW(root, cls, 128);
    if (ClassIsDesktop(cls)) {
      return true;
    }
  }
  return false;
}

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    const auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
    if (info) {
      // Record desktop-background right-clicks (for shell-verb new-fence).
      if (wparam == WM_RBUTTONUP && IsDesktopBlankPoint(info->pt.x, info->pt.y)) {
        g_last_rclick = info->pt;
      }
      // Blank desktop (DefView / Progman). Also used when the transparent
      // listview sits above a fence: click never reaches the fence HWND, so
      // deselection must be driven from this hook (Explorer / Fences parity).
      if (wparam == WM_LBUTTONDOWN && g_down_msg && g_notify &&
          IsWindow(g_notify) && IsDesktopBlankPoint(info->pt.x, info->pt.y)) {
        const DWORD now = GetTickCount();
        if (now - g_last_down_ms >= 80) {
          g_last_down_ms = now;
          PostMessageW(g_notify, g_down_msg, 0,
                       MAKELPARAM(static_cast<WORD>(info->pt.x),
                                  static_cast<WORD>(info->pt.y)));
        }
      }
      if (wparam == WM_LBUTTONDBLCLK && g_msg && g_notify &&
          IsWindow(g_notify) && IsDesktopBlankPoint(info->pt.x, info->pt.y)) {
        const DWORD now = GetTickCount();
        if (now - g_last_fire_ms >= 400) {
          g_last_fire_ms = now;
          PostMessageW(g_notify, g_msg, 0, 0);
        }
      }
    }
  }
  return CallNextHookEx(g_mouse_hook, code, wparam, lparam);
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION && g_paste_msg && g_notify && IsWindow(g_notify)) {
    const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (info && (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) &&
        info->vkCode == 'V' &&
        (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
        (GetAsyncKeyState(VK_MENU) & 0x8000) == 0 &&
        ForegroundIsDesktopShell()) {
      const DWORD now = GetTickCount();
      if (now - g_last_paste_ms >= 200) {
        g_last_paste_ms = now;
        PostMessageW(g_notify, g_paste_msg, 0, 0);
        // Swallow so a half-hidden DefView does not also try (and fail) paste.
        return 1;
      }
    }
  }
  return CallNextHookEx(g_key_hook, code, wparam, lparam);
}

}  // namespace

bool IsDesktopBlankPoint(int screen_x, int screen_y) {
  POINT pt{screen_x, screen_y};
  HWND hwnd = WindowFromPoint(pt);
  if (!hwnd) {
    return false;
  }
  if (IsOwnOverlay(hwnd)) {
    return false;
  }
  HWND cur = hwnd;
  wchar_t cls[128]{};
  while (cur) {
    cls[0] = 0;
    GetClassNameW(cur, cls, 128);
    if (ClassIsDesktop(cls)) {
      return true;
    }
    cur = GetParent(cur);
  }
  HWND root = GetAncestor(hwnd, GA_ROOT);
  if (root) {
    cls[0] = 0;
    GetClassNameW(root, cls, 128);
    if (ClassIsDesktop(cls)) {
      return true;
    }
  }
  return false;
}

void StartDesktopDblClickMonitor(HWND notify_hwnd, UINT notify_msg,
                                 UINT notify_down_msg, UINT notify_paste_msg) {
  g_notify = notify_hwnd;
  g_msg = notify_msg;
  g_down_msg = notify_down_msg;
  g_paste_msg = notify_paste_msg;
  if (!g_mouse_hook) {
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                     GetModuleHandleW(nullptr), 0);
  }
  if (g_paste_msg && !g_key_hook) {
    g_key_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                   GetModuleHandleW(nullptr), 0);
  }
}

void StopDesktopDblClickMonitor() {
  if (g_mouse_hook) {
    UnhookWindowsHookEx(g_mouse_hook);
    g_mouse_hook = nullptr;
  }
  if (g_key_hook) {
    UnhookWindowsHookEx(g_key_hook);
    g_key_hook = nullptr;
  }
  g_notify = nullptr;
  g_msg = 0;
  g_down_msg = 0;
  g_paste_msg = 0;
}

bool GetLastDesktopRightClick(POINT* out) {
  if (!out) {
    return false;
  }
  if (g_last_rclick.x < 0 || g_last_rclick.y < 0) {
    return false;
  }
  *out = g_last_rclick;
  return true;
}
