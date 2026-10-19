#pragma once

#include <Windows.h>

#include <functional>

// Normal APPWINDOW - must stay above Fences-layer overlays.
class SettingsWindow {
 public:
  using ShowSettingsFn = std::function<void()>;

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  HWND Hwnd() const { return hwnd_; }
  bool IsOpen() const;

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);

  HWND hwnd_ = nullptr;
  HWND button_ = nullptr;
};
