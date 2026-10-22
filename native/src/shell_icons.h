#pragma once

#include <string>
#include <vector>

struct ShellIconPos {
  std::wstring name;
  int x = 0;
  int y = 0;
};

// Best-effort capture/restore of Explorer desktop icon positions (cross-process).
std::vector<ShellIconPos> CaptureShellIconPositions();
int RestoreShellIconPositions(const std::vector<ShellIconPos>& icons);
bool SaveShellIconsSidecar(const std::wstring& snapshot_json_path,
                           const std::vector<ShellIconPos>& icons);
std::vector<ShellIconPos> LoadShellIconsSidecar(
    const std::wstring& snapshot_json_path);

// Match Python win_shell.set_desktop_icons_visible — hide Explorer icons while
// fences host the same items so they do not double-draw on the wallpaper.
bool AreDesktopIconsVisible();
void SetDesktopIconsVisible(bool visible);
