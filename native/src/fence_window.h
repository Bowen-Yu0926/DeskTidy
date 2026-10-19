#pragma once

#include "desktop_items.h"
#include "layout.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

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

  void SetItems(std::vector<DesktopItem> items);
  void ClearIcons();

 private:
  struct Cell {
    DesktopItem item;
    HICON icon = nullptr;
    RECT bounds{};
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void LayoutCells();
  int HitTestIcon(int x, int y) const;
  void OpenItem(int index);

  HWND hwnd_ = nullptr;
  bool dragging_ = false;
  bool drag_moved_ = false;
  POINT drag_offset_{};
  POINT press_client_{};
  std::string id_;
  std::wstring title_;
  MovedFn on_moved_;
  std::vector<Cell> cells_;
};
