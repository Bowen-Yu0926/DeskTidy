#pragma once

#include <Windows.h>

#include <string>

std::wstring MinutesFolder(bool ensure = true);
void ConfigureMinutesFolder(HWND owner);
// Create next dated markdown and open in notepad. Returns path.
std::wstring CreateAndOpenMinutes(HWND owner);
// Create dated .rtf (Word-friendly) and open with default app.
std::wstring CreateAndOpenMinutesRtf(HWND owner);
void OpenMinutesFolder(HWND owner);
// Prompt to open one of the most recent .md/.rtf notes.
bool OpenRecentMinutes(HWND owner);
