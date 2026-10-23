#pragma once

#include "vault_http.h"

#include <Windows.h>

#include <string>
#include <vector>

// Desktop account vault panel — WebView2 UI aligned with Python AccountVaultWidget.
class VaultWindow {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  // Prefetch WebView2 so the first buoy click does not cold-start the runtime.
  void WarmWebView();
  HWND Hwnd() const { return hwnd_; }
  // 0 = never auto-clear; else seconds after password copy only.
  void SetClipboardClearSeconds(int seconds);
  // Background pull when session has a token (enc_ver=0 path). Returns true if merged.
  // Must run on the UI thread (mutates entries_ / WebView).
  bool QuietCloudRefresh();
  // Snapshot local rows for a worker-thread VaultHttpPullMerge (no UI touches).
  std::vector<VaultHttpCredential> SnapshotCredentials() const;
  // Apply a pull-merge result on the UI thread after a worker finishes.
  void ApplyPulledCredentials(const std::vector<VaultHttpCredential>& remote);
  // Prompt login/password and store session token.
  void CloudLogin();
  // Local DPAPI vault file export/import (contains passwords).
  void ExportBackup();
  void ImportBackup();
  // Place beside an anchor HWND (vault buoy); nullptr → default corner.
  void PlaceBeside(HWND anchor);

  // Settings page list (no secrets — pass_len only).
  struct ListRow {
    int index = 0;
    std::wstring title;
    std::wstring username;
    std::wstring url;
    std::wstring note;
    int pass_len = 0;
  };
  void ReloadFromDisk();
  std::vector<ListRow> ListRows() const;
  void CopyPasswordAt(int index);
  void CopyUsernameAt(int index);
  void CopyUrlAt(int index);
  void DeleteAt(int index);

 private:
  struct Entry {
    std::wstring title;
    std::wstring user;
    std::wstring pass;
    std::wstring url;
    std::wstring note;
    bool pinned = false;
    int sort_order = 0;
    // true only when sort_order was explicitly assigned (cloud pull) — local
    // DPAPI rows have no sort_order and keep their file/insertion order.
    bool has_sort_order = false;
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  bool CreateWebView();
  void DestroyWebView();
  void ResizeWebView();
  void NavigateHome();
  void PushState(const std::wstring& generated_password = L"");
  void OnWebMessage(const std::wstring& json);
  void DispatchAction(const std::wstring& json);
  void OnWebReady();
  void Load();
  void Save();
  void SortEntries();
  void CopyNoteAt(int index);
  void SaveEntryFromJson(const std::wstring& json);
  void CopyFieldToClipboard(const std::wstring& value, const wchar_t* tip,
                            bool schedule_clear);
  void ClearClipboardIfOurs();
  void CloudRegister();
  void CloudSync();
  void StartCloudRefresh();
  // After local add/edit/delete/import: push mirror to cloud when logged in (P0-8).
  void ScheduleCloudPush();
  void GeneratePassword(const std::wstring& length, const std::wstring& charset);

  int clipboard_clear_seconds_ = 30;
  HWND hwnd_ = nullptr;
  HWND host_ = nullptr;
  void* controller_ = nullptr;
  void* webview_ = nullptr;
  bool web_ready_ = false;
  bool web_available_ = false;
  std::vector<std::wstring> pending_action_jsons_;
  std::vector<Entry> entries_;
  bool dirty_ = false;
  std::wstring clip_secret_;
  UINT_PTR clip_timer_ = 0;
  bool dragging_ = false;
  POINT drag_origin_{};
  POINT drag_start_{};
};
