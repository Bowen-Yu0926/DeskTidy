#pragma once

#include "fence_window.h"
#include "layout.h"
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
  void SeatAllFencesUnderApps();
  void PersistLayouts();
  void RefreshSettingsList();
  void RefreshDesktopIcons();
  void AddFence();
  void RemoveFenceAt(int index);

 private:
  static LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);
  void CreateTrayIcon();
  void RemoveTrayIcon();
  bool SpawnFence(const FenceLayout& layout);

  HINSTANCE instance_ = nullptr;
  HWND message_hwnd_ = nullptr;
  std::vector<std::unique_ptr<FenceWindow>> fences_;
  SettingsWindow settings_;
  NOTIFYICONDATAW nid_{};
  bool tray_added_ = false;
};
