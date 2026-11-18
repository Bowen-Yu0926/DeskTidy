#pragma once

#include <Windows.h>

#include <string>

// Messages posted back to `owner` (the App message window) by the background
// update workers. lParam carries a heap-allocated result the UI thread frees.
constexpr UINT kMsgUpdateCheckDone = WM_APP + 101;
constexpr UINT kMsgUpdateInstallDone = WM_APP + 102;
constexpr UINT kMsgUpdateProgress = WM_APP + 103;
// Posted after the installer has been launched and waited-for-idle on a
// background thread; lParam is a heap-allocated UpdateInstallResult* (reuses
// `ok` + `error` + `path` fields) that the UI thread frees.
constexpr UINT kMsgUpdateInstallLaunched = WM_APP + 104;

// Current product version (matches version.rc FileVersion, pure X.Y.Z).
std::wstring NativeProductVersion();
// Human-facing version label (`X.Y.Z-dev` when running from source, else `X.Y.Z`).
std::wstring NativeDisplayVersion();

struct UpdateCheckResult {
  // 0 = error, 1 = up-to-date, 2 = update available.
  int status = 0;
  bool interactive = false;  // manual check toasts progress / up-to-date.
  std::wstring current;
  std::wstring latest;
  std::wstring html_url;
  std::wstring error;
};

struct UpdateInstallResult {
  bool ok = false;            // downloaded + verified + launched.
  bool has_checksum = false;  // release published a sha256 digest.
  bool verify_ok = false;     // digest (or size) verification passed.
  std::wstring path;          // installer path — kept so failures can be shown.
  std::wstring error;
};

// Start a background update check. Posts kMsgUpdateCheckDone with a heap
// UpdateCheckResult*. force=true ignores the 6h disk cache.
void StartUpdateCheckAsync(HWND owner, bool force, bool interactive);

// Start background download → verify → post kMsgUpdateInstallDone. The caller
// (UI thread) asks the user to confirm and then calls LaunchInstallerSilent.
void StartUpdateInstallAsync(HWND owner);

// Launch the downloaded installer (/SILENT keeps Inno progress UI).
// Returns false + err on failure.
bool LaunchInstallerSilent(const std::wstring& path, std::wstring* err);

// Launch the installer on a background thread (so WaitForInputIdle/Sleep never
// block the UI thread), then post kMsgUpdateInstallLaunched back to `owner`
// with a heap-allocated UpdateInstallResult* (ok/error/path). Captures only
// path + HWND, never `this`.
void StartInstallerLaunchAsync(HWND owner, const std::wstring& path);

// Legacy manual-check entry (tools_misc OpenUpdatePage): async + forced refresh.
void CheckForUpdates(HWND owner);
