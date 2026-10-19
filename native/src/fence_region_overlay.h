#pragma once

#include <Windows.h>

#include <functional>

// Full-screen dim overlay; drag a rectangle to place a new fence.
class FenceRegionOverlay {
 public:
  using DoneFn = std::function<void(const RECT& screen)>;

  bool Begin(HINSTANCE instance, DoneFn on_done);
  void Cancel();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void Finish();

  HWND hwnd_ = nullptr;
  bool dragging_ = false;
  POINT start_{};
  POINT end_{};
  DoneFn on_done_;
};
