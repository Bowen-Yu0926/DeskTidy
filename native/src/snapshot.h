#pragma once

#include "layout.h"

#include <string>
#include <vector>

struct SnapshotInfo {
  std::wstring name;
  std::wstring path;
};

std::wstring SnapshotDir();
bool SaveSnapshot(const AppState& state, const std::wstring& name);
std::vector<SnapshotInfo> ListSnapshots();
bool LoadSnapshot(const std::wstring& path, AppState* out);
// Also capture/restore Explorer icon positions alongside fence layout.
bool SaveSnapshotWithIcons(const AppState& state, const std::wstring& name);
bool LoadSnapshotWithIcons(const std::wstring& path, AppState* out);
