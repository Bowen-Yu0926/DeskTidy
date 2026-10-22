#pragma once

#include <string>
#include <unordered_map>
#include <vector>

struct UiThemeOption {
  const char* id;
  const wchar_t* name_zh;
};

// Python THEME_OPTIONS / get_theme_palette (with dark sidebar ink).
const std::vector<UiThemeOption>& AllUiThemes();
std::string NormalizeUiTheme(const std::string& id);
const UiThemeOption* UiThemeById(const std::string& id);
// CSS custom properties for settings.html :root (keys without leading --).
std::unordered_map<std::string, std::string> UiThemeCssVars(const std::string& id);
