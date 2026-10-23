#pragma once

#include <string>

// Notepad++-style elevated (UAC) save handoff for system-protected paths.
// Pending payload lives under NativeDataDir()/elevated_save_pending.json.

std::wstring ElevatedPendingPath();

bool WriteElevatedPending(const std::wstring& target, const std::wstring& text);
bool ClearElevatedPending();
void PurgeOrphanElevatedPending();

// Elevated relaunch: write pending payload to *target* then clear the file.
// Returns false when payload missing/mismatched or the write fails.
bool ConsumeElevatedPending(const std::wstring& target);

// ShellExecuteW("runas") deskNote.exe --elevated-save "<target>".
bool RelaunchDeskNoteElevated(const std::wstring& target);

// True when last CreateFile/Write failed with ERROR_ACCESS_DENIED (or share).
bool LastWriteWasAccessDenied();

// Write raw bytes to path (CREATE_ALWAYS). Sets LastWriteWasAccessDenied on fail.
bool WriteFileBytes(const std::wstring& path, const void* data, size_t size);
