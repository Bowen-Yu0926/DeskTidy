#pragma once

#include <Windows.h>

#include <string>

class PinnedImage {
 public:
  PinnedImage();
  ~PinnedImage();

  bool Create(HINSTANCE instance, const std::wstring& path);
  bool CreateFromHBitmap(HINSTANCE instance, HBITMAP hbmp, const std::wstring& cache_path = L"");
  void Destroy();
  void Show();
  HWND Hwnd() const { return hwnd_; }
  const std::wstring& Path() const { return path_; }
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

 private:
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void ApplyAlpha();
  void ShowContextMenu(POINT screen);
  void SaveAs();
  void CopyToClipboard();
  void Zoom(float factor, POINT anchor_screen);
  void ResetZoom();
  bool InCloseButton(POINT client) const;

  HWND hwnd_ = nullptr;
  HBITMAP bmp_ = nullptr;
  int orig_w_ = 0;
  int orig_h_ = 0;
  float zoom_ = 1.0f;
  BYTE alpha_ = 255;
  std::wstring path_;
  bool dragging_ = false;
  POINT drag_offset_{};
  bool close_hover_ = false;
};
