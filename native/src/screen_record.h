#pragma once

#include <Windows.h>

#include <string>
#include <vector>

// Toggle ffmpeg gdigrab recording. Shows a REC badge (pause / stop).
bool ToggleScreenRecord(HWND owner);
// Enumerate system audio-capture (microphone) devices via DirectShow, for the
// record config dialog (friendly names, never hand-typed).
std::vector<std::wstring> ListAudioCaptureDevices();
bool IsScreenRecording();
bool IsScreenRecordPaused();
void PauseScreenRecord(bool pause);
void StopScreenRecord();
std::wstring ScreenRecordOutputPath();
// Reap any in-flight ffmpeg subprocess at process exit (no UI). Deletes
// zero-byte / unreadable output files; logs path + size for valid ones.
void ReapScreenRecordOnExit();
