#pragma once

#include <Windows.h>

#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Geometry primitives shared with fence_window / layout_editor (G1/G2).
// ---------------------------------------------------------------------------
struct LayoutPoint {
  int x = 0;
  int y = 0;
};

struct LayoutRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  int right() const { return x + w; }
  int bottom() const { return y + h; }
  int center_x() const { return x + w / 2; }
  int center_y() const { return y + h / 2; }

  bool Overlaps(const LayoutRect& o) const {
    return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
  }
};

// Per-page fence geometry / visibility (G3). Empty map on a FenceLayout means
// the top-level x/y/w/h applies to every page (old-format compatibility).
struct FencePageGeometry {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  bool visible = true;
  bool collapsed = false;
};

struct FenceLayout {
  std::string id;
  std::wstring title;
  int x = 100;
  int y = 40;
  int w = 280;
  int h = 360;
  int page = 0;
  // P2-11: multi-page membership (Python fence["pages"]). Empty → {page}.
  std::vector<int> pages;
  std::string style_id = "charcoal";
  std::wstring portal_path;
  // When true, window height is collapsed chrome; h still stores expanded height.
  bool collapsed = false;
  // User lock: drag/resize off (not system locked fences).
  bool position_locked = false;
  // name | date | size | type | manual
  std::string sort_by = "name";
  // grid | list
  std::string view_mode = "grid";
  // Panel alpha 0–255; -1 = inherit from style preset.
  int opacity = -1;
  // P2-12: optional overrides from Python style dict (-1 / empty = use preset).
  int radius = -1;
  bool show_title = true;
  bool collapsible = true;
  bool has_custom_colors = false;
  COLORREF custom_bg = 0;
  COLORREF custom_accent = 0;
  // Fence shown on its page(s) (Python fence["visible"]).
  bool visible = true;
  // Per-fence organize kinds (G4): "icon" (.lnk/.url/.exe) / "file" (文档).
  // Empty = legacy extension-rule resolution (Python get_fence_organize_kinds).
  std::vector<std::string> organize_kinds;
  // Per-page geometry/visibility (G3). Key = 0-based page id.
  std::map<int, FencePageGeometry> page_geometries;
  // Per-fence icon zoom percent (70..160). Persisted (H1).
  int icon_zoom = 100;
  // Manual icon order (paths) when sort_by == "manual" (H1).
  std::vector<std::wstring> pin_order;
};

struct AppState {
  std::vector<FenceLayout> fences;
  std::vector<std::wstring> page_names;
  // U7 page.locked: per-page lock flag (index = page id). System pages 0/1 are
  // always locked regardless (Python is_locked_page_id); this vector controls
  // pages >= 2 (Python desktop_pages[].locked). Empty/missing entries = false.
  std::vector<bool> page_locked;
  int current_page = 0;
  bool dock_visible = true;
  bool pet_visible = true;
  std::string last_display_fingerprint;
};

// U7: a page is locked when it is a system page (0/1) or its `locked` flag is
// set. Mirrors Python system_defaults.is_locked_page_id / is_locked_page.
bool IsPageLocked(const AppState& state, int page);

std::wstring LayoutFilePath();
std::vector<FenceLayout> DefaultLayouts();
// Product defaults match Python system_defaults: 工作 / 文档.
std::vector<std::wstring> DefaultPageNames();
// Extra pages use 页N (Python pet chrome fallback).
std::wstring NextPageName(size_t one_based_index);
// Migrate legacy「第 N 页」defaults → 工作/文档; pad to at least 2 names.
void NormalizePageNames(std::vector<std::wstring>* names);
// Remap old ids system_software / system_page2 → system_software_side / system_docs.
void MigrateLegacySystemFences(std::vector<FenceLayout>* fences);
AppState LoadAppState();
// Load any layout JSON (snapshots). When inject_defaults is false, missing /
// empty files stay empty — no synthetic page-2 fence.
AppState LoadAppStateFromPath(const std::wstring& path, bool inject_defaults = true);
bool SaveAppState(const AppState& state);
// Back-compat helpers
std::vector<FenceLayout> LoadLayouts();
bool SaveLayouts(const std::vector<FenceLayout>& layouts);
std::string MakeFenceId();
// Clamp / rescue fences that are off-screen or tiny. Returns how many changed.
int HealFenceLayouts(std::vector<FenceLayout>* fences);

// ---------------------------------------------------------------------------
// G1 — snap a moving fence to screen work-area edges (Python snap_geometry,
// EDGE_SNAP_THRESHOLD = 24). `others` are same-page sibling rects (reserved
// for fence-to-fence alignment; Python currently snaps screen edges only).
// Pure: no side effects; result is always clamped inside `work`.
// ---------------------------------------------------------------------------
LayoutPoint SnapFencePosition(const LayoutRect& moving,
                              const std::vector<LayoutRect>& others,
                              const LayoutRect& work, int snap_px);

// ---------------------------------------------------------------------------
// G2 — resolve same-page overlaps by dominant-axis push (Python
// apply_siblings_rejection "move" mode). Each rect is clamped to min_w/min_h
// first; rects processed in order push later rects off earlier ones. Pure.
// ---------------------------------------------------------------------------
void ResolveFenceCollisions(std::vector<LayoutRect>* rects, int min_w, int min_h);

// ---------------------------------------------------------------------------
// G3 — per-page fence geometry accessors.
// ---------------------------------------------------------------------------
// Geometry/visibility for `fence` on `page`; falls back to top-level x/y/w/h
// (visible=true, collapsed from the fence) when no per-page entry exists.
FencePageGeometry FenceGeometryForPage(const FenceLayout& fence, int page);
void SetFenceGeometryForPage(FenceLayout* fence, int page,
                             const FencePageGeometry& geo);
