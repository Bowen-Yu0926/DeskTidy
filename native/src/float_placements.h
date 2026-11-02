#pragma once

#include <Windows.h>

#include <string>
#include <unordered_map>
#include <vector>

// Loose desktop float placements (Fences-class public icons): page + work-area
// ratios so unpin land / reorder survive restart and resolution changes.
struct FloatPlacement {
  std::wstring path;
  int page = 0;
  double rx = 0.05;
  double ry = 0.05;
  bool has_pos = false;
  // true = only show on `page` even when enable_public_desktop shares floats.
  // Set by「移动到分页」; new loose icons default false (shared when public on).
  bool page_local = false;
};

using FloatPlacementMap = std::unordered_map<std::wstring, FloatPlacement>;

std::wstring FloatPlacementsPath();
FloatPlacementMap LoadFloatPlacements();
bool SaveFloatPlacements(const FloatPlacementMap& map);

void UpsertFloatPlacement(FloatPlacementMap* map, const std::wstring& path,
                          int page, POINT screen);
// Move-to-page: set page and mark page_local so the float leaves other pages.
void SetFloatPage(FloatPlacementMap* map, const std::wstring& path, int page,
                  bool page_local = true);
void EraseFloatPlacement(FloatPlacementMap* map, const std::wstring& path);
void RetargetFloatPlacement(FloatPlacementMap* map, const std::wstring& from,
                            const std::wstring& to);

// Screen point from stored ratios (work area under preferred point, else primary).
POINT FloatScreenPoint(const FloatPlacement& fp);
void FloatRatiosFromScreen(POINT screen, double* rx, double* ry);
