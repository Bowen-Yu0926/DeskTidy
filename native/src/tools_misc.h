#pragma once

#include <Windows.h>

#include <string>
#include <vector>

bool PickAndSetWallpaper(HWND owner);
bool WallpaperMenu(HWND owner);  // pick / random from folder / restore previous
std::wstring WallpaperLibraryFolder();
void SetWallpaperLibraryFolder(const std::wstring& folder);
bool BrowseWallpaperLibrary(HWND owner);
// Desktop + Start Menu DeskNote.lnk → native.exe --notes when enabled.
void SyncDeskNoteShortcuts(bool enabled);
// Install-dir\录屏 when prefs empty (Python parity).
std::wstring ResolveRecordOutputDir();
bool RandomWallpaperFromLibrary(HWND owner);
bool CaptureRegionToPng(HWND owner);
bool StartScreenRecord(HWND owner);  // toggle record (badge to stop)
void ConfigureScreenRecord(HWND owner);
bool BrowseRecordOutputDir(HWND owner);
void ResetRecordOutputDir();
std::wstring BrowseFolderDialog(HWND owner, const wchar_t* title);
void OpenCalculator();
void ShowAppHelp(HWND owner);
void OpenUpdatePage(HWND owner = nullptr);  // GitHub API compare + optional open Releases
bool SendPathsToRecycleBin(const std::vector<std::wstring>& paths);
