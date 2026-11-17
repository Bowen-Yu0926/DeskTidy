#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

struct ShellMenuExtra {
  std::wstring label;
  // Leaf command id in 1..99. Parents with children may use 0.
  int id = 0;
  bool enabled = true;
  bool checked = false;
  // Non-empty → cascading submenu (Fences-style Sort / View / Opacity).
  std::vector<ShellMenuExtra> children;
};

// Cascading 「移动到分页」→ page → fence. `pages` = membership (empty → page 0).
struct MoveFenceMenuTarget {
  std::string id;
  std::wstring title;
  std::vector<int> pages;
};
// Returns chosen fence id, or empty if cancelled / no pick.
std::string TrackMoveToPageThenFenceMenu(
    HWND owner, POINT screen, const std::vector<std::wstring>& page_names,
    const std::vector<MoveFenceMenuTarget>& fences,
    const std::string& exclude_fence_id = {});

// Fired with the canonical verb name (e.g. "rename", "refresh", "undo",
// "newfolder", "paste"). For host-handled rename: when the user picks it
// (no InvokeCommand). For other verbs: after InvokeCommand returns so
// paste/newfolder/undo re-scan sees landed files. Not fired for extras/cancel.
using ShellVerbFn = std::function<void(const std::wstring& verb)>;
// Fired immediately before InvokeCommand runs (not for extras, rename or
// cancel) so callers can snapshot the folder before the shell mutates it.
using ShellBeforeInvokeFn = std::function<void()>;
// If set and returns true for a verb, host handles it (no InvokeCommand).
// Used like CMF_CANRENAME: virtual-fence Paste → pin paste, not Desktop FO.
using ShellInterceptFn = std::function<bool(const std::wstring& verb)>;

// Fences / Explorer: only filesystem-mutating verbs warrant a desktop rescan.
// open / properties / copy / cut stay quiet (cut ghost follows clipboard).
bool ShellVerbNeedsDesktopRescan(const std::wstring& verb);

// Host Explorer IContextMenu for filesystem path(s). Prepends extras (ids 1..99).
// Returns extra id if chosen; 0 if cancelled or a shell verb ran; -1 if menu
// could not be built (caller may fall back to a hand-rolled menu).
int TrackShellContextMenu(HWND owner, POINT screen,
                          const std::vector<std::wstring>& paths,
                          const std::vector<ShellMenuExtra>& extras = {},
                          ShellVerbFn on_verb = nullptr,
                          ShellBeforeInvokeFn on_before_invoke = nullptr,
                          ShellInterceptFn on_intercept = nullptr);

// Host Explorer's FOLDER BACKGROUND IContextMenu (New / Paste / View /
// Personalize / Refresh / Undo / Sort-by, ...) for a directory's blank area.
// `folder` is the directory whose blank-area menu to show (typically the user
// desktop). Prepends extras (ids 1..99). Return semantics match
// TrackShellContextMenu.
int TrackShellFolderBackgroundMenu(HWND owner, POINT screen,
                                   const std::wstring& folder,
                                   const std::vector<ShellMenuExtra>& extras = {},
                                   ShellVerbFn on_verb = nullptr,
                                   ShellBeforeInvokeFn on_before_invoke = nullptr,
                                   ShellInterceptFn on_intercept = nullptr);

// Silent folder-background verb (e.g. "undo") — Explorer Ctrl+Z / blank Undo.
bool InvokeShellFolderVerb(HWND owner, const std::wstring& folder,
                           const wchar_t* verb);

// Minimal fallback menu for the desktop blank area, used when the Explorer
// background IContextMenu cannot be acquired (shell restart, Explorer crash,
// unusual shell replacements). Without it the user gets a menu with no
// system items at all — no Refresh / New / Paste / View — which looks like
// the feature is broken rather than degraded.
// Returns the chosen custom id (0 = nothing / cancelled).
int TrackFallbackDesktopMenu(HWND owner, POINT screen,
                             const std::wstring& folder);
