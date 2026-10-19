#include "app.h"

#include "autostart.h"
#include "desktop_items.h"
#include "desktop_shell.h"
#include "first_run.h"
#include "hotkey_help.h"
#include "hotkey_store.h"
#include "i18n.h"
#include "organize.h"
#include "organize_suppress.h"
#include "screen_record.h"
#include "toast.h"
#include "minutes.h"
#include "native_cleanup.h"
#include "page_folders.h"
#include "snapshot_ui.h"
#include "whitelist.h"
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
constexpr UINT kHotkeyBase = 100;
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
  fence->SetOnStyleChanged([this](FenceWindow*) { PersistLayouts(); });
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
  LoadLanguage();
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

  hotkeys_ = LoadHotkeys();
  SaveHotkeys(hotkeys_);

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
      [this](bool on) { SetAutostart(on); },
      [this]() { EditWhitelistDialog(settings_.Hwnd()); },
      [this]() { OpenPageFolderMenu(settings_.Hwnd()); },
      [this]() { ShowAppHelp(settings_.Hwnd()); },
      [this](bool on) { SetOrganizeStartup(on); },
      [this](bool on) { SetOrganizeWatch(on); },
      [this]() { ConfigureScreenRecord(settings_.Hwnd()); },
      [this]() {
        const auto names =
            OrganizePreviewVirtualPins(pins_, ScanDesktopItems());
        settings_.SetOrganizePreview(names);
      });

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
  page_bar_.SetOnNotes([this]() { OpenNotes(); });
  page_bar_.SetOnSearch([this]() { OpenSearch(); });
  page_bar_.SetOnCapture([this]() { StartRegionCapture(); });
  public_host_.Create(instance);

  prefs_ = LoadAppPrefs();

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
  settings_.SetOrganizePrefs(prefs_.auto_organize_on_startup,
                             prefs_.auto_organize_watch);
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
  ReregisterHotkeys();
  MaybeShowFirstRun(settings_.Hwnd());
  if (prefs_.auto_organize_on_startup) {
    OrganizeVirtual(true);
  }
  return true;
}

void App::ReregisterHotkeys() {
  if (!message_hwnd_) {
    return;
  }
  for (size_t i = 0; i < 32; ++i) {
    UnregisterHotKey(message_hwnd_, static_cast<int>(kHotkeyBase + i));
  }
  for (size_t i = 0; i < hotkeys_.size(); ++i) {
    RegisterHotKey(message_hwnd_, static_cast<int>(kHotkeyBase + i),
                   hotkeys_[i].modifiers, hotkeys_[i].vk);
  }
}

void App::DispatchHotkey(const std::string& id) {
  if (id == "settings") {
    OpenSettings();
  } else if (id == "notes") {
    OpenNotes();
  } else if (id == "peek") {
    TogglePeek();
  } else if (id == "fences") {
    ToggleFencesVisible();
  } else if (id == "page1") {
    SetPage(0);
  } else if (id == "page2") {
    SetPage(1);
  } else if (id == "search") {
    OpenSearch();
  } else if (id == "shot") {
    StartRegionCapture();
  } else if (id == "fullscreen") {
    CaptureRegionToPng(settings_.Hwnd());
  } else if (id == "record") {
    StartScreenRecord(settings_.Hwnd());
  } else if (id == "organize") {
    OrganizeVirtual(true);
  } else if (id == "vault") {
    OpenVault();
  } else if (id == "todos") {
    OpenTodos();
  }
}

void App::EditHotkeys() {
  EditHotkeysDialog(settings_.Hwnd(), &hotkeys_);
  ReregisterHotkeys();
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
    for (size_t i = 0; i < 32; ++i) {
      UnregisterHotKey(message_hwnd_, static_cast<int>(kHotkeyBase + i));
    }
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
  if (SaveSnapshotWithIcons(state, name)) {
    MessageBoxW(settings_.Hwnd(), (std::wstring(L"已保存快照：") + name).c_str(),
                L"布局快照", MB_OK | MB_ICONINFORMATION);
  }
}

void App::RestoreLayoutSnapshot() {
  AppState state;
  if (!ManageSnapshotsDialog(settings_.Hwnd(), &state)) {
    return;
  }
  ReloadFencesFromState(state);
  MessageBoxW(settings_.Hwnd(), L"已恢复所选快照。", L"布局快照",
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
  const int choice = MessageBoxW(
      settings_.Hwnd(),
      L"会议纪要：\n\n"
      L"是 = 新建 Markdown\n"
      L"否 = 新建 RTF（可用 Word 打开）\n"
      L"取消 = 更多…",
      L"会议纪要", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDYES) {
    CreateAndOpenMinutes(settings_.Hwnd());
    return;
  }
  if (choice == IDNO) {
    CreateAndOpenMinutesRtf(settings_.Hwnd());
    return;
  }
  const int more = MessageBoxW(
      settings_.Hwnd(),
      L"更多：\n\n是 = 打开最近纪要\n否 = 打开文件夹\n取消 = 设置文件夹",
      L"会议纪要", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (more == IDYES) {
    OpenRecentMinutes(settings_.Hwnd());
  } else if (more == IDNO) {
    OpenMinutesFolder(settings_.Hwnd());
  } else if (more == IDCANCEL) {
    ConfigureMinutesFolder(settings_.Hwnd());
  }
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
  settings_.SetOrganizePreview(
      OrganizePreviewVirtualPins(pins_, ScanDesktopItems()));
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
  SuppressOrganizePath(path);
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

void App::OrganizeVirtual(bool quiet) {
  const auto items = ScanDesktopItems();
  const int n =
      OrganizeApplyDefaultPins(&pins_, items, "system_software", "system_common");
  SavePins(pins_);
  RefreshDesktopIcons();
  settings_.SetOrganizePreview(
      OrganizePreviewVirtualPins(pins_, ScanDesktopItems()));
  if (quiet) {
    if (n > 0) {
      ShowToast(L"已自动钉选 " + std::to_wstring(n) + L" 项");
    }
    return;
  }
  wchar_t buf[128];
  swprintf_s(buf, L"虚拟整理完成：写入 %d 条钉选。", n);
  MessageBoxW(settings_.Hwnd(), buf, L"DeskTidy Native",
              MB_OK | MB_ICONINFORMATION);
}

void App::SetOrganizeStartup(bool on) {
  prefs_.auto_organize_on_startup = on;
  SaveAppPrefs(prefs_);
}

void App::SetOrganizeWatch(bool on) {
  prefs_.auto_organize_watch = on;
  SaveAppPrefs(prefs_);
}

void App::OrganizePhysical() {
  const auto items = ScanDesktopItems();
  if (!OrganizeConfirmPhysical(settings_.Hwnd(), items)) {
    return;
  }
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
              L"键：扩展名（如 .pdf）或文件名通配（如 Invoice*、name:报告*.docx）。\n"
              L"值：分区 id（如 system_common）。\n"
              L"保存后点托盘「刷新桌面图标」或等待自动监视生效。",
              L"整理规则", MB_OK | MB_ICONINFORMATION);
}

void App::AddFence() {
  if (!fence_region_.Begin(instance_, [this](const RECT& screen) {
        AddFenceAtRect(screen);
      })) {
    // Fallback: place a default-sized fence if overlay fails.
    RECT r{80 + static_cast<int>(fences_.size()) * 28,
           80 + static_cast<int>(fences_.size()) * 28, 0, 0};
    r.right = r.left + 260;
    r.bottom = r.top + 280;
    AddFenceAtRect(r);
  }
}

void App::AddFenceAtRect(const RECT& screen) {
  FenceLayout layout;
  layout.id = MakeFenceId();
  layout.title = L"新建分区";
  layout.x = screen.left;
  layout.y = screen.top;
  layout.w = (std::max)(160, static_cast<int>(screen.right - screen.left));
  layout.h = (std::max)(180, static_cast<int>(screen.bottom - screen.top));
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
        // Clear REC badge if ffmpeg exited (e.g. timed -t duration).
        (void)IsScreenRecording();
        return 0;
      }
      if (wparam == kTimerWatch) {
        if (self->desktop_watch_.PollChanged()) {
          self->rules_ = LoadRules();
          if (self->prefs_.auto_organize_watch) {
            self->OrganizeVirtual(true);
          } else {
            self->RefreshDesktopIcons();
          }
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
        AppendMenuW(menu, MF_STRING, 1, Tr("settings").c_str());
        AppendMenuW(menu, MF_STRING, 2, Tr("notes").c_str());
        AppendMenuW(menu, MF_STRING, 3, Tr("todos").c_str());
        AppendMenuW(menu, MF_STRING, 4, Tr("search").c_str());
        AppendMenuW(menu, MF_STRING, 5, Tr("vault").c_str());
        AppendMenuW(menu, MF_STRING, 6, Tr("minutes").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 7, Tr("screenshot").c_str());
        AppendMenuW(menu, MF_STRING, 8,
                    IsScreenRecording() ? L"停止录屏" : Tr("record").c_str());
        AppendMenuW(menu, MF_STRING, 9, Tr("wallpaper").c_str());
        AppendMenuW(menu, MF_STRING, 10, Tr("updates").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 11, Tr("save_snapshot").c_str());
        AppendMenuW(menu, MF_STRING, 12, Tr("manage_snapshots").c_str());
        AppendMenuW(menu, MF_STRING, 13, Tr("peek").c_str());
        AppendMenuW(menu, MF_STRING, 14, Tr("toggle_fences").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 15, Tr("page1").c_str());
        AppendMenuW(menu, MF_STRING, 16, Tr("page2").c_str());
        AppendMenuW(menu, MF_STRING, 17, Tr("organize_virtual").c_str());
        AppendMenuW(menu, MF_STRING, 18, Tr("organize_physical").c_str());
        AppendMenuW(menu, MF_STRING, 19, Tr("edit_rules").c_str());
        AppendMenuW(menu, MF_STRING, 30, Tr("whitelist").c_str());
        AppendMenuW(menu, MF_STRING, 20, Tr("refresh_icons").c_str());
        AppendMenuW(menu, MF_STRING, 21, Tr("add_fence").c_str());
        AppendMenuW(menu, MF_STRING, 22, Tr("toggle_dock").c_str());
        AppendMenuW(menu, MF_STRING, 23, Tr("toggle_pet").c_str());
        AppendMenuW(menu,
                    MF_STRING | (IsNativeAutostartEnabled() ? MF_CHECKED : 0),
                    24, L"开机启动");
        AppendMenuW(menu, MF_STRING, 25, L"恢复桌面层");
        AppendMenuW(menu, MF_STRING, 26, Tr("hotkey_help").c_str());
        AppendMenuW(menu, MF_STRING, 27, Tr("edit_hotkeys").c_str());
        AppendMenuW(menu, MF_STRING, 28, Tr("fullscreen_shot").c_str());
        AppendMenuW(menu, MF_STRING, 31, Tr("language").c_str());
        AppendMenuW(menu, MF_STRING, 32, Tr("calculator").c_str());
        AppendMenuW(menu, MF_STRING, 33, Tr("page_folders").c_str());
        AppendMenuW(menu, MF_STRING, 34, Tr("help").c_str());
        AppendMenuW(menu, MF_STRING, 35, L"卸载清理…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 29, Tr("exit").c_str());
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
            WallpaperMenu(self->settings_.Hwnd());
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
          case 30:
            EditWhitelistDialog(self->settings_.Hwnd());
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
            self->EditHotkeys();
            break;
          case 28:
            CaptureRegionToPng(self->settings_.Hwnd());
            break;
          case 31:
            ToggleLanguage();
            MessageBoxW(self->settings_.Hwnd(),
                        (std::wstring(L"Language: ") + WidenUtf8(CurrentLang())).c_str(),
                        Tr("language").c_str(), MB_OK | MB_ICONINFORMATION);
            break;
          case 32:
            OpenCalculator();
            break;
          case 33:
            OpenPageFolderMenu(self->settings_.Hwnd());
            break;
          case 34:
            ShowAppHelp(self->settings_.Hwnd());
            break;
          case 35:
            RunNativeCleanup(self->settings_.Hwnd());
            break;
          case 29:
            PostQuitMessage(0);
            break;
          default:
            break;
        }
        return 0;
      }
      break;
    case WM_DISPLAYCHANGE:
      self->ReloadFencesFromState(LoadAppState());
      return 0;
    case WM_HOTKEY: {
      const int idx = static_cast<int>(wparam) - static_cast<int>(kHotkeyBase);
      if (idx >= 0 && idx < static_cast<int>(self->hotkeys_.size())) {
        self->DispatchHotkey(self->hotkeys_[static_cast<size_t>(idx)].id);
        return 0;
      }
      break;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}
