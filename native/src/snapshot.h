#pragma once

#include "layout.h"

#include <Windows.h>

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
  std::string style_id = "navy";
  bool collapsed = false;
  int icon_count = 0;  // pinned paths (virtual_items) in this fence
  // Small PNG data-URLs for in-fence pins (so thumbs show real icons).
  std::vector<std::string> pin_thumbs;
};

struct SnapshotPreviewIcon {
  int x = 0;
  int y = 0;
  // Optional PNG data-URL for free / shell icons on the schematic.
  std::string thumb;
};

struct SnapshotCardData {
  std::wstring name;
  std::wstring path;
  std::wstring meta;
  int focus_page = 0;
  // 快照保存时的工作区逻辑尺寸（用于放大预览 letterbox，避免扁分区竖向撑满）。
  int work_w = 0;
  int work_h = 0;
  // Page labels for preview pager (工作 / 文档 / …). Empty → derive from boxes.
  std::vector<std::wstring> page_names;
  std::vector<SnapshotPreviewBox> boxes;
  // Shell desktop icon positions from *.icons.json (schematic dots).
  std::vector<SnapshotPreviewIcon> icons;
};

std::wstring SnapshotDir();
// Default save name (today's timestamp, editable) — Python default_snapshot_name.
std::wstring DefaultSnapshotName();
bool SaveSnapshot(const AppState& state, const std::wstring& name);
std::vector<SnapshotInfo> ListSnapshots();
// Non-destructive summary for UI cards (does not touch live layout file).
std::wstring SnapshotBrief(const std::wstring& path);

// Bounds the thumbnail work BuildSnapshotCard performs. Each thumb is an icon
// extract + GDI+ PNG encode + base64, so a settings list over many snapshots
// used to re-encode every thumb on the UI thread (seconds of stall).
struct SnapshotCardOptions {
  // false → geometry / meta only; pin_thumbs and icon thumbs stay empty
  // (the settings card renders placeholders). Skips all PNG encoding.
  bool render_thumbs = true;
  // Per-fence pin thumbs (was a fixed 24) and free-float thumbs (was 40).
  // Lower these when the snapshot list is long.
  size_t max_pin_thumbs = 24;
  size_t max_free_thumbs = 40;
};

SnapshotCardData BuildSnapshotCard(
    const SnapshotInfo& info,
    const SnapshotCardOptions& opts = SnapshotCardOptions{});

// Same card as BuildSnapshotCard but memoized by snapshot mtime: reopening the
// settings window no longer re-encodes every thumbnail. Only cards built WITH
// thumbs are cached — placeholder builds are cheap enough to redo, and caching
// them would freeze a thumb-less card forever (mtime never changes).
SnapshotCardData BuildSnapshotCardCached(const SnapshotInfo& info,
                                        const SnapshotCardOptions& opts);
// Last-write time of a snapshot json as a single comparable value
// (0 when the file is missing). Used to pick the most recent snapshots.
ULONGLONG SnapshotMtimeQuad(const std::wstring& path);
bool LoadSnapshot(const std::wstring& path, AppState* out);
// Also capture Explorer icon positions alongside fence layout.
bool SaveSnapshotWithIcons(const AppState& state, const std::wstring& name);
// Load layout + pins only. Caller must ReloadFencesFromState then
// RestoreSnapshotShellIcons so RefreshDesktopIcons does not stomp positions.
bool LoadSnapshotWithIcons(const std::wstring& path, AppState* out);
bool RestoreSnapshotShellIcons(const std::wstring& path);
// Rename a snapshot (moves json + icons + pins sidecars). Returns false on
// invalid name or filesystem error. Name collisions get a numeric suffix.
bool RenameSnapshot(const std::wstring& path, const std::wstring& new_name);
bool DeleteSnapshot(const std::wstring& path);
