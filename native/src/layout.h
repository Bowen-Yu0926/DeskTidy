#pragma once

#include <string>
#include <vector>

struct FenceLayout {
  std::string id;
  std::wstring title;
  int x = 100;
  int y = 40;
  int w = 280;
  int h = 360;
  int page = 0;
  std::string style_id = "charcoal";
};

struct AppState {
  std::vector<FenceLayout> fences;
  int current_page = 0;
  bool dock_visible = true;
  bool pet_visible = true;
};

std::wstring LayoutFilePath();
std::vector<FenceLayout> DefaultLayouts();
AppState LoadAppState();
bool SaveAppState(const AppState& state);
// Back-compat helpers
std::vector<FenceLayout> LoadLayouts();
bool SaveLayouts(const std::vector<FenceLayout>& layouts);
std::string MakeFenceId();
