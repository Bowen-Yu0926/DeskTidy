#pragma once

#include <Windows.h>

#include <functional>
#include <string>

// Full-screen dim overlay; drag to select a region, save PNG, optional pin.
class CaptureOverlay {
 public:
  using DoneFn = std::function<void(const std::wstring& png_path, bool pin)>;

  bool Begin(HINSTANCE instance, DoneFn on_done);
  void Cancel();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  bool SaveSelection();

  HWND hwnd_ = nullptr;
  HINSTANCE instance_ = nullptr;
  bool dragging_ = false;
  POINT start_{};
  POINT end_{};
  DoneFn on_done_;
};
