#include "fence_style.h"

namespace {

// 精选 6 款经典 360 桌面整理与现代高颜值磨砂预设
constexpr FenceStylePreset kPresets[] = {
    // 1. 经典黑曜暗晶（360 经典黑曜暗晶磨砂，通透深邃）
    {"obsidian", L"曜石黑", RGB(0x13, 0x16, 0x20), RGB(0x93, 0xC5, 0xFD),
     RGB(0xFF, 0xFF, 0xFF), 140, 10, false},

    // 2. 商务深邃深海蓝（360 经典海蓝磨砂，科技沉稳）
    {"navy", L"深海蓝", RGB(0x0E, 0x18, 0x2C), RGB(0x38, 0xBD, 0xF8),
     RGB(0xFF, 0xFF, 0xFF), 142, 10, false},

    // 3. 通透浅色云母白（360 经典清新透白，极简明亮）
    {"mica", L"云母白", RGB(0xF6, 0xF8, 0xFC), RGB(0x25, 0x63, 0xEB),
     RGB(0x1E, 0x29, 0x3B), 160, 10, true},

    // 4. 清新翡翠极光绿（360 清新翡翠薄荷，生机护眼）
    {"aurora", L"极光绿", RGB(0x0C, 0x1F, 0x1A), RGB(0x34, 0xD3, 0x99),
     RGB(0xFF, 0xFF, 0xFF), 142, 10, false},

    // 5. 温暖琥珀落霞金（360 暖阳轻奢琥珀，温润典雅）
    {"sunset", L"落霞金", RGB(0x22, 0x16, 0x12), RGB(0xFB, 0x92, 0x3C),
     RGB(0xFF, 0xFF, 0xFF), 142, 10, false},

    // 6. 神秘罗兰幻夜紫（360 神秘暗夜紫晶，梦幻高贵）
    {"midnight", L"幻夜紫", RGB(0x18, 0x11, 0x26), RGB(0xC0, 0x84, 0xFC),
     RGB(0xFF, 0xFF, 0xFF), 142, 10, false},
};

}  // namespace

const FenceStylePreset& DefaultFenceStyle() { return kPresets[0]; }

std::string NormalizeFenceStyleId(const std::string& id) {
  for (const auto& p : kPresets) {
    if (id == p.id) {
      return id;
    }
  }
  // 历史与扩展旧预设向前兼容映射至 6 款新预设
  if (id == "cyberpunk" || id == "sky") return "navy";
  if (id == "graphite" || id == "dark" || id == "black") return "obsidian";
  if (id == "latte" || id == "peach") return "sunset";
  if (id == "matcha" || id == "mint") return "aurora";
  if (id == "mist" || id == "light" || id == "white") return "mica";
  if (id == "lavender" || id == "purple") return "midnight";

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
