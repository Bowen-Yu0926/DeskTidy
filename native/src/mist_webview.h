#pragma once

#include <Windows.h>

#include <functional>
#include <string>

// Shared mist-card WebView2 modal host (whitelist / rules / snapshot).
// Do NOT call MistWebDialogRun from inside another WebView2 WebMessageReceived
// without PostMessage deferral — nested create leaves a blank caption window.
struct MistWebDialog {
  HWND hwnd = nullptr;
  HWND owner = nullptr;
  HWND host = nullptr;
  void* controller = nullptr;  // ICoreWebView2Controller*
  void* webview = nullptr;     // ICoreWebView2*
  bool done = false;
  bool web_ready = false;
  bool web_available = false;
  // When false, owner stays enabled so the user can edit while the dialog is
  // open (DeskNote find/replace — Python setModal(False)).
  bool disable_owner = true;
  // Borderless popup — HTML draws the card / close button (DeskNote mist UI).
  bool frameless = false;
  // Optional: translate owner accelerators (F3, etc.) while the dialog loop runs.
  HACCEL owner_accel = nullptr;
  std::wstring html_path;
  std::wstring user_data_subdir;
  void* user = nullptr;
  std::function<std::wstring()> build_state;
  // Return true to close the dialog after handling.
  std::function<bool(const std::wstring& json)> on_message;
  // Optional: run off the WebMessageReceived stack via
  // PostMessageW(hwnd, WM_APP+81, wparam, lparam). Use for folder pickers /
  // nested PromptText (SHBrowse / Mist from inside on_message freezes).
  std::function<void(WPARAM, LPARAM)> on_deferred;
};

// Blocks until the dialog closes. Returns after WM_DESTROY.
void MistWebDialogRun(MistWebDialog* dlg, const wchar_t* title, int width,
                      int height);
void MistWebDialogClose(MistWebDialog* dlg);
void MistWebDialogPushState(MistWebDialog* dlg);
void MistWebDialogPostJson(MistWebDialog* dlg, const std::wstring& json);

std::wstring MistJsonEscape(const std::wstring& s);
std::wstring MistJsonStringField(const std::wstring& json, const wchar_t* key);
int MistJsonIntField(const std::wstring& json, const wchar_t* key, int fallback);
std::wstring MistFindAssetHtml(const wchar_t* relative_under_assets);
