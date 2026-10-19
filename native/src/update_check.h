#pragma once

#include <Windows.h>

#include <string>

// Current product version (matches version.rc FileVersion).
std::wstring NativeProductVersion();

// Query GitHub latest release; prompt to open page when newer.
void CheckForUpdates(HWND owner);
