#pragma once

#include <Windows.h>

#include <string>

// Pick / create a note file under DeskTidyNative\notes\. Returns empty on cancel.
std::wstring PickNoteFromLibrary(HWND owner);
std::wstring NotesLibraryDir(bool ensure = true);
