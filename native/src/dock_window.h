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
  // P1-10: read dock items from the settings UI field (comma-separated paths,
  // "ext_dock_items") — validates existence, caps at 8, renders.
  void SetItemsFromCsv(const std::wstring& csv);
  // P1-10: read dock items straight from app_prefs (dock_items), cap 8, render.
  void ReloadFromPrefs();
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
  void ApplyPaths(const std::vector<std::wstring>& paths);

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

// P1-10: split a comma-separated path list (settings "ext_dock_items" input),
// trimming whitespace and dropping empties. Used by App to persist dock_items.
std::vector<std::wstring> SplitDockCsv(const std::wstring& csv);
