#pragma once

#include "desktop_items.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Cross-page public icon strip (always visible on desktop band).
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

 private:
  struct Slot {
    DesktopItem item;
    HICON icon = nullptr;
    RECT bounds{};
  };
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void Layout();
  int HitTest(int x, int y) const;

  void ShowSlotMenu(int index, POINT screen);

  HWND hwnd_ = nullptr;
  std::vector<Slot> slots_;
  std::function<void(const std::wstring&, const std::wstring&)> on_path_replaced_;
  std::function<void(const std::wstring&)> on_removed_;
};
