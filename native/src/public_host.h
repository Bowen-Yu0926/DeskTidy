#pragma once

#include "desktop_items.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Left-edge public desktop icon host (Python public_desktop.py /
// public_icon_host.py). Draws Public Desktop icons on a Windows-like
// column-major grid; empty cells stay click-through (per-pixel alpha).
class PublicHost {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  void SetVisible(bool on);
  HWND Hwnd() const { return hwnd_; }
  void SetItems(std::vector<DesktopItem> items);
  void AttachToDesktopLayer();
  void SetOnPathReplaced(
      std::function<void(const std::wstring&, const std::wstring&)> fn) {
    on_path_replaced_ = std::move(fn);
  }
  void SetOnRemoved(std::function<void(const std::wstring&)> fn) {
    on_removed_ = std::move(fn);
  }
  void SetOnRefresh(std::function<void()> fn) { on_refresh_ = std::move(fn); }

 private:
  struct Slot {
    DesktopItem item;
    HICON icon = nullptr;
    RECT bounds{};
  };
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void Present();
  void Layout();
  void ApplyVisibility();
  int HitTest(int x, int y) const;

  void ShowSlotMenu(int index, POINT screen);

  HWND hwnd_ = nullptr;
  bool want_visible_ = false;
  std::vector<Slot> slots_;
  std::function<void(const std::wstring&, const std::wstring&)> on_path_replaced_;
  std::function<void(const std::wstring&)> on_removed_;
  std::function<void()> on_refresh_;
};
