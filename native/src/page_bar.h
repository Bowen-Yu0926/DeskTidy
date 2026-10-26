#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Right-edge page peeks (Python PageIndicatorWidget): vertical dock, draggable
// on Y, always snapped to the right. Tools follow float_bar_tool_flags —
// record / minutes / calculator / todo only when enabled; never note/search/±.
class PageBar {
 public:
  using PageFn = std::function<void(int)>;
  using VoidFn = std::function<void()>;
  using ToolFn = std::function<void(const std::wstring& id)>;
  using FolderFn = std::function<void(const std::wstring& path)>;

  struct FolderChip {
    std::wstring name;
    std::wstring path;
  };

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  HWND Hwnd() const { return hwnd_; }
  void SetPage(int page);
  void SetPageCount(int count);
  void SetPageNames(const std::vector<std::wstring>& names);
  // U7 page.locked — mirror the layout lock flags so locked pages show a lock
  // marker and disable fence create/move/delete (Python is_locked_page_id).
  void SetPageLocks(const std::vector<bool>& locks);
  // When false (pet hosts page chips), hide pages; tools also hidden (Python).
  void SetPagesVisible(bool on);
  // Python float_bar_tool_flags + vault peek (edge bar hosts 账号 when enabled).
  void SetToolFlags(bool record, bool minutes, bool calculator, bool todo,
                    bool vault = false, bool note = false);
  void SetFolders(const std::vector<FolderChip>& folders);
  void SetAnchorY(int y);  // work-area relative preferred; -1 = center
  int AnchorY() const { return anchor_y_; }
  // X2: rescale chip height / slot pitch / font size for a runtime DPI change.
  void ScaleForDpi(int old_dpi, int new_dpi);
  void ApplyStyle(COLORREF background, COLORREF accent, COLORREF title);
  void SetOnPage(PageFn fn) { on_page_ = std::move(fn); }
  void SetOnRenamePage(VoidFn fn) { on_rename_page_ = std::move(fn); }
  void SetOnTool(ToolFn fn) { on_tool_ = std::move(fn); }
  void SetOnFolder(FolderFn fn) { on_folder_ = std::move(fn); }
  void SetOnMoved(VoidFn fn) { on_moved_ = std::move(fn); }
  // R5 page-bar context menu (per-page actions; 0/1 are locked system pages).
  void SetOnCreateFence(PageFn fn) { on_create_fence_ = std::move(fn); }
  void SetOnDeletePage(PageFn fn) { on_delete_page_ = std::move(fn); }
  void SetOnRenamePageAt(PageFn fn) { on_rename_page_at_ = std::move(fn); }
  // U7 page.locked — toggle lock for a page via the bar's right-click menu.
  void SetOnTogglePageLock(PageFn fn) { on_toggle_page_lock_ = std::move(fn); }
  // P1-16: "移动分区到该页" — list all fences so the user can move one onto the
  // right-clicked page. Titles are refreshed via SyncPageChrome.
  void SetFenceTitles(const std::vector<std::wstring>& titles) {
    fence_titles_ = titles;
  }
  void SetOnMoveFenceToPage(std::function<void(int page, int fence)> fn) {
    on_move_fence_to_page_ = std::move(fn);
  }
  // Page count ceiling (Python semantics: callers prompt, never silently cut).
  static constexpr int kMaxPages = 12;
  void AttachToDesktopLayer();

 private:
  enum class SlotKind { Page, Folder, Tool };
  struct Slot {
    SlotKind kind = SlotKind::Page;
    int page_index = 0;
    std::wstring tool_id;
    std::wstring folder_path;
    std::wstring label;
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void Relayout();
  void RebuildSlots();
  int HitTest(int x, int y) const;
  std::wstring PageLabel(int index) const;
  bool PageLockedAt(int index) const;
  void DockToRightEdge(int preferred_y);
  void ActivateSlot(int index);
  void ShowPageContextMenu(int page_index);
  void EnsureTooltip();
  void UpdateHoverTip(int index);

  HWND hwnd_ = nullptr;
  HWND tip_ = nullptr;
  int page_ = 0;
  int page_count_ = 2;
  std::vector<std::wstring> page_names_;
  std::vector<bool> page_locked_;
  std::vector<Slot> slots_;
  bool pages_visible_ = true;
  bool tool_record_ = false;
  bool tool_minutes_ = false;
  bool tool_calculator_ = false;
  bool tool_todo_ = false;
  bool tool_vault_ = false;
  bool tool_note_ = false;
  std::vector<FolderChip> folders_;
  int anchor_y_ = -1;  // -1 = vertical center
  // X2: DPI-scaled chip metrics (start at the 96-DPI design values).
  int chip_w_ = 86;
  int chip_h_ = 34;
  int slot_h_ = 38;
  float font_scale_ = 1.f;
  bool dragging_ = false;
  bool drag_moved_ = false;
  int hover_index_ = -1;
  POINT drag_grab_{};
  POINT press_pt_{};
  COLORREF bg_ = RGB(24, 26, 32);
  COLORREF accent_ = RGB(70, 130, 200);
  COLORREF title_ = RGB(230, 230, 230);
  PageFn on_page_;
  VoidFn on_rename_page_;
  ToolFn on_tool_;
  FolderFn on_folder_;
  VoidFn on_moved_;
  PageFn on_create_fence_;
  PageFn on_delete_page_;
  PageFn on_rename_page_at_;
  PageFn on_toggle_page_lock_;
  std::vector<std::wstring> fence_titles_;
  std::function<void(int, int)> on_move_fence_to_page_;
};
