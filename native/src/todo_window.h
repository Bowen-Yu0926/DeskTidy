#pragma once

#include <Windows.h>

#include <string>
#include <vector>

class TodoWindow {
 public:
  struct Item {
    bool done = false;
    std::wstring text;
    // Python todos.json interop (id / created_at) — kept but not shown in UI.
    std::string id;
    std::string created_at;
    // Unknown JSON fields preserved verbatim (raw "key":value fragments).
    std::vector<std::string> extra;
  };

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  // Match Python DesktopTodoWidget theme palette (settings ui_theme).
  void ApplyUiTheme(const std::string& ui_theme);
  HWND Hwnd() const { return hwnd_; }

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  bool CreateWebView();
  void DestroyWebView();
  void ResizeWebView();
  void NavigateHome();
  void PushState();
  void OnWebMessage(const std::wstring& json);
  void Load();
  void Save();
  void LoadGeo();
  void PersistGeo();

  HWND hwnd_ = nullptr;
  HWND host_ = nullptr;
  void* controller_ = nullptr;
  void* webview_ = nullptr;
  bool web_available_ = false;
  bool web_ready_ = false;
  bool dragging_ = false;
  POINT drag_origin_{};
  POINT win_origin_{};
  std::vector<Item> items_;
  std::string ui_theme_ = "mist";
  bool dirty_ = false;
};
