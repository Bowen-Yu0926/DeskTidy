#include "settings_window.h"

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeSettings";
constexpr int kBtnReassert = 1001;
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
      WS_EX_APPWINDOW, kClassName, L"DeskTidy Native — Settings",
      WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
      CW_USEDEFAULT, CW_USEDEFAULT, 420, 280, nullptr, nullptr, instance, this);

  if (!hwnd_) {
    return false;
  }

  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

  CreateWindowW(L"STATIC",
                L"Fences desktop-layer MVP\r\n\r\n"
                L"This settings window is a normal APPWINDOW.\r\n"
                L"The fence overlay must stay underneath it.\r\n\r\n"
                L"If the fence covers this window, Z-order is wrong.",
                WS_CHILD | WS_VISIBLE, 24, 24, 360, 120, hwnd_, nullptr,
                instance, nullptr);

  button_ = CreateWindowW(L"BUTTON", L"Bring settings to front",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 24, 160, 200,
                          32, hwnd_, reinterpret_cast<HMENU>(kBtnReassert),
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
  // Win32 raise above desktop-band overlays (Qt raise_ alone is not enough).
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
    case WM_COMMAND:
      if (LOWORD(wparam) == kBtnReassert) {
        Show();
        return 0;
      }
      break;
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
