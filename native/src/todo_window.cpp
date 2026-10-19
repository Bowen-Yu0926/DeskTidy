#include "todo_window.h"

#include "desktop_shell.h"
#include "paths_util.h"

#include <fstream>
#include <sstream>
#include <string>

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeTodo";
constexpr int kEdit = 3001;
}  // namespace

bool TodoWindow::Create(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = TodoWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(250, 250, 252));
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClass, L"待办",
      WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, 40, 200, 280, 320,
      nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 245, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  edit_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
          ES_WANTRETURN,
      6, 6, 250, 260, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEdit)), instance, nullptr);
  Load();
  return true;
}

void TodoWindow::Destroy() {
  Save();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void TodoWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

void TodoWindow::Hide() {
  Save();
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void TodoWindow::Load() {
  if (!edit_) {
    return;
  }
  std::ifstream in(JoinPath(NativeDataDir(), L"todos.txt"), std::ios::binary);
  if (!in) {
    SetWindowTextW(edit_, L"- 示例待办\r\n- ");
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  SetWindowTextW(edit_, WidenUtf8(ss.str()).c_str());
}

void TodoWindow::Save() {
  if (!edit_) {
    return;
  }
  const int len = GetWindowTextLengthW(edit_);
  std::wstring wide(static_cast<size_t>(len) + 1, L'\0');
  GetWindowTextW(edit_, wide.data(), len + 1);
  wide.resize(static_cast<size_t>(len));
  std::ofstream out(JoinPath(NativeDataDir(), L"todos.txt"), std::ios::binary);
  if (out) {
    out << NarrowUtf8(wide);
  }
}

LRESULT CALLBACK TodoWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  TodoWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<TodoWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<TodoWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT TodoWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
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
