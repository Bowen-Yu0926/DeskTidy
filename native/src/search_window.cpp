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
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::Callback;

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeSearch";
constexpr wchar_t kHostClass[] = L"DeskTidyNativeSearchWeb";
constexpr int kPanelW = 680;
constexpr int kPanelH = 580;
constexpr UINT kMsgSearchUi = WM_APP + 81;
constexpr UINT kTimerDebounce = 1;
constexpr UINT kDebounceMs = 180;

struct SearchUiMsg {
  unsigned gen = 0;
  int pct = 0;
  bool done = false;
  std::wstring status;
  std::vector<std::wstring> items;
};

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

// Parse JSON string-array field: "types":["word","pdf"] / "drives":["C","D"]
std::vector<std::wstring> ParseJsonStringArray(const std::wstring& json,
                                               const wchar_t* key,
                                               bool types_only) {
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
    if (!val.empty() && (!types_only || IsKnownTypeId(val))) {
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
                     const std::unordered_set<std::wstring>* allowed_exts,
                     const std::atomic<unsigned>* gen_ptr, unsigned gen) {
  if (!out || !budget || *budget <= 0 || needle.empty() || depth > max_depth ||
      !gen_ptr || gen_ptr->load() != gen) {
    return;
  }
  const std::wstring pattern = dir + L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (gen_ptr->load() != gen) {
      break;
    }
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
                      include_hidden, allowed_exts, gen_ptr, gen);
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

// CreateProcess command-line quoting: a trailing \ before " escapes the quote
// ("C:\" "D:\" → one broken arg C:" D:"). Double a final backslash; escape ".
std::wstring QuoteCmdArg(const std::wstring& s) {
  std::wstring out = L"\"";
  for (wchar_t c : s) {
    if (c == L'"') {
      out += L"\\\"";
    } else {
      out.push_back(c);
    }
  }
  if (!s.empty() && s.back() == L'\\') {
    out.push_back(L'\\');
  }
  out.push_back(L'"');
  return out;
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

std::vector<std::wstring> FdSearchRoots(
    const std::wstring& fd, const std::wstring& needle,
    const std::vector<std::wstring>& roots, int max_results, int max_depth,
    int threads, bool include_hidden,
    const std::vector<std::wstring>& exts) {
  std::vector<std::wstring> hits;
  if (fd.empty() || roots.empty() || needle.empty()) {
    return hits;
  }
  std::wstring cmd = L"\"" + fd + L"\" -i -a --max-results " +
                     std::to_wstring(max_results) + L" --max-depth " +
                     std::to_wstring(max_depth) + L" --threads " +
                     std::to_wstring(threads);
  if (include_hidden) {
    cmd += L" --hidden --no-ignore-vcs";
  }
  for (const auto& e : exts) {
    cmd += L" -e ";
    cmd += e;
  }
  cmd += L" ";
  cmd += QuoteCmdArg(needle);
  for (const auto& root : roots) {
    std::wstring path = root;
    if (IsDriveRootPath(path)) {
      path[2] = L'/';
    }
    cmd += L" ";
    cmd += QuoteCmdArg(path);
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    return hits;
  }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(wr);
    CloseHandle(rd);
    return hits;
  }
  CloseHandle(wr);
  CloseHandle(pi.hThread);
  std::string utf8;
  char chunk[4096];
  DWORD n = 0;
  while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n) {
    utf8.append(chunk, chunk + n);
  }
  WaitForSingleObject(pi.hProcess, 20000);
  CloseHandle(pi.hProcess);
  CloseHandle(rd);
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
    if (!line.empty() && line.rfind("[fd ", 0) != 0 &&
        line.rfind("error:", 0) != 0) {
      const int wn =
          MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
      std::wstring wide(wn > 0 ? wn - 1 : 0, L'\0');
      if (wn > 1) {
        MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide.data(), wn);
      }
      hits.push_back(std::move(wide));
    }
    pos = eol + 1;
  }
  return hits;
}

}  // namespace

std::vector<std::wstring> SearchWindow::EnumFixedDriveLetters() {
  std::vector<std::wstring> out;
  const DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if ((mask & (1u << i)) == 0) {
      continue;
    }
    const wchar_t letter = static_cast<wchar_t>(L'A' + i);
    const std::wstring root = std::wstring(1, letter) + L":\\";
    const UINT type = GetDriveTypeW(root.c_str());
    if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
      out.push_back(std::wstring(1, letter));
    }
  }
  return out;
}

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
  // WebView2 created lazily on first Show() — see TodoWindow (boot latency).
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
  type_filters_ = ParseJsonStringArray(json, L"types", /*types_only=*/true);
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

void SearchWindow::NotifyAllDrivesPref() {
  const auto all = EnumFixedDriveLetters();
  bool every = !all.empty() && selected_drives_.size() == all.size();
  if (every) {
    for (const auto& a : all) {
      bool found = false;
      for (const auto& s : selected_drives_) {
        if (_wcsicmp(a.c_str(), s.c_str()) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        every = false;
        break;
      }
    }
  }
  options_.all_drives = every;
  if (on_all_drives_) {
    on_all_drives_(options_.all_drives);
  }
}

void SearchWindow::PushProgress(int pct, const std::wstring& status,
                                const std::vector<std::wstring>* live_items) {
  if (!webview_ || !web_ready_) {
    return;
  }
  progress_pct_ = pct;
  std::wstring json = L"{\"type\":\"progress\",\"pct\":";
  json += std::to_wstring(pct);
  json += L",\"status\":\"";
  json += MistJsonEscape(status);
  json += L"\"";
  if (live_items) {
    json += L",\"items\":[";
    for (size_t i = 0; i < live_items->size(); ++i) {
      if (i) {
        json += L',';
      }
      json += L'"';
      json += MistJsonEscape((*live_items)[i]);
      json += L'"';
    }
    json += L']';
  }
  json += L'}';
  auto* web = static_cast<ICoreWebView2*>(webview_);
  web->PostWebMessageAsString(json.c_str());
  std::wstring script =
      L"(function(){try{var m=" + json +
      L";if(typeof window.applyState==='function'){window.applyState(m);}"
      L"}catch(e){}})();";
  web->ExecuteScript(script.c_str(), nullptr);
}

void SearchWindow::PushState(const std::wstring& status) {
  if (!webview_ || !web_ready_) {
    return;
  }
  const auto drives = EnumFixedDriveLetters();
  std::wstring json = L"{\"type\":\"state\",\"searching\":";
  json += searching_ ? L"true" : L"false";
  json += L",\"progress\":";
  json += std::to_wstring(progress_pct_);
  json += L",\"all_drives\":";
  json += options_.all_drives ? L"true" : L"false";
  json += L",\"drives\":[";
  for (size_t i = 0; i < drives.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += MistJsonEscape(drives[i]);
    json += L'"';
  }
  json += L"],\"selected_drives\":[";
  for (size_t i = 0; i < selected_drives_.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L'"';
    json += MistJsonEscape(selected_drives_[i]);
    json += L'"';
  }
  json += L"],\"types\":[";
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
    if (json.find(L"\"drives\"") != std::wstring::npos) {
      selected_drives_ = ParseJsonStringArray(json, L"drives", false);
      for (auto& d : selected_drives_) {
        if (d.size() > 1 && d[1] == L':') {
          d.resize(1);
        }
        if (!d.empty() && d[0] >= L'a' && d[0] <= L'z') {
          d[0] = static_cast<wchar_t>(d[0] - L'a' + L'A');
        }
      }
      NotifyAllDrivesPref();
    }
    if (json.find(L"\"types\"") != std::wstring::npos) {
      ParseTypesFromJson(json);
    }
    // Debounce rapid keystrokes: cancel the pending timer and reschedule so the
    // last query always runs; an empty query clears immediately (no scan cost).
    KillTimer(hwnd_, kTimerDebounce);
    if (query_.empty()) {
      RunSearch(query_);
    } else {
      SetTimer(hwnd_, kTimerDebounce, kDebounceMs, nullptr);
    }
  } else if (id == L"drives") {
    selected_drives_ = ParseJsonStringArray(json, L"drives", false);
    for (auto& d : selected_drives_) {
      if (d.size() > 1 && d[1] == L':') {
        d.resize(1);
      }
      if (!d.empty() && d[0] >= L'a' && d[0] <= L'z') {
        d[0] = static_cast<wchar_t>(d[0] - L'a' + L'A');
      }
    }
    NotifyAllDrivesPref();
  } else if (id == L"all_drives") {
    options_.all_drives =
        json.find(L"\"value\":true") != std::wstring::npos;
    if (options_.all_drives) {
      selected_drives_ = EnumFixedDriveLetters();
    } else {
      selected_drives_.clear();
    }
    if (on_all_drives_) {
      on_all_drives_(options_.all_drives);
    }
    PushState();
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
  // Fresh open: no drive chips selected (avoid sticky C: from prior session /
  // settings "search all drives").
  selected_drives_.clear();
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

std::vector<std::wstring> SearchWindow::BuildRoots() const {
  std::vector<std::wstring> roots;
  if (!selected_drives_.empty()) {
    for (const auto& letter : selected_drives_) {
      if (letter.empty()) {
        continue;
      }
      const std::wstring root = std::wstring(1, letter[0]) + L":\\";
      const UINT type = GetDriveTypeW(root.c_str());
      if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE ||
          type == DRIVE_REMOTE) {
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
    if (options_.include_open_folders) {
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
  }
  if (roots.empty()) {
    const std::wstring p = KnownFolder(FOLDERID_Desktop);
    if (!p.empty()) {
      roots.push_back(p);
    }
  }
  return roots;
}

void SearchWindow::RunSearch(const std::wstring& needle) {
  results_.clear();
  if (needle.empty()) {
    searching_ = false;
    progress_pct_ = 0;
    PushState(Tr("search_hint"));
    return;
  }
  const unsigned gen = ++search_gen_;
  searching_ = true;
  progress_pct_ = 0;
  PushState(L"搜索中…");
  PushProgress(0, L"准备搜索…");

  const auto roots = BuildRoots();
  const int max_results =
      options_.max_results > 0 ? options_.max_results : 50;
  const int max_depth = options_.max_depth > 0 ? options_.max_depth : 12;
  const int threads = options_.threads < 1 ? 1
                      : (options_.threads > 64 ? 64 : options_.threads);
  const std::vector<std::wstring> exts = ResolvedExtensions();
  const bool include_hidden = options_.include_hidden;
  const std::wstring fd = FindFdExe();
  HWND hwnd = hwnd_;

  std::thread([this, hwnd, gen, needle, roots, max_results, max_depth, threads,
               exts, include_hidden, fd]() {
    std::vector<std::wstring> hits;
    hits.reserve(static_cast<size_t>(max_results));
    std::unordered_set<std::wstring> allowed;
    for (const auto& e : exts) {
      allowed.insert(e);
    }
    const std::unordered_set<std::wstring>* allowed_ptr =
        allowed.empty() ? nullptr : &allowed;

    const int n = static_cast<int>(roots.size());
    for (int i = 0; i < n; ++i) {
      if (search_gen_.load() != gen) {
        return;
      }
      const std::wstring& root = roots[static_cast<size_t>(i)];
      std::wstring label = root;
      if (IsDriveRootPath(root)) {
        label = std::wstring(1, root[0]) + L":";
      } else {
        const size_t slash = root.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash + 1 < root.size()) {
          label = root.substr(slash + 1);
        }
      }
      const int pct = static_cast<int>((i * 100) / (std::max)(n, 1));
      auto* prog = new SearchUiMsg;
      prog->gen = gen;
      prog->pct = pct;
      prog->done = false;
      prog->status = L"正在搜索 " + label + L"…";
      prog->items = hits;
      if (!PostMessageW(hwnd, kMsgSearchUi, 0,
                        reinterpret_cast<LPARAM>(prog))) {
        delete prog;
        return;
      }

      const int remain =
          max_results - static_cast<int>(hits.size());
      if (remain <= 0) {
        break;
      }
      std::vector<std::wstring> batch;
      if (!fd.empty()) {
        batch = FdSearchRoots(fd, needle, {root}, remain, max_depth, threads,
                              include_hidden, exts);
      }
      if (batch.empty()) {
        int budget = remain;
        SearchRecursive(root, needle, &batch, &budget, 0, max_depth,
                        include_hidden, allowed_ptr, &search_gen_, gen);
      }
      for (auto& p : batch) {
        if (hits.size() >= static_cast<size_t>(max_results)) {
          break;
        }
        hits.push_back(std::move(p));
      }
    }

    if (search_gen_.load() != gen) {
      return;
    }
    auto* done = new SearchUiMsg;
    done->gen = gen;
    done->pct = 100;
    done->done = true;
    done->status = hits.empty() ? L"无结果" : (std::to_wstring(hits.size()) + L" 项");
    done->items = std::move(hits);
    if (!PostMessageW(hwnd, kMsgSearchUi, 0, reinterpret_cast<LPARAM>(done))) {
      delete done;
    }
  }).detach();
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
    case kMsgSearchUi: {
      auto* m = reinterpret_cast<SearchUiMsg*>(lparam);
      if (!m) {
        return 0;
      }
      std::unique_ptr<SearchUiMsg> own(m);
      if (own->gen != search_gen_.load()) {
        return 0;
      }
      if (own->done) {
        results_ = std::move(own->items);
        searching_ = false;
        progress_pct_ = 100;
        PushState(own->status);
      } else {
        progress_pct_ = own->pct;
        PushProgress(own->pct, own->status, &own->items);
      }
      return 0;
    }
    case WM_TIMER:
      if (wparam == kTimerDebounce) {
        KillTimer(hwnd_, kTimerDebounce);
        RunSearch(query_);
        return 0;
      }
      break;
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
      ++search_gen_;
      DestroyWebView();
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}
