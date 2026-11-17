#include "vault_window.h"

#include "brand_mark.h"
#include "display_geom.h"
#include "i18n.h"
#include "mist_msg.h"
#include "notes_cloud_sync.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "toast.h"
#include "vault_crypto.h"
#include "vault_http.h"

#include <commdlg.h>
#include <objbase.h>
#ifndef interface
#define interface struct
#endif
#include <WebView2.h>
#include <wincrypt.h>
#include <wrl.h>

#include <Shellapi.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "comdlg32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeVault";
constexpr wchar_t kHostClass[] = L"DeskTidyNativeVaultWeb";
constexpr UINT_PTR kClipTimer = 70;
constexpr UINT kMsgLoginDone = WM_APP + 71;
constexpr UINT kMsgSyncDone = WM_APP + 72;
constexpr int kPanelW = 460;
constexpr int kPanelH = 560;

struct SyncOutcome {
  bool ok = false;
  bool have_entries = false;
  bool quiet = false;
  std::wstring message;
  std::vector<VaultHttpCredential> entries;
};

// Masked password prompt (classic Win32 modal, ES_PASSWORD) — never WebView2.
namespace {
struct PassCtx {
  std::wstring result;
  std::wstring label;
  bool accepted = false;
  bool done = false;
  HWND edit = nullptr;
  HWND owner = nullptr;
};

UINT PwDpi(HWND h) {
  using Fn = UINT(WINAPI*)(HWND);
  static Fn fn = nullptr;
  if (!fn) {
    fn = reinterpret_cast<Fn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  }
  if (fn && h) {
    const UINT d = fn(h);
    if (d) {
      return d;
    }
  }
  return 96;
}

int PwS(HWND h, int px) { return MulDiv(px, static_cast<int>(PwDpi(h)), 96); }

void PwFinish(HWND hwnd, PassCtx* ctx, bool ok) {
  if (ok) {
    wchar_t buf[512] = {};
    if (ctx->edit) {
      GetWindowTextW(ctx->edit, buf, 512);
    }
    ctx->result = buf;
    ctx->accepted = true;
  } else {
    ctx->accepted = false;
    ctx->result.clear();
  }
  DestroyWindow(hwnd);
}

LRESULT CALLBACK PwProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  PassCtx* ctx =
      reinterpret_cast<PassCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
      ctx = static_cast<PassCtx*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
      return TRUE;
    }
    case WM_CREATE: {
      if (!ctx) {
        return -1;
      }
      const int pad = PwS(hwnd, 16);
      const int lh = PwS(hwnd, 22);
      const int eh = PwS(hwnd, 28);
      const int bw = PwS(hwnd, 88);
      const int bh = PwS(hwnd, 30);
      const int gap = PwS(hwnd, 10);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const int cw = (std::max)(80, static_cast<int>(rc.right - rc.left));
      CreateWindowExW(0, L"STATIC",
                      (ctx->label.empty() ? L"主密码：" : ctx->label.c_str()),
                      WS_CHILD | WS_VISIBLE | SS_LEFT, pad, pad,
                      (std::max)(40, cw - pad * 2), lh, hwnd, nullptr,
                      GetModuleHandleW(nullptr), nullptr);
      ctx->edit = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
          pad, pad + lh + gap / 2, (std::max)(40, cw - pad * 2), eh, hwnd,
          reinterpret_cast<HMENU>(1001), GetModuleHandleW(nullptr), nullptr);
      const int by = pad + lh + gap / 2 + eh + gap;
      CreateWindowExW(0, L"BUTTON", L"取消",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                      cw - pad - bw * 2 - gap, by, bw, bh, hwnd,
                      reinterpret_cast<HMENU>(IDCANCEL),
                      GetModuleHandleW(nullptr), nullptr);
      CreateWindowExW(0, L"BUTTON", L"确定",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      cw - pad - bw, by, bw, bh, hwnd,
                      reinterpret_cast<HMENU>(IDOK), GetModuleHandleW(nullptr),
                      nullptr);
      HFONT f = CreateFontW(PwS(hwnd, 14), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
      if (f) {
        EnumChildWindows(
            hwnd,
            [](HWND c, LPARAM lp) -> BOOL {
              SendMessageW(c, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
              return TRUE;
            },
            reinterpret_cast<LPARAM>(f));
        SetPropW(hwnd, L"PwFont", f);
      }
      if (ctx->edit) {
        SendMessageW(ctx->edit, EM_SETPASSWORDCHAR, static_cast<WPARAM>(L'●'),
                     0);
        SetFocus(ctx->edit);
      }
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(w) == IDOK) {
        PwFinish(hwnd, ctx, true);
        return 0;
      }
      if (LOWORD(w) == IDCANCEL) {
        PwFinish(hwnd, ctx, false);
        return 0;
      }
      break;
    case WM_CLOSE:
      PwFinish(hwnd, ctx, false);
      return 0;
    case WM_DESTROY: {
      HFONT f = static_cast<HFONT>(RemovePropW(hwnd, L"PwFont"));
      if (f) {
        DeleteObject(f);
      }
      if (ctx) {
        ctx->done = true;
      }
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, w, l);
}

std::wstring PromptPassword(HWND owner, const wchar_t* title,
                            const wchar_t* label) {
  static bool registered = false;
  constexpr wchar_t kPwClass[] = L"DeskTidyNativeVaultPwd";
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PwProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
    wc.lpszClassName = kPwClass;
    RegisterClassExW(&wc);
    registered = true;
  }
  PassCtx ctx;
  ctx.owner = owner && IsWindow(owner) ? owner : nullptr;
  ctx.label = label ? label : L"";
  const UINT dpi = PwDpi(ctx.owner ? ctx.owner : GetDesktopWindow());
  const int width = MulDiv(420, static_cast<int>(dpi), 96);
  const int height = MulDiv(158, static_cast<int>(dpi), 96);
  RECT anchor{};
  if (ctx.owner) {
    GetWindowRect(ctx.owner, &anchor);
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
  }
  const int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
  const int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 3;
  HWND hwnd = CreateWindowExW(
      WS_EX_DLGMODALFRAME, kPwClass, title && title[0] ? title : L"输入",
      WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN, x, y, width, height,
      ctx.owner, nullptr, GetModuleHandleW(nullptr), &ctx);
  if (!hwnd) {
    return {};
  }
  if (ctx.owner) {
    EnableWindow(ctx.owner, FALSE);
  }
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
  if (ctx.edit) {
    SetFocus(ctx.edit);
  }
  MSG m{};
  while (!ctx.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
    if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE && m.hwnd &&
        (m.hwnd == hwnd || IsChild(hwnd, m.hwnd))) {
      PwFinish(hwnd, &ctx, false);
      continue;
    }
    if (!IsDialogMessageW(hwnd, &m)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
  }
  if (ctx.owner) {
    EnableWindow(ctx.owner, TRUE);
    SetForegroundWindow(ctx.owner);
  }
  return ctx.accepted ? ctx.result : std::wstring{};
}
}  // namespace

std::wstring VaultPath() { return JoinPath(NativeDataDir(), L"vault.dpapi"); }

bool ProtectBlob(const std::string& plain, std::vector<BYTE>* out) {
  DATA_BLOB in{};
  in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
  in.cbData = static_cast<DWORD>(plain.size());
  DATA_BLOB enc{};
  if (!CryptProtectData(&in, L"DeskTidyNativeVault", nullptr, nullptr, nullptr,
                        0, &enc)) {
    return false;
  }
  out->assign(enc.pbData, enc.pbData + enc.cbData);
  LocalFree(enc.pbData);
  return true;
}

bool UnprotectBlob(const std::vector<BYTE>& enc, std::string* plain) {
  DATA_BLOB in{};
  in.pbData = const_cast<BYTE*>(enc.data());
  in.cbData = static_cast<DWORD>(enc.size());
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
    return false;
  }
  plain->assign(reinterpret_cast<char*>(out.pbData), out.cbData);
  LocalFree(out.pbData);
  return true;
}

std::wstring JsonEscape(const std::wstring& s) {
  static const wchar_t kHex[] = L"0123456789ABCDEF";
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
      case L'\b':
        o += L"\\b";
        break;
      case L'\f':
        o += L"\\f";
        break;
      default:
        if (static_cast<unsigned>(c) < 0x20u) {
          o += L"\\u";
          o += kHex[(static_cast<unsigned>(c) >> 12) & 0xFu];
          o += kHex[(static_cast<unsigned>(c) >> 8) & 0xFu];
          o += kHex[(static_cast<unsigned>(c) >> 4) & 0xFu];
          o += kHex[static_cast<unsigned>(c) & 0xFu];
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
  p = json.find(L'"', json.find(L':', p) + 1);
  if (p == std::wstring::npos) {
    return {};
  }
  ++p;
  std::wstring v;
  while (p < json.size() && json[p] != L'"') {
    if (json[p] == L'\\' && p + 1 < json.size()) {
      ++p;
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
  p = json.find(L':', p);
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

bool PathExists(const std::wstring& path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES;
}

std::wstring FindVaultHtml() {
  wchar_t mod[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, mod, MAX_PATH);
  std::wstring dir = mod;
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    dir.resize(slash);
  }
  const std::wstring candidates[] = {
      dir + L"\\assets\\vault\\vault.html",
      dir + L"\\..\\assets\\vault\\vault.html",
      dir + L"\\..\\..\\assets\\vault\\vault.html",
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

std::wstring StripTabs(std::wstring s) {
  for (wchar_t& c : s) {
    if (c == L'\t' || c == L'\r' || c == L'\n') {
      c = L' ';
    }
  }
  return s;
}

bool JsonBoolFieldW(const std::wstring& json, const wchar_t* key,
                    bool fallback) {
  const std::wstring needle = std::wstring(L"\"") + key + L"\"";
  size_t p = json.find(needle);
  if (p == std::wstring::npos) {
    return fallback;
  }
  p = json.find(L':', p);
  if (p == std::wstring::npos) {
    return fallback;
  }
  ++p;
  while (p < json.size() && (json[p] == L' ' || json[p] == L'\t')) {
    ++p;
  }
  if (p >= json.size()) {
    return fallback;
  }
  if (json.compare(p, 4, L"true") == 0) {
    return true;
  }
  if (json.compare(p, 5, L"false") == 0) {
    return false;
  }
  if (json[p] == L'1') {
    return true;
  }
  return fallback;
}

std::string ReadWholeFileBytes(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

bool WriteWholeFileBytes(const std::wstring& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

std::wstring NowTimestamp() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t buf[32] = {};
  swprintf_s(buf, L"%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  return buf;
}

// Parse a Python-format export JSON into credentials (returns empty on bad
// input). Accepts {items:[...]}, a bare [...], or a single {...} object.
std::vector<VaultHttpCredential> ParseImportCredentials(
    const std::wstring& json) {
  std::vector<VaultHttpCredential> out;
  auto from_obj = [&](const std::wstring& obj) {
    VaultHttpCredential c;
    c.title = StripTabs(JsonStringField(obj, L"title"));
    if (c.title.empty()) {
      return;
    }
    c.username = StripTabs(JsonStringField(obj, L"username"));
    c.password = StripTabs(JsonStringField(obj, L"password"));
    c.url = StripTabs(JsonStringField(obj, L"url"));
    c.note = StripTabs(JsonStringField(obj, L"note"));
    c.pinned = JsonBoolFieldW(obj, L"pinned", false);
    out.push_back(std::move(c));
  };
  size_t arr = json.find(L"\"items\"");
  size_t begin = std::wstring::npos;
  if (arr != std::wstring::npos) {
    begin = json.find(L'[', arr);
  } else if (!json.empty() && json[0] == L'[') {
    begin = 0;
  } else if (json.find(L"\"title\"") != std::wstring::npos) {
    from_obj(json);
    return out;
  }
  if (begin == std::wstring::npos) {
    return out;
  }
  size_t i = begin + 1;
  while (i < json.size()) {
    while (i < json.size() && json[i] != L'{') {
      if (json[i] == L']') {
        return out;
      }
      ++i;
    }
    if (i >= json.size()) {
      break;
    }
    const size_t obj_end = json.find(L'}', i);
    if (obj_end == std::wstring::npos) {
      break;
    }
    from_obj(json.substr(i, obj_end - i + 1));
    i = obj_end + 1;
  }
  return out;
}

}  // namespace

bool VaultWindow::Create(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = VaultWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(0xEE, 0xF1, 0xF6));
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  // APPWINDOW only — TOOLWINDOW+APPWINDOW made the taskbar button look broken.
  hwnd_ = CreateWindowExW(
      WS_EX_APPWINDOW, kClass, L"账号管理",
      WS_POPUP | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, kPanelW,
      kPanelH, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  ApplyBrandWindowIcons(hwnd_);
  StartCloudPushWorker();
  Load();
  return true;
}

bool VaultWindow::CreateWebView() {
  if (web_available_ || controller_ || !hwnd_) {
    return web_available_;
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = VaultWindow::HostProc;
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
      rc.right = kPanelW;
      rc.bottom = kPanelH;
    }
    host_ = CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_VISIBLE, 0, 0,
                            rc.right, rc.bottom, hwnd_, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!host_) {
      return false;
    }
    SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  }

  const std::wstring user_data = JoinPath(NativeDataDir(), L"webview2-vault");
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
                                  OnWebReady();
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

void VaultWindow::DestroyWebView() {
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

void VaultWindow::ResizeWebView() {
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

void VaultWindow::NavigateHome() {
  if (!webview_) {
    return;
  }
  const std::wstring html = FindVaultHtml();
  if (html.empty()) {
    return;
  }
  static_cast<ICoreWebView2*>(webview_)->Navigate(FileUrlFromPath(html).c_str());
}

void VaultWindow::OnWebReady() {
  web_ready_ = true;
  PushState();
}

void VaultWindow::PushState() {
  if (!webview_ || !web_ready_) {
    return;
  }
  std::wstring json = L"{\"type\":\"state\",\"logged_in\":";
  json += VaultHttpLoggedIn() ? L"true" : L"false";
  json += L",\"session\":\"";
  json += JsonEscape(VaultHttpSessionLogin());
  json += L"\",\"status\":\"";
  json += JsonEscape(L"共 " + std::to_wstring(entries_.size()) +
                     L" 条 · 已同步");
  json += L"\",\"items\":[";
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (i) {
      json += L',';
    }
    const auto& e = entries_[i];
    json += L"{\"index\":";
    json += std::to_wstring(static_cast<int>(i));
    json += L",\"title\":\"";
    json += JsonEscape(e.title);
    json += L"\",\"username\":\"";
    json += JsonEscape(e.user);
    json += L"\",\"url\":\"";
    json += JsonEscape(e.url);
    json += L"\",\"note\":\"";
    json += JsonEscape(e.note);
    json += L"\",\"pass_len\":";
    json += std::to_wstring(static_cast<int>(e.pass.size()));
    json += L",\"pinned\":";
    json += e.pinned ? L"true" : L"false";
    json += L'}';
  }
  json += L']';
  json += L'}';

  auto* web = static_cast<ICoreWebView2*>(webview_);
  // Single channel: ExecuteScript only. PostWebMessageAsString is dropped when
  // nested inside another message callback (double IPC), which left the panel
  // empty on some machines.
  std::wstring script =
      L"(function(){try{var m=" + json +
      L";if(typeof window.applyState==='function'){window.applyState(m);}"
      L"}catch(e){}})();";
  web->ExecuteScript(script.c_str(), nullptr);
}

void VaultWindow::OnWebMessage(const std::wstring& json) {
  const std::wstring type = JsonStringField(json, L"type");
  if (type == L"ready") {
    OnWebReady();
    return;
  }
  if (type != L"action") {
    return;
  }
  const std::wstring id = JsonStringField(json, L"id");
  // Drag must stay synchronous for smooth move; everything else leaves
  // WebMessageReceived first (login/PromptText, file dialogs, MessageBox).
  if (id == L"drag_begin" || id == L"drag_move" || id == L"drag_end") {
    DispatchAction(json);
    return;
  }
  if (hwnd_) {
    pending_action_jsons_.push_back(json);
    PostMessageW(hwnd_, WM_APP + 70, 0, 0);
  } else {
    DispatchAction(json);
  }
}

void VaultWindow::DispatchAction(const std::wstring& json) {
  const std::wstring id = JsonStringField(json, L"id");
  if (id == L"hide") {
    Hide();
  } else if (id == L"refresh") {
    Load();
    PushState();
  } else if (id == L"logout") {
    VaultHttpLogout();
    PushState();
    ShowToast(L"已退出登录");
  } else if (id == L"login") {
    CloudLogin();
    PushState();
  } else if (id == L"register") {
    CloudRegister();
  } else if (id == L"copy_pass") {
    CopyPasswordAt(JsonIntField(json, L"index", -1));
  } else if (id == L"reveal_pass") {
    RevealPasswordAt(JsonIntField(json, L"index", -1));
  } else if (id == L"copy_user") {
    CopyUsernameAt(JsonIntField(json, L"index", -1));
  } else if (id == L"copy_url") {
    CopyUrlAt(JsonIntField(json, L"index", -1));
  } else if (id == L"copy_note") {
    CopyNoteAt(JsonIntField(json, L"index", -1));
  } else if (id == L"delete") {
    DeleteAt(JsonIntField(json, L"index", -1));
  } else if (id == L"toggle_pin") {
    const int index = JsonIntField(json, L"index", -1);
    if (index >= 0 && index < static_cast<int>(entries_.size())) {
      entries_[static_cast<size_t>(index)].pinned =
          !entries_[static_cast<size_t>(index)].pinned;
      dirty_ = true;
      SortEntries();
      Save();
      PushState();
      ScheduleCloudPush();
    }
  } else if (id == L"reorder") {
    // P1-27: drag-drop within the same pin group (Python reorder_credential_ids).
    const int from = JsonIntField(json, L"from", -1);
    const int to = JsonIntField(json, L"to", -1);
    if (from >= 0 && to >= 0 && from != to &&
        from < static_cast<int>(entries_.size()) &&
        to < static_cast<int>(entries_.size())) {
      const bool pin = entries_[static_cast<size_t>(from)].pinned;
      if (entries_[static_cast<size_t>(to)].pinned == pin) {
        Entry moved = std::move(entries_[static_cast<size_t>(from)]);
        entries_.erase(entries_.begin() + from);
        const int insert_at = from < to ? to - 1 : to;
        entries_.insert(entries_.begin() + insert_at, std::move(moved));
        for (size_t i = 0; i < entries_.size(); ++i) {
          entries_[i].sort_order = static_cast<int>(i);
          entries_[i].has_sort_order = true;
        }
        dirty_ = true;
        Save();
        PushState();
        ScheduleCloudPush();
      }
    }
  } else if (id == L"save_entry") {
    SaveEntryFromJson(json);
  } else if (id == L"open_url") {
    const int index = JsonIntField(json, L"index", -1);
    if (index >= 0 && index < static_cast<int>(entries_.size())) {
      const std::wstring& url = entries_[static_cast<size_t>(index)].url;
      if (!url.empty()) {
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
      }
    }
  } else if (id == L"drag_begin") {
    dragging_ = true;
    drag_start_.x = JsonIntField(json, L"x", 0);
    drag_start_.y = JsonIntField(json, L"y", 0);
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    drag_origin_.x = rc.left;
    drag_origin_.y = rc.top;
  } else if (id == L"drag_move" && dragging_) {
    const int x = JsonIntField(json, L"x", 0);
    const int y = JsonIntField(json, L"y", 0);
    SetWindowPos(hwnd_, nullptr, drag_origin_.x + (x - drag_start_.x),
                 drag_origin_.y + (y - drag_start_.y), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  } else if (id == L"drag_end") {
    dragging_ = false;
  } else if (id == L"export") {
    ExportBackup();
  } else if (id == L"import") {
    ImportBackup();
  } else if (id == L"sync") {
    CloudSync();
  }
}

void VaultWindow::Destroy() {
  destroying_ = true;
  Save();
  ClearClipboardIfOurs();
  DestroyWebView();
  // Stop the single-flight push worker (it owns the shared state, so it exits
  // safely even after this window is gone).
  if (cloud_push_) {
    {
      std::lock_guard<std::mutex> lk(cloud_push_->mu);
      cloud_push_->stopping = true;
    }
    cloud_push_->cv.notify_all();
    cloud_push_.reset();
  }
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void VaultWindow::WarmWebView() {
  if (!hwnd_ || web_available_) {
    return;
  }
  CreateWebView();
}

void VaultWindow::Show() {
  if (!hwnd_) {
    return;
  }
  if (!web_available_) {
    CreateWebView();
  }
  ResizeWebView();
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
  ApplyBrandWindowIcons(hwnd_);
  if (controller_) {
    static_cast<ICoreWebView2Controller*>(controller_)->put_IsVisible(TRUE);
  }
  // Keep in-memory entries from Create/edits — do not CryptUnprotect on every open.
  if (web_ready_) {
    PushState();
  } else if (webview_) {
    NavigateHome();
  }
  StartCloudRefresh();
}

void VaultWindow::Hide() {
  const bool was_open =
      hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
  if (dirty_) {
    Save();
    dirty_ = false;
  }
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
  // Notify after hide so App can sink fences before desktop-FG band restore
  // Raise climbs partitions over apps (open path already SeatAllFencesUnderApps).
  if (was_open && on_hide_) {
    on_hide_();
  }
}

void VaultWindow::PlaceBeside(HWND anchor) {
  if (!hwnd_) {
    return;
  }
  RECT work = PrimaryWorkArea();
  int x = work.right - kPanelW - 48;
  int y = work.top + 120;
  if (anchor && IsWindow(anchor)) {
    RECT ar{};
    GetWindowRect(anchor, &ar);
    const int ah = ar.bottom - ar.top;
    // Float-panel convention: sit beside the launcher; align mid-lines.
    x = ar.left - kPanelW - 12;
    y = ar.top + ah / 2 - kPanelH / 2;
    if (x < work.left) {
      x = ar.right + 12;
    }
  }
  if (y + kPanelH > work.bottom) {
    y = work.bottom - kPanelH;
  }
  if (y < work.top) {
    y = work.top;
  }
  SetWindowPos(hwnd_, HWND_TOP, x, y, kPanelW, kPanelH,
               SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

void VaultWindow::SetClipboardClearSeconds(int seconds) {
  if (seconds < 0) {
    seconds = 0;
  }
  if (seconds > 300) {
    seconds = 300;
  }
  clipboard_clear_seconds_ = seconds;
}

void VaultWindow::CopyFieldToClipboard(const std::wstring& value,
                                       const wchar_t* tip,
                                       bool schedule_clear) {
  if (value.empty() || !OpenClipboard(hwnd_)) {
    return;
  }
  EmptyClipboard();
  const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (!mem) {
    CloseClipboard();
    return;
  }
  void* dst = GlobalLock(mem);
  memcpy(dst, value.c_str(), bytes);
  GlobalUnlock(mem);
  SetClipboardData(CF_UNICODETEXT, mem);
  CloseClipboard();
  if (clip_timer_) {
    KillTimer(hwnd_, clip_timer_);
    clip_timer_ = 0;
  }
  if (schedule_clear && clipboard_clear_seconds_ > 0) {
    clip_secret_ = value;
    clip_timer_ = SetTimer(
        hwnd_, kClipTimer,
        static_cast<UINT>(clipboard_clear_seconds_) * 1000u, nullptr);
  } else {
    clip_secret_.clear();
  }
  ShowToast(tip ? tip : L"已复制");
}

void VaultWindow::ReloadFromDisk() { Load(); }

std::vector<VaultWindow::ListRow> VaultWindow::ListRows() const {
  std::vector<ListRow> out;
  out.reserve(entries_.size());
  for (size_t i = 0; i < entries_.size(); ++i) {
    ListRow row;
    row.index = static_cast<int>(i);
    row.title = entries_[i].title;
    row.username = entries_[i].user;
    row.url = entries_[i].url;
    row.note = entries_[i].note;
    row.pass_len = static_cast<int>(entries_[i].pass.size());
    out.push_back(std::move(row));
  }
  return out;
}

void VaultWindow::CopyPasswordAt(int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) {
    return;
  }
  std::wstring tip = L"密码已复制";
  if (clipboard_clear_seconds_ > 0) {
    tip += L" · ";
    tip += std::to_wstring(clipboard_clear_seconds_);
    tip += L" 秒后清空";
  }
  CopyFieldToClipboard(entries_[static_cast<size_t>(index)].pass, tip.c_str(),
                       true);
}

void VaultWindow::RevealPasswordAt(int index) {
  if (!webview_ || !web_ready_) {
    return;
  }
  if (index < 0 || index >= static_cast<int>(entries_.size())) {
    return;
  }
  const std::wstring& pass = entries_[static_cast<size_t>(index)].pass;
  std::wstring json = L"{\"type\":\"revealed_password\",\"index\":";
  json += std::to_wstring(index);
  json += L",\"password\":\"";
  json += JsonEscape(pass);
  json += L"\"}";
  auto* web = static_cast<ICoreWebView2*>(webview_);
  web->PostWebMessageAsString(json.c_str());
}

void VaultWindow::CopyUsernameAt(int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) {
    return;
  }
  CopyFieldToClipboard(entries_[static_cast<size_t>(index)].user, L"账号已复制",
                       false);
}

void VaultWindow::CopyUrlAt(int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) {
    return;
  }
  CopyFieldToClipboard(entries_[static_cast<size_t>(index)].url, L"网址已复制",
                       false);
}

void VaultWindow::CopyNoteAt(int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) {
    return;
  }
  CopyFieldToClipboard(entries_[static_cast<size_t>(index)].note, L"备注已复制",
                       false);
}

void VaultWindow::DeleteAt(int index) {
  if (index < 0 || index >= static_cast<int>(entries_.size())) {
    return;
  }
  // Owner for the confirm box: prefer the vault window when visible, else the
  // current foreground window (e.g. the settings page that triggered delete).
  HWND owner = (hwnd_ && IsWindow(hwnd_) && IsWindowVisible(hwnd_))
                   ? hwnd_
                   : GetForegroundWindow();
  if (!MistConfirm(owner, L"账号管理", L"确定删除该账号？",
                   MistMsgTone::Danger)) {
    return;
  }
  entries_.erase(entries_.begin() + index);
  Save();
  PushState();
  ScheduleCloudPush();
}

void VaultWindow::SaveEntryFromJson(const std::wstring& json) {
  Entry e;
  e.title = StripTabs(JsonStringField(json, L"title"));
  e.user = StripTabs(JsonStringField(json, L"username"));
  e.pass = StripTabs(JsonStringField(json, L"password"));
  e.url = StripTabs(JsonStringField(json, L"url"));
  e.note = StripTabs(JsonStringField(json, L"note"));
  if (e.title.empty()) {
    return;
  }
  const int index = JsonIntField(json, L"index", -1);
  if (index >= 0 && index < static_cast<int>(entries_.size())) {
    if (e.pass.empty()) {
      e.pass = entries_[static_cast<size_t>(index)].pass;
    }
    entries_[static_cast<size_t>(index)] = std::move(e);
  } else {
    entries_.push_back(std::move(e));
  }
  Save();
  PushState();
  ScheduleCloudPush();
}

void VaultWindow::ClearClipboardIfOurs() {
  if (clip_timer_ && hwnd_) {
    KillTimer(hwnd_, clip_timer_);
    clip_timer_ = 0;
  }
  if (clip_secret_.empty()) {
    return;
  }
  if (OpenClipboard(hwnd_ ? hwnd_ : nullptr)) {
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data) {
      const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data));
      if (text && clip_secret_ == text) {
        GlobalUnlock(data);
        EmptyClipboard();
      } else if (text) {
        GlobalUnlock(data);
      }
    }
    CloseClipboard();
  }
  clip_secret_.clear();
}

std::vector<VaultHttpCredential> VaultWindow::SnapshotCredentials() const {
  std::vector<VaultHttpCredential> out;
  out.reserve(entries_.size());
  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto& e = entries_[i];
    VaultHttpCredential c;
    c.title = e.title;
    c.username = e.user;
    c.password = e.pass;
    c.url = e.url;
    c.note = e.note;
    c.pinned = e.pinned;
    c.sort_order = static_cast<int>(i);
    out.push_back(std::move(c));
  }
  return out;
}

void VaultWindow::ApplyPulledCredentials(
    const std::vector<VaultHttpCredential>& remote) {
  std::vector<Entry> previous = entries_;
  entries_.clear();
  for (const auto& c : remote) {
    Entry e;
    e.title = c.title;
    e.user = c.username;
    e.pass = c.password;
    e.url = c.url;
    e.note = c.note;
    e.pinned = c.pinned;
    e.sort_order = c.sort_order;
    e.has_sort_order = true;
    if (e.note.empty()) {
      for (const auto& old : previous) {
        if (old.title == e.title && old.user == e.user) {
          e.note = old.note;
          break;
        }
      }
    }
    entries_.push_back(std::move(e));
  }
  SortEntries();
  Save();
  if (hwnd_ && IsWindowVisible(hwnd_)) {
    PushState();
  }
}

bool VaultWindow::QuietCloudRefresh() {
  if (!VaultHttpLoggedIn()) {
    return false;
  }
  if (entries_.empty()) {
    Load();
  }
  std::vector<VaultHttpCredential> remote = SnapshotCredentials();
  if (!VaultHttpPullMerge(&remote)) {
    return false;
  }
  ApplyPulledCredentials(remote);
  return true;
}

void VaultWindow::Save() {
  std::ostringstream ss;
  for (const auto& e : entries_) {
    ss << NarrowUtf8(e.title) << '\t' << NarrowUtf8(e.user) << '\t'
       << NarrowUtf8(e.pass) << '\t' << NarrowUtf8(e.url) << '\t'
       << NarrowUtf8(e.note) << '\t' << (e.pinned ? '1' : '0') << '\n';
  }
  std::vector<BYTE> enc;
  if (!ProtectBlob(ss.str(), &enc)) {
    return;
  }
  std::ofstream out(VaultPath(), std::ios::binary);
  if (out) {
    out.write(reinterpret_cast<const char*>(enc.data()),
              static_cast<std::streamsize>(enc.size()));
  }
  dirty_ = false;
}

void VaultWindow::SortEntries() {
  // Pinned first, then explicit sort_order ascending, then insertion order.
  std::stable_sort(entries_.begin(), entries_.end(),
                   [](const Entry& a, const Entry& b) {
                     if (a.pinned != b.pinned) {
                       return a.pinned && !b.pinned;
                     }
                     if (a.has_sort_order != b.has_sort_order) {
                       return a.has_sort_order;
                     }
                     if (a.has_sort_order) {
                       return a.sort_order < b.sort_order;
                     }
                     return false;
                   });
}

void VaultWindow::Load() {
  entries_.clear();
  std::ifstream in(VaultPath(), std::ios::binary);
  if (!in) {
    return;
  }
  std::vector<BYTE> enc((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  std::string plain;
  if (!UnprotectBlob(enc, &plain)) {
    return;
  }
  std::istringstream ss(plain);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) {
      continue;
    }
    Entry e;
    size_t p0 = 0;
    auto next = [&](std::wstring* field) {
      const size_t p1 = line.find('\t', p0);
      const std::string part =
          (p1 == std::string::npos) ? line.substr(p0) : line.substr(p0, p1 - p0);
      *field = WidenUtf8(part);
      p0 = (p1 == std::string::npos) ? line.size() : p1 + 1;
    };
    next(&e.title);
    next(&e.user);
    next(&e.pass);
    next(&e.url);
    if (p0 < line.size()) {
      next(&e.note);
    }
    if (p0 < line.size()) {
      std::wstring pinned;
      next(&pinned);
      e.pinned = (pinned == L"1" || pinned == L"true");
    }
    entries_.push_back(std::move(e));
  }
  SortEntries();
}

void VaultWindow::ExportBackup() {
  Save();
  wchar_t file[MAX_PATH] = L"vault_export.json";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter =
      L"DeskTidy vault JSON\0*.json\0DPAPI backup\0*.dpapi\0All\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_OVERWRITEPROMPT;
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }
  const std::wstring path = file;
  if (path.size() >= 6 && _wcsicmp(path.c_str() + path.size() - 6,
                                    L".dpapi") == 0) {
    if (CopyFileW(VaultPath().c_str(), file, FALSE)) {
      ShowToast(L"账号库已导出（DPAPI）");
    }
    return;
  }
  // Python-compatible plaintext JSON (matches account_vault.py semantics).
  std::wstring json = L"{\n  \"format\": \"desktidy-vault\",\n";
  json += L"  \"version\": 1,\n";
  json += L"  \"exported_at\": \"" + JsonEscape(NowTimestamp()) + L"\",\n";
  json += L"  \"items\": [\n";
  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto& e = entries_[i];
    if (i) {
      json += L",\n";
    }
    json += L"    {\"title\": \"" + JsonEscape(e.title) + L"\",\n";
    json += L"     \"username\": \"" + JsonEscape(e.user) + L"\",\n";
    json += L"     \"password\": \"" + JsonEscape(e.pass) + L"\",\n";
    json += L"     \"url\": \"" + JsonEscape(e.url) + L"\",\n";
    json += L"     \"note\": \"" + JsonEscape(e.note) + L"\",\n";
    json += L"     \"pinned\": " + std::wstring(e.pinned ? L"true" : L"false") +
            L"}";
  }
  json += L"\n  ]\n}\n";
  if (WriteWholeFileBytes(path, NarrowUtf8(json))) {
    ShowToast(L"账号库已导出（JSON，含明文密码，请妥善保管）");
  }
}

void VaultWindow::ImportBackup() {
  wchar_t file[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter =
      L"DeskTidy vault JSON\0*.json\0DPAPI backup\0*.dpapi\0All\0*.*\0";
  ofn.Flags = OFN_FILEMUSTEXIST;
  if (!GetOpenFileNameW(&ofn)) {
    return;
  }
  std::string raw = ReadWholeFileBytes(file);
  if (raw.empty()) {
    MistAlert(hwnd_, L"账号管理", L"导入失败：文件为空或无法读取。",
              MistMsgTone::Warn);
    return;
  }
  // Strip an optional UTF-8 BOM (Python reads with encoding="utf-8-sig").
  if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
      static_cast<unsigned char>(raw[1]) == 0xBB &&
      static_cast<unsigned char>(raw[2]) == 0xBF) {
    raw.erase(0, 3);
  }
  // JSON (Python) format if it starts with '{' or '['.
  if (raw[0] == '{' || raw[0] == '[') {
    std::vector<VaultHttpCredential> creds =
        ParseImportCredentials(WidenUtf8(raw));
    if (creds.empty()) {
      MistAlert(hwnd_, L"账号管理", L"导入失败：JSON 中无可导入的账号条目。",
                MistMsgTone::Warn);
      return;
    }
    // Merge by title+username (Python import creates; never wipe local) — P0-9.
    int added = 0;
    int updated = 0;
    for (const auto& c : creds) {
      bool found = false;
      for (auto& e : entries_) {
        if (_wcsicmp(e.title.c_str(), c.title.c_str()) == 0 &&
            _wcsicmp(e.user.c_str(), c.username.c_str()) == 0) {
          e.pass = c.password.empty() ? e.pass : c.password;
          e.url = c.url.empty() ? e.url : c.url;
          e.note = c.note.empty() ? e.note : c.note;
          e.pinned = c.pinned;
          found = true;
          ++updated;
          break;
        }
      }
      if (!found) {
        Entry e;
        e.title = c.title;
        e.user = c.username;
        e.pass = c.password;
        e.url = c.url;
        e.note = c.note;
        e.pinned = c.pinned;
        entries_.push_back(std::move(e));
        ++added;
      }
    }
    Save();
    PushState();
    ScheduleCloudPush();
    ShowToast(L"账号库已导入：新增 " + std::to_wstring(added) + L"，更新 " +
              std::to_wstring(updated));
    return;
  }
  // Legacy DPAPI whole-file blob.
  std::vector<BYTE> enc(raw.begin(), raw.end());
  std::string plain;
  if (!UnprotectBlob(enc, &plain)) {
    MistAlert(hwnd_, L"账号管理",
              L"导入失败：DPAPI 文件无法解密（可能来自其他 Windows 账户）。",
              MistMsgTone::Warn);
    return;
  }
  std::istringstream ss(plain);
  std::string line;
  int added = 0;
  int updated = 0;
  while (std::getline(ss, line)) {
    if (line.empty()) {
      continue;
    }
    Entry incoming;
    size_t p0 = 0;
    auto next = [&](std::wstring* field) {
      const size_t p1 = line.find('\t', p0);
      const std::string part =
          (p1 == std::string::npos) ? line.substr(p0) : line.substr(p0, p1 - p0);
      *field = WidenUtf8(part);
      p0 = (p1 == std::string::npos) ? line.size() : p1 + 1;
    };
    next(&incoming.title);
    next(&incoming.user);
    next(&incoming.pass);
    next(&incoming.url);
    if (p0 < line.size()) {
      next(&incoming.note);
    }
    bool found = false;
    for (auto& e : entries_) {
      if (_wcsicmp(e.title.c_str(), incoming.title.c_str()) == 0 &&
          _wcsicmp(e.user.c_str(), incoming.user.c_str()) == 0) {
        e.pass = incoming.pass.empty() ? e.pass : incoming.pass;
        e.url = incoming.url.empty() ? e.url : incoming.url;
        e.note = incoming.note.empty() ? e.note : incoming.note;
        found = true;
        ++updated;
        break;
      }
    }
    if (!found) {
      entries_.push_back(std::move(incoming));
      ++added;
    }
  }
  Save();
  PushState();
  ScheduleCloudPush();
  ShowToast(L"账号库已导入（DPAPI）：新增 " + std::to_wstring(added) +
            L"，更新 " + std::to_wstring(updated));
}

void VaultWindow::ScheduleCloudPush() {
  if (!VaultHttpLoggedIn()) {
    return;
  }
  if (!cloud_push_) {
    StartCloudPushWorker();
  }
  // Snapshot on the UI thread (entries_ is not touched off-thread).
  std::vector<VaultHttpCredential> snapshot = SnapshotCredentials();
  {
    std::lock_guard<std::mutex> lk(cloud_push_->mu);
    cloud_push_->pending = std::move(snapshot);
    cloud_push_->has_pending = true;
  }
  cloud_push_->cv.notify_one();
}

void VaultWindow::StartCloudPushWorker() {
  cloud_push_ = std::make_shared<CloudPushWorker>();
  auto st = cloud_push_;
  // Single-flight worker: serializes pushes; debounce coalesces rapid edits;
  // any change that arrives mid-push is relayed afterwards. Never touches
  // this / UI — holds only the shared state.
  std::thread([st]() {
    std::unique_lock<std::mutex> lk(st->mu);
    for (;;) {
      st->cv.wait(lk, [&] { return st->stopping || st->has_pending; });
      if (st->stopping) {
        break;
      }
      // Debounce: wait for a quiet window so rapid edits coalesce into one push.
      while (!st->stopping && st->has_pending) {
        if (st->cv.wait_for(lk, std::chrono::milliseconds(300)) ==
            std::cv_status::timeout) {
          break;
        }
      }
      if (st->stopping) {
        break;
      }
      if (!st->has_pending) {
        continue;
      }
      std::vector<VaultHttpCredential> snapshot = std::move(st->pending);
      st->has_pending = false;
      lk.unlock();
      VaultHttpPushLocalMirror(snapshot);
      lk.lock();
    }
  }).detach();
}

void VaultWindow::CloudLogin() {
  const std::wstring user =
      PromptText(hwnd_, L"云保险库", L"登录名：", L"");
  if (user.empty()) {
    if (on_login_result_) {
      auto cb = std::move(on_login_result_);
      on_login_result_ = nullptr;
      cb(false);
    }
    return;
  }
  const std::wstring pass = PromptPassword(hwnd_, L"云保险库", L"登录密码：");
  if (pass.empty()) {
    if (on_login_result_) {
      auto cb = std::move(on_login_result_);
      on_login_result_ = nullptr;
      cb(false);
    }
    return;
  }
  const HWND h = hwnd_;
  std::thread([user, pass, h]() {
    const bool ok = VaultHttpLogin(user, pass);
    PostMessageW(h, kMsgLoginDone, ok ? 1 : 0, 0);
  }).detach();
}

void VaultWindow::CloudRegister() {
  const std::wstring user =
      PromptText(hwnd_, L"云保险库注册", L"登录名：", L"");
  if (user.empty()) {
    return;
  }
  const std::wstring display =
      PromptText(hwnd_, L"云保险库注册", L"显示名称（可留空）：", L"");
  const std::wstring pass =
      PromptPassword(hwnd_, L"云保险库注册", L"登录密码：");
  if (pass.empty()) {
    return;
  }
  const std::wstring pass2 =
      PromptPassword(hwnd_, L"云保险库注册", L"确认密码：");
  if (pass != pass2) {
    MistAlert(hwnd_, L"云保险库注册", L"两次输入的密码不一致。",
              MistMsgTone::Warn);
    return;
  }
  const HWND h = hwnd_;
  std::thread([user, pass, display, h]() {
    std::wstring err;
    const bool ok = VaultHttpRegister(user, pass, display, &err);
    if (ok) {
      VaultHttpLogin(user, pass);
    }
    auto* outcome = new SyncOutcome;
    outcome->ok = ok;
    outcome->message = ok ? L"注册成功，已登录"
                          : (err.empty() ? L"注册失败" : err);
    if (!PostMessageW(h, kMsgSyncDone, 0, reinterpret_cast<LPARAM>(outcome))) {
      delete outcome;
    }
  }).detach();
}

void VaultWindow::StartCloudRefresh() {
  if (!VaultHttpLoggedIn()) {
    return;
  }
  const std::vector<VaultHttpCredential> snapshot = SnapshotCredentials();
  const HWND h = hwnd_;

  // P1-26: after restart the in-process master key is gone. If ZK KDF params
  // exist, unlock before merge so enc_ver=1 rows decrypt instead of landing as
  // ciphertext-as-plaintext.
  std::wstring salt_b64;
  int iterations = 0;
  std::wstring key_check;
  std::wstring algo;
  const bool zk_configured =
      VaultHttpGetKdfParams(&salt_b64, &iterations, &key_check, &algo);
  if (zk_configured && !VaultHttpHasMasterKey()) {
    const std::wstring master =
        PromptPassword(hwnd_, L"零知识保险库", L"主密码（云同步解密）：");
    if (master.empty()) {
      ShowToast(L"已跳过云同步：零知识库需主密码才能解密");
      return;
    }
    const std::vector<BYTE> salt = VaultDecodeSalt(NarrowUtf8(salt_b64));
    if (salt.empty()) {
      ShowToast(L"KDF 参数无效，已跳过云同步");
      return;
    }
    std::thread([snapshot, h, master, salt, iterations, key_check, algo]() {
      auto* outcome = new SyncOutcome;
      outcome->quiet = true;
      std::vector<BYTE> key;
      if (!VaultDeriveKeyForAlgo(master, salt, iterations, algo, &key)) {
        outcome->message = L"派生密钥失败";
        if (!PostMessageW(h, kMsgSyncDone, 0,
                          reinterpret_cast<LPARAM>(outcome))) {
          delete outcome;
        }
        return;
      }
      if (!VaultVerifyKeyCheck(key_check, key)) {
        outcome->message = L"主密码错误，已跳过云同步";
        if (!PostMessageW(h, kMsgSyncDone, 0,
                          reinterpret_cast<LPARAM>(outcome))) {
          delete outcome;
        }
        return;
      }
      VaultHttpSetMasterKey(key);
      std::vector<VaultHttpCredential> remote = snapshot;
      outcome->ok = VaultHttpPullMergeZk(&remote, key);
      outcome->have_entries = outcome->ok;
      outcome->entries = remote;
      outcome->message = L"同步失败，已离线（网络或服务端不可达）";
      if (!PostMessageW(h, kMsgSyncDone, 0, reinterpret_cast<LPARAM>(outcome))) {
        delete outcome;
      }
    }).detach();
    return;
  }

  std::thread([snapshot, h]() {
    auto* outcome = new SyncOutcome;
    outcome->quiet = true;
    std::vector<VaultHttpCredential> remote = snapshot;
    outcome->ok = VaultHttpPullMerge(&remote);
    outcome->have_entries = outcome->ok;
    outcome->entries = remote;
    outcome->message = L"同步失败，已离线（网络或服务端不可达）";
    if (!PostMessageW(h, kMsgSyncDone, 0, reinterpret_cast<LPARAM>(outcome))) {
      delete outcome;
    }
  }).detach();
}

void VaultWindow::CloudSync() {
  if (!VaultHttpLoggedIn()) {
    CloudLogin();
    return;
  }
  const MistMsgResult mode = MistMessage(
      hwnd_, Tr("vault").c_str(),
      L"云同步模式：\n\n是 = 零知识 (enc_ver=1，需主密码)\n否 = 服务端加密 (enc_ver=0)\n取消 = 返回",
      MistMsgKind::YesNoCancel, MistMsgTone::Info);
  if (mode == MistMsgResult::Cancel) {
    return;
  }
  const std::vector<VaultHttpCredential> snapshot = SnapshotCredentials();
  const HWND h = hwnd_;

  auto zk_worker = [snapshot, h](const std::wstring& master,
                                 const std::vector<BYTE>& salt, int iterations,
                                 const std::wstring& key_check,
                                 const std::wstring& algo, bool verify) {
    std::thread([snapshot, h, master, salt, iterations, key_check, algo,
                 verify]() {
      auto* outcome = new SyncOutcome;
      std::vector<BYTE> key;
      const std::wstring use_algo =
          verify ? algo
                 : (algo.empty() ? std::wstring(VaultPreferredKdfAlgo()) : algo);
      if (!VaultDeriveKeyForAlgo(master, salt, iterations, use_algo, &key)) {
        outcome->message = L"派生密钥失败";
        PostMessageW(h, kMsgSyncDone, 0, reinterpret_cast<LPARAM>(outcome));
        return;
      }
      if (verify && !VaultVerifyKeyCheck(key_check, key)) {
        outcome->message = L"主密码错误";
        PostMessageW(h, kMsgSyncDone, 0, reinterpret_cast<LPARAM>(outcome));
        return;
      }
      if (!verify) {
        const std::wstring check = VaultMakeKeyCheck(key);
        const std::wstring salt_b64 = WidenUtf8(VaultBytesToB64(salt));
        if (!VaultHttpEnsureMaster(salt_b64, iterations, check, use_algo)) {
          outcome->message = L"主密码初始化失败（服务端不可达）";
          PostMessageW(h, kMsgSyncDone, 0, reinterpret_cast<LPARAM>(outcome));
          return;
        }
        VaultHttpSetKdfParams(salt_b64, iterations, check, use_algo);
      }
      VaultHttpSetMasterKey(key);
      std::vector<VaultHttpCredential> remote = snapshot;
      bool ok = VaultHttpPullMergeZk(&remote, key);
      if (ok) {
        ok = VaultHttpPushAllZk(remote, key);
      }
      VaultHttpMigrateLegacyToE2e(key);
      outcome->ok = ok;
      outcome->have_entries = true;
      outcome->message =
          ok ? L"零知识云同步完成" : L"云同步失败（网络或服务端错误）";
      outcome->entries = remote;
      PostMessageW(h, kMsgSyncDone, 0, reinterpret_cast<LPARAM>(outcome));
    }).detach();
  };

  if (mode == MistMsgResult::Yes) {
    std::wstring salt_b64;
    int iterations = 0;
    std::wstring key_check;
    std::wstring algo;
    if (VaultHttpGetKdfParams(&salt_b64, &iterations, &key_check, &algo)) {
      const std::wstring master =
          PromptPassword(hwnd_, L"零知识保险库", L"主密码：");
      if (master.empty()) {
        return;
      }
      const std::vector<BYTE> salt = VaultDecodeSalt(NarrowUtf8(salt_b64));
      if (salt.empty()) {
        MistAlert(hwnd_, Tr("vault").c_str(), L"KDF 参数无效。",
                  MistMsgTone::Warn);
        return;
      }
      zk_worker(master, salt, iterations, key_check, algo, true);
    } else {
      const std::wstring master =
          PromptPassword(hwnd_, L"零知识保险库", L"设置主密码：");
      if (master.empty()) {
        return;
      }
      const std::wstring master2 =
          PromptPassword(hwnd_, L"零知识保险库", L"确认主密码：");
      if (master != master2) {
        MistAlert(hwnd_, Tr("vault").c_str(), L"两次输入的主密码不一致。",
                  MistMsgTone::Warn);
        return;
      }
      zk_worker(master, VaultNewSalt(16), kVaultKdfIterations, L"",
                VaultPreferredKdfAlgo(), false);
    }
    return;
  }

  std::thread([snapshot, h]() {
    auto* outcome = new SyncOutcome;
    std::vector<VaultHttpCredential> remote = snapshot;
    bool ok = VaultHttpPullMerge(&remote);
    if (ok) {
      ok = VaultHttpPushAll(remote);
    }
    outcome->ok = ok;
    outcome->have_entries = true;
    outcome->message = ok ? L"云同步完成" : L"云同步失败（网络或服务端错误）";
    outcome->entries = remote;
    PostMessageW(h, kMsgSyncDone, 0, reinterpret_cast<LPARAM>(outcome));
  }).detach();
}

LRESULT CALLBACK VaultWindow::HostProc(HWND hwnd, UINT msg, WPARAM wparam,
                                       LPARAM lparam) {
  auto* self =
      reinterpret_cast<VaultWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  if (self && msg == WM_SIZE) {
    self->ResizeWebView();
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK VaultWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
  VaultWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<VaultWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<VaultWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT VaultWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_APP + 70: {
      while (!pending_action_jsons_.empty()) {
        const std::wstring json = std::move(pending_action_jsons_.front());
        pending_action_jsons_.erase(pending_action_jsons_.begin());
        if (!json.empty()) {
          DispatchAction(json);
        }
      }
      return 0;
    }
    case kMsgLoginDone: {
      const bool ok = wparam != 0;
      if (ok) {
        ShowToast(L"云登录成功");
        // DeskNote may have parked cloud-note uploads while logged out.
        NoteCloudNotifyPushWorker();
        StartCloudRefresh();
      } else {
        MistAlert(hwnd_, Tr("vault").c_str(),
                  L"登录失败（用户名或密码错误，或服务端不可达）。",
                  MistMsgTone::Warn);
      }
      PushState();
      if (on_login_result_) {
        auto cb = std::move(on_login_result_);
        on_login_result_ = nullptr;
        cb(ok);
      }
      return 0;
    }
    case kMsgSyncDone: {
      std::unique_ptr<SyncOutcome> outcome(
          reinterpret_cast<SyncOutcome*>(lparam));
      if (outcome) {
        if (outcome->have_entries) {
          ApplyPulledCredentials(outcome->entries);
        }
        if (outcome->ok) {
          if (!outcome->quiet) {
            ShowToast(outcome->message.c_str());
          }
        } else if (outcome->quiet) {
          // Background refresh: non-intrusive offline marker, never block UI.
          ShowToast(outcome->message.empty() ? L"同步失败，已离线"
                                             : outcome->message.c_str());
        } else {
          MistAlert(hwnd_, Tr("vault").c_str(), outcome->message.c_str(),
                    MistMsgTone::Warn);
        }
        PushState();
      }
      return 0;
    }
    case WM_SIZE:
      ResizeWebView();
      return 0;
    case WM_TIMER:
      if (wparam == kClipTimer) {
        ClearClipboardIfOurs();
        return 0;
      }
      break;
    case WM_ACTIVATE: {
      if (destroying_) {
        break;  // DestroyWindow re-sends WA_INACTIVE — do not re-Hide/Save.
      }
      // 点击浮窗外部（桌面/其他窗口）自动收起。激活若转到本窗口 owned 的
      // 模态对话框（MistAlert/PromptPassword 等，owner 链含 hwnd_）则保持打开。
      if (LOWORD(wparam) == WA_INACTIVE) {
        HWND next = reinterpret_cast<HWND>(lparam);
        HWND owner = next;
        while (owner) {
          if (owner == hwnd_) {
            return 0;
          }
          owner = GetWindow(owner, GW_OWNER);
        }
        Hide();
        return 0;
      }
      break;
    }
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
