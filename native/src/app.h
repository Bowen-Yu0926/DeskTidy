#pragma once

#include "dock_window.h"
#include "fence_window.h"
#include "layout.h"
#include "notes_window.h"
#include "page_bar.h"
#include "pet_window.h"
#include "pin_store.h"
#include "settings_window.h"

#include <Windows.h>
#include <shellapi.h>

#include <memory>
#include <vector>

class App {
 public:
  bool Init(HINSTANCE instance);
  int Run();
  void Shutdown();

  void OpenSettings();
  void OpenNotes();
  void SeatAllFencesUnderApps();
  void PersistLayouts();
  void RefreshSettingsList();
  void RefreshDesktopIcons();
  void AddFence();
  void RemoveFenceAt(int index);
  void SetPage(int page);
  void OrganizeVirtual();
  void OrganizePhysical();
  void ToggleDock();
  void TogglePet();
  void RestoreDesktopChrome();

 private:
  static LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);
  void CreateTrayIcon();
  void RemoveTrayIcon();
  bool SpawnFence(const FenceLayout& layout);
  void ApplyPageVisibility();
  void OnPathsDropped(FenceWindow* fence, const std::vector<std::wstring>& paths);
  void RebuildDockItems();

  HINSTANCE instance_ = nullptr;
  HWND message_hwnd_ = nullptr;
  std::vector<std::unique_ptr<FenceWindow>> fences_;
  SettingsWindow settings_;
  NotesWindow notes_;
  DockWindow dock_;
  PetWindow pet_;
  PageBar page_bar_;
  PinMap pins_;
  int current_page_ = 0;
  bool dock_visible_ = true;
  bool pet_visible_ = true;
  NOTIFYICONDATAW nid_{};
  bool tray_added_ = false;
  UINT_PTR restore_timer_ = 0;
  UINT_PTR pet_timer_ = 0;
};
