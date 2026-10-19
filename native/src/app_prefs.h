#pragma once

struct AppPrefs {
  bool auto_organize_on_startup = false;
  bool auto_organize_watch = false;
};

AppPrefs LoadAppPrefs();
void SaveAppPrefs(const AppPrefs& prefs);
