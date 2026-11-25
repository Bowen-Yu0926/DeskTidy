#include "app_prefs.h"

#include "paths_util.h"

#include <Windows.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::wstring PrefsPath() { return JoinPath(NativeDataDir(), L"app_prefs.json"); }

bool ParseBoolField(const std::string& json, const char* key, bool fallback) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return fallback;
  }
  p = json.find(':', p);
  if (p == std::string::npos) {
    return fallback;
  }
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  if (json.compare(p, 4, "true") == 0) {
    return true;
  }
  if (json.compare(p, 5, "false") == 0) {
    return false;
  }
  return fallback;
}

// Consume the numeric token at `p` in place. Returns false when the token is
// not a well-formed integer (no digits, or a value beyond `limit`), in which
// case the caller keeps its fallback. Deliberately non-throwing: this runs on
// the startup path with no top-level catch, so a truncated or hostile prefs
// file ("record_fps": -, "record_fps": 99999999999) must not terminate the app.
bool ConsumeIntToken(const std::string& json, size_t p, long long limit,
                     long long* out) {
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  bool negative = false;
  if (p < json.size() && json[p] == '-') {
    negative = true;
    ++p;
  }
  const size_t digits_begin = p;
  long long value = 0;
  while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
    const int digit = json[p] - '0';
    // Compare before multiplying so a long digit run cannot overflow.
    if (value > (limit - digit) / 10) {
      return false;  // saturate to the caller's fallback, never wrap
    }
    value = value * 10 + digit;
    ++p;
  }
  if (p == digits_begin) {
    return false;
  }
  *out = negative ? -value : value;
  return true;
}

// Locate the value of "key" and parse it as an integer no larger than limit.
bool FindIntField(const std::string& json, const char* key, long long limit,
                  long long* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  return ConsumeIntToken(json, p + 1, limit, out);
}

int ParseIntField(const std::string& json, const char* key, int fallback) {
  long long v = 0;
  if (!FindIntField(json, key, 0x7FFFFFFF, &v)) {
    return fallback;
  }
  return static_cast<int>(v);
}

long long ParseInt64Field(const std::string& json, const char* key,
                          long long fallback) {
  long long v = 0;
  if (!FindIntField(json, key, 0x7FFFFFFFFFFFFFFFLL, &v)) {
    return fallback;
  }
  return v;
}

std::string ParseStringField(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return {};
  }
  const size_t colon = json.find(':', p + needle.size());
  if (colon == std::string::npos) {
    return {};
  }
  p = json.find('"', colon + 1);
  if (p == std::string::npos) {
    return {};
  }
  ++p;
  std::string v;
  while (p < json.size() && json[p] != '"') {
    if (json[p] == '\\' && p + 1 < json.size()) {
      ++p;
    }
    v.push_back(json[p++]);
  }
  return v;
}

std::string EscapeJson(const std::string& s) { return EscapeJsonUtf8(s); }

// Parse a JSON array of strings, e.g. "exclude_patterns":["desktop.ini","DeskTidy"].
std::vector<std::wstring> ParseStringArrayField(const std::string& json,
                                                const char* key) {
  std::vector<std::wstring> out;
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return out;
  }
  p = json.find('[', p + needle.size());
  if (p == std::string::npos) {
    return out;
  }
  const size_t end = json.find(']', p);
  if (end == std::string::npos) {
    return out;
  }
  size_t q = p + 1;
  while (q < end) {
    while (q < end && (json[q] == ' ' || json[q] == '\t' || json[q] == '\n' ||
                       json[q] == '\r' || json[q] == ',')) {
      ++q;
    }
    if (q >= end || json[q] != '"') {
      break;
    }
    ++q;
    std::string raw;
    while (q < end && json[q] != '"') {
      if (json[q] == '\\' && q + 1 < end) {
        ++q;
        if (json[q] == 'n') {
          raw.push_back('\n');
        } else if (json[q] == 'r') {
          raw.push_back('\r');
        } else if (json[q] == 't') {
          raw.push_back('\t');
        } else {
          raw.push_back(json[q]);
        }
      } else {
        raw.push_back(json[q]);
      }
      ++q;
    }
    if (q < end && json[q] == '"') {
      ++q;
    }
    out.push_back(WidenUtf8(raw));
  }
  return out;
}

bool ReadTextFile(const std::wstring& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

// A prefs file is usable if it is non-empty and has a brace pair (not a
// truncated / corrupted write). The per-field parser is otherwise lenient.
bool LooksLikePrefsJson(const std::string& s) {
  return s.find('{') != std::string::npos && s.find('}') != std::string::npos;
}

}  // namespace

// Normalize the record microphone device string. Python used to persist a
// literal `""` (two quote characters) as the audio device; treat that and any
// other pure-quote / whitespace value as "no device" so ffmpeg never receives
// `audio=""""`.
std::wstring NormalizeRecordMic(const std::wstring& mic) {
  const std::wstring ws = L" \t\r\n";
  const size_t b = mic.find_first_not_of(ws);
  if (b == std::wstring::npos) {
    return {};
  }
  const size_t e = mic.find_last_not_of(ws);
  const std::wstring t = mic.substr(b, e - b + 1);
  for (wchar_t c : t) {
    if (c != L'"' && c != L'\'' && c != L' ' && c != L'\t' && c != L'\r' &&
        c != L'\n') {
      return t;
    }
  }
  return {};
}

AppPrefs LoadAppPrefsFromPath(const std::wstring& path) {
  std::string json;
  if (!ReadTextFile(path, &json) || !LooksLikePrefsJson(json)) {
    // Main file missing / empty / truncated — fall back to the .bak copy.
    // Do not overwrite the main file here; the next normal save will.
    const std::wstring bak = path + L".bak";
    if (!ReadTextFile(bak, &json) || !LooksLikePrefsJson(json)) {
      return AppPrefs{};
    }
  }
  AppPrefs prefs;
  prefs.auto_organize_on_startup =
      ParseBoolField(json, "auto_organize_on_startup", false);
  prefs.auto_organize_watch =
      ParseBoolField(json, "auto_organize_watch", true);
  prefs.record_seconds = ParseIntField(json, "record_seconds", 0);
  if (prefs.record_seconds < 0) {
    prefs.record_seconds = 0;
  }
  prefs.record_max_mb = ParseIntField(json, "record_max_mb", 2048);
  if (prefs.record_max_mb < 0) {
    prefs.record_max_mb = 0;
  }
  prefs.record_quality = ParseIntField(json, "record_quality", 2);
  if (prefs.record_quality < 0 || prefs.record_quality > 2) {
    prefs.record_quality = 2;
  }
  prefs.record_fps = ParseIntField(json, "record_fps", 30);
  if (prefs.record_fps < 15 || prefs.record_fps > 60) {
    prefs.record_fps = 30;
  }
  prefs.record_enabled = ParseBoolField(json, "record_enabled", false);
  const std::string dir = ParseStringField(json, "record_output_dir");
  if (!dir.empty()) {
    prefs.record_output_dir = WidenUtf8(dir);
  }
  const std::string mic = ParseStringField(json, "record_mic");
  if (!mic.empty()) {
    const std::wstring norm = NormalizeRecordMic(WidenUtf8(mic));
    if (!norm.empty()) {
      prefs.record_mic = norm;
    }
  }
  const std::string theme = ParseStringField(json, "theme_id");
  if (!theme.empty()) {
    prefs.theme_id = theme;
  }
  // Python fence_style_preset / theme keys (import compatibility).
  const std::string fence_preset = ParseStringField(json, "fence_style_preset");
  if (!fence_preset.empty()) {
    prefs.theme_id = fence_preset;
  }
  const std::string ui = ParseStringField(json, "ui_theme");
  if (!ui.empty()) {
    prefs.ui_theme = ui;
  } else {
    const std::string py_theme = ParseStringField(json, "theme");
    // Avoid treating fence ids (charcoal/…) as UI themes.
    if (py_theme == "mist" || py_theme == "graphite" || py_theme == "sky" ||
        py_theme == "mint" || py_theme == "lavender" || py_theme == "peach") {
      prefs.ui_theme = py_theme;
    }
  }
  prefs.minutes_on_bar = ParseBoolField(json, "minutes_on_bar", true);
  prefs.minutes_enabled = ParseBoolField(json, "minutes_enabled", false);
  prefs.calculator_enabled = ParseBoolField(json, "calculator_enabled", false);
  prefs.todos_enabled = ParseBoolField(json, "todos_enabled", false);
  prefs.notepad_enabled = ParseBoolField(json, "notepad_enabled", true);
  prefs.elevate_hotkey_helper =
      ParseBoolField(json, "elevate_hotkey_helper", false);
  prefs.record_on_bar = ParseBoolField(json, "record_on_bar", false);
  prefs.calculator_on_bar = ParseBoolField(json, "calculator_on_bar", false);
  prefs.todos_on_bar = ParseBoolField(json, "todos_on_bar", false);
  prefs.screenshot_auto_pin =
      ParseBoolField(json, "screenshot_auto_pin", false);
  prefs.screenshot_max_pins = ParseIntField(json, "screenshot_max_pins", 3);
  if (prefs.screenshot_max_pins < 1) {
    prefs.screenshot_max_pins = 1;
  }
  if (prefs.screenshot_max_pins > 5) {
    prefs.screenshot_max_pins = 5;
  }
  const std::string notes = ParseStringField(json, "notepad_folder");
  if (!notes.empty()) {
    prefs.notepad_folder = WidenUtf8(notes);
  }
  prefs.notes_migrated = ParseBoolField(json, "notes_migrated", false);
  prefs.page_bar_anchor_y = ParseIntField(json, "page_bar_anchor_y", -1);
  prefs.search_desktop = ParseBoolField(json, "search_desktop", true);
  prefs.search_documents = ParseBoolField(json, "search_documents", true);
  prefs.search_downloads = ParseBoolField(json, "search_downloads", true);
  prefs.search_all_drives = ParseBoolField(json, "search_all_drives", false);
  prefs.search_max_results = ParseIntField(json, "search_max_results", 50);
  if (prefs.search_max_results < 10) {
    prefs.search_max_results = 10;
  }
  if (prefs.search_max_results > 500) {
    prefs.search_max_results = 500;
  }
  prefs.search_max_depth = ParseIntField(json, "search_max_depth", 12);
  if (prefs.search_max_depth < 1) {
    prefs.search_max_depth = 1;
  }
  if (prefs.search_max_depth > 32) {
    prefs.search_max_depth = 32;
  }
  prefs.search_include_open_folders =
      ParseBoolField(json, "search_include_open_folders", true);
  prefs.search_include_hidden =
      ParseBoolField(json, "search_include_hidden", false);
  prefs.search_threads = ParseIntField(json, "search_threads", 2);
  if (prefs.search_threads < 1) {
    prefs.search_threads = 1;
  }
  if (prefs.search_threads > 64) {
    prefs.search_threads = 64;
  }
  prefs.dock_edge = ParseIntField(json, "dock_edge", 0);
  if (prefs.dock_edge < 0 || prefs.dock_edge > 2) {
    prefs.dock_edge = 0;
  }
  prefs.dock_opacity = ParseIntField(json, "dock_opacity", 85);
  if (prefs.dock_opacity < 50) {
    prefs.dock_opacity = 50;
  }
  if (prefs.dock_opacity > 100) {
    prefs.dock_opacity = 100;
  }
  prefs.dock_visible = ParseBoolField(json, "dock_visible", false);
  prefs.dock_items = ParseStringArrayField(json, "dock_items");
  prefs.check_update_on_startup =
      ParseBoolField(json, "check_update_on_startup", true);
  const std::string notified = ParseStringField(json, "update_notified_tag");
  if (!notified.empty()) {
    prefs.update_notified_tag = WidenUtf8(notified);
  }
  prefs.update_notified_at =
      ParseInt64Field(json, "update_notified_at", 0);
  // auto_snapshot_on_change removed — ignore if present in older prefs files.
  // Always on; settings toggle removed (parity: shell verbs stay registered).
  prefs.desktop_right_click_menu = true;
  prefs.close_to_tray = ParseBoolField(json, "close_to_tray", true);
  prefs.close_behavior_prompted =
      ParseBoolField(json, "close_behavior_prompted", false);
  prefs.session_active = ParseBoolField(json, "session_active", false);
  prefs.show_fences = true;  // always shown; ignore persisted false
  prefs.double_click_hide = false;  // feature removed
  const bool public_migrated =
      ParseBoolField(json, "public_desktop_pref_migrated_v1", false);
  // 公共区域功能（未入分区图标跨分页共享）默认且始终启用
  prefs.enable_public_desktop = true;
  prefs.public_icon_zoom =
      (std::min)(160, (std::max)(70,
                                 ParseIntField(json, "public_icon_zoom", 100)));
  prefs.show_page_indicator =
      ParseBoolField(json, "show_page_indicator", true);
  {
    const std::string chrome = ParseStringField(json, "page_chrome_style");
    if (chrome == "tabs" || chrome == "tab" || chrome == "chips") {
      prefs.page_chrome_style = "tabs";
    } else {
      prefs.page_chrome_style = "float";
    }
  }
  {
    const std::string bar_style = ParseStringField(json, "page_bar_style");
    if (!bar_style.empty()) {
      prefs.page_bar_style = bar_style;
    } else {
      prefs.page_bar_style = "navy";
    }
  }
  prefs.exclude_patterns = ParseStringArrayField(json, "exclude_patterns");
  if (prefs.exclude_patterns.empty()) {
    prefs.exclude_patterns = {L"desktop.ini", L"DeskTidy"};
  }
  prefs.first_run_guide_done =
      ParseBoolField(json, "first_run_guide_done", false);
  prefs.first_run_organize_done =
      ParseBoolField(json, "first_run_organize_done", false);
  const std::string seen_ver = ParseStringField(json, "last_seen_version");
  if (!seen_ver.empty()) {
    prefs.last_seen_version = WidenUtf8(seen_ver);
  }
  prefs.notes_search_case_sensitive =
      ParseBoolField(json, "notes_search_case_sensitive", false);
  prefs.notes_search_max_hits = ParseIntField(json, "notes_search_max_hits", 200);
  if (prefs.notes_search_max_hits < 10) {
    prefs.notes_search_max_hits = 10;
  }
  if (prefs.notes_search_max_hits > 2000) {
    prefs.notes_search_max_hits = 2000;
  }
  prefs.notes_search_max_file_bytes =
      ParseIntField(json, "notes_search_max_file_bytes", 512 * 1024);
  if (prefs.notes_search_max_file_bytes < 64 * 1024) {
    prefs.notes_search_max_file_bytes = 64 * 1024;
  }
  if (prefs.notes_search_max_file_bytes > 16 * 1024 * 1024) {
    prefs.notes_search_max_file_bytes = 16 * 1024 * 1024;
  }
  if (!public_migrated) {
    // Persist heal so a later opt-out is respected across restarts.
    SaveAppPrefsToPath(prefs, path);
  }
  return prefs;
}

AppPrefs LoadAppPrefs() { return LoadAppPrefsFromPath(PrefsPath()); }

void SaveAppPrefsToPath(const AppPrefs& prefs, const std::wstring& path) {
  std::ostringstream out;
  out << "{\n  \"auto_organize_on_startup\": "
      << (prefs.auto_organize_on_startup ? "true" : "false")
      << ",\n  \"auto_organize_watch\": "
      << (prefs.auto_organize_watch ? "true" : "false")
      << ",\n  \"record_seconds\": " << prefs.record_seconds
      << ",\n  \"record_max_mb\": " << prefs.record_max_mb
      << ",\n  \"record_quality\": " << prefs.record_quality
      << ",\n  \"record_fps\": " << prefs.record_fps
      << ",\n  \"record_enabled\": "
      << (prefs.record_enabled ? "true" : "false")
      << ",\n  \"record_output_dir\": \""
      << EscapeJson(NarrowUtf8(prefs.record_output_dir))
      << "\",\n  \"record_mic\": \""
      << EscapeJson(NarrowUtf8(prefs.record_mic))
      << "\",\n  \"theme_id\": \"" << EscapeJson(prefs.theme_id)
      << "\",\n  \"fence_style_preset\": \"" << EscapeJson(prefs.theme_id)
      << "\",\n  \"ui_theme\": \"" << EscapeJson(prefs.ui_theme)
      << "\",\n  \"theme\": \"" << EscapeJson(prefs.ui_theme)
      << "\",\n  \"minutes_on_bar\": "
      << (prefs.minutes_on_bar ? "true" : "false")
      << ",\n  \"minutes_enabled\": "
      << (prefs.minutes_enabled ? "true" : "false")
      << ",\n  \"calculator_enabled\": "
      << (prefs.calculator_enabled ? "true" : "false")
      << ",\n  \"todos_enabled\": "
      << (prefs.todos_enabled ? "true" : "false")
      << ",\n  \"notepad_enabled\": "
      << (prefs.notepad_enabled ? "true" : "false")
      << ",\n  \"elevate_hotkey_helper\": "
      << (prefs.elevate_hotkey_helper ? "true" : "false")
      << ",\n  \"record_on_bar\": "
      << (prefs.record_on_bar ? "true" : "false")
      << ",\n  \"calculator_on_bar\": "
      << (prefs.calculator_on_bar ? "true" : "false")
      << ",\n  \"todos_on_bar\": "
      << (prefs.todos_on_bar ? "true" : "false")
      << ",\n  \"screenshot_auto_pin\": "
      << (prefs.screenshot_auto_pin ? "true" : "false")
      << ",\n  \"screenshot_max_pins\": " << prefs.screenshot_max_pins
      << ",\n  \"notepad_folder\": \""
      << EscapeJson(NarrowUtf8(prefs.notepad_folder))
      << "\",\n  \"notes_migrated\": "
      << (prefs.notes_migrated ? "true" : "false")
      << ",\n  \"page_bar_anchor_y\": " << prefs.page_bar_anchor_y
      << ",\n  \"search_desktop\": "
      << (prefs.search_desktop ? "true" : "false")
      << ",\n  \"search_documents\": "
      << (prefs.search_documents ? "true" : "false")
      << ",\n  \"search_downloads\": "
      << (prefs.search_downloads ? "true" : "false")
      << ",\n  \"search_all_drives\": "
      << (prefs.search_all_drives ? "true" : "false")
      << ",\n  \"search_max_results\": " << prefs.search_max_results
      << ",\n  \"search_max_depth\": " << prefs.search_max_depth
      << ",\n  \"search_include_open_folders\": "
      << (prefs.search_include_open_folders ? "true" : "false")
      << ",\n  \"search_include_hidden\": "
      << (prefs.search_include_hidden ? "true" : "false")
      << ",\n  \"search_threads\": " << prefs.search_threads
      << ",\n  \"dock_edge\": " << prefs.dock_edge
      << ",\n  \"dock_opacity\": " << prefs.dock_opacity
      << ",\n  \"dock_visible\": " << (prefs.dock_visible ? "true" : "false")
      << ",\n  \"check_update_on_startup\": "
      << (prefs.check_update_on_startup ? "true" : "false")
      << ",\n  \"update_notified_tag\": \""
      << EscapeJson(NarrowUtf8(prefs.update_notified_tag))
      << "\",\n  \"update_notified_at\": " << prefs.update_notified_at
      << ",\n  \"desktop_right_click_menu\": "
      << (prefs.desktop_right_click_menu ? "true" : "false")
      << ",\n  \"close_to_tray\": "
      << (prefs.close_to_tray ? "true" : "false")
      << ",\n  \"close_behavior_prompted\": "
      << (prefs.close_behavior_prompted ? "true" : "false")
      << ",\n  \"session_active\": "
      << (prefs.session_active ? "true" : "false")
      << ",\n  \"show_fences\": " << (prefs.show_fences ? "true" : "false")
      << ",\n  \"double_click_hide\": "
      << (prefs.double_click_hide ? "true" : "false")
      << ",\n  \"enable_public_desktop\": "
      << (prefs.enable_public_desktop ? "true" : "false")
      << ",\n  \"public_desktop_pref_migrated_v1\": true"
      << ",\n  \"public_icon_zoom\": " << prefs.public_icon_zoom
      << ",\n  \"show_page_indicator\": "
      << (prefs.show_page_indicator ? "true" : "false")
      << ",\n  \"page_chrome_style\": \""
      << EscapeJson(prefs.page_chrome_style) << "\""
      << ",\n  \"page_bar_style\": \""
      << EscapeJson(prefs.page_bar_style) << "\""
       << ",\n  \"exclude_patterns\": [";
  for (size_t i = 0; i < prefs.exclude_patterns.size(); ++i) {
    if (i) {
      out << ", ";
    }
    out << "\"" << EscapeJson(NarrowUtf8(prefs.exclude_patterns[i])) << "\"";
  }
  out << "],\n  \"dock_items\": [";
  for (size_t i = 0; i < prefs.dock_items.size(); ++i) {
    if (i) {
      out << ", ";
    }
    out << "\"" << EscapeJson(NarrowUtf8(prefs.dock_items[i])) << "\"";
  }
  out << "],\n  \"notes_search_case_sensitive\": "
      << (prefs.notes_search_case_sensitive ? "true" : "false")
      << ",\n  \"notes_search_max_hits\": " << prefs.notes_search_max_hits
      << ",\n  \"notes_search_max_file_bytes\": "
      << prefs.notes_search_max_file_bytes
      << ",\n  \"first_run_guide_done\": "
      << (prefs.first_run_guide_done ? "true" : "false")
      << ",\n  \"first_run_organize_done\": "
      << (prefs.first_run_organize_done ? "true" : "false")
      << ",\n  \"last_seen_version\": \""
      << EscapeJson(NarrowUtf8(prefs.last_seen_version))
      << "\"\n}\n";
  const std::string json = out.str();

  // Atomic write: unique temp + write-through flush, then atomic replace with
  // a .bak of the previous good file. Shared helper so this and the layout /
  // rules / todos stores cannot drift apart.
  AtomicWriteUtf8File(path, json);
}

void SaveAppPrefs(const AppPrefs& prefs) {
  SaveAppPrefsToPath(prefs, PrefsPath());
}
