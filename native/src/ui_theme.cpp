#include "ui_theme.h"

#include <cstdio>
#include <string>

namespace {

struct Palette {
  const char* window;
  const char* content;
  const char* card;
  const char* card_sub;
  const char* border;
  const char* text;
  const char* muted;
  const char* accent;
  const char* accent_soft;
  const char* accent_hover;
  const char* primary;
  const char* primary_hover;
  const char* danger;
  const char* sidebar;
  const char* sidebar_text;
  const char* sidebar_muted;
  const char* nav_active;
};

std::string RgbaFromHex(const char* hex, float alpha) {
  unsigned r = 0, g = 0, b = 0;
  if (hex && hex[0] == '#') {
    sscanf_s(hex + 1, "%02x%02x%02x", &r, &g, &b);
  }
  char buf[64];
  sprintf_s(buf, "rgba(%u, %u, %u, %.3f)", r, g, b, alpha);
  return buf;
}

const Palette& PaletteFor(const std::string& id) {
  // 6 款 360 风格现代高颜值 UI 配色方案（与桌面分区 100% 对齐，具备极佳文字对比度）
  static const Palette kObsidian{
      "#12151D", "#161B26", "#1F2637", "#171F2D", "#2E3A4E", "#F8FAFC", "#94A3B8",
      "#38BDF8", "rgba(56, 189, 248, 0.18)", "#0284C7", "#38BDF8", "#0284C7", "#EF4444",
      "#0F131D", "#F8FAFC", "#94A3B8", "#38BDF8"};
  static const Palette kNavy{
      "#0E1726", "#121E33", "#1A2946", "#142138", "#253B61", "#F8FAFC", "#94A3B8",
      "#38BDF8", "rgba(56, 189, 248, 0.18)", "#0EA5E9", "#38BDF8", "#0284C7", "#EF4444",
      "#0B1120", "#F8FAFC", "#94A3B8", "#38BDF8"};
  static const Palette kMica{
      "#EEF2F6", "#F4F7FA", "#FFFFFF", "#F1F5F9", "#CBD5E1", "#0F172A", "#475569",
      "#2563EB", "#DBEAFE", "#1D4ED8", "#2563EB", "#1D4ED8", "#DC2626",
      "#F8FAFC", "#0F172A", "#64748B", "#2563EB"};
  static const Palette kAurora{
      "#0B1B17", "#0F241F", "#15332B", "#102923", "#1F4D41", "#F0FDF4", "#86EFAC",
      "#34D399", "rgba(52, 211, 153, 0.18)", "#10B981", "#10B981", "#059669", "#EF4444",
      "#081612", "#F0FDF4", "#86EFAC", "#34D399"};
  static const Palette kSunset{
      "#1F1511", "#281B16", "#37251E", "#2C1D18", "#52372D", "#FFF7ED", "#FDBA74",
      "#FB923C", "rgba(251, 146, 60, 0.18)", "#F97316", "#F97316", "#EA580C", "#EF4444",
      "#18100D", "#FFF7ED", "#FDBA74", "#FB923C"};
  static const Palette kMidnight{
      "#161021", "#1C142B", "#271C3D", "#201732", "#3F2D60", "#FAF5FF", "#D8B4FE",
      "#C084FC", "rgba(192, 132, 252, 0.18)", "#A855F7", "#A855F7", "#9333EA", "#EF4444",
      "#110C1B", "#FAF5FF", "#D8B4FE", "#C084FC"};

  if (id == "navy") {
    return kNavy;
  }
  if (id == "mica") {
    return kMica;
  }
  if (id == "aurora") {
    return kAurora;
  }
  if (id == "sunset") {
    return kSunset;
  }
  if (id == "midnight") {
    return kMidnight;
  }
  return kObsidian;
}

}  // namespace

const std::vector<UiThemeOption>& AllUiThemes() {
  static const std::vector<UiThemeOption> kThemes = {
      {"obsidian", L"曜石黑（360经典黑曜暗晶）"},
      {"navy", L"深海蓝（360经典海蓝磨砂）"},
      {"mica", L"云母白（360经典清新透白）"},
      {"aurora", L"极光绿（360清新翡翠薄荷）"},
      {"sunset", L"落霞金（360暖阳轻奢琥珀）"},
      {"midnight", L"幻夜紫（360神秘暗夜紫晶）"},
      {"liquid_glass", L"液态玻璃（晶莹通透超清毛玻璃）"},
  };
  return kThemes;
}

std::string NormalizeUiTheme(const std::string& id) {
  if (id == "graphite" || id == "dark" || id == "black") {
    return "obsidian";
  }
  if (id == "sky" || id == "cyberpunk") {
    return "navy";
  }
  if (id == "mist" || id == "light" || id == "white") {
    return "mica";
  }
  if (id == "mint" || id == "matcha") {
    return "aurora";
  }
  if (id == "peach" || id == "latte") {
    return "sunset";
  }
  if (id == "lavender" || id == "purple") {
    return "midnight";
  }
  if (id == "liquid_glass" || id == "glass" || id == "liquid") {
    return "liquid_glass";
  }
  for (const auto& t : AllUiThemes()) {
    if (id == t.id) {
      return id;
    }
  }
  return "obsidian";
}

const UiThemeOption* UiThemeById(const std::string& id) {
  const std::string key = NormalizeUiTheme(id);
  for (const auto& t : AllUiThemes()) {
    if (key == t.id) {
      return &t;
    }
  }
  return &AllUiThemes().front();
}

std::unordered_map<std::string, std::string> UiThemeCssVars(const std::string& id) {
  const Palette& p = PaletteFor(NormalizeUiTheme(id));
  std::unordered_map<std::string, std::string> out;
  out["window"] = p.window;
  out["content"] = p.content;
  out["card"] = p.card;
  out["card-sub"] = p.card_sub;
  out["border"] = p.border;
  out["text"] = p.text;
  out["muted"] = p.muted;
  out["accent"] = p.accent;
  out["accent-soft"] = p.accent_soft;
  out["accent-hover"] = p.accent_hover;
  out["primary"] = p.primary;
  out["primary-hover"] = p.primary_hover;
  out["danger"] = p.danger;
  out["sidebar"] = p.sidebar;
  out["sidebar-text"] = p.sidebar_text;
  out["sidebar-muted"] = p.sidebar_muted;
  out["nav-active"] = p.nav_active;
  out["sidebar-hover"] = RgbaFromHex(p.accent, 0.12f);
  out["sidebar-active"] = RgbaFromHex(p.accent, 0.18f);
  out["sidebar-line"] = RgbaFromHex(p.accent, 0.18f);
  out["inlay"] = p.accent;
  return out;
}
