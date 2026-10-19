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
      << (prefs.auto_organize_watch ? "true" : "false") << "\n}\n";
}
