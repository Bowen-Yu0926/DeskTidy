#include "notes_webview.h"

#include "paths_util.h"

#include <objbase.h>
#ifndef interface
#define interface struct
#endif
#include <WebView2.h>
#include <wrl.h>

#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

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
        o.push_back(c);
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
      if (json[p] == L'n') {
        v.push_back(L'\n');
      } else if (json[p] == L'r') {
        v.push_back(L'\r');
      } else if (json[p] == L't') {
        v.push_back(L'\t');
      } else {
        v.push_back(json[p]);
      }
    } else {
      v.push_back(json[p]);
    }
    ++p;
  }
  return v;
}

bool PathExists(const std::wstring& path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES;
}

std::wstring FindWysiwygHtml() {
  wchar_t mod[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, mod, MAX_PATH);
  std::wstring dir = mod;
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    dir.resize(slash);
  }
  const std::wstring candidates[] = {
      dir + L"\\assets\\md_preview\\wysiwyg_native.html",
      dir + L"\\..\\assets\\md_preview\\wysiwyg_native.html",
      dir + L"\\..\\..\\assets\\md_preview\\wysiwyg_native.html",
      JoinPath(NativeDataDir(), L"md_preview\\wysiwyg_native.html"),
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

bool NotesWebView::Create(HWND parent) {
  parent_ = parent;
  if (!parent_) {
    return false;
  }
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = NotesWebView::HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DeskTidyNativeNotesWeb";
    RegisterClassExW(&wc);
    registered = true;
  }
  RECT rc{};
  GetClientRect(parent_, &rc);
  host_ = CreateWindowExW(0, L"DeskTidyNativeNotesWeb", L"", WS_CHILD, 0, 0,
                          rc.right, rc.bottom, parent_, nullptr,
                          GetModuleHandleW(nullptr), this);
  if (!host_) {
    return false;
  }
  SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

  const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) {
              return S_OK;
            }
            env->CreateCoreWebView2Controller(
                host_,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
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
                      available_ = true;
                      RECT bounds{};
                      GetClientRect(host_, &bounds);
                      controller->put_Bounds(bounds);
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
                      NavigateHome();
                      return S_OK;
                    })
                    .Get());
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    DestroyWindow(host_);
    host_ = nullptr;
    return false;
  }
  return true;
}

void NotesWebView::NavigateHome() {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web) {
    return;
  }
  const std::wstring html = FindWysiwygHtml();
  if (html.empty()) {
    return;
  }
  web->Navigate(FileUrlFromPath(html).c_str());
}

void NotesWebView::OnReady() {
  ready_ = true;
  if (!pending_set_.empty()) {
    SetMarkdown(pending_set_);
    pending_set_.clear();
  }
}

void NotesWebView::OnWebMessage(const std::wstring& json) {
  const std::wstring type = JsonStringField(json, L"type");
  if (type == L"ready") {
    OnReady();
  } else if (type == L"md" || type == L"changed") {
    pending_md_ = JsonStringField(json, L"md");
    got_md_ = true;
  }
}

void NotesWebView::Destroy() {
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
  available_ = false;
  ready_ = false;
}

void NotesWebView::Show() {
  if (host_) {
    ShowWindow(host_, SW_SHOW);
    Resize();
  }
}

void NotesWebView::Hide() {
  if (host_) {
    ShowWindow(host_, SW_HIDE);
  }
}

void NotesWebView::Resize() {
  if (!host_ || !parent_) {
    return;
  }
  RECT rc{};
  GetClientRect(parent_, &rc);
  MoveWindow(host_, 0, 0, rc.right, rc.bottom, TRUE);
  if (controller_) {
    RECT bounds{0, 0, rc.right, rc.bottom};
    static_cast<ICoreWebView2Controller*>(controller_)->put_Bounds(bounds);
  }
}

void NotesWebView::SetMarkdown(const std::wstring& md) {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web || !ready_) {
    pending_set_ = md;
    return;
  }
  const std::wstring json = L"{\"type\":\"set\",\"theme\":\"light\",\"md\":\"" +
                            JsonEscape(md) + L"\"}";
  web->PostWebMessageAsJson(json.c_str());
}

std::wstring NotesWebView::PullMarkdown(DWORD timeout_ms) {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web || !ready_) {
    return pending_md_;
  }
  got_md_ = false;
  web->PostWebMessageAsJson(L"{\"type\":\"get\"}");
  const DWORD start = GetTickCount();
  MSG msg{};
  while (!got_md_ && GetTickCount() - start < timeout_ms) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(5);
  }
  return pending_md_;
}

LRESULT CALLBACK NotesWebView::HostProc(HWND hwnd, UINT msg, WPARAM wparam,
                                        LPARAM lparam) {
  NotesWebView* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<NotesWebView*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self) {
      self->host_ = hwnd;
    }
  } else {
    self =
        reinterpret_cast<NotesWebView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleHost(msg, wparam, lparam);
}

LRESULT NotesWebView::HandleHost(UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_SIZE) {
    Resize();
    return 0;
  }
  if (msg == WM_DESTROY) {
    host_ = nullptr;
    return 0;
  }
  return DefWindowProcW(host_, msg, wparam, lparam);
}
