#pragma once

#include <Windows.h>

#include <string>

std::wstring MinutesFolder(bool ensure = true);
// Empty when using the default install-dir folder.
std::wstring MinutesFolderOverride();
void ConfigureMinutesFolder(HWND owner);
void ResetMinutesFolder();
void SetMinutesFolder(const std::wstring& folder);
// Create next dated markdown and open in notepad. Returns path.
std::wstring CreateAndOpenMinutes(HWND owner);
// Create dated .rtf (Word-friendly) and open with default app.
std::wstring CreateAndOpenMinutesRtf(HWND owner);
// Create next dated .docx (matches Python meeting_minutes.py structure).
std::wstring CreateAndOpenMinutesDocx(HWND owner);
void OpenMinutesFolder(HWND owner);
// Prompt to open one of the most recent .md/.rtf notes.
bool OpenRecentMinutes(HWND owner);
