#pragma once

#include <Windows.h>

#include <string>

// Toggle ffmpeg gdigrab recording. Shows a REC badge (pause / stop).
bool ToggleScreenRecord(HWND owner);
bool IsScreenRecording();
bool IsScreenRecordPaused();
void PauseScreenRecord(bool pause);
void StopScreenRecord();
std::wstring ScreenRecordOutputPath();
