#pragma once

#include "desktop_items.h"
#include "layout.h"
#include "ole_drag.h"

#include <Windows.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class FenceWindow {
 public:
  using MovedFn = std::function<void(FenceWindow*)>;
  using PathsFn = std::function<void(FenceWindow*, const std::vector<std::wstring>&)>;
  using DragDoneFn = std::function<void(FenceWindow*)>;

  bool Create(HINSTANCE instance, const FenceLayout& layout);
  void Destroy();
  HWND Hwnd() const { return hwnd_; }
  const std::string& Id() const { return id_; }
  const std::wstring& Title() const { return title_; }
  int Page() const { return page_; }
  void SetPage(int page) { page_ = page; }
  void SetTitle(const std::wstring& title);
  FenceLayout CurrentLayout() const;
  void Show();
  void Hide();
  void AttachToDesktopLayer();
  void SetOnMoved(MovedFn fn) { on_moved_ = std::move(fn); }
  void SetOnPathsDropped(PathsFn fn) { on_paths_dropped_ = std::move(fn); }
  void SetOnDragDone(DragDoneFn fn) { on_drag_done_ = std::move(fn); }

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
  void BeginIconDrag(int index);
  void EnsureDropTarget();

  HWND hwnd_ = nullptr;
  bool dragging_ = false;
  bool drag_moved_ = false;
  bool icon_press_ = false;
  int icon_press_index_ = -1;
  POINT drag_offset_{};
  POINT press_client_{};
  std::string id_;
  std::wstring title_;
  int page_ = 0;
  MovedFn on_moved_;
  PathsFn on_paths_dropped_;
  DragDoneFn on_drag_done_;
  std::vector<Cell> cells_;
  PathDropTarget* drop_target_ = nullptr;
};
