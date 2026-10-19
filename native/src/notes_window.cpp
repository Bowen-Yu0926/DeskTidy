#include "notes_window.h"

#include "desktop_shell.h"
#include "paths_util.h"

#include <commdlg.h>
#include <ShlObj.h>

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
}  // namespace

bool NotesWindow::Create(HINSTANCE instance) {
  path_ = JoinPath(NativeDataDir(), L"notes.txt");

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
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuTpl, L"插入模板");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件");
  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, kMenuBigger, L"放大字体");
  AppendMenuW(view, MF_STRING, kMenuSmaller, L"缩小字体");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"视图");
  SetMenu(hwnd_, menu);

  edit_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
          ES_WANTRETURN,
      6, 6, 300, 220, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), instance, nullptr);
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
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  SetWindowTextW(edit_, WidenUtf8(ss.str()).c_str());
}

void NotesWindow::Save() {
  if (!edit_) {
    return;
  }
  const int len = GetWindowTextLengthW(edit_);
  std::wstring wide(static_cast<size_t>(len) + 1, L'\0');
  GetWindowTextW(edit_, wide.data(), len + 1);
  wide.resize(static_cast<size_t>(len));
  std::ofstream out(path_, std::ios::binary);
  if (out) {
    out << NarrowUtf8(wide);
  }
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
  switch (msg) {
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kMenuSave:
          Save();
          return 0;
        case kMenuSaveAs:
          SaveAs();
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
