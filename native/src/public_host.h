#pragma once

#include "desktop_items.h"
#include "display_geom.h"
#include "float_placements.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Loose desktop float host (Fences / Explorer public icons).
class PublicHost {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  void SetVisible(bool on);
  HWND Hwnd() const { return hwnd_; }
  bool HasItems() const { return !slots_.empty(); }
  bool HasSelection() const;
  void SetItems(std::vector<DesktopItem> items);
  void SetPlacements(FloatPlacementMap placements);
  void SetBlockedRects(std::vector<RECT> rects);
  void Reflow();
  void AttachToDesktopLayer();
  void SetOnPathReplaced(
      std::function<void(const std::wstring&, const std::wstring&)> fn) {
    on_path_replaced_ = std::move(fn);
  }
  void SetOnRemoved(std::function<void(const std::wstring&)> fn) {
    on_removed_ = std::move(fn);
  }
  void SetOnRefresh(std::function<void()> fn) { on_refresh_ = std::move(fn); }
  void SetOnPlacementsChanged(
      std::function<void(const FloatPlacementMap&)> fn) {
    on_placements_changed_ = std::move(fn);
  }
  void SetTitle(const std::wstring& title) { title_ = title; }
  void SetCurrentPage(int page) { current_page_ = page; }

  // Screen-space slot rects for “draw fence around icons” scoop.
  std::vector<std::pair<std::wstring, RECT>> SlotScreenRects() const;

 private:
  struct Slot {
    DesktopItem item;
    HICON icon = nullptr;
    RECT bounds{};
    RECT icon_rc{};
  };
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void Present();
  void Layout();
  void ApplyVisibility();
  int HitTest(int x, int y) const;
  void ReorderTo(int from, int to);
  void PersistSlotPlacements();
  void BeginOleDrag();
  void ShowSlotMenu(int index, POINT screen);

  void EnsureSelSize();
  void ClearSelection();
  void SelectOnly(int index);
  void ToggleSelected(int index);
  void SelectRange(int from, int to);
  void SelectIntersecting(RECT client_rc);
  bool IsSelected(int index) const;
  std::vector<std::wstring> SelectedPaths() const;
  void DeleteSelection();
  void RenameSelection();
  void OpenSelection();

  HWND hwnd_ = nullptr;
  bool want_visible_ = false;
  IconGridMetrics metrics_{};
  std::wstring title_ = L"桌面图标";
  int drag_from_ = -1;
  bool drag_moved_ = false;
  POINT drag_press_{};
  bool rubber_ = false;
  POINT rubber_origin_{};
  RECT rubber_rc_{};
  std::vector<char> selected_;
  int selection_anchor_ = -1;
  int current_page_ = 0;
  FloatPlacementMap placements_;
  std::vector<Slot> slots_;
  std::vector<RECT> blocked_;
  std::function<void(const std::wstring&, const std::wstring&)> on_path_replaced_;
  std::function<void(const std::wstring&)> on_removed_;
  std::function<void()> on_refresh_;
  std::function<void(const FloatPlacementMap&)> on_placements_changed_;
};
