#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// Sibling deskNote.exe next to the calling module (DeskTidy or DeskNote).
std::wstring DeskNoteExePath();
bool DeskNoteExeExists();

// Product icon for shortcuts / ProgId (assets\\desknote_icon.ico, else exe).
std::wstring DeskNoteIconPath();

// Desktop + Start Menu DeskNote.lnk → deskNote.exe (no args).
void SyncDeskNoteShortcuts(bool enabled);

// Single-instance mutex / hidden IPC window for deskNote.exe.
bool AcquireDeskNoteInstanceLock();
void ReleaseDeskNoteInstanceLock();
bool IsDeskNoteRunning();

// Forward open/focus to a running DeskNote (WM_COPYDATA).
// action examples: L"--focus", L"--open", L"--open\0path1\0path2" via OpenPaths payload.
bool ForwardToDeskNote(const std::wstring& action);

// Launch deskNote.exe if needed, then open optional paths (empty = just present).
bool LaunchOrFocusDeskNote(const std::vector<std::wstring>& paths = {});

// IPC window class / title used by desknote_main.
constexpr wchar_t kDeskNoteMsgClass[] = L"DeskNoteNativeMsg";
constexpr wchar_t kDeskNoteMsgTitle[] = L"DeskNoteNative";
constexpr UINT kDeskNoteCliMsg = WM_APP + 91;

// Match Python win_app_id.DESKNOTE_AUMID / DESKTIDY_AUMID.
constexpr wchar_t kDeskNoteAumid[] = L"DeskTidy.DeskNote";
constexpr wchar_t kDeskTidyAumid[] = L"DeskTidy.Desktop";

bool SetProcessAppUserModelId(const wchar_t* aumid);
bool ApplyShortcutAppUserModelId(const std::wstring& lnk_path,
                                 const wchar_t* aumid);
