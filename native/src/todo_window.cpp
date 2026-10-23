#include "todo_window.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "i18n.h"
#include "mist_webview.h"
#include "paths_util.h"
#include "ui_theme.h"

#include <Windows.h>

#include <objbase.h>
#ifndef interface
#define interface struct
#endif
#include <WebView2.h>
#include <wrl.h>

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeTodo";
constexpr wchar_t kHostClass[] = L"DeskTidyNativeTodoWeb";

std::wstring TodosPath() { return JoinPath(NativeDataDir(), L"todos.json"); }
std::wstring TodosTxtPath() { return JoinPath(NativeDataDir(), L"todos.txt"); }
std::wstring TodosGeoPath() {
  return JoinPath(NativeDataDir(), L"todos_geo.json");
}

int ReadGeoInt(const std::string& json, const char* key, int fallback) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return fallback;
  }
  p = json.find(':', p);
  if (p == std::string::npos) {
    return fallback;
  }
  return atoi(json.c_str() + p + 1);
}

bool ReadTextFile(const std::wstring& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

// Index just past the closing quote of a JSON string starting at s[start]=='"'.
size_t SkipJsonString(const std::string& s, size_t start) {
  size_t i = start + 1;
  while (i < s.size()) {
    if (s[i] == '\\') {
      i += 2;
      continue;
    }
    if (s[i] == '"') {
      return i + 1;
    }
    ++i;
  }
  return s.size();
}

// Index just past the JSON value starting at s[start].
size_t SkipJsonValue(const std::string& s, size_t start) {
  if (start >= s.size()) {
    return start;
  }
  const char c = s[start];
  if (c == '"') {
    return SkipJsonString(s, start);
  }
  if (c == '{' || c == '[') {
    int depth = 0;
    for (size_t i = start; i < s.size(); ++i) {
      if (s[i] == '"') {
        i = SkipJsonString(s, i) - 1;
        continue;
      }
      if (s[i] == '{' || s[i] == '[') {
        ++depth;
      } else if (s[i] == '}' || s[i] == ']') {
        --depth;
        if (depth == 0) {
          return i + 1;
        }
      }
    }
    return s.size();
  }
  size_t i = start;
  while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
         s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') {
    ++i;
  }
  return i;
}

std::string UnescapeJsonString(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c != '\\' || i + 1 >= s.size()) {
      out.push_back(c);
      continue;
    }
    const char n = s[i + 1];
    switch (n) {
      case '"': out.push_back('"'); ++i; break;
      case '\\': out.push_back('\\'); ++i; break;
      case '/': out.push_back('/'); ++i; break;
      case 'b': out.push_back('\b'); ++i; break;
      case 'f': out.push_back('\f'); ++i; break;
      case 'n': out.push_back('\n'); ++i; break;
      case 'r': out.push_back('\r'); ++i; break;
      case 't': out.push_back('\t'); ++i; break;
      case 'u': {
        if (i + 5 < s.size()) {
          unsigned cp = 0;
          for (int k = 0; k < 4; ++k) {
            const char h = s[i + 2 + k];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
            else { cp = 0; break; }
          }
          if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
          } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          }
          i += 5;
        } else {
          out.push_back('\\');
          out.push_back('u');
          ++i;
        }
        break;
      }
      default: out.push_back(n); ++i; break;
    }
  }
  return out;
}

std::string EscapeJsonString(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          sprintf_s(buf, "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

// Split a JSON object (s points at '{') into raw "key":value fragments.
std::vector<std::string> ParseJsonObjectPairs(const std::string& s,
                                              size_t obj_start) {
  std::vector<std::string> pairs;
  size_t i = obj_start + 1;
  while (i < s.size()) {
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' ||
            s[i] == ',')) {
      ++i;
    }
    if (i >= s.size() || s[i] == '}') {
      break;
    }
    if (s[i] != '"') {
      ++i;
      continue;
    }
    const size_t key_end = SkipJsonString(s, i);
    size_t colon = key_end;
    while (colon < s.size() &&
           (s[colon] == ' ' || s[colon] == '\t' || s[colon] == '\r' ||
            s[colon] == '\n')) {
      ++colon;
    }
    if (colon >= s.size() || s[colon] != ':') {
      break;
    }
    size_t val_start = colon + 1;
    while (val_start < s.size() &&
           (s[val_start] == ' ' || s[val_start] == '\t' ||
            s[val_start] == '\r' || s[val_start] == '\n')) {
      ++val_start;
    }
    const size_t val_end = SkipJsonValue(s, val_start);
    pairs.push_back(s.substr(i, val_end - i));
    i = val_end;
  }
  return pairs;
}

void SplitRawPair(const std::string& raw, std::string* key,
                  std::string* value) {
  const size_t key_end = SkipJsonString(raw, 0);
  size_t colon = key_end;
  while (colon < raw.size() &&
         (raw[colon] == ' ' || raw[colon] == '\t' || raw[colon] == '\r' ||
          raw[colon] == '\n')) {
    ++colon;
  }
  if (colon < raw.size() && raw[colon] == ':') {
    ++colon;
  }
  while (colon < raw.size() &&
         (raw[colon] == ' ' || raw[colon] == '\t' || raw[colon] == '\r' ||
          raw[colon] == '\n')) {
    ++colon;
  }
  *value = raw.substr(colon);
  *key = UnescapeJsonString(raw.substr(1, key_end - 2));
}

std::string ParseJsonStringValue(const std::string& raw) {
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
    return UnescapeJsonString(raw.substr(1, raw.size() - 2));
  }
  return {};
}

std::string NewTodoId() {
  static std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> dist(0, 15);
  static const char kHex[] = "0123456789abcdef";
  std::string s(12, '0');
  for (char& c : s) {
    c = kHex[dist(rng)];
  }
  return s;
}

std::string NowTodoTimestamp() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  char buf[32];
  sprintf_s(buf, "%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond);
  return buf;
}

TodoWindow::Item NewTodoItem(const std::wstring& text, bool done) {
  TodoWindow::Item it;
  it.done = done;
  it.text = text;
  it.id = NewTodoId();
  it.created_at = NowTodoTimestamp();
  return it;
}

// Atomic UTF-8 write: temp + write-through + replace, preserving .bak first.
void SaveStringAtomic(const std::string& data, const std::wstring& path) {
  const std::wstring tmp = path + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()),
                            &written, nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok || written != data.size()) {
    DeleteFileW(tmp.c_str());
    return;
  }
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
      !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
      (fad.nFileSizeHigh != 0 || fad.nFileSizeLow != 0)) {
    CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp.c_str());
  }
}

std::string SerializeTodos(const std::vector<TodoWindow::Item>& items) {
  std::ostringstream os;
  os << "{\n  \"version\": 1,\n  \"items\": [\n";
  for (size_t i = 0; i < items.size(); ++i) {
    const auto& it = items[i];
    os << "    {\n";
    os << "      \"id\": \"" << EscapeJsonString(it.id.empty() ? NewTodoId() : it.id)
       << "\",\n";
    os << "      \"text\": \"" << EscapeJsonString(NarrowUtf8(it.text))
       << "\",\n";
    os << "      \"done\": " << (it.done ? "true" : "false") << ",\n";
    os << "      \"created_at\": \"" << EscapeJsonString(it.created_at) << "\"";
    for (const auto& e : it.extra) {
      os << ",\n      " << e;
    }
    os << "\n    }";
    if (i + 1 < items.size()) {
      os << ",";
    }
    os << "\n";
  }
  os << "  ]\n}\n";
  return os.str();
}

TodoWindow::Item ParseTodoItem(const std::string& obj) {
  TodoWindow::Item it;
  for (const auto& raw : ParseJsonObjectPairs(obj, 0)) {
    std::string key, value;
    SplitRawPair(raw, &key, &value);
    if (key == "text") {
      it.text = WidenUtf8(ParseJsonStringValue(value));
    } else if (key == "done") {
      it.done = (value == "true");
    } else if (key == "id") {
      it.id = ParseJsonStringValue(value);
    } else if (key == "created_at") {
      it.created_at = ParseJsonStringValue(value);
    } else {
      it.extra.push_back(raw);
    }
  }
  return it;
}

void ParseTodosJson(const std::string& json, std::vector<TodoWindow::Item>* out) {
  const size_t arr = json.find('[');
  if (arr == std::string::npos) {
    return;
  }
  int depth = 0;
  size_t arr_end = std::string::npos;
  for (size_t i = arr; i < json.size(); ++i) {
    if (json[i] == '"') {
      i = SkipJsonString(json, i) - 1;
      continue;
    }
    if (json[i] == '[') {
      ++depth;
    } else if (json[i] == ']') {
      --depth;
      if (depth == 0) {
        arr_end = i;
        break;
      }
    }
  }
  if (arr_end == std::string::npos) {
    return;
  }
  size_t p = arr + 1;
  while (p < arr_end) {
    while (p < arr_end &&
           (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' ||
            json[p] == '\n' || json[p] == ',')) {
      ++p;
    }
    if (p >= arr_end) {
      break;
    }
    if (json[p] != '{') {
      const size_t e = SkipJsonValue(json, p);
      p = (e > p) ? e : p + 1;
      continue;
    }
    const size_t e = SkipJsonValue(json, p);
    TodoWindow::Item it = ParseTodoItem(json.substr(p, e - p));
    if (!it.text.empty() || it.done) {
      out->push_back(std::move(it));
    }
    p = e;
  }
}

// One-shot migration from the legacy todos.txt (line-per-item). Preserves the
// old [x]/[ ] done markers, generates id/created_at, then renames to .bak.
bool MigrateTodosTxt(const std::wstring& json_path) {
  const std::wstring txt_path = TodosTxtPath();
  const DWORD a = GetFileAttributesW(txt_path.c_str());
  if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
    return false;
  }
  std::string txt;
  if (!ReadTextFile(txt_path, &txt)) {
    return false;
  }
  std::vector<TodoWindow::Item> items;
  const std::wstring text = WidenUtf8(txt);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find_first_of(L"\r\n", pos);
    if (eol == std::wstring::npos) {
      eol = text.size();
    }
    std::wstring line = text.substr(pos, eol - pos);
    pos = eol;
    while (pos < text.size() && (text[pos] == L'\r' || text[pos] == L'\n')) {
      ++pos;
    }
    if (line.empty()) {
      continue;
    }
    bool done = false;
    if (line.size() >= 4 &&
        (line.compare(0, 4, L"[x] ") == 0 || line.compare(0, 4, L"[X] ") == 0)) {
      done = true;
      line = line.substr(4);
    } else if (line.size() >= 4 && line.compare(0, 4, L"[ ] ") == 0) {
      done = false;
      line = line.substr(4);
    } else if (line[0] == L'-') {
      line = line.substr(1);
      while (!line.empty() && line[0] == L' ') {
        line.erase(line.begin());
      }
    }
    if (!line.empty()) {
      items.push_back(NewTodoItem(line, done));
    }
  }
  SaveStringAtomic(SerializeTodos(items), json_path);
  MoveFileExW(txt_path.c_str(), (txt_path + L".bak").c_str(),
              MOVEFILE_REPLACE_EXISTING);
  return true;
}

}  // namespace

bool TodoWindow::Create(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = TodoWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // Transparent host — card chrome lives in todo.html (Python sticky feel).
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  // Default: left side of work area, ~280px — same as DesktopTodoWidget.
  const RECT work = PrimaryWorkArea();
  const int ww = 280;
  const int wh = 220;
  const int def_x = work.left + 48;
  const int def_y =
      work.top + (std::max)(80, static_cast<int>(work.bottom - work.top - wh) / 4);
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"待办",
      WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, def_x, def_y, ww, wh,
      nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  // Near-opaque; card itself uses CSS rgba translucency over wallpaper.
  SetLayeredWindowAttributes(hwnd_, 0, 250, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  LoadGeo();
  Load();
  CreateWebView();
  return true;
}

bool TodoWindow::CreateWebView() {
  if (web_available_ || controller_ || !hwnd_) {
    return web_available_;
  }
  static bool host_reg = false;
  if (!host_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    host_reg = true;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  host_ = CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_VISIBLE, 0, 0,
                          rc.right, rc.bottom, hwnd_, nullptr,
                          GetModuleHandleW(nullptr), nullptr);
  if (!host_) {
    return false;
  }
  const std::wstring user_data = JoinPath(NativeDataDir(), L"webview2-todo");
  CreateDirectoryW(user_data.c_str(), nullptr);
  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env || !host_) {
              return S_OK;
            }
            env->CreateCoreWebView2Controller(
                host_,
                Callback<
                    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT result2,
                           ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(result2) || !controller) {
                        return S_OK;
                      }
                      controller_ = controller;
                      controller->AddRef();
                      ICoreWebView2* web = nullptr;
                      controller->get_CoreWebView2(&web);
                      if (!web) {
                        return S_OK;
                      }
                      webview_ = web;
                      web_available_ = true;
                      ResizeWebView();
                      controller->put_IsVisible(TRUE);
                      web->add_WebMessageReceived(
                          Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [this](ICoreWebView2*,
                                     ICoreWebView2WebMessageReceivedEventArgs*
                                         args) -> HRESULT {
                                LPWSTR raw = nullptr;
                                if (SUCCEEDED(args->TryGetWebMessageAsString(
                                        &raw)) &&
                                    raw) {
                                  OnWebMessage(raw);
                                  CoTaskMemFree(raw);
                                }
                                return S_OK;
                              })
                              .Get(),
                          nullptr);
                      web->add_NavigationCompleted(
                          Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [this](ICoreWebView2*,
                                     ICoreWebView2NavigationCompletedEventArgs*
                                         args) -> HRESULT {
                                BOOL ok = FALSE;
                                if (args) {
                                  args->get_IsSuccess(&ok);
                                }
                                if (ok) {
                                  web_ready_ = true;
                                  PushState();
                                }
                                return S_OK;
                              })
                              .Get(),
                          nullptr);
                      NavigateHome();
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());
  return true;
}

void TodoWindow::DestroyWebView() {
  if (webview_) {
    static_cast<ICoreWebView2*>(webview_)->Release();
    webview_ = nullptr;
  }
  if (controller_) {
    static_cast<ICoreWebView2Controller*>(controller_)->Close();
    static_cast<ICoreWebView2Controller*>(controller_)->Release();
    controller_ = nullptr;
  }
  if (host_) {
    DestroyWindow(host_);
    host_ = nullptr;
  }
  web_available_ = false;
  web_ready_ = false;
}

void TodoWindow::ResizeWebView() {
  if (!host_ || !hwnd_) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  MoveWindow(host_, 0, 0, rc.right, rc.bottom, TRUE);
  if (controller_) {
    RECT bounds{0, 0, rc.right, rc.bottom};
    static_cast<ICoreWebView2Controller*>(controller_)->put_Bounds(bounds);
  }
}

void TodoWindow::NavigateHome() {
  if (!webview_) {
    return;
  }
  const std::wstring html = MistFindAssetHtml(L"overlays\\todo.html");
  if (html.empty()) {
    return;
  }
  std::wstring url = html;
  for (auto& c : url) {
    if (c == L'\\') {
      c = L'/';
    }
  }
  static_cast<ICoreWebView2*>(webview_)->Navigate((L"file:///" + url).c_str());
}

void TodoWindow::ApplyUiTheme(const std::string& ui_theme) {
  ui_theme_ = NormalizeUiTheme(ui_theme);
  PushState();
}

void TodoWindow::PushState() {
  if (!webview_ || !web_ready_) {
    return;
  }
  const auto vars = UiThemeCssVars(ui_theme_);
  auto hex = [&](const char* key, const wchar_t* fallback) -> std::wstring {
    const auto it = vars.find(key);
    if (it == vars.end()) {
      return fallback;
    }
    return WidenUtf8(it->second);
  };
  // Title label matches Python sticky: 「待办」 (not 「待办便签」).
  std::wstring json = L"{\"type\":\"state\",\"title\":\"待办\",\"theme\":{";
  json += L"\"card\":\"" + MistJsonEscape(hex("card", L"#FFFFFF")) + L"\",";
  json += L"\"accent_soft\":\"" +
          MistJsonEscape(hex("accent-soft", L"#E0E7FF")) + L"\",";
  json += L"\"border\":\"" + MistJsonEscape(hex("border", L"#C9D2DE")) + L"\",";
  json += L"\"text\":\"" + MistJsonEscape(hex("text", L"#1A2332")) + L"\",";
  json += L"\"muted\":\"" + MistJsonEscape(hex("muted", L"#64748B")) + L"\",";
  json += L"\"accent\":\"" + MistJsonEscape(hex("accent", L"#1D4ED8")) + L"\"";
  json += L"},\"items\":[";
  for (size_t i = 0; i < items_.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L"{\"done\":";
    json += items_[i].done ? L"true" : L"false";
    json += L",\"text\":\"";
    json += MistJsonEscape(items_[i].text);
    json += L"\"}";
  }
  json += L"]}";
  auto* web = static_cast<ICoreWebView2*>(webview_);
  web->PostWebMessageAsString(json.c_str());
  std::wstring script =
      L"(function(){try{var m=" + json +
      L";if(typeof window.applyState==='function'){window.applyState(m);}"
      L"}catch(e){}})();";
  web->ExecuteScript(script.c_str(), nullptr);
}

void TodoWindow::OnWebMessage(const std::wstring& json) {
  const std::wstring type = MistJsonStringField(json, L"type");
  if (type != L"action") {
    return;
  }
  const std::wstring id = MistJsonStringField(json, L"id");
  if (id == L"ready") {
    PushState();
  } else if (id == L"hide") {
    Hide();
  } else if (id == L"add") {
    // Empty text like Python add_todo("") — user types in the new row.
    items_.insert(items_.begin(), NewTodoItem(L"", false));
    Save();
    PushState();
  } else if (id == L"clear") {
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const Item& it) { return it.done; }),
                 items_.end());
    Save();
    PushState();
  } else if (id == L"del") {
    const int index = MistJsonIntField(json, L"index", -1);
    if (index >= 0 && index < static_cast<int>(items_.size())) {
      items_.erase(items_.begin() + index);
      Save();
      PushState();
    }
  } else if (id == L"toggle") {
    const int index = MistJsonIntField(json, L"index", -1);
    if (index >= 0 && index < static_cast<int>(items_.size())) {
      items_[static_cast<size_t>(index)].done =
          json.find(L"\"done\":true") != std::wstring::npos;
      std::stable_partition(items_.begin(), items_.end(),
                            [](const Item& it) { return !it.done; });
      Save();
      PushState();
    }
  } else if (id == L"edit") {
    const int index = MistJsonIntField(json, L"index", -1);
    if (index >= 0 && index < static_cast<int>(items_.size())) {
      items_[static_cast<size_t>(index)].text =
          MistJsonStringField(json, L"text");
      Save();
    }
  } else if (id == L"drag_start") {
    dragging_ = true;
    drag_origin_ = {MistJsonIntField(json, L"x", 0),
                    MistJsonIntField(json, L"y", 0)};
    RECT wr{};
    GetWindowRect(hwnd_, &wr);
    win_origin_ = {wr.left, wr.top};
  } else if (id == L"drag_move" && dragging_) {
    const int x = MistJsonIntField(json, L"x", 0);
    const int y = MistJsonIntField(json, L"y", 0);
    SetWindowPos(hwnd_, nullptr, win_origin_.x + (x - drag_origin_.x),
                 win_origin_.y + (y - drag_origin_.y), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  } else if (id == L"drag_end") {
    dragging_ = false;
    PersistGeo();
    AttachOverlayToDesktop(hwnd_, true);
    PlaceOverlayInDesktopBand(hwnd_, true);
  }
}

void TodoWindow::Destroy() {
  PersistGeo();
  Save();
  DestroyWebView();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void TodoWindow::Show() {
  if (!hwnd_) {
    return;
  }
  if (!web_available_) {
    CreateWebView();
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
  PushState();
}

void TodoWindow::Hide() {
  PersistGeo();
  Save();
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void TodoWindow::LoadGeo() {
  std::ifstream in(TodosGeoPath());
  if (!in || !hwnd_) {
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  const int x = ReadGeoInt(json, "x", -1);
  const int y = ReadGeoInt(json, "y", -1);
  // Width clamped like Python (_MIN_W=220 … _MAX_W=420).
  const int w = (std::max)(220, (std::min)(420, ReadGeoInt(json, "w", 280)));
  const int h = (std::max)(160, ReadGeoInt(json, "h", 220));
  if (x < 0 || y < 0) {
    return;
  }
  RECT rc{x, y, x + w, y + h};
  ClampRectToWorkArea(&rc);
  SetWindowPos(hwnd_, nullptr, rc.left, rc.top, rc.right - rc.left,
               rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

void TodoWindow::PersistGeo() {
  if (!hwnd_) {
    return;
  }
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  std::ofstream out(TodosGeoPath(), std::ios::binary);
  if (!out) {
    return;
  }
  out << "{\n  \"x\":" << rc.left << ",\n  \"y\":" << rc.top
      << ",\n  \"w\":" << (rc.right - rc.left) << ",\n  \"h\":"
      << (rc.bottom - rc.top) << "\n}\n";
}

void TodoWindow::Load() {
  items_.clear();
  std::string json;
  bool have_file = ReadTextFile(TodosPath(), &json);
  if (!have_file && MigrateTodosTxt(TodosPath())) {
    have_file = ReadTextFile(TodosPath(), &json);
  }
  if (!have_file) {
    items_.push_back(NewTodoItem(Tr("todo_sample"), false));
    return;
  }
  ParseTodosJson(json, &items_);
}

void TodoWindow::Save() {
  SaveStringAtomic(SerializeTodos(items_), TodosPath());
}

LRESULT CALLBACK TodoWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  TodoWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<TodoWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<TodoWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT TodoWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_SIZE:
      ResizeWebView();
      return 0;
    case WM_EXITSIZEMOVE:
      PersistGeo();
      return 0;
    case WM_CLOSE:
      Hide();
      return 0;
    case WM_DESTROY:
      DestroyWebView();
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}
