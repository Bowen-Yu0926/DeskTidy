#include "fence_keys.h"

#include "desktop_dblclick.h"

#include <Windows.h>

namespace {

HWND g_key_target = nullptr;

// The fence quick-filter EDIT is marked with this property at creation so the
// key hook can tell it apart from a rename EDIT (which must keep its own keys).
constexpr wchar_t kFilterEditProp[] = L"desktidy_fence_filter";

bool IsFenceFilterEdit(HWND hwnd) {
  return hwnd && GetPropW(hwnd, kFilterEditProp) != nullptr;
}

bool IsFenceOrPublicClass(const wchar_t* cls) {
  return cls && (wcscmp(cls, L"DeskTidyNativeFence") == 0 ||
                 wcscmp(cls, L"DeskTidyNativePublic") == 0);
}

bool IsFenceOrPublicHwnd(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) {
    return false;
  }
  wchar_t cls[64] = {};
  GetClassNameW(hwnd, cls, 64);
  return IsFenceOrPublicClass(cls);
}

// Fences keep keyboard while desktop shell OR a fence/public chrome is
// foreground (public host SetFocus; fences stay MA_NOACTIVATE on Progman).
bool ForegroundAllowsFenceKeys() {
  if (ForegroundIsDesktopShell()) {
    return true;
  }
  HWND fg = GetForegroundWindow();
  // The quick-filter box takes the foreground while it is open; its keys must
  // still reach the fence that owns it.
  if (IsFenceFilterEdit(fg)) {
    return true;
  }
  return IsFenceOrPublicHwnd(fg);
}

HWND ResolveFenceKeyTarget(POINT pt) {
  HWND under = WindowFromPoint(pt);
  wchar_t cls[64] = {};
  if (under) {
    GetClassNameW(under, cls, 64);
  }
  if (under && wcscmp(cls, L"EDIT") == 0 && !IsFenceFilterEdit(under)) {
    // Inline rename owns keys — do not steal. The fence quick-filter box is
    // the exception: it cannot receive keys itself, so the hook drives it.
    return nullptr;
  }
  // Settings / notes / Explorer folders own the keyboard — never steal.
  if (!ForegroundAllowsFenceKeys()) {
    return nullptr;
  }
  // Prefer last-activated fence/public (Fences activation model).
  if (IsFenceOrPublicHwnd(g_key_target)) {
    return g_key_target;
  }
  // Filter box has the foreground: route its keys to the owning fence.
  if (IsFenceFilterEdit(GetForegroundWindow())) {
    HWND owner = GetWindow(GetForegroundWindow(), GW_OWNER);
    if (IsFenceOrPublicHwnd(owner)) {
      return owner;
    }
  }
  // Fallback: cursor directly over fence/public chrome.
  if (IsFenceOrPublicClass(cls)) {
    return under;
  }
  // Organized desktop: DefView has no focus — float host owns Explorer chords
  // (Ctrl+A/C/X/Z) even before the first blank click sets g_key_target.
  HWND pub = FindWindowW(L"DeskTidyNativePublic", nullptr);
  if (pub && IsWindow(pub)) {
    return pub;
  }
  return nullptr;
}

// Translate VK → Unicode when possible (Explorer typeahead / IME Latin).
bool TryTypeAheadChar(DWORD vk, wchar_t* out_ch) {
  if (!out_ch) {
    return false;
  }
  *out_ch = 0;
  BYTE state[256] = {};
  if (!GetKeyboardState(state)) {
    return false;
  }
  WCHAR buf[8] = {};
  const int n = ToUnicodeEx(vk, MapVirtualKeyW(vk, MAPVK_VK_TO_VSC), state, buf,
                            8, 0, GetKeyboardLayout(0));
  if (n < 1) {
    return false;
  }
  const wchar_t ch = buf[0];
  // Printable (BMP): letters, digits, CJK — skip controls / space (Space selects).
  if (ch < 0x20 || ch == 0x7F || ch == L' ') {
    return false;
  }
  *out_ch = ch;
  return true;
}

bool HandleFenceKeyDown(DWORD vk) {
  const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;

  const bool is_base_key =
      (vk == VK_DELETE || vk == VK_F2 || vk == VK_F3 || vk == VK_F5 ||
       vk == VK_ESCAPE || vk == VK_RETURN || vk == VK_BACK ||
       vk == VK_BROWSER_BACK || vk == VK_BROWSER_FORWARD || vk == VK_SPACE ||
       vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
       vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
       vk == VK_APPS);
  // Explorer / Fences: Ctrl+/- (main + numpad) zoom; Ctrl+0 resets to 100%.
  const bool is_zoom_chord =
      ctrl && !shift && !alt &&
      (vk == VK_OEM_PLUS || vk == VK_ADD || vk == VK_OEM_MINUS ||
       vk == VK_SUBTRACT || vk == '0' || vk == VK_NUMPAD0);
  // Include Ctrl+Shift+V (paste shortcut) — shift must not drop the chord.
  const bool is_clip_chord =
      ctrl && !alt &&
      (vk == 'A' || vk == 'C' || vk == 'V' || vk == 'X' || vk == 'Z' ||
       vk == 'Y');
  // Explorer: Ctrl+Insert = copy; Shift+Insert = paste (+Ctrl = pastelink).
  const bool is_insert_clip =
      !alt && vk == VK_INSERT && (ctrl || shift);
  // Ctrl+F opens the fence's quick filter box.
  const bool is_filter_chord = ctrl && !shift && !alt && vk == 'F';
  // Explorer: Ctrl+Shift+N new folder. Swallow only when a fence/public target
  // resolves — otherwise DeskNote's RegisterHotKey(Ctrl+Shift+N) keeps it.
  const bool is_new_folder = ctrl && shift && !alt && vk == 'N';
  // Explorer: Shift+F10 → context menu (same as Apps key).
  const bool is_shift_f10 = shift && !ctrl && !alt && vk == VK_F10;
  wchar_t type_ch = 0;
  // Shift+letter still typeahead (case-insensitive match); Ctrl/Alt do not.
  const bool is_typeahead =
      !ctrl && !alt && !is_shift_f10 && !is_insert_clip &&
      TryTypeAheadChar(vk, &type_ch);

  if (!is_base_key && !is_clip_chord && !is_typeahead && !is_new_folder &&
      !is_shift_f10 && !is_insert_clip && !is_zoom_chord &&
      !is_filter_chord) {
    return false;
  }
  // Default Peek chord is Ctrl+Space — never swallow it over fence/public.
  if (vk == VK_SPACE && ctrl) {
    return false;
  }

  POINT pt{};
  GetCursorPos(&pt);
  HWND target = ResolveFenceKeyTarget(pt);
  if (!target) {
    return false;
  }
  HWND focus = GetFocus();
  wchar_t focus_cls[32] = {};
  if (focus) {
    GetClassNameW(focus, focus_cls, 32);
  }
  // The fence filter box is a popup EDIT that never owns the input queue (the
  // fence itself is never activated), so its characters must keep coming
  // through this hook. A rename EDIT is a real focused control — let it be.
  if (wcscmp(focus_cls, L"EDIT") == 0 && !IsFenceFilterEdit(focus)) {
    return false;
  }
  // lParam: bit0=ctrl, bit1=shift, bit2=typeahead (wParam = Unicode char)
  LPARAM flags = (ctrl ? 1 : 0) | (shift ? 2 : 0);
  if (is_typeahead) {
    flags |= 4;
    PostMessageW(target, WM_APP + 60, static_cast<WPARAM>(type_ch), flags);
    return true;
  }
  PostMessageW(target, WM_APP + 60, vk, flags);
  return true;
}

}  // namespace

void SetFenceKeyTarget(HWND hwnd) {
  if (IsFenceOrPublicHwnd(hwnd)) {
    g_key_target = hwnd;
  }
}

void ClearFenceKeyTarget() { g_key_target = nullptr; }

HWND GetFenceKeyTarget() {
  return IsFenceOrPublicHwnd(g_key_target) ? g_key_target : nullptr;
}

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
