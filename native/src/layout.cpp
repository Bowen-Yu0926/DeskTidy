#include "layout.h"

#include "display_geom.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
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
    std::string fp;
    if (FindString(obj, "fp", &fp) && !fp.empty()) {
      continue;  // display-profile rows applied below
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
      const RECT work = PrimaryWorkArea();
      const int ww = (std::max)(1, static_cast<int>(work.right - work.left));
      const int wh = (std::max)(1, static_cast<int>(work.bottom - work.top));
      for (auto& f : state.fences) {
        const auto* r = find_row(last_fp, f.id);
        if (!r || r->rw < 0) {
          continue;
        }
        f.x = work.left + r->rx * ww / 10000;
        f.y = work.top + r->ry * wh / 10000;
        f.w = (std::max)(160, r->rw * ww / 10000);
        f.h = (std::max)(120, r->rh * wh / 10000);
      }
    }
  }
  return state;
}

bool SaveAppState(const AppState& state) {
  const std::wstring path = LayoutFilePath();
  std::string previous;
  {
    std::ifstream in(path);
    if (in) {
      std::ostringstream ss;
      ss << in.rdbuf();
      previous = ss.str();
    }
  }
  struct Kept {
    std::string fp, id;
    int x, y, w, h, rx, ry, rw, rh;
  };
  std::vector<Kept> kept;
  const std::string current = DisplayFingerprint();
  const RECT work = PrimaryWorkArea();
  const int ww = (std::max)(1, static_cast<int>(work.right - work.left));
  const int wh = (std::max)(1, static_cast<int>(work.bottom - work.top));

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
    if (!FindString(obj, "fp", &fp) || fp.empty() || fp == current) {
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

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  const std::string fp_out =
      FingerprintIsTransient(current) ? state.last_display_fingerprint : current;
  out << "{\n"
      << "  \"current_page\":" << state.current_page << ",\n"
      << "  \"dock_visible\":" << (state.dock_visible ? "true" : "false")
      << ",\n"
      << "  \"pet_visible\":" << (state.pet_visible ? "true" : "false") << ",\n"
      << "  \"last_display_fingerprint\":\"" << EscapeJson(fp_out) << "\",\n"
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
  if (!FingerprintIsTransient(current)) {
    for (const auto& f : state.fences) {
      const int rx = (f.x - work.left) * 10000 / ww;
      const int ry = (f.y - work.top) * 10000 / wh;
      const int rw = f.w * 10000 / ww;
      const int rh = f.h * 10000 / wh;
      write_row(current, f.id, f.x, f.y, f.w, f.h, rx, ry, rw, rh);
    }
  }
  out << "\n  ]\n}\n";
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
