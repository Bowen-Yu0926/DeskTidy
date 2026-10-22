#pragma once

#include <Windows.h>

#include <string>

constexpr wchar_t kNativeMsgWindowClass[] = L"DeskTidyNativeMsg";
constexpr UINT kCliActionMsg = WM_APP + 90;

// Forward a CLI flag like L"--notes" to the running instance.
bool ForwardCliToRunningInstance(const std::wstring& action);

// Probe whether another process holds the single-instance mutex (no ownership
// taken). Used by --status so a fresh launch can report run state.
bool IsNativeInstanceRunning();
