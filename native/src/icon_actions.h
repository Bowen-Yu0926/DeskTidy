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

// Explorer-style "name (2).ext" collision destination inside dest_dir.
std::wstring UniqueDestPath(const std::wstring& dest_dir,
                            const std::wstring& name);

// True when both paths live on the same volume (Explorer drag default).
bool SameVolume(const std::wstring& a, const std::wstring& b);

// Move/copy a single source into dest_dir, resolving "name (2).ext" collisions
// item-by-item. On success sets *landed to the final path and returns true.
// Never aborts the batch caller; failures return false.
bool MoveOrCopyFileIntoDir(HWND owner, const std::wstring& src,
                           const std::wstring& dest_dir, bool move,
                           std::wstring* landed);

// Rename plan with extension protection (.lnk/.exe/.url keep their extension
// on stem-only edits) and Windows name validation.
struct RenamePlan {
  bool valid = false;   // false = no-op or error (see error text)
  std::wstring dest;    // full new path in the same folder
  std::wstring error;   // user-facing reason when !valid
};
RenamePlan PlanRename(const std::wstring& old_path,
                      const std::wstring& new_name);

// 1 open, 2 reveal, 4 copy, 5 cut, 6 copy path, 7 rename, 8 properties, 9 delete.
// include_unpin adds command 3.
int TrackIconShellMenu(HWND owner, POINT screen, bool include_unpin);
