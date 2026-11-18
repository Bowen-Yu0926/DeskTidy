#include "hotkey_store.h"

#include "display_geom.h"
#include "i18n.h"
#include "paths_util.h"
#include "prompt_text.h"

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::wstring HotkeyFile() { return JoinPath(NativeDataDir(), L"hotkeys.json"); }

std::string Escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '"') {
      o.push_back('\\');
    }
    o.push_back(c);
  }
  return o;
}

bool FindStringField(const std::string& obj, const char* key, std::string* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = obj.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = obj.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  p = obj.find('"', p + 1);
  if (p == std::string::npos) {
    return false;
  }
  ++p;
  std::string v;
  while (p < obj.size() && obj[p] != '"') {
    if (obj[p] == '\\' && p + 1 < obj.size()) {
      ++p;
    }
    v.push_back(obj[p++]);
  }
  *out = v;
  return true;
}

constexpr wchar_t kCaptureClass[] = L"DeskTidyNativeHotkeyCap";

struct CapState {
  UINT mods = 0;
  UINT vk = 0;
  bool done = false;
};

LRESULT CALLBACK CapProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* st = reinterpret_cast<CapState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  auto close_modal = [hwnd]() {
    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner) {
      EnableWindow(owner, TRUE);
    }
    DestroyWindow(hwnd);
  };
  switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      if (!st) {
        break;
      }
      const UINT vk = static_cast<UINT>(wparam);
      if (vk == VK_ESCAPE) {
        close_modal();
        return 0;
      }
      if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN ||
          vk == VK_RWIN) {
        return 0;
      }
      UINT mods = 0;
      if (GetKeyState(VK_CONTROL) & 0x8000) {
        mods |= MOD_CONTROL;
      }
      if (GetKeyState(VK_SHIFT) & 0x8000) {
        mods |= MOD_SHIFT;
      }
      if (GetKeyState(VK_MENU) & 0x8000) {
        mods |= MOD_ALT;
      }
      st->mods = mods;
      st->vk = vk;
      st->done = true;
      close_modal();
      return 0;
    }
    case WM_CLOSE:
      close_modal();
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool CaptureChord(HWND owner, UINT* mods, UINT* vk) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = CapProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kCaptureClass;
  RegisterClassExW(&wc);

  CapState st{};
  POINT cursor{};
  GetCursorPos(&cursor);
  int dlg_w = 320;
  int dlg_h = 120;
  ScaleDialogSizeForPoint(cursor, &dlg_w, &dlg_h);
  const int pad = ScalePxForDpi(20, DpiAtPoint(cursor));
  const int label_h = ScalePxForDpi(24, DpiAtPoint(cursor));
  HWND hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, kCaptureClass,
      Tr("hk_capture_title").c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0,
      dlg_w, dlg_h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!hwnd) {
    return false;
  }
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
  CreateWindowW(L"STATIC", Tr("hk_capture_hint").c_str(),
                WS_CHILD | WS_VISIBLE, pad, ScalePxForDpi(30, DpiAtPoint(cursor)),
                dlg_w - pad * 2, label_h, hwnd, nullptr,
                GetModuleHandleW(nullptr), nullptr);
  const RECT work = WorkAreaAtPoint(cursor);
  SetWindowPos(hwnd, nullptr,
               work.left + ((work.right - work.left) - dlg_w) / 2,
               work.top + ((work.bottom - work.top) - dlg_h) / 3, dlg_w, dlg_h,
               SWP_NOZORDER);
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
  if (owner) {
    EnableWindow(owner, FALSE);
  }
  MSG msg{};
  while (IsWindow(hwnd)) {
    const BOOL gm = GetMessageW(&msg, nullptr, 0, 0);
    if (gm == 0) {
      PostQuitMessage(static_cast<int>(msg.wParam));
      break;
    }
    if (gm < 0) {
      break;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (owner) {
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
  }
  if (IsWindow(hwnd)) {
    DestroyWindow(hwnd);
  }
  if (!st.done) {
    return false;
  }
  *mods = st.mods;
  *vk = st.vk;
  return true;
}

}  // namespace

std::vector<HotkeyDef> DefaultHotkeys() {
  // Match config/default_settings.json + HOTKEY_LABELS (Python).
  // Extra entries (settings/notes/…) ship unbound so用户可在设置里自行绑定.
  auto H = [](const char* id, const char* tr_key, UINT mods, UINT vk) {
    return HotkeyDef{id, Tr(tr_key), mods, vk};
  };
  return {
      H("organize", "hk_organize", MOD_CONTROL | MOD_SHIFT, 'O'),
      // Show/hide fences removed — keep id for prefs compat, unbound.
      H("toggle_fences", "hk_toggle_fences", 0, 0),
      H("toggle_icons", "hk_toggle_icons", MOD_CONTROL | MOD_SHIFT, 'H'),
      H("peek_fences", "hk_peek_fences", MOD_CONTROL, VK_SPACE),
      H("page_next", "hk_page_next", MOD_CONTROL | MOD_SHIFT, VK_RIGHT),
      H("page_prev", "hk_page_prev", MOD_CONTROL | MOD_SHIFT, VK_LEFT),
      // PRD P0-2 / market: bare F1–F4 collide with Help/Rename/Find — use
      // Ctrl+Alt+N (Fences-class tools also avoid naked F-keys as defaults).
      H("screenshot", "hk_screenshot", MOD_CONTROL | MOD_ALT, '1'),
      H("show_screenshot", "hk_show_screenshot", 0, VK_F2),
      H("screen_record", "hk_screen_record", MOD_CONTROL | MOD_ALT, '3'),
      H("file_search", "hk_file_search", MOD_CONTROL | MOD_ALT, '4'),
      H("calculator", "hk_calculator", MOD_CONTROL | MOD_ALT, 'C'),
      H("settings", "hk_settings", 0, 0),
      // DeskNote / 记事本 — match docs + Python Ctrl+Shift+N.
      H("notes", "hk_notes", MOD_CONTROL | MOD_SHIFT, 'N'),
      H("todos", "hk_todos", 0, 0),
  };
}

std::wstring FormatHotkey(UINT modifiers, UINT vk) {
  if (vk == 0) {
    return {};
  }
  std::wstring s;
  if (modifiers & MOD_WIN) {
    s += L"Win+";
  }
  if (modifiers & MOD_CONTROL) {
    s += L"Ctrl+";
  }
  if (modifiers & MOD_ALT) {
    s += L"Alt+";
  }
  if (modifiers & MOD_SHIFT) {
    s += L"Shift+";
  }
  if (vk == VK_SPACE) {
    s += L"Space";
  } else if (vk == VK_LEFT) {
    s += L"Left";
  } else if (vk == VK_RIGHT) {
    s += L"Right";
  } else if (vk == VK_UP) {
    s += L"Up";
  } else if (vk == VK_DOWN) {
    s += L"Down";
  } else if (vk >= VK_F1 && vk <= VK_F12) {
    s += L"F" + std::to_wstring(vk - VK_F1 + 1);
  } else if (vk >= '0' && vk <= '9') {
    s.push_back(static_cast<wchar_t>(vk));
  } else if (vk >= 'A' && vk <= 'Z') {
    s.push_back(static_cast<wchar_t>(vk));
  } else {
    wchar_t buf[16];
    swprintf_s(buf, L"0x%02X", vk);
    s += buf;
  }
  return s;
}

std::vector<HotkeyDef> LoadHotkeys() {
  auto defs = DefaultHotkeys();
  std::ifstream in(HotkeyFile());
  if (!in) {
    return defs;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  for (auto& d : defs) {
    size_t pos = 0;
    while (true) {
      const size_t start = json.find('{', pos);
      if (start == std::string::npos) {
        break;
      }
      const size_t end = json.find('}', start);
      if (end == std::string::npos) {
        break;
      }
      const std::string obj = json.substr(start, end - start + 1);
      pos = end + 1;
      std::string id, chord;
      if (!FindStringField(obj, "id", &id) || id != d.id) {
        continue;
      }
      if (FindStringField(obj, "chord", &chord)) {
        UINT mods = 0, vk = 0;
        if (ParseHotkey(WidenUtf8(chord), &mods, &vk) && vk != 0) {
          d.modifiers = mods;
          d.vk = vk;
        }
        // Empty / unbound chord in file: keep DefaultHotkeys() (notes = Ctrl+Shift+N).
      }
    }
  }
  return defs;
}

bool SaveHotkeys(const std::vector<HotkeyDef>& keys) {
  std::ostringstream out;
  out << "{\n  \"hotkeys\": [\n";
  for (size_t i = 0; i < keys.size(); ++i) {
    const auto& k = keys[i];
    out << "    {\"id\":\"" << Escape(k.id) << "\",\"chord\":\""
        << Escape(NarrowUtf8(FormatHotkey(k.modifiers, k.vk))) << "\"}";
    if (i + 1 < keys.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
  return AtomicWriteUtf8File(HotkeyFile(), out.str());
}

bool ParseHotkey(const std::wstring& text, UINT* modifiers, UINT* vk) {
  if (!modifiers || !vk) {
    return false;
  }
  *modifiers = 0;
  *vk = 0;
  std::wstring t = text;
  auto eat = [&](const wchar_t* token, UINT mod) {
    const size_t n = wcslen(token);
    if (t.size() >= n && _wcsnicmp(t.c_str(), token, n) == 0) {
      *modifiers |= mod;
      t.erase(0, n);
      return true;
    }
    return false;
  };
  while (eat(L"Win+", MOD_WIN) || eat(L"Windows+", MOD_WIN) ||
         eat(L"Meta+", MOD_WIN) || eat(L"Super+", MOD_WIN) ||
         eat(L"Ctrl+", MOD_CONTROL) || eat(L"Control+", MOD_CONTROL) ||
         eat(L"Alt+", MOD_ALT) || eat(L"Shift+", MOD_SHIFT)) {
  }
  if (t.empty()) {
    return false;
  }
  if (_wcsicmp(t.c_str(), L"Space") == 0) {
    *vk = VK_SPACE;
    return true;
  }
  if (_wcsicmp(t.c_str(), L"Left") == 0) {
    *vk = VK_LEFT;
    return true;
  }
  if (_wcsicmp(t.c_str(), L"Right") == 0) {
    *vk = VK_RIGHT;
    return true;
  }
  if (_wcsicmp(t.c_str(), L"Up") == 0) {
    *vk = VK_UP;
    return true;
  }
  if (_wcsicmp(t.c_str(), L"Down") == 0) {
    *vk = VK_DOWN;
    return true;
  }
  if (t.size() >= 2 && (t[0] == L'F' || t[0] == L'f')) {
    const int n = _wtoi(t.c_str() + 1);
    if (n >= 1 && n <= 12) {
      *vk = static_cast<UINT>(VK_F1 + n - 1);
      return true;
    }
  }
  if (t.size() == 1) {
    wchar_t c = t[0];
    if (c >= L'a' && c <= L'z') {
      c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    *vk = static_cast<UINT>(c);
    return true;
  }
  return false;
}

bool CaptureHotkeyChord(HWND owner, UINT* mods, UINT* vk) {
  return CaptureChord(owner, mods, vk);
}

std::vector<std::wstring> HotkeyFallbacks(const std::string& id) {
  // Mirror Python HOTKEY_FALLBACKS (hotkey_manager.py). Only actions whose
  // binding commonly collides get alternates; the rest return empty.
  if (id == "screenshot") {
    return {L"Ctrl+Alt+1", L"Ctrl+Shift+S", L"Ctrl+Alt+S"};
  }
  if (id == "show_screenshot") {
    return {L"F2", L"Ctrl+Alt+2", L"Ctrl+Shift+P", L"Ctrl+Alt+P"};
  }
  if (id == "screen_record") {
    return {L"Ctrl+Alt+3", L"Ctrl+Shift+R"};
  }
  if (id == "file_search") {
    return {L"Ctrl+Alt+4", L"Ctrl+Alt+F", L"Ctrl+Alt+Space"};
  }
  if (id == "calculator") {
    return {L"Ctrl+Alt+C"};
  }
  return {};
}
