#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// Explorer-compatible CF_HDROP + Preferred DropEffect.
bool ClipboardSetFiles(HWND owner, const std::vector<std::wstring>& paths,
                       bool cut);
bool ClipboardGetFiles(std::vector<std::wstring>* paths);
bool ClipboardSetText(HWND owner, const std::wstring& text);
// True when the clipboard holds this path with Preferred DropEffect = move.
bool ClipboardPathIsCut(const std::wstring& path);
