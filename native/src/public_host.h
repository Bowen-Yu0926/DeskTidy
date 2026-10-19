#pragma once

#include "desktop_items.h"

#include <Windows.h>

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

  HWND hwnd_ = nullptr;
  std::vector<Slot> slots_;
};
