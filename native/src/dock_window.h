#pragma once

#include "desktop_items.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Bottom-edge shortcut dock in the desktop band.
class DockWindow {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  void SetVisible(bool on);
  HWND Hwnd() const { return hwnd_; }
  void SetItems(std::vector<DesktopItem> items);
  // edge: "bottom" | "left" | "right"
  void SetEdge(const std::wstring& edge);
  void SetOpacity(float opacity);  // 0.4 – 1.0
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
  void Present();
  void LayoutSlots();
  void ApplyVisibility();
  int HitTest(int x, int y) const;

  void ShowSlotMenu(int index, POINT screen);
  void EnsureTooltip();
  void UpdateHoverTip(int index);

  HWND hwnd_ = nullptr;
  HWND tip_ = nullptr;
  bool want_visible_ = false;
  std::wstring edge_ = L"bottom";
  BYTE fill_alpha_ = 217;  // ≈ 0.85
  int hover_index_ = -1;
  std::vector<Slot> slots_;
  std::function<void(const std::wstring&, const std::wstring&)> on_path_replaced_;
  std::function<void(const std::wstring&)> on_removed_;
};
