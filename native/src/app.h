#pragma once

#include "app_prefs.h"
#include "capture_overlay.h"
#include "fence_region_overlay.h"
#include "desktop_watch.h"
#include "dock_window.h"
#include "fence_window.h"
#include "hotkey_store.h"
#include "layout.h"
#include "notes_window.h"
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
#include <shellapi.h>

#include <memory>
#include <string>
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
  void SeatAllFencesUnderApps();
  void PersistLayouts();
  void RefreshSettingsList();
  void RefreshOrganizeSettingsPage();
  void RefreshDesktopIcons();
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
  void CreateFenceOnPage(int page);
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
  void SetFenceOpacityPctAt(int index, int opacity_pct);
  void ApplyDockPrefs();
  void SyncPageChrome();
  void MaybeAutoSnapshot();
  void MaybeStartupUpdateCheck();
  void OnUpdateCheckDone(UpdateCheckResult* r);
  void OnUpdateInstallDone(UpdateInstallResult* r);
  void OnUpdateProgress(int pct);
  void OrganizeVirtual(bool quiet = false);
  void OrganizePhysical();
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
  void UnpinPath(const std::wstring& path);
  void RetargetPin(const std::wstring& from, const std::wstring& to);
  void TogglePeek();
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
  void PushExtensionSettings();
  void PushHotkeyState();
  void ApplyUiLanguage();
  void RestartVaultRefreshTimer();
  void ExportPrefs();
  void ImportPrefs();
  void ApplyLoadedGeneralPrefs();
  void OnDesktopBlankDoubleClick();

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
  COLORREF TrayAccentColor() const;
  void OnPathsDropped(FenceWindow* fence, const std::vector<std::wstring>& paths);
  void OnPathsPasted(FenceWindow* fence, const std::vector<std::wstring>& paths,
                     bool move);
  void DissolveFence(FenceWindow* fence);
  void SetWhitelistForPaths(const std::vector<std::wstring>& paths, bool add);
  void HandleIncomingPaths(FenceWindow* fence,
                           const std::vector<std::wstring>& paths, bool paste,
                           bool move);
  void RebuildDockItems();
  void WireFence(FenceWindow* fence);
  void ReloadFencesFromState(const AppState& state);
  void HealLayoutsNow(bool toast);
  void OnDisplayTopologyMaybeChanged();
  // U7: fence create/move/delete gate — true when the page is locked. System
  // pages 0/1 are always locked (Python is_locked_page_id); user pages honor
  // their page_locked flag. Mirrors PageBar::PageLockedAt / IsPageLocked.
  bool PageLockedForFences(int page) const;
  void TogglePageLock(int page);
  // U4: single converge action for WM_DISPLAYCHANGE (scale + reload).
  void ConvergeDisplayChange();
  void DispatchHotkey(const std::string& id);
  void ScheduleDesktopBandRestore();
  void StartForegroundHook();
  void StopForegroundHook();
  static void CALLBACK ForegroundWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG,
                                          LONG, DWORD, DWORD);

  HINSTANCE instance_ = nullptr;
  HWND message_hwnd_ = nullptr;
  HWINEVENTHOOK fg_hook_ = nullptr;
  int band_restore_left_ = 0;
  std::vector<std::unique_ptr<FenceWindow>> fences_;
  SettingsWindow settings_;
  NotesWindow notes_;
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
  RuleMap rules_;
  std::vector<HotkeyDef> hotkeys_;
  DesktopWatch desktop_watch_;
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
  // Public desktop strip visibility mirrors prefs_.enable_public_desktop.
  bool public_visible_ = false;
  bool peek_active_ = false;
  bool fences_hidden_ = false;
  NOTIFYICONDATAW nid_{};
  bool tray_added_ = false;
  UINT_PTR restore_timer_ = 0;
  UINT_PTR pet_timer_ = 0;
  UINT_PTR watch_timer_ = 0;
  UINT_PTR update_timer_ = 0;
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
