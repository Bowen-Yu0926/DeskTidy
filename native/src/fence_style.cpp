#include "fence_style.h"

namespace {

// 精选 8 款市面主流桌面整理高颜值磨砂配色，色相鲜明、辨识度极高、通透微光
constexpr FenceStylePreset kPresets[] = {
    // 1. 经典黑曜暗晶（纯净透光磨砂灰黑，沉稳高贵，不挑壁纸）
    {"obsidian", L"曜石黑", RGB(22, 26, 36), RGB(147, 197, 253),
     RGB(255, 255, 255), 148, 13, false},

    // 2. 深海湛蓝（科技深海宝石蓝，色泽深邃澄澈）
    {"navy", L"深海蓝", RGB(18, 48, 88), RGB(56, 189, 248),
     RGB(255, 255, 255), 150, 13, false},

    // 3. 通透云母白（极简明净透白，微透桌面纹理）
    {"mica", L"云母白", RGB(246, 248, 252), RGB(37, 99, 235),
     RGB(30, 41, 59), 165, 13, true},

    // 4. 清新极光绿（生机盎然的深翡翠薄荷色）
    {"aurora", L"极光绿", RGB(16, 56, 44), RGB(52, 211, 153),
     RGB(255, 255, 255), 150, 13, false},

    // 5. 温暖落霞金（温润高级的落霞轻奢琥珀金）
    {"sunset", L"落霞金", RGB(60, 34, 20), RGB(251, 146, 60),
     RGB(255, 255, 255), 150, 13, false},

    // 6. 幻夜紫晶（神秘梦幻的深紫罗兰宝石色）
    {"midnight", L"幻夜紫", RGB(44, 22, 68), RGB(192, 132, 252),
     RGB(255, 255, 255), 150, 13, false},

    // 7. 暮色嫣红（浪漫轻奢深玫红，红玛瑙质感）
    {"rose", L"暮色红", RGB(60, 20, 34), RGB(251, 113, 133),
     RGB(255, 255, 255), 150, 13, false},

    // 8. 静谧黛灰（极简高级商务金属冷灰）
    {"slate", L"静谧灰", RGB(34, 44, 58), RGB(148, 163, 184),
     RGB(255, 255, 255), 148, 13, false},
};

}  // namespace

const FenceStylePreset& DefaultFenceStyle() { return kPresets[1]; } // 默认 navy (深海蓝)

std::string NormalizeFenceStyleId(const std::string& id) {
  for (const auto& p : kPresets) {
    if (id == p.id) {
      return id;
    }
  }
  // 历史与扩展旧预设向前兼容映射至 8 款新预设
  if (id == "cyberpunk" || id == "sky") return "navy";
  if (id == "graphite" || id == "dark" || id == "black") return "obsidian";
  if (id == "latte" || id == "peach") return "sunset";
  if (id == "matcha" || id == "mint") return "aurora";
  if (id == "mist" || id == "light" || id == "white") return "mica";
  if (id == "lavender" || id == "purple") return "midnight";
  if (id == "red" || id == "crimson") return "rose";
  if (id == "gray" || id == "grey") return "slate";

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
