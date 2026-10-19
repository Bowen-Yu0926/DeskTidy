#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Normal APPWINDOW - must stay above Fences-layer overlays.
class SettingsWindow {
 public:
  using VoidFn = std::function<void()>;
  using IndexFn = std::function<void(int)>;

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  HWND Hwnd() const { return hwnd_; }
  bool IsOpen() const;

  void SetFenceNames(const std::vector<std::wstring>& names);
  void SetHandlers(VoidFn on_add, IndexFn on_remove, VoidFn on_seat);

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);

  HWND hwnd_ = nullptr;
  HWND list_ = nullptr;
  HWND btn_add_ = nullptr;
  HWND btn_remove_ = nullptr;
  HWND btn_seat_ = nullptr;
  VoidFn on_add_;
  IndexFn on_remove_;
  VoidFn on_seat_;
};
