#include "desktop_dblclick.h"

#include <cstdlib>
#include <cstring>

namespace {

HHOOK g_mouse_hook = nullptr;
HHOOK g_key_hook = nullptr;
HWND g_notify = nullptr;
UINT g_msg = 0;
UINT g_down_msg = 0;
UINT g_paste_msg = 0;
UINT g_blank_rclick_msg = 0;
UINT g_rubber_move_msg = 0;
UINT g_rubber_up_msg = 0;
bool g_rubber_dragging = false;
bool g_rubber_moved = false;
POINT g_rubber_origin{-1, -1};
DWORD g_last_rubber_move_ms = 0;
// Matches PublicHost::kDragThreshold so the hook and the host agree on when a
// press has become a drag.
constexpr int kRubberThreshold = 6;
bool g_organized_blank_menu = false;
DWORD g_last_fire_ms = 0;
DWORD g_last_down_ms = 0;
DWORD g_last_paste_ms = 0;
DWORD g_last_blank_rclick_ms = 0;
POINT g_last_rclick{-1, -1};
ULONGLONG g_last_rclick_tick = 0;

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
         wcscmp(cls, L"DirectUIHWND") == 0 ||
         wcscmp(cls, L"DesktopWindowContentBridge") == 0 ||
         wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") ==
             0;
}

LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    const auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
    if (info) {
      // Record desktop-background right-clicks (for shell-verb new-fence position).
      // Never swallow desktop right-clicks: let Windows Explorer host its full
      // native desktop menu (View / Sort / Refresh / Display / Personalize / New)
      // and manage its dismiss lifecycle naturally.
      if (wparam == WM_RBUTTONUP && IsDesktopBlankPoint(info->pt.x, info->pt.y)) {
        g_last_rclick = info->pt;
        g_last_rclick_tick = GetTickCount64();
      }
      // Blank desktop (DefView / Progman). Also used when the transparent
      // listview sits above a fence: click never reaches the fence HWND, so
      // deselection must be driven from this hook (Explorer / Fences parity).
      if (wparam == WM_LBUTTONDOWN && g_down_msg && g_notify &&
          IsWindow(g_notify) && IsDesktopBlankPoint(info->pt.x, info->pt.y)) {
        const DWORD now = GetTickCount();
        if (now - g_last_down_ms >= 80) {
          g_last_down_ms = now;
          g_rubber_dragging = true;
          g_rubber_moved = false;
          g_rubber_origin = {info->pt.x, info->pt.y};
          g_last_rubber_move_ms = 0;
          PostMessageW(g_notify, g_down_msg, 0,
                       MAKELPARAM(static_cast<WORD>(info->pt.x),
                                  static_cast<WORD>(info->pt.y)));
        }
      }
      // Feed the desktop marquee from the hook — but only after the press has
      // actually become a drag. Forwarding every WM_MOUSEMOVE floods the UI
      // thread's message queue (mouse moves arrive at several hundred Hz),
      // which starves it of everything else and makes the whole desktop feel
      // frozen. Threshold + throttle keep the hook cheap.
      if (g_rubber_dragging && g_notify && IsWindow(g_notify)) {
        if (wparam == WM_MOUSEMOVE && g_rubber_move_msg) {
          if (!g_rubber_moved &&
              (abs(info->pt.x - g_rubber_origin.x) >= kRubberThreshold ||
               abs(info->pt.y - g_rubber_origin.y) >= kRubberThreshold)) {
            g_rubber_moved = true;
          }
          const DWORD now = GetTickCount();
          if (g_rubber_moved && now - g_last_rubber_move_ms >= 16) {
            g_last_rubber_move_ms = now;
            PostMessageW(g_notify, g_rubber_move_msg, 0,
                         MAKELPARAM(static_cast<WORD>(info->pt.x),
                                    static_cast<WORD>(info->pt.y)));
          }
        } else if (wparam == WM_LBUTTONUP && g_rubber_up_msg) {
          const bool was_moved = g_rubber_moved;
          g_rubber_dragging = false;
          g_rubber_moved = false;
          // Only tell the host about a real marquee; a plain click needs no
          // update at all.
          if (was_moved) {
            PostMessageW(g_notify, g_rubber_up_msg, 0,
                         MAKELPARAM(static_cast<WORD>(info->pt.x),
                                    static_cast<WORD>(info->pt.y)));
          }
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

}  // namespace

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

bool TryHandleDesktopPasteLlKey(WPARAM wparam, LPARAM lparam) {
  if (!g_paste_msg || !g_notify || !IsWindow(g_notify)) {
    return false;
  }
  if (wparam != WM_KEYDOWN && wparam != WM_SYSKEYDOWN) {
    return false;
  }
  const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
  if (!info || info->vkCode != 'V') {
    return false;
  }
  if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0 ||
      (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
      !ForegroundIsDesktopShell()) {
    return false;
  }
  // Explorer / Fences: Ctrl+Shift+V → 粘贴快捷方式 (wParam=1).
  const bool as_link = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  const DWORD now = GetTickCount();
  if (now - g_last_paste_ms < 200) {
    return false;
  }
  g_last_paste_ms = now;
  PostMessageW(g_notify, g_paste_msg, as_link ? 1 : 0, 0);
  // Swallow so a half-hidden DefView does not also try (and fail) paste.
  return true;
}

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
                                 UINT notify_down_msg, UINT notify_paste_msg,
                                 UINT notify_blank_rclick_msg,
                                 UINT notify_rubber_move_msg,
                                 UINT notify_rubber_up_msg) {
  g_notify = notify_hwnd;
  g_msg = notify_msg;
  g_down_msg = notify_down_msg;
  g_paste_msg = notify_paste_msg;
  g_blank_rclick_msg = notify_blank_rclick_msg;
  g_rubber_move_msg = notify_rubber_move_msg;
  g_rubber_up_msg = notify_rubber_up_msg;
  if (!g_mouse_hook) {
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc,
                                     GetModuleHandleW(nullptr), 0);
  }
  // Keyboard paste rides App's single WH_KEYBOARD_LL (no second key hook).
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
  g_blank_rclick_msg = 0;
  g_rubber_move_msg = 0;
  g_rubber_up_msg = 0;
  g_rubber_dragging = false;
  g_rubber_moved = false;
  g_rubber_origin = {-1, -1};
  g_last_rubber_move_ms = 0;
  g_organized_blank_menu = false;
}

void SetDesktopOrganizedBlankMenu(bool organized) {
  g_organized_blank_menu = organized;
}

bool GetLastDesktopRightClick(POINT* out, ULONGLONG* tick_ms) {
  if (g_last_rclick.x < 0 || g_last_rclick.y < 0) {
    return false;
  }
  if (out) {
    *out = g_last_rclick;
  }
  if (tick_ms) {
    *tick_ms = g_last_rclick_tick;
  }
  return true;
}
