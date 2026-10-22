#include "python_layout_sync.h"

#include "fence_style.h"
#include "layout.h"
#include "page_folders.h"
#include "paths_util.h"
#include "pin_store.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

bool ParseAndWrite(const std::string& json) {
  AppState state;
  state.dock_visible = false;
  state.pet_visible = true;
  FindNumber(json, "current_page", &state.current_page);
  FindBool(json, "pet_visible", &state.pet_visible);

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
  return true;
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
