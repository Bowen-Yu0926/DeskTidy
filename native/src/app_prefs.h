#pragma once

#include <string>
#include <vector>

struct AppPrefs {
  bool auto_organize_on_startup = false;
  // Fences-class: new Desktop files pin by rules (Explorer icons stay hidden).
  bool auto_organize_watch = true;
  // 0 = record until toggle/stop; >0 = ffmpeg -t seconds auto-stop.
  int record_seconds = 0;
  // Soft file-size cap in MiB (P2-38). 0 = unlimited; default 2048.
  int record_max_mb = 2048;
  // 0 low, 1 medium, 2 high (libx264 CRF).
  int record_quality = 2;
  // Capture frame rate (Python screen_record.fps). Clamped 15–60 at load.
  int record_fps = 30;
  // U1 screen_record.enabled — tray (T1) reads this to decide whether recording
  // is available. Default false (Python screen_record.enabled).
  bool record_enabled = false;
  std::wstring record_output_dir;
  // DirectShow microphone name. Empty skips audio.
  std::wstring record_mic;
  // Fence style preset (Python fence_style_preset). Legacy JSON key: theme_id.
  std::string theme_id = "navy";
  // Settings UI theme (Python theme): mist / graphite / sky / mint / lavender / peach.
  std::string ui_theme = "mist";
  // Extensions page (Python float-bar / feature toggles).
  bool minutes_on_bar = true;
  // U1: real enable/disable of each tool (Python meeting_minutes.enabled /
  // calculator.enabled / desktop_todos.enabled). Distinct from *_on_bar which
  // only controls the page-bar visibility.
  bool minutes_enabled = false;
  bool calculator_enabled = false;
  bool todos_enabled = false;
  bool notepad_enabled = true;
  bool record_on_bar = false;
  bool calculator_on_bar = false;
  bool todos_on_bar = false;
  bool screenshot_auto_pin = false;
  int screenshot_max_pins = 3;
  std::wstring notepad_folder;
  // One-shot migration marker: legacy DeskTidyNative\notes copied to the
  // configured notes folder (Python notepad folder). Empty = not yet migrated.
  bool notes_migrated = false;
  // Right-edge page bar Y offset within the primary work area (-1 = center).
  int page_bar_anchor_y = -1;
  // File search (Python file_search).
  bool search_desktop = true;
  bool search_documents = true;
  bool search_downloads = true;
  // When true, search all fixed drives. Default false — pick disks in the
  // search panel (otherwise Desktop/Documents/Downloads).
  bool search_all_drives = false;
  int search_max_results = 50;
  int search_max_depth = 12;
  // U1 file_search.include_open_folders / include_hidden / threads (Python
  // defaults: true / false / 2).
  bool search_include_open_folders = true;
  bool search_include_hidden = false;
  int search_threads = 2;
  // Dock (Python dock.position / opacity).
  // 0=bottom, 1=left, 2=right
  int dock_edge = 0;
  // 50–100 percent opacity for the dock chrome.
  int dock_opacity = 85;
  // Dock visibility (Python dock.enabled). Off by default.
  bool dock_visible = false;
  // U1 dock.items — independent dock item list (Python dock.items). No longer
  // borrows the system_software fence pins.
  std::vector<std::wstring> dock_items;
  bool check_update_on_startup = true;
  // Opt-in: ShellExecute runas hotkey helper for elevated/fullscreen FG (P1-28).
  // Default false — never UAC on every cold start (market: normal-priv always-on).
  bool elevate_hotkey_helper = false;
  std::wstring update_notified_tag;
  // Unix seconds when update_notified_tag was last soft-reminded (24h dedupe).
  long long update_notified_at = 0;
  // Desktop background cascade menu (Python desktop_right_click_menu).
  bool desktop_right_click_menu = true;
  // Account vault (Python account_vault).
  bool vault_enabled = false;
  std::wstring vault_api_base =
      L"https://bowen-yu0926.byethost10.com/desktidy-vault/";
  int vault_refresh_hours = 2;
  // 0 = never clear; else seconds after password copy.
  int vault_clipboard_clear_seconds = 30;
  // Settings → 常规 (Python MainWindow general prefs).
  bool close_to_tray = true;
  // One-shot "close vs minimize-to-tray" prompt (Python close_behavior_prompted).
  bool close_behavior_prompted = false;
  // Running-session marker (Python session_active).
  bool session_active = false;
  bool show_fences = true;          // always on; UI toggle removed
  bool double_click_hide = false;   // feature removed; kept for prefs compat
  // Fences-class: unpinned desktop floats shared across pages (default on).
  bool enable_public_desktop = true;
  // Desktop 「账」float position (-1 = default corner).
  int vault_launcher_x = -1;
  int vault_launcher_y = -1;
  // Right-edge page bar when pet does not host pages (Python show_page_indicator).
  bool show_page_indicator = true;
  // Edge page chrome: "float" = mist card peek; "tabs" = discrete bookmark chips.
  std::string page_chrome_style = "float";
  // Portal / desktop scan exclude patterns (Python exclude_patterns).
  std::vector<std::wstring> exclude_patterns;
  // U6 first-run markers (Python first_run_guide_done / first_run_organize_done).
  // False = not yet shown/done, so a true first install shows the guide and runs
  // the one-time organize.
  bool first_run_guide_done = false;
  bool first_run_organize_done = false;
  // U6 last-seen product version, for the upgrade "new features" tip.
  std::wstring last_seen_version;
  // Notes library search (Python notes_library.py MAX_SEARCH_* / casefold).
  // Case-sensitive content search (default false = case-insensitive).
  bool notes_search_case_sensitive = false;
  // Cap on search hits so the UI stays responsive (Python 200).
  int notes_search_max_hits = 200;
  // Skip files larger than this many bytes during content search (Python 512KiB).
  int notes_search_max_file_bytes = 512 * 1024;
};

AppPrefs LoadAppPrefs();
void SaveAppPrefs(const AppPrefs& prefs);
std::wstring NormalizeRecordMic(const std::wstring& mic);
AppPrefs LoadAppPrefsFromPath(const std::wstring& path);
void SaveAppPrefsToPath(const AppPrefs& prefs, const std::wstring& path);
