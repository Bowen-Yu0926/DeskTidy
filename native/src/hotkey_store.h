#pragma once

#include <Windows.h>

#include <string>
#include <vector>

struct HotkeyDef {
  std::string id;  // settings, notes, peek, fences, page1/2, search, shot,
                   // fullscreen, record, organize, vault, todos
  std::wstring label;
  UINT modifiers = 0;
  UINT vk = 0;
};

std::vector<HotkeyDef> DefaultHotkeys();
std::vector<HotkeyDef> LoadHotkeys();
bool SaveHotkeys(const std::vector<HotkeyDef>& keys);
std::wstring FormatHotkey(UINT modifiers, UINT vk);
bool ParseHotkey(const std::wstring& text, UINT* modifiers, UINT* vk);

// Simple modal: click an item then press a new chord.
void EditHotkeysDialog(HWND owner, std::vector<HotkeyDef>* keys);
