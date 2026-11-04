#include "fence_style.h"

namespace {

// 6 curated presets across mainstream desktop-organizer looks (360 / Tencent
// / Fences / Windows 11 Mica / macOS graphite). radius 12 ≈ Windows 11 feel.
constexpr FenceStylePreset kPresets[] = {
    // —— 深色系 ——
    // 墨夜蓝：腾讯桌面整理 / Win11 暗色亚克力，深蓝灰 + 青蓝高亮
    {"navy", L"墨夜蓝", RGB(0x20, 0x26, 0x33), RGB(0x4C, 0xC2, 0xFF),
     RGB(0xFF, 0xFF, 0xFF), 185, 12, false},
    // 石墨灰：macOS 石墨 / Fences，中性深灰 + 蓝高亮
    {"graphite", L"石墨灰", RGB(0x2E, 0x30, 0x36), RGB(0x64, 0xA0, 0xFF),
     RGB(0xFF, 0xFF, 0xFF), 178, 12, false},
    // 暗夜紫：元气桌面暗色霓虹，深紫黑 + 紫高亮
    {"midnight", L"暗夜紫", RGB(0x20, 0x1A, 0x2E), RGB(0xA5, 0x78, 0xFF),
     RGB(0xFF, 0xFF, 0xFF), 182, 12, false},
    // —— 浅色系 ——
    // 云母白：Windows 11 云母 / 腾讯浅色，极浅灰白 + 蓝高亮
    {"mica", L"云母白", RGB(0xF3, 0xF4, 0xF8), RGB(0x2F, 0x6F, 0xED),
     RGB(0x16, 0x1C, 0x28), 150, 12, true},
    // 奶咖：360 商务暖棕，暖米 + 咖橙高亮
    {"latte", L"奶咖", RGB(0xF2, 0xE7, 0xDA), RGB(0xC0, 0x7A, 0x4A),
     RGB(0x2B, 0x1E, 0x14), 148, 12, true},
    // 抹茶绿：莫兰迪低饱和灰绿，灰绿 + 草绿高亮
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
