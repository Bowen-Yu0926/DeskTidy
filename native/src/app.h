#pragma once

#include "app_prefs.h"
#include "capture_overlay.h"
#include "fence_region_overlay.h"
#include "desktop_watch.h"
#include "dock_window.h"
#include "fence_window.h"
#include "hotkey_relay.h"
#include "hotkey_store.h"
#include "layout.h"
#include "page_bar.h"
#include "pet_window.h"
#include "pin_store.h"
#include "pinned_image.h"
#include "public_host.h"
#include "rule_store.h"
#include "search_window.h"
#include "settings_window.h"
#include "todo_window.h"
#include "update_check.h"
#include "vault_launcher.h"
#include "vault_window.h"

#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>

#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class App {
 public:
  bool Init(HINSTANCE instance);
  int Run();
  void Shutdown();
  void HandleCliAction(const std::wstring& action);

  void OpenSettings();
  void OpenNotes();
  void OpenPathsInNotes(const std::vector<std::wstring>& paths);
  void RequestQuit();
  void HandleSettingsClose();
  void OpenTodos();
  void OpenSearch();
  void OpenVault();
  // After 账号浮窗 hides: cancel band-settle Raise and sink fences under apps.
  void OnVaultHidden();
  void SeatAllFencesUnderApps();
  // Soft-park: SW_HIDE every fence that must not paint on this page (leaks of
  // IsWindowVisible off-page fences ghost under apps when DefView is covered).
  void HideOffPageFences();
  void PersistLayouts();
  void SchedulePersistLayouts();
  // Idle off-screen Present for soft-parked dirty fences (snappy page switch).
  void ScheduleWarmFenceLayers();
  // Update public-float blocked rects + layout without rescanning the desktop.
  void ReflowPublicFloats();
  void RefreshSettingsList();
  void RefreshOrganizeSettingsPage();
  void RefreshDesktopIcons();
  // Python fence_rules.prune_missing_virtual_items: drop pins whose path has
  // been missing past the sticky grace (45s) so save-then-rewrite keeps them.
  void PruneStickyMissingPins();
  void RefreshIconGrids();
  void AddFence();
  void AddFenceAtRect(const RECT& screen);
  void MoveDesktopItemsToFence(const std::vector<std::wstring>& paths,
                               const std::string& fence_id);
  void MoveDesktopItemsToPage(const std::vector<std::wstring>& paths,
                              int page_index);
  void RemoveFenceAt(int index);
  void RenameFenceAt(int index, const std::wstring& title);
  void SetFencePageAt(int index, int page);
  void SetPage(int page);
  void AddPage();
  void RemoveCurrentPage();
  void RenameCurrentPage();
  void CreateFenceOnPage(int page, bool switch_to_page = true);
  // P2-9: empty pages get a document fence during virtual organize.
  void EnsureOrganizeFencesForEmptyPages();
  void DeletePageAt(int page);
  void RenamePageAt(int page);
  void OpenLayoutEditor();
  void CycleTheme();
  void ApplyThemeId(const std::string& theme_id);
  void ApplyUiThemeId(const std::string& ui_theme);
  void SyncVaultSessionState();
  void SyncVaultLauncher();
  void RefreshVaultListForSettings();
  void SetFenceStyleAt(int index, const std::wstring& style_id);
  void SetAllFenceStyle(const std::wstring& style_id);
  void SetFenceOpacityPctAt(int index, int opacity_pct);
  void ApplyDockPrefs();
  void SyncPageChrome();
  void MaybeStartupUpdateCheck();
  void OnUpdateCheckDone(UpdateCheckResult* r);
  void OnUpdateInstallDone(UpdateInstallResult* r);
  void OnUpdateProgress(int pct);
  // Shared Yes/No/Cancel offer after a soft toast click or manual check.
  void PromptAvailableUpdate(const std::wstring& latest,
                             const std::wstring& current,
                             const std::wstring& html_url);
  // quiet: no toast/dialog. reassign: recompute targets for already-pinned
  // items too (only pins whose target changed are written).
  void OrganizeVirtual(bool quiet = false, bool reassign = false);
  void OrganizeReassign();
  void OnOrganizeDone(struct OrganizeJobResult* job);
  void SetOrganizeStartup(bool on);
  void SetOrganizeWatch(bool on);
  void SetDockVisible(bool on);
  void SetPublicVisible(bool on);
  void SetPetVisible(bool on);
  void ToggleDock();
  void TogglePet();
  void SetAutostart(bool on);
  void ToggleAutostart();
  void RestoreDesktopChrome();
  void EditRules();
  // `drop` optional: when set (drag-out unpin), float lands near that screen point.
  void UnpinPath(const std::wstring& path, const POINT* drop = nullptr);
  void RetargetPin(const std::wstring& from, const std::wstring& to);
  // P1-13: move pins from `src` fence to `dst` fence (pin_order tail), persist,
  // refresh both fences. Returns false (no mutation) for src==dst / missing dst.
  bool TransferPins(const std::wstring& src, const std::wstring& dst,
                    const std::vector<std::wstring>& paths);
  void TogglePeek();
  void EnsureFencesVisible();
  void ToggleFencesVisible();
  void SaveLayoutSnapshot();
  void RestoreLayoutSnapshot();
  void OpenMinutes();
  void RebuildPublicItems();
  void StartRegionCapture();
  void ShowHotkeys();
  void EditHotkeys();
  void CaptureHotkey(const std::wstring& key_id);
  void ApplyHotkeyChord(const std::wstring& key_id, const std::wstring& chord);
  void DispatchSettingsCommand(const std::wstring& id);
  void PinImage(const std::wstring& path);
  void ReregisterHotkeys();
  // P1-28: start/update elevated hotkey helper relay.
  void SyncHotkeyRelay();
  void OnRelayHotkeyAction(const std::string& action);
  void PushExtensionSettings();
  void PushHotkeyState();
  void ApplyUiLanguage();
  void RestartVaultRefreshTimer();
  void ExportPrefs();
  void ImportPrefs();
  void ApplyLoadedGeneralPrefs();
  void OnDesktopBlankDoubleClick();
  // Explorer / Fences: left-click blank desktop clears fence icon selection.
  void OnDesktopBlankLeftDown(LPARAM screen_pt);
  // Organized mode: paste clipboard files onto the real Desktop folder (floats).
  void OnDesktopPaste();
  // Explorer drop onto public float chrome → Desktop folder. Returns OLE effect.
  DWORD OnDesktopDropPaths(const std::vector<std::wstring>& paths);
  // P1-6: double-click toggles stock Explorer desktop ↔ organized DeskTidy view.
  void ShowStockDesktopView();
  void ShowOrganizedDesktopView();
  // Single source of truth for Explorer icon visibility: Shell applies the
  // toggle asynchronously, so loose floats must follow our intent, not the
  // (lagging) live window state.
  void SetIconsVisible(bool visible, bool toast);
  HWND MessageHwnd() const { return message_hwnd_; }
  // P1-28: LL-hook fallback when WM_HOTKEY is swallowed (exclusive fullscreen).
  void TryDispatchLlHotkey(UINT vk);

 private:
  static LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);
  void CreateTrayIcon();
  void RemoveTrayIcon();
  void DeferredOpenSettingsVaultSync();
  bool TryConsumeTrayOpen();
  bool SpawnFence(const FenceLayout& layout);
  void ApplyPageVisibility();
  void ApplyChromeTheme();
  void ApplyTrayThemeIcon();
  void TrimMemory();
  void ShowTrayBalloon(const std::wstring& title, const std::wstring& text);
  COLORREF TrayAccentColor() const;
  DWORD OnPathsDropped(FenceWindow* fence,
                       const std::vector<std::wstring>& paths);
  void OnPathsPasted(FenceWindow* fence, const std::vector<std::wstring>& paths,
                     bool move);
  // P1-13: pin-transfer drop handler + floating hint lifecycle.
  DWORD OnPinDropped(FenceWindow* fence, const std::wstring& src_fence_id,
                     const std::vector<std::wstring>& paths, bool commit);
  void UpdatePinDragHint(POINT cursor, const PinDragHint& hint);
  void HidePinDragHint();
  void DissolveFence(FenceWindow* fence);
  void SetWhitelistForPaths(const std::vector<std::wstring>& paths, bool add);
  // Returns OLE effect for drag sinks (COPY for virtual pin; MOVE/COPY/NONE
  // for portal imports). Paste callers may ignore the return.
  DWORD HandleIncomingPaths(FenceWindow* fence,
                            const std::vector<std::wstring>& paths, bool paste,
                            bool move);
  void RebuildDockItems();
  void WireFence(FenceWindow* fence);
  // heal_geometry: true after display change; false when applying a snapshot
  // (keep exact fence sizes/positions from the saved layout).
  void ReloadFencesFromState(const AppState& state, bool heal_geometry = true);
  void HealLayoutsNow(bool toast);
  void OnDisplayTopologyMaybeChanged();
  void TogglePageLock(int page);
  // U4: single converge action for WM_DISPLAYCHANGE (scale + reload).
  void ConvergeDisplayChange();
  void DispatchHotkey(const std::string& id);
  void ScheduleDesktopBandRestore();
  // False while settings/vault/todos (owned modals) should keep desktop chrome still.
  bool DesktopChromeMayRestack() const;
  // True when current-page overlays are already attached + mapped (settle no-op).
  bool DesktopChromeHealthy() const;
  void StartForegroundHook();
  void StopForegroundHook();
  // Explorer desktop membership (Fences-class): SHChangeNotify + debounce,
  // not a blind PresentLayered patch after every poll tick.
  void StartDesktopShellNotify();
  void StopDesktopShellNotify();
  void ScheduleDesktopContentRefresh();
  void OnDesktopContentRefresh();
  // Worker threads (vault pull / organize) are joined with a bounded wait on
  // shutdown instead of being detached (P1 exit race).
  void ReapFinishedWorkers();
  void JoinWorkers();
  static void CALLBACK ForegroundWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG,
                                          LONG, DWORD, DWORD);

  HINSTANCE instance_ = nullptr;
  HWND message_hwnd_ = nullptr;
  HWINEVENTHOOK fg_hook_ = nullptr;
  std::vector<std::thread> workers_;
  int band_restore_left_ = 0;
  // Last known Win+D / Show Desktop band (DefView TOPMOST). Re-seat only on
  // transition — Rainmeter CheckDesktopState, not every settle tick.
  bool show_desktop_seated_ = false;
  // Tick until which ScheduleDesktopBandRestore is ignored (vault/settings dismiss).
  ULONGLONG suppress_band_restore_until_ = 0;
  std::vector<std::unique_ptr<FenceWindow>> fences_;
  SettingsWindow settings_;
  TodoWindow todos_;
  SearchWindow search_;
  VaultWindow vault_;
  VaultLauncher vault_launcher_;
  DockWindow dock_;
  PetWindow pet_;
  PageBar page_bar_;
  PublicHost public_host_;
  CaptureOverlay capture_;
  FenceRegionOverlay fence_region_;
  std::vector<std::unique_ptr<PinnedImage>> pins_img_;
  // Newest-first paths for show_screenshot walk (Python recent stack).
  std::vector<std::wstring> recent_shots_;
  size_t recent_shot_i_ = 0;
  PinMap pins_;
  FloatPlacementMap float_placements_;
  // Bumped on every interactive pin mutate; organize workers snapshot this so
  // OnOrganizeDone can drop stale results instead of clobbering a fresh drop.
  unsigned long long pins_epoch_ = 0;
  void BumpPinsEpoch() { ++pins_epoch_; }
  std::unordered_map<std::wstring, ULONGLONG> pin_missing_since_;
  ULONGLONG pin_prune_last_ms_ = 0;
  RuleMap rules_;
  std::vector<HotkeyDef> hotkeys_;
  HotkeyRelay hotkey_relay_;
  DesktopWatch desktop_watch_;
  ULONG shell_notify_id_ = 0;
  PIDLIST_ABSOLUTE shell_notify_pidls_[2]{};
  int shell_notify_pidl_count_ = 0;
  AppPrefs prefs_;
  std::string display_fp_;
  int app_dpi_ = 96;
  std::vector<std::wstring> page_names_;
  // U7: per-page lock flags (index = page id). Consulted for fence create/move/
  // delete gating; page_bar lock marker mirrors it. Loaded from AppState.
  std::vector<bool> page_locked_;
  std::vector<SettingsWindow::ExtState::VaultItem> vault_items_cache_;
  std::wstring vault_list_status_;
  int current_page_ = 0;
  bool dock_visible_ = true;
  bool pet_visible_ = true;
  // Float host visible while Explorer icons are hidden; cross-page sharing
  // still follows prefs_.enable_public_desktop inside RebuildPublicItems.
  bool public_visible_ = false;
  // Intended Explorer-icon visibility. Shell applies toggles asynchronously,
  // so AreDesktopIconsVisible() can lag and must not drive float visibility.
  bool icons_visible_intent_ = true;
  bool peek_active_ = false;
  bool fences_hidden_ = false;
  NOTIFYICONDATAW nid_{};
  bool tray_added_ = false;
  UINT_PTR restore_timer_ = 0;
  UINT_PTR pet_timer_ = 0;
  UINT pet_tick_ms_ = 50;
  UINT_PTR watch_timer_ = 0;
  UINT_PTR update_timer_ = 0;
  int update_progress_last_pct_ = -1;
  UINT_PTR vault_timer_ = 0;
  UINT_PTR open_settings_vault_timer_ = 0;
  UINT_PTR display_change_timer_ = 0;
  // 0 idle; 1 add (single converge); 2 remove stage1; 3 remove stage2.
  int display_change_stage_ = 0;
  // Primary work area captured alongside display_fp_ (U4 scale reference).
  RECT display_work_{};
  DWORD last_tray_open_tick_ = 0;
  // Debounce / post-capture suppression per action id (GetTickCount64 ms).
  std::unordered_map<std::string, ULONGLONG> hotkey_last_fire_;
  std::unordered_map<std::string, ULONGLONG> hotkey_suppress_until_;
};
