#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <Windows.h>

#include "hotkey_store.h"

#include <string>
#include <vector>

// Main-app side of P1-28: localhost listener + ShellExecuteW("runas") helper.
// On each relayed action, posts ``notify_msg`` to ``notify_hwnd`` with
// LPARAM = new std::string* (action id); caller must delete it.
class HotkeyRelay {
 public:
  HotkeyRelay() = default;
  ~HotkeyRelay();

  HotkeyRelay(const HotkeyRelay&) = delete;
  HotkeyRelay& operator=(const HotkeyRelay&) = delete;

  // Bind listener + write mapping. Elevated helper is opt-in (UAC) via
  // ``launch_elevated`` — never prompt on every cold start by default.
  // Returns true when the localhost relay is up (helper may be skipped).
  bool Start(HWND notify_hwnd, UINT notify_msg,
             const std::vector<HotkeyDef>& keys, bool launch_elevated = false);
  // Rewrite mapping JSON; helper reloads on mtime change when already running.
  void UpdateMapping(const std::vector<HotkeyDef>& keys);
  // Explicit UAC launch (settings / user request). False if cancelled.
  bool EnsureElevatedHelper();
  void Stop();
  bool Running() const { return sock_ != INVALID_SOCKET; }
  bool HelperLaunched() const { return helper_launched_; }

 private:
  bool WriteMapping(const std::vector<HotkeyDef>& keys);
  bool LaunchHelper();
  void ServeLoop();
  static DWORD WINAPI ThreadProc(LPVOID param);

  SOCKET sock_ = INVALID_SOCKET;
  HANDLE thread_ = nullptr;
  volatile LONG stop_ = 0;
  int port_ = 0;
  std::wstring mapping_path_;
  HWND notify_hwnd_ = nullptr;
  UINT notify_msg_ = 0;
  bool helper_launched_ = false;
};

// Fences-class Win+D: minimize monitor-covering APPWINDOW (game/fullscreen)
// so show-desktop can reveal the desktop band instead of only lifting fences.
bool MinimizeFullscreenCover();
