#pragma once

#include <Windows.h>

#include <string>

// Toast UI Editor hosted in WebView2 (optional; falls back if runtime missing).
class NotesWebView {
 public:
  bool Create(HWND parent);
  void Destroy();
  bool Available() const { return available_; }
  bool Ready() const { return ready_; }
  void Show();
  void Hide();
  void Resize();
  void SetMarkdown(const std::wstring& md);
  // Request current markdown from the page (pumps messages briefly).
  std::wstring PullMarkdown(DWORD timeout_ms = 1500);
  HWND Host() const { return host_; }

 private:
  void NavigateHome();
  void OnReady();
  void OnWebMessage(const std::wstring& json);
  static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleHost(UINT msg, WPARAM wparam, LPARAM lparam);

  HWND parent_ = nullptr;
  HWND host_ = nullptr;
  bool available_ = false;
  bool ready_ = false;
  bool got_md_ = false;
  std::wstring pending_md_;
  std::wstring pending_set_;
  void* controller_ = nullptr;  // ICoreWebView2Controller*
  void* webview_ = nullptr;     // ICoreWebView2*
};
