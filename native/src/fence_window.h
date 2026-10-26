#pragma once

#include "desktop_items.h"
#include "display_geom.h"
#include "layout.h"
#include "ole_drag.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Target fence for the "move to fence" submenu (id + display title).
struct FenceMoveTarget {
  std::string id;
  std::wstring title;
};

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
  using PortalFn =
      std::function<void(FenceWindow*, const std::wstring& path)>;
  // Paste with clipboard effect: move = true for cut (Preferred DropEffect MOVE).
  using PasteFn = std::function<void(FenceWindow*,
                                     const std::vector<std::wstring>&, bool)>;
  using QueryFencesFn = std::function<std::vector<FenceMoveTarget>()>;
  using MoveToFenceFn = std::function<void(
      FenceWindow*, const std::vector<std::wstring>&, const std::string&)>;
  using MoveToPageFn =
      std::function<void(FenceWindow*, const std::vector<std::wstring>&, int)>;
  using WhitelistFn =
      std::function<void(FenceWindow*, const std::vector<std::wstring>&, bool)>;
  using OpenNotesFn =
      std::function<void(FenceWindow*, const std::vector<std::wstring>&)>;
  using DissolveFn = std::function<void(FenceWindow*)>;
  using RefreshFn = std::function<void(FenceWindow*)>;
  // Manual in-fence reorder (F2): full ordered path list after a drop.
  using ReorderFn =
      std::function<void(FenceWindow*, const std::vector<std::wstring>&)>;
  // Drop-at with insertion index (F1): index < 0 means append.
  using DropAtFn = std::function<void(FenceWindow*,
                                      const std::vector<std::wstring>&, int)>;
  // Per-fence icon zoom changed (F4): zoom_percent in [70, 160].
  using ZoomFn = std::function<void(FenceWindow*, int zoom_percent)>;
  // Fired when a fence finishes a move (drag-end); app snaps + resolves
  // collisions + clamps + persists per-page geometry (H2/H3).
  using GeometryMovedFn = std::function<void(FenceWindow*)>;

  bool Create(HINSTANCE instance, const FenceLayout& layout);
  void Destroy();
  HWND Hwnd() const { return hwnd_; }
  const std::string& Id() const { return id_; }
  const std::wstring& Title() const { return title_; }
  int Page() const { return page_; }
  void SetPage(int page) { page_ = page; }
  // P2-11: multi-page membership (at least one page).
  const std::vector<int>& Pages() const { return pages_; }
  void SetPages(std::vector<int> pages);
  bool OnPage(int page) const;
  void TogglePageMembership(int page);
  void SetTitle(const std::wstring& title);
  void SetStyleId(const std::string& style_id);
  const std::string& StyleId() const { return style_id_; }
  FenceLayout CurrentLayout() const;
  void Show();
  void Hide();
  // Temporary hide: hides the window without touching visible_/page geometry,
  // so page switches, the global show_fences toggle and the "stock desktop"
  // double-click view cannot permanently clear a fence (P1-2 leaked into them).
  void HideTransient();
  void AttachToDesktopLayer();
  void RefreshLayer();  // re-upload UpdateLayeredWindow pixels
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
  void SetOnPortal(PortalFn fn) { on_portal_ = std::move(fn); }
  void SetOnPasted(PasteFn fn) { on_pasted_ = std::move(fn); }
  void SetQueryFences(QueryFencesFn fn) { on_query_fences_ = std::move(fn); }
  void SetOnMoveToFence(MoveToFenceFn fn) {
    on_move_to_fence_ = std::move(fn);
  }
  void SetOnMoveToPage(MoveToPageFn fn) { on_move_to_page_ = std::move(fn); }
  void SetOnWhitelist(WhitelistFn fn) { on_whitelist_ = std::move(fn); }
  void SetOnOpenNotes(OpenNotesFn fn) { on_open_notes_ = std::move(fn); }
  void SetOnDissolve(DissolveFn fn) { on_dissolve_ = std::move(fn); }
  void SetOnRefresh(RefreshFn fn) { on_refresh_ = std::move(fn); }
  void SetOnReorder(ReorderFn fn) { on_reorder_ = std::move(fn); }
  void SetOnPathsDroppedAt(DropAtFn fn) { on_paths_dropped_at_ = std::move(fn); }
  void SetOnZoomChanged(ZoomFn fn) { on_zoom_changed_ = std::move(fn); }
  // P1-13: pin-transfer drop. Returns the DROPEFFECT to advertise (NONE = reject).
  using PinDropFn = std::function<DWORD(
      FenceWindow*, const std::wstring& src_fence_id,
      const std::vector<std::wstring>& paths, bool commit)>;
  void SetOnPinDrop(PinDropFn fn) { on_pin_drop_ = std::move(fn); }
  // P1-12: fired once when a pin-transfer drag ends (Drop/Escape) — hides the
  // floating drop hint owned by App.
  void SetOnPinDragEnd(DragDoneFn fn) { on_pin_drag_end_ = std::move(fn); }
  // P1-12: drop-target highlight (inner stroke + translucent overlay).
  void SetDropHighlight(bool on);
  void SetOnGeometryMoved(GeometryMovedFn fn) {
    on_geometry_moved_ = std::move(fn);
  }
  // Per-fence icon zoom percent (70..160). Persisted by app via layout field.
  void SetIconZoom(int percent);
  int IconZoom() const { return icon_zoom_percent_; }

  // H1: manual pin order persistence.
  void SetPinOrder(std::vector<std::wstring> order);
  const std::vector<std::wstring>& PinOrder() const { return pin_order_; }
  // Insert dropped paths at `at` (dedup existing occurrences); `at<0` appends.
  void InsertPinOrder(const std::vector<std::wstring>& paths, int at);
  // Remove every occurrence of `paths` from the manual pin order.
  void RemovePinOrder(const std::vector<std::wstring>& paths);
  // H3: per-page geometry / visibility.
  void SetPageGeometry(int page, const FencePageGeometry& geo);
  const std::map<int, FencePageGeometry>& PageGeometries() const {
    return page_geometries_;
  }
  FencePageGeometry GeometryForPage(int page) const;
  // present=false: move/resize only (page-switch batches Present after show).
  void ApplyPageGeometry(const FencePageGeometry& geo, bool present = true);
  bool Visible() const { return visible_; }
  void SetVisible(bool visible) { visible_ = visible; }
  bool VisibleOnPage(int page) const;
  void SetOrganizeKinds(std::vector<std::string> kinds);
  const std::vector<std::string>& OrganizeKinds() const {
    return organize_kinds_;
  }
  // H6: replace fixed grid constants with queried metrics.
  void SetIconGrid(const IconGridMetrics& m);
  // H5: rescale window rect + per-page geometry for a DPI change.
  void ScaleGeometryForDpi(int old_dpi, int new_dpi);

  void SetItems(std::vector<DesktopItem> items);
  void ClearIcons();
  const std::vector<DesktopItem> ItemSnapshot() const;
  void SetSortBy(const std::string& sort_by);
  const std::string& SortBy() const { return sort_by_; }
  void SetViewMode(const std::string& mode);
  const std::string& ViewMode() const { return view_mode_; }
  // -1 = inherit style preset; otherwise 0–255 panel alpha.
  void SetOpacity(int opacity);
  int Opacity() const { return opacity_override_; }
  void PasteClipboardFiles();
  void SetPortalPath(const std::wstring& path);
  const std::wstring& PortalPath() const { return portal_path_; }
  bool IsCollapsed() const { return collapsed_; }
  void SetCollapsed(bool collapsed);
  void ToggleCollapsed();
  bool IsPositionLocked() const { return position_locked_; }
  void SetPositionLocked(bool locked);
  void SetPageNames(std::vector<std::wstring> names) {
    page_names_ = std::move(names);
  }
  void CopySelection();
  void CutSelection();
  void DeleteSelection();
  void SelectAll();
  void BeginInlineRename();
  void FinishInlineRename(bool commit);

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

  // Title-bar chrome buttons (Python: collapse + lock; dissolve for delete).
  enum class HeaderBtn : int {
    None = 0,
    Collapse = 1,
    Lock = 2,
    Dissolve = 3,
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void PresentLayered();
  void LayoutCells();
  int HitTestIcon(int x, int y) const;
  Edge HitTestEdge(int x, int y) const;
  bool IsSystemFence() const;
  void HeaderBtnRects(RECT* collapse, RECT* lock, RECT* dissolve) const;
  HeaderBtn HitTestHeaderBtn(int x, int y) const;
  void ActivateHeaderBtn(HeaderBtn btn);
  void OpenItem(int index);
  void RevealItem(int index);
  void BeginIconDrag(int index);
  void EnsureDropTarget();
  void ShowIconMenu(int index, POINT screen);
  void RenameItem(int index);
  void RecycleItem(int index);
  void ShowItemProperties(int index);
  void ShowFenceMenu(POINT screen);
  void ShowMoveToFenceMenu(POINT screen);
  void ShowMoveToPageMenu(POINT screen);
  void SelectOnly(int index);
  void ToggleSelected(int index);
  void SelectRange(int index);
  void ClearSelection();
  bool IsSelected(int index) const;
  std::vector<std::wstring> SelectedPaths() const;
  void ApplySelectionCommand(int cmd, int clicked);
  void ApplyRubberBandSelection();
  void NavigateSelection(int dx, int dy, bool extend);
  void EnsureCellVisible(int index);
  void OpenSelectedItems();
  int GridColumnCount() const;
  void ApplyStyle();
  void ClampScroll();
  void SortCells();
  void EnsureTooltip();
  void UpdateHoverTip(int index);
  void HandleFenceHotkey(int vk, bool ctrl, bool shift);
  // F2/F3: manual icon drag (reorder in fence / unpin when dropped outside).
  void StartManualIconDrag(int index);
  void UpdateManualIconDrag(int x, int y);
  void FinishManualIconDrag(int x, int y);
  // P1-13: OLE pin-transfer drag out of the fence; unpins unless transferred.
  void CommitDragOutPins(std::vector<std::wstring> paths);
  int InsertIndexAt(int x, int y) const;
  void CommitReorder(int insert_index);
  // F4: icon zoom + geometry helpers.
  int IconSizePx() const;
  int CellWidthPx() const;
  int CellHeightPx() const;
  void RebuildIconsForZoom();
  void ApplyZoomWheel(int delta);
  // F7: scrollbar interaction.
  bool ScrollbarVisible() const;
  int HitTestScrollbar(int x, int y) const;  // 0 none, 1 thumb, 2 track
  void ScrollbarPage(int dir);               // dir: -1 up, +1 down
  // F8: style preview + revert.
  void PreviewStyleId(const std::string& style_id);
  void RevertStylePreview();

  HWND hwnd_ = nullptr;
  HWND tip_ = nullptr;
  int tip_index_ = -1;
  bool dragging_ = false;
  bool drag_moved_ = false;
  bool resizing_ = false;
  Edge resize_edge_ = Edge::None;
  bool icon_press_ = false;
  int icon_press_index_ = -1;
  // Manual double-click fallback (in case class lacks CS_DBLCLKS).
  int last_icon_click_ = -1;
  DWORD last_icon_click_tick_ = 0;
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
  COLORREF title_color_ = RGB(255, 255, 255);
  COLORREF caption_color_ = RGB(255, 255, 255);
  BYTE panel_alpha_ = 178;
  int radius_ = 8;
  int radius_override_ = -1;
  bool light_panel_ = false;
  bool show_title_ = true;
  bool collapsible_ = true;
  bool has_custom_colors_ = false;
  int page_ = 0;
  std::vector<int> pages_;
  MovedFn on_moved_;
  PathsFn on_paths_dropped_;
  DragDoneFn on_drag_done_;
  UnpinFn on_unpin_;
  PathReplaceFn on_path_replaced_;
  PageFn on_set_page_;
  RenameFn on_rename_;
  StyleFn on_style_changed_;
  PortalFn on_portal_;
  PasteFn on_pasted_;
  QueryFencesFn on_query_fences_;
  MoveToFenceFn on_move_to_fence_;
  MoveToPageFn on_move_to_page_;
  WhitelistFn on_whitelist_;
  OpenNotesFn on_open_notes_;
  DissolveFn on_dissolve_;
  RefreshFn on_refresh_;
  ReorderFn on_reorder_;
  DropAtFn on_paths_dropped_at_;
  ZoomFn on_zoom_changed_;
  GeometryMovedFn on_geometry_moved_;
  PinDropFn on_pin_drop_;
  DragDoneFn on_pin_drag_end_;
  std::wstring portal_path_;
  bool collapsed_ = false;
  bool position_locked_ = false;
  HeaderBtn header_press_ = HeaderBtn::None;
  int expanded_h_ = 360;
  std::string sort_by_ = "name";
  std::string view_mode_ = "grid";
  int opacity_override_ = -1;
  // H1: manual pin order (paths) mirrored to layout.pin_order.
  std::vector<std::wstring> pin_order_;
  // H3: top-level visibility + per-page geometry overrides.
  bool visible_ = true;
  std::vector<std::string> organize_kinds_;
  std::map<int, FencePageGeometry> page_geometries_;
  // H6: resolved icon grid metrics (replaces fixed 96/102/40).
  IconGridMetrics grid_metrics_{};
  std::vector<std::wstring> page_names_;
  bool rubber_ = false;
  bool rubber_additive_ = false;
  POINT rubber_a_{};
  POINT rubber_b_{};
  HWND rename_edit_ = nullptr;
  int rename_index_ = -1;
  WNDPROC rename_proc_ = nullptr;
  std::vector<Cell> cells_;
  std::vector<char> selected_;
  int selection_anchor_ = -1;
  PathDropTarget* drop_target_ = nullptr;
  // F2/F3 manual icon drag state.
  bool icon_dragging_ = false;
  std::vector<std::wstring> drag_paths_;
  std::vector<int> drag_indices_;
  int drag_insert_ = -1;   // live insert indicator position
  bool drag_left_ = false;  // pointer has left the client area
  // P1-12: fence-level drop-target highlight (DragEnter on / DragLeave+Drop off).
  bool drop_highlight_ = false;
  // F4 icon zoom.
  int icon_zoom_percent_ = 100;
  // F7 scrollbar drag state.
  bool thumb_dragging_ = false;
  int thumb_start_y_ = 0;
  int thumb_start_scroll_ = 0;
  // F8 style preview.
  std::string preview_style_id_;
  bool style_preview_active_ = false;
  bool style_menu_open_ = false;
  DWORD footer_zoom_hint_until_ = 0;
};
