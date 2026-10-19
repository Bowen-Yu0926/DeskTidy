#include "app.h"

#include "autostart.h"
#include "desktop_items.h"
#include "desktop_shell.h"
#include "first_run.h"
#include "hotkey_help.h"
#include "organize.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "shell_verbs.h"
#include "snapshot.h"
#include "tools_misc.h"

#include <CommCtrl.h>
#include <Shellapi.h>
#include <ShlObj.h>

#include <fstream>
#include <unordered_map>
#include <utility>

namespace {
constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kHotkeySettings = 1;
constexpr UINT kHotkeyPage1 = 2;
constexpr UINT kHotkeyPage2 = 3;
constexpr UINT kHotkeyNotes = 4;
constexpr UINT kHotkeyPeek = 5;
constexpr UINT kHotkeyFences = 6;
constexpr UINT kHotkeySearch = 7;
constexpr UINT kHotkeyShot = 8;
constexpr UINT kTimerRestore = 40;
constexpr UINT kTimerPet = 41;
constexpr UINT kTimerWatch = 42;
constexpr wchar_t kMsgClass[] = L"DeskTidyNativeMsg";
}  // namespace

void App::WireFence(FenceWindow* fence) {
  if (!fence) {
    return;
  }
  fence->SetOnMoved([this](FenceWindow*) { PersistLayouts(); });
  fence->SetOnPathsDropped(
      [this](FenceWindow* f, const std::vector<std::wstring>& paths) {
        OnPathsDropped(f, paths);
      });
  fence->SetOnDragDone([this](FenceWindow*) { RefreshDesktopIcons(); });
  fence->SetOnUnpin([this](FenceWindow*, const std::wstring& path) {
    UnpinPath(path);
  });
  fence->SetOnSetPage([this](FenceWindow* f, int page) {
    if (!f) {
      return;
    }
    f->SetPage(page);
    PersistLayouts();
    ApplyPageVisibility();
    RefreshSettingsList();
  });
  fence->SetOnRename([this](FenceWindow* f) {
    if (!f) {
      return;
    }
    const auto name =
        PromptText(settings_.Hwnd(), L"重命名分区", L"新标题：", f->Title());
    if (!name.empty()) {
      f->SetTitle(name);
      PersistLayouts();
      RefreshSettingsList();
    }
  });
}

bool App::SpawnFence(const FenceLayout& layout) {
  auto fence = std::make_unique<FenceWindow>();
  if (!fence->Create(instance_, layout)) {
    return false;
  }
  WireFence(fence.get());
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
  RegisterHotKey(message_hwnd_, kHotkeyNotes, MOD_CONTROL | MOD_SHIFT, 'N');
  RegisterHotKey(message_hwnd_, kHotkeyPeek, MOD_CONTROL, VK_SPACE);
  RegisterHotKey(message_hwnd_, kHotkeyFences, MOD_CONTROL | MOD_SHIFT, 'F');
  RegisterHotKey(message_hwnd_, kHotkeySearch, MOD_CONTROL | MOD_ALT, '4');
  RegisterHotKey(message_hwnd_, kHotkeyShot, MOD_CONTROL | MOD_SHIFT, '1');

  if (!settings_.Create(instance)) {
    return false;
  }
  settings_.SetHandlers(
      [this]() { AddFence(); },
      [this](int index) { RemoveFenceAt(index); },
      [this]() {
        SeatAllFencesUnderApps();
        settings_.Show();
      },
      [this](int index, const std::wstring& title) {
        RenameFenceAt(index, title);
      },
      [this](int index, int page) { SetFencePageAt(index, page); },
      [this]() { OrganizeVirtual(); }, [this]() { OrganizePhysical(); },
      [this](bool on) { SetDockVisible(on); },
      [this](bool on) { SetPetVisible(on); }, [this]() { EditRules(); },
      [this](bool on) { SetAutostart(on); });

  notes_.Create(instance);
  todos_.Create(instance);
  search_.Create(instance);
  vault_.Create(instance);
  dock_.Create(instance);
  pet_.Create(instance);
  pet_.SetOnTrash([this](const std::vector<std::wstring>& paths) {
    if (SendPathsToRecycleBin(paths)) {
      RefreshDesktopIcons();
    }
  });
  page_bar_.Create(instance);
  page_bar_.SetOnPage([this](int page) { SetPage(page); });
  public_host_.Create(instance);

  const AppState state = LoadAppState();
  current_page_ = state.current_page;
  dock_visible_ = state.dock_visible;
  pet_visible_ = state.pet_visible;
  pins_ = LoadPins();
  rules_ = LoadRules();
  SaveRules(rules_);

  for (const auto& layout : state.fences) {
    if (!SpawnFence(layout)) {
      return false;
    }
  }
  PersistLayouts();
  RefreshDesktopIcons();
  RefreshSettingsList();
  settings_.SetToggles(dock_visible_, pet_visible_, IsNativeAutostartEnabled());
  ApplyPageVisibility();

  page_bar_.SetPage(current_page_);
  page_bar_.Show();
  dock_.SetVisible(dock_visible_);
  pet_.SetVisible(pet_visible_);
  public_host_.SetVisible(public_visible_);
  RebuildDockItems();
  RebuildPublicItems();
  desktop_watch_.Reset();
  RegisterNativeShellVerbs();

  restore_timer_ = SetTimer(message_hwnd_, kTimerRestore, 800, nullptr);
  pet_timer_ = SetTimer(message_hwnd_, kTimerPet, 50, nullptr);
  watch_timer_ = SetTimer(message_hwnd_, kTimerWatch, 2000, nullptr);

  CreateTrayIcon();
  MaybeShowFirstRun(settings_.Hwnd());
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
  SaveRules(rules_);
  if (message_hwnd_) {
    KillTimer(message_hwnd_, kTimerRestore);
    KillTimer(message_hwnd_, kTimerPet);
    KillTimer(message_hwnd_, kTimerWatch);
    restore_timer_ = pet_timer_ = watch_timer_ = 0;
  }
  RemoveTrayIcon();
  if (message_hwnd_) {
    UnregisterHotKey(message_hwnd_, kHotkeySettings);
    UnregisterHotKey(message_hwnd_, kHotkeyPage1);
    UnregisterHotKey(message_hwnd_, kHotkeyPage2);
    UnregisterHotKey(message_hwnd_, kHotkeyNotes);
    UnregisterHotKey(message_hwnd_, kHotkeyPeek);
    UnregisterHotKey(message_hwnd_, kHotkeyFences);
    UnregisterHotKey(message_hwnd_, kHotkeySearch);
    UnregisterHotKey(message_hwnd_, kHotkeyShot);
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
  public_host_.Destroy();
  dock_.Destroy();
  pet_.Destroy();
  notes_.Destroy();
  todos_.Destroy();
  search_.Destroy();
  vault_.Destroy();
  settings_.Destroy();
  for (auto& p : pins_img_) {
    if (p) {
      p->Destroy();
    }
  }
  pins_img_.clear();
}

void App::StartRegionCapture() {
  capture_.Cancel();
  capture_.Begin(instance_, [this](const std::wstring& path, bool pin) {
    if (pin) {
      PinImage(path);
    }
  });
}

void App::PinImage(const std::wstring& path) {
  auto img = std::make_unique<PinnedImage>();
  if (!img->Create(instance_, path)) {
    return;
  }
  img->Show();
  pins_img_.push_back(std::move(img));
}

void App::ShowHotkeys() { ShowHotkeyHelp(settings_.Hwnd()); }

void App::HandleCliAction(const std::wstring& action) {
  if (action == L"--settings") {
    OpenSettings();
  } else if (action == L"--refresh") {
    RefreshDesktopIcons();
  } else if (action == L"--organize") {
    OrganizeVirtual();
  }
}

void App::OpenTodos() { todos_.Show(); }
void App::OpenSearch() { search_.Show(); }
void App::OpenVault() { vault_.Show(); }

void App::TogglePeek() {
  peek_hidden_ = !peek_hidden_;
  if (peek_hidden_) {
    for (auto& f : fences_) {
      if (f) {
        f->Hide();
      }
    }
    dock_.Hide();
    page_bar_.Hide();
    public_host_.Hide();
  } else {
    ApplyPageVisibility();
  }
}

void App::ToggleFencesVisible() {
  fences_hidden_ = !fences_hidden_;
  if (fences_hidden_) {
    for (auto& f : fences_) {
      if (f) {
        f->Hide();
      }
    }
  } else {
    ApplyPageVisibility();
  }
}

void App::SaveLayoutSnapshot() {
  AppState state;
  state.current_page = current_page_;
  state.dock_visible = dock_visible_;
  state.pet_visible = pet_visible_;
  for (const auto& f : fences_) {
    if (f) {
      state.fences.push_back(f->CurrentLayout());
    }
  }
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf_s(name, L"snap_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  if (SaveSnapshot(state, name)) {
    MessageBoxW(settings_.Hwnd(), (std::wstring(L"已保存快照：") + name).c_str(),
                L"布局快照", MB_OK | MB_ICONINFORMATION);
  }
}

void App::RestoreLayoutSnapshot() {
  const auto list = ListSnapshots();
  if (list.empty()) {
    MessageBoxW(settings_.Hwnd(), L"还没有快照。请先「保存布局快照」。",
                L"布局快照", MB_OK | MB_ICONINFORMATION);
    return;
  }
  const auto& last = list.back();
  AppState state;
  if (!LoadSnapshot(last.path, &state)) {
    MessageBoxW(settings_.Hwnd(), L"读取快照失败。", L"布局快照",
                MB_OK | MB_ICONWARNING);
    return;
  }
  ReloadFencesFromState(state);
  MessageBoxW(settings_.Hwnd(), (L"已恢复：" + last.name).c_str(), L"布局快照",
              MB_OK | MB_ICONINFORMATION);
}

void App::ReloadFencesFromState(const AppState& state) {
  for (auto& f : fences_) {
    if (f) {
      f->Destroy();
    }
  }
  fences_.clear();
  current_page_ = state.current_page;
  dock_visible_ = state.dock_visible;
  pet_visible_ = state.pet_visible;
  for (const auto& layout : state.fences) {
    SpawnFence(layout);
  }
  PersistLayouts();
  RefreshDesktopIcons();
  RefreshSettingsList();
  ApplyPageVisibility();
  dock_.SetVisible(dock_visible_);
  pet_.SetVisible(pet_visible_);
}

void App::OpenMinutes() {
  const std::wstring path = JoinPath(NativeDataDir(), L"meeting_minutes.md");
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::ofstream out(path, std::ios::binary);
    out << "# 会议纪要\n\n- 日期：\n- 参与人：\n- 决议：\n";
  }
  ShellExecuteW(nullptr, L"open", L"notepad.exe", path.c_str(), nullptr,
                SW_SHOWNORMAL);
}

void App::RebuildPublicItems() {
  std::vector<DesktopItem> pub;
  const auto items = ScanDesktopItems();
  for (const auto& it : items) {
    if (pub.size() >= 5) {
      break;
    }
    if (it.is_link || it.name.find(L"此电脑") != std::wstring::npos ||
        it.name.find(L"Computer") != std::wstring::npos ||
        it.name.find(L"回收站") != std::wstring::npos ||
        it.name.find(L"Recycle") != std::wstring::npos) {
      pub.push_back(it);
    }
  }
  if (pub.empty()) {
    for (size_t i = 0; i < items.size() && pub.size() < 4; ++i) {
      pub.push_back(items[i]);
    }
  }
  public_host_.SetItems(std::move(pub));
}

void App::OpenSettings() {
  SeatAllFencesUnderApps();
  RefreshSettingsList();
  settings_.SetToggles(dock_visible_, pet_visible_, IsNativeAutostartEnabled());
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
  if (notes_.Hwnd() && notes_.IsOpen()) {
    PlaceOverlayInDesktopBand(notes_.Hwnd(), true);
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

void App::UnpinPath(const std::wstring& path) {
  ClearPin(&pins_, path);
  SavePins(pins_);
  RefreshDesktopIcons();
}

void App::RefreshDesktopIcons() {
  const auto items = ScanDesktopItems();
  std::unordered_map<std::string, std::vector<DesktopItem>> by_fence;
  for (const auto& it : items) {
    const std::string target =
        ResolveFenceId(it.path, it.is_link, pins_, rules_, "system_software",
                       "system_common");
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
  RebuildPublicItems();
}

void App::RebuildDockItems() {
  std::vector<DesktopItem> dock_items;
  const auto items = ScanDesktopItems();
  for (const auto& it : items) {
    if (!it.is_link) {
      continue;
    }
    const std::string target =
        ResolveFenceId(it.path, it.is_link, pins_, rules_, "system_software",
                       "system_common");
    if (target != "system_software" && target != "system_software_side") {
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
  if (peek_hidden_) {
    return;
  }
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    if (!fences_hidden_ && fence->Page() == current_page_) {
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
  if (public_visible_) {
    public_host_.Show();
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
  if (notes_.IsOpen()) {
    notes_.Show();
  }
}

void App::OrganizeVirtual() {
  const auto items = ScanDesktopItems();
  const int n =
      OrganizeApplyDefaultPins(&pins_, items, "system_software", "system_common");
  SavePins(pins_);
  RefreshDesktopIcons();
  wchar_t buf[128];
  swprintf_s(buf, L"虚拟整理完成：写入 %d 条钉选。", n);
  MessageBoxW(settings_.Hwnd(), buf, L"DeskTidy Native",
              MB_OK | MB_ICONINFORMATION);
}

void App::OrganizePhysical() {
  const auto items = ScanDesktopItems();
  const int n = OrganizeMoveIntoFolders(items);
  pins_.clear();
  SavePins(pins_);
  RefreshDesktopIcons();
  wchar_t buf[128];
  swprintf_s(buf, L"物理整理完成：移动 %d 个项目到 Desktop\\DeskTidy。", n);
  MessageBoxW(settings_.Hwnd(), buf, L"DeskTidy Native",
              MB_OK | MB_ICONINFORMATION);
}

void App::SetDockVisible(bool on) {
  dock_visible_ = on;
  dock_.SetVisible(dock_visible_);
  PersistLayouts();
}

void App::SetPetVisible(bool on) {
  pet_visible_ = on;
  pet_.SetVisible(pet_visible_);
  PersistLayouts();
}

void App::ToggleDock() { SetDockVisible(!dock_visible_); }

void App::TogglePet() { SetPetVisible(!pet_visible_); }

void App::SetAutostart(bool on) {
  if (!SetNativeAutostart(on)) {
    MessageBoxW(settings_.Hwnd(), L"无法更新开机启动快捷方式。",
                L"DeskTidy Native", MB_OK | MB_ICONWARNING);
  }
  settings_.SetToggles(dock_visible_, pet_visible_, IsNativeAutostartEnabled());
}

void App::ToggleAutostart() { SetAutostart(!IsNativeAutostartEnabled()); }

void App::EditRules() {
  SaveRules(rules_);
  const std::wstring path = RuleFilePath();
  ShellExecuteW(nullptr, L"open", L"notepad.exe", path.c_str(), nullptr,
                SW_SHOWNORMAL);
  MessageBoxW(settings_.Hwnd(),
              L"已用记事本打开 rules.json。\n"
              L"保存后点托盘「刷新桌面图标」或等待自动监视生效。",
              L"扩展名规则", MB_OK | MB_ICONINFORMATION);
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

void App::RenameFenceAt(int index, const std::wstring& title) {
  if (index < 0 || index >= static_cast<int>(fences_.size()) || title.empty()) {
    return;
  }
  fences_[static_cast<size_t>(index)]->SetTitle(title);
  PersistLayouts();
  RefreshSettingsList();
}

void App::SetFencePageAt(int index, int page) {
  if (index < 0 || index >= static_cast<int>(fences_.size())) {
    return;
  }
  fences_[static_cast<size_t>(index)]->SetPage(page);
  PersistLayouts();
  ApplyPageVisibility();
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
  wcsncpy_s(nid_.szTip, L"DeskTidy Native", _TRUNCATE);
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
      if (wparam == kTimerWatch) {
        if (self->desktop_watch_.PollChanged()) {
          self->rules_ = LoadRules();
          self->RefreshDesktopIcons();
        }
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
        AppendMenuW(menu, MF_STRING, 1, L"设置");
        AppendMenuW(menu, MF_STRING, 2, L"便签");
        AppendMenuW(menu, MF_STRING, 3, L"待办");
        AppendMenuW(menu, MF_STRING, 4, L"文件搜索");
        AppendMenuW(menu, MF_STRING, 5, L"账号保险库");
        AppendMenuW(menu, MF_STRING, 6, L"会议纪要");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 7, L"截图");
        AppendMenuW(menu, MF_STRING, 8, L"录屏(15s)");
        AppendMenuW(menu, MF_STRING, 9, L"设置壁纸");
        AppendMenuW(menu, MF_STRING, 10, L"检查更新");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 11, L"保存布局快照");
        AppendMenuW(menu, MF_STRING, 12, L"恢复最近快照");
        AppendMenuW(menu, MF_STRING, 13, L"Peek 显隐分区");
        AppendMenuW(menu, MF_STRING, 14, L"显示/隐藏分区");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 15, L"第 1 页");
        AppendMenuW(menu, MF_STRING, 16, L"第 2 页");
        AppendMenuW(menu, MF_STRING, 17, L"虚拟整理");
        AppendMenuW(menu, MF_STRING, 18, L"物理整理");
        AppendMenuW(menu, MF_STRING, 19, L"编辑规则");
        AppendMenuW(menu, MF_STRING, 20, L"刷新图标");
        AppendMenuW(menu, MF_STRING, 21, L"添加分区");
        AppendMenuW(menu, MF_STRING, 22, L"切换 Dock");
        AppendMenuW(menu, MF_STRING, 23, L"切换宠物");
        AppendMenuW(menu,
                    MF_STRING | (IsNativeAutostartEnabled() ? MF_CHECKED : 0),
                    24, L"开机启动");
        AppendMenuW(menu, MF_STRING, 25, L"恢复桌面层");
        AppendMenuW(menu, MF_STRING, 26, L"快捷键说明");
        AppendMenuW(menu, MF_STRING, 27, L"全屏截图");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 28, L"退出");
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
            self->OpenTodos();
            break;
          case 4:
            self->OpenSearch();
            break;
          case 5:
            self->OpenVault();
            break;
          case 6:
            self->OpenMinutes();
            break;
          case 7:
            self->StartRegionCapture();
            break;
          case 8:
            StartScreenRecord(self->settings_.Hwnd());
            break;
          case 9:
            PickAndSetWallpaper(self->settings_.Hwnd());
            break;
          case 10:
            OpenUpdatePage();
            break;
          case 11:
            self->SaveLayoutSnapshot();
            break;
          case 12:
            self->RestoreLayoutSnapshot();
            break;
          case 13:
            self->TogglePeek();
            break;
          case 14:
            self->ToggleFencesVisible();
            break;
          case 15:
            self->SetPage(0);
            break;
          case 16:
            self->SetPage(1);
            break;
          case 17:
            self->OrganizeVirtual();
            break;
          case 18:
            self->OrganizePhysical();
            break;
          case 19:
            self->EditRules();
            break;
          case 20:
            self->rules_ = LoadRules();
            self->RefreshDesktopIcons();
            break;
          case 21:
            self->AddFence();
            break;
          case 22:
            self->ToggleDock();
            break;
          case 23:
            self->TogglePet();
            break;
          case 24:
            self->ToggleAutostart();
            break;
          case 25:
            self->RestoreDesktopChrome();
            break;
          case 26:
            self->ShowHotkeys();
            break;
          case 27:
            CaptureRegionToPng(self->settings_.Hwnd());
            break;
          case 28:
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
      if (wparam == kHotkeyNotes) {
        self->OpenNotes();
        return 0;
      }
      if (wparam == kHotkeyPeek) {
        self->TogglePeek();
        return 0;
      }
      if (wparam == kHotkeyFences) {
        self->ToggleFencesVisible();
        return 0;
      }
      if (wparam == kHotkeySearch) {
        self->OpenSearch();
        return 0;
      }
      if (wparam == kHotkeyShot) {
        self->StartRegionCapture();
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
