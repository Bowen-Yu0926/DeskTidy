#pragma once

#include "capture_overlay.h"
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
#include "vault_window.h"

#include <Windows.h>
#include <shellapi.h>

#include <memory>
#include <string>
#include <vector>

class App {
 public:
  bool Init(HINSTANCE instance);
  int Run();
  void Shutdown();
  void HandleCliAction(const std::wstring& action);

  void OpenSettings();
  void OpenNotes();
  void OpenTodos();
  void OpenSearch();
  void OpenVault();
  void SeatAllFencesUnderApps();
  void PersistLayouts();
  void RefreshSettingsList();
  void RefreshDesktopIcons();
  void AddFence();
  void RemoveFenceAt(int index);
  void RenameFenceAt(int index, const std::wstring& title);
  void SetFencePageAt(int index, int page);
  void SetPage(int page);
  void OrganizeVirtual();
  void OrganizePhysical();
  void SetDockVisible(bool on);
  void SetPetVisible(bool on);
  void ToggleDock();
  void TogglePet();
  void SetAutostart(bool on);
  void ToggleAutostart();
  void RestoreDesktopChrome();
  void EditRules();
  void UnpinPath(const std::wstring& path);
  void TogglePeek();
  void ToggleFencesVisible();
  void SaveLayoutSnapshot();
  void RestoreLayoutSnapshot();
  void OpenMinutes();
  void RebuildPublicItems();
  void StartRegionCapture();
  void ShowHotkeys();
  void EditHotkeys();
  void PinImage(const std::wstring& path);
  void ReregisterHotkeys();

 private:
  static LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);
  void CreateTrayIcon();
  void RemoveTrayIcon();
  bool SpawnFence(const FenceLayout& layout);
  void ApplyPageVisibility();
  void OnPathsDropped(FenceWindow* fence, const std::vector<std::wstring>& paths);
  void RebuildDockItems();
  void WireFence(FenceWindow* fence);
  void ReloadFencesFromState(const AppState& state);
  void DispatchHotkey(const std::string& id);

  HINSTANCE instance_ = nullptr;
  HWND message_hwnd_ = nullptr;
  std::vector<std::unique_ptr<FenceWindow>> fences_;
  SettingsWindow settings_;
  NotesWindow notes_;
  TodoWindow todos_;
  SearchWindow search_;
  VaultWindow vault_;
  DockWindow dock_;
  PetWindow pet_;
  PageBar page_bar_;
  PublicHost public_host_;
  CaptureOverlay capture_;
  std::vector<std::unique_ptr<PinnedImage>> pins_img_;
  PinMap pins_;
  RuleMap rules_;
  std::vector<HotkeyDef> hotkeys_;
  DesktopWatch desktop_watch_;
  int current_page_ = 0;
  bool dock_visible_ = true;
  bool pet_visible_ = true;
  bool public_visible_ = true;
  bool peek_hidden_ = false;
  bool fences_hidden_ = false;
  NOTIFYICONDATAW nid_{};
  bool tray_added_ = false;
  UINT_PTR restore_timer_ = 0;
  UINT_PTR pet_timer_ = 0;
  UINT_PTR watch_timer_ = 0;
};
