#include "fence_keys.h"

#include <Windows.h>

namespace {
HHOOK g_hook = nullptr;

LRESULT CALLBACK FenceKeyProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION &&
      (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
    const auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (!kb) {
      return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    const DWORD vk = kb->vkCode;
    // Fast path: bypass everything if key cannot belong to a fence shortcut.
    const bool is_base_key =
        (vk == VK_DELETE || vk == VK_F2 || vk == VK_ESCAPE ||
         vk == VK_RETURN || vk == VK_SPACE || vk == VK_HOME || vk == VK_END ||
         vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN);
    const bool is_alpha_candidate = (vk == 'A' || vk == 'C' || vk == 'V' || vk == 'X');
    if (!is_base_key && !is_alpha_candidate) {
      return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (is_alpha_candidate && !ctrl) {
      return CallNextHookEx(nullptr, code, wparam, lparam);
    }
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool interesting = is_base_key || (ctrl && is_alpha_candidate);
    if (interesting) {
      POINT pt{};
      GetCursorPos(&pt);
      HWND target = WindowFromPoint(pt);
      wchar_t cls[64] = {};
      if (target && GetClassNameW(target, cls, 64) &&
          wcscmp(cls, L"EDIT") == 0) {
        // Inline rename owns Esc/Enter/Space — do not steal.
        if (vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_SPACE) {
          return CallNextHookEx(nullptr, code, wparam, lparam);
        }
        target = GetParent(target);
        cls[0] = 0;
        if (target) {
          GetClassNameW(target, cls, 64);
        }
      }
      if (target && wcscmp(cls, L"DeskTidyNativeFence") == 0) {
        HWND focus = GetFocus();
        wchar_t focus_cls[32] = {};
        if (focus) {
          GetClassNameW(focus, focus_cls, 32);
        }
        if (wcscmp(focus_cls, L"EDIT") != 0) {
          // lParam: bit0=ctrl, bit1=shift
          const LPARAM flags = (ctrl ? 1 : 0) | (shift ? 2 : 0);
          PostMessageW(target, WM_APP + 60, vk, flags);
          return 1;
        }
      }
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}
}  // namespace

void InstallFenceKeyHook() {
  if (!g_hook) {
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, FenceKeyProc, GetModuleHandleW(nullptr),
                               0);
  }
}

void RemoveFenceKeyHook() {
  if (g_hook) {
    UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
  }
}
