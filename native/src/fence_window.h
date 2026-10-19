#pragma once

#include "layout.h"

#include <Windows.h>

#include <functional>
#include <string>

class FenceWindow {
 public:
  using MovedFn = std::function<void(FenceWindow*)>;

  bool Create(HINSTANCE instance, const FenceLayout& layout);
  void Destroy();
  HWND Hwnd() const { return hwnd_; }
  const std::string& Id() const { return id_; }
  const std::wstring& Title() const { return title_; }
  void SetTitle(const std::wstring& title);
  FenceLayout CurrentLayout() const;
  void Show();
  void AttachToDesktopLayer();
  void SetOnMoved(MovedFn fn) { on_moved_ = std::move(fn); }

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);

  HWND hwnd_ = nullptr;
  bool dragging_ = false;
  POINT drag_offset_{};
  std::string id_;
  std::wstring title_;
  MovedFn on_moved_;
};
