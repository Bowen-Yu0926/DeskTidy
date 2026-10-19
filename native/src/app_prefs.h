#pragma once

#include <string>

struct AppPrefs {
  bool auto_organize_on_startup = false;
  bool auto_organize_watch = false;
  // 0 = record until toggle/stop; >0 = ffmpeg -t seconds auto-stop.
  int record_seconds = 0;
  std::wstring record_output_dir;
};

AppPrefs LoadAppPrefs();
void SaveAppPrefs(const AppPrefs& prefs);
