#pragma once

#include <Windows.h>

#include <string>

class PinnedImage {
 public:
  bool Create(HINSTANCE instance, const std::wstring& path);
  void Destroy();
  void Show();
  HWND Hwnd() const { return hwnd_; }

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void ApplyAlpha();
  void ShowContextMenu(POINT screen);
  void SaveAs();
  void CopyToClipboard();
  // True when a client point lands on the top-right close (×) button.
  bool InCloseButton(POINT client) const;

  HWND hwnd_ = nullptr;
  HBITMAP bmp_ = nullptr;
  int bmp_w_ = 0;
  int bmp_h_ = 0;
  BYTE alpha_ = 255;
  std::wstring path_;
  bool dragging_ = false;
  POINT drag_offset_{};
};
