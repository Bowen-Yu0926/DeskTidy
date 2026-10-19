#include "app.h"

#include "desktop_items.h"
#include "desktop_shell.h"

#include <CommCtrl.h>
#include <Shellapi.h>

#include <utility>

namespace {
constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kHotkeySettings = 1;
constexpr wchar_t kMsgClass[] = L"DeskTidyNativeMsg";
}  // namespace

bool App::SpawnFence(const FenceLayout& layout) {
  auto fence = std::make_unique<FenceWindow>();
  if (!fence->Create(instance_, layout)) {
    return false;
  }
  fence->SetOnMoved([this](FenceWindow*) { PersistLayouts(); });
  fence->Show();
  fences_.push_back(std::move(fence));
  return true;
}

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

  RegisterHotKey(message_hwnd_, kHotkeySettings, MOD_CONTROL | MOD_SHIFT, 'S');

  if (!settings_.Create(instance)) {
    return false;
  }
  settings_.SetHandlers(
      [this]() { AddFence(); },
      [this](int index) { RemoveFenceAt(index); },
      [this]() {
        SeatAllFencesUnderApps();
        settings_.Show();
      });

  const auto layouts = LoadLayouts();
  for (const auto& layout : layouts) {
    if (!SpawnFence(layout)) {
      return false;
    }
  }
  PersistLayouts();
  RefreshDesktopIcons();
  RefreshSettingsList();

  CreateTrayIcon();
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
  PersistLayouts();
  RemoveTrayIcon();
  if (message_hwnd_) {
    UnregisterHotKey(message_hwnd_, kHotkeySettings);
    DestroyWindow(message_hwnd_);
    message_hwnd_ = nullptr;
  }
  for (auto& fence : fences_) {
    if (fence) {
      fence->Destroy();
    }
  }
  fences_.clear();
  settings_.Destroy();
}

void App::OpenSettings() {
  SeatAllFencesUnderApps();
  RefreshSettingsList();
  settings_.Show();
}

void App::SeatAllFencesUnderApps() {
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd()) {
      continue;
    }
    AttachOverlayToDesktop(fence->Hwnd(), true);
    PlaceOverlayInDesktopBand(fence->Hwnd(), true);
  }
}

void App::PersistLayouts() {
  std::vector<FenceLayout> layouts;
  layouts.reserve(fences_.size());
  for (const auto& fence : fences_) {
    if (fence) {
      layouts.push_back(fence->CurrentLayout());
    }
  }
  SaveLayouts(layouts);
}

void App::RefreshSettingsList() {
  std::vector<std::wstring> names;
  names.reserve(fences_.size());
  for (const auto& fence : fences_) {
    if (fence) {
      names.push_back(fence->Title() + L"  [" +
                      std::wstring(fence->Id().begin(), fence->Id().end()) +
                      L"]");
    }
  }
  settings_.SetFenceNames(names);
}

void App::RefreshDesktopIcons() {
  const auto items = ScanDesktopItems();
  std::vector<DesktopItem> links;
  std::vector<DesktopItem> other;
  links.reserve(items.size());
  other.reserve(items.size());
  for (const auto& it : items) {
    if (it.is_link) {
      links.push_back(it);
    } else {
      other.push_back(it);
    }
  }

  FenceWindow* software = nullptr;
  FenceWindow* common = nullptr;
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    if (fence->Id() == "system_software" || fence->Id() == "system_software_side") {
      software = fence.get();
    } else if (fence->Id() == "system_common") {
      common = fence.get();
    }
  }

  if (software) {
    software->SetItems(std::move(links));
  }
  if (common) {
    common->SetItems(std::move(other));
  }

}

void App::AddFence() {
  FenceLayout layout;
  layout.id = MakeFenceId();
  layout.title = L"新建分区";
  layout.w = 260;
  layout.h = 280;
  layout.x = 80 + static_cast<int>(fences_.size()) * 28;
  layout.y = 80 + static_cast<int>(fences_.size()) * 28;
  if (!SpawnFence(layout)) {
    return;
  }
  SeatAllFencesUnderApps();
  PersistLayouts();
  RefreshSettingsList();
  if (settings_.IsOpen()) {
    settings_.Show();
  }
}

void App::RemoveFenceAt(int index) {
  if (index < 0 || index >= static_cast<int>(fences_.size())) {
    return;
  }
  fences_[static_cast<size_t>(index)]->Destroy();
  fences_.erase(fences_.begin() + index);
  PersistLayouts();
  RefreshSettingsList();
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
        AppendMenuW(menu, MF_STRING, 2, L"Add fence");
        AppendMenuW(menu, MF_STRING, 3, L"Refresh desktop icons");
        AppendMenuW(menu, MF_STRING, 4, L"Seat fences under apps");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 5, L"Exit");
        SetForegroundWindow(hwnd);
        const int cmd =
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
                           hwnd, nullptr);
        DestroyMenu(menu);
        if (cmd == 1) {
          self->OpenSettings();
        } else if (cmd == 2) {
          self->AddFence();
        } else if (cmd == 3) {
          self->RefreshDesktopIcons();
        } else if (cmd == 4) {
          self->SeatAllFencesUnderApps();
        } else if (cmd == 5) {
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
