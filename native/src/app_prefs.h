#pragma once

#include <string>

struct AppPrefs {
  bool auto_organize_on_startup = false;
  bool auto_organize_watch = false;
  // 0 = record until toggle/stop; >0 = ffmpeg -t seconds auto-stop.
  int record_seconds = 0;
  // 0 low, 1 medium, 2 high (libx264 CRF).
  int record_quality = 1;
  std::wstring record_output_dir;
  // DirectShow microphone name. Empty skips audio.
  std::wstring record_mic;
  // Fence style id applied as the app theme.
  std::string theme_id = "charcoal";
};

AppPrefs LoadAppPrefs();
void SaveAppPrefs(const AppPrefs& prefs);
