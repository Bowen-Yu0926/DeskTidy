#pragma once

#include <Windows.h>

#include <string>

std::wstring MinutesFolder(bool ensure = true);
void ConfigureMinutesFolder(HWND owner);
// Create next dated markdown and open in notepad. Returns path.
std::wstring CreateAndOpenMinutes(HWND owner);
void OpenMinutesFolder(HWND owner);
