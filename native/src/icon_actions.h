#pragma once

#include <Windows.h>

#include <string>
#include <vector>

enum class IconFsResult { Ok, Cancelled, Failed };

void OpenShellPath(const std::wstring& path);
void RevealShellPath(const std::wstring& path);
// Explorer Ctrl+dblclick folder → new Explorer window (/e,path).
void ExploreShellPath(const std::wstring& path);
bool CopyOrCutShellPath(HWND owner, const std::wstring& path, bool cut);
bool CopyShellPathText(HWND owner, const std::wstring& path);
IconFsResult RenameShellPath(HWND owner, const std::wstring& path,
                             std::wstring* dest);
bool ShowShellPathProperties(HWND owner, const std::wstring& path);
// Explorer multi-select Properties (SHMultiFileProperties); n==1 → single sheet.
bool ShowShellPathsProperties(HWND owner,
                              const std::vector<std::wstring>& paths);
// permanent=false → recycle (FOF_ALLOWUNDO); true → Explorer Shift+Delete.
IconFsResult RecycleShellPath(HWND owner, const std::wstring& path,
                              bool permanent = false);
IconFsResult RecycleShellPaths(HWND owner,
                              const std::vector<std::wstring>& paths,
                              bool permanent = false);

// Explorer-style "name (2).ext" collision destination inside dest_dir.
std::wstring UniqueDestPath(const std::wstring& dest_dir,
                            const std::wstring& name);

// True when both paths live on the same volume (Explorer drag default).
bool SameVolume(const std::wstring& a, const std::wstring& b);

// True when path is strictly under ancestor (Explorer: refuse folder→child).
bool IsStrictPathPrefix(const std::wstring& ancestor,
                        const std::wstring& path);

// Move/copy a single source into dest_dir, resolving "name (2).ext" collisions
// item-by-item. On success sets *landed to the final path and returns true.
// Never aborts the batch caller; failures return false.
bool MoveOrCopyFileIntoDir(HWND owner, const std::wstring& src,
                           const std::wstring& dest_dir, bool move,
                           std::wstring* landed);

// Explorer / Fences: Alt or Ctrl+Shift drop → .lnk in dest_dir (source untouched).
bool CreateShortcutIntoDir(HWND owner, const std::wstring& src,
                           const std::wstring& dest_dir, std::wstring* landed);

// True when drop modifiers request DROPEFFECT_LINK (Alt or Ctrl+Shift).
bool WantDropLinkEffect();

// Fences / Explorer: paste CF_HDROP into the user Desktop folder (copy or
// cut per Preferred DropEffect). Returns how many items landed; fills
// *landed_paths when non-null.
int PasteClipboardToDesktopFolder(HWND owner,
                                  std::vector<std::wstring>* landed_paths);

// Explorer 「粘贴快捷方式」(pastelink): create .lnk into dest_dir for each
// CF_HDROP path (source untouched). Returns how many .lnk landed.
int PasteClipboardLinksIntoDir(HWND owner, const std::wstring& dest_dir,
                               std::vector<std::wstring>* landed_paths);

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
