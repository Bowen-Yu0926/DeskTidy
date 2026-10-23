#pragma once

#include <Windows.h>

#include <functional>
#include <string>

// Hosts a WebView2 surface for the notes window. Two kinds are supported:
//   kWysiwyg — the ToastUI markdown editor (edits markdown in place).
//   kViewer  — the offline markdown preview (read-only render + PDF export).
class NotesWebView {
 public:
  enum class Kind { kWysiwyg, kViewer };

  bool Create(HWND parent);
  void Destroy();
  bool Available() const { return available_; }
  bool Ready() const { return ready_; }
  void Show();
  void Hide();
  void Resize();
  void SetKind(Kind kind) { kind_ = kind; }
  // Preview/editor chrome theme: "light" or "dark" (P2-30).
  void SetTheme(const std::wstring& theme) { theme_ = theme; }
  const std::wstring& Theme() const { return theme_; }

  // WYSIWYG editor (kWysiwyg).
  void SetMarkdown(const std::wstring& md);
  std::wstring PullMarkdown(DWORD timeout_ms = 1500);

  // Preview viewer (kViewer).
  // `base_dir` is the note's directory, used to resolve relative images.
  void SetPreview(const std::wstring& md, const std::wstring& base_dir);
  void SetScrollRatio(double ratio);
  double RequestScrollRatio(DWORD timeout_ms = 800);
  // Export the current page to PDF via ICoreWebView2_7. Returns false and fills
  // `err` on failure.
  bool PrintToPdf(const std::wstring& path, std::wstring* err);

  HWND Host() const { return host_; }

  // Invoked with the document scroll ratio (0..1) whenever the page scrolls.
  std::function<void(double)> on_scroll;

 private:
  void NavigateHome();
  void OnReady();
  void OnWebMessage(const std::wstring& json);
  static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleHost(UINT msg, WPARAM wparam, LPARAM lparam);

  HWND parent_ = nullptr;
  HWND host_ = nullptr;
  Kind kind_ = Kind::kWysiwyg;
  bool available_ = false;
  bool ready_ = false;
  bool got_md_ = false;
  std::wstring theme_ = L"light";
  std::wstring pending_md_;
  std::wstring pending_set_;
  std::wstring pending_base_;
  bool got_scroll_ = false;
  double scroll_ratio_ = 0.0;
  bool print_done_ = false;
  bool print_ok_ = false;
  void* controller_ = nullptr;  // ICoreWebView2Controller*
  void* webview_ = nullptr;     // ICoreWebView2*
};
