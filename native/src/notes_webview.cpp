#include "notes_webview.h"

#include "paths_util.h"

#include <objbase.h>
#include <Shellapi.h>
#ifndef interface
#define interface struct
#endif
#include <WebView2.h>
#include <wrl.h>

#include <algorithm>
#include <string>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

int HexNibble(wchar_t c) {
  if (c >= L'0' && c <= L'9') {
    return c - L'0';
  }
  if (c >= L'a' && c <= L'f') {
    return c - L'a' + 10;
  }
  if (c >= L'A' && c <= L'F') {
    return c - L'A' + 10;
  }
  return -1;
}

std::wstring UrlDecodePath(const std::wstring& in) {
  std::wstring out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == L'%' && i + 2 < in.size()) {
      const int hi = HexNibble(in[i + 1]);
      const int lo = HexNibble(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<wchar_t>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    if (in[i] == L'+') {
      out.push_back(L' ');
      continue;
    }
    out.push_back(in[i]);
  }
  return out;
}

// file:///C:/a/b.md → C:\a\b.md (strips ?query / #frag).
std::wstring PathFromFileUri(const std::wstring& uri) {
  if (uri.size() < 7 || _wcsnicmp(uri.c_str(), L"file:", 5) != 0) {
    return {};
  }
  size_t start = 5;
  while (start < uri.size() && uri[start] == L'/') {
    ++start;
  }
  // file://localhost/C:/... or file:///C:/...
  if (start < uri.size() && _wcsnicmp(uri.c_str() + start, L"localhost/", 10) == 0) {
    start += 10;
  }
  std::wstring path = uri.substr(start);
  const size_t cut = path.find_first_of(L"?#");
  if (cut != std::wstring::npos) {
    path.resize(cut);
  }
  path = UrlDecodePath(path);
  for (auto& ch : path) {
    if (ch == L'/') {
      ch = L'\\';
    }
  }
  return path;
}

bool IsDeskNoteShellHtmlUri(const std::wstring& uri) {
  std::wstring lower = uri;
  for (auto& ch : lower) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  // file://… or https://app.desknote/… (virtual host mapping).
  if (lower.find(L"https://app.desknote/") == 0 ||
      lower.find(L"http://app.desknote/") == 0) {
    return true;
  }
  return lower.find(L"wysiwyg_native.html") != std::wstring::npos ||
         lower.find(L"viewer_native.html") != std::wstring::npos;
}

std::wstring ParentDir(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return {};
  }
  return path.substr(0, slash);
}

// Bust only when the shell HTML on disk changes — keep Chromium disk cache warm.
std::wstring AssetCacheToken(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
    return L"0";
  }
  ULARGE_INTEGER u{};
  u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
  u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
  wchar_t buf[32];
  swprintf_s(buf, L"%llX", static_cast<unsigned long long>(u.QuadPart));
  return buf;
}

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

double JsonDoubleField(const std::wstring& json, const wchar_t* key,
                       double fallback) {
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
  return _wtof(json.c_str() + p);
}

bool PathExists(const std::wstring& path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES;
}

std::wstring FindAsset(const std::wstring& rel) {
  wchar_t mod[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, mod, MAX_PATH);
  std::wstring dir = mod;
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    dir.resize(slash);
  }
  const std::wstring candidates[] = {
      dir + L"\\assets\\" + rel,
      dir + L"\\..\\assets\\" + rel,
      dir + L"\\..\\..\\assets\\" + rel,
      JoinPath(NativeDataDir(), rel),
  };
  for (const auto& c : candidates) {
    if (PathExists(c)) {
      return c;
    }
  }
  return {};
}

std::wstring FindWysiwygHtml() { return FindAsset(L"md_preview\\wysiwyg_native.html"); }
std::wstring FindViewerHtml() { return FindAsset(L"md_preview\\viewer_native.html"); }

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

void NotesWarmWebViewEnv() {
  static bool started = false;
  if (started) {
    return;
  }
  started = true;
  static ComPtr<ICoreWebView2Environment> env_keep;
  const std::wstring udir =
      JoinPath(NativeDataDir(), L"webview2-desknote");
  CreateDirectoryW(NativeDataDir().c_str(), nullptr);
  CreateDirectoryW(udir.c_str(), nullptr);
  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, udir.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (SUCCEEDED(result) && env) {
              env_keep = env;
            }
            return S_OK;
          })
          .Get());
}

bool NotesWebView::Create(HWND parent) {
  parent_ = parent;
  if (!parent_) {
    return false;
  }
  // Idempotent: OpenPaths/Present can re-enter ShowPreviewMode while the
  // WebView2 environment callback is still pending — never create a second host.
  if (host_) {
    return true;
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
  host_ = CreateWindowExW(0, L"DeskTidyNativeNotesWeb", L"",
                          WS_CHILD | WS_CLIPSIBLINGS, 0, 0, rc.right, rc.bottom,
                          parent_, nullptr, GetModuleHandleW(nullptr), this);
  if (!host_) {
    return false;
  }
  SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

  // Persistent profile (like Mist dialogs): second launch skips full Edge cold start.
  const std::wstring udir =
      JoinPath(NativeDataDir(), L"webview2-desknote");
  CreateDirectoryW(NativeDataDir().c_str(), nullptr);
  CreateDirectoryW(udir.c_str(), nullptr);

  const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, udir.c_str(), nullptr,
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
                      // Allow Explorer drops onto the WebView surface; we cancel
                      // the resulting file:// navigation in NavigationStarting
                      // and open/insert via on_external_drop (OLE on parent
                      // alone misses the Chromium child HWND under the cursor).
                      {
                        ComPtr<ICoreWebView2Controller4> c4;
                        if (SUCCEEDED(controller->QueryInterface(
                                IID_PPV_ARGS(&c4))) &&
                            c4) {
                          c4->put_AllowExternalDrop(TRUE);
                        }
                      }
                      // Own find/replace (DeskNote) — Chromium's bottom find
                      // bar was a dead-end UX and TOPMOST find churn could
                      // bury DeskTidy fences under DefView until page switch.
                      {
                        EventRegistrationToken accel_tok{};
                        controller->add_AcceleratorKeyPressed(
                            Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
                                [this](ICoreWebView2Controller* ctrl,
                                       ICoreWebView2AcceleratorKeyPressedEventArgs*
                                           args) -> HRESULT {
                                  if (!args) {
                                    return S_OK;
                                  }
                                  COREWEBVIEW2_KEY_EVENT_KIND kind =
                                      COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
                                  args->get_KeyEventKind(&kind);
                                  if (kind !=
                                          COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                                      kind !=
                                          COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
                                    return S_OK;
                                  }
                                  UINT key = 0;
                                  args->get_VirtualKey(&key);
                                  COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
                                  args->get_PhysicalKeyStatus(&status);
                                  if (status.WasKeyDown) {
                                    return S_OK;
                                  }
                                  const bool ctrl_down =
                                      (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                                  const bool shift =
                                      (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                                  if (ctrl_down && (key == 'F' || key == 'H')) {
                                    args->put_Handled(TRUE);
                                    if (on_find) {
                                      on_find(key == 'H');
                                    }
                                    return S_OK;
                                  }
                                  // Ctrl+S / Ctrl+Shift+S — steal from Chromium
                                  // "Save page" so DeskNote Save runs (Typora).
                                  if (ctrl_down && (key == 'S' || key == 's')) {
                                    args->put_Handled(TRUE);
                                    if (shift) {
                                      // Save As is host-menu only; still block
                                      // browser save-as.
                                      if (parent_) {
                                        PostMessageW(parent_, WM_COMMAND, 2102,
                                                     0);
                                      }
                                    } else if (on_save) {
                                      on_save();
                                    } else if (parent_) {
                                      PostMessageW(parent_, WM_COMMAND, 2101, 0);
                                    }
                                    return S_OK;
                                  }
                                  if (key == VK_F3) {
                                    args->put_Handled(TRUE);
                                    if (on_find_again) {
                                      on_find_again(!shift);
                                    } else if (on_find) {
                                      on_find(false);
                                    }
                                    return S_OK;
                                  }
                                  // Shift+Enter: Toast maps this to Enter (new
                                  // paragraph). Steal the key and soft-break in JS
                                  // (split textblock + pad column — no hardBreak
                                  // node in Toast schema).
                                  if (key == VK_RETURN && shift && !ctrl_down &&
                                      kind_ == Kind::kWysiwyg) {
                                    args->put_Handled(TRUE);
                                    ICoreWebView2* w =
                                        static_cast<ICoreWebView2*>(webview_);
                                    if (!w && ctrl) {
                                      ctrl->get_CoreWebView2(&w);
                                    }
                                    if (w) {
                                      w->ExecuteScript(
                                          L"try{window.__desknoteSoftBreak&&"
                                          L"window.__desknoteSoftBreak()}"
                                          L"catch(e){}",
                                          nullptr);
                                    }
                                    return S_OK;
                                  }
                                  return S_OK;
                                })
                                .Get(),
                            &accel_tok);
                      }
                      ICoreWebView2* web = nullptr;
                      controller->get_CoreWebView2(&web);
                      if (!web) {
                        return S_OK;
                      }
                      webview_ = web;
                      available_ = true;
                      // https://app.desknote/* → md_preview folder (faster than
                      // file:// for the Toast bundle; keeps disk cache coherent).
                      {
                        ComPtr<ICoreWebView2_3> web3;
                        if (SUCCEEDED(web->QueryInterface(IID_PPV_ARGS(&web3))) &&
                            web3) {
                          const std::wstring html = kind_ == Kind::kViewer
                                                        ? FindViewerHtml()
                                                        : FindWysiwygHtml();
                          const std::wstring folder = ParentDir(html);
                          if (!folder.empty()) {
                            web3->SetVirtualHostNameToFolderMapping(
                                L"app.desknote", folder.c_str(),
                                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                          }
                        }
                      }
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
                      // Explorer drop → WebView2 navigates to file://… — cancel
                      // shell pages stay allowed (wysiwyg_native / viewer_native).
                      EventRegistrationToken start_tok{};
                      web->add_NavigationStarting(
                          Callback<ICoreWebView2NavigationStartingEventHandler>(
                              [this](ICoreWebView2*,
                                     ICoreWebView2NavigationStartingEventArgs*
                                         args) -> HRESULT {
                                if (!args || !on_external_drop) {
                                  return S_OK;
                                }
                                LPWSTR uri_raw = nullptr;
                                if (FAILED(args->get_Uri(&uri_raw)) ||
                                    !uri_raw) {
                                  return S_OK;
                                }
                                const std::wstring uri(uri_raw);
                                CoTaskMemFree(uri_raw);
                                if (_wcsnicmp(uri.c_str(), L"file:", 5) != 0) {
                                  return S_OK;
                                }
                                if (IsDeskNoteShellHtmlUri(uri)) {
                                  return S_OK;
                                }
                                const std::wstring path = PathFromFileUri(uri);
                                if (path.empty()) {
                                  return S_OK;
                                }
                                args->put_Cancel(TRUE);
                                on_external_drop(
                                    std::vector<std::wstring>{path});
                                return S_OK;
                              })
                              .Get(),
                          &start_tok);
                      // If the page scripts fail to post previewReady, still
                      // flush pending markdown once navigation finishes.
                      EventRegistrationToken nav_tok{};
                      web->add_NavigationCompleted(
                          Callback<ICoreWebView2NavigationCompletedEventHandler>(
                              [this](ICoreWebView2*,
                                     ICoreWebView2NavigationCompletedEventArgs*
                                         args) -> HRESULT {
                                BOOL ok = FALSE;
                                if (args) {
                                  args->get_IsSuccess(&ok);
                                }
                                if (ok && !ready_) {
                                  OnReady();
                                } else if (ok && ready_ &&
                                           !pending_set_.empty()) {
                                  if (kind_ == Kind::kViewer) {
                                    SetPreview(pending_set_, pending_base_);
                                  } else {
                                    SetMarkdown(pending_set_);
                                  }
                                  pending_set_.clear();
                                  pending_base_.clear();
                                }
                                return S_OK;
                              })
                              .Get(),
                          &nav_tok);
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
  const std::wstring html =
      kind_ == Kind::kViewer ? FindViewerHtml() : FindWysiwygHtml();
  if (html.empty()) {
    return;
  }
  ready_ = false;
  content_ready_ = false;
  const std::wstring leaf = kind_ == Kind::kViewer ? L"viewer_native.html"
                                                   : L"wysiwyg_native.html";
  const std::wstring token = AssetCacheToken(html);
  // Prefer virtual host (mapped in Create). Fall back to file:// if mapping
  // failed on older runtimes — still works, just slower cache semantics.
  ComPtr<ICoreWebView2_3> web3;
  if (SUCCEEDED(web->QueryInterface(IID_PPV_ARGS(&web3))) && web3) {
    const std::wstring folder = ParentDir(html);
    if (!folder.empty()) {
      web3->SetVirtualHostNameToFolderMapping(
          L"app.desknote", folder.c_str(),
          COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }
    const std::wstring url =
        L"https://app.desknote/" + leaf + L"?v=" + token;
    web->Navigate(url.c_str());
    return;
  }
  web->Navigate((FileUrlFromPath(html) + L"?v=" + token).c_str());
}

void NotesWebView::OnReady() {
  ready_ = true;
  // Prefer explicit pending push; else re-push cache. Always call SetMarkdown
  // for WYSIWYG — even "" — so Toast boots (skipping empty left a blank host).
  std::wstring md = pending_set_;
  const std::wstring base = pending_base_;
  pending_set_.clear();
  pending_base_.clear();
  if (md.empty()) {
    md = pending_md_;
  }
  if (kind_ == Kind::kViewer) {
    if (!md.empty() || !base.empty()) {
      SetPreview(md, base);
    }
    return;
  }
  SetMarkdown(md);
}

void NotesWebView::OnWebMessage(const std::wstring& json) {
  const std::wstring type = JsonStringField(json, L"type");
  if (type == L"ready" || type == L"previewReady") {
    OnReady();
  } else if (type == L"dirty") {
    // setMarkdown / PullMarkdown / post-Save Toast churn — ignore so a
    // freshly saved or seeded note does not stick on the unsaved disc.
    if (applying_set_ || pulling_md_ ||
        (quiet_dirty_until_ != 0 &&
         GetTickCount64() < quiet_dirty_until_)) {
      return;
    }
    if (on_dirty) {
      on_dirty();
    } else if (on_changed) {
      on_changed();
    }
  } else if (type == L"md" || type == L"changed") {
    if (pulling_md_ || applying_set_) {
      // Pull owns pending_md_; SetMarkdown owns it until contentReady — a late
      // debounce must not MarkDirty after Ctrl+S (glyph stuck on disc).
    } else {
      pending_md_ = JsonStringField(json, L"md");
      got_md_ = true;
      if (type == L"changed" && on_changed) {
        if (quiet_dirty_until_ != 0 &&
            GetTickCount64() < quiet_dirty_until_) {
          // Keep cache fresh, but do not flip the tab disc after Save.
          return;
        }
        on_changed();
      }
    }
  } else if (type == L"needContent") {
    if (on_need_content) {
      on_need_content();
    } else if (!pending_md_.empty()) {
      SetMarkdown(pending_md_);
    }
  } else if (type == L"fontPrefs") {
    if (on_font_prefs) {
      const std::wstring face = JsonStringField(json, L"face");
      const int pt = static_cast<int>(JsonDoubleField(json, L"pt", 12.0));
      on_font_prefs(face, pt);
    }
  } else if (type == L"chromeReady") {
    // Reveal body host; keep applying_set_ until contentReady so an empty
    // Toast `changed` cannot clobber pending_md_ and soft-save wipe the file.
    content_ready_ = true;
    if (on_content_ready) {
      on_content_ready();
    }
  } else if (type == L"contentReady") {
    content_ready_ = true;
    applying_set_ = false;
    if (on_content_ready) {
      on_content_ready();
    }
  } else if (type == L"scroll") {
    scroll_ratio_ = JsonDoubleField(json, L"ratio", 0.0);
    got_scroll_ = true;
    if (on_scroll) {
      on_scroll(scroll_ratio_);
    }
  } else if (type == L"openExternal") {
    const std::wstring href = JsonStringField(json, L"href");
    if (!href.empty()) {
      ShellExecuteW(nullptr, L"open", href.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
    }
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
  content_ready_ = false;
}

void NotesWebView::Show() {
  if (host_) {
    ShowWindow(host_, SW_SHOW);
    if (controller_) {
      static_cast<ICoreWebView2Controller*>(controller_)->put_IsVisible(TRUE);
    }
    Resize();
  }
}

void NotesWebView::Hide() {
  if (host_) {
    ShowWindow(host_, SW_HIDE);
  }
  if (controller_) {
    static_cast<ICoreWebView2Controller*>(controller_)->put_IsVisible(FALSE);
  }
}

void NotesWebView::Resize() {
  // Only sync WebView2 bounds to the host HWND. Parent layout (tabs / sidebar /
  // editor rect) owns host_ geometry via MoveWindow — never fill the whole
  // parent here (that covered DeskNote chrome after opening a .md preview).
  if (!host_) {
    return;
  }
  RECT bounds{};
  GetClientRect(host_, &bounds);
  if (controller_) {
    static_cast<ICoreWebView2Controller*>(controller_)->put_Bounds(bounds);
  }
  // Toast WW pane keeps its boot-time pixel height unless we nudge it.
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (web && ready_ && kind_ == Kind::kWysiwyg) {
    const int h = (std::max)(120, static_cast<int>(bounds.bottom - bounds.top));
    if (h == last_ww_height_) {
      return;
    }
    last_ww_height_ = h;
    wchar_t script[160];
    swprintf_s(script,
               L"try{if(window.__desknoteWysiwygResize)__desknoteWysiwygResize(%d);}"
               L"catch(e){}",
               h);
    web->ExecuteScript(script, nullptr);
  }
}

void NotesWebView::EvalScript(const std::wstring& js) {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web || js.empty()) {
    return;
  }
  web->ExecuteScript(js.c_str(), nullptr);
}

void NotesWebView::AckClean() {
  // Cover Toast 150ms debounce + trailing exit-slot + in-flight posts.
  quiet_dirty_until_ = GetTickCount64() + 600;
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web || !ready_ || kind_ != Kind::kWysiwyg) {
    return;
  }
  web->PostWebMessageAsJson(L"{\"type\":\"ackClean\"}");
}

void NotesWebView::SetMarkdown(const std::wstring& md) {
  // Keep cache coherent so SyncFromWysiwyg / Save can skip PullMarkdown IPC.
  pending_md_ = md;
  applying_set_ = true;
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web || !ready_) {
    pending_set_ = md;
    // Not painted yet — cache is already the new doc; allow later changed.
    applying_set_ = false;
    return;
  }
  const std::wstring theme =
      theme_.empty() ? L"light" : theme_;
  const std::wstring json = L"{\"type\":\"set\",\"theme\":\"" + theme +
                            L"\",\"md\":\"" + JsonEscape(md) + L"\"}";
  web->PostWebMessageAsJson(json.c_str());
}

void NotesWebView::SetPreview(const std::wstring& md,
                              const std::wstring& base_dir) {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web || !ready_) {
    pending_set_ = md;
    pending_base_ = base_dir;
    return;
  }
  const std::wstring theme =
      theme_.empty() ? L"light" : theme_;
  const std::wstring json =
      L"{\"type\":\"set\",\"theme\":\"" + theme + L"\",\"baseUrl\":\"" +
      JsonEscape(base_dir) + L"\",\"md\":\"" + JsonEscape(md) + L"\"}";
  web->PostWebMessageAsJson(json.c_str());
}

void NotesWebView::SetScrollRatio(double ratio) {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web || !ready_) {
    return;
  }
  wchar_t buf[64];
  swprintf_s(buf, L"{\"type\":\"scrollTo\",\"ratio\":%f}", ratio);
  web->PostWebMessageAsJson(buf);
}

double NotesWebView::RequestScrollRatio(DWORD timeout_ms) {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web || !ready_) {
    return scroll_ratio_;
  }
  got_scroll_ = false;
  web->PostWebMessageAsJson(L"{\"type\":\"getScroll\"}");
  const DWORD start = GetTickCount();
  MSG msg{};
  while (!got_scroll_ && GetTickCount() - start < timeout_ms) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(5);
  }
  return scroll_ratio_;
}

bool NotesWebView::PrintToPdf(const std::wstring& path, std::wstring* err) {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web || !ready_) {
    if (err) {
      *err = L"预览未就绪";
    }
    return false;
  }
  ComPtr<ICoreWebView2_7> p7;
  const HRESULT qi = web->QueryInterface(IID_ICoreWebView2_7, &p7);
  if (FAILED(qi) || !p7) {
    if (err) {
      *err = L"当前 WebView2 运行时不支持 PDF 导出";
    }
    return false;
  }
  print_done_ = false;
  print_ok_ = false;
  const HRESULT hr = p7->PrintToPdf(
      path.c_str(), nullptr,
      Callback<ICoreWebView2PrintToPdfCompletedHandler>(
          [this](HRESULT errorCode, BOOL result) -> HRESULT {
            print_done_ = true;
            print_ok_ = SUCCEEDED(errorCode) && result;
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    if (err) {
      *err = L"PrintToPdf 调用失败";
    }
    return false;
  }
  const DWORD start = GetTickCount();
  MSG msg{};
  while (!print_done_ && GetTickCount() - start < 30000) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(10);
  }
  // Verify output file exists and is non-empty.
  const DWORD attr = GetFileAttributesW(path.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
    if (err) {
      *err = L"导出文件未生成";
    }
    return false;
  }
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
    const ULONGLONG sz = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) |
                         fad.nFileSizeLow;
    if (sz == 0) {
      if (err) {
        *err = L"导出的 PDF 为空";
      }
      return false;
    }
  }
  if (!print_ok_) {
    if (err) {
      *err = L"PDF 导出未完成";
    }
    return false;
  }
  return true;
}

namespace {
std::wstring JsonUnquote(const std::wstring& s) {
  if (s.size() < 2 || s.front() != L'"' || s.back() != L'"') {
    return s;
  }
  std::wstring v;
  v.reserve(s.size());
  for (size_t i = 1; i + 1 < s.size(); ++i) {
    if (s[i] == L'\\' && i + 1 + 1 <= s.size()) {
      ++i;
      switch (s[i]) {
        case L'n':
          v.push_back(L'\n');
          break;
        case L'r':
          v.push_back(L'\r');
          break;
        case L't':
          v.push_back(L'\t');
          break;
        case L'u':
          if (i + 4 < s.size() - 1) {
            unsigned code = 0;
            for (int k = 0; k < 4; ++k) {
              const wchar_t c = s[++i];
              code <<= 4;
              if (c >= L'0' && c <= L'9') {
                code |= static_cast<unsigned>(c - L'0');
              } else if (c >= L'a' && c <= L'f') {
                code |= static_cast<unsigned>(c - L'a' + 10);
              } else if (c >= L'A' && c <= L'F') {
                code |= static_cast<unsigned>(c - L'A' + 10);
              }
            }
            v.push_back(static_cast<wchar_t>(code));
          }
          break;
        default:
          v.push_back(s[i]);
          break;
      }
    } else {
      v.push_back(s[i]);
    }
  }
  return v;
}
}  // namespace

std::wstring NotesWebView::PullMarkdown(DWORD timeout_ms) {
  auto* web = static_cast<ICoreWebView2*>(webview_);
  if (!web || !ready_) {
    return pending_md_;
  }
  got_md_ = false;
  pulling_md_ = true;
  // ExecuteScript alone — dual get+postMessage doubled IPC on every Save/tab.
  web->ExecuteScript(
      L"(function(){try{return (window.__desknoteWysiwygGet"
      L"?window.__desknoteWysiwygGet():'');}catch(e){return '';}})()",
      Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
          [this](HRESULT hr, LPCWSTR result_json) -> HRESULT {
            if (SUCCEEDED(hr) && result_json) {
              pending_md_ = JsonUnquote(result_json);
            }
            got_md_ = true;
            return S_OK;
          })
          .Get());
  const DWORD start = GetTickCount();
  MSG msg{};
  while (!got_md_ && GetTickCount() - start < timeout_ms) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(5);
  }
  pulling_md_ = false;
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
  if (msg == WM_DROPFILES && parent_) {
    return SendMessageW(parent_, WM_DROPFILES, wparam, lparam);
  }
  if (msg == WM_DESTROY) {
    host_ = nullptr;
    return 0;
  }
  return DefWindowProcW(host_, msg, wparam, lparam);
}
