#pragma once

#include <string>

// Notepad++-style elevated (UAC) save handoff for system-protected paths.
// Pending payload lives under NativeDataDir()/elevated_save_pending.json.

std::wstring ElevatedPendingPath();
// Helper writes a small result file so the original window can distinguish a
// completed write from a UAC denial / helper crash / 30s timeout.
std::wstring ElevatedResultPath();
bool WriteElevatedResult(const std::wstring& target, unsigned request_id,
                         bool ok);
// Returns true when a result exists for `target`/`request_id`; *ok reports
// the write outcome. request_id=0 accepts any result (legacy compatibility).
bool ReadElevatedResult(const std::wstring& target, unsigned request_id,
                        bool* ok);
bool ClearElevatedResult();

bool WriteElevatedPending(const std::wstring& target, const std::wstring& text);
bool ClearElevatedPending();
void PurgeOrphanElevatedPending();

// Elevated relaunch: write pending payload to *target* then clear the file.
// Returns false when payload missing/mismatched or the write fails.
bool ConsumeElevatedPending(const std::wstring& target);

// ShellExecuteW("runas") deskNote.exe --elevated-save[-reopen] "<target>".
// reopen=true asks the elevated helper to relaunch a normal DeskNote window
// with the saved target after the handoff completes.
bool RelaunchDeskNoteElevated(const std::wstring& target, unsigned request_id,
                              bool reopen = false);

// True when current process runs with elevated (admin) token.
bool IsCurrentProcessElevated();

// True when last CreateFile/Write failed with ERROR_ACCESS_DENIED (or share).
bool LastWriteWasAccessDenied();

// Write raw bytes to path. Sets LastWriteWasAccessDenied on fail.
bool WriteFileBytes(const std::wstring& path, const void* data, size_t size);
