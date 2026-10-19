#pragma once

#include "desktop_items.h"
#include "layout.h"
#include "ole_drag.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

class FenceWindow {
 public:
  using MovedFn = std::function<void(FenceWindow*)>;
  using PathsFn =
      std::function<void(FenceWindow*, const std::vector<std::wstring>&)>;
  using DragDoneFn = std::function<void(FenceWindow*)>;
  using UnpinFn = std::function<void(FenceWindow*, const std::wstring& path)>;
  using PathReplaceFn =
      std::function<void(const std::wstring& from, const std::wstring& to)>;
  using PageFn = std::function<void(FenceWindow*, int page)>;
  using RenameFn = std::function<void(FenceWindow*)>;
  using StyleFn = std::function<void(FenceWindow*)>;

  bool Create(HINSTANCE instance, const FenceLayout& layout);
  void Destroy();
  HWND Hwnd() const { return hwnd_; }
  const std::string& Id() const { return id_; }
  const std::wstring& Title() const { return title_; }
  int Page() const { return page_; }
  void SetPage(int page) { page_ = page; }
  void SetTitle(const std::wstring& title);
  void SetStyleId(const std::string& style_id);
  const std::string& StyleId() const { return style_id_; }
  FenceLayout CurrentLayout() const;
  void Show();
  void Hide();
  void AttachToDesktopLayer();
  void SetOnMoved(MovedFn fn) { on_moved_ = std::move(fn); }
  void SetOnPathsDropped(PathsFn fn) { on_paths_dropped_ = std::move(fn); }
  void SetOnDragDone(DragDoneFn fn) { on_drag_done_ = std::move(fn); }
  void SetOnUnpin(UnpinFn fn) { on_unpin_ = std::move(fn); }
  void SetOnPathReplaced(PathReplaceFn fn) {
    on_path_replaced_ = std::move(fn);
  }
  void SetOnSetPage(PageFn fn) { on_set_page_ = std::move(fn); }
  void SetOnRename(RenameFn fn) { on_rename_ = std::move(fn); }
  void SetOnStyleChanged(StyleFn fn) { on_style_changed_ = std::move(fn); }

  void SetItems(std::vector<DesktopItem> items);
  void ClearIcons();
  const std::vector<DesktopItem> ItemSnapshot() const;
  void PasteClipboardFiles();

 private:
  struct Cell {
    DesktopItem item;
    HICON icon = nullptr;
    RECT bounds{};
  };

  enum class Edge : int {
    None = 0,
    Left = 1,
    Right = 2,
    Top = 4,
    Bottom = 8,
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void LayoutCells();
  int HitTestIcon(int x, int y) const;
  Edge HitTestEdge(int x, int y) const;
  void OpenItem(int index);
  void RevealItem(int index);
  void BeginIconDrag(int index);
  void EnsureDropTarget();
  void ShowIconMenu(int index, POINT screen);
  void RenameItem(int index);
  void RecycleItem(int index);
  void ShowItemProperties(int index);
  void ShowFenceMenu(POINT screen);
  void SelectOnly(int index);
  void ToggleSelected(int index);
  void SelectRange(int index);
  void ClearSelection();
  bool IsSelected(int index) const;
  std::vector<std::wstring> SelectedPaths() const;
  void ApplySelectionCommand(int cmd, int clicked);
  void ApplyStyle();
  void ClampScroll();

  HWND hwnd_ = nullptr;
  bool dragging_ = false;
  bool drag_moved_ = false;
  bool resizing_ = false;
  Edge resize_edge_ = Edge::None;
  bool icon_press_ = false;
  int icon_press_index_ = -1;
  POINT drag_offset_{};
  POINT press_client_{};
  RECT resize_start_rc_{};
  POINT resize_start_pt_{};
  int scroll_y_ = 0;
  int content_h_ = 0;
  std::string id_;
  std::wstring title_;
  std::string style_id_ = "charcoal";
  COLORREF bg_ = RGB(42, 46, 53);
  COLORREF accent_ = RGB(76, 141, 255);
  COLORREF title_color_ = RGB(220, 224, 230);
  int page_ = 0;
  MovedFn on_moved_;
  PathsFn on_paths_dropped_;
  DragDoneFn on_drag_done_;
  UnpinFn on_unpin_;
  PathReplaceFn on_path_replaced_;
  PageFn on_set_page_;
  RenameFn on_rename_;
  StyleFn on_style_changed_;
  std::vector<Cell> cells_;
  std::vector<char> selected_;
  int selection_anchor_ = -1;
  PathDropTarget* drop_target_ = nullptr;
};
