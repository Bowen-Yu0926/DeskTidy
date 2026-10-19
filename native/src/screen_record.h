#pragma once

#include <Windows.h>

#include <string>

// Toggle ffmpeg gdigrab recording. Second call stops. Shows a small REC badge.
bool ToggleScreenRecord(HWND owner);
bool IsScreenRecording();
void StopScreenRecord();
std::wstring ScreenRecordOutputPath();
