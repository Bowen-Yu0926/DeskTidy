#pragma once

#include <Windows.h>

#include <functional>

// Small page switcher in the desktop band (page 0 / 1).
class PageBar {
 public:
  using PageFn = std::function<void(int)>;

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  HWND Hwnd() const { return hwnd_; }
  void SetPage(int page);
  void SetOnPage(PageFn fn) { on_page_ = std::move(fn); }
  void AttachToDesktopLayer();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);

  HWND hwnd_ = nullptr;
  int page_ = 0;
  PageFn on_page_;
};
