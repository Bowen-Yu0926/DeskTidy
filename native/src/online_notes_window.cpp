#include "online_notes_window.h"

#include "display_geom.h"
#include "mist_msg.h"
#include "notes_cloud_sync.h"
#include "notes_library.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "vault_http.h"

#include <fstream>

#include <objbase.h>
#ifndef interface
#define interface struct
#endif
#include <WebView2.h>
#include <wrl.h>

#include <Shellapi.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;

namespace {

constexpr wchar_t kClass[] = L"DeskNoteOnlineNotes";
constexpr wchar_t kHostClass[] = L"DeskNoteOnlineNotesWeb";
constexpr int kPanelW = 960;
constexpr int kPanelH = 640;

std::wstring JsonEscape(const std::wstring& s) {
  std::wstring o;
  o.reserve(s.size() + 8);
  for (wchar_t c : s) {
    if (c == L'\\' || c == L'"') {
      o.push_back(L'\\');
      o.push_back(c);
    } else if (c == L'\n') {
      o += L"\\n";
    } else if (c == L'\r') {
      o += L"\\r";
    } else if (c == L'\t') {
      o += L"\\t";
    } else {
      o.push_back(c);
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
  ++p;
  while (p < json.size() &&
         (json[p] == L' ' || json[p] == L'\t' || json[p] == L'\r' ||
          json[p] == L'\n')) {
    ++p;
  }
  if (p >= json.size() || json[p] != L'"') {
    return {};
  }
  ++p;
  std::wstring v;
  while (p < json.size() && json[p] != L'"') {
    if (json[p] == L'\\' && p + 1 < json.size()) {
      ++p;
      const wchar_t e = json[p++];
      if (e == L'n') {
        v.push_back(L'\n');
      } else if (e == L'r') {
        v.push_back(L'\r');
      } else if (e == L't') {
        v.push_back(L'\t');
      } else {
        v.push_back(e);
      }
      continue;
    }
    v.push_back(json[p++]);
  }
  return v;
}

int JsonIntField(const std::wstring& json, const wchar_t* key) {
  const std::wstring needle = std::wstring(L"\"") + key + L"\"";
  size_t p = json.find(needle);
  if (p == std::wstring::npos) {
    return 0;
  }
  p = json.find(L':', p + needle.size());
  if (p == std::wstring::npos) {
    return 0;
  }
  ++p;
  while (p < json.size() &&
         (json[p] == L' ' || json[p] == L'\t' || json[p] == L'\r' ||
          json[p] == L'\n')) {
    ++p;
  }
  return _wtoi(json.c_str() + p);
}

std::wstring FileUrlFromPath(const std::wstring& path) {
  std::wstring p = path;
  for (auto& c : p) {
    if (c == L'\\') {
      c = L'/';
    }
  }
  if (p.size() >= 2 && p[1] == L':') {
    return L"file:///" + p;
  }
  return L"file://" + p;
}

std::wstring SanitizeFileStem(std::wstring title) {
  if (title.empty()) {
    title = L"未命名";
  }
  for (wchar_t& c : title) {
    if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
        c == L'"' || c == L'<' || c == L'>' || c == L'|') {
      c = L'_';
    }
  }
  while (!title.empty() && (title.back() == L' ' || title.back() == L'.')) {
    title.pop_back();
  }
  if (title.empty()) {
    title = L"未命名";
  }
  if (title.size() > 80) {
    title.resize(80);
  }
  return title;
}

std::wstring UniqueCloudNotePath(const std::wstring& stem) {
  const std::wstring root = NotesLibraryDir(true);
  const std::wstring dir = JoinPath(root, L"云笔记");
  CreateDirectoryW(dir.c_str(), nullptr);
  std::wstring path = JoinPath(dir, stem + L".md");
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return path;
  }
  for (int n = 1; n < 1000; ++n) {
    wchar_t suffix[32];
    swprintf_s(suffix, L"_%02d.md", n);
    path = JoinPath(dir, stem + suffix);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return path;
    }
  }
  return JoinPath(dir, stem + L"_copy.md");
}

std::wstring FindOnlineNotesHtml() {
  wchar_t mod[MAX_PATH]{};
  GetModuleFileNameW(nullptr, mod, MAX_PATH);
  std::wstring dir = mod;
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    dir = dir.substr(0, slash);
  }
  const std::wstring cands[] = {
      dir + L"\\assets\\online_notes\\online_notes.html",
      dir + L"\\..\\assets\\online_notes\\online_notes.html",
      dir + L"\\..\\..\\assets\\online_notes\\online_notes.html",
  };
  for (const auto& c : cands) {
    if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return c;
    }
  }
  return {};
}

struct PassCtx {
  HWND owner = nullptr;
  std::wstring label;
  std::wstring value;
  bool ok = false;
  bool done = false;
};

void OnlinePwFinish(HWND hwnd, PassCtx* ctx, bool ok) {
  if (!ctx) {
    DestroyWindow(hwnd);
    return;
  }
  if (ok) {
    wchar_t buf[512]{};
    GetWindowTextW(GetDlgItem(hwnd, 1), buf, 512);
    ctx->value = buf;
    ctx->ok = true;
  } else {
    ctx->ok = false;
    ctx->value.clear();
  }
  DestroyWindow(hwnd);
}

LRESULT CALLBACK OnlinePwProc(HWND hwnd, UINT msg, WPARAM wparam,
                              LPARAM lparam) {
  PassCtx* ctx =
      reinterpret_cast<PassCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_CREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
      ctx = static_cast<PassCtx*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
      CreateWindowExW(0, L"STATIC", ctx->label.c_str(),
                      WS_CHILD | WS_VISIBLE, 16, 16, 360, 20, hwnd, nullptr,
                      GetModuleHandleW(nullptr), nullptr);
      CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD |
                          ES_AUTOHSCROLL,
                      16, 42, 370, 26, hwnd, reinterpret_cast<HMENU>(1),
                      GetModuleHandleW(nullptr), nullptr);
      CreateWindowExW(0, L"BUTTON", L"确定",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 220,
                      84, 80, 28, hwnd, reinterpret_cast<HMENU>(IDOK),
                      GetModuleHandleW(nullptr), nullptr);
      CreateWindowExW(0, L"BUTTON", L"取消",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP, 308, 84, 80, 28, hwnd,
                      reinterpret_cast<HMENU>(IDCANCEL),
                      GetModuleHandleW(nullptr), nullptr);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == IDOK) {
        OnlinePwFinish(hwnd, ctx, true);
        return 0;
      }
      if (LOWORD(wparam) == IDCANCEL) {
        OnlinePwFinish(hwnd, ctx, false);
        return 0;
      }
      break;
    case WM_CLOSE:
      OnlinePwFinish(hwnd, ctx, false);
      return 0;
    case WM_DESTROY:
      if (ctx) {
        ctx->done = true;
        if (ctx->owner) {
          EnableWindow(ctx->owner, TRUE);
        }
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

std::wstring PromptPassword(HWND owner, const wchar_t* title,
                            const wchar_t* label) {
  static bool registered = false;
  constexpr wchar_t kPwClass[] = L"DeskNoteOnlineNotesPwd";
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OnlinePwProc;
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
  RECT anchor{};
  if (ctx.owner) {
    GetWindowRect(ctx.owner, &anchor);
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
  }
  const int width = 420;
  const int height = 158;
  const int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
  const int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 3;
  // Owned popup (not child of WebView host) — avoids nested Mist/WebView2.
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
  MSG msg{};
  while (!ctx.done) {
    const BOOL gm = GetMessageW(&msg, nullptr, 0, 0);
    if (gm == 0) {
      // Preserve app quit for the outer DeskNote loop.
      PostQuitMessage(static_cast<int>(msg.wParam));
      break;
    }
    if (gm < 0) {
      break;
    }
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (ctx.owner && IsWindow(ctx.owner)) {
    EnableWindow(ctx.owner, TRUE);
    SetForegroundWindow(ctx.owner);
  }
  return ctx.ok ? ctx.value : L"";
}

}  // namespace

bool OnlineNotesWindow::Create(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = OnlineNotesWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(0xEE, 0xF1, 0xF6));
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  hwnd_ = CreateWindowExW(
      WS_EX_APPWINDOW, kClass, L"在线笔记",
      WS_POPUP | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, kPanelW,
      kPanelH, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void OnlineNotesWindow::Destroy() {
  DestroyWebView();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void OnlineNotesWindow::Show() {
  if (!hwnd_) {
    return;
  }
  if (!web_available_) {
    CreateWebView();
  }
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
  if (VaultHttpLoggedIn()) {
    RefreshList();
  } else {
    status_ = L"请先登录云账号（与账号库相同）";
    PushState();
  }
}

void OnlineNotesWindow::Hide() {
  if (!hwnd_) {
    return;
  }
  const bool was = IsWindowVisible(hwnd_) != FALSE;
  ShowWindow(hwnd_, SW_HIDE);
  if (was && on_hide_) {
    on_hide_();
  }
}

void OnlineNotesWindow::PlaceBeside(HWND anchor) {
  if (!hwnd_) {
    return;
  }
  RECT work = PrimaryWorkArea();
  int x = work.right - kPanelW - 48;
  int y = work.top + 72;
  if (anchor && IsWindow(anchor)) {
    RECT ar{};
    GetWindowRect(anchor, &ar);
    x = ar.left - kPanelW - 12;
    y = ar.top;
  }
  if (x < work.left + 8) {
    x = work.left + 8;
  }
  if (y < work.top + 8) {
    y = work.top + 8;
  }
  SetWindowPos(hwnd_, HWND_TOP, x, y, kPanelW, kPanelH,
               SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

bool OnlineNotesWindow::CreateWebView() {
  if (web_available_ || controller_ || !hwnd_) {
    return web_available_;
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OnlineNotesWindow::HostProc;
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

  const std::wstring user_data =
      JoinPath(NativeDataDir(), L"webview2-online-notes");
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

void OnlineNotesWindow::DestroyWebView() {
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

void OnlineNotesWindow::ResizeWebView() {
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

void OnlineNotesWindow::NavigateHome() {
  if (!webview_) {
    return;
  }
  const std::wstring html = FindOnlineNotesHtml();
  if (html.empty()) {
    return;
  }
  static_cast<ICoreWebView2*>(webview_)->Navigate(FileUrlFromPath(html).c_str());
}

void OnlineNotesWindow::OnWebReady() {
  web_ready_ = true;
  PushState();
}

void OnlineNotesWindow::PushState() {
  if (!webview_ || !web_ready_) {
    return;
  }
  std::wstring json = L"{\"type\":\"state\",\"logged_in\":";
  json += VaultHttpLoggedIn() ? L"true" : L"false";
  json += L",\"session\":\"";
  json += JsonEscape(VaultHttpSessionLogin());
  json += L"\",\"status\":\"";
  json += JsonEscape(status_);
  json += L"\",\"current_id\":";
  json += std::to_wstring(current_id_);
  json += L",\"title\":\"";
  json += JsonEscape(current_title_);
  json += L"\",\"body\":\"";
  json += JsonEscape(current_body_);
  json += L"\",\"items\":[";
  for (size_t i = 0; i < notes_.size(); ++i) {
    if (i) {
      json += L',';
    }
    const auto& n = notes_[i];
    json += L"{\"id\":";
    json += std::to_wstring(n.id);
    json += L",\"title\":\"";
    json += JsonEscape(n.title);
    json += L"\",\"updated_at\":\"";
    json += JsonEscape(n.updated_at);
    json += L"\"}";
  }
  json += L"]}";

  auto* web = static_cast<ICoreWebView2*>(webview_);
  std::wstring script =
      L"(function(){try{var m=" + json +
      L";if(typeof window.applyState==='function'){window.applyState(m);}"
      L"}catch(e){}})();";
  web->ExecuteScript(script.c_str(), nullptr);
}

void OnlineNotesWindow::OnWebMessage(const std::wstring& json) {
  const std::wstring type = JsonStringField(json, L"type");
  if (type == L"ready") {
    OnWebReady();
    return;
  }
  if (type != L"action") {
    return;
  }
  const std::wstring id = JsonStringField(json, L"id");
  if (id == L"drag_begin" || id == L"drag_move" || id == L"drag_end") {
    DispatchAction(json);
    return;
  }
  if (hwnd_) {
    pending_action_jsons_.push_back(json);
    PostMessageW(hwnd_, WM_APP + 80, 0, 0);
  } else {
    DispatchAction(json);
  }
}

void OnlineNotesWindow::DispatchAction(const std::wstring& json) {
  const std::wstring id = JsonStringField(json, L"id");
  if (id == L"hide") {
    Hide();
  } else if (id == L"refresh") {
    RefreshList();
  } else if (id == L"add") {
    CreateNote();
  } else if (id == L"save") {
    current_title_ = JsonStringField(json, L"title");
    current_body_ = JsonStringField(json, L"body");
    SaveCurrent();
  } else if (id == L"to_local") {
    ConvertToLocal(JsonStringField(json, L"title"),
                   JsonStringField(json, L"body"));
  } else if (id == L"delete") {
    DeleteCurrent();
  } else if (id == L"open") {
    OpenNote(JsonIntField(json, L"note_id"));
  } else if (id == L"auth") {
    if (VaultHttpLoggedIn()) {
      VaultHttpLogout();
      notes_.clear();
      current_id_ = 0;
      current_title_.clear();
      current_body_.clear();
      status_ = L"已退出登录";
      PushState();
    } else {
      EnsureLogin();
    }
  } else if (id == L"drag_begin") {
    dragging_ = true;
    drag_last_.x = JsonIntField(json, L"x");
    drag_last_.y = JsonIntField(json, L"y");
  } else if (id == L"drag_move" && dragging_ && hwnd_) {
    const int x = JsonIntField(json, L"x");
    const int y = JsonIntField(json, L"y");
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    const int dx = x - drag_last_.x;
    const int dy = y - drag_last_.y;
    SetWindowPos(hwnd_, nullptr, rc.left + dx, rc.top + dy, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
    drag_last_.x = x;
    drag_last_.y = y;
  } else if (id == L"drag_end") {
    dragging_ = false;
  }
}

void OnlineNotesWindow::EnsureLogin() {
  if (VaultHttpLoggedIn()) {
    RefreshList();
    return;
  }
  // Win32 prompts only — Mist/WebView2 prompts nested with this panel's
  // WebView2 bring-up have been observed to tear down DeskNote (闪退).
  const std::wstring login =
      PromptTextWin32(hwnd_, L"在线笔记", L"云账号登录名：", L"");
  if (login.empty()) {
    status_ = L"已取消登录";
    PushState();
    return;
  }
  const std::wstring pass =
      PromptPassword(hwnd_, L"在线笔记", L"登录密码：");
  if (pass.empty()) {
    status_ = L"已取消登录";
    PushState();
    return;
  }
  if (!VaultHttpLogin(login, pass)) {
    MistAlert(hwnd_, L"在线笔记",
              L"登录失败（用户名或密码错误，或服务端不可达）。",
              MistMsgTone::Danger);
    status_ = L"登录失败";
    PushState();
    return;
  }
  status_ = L"登录成功";
  NoteCloudNotifyPushWorker();  // flush batches parked while logged out
  RefreshList();
}

void OnlineNotesWindow::RefreshList() {
  if (!VaultHttpLoggedIn()) {
    status_ = L"请先登录云账号";
    PushState();
    return;
  }
  std::vector<VaultHttpNote> remote;
  if (!VaultHttpListNotes(&remote)) {
    status_ = L"拉取失败（请检查网络或稍后重试）";
    PushState();
    return;
  }
  notes_ = std::move(remote);
  status_ = L"共 " + std::to_wstring(notes_.size()) + L" 条在线笔记";
  if (current_id_ > 0) {
    bool still = false;
    for (const auto& n : notes_) {
      if (n.id == current_id_) {
        still = true;
        break;
      }
    }
    if (!still) {
      current_id_ = 0;
      current_title_.clear();
      current_body_.clear();
    }
  }
  PushState();
}

void OnlineNotesWindow::OpenNote(int id) {
  if (id <= 0) {
    return;
  }
  if (!VaultHttpLoggedIn()) {
    EnsureLogin();
    if (!VaultHttpLoggedIn()) {
      return;
    }
  }
  VaultHttpNote n;
  if (!VaultHttpGetNote(id, &n)) {
    status_ = L"打开失败";
    PushState();
    return;
  }
  current_id_ = n.id;
  current_title_ = n.title;
  current_body_ = n.body;
  status_ = L"已打开 · " + n.updated_at;
  PushState();
}

void OnlineNotesWindow::CreateNote() {
  if (!VaultHttpLoggedIn()) {
    EnsureLogin();
    if (!VaultHttpLoggedIn()) {
      return;
    }
  }
  VaultHttpNote created;
  if (!VaultHttpCreateNote(L"未命名", L"", &created) || created.id <= 0) {
    MistAlert(hwnd_, L"在线笔记", L"新建失败（服务端可能尚未升级 notes 接口）。",
              MistMsgTone::Danger);
    status_ = L"新建失败";
    PushState();
    return;
  }
  current_id_ = created.id;
  current_title_ = created.title.empty() ? L"未命名" : created.title;
  current_body_ = created.body;
  RefreshList();
  status_ = L"已新建并同步";
  PushState();
}

void OnlineNotesWindow::SaveCurrent() {
  if (current_id_ <= 0) {
    status_ = L"请先新建或打开一条笔记";
    PushState();
    return;
  }
  if (!VaultHttpLoggedIn()) {
    EnsureLogin();
    if (!VaultHttpLoggedIn()) {
      return;
    }
  }
  if (current_title_.empty()) {
    current_title_ = L"未命名";
  }
  VaultHttpNote updated;
  if (!VaultHttpUpdateNote(current_id_, current_title_, current_body_,
                           &updated)) {
    MistAlert(hwnd_, L"在线笔记", L"保存失败。", MistMsgTone::Danger);
    status_ = L"保存失败";
    PushState();
    return;
  }
  for (auto& n : notes_) {
    if (n.id == current_id_) {
      n.title = current_title_;
      n.updated_at = updated.updated_at;
      break;
    }
  }
  status_ = L"已保存并同步";
  PushState();
}

void OnlineNotesWindow::ShowNoteId(int id) {
  Show();
  if (id > 0) {
    OpenNote(id);
  }
}

void OnlineNotesWindow::ShowAndCreateNew() {
  // Show first (kicks off WebView2). Defer login/create so we never nest
  // modal prompts inside CreateCoreWebView2* callbacks on the same stack.
  Show();
  if (hwnd_) {
    PostMessageW(hwnd_, WM_APP + 81, 1, 0);
  }
}

void OnlineNotesWindow::ConvertToLocal(const std::wstring& title,
                                       const std::wstring& body) {
  std::wstring use_title = title.empty() ? current_title_ : title;
  std::wstring use_body = body;
  if (use_body.empty() && !current_body_.empty()) {
    use_body = current_body_;
  }
  if (current_id_ <= 0 && use_body.empty() && use_title.empty()) {
    status_ = L"请先打开或新建一条在线笔记";
    PushState();
    return;
  }
  if (use_title.empty()) {
    use_title = L"未命名";
  }
  // Keep editor fields in sync before landing locally.
  current_title_ = use_title;
  current_body_ = use_body;
  if (current_id_ > 0) {
    VaultHttpUpdateNote(current_id_, current_title_, current_body_, nullptr);
  }
  const std::wstring path = UniqueCloudNotePath(SanitizeFileStem(use_title));
  {
    std::string utf8;
    // Narrow via Win32 for Unicode titles/bodies.
    const int n = WideCharToMultiByte(CP_UTF8, 0, use_body.c_str(), -1, nullptr,
                                      0, nullptr, nullptr);
    if (n > 1) {
      utf8.assign(static_cast<size_t>(n - 1), '\0');
      WideCharToMultiByte(CP_UTF8, 0, use_body.c_str(), -1, utf8.data(), n,
                          nullptr, nullptr);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
      MistAlert(hwnd_, L"在线笔记", L"无法写入本地文件。", MistMsgTone::Danger);
      status_ = L"转本地失败";
      PushState();
      return;
    }
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    out.write(reinterpret_cast<const char*>(bom), 3);
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
  }
  InvalidateNotesFileCache();
  status_ = L"已转到本地「云笔记」文件夹";
  PushState();
  if (on_open_local_) {
    on_open_local_(path);
  }
}

void OnlineNotesWindow::DeleteCurrent() {
  if (current_id_ <= 0) {
    status_ = L"没有可删除的笔记";
    PushState();
    return;
  }
  if (!MistConfirm(hwnd_, L"在线笔记", L"确定删除当前在线笔记？",
                   MistMsgTone::Warn)) {
    return;
  }
  if (!VaultHttpDeleteNote(current_id_)) {
    MistAlert(hwnd_, L"在线笔记", L"删除失败。", MistMsgTone::Danger);
    status_ = L"删除失败";
    PushState();
    return;
  }
  current_id_ = 0;
  current_title_.clear();
  current_body_.clear();
  RefreshList();
  status_ = L"已删除";
  PushState();
}

LRESULT CALLBACK OnlineNotesWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                            LPARAM lparam) {
  OnlineNotesWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<OnlineNotesWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self) {
      self->hwnd_ = hwnd;
    }
  } else {
    self = reinterpret_cast<OnlineNotesWindow*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT CALLBACK OnlineNotesWindow::HostProc(HWND hwnd, UINT msg, WPARAM wparam,
                                             LPARAM lparam) {
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT OnlineNotesWindow::HandleMessage(UINT msg, WPARAM wparam,
                                         LPARAM lparam) {
  const HWND hwnd = hwnd_;
  switch (msg) {
    case WM_SIZE:
      ResizeWebView();
      return 0;
    case WM_CLOSE:
      Hide();
      return 0;
    case WM_APP + 80:
      while (!pending_action_jsons_.empty()) {
        const std::wstring j = pending_action_jsons_.front();
        pending_action_jsons_.erase(pending_action_jsons_.begin());
        DispatchAction(j);
      }
      return 0;
    case WM_APP + 81:
      // Deferred from ShowAndCreateNew / open-login: safe after Show returns.
      if (!VaultHttpLoggedIn()) {
        EnsureLogin();
      }
      if (wparam && VaultHttpLoggedIn()) {
        CreateNote();
      }
      return 0;
    case WM_DESTROY:
      DestroyWebView();
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd ? hwnd : hwnd_, msg, wparam, lparam);
}
