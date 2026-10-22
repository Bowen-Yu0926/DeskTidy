#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// Explorer-compatible CF_HDROP + FileNameW + Shell IDList Array +
// Preferred DropEffect (COPY/MOVE).
bool ClipboardSetFiles(HWND owner, const std::vector<std::wstring>& paths,
                       bool cut);
bool ClipboardGetFiles(std::vector<std::wstring>* paths);
// One OpenClipboard: CF_HDROP paths + Preferred DropEffect. *cut is set true
// when the clipboard holds Preferred DropEffect == DROPEFFECT_MOVE (剪切).
bool ClipboardGetFilesWithEffect(std::vector<std::wstring>* paths, bool* cut);
bool ClipboardSetText(HWND owner, const std::wstring& text);
// True when the clipboard holds this path with Preferred DropEffect = move.
bool ClipboardPathIsCut(const std::wstring& path);
