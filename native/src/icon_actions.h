#pragma once

#include <Windows.h>

#include <string>
#include <vector>

enum class IconFsResult { Ok, Cancelled, Failed };

void OpenShellPath(const std::wstring& path);
void RevealShellPath(const std::wstring& path);
bool CopyOrCutShellPath(HWND owner, const std::wstring& path, bool cut);
bool CopyShellPathText(HWND owner, const std::wstring& path);
IconFsResult RenameShellPath(HWND owner, const std::wstring& path,
                             std::wstring* dest);
bool ShowShellPathProperties(HWND owner, const std::wstring& path);
IconFsResult RecycleShellPath(HWND owner, const std::wstring& path);
IconFsResult RecycleShellPaths(HWND owner,
                              const std::vector<std::wstring>& paths);

// 1 open, 2 reveal, 4 copy, 5 cut, 6 copy path, 7 rename, 8 properties, 9 delete.
// include_unpin adds command 3.
int TrackIconShellMenu(HWND owner, POINT screen, bool include_unpin);
