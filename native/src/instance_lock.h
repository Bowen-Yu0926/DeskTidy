#pragma once

#include <Windows.h>

// Single source of truth for the single-instance mutex name. Every caller that
// probes "is DeskTidy already running" must use this, or the probe silently
// answers for a different mutex (the old cli_ipc copy drifted out of sync and
// made --status report "stopped" forever).
constexpr wchar_t kNativeInstanceMutexName[] = L"Local\\DeskTidyMainInstance";

// Returns true if this process owns the single-instance mutex.
bool AcquireNativeInstanceLock();
void ReleaseNativeInstanceLock();
