#include "layout.h"

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
  const int n =
      MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
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

// Minimal extractor: "key": number  or "key": "string"
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
  const int screen_w = GetSystemMetrics(SM_CXSCREEN);
  std::vector<FenceLayout> out;
  FenceLayout common;
  common.id = "system_common";
  common.title = L"常用";
  common.x = 20;
  common.y = 40;
  common.w = 520;
  common.h = 300;
  out.push_back(common);

  FenceLayout soft;
  soft.id = "system_software";
  soft.title = L"软件";
  soft.w = 280;
  soft.h = 360;
  soft.x = screen_w - soft.w - 40;
  soft.y = 40;
  out.push_back(soft);
  return out;
}

std::string MakeFenceId() {
  static int seq = 0;
  ++seq;
  char buf[64];
  sprintf_s(buf, "fence_%lu_%d", GetTickCount(), seq);
  return buf;
}

std::vector<FenceLayout> LoadLayouts() {
  const std::wstring path = LayoutFilePath();
  std::ifstream in(path);
  if (!in) {
    return DefaultLayouts();
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  if (json.find("\"fences\"") == std::string::npos) {
    return DefaultLayouts();
  }

  std::vector<FenceLayout> out;
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
    int x = 0, y = 0, w = 0, h = 0;
    if (!FindString(obj, "id", &id)) {
      continue;
    }
    FindString(obj, "title", &title);
    FindNumber(obj, "x", &x);
    FindNumber(obj, "y", &y);
    FindNumber(obj, "w", &w);
    FindNumber(obj, "h", &h);
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
    out.push_back(fl);
  }
  if (out.empty()) {
    return DefaultLayouts();
  }
  return out;
}

bool SaveLayouts(const std::vector<FenceLayout>& layouts) {
  const std::wstring path = LayoutFilePath();
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "{\n  \"fences\": [\n";
  for (size_t i = 0; i < layouts.size(); ++i) {
    const auto& f = layouts[i];
    out << "    {"
        << "\"id\":\"" << EscapeJson(f.id) << "\","
        << "\"title\":\"" << EscapeJson(Narrow(f.title)) << "\","
        << "\"x\":" << f.x << ","
        << "\"y\":" << f.y << ","
        << "\"w\":" << f.w << ","
        << "\"h\":" << f.h << "}";
    if (i + 1 < layouts.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
  return true;
}
