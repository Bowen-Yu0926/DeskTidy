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
// Cheap: true when ffmpeg handle or pause state may need a poll (no Wait).
bool ScreenRecordSessionLive();
bool IsScreenRecordPaused();
void PauseScreenRecord(bool pause);
void StopScreenRecord();
std::wstring ScreenRecordOutputPath();
// P1-25: after a crash, adopt an orphan ffmpeg from screen_record_state.json.
// Returns true when a live recording was reclaimed (badge shown).
bool AdoptOrphanScreenRecord();
// P1-25: startup recovery — adopt a live orphan ffmpeg, or merge the leftover
// segments of a dead one (toasts "已恢复上次录制" / failure path keeps segments).
// Wired from App::Init (development A's file) — not called from app.cpp here.
bool RecoverInterruptedRecording();
// Reap any in-flight ffmpeg at process exit: finalize + merge (keep files),
// clear state. Never silently delete recoverable segments (P1-25).
void ReapScreenRecordOnExit();
