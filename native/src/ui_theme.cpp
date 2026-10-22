#include "ui_theme.h"

#include <cstdio>
#include <string>

namespace {

struct Palette {
  const char* window;
  const char* content;
  const char* card;
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
  // Content colors from Python _PALETTES; sidebar from _SIDEBAR_INK.
  static const Palette kMist{
      "#EEF1F6", "#F4F6F9", "#FFFFFF", "#C9D2DE", "#1A2332", "#64748B",
      "#1D4ED8", "#E0E7FF", "#1E40AF", "#16A34A", "#15803D", "#DC2626",
      "#1A2740", "#EAF0FA", "#93A4BF"};
  static const Palette kGraphite{
      "#E8EAED", "#F0F1F3", "#FFFFFF", "#C5CAD3", "#1F2933", "#6B7280",
      "#4B5563", "#E5E7EB", "#374151", "#374151", "#1F2937", "#DC2626",
      "#1C1F24", "#F1F3F5", "#9AA3AF"};
  static const Palette kSky{
      "#EAF4FF", "#F2F8FF", "#FFFFFF", "#B9D4F0", "#0F2744", "#5B7391",
      "#0EA5E9", "#E0F2FE", "#0284C7", "#0EA5E9", "#0284C7", "#E11D48",
      "#0A2F4D", "#E7F5FF", "#7EB6D9"};
  static const Palette kMint{
      "#EAF8F2", "#F3FBF7", "#FFFFFF", "#B7DFCB", "#12352A", "#4F7466",
      "#10B981", "#D1FAE5", "#059669", "#059669", "#047857", "#DC2626",
      "#0D2F26", "#E8FBF4", "#7DBFA8"};
  static const Palette kLavender{
      "#F2EEFF", "#F7F4FF", "#FFFFFF", "#D2C7EF", "#2A2148", "#6B6288",
      "#8B5CF6", "#EDE9FE", "#7C3AED", "#7C3AED", "#6D28D9", "#E11D48",
      "#24183A", "#F3EDFF", "#B5A4D9"};
  static const Palette kPeach{
      "#FFF1E8", "#FFF7F2", "#FFFFFF", "#F0CDB8", "#3B2418", "#8A6452",
      "#F97316", "#FFEDD5", "#EA580C", "#EA580C", "#C2410C", "#DC2626",
      "#2E1A12", "#FFF4EC", "#D4A88A"};
  if (id == "graphite") {
    return kGraphite;
  }
  if (id == "sky") {
    return kSky;
  }
  if (id == "mint") {
    return kMint;
  }
  if (id == "lavender") {
    return kLavender;
  }
  if (id == "peach") {
    return kPeach;
  }
  return kMist;
}

}  // namespace

const std::vector<UiThemeOption>& AllUiThemes() {
  static const std::vector<UiThemeOption> kThemes = {
      {"mist", L"薄雾浅灰（推荐）"},
      {"graphite", L"石墨灰（推荐）"},
      {"sky", L"天空蓝"},
      {"mint", L"薄荷绿"},
      {"lavender", L"淡紫 · 柔和"},
      {"peach", L"浅杏 · 柔和"},
  };
  return kThemes;
}

std::string NormalizeUiTheme(const std::string& id) {
  if (id == "dark" || id == "black") {
    return "mist";
  }
  for (const auto& t : AllUiThemes()) {
    if (id == t.id) {
      return id;
    }
  }
  return "mist";
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
  out["sidebar-hover"] = RgbaFromHex(p.accent, 0.16f);
  out["sidebar-active"] = RgbaFromHex(p.accent, 0.26f);
  out["sidebar-line"] = RgbaFromHex(p.accent, 0.28f);
  out["inlay"] = p.accent;
  return out;
}
