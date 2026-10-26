#include "python_layout_sync.h"

#include "app_prefs.h"
#include "fence_style.h"
#include "hotkey_store.h"
#include "layout.h"
#include "page_folders.h"
#include "paths_util.h"
#include "pin_store.h"
#include "whitelist.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Forward declaration (defined in app_prefs.cpp): normalizes record mic device.
std::wstring NormalizeRecordMic(const std::wstring& mic);

namespace {

std::wstring MarkerPath() {
  return JoinPath(NativeDataDir(), L"python_layout_migrated.done");
}

std::wstring PythonSettingsPath() {
  wchar_t profile[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return {};
  }
  return JoinPath(profile, L".desktidy\\settings.json");
}

bool FileExists(const std::wstring& path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES &&
         !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

void WriteMarker() {
  std::ofstream out(MarkerPath(), std::ios::binary);
  out << "ok\n";
}

bool FindNumber(const std::string& body, const char* key, int* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = body.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = body.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  ++p;
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) {
    ++p;
  }
  int sign = 1;
  if (p < body.size() && body[p] == '-') {
    sign = -1;
    ++p;
  }
  if (p >= body.size() || body[p] < '0' || body[p] > '9') {
    return false;
  }
  int v = 0;
  while (p < body.size() && body[p] >= '0' && body[p] <= '9') {
    v = v * 10 + (body[p] - '0');
    ++p;
  }
  *out = v * sign;
  return true;
}

bool FindString(const std::string& body, const char* key, std::string* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = body.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = body.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  p = body.find('"', p + 1);
  if (p == std::string::npos) {
    return false;
  }
  ++p;
  std::string val;
  while (p < body.size() && body[p] != '"') {
    if (body[p] == '\\' && p + 1 < body.size()) {
      ++p;
      val.push_back(body[p]);
    } else {
      val.push_back(body[p]);
    }
    ++p;
  }
  *out = val;
  return true;
}

bool FindBool(const std::string& body, const char* key, bool* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = body.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = body.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  ++p;
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) {
    ++p;
  }
  if (body.compare(p, 4, "true") == 0) {
    *out = true;
    return true;
  }
  if (body.compare(p, 5, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

// Parse a JSON number (int or float), e.g. Python icon_zoom "1.0" / "1.25".
bool FindFloat(const std::string& body, const char* key, double* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = body.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = body.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  ++p;
  while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) {
    ++p;
  }
  double sign = 1.0;
  if (p < body.size() && body[p] == '-') {
    sign = -1.0;
    ++p;
  }
  if (p >= body.size() || (body[p] < '0' || body[p] > '9')) {
    return false;
  }
  double v = 0.0;
  while (p < body.size() && body[p] >= '0' && body[p] <= '9') {
    v = v * 10.0 + (body[p] - '0');
    ++p;
  }
  if (p < body.size() && body[p] == '.') {
    ++p;
    double frac = 0.1;
    while (p < body.size() && body[p] >= '0' && body[p] <= '9') {
      v += frac * (body[p] - '0');
      frac *= 0.1;
      ++p;
    }
  }
  *out = sign * v;
  return true;
}

// Parse a JSON array of strings, e.g. "organize_kinds":["icon","file"].
size_t FindArrayStart(const std::string& json, const char* key);
bool FindStringArray(const std::string& json, const char* key,
                     std::vector<std::string>* out) {
  if (!out) {
    return false;
  }
  const size_t arr = FindArrayStart(json, key);
  if (arr == std::string::npos) {
    return false;
  }
  size_t p = arr + 1;
  bool any = false;
  while (p < json.size()) {
    while (p < json.size() && json[p] != '"' && json[p] != ']') {
      ++p;
    }
    if (p >= json.size() || json[p] == ']') {
      break;
    }
    ++p;
    std::string v;
    while (p < json.size() && json[p] != '"') {
      if (json[p] == '\\' && p + 1 < json.size()) {
        ++p;
      }
      v.push_back(json[p++]);
    }
    if (p < json.size() && json[p] == '"') {
      ++p;
    }
    if (!v.empty()) {
      out->push_back(v);
      any = true;
    }
  }
  return any;
}

std::string ExtractObject(const std::string& json, size_t start, size_t* end_out);

// Locate `"key": { ... }` and return the balanced object body (inclusive braces).
std::string FindNestedObject(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return {};
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) {
    return {};
  }
  ++p;
  while (p < json.size() &&
         (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' ||
          json[p] == '\n')) {
    ++p;
  }
  if (p >= json.size() || json[p] != '{') {
    return {};
  }
  size_t end = 0;
  return ExtractObject(json, p, &end);
}

// Extract balanced {...} starting at `start` (must point at '{').
std::string ExtractObject(const std::string& json, size_t start, size_t* end_out) {
  if (start >= json.size() || json[start] != '{') {
    return {};
  }
  int depth = 0;
  bool in_str = false;
  for (size_t i = start; i < json.size(); ++i) {
    const char c = json[i];
    if (in_str) {
      if (c == '\\' && i + 1 < json.size()) {
        ++i;
        continue;
      }
      if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      --depth;
      if (depth == 0) {
        if (end_out) {
          *end_out = i + 1;
        }
        return json.substr(start, i - start + 1);
      }
    }
  }
  return {};
}

size_t FindArrayStart(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return std::string::npos;
  }
  p = json.find('[', p + needle.size());
  return p;
}

void NormalizeFenceId(std::string* id, std::wstring* title, int* page) {
  if (!id) {
    return;
  }
  if (*id == "system_software") {
    *id = "system_software_side";
  } else if (*id == "system_page2") {
    *id = "system_docs";
  }
  if (*id == "system_software_side") {
    if (title && (title->empty() || *title == L"软件")) {
      *title = L"其他";
    }
    if (page) {
      *page = 0;
    }
  } else if (*id == "system_docs") {
    if (title && (title->empty() || *title == L"第二页")) {
      *title = L"文档";
    }
    if (page) {
      *page = 1;
    }
  } else if (*id == "system_common") {
    if (title && title->empty()) {
      *title = L"常用";
    }
    if (page) {
      *page = 0;
    }
  }
}

void CollectVirtualItemPins(const std::string& fence_obj, const std::string& fence_id,
                            PinMap* pins) {
  if (!pins || fence_id.empty()) {
    return;
  }
  const size_t arr = FindArrayStart(fence_obj, "virtual_items");
  if (arr == std::string::npos) {
    return;
  }
  size_t p = arr + 1;
  while (p < fence_obj.size()) {
    while (p < fence_obj.size() &&
           (fence_obj[p] == ' ' || fence_obj[p] == '\n' || fence_obj[p] == '\r' ||
            fence_obj[p] == '\t' || fence_obj[p] == ',')) {
      ++p;
    }
    if (p >= fence_obj.size() || fence_obj[p] == ']') {
      break;
    }
    if (fence_obj[p] == '"') {
      ++p;
      std::string path;
      while (p < fence_obj.size() && fence_obj[p] != '"') {
        if (fence_obj[p] == '\\' && p + 1 < fence_obj.size()) {
          ++p;
          path.push_back(fence_obj[p++]);
        } else {
          path.push_back(fence_obj[p++]);
        }
      }
      if (p < fence_obj.size() && fence_obj[p] == '"') {
        ++p;
      }
      if (!path.empty()) {
        SetPin(pins, WidenUtf8(path), fence_id);
      }
      continue;
    }
    if (fence_obj[p] == '{') {
      size_t end = p;
      const std::string item = ExtractObject(fence_obj, p, &end);
      p = end;
      std::string path;
      if (FindString(item, "path", &path) && !path.empty()) {
        SetPin(pins, WidenUtf8(path), fence_id);
      }
      continue;
    }
    ++p;
  }
}

bool ApplyLayoutsByPage(const std::string& json, const std::string& fence_id,
                        int page, FenceLayout* layout) {
  if (!layout) {
    return false;
  }
  // fence_layouts_by_page is an object keyed by page string, not an array.
  const std::string needle = "\"fence_layouts_by_page\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = json.find('{', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  size_t end = p;
  const std::string root_obj = ExtractObject(json, p, &end);
  if (root_obj.empty()) {
    return false;
  }
  const std::string page_key = "\"" + std::to_string(page) + "\"";
  size_t pk = root_obj.find(page_key);
  if (pk == std::string::npos) {
    return false;
  }
  pk = root_obj.find('{', pk + page_key.size());
  if (pk == std::string::npos) {
    return false;
  }
  size_t page_end = pk;
  const std::string page_obj = ExtractObject(root_obj, pk, &page_end);
  const std::string fid_key = "\"" + fence_id + "\"";
  size_t fk = page_obj.find(fid_key);
  if (fk == std::string::npos) {
    // Try legacy id inside layouts map.
    return false;
  }
  fk = page_obj.find('{', fk + fid_key.size());
  if (fk == std::string::npos) {
    return false;
  }
  size_t geo_end = fk;
  const std::string geo = ExtractObject(page_obj, fk, &geo_end);
  int x = 0, y = 0, w = 0, h = 0;
  const bool ok_x = FindNumber(geo, "x", &x);
  const bool ok_y = FindNumber(geo, "y", &y);
  bool ok_w = FindNumber(geo, "width", &w);
  if (!ok_w) {
    ok_w = FindNumber(geo, "w", &w);
  }
  bool ok_h = FindNumber(geo, "height", &h);
  if (!ok_h) {
    ok_h = FindNumber(geo, "h", &h);
  }
  if (!(ok_x && ok_y && ok_w && ok_h) || w < 80 || h < 80) {
    return false;
  }
  layout->x = x;
  layout->y = y;
  layout->w = w;
  layout->h = h;
  bool collapsed = false;
  if (FindBool(geo, "collapsed", &collapsed)) {
    layout->collapsed = collapsed;
  }
  return true;
}

void MigratePythonPrefs(const std::string& json);

bool ParseAndWrite(const std::string& json) {
  AppState state;
  state.dock_visible = false;
  state.pet_visible = true;
  FindNumber(json, "current_page", &state.current_page);
  FindBool(json, "pet_visible", &state.pet_visible);
  {
    const std::string pet = FindNestedObject(json, "desktop_pet");
    if (!pet.empty()) {
      bool vis = false;
      if (FindBool(pet, "visible", &vis)) {
        state.pet_visible = vis;
      } else if (FindBool(pet, "enabled", &vis)) {
        state.pet_visible = vis;
      }
    }
    const std::string dock = FindNestedObject(json, "dock");
    if (!dock.empty()) {
      FindBool(dock, "enabled", &state.dock_visible);
    }
  }

  // desktop_pages → page names (ordered by appearance).
  const size_t pages_arr = FindArrayStart(json, "desktop_pages");
  if (pages_arr != std::string::npos) {
    size_t p = pages_arr + 1;
    while (p < json.size()) {
      while (p < json.size() && json[p] != '{' && json[p] != ']') {
        ++p;
      }
      if (p >= json.size() || json[p] == ']') {
        break;
      }
      size_t end = p;
      const std::string obj = ExtractObject(json, p, &end);
      p = end;
      std::string name;
      if (FindString(obj, "name", &name) && !name.empty()) {
        state.page_names.push_back(WidenUtf8(name));
      }
    }
  }
  NormalizePageNames(&state.page_names);

  PinMap pins;
  const size_t fences_arr = FindArrayStart(json, "fences");
  if (fences_arr == std::string::npos) {
    return false;
  }
  size_t p = fences_arr + 1;
  int fence_depth_guard = 0;
  while (p < json.size() && fence_depth_guard < 64) {
    while (p < json.size() && json[p] != '{' && json[p] != ']') {
      ++p;
    }
    if (p >= json.size() || json[p] == ']') {
      break;
    }
    size_t end = p;
    const std::string obj = ExtractObject(json, p, &end);
    p = end;
    ++fence_depth_guard;

    std::string id;
    if (!FindString(obj, "id", &id) || id.empty()) {
      continue;
    }
    // Skip non-fence objects accidentally matched.
    if (id == "current_page") {
      continue;
    }
    std::string title_u8;
    FindString(obj, "name", &title_u8);
    if (title_u8.empty()) {
      FindString(obj, "title", &title_u8);
    }
    std::wstring title = WidenUtf8(title_u8);
    int page = 0;
    FindNumber(obj, "page", &page);
    const std::string raw_id = id;
    NormalizeFenceId(&id, &title, &page);

    FenceLayout fl;
    fl.id = id;
    fl.title = title;
    fl.page = page;
    int x = 0, y = 0, w = 0, h = 0;
    FindNumber(obj, "x", &x);
    FindNumber(obj, "y", &y);
    if (!FindNumber(obj, "width", &w)) {
      FindNumber(obj, "w", &w);
    }
    if (!FindNumber(obj, "height", &h)) {
      FindNumber(obj, "h", &h);
    }
    fl.x = x;
    fl.y = y;
    fl.w = w > 0 ? w : 280;
    fl.h = h > 0 ? h : 200;
    // Prefer per-page layout map (Python runtime geometry).
    ApplyLayoutsByPage(json, raw_id, page, &fl);
    ApplyLayoutsByPage(json, id, page, &fl);

    std::string style;
    // Per-fence style preset id (Python fence["style_preset"]) takes priority;
    // fall back to legacy "style" then the global fence_style_preset.
    if (FindString(obj, "style_preset", &style) && !style.empty()) {
      fl.style_id = NormalizeFenceStyleId(style);
    } else if (FindString(obj, "style", &style) && !style.empty() &&
               style[0] != '{') {
      fl.style_id = NormalizeFenceStyleId(style);
    } else {
      FindString(json, "fence_style_preset", &fl.style_id);
      fl.style_id = NormalizeFenceStyleId(fl.style_id);
    }
    // P2-12: Python style dict → opacity / colors / radius / title / collapse.
    {
      const std::string style_obj = FindNestedObject(obj, "style");
      if (!style_obj.empty()) {
        double opacity = 0.0;
        if (FindFloat(style_obj, "opacity", &opacity) && opacity > 0.0) {
          fl.opacity = (std::min)(
              255, (std::max)(40, static_cast<int>(opacity * 255.0 + 0.5)));
        }
        double radius = 0.0;
        if (FindFloat(style_obj, "border_radius", &radius) && radius >= 0.0) {
          fl.radius = static_cast<int>(radius + 0.5);
        }
        FindBool(style_obj, "show_title", &fl.show_title);
        FindBool(style_obj, "collapsible", &fl.collapsible);
        auto parse_hex = [](const std::string& hex, COLORREF* out) -> bool {
          if (!out || hex.size() < 7 || hex[0] != '#') {
            return false;
          }
          unsigned r = 0, g = 0, b = 0;
          if (sscanf_s(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b) != 3) {
            return false;
          }
          *out = RGB(r, g, b);
          return true;
        };
        std::string bg_hex;
        std::string accent_hex;
        COLORREF bg = 0;
        COLORREF accent = 0;
        bool got = false;
        if (FindString(style_obj, "background", &bg_hex) &&
            parse_hex(bg_hex, &bg)) {
          fl.custom_bg = bg;
          got = true;
        }
        if (FindString(style_obj, "accent", &accent_hex) &&
            parse_hex(accent_hex, &accent)) {
          fl.custom_accent = accent;
          got = true;
        }
        fl.has_custom_colors = got;
        std::string vm;
        if (FindString(style_obj, "view_mode", &vm) && !vm.empty()) {
          fl.view_mode = (vm == "list") ? "list" : "grid";
        }
      }
    }
    // P2-11: Python fence["pages"] multi-page membership.
    {
      const size_t arr = FindArrayStart(obj, "pages");
      if (arr != std::string::npos) {
        size_t lb = arr;
        // FindArrayStart returns index of '['.
        size_t rb = obj.find(']', lb);
        if (rb != std::string::npos) {
          size_t p = lb + 1;
          while (p < rb) {
            while (p < rb &&
                   (obj[p] == ' ' || obj[p] == ',' || obj[p] == '\t' ||
                    obj[p] == '\n' || obj[p] == '\r')) {
              ++p;
            }
            if (p >= rb || obj[p] < '0' || obj[p] > '9') {
              break;
            }
            int v = 0;
            while (p < rb && obj[p] >= '0' && obj[p] <= '9') {
              v = v * 10 + (obj[p] - '0');
              ++p;
            }
            fl.pages.push_back(v);
          }
        }
      }
      if (fl.pages.empty()) {
        fl.pages.push_back(fl.page);
      } else {
        fl.page = fl.pages[0];
      }
    }
    std::string portal;
    // Folder Portal mirrors a real directory (Python fence_rules.is_portal_fence:
    // portal_path or type=="portal"). `folder` is only a storage name, NOT a
    // mirror path — never map it to portal_path (P0: otherwise every normal
    // fence turns into an empty mirror and its pinned content disappears).
    if (FindString(obj, "portal_path", &portal) && !portal.empty()) {
      fl.portal_path = WidenUtf8(portal);
    }
    FindBool(obj, "collapsed", &fl.collapsed);
    FindBool(obj, "position_locked", &fl.position_locked);
    FindBool(obj, "visible", &fl.visible);

    // R6: sort_by / view_mode / organize_kinds / icon_zoom per fence.
    std::string sort_by;
    if (FindString(obj, "sort_by", &sort_by) && !sort_by.empty()) {
      fl.sort_by = sort_by;
    }
    std::string view_mode;
    if (FindString(obj, "view_mode", &view_mode) && !view_mode.empty()) {
      fl.view_mode = (view_mode == "list") ? "list" : "grid";
    }
    FindStringArray(obj, "organize_kinds", &fl.organize_kinds);
    double zoom = 0.0;
    if (FindFloat(obj, "icon_zoom", &zoom) && zoom > 0.05) {
      const int percent = static_cast<int>(std::lround(zoom * 100.0));
      fl.icon_zoom = (std::min)(160, (std::max)(70, percent));
    }

    CollectVirtualItemPins(obj, id, &pins);
    state.fences.push_back(std::move(fl));
  }

  if (state.fences.empty()) {
    return false;
  }
  MigrateLegacySystemFences(&state.fences);

  // Keep existing display fingerprint if any.
  AppState prev = LoadAppStateFromPath(LayoutFilePath(), false);
  state.last_display_fingerprint = prev.last_display_fingerprint;
  if (state.current_page < 0 ||
      state.current_page >= static_cast<int>(state.page_names.size())) {
    state.current_page = 0;
  }

  if (!SaveAppState(state)) {
    return false;
  }
  if (!SavePins(pins)) {
    return false;
  }

  // R6: page_folders [{name, path}, ...] → native page_folders.json.
  const size_t pf_arr = FindArrayStart(json, "page_folders");
  if (pf_arr != std::string::npos) {
    std::vector<PageFolderItem> folders;
    size_t fp = pf_arr + 1;
    while (fp < json.size()) {
      while (fp < json.size() && json[fp] != '{' && json[fp] != ']') {
        ++fp;
      }
      if (fp >= json.size() || json[fp] == ']') {
        break;
      }
      size_t end = fp;
      const std::string fobj = ExtractObject(json, fp, &end);
      fp = end;
      std::string path;
      if (FindString(fobj, "path", &path) && !path.empty()) {
        std::string name;
        FindString(fobj, "name", &name);
        PageFolderItem it;
        it.path = WidenUtf8(path);
        it.name = name.empty() ? L"" : WidenUtf8(name);
        folders.push_back(std::move(it));
      }
    }
    if (!folders.empty()) {
      SavePageFolders(folders);
    }
  }

  // P1-30: also migrate prefs / hotkeys / pet.json (not only layout+pins).
  MigratePythonPrefs(json);
  // P2-14: organize_whitelist → whitelist.json
  {
    std::vector<std::string> wl;
    if (FindStringArray(json, "organize_whitelist", &wl) && !wl.empty()) {
      std::vector<std::wstring> names;
      names.reserve(wl.size());
      for (const auto& s : wl) {
        names.push_back(WidenUtf8(s));
      }
      SaveWhitelist(names);
    }
  }
  return true;
}

void MigratePythonPrefs(const std::string& json) {
  AppPrefs prefs = LoadAppPrefs();

  FindBool(json, "auto_organize_on_startup", &prefs.auto_organize_on_startup);
  FindBool(json, "auto_organize_watch", &prefs.auto_organize_watch);
  FindBool(json, "check_update_on_startup", &prefs.check_update_on_startup);
  FindBool(json, "show_fences", &prefs.show_fences);
  FindBool(json, "show_page_indicator", &prefs.show_page_indicator);
  FindBool(json, "double_click_hide", &prefs.double_click_hide);
  FindBool(json, "close_to_tray", &prefs.close_to_tray);
  FindBool(json, "close_behavior_prompted", &prefs.close_behavior_prompted);
  FindBool(json, "desktop_right_click_menu", &prefs.desktop_right_click_menu);
  FindBool(json, "enable_public_desktop", &prefs.enable_public_desktop);
  // Existing Python profiles: treat missing first-run keys as already done.
  if (!FindBool(json, "first_run_guide_done", &prefs.first_run_guide_done)) {
    prefs.first_run_guide_done = true;
  }
  if (!FindBool(json, "first_run_organize_done",
                &prefs.first_run_organize_done)) {
    prefs.first_run_organize_done = true;
  }

  std::string theme;
  if (FindString(json, "theme", &theme) && !theme.empty()) {
    if (theme == "mist" || theme == "graphite" || theme == "sky" ||
        theme == "mint" || theme == "lavender" || theme == "peach") {
      prefs.ui_theme = theme;
    }
  }
  std::string fence_preset;
  if (FindString(json, "fence_style_preset", &fence_preset) &&
      !fence_preset.empty()) {
    prefs.theme_id = NormalizeFenceStyleId(fence_preset);
  }
  std::string update_tag;
  if (FindString(json, "update_notified_tag", &update_tag)) {
    prefs.update_notified_tag = WidenUtf8(update_tag);
  }

  const std::string shot = FindNestedObject(json, "screenshot");
  if (!shot.empty()) {
    FindBool(shot, "auto_pin_after_copy", &prefs.screenshot_auto_pin);
    FindNumber(shot, "max_pins", &prefs.screenshot_max_pins);
  }

  const std::string rec = FindNestedObject(json, "screen_record");
  if (!rec.empty()) {
    FindBool(rec, "enabled", &prefs.record_enabled);
    FindNumber(rec, "fps", &prefs.record_fps);
    std::string out_dir;
    if (FindString(rec, "output_dir", &out_dir) && !out_dir.empty()) {
      prefs.record_output_dir = WidenUtf8(out_dir);
    }
    std::string mic;
    if (FindString(rec, "audio_device", &mic)) {
      const std::wstring norm = NormalizeRecordMic(WidenUtf8(mic));
      if (!norm.empty()) {
        prefs.record_mic = norm;
      }
    }
    std::string q;
    if (FindString(rec, "quality_preset", &q)) {
      if (q == "low") {
        prefs.record_quality = 0;
      } else if (q == "high") {
        prefs.record_quality = 2;
      } else {
        prefs.record_quality = 1;
      }
    }
  }

  const std::string search = FindNestedObject(json, "file_search");
  if (!search.empty()) {
    FindBool(search, "search_all_drives", &prefs.search_all_drives);
    FindBool(search, "include_open_folders", &prefs.search_include_open_folders);
    FindBool(search, "include_hidden", &prefs.search_include_hidden);
    FindNumber(search, "max_results", &prefs.search_max_results);
    FindNumber(search, "max_depth", &prefs.search_max_depth);
    FindNumber(search, "threads", &prefs.search_threads);
    std::vector<std::string> roots;
    if (FindStringArray(search, "roots", &roots)) {
      prefs.search_desktop = false;
      prefs.search_documents = false;
      prefs.search_downloads = false;
      for (const auto& r : roots) {
        if (r == "desktop") {
          prefs.search_desktop = true;
        } else if (r == "documents") {
          prefs.search_documents = true;
        } else if (r == "downloads") {
          prefs.search_downloads = true;
        }
      }
    }
  }

  const std::string notepad = FindNestedObject(json, "notepad");
  if (!notepad.empty()) {
    FindBool(notepad, "enabled", &prefs.notepad_enabled);
    std::string folder;
    if (FindString(notepad, "folder", &folder) && !folder.empty()) {
      prefs.notepad_folder = WidenUtf8(folder);
    }
  }
  const std::string minutes = FindNestedObject(json, "meeting_minutes");
  if (!minutes.empty()) {
    FindBool(minutes, "enabled", &prefs.minutes_enabled);
  }
  const std::string calc = FindNestedObject(json, "calculator");
  if (!calc.empty()) {
    FindBool(calc, "enabled", &prefs.calculator_enabled);
  }
  const std::string todos = FindNestedObject(json, "desktop_todos");
  if (!todos.empty()) {
    FindBool(todos, "enabled", &prefs.todos_enabled);
  }

  const std::string vault = FindNestedObject(json, "account_vault");
  if (!vault.empty()) {
    FindBool(vault, "enabled", &prefs.vault_enabled);
    std::string api;
    if (FindString(vault, "api_base", &api) && !api.empty()) {
      prefs.vault_api_base = WidenUtf8(api);
    }
    FindNumber(vault, "refresh_interval_hours", &prefs.vault_refresh_hours);
    FindNumber(vault, "clipboard_clear_seconds",
               &prefs.vault_clipboard_clear_seconds);
  }

  const std::string dock = FindNestedObject(json, "dock");
  if (!dock.empty()) {
    FindBool(dock, "enabled", &prefs.dock_visible);
    double opacity = 0.0;
    if (FindFloat(dock, "opacity", &opacity) && opacity > 0.0) {
      prefs.dock_opacity =
          (std::min)(100, (std::max)(50, static_cast<int>(opacity * 100.0)));
    }
    std::string pos;
    if (FindString(dock, "position", &pos)) {
      if (pos == "left") {
        prefs.dock_edge = 1;
      } else if (pos == "right") {
        prefs.dock_edge = 2;
      } else {
        prefs.dock_edge = 0;
      }
    }
    std::vector<std::string> items;
    if (FindStringArray(dock, "items", &items)) {
      prefs.dock_items.clear();
      for (const auto& it : items) {
        prefs.dock_items.push_back(WidenUtf8(it));
      }
    }
  }

  std::vector<std::string> excludes;
  if (FindStringArray(json, "exclude_patterns", &excludes)) {
    prefs.exclude_patterns.clear();
    for (const auto& e : excludes) {
      prefs.exclude_patterns.push_back(WidenUtf8(e));
    }
  }

  if (prefs.record_fps < 15 || prefs.record_fps > 60) {
    prefs.record_fps = 30;
  }
  if (prefs.screenshot_max_pins < 1) {
    prefs.screenshot_max_pins = 1;
  }
  if (prefs.screenshot_max_pins > 5) {
    prefs.screenshot_max_pins = 5;
  }
  SaveAppPrefs(prefs);

  // Hotkeys: Python `"hotkeys":{"organize":"Ctrl+Shift+O",...}` → hotkeys.json.
  const std::string hk = FindNestedObject(json, "hotkeys");
  if (!hk.empty()) {
    auto defs = DefaultHotkeys();
    for (auto& d : defs) {
      std::string chord;
      if (FindString(hk, d.id.c_str(), &chord) && !chord.empty()) {
        // Upgrade legacy Ctrl+Alt+F → Ctrl+Alt+4 (Python apply_file_search…).
        if (d.id == "file_search") {
          std::string lower = chord;
          for (char& c : lower) {
            if (c >= 'A' && c <= 'Z') {
              c = static_cast<char>(c - 'A' + 'a');
            }
          }
          if (lower == "ctrl+alt+f" || lower == "ctrl + alt + f") {
            chord = "Ctrl+Alt+4";
          }
        }
        UINT mods = 0, vk = 0;
        if (ParseHotkey(WidenUtf8(chord), &mods, &vk)) {
          d.modifiers = mods;
          d.vk = vk;
        }
      }
    }
    SaveHotkeys(defs);
  }

  // Pet: Python desktop_pet → pet.json (native PetWindow::PersistPrefs shape).
  const std::string pet = FindNestedObject(json, "desktop_pet");
  if (!pet.empty()) {
    bool auto_wander = true;
    bool show_bubbles = true;
    FindBool(pet, "auto_wander", &auto_wander);
    FindBool(pet, "show_page_bubbles", &show_bubbles);
    double scale = 1.0;
    FindFloat(pet, "size_scale", &scale);
    if (scale < 0.5 || scale > 2.0) {
      scale = 1.0;
    }
    std::string character = "hoodie";
    FindString(pet, "character", &character);
    if (character.empty()) {
      character = "hoodie";
    }
    auto join_acts = [](const std::vector<std::string>& acts) {
      std::string s;
      for (size_t i = 0; i < acts.size(); ++i) {
        if (i) {
          s += ',';
        }
        s += acts[i];
      }
      return s;
    };
    std::vector<std::string> hoodie_acts;
    std::vector<std::string> voyage_acts;
    std::vector<std::string> pixehood_acts;
    const std::string by_char =
        FindNestedObject(pet, "enabled_actions_by_character");
    if (!by_char.empty()) {
      FindStringArray(by_char, "hoodie", &hoodie_acts);
      FindStringArray(by_char, "voyage", &voyage_acts);
      FindStringArray(by_char, "pixehood", &pixehood_acts);
    } else {
      FindStringArray(pet, "enabled_actions", &hoodie_acts);
    }
    if (hoodie_acts.empty()) {
      hoodie_acts = {"pet", "wait", "sleep", "fall"};
    }
    if (voyage_acts.empty()) {
      voyage_acts = {"pet", "wait", "fall"};
    }
    if (pixehood_acts.empty()) {
      pixehood_acts = {"pet", "wait", "sleep", "fall"};
    }
    std::ofstream out(JoinPath(NativeDataDir(), L"pet.json"), std::ios::binary);
    if (out) {
      out << "{\n  \"character\":\"" << character << "\",\n  \"pose\":\"idle\","
          << "\n  \"auto_wander\": " << (auto_wander ? "true" : "false")
          << ",\n  \"show_page_bubbles\": " << (show_bubbles ? "true" : "false")
          << ",\n  \"size_scale\": " << scale
          << ",\n  \"pos_x\": 0,\n  \"pos_y\": 0"
          << ",\n  \"actions_hoodie\":\"" << join_acts(hoodie_acts)
          << "\",\n  \"actions_voyage\":\"" << join_acts(voyage_acts)
          << "\",\n  \"actions_pixehood\":\"" << join_acts(pixehood_acts)
          << "\"\n}\n";
    }
  }
}

}  // namespace

bool MigratePythonLayoutOnce() {
  if (FileExists(MarkerPath())) {
    return false;
  }
  const std::wstring src = PythonSettingsPath();
  if (src.empty() || !FileExists(src)) {
    // No Python install to migrate — mark done so we never look again.
    WriteMarker();
    return false;
  }
  std::ifstream in(src, std::ios::binary);
  if (!in) {
    WriteMarker();
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  if (json.size() < 32) {
    WriteMarker();
    return false;
  }
  const bool wrote = ParseAndWrite(json);
  WriteMarker();
  return wrote;
}
