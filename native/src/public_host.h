#pragma once

#include "desktop_items.h"
#include "display_geom.h"
#include "float_placements.h"
#include "ole_drag.h"
#include "shell_context_menu.h"
#include "shell_context_menu.h"

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
class StringFormat;
}  // namespace Gdiplus

// Loose desktop float host (Fences / Explorer public icons).
class PublicHost {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  // Force re-upload of the layered pixels (Optimus / ULW-loss recovery).
  void RefreshLayer();
  // Suppress UpdateLayeredWindow (OLE unpin stack paints poison Win11 ULW).
  void BeginDeferPresent();
  void EndDeferPresent();
  // Remap desktop seat + reset WS_EX_LAYERED + Present (post-OLE reveal).
  void ForceRemapPresent();
  void SetVisible(bool on);
  HWND Hwnd() const { return hwnd_; }
  bool HasItems() const { return !slots_.empty(); }
  bool HasSelection() const;
  // Fences / Explorer: drop float selection (blank wallpaper / other chrome).
  void ClearSelection();
  // Select by path after paste/New (Explorer keeps focus on landed items).
  void SelectPaths(const std::vector<std::wstring>& paths);
  // Explorer Ctrl+Shift+C → CF_UNICODETEXT of selected paths.
  void CopySelectionPathsAsText();
  void SetItems(std::vector<DesktopItem> items);
  // Drop cached caption fonts so the next paint rebuilds them for the window's
  // current DPI. Call after WM_DPICHANGED / moving between monitors.
  void InvalidateDpiCaches();
  bool FillMissingIcons();
  void SetPlacements(FloatPlacementMap placements);
  void SetBlockedRects(std::vector<RECT> rects);
  void Reflow();
  void AttachToDesktopLayer();
  // Write current Layout seats back into placements_ (+ on_placements_changed).
  void PersistSlotPlacements();
  // Explorer/Fences: snap loose floats to the icon grid (nearest free cells).
  void AlignIconsToGrid();
  // Loose-float icon scale, percent of the desktop's own grid metrics.
  // 70–160 mirrors the per-fence zoom range so both surfaces feel the same.
  void SetIconZoom(int percent);
  int IconZoom() const { return icon_zoom_; }
  // Fired after a user-driven zoom so App can persist it in prefs.
  void SetOnIconZoomChanged(std::function<void(int)> fn) {
    on_icon_zoom_changed_ = std::move(fn);
  }
  void SetOnPathReplaced(
      std::function<void(const std::wstring&, const std::wstring&)> fn) {
    on_path_replaced_ = std::move(fn);
  }
  void SetOnRemoved(std::function<void(const std::wstring&)> fn) {
    on_removed_ = std::move(fn);
  }
  void SetOnRefresh(std::function<void()> fn) { on_refresh_ = std::move(fn); }
  void SetOnOpenSettings(std::function<void()> fn) { on_open_settings_ = std::move(fn); }
  void SetOnNewFence(std::function<void(POINT)> fn) { on_new_fence_ = std::move(fn); }
  void SetOnOrganize(std::function<void()> fn) { on_organize_ = std::move(fn); }
  void SetOnNotes(std::function<void()> fn) { on_notes_ = std::move(fn); }
  // Fences / Explorer: clicking floats clears fence icon selection.
  void SetOnActivated(std::function<void()> fn) {
    on_activated_ = std::move(fn);
  }
  // Fences: Ctrl+V / blank-area Paste lands files on the real Desktop folder.
  // Arg true = Explorer 「粘贴快捷方式」(pastelink).
  void SetOnPaste(std::function<void(bool)> fn) { on_paste_ = std::move(fn); }
  // Explorer drop onto float chrome → Desktop folder. Returns OLE effect.
  void SetOnPathsDropped(
      std::function<DWORD(const std::vector<std::wstring>&)> fn) {
    on_paths_dropped_ = std::move(fn);
  }
  void SetOnPlacementsChanged(
      std::function<void(const FloatPlacementMap&)> fn) {
    on_placements_changed_ = std::move(fn);
  }
  // Cascading「移动到分页 → 分区」for multi-select / marquee.
  void SetOnMoveToFence(
      std::function<void(const std::vector<std::wstring>&,
                         const std::string&)> fn) {
    on_move_to_fence_ = std::move(fn);
  }
  void SetQueryFences(
      std::function<std::vector<MoveFenceMenuTarget>()> fn) {
    on_query_fences_ = std::move(fn);
  }
  void SetPageNames(std::vector<std::wstring> names) {
    page_names_ = std::move(names);
  }
  void SetTitle(const std::wstring& title) { title_ = title; }
  void SetCurrentPage(int page) { current_page_ = page; }

  // Screen-space slot rects for “draw fence around icons” scoop.
  std::vector<std::pair<std::wstring, RECT>> SlotScreenRects() const;
  // Organized mode: blank DefView click → start float marquee (Explorer/Fences).
  // The global mouse hook drives Update/End so the overlay never takes capture
  // (capturing from a hook would swallow every later click system-wide).
  void BeginRubberFromScreen(POINT screen, bool additive);
  void UpdateRubberFromScreen(POINT screen);
  void EndRubberFromScreen(POINT screen);
  // Fences / Explorer: inline label EDIT (F2 / context Rename / slow name click).
  void BeginInlineRename();
  void FinishInlineRename(bool commit);
  // Explorer: Tab / Shift+Tab after rename → next / previous icon.
  void CommitRenameAndAdvance(int delta);
  // Explorer / Fences: Ctrl+Shift+N → new folder on Desktop (float).
  void CreateNewFolder();
  // Explorer: Apps / Shift+F10 → item context menu for the selection.
  void ShowSelectionContextMenu();
  // Exactly one selected filesystem directory (Explorer paste-into-folder).
  std::wstring SelectedSingleDirectory() const;
  // Explorer Ctrl+Z → shell undo + re-pin (App owns stack).
  void SetOnUndo(std::function<void()> fn) { on_undo_ = std::move(fn); }

 private:
  struct Slot {
    DesktopItem item;
    HICON icon = nullptr;
    RECT bounds{};
    RECT icon_rc{};
  };
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc, bool draw_selection, bool draw_rubber);
  void Present();
  void PresentRubberFast();
  void Layout();
  void ApplyVisibility();
  int HitTest(int x, int y) const;
  void ReorderTo(int from, int to);
  void BeginOleDrag();
  void ShowSlotMenu(int index, POINT screen);
  void ShowMoveToPageFenceMenu(POINT screen);
  void ShowBackgroundMenu(POINT screen);
  void PasteClipboardFiles(bool as_link = false);
  void EnsureDropTarget();
  // Fences-class: icon host geometry stays fixed; marquee is a separate band.
  void PresentMarqueeBand();
  void HideMarqueeBand();
  void ReleaseMarqueeBuffer();
  bool EnsureMarqueeBuffer(int w, int h);
  void ClearRubberBaseCache();
  bool EnsureRubberBaseCache(int w, int h);
  void ReleasePresentBuffer();
  bool EnsurePresentBuffer(int w, int h);
  bool SelectionFingerprint(std::vector<char>* out) const;

  void EnsureSelSize();
  void SelectOnly(int index);
  void ToggleSelected(int index);
  void SelectRange(int from, int to);
  void SelectIntersecting(RECT client_rc);
  bool IsSelected(int index) const;
  std::vector<std::wstring> SelectedPaths() const;
  void CopySelection();
  void CutSelection();
  void DeleteSelection(bool permanent = false);
  void RenameSelection();
  void OpenSelection();
  bool IconLabelRect(size_t index, RECT* out) const;
  void NavigateSelection(int dx, int dy, bool extend, bool focus_only = false);
  void HandlePublicHotkey(int vk, bool ctrl, bool shift);
  void TypeAheadFind(wchar_t ch);
  bool TypeAheadBackspace();
  void TypeAheadFindNext();
  void SelectBetween(int from, int to);
  int FocusedSlotIndex() const;

  HWND hwnd_ = nullptr;
  HWND marquee_hwnd_ = nullptr;
  PathDropTarget* drop_target_ = nullptr;
  bool want_visible_ = false;
  // Win11: UpdateLayeredWindow while hidden → blank after Show until click.
  // Mirror FenceWindow — only ULW when mapped; mark dirty otherwise.
  bool layer_dirty_ = true;
  bool defer_present_ = false;
  IconGridMetrics metrics_{};
  std::wstring title_ = L"桌面图标";
  int drag_from_ = -1;
  bool drag_moved_ = false;
  POINT drag_press_{};
  // Explorer right-drag → OLE Copy/Move/Create-shortcut menu.
  bool rbutton_press_ = false;
  bool rbutton_dragged_ = false;
  int rbutton_icon_ = -1;
  POINT rbutton_press_pt_{};
  bool rename_finishing_ = false;
  bool rubber_ = false;
  bool rubber_additive_ = false;
  POINT rubber_origin_{};
  RECT rubber_rc_{};
  HBITMAP rubber_base_dib_ = nullptr;
  void* rubber_base_bits_ = nullptr;
  int rubber_base_w_ = 0;
  int rubber_base_h_ = 0;
  // Steady Present() back-buffer (marquee/rubber keep their own caches).
  HDC present_dc_ = nullptr;
  HBITMAP present_dib_ = nullptr;
  HGDIOBJ present_old_ = nullptr;
  void* present_bits_ = nullptr;
  int present_w_ = 0;
  int present_h_ = 0;
  HDC marquee_dc_ = nullptr;
  HBITMAP marquee_dib_ = nullptr;
  HGDIOBJ marquee_old_ = nullptr;
  void* marquee_bits_ = nullptr;
  int marquee_dib_w_ = 0;
  int marquee_dib_h_ = 0;
  ULONGLONG rubber_present_ms_ = 0;
  std::vector<char> rubber_sel_finger_;
  // #1 premultiplied icon bitmap cache (DrawIconAlpha was per-frame GDI alloc).
  // O(1) hash lookup keyed by (HICON, dest) instead of a per-frame linear scan.
  struct IconCacheKey {
    HICON icon = nullptr;
    int dest = 0;
    bool operator==(const IconCacheKey& o) const {
      return icon == o.icon && dest == o.dest;
    }
  };
  struct IconCacheKeyHash {
    std::size_t operator()(const IconCacheKey& k) const {
      std::size_t h = std::hash<HICON>{}(k.icon);
      h ^= std::hash<int>{}(k.dest) + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };
  std::unordered_map<IconCacheKey, Gdiplus::Bitmap*, IconCacheKeyHash>
      icon_cache_;
  void ClearIconCache();
  void DrawIconCached(Gdiplus::Graphics& g, HICON icon, int x, int y, int dest,
                      BYTE opacity = 255);
  void EnsureCaptionFonts(int dpi);
  void ReleaseCaptionFonts();
  Gdiplus::FontFamily* caption_family_ = nullptr;
  Gdiplus::Font* caption_font_ = nullptr;
  Gdiplus::StringFormat* caption_fmt_ = nullptr;
  int caption_font_dpi_ = 0;
  std::vector<char> selected_;
  // Explorer: anchor (mark) + focus (active end) for Shift+arrow extend.
  int selection_anchor_ = -1;
  int selection_focus_ = -1;
  HWND rename_edit_ = nullptr;
  int rename_index_ = -1;
  WNDPROC rename_proc_ = nullptr;
  int last_icon_click_ = -1;
  DWORD last_icon_click_tick_ = 0;
  std::wstring typeahead_;
  DWORD typeahead_tick_ = 0;
  HWND tip_ = nullptr;
  int tip_index_ = -1;
  // TME_LEAVE is one-shot; re-armed on the next mouse move.
  bool tracking_mouse_ = false;
  std::wstring tip_text_;
  void EnsureTooltip();
  void UpdateHoverTip(int index);
  bool CaptionFitsLabel(const std::wstring& text, const RECT& label) const;
  int current_page_ = 0;
  // Loose-float icon scale (percent of the desktop grid metrics). Loose floats
  // are free-placed, so there is no sort order to apply — only zoom.
  int icon_zoom_ = 100;
  FloatPlacementMap placements_;
  std::vector<Slot> slots_;
  std::vector<RECT> blocked_;
  std::vector<std::wstring> page_names_;
  std::function<void(const std::wstring&, const std::wstring&)> on_path_replaced_;
  std::function<void(const std::wstring&)> on_removed_;
  std::function<void()> on_refresh_;
  std::function<void()> on_open_settings_;
  std::function<void(POINT)> on_new_fence_;
  std::function<void()> on_organize_;
  std::function<void()> on_notes_;
  std::function<void()> on_undo_;
  std::function<void()> on_activated_;
  std::function<void(bool)> on_paste_;
  std::function<DWORD(const std::vector<std::wstring>&)> on_paths_dropped_;
  std::function<void(int)> on_icon_zoom_changed_;
  std::function<void(const FloatPlacementMap&)> on_placements_changed_;
  std::function<void(const std::vector<std::wstring>&, const std::string&)>
      on_move_to_fence_;
  std::function<std::vector<MoveFenceMenuTarget>()> on_query_fences_;
};
