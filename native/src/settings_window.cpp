#include "settings_window.h"

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeSettings";
constexpr int kBtnAdd = 1001;
constexpr int kBtnRemove = 1002;
constexpr int kBtnSeat = 1003;
constexpr int kListId = 1100;
}  // namespace

bool SettingsWindow::Create(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = SettingsWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  hwnd_ = CreateWindowExW(
      WS_EX_APPWINDOW, kClassName, L"DeskTidy Native - Settings",
      WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, CW_USEDEFAULT,
      CW_USEDEFAULT, 460, 420, nullptr, nullptr, instance, this);

  if (!hwnd_) {
    return false;
  }

  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

  CreateWindowW(L"STATIC",
                L"Fences desktop-layer MVP\r\n"
                L"Settings is a normal APPWINDOW; fences stay underneath.\r\n"
                L"Hotkey: Ctrl+Shift+S",
                WS_CHILD | WS_VISIBLE, 20, 16, 400, 56, hwnd_, nullptr, instance,
                nullptr);

  CreateWindowW(L"STATIC", L"Fences", WS_CHILD | WS_VISIBLE, 20, 80, 100, 20,
                hwnd_, nullptr, instance, nullptr);

  list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                          WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 20,
                          104, 400, 180, hwnd_,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
                          instance, nullptr);

  btn_add_ = CreateWindowW(
      L"BUTTON", L"Add fence", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 300,
      120, 32, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnAdd)),
      instance, nullptr);
  btn_remove_ = CreateWindowW(
      L"BUTTON", L"Remove selected", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 150,
      300, 140, 32, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnRemove)), instance,
      nullptr);
  btn_seat_ = CreateWindowW(
      L"BUTTON", L"Seat under apps", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 300,
      300, 120, 32, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnSeat)),
      instance, nullptr);
  return true;
}

void SettingsWindow::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void SettingsWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
  SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

void SettingsWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

bool SettingsWindow::IsOpen() const {
  return hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
}

void SettingsWindow::SetFenceNames(const std::vector<std::wstring>& names) {
  if (!list_) {
    return;
  }
  SendMessageW(list_, LB_RESETCONTENT, 0, 0);
  for (const auto& name : names) {
    SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
  }
}

void SettingsWindow::SetHandlers(VoidFn on_add, IndexFn on_remove,
                                 VoidFn on_seat) {
  on_add_ = std::move(on_add);
  on_remove_ = std::move(on_remove);
  on_seat_ = std::move(on_seat);
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam) {
  SettingsWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<SettingsWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT SettingsWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      if (id == kBtnAdd && on_add_) {
        on_add_();
        return 0;
      }
      if (id == kBtnRemove && on_remove_ && list_) {
        const int sel =
            static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
        if (sel >= 0) {
          on_remove_(sel);
        }
        return 0;
      }
      if (id == kBtnSeat && on_seat_) {
        on_seat_();
        return 0;
      }
      break;
    }
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
