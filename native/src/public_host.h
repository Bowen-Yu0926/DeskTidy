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
  // Re-blit layered content (Win+D restore); no-op when hidden.
  void RefreshLayer();
  void AttachToDesktopLayer();
  void SetOnPathReplaced(
      std::function<void(const std::wstring&, const std::wstring&)> fn) {
    on_path_replaced_ = std::move(fn);
  }
  void SetOnRemoved(std::function<void(const std::wstring&)> fn) {
    on_removed_ = std::move(fn);
  }
  void SetOnRefresh(std::function<void()> fn) { on_refresh_ = std::move(fn); }
  // Fences: Ctrl+V / blank-area Paste lands files on the real Desktop folder.
  void SetOnPaste(std::function<void()> fn) { on_paste_ = std::move(fn); }
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
  void BeginRubberFromScreen(POINT screen, bool additive);

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
  void PersistSlotPlacements();
  void BeginOleDrag();
  void ShowSlotMenu(int index, POINT screen);
  void ShowMoveToPageFenceMenu(POINT screen);
  void ShowBackgroundMenu(POINT screen);
  void PasteClipboardFiles();
  void EnsureDropTarget();
  // Fences-class: icon host geometry stays fixed; marquee is a separate band.
  void PresentMarqueeBand();
  void HideMarqueeBand();
  void ClearRubberBaseCache();
  bool EnsureRubberBaseCache(int w, int h);
  void ReleasePresentBuffer();
  bool EnsurePresentBuffer(int w, int h);
  bool SelectionFingerprint(std::vector<char>* out) const;

  void EnsureSelSize();
  void ClearSelection();
  void SelectOnly(int index);
  void ToggleSelected(int index);
  void SelectRange(int from, int to);
  void SelectIntersecting(RECT client_rc);
  bool IsSelected(int index) const;
  std::vector<std::wstring> SelectedPaths() const;
  void CopySelection();
  void CutSelection();
  void DeleteSelection();
  void RenameSelection();
  void OpenSelection();

  HWND hwnd_ = nullptr;
  HWND marquee_hwnd_ = nullptr;
  PathDropTarget* drop_target_ = nullptr;
  bool want_visible_ = false;
  IconGridMetrics metrics_{};
  std::wstring title_ = L"桌面图标";
  int drag_from_ = -1;
  bool drag_moved_ = false;
  POINT drag_press_{};
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
  ULONGLONG rubber_present_ms_ = 0;
  std::vector<char> rubber_sel_finger_;
  std::vector<char> selected_;
  int selection_anchor_ = -1;
  int current_page_ = 0;
  FloatPlacementMap placements_;
  std::vector<Slot> slots_;
  std::vector<RECT> blocked_;
  std::vector<std::wstring> page_names_;
  std::function<void(const std::wstring&, const std::wstring&)> on_path_replaced_;
  std::function<void(const std::wstring&)> on_removed_;
  std::function<void()> on_refresh_;
  std::function<void()> on_paste_;
  std::function<DWORD(const std::vector<std::wstring>&)> on_paths_dropped_;
  std::function<void(const FloatPlacementMap&)> on_placements_changed_;
  std::function<void(const std::vector<std::wstring>&, const std::string&)>
      on_move_to_fence_;
  std::function<std::vector<MoveFenceMenuTarget>()> on_query_fences_;
};
