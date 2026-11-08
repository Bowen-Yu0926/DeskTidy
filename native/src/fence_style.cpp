#include "fence_style.h"

namespace {

// 精选 6 款经典 360 桌面整理与现代高颜值磨砂预设
constexpr FenceStylePreset kPresets[] = {
    // 1. 经典黑曜暗晶（最耐看主流深色磨砂）
    {"obsidian", L"曜石黑", RGB(0x14, 0x18, 0x22), RGB(0xE2, 0xE8, 0xF0),
     RGB(0xFF, 0xFF, 0xFF), 145, 12, false},

    // 2. 商务深邃深海蓝（360 经典海蓝）
    {"navy", L"深海蓝", RGB(0x10, 0x1A, 0x2E), RGB(0x38, 0xBD, 0xF8),
     RGB(0xFF, 0xFF, 0xFF), 150, 12, false},

    // 3. 通透浅色云母白（360 经典浅色清新）
    {"mica", L"云母白", RGB(0xF6, 0xF8, 0xFC), RGB(0x25, 0x63, 0xEB),
     RGB(0x1E, 0x29, 0x3B), 165, 12, true},

    // 4. 清新翡翠极光绿（360 薄荷绿）
    {"aurora", L"极光绿", RGB(0x0E, 0x1E, 0x1A), RGB(0x34, 0xD3, 0x99),
     RGB(0xFF, 0xFF, 0xFF), 148, 12, false},

    // 5. 温暖琥珀落霞金（360 暖阳金）
    {"sunset", L"落霞金", RGB(0x24, 0x18, 0x14), RGB(0xFB, 0x92, 0x3C),
     RGB(0xFF, 0xFF, 0xFF), 150, 12, false},

    // 6. 神秘罗兰幻夜紫（360 暗夜紫晶）
    {"midnight", L"幻夜紫", RGB(0x1A, 0x12, 0x2A), RGB(0xC0, 0x84, 0xFC),
     RGB(0xFF, 0xFF, 0xFF), 150, 12, false},
};

}  // namespace

const FenceStylePreset& DefaultFenceStyle() { return kPresets[0]; }

std::string NormalizeFenceStyleId(const std::string& id) {
  for (const auto& p : kPresets) {
    if (id == p.id) {
      return id;
    }
  }
  // 历史旧预设向前兼容映射至 6 款新预设
  if (id == "cyberpunk") return "navy";
  if (id == "graphite") return "obsidian";
  if (id == "latte") return "sunset";
  if (id == "matcha") return "aurora";

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
