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
};

std::wstring LayoutFilePath();
std::vector<FenceLayout> DefaultLayouts();
std::vector<FenceLayout> LoadLayouts();
bool SaveLayouts(const std::vector<FenceLayout>& layouts);
std::string MakeFenceId();
