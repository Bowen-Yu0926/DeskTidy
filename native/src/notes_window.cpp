#include "notes_window.h"

#include "desktop_shell.h"
#include "markdown_preview.h"
#include "notes_library.h"
#include "paths_util.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <Richedit.h>
#include <Shellapi.h>
#include <ShlObj.h>

#pragma comment(lib, "comctl32.lib")

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#pragma comment(lib, "comdlg32.lib")

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeNotes";
constexpr int kEditId = 2001;
constexpr int kMenuSave = 2101;
constexpr int kMenuSaveAs = 2102;
constexpr int kMenuBigger = 2103;
constexpr int kMenuSmaller = 2104;
constexpr int kMenuTpl = 2105;
constexpr int kMenuBold = 2106;
constexpr int kMenuItalic = 2107;
constexpr int kMenuUnder = 2108;
constexpr int kMenuLibrary = 2109;
constexpr int kMenuPreview = 2110;
constexpr int kMenuFind = 2111;
constexpr int kMenuReplace = 2112;
constexpr int kMenuSearchLib = 2113;
constexpr int kMenuExportMd = 2114;
constexpr int kMenuPandoc = 2115;
constexpr int kMenuOpenWith = 2116;
constexpr int kMenuPasteHtml = 2117;
constexpr int kMenuToast = 2118;
constexpr int kMenuWysiwyg = 2119;
constexpr int kMenuFindNext = 2120;
constexpr int kMenuFindPrev = 2121;
constexpr int kMenuExportHtml = 2122;
constexpr int kMenuOutline = 2123;
constexpr int kMenuFocus = 2124;
constexpr int kMenuTypewriter = 2125;
constexpr int kMenuNewTab = 2126;
constexpr int kMenuCloseTab = 2127;
constexpr int kOutlineId = 2002;
constexpr int kTabsId = 2003;

struct StreamCookie {
  std::string data;
  size_t pos = 0;
};

DWORD CALLBACK StreamInCb(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb) {
  auto* c = reinterpret_cast<StreamCookie*>(cookie);
  const size_t left = c->data.size() - c->pos;
  const LONG n = static_cast<LONG>((std::min)(left, static_cast<size_t>(cb)));
  if (n > 0) {
    memcpy(buf, c->data.data() + c->pos, static_cast<size_t>(n));
  }
  c->pos += static_cast<size_t>(n);
  *pcb = n;
  return 0;
}

DWORD CALLBACK StreamOutCb(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb) {
  auto* c = reinterpret_cast<StreamCookie*>(cookie);
  c->data.append(reinterpret_cast<char*>(buf), reinterpret_cast<char*>(buf) + cb);
  *pcb = cb;
  return 0;
}

void ApplyCharEffect(HWND edit, DWORD mask, DWORD effect, bool on) {
  CHARFORMAT2W cf{};
  cf.cbSize = sizeof(cf);
  cf.dwMask = mask;
  cf.dwEffects = on ? effect : 0;
  SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
}

std::string EscapeJson(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      char buf[8];
      sprintf_s(buf, "\\u%04x", c);
      out += buf;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}
}  // namespace

bool NotesWindow::Create(HINSTANCE instance) {
  path_ = JoinPath(NativeDataDir(), L"notes.rtf");
  LoadLibraryW(L"Msftedit.dll");
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES};
  InitCommonControlsEx(&icc);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = NotesWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(255, 244, 170));
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  const int sw = GetSystemMetrics(SM_CXSCREEN);
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClassName, L"便签",
      WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, sw - 360, 100, 320,
      300, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 245, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

  HMENU menu = CreateMenu();
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, kMenuNewTab, L"新建标签\tCtrl+T");
  AppendMenuW(file, MF_STRING, kMenuCloseTab, L"关闭标签\tCtrl+W");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuSave, L"保存");
  AppendMenuW(file, MF_STRING, kMenuSaveAs, L"另存为…");
  AppendMenuW(file, MF_STRING, kMenuLibrary, L"笔记库…");
  AppendMenuW(file, MF_STRING, kMenuSearchLib, L"搜索笔记…");
  AppendMenuW(file, MF_STRING, kMenuPreview, L"Markdown 预览");
  AppendMenuW(file, MF_STRING, kMenuExportMd, L"导出 Markdown…");
  AppendMenuW(file, MF_STRING, kMenuExportHtml, L"导出 HTML…");
  AppendMenuW(file, MF_STRING, kMenuPandoc, L"用 Pandoc 导出 DOCX");
  AppendMenuW(file, MF_STRING, kMenuOpenWith, L"打开方式…");
  AppendMenuW(file, MF_STRING, kMenuPasteHtml, L"粘贴 HTML 为 Markdown");
  AppendMenuW(file, MF_STRING, kMenuToast, L"便签条模式");
  AppendMenuW(file, MF_STRING, kMenuWysiwyg, L"Toast WYSIWYG 编辑器");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuTpl, L"插入模板");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件");
  HMENU editm = CreatePopupMenu();
  AppendMenuW(editm, MF_STRING, kMenuFind, L"查找…\tCtrl+F");
  AppendMenuW(editm, MF_STRING, kMenuFindNext, L"查找下一个\tF3");
  AppendMenuW(editm, MF_STRING, kMenuFindPrev, L"查找上一个\tShift+F3");
  AppendMenuW(editm, MF_STRING, kMenuReplace, L"替换…\tCtrl+H");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(editm), L"编辑");
  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, kMenuBigger, L"放大字体");
  AppendMenuW(view, MF_STRING, kMenuSmaller, L"缩小字体");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kMenuOutline, L"标题大纲");
  AppendMenuW(view, MF_STRING, kMenuFocus, L"专注模式");
  AppendMenuW(view, MF_STRING, kMenuTypewriter, L"打字机滚动");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"视图");
  HMENU fmt = CreatePopupMenu();
  AppendMenuW(fmt, MF_STRING, kMenuBold, L"粗体");
  AppendMenuW(fmt, MF_STRING, kMenuItalic, L"斜体");
  AppendMenuW(fmt, MF_STRING, kMenuUnder, L"下划线");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fmt), L"格式");
  menu_ = menu;
  SetMenu(hwnd_, menu);
  find_msg_ = RegisterWindowMessageW(FINDMSGSTRING);

  tabs_ = CreateWindowExW(
      0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 300,
      28, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabsId)),
      instance, nullptr);

  edit_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
          ES_WANTRETURN | ES_SAVESEL,
      6, 6, 300, 220, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), instance, nullptr);
  if (!edit_) {
    edit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_WANTRETURN,
        6, 6, 300, 220, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), instance,
        nullptr);
  }
  if (!edit_) {
    return false;
  }
  edit_proc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(NotesWindow::EditProc)));
  SendMessageW(edit_, EM_SETBKGNDCOLOR, 0, RGB(255, 244, 170));
  SetFontSize(font_pt_);
  outline_list_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"LISTBOX", L"",
      WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 6, 6, 140, 220,
      hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOutlineId)), instance,
      nullptr);
  if (outline_list_) {
    ShowWindow(outline_list_, SW_HIDE);
  }
  web_.Create(hwnd_);
  LoadTabSession();
  if (tab_paths_.empty()) {
    tab_paths_.push_back(path_);
    tab_index_ = 0;
  }
  path_ = tab_paths_[static_cast<size_t>(tab_index_)];
  RebuildTabControl();
  Load();
  LayoutClient();
  return true;
}

void NotesWindow::SetFontSize(int pt) {
  font_pt_ = pt;
  if (font_) {
    DeleteObject(font_);
  }
  font_ = CreateFontW(font_pt_, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                      L"Segoe UI");
  if (edit_) {
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  }
}

void NotesWindow::Destroy() {
  Save();
  PersistTabs();
  web_.Destroy();
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void NotesWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

void NotesWindow::Hide() {
  Save();
  PersistTabs();
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

std::wstring NotesWindow::TabLabel(const std::wstring& path) const {
  const size_t slash = path.find_last_of(L"\\/");
  std::wstring name =
      slash == std::wstring::npos ? path : path.substr(slash + 1);
  if (name.empty()) {
    name = L"未命名";
  }
  if (name.size() > 18) {
    name = name.substr(0, 16) + L"…";
  }
  return name;
}

void NotesWindow::RebuildTabControl() {
  if (!tabs_) {
    return;
  }
  while (TabCtrl_GetItemCount(tabs_) > 0) {
    TabCtrl_DeleteItem(tabs_, 0);
  }
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    std::wstring label = TabLabel(tab_paths_[i]);
    ti.pszText = label.data();
    TabCtrl_InsertItem(tabs_, static_cast<int>(i), &ti);
  }
  if (tab_index_ < 0 ||
      tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = 0;
  }
  TabCtrl_SetCurSel(tabs_, tab_index_);
}

void NotesWindow::PersistTabs() {
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_tabs.json");
  std::ofstream out(file, std::ios::binary);
  if (!out) {
    return;
  }
  out << "{\n  \"current\": " << tab_index_ << ",\n  \"tabs\": [\n";
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    out << "    \"" << EscapeJson(NarrowUtf8(tab_paths_[i])) << "\"";
    if (i + 1 < tab_paths_.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
}

void NotesWindow::LoadTabSession() {
  tab_paths_.clear();
  tab_index_ = 0;
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_tabs.json");
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  size_t pos = 0;
  while (true) {
    const size_t q1 = json.find('"', pos);
    if (q1 == std::string::npos) {
      break;
    }
    // Prefer paths that look like absolute Windows paths inside the tabs array.
    const size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) {
      break;
    }
    const std::string raw = json.substr(q1 + 1, q2 - q1 - 1);
    pos = q2 + 1;
    if (raw == "tabs" || raw == "current") {
      continue;
    }
    std::string unescaped;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '\\' && i + 1 < raw.size()) {
        unescaped.push_back(raw[++i]);
      } else {
        unescaped.push_back(raw[i]);
      }
    }
    if (unescaped.size() >= 3 &&
        ((unescaped[1] == ':' && (unescaped[2] == '\\' || unescaped[2] == '/')) ||
         unescaped[0] == '\\')) {
      tab_paths_.push_back(WidenUtf8(unescaped));
    }
  }
  const size_t cur_key = json.find("\"current\"");
  if (cur_key != std::string::npos) {
    const size_t colon = json.find(':', cur_key);
    if (colon != std::string::npos) {
      tab_index_ = atoi(json.c_str() + colon + 1);
    }
  }
  if (tab_paths_.empty()) {
    tab_index_ = 0;
  } else if (tab_index_ < 0 ||
             tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = 0;
  }
}

void NotesWindow::SwitchToTab(int index, bool save_current) {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size())) {
    return;
  }
  if (save_current) {
    Save();
  }
  SyncFromWysiwyg();
  tab_index_ = index;
  path_ = tab_paths_[static_cast<size_t>(index)];
  if (tabs_) {
    TabCtrl_SetCurSel(tabs_, tab_index_);
  }
  Load();
  SetWindowTextW(hwnd_, (L"便签 · " + TabLabel(path_)).c_str());
  if (outline_) {
    RebuildOutline();
  }
  PersistTabs();
}

void NotesWindow::NewTab() {
  if (tab_paths_.size() >= 12) {
    MessageBoxW(hwnd_, L"最多 12 个标签。", L"便签", MB_OK | MB_ICONINFORMATION);
    return;
  }
  Save();
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf_s(name, L"note_%04d%02d%02d_%02d%02d%02d.md", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  const std::wstring path = JoinPath(NotesLibraryDir(true), name);
  {
    std::ofstream out(path, std::ios::binary);
    out << "";
  }
  tab_paths_.push_back(path);
  RebuildTabControl();
  SwitchToTab(static_cast<int>(tab_paths_.size()) - 1, false);
}

void NotesWindow::CloseTab() {
  if (tab_paths_.size() <= 1) {
    MessageBoxW(hwnd_, L"至少保留一个标签。", L"便签",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  Save();
  tab_paths_.erase(tab_paths_.begin() + tab_index_);
  if (tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
  }
  RebuildTabControl();
  SwitchToTab(tab_index_, false);
}

void NotesWindow::AdoptPath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (_wcsicmp(tab_paths_[i].c_str(), path.c_str()) == 0) {
      SwitchToTab(static_cast<int>(i), true);
      return;
    }
  }
  Save();
  if (tab_paths_.empty()) {
    tab_paths_.push_back(path);
  } else if (tab_index_ >= 0 &&
             tab_index_ < static_cast<int>(tab_paths_.size())) {
    tab_paths_[static_cast<size_t>(tab_index_)] = path;
  } else {
    tab_paths_.push_back(path);
    tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
  }
  path_ = path;
  RebuildTabControl();
  Load();
  SetWindowTextW(hwnd_, (L"便签 · " + TabLabel(path_)).c_str());
  PersistTabs();
}

bool NotesWindow::IsOpen() const {
  return hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
}

void NotesWindow::Load() {
  if (!edit_) {
    return;
  }
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    std::ifstream legacy(JoinPath(NativeDataDir(), L"notes.txt"), std::ios::binary);
    if (!legacy) {
      return;
    }
    std::ostringstream ss;
    ss << legacy.rdbuf();
    SetWindowTextW(edit_, WidenUtf8(ss.str()).c_str());
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string data = ss.str();
  if (data.rfind("{\\rtf", 0) == 0) {
    StreamCookie cookie;
    cookie.data = data;
    EDITSTREAM es{};
    es.dwCookie = reinterpret_cast<DWORD_PTR>(&cookie);
    es.pfnCallback = StreamInCb;
    SendMessageW(edit_, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&es));
  } else {
    SetWindowTextW(edit_, WidenUtf8(data).c_str());
  }
}

void NotesWindow::Save() {
  SyncFromWysiwyg();
  if (!edit_) {
    return;
  }
  if (path_.size() >= 3 &&
      (_wcsicmp(path_.c_str() + path_.size() - 3, L".md") == 0 ||
       (path_.size() >= 4 &&
        _wcsicmp(path_.c_str() + path_.size() - 4, L".txt") == 0))) {
    std::ofstream out(path_, std::ios::binary);
    if (out) {
      out << NarrowUtf8(CurrentPlainText());
    }
    return;
  }
  StreamCookie cookie;
  EDITSTREAM es{};
  es.dwCookie = reinterpret_cast<DWORD_PTR>(&cookie);
  es.pfnCallback = StreamOutCb;
  const LRESULT streamed =
      SendMessageW(edit_, EM_STREAMOUT, SF_RTF, reinterpret_cast<LPARAM>(&es));
  std::ofstream out(path_, std::ios::binary);
  if (!out) {
    return;
  }
  if (streamed && !cookie.data.empty()) {
    out << cookie.data;
    return;
  }
  const int len = GetWindowTextLengthW(edit_);
  std::wstring wide(static_cast<size_t>(len) + 1, L'\0');
  GetWindowTextW(edit_, wide.data(), len + 1);
  wide.resize(static_cast<size_t>(len));
  out << NarrowUtf8(wide);
}

std::wstring NotesWindow::CurrentPlainText() const {
  if (!edit_) {
    return {};
  }
  const int len = GetWindowTextLengthW(edit_);
  std::wstring wide(static_cast<size_t>(len) + 1, L'\0');
  GetWindowTextW(edit_, wide.data(), len + 1);
  wide.resize(static_cast<size_t>(len));
  return wide;
}

void NotesWindow::PreviewMarkdown() {
  Save();
  const bool is_md =
      path_.size() >= 3 &&
      (_wcsicmp(path_.c_str() + path_.size() - 3, L".md") == 0 ||
       _wcsicmp(path_.c_str() + path_.size() - 4, L".txt") == 0 ||
       (path_.size() >= 4 &&
        _wcsicmp(path_.c_str() + path_.size() - 4, L".markdown") == 0));
  if (is_md && path_.find(L".rtf") == std::wstring::npos) {
    if (PreviewMarkdownFile(hwnd_, path_)) {
      return;
    }
  }
  PreviewMarkdownText(hwnd_, CurrentPlainText(), L"便签预览");
}

void NotesWindow::ExportHtml() {
  wchar_t file[MAX_PATH] = L"note.html";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter = L"HTML\0*.html\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_OVERWRITEPROMPT;
  ofn.lpstrDefExt = L"html";
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }
  const std::wstring text = CurrentPlainText();
  std::string body;
  std::wstring line;
  auto flush = [&](const std::wstring& raw) {
    std::wstring src = raw;
    while (!src.empty() && (src.back() == L'\r' || src.back() == L'\n')) {
      src.pop_back();
    }
    int level = 0;
    while (level < 3 && level < static_cast<int>(src.size()) &&
           src[static_cast<size_t>(level)] == L'#') {
      ++level;
    }
    std::wstring content = src;
    if (level > 0 && static_cast<size_t>(level) < src.size() &&
        src[static_cast<size_t>(level)] == L' ') {
      content = src.substr(static_cast<size_t>(level + 1));
    } else if (level > 0) {
      level = 0;
    }
    std::string escaped;
    const std::string utf = NarrowUtf8(content);
    for (unsigned char c : utf) {
      if (c == '<') {
        escaped += "&lt;";
      } else if (c == '>') {
        escaped += "&gt;";
      } else if (c == '&') {
        escaped += "&amp;";
      } else {
        escaped.push_back(static_cast<char>(c));
      }
    }
    if (level > 0) {
      body += "<h" + std::to_string(level) + ">" + escaped + "</h" +
              std::to_string(level) + ">\n";
    } else if (escaped.empty()) {
      body += "<br>\n";
    } else {
      body += "<p>" + escaped + "</p>\n";
    }
  };
  for (wchar_t ch : text) {
    if (ch == L'\n') {
      flush(line);
      line.clear();
    } else {
      line.push_back(ch);
    }
  }
  flush(line);
  std::ofstream out(file, std::ios::binary);
  if (!out) {
    return;
  }
  out << "<!DOCTYPE html><meta charset=\"utf-8\"><title>DeskTidy</title>\n"
      << "<body style=\"font-family:sans-serif;max-width:42em;margin:2em auto\">\n"
      << body << "</body>\n";
}

void NotesWindow::ExportMarkdown() {
  wchar_t file[MAX_PATH] = L"note.md";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter = L"Markdown\0*.md\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_OVERWRITEPROMPT;
  ofn.lpstrDefExt = L"md";
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }
  std::ofstream out(file, std::ios::binary);
  if (out) {
    out << NarrowUtf8(CurrentPlainText());
  }
}

void NotesWindow::ExportWithPandoc() {
  wchar_t temp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp);
  const std::wstring src = std::wstring(temp) + L"desktidy_note.md";
  const std::wstring dst = std::wstring(temp) + L"desktidy_note.docx";
  {
    std::ofstream out(src, std::ios::binary);
    if (!out) {
      return;
    }
    out << NarrowUtf8(CurrentPlainText());
  }
  std::wstring cmd = L"pandoc \"" + src + L"\" -o \"" + dst + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    MessageBoxW(hwnd_, L"未找到 pandoc。安装后把它加入 PATH 再导出。", L"导出",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  WaitForSingleObject(pi.hProcess, 20000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  ShellExecuteW(nullptr, L"open", dst.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void NotesWindow::OpenWithShell() {
  Save();
  if (path_.empty()) {
    return;
  }
  const std::wstring cmd =
      L"rundll32.exe shell32.dll,OpenAs_RunDLL " + path_;
  ShellExecuteW(hwnd_, L"open", L"rundll32.exe",
                (L"shell32.dll,OpenAs_RunDLL \"" + path_ + L"\"").c_str(),
                nullptr, SW_SHOWNORMAL);
  (void)cmd;
}

void NotesWindow::PasteHtmlAsMarkdown() {
  const UINT fmt = RegisterClipboardFormatW(L"HTML Format");
  if (!fmt || !OpenClipboard(hwnd_)) {
    return;
  }
  HANDLE data = GetClipboardData(fmt);
  std::string html;
  if (data) {
    const char* text = static_cast<const char*>(GlobalLock(data));
    if (text) {
      html = text;
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  if (html.empty()) {
    MessageBoxW(hwnd_, L"剪贴板里没有 HTML。", L"便签", MB_OK);
    return;
  }
  const auto start = html.find("<!--StartFragment-->");
  const auto end = html.find("<!--EndFragment-->");
  std::string frag = (start != std::string::npos && end != std::string::npos &&
                      end > start)
                         ? html.substr(start + 20, end - (start + 20))
                         : html;
  std::string md;
  for (size_t i = 0; i < frag.size(); ++i) {
    if (frag.compare(i, 4, "<br>") == 0 || frag.compare(i, 5, "<br/>") == 0 ||
        frag.compare(i, 3, "<p>") == 0 || frag.compare(i, 4, "</p>") == 0) {
      md.push_back('\n');
      while (i < frag.size() && frag[i] != '>') {
        ++i;
      }
      continue;
    }
    if (frag[i] == '<') {
      while (i < frag.size() && frag[i] != '>') {
        ++i;
      }
      continue;
    }
    if (frag[i] == '&') {
      if (frag.compare(i, 5, "&amp;") == 0) {
        md.push_back('&');
        i += 4;
        continue;
      }
      if (frag.compare(i, 4, "&lt;") == 0) {
        md.push_back('<');
        i += 3;
        continue;
      }
    }
    md.push_back(frag[i]);
  }
  const std::wstring wide = WidenUtf8(md);
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(wide.c_str()));
}

void NotesWindow::ToggleToast() {
  toast_ = !toast_;
  if (toast_) {
    SetWindowPos(hwnd_, HWND_TOPMOST, 40, 40, 280, 140, SWP_SHOWWINDOW);
    SetWindowTextW(hwnd_, L"便签条");
  } else {
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 80, 80, 320, 300, SWP_SHOWWINDOW);
    SetWindowTextW(hwnd_, L"便签");
  }
}

void NotesWindow::SyncFromWysiwyg() {
  if (!wysiwyg_ || !web_.Ready()) {
    return;
  }
  const std::wstring md = web_.PullMarkdown();
  if (edit_) {
    SetWindowTextW(edit_, md.c_str());
  }
}

void NotesWindow::ToggleWysiwyg() {
  if (!web_.Available()) {
    const DWORD start = GetTickCount();
    MSG msg{};
    while (!web_.Available() && GetTickCount() - start < 4000) {
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      Sleep(15);
    }
  }
  if (!web_.Available()) {
    MessageBoxW(hwnd_,
                L"无法启动 WebView2。请安装 Microsoft Edge WebView2 Runtime，"
                L"并确认 assets\\md_preview\\wysiwyg_native.html 存在。",
                L"Toast 编辑器", MB_OK | MB_ICONWARNING);
    return;
  }
  wysiwyg_ = !wysiwyg_;
  if (wysiwyg_) {
    const std::wstring text = CurrentPlainText();
    web_.Show();
    web_.Resize();
    web_.SetMarkdown(text);
    if (edit_) {
      ShowWindow(edit_, SW_HIDE);
    }
    SetWindowTextW(hwnd_, L"便签 · Toast WYSIWYG");
    SetWindowPos(hwnd_, nullptr, 0, 0, 720, 560,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW);
  } else {
    SyncFromWysiwyg();
    web_.Hide();
    if (edit_) {
      ShowWindow(edit_, SW_SHOW);
    }
    SetWindowTextW(hwnd_, toast_ ? L"便签条" : L"便签");
  }
  LayoutClient();
}

void NotesWindow::SearchLibrary() {
  const std::wstring picked = SearchNotesInLibrary(hwnd_);
  if (picked.empty()) {
    return;
  }
  AdoptPath(picked);
}

void NotesWindow::OpenLibrary() {
  const std::wstring picked = PickNoteFromLibrary(hwnd_);
  if (picked.empty()) {
    return;
  }
  if (GetFileAttributesW(picked.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::ofstream out(picked, std::ios::binary);
    out << "";
  }
  AdoptPath(picked);
}

void NotesWindow::SaveAs() {
  wchar_t file[MAX_PATH] = {};
  wcsncpy_s(file, path_.c_str(), _TRUNCATE);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Text\0*.txt;*.md\0All\0*.*\0";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }
  AdoptPath(file);
  Save();
}

void NotesWindow::FindAgain(bool down) {
  if (!edit_ || find_what_[0] == 0) {
    ShowFind(false);
    return;
  }
  FINDREPLACEW fr = find_;
  fr.Flags = (down ? FR_DOWN : 0) | (find_.Flags & (FR_MATCHCASE | FR_WHOLEWORD));
  fr.lpstrFindWhat = find_what_;
  HandleFindMsg(&fr);
}

void NotesWindow::LayoutClient() {
  if (!hwnd_ || !edit_) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  int top = 6;
  if (tabs_ && !toast_) {
    MoveWindow(tabs_, 0, 0, rc.right, 28, TRUE);
    ShowWindow(tabs_, SW_SHOW);
    top = 32;
  } else if (tabs_) {
    ShowWindow(tabs_, SW_HIDE);
  }
  int left = 6;
  int right = rc.right - 6;
  int bottom = rc.bottom - 6;
  if (outline_ && outline_list_ && !wysiwyg_) {
    const int side = 150;
    MoveWindow(outline_list_, 6, top, side, bottom - top, TRUE);
    ShowWindow(outline_list_, SW_SHOW);
    left = 6 + side + 6;
  } else if (outline_list_) {
    ShowWindow(outline_list_, SW_HIDE);
  }
  if (focus_ && !wysiwyg_) {
    const int pad_x =
        (std::max)(24, (static_cast<int>(rc.right) - 520) / 2);
    left = (std::max)(left, pad_x);
    right = static_cast<int>(rc.right) - pad_x;
    top = (std::max)(top, top + 12);
    bottom = static_cast<int>(rc.bottom) - 28;
  }
  if (wysiwyg_) {
    if (web_.Host()) {
      MoveWindow(web_.Host(), left, top, (std::max)(40, right - left),
                 (std::max)(40, bottom - top), TRUE);
    } else {
      web_.Resize();
    }
  } else {
    MoveWindow(edit_, left, top, (std::max)(40, right - left),
               (std::max)(40, bottom - top), TRUE);
  }
}

void NotesWindow::RebuildOutline() {
  if (!outline_list_ || !edit_) {
    return;
  }
  SendMessageW(outline_list_, LB_RESETCONTENT, 0, 0);
  const std::wstring text = CurrentPlainText();
  int line = 0;
  size_t i = 0;
  while (i <= text.size()) {
    size_t end = text.find(L'\n', i);
    if (end == std::wstring::npos) {
      end = text.size();
    }
    std::wstring raw = text.substr(i, end - i);
    while (!raw.empty() && raw.back() == L'\r') {
      raw.pop_back();
    }
    int level = 0;
    while (level < 6 && level < static_cast<int>(raw.size()) &&
           raw[static_cast<size_t>(level)] == L'#') {
      ++level;
    }
    if (level > 0 && static_cast<size_t>(level) < raw.size() &&
        raw[static_cast<size_t>(level)] == L' ') {
      std::wstring title(level, L' ');
      title += raw.substr(static_cast<size_t>(level + 1));
      const int idx = static_cast<int>(SendMessageW(
          outline_list_, LB_ADDSTRING, 0,
          reinterpret_cast<LPARAM>(title.c_str())));
      if (idx >= 0) {
        SendMessageW(outline_list_, LB_SETITEMDATA, idx, line);
      }
    }
    ++line;
    if (end >= text.size()) {
      break;
    }
    i = end + 1;
  }
}

void NotesWindow::JumpToOutline() {
  if (!outline_list_ || !edit_) {
    return;
  }
  const int sel =
      static_cast<int>(SendMessageW(outline_list_, LB_GETCURSEL, 0, 0));
  if (sel < 0) {
    return;
  }
  const int line = static_cast<int>(
      SendMessageW(outline_list_, LB_GETITEMDATA, sel, 0));
  const LRESULT index =
      SendMessageW(edit_, EM_LINEINDEX, static_cast<WPARAM>(line), 0);
  if (index < 0) {
    return;
  }
  CHARRANGE cr{};
  cr.cpMin = static_cast<LONG>(index);
  cr.cpMax = static_cast<LONG>(index);
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
  if (typewriter_) {
    CenterCaret();
  }
  SetFocus(edit_);
}

void NotesWindow::ToggleOutline() {
  outline_ = !outline_;
  if (menu_) {
    CheckMenuItem(menu_, kMenuOutline,
                  MF_BYCOMMAND | (outline_ ? MF_CHECKED : MF_UNCHECKED));
  }
  if (outline_) {
    RebuildOutline();
  }
  LayoutClient();
}

void NotesWindow::ToggleFocus() {
  focus_ = !focus_;
  if (menu_) {
    CheckMenuItem(menu_, kMenuFocus,
                  MF_BYCOMMAND | (focus_ ? MF_CHECKED : MF_UNCHECKED));
  }
  LayoutClient();
}

void NotesWindow::ToggleTypewriter() {
  typewriter_ = !typewriter_;
  if (menu_) {
    CheckMenuItem(menu_, kMenuTypewriter,
                  MF_BYCOMMAND | (typewriter_ ? MF_CHECKED : MF_UNCHECKED));
  }
  if (typewriter_) {
    CenterCaret();
  }
}

void NotesWindow::CenterCaret() {
  if (!edit_) {
    return;
  }
  CHARRANGE sel{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
  const LRESULT line =
      SendMessageW(edit_, EM_LINEFROMCHAR, static_cast<WPARAM>(sel.cpMin), 0);
  RECT rc{};
  GetClientRect(edit_, &rc);
  TEXTMETRICW tm{};
  HDC hdc = GetDC(edit_);
  HFONT old = nullptr;
  if (font_) {
    old = static_cast<HFONT>(SelectObject(hdc, font_));
  }
  GetTextMetricsW(hdc, &tm);
  if (old) {
    SelectObject(hdc, old);
  }
  ReleaseDC(edit_, hdc);
  const int visible =
      (std::max)(1, (static_cast<int>(rc.bottom - rc.top)) /
                        (std::max)(1, static_cast<int>(tm.tmHeight)));
  const int first = static_cast<int>(
      SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0));
  const int target = static_cast<int>(line) - visible / 2;
  const int delta = target - first;
  if (delta != 0) {
    SendMessageW(edit_, EM_LINESCROLL, 0, delta);
  }
}

void NotesWindow::ShowFind(bool replace) {
  if (find_dlg_ && IsWindow(find_dlg_)) {
    SetForegroundWindow(find_dlg_);
    return;
  }
  ZeroMemory(&find_, sizeof(find_));
  find_.lStructSize = sizeof(find_);
  find_.hwndOwner = hwnd_;
  find_.lpstrFindWhat = find_what_;
  find_.wFindWhatLen = 256;
  find_.lpstrReplaceWith = replace_with_;
  find_.wReplaceWithLen = 256;
  find_.Flags = FR_DOWN;
  find_dlg_ = replace ? ReplaceTextW(&find_) : FindTextW(&find_);
}

void NotesWindow::HandleFindMsg(LPFINDREPLACEW fr) {
  if (!fr || !edit_) {
    return;
  }
  if (fr->Flags & FR_DIALOGTERM) {
    find_dlg_ = nullptr;
    return;
  }
  const bool down = (fr->Flags & FR_DOWN) != 0;
  const bool match_case = (fr->Flags & FR_MATCHCASE) != 0;
  const bool whole = (fr->Flags & FR_WHOLEWORD) != 0;
  DWORD flags = FR_DOWN;
  if (match_case) {
    flags |= FR_MATCHCASE;
  }
  if (whole) {
    flags |= FR_WHOLEWORD;
  }

  auto find_once = [&](bool search_down) -> bool {
    FINDTEXTEXW ft{};
    ft.chrg.cpMin = 0;
    ft.chrg.cpMax = -1;
    CHARRANGE sel{};
    SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    if (search_down) {
      ft.chrg.cpMin = sel.cpMax;
    } else {
      flags &= ~FR_DOWN;
      ft.chrg.cpMin = 0;
      ft.chrg.cpMax = sel.cpMin;
    }
    ft.lpstrText = fr->lpstrFindWhat;
    const LRESULT hit = SendMessageW(
        edit_, EM_FINDTEXTEXW, flags | (search_down ? FR_DOWN : 0),
        reinterpret_cast<LPARAM>(&ft));
    if (hit < 0) {
      return false;
    }
    SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&ft.chrgText));
    SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    return true;
  };

  if (fr->Flags & FR_REPLACEALL) {
    int n = 0;
    CHARRANGE top{};
    top.cpMin = 0;
    top.cpMax = 0;
    SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&top));
    while (find_once(true)) {
      SendMessageW(edit_, EM_REPLACESEL, TRUE,
                   reinterpret_cast<LPARAM>(fr->lpstrReplaceWith));
      ++n;
    }
    wchar_t buf[64];
    swprintf_s(buf, L"已替换 %d 处。", n);
    MessageBoxW(hwnd_, buf, L"替换", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (fr->Flags & FR_REPLACE) {
    CHARRANGE sel{};
    SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    if (sel.cpMax > sel.cpMin) {
      SendMessageW(edit_, EM_REPLACESEL, TRUE,
                   reinterpret_cast<LPARAM>(fr->lpstrReplaceWith));
    }
  }
  if (!find_once(down)) {
    MessageBoxW(hwnd_, L"未找到。", L"查找", MB_OK | MB_ICONINFORMATION);
  }
}

void NotesWindow::InsertTemplate() {
  if (!edit_) {
    return;
  }
  const wchar_t* tpl =
      L"\r\n---\r\n# 笔记\r\n\r\n- 要点：\r\n- 待办：\r\n- 链接：\r\n";
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(tpl));
}

LRESULT CALLBACK NotesWindow::EditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                        LPARAM lparam) {
  auto* self = reinterpret_cast<NotesWindow*>(
      GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
  if (self && msg == WM_KEYDOWN) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (wparam == VK_F3) {
      self->FindAgain(!shift);
      return 0;
    }
    if (ctrl && (wparam == 'F' || wparam == 'f')) {
      self->ShowFind(false);
      return 0;
    }
    if (ctrl && (wparam == 'H' || wparam == 'h')) {
      self->ShowFind(true);
      return 0;
    }
    if (ctrl && (wparam == 'T' || wparam == 't')) {
      self->NewTab();
      return 0;
    }
    if (ctrl && (wparam == 'W' || wparam == 'w')) {
      self->CloseTab();
      return 0;
    }
  }
  LRESULT result = 0;
  if (self && self->edit_proc_) {
    result = CallWindowProcW(self->edit_proc_, hwnd, msg, wparam, lparam);
  } else {
    result = DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (self && self->typewriter_ &&
      (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_KEYUP)) {
    self->CenterCaret();
  }
  return result;
}

LRESULT CALLBACK NotesWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
  NotesWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<NotesWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<NotesWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT NotesWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  if (find_msg_ && msg == find_msg_) {
    HandleFindMsg(reinterpret_cast<LPFINDREPLACEW>(lparam));
    return 0;
  }
  switch (msg) {
    case WM_NOTIFY: {
      const auto* hdr = reinterpret_cast<LPNMHDR>(lparam);
      if (hdr && hdr->idFrom == kTabsId && hdr->code == TCN_SELCHANGE) {
        const int sel = TabCtrl_GetCurSel(tabs_);
        if (sel != tab_index_) {
          SwitchToTab(sel, true);
        }
        return 0;
      }
      break;
    }
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kMenuNewTab:
          NewTab();
          return 0;
        case kMenuCloseTab:
          CloseTab();
          return 0;
        case kMenuSave:
          Save();
          return 0;
        case kMenuSaveAs:
          SaveAs();
          return 0;
        case kMenuLibrary:
          OpenLibrary();
          return 0;
        case kMenuSearchLib:
          SearchLibrary();
          return 0;
        case kMenuPreview:
          PreviewMarkdown();
          return 0;
        case kMenuExportMd:
          ExportMarkdown();
          return 0;
        case kMenuExportHtml:
          ExportHtml();
          return 0;
        case kMenuPandoc:
          ExportWithPandoc();
          return 0;
        case kMenuOpenWith:
          OpenWithShell();
          return 0;
        case kMenuPasteHtml:
          PasteHtmlAsMarkdown();
          return 0;
        case kMenuToast:
          ToggleToast();
          return 0;
        case kMenuWysiwyg:
          ToggleWysiwyg();
          return 0;
        case kMenuFind:
          ShowFind(false);
          return 0;
        case kMenuFindNext:
          FindAgain(true);
          return 0;
        case kMenuFindPrev:
          FindAgain(false);
          return 0;
        case kMenuReplace:
          ShowFind(true);
          return 0;
        case kMenuBigger:
          SetFontSize(font_pt_ + 2);
          return 0;
        case kMenuSmaller:
          if (font_pt_ > 10) {
            SetFontSize(font_pt_ - 2);
          }
          return 0;
        case kMenuOutline:
          ToggleOutline();
          return 0;
        case kMenuFocus:
          ToggleFocus();
          return 0;
        case kMenuTypewriter:
          ToggleTypewriter();
          return 0;
        case kMenuTpl:
          InsertTemplate();
          return 0;
        case kMenuBold:
          ApplyCharEffect(edit_, CFM_BOLD, CFE_BOLD, true);
          return 0;
        case kMenuItalic:
          ApplyCharEffect(edit_, CFM_ITALIC, CFE_ITALIC, true);
          return 0;
        case kMenuUnder:
          ApplyCharEffect(edit_, CFM_UNDERLINE, CFE_UNDERLINE, true);
          return 0;
        default:
          break;
      }
      if (LOWORD(wparam) == kOutlineId && HIWORD(wparam) == LBN_SELCHANGE) {
        JumpToOutline();
        return 0;
      }
      break;
    case WM_SIZE:
      LayoutClient();
      return 0;
    case WM_CLOSE:
      SyncFromWysiwyg();
      Hide();
      return 0;
    case WM_DESTROY:
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}
