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
};

using FloatPlacementMap = std::unordered_map<std::wstring, FloatPlacement>;

std::wstring FloatPlacementsPath();
FloatPlacementMap LoadFloatPlacements();
bool SaveFloatPlacements(const FloatPlacementMap& map);

void UpsertFloatPlacement(FloatPlacementMap* map, const std::wstring& path,
                          int page, POINT screen);
void SetFloatPage(FloatPlacementMap* map, const std::wstring& path, int page);
void EraseFloatPlacement(FloatPlacementMap* map, const std::wstring& path);
void RetargetFloatPlacement(FloatPlacementMap* map, const std::wstring& from,
                            const std::wstring& to);

// Screen point from stored ratios (work area under preferred point, else primary).
POINT FloatScreenPoint(const FloatPlacement& fp);
void FloatRatiosFromScreen(POINT screen, double* rx, double* ry);
