#pragma once

#include <Windows.h>

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// APPWINDOW settings host — WebView2 UI (Python mist sidebar style).
class SettingsWindow {
 public:
  using VoidFn = std::function<void()>;
  using IndexFn = std::function<void(int)>;
  using RenameFn = std::function<void(int, const std::wstring&)>;
  using PageFn = std::function<void(int, int)>;
  using BoolFn = std::function<void(bool)>;

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  // Prefetch WebView2 so the first tray open is not a cold start.
  void WarmWebView();
  HWND Hwnd() const { return hwnd_; }
  bool IsOpen() const;

  void SetFenceNames(const std::vector<std::wstring>& names);
  void SetPageNames(const std::vector<std::wstring>& names, bool push = true);
  void SetCurrentPage(int page, bool push = true);
  // Mirror a settings.html toggle into ext_ so the next PushState is not stale.
  bool ApplyExtToggleLocal(const std::wstring& id, bool value);
  void ApplyStyle(COLORREF background, COLORREF accent, COLORREF title);
  void SetToggles(bool dock_on, bool pet_on, bool autostart_on);
  void SetOrganizePrefs(bool startup_on, bool watch_on);
  struct OrgFileRow {
    std::wstring name;
    std::wstring type;
    std::wstring size;
  };
  // pending / organized / total KPIs + table rows + dry-run preview lines.
  void SetOrganizePreview(const std::vector<std::wstring>& preview_lines,
                          int pending = -1, int organized = -1, int total = -1,
                          const std::vector<OrgFileRow>& files = {});
  // Extensions page fields (Python ExtensionsWidget parity).
  struct ExtState {
    bool minutes_on_bar = true;
    // U1: real tool enable/disable (Python meeting_minutes/calculator/desktop_todos
    // .enabled). *_on_bar only controls page-bar visibility.
    bool minutes_enabled = false;
    bool calculator_enabled = false;
    bool todos_enabled = false;
    bool notepad_enabled = true;
    // Opt-in UAC hotkey helper for elevated/fullscreen FG (default off).
    bool elevate_hotkey_helper = false;
    bool record_on_bar = false;
    // U1 screen_record.enabled (tray T1 gating).
    bool record_enabled = false;
    bool calculator_on_bar = false;
    bool todos_on_bar = false;
    int screenshot_max_pins = 3;
    int record_quality = 2;
    std::wstring minutes_folder;
    std::wstring notepad_folder;
    std::wstring wallpaper_folder;
    std::wstring record_dir;
    std::wstring record_mic;
    std::vector<std::wstring> record_mics;
    // Resolved defaults for empty-path placeholders (Python resolve_*).
    std::wstring minutes_folder_default;
    std::wstring notepad_folder_default;
    std::wstring record_dir_default;
    struct PageFolder {
      std::wstring name;
      std::wstring path;
    };
    std::vector<PageFolder> page_folders;
    std::vector<std::wstring> whitelist;
    bool search_desktop = true;
    bool search_documents = true;
    bool search_downloads = true;
    bool search_all_drives = false;
    int search_max_results = 50;
    int search_max_depth = 12;
    // U1 file_search.include_open_folders / include_hidden / threads.
    bool search_include_open_folders = true;
    bool search_include_hidden = false;
    int search_threads = 2;
    bool pet_auto_wander = true;
    bool pet_show_bubbles = true;
    float pet_size_scale = 1.f;
    std::wstring pet_character = L"hoodie";
    std::vector<std::wstring> pet_characters;
    // character → enabled action ids (pet/wait/sleep/fall).
    std::map<std::wstring, std::vector<std::wstring>> pet_actions;
    int dock_edge = 0;
    int dock_opacity = 85;
    // U1 dock.items — independent dock item paths (Python dock.items).
    std::vector<std::wstring> dock_items;
    // U1 exclude_patterns — editable portal scan excludes (Python exclude_patterns).
    std::vector<std::wstring> exclude_patterns;
    // U1 filename_rules / category_fence_map (Python organize categories),
    // editable single-line forms ("cat=fence; ..." / "cat=p1,p2; ...").
    std::wstring category_fence_map_text;
    std::wstring filename_rules_text;
    bool check_update_on_startup = true;
    bool desktop_right_click_menu = true;
    bool close_to_tray = true;
  bool show_fences = true;
  bool double_click_hide = false;
    bool enable_public_desktop = true;
    bool show_page_indicator = true;
    std::string page_chrome_style = "float";  // float | tabs
    std::string page_bar_style = "navy";
    std::string lang = "zh";
    std::wstring app_version = L"0.0.0";
    // Fence style preset (legacy field name theme_id in JSON).
    std::string theme_id = "navy";
    std::string ui_theme = "mist";
    struct ThemeOption {
      std::string id;
      std::wstring name;
    };
    std::vector<ThemeOption> themes;
    std::vector<ThemeOption> ui_themes;
    struct FenceCard {
      int index = 0;
      std::wstring title;
      int page = 0;
      // Multi-page membership (Fences); empty → treat as {page}.
      std::vector<int> pages;
      std::string style_id = "navy";
      int opacity_pct = 70;  // 35–100
      // Organize rule kinds: "icon" (软件) / "file" (文档). Empty = no rule.
      std::vector<std::string> organize_kinds;
      bool system_fence = false;
      bool position_locked = false;
      // Freezes icon set + order (separate from position lock, as in Fences).
      bool content_locked = false;
      // Caption lines for idle tiles (1..3) and caption visibility.
      int label_lines = 2;
      bool show_labels = true;
      // Grid pitch percent, title bar visibility, idle fade.
      int cell_scale = 100;
      // Sort key + direction (direction is ignored when sort_by == "manual").
      std::string sort_by = "name";
      bool sort_desc = false;
      // Corner radius px @96 DPI; -1 = follow the style preset.
      int radius = -1;
      bool show_title = true;
      bool fade_on_idle = false;
      // User-hide (fence menu 「隐藏」); settings can Show again (Fences recover).
      bool visible = true;
      // Custom colours override the preset; "#rrggbb" for the pickers.
      bool has_custom_colors = false;
      std::wstring custom_bg;
      std::wstring custom_accent;
    };
    std::vector<FenceCard> fence_cards;
    // Per-page lock flags (index aligned with page_names_).
    std::vector<bool> page_locked;
    struct SnapPreviewBox {
      std::wstring title;
      int x = 0;
      int y = 0;
      int w = 0;
      int h = 0;
      int page = 0;
      std::string style_id = "navy";
      bool collapsed = false;
      int icon_count = 0;
      std::vector<std::string> pin_thumbs;
    };
    struct SnapPreviewIcon {
      int x = 0;
      int y = 0;
      std::string thumb;
    };
    struct SnapCard {
      std::wstring name;
      std::wstring meta;
      int focus_page = 0;
      int work_w = 0;
      int work_h = 0;
      std::vector<std::wstring> page_names;
      std::vector<SnapPreviewBox> preview;
      std::vector<SnapPreviewIcon> icons;
    };
    std::vector<SnapCard> snapshots;
  };
  void SetExtensionState(const ExtState& ext);
  void SetOnExit(VoidFn fn) { on_exit_ = std::move(fn); }
  // Fired when close-to-tray hides the window (P2-1 tray tip + trim).
  void SetOnHideToTray(VoidFn fn) { on_hide_to_tray_ = std::move(fn); }
  // Fired after settings.html is ready (each Navigate). App should re-push
  // extension state so snapshot/fence cards are not lost across reloads.
  void SetOnReady(VoidFn fn) { on_ready_ = std::move(fn); }
  void ApplyUiThemeVars(const std::unordered_map<std::string, std::string>& vars);
  void SetOnFenceStyle(std::function<void(int, const std::wstring&)> fn) {
    on_fence_style_ = std::move(fn);
  }
  void SetOnFenceOpacity(std::function<void(int, int)> fn) {
    on_fence_opacity_ = std::move(fn);
  }
  // Custom bg/accent colours for one fence. `enabled` false = back to preset.
  void SetOnFenceColors(std::function<void(int, bool, const std::wstring&,
                                            const std::wstring&)> fn) {
    on_fence_colors_ = std::move(fn);
  }
  void SetOnFenceStyleAll(std::function<void(const std::wstring&)> fn) {
    on_fence_style_all_ = std::move(fn);
  }
  void SetOnFencePage(PageFn fn) { on_fence_page_card_ = std::move(fn); }
  // Caption lines (1..3) and caption visibility for one fence.
  void SetOnFenceLabels(std::function<void(int, int, bool)> fn) {
    on_fence_labels_ = std::move(fn);
  }
  // Per-fence icon grid pitch (percent), title-bar visibility, idle fade.
  void SetOnFenceScale(std::function<void(int, int)> fn) {
    on_fence_scale_ = std::move(fn);
  }
  // Sort key + direction for one fence. `by` is name/date/created/size/type/
  // manual; `desc` is ignored for manual.
  void SetOnFenceSort(std::function<void(int, const std::wstring&, bool)> fn) {
    on_fence_sort_ = std::move(fn);
  }
  // Corner radius px @96 DPI; -1 = follow the preset.
  void SetOnFenceRadius(std::function<void(int, int)> fn) {
    on_fence_radius_ = std::move(fn);
  }
  void SetOnFenceTitle(std::function<void(int)> fn) {
    on_fence_title_ = std::move(fn);
  }
  void SetOnFenceFade(std::function<void(int)> fn) {
    on_fence_fade_ = std::move(fn);
  }
  struct HotkeyRow {
    std::wstring id;
    std::wstring label;
    std::wstring chord;
  };
  void SetHotkeyRows(const std::vector<HotkeyRow>& rows);
  // Switch 设置 page subtab: general / hotkey / rules / backup.
  void FocusSubpage(const std::wstring& sub);
  void SetOnExtToggle(std::function<void(const std::wstring&, bool)> fn) {
    on_ext_toggle_ = std::move(fn);
  }
  void SetOnExtField(std::function<void(const std::wstring&, const std::wstring&)> fn) {
    on_ext_field_ = std::move(fn);
  }
  void SetOnHotkeyCapture(std::function<void(const std::wstring&)> fn) {
    on_hotkey_capture_ = std::move(fn);
  }
  void SetOnHotkeySet(
      std::function<void(const std::wstring&, const std::wstring&)> fn) {
    on_hotkey_set_ = std::move(fn);
  }
  void SetHandlers(VoidFn on_add, IndexFn on_remove, VoidFn on_seat,
                   RenameFn on_rename, PageFn on_page, VoidFn on_org_virtual,
                   BoolFn on_dock, BoolFn on_pet, VoidFn on_edit_rules,
                   BoolFn on_autostart, VoidFn on_whitelist = {},
                   VoidFn on_page_folders = {}, VoidFn on_help = {},
                   BoolFn on_org_startup = {}, BoolFn on_org_watch = {},
                   VoidFn on_record_cfg = {}, VoidFn on_org_preview = {});
  // Optional: recompute pins for every desktop item ("重新整理").
  void SetOnOrganizeReassign(VoidFn fn) { on_org_reassign_ = std::move(fn); }
  void SetExtraHandlers(VoidFn on_snap_save, VoidFn on_snap_manage,
                        VoidFn on_minutes, VoidFn on_wallpaper,
                        VoidFn on_layout, VoidFn on_notes, VoidFn on_theme,
                        VoidFn on_hotkeys, VoidFn on_hotkey_help);
  // Any action id not handled above (todos, search, shot, …).
  void SetCommandHandler(std::function<void(const std::wstring&)> on_command);

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  bool CreateWebView();
  void DestroyWebView();
  void ResizeWebView();
  void NavigateHome();
  void NavigateFallback();
  void PushState();
  void OnWebMessage(const std::wstring& json);
  void DispatchAction(const std::wstring& json);
  void OnWebReady();
  std::wstring BuildStateJson() const;

  HWND hwnd_ = nullptr;
  HWND host_ = nullptr;
  void* controller_ = nullptr;
  void* webview_ = nullptr;
  bool web_ready_ = false;
  bool web_available_ = false;
  bool web_creating_ = false;
  bool push_pending_ = false;
  bool ready_handling_ = false;
  // Actions that may open MistWebDialog / MessageBox — never run inside
  // WebMessageReceived (nested WebView2 stays blank / freezes).
  std::vector<std::wstring> pending_action_jsons_;

  VoidFn on_add_;
  IndexFn on_remove_;
  VoidFn on_seat_;
  RenameFn on_rename_;
  PageFn on_page_;
  VoidFn on_org_virtual_;
  VoidFn on_org_reassign_;
  BoolFn on_dock_;
  BoolFn on_pet_;
  VoidFn on_edit_rules_;
  BoolFn on_autostart_;
  VoidFn on_whitelist_;
  VoidFn on_page_folders_;
  VoidFn on_help_;
  BoolFn on_org_startup_;
  BoolFn on_org_watch_;
  VoidFn on_record_cfg_;
  VoidFn on_org_preview_;
  VoidFn on_snap_save_;
  VoidFn on_snap_manage_;
  VoidFn on_minutes_;
  VoidFn on_wallpaper_;
  VoidFn on_layout_;
  VoidFn on_notes_;
  VoidFn on_theme_;
  VoidFn on_hotkeys_;
  VoidFn on_hotkey_help_;
  VoidFn on_exit_;
  VoidFn on_hide_to_tray_;
  VoidFn on_ready_;
  std::function<void(const std::wstring&)> on_command_;
  std::function<void(const std::wstring&, bool)> on_ext_toggle_;
  std::function<void(const std::wstring&, const std::wstring&)> on_ext_field_;
  std::function<void(const std::wstring&)> on_hotkey_capture_;
  std::function<void(const std::wstring&, const std::wstring&)> on_hotkey_set_;
  std::function<void(int, const std::wstring&)> on_fence_style_;
  std::function<void(int, int)> on_fence_opacity_;
  std::function<void(int, bool, const std::wstring&, const std::wstring&)>
      on_fence_colors_;
  std::function<void(const std::wstring&)> on_fence_style_all_;
  PageFn on_fence_page_card_;
  std::function<void(int, int, bool)> on_fence_labels_;
  std::function<void(int, int)> on_fence_scale_;
  std::function<void(int, const std::wstring&, bool)> on_fence_sort_;
  std::function<void(int, int)> on_fence_radius_;
  std::function<void(int)> on_fence_title_;
  std::function<void(int)> on_fence_fade_;

  std::vector<std::wstring> fence_names_;
  std::vector<std::wstring> page_names_;
  std::vector<std::wstring> org_preview_;
  std::vector<OrgFileRow> org_files_;
  int current_page_ = 0;
  int org_pending_ = 0;
  int org_organized_ = 0;
  int org_total_ = 0;
  std::vector<HotkeyRow> hotkey_rows_;
  ExtState ext_;
  bool dock_on_ = false;
  bool pet_on_ = true;
  bool autostart_on_ = true;
  bool org_startup_on_ = false;
  bool org_watch_on_ = false;
  int selected_ = 0;
  std::wstring focus_sub_;
};
