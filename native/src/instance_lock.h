#pragma once

#include <Windows.h>

// Returns true if this process owns the single-instance mutex.
bool AcquireNativeInstanceLock();
void ReleaseNativeInstanceLock();
