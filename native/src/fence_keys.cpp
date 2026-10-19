#include "fence_keys.h"

#include <Windows.h>

namespace {
HHOOK g_hook = nullptr;

LRESULT CALLBACK FenceKeyProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION &&
      (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
    const auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    const DWORD vk = kb->vkCode;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool interesting =
        vk == VK_DELETE || vk == VK_F2 ||
        (ctrl && (vk == 'A' || vk == 'C' || vk == 'V' || vk == 'X'));
    if (interesting) {
      POINT pt{};
      GetCursorPos(&pt);
      HWND target = WindowFromPoint(pt);
      wchar_t cls[64] = {};
      if (target && GetClassNameW(target, cls, 64) &&
          wcscmp(cls, L"EDIT") == 0) {
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
          PostMessageW(target, WM_APP + 60, vk, ctrl ? 1 : 0);
          return 1;
        }
      }
    }
  }
  return CallNextHookEx(g_hook, code, wparam, lparam);
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
