#include "layout.h"

#include "display_geom.h"
#include "log.h"
#include "paths_util.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

std::string Narrow(const std::wstring& ws) {
  if (ws.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), n, nullptr,
                        nullptr);
  }
  return out;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  }
  return out;
}

std::string EscapeJson(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
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
      if (body[p] == 'n') {
        val.push_back('\n');
      } else if (body[p] == 'r') {
        val.push_back('\r');
      } else {
        val.push_back(body[p]);
      }
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

// Decode one JSON string starting at the opening quote index ``q``.
// Advances ``q`` past the closing quote. Mirrors FindString unescaping so
// Windows paths ("D:\\foo") do not accumulate backslashes across save cycles.
bool ParseJsonStringAt(const std::string& body, size_t* q, std::string* out) {
  if (*q >= body.size() || body[*q] != '"') {
    return false;
  }
  ++(*q);
  std::string val;
  while (*q < body.size() && body[*q] != '"') {
    if (body[*q] == '\\' && *q + 1 < body.size()) {
      ++(*q);
      if (body[*q] == 'n') {
        val.push_back('\n');
      } else if (body[*q] == 'r') {
        val.push_back('\r');
      } else if (body[*q] == 't') {
        val.push_back('\t');
      } else {
        // \\ \" \/ and any other escaped char → take the next literally.
        val.push_back(body[*q]);
      }
    } else {
      val.push_back(body[*q]);
    }
    ++(*q);
  }
  if (*q >= body.size() || body[*q] != '"') {
    return false;
  }
  ++(*q);  // closing quote
  *out = std::move(val);
  return true;
}

// Extract a JSON array of strings, e.g. "organize_kinds":["icon","file"].
bool FindStringArray(const std::string& body, const char* key,
                     std::vector<std::string>* out) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = body.find(needle);
  if (p == std::string::npos) {
    return false;
  }
  p = body.find('[', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  // Do not use find(']') — paths with escaped quotes / huge corrupt runs
  // make a naive scan wrong; walk tokens instead.
  size_t q = p + 1;
  while (q < body.size()) {
    while (q < body.size() &&
           (body[q] == ' ' || body[q] == '\t' || body[q] == '\n' ||
            body[q] == '\r' || body[q] == ',')) {
      ++q;
    }
    if (q >= body.size()) {
      break;
    }
    if (body[q] == ']') {
      return true;
    }
    if (body[q] != '"') {
      break;
    }
    std::string item;
    if (!ParseJsonStringAt(body, &q, &item)) {
      break;
    }
    out->push_back(std::move(item));
  }
  // True when ']' reached (including empty []); false only on parse error.
  return q < body.size() && body[q] == ']';
}

}  // namespace

std::wstring LayoutFilePath() {
  // U4 portable + single source of truth: route through NativeDataDir() so
  // layout.json follows the resolved data root (portable.txt / --portable).
  return JoinPath(NativeDataDir(), L"layout.json");
}

std::vector<std::wstring> DefaultPageNames() {
  return {L"工作", L"文档"};
}

std::wstring NextPageName(size_t one_based_index) {
  if (one_based_index == 0) {
    one_based_index = 1;
  }
  return L"页" + std::to_wstring(one_based_index);
}

void NormalizePageNames(std::vector<std::wstring>* names) {
  if (!names) {
    return;
  }
  auto is_legacy = [](const std::wstring& n, int i) {
    const std::wstring a = L"第 " + std::to_wstring(i + 1) + L" 页";
    const std::wstring b = L"第" + std::to_wstring(i + 1) + L"页";
    return n == a || n == b;
  };
  // Old native default pair → product names (Python 工作/文档).
  if (names->size() >= 2 && is_legacy((*names)[0], 0) &&
      is_legacy((*names)[1], 1)) {
    (*names)[0] = L"工作";
    (*names)[1] = L"文档";
  }
  if (names->empty()) {
    *names = DefaultPageNames();
  } else if (names->size() == 1) {
    if (is_legacy((*names)[0], 0)) {
      (*names)[0] = L"工作";
    }
    names->push_back(L"文档");
  }
}

std::vector<FenceLayout> DefaultLayouts() {
  // Fences-class: 工作页「常用」与「其他」采用均衡适中的网格列宽（约 1.25:1 比例，两侧均工整对齐单元格）；
  // 底部留出呼吸边距，高度自适应容纳 2~3 行完整图标，避免底部切断。文档页「文档」铺满工作区。
  const RECT work = PrimaryWorkArea();
  const int aw = (std::max)(320, static_cast<int>(work.right - work.left));
  const int ah = (std::max)(240, static_cast<int>(work.bottom - work.top));
  const int dpi = SystemDpi();
  const int gap = MulDiv(10, dpi, 96);
  const int margin_x = MulDiv(10, dpi, 96);
  const int margin_y = MulDiv(8, dpi, 96);
  const int pad = MulDiv(10, dpi, 96);

  const IconGridMetrics grid = QueryIconGridMetrics();
  const int cell_w = (std::max)(64, grid.cell_w);
  const int cell_h = (std::max)(64, grid.cell_h);

  const int usable_w = (std::max)(280, aw - margin_x * 2 - gap);
  const int total_cols = (std::max)(6, (usable_w - pad * 4) / cell_w);

  // 比例约 1.25:1（常用分配 ~56% 列数，其他分配 ~44% 列数），确保两侧均有足够列宽容纳图标
  const int common_cols = (std::max)(3, (total_cols * 56 + 50) / 100);
  const int side_cols = (std::max)(3, total_cols - common_cols);

  int left_w = pad * 2 + common_cols * cell_w;
  int right_w = usable_w - left_w;
  if (right_w < pad * 2 + side_cols * cell_w) {
    right_w = pad * 2 + side_cols * cell_w;
  }
  if (left_w + gap + right_w > aw - margin_x * 2) {
    right_w = (std::max)(pad * 2 + 2 * cell_w, aw - margin_x * 2 - gap - left_w);
  }

  // 高度：自适应容纳完整图标行数 + header + pad + footer，绝不切断半个图标
  const int target_rows = (ah >= 860) ? 3 : 2;
  const int content_h = MinFenceHeightForIconRows(target_rows, &grid);
  const int content_min = MinFenceHeightForIconRows(2, &grid);
  const int row_h = (std::min)(ah * 48 / 100, (std::max)(content_min, content_h));

  std::vector<FenceLayout> out;
  FenceLayout common;
  common.id = "system_common";
  common.title = L"常用";
  common.x = work.left + margin_x;
  common.y = work.top + margin_y;
  common.w = left_w;
  common.h = row_h;
  common.page = 0;
  common.organize_kinds = {"icon"};
  out.push_back(common);

  FenceLayout side;
  side.id = "system_software_side";
  side.title = L"其他";
  side.x = work.left + margin_x + left_w + gap;
  side.y = work.top + margin_y;
  side.w = right_w;
  side.h = row_h;
  side.page = 0;
  out.push_back(side);

  FenceLayout docs;
  docs.id = "system_docs";
  docs.title = L"文档";
  docs.x = work.left;
  docs.y = work.top;
  docs.w = aw;
  docs.h = ah;  // 铺满工作区（宽+高）
  docs.page = 1;
  docs.organize_kinds = {"file"};
  out.push_back(docs);
  return out;
}

void MigrateLegacySystemFences(std::vector<FenceLayout>* fences) {
  if (!fences) {
    return;
  }
  for (auto& f : *fences) {
    if (f.id == "system_software" || f.id == "system_software_side") {
      f.id = "system_software_side";
      if (f.title == L"软件" || f.title.empty()) {
        f.title = L"其他";
      }
      f.page = 0;
      f.pages = {0};
      // Product default:「其他」不参与一键整理（empty kinds）. A prior bug in
      // EnsureOrganizeFencesForEmptyPages forced {"file"} onto this fence
      // whenever page 0 lacked a file zone — clear that mistaken heal.
      if (f.organize_kinds.size() == 1 && f.organize_kinds[0] == "file") {
        f.organize_kinds.clear();
      }
    } else if (f.id == "system_page2" || f.id == "system_docs") {
      f.id = "system_docs";
      if (f.title == L"第二页" || f.title.empty()) {
        f.title = L"文档";
      }
      f.page = 1;
      f.pages = {1};
      // Python 导入后 organize_kinds 常为空 → 一键整理退回扩展名规则，
      // 文档会被钉到「常用」。空 kinds 时按默认补「文档」规则。
      if (f.organize_kinds.empty()) {
        f.organize_kinds = {"file"};
      }
    } else if (f.id == "system_common") {
      if (f.title.empty()) {
        f.title = L"常用";
      }
      f.page = 0;
      f.pages = {0};
      if (f.organize_kinds.empty()) {
        f.organize_kinds = {"icon"};
      }
    }
  }
}

std::string MakeFenceId() {
  static int seq = 0;
  ++seq;
  char buf[64];
  sprintf_s(buf, "fence_%lu_%d", GetTickCount(), seq);
  return buf;
}

bool IsPageLocked(const AppState& state, int page) {
  // System pages 0/1 are always locked (Python is_locked_page_id).
  if (page == 0 || page == 1) {
    return true;
  }
  if (page < 0 || page >= static_cast<int>(state.page_locked.size())) {
    return false;
  }
  return state.page_locked[static_cast<size_t>(page)];
}

// Hard cap: a healthy layout.json is a few KB. A runaway EscapeJson /
// FindStringArray bug once wrote ~2GB of backslashes — never mmap that again.
constexpr std::uintmax_t kMaxLayoutJsonBytes = 8ull * 1024ull * 1024ull;

// Read a layout JSON, falling back to the .bak copy when the main file is
// missing, empty, oversized, or truncated (no brace pair). Returns false only
// when both copies are unusable.
bool ReadLayoutJson(const std::wstring& path, std::string* out) {
  for (const std::wstring& candidate : {path, path + L".bak"}) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &fad)) {
      continue;
    }
    const ULARGE_INTEGER sz{fad.nFileSizeLow, fad.nFileSizeHigh};
    if (sz.QuadPart == 0 || sz.QuadPart > kMaxLayoutJsonBytes) {
      LogLine(L"[layout] skip oversized/empty %s (%llu bytes)",
              candidate.c_str(),
              static_cast<unsigned long long>(sz.QuadPart));
      // Quarantine runaway main file so the next save cannot promote it to .bak.
      if (sz.QuadPart > kMaxLayoutJsonBytes && candidate == path) {
        const std::wstring bad = path + L".corrupt";
        DeleteFileW(bad.c_str());
        MoveFileExW(path.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING);
        LogLine(L"[layout] quarantined oversized main → %s", bad.c_str());
      }
      continue;
    }
    std::ifstream in(candidate, std::ios::binary);
    if (!in) {
      continue;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    if (s.find('{') != std::string::npos && s.find('}') != std::string::npos) {
      *out = s;
      return true;
    }
  }
  return false;
}

AppState LoadAppStateFromPath(const std::wstring& path, bool inject_defaults) {
  AppState state;
  std::string json;
  if (!ReadLayoutJson(path, &json)) {
    // U2: corrupt/missing layout — log, then fall back to defaults. The next
    // SaveAppState will rewrite a good file (the bad one is kept as .bak there).
    LogLine(L"[layout] layout.json unreadable or truncated — using defaults");
    if (inject_defaults) {
      state.fences = DefaultLayouts();
    }
    return state;
  }
  FindNumber(json, "current_page", &state.current_page);
  FindBool(json, "dock_visible", &state.dock_visible);
  FindBool(json, "pet_visible", &state.pet_visible);
  if (state.current_page < 0) {
    state.current_page = 0;
  }

  size_t pos = 0;
  while (true) {
    const size_t start = json.find('{', pos);
    if (start == std::string::npos) {
      break;
    }
    const size_t end = json.find('}', start);
    if (end == std::string::npos) {
      break;
    }
    const std::string obj = json.substr(start, end - start + 1);
    pos = end + 1;

    std::string id;
    std::string title;
    int x = 0, y = 0, w = 0, h = 0, page = 0;
    std::string style_id;
    if (!FindString(obj, "id", &id)) {
      continue;
    }
    // Skip root object fields mistaken as fences
    if (id == "current_page" || id.empty()) {
      continue;
    }
    std::string fp;
    if (FindString(obj, "fp", &fp) && !fp.empty()) {
      continue;  // display-profile rows applied below
    }
    if (obj.find("\"pg\"") != std::string::npos) {
      continue;  // per-page geometry rows applied below
    }
    FindString(obj, "title", &title);
    FindNumber(obj, "x", &x);
    FindNumber(obj, "y", &y);
    FindNumber(obj, "w", &w);
    FindNumber(obj, "h", &h);
    FindNumber(obj, "page", &page);
    FindString(obj, "style", &style_id);
    std::string portal;
    FindString(obj, "portal", &portal);
    bool collapsed = false;
    FindBool(obj, "collapsed", &collapsed);
    bool position_locked = false;
    FindBool(obj, "position_locked", &position_locked);
    std::string sort_by;
    FindString(obj, "sort_by", &sort_by);
    std::string view_mode;
    FindString(obj, "view_mode", &view_mode);
    int opacity = -1;
    FindNumber(obj, "opacity", &opacity);
    bool visible = true;
    FindBool(obj, "visible", &visible);
    std::vector<std::string> organize_kinds;
    FindStringArray(obj, "organize_kinds", &organize_kinds);
    std::vector<int> fence_pages;
    {
      const size_t arr = obj.find("\"pages\"");
      if (arr != std::string::npos) {
        size_t lb = obj.find('[', arr);
        size_t rb = obj.find(']', lb == std::string::npos ? 0 : lb);
        if (lb != std::string::npos && rb != std::string::npos && rb > lb) {
          size_t p = lb + 1;
          while (p < rb) {
            while (p < rb && (obj[p] == ' ' || obj[p] == ',' || obj[p] == '\t' ||
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
            fence_pages.push_back(v);
          }
        }
      }
    }
    int radius = -1;
    FindNumber(obj, "radius", &radius);
    bool show_title = true;
    FindBool(obj, "show_title", &show_title);
    bool collapsible = true;
    FindBool(obj, "collapsible", &collapsible);
    bool has_custom = false;
    FindBool(obj, "has_custom_colors", &has_custom);
    int custom_bg = 0;
    int custom_accent = 0;
    FindNumber(obj, "custom_bg", &custom_bg);
    FindNumber(obj, "custom_accent", &custom_accent);
    if (w < kFenceMinW) {
      w = kFenceDefaultW;
    }
    if (h < kFenceMinH) {
      h = (std::max)(kFenceExpandedFloor, MinFenceHeightForIconRows(2));
    }
    FenceLayout fl;
    fl.id = id;
    fl.title = Widen(title.empty() ? id : title);
    fl.x = x;
    fl.y = y;
    fl.w = w;
    fl.h = h;
    fl.page = page;
    if (fence_pages.empty()) {
      fl.pages = {page};
    } else {
      fl.pages = fence_pages;
      // Prefer the explicit "page" when it is a member; only fall back to
      // pages[0] when the singleton/list disagrees (stale pages after move).
      bool page_in_list = false;
      for (int p : fl.pages) {
        if (p == page) {
          page_in_list = true;
          break;
        }
      }
      if (!page_in_list) {
        fl.page = fl.pages[0];
      }
    }
    fl.style_id = style_id.empty() ? "navy" : style_id;
    fl.portal_path = Widen(portal);
    fl.collapsed = collapsed;
    fl.position_locked = position_locked;
    fl.sort_by = sort_by.empty() ? "name" : sort_by;
    fl.view_mode = (view_mode == "list") ? "list" : "grid";
    if (opacity >= 0 && opacity <= 255) {
      fl.opacity = opacity;
    }
    fl.radius = radius;
    fl.show_title = show_title;
    fl.collapsible = collapsible;
    fl.has_custom_colors = has_custom;
    fl.custom_bg = static_cast<COLORREF>(custom_bg);
    fl.custom_accent = static_cast<COLORREF>(custom_accent);
    fl.visible = visible;
    fl.organize_kinds = organize_kinds;
    int icon_zoom = 100;
    if (FindNumber(obj, "icon_zoom", &icon_zoom)) {
      fl.icon_zoom = (std::min)(160, (std::max)(70, icon_zoom));
    }
    std::vector<std::string> pin_order;
    FindStringArray(obj, "pin_order", &pin_order);
    for (const auto& p : pin_order) {
      fl.pin_order.push_back(NormalizeStoredPath(Widen(p)));
    }
    state.fences.push_back(fl);
  }
  if (state.fences.empty()) {
    if (inject_defaults) {
      state.fences = DefaultLayouts();
    }
  } else if (inject_defaults) {
    bool has_page2 = false;
    for (const auto& f : state.fences) {
      if (f.page == 1 || f.id == "system_page2") {
        has_page2 = true;
        break;
      }
    }
    if (!has_page2) {
      FenceLayout page2;
      page2.id = "system_page2";
      page2.title = L"第二页";
      page2.x = 80;
      page2.y = 80;
      page2.w = kFenceDefaultW + 80;
      page2.h = (std::max)(kFenceExpandedFloor, MinFenceHeightForIconRows(2));
      page2.page = 1;
      state.fences.push_back(page2);
    }
  }

  struct ProfileRow {
    std::string fp;
    std::string id;
    int x = 0, y = 0, w = 0, h = 0;
    int rx = -1, ry = -1, rw = -1, rh = -1;
  };
  std::vector<ProfileRow> profiles;
  pos = 0;
  while (true) {
    const size_t start = json.find('{', pos);
    if (start == std::string::npos) {
      break;
    }
    const size_t end = json.find('}', start);
    if (end == std::string::npos) {
      break;
    }
    const std::string obj = json.substr(start, end - start + 1);
    pos = end + 1;
    ProfileRow row;
    if (!FindString(obj, "fp", &row.fp) || row.fp.empty()) {
      continue;
    }
    if (!FindString(obj, "id", &row.id)) {
      continue;
    }
    FindNumber(obj, "x", &row.x);
    FindNumber(obj, "y", &row.y);
    FindNumber(obj, "w", &row.w);
    FindNumber(obj, "h", &row.h);
    FindNumber(obj, "rx", &row.rx);
    FindNumber(obj, "ry", &row.ry);
    FindNumber(obj, "rw", &row.rw);
    FindNumber(obj, "rh", &row.rh);
    profiles.push_back(row);
  }

  // Per-page geometry rows (G3): {"pg":true,"id":...,"page":...,"x":...,...}.
  struct PageGeoRow {
    std::string id;
    int page = 0;
    int x = 0, y = 0, w = 0, h = 0;
    bool visible = true;
    bool collapsed = false;
  };
  std::vector<PageGeoRow> page_rows;
  pos = 0;
  while (true) {
    const size_t start = json.find('{', pos);
    if (start == std::string::npos) {
      break;
    }
    const size_t end = json.find('}', start);
    if (end == std::string::npos) {
      break;
    }
    const std::string obj = json.substr(start, end - start + 1);
    pos = end + 1;
    if (obj.find("\"pg\"") == std::string::npos) {
      continue;
    }
    PageGeoRow row;
    if (!FindString(obj, "id", &row.id) || row.id.empty()) {
      continue;
    }
    FindNumber(obj, "page", &row.page);
    FindNumber(obj, "x", &row.x);
    FindNumber(obj, "y", &row.y);
    FindNumber(obj, "w", &row.w);
    FindNumber(obj, "h", &row.h);
    FindBool(obj, "visible", &row.visible);
    FindBool(obj, "collapsed", &row.collapsed);
    page_rows.push_back(row);
  }
  for (const auto& row : page_rows) {
    for (auto& f : state.fences) {
      if (f.id != row.id) {
        continue;
      }
      FencePageGeometry geo;
      geo.x = row.x;
      geo.y = row.y;
      geo.w = row.w;
      geo.h = row.h;
      geo.visible = row.visible;
      geo.collapsed = row.collapsed;
      f.page_geometries[row.page] = geo;
      break;
    }
  }

  std::string last_fp;
  FindString(json, "last_display_fingerprint", &last_fp);
  state.last_display_fingerprint = last_fp;
  const std::string current = DisplayFingerprint();
  auto find_row = [&](const std::string& fp, const std::string& id) -> const ProfileRow* {
    for (const auto& r : profiles) {
      if (r.fp == fp && r.id == id) {
        return &r;
      }
    }
    return nullptr;
  };
  if (!FingerprintIsTransient(current)) {
    bool any = false;
    for (auto& f : state.fences) {
      if (const auto* r = find_row(current, f.id)) {
        f.x = r->x;
        f.y = r->y;
        f.w = r->w;
        f.h = r->h;
        any = true;
      }
    }
    if (!any && !last_fp.empty() && last_fp != current) {
      const IconGridMetrics grid = QueryIconGridMetrics();
      const int content_min = MinFenceHeightForIconRows(2, &grid);
      for (auto& f : state.fences) {
        const auto* r = find_row(last_fp, f.id);
        if (!r || r->rw < 0) {
          continue;
        }
        // U3: pick the target monitor from the stored absolute position so the
        // fence lands on the matching screen (Python per-monitor work area),
        // not the primary monitor.
        POINT stored{r->x, r->y};
        const RECT work = WorkAreaAtPoint(stored);
        const int ww = (std::max)(1, static_cast<int>(work.right - work.left));
        const int wh = (std::max)(1, static_cast<int>(work.bottom - work.top));
        f.x = work.left + r->rx * ww / 10000;
        f.y = work.top + r->ry * wh / 10000;
        // Cap ratios: rh>10000 means taller than the work area (corrupt persist).
        const int rw = (std::min)(10000, (std::max)(0, r->rw));
        const int rh = (std::min)(8500, (std::max)(0, r->rh));
        f.w = (std::max)(kFenceMinW, rw * ww / 10000);
        f.h = (std::max)(content_min, rh * wh / 10000);
        if (f.h > wh) {
          f.h = wh;
        }
      }
    }
  }
  // G3: page_geometries is the last user resize on that page. Profiles / fences[]
  // can lag after automatic proportion "heal" wiped top-level sizes — promote
  // home-page overrides so reboot matches what the user left on screen.
  // Cross-fingerprint: skip absolute promote when page_geo cannot fit the
  // current work area (would stomp donor ratios with old-machine pixels).
  PromoteHomePageGeometries(&state.fences);
  if (inject_defaults) {
    HealFenceLayouts(&state.fences);
  }
  const auto pages_key = json.find("\"pages\"");
  if (pages_key != std::string::npos) {
    const auto lb = json.find('[', pages_key);
    const auto rb = lb == std::string::npos ? std::string::npos : json.find(']', lb);
    if (lb != std::string::npos && rb != std::string::npos) {
      const std::string arr = json.substr(lb, rb - lb + 1);
      size_t q = 0;
      while (true) {
        const size_t a = arr.find('"', q);
        if (a == std::string::npos) {
          break;
        }
        size_t b = a + 1;
        std::string name;
        while (b < arr.size() && arr[b] != '"') {
          if (arr[b] == '\\' && b + 1 < arr.size()) {
            ++b;
          }
          name.push_back(arr[b++]);
        }
        if (!name.empty()) {
          state.page_names.push_back(Widen(name));
        }
        q = b + 1;
      }
    }
  }
  // U7 page.locked: parse the per-page lock flags array (index = page id).
  {
    const auto lock_key = json.find("\"page_locked\"");
    if (lock_key != std::string::npos) {
      const auto lb = json.find('[', lock_key);
      const auto rb = lb == std::string::npos ? std::string::npos
                                              : json.find(']', lb);
      if (lb != std::string::npos && rb != std::string::npos) {
        for (size_t p = lb + 1; p < rb;) {
          if (json.compare(p, 4, "true") == 0) {
            state.page_locked.push_back(true);
            p += 4;
          } else if (json.compare(p, 5, "false") == 0) {
            state.page_locked.push_back(false);
            p += 5;
          } else {
            ++p;
          }
        }
      }
    }
  }
  if (inject_defaults) {
    NormalizePageNames(&state.page_names);
  }
  int max_page = 0;
  for (const auto& f : state.fences) {
    if (f.page > max_page) {
      max_page = f.page;
    }
  }
  while (inject_defaults &&
         static_cast<int>(state.page_names.size()) <= max_page) {
    state.page_names.push_back(NextPageName(state.page_names.size() + 1));
  }
  if (state.current_page < 0 ||
      (!state.page_names.empty() &&
       state.current_page >= static_cast<int>(state.page_names.size()))) {
    state.current_page = 0;
  }
  if (inject_defaults) {
    MigrateLegacySystemFences(&state.fences);
  }
  return state;
}

AppState LoadAppState() {
  return LoadAppStateFromPath(LayoutFilePath(), true);
}

bool SaveAppState(const AppState& state) {
  const std::wstring path = LayoutFilePath();
  std::string previous;
  {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
      const ULARGE_INTEGER sz{fad.nFileSizeLow, fad.nFileSizeHigh};
      // Never re-ingest a runaway layout as profile donor / .bak source.
      if (sz.QuadPart > 0 && sz.QuadPart <= kMaxLayoutJsonBytes) {
        std::ifstream in(path, std::ios::binary);
        if (in) {
          std::ostringstream ss;
          ss << in.rdbuf();
          previous = ss.str();
        }
      }
    }
  }
  struct Kept {
    std::string fp, id;
    int x, y, w, h, rx, ry, rw, rh;
  };
  std::vector<Kept> kept;
  const std::string live = DisplayFingerprint();
  // PersistLayouts passes display_fp_ via last_display_fingerprint. On
  // WM_DISPLAYCHANGE that is still the OLD topology while live already moved —
  // write the profile row under the old key so donor restore can find it
  // (Fences: remember geometry on the outgoing display before switching).
  std::string profile_fp = state.last_display_fingerprint;
  if (profile_fp.empty() || FingerprintIsTransient(profile_fp)) {
    profile_fp = live;
  }

  size_t pos = 0;
  while (!previous.empty()) {
    const size_t start = previous.find('{', pos);
    if (start == std::string::npos) {
      break;
    }
    const size_t end = previous.find('}', start);
    if (end == std::string::npos) {
      break;
    }
    const std::string obj = previous.substr(start, end - start + 1);
    pos = end + 1;
    std::string fp, id;
    // U3: drop transient / stale fingerprints (Python purge_transient_display_profiles)
    // so plug/unplug intermediates never survive as donor profiles.
    // Also drop rows we are about to rewrite (profile_fp and live).
    if (!FindString(obj, "fp", &fp) || fp.empty() || fp == profile_fp ||
        fp == live || FingerprintIsTransient(fp)) {
      continue;
    }
    if (!FindString(obj, "id", &id)) {
      continue;
    }
    Kept k{};
    k.fp = fp;
    k.id = id;
    FindNumber(obj, "x", &k.x);
    FindNumber(obj, "y", &k.y);
    FindNumber(obj, "w", &k.w);
    FindNumber(obj, "h", &k.h);
    FindNumber(obj, "rx", &k.rx);
    FindNumber(obj, "ry", &k.ry);
    FindNumber(obj, "rw", &k.rw);
    FindNumber(obj, "rh", &k.rh);
    kept.push_back(k);
  }

  std::ostringstream out;
  const std::string fp_out = profile_fp;
  out << "{\n"
      << "  \"current_page\":" << state.current_page << ",\n"
      << "  \"dock_visible\":" << (state.dock_visible ? "true" : "false")
      << ",\n"
      << "  \"pet_visible\":" << (state.pet_visible ? "true" : "false") << ",\n"
      << "  \"last_display_fingerprint\":\"" << EscapeJson(fp_out) << "\",\n"
      << "  \"pages\": [";
  std::vector<std::wstring> pages = state.page_names;
  NormalizePageNames(&pages);
  for (size_t i = 0; i < pages.size(); ++i) {
    if (i) {
      out << ", ";
    }
    out << "\"" << EscapeJson(Narrow(pages[i])) << "\"";
  }
  out << "],\n  \"page_locked\": [";
  for (size_t i = 0; i < pages.size(); ++i) {
    if (i) {
      out << ", ";
    }
    // U7: system pages 0/1 always locked; others persist their flag.
    bool locked = (i == 0 || i == 1);
    if (!locked && i < state.page_locked.size()) {
      locked = state.page_locked[i];
    }
    out << (locked ? "true" : "false");
  }
  out << "],\n  \"fences\": [\n";
  for (size_t i = 0; i < state.fences.size(); ++i) {
    const auto& f = state.fences[i];
    out << "    {"
        << "\"id\":\"" << EscapeJson(f.id) << "\","
        << "\"title\":\"" << EscapeJson(Narrow(f.title)) << "\","
        << "\"x\":" << f.x << ","
        << "\"y\":" << f.y << ","
        << "\"w\":" << f.w << ","
        << "\"h\":" << f.h << ","
        << "\"page\":" << f.page << ","
        << "\"pages\":[";
    {
      const auto& pg =
          f.pages.empty() ? std::vector<int>{f.page} : f.pages;
      for (size_t pi = 0; pi < pg.size(); ++pi) {
        if (pi) {
          out << ",";
        }
        out << pg[pi];
      }
    }
    out << "],"
        << "\"style\":\""
        << EscapeJson(f.style_id.empty() ? "navy" : f.style_id) << "\","
        << "\"portal\":\"" << EscapeJson(Narrow(f.portal_path)) << "\","
        << "\"collapsed\":" << (f.collapsed ? "true" : "false") << ","
        << "\"position_locked\":"
        << (f.position_locked ? "true" : "false") << ","
        << "\"sort_by\":\""
        << EscapeJson(f.sort_by.empty() ? "name" : f.sort_by) << "\","
        << "\"view_mode\":\""
        << EscapeJson(f.view_mode == "list" ? "list" : "grid") << "\","
        << "\"visible\":" << (f.visible ? "true" : "false") << ","
        << "\"opacity\":" << f.opacity << ","
        << "\"radius\":" << f.radius << ","
        << "\"show_title\":" << (f.show_title ? "true" : "false") << ","
        << "\"collapsible\":" << (f.collapsible ? "true" : "false") << ","
        << "\"has_custom_colors\":"
        << (f.has_custom_colors ? "true" : "false") << ","
        << "\"custom_bg\":" << static_cast<unsigned>(f.custom_bg) << ","
        << "\"custom_accent\":" << static_cast<unsigned>(f.custom_accent)
        << ","
        << "\"organize_kinds\":[";
    for (size_t k = 0; k < f.organize_kinds.size(); ++k) {
      if (k) {
        out << ",";
      }
      out << "\"" << EscapeJson(f.organize_kinds[k]) << "\"";
    }
    out << "],"
        << "\"icon_zoom\":" << f.icon_zoom << ","
        << "\"pin_order\":[";
    for (size_t k = 0; k < f.pin_order.size(); ++k) {
      if (k) {
        out << ",";
      }
      out << "\""
          << EscapeJson(Narrow(NormalizeStoredPath(f.pin_order[k]))) << "\"";
    }
    out << "]}";
    if (i + 1 < state.fences.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n  \"profiles\": [\n";
  bool first = true;
  auto write_row = [&](const std::string& fp, const std::string& id, int x, int y,
                       int w, int h, int rx, int ry, int rw, int rh) {
    if (!first) {
      out << ",\n";
    }
    first = false;
    out << "    {\"fp\":\"" << EscapeJson(fp) << "\",\"id\":\"" << EscapeJson(id)
        << "\",\"x\":" << x << ",\"y\":" << y << ",\"w\":" << w << ",\"h\":" << h
        << ",\"rx\":" << rx << ",\"ry\":" << ry << ",\"rw\":" << rw
        << ",\"rh\":" << rh << "}";
  };
  for (const auto& k : kept) {
    write_row(k.fp, k.id, k.x, k.y, k.w, k.h, k.rx, k.ry, k.rw, k.rh);
  }
  if (!FingerprintIsTransient(profile_fp)) {
    for (const auto& f : state.fences) {
      // U3: ratios are relative to the fence's own monitor work area (Python
      // _target_screen_rect / _ratios_for), not the primary work area. This
      // keeps each fence anchored to its display across hot-plug changes.
      const POINT c{f.x + f.w / 2, f.y + f.h / 2};
      const RECT wa = WorkAreaAtPoint(c);
      const int ww = (std::max)(1, static_cast<int>(wa.right - wa.left));
      const int wh = (std::max)(1, static_cast<int>(wa.bottom - wa.top));
      const int rx = (f.x - wa.left) * 10000 / ww;
      const int ry = (f.y - wa.top) * 10000 / wh;
      const int rw = f.w * 10000 / ww;
      const int rh = f.h * 10000 / wh;
      write_row(profile_fp, f.id, f.x, f.y, f.w, f.h, rx, ry, rw, rh);
    }
  }
  out << "\n  ],\n  \"page_geometries\": [";
  bool first_pg = true;
  for (const auto& f : state.fences) {
    for (const auto& kv : f.page_geometries) {
      const FencePageGeometry& g = kv.second;
      if (!first_pg) {
        out << ",";
      }
      first_pg = false;
      out << "\n    {\"pg\":true,\"id\":\"" << EscapeJson(f.id) << "\","
          << "\"page\":" << kv.first << ","
          << "\"x\":" << g.x << ",\"y\":" << g.y << ",\"w\":" << g.w
          << ",\"h\":" << g.h << ","
          << "\"visible\":" << (g.visible ? "true" : "false") << ","
          << "\"collapsed\":" << (g.collapsed ? "true" : "false") << "}";
    }
  }
  out << "\n  ]\n}\n";
  const std::string json = out.str();

  // U2: atomic write — temp + flush, preserve a single .bak of the previous good
  // file, then atomic replace (same policy as app_prefs).
  const std::wstring tmp = path + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    LogLine(L"[layout] save failed: cannot create temp file %s", tmp.c_str());
    return false;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(h, json.data(), static_cast<DWORD>(json.size()),
                            &written, nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok || written != json.size()) {
    DeleteFileW(tmp.c_str());
    LogLine(L"[layout] save failed: temp write short (%lu != %zu)", written,
            json.size());
    return false;
  }
  // Only promote a previous *good* layout to .bak — never a runaway 2GB file.
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
      !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    const ULARGE_INTEGER sz{fad.nFileSizeLow, fad.nFileSizeHigh};
    if (sz.QuadPart > 0 && sz.QuadPart <= kMaxLayoutJsonBytes) {
      CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
    } else if (sz.QuadPart > kMaxLayoutJsonBytes) {
      const std::wstring bad = path + L".corrupt";
      DeleteFileW(bad.c_str());
      MoveFileExW(path.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING);
      LogLine(L"[layout] quarantined oversized before save → %s", bad.c_str());
    }
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp.c_str());
    LogLine(L"[layout] save failed: MoveFileExW replace");
    return false;
  }
  return true;
}

std::vector<FenceLayout> LoadLayouts() {
  return LoadAppState().fences;
}

bool SaveLayouts(const std::vector<FenceLayout>& layouts) {
  AppState state = LoadAppState();
  state.fences = layouts;
  return SaveAppState(state);
}

std::wstring NormalizeStoredPath(const std::wstring& path) {
  if (path.empty()) {
    return path;
  }
  std::wstring out;
  out.reserve(path.size());
  size_t i = 0;
  // Preserve UNC \\server\share prefix.
  if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
    out.push_back(L'\\');
    out.push_back(L'\\');
    i = 2;
  }
  for (; i < path.size(); ++i) {
    const wchar_t c = path[i];
    if (c == L'\\' || c == L'/') {
      if (out.empty() || out.back() != L'\\') {
        out.push_back(L'\\');
      }
      while (i + 1 < path.size() &&
             (path[i + 1] == L'\\' || path[i + 1] == L'/')) {
        ++i;
      }
    } else {
      out.push_back(c);
    }
  }
  return out;
}

void PromoteHomePageGeometries(std::vector<FenceLayout>* fences) {
  if (!fences) {
    return;
  }
  const RECT virt = VirtualScreenRect();
  for (auto& f : *fences) {
    const auto it = f.page_geometries.find(f.page);
    if (it == f.page_geometries.end()) {
      continue;
    }
    const FencePageGeometry& g = it->second;
    if (g.w < 120 || g.h < 80) {
      continue;
    }
    // Absolute page_geometries are not fingerprint-keyed. After a resolution /
    // monitor change they can be larger than the current work area or sit off
    // the virtual desktop — promoting them undoes donor ratio restore (Fences
    // keeps per-display sizes; never overlay another machine's pixels).
    POINT mid{g.x + g.w / 2, g.y + g.h / 2};
    const RECT work = WorkAreaAtPoint(mid);
    const int ww = (std::max)(1, static_cast<int>(work.right - work.left));
    const int wh = (std::max)(1, static_cast<int>(work.bottom - work.top));
    if (g.w > ww + 48 || g.h > wh + 48) {
      continue;
    }
    // Skip only corrupt / off-topology absolutes — keep intentional tall (~80%).
    if (g.h > wh || g.h * 100 >= wh * 95) {
      continue;
    }
    const RECT rc{g.x, g.y, g.x + g.w, g.y + g.h};
    if (rc.right < virt.left + 40 || rc.bottom < virt.top + 40 ||
        rc.left > virt.right - 40 || rc.top > virt.bottom - 40) {
      continue;
    }
    f.x = g.x;
    f.y = g.y;
    f.w = g.w;
    f.h = g.h;
    f.collapsed = g.collapsed;
    f.visible = g.visible;
  }
}

void SyncPageGeometryFromTopLevel(FenceLayout* fence, int page) {
  if (!fence) {
    return;
  }
  auto it = fence->page_geometries.find(page);
  if (it == fence->page_geometries.end()) {
    return;
  }
  it->second.x = fence->x;
  it->second.y = fence->y;
  it->second.w = fence->w;
  it->second.h = fence->h;
  it->second.collapsed = fence->collapsed;
  it->second.visible = fence->visible;
}

int RestoreSystemFenceProportions(std::vector<FenceLayout>* fences) {
  if (!fences || fences->empty()) {
    return 0;
  }
  FenceLayout* common = nullptr;
  FenceLayout* side = nullptr;
  FenceLayout* docs = nullptr;
  for (auto& f : *fences) {
    if (f.id == "system_common") {
      common = &f;
    } else if (f.id == "system_software_side") {
      side = &f;
    } else if (f.id == "system_docs") {
      docs = &f;
    }
  }
  if (!common || !side) {
    return 0;
  }
  const auto defaults = DefaultLayouts();
  FenceLayout def_common = defaults[0];
  FenceLayout def_side = defaults[1];
  FenceLayout def_docs = defaults.size() > 2 ? defaults[2] : FenceLayout{};
  // Skewed when far from balanced layout (常用 ate the row >78% / 其他 vanished <22%).
  const int row_w =
      (std::max)(1, common->w + side->w +
                        (std::max)(0, side->x - (common->x + common->w)));
  const bool skewed =
      common->w * 100 >= row_w * 78 || side->w * 100 <= row_w * 22;
  const RECT work = PrimaryWorkArea();
  const int ah = (std::max)(1, static_cast<int>(work.bottom - work.top));
  // Full-bleed height from display-scale corruption — same restore as skew.
  const bool stretched =
      common->h * 100 >= ah * 75 || side->h * 100 >= ah * 75;
  int changed = 0;
  if (skewed || stretched) {
    const FenceLayout before_c = *common;
    const FenceLayout before_s = *side;
    common->x = def_common.x;
    common->y = def_common.y;
    common->w = def_common.w;
    common->h = def_common.h;
    side->x = def_side.x;
    side->y = def_side.y;
    side->w = def_side.w;
    side->h = def_side.h;
    if (before_c.x != common->x || before_c.y != common->y ||
        before_c.w != common->w || before_c.h != common->h) {
      SyncPageGeometryFromTopLevel(common, common->page);
      ++changed;
    }
    if (before_s.x != side->x || before_s.y != side->y ||
        before_s.w != side->w || before_s.h != side->h) {
      SyncPageGeometryFromTopLevel(side, side->page);
      ++changed;
    }
  }
  if (docs && def_docs.w > 0) {
    // Docs default is full work area. Restore when collapsed short, or when
    // taller than the work area (hotplug corruption) — do not punish intentional
    // mid-size user layouts.
    const int aw = (std::max)(1, static_cast<int>(work.right - work.left));
    if ((docs->w * 100 >= aw * 90 && docs->h + 40 < def_docs.h) ||
        docs->h > ah) {
      docs->x = def_docs.x;
      docs->y = def_docs.y;
      docs->w = def_docs.w;
      docs->h = def_docs.h;
      SyncPageGeometryFromTopLevel(docs, docs->page);
      ++changed;
    }
  }
  return changed;
}

int HealFenceLayouts(std::vector<FenceLayout>* fences) {
  if (!fences || fences->empty()) {
    return 0;
  }
  // Do not call RestoreSystemFenceProportions here — that wiped user-resized
  // 常用/其他 rows on every boot (side <18% of row looked "skewed").
  int changed = 0;
  const IconGridMetrics grid = QueryIconGridMetrics();
  const int content_min = MinFenceHeightForIconRows(2, &grid);
  const int kMinH = (std::max)(kFenceExpandedFloor, content_min);
  const RECT virt = VirtualScreenRect();
  const RECT primary = PrimaryWorkArea();
  const auto defaults = DefaultLayouts();
  int cascade = 0;
  for (auto& f : *fences) {
    FenceLayout before = f;
    if (f.w < kFenceMinW) {
      f.w = (std::max)(kFenceMinW, kFenceDefaultW);
    }
    if (f.h < kMinH) {
      f.h = kMinH;
    }
    POINT mid{f.x + f.w / 2, f.y + f.h / 2};
    const RECT work = WorkAreaAtPoint(mid);
    const int wh = (std::max)(1, static_cast<int>(work.bottom - work.top));
    // Only heal *corrupt* heights (hotplug ×10 / virtual bleed). Intentional
    // tall user fences (~80% work) must survive (Fences keeps user size).
    const bool taller_than_work = f.h > wh;
    const bool near_full_bleed = f.h * 100 >= wh * 95;
    const bool absurd_abs = f.h >= 2000 || f.h > wh * 2;
    if (taller_than_work || near_full_bleed || absurd_abs) {
      bool restored = false;
      for (const auto& d : defaults) {
        if (d.id == f.id && d.h >= kMinH) {
          f.h = d.h;
          restored = true;
          break;
        }
      }
      if (!restored) {
        f.h = (std::max)(kMinH, wh * 40 / 100);
      }
      if (f.y + f.h > work.bottom) {
        f.y = work.top;
      }
    }
    RECT rc{f.x, f.y, f.x + f.w, f.y + f.h};
    const bool mostly_off =
        rc.right < virt.left + 40 || rc.bottom < virt.top + 40 ||
        rc.left > virt.right - 40 || rc.top > virt.bottom - 40;
    if (mostly_off) {
      // Rescue onto the nearest work area (fallback: primary).
      POINT pt{primary.left + 80, primary.top + 80};
      const RECT rescue = WorkAreaAtPoint(pt);
      f.x = rescue.left + 40 + cascade * 28;
      f.y = rescue.top + 40 + cascade * 28;
      ++cascade;
      RECT placed{f.x, f.y, f.x + f.w, f.y + f.h};
      ClampRectToWorkArea(&placed);
      f.x = placed.left;
      f.y = placed.top;
      f.w = placed.right - placed.left;
      f.h = placed.bottom - placed.top;
    } else {
      ClampRectToWorkArea(&rc);
      f.x = rc.left;
      f.y = rc.top;
      f.w = rc.right - rc.left;
      f.h = rc.bottom - rc.top;
    }
    // Keep G3 page overrides from re-promoting corrupt heights on next load.
    for (auto& kv : f.page_geometries) {
      if (kv.second.h > f.h &&
          (kv.second.h > wh || kv.second.h * 100 >= wh * 95 ||
           kv.second.h >= 2000)) {
        kv.second.h = f.h;
      }
    }
    if (f.x != before.x || f.y != before.y || f.w != before.w ||
        f.h != before.h) {
      SyncPageGeometryFromTopLevel(&f, f.page);
      ++changed;
    }
  }
  // Heavy pairwise overlap on the same page → cascade later fence right.
  for (size_t i = 0; i < fences->size(); ++i) {
    for (size_t j = i + 1; j < fences->size(); ++j) {
      auto& a = (*fences)[i];
      auto& b = (*fences)[j];
      if (a.page != b.page) {
        continue;
      }
      const int ax2 = a.x + a.w;
      const int ay2 = a.y + a.h;
      const int bx2 = b.x + b.w;
      const int by2 = b.y + b.h;
      const int ix = (std::max)(0, (std::min)(ax2, bx2) - (std::max)(a.x, b.x));
      const int iy = (std::max)(0, (std::min)(ay2, by2) - (std::max)(a.y, b.y));
      const int inter = ix * iy;
      if (inter <= 0) {
        continue;
      }
      const int smaller = (std::min)(a.w * a.h, b.w * b.h);
      if (smaller <= 0 || inter * 2 < smaller) {
        continue;
      }
      FenceLayout before = b;
      b.x = a.x + a.w + 16;
      RECT rc{b.x, b.y, b.x + b.w, b.y + b.h};
      ClampRectToWorkArea(&rc);
      b.x = rc.left;
      b.y = rc.top;
      b.w = rc.right - rc.left;
      b.h = rc.bottom - rc.top;
      if (b.x != before.x || b.y != before.y || b.w != before.w ||
          b.h != before.h) {
        SyncPageGeometryFromTopLevel(&b, b.page);
        ++changed;
      }
    }
  }
  return changed;
}

bool PrepareDestinationProfileForDisplayChange(const RECT& old_work,
                                               const RECT& new_work) {
  // Fences-class: size restore is fingerprint profile / donor ratios in
  // LoadAppState — never unary-scale every fence by primary work delta (that
  // stretched secondary-monitor fences and ×10'd heights on hotplug stubs).
  (void)old_work;
  (void)new_work;
  AppState state = LoadAppState();
  if (state.fences.empty()) {
    return false;
  }
  const int n = HealFenceLayouts(&state.fences);
  // Align G3 absolutes with healed top-level so the next promote cannot
  // resurrect pre-switch pixels.
  for (auto& f : state.fences) {
    SyncPageGeometryFromTopLevel(&f, f.page);
  }
  if (n <= 0) {
    // Still rewrite page_geometries sync even when heal was a no-op.
    return SaveAppState(state);
  }
  LogLine(L"[layout] display converge healed %d fence(s) (no primary scale)", n);
  return SaveAppState(state);
}

// ---------------------------------------------------------------------------
// G1 / G2 / G3 — pure geometry (see layout.h).
// ---------------------------------------------------------------------------
namespace {
int AbsInt(int v) { return v < 0 ? -v : v; }
}  // namespace

LayoutPoint SnapFencePosition(const LayoutRect& moving,
                              const std::vector<LayoutRect>& others,
                              const LayoutRect& work, int snap_px) {
  const int threshold = snap_px > 0 ? snap_px : 24;
  const int top_threshold = (std::min)(8, threshold);

  int x = moving.x;
  int y = moving.y;
  int w = moving.w < 1 ? 1 : moving.w;
  int h = moving.h < 1 ? 1 : moving.h;

  // 1) Screen work-area edges (Python snap_geometry). Top is tighter.
  if (AbsInt(x - work.x) <= threshold) {
    x = work.x;
  } else if (AbsInt((x + w) - work.right()) <= threshold) {
    x = work.right() - w;
  }
  if (AbsInt(y - work.y) <= top_threshold) {
    y = work.y;
  } else if (AbsInt((y + h) - work.bottom()) <= threshold) {
    y = work.bottom() - h;
  }

  // 2) Sibling edge / abut snap (Fences-class fence↔fence align). Pick the
  // closest candidate within threshold so multiple neighbors don't fight.
  {
    int best_dx = threshold + 1;
    int snap_x = x;
    auto try_x = [&](int nx) {
      const int d = AbsInt(nx - x);
      if (d <= threshold && d < best_dx) {
        best_dx = d;
        snap_x = nx;
      }
    };
    int best_dy = threshold + 1;
    int snap_y = y;
    auto try_y = [&](int ny) {
      const int d = AbsInt(ny - y);
      if (d <= threshold && d < best_dy) {
        best_dy = d;
        snap_y = ny;
      }
    };
    for (const auto& o : others) {
      if (o.w < 1 || o.h < 1) {
        continue;
      }
      try_x(o.x);            // left ↔ left
      try_x(o.right() - w);  // right ↔ right
      try_x(o.right());      // left abut other's right
      try_x(o.x - w);        // right abut other's left
      try_y(o.y);            // top ↔ top
      try_y(o.bottom() - h); // bottom ↔ bottom
      try_y(o.bottom());     // top abut other's bottom
      try_y(o.y - h);        // bottom abut other's top
    }
    if (best_dx <= threshold) {
      x = snap_x;
    }
    if (best_dy <= threshold) {
      y = snap_y;
    }
  }

  // 3) Always keep the result fully inside the work area.
  if (w > work.w) {
    w = work.w;
  }
  if (h > work.h) {
    h = work.h;
  }
  if (x < work.x) {
    x = work.x;
  }
  if (y < work.y) {
    y = work.y;
  }
  if (x + w > work.right()) {
    x = work.right() - w;
  }
  if (y + h > work.bottom()) {
    y = work.bottom() - h;
  }

  LayoutPoint out;
  out.x = x;
  out.y = y;
  return out;
}

void ResolveFenceCollisions(std::vector<LayoutRect>* rects, int min_w, int min_h) {
  if (!rects || rects->empty()) {
    return;
  }
  const int mw = min_w > 0 ? min_w : kFenceMinW;
  const int mh = min_h > 0 ? min_h : kFenceMinH;

  for (auto& r : *rects) {
    if (r.w < mw) {
      r.w = mw;
    }
    if (r.h < mh) {
      r.h = mh;
    }
  }

  // Python apply_siblings_rejection "move": at most 2 passes; push a later
  // rect off an earlier one along the dominant (center-delta) axis.
  for (int pass = 0; pass < 2; ++pass) {
    bool touched = false;
    for (size_t i = 0; i < rects->size(); ++i) {
      for (size_t j = i + 1; j < rects->size(); ++j) {
        LayoutRect& a = (*rects)[i];
        LayoutRect& b = (*rects)[j];
        if (!a.Overlaps(b)) {
          continue;
        }
        const int dx = a.center_x() - b.center_x();
        const int dy = a.center_y() - b.center_y();
        if (AbsInt(dx) >= AbsInt(dy)) {
          b.x = (dx >= 0) ? (a.x - 1 - b.w) : (a.right() + 1);
        } else {
          b.y = (dy >= 0) ? (a.y - 1 - b.h) : (a.bottom() + 1);
        }
        touched = true;
      }
    }
    if (!touched) {
      break;
    }
  }
}

FencePageGeometry FenceGeometryForPage(const FenceLayout& fence, int page) {
  const auto it = fence.page_geometries.find(page);
  if (it != fence.page_geometries.end()) {
    return it->second;
  }
  FencePageGeometry out;
  out.x = fence.x;
  out.y = fence.y;
  out.w = fence.w;
  out.h = fence.h;
  out.visible = fence.visible;
  out.collapsed = fence.collapsed;
  return out;
}

void SetFenceGeometryForPage(FenceLayout* fence, int page,
                             const FencePageGeometry& geo) {
  if (!fence) {
    return;
  }
  fence->page_geometries[page] = geo;
}
