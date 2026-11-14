#pragma once

#include "desktop_items.h"
#include "display_geom.h"
#include "layout.h"
#include "ole_drag.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Gdiplus {
class Bitmap;
class Font;
class FontFamily;
class Graphics;
}  // namespace Gdiplus

// Target fence for cascading「移动到分页 → 分区」(id + title + page membership).
struct FenceMoveTarget {
  std::string id;
  std::wstring title;
  std::vector<int> pages;
};

class FenceWindow {
 public:
  using MovedFn = std::function<void(FenceWindow*)>;
  using PathsFn =
      std::function<DWORD(FenceWindow*, const std::vector<std::wstring>&)>;
  using DragDoneFn = std::function<void(FenceWindow*)>;
  // `drop` = screen seat for drag-unpin; nullptr = menu unpin (auto-pack).
  using UnpinFn = std::function<void(FenceWindow*, const std::wstring& path,
                                     const POINT* drop)>;
  using UnpinManyFn = std::function<void(
      FenceWindow*, const std::vector<std::wstring>& paths, const POINT* drop)>;
  using PathReplaceFn =
      std::function<void(const std::wstring& from, const std::wstring& to)>;
  using PageFn = std::function<void(FenceWindow*, int page)>;
  using RenameFn = std::function<void(FenceWindow*)>;
  using StyleFn = std::function<void(FenceWindow*)>;
  using PortalFn =
      std::function<void(FenceWindow*, const std::wstring& path)>;
  // Paste with clipboard effect: move = true for cut (Preferred DropEffect MOVE).
  // link = true for Explorer 「粘贴快捷方式」(pastelink) → land .lnk then pin.
  using PasteFn = std::function<void(FenceWindow*,
                                     const std::vector<std::wstring>&, bool,
                                     bool)>;
  // Shell New landed on Desktop — pin as-is (do not FO_COPY).
  using CreatedFn =
      std::function<void(FenceWindow*, const std::vector<std::wstring>&)>;
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
  // Returns the OLE effect actually applied (NONE if the drop was a no-op).
  using DropAtFn = std::function<DWORD(FenceWindow*,
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
  void SetPage(int page);
  // P2-11: multi-page membership (at least one page).
  const std::vector<int>& Pages() const { return pages_; }
  void SetPages(std::vector<int> pages);
  bool OnPage(int page) const;
  void TogglePageMembership(int page);
  // Drop page `deleted` and renumber higher indexes (page bar delete).
  void RemapPagesAfterDelete(int deleted);
  void SetTitle(const std::wstring& title);
  // Painted caption: fence title at portal root; folder leaf when drilled in.
  std::wstring DisplayTitle() const;
  void SetStyleId(const std::string& style_id);
  const std::string& StyleId() const { return style_id_; }
  FenceLayout CurrentLayout() const;
  // Returns true if this call re-uploaded pixels (PresentLayered) — i.e. the
  // fence was hidden / iconic / buried-under-DefView / dirty. Callers that only
  // need the current page painted (SetPage) can skip a redundant RefreshLayer.
  bool Show();
  void Hide();
  // Force re-upload of the layered pixels (Optimus / ULW-loss recovery).
  void RefreshLayer();
  // Temporary hide: hides the window without touching visible_/page geometry,
  // so page switches, the global show_fences toggle and the "stock desktop"
  // double-click view cannot permanently clear a fence (P1-2 leaked into them).
  void HideTransient();
  void AttachToDesktopLayer();
  // Soft-park: content changed while HWND hidden — needs Present after Show.
  bool LayerDirty() const { return layer_dirty_; }
  // Paint off-screen then soft-hide so the first page visit is Show-only.
  void WarmLayerOffscreen();
  void SetOnMoved(MovedFn fn) { on_moved_ = std::move(fn); }
  void SetOnPathsDropped(PathsFn fn) { on_paths_dropped_ = std::move(fn); }
  void SetOnDragDone(DragDoneFn fn) { on_drag_done_ = std::move(fn); }
  void SetOnUnpin(UnpinFn fn) { on_unpin_ = std::move(fn); }
  void SetOnUnpinMany(UnpinManyFn fn) { on_unpin_many_ = std::move(fn); }
  void SetOnPathReplaced(PathReplaceFn fn) {
    on_path_replaced_ = std::move(fn);
  }
  void SetOnSetPage(PageFn fn) { on_set_page_ = std::move(fn); }
  void SetOnRename(RenameFn fn) { on_rename_ = std::move(fn); }
  void SetOnStyleChanged(StyleFn fn) { on_style_changed_ = std::move(fn); }
  void SetOnPortal(PortalFn fn) { on_portal_ = std::move(fn); }
  void SetOnPasted(PasteFn fn) { on_pasted_ = std::move(fn); }
  void SetOnCreated(CreatedFn fn) { on_created_ = std::move(fn); }
  void SetQueryFences(QueryFencesFn fn) { on_query_fences_ = std::move(fn); }
  void SetOnMoveToFence(MoveToFenceFn fn) {
    on_move_to_fence_ = std::move(fn);
  }
  void SetOnMoveToPage(MoveToPageFn fn) { on_move_to_page_ = std::move(fn); }
  void SetOnWhitelist(WhitelistFn fn) { on_whitelist_ = std::move(fn); }
  void SetOnOpenNotes(OpenNotesFn fn) { on_open_notes_ = std::move(fn); }
  void SetOnDissolve(DissolveFn fn) { on_dissolve_ = std::move(fn); }
  void SetOnRefresh(RefreshFn fn) { on_refresh_ = std::move(fn); }
  // Fences / Explorer: clicking this fence clears selection elsewhere.
  void SetOnActivated(MovedFn fn) { on_activated_ = std::move(fn); }
  void SetOnReorder(ReorderFn fn) { on_reorder_ = std::move(fn); }
  void SetOnPathsDroppedAt(DropAtFn fn) { on_paths_dropped_at_ = std::move(fn); }
  void SetOnZoomChanged(ZoomFn fn) { on_zoom_changed_ = std::move(fn); }
  // P1-13: pin-transfer drop. `at` = visual insert index (-1 = append).
  // Returns the DROPEFFECT to advertise (NONE = reject).
  using PinDropFn = std::function<DWORD(
      FenceWindow*, const std::wstring& src_fence_id,
      const std::vector<std::wstring>& paths, bool commit, int at)>;
  void SetOnPinDrop(PinDropFn fn) { on_pin_drop_ = std::move(fn); }
  // P1-12: fired once when a pin-transfer drag ends (Drop/Escape) — hides the
  // floating drop hint owned by App.
  void SetOnPinDragEnd(DragDoneFn fn) { on_pin_drag_end_ = std::move(fn); }
  // Legacy whole-fence flash (prefer OLE insert/recycle preview).
  void SetDropHighlight(bool on);
  void UpdateOleDropPreview(int x, int y);
  void ClearOleDropPreview();
  void SetOnGeometryMoved(GeometryMovedFn fn) {
    on_geometry_moved_ = std::move(fn);
  }
  // Per-fence icon zoom percent (70..160). Persisted by app via layout field.
  void SetIconZoom(int percent);
  int IconZoom() const { return icon_zoom_percent_; }
  int IconSizePx() const;
  // Caption lines shown for an unselected tile (1..3) and caption visibility.
  void SetLabelStyle(int lines, bool show);
  int LabelLines() const { return label_lines_; }
  bool ShowLabels() const { return show_labels_; }
  // Per-fence grid pitch as a percent of the desktop's own cell metrics.
  void SetCellScale(int percent);
  int CellScale() const { return cell_scale_; }
  // Title bar visibility (Fences lets a fence run title-less).
  void SetShowTitle(bool show);
  bool ShowTitle() const { return show_title_; }
  // Hover-away fade: dims the whole panel via ULW SourceConstantAlpha so the
  // layer bitmap is reused instead of repainted every frame.
  void SetFadeOnIdle(bool on);
  // Fences-style quick filter: narrows the visible tiles to names matching the
  // typed substring. Kept out of the layout so a filter never rewrites pins.
  void BeginFilter();
  void ApplyFilter(const std::wstring& text);
  void EndFilter();
  // Dismiss the filter box but keep the narrowed result (Enter).
  void CloseFilterEdit();
  bool FilterActive() const { return !filter_text_.empty(); }

 private:
  bool NameMatchesFilter(const std::wstring& name) const;

 public:
  // H1: manual pin order persistence.
  void SetPinOrder(std::vector<std::wstring> order);
  const std::vector<std::wstring>& PinOrder() const { return pin_order_; }
  // Insert dropped paths at `at` (dedup existing occurrences); `at<0` appends.
  void InsertPinOrder(const std::vector<std::wstring>& paths, int at);
  // Remove every occurrence of `paths` from the manual pin order.
  void RemovePinOrder(const std::vector<std::wstring>& paths);
  // Rename / retarget: keep manual seat when the path changes.
  void RetargetPinOrder(const std::wstring& from, const std::wstring& to);
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
  // After STA icon preload: fill nullptr cells from the master cache + Present.
  bool FillMissingIcons();
  void ClearIcons();
  const std::vector<DesktopItem> ItemSnapshot() const;
  void SetSortBy(const std::string& sort_by);
  const std::string& SortBy() const { return sort_by_; }
  void SetViewMode(const std::string& mode);
  const std::string& ViewMode() const { return view_mode_; }
  // -1 = inherit style preset; otherwise 0–255 panel alpha.
  void SetOpacity(int opacity);
  // Override the preset's background/accent with explicit colours. Passing
  // enabled=false falls back to the current style preset.
  void SetCustomColors(bool enabled, COLORREF bg, COLORREF accent);
  bool HasCustomColors() const { return has_custom_colors_; }
  COLORREF CustomBg() const { return bg_; }
  COLORREF CustomAccent() const { return accent_; }
  int Opacity() const { return opacity_override_; }
  // as_link: shell verb pastelink → create Desktop/portal .lnk (not copy/move).
  void PasteClipboardFiles(bool as_link = false);
  void SetPortalPath(const std::wstring& path);
  const std::wstring& PortalPath() const { return portal_path_; }
  // Fences Folder Portal: current browse dir (root or drilled-in child).
  const std::wstring& PortalBrowsePath() const;
  bool PortalCanGoUp() const;
  bool PortalCanGoForward() const;
  bool NavigatePortalInto(const std::wstring& folder);
  bool PortalGoUp();
  bool PortalGoForward();
  bool IsCollapsed() const { return collapsed_; }
  void SetCollapsed(bool collapsed, bool persist = true);
  void ToggleCollapsed();
  bool AutoHide() const { return auto_hide_; }
  void SetAutoHide(bool on);
  bool IsPositionLocked() const { return position_locked_; }
  void SetPositionLocked(bool locked);
  // Content lock freezes the fence's icon set and order: no drag-reorder,
  // no inbound/outbound drops, no delete/rename/paste. Moving the frame stays
  // governed by position_locked (Fences keeps the two locks separate).
  bool IsContentLocked() const { return content_locked_; }
  void SetContentLocked(bool locked);
  void SetPageNames(std::vector<std::wstring> names) {
    page_names_ = std::move(names);
  }
  void CopySelection();
  void CutSelection();
  void DeleteSelection(bool permanent = false);
  void SelectAll();
  void ClearSelection();
  // Select by path after paste/New (Explorer keeps focus on landed items).
  void SelectPaths(const std::vector<std::wstring>& paths);
  // Explorer Ctrl+Shift+C → CF_UNICODETEXT of selected paths.
  void CopySelectionPathsAsText();
  void BeginInlineRename();
  void FinishInlineRename(bool commit);
  // Explorer: Tab / Shift+Tab after rename → next / previous icon.
  void CommitRenameAndAdvance(int delta);
  // Fences: blank-menu「重命名」→ title-bar inline EDIT (not Mist dialog).
  void BeginTitleInlineRename();
  // Explorer/Fences: type letter prefix to jump to matching icon.
  void TypeAheadFind(wchar_t ch);
  // Backspace pops typeahead; F3 jumps to next prefix match.
  bool TypeAheadBackspace();
  void TypeAheadFindNext();
  // Explorer / Fences: Ctrl+Shift+N → new folder (Desktop pin or portal cwd).
  void CreateNewFolder();
  // Explorer: Apps / Shift+F10 → item context menu for the selection.
  void ShowSelectionContextMenu();
  // Exactly one selected filesystem directory (Explorer paste-into-folder).
  std::wstring SelectedSingleDirectory() const;
  // Explorer Ctrl+Z / blank Undo → shell undo + re-pin (App owns stack).
  using UndoFn = std::function<void()>;
  void SetOnUndo(UndoFn fn) { on_undo_ = std::move(fn); }

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

  // Title-bar chrome: portal-up (left) + collapse/lock/dissolve (right).
  enum class HeaderBtn : int {
    None = 0,
    Collapse = 1,
    Lock = 2,
    Dissolve = 3,
    PortalUp = 4,
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void PresentLayered();
  // Re-blit the existing layer bitmap at a new constant alpha (no repaint).
  void BlitLayerAtAlpha(BYTE alpha);
  // One animation step toward the idle / active alpha target.
  void TickIdleFade();
  void LayoutCells();
  // Fences-class: paint size is owned by the fence, not a transient HWND rect
  // during Explorer attach/create churn.
  bool ResolveLayerSize(int* out_w, int* out_h) const;
  void LayoutCellsTo(int w, int h);
  void ClampScrollForView(int view_h);
  int HitTestIcon(int x, int y) const;
  // Glyph + caption ink (not full grid cell). Used by hit-test and rubber-band
  // so cell padding / empty chrome clear selection like Explorer / Fences.
  bool IconContentRect(size_t index, RECT* out) const;
  // Caption shelf only (Explorer: second click on name starts rename).
  bool IconLabelRect(size_t index, RECT* out) const;
  Edge HitTestEdge(int x, int y) const;
  bool IsSystemFence() const;
  void HeaderBtnRects(RECT* collapse, RECT* lock, RECT* dissolve,
                      RECT* portal_up = nullptr) const;
  HeaderBtn HitTestHeaderBtn(int x, int y) const;
  void ActivateHeaderBtn(HeaderBtn btn);
  // external=true: open in Explorer (portal Shift+Enter) — skip drill-in.
  void OpenItem(int index, bool external = false);
  void RevealItem(int index);
  void BeginIconDrag(int index);
  void EnsureDropTarget();
  void ShowIconMenu(int index, POINT screen);
  void RenameItem(int index);
  void RecycleItem(int index);
  // Drop onto hosted Recycle Bin pin → FO_DELETE (not pin-into-fence).
  bool IsRecycleBinIconAt(int x, int y) const;
  // Explorer: drop onto a folder icon → import into that directory.
  // Empty when not over a filesystem directory (or over Recycle Bin).
  std::wstring DirectoryIconAt(int x, int y) const;
  // Returns true when paths were sent to the Recycle Bin.
  bool RecycleDroppedPaths(std::vector<std::wstring> paths);
  // Land CF_HDROP into `dest_dir` (MOVE/COPY/LINK from proposed OLE effect).
  DWORD ImportPathsIntoDirectory(const std::vector<std::wstring>& paths,
                                 const std::wstring& dest_dir,
                                 DWORD proposed);
  void ShowItemProperties(int index);
  void ShowFenceMenu(POINT screen);
  // Cascading「移动到分页」→ page → fence.
  void ShowMoveToPageFenceMenu(POINT screen);
  void SelectOnly(int index);
  void ToggleSelected(int index);
  void SelectRange(int index);
  bool IsSelected(int index) const;
  std::vector<std::wstring> SelectedPaths() const;
  void ApplySelectionCommand(int cmd, int clicked);
  void ApplyRubberBandSelection();
  // focus_only: Ctrl+Arrow — move keyboard focus without changing selection.
  void NavigateSelection(int dx, int dy, bool extend, bool focus_only = false);
  void ResetPortalBrowseChrome();
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
  // Scroll one step when an in-progress icon drag sits against the top/bottom
  // edge. Returns true when the viewport actually moved (needs a re-present).
  bool ScrollIfAtDragEdge(int x, int y);
  // P1-13: OLE pin-transfer drag out; unpin only on desktop release / real MOVE.
  void CommitDragOutPins(std::vector<std::wstring> paths);
  int InsertIndexAt(int x, int y) const;
  void CommitReorder(int insert_index);
  // F4: icon zoom + geometry helpers.
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
  std::wstring tip_text_;
  bool TitleLabelRect(RECT* out) const;
  bool CaptionFitsLabel(const std::wstring& text, const RECT& label) const;
  bool dragging_ = false;
  bool drag_moved_ = false;
  bool resizing_ = false;
  Edge resize_edge_ = Edge::None;
  bool icon_press_ = false;
  int icon_press_index_ = -1;
  // Explorer right-drag → OLE Copy/Move/Create-shortcut menu.
  bool rbutton_press_ = false;
  bool rbutton_dragged_ = false;
  int rbutton_icon_ = -1;
  POINT rbutton_press_pt_{};
  // Manual double-click fallback (in case class lacks CS_DBLCLKS).
  int last_icon_click_ = -1;
  DWORD last_icon_click_tick_ = 0;
  bool rename_finishing_ = false;
  std::wstring typeahead_;
  DWORD typeahead_tick_ = 0;
  POINT drag_offset_{};
  POINT press_client_{};
  RECT resize_start_rc_{};
  POINT resize_start_pt_{};
  int scroll_y_ = 0;
  int content_h_ = 0;
  std::string id_;
  std::wstring title_;
  std::string style_id_ = "navy";
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
  UnpinManyFn on_unpin_many_;
  PathReplaceFn on_path_replaced_;
  PageFn on_set_page_;
  RenameFn on_rename_;
  StyleFn on_style_changed_;
  PortalFn on_portal_;
  PasteFn on_pasted_;
  CreatedFn on_created_;
  QueryFencesFn on_query_fences_;
  MoveToFenceFn on_move_to_fence_;
  MoveToPageFn on_move_to_page_;
  WhitelistFn on_whitelist_;
  OpenNotesFn on_open_notes_;
  DissolveFn on_dissolve_;
  RefreshFn on_refresh_;
  UndoFn on_undo_;
  MovedFn on_activated_;
  ReorderFn on_reorder_;
  DropAtFn on_paths_dropped_at_;
  ZoomFn on_zoom_changed_;
  GeometryMovedFn on_geometry_moved_;
  PinDropFn on_pin_drop_;
  DragDoneFn on_pin_drag_end_;
  std::wstring portal_path_;
  // Empty → browsing portal_path_ root. Non-empty → in-portal cwd.
  std::wstring portal_cwd_;
  // Forward stack after PortalGoUp (Explorer/browser: Alt+→ / XBUTTON2).
  std::vector<std::wstring> portal_forward_;
  bool collapsed_ = false;
  bool auto_hide_ = false;
  bool position_locked_ = false;
  bool content_locked_ = false;
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
  // Quick filter: the popup EDIT, its text, and the unfiltered item list so
  // clearing the filter restores every pin without a disk rescan.
  HWND filter_edit_ = nullptr;
  WNDPROC filter_proc_ = nullptr;
  std::wstring filter_text_;
  std::vector<DesktopItem> all_items_;
  WNDPROC rename_proc_ = nullptr;
  std::vector<Cell> cells_;
  std::vector<char> selected_;
  // Explorer ListView: mark (anchor) stays during Shift-extend; focus moves.
  int selection_anchor_ = -1;
  int selection_focus_ = -1;
  PathDropTarget* drop_target_ = nullptr;
  // F2/F3 manual icon drag state.
  bool icon_dragging_ = false;
  std::vector<std::wstring> drag_paths_;
  std::vector<int> drag_indices_;
  int drag_insert_ = -1;   // live insert indicator position
  bool drag_left_ = false;  // pointer has left the client area
  POINT drag_pt_{};         // client cursor while icon_dragging_ (ghost)
  DWORD last_manual_drag_present_ms_ = 0;
  // OLE inbound drop preview (Explorer / desktop / other fence).
  bool drop_preview_ = false;
  bool drop_over_recycle_ = false;
  bool drop_over_folder_ = false;
  int drop_insert_ = -1;
  POINT drop_pt_{};
  DWORD last_ole_preview_present_ms_ = 0;
  // Legacy whole-fence flash — kept off; insert bar replaces it.
  bool drop_highlight_ = false;
  // F4 icon zoom.
  int icon_zoom_percent_ = 100;
  // Caption lines for idle tiles / caption visibility (Fences label options).
  int label_lines_ = 2;
  bool show_labels_ = true;
  // Grid pitch as a percent of the desktop's cell metrics (100 = follow).
  int cell_scale_ = 100;
  // Idle fade: dim to this ULW constant alpha when the pointer is away.
  bool fade_on_idle_ = false;
  BYTE fade_alpha_ = 255;
  DWORD fade_tick_ = 0;
  // F7 scrollbar drag state.
  bool thumb_dragging_ = false;
  int thumb_start_y_ = 0;
  int thumb_start_scroll_ = 0;
  // F8 style preview.
  std::string preview_style_id_;
  bool style_preview_active_ = false;
  bool style_menu_open_ = false;
  DWORD footer_zoom_hint_until_ = 0;
  // True when pixels must be re-uploaded after the next Show (never ULW while
  // hidden — Win11 often leaves a blank layered surface after mapping).
  bool layer_dirty_ = true;
  // #1/#12 hot-path caches: premultiplied icon bitmaps + GDI+ fonts.
  // O(1) hash lookup keyed by (HICON, dest, opacity).
  struct IconCacheKey {
    HICON icon = nullptr;
    int dest = 0;
    BYTE opacity = 255;
    bool operator==(const IconCacheKey& o) const {
      return icon == o.icon && dest == o.dest && opacity == o.opacity;
    }
  };
  struct IconCacheKeyHash {
    std::size_t operator()(const IconCacheKey& k) const {
      std::size_t h = std::hash<HICON>{}(k.icon);
      h ^= std::hash<int>{}(k.dest) + 0x9e3779b9 + (h << 6) + (h >> 2);
      h ^= std::hash<unsigned>{}(k.opacity) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };
  std::unordered_map<IconCacheKey, Gdiplus::Bitmap*, IconCacheKeyHash>
      icon_cache_;
  void ClearIconCache();
  void DrawIconCached(Gdiplus::Graphics& g, HICON icon, int x, int y, int dest,
                      BYTE opacity);
  Gdiplus::FontFamily* caption_family_ = nullptr;  // owned unless borrowed
  bool caption_family_borrowed_ = false;
  Gdiplus::Font* title_font_ = nullptr;            // owned
  Gdiplus::Font* caption_font_ = nullptr;          // owned
  Gdiplus::Font* footer_font_ = nullptr;           // owned
  int cached_title_px_ = 0;
  int cached_caption_px_ = 0;
  int cached_footer_px_ = 0;
  void EnsureFonts(int title_px, int caption_px, int footer_px);
  void ReleaseFonts();
  // Reused ULW back-buffer — CreateDIBSection every Present was a hot-path tax.
  void ReleaseLayerBuffer();
  bool EnsureLayerBuffer(int w, int h);
  void InvalidatePanelChromeCache();
  HDC layer_dc_ = nullptr;
  HBITMAP layer_dib_ = nullptr;
  HGDIOBJ layer_old_ = nullptr;
  void* layer_bits_ = nullptr;
  int layer_buf_w_ = 0;
  int layer_buf_h_ = 0;
  // Frosted panel chrome (pre-Paint). Hit skips GDI+ gradient path when
  // size/style unchanged — icons/text still redraw via Paint every Present.
  std::vector<BYTE> panel_chrome_;
  int panel_chrome_w_ = 0;
  int panel_chrome_h_ = 0;
  COLORREF panel_chrome_bg_ = 0;
  COLORREF panel_chrome_accent_ = 0;
  BYTE panel_chrome_alpha_ = 0;
  bool panel_chrome_light_ = false;
  int panel_chrome_radius_ = -1;
};
