#pragma once

#include "layout.h"

#include <string>
#include <vector>

struct SnapshotInfo {
  std::wstring name;
  std::wstring path;
};

struct SnapshotPreviewBox {
  std::wstring title;
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  int page = 0;
  std::string style_id = "charcoal";
  bool collapsed = false;
  int icon_count = 0;  // pinned paths (virtual_items) in this fence
};

struct SnapshotCardData {
  std::wstring name;
  std::wstring path;
  std::wstring meta;
  int focus_page = 0;
  std::vector<SnapshotPreviewBox> boxes;
};

std::wstring SnapshotDir();
// Default save name (today's timestamp, editable) — Python default_snapshot_name.
std::wstring DefaultSnapshotName();
bool SaveSnapshot(const AppState& state, const std::wstring& name);
std::vector<SnapshotInfo> ListSnapshots();
// Non-destructive summary for UI cards (does not touch live layout file).
std::wstring SnapshotBrief(const std::wstring& path);
SnapshotCardData BuildSnapshotCard(const SnapshotInfo& info);
bool LoadSnapshot(const std::wstring& path, AppState* out);
// Also capture/restore Explorer icon positions alongside fence layout.
bool SaveSnapshotWithIcons(const AppState& state, const std::wstring& name);
bool LoadSnapshotWithIcons(const std::wstring& path, AppState* out);
// Rename a snapshot (moves json + icons + pins sidecars). Returns false on
// invalid name or filesystem error. Name collisions get a numeric suffix.
bool RenameSnapshot(const std::wstring& path, const std::wstring& new_name);
bool DeleteSnapshot(const std::wstring& path);
