#include "app.h"

#include "desktop_shell.h"

#include <CommCtrl.h>
#include <Shellapi.h>

namespace {
constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kHotkeySettings = 1;
constexpr wchar_t kMsgClass[] = L"DeskTidyNativeMsg";
}  // namespace

bool App::Init(HINSTANCE instance) {
  instance_ = instance;
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = App::TrayWndProc;
  wc.hInstance = instance;
  wc.lpszClassName = kMsgClass;
  RegisterClassExW(&wc);

  message_hwnd_ =
      CreateWindowExW(0, kMsgClass, L"DeskTidyNative", 0, 0, 0, 0, 0,
                      HWND_MESSAGE, nullptr, instance, this);
  if (!message_hwnd_) {
    return false;
  }
  SetWindowLongPtrW(message_hwnd_, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(this));

  // Register Ctrl+Shift+S -> settings (MVP; avoids hunting a tray).
  RegisterHotKey(message_hwnd_, kHotkeySettings, MOD_CONTROL | MOD_SHIFT, 'S');

  const int screen_w = GetSystemMetrics(SM_CXSCREEN);
  const int fence_w = 280;
  const int fence_h = 360;
  const int x = screen_w - fence_w - 40;
  const int y = 40;

  if (!fence_.Create(instance, x, y, fence_w, fence_h, L"软件")) {
    return false;
  }
  if (!settings_.Create(instance)) {
    return false;
  }

  CreateTrayIcon();
  fence_.Show();
  return true;
}

int App::Run() {
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}

void App::Shutdown() {
  RemoveTrayIcon();
  if (message_hwnd_) {
    UnregisterHotKey(message_hwnd_, kHotkeySettings);
    DestroyWindow(message_hwnd_);
    message_hwnd_ = nullptr;
  }
  fence_.Destroy();
  settings_.Destroy();
}

void App::OpenSettings() {
  // Fences layer: seat fence under apps, then raise settings APPWINDOW.
  SeatFenceUnderApps();
  settings_.Show();
}


void App::SeatFenceUnderApps() {
  HWND hwnd = fence_.Hwnd();
  if (!hwnd) {
    return;
  }
  AttachOverlayToDesktop(hwnd, true);
  PlaceOverlayInDesktopBand(hwnd, true);
}

void App::CreateTrayIcon() {
  ZeroMemory(&nid_, sizeof(nid_));
  nid_.cbSize = sizeof(nid_);
  nid_.hWnd = message_hwnd_;
  nid_.uID = 1;
  nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid_.uCallbackMessage = kTrayMsg;
  nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcsncpy_s(nid_.szTip, L"DeskTidy Native (C++ MVP)", _TRUNCATE);
  tray_added_ = Shell_NotifyIconW(NIM_ADD, &nid_) == TRUE;
}

void App::RemoveTrayIcon() {
  if (tray_added_) {
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    tray_added_ = false;
  }
}

LRESULT CALLBACK App::TrayWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam) {
  App* self =
      reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<App*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  switch (msg) {
    case kTrayMsg:
      if (lparam == WM_LBUTTONDBLCLK || lparam == WM_LBUTTONUP) {
        self->OpenSettings();
        return 0;
      }
      if (lparam == WM_RBUTTONUP) {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"Settings");
        AppendMenuW(menu, MF_STRING, 2, L"Seat fence under apps");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"Exit");
        SetForegroundWindow(hwnd);
        const int cmd =
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
                           hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd == 1) {
          self->OpenSettings();
        } else if (cmd == 2) {
          self->SeatFenceUnderApps();
        } else if (cmd == 3) {
          PostQuitMessage(0);
        }
        return 0;
      }
      break;
    case WM_HOTKEY:
      if (wparam == kHotkeySettings) {
        self->OpenSettings();
        return 0;
      }
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}
