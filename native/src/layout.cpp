#include "layout.h"

#include "display_geom.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstdio>
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

}  // namespace

std::wstring LayoutFilePath() {
  wchar_t* appdata = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata)) ||
      !appdata) {
    return L"layout.json";
  }
  std::wstring dir = appdata;
  CoTaskMemFree(appdata);
  dir += L"\\DeskTidyNative";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\layout.json";
}

std::vector<FenceLayout> DefaultLayouts() {
  const RECT work = PrimaryWorkArea();
  std::vector<FenceLayout> out;
  FenceLayout common;
  common.id = "system_common";
  common.title = L"常用";
  common.x = work.left + 20;
  common.y = work.top + 40;
  common.w = 520;
  common.h = 300;
  common.page = 0;
  out.push_back(common);

  FenceLayout soft;
  soft.id = "system_software";
  soft.title = L"软件";
  soft.w = 280;
  soft.h = 360;
  soft.x = work.right - soft.w - 40;
  soft.y = work.top + 40;
  soft.page = 0;
  out.push_back(soft);

  FenceLayout page2;
  page2.id = "system_page2";
  page2.title = L"第二页";
  page2.x = 80;
  page2.y = 80;
  page2.w = 360;
  page2.h = 280;
  page2.page = 1;
  out.push_back(page2);
  return out;
}

std::string MakeFenceId() {
  static int seq = 0;
  ++seq;
  char buf[64];
  sprintf_s(buf, "fence_%lu_%d", GetTickCount(), seq);
  return buf;
}

AppState LoadAppState() {
  AppState state;
  const std::wstring path = LayoutFilePath();
  std::ifstream in(path);
  if (!in) {
    state.fences = DefaultLayouts();
    return state;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
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
    FindString(obj, "title", &title);
    FindNumber(obj, "x", &x);
    FindNumber(obj, "y", &y);
    FindNumber(obj, "w", &w);
    FindNumber(obj, "h", &h);
    FindNumber(obj, "page", &page);
    FindString(obj, "style", &style_id);
    if (w < 120) {
      w = 280;
    }
    if (h < 80) {
      h = 200;
    }
    FenceLayout fl;
    fl.id = id;
    fl.title = Widen(title.empty() ? id : title);
    fl.x = x;
    fl.y = y;
    fl.w = w;
    fl.h = h;
    fl.page = page;
    fl.style_id = style_id.empty() ? "charcoal" : style_id;
    state.fences.push_back(fl);
  }
  if (state.fences.empty()) {
    state.fences = DefaultLayouts();
  } else {
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
      page2.w = 360;
      page2.h = 280;
      page2.page = 1;
      state.fences.push_back(page2);
    }
  }
  return state;
}

bool SaveAppState(const AppState& state) {
  const std::wstring path = LayoutFilePath();
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "{\n"
      << "  \"current_page\":" << state.current_page << ",\n"
      << "  \"dock_visible\":" << (state.dock_visible ? "true" : "false")
      << ",\n"
      << "  \"pet_visible\":" << (state.pet_visible ? "true" : "false") << ",\n"
      << "  \"fences\": [\n";
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
        << "\"style\":\""
        << EscapeJson(f.style_id.empty() ? "charcoal" : f.style_id) << "\"}";
    if (i + 1 < state.fences.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
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
