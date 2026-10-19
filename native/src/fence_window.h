#pragma once

#include <Windows.h>

#include <string>

class FenceWindow {
 public:
  bool Create(HINSTANCE instance, int x, int y, int w, int h,
              const std::wstring& title);
  void Destroy();
  HWND Hwnd() const { return hwnd_; }
  void Show();
  void AttachToDesktopLayer();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);

  HWND hwnd_ = nullptr;
  bool dragging_ = false;
  POINT drag_offset_{};
  std::wstring title_;
};
