#include "fence_style.h"

namespace {

// Match src/fence_style.py FENCE_STYLE_PRESETS (opacity → alpha, radius 10).
constexpr FenceStylePreset kPresets[] = {
    {"charcoal", L"深空灰", RGB(0x2A, 0x2E, 0x35), RGB(0x4C, 0x8D, 0xFF),
     RGB(0xFF, 0xFF, 0xFF), 178, 10, false},
    {"slate", L"岩灰", RGB(0x3D, 0x44, 0x50), RGB(0x7D, 0xD3, 0xFC),
     RGB(0xFF, 0xFF, 0xFF), 168, 10, false},
    {"frost", L"霜透白", RGB(0xF4, 0xF6, 0xF8), RGB(0x3B, 0x82, 0xF6),
     RGB(0x0F, 0x17, 0x2A), 133, 10, true},
    {"mist_blue", L"晴空蓝", RGB(0xC5, 0xE1, 0xF5), RGB(0x1D, 0x7B, 0xB8),
     RGB(0x0F, 0x17, 0x2A), 140, 10, true},
    {"forest", L"清新绿", RGB(0xC8, 0xEB, 0xD6), RGB(0x1E, 0x9B, 0x55),
     RGB(0x0F, 0x17, 0x2A), 140, 10, true},
    {"sunset", L"暖沙", RGB(0xF8, 0xD5, 0xB5), RGB(0xE0, 0x7A, 0x2F),
     RGB(0x0F, 0x17, 0x2A), 140, 10, true},
};

}  // namespace

const FenceStylePreset& DefaultFenceStyle() { return kPresets[0]; }

std::string NormalizeFenceStyleId(const std::string& id) {
  for (const auto& p : kPresets) {
    if (id == p.id) {
      return id;
    }
  }
  return DefaultFenceStyle().id;
}

const FenceStylePreset* FenceStyleById(const std::string& id) {
  const std::string key = NormalizeFenceStyleId(id);
  for (const auto& p : kPresets) {
    if (key == p.id) {
      return &p;
    }
  }
  return &DefaultFenceStyle();
}

std::vector<const FenceStylePreset*> AllFenceStyles() {
  std::vector<const FenceStylePreset*> out;
  for (const auto& p : kPresets) {
    out.push_back(&p);
  }
  return out;
}
