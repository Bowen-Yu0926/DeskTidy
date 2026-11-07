#include "fence_style.h"

namespace {

// 10 curated presets across mainstream & cyberpunk desktop-organizer looks.
constexpr FenceStylePreset kPresets[] = {
    // ── 科技/深色系 ──
    {"navy", L"墨夜蓝", RGB(0x20, 0x26, 0x33), RGB(0x4C, 0xC2, 0xFF),
     RGB(0xFF, 0xFF, 0xFF), 185, 12, false},
    {"cyberpunk", L"赛博霓虹", RGB(0x10, 0x14, 0x22), RGB(0x00, 0xF0, 0xFF),
     RGB(0xFF, 0xFF, 0xFF), 190, 14, false},
    {"aurora", L"极光幽夜", RGB(0x11, 0x20, 0x1D), RGB(0x00, 0xFF, 0xAB),
     RGB(0xFF, 0xFF, 0xFF), 185, 14, false},
    {"sunset", L"落霞霓光", RGB(0x24, 0x14, 0x22), RGB(0xFF, 0x5C, 0x8A),
     RGB(0xFF, 0xFF, 0xFF), 188, 14, false},
    {"obsidian", L"黑曜暗晶", RGB(0x13, 0x15, 0x1C), RGB(0x94, 0xA3, 0xB8),
     RGB(0xFF, 0xFF, 0xFF), 195, 14, false},
    {"graphite", L"石墨灰", RGB(0x2E, 0x30, 0x36), RGB(0x64, 0xA0, 0xFF),
     RGB(0xFF, 0xFF, 0xFF), 178, 12, false},
    {"midnight", L"午夜紫", RGB(0x20, 0x1A, 0x2E), RGB(0xA5, 0x78, 0xFF),
     RGB(0xFF, 0xFF, 0xFF), 182, 12, false},
    // ── 浅色/自然系 ──
    {"mica", L"云母白", RGB(0xF3, 0xF4, 0xF8), RGB(0x2F, 0x6F, 0xED),
     RGB(0x16, 0x1C, 0x28), 150, 12, true},
    {"latte", L"拿铁咖", RGB(0xF2, 0xE7, 0xDA), RGB(0xC0, 0x7A, 0x4A),
     RGB(0x2B, 0x1E, 0x14), 148, 12, true},
    {"matcha", L"抹茶绿", RGB(0xDE, 0xE7, 0xDA), RGB(0x5E, 0x8C, 0x6A),
     RGB(0x1C, 0x26, 0x1E), 146, 12, true},
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
