#include "mist_webview.h"

#include "display_geom.h"
#include "paths_util.h"

#include <dwmapi.h>
#include <objbase.h>
#ifndef interface
#define interface struct
#endif
#include <WebView2.h>
#include <wrl.h>

#include <memory>
#include <string>
#include <unordered_map>

#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClass[] = L"DeskTidyNativeMistDialog";
constexpr wchar_t kHostClass[] = L"DeskTidyNativeMistDialogHost";

std::unordered_map<MistWebDialog*, std::shared_ptr<bool>>& MistAliveMap() {
  static std::unordered_map<MistWebDialog*, std::shared_ptr<bool>> m;
  return m;
}

std::shared_ptr<bool> EnsureMistAlive(MistWebDialog* dlg) {
  auto& m = MistAliveMap();
  auto it = m.find(dlg);
  if (it == m.end() || !it->second) {
    auto tok = std::make_shared<bool>(true);
    m[dlg] = tok;
    return tok;
  }
  *it->second = true;
  return it->second;
}

void InvalidateMistAlive(MistWebDialog* dlg) {
  auto& m = MistAliveMap();
  auto it = m.find(dlg);
  if (it != m.end()) {
    if (it->second) {
      *it->second = false;
    }
    m.erase(it);
  }
}

bool PathExists(const std::wstring& path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES;
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

void MistFocusDialog(MistWebDialog* dlg) {
  // WebView2 does not take keyboard input until the host + controller are
  // focused — JS el.focus() alone leaves rename prompts needing a click.
  if (!dlg || !dlg->hwnd || !IsWindow(dlg->hwnd)) {
    return;
  }
  SetForegroundWindow(dlg->hwnd);
  BringWindowToTop(dlg->hwnd);
  SetActiveWindow(dlg->hwnd);
  if (dlg->host && IsWindow(dlg->host)) {
    SetFocus(dlg->host);
  }
  if (dlg->controller) {
    static_cast<ICoreWebView2Controller*>(dlg->controller)
        ->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  }
}

void ResizeWeb(MistWebDialog* dlg) {
  if (!dlg || !dlg->hwnd || !dlg->host) {
    return;
  }
  RECT rc{};
  GetClientRect(dlg->hwnd, &rc);
  MoveWindow(dlg->host, 0, 0, rc.right, rc.bottom, TRUE);
  if (dlg->controller) {
    RECT bounds{0, 0, rc.right, rc.bottom};
    static_cast<ICoreWebView2Controller*>(dlg->controller)->put_Bounds(bounds);
  }
}

void NavigateHome(MistWebDialog* dlg) {
  if (!dlg || !dlg->webview || dlg->html_path.empty()) {
    return;
  }
  static_cast<ICoreWebView2*>(dlg->webview)
      ->Navigate(FileUrlFromPath(dlg->html_path).c_str());
}

void CreateWeb(MistWebDialog* dlg) {
  if (!dlg || !dlg->hwnd || dlg->web_available || dlg->controller) {
    return;
  }
  static bool host_reg = false;
  if (!host_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // NULL brush: frameless mist cards need a transparent host; captioned
    // dialogs paint via WebView page background instead.
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    host_reg = true;
  }
  RECT rc{};
  GetClientRect(dlg->hwnd, &rc);
  dlg->host = CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_VISIBLE, 0, 0,
                              rc.right, rc.bottom, dlg->hwnd, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
  if (!dlg->host) {
    return;
  }

  std::wstring user_data =
      JoinPath(NativeDataDir(), dlg->user_data_subdir.empty()
                                    ? L"webview2-mist"
                                    : dlg->user_data_subdir);
  CreateDirectoryW(user_data.c_str(), nullptr);

  const auto alive = EnsureMistAlive(dlg);
  const HWND host_hwnd = dlg->host;

  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [dlg, alive, host_hwnd](HRESULT result,
                                  ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env || !alive || !*alive || !dlg ||
                !dlg->hwnd || !IsWindow(dlg->hwnd) || !dlg->host ||
                dlg->host != host_hwnd || !IsWindow(host_hwnd)) {
              return S_OK;
            }
            env->CreateCoreWebView2Controller(
                dlg->host,
                Callback<
                    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [dlg, alive, host_hwnd](
                        HRESULT result2,
                        ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(result2) || !controller) {
                        return S_OK;
                      }
                      if (!alive || !*alive || !dlg || !dlg->hwnd ||
                          !IsWindow(dlg->hwnd) || !dlg->host ||
                          dlg->host != host_hwnd || !IsWindow(host_hwnd)) {
                        controller->Close();
                        return S_OK;
                      }
                      dlg->controller = controller;
                      controller->AddRef();
                      ICoreWebView2* web = nullptr;
                      controller->get_CoreWebView2(&web);
                      if (!web) {
                        return S_OK;
                      }
                      dlg->webview = web;
                      dlg->web_available = true;
                      // Frameless mist cards: transparent WebView so the rounded
                      // HTML card is not boxed by an opaque white HWND fill.
                      if (dlg->frameless) {
                        ComPtr<ICoreWebView2Controller2> c2;
                        if (SUCCEEDED(controller->QueryInterface(
                                IID_PPV_ARGS(&c2))) &&
                            c2) {
                          COREWEBVIEW2_COLOR clear{};
                          clear.A = 0;
                          c2->put_DefaultBackgroundColor(clear);
                        }
                      }
                      ResizeWeb(dlg);
                      controller->put_IsVisible(TRUE);
                      MistFocusDialog(dlg);
                      web->add_WebMessageReceived(
                          Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [dlg, alive](
                                  ICoreWebView2*,
                                  ICoreWebView2WebMessageReceivedEventArgs*
                                      args) -> HRESULT {
                                if (!alive || !*alive || !dlg || !dlg->hwnd ||
                                    !IsWindow(dlg->hwnd)) {
                                  return S_OK;
                                }
                                LPWSTR raw = nullptr;
                                if (SUCCEEDED(args->TryGetWebMessageAsString(
                                        &raw)) &&
                                    raw) {
                                  if (dlg->on_message) {
                                    if (dlg->on_message(raw)) {
                                      MistWebDialogClose(dlg);
                                    }
                                  }
                                  CoTaskMemFree(raw);
                                }
                                return S_OK;
                              })
                              .Get(),
                          nullptr);
                      web->add_NavigationCompleted(
                          Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [dlg, alive](
                                  ICoreWebView2*,
                                  ICoreWebView2NavigationCompletedEventArgs*
                                      args) -> HRESULT {
                                if (!alive || !*alive || !dlg || !dlg->hwnd ||
                                    !IsWindow(dlg->hwnd)) {
                                  return S_OK;
                                }
                                BOOL ok = FALSE;
                                if (args) {
                                  args->get_IsSuccess(&ok);
                                }
                                if (ok) {
                                  dlg->web_ready = true;
                                  MistWebDialogPushState(dlg);
                                  MistFocusDialog(dlg);
                                  // Second pass after HTML applyState focus/select.
                                  PostMessageW(dlg->hwnd, WM_APP + 2, 0, 0);
                                }
                                return S_OK;
                              })
                              .Get(),
                          nullptr);
                      NavigateHome(dlg);
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());
}

void DestroyWeb(MistWebDialog* dlg) {
  if (!dlg) {
    return;
  }
  InvalidateMistAlive(dlg);
  if (dlg->webview) {
    static_cast<ICoreWebView2*>(dlg->webview)->Release();
    dlg->webview = nullptr;
  }
  if (dlg->controller) {
    static_cast<ICoreWebView2Controller*>(dlg->controller)->Close();
    static_cast<ICoreWebView2Controller*>(dlg->controller)->Release();
    dlg->controller = nullptr;
  }
  if (dlg->host) {
    DestroyWindow(dlg->host);
    dlg->host = nullptr;
  }
  dlg->web_available = false;
  dlg->web_ready = false;
}

LRESULT CALLBACK MistWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  MistWebDialog* dlg =
      reinterpret_cast<MistWebDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_CREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
      dlg = static_cast<MistWebDialog*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dlg));
      dlg->hwnd = hwnd;
      // Defer WebView2 create off WM_CREATE — starting it inside another
      // WebView2 message handler often never completes (blank dialog body).
      PostMessageW(hwnd, WM_APP + 1, 0, 0);
      return 0;
    }
    case WM_APP + 1:
      if (dlg) {
        CreateWeb(dlg);
      }
      return 0;
    case WM_APP + 2:
      if (dlg) {
        MistFocusDialog(dlg);
      }
      return 0;
    case WM_APP + 81:
      // Deferred host work posted from on_message (browse / nested prompt).
      if (dlg && dlg->on_deferred) {
        dlg->on_deferred(wparam, lparam);
      }
      return 0;
    case WM_ERASEBKGND:
      // Frameless: skip GDI fill so DWM/WebView transparency shows through.
      if (dlg && dlg->frameless) {
        return 1;
      }
      break;
    case WM_SIZE:
      ResizeWeb(dlg);
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      DestroyWeb(dlg);
      if (dlg) {
        dlg->done = true;
        dlg->hwnd = nullptr;
      }
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

std::wstring MistJsonEscape(const std::wstring& s) {
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
      case L'\b':
        o += L"\\b";
        break;
      case L'\f':
        o += L"\\f";
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
        if (c < 0x20 || c == 0x2028 || c == 0x2029) {
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

std::wstring MistJsonStringField(const std::wstring& json, const wchar_t* key) {
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

int MistJsonIntField(const std::wstring& json, const wchar_t* key, int fallback) {
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
  if (p >= json.size() ||
      !(json[p] == L'-' || (json[p] >= L'0' && json[p] <= L'9'))) {
    return fallback;
  }
  return _wtoi(json.c_str() + p);
}

std::wstring MistFindAssetHtml(const wchar_t* relative_under_assets) {
  wchar_t mod[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, mod, MAX_PATH);
  std::wstring dir = mod;
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    dir.resize(slash);
  }
  const std::wstring rel = std::wstring(L"assets\\") + relative_under_assets;
  const std::wstring candidates[] = {
      dir + L"\\" + rel,
      dir + L"\\..\\" + rel,
      dir + L"\\..\\..\\" + rel,
  };
  for (const auto& c : candidates) {
    if (PathExists(c)) {
      return c;
    }
  }
  return {};
}

void MistWebDialogPostJson(MistWebDialog* dlg, const std::wstring& json) {
  if (!dlg || !dlg->webview || !dlg->web_ready || json.empty()) {
    return;
  }
  auto* web = static_cast<ICoreWebView2*>(dlg->webview);
  std::wstring script =
      L"(function(){try{var m=" + json +
      L";if(m&&m.type==='dir_picked'){"
      L"if(document.getElementById('dir')){"
      L"document.getElementById('dir').value=m.path||'';}"
      L"}else if(typeof window.applyState==='function'){window.applyState(m);}"
      L"}catch(e){}})();";
  web->ExecuteScript(script.c_str(), nullptr);
}

void MistWebDialogPushState(MistWebDialog* dlg) {
  if (!dlg || !dlg->build_state) {
    return;
  }
  MistWebDialogPostJson(dlg, dlg->build_state());
}

void MistWebDialogFocus(MistWebDialog* dlg) {
  MistFocusDialog(dlg);
  if (dlg) {
    MistWebDialogPostJson(dlg, L"{\"type\":\"focus\"}");
  }
}

void MistWebDialogClose(MistWebDialog* dlg) {
  if (dlg && dlg->hwnd) {
    DestroyWindow(dlg->hwnd);
  }
}

void MistWebDialogRun(MistWebDialog* dlg, const wchar_t* title, int width,
                      int height) {
  if (!dlg) {
    return;
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MistWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    registered = true;
  }

  // Prefer the monitor under the cursor (multi-monitor safe).
  POINT cursor{};
  if (!GetCursorPos(&cursor)) {
    cursor = {0, 0};
  }
  // Design sizes are @96 DPI — scale + clamp to the host work area.
  ScaleDialogSizeForPoint(cursor, &width, &height);
  HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  RECT work{};
  if (GetMonitorInfoW(mon, &mi)) {
    work = mi.rcWork;
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  }
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 3;

  const DWORD ex = dlg->frameless
                       ? (WS_EX_TOOLWINDOW |
                          (dlg->topmost ? WS_EX_TOPMOST : 0))
                       : WS_EX_DLGMODALFRAME;
  const DWORD style = dlg->frameless
                          ? (WS_POPUP | WS_CLIPCHILDREN)
                          : (WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN);
  HWND hwnd = CreateWindowExW(ex, kClass, title, style, x, y, width, height,
                              dlg->owner, nullptr, GetModuleHandleW(nullptr),
                              dlg);
  if (!hwnd) {
    dlg->done = true;
    return;
  }
  if (dlg->frameless) {
    // Let the rounded HTML card float without an opaque rectangular plate.
    MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);
  }
  if (dlg->disable_owner && dlg->owner) {
    EnableWindow(dlg->owner, FALSE);
  }
  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
  MistFocusDialog(dlg);

  MSG msg{};
  while (!dlg->done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (dlg->owner && dlg->owner_accel &&
        TranslateAcceleratorW(dlg->owner, dlg->owner_accel, &msg)) {
      continue;
    }
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  DestroyWeb(dlg);
  if (dlg->hwnd && IsWindow(dlg->hwnd)) {
    DestroyWindow(dlg->hwnd);
    dlg->hwnd = nullptr;
  }
  if (dlg->disable_owner && dlg->owner) {
    EnableWindow(dlg->owner, TRUE);
    if (GetForegroundWindow() != dlg->owner) {
      SetForegroundWindow(dlg->owner);
    }
  }
}
