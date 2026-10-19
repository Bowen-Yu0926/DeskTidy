#pragma once

#include "fence_window.h"
#include "settings_window.h"

#include <Windows.h>
#include <shellapi.h>

class App {
 public:
  bool Init(HINSTANCE instance);
  int Run();
  void Shutdown();

  void OpenSettings();
  void SeatFenceUnderApps();

 private:
  static LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);
  void CreateTrayIcon();
  void RemoveTrayIcon();

  HINSTANCE instance_ = nullptr;
  HWND message_hwnd_ = nullptr;
  FenceWindow fence_;
  SettingsWindow settings_;
  NOTIFYICONDATAW nid_{};
  bool tray_added_ = false;
};
