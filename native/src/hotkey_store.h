#pragma once

#include <Windows.h>

#include <string>
#include <vector>

struct HotkeyDef {
  // Python ids: organize, toggle_fences, toggle_icons, peek_fences,
  // page_next, page_prev, screenshot, show_screenshot, screen_record,
  // file_search, calculator
  std::string id;
  std::wstring label;
  UINT modifiers = 0;
  UINT vk = 0;
};

std::vector<HotkeyDef> DefaultHotkeys();
std::vector<HotkeyDef> LoadHotkeys();
bool SaveHotkeys(const std::vector<HotkeyDef>& keys);
std::wstring FormatHotkey(UINT modifiers, UINT vk);
bool ParseHotkey(const std::wstring& text, UINT* modifiers, UINT* vk);
bool CaptureHotkeyChord(HWND owner, UINT* mods, UINT* vk);

// Fallback chords tried when a configured binding is occupied/unparseable
// (mirrors Python HOTKEY_FALLBACKS). Empty for actions with no fallbacks.
std::vector<std::wstring> HotkeyFallbacks(const std::string& id);

// Fallback MessageBox summary — prefer the settings WebView list.
void EditHotkeysDialog(HWND owner, std::vector<HotkeyDef>* keys);
