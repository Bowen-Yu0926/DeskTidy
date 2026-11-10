#include "fence_keys.h"

#include <Windows.h>

namespace {

bool IsTypeAheadVk(DWORD vk) {
  if (vk >= '0' && vk <= '9') {
    return true;
  }
  if (vk >= 'A' && vk <= 'Z') {
    return true;
  }
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
    return true;
  }
  return false;
}

bool HandleFenceKeyDown(DWORD vk) {
  const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

  const bool is_base_key =
      (vk == VK_DELETE || vk == VK_F2 || vk == VK_ESCAPE || vk == VK_RETURN ||
       vk == VK_BACK || vk == VK_SPACE || vk == VK_HOME || vk == VK_END ||
       vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN);
  const bool is_clip_chord =
      (vk == 'A' || vk == 'C' || vk == 'V' || vk == 'X');
  const bool is_typeahead = IsTypeAheadVk(vk) && !ctrl && !alt;

  if (!is_base_key && !(is_clip_chord && ctrl) && !is_typeahead) {
    return false;
  }
  // Default Peek chord is Ctrl+Space — never swallow it over fence/public.
  if (vk == VK_SPACE && ctrl) {
    return false;
  }

  POINT pt{};
  GetCursorPos(&pt);
  HWND target = WindowFromPoint(pt);
  wchar_t cls[64] = {};
  if (target && GetClassNameW(target, cls, 64) && wcscmp(cls, L"EDIT") == 0) {
    // Inline rename owns keys — do not steal.
    return false;
  }
  // Fences / Explorer: same LL keys work over fence chrome and public floats.
  if (!target || (wcscmp(cls, L"DeskTidyNativeFence") != 0 &&
                  wcscmp(cls, L"DeskTidyNativePublic") != 0)) {
    return false;
  }
  HWND focus = GetFocus();
  wchar_t focus_cls[32] = {};
  if (focus) {
    GetClassNameW(focus, focus_cls, 32);
  }
  if (wcscmp(focus_cls, L"EDIT") == 0) {
    return false;
  }
  // lParam: bit0=ctrl, bit1=shift, bit2=typeahead
  LPARAM flags = (ctrl ? 1 : 0) | (shift ? 2 : 0);
  if (is_typeahead) {
    flags |= 4;
  }
  PostMessageW(target, WM_APP + 60, vk, flags);
  return true;
}

}  // namespace

bool TryHandleFenceLlKey(WPARAM wparam, LPARAM lparam) {
  if (wparam != WM_KEYDOWN && wparam != WM_SYSKEYDOWN) {
    return false;
  }
  const auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
  if (!kb) {
    return false;
  }
  return HandleFenceKeyDown(kb->vkCode);
}

void InstallFenceKeyHook() {
  // No separate WH_KEYBOARD_LL — App owns the single LL keyboard hook.
}

void RemoveFenceKeyHook() {}
