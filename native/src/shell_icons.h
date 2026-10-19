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
