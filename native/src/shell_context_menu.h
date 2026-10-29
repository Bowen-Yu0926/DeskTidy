#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

struct ShellMenuExtra {
  std::wstring label;
  int id = 0;  // 1..99
  bool enabled = true;
  bool checked = false;
};

// Fired with the canonical verb name (e.g. "rename", "refresh", "undo",
// "newfolder", "paste"). For host-handled rename: when the user picks it
// (no InvokeCommand). For other verbs: after InvokeCommand returns so
// paste/newfolder/undo re-scan sees landed files. Not fired for extras/cancel.
using ShellVerbFn = std::function<void(const std::wstring& verb)>;
// Fired immediately before InvokeCommand runs (not for extras, rename or
// cancel) so callers can snapshot the folder before the shell mutates it.
using ShellBeforeInvokeFn = std::function<void()>;

// Host Explorer IContextMenu for filesystem path(s). Prepends extras (ids 1..99).
// Returns extra id if chosen; 0 if cancelled or a shell verb ran; -1 if menu
// could not be built (caller may fall back to a hand-rolled menu).
int TrackShellContextMenu(HWND owner, POINT screen,
                          const std::vector<std::wstring>& paths,
                          const std::vector<ShellMenuExtra>& extras = {},
                          ShellVerbFn on_verb = nullptr,
                          ShellBeforeInvokeFn on_before_invoke = nullptr);

// Host Explorer's FOLDER BACKGROUND IContextMenu (New / Paste / View /
// Personalize / Refresh / Undo / Sort-by, ...) for a directory's blank area.
// `folder` is the directory whose blank-area menu to show (typically the user
// desktop). Prepends extras (ids 1..99). Return semantics match
// TrackShellContextMenu.
int TrackShellFolderBackgroundMenu(HWND owner, POINT screen,
                                   const std::wstring& folder,
                                   const std::vector<ShellMenuExtra>& extras = {},
                                   ShellVerbFn on_verb = nullptr,
                                   ShellBeforeInvokeFn on_before_invoke = nullptr);
