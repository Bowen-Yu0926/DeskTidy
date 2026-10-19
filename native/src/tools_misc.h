#pragma once

#include <Windows.h>

#include <string>
#include <vector>

bool PickAndSetWallpaper(HWND owner);
bool CaptureRegionToPng(HWND owner);
bool StartScreenRecord(HWND owner);
void OpenUpdatePage();
bool SendPathsToRecycleBin(const std::vector<std::wstring>& paths);
