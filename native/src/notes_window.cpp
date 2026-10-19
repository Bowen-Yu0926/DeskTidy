#include "notes_window.h"

#include "desktop_shell.h"
#include "markdown_preview.h"
#include "notes_library.h"
#include "paths_util.h"

#include <commdlg.h>
#include <Richedit.h>
#include <Shellapi.h>
#include <ShlObj.h>

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
}  // namespace

bool NotesWindow::Create(HINSTANCE instance) {
  path_ = JoinPath(NativeDataDir(), L"notes.rtf");
  LoadLibraryW(L"Msftedit.dll");

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
  AppendMenuW(file, MF_STRING, kMenuSave, L"保存");
  AppendMenuW(file, MF_STRING, kMenuSaveAs, L"另存为…");
  AppendMenuW(file, MF_STRING, kMenuLibrary, L"笔记库…");
  AppendMenuW(file, MF_STRING, kMenuSearchLib, L"搜索笔记…");
  AppendMenuW(file, MF_STRING, kMenuPreview, L"Markdown 预览");
  AppendMenuW(file, MF_STRING, kMenuExportMd, L"导出 Markdown…");
  AppendMenuW(file, MF_STRING, kMenuPandoc, L"用 Pandoc 导出 DOCX");
  AppendMenuW(file, MF_STRING, kMenuOpenWith, L"打开方式…");
  AppendMenuW(file, MF_STRING, kMenuPasteHtml, L"粘贴 HTML 为 Markdown");
  AppendMenuW(file, MF_STRING, kMenuToast, L"便签条模式");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuTpl, L"插入模板");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件");
  HMENU editm = CreatePopupMenu();
  AppendMenuW(editm, MF_STRING, kMenuFind, L"查找…\tCtrl+F");
  AppendMenuW(editm, MF_STRING, kMenuReplace, L"替换…\tCtrl+H");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(editm), L"编辑");
  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, kMenuBigger, L"放大字体");
  AppendMenuW(view, MF_STRING, kMenuSmaller, L"缩小字体");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"视图");
  HMENU fmt = CreatePopupMenu();
  AppendMenuW(fmt, MF_STRING, kMenuBold, L"粗体");
  AppendMenuW(fmt, MF_STRING, kMenuItalic, L"斜体");
  AppendMenuW(fmt, MF_STRING, kMenuUnder, L"下划线");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fmt), L"格式");
  SetMenu(hwnd_, menu);
  find_msg_ = RegisterWindowMessageW(FINDMSGSTRING);

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
  SendMessageW(edit_, EM_SETBKGNDCOLOR, 0, RGB(255, 244, 170));
  SetFontSize(font_pt_);
  Load();
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
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
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
  if (!edit_) {
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

void NotesWindow::SearchLibrary() {
  Save();
  const std::wstring picked = SearchNotesInLibrary(hwnd_);
  if (picked.empty()) {
    return;
  }
  path_ = picked;
  Load();
  SetWindowTextW(hwnd_, (L"便签 · " + path_).c_str());
}

void NotesWindow::OpenLibrary() {
  Save();
  const std::wstring picked = PickNoteFromLibrary(hwnd_);
  if (picked.empty()) {
    return;
  }
  path_ = picked;
  if (GetFileAttributesW(path_.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::ofstream out(path_, std::ios::binary);
    out << "";
  }
  Load();
  SetWindowTextW(hwnd_, (L"便签 · " + path_).c_str());
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
  path_ = file;
  Save();
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
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
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
        case kMenuFind:
          ShowFind(false);
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
      break;
    case WM_SIZE:
      if (edit_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        MoveWindow(edit_, 6, 6, rc.right - 12, rc.bottom - 12, TRUE);
      }
      return 0;
    case WM_CLOSE:
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
