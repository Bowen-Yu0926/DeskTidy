#include "settings_window.h"

#include "brand_mark.h"
#include "paths_util.h"
#include "rule_store.h"

#include <objbase.h>
#ifndef interface
#define interface struct
#endif
#include <WebView2.h>
#include <wrl.h>

#include <fstream>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeSettings";
constexpr wchar_t kHostClass[] = L"DeskTidyNativeSettingsWeb";

std::wstring JsonEscape(const std::wstring& s) {
  std::wstring o;
  o.reserve(s.size() + 8);
  for (wchar_t c : s) {
    switch (c) {
      case L'\\':
        o += L"\\\\";
        break;
      case L'"':
        o += L"\\\"";
        break;
      case L'\n':
        o += L"\\n";
        break;
      case L'\r':
        o += L"\\r";
        break;
      case L'\t':
        o += L"\\t";
        break;
      default:
        if (c < 0x20) {
          wchar_t buf[8];
          swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
          o += buf;
        } else {
          o.push_back(c);
        }
        break;
    }
  }
  return o;
}

// Escape a full JSON document so it can be embedded inside a JS "..." literal
// and recovered with JSON.parse — avoids ExecuteScript choking on raw Unicode
// object literals in some WebView2 builds.
std::wstring EscapeJsStringLiteral(const std::wstring& s) {
  std::wstring o;
  o.reserve(s.size() * 2 + 8);
  for (wchar_t c : s) {
    switch (c) {
      case L'\\':
        o += L"\\\\";
        break;
      case L'"':
        o += L"\\\"";
        break;
      case L'\n':
        o += L"\\n";
        break;
      case L'\r':
        o += L"\\r";
        break;
      case L'\t':
        o += L"\\t";
        break;
      case 0x2028:
        o += L"\\u2028";
        break;
      case 0x2029:
        o += L"\\u2029";
        break;
      default:
        if (c < 0x20 || c > 0x7E) {
          wchar_t buf[8];
          swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c & 0xFFFF));
          o += buf;
        } else {
          o.push_back(c);
        }
        break;
    }
  }
  return o;
}

std::wstring JsonStringField(const std::wstring& json, const wchar_t* key) {
  const std::wstring needle = std::wstring(L"\"") + key + L"\"";
  size_t p = json.find(needle);
  if (p == std::wstring::npos) {
    return {};
  }
  p = json.find(L':', p + needle.size());
  if (p == std::wstring::npos) {
    return {};
  }
  p = json.find(L'"', p + 1);
  if (p == std::wstring::npos) {
    return {};
  }
  ++p;
  std::wstring v;
  while (p < json.size() && json[p] != L'"') {
    if (json[p] == L'\\' && p + 1 < json.size()) {
      ++p;
      const wchar_t e = json[p++];
      switch (e) {
        case L'n':
          v.push_back(L'\n');
          break;
        case L'r':
          v.push_back(L'\r');
          break;
        case L't':
          v.push_back(L'\t');
          break;
        case L'b':
          v.push_back(L'\b');
          break;
        case L'f':
          v.push_back(L'\f');
          break;
        case L'u':
          if (p + 4 <= json.size()) {
            unsigned code = 0;
            bool ok = true;
            for (int k = 0; k < 4; ++k) {
              const wchar_t hc = json[p + k];
              code <<= 4;
              if (hc >= L'0' && hc <= L'9') {
                code |= static_cast<unsigned>(hc - L'0');
              } else if (hc >= L'a' && hc <= L'f') {
                code |= static_cast<unsigned>(hc - L'a' + 10);
              } else if (hc >= L'A' && hc <= L'F') {
                code |= static_cast<unsigned>(hc - L'A' + 10);
              } else {
                ok = false;
                break;
              }
            }
            if (ok) {
              v.push_back(static_cast<wchar_t>(code));
              p += 4;
            } else {
              v.push_back(L'u');
            }
          } else {
            v.push_back(L'u');
          }
          break;
        default:
          v.push_back(e);
          break;
      }
      continue;
    }
    v.push_back(json[p++]);
  }
  return v;
}

int JsonIntField(const std::wstring& json, const wchar_t* key, int fallback) {
  const std::wstring needle = std::wstring(L"\"") + key + L"\"";
  size_t p = json.find(needle);
  if (p == std::wstring::npos) {
    return fallback;
  }
  p = json.find(L':', p + needle.size());
  if (p == std::wstring::npos) {
    return fallback;
  }
  ++p;
  while (p < json.size() && (json[p] == L' ' || json[p] == L'\t')) {
    ++p;
  }
  return _wtoi(json.c_str() + p);
}

bool JsonBoolField(const std::wstring& json, const wchar_t* key, bool fallback) {
  const std::wstring needle = std::wstring(L"\"") + key + L"\"";
  size_t p = json.find(needle);
  if (p == std::wstring::npos) {
    return fallback;
  }
  p = json.find(L':', p + needle.size());
  if (p == std::wstring::npos) {
    return fallback;
  }
  ++p;
  while (p < json.size() && (json[p] == L' ' || json[p] == L'\t')) {
    ++p;
  }
  if (json.compare(p, 4, L"true") == 0) {
    return true;
  }
  if (json.compare(p, 5, L"false") == 0) {
    return false;
  }
  return fallback;
}

bool PathExists(const std::wstring& path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES;
}

std::wstring FindSettingsHtml() {
  wchar_t mod[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, mod, MAX_PATH);
  std::wstring dir = mod;
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    dir.resize(slash);
  }
  const std::wstring candidates[] = {
      dir + L"\\assets\\settings\\settings.html",
      dir + L"\\..\\assets\\settings\\settings.html",
      dir + L"\\..\\..\\assets\\settings\\settings.html",
      JoinPath(NativeDataDir(), L"settings\\settings.html"),
  };
  for (const auto& c : candidates) {
    if (PathExists(c)) {
      return c;
    }
  }
  return {};
}

std::wstring FileUrlFromPath(const std::wstring& path) {
  std::wstring abs = path;
  for (auto& c : abs) {
    if (c == L'\\') {
      c = L'/';
    }
  }
  return L"file:///" + abs;
}

}  // namespace

bool SettingsWindow::Create(HINSTANCE instance) {
  static bool s_class_registered = false;
  if (!s_class_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0xEE, 0xF1, 0xF6));
    wc.lpszClassName = kClassName;
    // Brand icons on the class so the taskbar never falls back to a live preview.
    wc.hIcon = CreateBrandMarkIcon(32);
    wc.hIconSm = CreateBrandMarkIcon(16);
    RegisterClassExW(&wc);
    s_class_registered = true;
  }

  // 现代桌面软件后台标准尺寸（1120x740），并自适应当前工作区居中展示
  int default_w = 1120;
  int default_h = 740;
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int work_w = work.right - work.left;
  const int work_h = work.bottom - work.top;
  if (default_w > static_cast<int>(work_w * 0.9f)) {
    default_w = static_cast<int>(work_w * 0.9f);
  }
  if (default_h > static_cast<int>(work_h * 0.9f)) {
    default_h = static_cast<int>(work_h * 0.9f);
  }
  const int pos_x = work.left + (work_w - default_w) / 2;
  const int pos_y = work.top + (work_h - default_h) / 2;

  hwnd_ = CreateWindowExW(
      WS_EX_APPWINDOW, kClassName,
      IsDevRuntime() ? L"DeskTidy 设置 [dev]" : L"DeskTidy 设置",
      WS_OVERLAPPEDWINDOW, pos_x, pos_y, default_w, default_h, nullptr,
      nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ApplyBrandWindowIcons(hwnd_);
  // Defer WebView2 until Show() so the HWND has a real client size.
  return true;
}

bool SettingsWindow::CreateWebView() {
  if (web_available_ || web_creating_ || controller_ || !hwnd_) {
    return web_available_ || web_creating_;
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsWindow::HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0xF4, 0xF6, 0xF9));
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    registered = true;
  }
  if (!host_) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    if (rc.right < 40 || rc.bottom < 40) {
      rc.right = 900;
      rc.bottom = 600;
    }
    host_ = CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_VISIBLE, 0, 0,
                            rc.right, rc.bottom, hwnd_, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!host_) {
      return false;
    }
    SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  }

  // Dedicated profile — sharing the default folder with Notes WebView2 fails.
  const std::wstring user_data =
      JoinPath(NativeDataDir(), L"webview2-settings");
  CreateDirectoryW(user_data.c_str(), nullptr);

  web_creating_ = true;
  const HWND expected_host = host_;
  const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this, expected_host](HRESULT result,
                                ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env || !host_ || host_ != expected_host ||
                !IsWindow(expected_host)) {
              web_creating_ = false;
              return S_OK;
            }
            env->CreateCoreWebView2Controller(
                host_,
                Callback<
                    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this, expected_host](
                        HRESULT result2,
                        ICoreWebView2Controller* controller) -> HRESULT {
                      web_creating_ = false;
                      if (FAILED(result2) || !controller) {
                        return S_OK;
                      }
                      if (!host_ || host_ != expected_host ||
                          !IsWindow(expected_host) || controller_) {
                        controller->Close();
                        return S_OK;
                      }
                      controller_ = controller;
                      controller->AddRef();
                      ICoreWebView2* web = nullptr;
                      controller->get_CoreWebView2(&web);
                      if (!web) {
                        controller->Close();
                        controller->Release();
                        controller_ = nullptr;
                        return S_OK;
                      }
                      webview_ = web;
                      web_available_ = true;
                      controller->put_IsVisible(TRUE);
                      ResizeWebView();
                      EventRegistrationToken token{};
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
                          &token);
                      EventRegistrationToken nav_token{};
                      web->add_NavigationCompleted(
                          Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [this](ICoreWebView2*,
                                     ICoreWebView2NavigationCompletedEventArgs*
                                         args) -> HRESULT {
                                BOOL ok = FALSE;
                                if (args) {
                                  args->get_IsSuccess(&ok);
                                }
                                if (!ok) {
                                  NavigateFallback();
                                  return S_OK;
                                }
                                // Do NOT PushState here. NavigationCompleted
                                // often fires before settings.html registers
                                // chrome.webview message listeners — those
                                // posts are dropped and the UI stays empty.
                                // settings.html posts {type:'ready'} when safe.
                                // Fallback if script never signals ready.
                                HWND host = host_;
                                SetTimer(host, 7701, 2000, nullptr);
                                return S_OK;
                              })
                              .Get(),
                          &nav_token);
                      NavigateHome();
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    web_creating_ = false;
    return false;
  }
  return true;
}

void SettingsWindow::NavigateFallback() {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web) {
    return;
  }
  web->NavigateToString(
      L"<!DOCTYPE html><html><body style='font-family:Microsoft YaHei UI;"
      L"padding:24px;background:#F4F6F9;color:#1A2332'>"
      L"<h2>DeskTidy \u8bbe\u7f6e</h2>"
      L"<p>\u8bbe\u7f6e\u9875\u52a0\u8f7d\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5 "
      L"assets/settings/settings.html \u662f\u5426\u5b58\u5728\u3002</p>"
      L"</body></html>");
}

void SettingsWindow::NavigateHome() {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web) {
    return;
  }
  // Drop ready flag so PushState waits until the new document listens.
  web_ready_ = false;
  ready_handling_ = false;
  if (host_) {
    KillTimer(host_, 7701);
    KillTimer(host_, 7702);
  }
  const std::wstring html = FindSettingsHtml();
  if (html.empty()) {
    NavigateFallback();
    return;
  }
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  std::wstring url = FileUrlFromPath(html);
  if (GetFileAttributesExW(html.c_str(), GetFileExInfoStandard, &fad)) {
    wchar_t q[48];
    swprintf_s(q, L"?v=%lu", fad.ftLastWriteTime.dwLowDateTime);
    url += q;
  }
  web->Navigate(url.c_str());
}

void SettingsWindow::DestroyWebView() {
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
  web_creating_ = false;
  web_ready_ = false;
}

void SettingsWindow::ResizeWebView() {
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

std::wstring SettingsWindow::BuildStateJson() const {
  std::wstring json = L"{\"type\":\"state\",\"selected\":" +
                      std::to_wstring(selected_) + L",\"dock\":" +
                      (dock_on_ ? L"true" : L"false") + L",\"pet\":" +
                      (pet_on_ ? L"true" : L"false") + L",\"autostart\":" +
                      (autostart_on_ ? L"true" : L"false") +
                      L",\"org_startup\":" +
                      (org_startup_on_ ? L"true" : L"false") +
                      L",\"org_watch\":" +
                      (org_watch_on_ ? L"true" : L"false") + L",\"fences\":[";
  for (size_t i = 0; i < fence_names_.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += JsonEscape(fence_names_[i]);
    json += L'"';
  }
  json += L"],\"pages\":[";
  for (size_t i = 0; i < page_names_.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += JsonEscape(page_names_[i]);
    json += L'"';
  }
  json += L"],\"page_locked\":[";
  for (size_t i = 0; i < page_names_.size(); ++i) {
    if (i) {
      json += L',';
    }
    const bool locked =
        i < 2 ||
        (i < ext_.page_locked.size() && ext_.page_locked[i]);
    json += locked ? L"true" : L"false";
  }
  json += L"],\"current_page\":";
  json += std::to_wstring(current_page_);
  json += L",\"preview\":[";
  for (size_t i = 0; i < org_preview_.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += JsonEscape(org_preview_[i]);
    json += L'"';
  }
  json += L"],\"org_pending\":";
  json += std::to_wstring(org_pending_);
  json += L",\"org_organized\":";
  json += std::to_wstring(org_organized_);
  json += L",\"org_total\":";
  json += std::to_wstring(org_total_);
  json += L",\"org_files\":[";
  for (size_t i = 0; i < org_files_.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L"{\"name\":\"";
    json += JsonEscape(org_files_[i].name);
    json += L"\",\"type\":\"";
    json += JsonEscape(org_files_[i].type);
    json += L"\",\"size\":\"";
    json += JsonEscape(org_files_[i].size);
    json += L"\"}";
  }
  auto flag = [](bool v) { return v ? L"true" : L"false"; };
  json += L"],\"minutes_on_bar\":";
  json += flag(ext_.minutes_on_bar);
  json += L",\"notepad_enabled\":";
  json += flag(ext_.notepad_enabled);
  json += L",\"elevate_hotkey_helper\":";
  json += flag(ext_.elevate_hotkey_helper);
  json += L",\"record_on_bar\":";
  json += flag(ext_.record_on_bar);
  json += L",\"calculator_on_bar\":";
  json += flag(ext_.calculator_on_bar);
  json += L",\"todos_on_bar\":";
  json += flag(ext_.todos_on_bar);
  json += L",\"minutes_enabled\":";
  json += flag(ext_.minutes_enabled);
  json += L",\"calculator_enabled\":";
  json += flag(ext_.calculator_enabled);
  json += L",\"todos_enabled\":";
  json += flag(ext_.todos_enabled);
  json += L",\"record_enabled\":";
  json += flag(ext_.record_enabled);
  json += L",\"screenshot_max_pins\":";
  json += std::to_wstring(ext_.screenshot_max_pins);
  json += L",\"record_quality\":";
  json += std::to_wstring(ext_.record_quality);
  json += L",\"minutes_folder\":\"";
  json += JsonEscape(ext_.minutes_folder);
  json += L"\",\"notepad_folder\":\"";
  json += JsonEscape(ext_.notepad_folder);
  json += L"\",\"wallpaper_folder\":\"";
  json += JsonEscape(ext_.wallpaper_folder);
  json += L"\",\"record_dir\":\"";
  json += JsonEscape(ext_.record_dir);
  json += L"\",\"record_mic\":\"";
  json += JsonEscape(ext_.record_mic);
  json += L"\",\"record_mics\":[";
  for (size_t i = 0; i < ext_.record_mics.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L"\"" + JsonEscape(ext_.record_mics[i]) + L"\"";
  }
  json += L"]";
  json += L",\"minutes_folder_default\":\"";
  json += JsonEscape(ext_.minutes_folder_default);
  json += L"\",\"notepad_folder_default\":\"";
  json += JsonEscape(ext_.notepad_folder_default);
  json += L"\",\"record_dir_default\":\"";
  json += JsonEscape(ext_.record_dir_default);
  json += L"\",\"page_folders\":[";
  for (size_t i = 0; i < ext_.page_folders.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L"{\"name\":\"";
    json += JsonEscape(ext_.page_folders[i].name);
    json += L"\",\"path\":\"";
    json += JsonEscape(ext_.page_folders[i].path);
    json += L"\"}";
  }
  json += L"],\"whitelist\":[";
  for (size_t i = 0; i < ext_.whitelist.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += JsonEscape(ext_.whitelist[i]);
    json += L'"';
  }
  json += L"],\"search_desktop\":";
  json += flag(ext_.search_desktop);
  json += L",\"search_documents\":";
  json += flag(ext_.search_documents);
  json += L",\"search_downloads\":";
  json += flag(ext_.search_downloads);
  json += L",\"search_all_drives\":";
  json += flag(ext_.search_all_drives);
  json += L",\"search_max_results\":";
  json += std::to_wstring(ext_.search_max_results);
  json += L",\"search_max_depth\":";
  json += std::to_wstring(ext_.search_max_depth);
  json += L",\"search_include_open_folders\":";
  json += flag(ext_.search_include_open_folders);
  json += L",\"search_include_hidden\":";
  json += flag(ext_.search_include_hidden);
  json += L",\"search_threads\":";
  json += std::to_wstring(ext_.search_threads);
  json += L",\"pet_auto_wander\":";
  json += flag(ext_.pet_auto_wander);
  json += L",\"pet_show_bubbles\":";
  json += flag(ext_.pet_show_bubbles);
  json += L",\"pet_size_scale\":";
  {
    // Invariant decimal point — locale swprintf("%.2f") can emit "1,00" and
    // break JSON.parse / ExecuteScript for the entire state payload.
    float scale = ext_.pet_size_scale;
    if (!(scale > 0.f) || scale > 8.f) {
      scale = 1.f;
    }
    const int centi = static_cast<int>(scale * 100.f + 0.5f);
    json += std::to_wstring(centi / 100);
    json += L'.';
    wchar_t frac[8];
    swprintf_s(frac, L"%02d", centi % 100);
    json += frac;
  }
  json += L",\"pet_character\":\"";
  json += JsonEscape(ext_.pet_character);
  json += L"\",\"pet_characters\":[";
  for (size_t i = 0; i < ext_.pet_characters.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += JsonEscape(ext_.pet_characters[i]);
    json += L'"';
  }
  json += L"],\"pet_actions\":{";
  {
    bool first_char = true;
    for (const auto& kv : ext_.pet_actions) {
      if (!first_char) {
        json += L',';
      }
      first_char = false;
      json += L'"';
      json += JsonEscape(kv.first);
      json += L"\":[";
      for (size_t i = 0; i < kv.second.size(); ++i) {
        if (i) {
          json += L',';
        }
        json += L'"';
        json += JsonEscape(kv.second[i]);
        json += L'"';
      }
      json += L']';
    }
  }
  json += L"},\"dock_edge\":";
  json += std::to_wstring(ext_.dock_edge);
  json += L",\"dock_opacity\":";
  json += std::to_wstring(ext_.dock_opacity);
  json += L",\"dock_items\":[";
  for (size_t i = 0; i < ext_.dock_items.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += JsonEscape(ext_.dock_items[i]);
    json += L'"';
  }
  json += L"],\"exclude_patterns\":[";
  for (size_t i = 0; i < ext_.exclude_patterns.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += JsonEscape(ext_.exclude_patterns[i]);
    json += L'"';
  }
  json += L']';
  // U1 organize categories — read fresh from rule_store so the UI always shows
  // the persisted values (not whatever App may have staged in ext_).
  json += L",\"category_fence_map\":\"";
  {
    CategoryFenceMap fence_map;
    FilenameRulesMap filename_rules;
    LoadCategoriesBoth(&fence_map, &filename_rules);  // single file read
    json += JsonEscape(SerializeCategoryFenceMap(fence_map));
    json += L"\",\"filename_rules\":\"";
    json += JsonEscape(SerializeFilenameRules(filename_rules));
  }
  json += L"\"";
  json += L",\"check_update_on_startup\":";
  json += flag(ext_.check_update_on_startup);
  json += L",\"desktop_right_click_menu\":";
  json += flag(ext_.desktop_right_click_menu);
  json += L",\"close_to_tray\":";
  json += flag(ext_.close_to_tray);
  json += L",\"show_fences\":";
  json += flag(ext_.show_fences);
  json += L",\"double_click_hide\":";
  json += flag(ext_.double_click_hide);
  json += L",\"enable_public_desktop\":";
  json += flag(ext_.enable_public_desktop);
  json += L",\"show_page_indicator\":";
  json += flag(ext_.show_page_indicator);
  json += L",\"page_chrome_style\":\"";
  json += JsonEscape(WidenUtf8(
      ext_.page_chrome_style == "tabs" ? "tabs" : "float"));
  json += L"\",\"page_bar_style\":\"";
  json += JsonEscape(WidenUtf8(ext_.page_bar_style));
  json += L"\",\"lang\":\"";
  json += JsonEscape(WidenUtf8(ext_.lang.empty() ? "zh" : ext_.lang));
  json += L"\",\"app_version\":\"";
  json += JsonEscape(ext_.app_version);
  json += L"\",\"theme_id\":\"";
  json += JsonEscape(WidenUtf8(ext_.theme_id));
  json += L"\",\"ui_theme\":\"";
  json += JsonEscape(WidenUtf8(ext_.ui_theme));
  json += L"\",\"themes\":[";
  for (size_t i = 0; i < ext_.themes.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L"{\"id\":\"";
    json += JsonEscape(WidenUtf8(ext_.themes[i].id));
    json += L"\",\"name\":\"";
    json += JsonEscape(ext_.themes[i].name);
    json += L"\"}";
  }
  json += L"],\"ui_themes\":[";
  for (size_t i = 0; i < ext_.ui_themes.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L"{\"id\":\"";
    json += JsonEscape(WidenUtf8(ext_.ui_themes[i].id));
    json += L"\",\"name\":\"";
    json += JsonEscape(ext_.ui_themes[i].name);
    json += L"\"}";
  }
  json += L"],\"fence_cards\":[";
  for (size_t i = 0; i < ext_.fence_cards.size(); ++i) {
    if (i) {
      json += L',';
    }
    const auto& c = ext_.fence_cards[i];
    json += L"{\"index\":";
    json += std::to_wstring(c.index);
    json += L",\"title\":\"";
    json += JsonEscape(c.title);
    json += L"\",\"page\":";
    json += std::to_wstring(c.page);
    json += L",\"pages\":[";
    if (c.pages.empty()) {
      json += std::to_wstring(c.page);
    } else {
      for (size_t pi = 0; pi < c.pages.size(); ++pi) {
        if (pi) {
          json += L',';
        }
        json += std::to_wstring(c.pages[pi]);
      }
    }
    json += L"],\"style_id\":\"";
    json += JsonEscape(WidenUtf8(c.style_id));
    json += L"\",\"opacity_pct\":";
    json += std::to_wstring(c.opacity_pct);
    json += L",\"system\":";
    json += c.system_fence ? L"true" : L"false";
    json += L",\"position_locked\":";
    json += c.position_locked ? L"true" : L"false";
    json += L",\"content_locked\":";
    json += c.content_locked ? L"true" : L"false";
    json += L",\"label_lines\":";
    json += std::to_wstring(c.label_lines);
    json += L",\"show_labels\":";
    json += c.show_labels ? L"true" : L"false";
    json += L",\"cell_scale\":";
    json += std::to_wstring(c.cell_scale);
    json += L",\"sort_by\":\"";
    json += JsonEscape(WidenUtf8(c.sort_by.empty() ? "name" : c.sort_by));
    json += L"\",\"sort_desc\":";
    json += c.sort_desc ? L"true" : L"false";
    json += L",\"radius\":";
    json += std::to_wstring(c.radius);
    json += L",\"show_title\":";
    json += c.show_title ? L"true" : L"false";
    json += L",\"fade_on_idle\":";
    json += c.fade_on_idle ? L"true" : L"false";
    json += L",\"visible\":";
    json += c.visible ? L"true" : L"false";
    json += L",\"has_custom_colors\":";
    json += c.has_custom_colors ? L"true" : L"false";
    json += L",\"custom_bg\":\"";
    json += JsonEscape(c.custom_bg);
    json += L"\",\"custom_accent\":\"";
    json += JsonEscape(c.custom_accent);
    json += L"\",\"organize_kinds\":[";
    for (size_t k = 0; k < c.organize_kinds.size(); ++k) {
      if (k) {
        json += L',';
      }
      json += L'"';
      json += JsonEscape(WidenUtf8(c.organize_kinds[k]));
      json += L'"';
    }
    json += L"]}";
  }
  json += L"],\"snapshots\":[";
  for (size_t i = 0; i < ext_.snapshots.size(); ++i) {
    if (i) {
      json += L',';
    }
    const auto& s = ext_.snapshots[i];
    json += L"{\"name\":\"";
    json += JsonEscape(s.name);
    json += L"\",\"meta\":\"";
    json += JsonEscape(s.meta);
    json += L"\",\"focus_page\":";
    json += std::to_wstring(s.focus_page);
    json += L",\"work_w\":";
    json += std::to_wstring(s.work_w);
    json += L",\"work_h\":";
    json += std::to_wstring(s.work_h);
    json += L",\"page_names\":[";
    for (size_t p = 0; p < s.page_names.size(); ++p) {
      if (p) {
        json += L',';
      }
      json += L'"';
      json += JsonEscape(s.page_names[p]);
      json += L'"';
    }
    json += L"],\"preview\":[";
    for (size_t j = 0; j < s.preview.size(); ++j) {
      if (j) {
        json += L',';
      }
      const auto& b = s.preview[j];
      json += L"{\"title\":\"";
      json += JsonEscape(b.title);
      json += L"\",\"x\":";
      json += std::to_wstring(b.x);
      json += L",\"y\":";
      json += std::to_wstring(b.y);
      json += L",\"w\":";
      json += std::to_wstring(b.w);
      json += L",\"h\":";
      json += std::to_wstring(b.h);
      json += L",\"page\":";
      json += std::to_wstring(b.page);
      json += L",\"style_id\":\"";
      json += JsonEscape(WidenUtf8(b.style_id));
      json += L"\",\"collapsed\":";
      json += b.collapsed ? L"true" : L"false";
      json += L",\"icon_count\":";
      json += std::to_wstring(b.icon_count);
      json += L",\"pin_thumbs\":[";
      for (size_t t = 0; t < b.pin_thumbs.size(); ++t) {
        if (t) {
          json += L',';
        }
        json += L'"';
        json += JsonEscape(WidenUtf8(b.pin_thumbs[t]));
        json += L'"';
      }
      json += L"]}";
    }
    json += L"],\"icons\":[";
    for (size_t k = 0; k < s.icons.size(); ++k) {
      if (k) {
        json += L',';
      }
      json += L"{\"x\":";
      json += std::to_wstring(s.icons[k].x);
      json += L",\"y\":";
      json += std::to_wstring(s.icons[k].y);
      if (!s.icons[k].thumb.empty()) {
        json += L",\"thumb\":\"";
        json += JsonEscape(WidenUtf8(s.icons[k].thumb));
        json += L'"';
      }
      json += L'}';
    }
    json += L"]}";
  }
  json += L"],\"hotkeys\":[";
  for (size_t i = 0; i < hotkey_rows_.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L"{\"id\":\"";
    json += JsonEscape(hotkey_rows_[i].id);
    json += L"\",\"label\":\"";
    json += JsonEscape(hotkey_rows_[i].label);
    json += L"\",\"chord\":\"";
    json += JsonEscape(hotkey_rows_[i].chord);
    json += L"\"}";
  }
  json += L"],\"focus_sub\":\"";
  json += JsonEscape(focus_sub_);
  json += L"\"}";
  return json;
}

void SettingsWindow::PushState() {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web || !web_ready_) {
    push_pending_ = true;
    return;
  }
  // Coalesce: while OnWebReady fills pages/fence_cards/snapshots, skip
  // intermediate posts so the page does not paint empty then miss the real one.
  if (ready_handling_) {
    push_pending_ = true;
    return;
  }
  push_pending_ = false;
  std::wstring json = BuildStateJson();
  focus_sub_.clear();  // one-shot navigation hint

  // Single channel: ExecuteScript(applyState) is the reliable path.
  // PostWebMessageAsString is dropped when nested inside WebMessageReceived,
  // which left the fence/page list empty on some machines.
  const std::wstring script =
      L"(function(){try{var s=\"" + EscapeJsStringLiteral(json) +
      L"\";var m=JSON.parse(s);"
      L"if(typeof window.applyState==='function'){window.applyState(m);}"
      L"else{window.__desktidyPendingState=m;}}catch(e){}})();";
  web->ExecuteScript(script.c_str(), nullptr);
}

void SettingsWindow::SetExtensionState(const ExtState& ext) {
  ext_ = ext;
  PushState();
}

void SettingsWindow::ApplyUiThemeVars(
    const std::unordered_map<std::string, std::string>& vars) {
  if (!webview_ || !web_ready_) {
    return;
  }
  std::wstring json = L"{\"type\":\"ui_theme\",\"vars\":{";
  bool first = true;
  for (const auto& kv : vars) {
    if (!first) {
      json += L',';
    }
    first = false;
    json += L'"';
    json += JsonEscape(WidenUtf8(kv.first));
    json += L"\":\"";
    json += JsonEscape(WidenUtf8(kv.second));
    json += L'"';
  }
  json += L"}}";
  auto* web = static_cast<ICoreWebView2*>(webview_);
  std::wstring script =
      L"(function(){try{var m=" + json +
      L";if(typeof window.applyUiTheme==='function'){window.applyUiTheme(m);}"
      L"}catch(e){}})();";
  web->ExecuteScript(script.c_str(), nullptr);
}

void SettingsWindow::SetHotkeyRows(const std::vector<HotkeyRow>& rows) {
  hotkey_rows_ = rows;
  PushState();
}

void SettingsWindow::FocusSubpage(const std::wstring& sub) {
  focus_sub_ = sub;
  PushState();
}

void SettingsWindow::OnWebReady() {
  if (ready_handling_) {
    return;
  }
  ready_handling_ = true;
  if (host_) {
    KillTimer(host_, 7701);
    KillTimer(host_, 7702);
  }
  web_ready_ = true;
  // Prefer App-driven full refresh (snapshots / fence_cards) when wired.
  // PushState is suppressed while ready_handling_ so we emit one full payload.
  push_pending_ = true;
  if (on_ready_) {
    on_ready_();
  }
  ready_handling_ = false;
  if (push_pending_) {
    PushState();
  }
}

void SettingsWindow::OnWebMessage(const std::wstring& json) {
  const std::wstring type = JsonStringField(json, L"type");
  if (type == L"ready") {
    // Defer out of WebMessageReceived — nested PostWebMessage is flaky.
    if (host_) {
      SetTimer(host_, 7702, 1, nullptr);
    } else {
      OnWebReady();
    }
    return;
  }
  if (type == L"toggle") {
    const std::wstring id = JsonStringField(json, L"id");
    const bool value = JsonBoolField(json, L"value", false);
    // Keep ext_ in sync before App callbacks trigger SyncPageChrome/PushState.
    ApplyExtToggleLocal(id, value);
    if (id == L"dock" && on_dock_) {
      dock_on_ = value;
      on_dock_(value);
    } else if (id == L"pet" && on_pet_) {
      pet_on_ = value;
      on_pet_(value);
    } else if (id == L"autostart" && on_autostart_) {
      autostart_on_ = value;
      on_autostart_(value);
    } else if (id == L"org_startup" && on_org_startup_) {
      org_startup_on_ = value;
      on_org_startup_(value);
    } else if (id == L"org_watch" && on_org_watch_) {
      org_watch_on_ = value;
      on_org_watch_(value);
    } else if (on_ext_toggle_) {
      on_ext_toggle_(id, value);
    }
    return;
  }
  if (type == L"field") {
    const std::wstring id = JsonStringField(json, L"id");
    // Detect the value's form from its first character, not from
    // JsonStringField() returning empty: an emptied text field is a legitimate
    // `"value":""`, and treating that as "unquoted" stored the two literal
    // quote characters as the folder path.
    std::wstring value;
    {
      const std::wstring needle = L"\"value\"";
      size_t p = json.find(needle);
      if (p != std::wstring::npos) {
        p = json.find(L':', p + needle.size());
        if (p != std::wstring::npos) {
          ++p;
          while (p < json.size() && (json[p] == L' ' || json[p] == L'\t')) {
            ++p;
          }
          if (p < json.size() && json[p] == L'"') {
            value = JsonStringField(json, L"value");
          } else {
            while (p < json.size() && json[p] != L',' && json[p] != L'}') {
              value.push_back(json[p++]);
            }
            while (!value.empty() &&
                   (value.back() == L' ' || value.back() == L'\t' ||
                    value.back() == L'\r' || value.back() == L'\n')) {
              value.pop_back();
            }
          }
        }
      }
    }
    // U1 organize categories: persist locally (rule_store) instead of the
    // generic app_prefs field path.
    if (id == L"category_fence_map") {
      ext_.category_fence_map_text = value;
      SaveCategoryFenceMap(ParseCategoryFenceMapText(value));
      return;
    }
    if (id == L"filename_rules") {
      ext_.filename_rules_text = value;
      SaveFilenameRules(ParseFilenameRulesText(value));
      return;
    }
    if (on_ext_field_) {
      on_ext_field_(id, value);
    }
    return;
  }
  if (type != L"action") {
    return;
  }
  // Leave WebMessageReceived before any host work. Nested MistWebDialog /
  // MessageBox / PromptText from this stack caused blank freezes (rename page).
  if (hwnd_) {
    pending_action_jsons_.push_back(json);
    PostMessageW(hwnd_, WM_APP + 70, 0, 0);
  } else {
    DispatchAction(json);
  }
}

void SettingsWindow::DispatchAction(const std::wstring& json) {
  const std::wstring id = JsonStringField(json, L"id");
  if (id == L"add" && on_add_) {
    on_add_();
  } else if (id == L"remove" && on_remove_) {
    selected_ = JsonIntField(json, L"index", selected_);
    on_remove_(selected_);
  } else if (id == L"seat" && on_seat_) {
    on_seat_();
  } else if (id == L"apply") {
    selected_ = JsonIntField(json, L"index", selected_);
    const std::wstring title = JsonStringField(json, L"title");
    const int page = JsonIntField(json, L"page", 0);
    if (on_rename_ && !title.empty()) {
      on_rename_(selected_, title);
    }
    if (on_page_) {
      on_page_(selected_, page);
    }
  } else if (id == L"org_virtual" && on_org_virtual_) {
    on_org_virtual_();
  } else if (id == L"rules" && on_edit_rules_) {
    on_edit_rules_();
  } else if (id == L"whitelist" && on_whitelist_) {
    on_whitelist_();
  } else if (id == L"whitelist_pick" && on_command_) {
    on_command_(L"whitelist_pick");
  } else if (id == L"whitelist_add" && on_command_) {
    on_command_(L"whitelist_add:" + JsonStringField(json, L"value"));
  } else if (id == L"whitelist_remove" && on_command_) {
    on_command_(L"whitelist_remove:" +
                std::to_wstring(JsonIntField(json, L"index", -1)));
  } else if (id == L"org_preview" && on_org_preview_) {
    on_org_preview_();
  } else if (id == L"org_reassign" && on_org_reassign_) {
    on_org_reassign_();
  } else if (id == L"folders" && on_page_folders_) {
    on_page_folders_();
  } else if (id == L"folder_remove" && on_command_) {
    on_command_(L"folder_remove:" +
                std::to_wstring(JsonIntField(json, L"index", 0)));
  } else if (id == L"record" && on_record_cfg_) {
    on_record_cfg_();
  } else if (id == L"help" && on_help_) {
    on_help_();
  } else if (id == L"snap_save" && on_snap_save_) {
    on_snap_save_();
  } else if (id == L"snap_manage" && on_snap_manage_) {
    on_snap_manage_();
  } else if (id == L"snap_refresh" && on_command_) {
    on_command_(L"snap_refresh");
  } else if (id == L"snap_apply" && on_command_) {
    on_command_(L"snap_apply:" +
                std::to_wstring(JsonIntField(json, L"index", 0)));
  } else if (id == L"snap_delete" && on_command_) {
    on_command_(L"snap_delete:" +
                std::to_wstring(JsonIntField(json, L"index", 0)));
  } else if (id == L"minutes" && on_minutes_) {
    on_minutes_();
  } else if (id == L"wallpaper" && on_wallpaper_) {
    on_wallpaper_();
  } else if (id == L"layout" && on_layout_) {
    on_layout_();
  } else if (id == L"notes" && on_notes_) {
    on_notes_();
  } else if (id == L"theme" && on_theme_) {
    on_theme_();
  } else if (id == L"hotkeys" && on_hotkeys_) {
    on_hotkeys_();
  } else if (id == L"hotkeys_refresh") {
    PushState();
  } else if (id == L"hotkey_help" && on_hotkey_help_) {
    on_hotkey_help_();
  } else if (id == L"hotkey_capture" && on_hotkey_capture_) {
    on_hotkey_capture_(JsonStringField(json, L"key"));
  } else if (id == L"hotkey_set" && on_hotkey_set_) {
    on_hotkey_set_(JsonStringField(json, L"key"),
                   JsonStringField(json, L"chord"));
  } else if (id == L"fence_set_style" && on_fence_style_) {
    on_fence_style_(JsonIntField(json, L"index", 0),
                    JsonStringField(json, L"value"));
  } else if (id == L"fence_set_style_all" && on_fence_style_all_) {
    on_fence_style_all_(JsonStringField(json, L"value"));
  } else if (id == L"fence_set_opacity" && on_fence_opacity_) {
    on_fence_opacity_(JsonIntField(json, L"index", 0),
                      JsonIntField(json, L"value", 70));
  } else if (id == L"fence_set_colors" && on_fence_colors_) {
    on_fence_colors_(JsonIntField(json, L"index", 0),
                     JsonIntField(json, L"enabled", 0) != 0,
                     JsonStringField(json, L"bg"),
                     JsonStringField(json, L"accent"));
  } else if (id == L"fence_set_page" && on_fence_page_card_) {
    on_fence_page_card_(JsonIntField(json, L"index", 0),
                        JsonIntField(json, L"page", 0));
  } else if (id == L"fence_set_labels" && on_fence_labels_) {
    on_fence_labels_(JsonIntField(json, L"index", 0),
                     JsonIntField(json, L"lines", 2),
                     JsonIntField(json, L"show", 1) != 0);
  } else if (id == L"fence_set_scale" && on_fence_scale_) {
    on_fence_scale_(JsonIntField(json, L"index", 0),
                    JsonIntField(json, L"value", 100));
  } else if (id == L"fence_set_sort" && on_fence_sort_) {
    on_fence_sort_(JsonIntField(json, L"index", 0),
                   JsonStringField(json, L"by"),
                   JsonIntField(json, L"desc", 0) != 0);
  } else if (id == L"fence_set_radius" && on_fence_radius_) {
    on_fence_radius_(JsonIntField(json, L"index", 0),
                     JsonIntField(json, L"value", -1));
  } else if (id == L"fence_toggle_title" && on_fence_title_) {
    on_fence_title_(JsonIntField(json, L"index", 0));
  } else if (id == L"fence_toggle_fade" && on_fence_fade_) {
    on_fence_fade_(JsonIntField(json, L"index", 0));
  } else if (id == L"goto_page" && on_command_) {
    current_page_ = JsonIntField(json, L"page", current_page_);
    on_command_(L"goto_page:" + std::to_wstring(current_page_));
  } else if (id == L"rename_fence") {
    selected_ = JsonIntField(json, L"index", selected_);
    if (on_command_) {
      on_command_(L"rename_fence:" + std::to_wstring(selected_));
    }
  } else if (id == L"edit_fence" && on_command_) {
    selected_ = JsonIntField(json, L"index", selected_);
    // edit_fence:INDEX|kinds|title — title last so it may contain '|' rarely;
    // UI escapes '|' out of the name field.
    const std::wstring kinds = JsonStringField(json, L"kinds");
    std::wstring title = JsonStringField(json, L"title");
    for (auto& ch : title) {
      if (ch == L'|') {
        ch = L'/';
      }
    }
    on_command_(L"edit_fence:" + std::to_wstring(selected_) + L"|" + kinds +
                L"|" + title);
  } else if (id == L"fence_toggle_lock" && on_command_) {
    on_command_(L"fence_toggle_lock:" +
                std::to_wstring(JsonIntField(json, L"index", 0)));
  } else if (id == L"fence_toggle_content_lock" && on_command_) {
    on_command_(L"fence_toggle_content_lock:" +
                std::to_wstring(JsonIntField(json, L"index", 0)));
  } else if (id == L"fence_toggle_visible" && on_command_) {
    on_command_(L"fence_toggle_visible:" +
                std::to_wstring(JsonIntField(json, L"index", 0)));
  } else if (id == L"toggle_page_lock" && on_command_) {
    on_command_(L"toggle_page_lock:" +
                std::to_wstring(JsonIntField(json, L"page", 0)));
  } else if (id == L"fence_apply_all" && on_ext_field_) {
    // Reuse theme_id path → ApplyThemeId (all fences + default preset).
    const std::wstring style = JsonStringField(json, L"value");
    on_ext_field_(L"theme_id",
                  style.empty() ? WidenUtf8("navy") : style);
  } else if (on_command_) {
    on_command_(id);
  }
}

void SettingsWindow::ApplyStyle(COLORREF, COLORREF, COLORREF) {}

void SettingsWindow::Destroy() {
  DestroyWebView();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void SettingsWindow::WarmWebView() {
  if (!hwnd_ || web_available_) {
    return;
  }
  CreateWebView();
}

void SettingsWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ApplyBrandWindowIcons(hwnd_);
  // Tray re-open: restore if minimized; SW_SHOW alone leaves iconic windows
  // stuck and looks like "no reaction".
  if (IsIconic(hwnd_)) {
    ShowWindow(hwnd_, SW_RESTORE);
  } else {
    ShowWindow(hwnd_, SW_SHOW);
  }
  // 市面标准规范：干净拉起前台，绝不在 TOPMOST 与 NOTOPMOST 间震荡
  SetForegroundWindow(hwnd_);
  BringWindowToTop(hwnd_);
  if (!web_available_) {
    CreateWebView();
  }
  ResizeWebView();
  if (controller_) {
    static_cast<ICoreWebView2Controller*>(controller_)->put_IsVisible(TRUE);
  }
  // Reload only when the document is not ready yet. Re-navigating on every
  // tray open triggered ready→push storms and desktop fence flashing.
  if (webview_ && !web_ready_) {
    NavigateHome();
  }
}

void SettingsWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

bool SettingsWindow::IsOpen() const {
  return hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
}

void SettingsWindow::SetPageNames(const std::vector<std::wstring>& names,
                                  bool push) {
  page_names_ = names;
  if (page_names_.empty()) {
    page_names_ = {L"工作", L"文档"};
  }
  if (current_page_ >= static_cast<int>(page_names_.size())) {
    current_page_ = page_names_.empty()
                        ? 0
                        : static_cast<int>(page_names_.size()) - 1;
  }
  if (push) {
    PushState();
  }
}

void SettingsWindow::SetCurrentPage(int page, bool push) {
  current_page_ = page < 0 ? 0 : page;
  if (current_page_ >= static_cast<int>(page_names_.size()) &&
      !page_names_.empty()) {
    current_page_ = static_cast<int>(page_names_.size()) - 1;
  }
  if (push) {
    PushState();
  }
}

bool SettingsWindow::ApplyExtToggleLocal(const std::wstring& id, bool value) {
  if (id == L"minutes_on_bar" || id == L"minutes_enabled") {
    // Single UI switch drives both prefs (enable ≡ show on page bar).
    ext_.minutes_enabled = value;
    ext_.minutes_on_bar = value;
  } else if (id == L"calculator_enabled") {
    ext_.calculator_enabled = value;
  } else if (id == L"todos_enabled") {
    ext_.todos_enabled = value;
  } else if (id == L"record_enabled") {
    ext_.record_enabled = value;
  } else if (id == L"notepad_enabled") {
    ext_.notepad_enabled = value;
  } else if (id == L"elevate_hotkey_helper") {
    ext_.elevate_hotkey_helper = value;
  } else if (id == L"record_on_bar") {
    ext_.record_on_bar = value;
  } else if (id == L"calculator_on_bar") {
    ext_.calculator_on_bar = value;
  } else if (id == L"todos_on_bar") {
    ext_.todos_on_bar = value;
  } else if (id == L"search_desktop") {
    ext_.search_desktop = value;
  } else if (id == L"search_documents") {
    ext_.search_documents = value;
  } else if (id == L"search_downloads") {
    ext_.search_downloads = value;
  } else if (id == L"search_all_drives") {
    ext_.search_all_drives = value;
  } else if (id == L"search_include_open_folders") {
    ext_.search_include_open_folders = value;
  } else if (id == L"search_include_hidden") {
    ext_.search_include_hidden = value;
  } else if (id == L"pet_auto_wander") {
    ext_.pet_auto_wander = value;
  } else if (id == L"pet_show_bubbles") {
    ext_.pet_show_bubbles = value;
  } else if (id == L"check_update_on_startup") {
    ext_.check_update_on_startup = value;
  } else if (id == L"desktop_right_click_menu") {
    ext_.desktop_right_click_menu = value;
  } else if (id == L"close_to_tray") {
    ext_.close_to_tray = value;
  } else if (id == L"show_fences") {
    ext_.show_fences = value;
  } else if (id == L"double_click_hide") {
    ext_.double_click_hide = value;
  } else if (id == L"enable_public_desktop") {
    ext_.enable_public_desktop = value;
  } else if (id == L"show_page_indicator") {
    ext_.show_page_indicator = value;
  } else {
    return false;
  }
  return true;
}

void SettingsWindow::SetFenceNames(const std::vector<std::wstring>& names) {
  fence_names_ = names;
  if (selected_ >= static_cast<int>(fence_names_.size())) {
    selected_ = fence_names_.empty() ? 0
                                     : static_cast<int>(fence_names_.size()) - 1;
  }
  // Do not PushState here — callers (RefreshSettingsList / OnReady) follow with
  // PushExtensionSettings so pages + fence_cards arrive in one payload. An early
  // push with empty page_names_ painted a blank 桌面分区 page.
}

void SettingsWindow::SetToggles(bool dock_on, bool pet_on, bool autostart_on) {
  dock_on_ = dock_on;
  pet_on_ = pet_on;
  autostart_on_ = autostart_on;
  PushState();
}

void SettingsWindow::SetOrganizePrefs(bool startup_on, bool watch_on) {
  org_startup_on_ = startup_on;
  org_watch_on_ = watch_on;
  PushState();
}

void SettingsWindow::SetOrganizePreview(
    const std::vector<std::wstring>& preview_lines, int pending, int organized,
    int total, const std::vector<OrgFileRow>& files) {
  org_preview_ = preview_lines;
  if (pending >= 0) {
    org_pending_ = pending;
  } else {
    org_pending_ = static_cast<int>(preview_lines.size());
  }
  if (organized >= 0) {
    org_organized_ = organized;
  }
  if (total >= 0) {
    org_total_ = total;
  }
  org_files_ = files;
  if (org_files_.empty() && !preview_lines.empty()) {
    for (const auto& line : preview_lines) {
      OrgFileRow row;
      const size_t arrow = line.find(L" → ");
      row.name = arrow == std::wstring::npos ? line : line.substr(0, arrow);
      row.type = L"文件";
      row.size = L"—";
      org_files_.push_back(std::move(row));
    }
  }
  PushState();
}

void SettingsWindow::SetHandlers(VoidFn on_add, IndexFn on_remove, VoidFn on_seat,
                                 RenameFn on_rename, PageFn on_page,
                                 VoidFn on_org_virtual, BoolFn on_dock,
                                 BoolFn on_pet, VoidFn on_edit_rules,
                                 BoolFn on_autostart, VoidFn on_whitelist,
                                 VoidFn on_page_folders, VoidFn on_help,
                                 BoolFn on_org_startup, BoolFn on_org_watch,
                                 VoidFn on_record_cfg, VoidFn on_org_preview) {
  on_add_ = std::move(on_add);
  on_remove_ = std::move(on_remove);
  on_seat_ = std::move(on_seat);
  on_rename_ = std::move(on_rename);
  on_page_ = std::move(on_page);
  on_org_virtual_ = std::move(on_org_virtual);
  on_dock_ = std::move(on_dock);
  on_pet_ = std::move(on_pet);
  on_edit_rules_ = std::move(on_edit_rules);
  on_autostart_ = std::move(on_autostart);
  on_whitelist_ = std::move(on_whitelist);
  on_page_folders_ = std::move(on_page_folders);
  on_help_ = std::move(on_help);
  on_org_startup_ = std::move(on_org_startup);
  on_org_watch_ = std::move(on_org_watch);
  on_record_cfg_ = std::move(on_record_cfg);
  on_org_preview_ = std::move(on_org_preview);
}

void SettingsWindow::SetExtraHandlers(
    VoidFn on_snap_save, VoidFn on_snap_manage,
    VoidFn on_minutes, VoidFn on_wallpaper, VoidFn on_layout, VoidFn on_notes,
    VoidFn on_theme, VoidFn on_hotkeys, VoidFn on_hotkey_help) {
  on_snap_save_ = std::move(on_snap_save);
  on_snap_manage_ = std::move(on_snap_manage);
  on_minutes_ = std::move(on_minutes);
  on_wallpaper_ = std::move(on_wallpaper);
  on_layout_ = std::move(on_layout);
  on_notes_ = std::move(on_notes);
  on_theme_ = std::move(on_theme);
  on_hotkeys_ = std::move(on_hotkeys);
  on_hotkey_help_ = std::move(on_hotkey_help);
}

void SettingsWindow::SetCommandHandler(
    std::function<void(const std::wstring&)> on_command) {
  on_command_ = std::move(on_command);
}

LRESULT CALLBACK SettingsWindow::HostProc(HWND hwnd, UINT msg, WPARAM wparam,
                                          LPARAM lparam) {
  auto* self =
      reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  if (self && msg == WM_SIZE) {
    self->ResizeWebView();
  }
  if (self && msg == WM_TIMER && wparam == 7701) {
    KillTimer(hwnd, 7701);
    // settings.html never posted ready — still try to push so UI is not empty.
    if (!self->web_ready_) {
      self->OnWebReady();
    }
    return 0;
  }
  if (self && msg == WM_TIMER && wparam == 7702) {
    KillTimer(hwnd, 7702);
    self->OnWebReady();
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam) {
  SettingsWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<SettingsWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT SettingsWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_APP + 70: {
      // Drain queue — goto_page + rename_page can arrive back-to-back.
      while (!pending_action_jsons_.empty()) {
        const std::wstring json = std::move(pending_action_jsons_.front());
        pending_action_jsons_.erase(pending_action_jsons_.begin());
        if (!json.empty()) {
          DispatchAction(json);
        }
      }
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
      const UINT dpi = GetDpiForWindow(hwnd_);
      mmi->ptMinTrackSize.x = MulDiv(820, dpi, 96);
      mmi->ptMinTrackSize.y = MulDiv(560, dpi, 96);
      return 0;
    }
    case WM_SIZE:
      ResizeWebView();
      return 0;
    case WM_CLOSE:
      if (ext_.close_to_tray) {
        Hide();
        if (on_hide_to_tray_) {
          on_hide_to_tray_();
        }
      } else if (on_exit_) {
        on_exit_();
      } else {
        Hide();
      }
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
