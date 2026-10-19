#pragma once

#include <Windows.h>

#include <string>
#include <vector>

struct FenceStylePreset {
  const char* id;
  const wchar_t* name;
  COLORREF background;
  COLORREF accent;
  COLORREF title;   // title + caption ink
  BYTE alpha;       // panel fill alpha 0-255 (wallpaper shows through)
  int radius;       // corner radius (px @ 96 DPI)
  bool light;       // light panel → dark ink / darker border
};

const FenceStylePreset* FenceStyleById(const std::string& id);
const FenceStylePreset& DefaultFenceStyle();
std::vector<const FenceStylePreset*> AllFenceStyles();
std::string NormalizeFenceStyleId(const std::string& id);
