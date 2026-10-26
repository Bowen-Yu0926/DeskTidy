#pragma once

#include "desktop_items.h"
#include "display_geom.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Left-edge float icon host (Python public_desktop.py / public_icon_host.py).
// Shows unpinned desktop items (shared public + loose when Explorer icons are
// hidden) on a Windows-like column-major grid; empty cells stay click-through.
class PublicHost {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  void SetVisible(bool on);
  HWND Hwnd() const { return hwnd_; }
  // False when every desktop item is pinned to a fence: the host then has no
  // floats and must stay hidden (an empty host shows a bare header plate and
  // swallows clicks over the wallpaper).
  bool HasItems() const { return !slots_.empty(); }
  void SetItems(std::vector<DesktopItem> items);
  // Screen-space fence frames to skip when placing floats (Python
  // collect_fence_rects / _slot_blocked). Call before SetItems/Layout.
  void SetBlockedRects(std::vector<RECT> rects);
  // Re-place existing floats around new blocked rects without reloading icons
  // (cheap path for page switches).
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
  void SetTitle(const std::wstring& title) { title_ = title; }

 private:
  struct Slot {
    DesktopItem item;
    HICON icon = nullptr;
    RECT bounds{};   // full cell
    RECT icon_rc{};  // icon rect inside cell
  };
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void Present();
  void Layout();
  void ApplyVisibility();
  int HitTest(int x, int y) const;
  void ReorderTo(int from, int to);

  void ShowSlotMenu(int index, POINT screen);

  HWND hwnd_ = nullptr;
  bool want_visible_ = false;
  IconGridMetrics metrics_{};
  std::wstring title_ = L"桌面图标";
  int drag_from_ = -1;
  bool drag_moved_ = false;
  std::vector<Slot> slots_;
  std::vector<RECT> blocked_;  // screen-space fence rects
  std::function<void(const std::wstring&, const std::wstring&)> on_path_replaced_;
  std::function<void(const std::wstring&)> on_removed_;
  std::function<void()> on_refresh_;
};
