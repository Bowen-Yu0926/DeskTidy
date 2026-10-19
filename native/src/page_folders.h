#pragma once

#include <Windows.h>

#include <string>
#include <vector>

struct PageFolderItem {
  std::wstring name;
  std::wstring path;
};

std::vector<PageFolderItem> LoadPageFolders();
bool SavePageFolders(const std::vector<PageFolderItem>& items);
void EditPageFoldersDialog(HWND owner);
void OpenPageFolderMenu(HWND owner);
