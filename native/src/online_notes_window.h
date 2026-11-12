#pragma once

#include "vault_http.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Vault-styled online notes panel (list + Toast UI editor). Syncs via the same
// cloud account as the account vault; local DeskNote library is untouched.
class OnlineNotesWindow {
 public:
  using VoidFn = std::function<void()>;
  using OpenLocalFn = std::function<void(const std::wstring& path)>;

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  void SetOnHide(VoidFn fn) { on_hide_ = std::move(fn); }
  // After「转本地」, open the landed .md in DeskNote.
  void SetOnOpenLocal(OpenLocalFn fn) { on_open_local_ = std::move(fn); }
  HWND Hwnd() const { return hwnd_; }
  void PlaceBeside(HWND anchor);
  // Focus / create-then-focus a remote note (used by DeskNote「转在线」).
  void ShowNoteId(int id);
  // Open panel and create a blank online note (login if needed).
  void ShowAndCreateNew();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  bool CreateWebView();
  void DestroyWebView();
  void ResizeWebView();
  void NavigateHome();
  void OnWebReady();
  void PushState();
  void OnWebMessage(const std::wstring& json);
  void DispatchAction(const std::wstring& json);
  void EnsureLogin();
  void RefreshList();
  void OpenNote(int id);
  void CreateNote();
  void SaveCurrent();
  void DeleteCurrent();
  void ConvertToLocal(const std::wstring& title, const std::wstring& body);

  HWND hwnd_ = nullptr;
  HWND host_ = nullptr;
  void* controller_ = nullptr;
  void* webview_ = nullptr;
  bool web_available_ = false;
  bool web_ready_ = false;
  bool dragging_ = false;
  POINT drag_last_{};
  VoidFn on_hide_;
  OpenLocalFn on_open_local_;
  std::vector<std::wstring> pending_action_jsons_;
  std::vector<VaultHttpNote> notes_;
  int current_id_ = 0;
  std::wstring current_title_;
  std::wstring current_body_;
  std::wstring status_;
};
