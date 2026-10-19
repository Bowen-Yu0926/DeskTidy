#include "hotkey_store.h"

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
  switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      if (!st) {
        break;
      }
      const UINT vk = static_cast<UINT>(wparam);
      if (vk == VK_ESCAPE) {
        DestroyWindow(hwnd);
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
      DestroyWindow(hwnd);
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
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
  HWND hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, kCaptureClass, L"按下新快捷键…",
      WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 320, 120, owner, nullptr,
      GetModuleHandleW(nullptr), nullptr);
  if (!hwnd) {
    return false;
  }
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&st));
  CreateWindowW(L"STATIC", L"请按下组合键（Esc 取消）", WS_CHILD | WS_VISIBLE, 20,
                30, 280, 24, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  SetWindowPos(hwnd, nullptr, (sw - 320) / 2, (sh - 120) / 2, 320, 120,
               SWP_NOZORDER);
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
  if (owner) {
    EnableWindow(owner, FALSE);
  }
  MSG msg{};
  while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (owner) {
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
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
  return {
      {"settings", L"设置", MOD_CONTROL | MOD_SHIFT, 'S'},
      {"notes", L"便签", MOD_CONTROL | MOD_SHIFT, 'N'},
      {"fences", L"显示/隐藏分区", MOD_CONTROL | MOD_SHIFT, 'F'},
      {"peek", L"Peek", MOD_CONTROL, VK_SPACE},
      {"page1", L"第 1 页", MOD_CONTROL | MOD_ALT, '1'},
      {"page2", L"第 2 页", MOD_CONTROL | MOD_ALT, '2'},
      {"search", L"文件搜索", MOD_CONTROL | MOD_ALT, '4'},
      {"shot", L"区域截图", MOD_CONTROL | MOD_SHIFT, '1'},
  };
}

std::wstring FormatHotkey(UINT modifiers, UINT vk) {
  std::wstring s;
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
        if (ParseHotkey(WidenUtf8(chord), &mods, &vk)) {
          d.modifiers = mods;
          d.vk = vk;
        }
      }
    }
  }
  return defs;
}

bool SaveHotkeys(const std::vector<HotkeyDef>& keys) {
  std::ofstream out(HotkeyFile(), std::ios::binary);
  if (!out) {
    return false;
  }
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
  return true;
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
  while (eat(L"Ctrl+", MOD_CONTROL) || eat(L"Alt+", MOD_ALT) ||
         eat(L"Shift+", MOD_SHIFT)) {
  }
  if (t.empty()) {
    return false;
  }
  if (_wcsicmp(t.c_str(), L"Space") == 0) {
    *vk = VK_SPACE;
    return true;
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

void EditHotkeysDialog(HWND owner, std::vector<HotkeyDef>* keys) {
  if (!keys || keys->empty()) {
    return;
  }
  std::wstring msg = L"当前快捷键：\n\n";
  for (size_t i = 0; i < keys->size(); ++i) {
    msg += std::to_wstring(i + 1) + L". " + (*keys)[i].label + L"  =  " +
           FormatHotkey((*keys)[i].modifiers, (*keys)[i].vk) + L"\n";
  }
  msg += L"\n是 = 修改某一项　否 = 关闭";
  if (MessageBoxW(owner, msg.c_str(), L"快捷键", MB_YESNO | MB_ICONQUESTION) !=
      IDYES) {
    return;
  }
  const std::wstring prompt =
      L"输入编号 (1-" + std::to_wstring(keys->size()) + L")：";
  const std::wstring ans = PromptText(owner, L"修改快捷键", prompt.c_str(), L"1");
  if (ans.empty()) {
    return;
  }
  const int idx = _wtoi(ans.c_str()) - 1;
  if (idx < 0 || idx >= static_cast<int>(keys->size())) {
    MessageBoxW(owner, L"编号无效。", L"快捷键", MB_OK | MB_ICONWARNING);
    return;
  }
  UINT mods = 0, vk = 0;
  if (!CaptureChord(owner, &mods, &vk)) {
    return;
  }
  (*keys)[static_cast<size_t>(idx)].modifiers = mods;
  (*keys)[static_cast<size_t>(idx)].vk = vk;
  SaveHotkeys(*keys);
  MessageBoxW(owner,
              (L"已更新为 " + FormatHotkey(mods, vk) + L"\n重启热键注册中…").c_str(),
              L"快捷键", MB_OK | MB_ICONINFORMATION);
}
