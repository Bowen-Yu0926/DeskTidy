#pragma once

#include <Windows.h>

#include <string>
#include <vector>

struct FenceStylePreset {
  const char* id;
  const wchar_t* name;
  COLORREF background;
  COLORREF accent;
  COLORREF title;
  BYTE alpha;  // 0-255 layered alpha
};

const FenceStylePreset* FenceStyleById(const std::string& id);
const FenceStylePreset& DefaultFenceStyle();
std::vector<const FenceStylePreset*> AllFenceStyles();
std::string NormalizeFenceStyleId(const std::string& id);
