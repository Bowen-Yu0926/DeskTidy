#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// Explorer-compatible CF_HDROP + Preferred DropEffect.
bool ClipboardSetFiles(HWND owner, const std::vector<std::wstring>& paths,
                       bool cut);
