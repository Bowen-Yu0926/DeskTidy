#include "app.h"

#include "desktop_items.h"
#include "desktop_shell.h"
#include "organize.h"

#include <CommCtrl.h>
#include <Shellapi.h>

#include <unordered_map>
#include <utility>

namespace {
constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kHotkeySettings = 1;
constexpr UINT kHotkeyPage1 = 2;
constexpr UINT kHotkeyPage2 = 3;
constexpr UINT kTimerRestore = 40;
constexpr UINT kTimerPet = 41;
constexpr wchar_t kMsgClass[] = L"DeskTidyNativeMsg";
}  // namespace

bool App::SpawnFence(const FenceLayout& layout) {
  auto fence = std::make_unique<FenceWindow>();
  if (!fence->Create(instance_, layout)) {
    return false;
  }
  fence->SetOnMoved([this](FenceWindow*) { PersistLayouts(); });
  fence->SetOnPathsDropped(
      [this](FenceWindow* f, const std::vector<std::wstring>& paths) {
        OnPathsDropped(f, paths);
      });
  fence->SetOnDragDone([this](FenceWindow*) { RefreshDesktopIcons(); });
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
  RegisterHotKey(message_hwnd_, kHotkeyPage1, MOD_CONTROL | MOD_ALT, '1');
  RegisterHotKey(message_hwnd_, kHotkeyPage2, MOD_CONTROL | MOD_ALT, '2');

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

  notes_.Create(instance);
  dock_.Create(instance);
  pet_.Create(instance);
  page_bar_.Create(instance);
  page_bar_.SetOnPage([this](int page) { SetPage(page); });

  const AppState state = LoadAppState();
  current_page_ = state.current_page;
  dock_visible_ = state.dock_visible;
  pet_visible_ = state.pet_visible;
  pins_ = LoadPins();

  for (const auto& layout : state.fences) {
    if (!SpawnFence(layout)) {
      return false;
    }
  }
  PersistLayouts();
  RefreshDesktopIcons();
  RefreshSettingsList();
  ApplyPageVisibility();

  page_bar_.SetPage(current_page_);
  page_bar_.Show();
  dock_.SetVisible(dock_visible_);
  pet_.SetVisible(pet_visible_);
  RebuildDockItems();

  restore_timer_ =
      SetTimer(message_hwnd_, kTimerRestore, 800, nullptr);
  pet_timer_ = SetTimer(message_hwnd_, kTimerPet, 50, nullptr);

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
  SavePins(pins_);
  if (message_hwnd_) {
    KillTimer(message_hwnd_, kTimerRestore);
    KillTimer(message_hwnd_, kTimerPet);
    restore_timer_ = 0;
    pet_timer_ = 0;
  }
  RemoveTrayIcon();
  if (message_hwnd_) {
    UnregisterHotKey(message_hwnd_, kHotkeySettings);
    UnregisterHotKey(message_hwnd_, kHotkeyPage1);
    UnregisterHotKey(message_hwnd_, kHotkeyPage2);
    DestroyWindow(message_hwnd_);
    message_hwnd_ = nullptr;
  }
  for (auto& fence : fences_) {
    if (fence) {
      fence->Destroy();
    }
  }
  fences_.clear();
  page_bar_.Destroy();
  dock_.Destroy();
  pet_.Destroy();
  notes_.Destroy();
  settings_.Destroy();
}

void App::OpenSettings() {
  SeatAllFencesUnderApps();
  RefreshSettingsList();
  settings_.Show();
}

void App::OpenNotes() { notes_.Show(); }

void App::SeatAllFencesUnderApps() {
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd()) {
      continue;
    }
    AttachOverlayToDesktop(fence->Hwnd(), true);
    PlaceOverlayInDesktopBand(fence->Hwnd(), true);
  }
  if (dock_.Hwnd()) {
    PlaceOverlayInDesktopBand(dock_.Hwnd(), true);
  }
  if (pet_.Hwnd()) {
    PlaceOverlayInDesktopBand(pet_.Hwnd(), true);
  }
  if (page_bar_.Hwnd()) {
    PlaceOverlayInDesktopBand(page_bar_.Hwnd(), true);
  }
}

void App::PersistLayouts() {
  AppState state;
  state.current_page = current_page_;
  state.dock_visible = dock_visible_;
  state.pet_visible = pet_visible_;
  state.fences.reserve(fences_.size());
  for (const auto& fence : fences_) {
    if (fence) {
      state.fences.push_back(fence->CurrentLayout());
    }
  }
  SaveAppState(state);
}

void App::RefreshSettingsList() {
  std::vector<std::wstring> names;
  names.reserve(fences_.size());
  for (const auto& fence : fences_) {
    if (fence) {
      names.push_back(fence->Title() + L"  p" +
                      std::to_wstring(fence->Page() + 1) + L"  [" +
                      std::wstring(fence->Id().begin(), fence->Id().end()) +
                      L"]");
    }
  }
  settings_.SetFenceNames(names);
}

void App::OnPathsDropped(FenceWindow* fence,
                         const std::vector<std::wstring>& paths) {
  if (!fence) {
    return;
  }
  for (const auto& p : paths) {
    SetPin(&pins_, p, fence->Id());
  }
  SavePins(pins_);
  RefreshDesktopIcons();
}

void App::RefreshDesktopIcons() {
  const auto items = ScanDesktopItems();
  std::unordered_map<std::string, std::vector<DesktopItem>> by_fence;
  for (const auto& it : items) {
    const std::string* pin = FindPin(pins_, it.path);
    std::string target;
    if (pin) {
      target = *pin;
    } else if (it.is_link) {
      target = "system_software";
    } else {
      target = "system_common";
    }
    by_fence[target].push_back(it);
  }

  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    auto it = by_fence.find(fence->Id());
    if (it != by_fence.end()) {
      fence->SetItems(std::move(it->second));
    } else {
      fence->SetItems({});
    }
  }
  RebuildDockItems();
}

void App::RebuildDockItems() {
  std::vector<DesktopItem> dock_items;
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    if (fence->Id() == "system_software" ||
        fence->Id() == "system_software_side") {
      // Pull from current software fence cells via rescan pins.
      break;
    }
  }
  const auto items = ScanDesktopItems();
  for (const auto& it : items) {
    if (!it.is_link) {
      continue;
    }
    const std::string* pin = FindPin(pins_, it.path);
    if (pin && *pin != "system_software" && *pin != "system_software_side") {
      continue;
    }
    if (!pin && !it.is_link) {
      continue;
    }
    dock_items.push_back(it);
    if (dock_items.size() >= 8) {
      break;
    }
  }
  dock_.SetItems(std::move(dock_items));
}

void App::ApplyPageVisibility() {
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    if (fence->Page() == current_page_) {
      fence->Show();
    } else {
      fence->Hide();
    }
  }
  page_bar_.SetPage(current_page_);
  page_bar_.Show();
  if (dock_visible_) {
    dock_.Show();
  }
  if (pet_visible_) {
    pet_.Show();
  }
}

void App::SetPage(int page) {
  if (page < 0) {
    page = 0;
  }
  if (page > 1) {
    page = 1;
  }
  current_page_ = page;
  ApplyPageVisibility();
  PersistLayouts();
}

void App::RestoreDesktopChrome() {
  // Win+D / show-desktop settle: re-attach + raise current page chrome.
  ApplyPageVisibility();
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd() || fence->Page() != current_page_) {
      continue;
    }
    if (!IsWindowVisible(fence->Hwnd())) {
      fence->Show();
    } else {
      fence->AttachToDesktopLayer();
    }
  }
  if (dock_visible_) {
    dock_.AttachToDesktopLayer();
  }
  if (pet_visible_) {
    pet_.AttachToDesktopLayer();
  }
  page_bar_.AttachToDesktopLayer();
}

void App::OrganizeVirtual() {
  const auto items = ScanDesktopItems();
  const int n =
      OrganizeApplyDefaultPins(&pins_, items, "system_software", "system_common");
  SavePins(pins_);
  RefreshDesktopIcons();
  wchar_t buf[128];
  swprintf_s(buf, L"Virtual organize: pinned %d items.", n);
  MessageBoxW(nullptr, buf, L"DeskTidy Native", MB_OK | MB_ICONINFORMATION);
}

void App::OrganizePhysical() {
  const auto items = ScanDesktopItems();
  const int n = OrganizeMoveIntoFolders(items);
  pins_.clear();
  SavePins(pins_);
  RefreshDesktopIcons();
  wchar_t buf[128];
  swprintf_s(buf, L"Physical organize: moved %d items into Desktop\\DeskTidy.",
             n);
  MessageBoxW(nullptr, buf, L"DeskTidy Native", MB_OK | MB_ICONINFORMATION);
}

void App::ToggleDock() {
  dock_visible_ = !dock_visible_;
  dock_.SetVisible(dock_visible_);
  PersistLayouts();
}

void App::TogglePet() {
  pet_visible_ = !pet_visible_;
  pet_.SetVisible(pet_visible_);
  PersistLayouts();
}

void App::AddFence() {
  FenceLayout layout;
  layout.id = MakeFenceId();
  layout.title = L"新建分区";
  layout.w = 260;
  layout.h = 280;
  layout.x = 80 + static_cast<int>(fences_.size()) * 28;
  layout.y = 80 + static_cast<int>(fences_.size()) * 28;
  layout.page = current_page_;
  if (!SpawnFence(layout)) {
    return;
  }
  SeatAllFencesUnderApps();
  PersistLayouts();
  RefreshSettingsList();
  ApplyPageVisibility();
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
  wcsncpy_s(nid_.szTip, L"DeskTidy Native (C++)", _TRUNCATE);
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
    case WM_TIMER:
      if (wparam == kTimerRestore) {
        self->RestoreDesktopChrome();
        return 0;
      }
      if (wparam == kTimerPet) {
        self->pet_.Tick();
        return 0;
      }
      break;
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
        AppendMenuW(menu, MF_STRING, 2, L"Notes");
        AppendMenuW(menu, MF_STRING, 3, L"Add fence");
        AppendMenuW(menu, MF_STRING, 4, L"Refresh desktop icons");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 5, L"Page 1  (Ctrl+Alt+1)");
        AppendMenuW(menu, MF_STRING, 6, L"Page 2  (Ctrl+Alt+2)");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 7, L"Organize (virtual pins)");
        AppendMenuW(menu, MF_STRING, 8, L"Organize (move folders)");
        AppendMenuW(menu, MF_STRING, 9, L"Toggle dock");
        AppendMenuW(menu, MF_STRING, 10, L"Toggle pet");
        AppendMenuW(menu, MF_STRING, 11, L"Seat / restore desktop chrome");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 12, L"Exit");
        SetForegroundWindow(hwnd);
        const int cmd =
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
                           hwnd, nullptr);
        DestroyMenu(menu);
        switch (cmd) {
          case 1:
            self->OpenSettings();
            break;
          case 2:
            self->OpenNotes();
            break;
          case 3:
            self->AddFence();
            break;
          case 4:
            self->RefreshDesktopIcons();
            break;
          case 5:
            self->SetPage(0);
            break;
          case 6:
            self->SetPage(1);
            break;
          case 7:
            self->OrganizeVirtual();
            break;
          case 8:
            self->OrganizePhysical();
            break;
          case 9:
            self->ToggleDock();
            break;
          case 10:
            self->TogglePet();
            break;
          case 11:
            self->RestoreDesktopChrome();
            break;
          case 12:
            PostQuitMessage(0);
            break;
          default:
            break;
        }
        return 0;
      }
      break;
    case WM_HOTKEY:
      if (wparam == kHotkeySettings) {
        self->OpenSettings();
        return 0;
      }
      if (wparam == kHotkeyPage1) {
        self->SetPage(0);
        return 0;
      }
      if (wparam == kHotkeyPage2) {
        self->SetPage(1);
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
