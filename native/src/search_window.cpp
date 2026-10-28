#include "search_window.h"

#include "display_geom.h"
#include "i18n.h"
#include "icon_actions.h"
#include "mist_webview.h"
#include "paths_util.h"
#include "tool_paths.h"

#include <ShlObj.h>
#include <Shellapi.h>

#include <objbase.h>
#include <exdisp.h>
#include <shldisp.h>
#ifndef interface
#define interface struct
#endif
#include <WebView2.h>
#include <wrl.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::Callback;

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeSearch";
constexpr wchar_t kHostClass[] = L"DeskTidyNativeSearchWeb";
constexpr int kPanelW = 640;
constexpr int kPanelH = 540;

struct TypeGroup {
  const wchar_t* id;
  const wchar_t* const* exts;
  size_t count;
};

constexpr const wchar_t* kWordExts[] = {L"doc", L"docx"};
constexpr const wchar_t* kExcelExts[] = {L"xls", L"xlsx", L"csv"};
constexpr const wchar_t* kPptExts[] = {L"ppt", L"pptx"};
constexpr const wchar_t* kPdfExts[] = {L"pdf"};
constexpr const wchar_t* kMdExts[] = {L"md", L"markdown"};
constexpr const wchar_t* kImageExts[] = {L"png", L"jpg", L"jpeg", L"gif",
                                         L"webp", L"bmp", L"ico"};
constexpr const wchar_t* kVideoExts[] = {L"mp4", L"mkv", L"avi", L"mov",
                                         L"wmv", L"webm", L"flv", L"m4v"};

constexpr TypeGroup kTypeGroups[] = {
    {L"word", kWordExts, sizeof(kWordExts) / sizeof(kWordExts[0])},
    {L"excel", kExcelExts, sizeof(kExcelExts) / sizeof(kExcelExts[0])},
    {L"ppt", kPptExts, sizeof(kPptExts) / sizeof(kPptExts[0])},
    {L"pdf", kPdfExts, sizeof(kPdfExts) / sizeof(kPdfExts[0])},
    {L"md", kMdExts, sizeof(kMdExts) / sizeof(kMdExts[0])},
    {L"image", kImageExts, sizeof(kImageExts) / sizeof(kImageExts[0])},
    {L"video", kVideoExts, sizeof(kVideoExts) / sizeof(kVideoExts[0])},
};

bool IsKnownTypeId(const std::wstring& id) {
  for (const auto& g : kTypeGroups) {
    if (_wcsicmp(g.id, id.c_str()) == 0) {
      return true;
    }
  }
  return false;
}

std::wstring ExtOfName(const std::wstring& name) {
  const size_t dot = name.find_last_of(L'.');
  if (dot == std::wstring::npos || dot + 1 >= name.size()) {
    return {};
  }
  std::wstring ext = name.substr(dot + 1);
  for (auto& c : ext) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  return ext;
}

bool ExtAllowed(const std::wstring& name,
                const std::unordered_set<std::wstring>* allowed) {
  if (!allowed || allowed->empty()) {
    return true;
  }
  return allowed->count(ExtOfName(name)) > 0;
}

// Parse JSON string-array field: "types":["word","pdf"]
std::vector<std::wstring> ParseJsonStringArray(const std::wstring& json,
                                               const wchar_t* key) {
  std::vector<std::wstring> out;
  if (!key) {
    return out;
  }
  const std::wstring needle = std::wstring(L"\"") + key + L"\"";
  size_t pos = json.find(needle);
  if (pos == std::wstring::npos) {
    return out;
  }
  pos = json.find(L'[', pos);
  if (pos == std::wstring::npos) {
    return out;
  }
  const size_t end = json.find(L']', pos);
  if (end == std::wstring::npos) {
    return out;
  }
  size_t i = pos + 1;
  while (i < end) {
    const size_t q1 = json.find(L'"', i);
    if (q1 == std::wstring::npos || q1 >= end) {
      break;
    }
    size_t q2 = q1 + 1;
    std::wstring val;
    while (q2 < end && json[q2] != L'"') {
      if (json[q2] == L'\\' && q2 + 1 < end) {
        ++q2;
      }
      val.push_back(json[q2++]);
    }
    if (!val.empty() && IsKnownTypeId(val)) {
      out.push_back(val);
    }
    i = q2 + 1;
  }
  return out;
}

bool NameContains(const std::wstring& name, const std::wstring& needle) {
  if (needle.empty()) {
    return true;
  }
  for (size_t i = 0; i + needle.size() <= name.size(); ++i) {
    bool ok = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      wchar_t a = name[i + j];
      wchar_t b = needle[j];
      if (a >= L'A' && a <= L'Z') {
        a = static_cast<wchar_t>(a - L'A' + L'a');
      }
      if (b >= L'A' && b <= L'Z') {
        b = static_cast<wchar_t>(b - L'A' + L'a');
      }
      if (a != b) {
        ok = false;
        break;
      }
    }
    if (ok) {
      return true;
    }
  }
  return false;
}

void SearchRecursive(const std::wstring& dir, const std::wstring& needle,
                     std::vector<std::wstring>* out, int* budget, int depth,
                     int max_depth, bool include_hidden,
                     const std::unordered_set<std::wstring>* allowed_exts) {
  if (!out || !budget || *budget <= 0 || needle.empty() || depth > max_depth) {
    return;
  }
  const std::wstring pattern = dir + L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (fd.cFileName[0] == L'.' &&
        (fd.cFileName[1] == L'\0' ||
         (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
      continue;
    }
    if (!include_hidden && (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) {
      continue;
    }
    const std::wstring name = fd.cFileName;
    const std::wstring full = dir + L"\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (_wcsicmp(name.c_str(), L"Windows") == 0 ||
          _wcsicmp(name.c_str(), L"$Recycle.Bin") == 0 ||
          _wcsicmp(name.c_str(), L"AppData") == 0) {
        continue;
      }
      SearchRecursive(full, needle, out, budget, depth + 1, max_depth,
                      include_hidden, allowed_exts);
    } else if (NameContains(name, needle) && ExtAllowed(name, allowed_exts)) {
      out->push_back(full);
      --(*budget);
      if (*budget <= 0) {
        break;
      }
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path)) || !path) {
    return {};
  }
  std::wstring out = path;
  CoTaskMemFree(path);
  return out;
}

bool IsDriveRootPath(const std::wstring& p) {
  return p.size() == 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/');
}

// Filesystem folders currently open in Explorer (Python
// open_explorer_folder_paths). Mirrors Shell.Application.Windows() enumeration:
// for each window, Document → Folder → Self → Path. Virtual namespaces yield
// empty paths (skipped); drive roots are too broad for the "open folder" bonus.
std::vector<std::wstring> OpenExplorerFolders() {
  std::vector<std::wstring> out;
  IShellWindows* sw = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                              IID_IShellWindows,
                              reinterpret_cast<void**>(&sw))) ||
      !sw) {
    return out;
  }
  long count = 0;
  if (FAILED(sw->get_Count(&count))) {
    sw->Release();
    return out;
  }
  for (long i = 0; i < count && out.size() < 12; ++i) {
    VARIANT v;
    VariantInit(&v);
    V_VT(&v) = VT_I4;
    V_I4(&v) = i;
    IDispatch* disp = nullptr;
    if (FAILED(sw->Item(v, &disp)) || !disp) {
      VariantClear(&v);
      continue;
    }
    IWebBrowser* browser = nullptr;
    if (SUCCEEDED(disp->QueryInterface(
            IID_IWebBrowser, reinterpret_cast<void**>(&browser))) &&
        browser) {
      IDispatch* doc = nullptr;
      if (SUCCEEDED(browser->get_Document(&doc)) && doc) {
        IShellFolderViewDual* view = nullptr;
        if (SUCCEEDED(doc->QueryInterface(
                IID_IShellFolderViewDual, reinterpret_cast<void**>(&view))) &&
            view) {
          Folder* folder = nullptr;
          if (SUCCEEDED(view->get_Folder(&folder)) && folder) {
            Folder2* folder2 = nullptr;
            if (SUCCEEDED(folder->QueryInterface(
                    IID_Folder2, reinterpret_cast<void**>(&folder2))) &&
                folder2) {
              FolderItem* self = nullptr;
              if (SUCCEEDED(folder2->get_Self(&self)) && self) {
                BSTR path = nullptr;
                if (SUCCEEDED(self->get_Path(&path)) && path &&
                    SysStringLen(path) > 0) {
                  const std::wstring p(path);
                  if (!p.empty() && p[0] != L':' && !IsDriveRootPath(p)) {
                    out.push_back(p);
                  }
                }
                if (path) {
                  SysFreeString(path);
                }
                self->Release();
              }
              folder2->Release();
            }
            folder->Release();
          }
          view->Release();
        }
        doc->Release();
      }
      browser->Release();
    }
    disp->Release();
    VariantClear(&v);
  }
  sw->Release();
  return out;
}

}  // namespace

bool SearchWindow::Create(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = SearchWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(0xEE, 0xF1, 0xF6));
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  const RECT work = PrimaryWorkArea();
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, Tr("search_title").c_str(),
      WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
      work.left + ((work.right - work.left) - kPanelW) / 2,
      work.top + ((work.bottom - work.top) - kPanelH) / 4, kPanelW, kPanelH,
      nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  CreateWebView();
  return true;
}

bool SearchWindow::CreateWebView() {
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
    wc.hbrBackground = CreateSolidBrush(RGB(0xF4, 0xF6, 0xF9));
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
  const std::wstring user_data =
      JoinPath(NativeDataDir(), L"webview2-search");
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

void SearchWindow::DestroyWebView() {
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

void SearchWindow::ResizeWebView() {
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

void SearchWindow::NavigateHome() {
  if (!webview_) {
    return;
  }
  const std::wstring html = MistFindAssetHtml(L"overlays\\search.html");
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

void SearchWindow::ParseTypesFromJson(const std::wstring& json) {
  type_filters_ = ParseJsonStringArray(json, L"types");
}

std::vector<std::wstring> SearchWindow::ResolvedExtensions() const {
  std::vector<std::wstring> out;
  if (type_filters_.empty()) {
    return out;
  }
  std::unordered_set<std::wstring> seen;
  for (const auto& id : type_filters_) {
    for (const auto& g : kTypeGroups) {
      if (_wcsicmp(g.id, id.c_str()) != 0) {
        continue;
      }
      for (size_t i = 0; i < g.count; ++i) {
        const std::wstring e = g.exts[i];
        if (seen.insert(e).second) {
          out.push_back(e);
        }
      }
      break;
    }
  }
  return out;
}

void SearchWindow::PushState(const std::wstring& status) {
  if (!webview_ || !web_ready_) {
    return;
  }
  std::wstring json = L"{\"type\":\"state\",\"all_drives\":";
  json += options_.all_drives ? L"true" : L"false";
  json += L",\"types\":[";
  for (size_t i = 0; i < type_filters_.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += MistJsonEscape(type_filters_[i]);
    json += L'"';
  }
  json += L"],\"query\":\"";
  json += MistJsonEscape(query_);
  json += L"\",\"status\":\"";
  json += MistJsonEscape(status.empty()
                             ? (results_.empty() ? Tr("search_hint")
                                                 : (std::to_wstring(results_.size()) +
                                                    L" 项"))
                             : status);
  json += L"\",\"items\":[";
  for (size_t i = 0; i < results_.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += MistJsonEscape(results_[i]);
    json += L'"';
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

void SearchWindow::OnWebMessage(const std::wstring& json) {
  const std::wstring type = MistJsonStringField(json, L"type");
  if (type != L"action") {
    return;
  }
  const std::wstring id = MistJsonStringField(json, L"id");
  if (id == L"ready") {
    PushState();
  } else if (id == L"hide") {
    Hide();
  } else if (id == L"search") {
    query_ = MistJsonStringField(json, L"value");
    if (query_.empty()) {
      query_ = MistJsonStringField(json, L"query");
    }
    // JSON bool may not parse as string — use presence of true in raw
    if (json.find(L"\"all_drives\":true") != std::wstring::npos) {
      options_.all_drives = true;
    } else if (json.find(L"\"all_drives\":false") != std::wstring::npos) {
      options_.all_drives = false;
    }
    if (json.find(L"\"types\"") != std::wstring::npos) {
      ParseTypesFromJson(json);
    }
    if (on_all_drives_) {
      on_all_drives_(options_.all_drives);
    }
    RunSearch(query_);
  } else if (id == L"all_drives") {
    options_.all_drives =
        json.find(L"\"value\":true") != std::wstring::npos;
    if (on_all_drives_) {
      on_all_drives_(options_.all_drives);
    }
  } else if (id == L"types") {
    ParseTypesFromJson(json);
  } else if (id == L"open") {
    OpenAt(MistJsonIntField(json, L"index", -1));
  } else if (id == L"reveal") {
    RevealAt(MistJsonIntField(json, L"index", -1));
  } else if (id == L"context") {
    ContextAt(MistJsonIntField(json, L"index", -1),
              MistJsonIntField(json, L"x", 0), MistJsonIntField(json, L"y", 0));
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
    SetWindowPos(hwnd_, HWND_TOPMOST, win_origin_.x + (x - drag_origin_.x),
                 win_origin_.y + (y - drag_origin_.y), 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
  } else if (id == L"drag_end") {
    dragging_ = false;
  }
}

void SearchWindow::OpenAt(int index) {
  if (index < 0 || index >= static_cast<int>(results_.size())) {
    return;
  }
  ShellExecuteW(nullptr, L"open", results_[static_cast<size_t>(index)].c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
}

void SearchWindow::RevealAt(int index) {
  if (index < 0 || index >= static_cast<int>(results_.size())) {
    return;
  }
  const std::wstring arg =
      L"/select,\"" + results_[static_cast<size_t>(index)] + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr,
                SW_SHOWNORMAL);
}

void SearchWindow::ContextAt(int index, int screen_x, int screen_y) {
  if (index < 0 || index >= static_cast<int>(results_.size())) {
    return;
  }
  const std::wstring path = results_[static_cast<size_t>(index)];
  POINT pt{screen_x, screen_y};
  const int cmd = TrackIconShellMenu(hwnd_, pt, false);
  if (cmd == 1) {
    OpenAt(index);
  } else if (cmd == 2) {
    RevealAt(index);
  } else if (cmd == 4 || cmd == 5) {
    CopyOrCutShellPath(hwnd_, path, cmd == 5);
  } else if (cmd == 6) {
    CopyShellPathText(hwnd_, path);
  } else if (cmd == 7) {
    std::wstring dest;
    if (RenameShellPath(hwnd_, path, &dest) == IconFsResult::Ok) {
      results_[static_cast<size_t>(index)] = dest;
      if (on_path_replaced_) {
        on_path_replaced_(path, dest);
      }
      PushState();
    }
  } else if (cmd == 8) {
    ShowShellPathProperties(hwnd_, path);
  } else if (cmd == 9) {
    if (RecycleShellPath(hwnd_, path) == IconFsResult::Ok) {
      results_.erase(results_.begin() + index);
      if (on_removed_) {
        on_removed_(path);
      }
      PushState();
    }
  }
}

void SearchWindow::Destroy() {
  DestroyWebView();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void SearchWindow::Show() {
  if (!hwnd_) {
    return;
  }
  if (!web_available_) {
    CreateWebView();
  }
  ShowWindow(hwnd_, SW_SHOW);
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetForegroundWindow(hwnd_);
  PushState();
}

void SearchWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void SearchWindow::RunSearch(const std::wstring& needle) {
  results_.clear();
  if (needle.empty()) {
    PushState(Tr("search_hint"));
    return;
  }
  PushState(L"搜索中…");

  std::vector<std::wstring> roots;
  if (options_.all_drives) {
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
      if ((mask & (1u << i)) == 0) {
        continue;
      }
      const wchar_t letter = static_cast<wchar_t>(L'A' + i);
      const std::wstring root = std::wstring(1, letter) + L":\\";
      const UINT type = GetDriveTypeW(root.c_str());
      if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
        roots.push_back(root);
      }
    }
  } else {
    if (options_.desktop) {
      const std::wstring p = KnownFolder(FOLDERID_Desktop);
      if (!p.empty()) {
        roots.push_back(p);
      }
    }
    if (options_.documents) {
      const std::wstring p = KnownFolder(FOLDERID_Documents);
      if (!p.empty()) {
        roots.push_back(p);
      }
    }
    if (options_.downloads) {
      const std::wstring p = KnownFolder(FOLDERID_Downloads);
      if (!p.empty()) {
        roots.push_back(p);
      }
    }
  }
  if (!options_.all_drives && options_.include_open_folders) {
    for (const auto& p : OpenExplorerFolders()) {
      bool dup = false;
      for (const auto& r : roots) {
        if (_wcsicmp(r.c_str(), p.c_str()) == 0) {
          dup = true;
          break;
        }
      }
      if (!dup) {
        roots.push_back(p);
      }
    }
  }

  if (roots.empty()) {
    const std::wstring p = KnownFolder(FOLDERID_Desktop);
    if (!p.empty()) {
      roots.push_back(p);
    }
  }

  const int max_results =
      options_.max_results > 0 ? options_.max_results : 50;
  const int max_depth = options_.max_depth > 0 ? options_.max_depth : 12;
  const int threads = options_.threads < 1 ? 1
                      : (options_.threads > 64 ? 64 : options_.threads);

  const std::vector<std::wstring> exts = ResolvedExtensions();
  std::unordered_set<std::wstring> allowed;
  for (const auto& e : exts) {
    allowed.insert(e);
  }
  const std::unordered_set<std::wstring>* allowed_ptr =
      allowed.empty() ? nullptr : &allowed;

  const std::wstring fd = FindFdExe();
  if (!fd.empty() && !roots.empty()) {
    std::wstring cmd = L"\"" + fd + L"\" -i -a --max-results " +
                       std::to_wstring(max_results) + L" --max-depth " +
                       std::to_wstring(max_depth) + L" --threads " +
                       std::to_wstring(threads);
    if (options_.include_hidden) {
      cmd += L" --hidden --no-ignore-vcs";
    }
    // fd -e: restrict by extension (Everything-style type filters).
    for (const auto& e : exts) {
      cmd += L" -e ";
      cmd += e;
    }
    cmd += L" " + needle;
    for (const auto& root : roots) {
      cmd += L" \"" + root + L"\"";
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (CreatePipe(&rd, &wr, &sa, 0)) {
      SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
      STARTUPINFOW si{};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdOutput = wr;
      si.hStdError = wr;
      PROCESS_INFORMATION pi{};
      std::vector<wchar_t> buf(cmd.begin(), cmd.end());
      buf.push_back(L'\0');
      if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        wr = nullptr;
        CloseHandle(pi.hThread);
        std::string utf8;
        char chunk[4096];
        DWORD n = 0;
        while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n) {
          utf8.append(chunk, chunk + n);
        }
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        size_t pos = 0;
        while (pos < utf8.size()) {
          size_t eol = utf8.find('\n', pos);
          if (eol == std::string::npos) {
            eol = utf8.size();
          }
          std::string line = utf8.substr(pos, eol - pos);
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }
          if (!line.empty()) {
            const int wn =
                MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
            std::wstring wide(wn > 0 ? wn - 1 : 0, L'\0');
            if (wn > 1) {
              MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide.data(), wn);
            }
            results_.push_back(wide);
          }
          pos = eol + 1;
        }
        CloseHandle(rd);
        PushState();
        return;
      }
      if (wr) {
        CloseHandle(wr);
      }
      CloseHandle(rd);
    }
  }

  int budget = max_results;
  for (const auto& root : roots) {
    if (budget <= 0) {
      break;
    }
    SearchRecursive(root, needle, &results_, &budget, 0, max_depth,
                    options_.include_hidden, allowed_ptr);
  }
  PushState();
}

LRESULT CALLBACK SearchWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                       LPARAM lparam) {
  SearchWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<SearchWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<SearchWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT SearchWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_SIZE:
      ResizeWebView();
      return 0;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        Hide();
        return 0;
      }
      break;
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
