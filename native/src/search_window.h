#pragma once

#include <Windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SearchWindow {
 public:
  struct SearchSharedState {
    std::atomic<unsigned> gen{0};
    std::atomic<bool> alive{true};
  };

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  HWND Hwnd() const { return hwnd_; }
  struct Options {
    bool desktop = true;
    bool documents = true;
    bool downloads = true;
    // Settings flag only; disk chips stay empty until the user picks them.
    // Empty chips → Desktop/Documents/Downloads (never auto-check C:).
    bool all_drives = false;
    int max_results = 50;
    int max_depth = 12;
    bool include_open_folders = true;
    bool include_hidden = false;
    int threads = 2;
  };
  void SetOptions(const Options& opt) {
    options_ = opt;
    if (options_.threads < 1) {
      options_.threads = 1;
    }
    if (options_.threads > 64) {
      options_.threads = 64;
    }
    // Do not auto-select drive chips (esp. C:) from prefs — user picks explicitly.
  }
  void SetOnPathReplaced(
      std::function<void(const std::wstring&, const std::wstring&)> fn) {
    on_path_replaced_ = std::move(fn);
  }
  void SetOnRemoved(std::function<void(const std::wstring&)> fn) {
    on_removed_ = std::move(fn);
  }
  void SetOnAllDrivesChanged(std::function<void(bool)> fn) {
    on_all_drives_ = std::move(fn);
  }
  bool AllDrives() const { return options_.all_drives; }

  static std::vector<std::wstring> EnumFixedDriveLetters();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  bool CreateWebView();
  void DestroyWebView();
  void ResizeWebView();
  void NavigateHome();
  void PushState(const std::wstring& status = {});
  void PushProgress(int pct, const std::wstring& status,
                    const std::vector<std::wstring>* live_items = nullptr);
  void OnWebMessage(const std::wstring& json);
  void RunSearch(const std::wstring& needle);
  void OpenAt(int index);
  void RevealAt(int index);
  void ContextAt(int index, int screen_x, int screen_y);
  void ParseTypesFromJson(const std::wstring& json);
  std::vector<std::wstring> ResolvedExtensions() const;
  std::vector<std::wstring> BuildRoots() const;
  void NotifyAllDrivesPref();

  HWND hwnd_ = nullptr;
  HWND host_ = nullptr;
  void* controller_ = nullptr;
  void* webview_ = nullptr;
  bool web_available_ = false;
  bool web_ready_ = false;
  bool dragging_ = false;
  POINT drag_origin_{};
  POINT win_origin_{};
  std::wstring query_;
  std::vector<std::wstring> results_;
  std::vector<std::wstring> type_filters_;
  // Drive letters without colon, e.g. L"C". Empty = user folders only.
  std::vector<std::wstring> selected_drives_;
  Options options_;
  std::shared_ptr<SearchSharedState> shared_state_ =
      std::make_shared<SearchSharedState>();
  bool searching_ = false;
  int progress_pct_ = 0;
  std::function<void(const std::wstring&, const std::wstring&)> on_path_replaced_;
  std::function<void(const std::wstring&)> on_removed_;
  std::function<void(bool)> on_all_drives_;
};
