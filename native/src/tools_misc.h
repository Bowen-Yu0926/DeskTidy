#pragma once

#include <Windows.h>

#include <string>
#include <vector>

bool PickAndSetWallpaper(HWND owner);
bool WallpaperMenu(HWND owner);  // pick / random from folder / restore previous
bool CaptureRegionToPng(HWND owner);
bool StartScreenRecord(HWND owner);  // toggle record (badge to stop)
void ConfigureScreenRecord(HWND owner);
void OpenCalculator();
void ShowAppHelp(HWND owner);
void OpenUpdatePage();
bool SendPathsToRecycleBin(const std::vector<std::wstring>& paths);
