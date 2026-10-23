#pragma once

#include <Windows.h>

#include <string>

// Validate that an ffmpeg.exe can serve the recording path: gdigrab + lavfi
// demuxers must be present, and at least one H.264 encoder (h264_mf or
// libx264) must be listed. On failure `error_out` gets a readable reason.
bool ValidateRecordingFfmpeg(const std::wstring& exe, std::wstring* error_out);

// Prefer Windows Media Foundation H.264 when listed (P1-24 / Python
// probe_preferred_encoder). Returns L"h264_mf" or L"libx264".
std::wstring PreferredRecordingEncoder(const std::wstring& exe);

// Ensure a usable ffmpeg.exe for recording. Returns true and fills `out_exe`
// when a validated ffmpeg is ready now; otherwise prompts to download the
// BtbN gpl-shared build (with progress), validates it, and only returns true
// on success. Every failure is reported to the user (never a silent no-op).
bool EnsureFfmpegForRecording(HWND owner, std::wstring* out_exe);
