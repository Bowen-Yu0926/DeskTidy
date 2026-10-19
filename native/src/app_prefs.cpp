#include "app_prefs.h"

#include "paths_util.h"

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

int ParseIntField(const std::string& json, const char* key, int fallback) {
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
  if (p >= json.size() ||
      !(json[p] == '-' || (json[p] >= '0' && json[p] <= '9'))) {
    return fallback;
  }
  return std::stoi(json.substr(p));
}

std::string ParseStringField(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return {};
  }
  p = json.find('"', json.find(':', p) + 1);
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

std::string EscapeJson(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '"') {
      o.push_back('\\');
    }
    o.push_back(c);
  }
  return o;
}

}  // namespace

AppPrefs LoadAppPrefs() {
  AppPrefs prefs;
  std::ifstream in(PrefsPath());
  if (!in) {
    return prefs;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  prefs.auto_organize_on_startup =
      ParseBoolField(json, "auto_organize_on_startup", false);
  prefs.auto_organize_watch =
      ParseBoolField(json, "auto_organize_watch", false);
  prefs.record_seconds = ParseIntField(json, "record_seconds", 0);
  if (prefs.record_seconds < 0) {
    prefs.record_seconds = 0;
  }
  const std::string dir = ParseStringField(json, "record_output_dir");
  if (!dir.empty()) {
    prefs.record_output_dir = WidenUtf8(dir);
  }
  const std::string theme = ParseStringField(json, "theme_id");
  if (!theme.empty()) {
    prefs.theme_id = theme;
  }
  return prefs;
}

void SaveAppPrefs(const AppPrefs& prefs) {
  std::ofstream out(PrefsPath(), std::ios::binary);
  if (!out) {
    return;
  }
  out << "{\n  \"auto_organize_on_startup\": "
      << (prefs.auto_organize_on_startup ? "true" : "false")
      << ",\n  \"auto_organize_watch\": "
      << (prefs.auto_organize_watch ? "true" : "false")
      << ",\n  \"record_seconds\": " << prefs.record_seconds
      << ",\n  \"record_output_dir\": \""
      << EscapeJson(NarrowUtf8(prefs.record_output_dir))
      << "\",\n  \"theme_id\": \"" << EscapeJson(prefs.theme_id) << "\"\n}\n";
}
